#pragma once
#include "EngineMessages.h"
#include <algorithm>
#include <deque>
#include <memory>

namespace harukoto::engine {
// One owner-thread object per authenticated transport, NOT per numeric connection
// ID. Workers receive immutable input and return a page; they never mutate sessions.
class ServerConnection {
public:
    static constexpr size_t ReplayLimit = 64;
    static constexpr size_t WorkLimit = 64;
    struct Work {
        Frame request;
        std::string input;
        KeyMessage key;
        uint64_t deadline = 0;
    };
    using Ticket = std::shared_ptr<const Work>;
    enum class Status { Reply, Replay, Scheduled, Pending, Rejected, Closed };
    enum class Reason { None, Malformed, Identity, Stale, Conflict, MissingSession,
                        Capacity, Busy, Composition, Candidate, InputLimit, Backend };
    struct Result {
        Status status = Status::Rejected;
        Reason reason = Reason::None;
        std::optional<Frame> response;
        Ticket work;
    };
private:
    struct Session {
        uint64_t composition = 1;
        std::string input;
        CandidateSnapshot candidates;
        Ticket pending;
    };
    struct Cached { Frame request, response; };
    const uint64_t engine_;
    uint64_t connection_ = 0, lastRequest_ = 0, lastSession_ = 0;
    bool closed_ = false;
    std::unordered_map<uint64_t, Session> sessions_;
    std::deque<Cached> replies_;
    std::vector<std::weak_ptr<const Work>> outstanding_;
    static bool equal(const Frame &a, const Frame &b) {
        return a.kind == b.kind && a.id == b.id && a.payload == b.payload;
    }
    static Result reject(Reason reason) { return {Status::Rejected, reason, {}, {}}; }
    Result respond(const Frame &request, Frame response) {
        // Allocate response/cache before the caller mutates session state.
        Result result{Status::Reply, Reason::None, response, {}};
        replies_.push_back({request, std::move(response)});
        if (replies_.size() > ReplayLimit) replies_.pop_front();
        lastRequest_ = std::max(lastRequest_, request.id.request);
        return result;
    }
    void purge(uint64_t session, uint64_t except) {
        replies_.erase(std::remove_if(replies_.begin(), replies_.end(), [&](const Cached &entry) {
            return entry.request.id.session == session && entry.request.id.request != except;
        }), replies_.end());
    }
    static Frame reply(const Frame &request) { return {Kind::Reply, request.id, {}}; }
public:
    explicit ServerConnection(uint64_t engine) : engine_(engine) {
        if (!engine) throw std::invalid_argument("zero engine generation");
    }
    ServerConnection(const ServerConnection &) = delete;
    ServerConnection &operator=(const ServerConnection &) = delete;
    bool closed() const { return closed_; }
    size_t sessionCount() const { return sessions_.size(); }
    size_t cachedReplies() const { return replies_.size(); }
    void disconnect() { closed_ = true; sessions_.clear(); replies_.clear(); outstanding_.clear(); }
    bool expire(uint64_t now) {
        for (const auto &entry : sessions_) {
            if (entry.second.pending && now >= entry.second.pending->deadline) {
                disconnect(); return true;
            }
        }
        return false;
    }
    // Local statuses are not wire error payloads. The transport must define how
    // to close/report rejection; it must not synthesize a successful empty reply.
    Result receive(const Frame &request, uint64_t now, uint64_t deadline) {
        if (closed_ || expire(now)) return {Status::Closed, Reason::None, {}, {}};
        if (!valid(request)) return reject(Reason::Malformed);
        if (connection_ && (request.id.connection != connection_ ||
            (request.kind != Kind::Hello && request.id.engine != engine_))) return reject(Reason::Identity);
        for (const auto &cached : replies_) {
            if (cached.request.id.request == request.id.request) {
                if (!equal(cached.request, request)) return reject(Reason::Conflict);
                return {Status::Replay, Reason::None, cached.response, {}};
            }
        }
        for (const auto &entry : sessions_) {
            if (entry.second.pending && entry.second.pending->request.id.request == request.id.request) {
                if (!equal(entry.second.pending->request, request)) return reject(Reason::Conflict);
                return {Status::Pending, Reason::None, {}, {}}; // never dispatch twice
            }
        }
        if (request.id.request <= lastRequest_) return reject(Reason::Stale);
        if (!connection_) {
            if (request.kind != Kind::Hello || !request.payload.empty()) return reject(Reason::Malformed);
            auto response = request; response.kind = Kind::HelloAck; response.id.engine = engine_;
            auto result = respond(request, std::move(response)); connection_ = request.id.connection;
            return result;
        }
        if (request.kind == Kind::Open) {
            if (!request.payload.empty() || request.id.composition != 1) return reject(Reason::Malformed);
            if (request.id.session <= lastSession_) return reject(Reason::Stale);
            if (sessions_.size() >= MaxSessions) return reject(Reason::Capacity);
            const auto added = sessions_.emplace(request.id.session, Session{});
            try {
                auto result = respond(request, reply(request)); lastSession_ = request.id.session; return result;
            } catch (...) { sessions_.erase(added.first); throw; }
        }
        auto found = sessions_.find(request.id.session);
        if (found == sessions_.end()) return reject(Reason::MissingSession);
        auto &session = found->second;
        if (request.kind == Kind::Cancel || request.kind == Kind::Close) {
            if (!request.payload.empty()) return reject(Reason::Malformed);
            if (request.id.composition <= session.composition) return reject(Reason::Composition);
            auto result = respond(request, reply(request));
            purge(request.id.session, request.id.request); // no replay of old candidate/commit after cancel
            if (request.kind == Kind::Close) sessions_.erase(found);
            else {
                session.pending.reset(); session.input.clear(); session.candidates.invalidate();
                session.composition = request.id.composition;
            }
            return result;
        }
        if (request.id.composition != session.composition) return reject(Reason::Composition);
        if (session.pending) return reject(Reason::Busy);
        if (request.kind == Kind::Key) {
            KeyMessage key;
            if (!decodePayload(request.payload, key) || deadline <= now) return reject(Reason::Malformed);
            outstanding_.erase(std::remove_if(outstanding_.begin(), outstanding_.end(),
                [](const auto &weak) { return weak.expired(); }), outstanding_.end());
            if (outstanding_.size() >= WorkLimit) return reject(Reason::Capacity);
            auto input = session.input;
            if (key.action == KeyAction::Text) {
                if (input.size() == MaxInput) return reject(Reason::InputLimit);
                input.push_back(static_cast<char>(key.text));
            } else if (key.action == KeyAction::Backspace && !input.empty()) input.pop_back();
            Ticket work = std::make_shared<const Work>(Work{request, std::move(input), key, deadline});
            outstanding_.push_back(work); // Includes canceled work still held by the executor.
            session.pending = work; session.candidates.invalidate(); lastRequest_ = request.id.request;
            return {Status::Scheduled, Reason::None, {}, std::move(work)};
        }
        if (request.kind == Kind::Select) {
            const auto expected = session.candidates.preview(request);
            if (!expected) return reject(Reason::Candidate);
            auto remaining = expected->remaining;
            auto response = reply(request); response.payload = encodePayload(*expected);
            auto result = respond(request, std::move(response));
            session.input.swap(remaining); session.candidates.invalidate();
            return result;
        }
        return reject(Reason::Malformed);
    }
    Result complete(const Ticket &work, const CandidatePage &page, uint64_t now) {
        if (closed_ || expire(now)) return {Status::Closed, Reason::None, {}, {}};
        if (!work) return reject(Reason::Stale);
        auto found = sessions_.find(work->request.id.session);
        // Pointer identity is a capability scoped to this object. Equal numeric
        // IDs from another connection or a copied Work are insufficient.
        if (found == sessions_.end() || found->second.pending != work) return reject(Reason::Stale);
        auto &session = found->second;
        if (page.input != work->input) return reject(Reason::Backend);
        auto response = reply(work->request);
        try { response.payload = encodePayload(page); }
        catch (const std::invalid_argument &) { return reject(Reason::Backend); }
        auto candidates = session.candidates;
        if (!candidates.publish(response)) return reject(Reason::Backend);
        auto input = work->input;
        auto result = respond(work->request, std::move(response));
        session.input.swap(input);
        session.candidates = std::move(candidates); session.pending.reset();
        return result;
    }
};
} // namespace harukoto::engine
