#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EnginePipe.h"
#include "EngineMessages.h"
#include "EngineIdentity.h"
#include "EngineMemory.h"
#include "EngineRelease.h"
#include "MixedComposition.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

// WIN-03 step 6a: the TSF side of the standalone engine (docs/ENGINE_TSF.md).
// The text service keeps its own core session as the authority for the
// reading, choosing and learning; the engine supplies the candidate list of
// a plain reading, which the service checks against that session before
// showing it. Everything here is independent of TSF objects:
//  - Driver: one engine session over a Transport (blocking, worker thread);
//  - Link: the worker thread and its mailbox (latest request wins);
//  - Client: what the TSF thread keeps (switch, tickets, fallback, counters);
//  - rebuild(): turns an engine page back into the in-process list.
namespace harukoto::tsf_engine {
using engine::Candidate;
using engine::CandidatePage;
using engine::Frame;
using engine::Kind;
using engine::KeyAction;
using engine::KeyMessage;

// Same budget as resident::KeyDeadlineMs (WIN-03 step 5).
constexpr uint64_t DeadlineMs = 1000;
// Consecutive failures before the engine is left alone for StickyMs.
constexpr unsigned StickyFailures = 3;
constexpr uint64_t StickyMs = 10 * 60 * 1000;

// HKCU\Software\Meowpuut\Input DWORD StandaloneEngine: on only when it is a
// DWORD other than 0. Absent, unreadable or of another type: off (default).
inline bool readStandaloneEngine(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 0, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"StandaloneEngine",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result == ERROR_SUCCESS && value != 0;
}

struct Timeout : std::runtime_error { Timeout() : std::runtime_error("engine deadline") {} };

// One request/response exchange; throws on failure, Timeout at the deadline.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Frame exchange(const Frame &request, uint64_t deadline) = 0;
    // WIN-03 step 7a: approved release sequence served by the engine at the
    // other end (0 for unversioned / test links).
    virtual uint64_t sequence() const { return 0; }
    // WIN-03 step 7b: optional event signaled by EngineOwner when this idle
    // slot is reclaimed by LRU eviction (graceful reclamation, not a failure).
    virtual HANDLE revokeEvent() const { return nullptr; }
};

class PipeTransport final : public Transport {
    std::unique_ptr<engine::pipe::Channel> channel_;
    std::shared_ptr<void> keep_; // e.g. the handed-off slot; released with the transport.
    uint64_t sequence_ = 0;
    HANDLE revoke_ = nullptr; // Borrowed from keep_; valid for keep_'s lifetime.
    void await(HANDLE event, uint64_t deadline) {
        const auto time = engine::pipe::now();
        if (time >= deadline) throw Timeout();
        if (event && WaitForSingleObject(event, static_cast<DWORD>(deadline - time)) == WAIT_FAILED)
            throw std::runtime_error("engine wait failed");
    }
    void check(uint64_t deadline) {
        if (channel_->failure() == engine::pipe::Failure::Deadline || engine::pipe::now() >= deadline) throw Timeout();
        if (channel_->closed()) throw std::runtime_error("engine connection closed");
    }
public:
    PipeTransport(std::unique_ptr<engine::pipe::Channel> channel, std::shared_ptr<void> keep = {}, uint64_t sequence = 0, HANDLE revoke = nullptr)
        : channel_(std::move(channel)), keep_(std::move(keep)), sequence_(sequence), revoke_(revoke) {
        if (!channel_) throw std::invalid_argument("missing engine channel");
    }
    uint64_t sequence() const override { return sequence_; }
    HANDLE revokeEvent() const override { return revoke_; }
    Frame exchange(const Frame &request, uint64_t deadline) override {
        if (!channel_->beginWrite(request, engine::pipe::now(), deadline)) { check(deadline); throw std::runtime_error("engine write refused"); }
        for (;;) {
            channel_->poll(engine::pipe::now()); check(deadline);
            if (channel_->writeDone()) break;
            if (!channel_->needsPoll()) await(channel_->writeEvent(), deadline);
        }
        if (!channel_->beginRead(engine::pipe::now(), deadline)) { check(deadline); throw std::runtime_error("engine read refused"); }
        for (;;) {
            channel_->poll(engine::pipe::now());
            if (auto frame = channel_->take()) return std::move(*frame);
            check(deadline);
            if (!channel_->needsPoll()) await(channel_->readEvent(), deadline);
        }
    }
};

// The round-0 list of one reading: complete unless it ran past MaxPages.
// behind (step 6b): the engine could not reach the client's learned state.
// identity (step 6d): the data identity of the dictionary that listed it.
struct Fetched { std::vector<Candidate> words; bool complete = false, behind = false; std::string identity; };

// One engine session. Mirrors the reading the engine holds and sends the
// smallest key sequence that brings it to the requested one: one letter,
// one backspace, a refresh, or cancel and replay. Never replays a selection
// (6a sends none). Any failure leaves the driver unusable.
class Driver {
    std::unique_ptr<Transport> transport_;
    engine::ClientGate gate_;
    uint64_t session_ = 0;
    std::string identity_, mirror_;
    bool paged_ = false;
    static uint64_t now() { return engine::pipe::now(); }
    CandidatePage key(const KeyMessage &message, const std::string &expect, uint64_t deadline) {
        auto request = gate_.request(session_, Kind::Key, now(), deadline);
        if (!request) throw std::runtime_error("engine key refused");
        request->payload = engine::encodePayload(message);
        const auto response = transport_->exchange(*request, deadline);
        CandidatePage page;
        if (response.kind != Kind::Reply || !engine::decodePayload(response.payload, page) || page.input != expect ||
            !gate_.accept(response, now())) throw std::runtime_error("engine reply rejected");
        return page;
    }
public:
    static constexpr size_t MaxPages = 32; // Of 128 for a Query (step 6c), of 16 otherwise.
    explicit Driver(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {
        if (!transport_) throw std::invalid_argument("missing engine transport");
    }
    void open(uint64_t deadline) {
        const auto hello = gate_.connect(now(), deadline);
        if (!gate_.acknowledge(transport_->exchange(hello, deadline), now())) throw std::runtime_error("engine hello rejected");
        auto request = gate_.open(now(), deadline);
        if (!request) throw std::runtime_error("engine open refused");
        session_ = request->id.session;
        const auto reply = transport_->exchange(*request, deadline);
        std::string identity;
        if (reply.kind == Kind::Reply && !reply.payload.empty()) {
            engine::Opened opened;
            if (!engine::decodePayload(reply.payload, opened)) throw std::runtime_error("engine identity rejected");
            identity = opened.identity;
        }
        if (!gate_.accept(reply, now())) throw std::runtime_error("engine open rejected");
        identity_ = std::move(identity);
    }
    const std::string &identity() const { return identity_; }
    uint64_t sequence() const { return transport_->sequence(); }
    HANDLE revokeEvent() const { return transport_->revokeEvent(); }
    Fetched fetch(const std::string &input, bool simplified, uint64_t deadline) {
        return fetch(input, simplified, std::nullopt, deadline);
    }
    // settings (step 6b): the client's ranking state, sent with every key.
    Fetched fetch(const std::string &input, bool simplified, const std::optional<engine::QuerySettings> &settings, uint64_t deadline) {
        if (input.empty() || !engine::inputValid(input)) throw std::invalid_argument("invalid engine reading");
        CandidatePage page;
        const auto message = [&](KeyAction action, uint8_t c, std::string full = {}) {
            KeyMessage m{action, c, simplified};
            if (settings) { m.extended = true; m.settings = *settings; }
            m.input = std::move(full);
            return m;
        };
        const auto text = [&](char c, const std::string &expect) {
            return key(message(KeyAction::Text, static_cast<uint8_t>(c)), expect, deadline);
        };
        if (input.size() == mirror_.size() + 1 && input.compare(0, mirror_.size(), mirror_) == 0) {
            page = text(input.back(), input);
        } else if (!mirror_.empty() && input.size() + 1 == mirror_.size() && mirror_.compare(0, input.size(), input) == 0) {
            page = key(message(KeyAction::Backspace, 0), input, deadline);
        } else if (input == mirror_ && !paged_) {
            page = key(message(KeyAction::Refresh, 0), input, deadline);
        } else {
            page = key(message(KeyAction::Set, 0, input), input, deadline);
        }
        mirror_ = input; paged_ = false;
        Fetched out;
        out.identity = page.identity;
        if (page.behind) { out.behind = true; return out; }
        for (size_t pages = 1;; ++pages) {
            bool later = false;
            for (auto &word : page.words) {
                if (engine::candidateRound(word)) later = true;
                else out.words.push_back(std::move(word));
            }
            if (later || !page.more) { out.complete = true; break; }
            if (pages >= MaxPages) break;
            paged_ = true;
            page = key(message(KeyAction::PageNext, 0), input, deadline);
            if (page.behind) { out = {}; out.behind = true; return out; }
        }
        return out;
    }
};

// The worker thread and its mailbox. The TSF thread never blocks on it:
// submit() replaces any request not yet started, take() returns finished
// replies. A threaded link holds a reference to its module until its thread
// has destroyed every object (the DLL may be unloaded otherwise).
class Link {
public:
    enum class State { Connecting, Ready, Reclaimed, Failed };
    struct Request { uint64_t ticket = 0; std::string input; bool simplified = true; std::optional<engine::QuerySettings> settings; };
    struct Reply {
        uint64_t ticket = 0;
        bool ok = false, timeout = false, reclaimed = false;
        std::string input;
        bool simplified = true;
        Fetched fetched;
    };
    using Connect = std::function<std::unique_ptr<Transport>(uint64_t deadline)>;
private:
    struct Shared {
        std::mutex mutex;
        engine::pipe::Handle wakeEvent;
        std::optional<Request> request;
        std::deque<Reply> replies;
        State state = State::Connecting;
        std::string identity;
        uint64_t sequence = 0;
        bool stop = false;
        std::function<void()> notify; // Called under the mutex; must not block or re-enter.
        Connect connect;
        uint64_t connectMs = 10000;
        std::unique_ptr<Driver> driver; // Worker (or manual pump) only.
    };
    std::shared_ptr<Shared> shared_;
    bool manual_ = false;
    static void post(Shared &s) { if (s.notify) try { s.notify(); } catch (...) {} }
    static bool revokedNow(const Driver *driver) {
        if (!driver) return false;
        const HANDLE revoked = driver->revokeEvent();
        return revoked && WaitForSingleObject(revoked, 0) == WAIT_OBJECT_0;
    }
    static bool start(Shared &s) {
        try {
            auto driver = std::make_unique<Driver>(s.connect(engine::pipe::now() + s.connectMs));
            driver->open(engine::pipe::now() + DeadlineMs);
            std::lock_guard<std::mutex> lock(s.mutex);
            s.identity = driver->identity(); s.sequence = driver->sequence(); s.driver = std::move(driver);
            s.state = State::Ready; post(s);
            return true;
        } catch (...) {}
        std::lock_guard<std::mutex> lock(s.mutex);
        s.state = State::Failed; post(s);
        return false;
    }
    static bool serve(Shared &s, Request request) {
        Reply reply{request.ticket, false, false, false, request.input, request.simplified, {}};
        try {
            reply.fetched = s.driver->fetch(request.input, request.simplified, request.settings, engine::pipe::now() + DeadlineMs);
            reply.ok = true;
            // Step 6d: the engine may have adopted newly published data.
            if (!reply.fetched.identity.empty()) { std::lock_guard<std::mutex> lock(s.mutex); s.identity = reply.fetched.identity; }
        } catch (const Timeout &) { reply.timeout = true; }
        catch (...) {
            // Step 7b: if EngineOwner evicted this idle slot right as the
            // request began, report graceful reclamation rather than a crash.
            if (revokedNow(s.driver.get())) reply.reclaimed = true;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        s.replies.push_back(std::move(reply));
        while (s.replies.size() > 4) s.replies.pop_front();
        if (!s.replies.back().ok) s.state = s.replies.back().reclaimed ? State::Reclaimed : State::Failed;
        post(s);
        return s.replies.back().ok;
    }
    static void run(const std::shared_ptr<Shared> &shared) {
        auto &s = *shared;
        if (!start(s)) return;
        const HANDLE revoked = s.driver ? s.driver->revokeEvent() : nullptr;
        for (;;) {
            Request request;
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    if (s.stop) return;
                    if (s.request) { request = std::move(*s.request); s.request.reset(); break; }
                }
                // Step 7b: wake immediately when EngineOwner reclaims this idle
                // slot so our EngineHost pipe closes and frees its endpoint at once.
                if (revoked && WaitForSingleObject(revoked, 0) == WAIT_OBJECT_0) {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    if (s.stop) return;
                    if (!s.request) { s.driver.reset(); s.state = State::Reclaimed; post(s); return; }
                    continue;
                }
                HANDLE handles[2]{s.wakeEvent.get(), revoked};
                const DWORD count = revoked ? 2 : 1;
                const DWORD waited = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
                if (waited == WAIT_OBJECT_0 + 1) {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    if (s.stop) return;
                    if (!s.request) { s.driver.reset(); s.state = State::Reclaimed; post(s); return; }
                }
            }
            if (!serve(s, std::move(request))) return;
        }
    }
    struct Start { std::shared_ptr<Shared> shared; HMODULE module; };
    static DWORD WINAPI worker(void *parameter) {
        HMODULE module = nullptr;
        {
            std::unique_ptr<Start> start(static_cast<Start *>(parameter));
            module = start->module;
            auto shared = std::move(start->shared);
            start.reset();
            try { run(shared); } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->driver.reset(); // Close the connection on this thread.
            }
        } // Destroy all C++ objects before releasing the module reference.
        FreeLibraryAndExitThread(module, 0);
    }
    explicit Link(Connect connect, std::function<void()> notify, uint64_t connectMs) : shared_(std::make_shared<Shared>()) {
        shared_->wakeEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        shared_->connect = std::move(connect); shared_->notify = std::move(notify); shared_->connectMs = connectMs;
    }
public:
    // Production: the worker connects (it may start the resident engine) and
    // then serves requests. Returns null when no thread could be started.
    static std::unique_ptr<Link> threaded(Connect connect, std::function<void()> notify, uint64_t connectMs = 10000) {
        std::unique_ptr<Link> link(new Link(std::move(connect), std::move(notify), connectMs));
        if (!link->shared_->wakeEvent) return {};
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &module)) return {};
        auto start = std::make_unique<Start>(Start{link->shared_, module});
        HANDLE thread = CreateThread(nullptr, 0, &worker, start.get(), 0, nullptr);
        if (!thread) { FreeLibrary(module); return {}; }
        start.release(); CloseHandle(thread);
        return link;
    }
    // Tests: connects now on the calling thread; requests are served by pump().
    static std::unique_ptr<Link> manual(Connect connect, std::function<void()> notify = {}) {
        std::unique_ptr<Link> link(new Link(std::move(connect), std::move(notify), 10000));
        link->manual_ = true;
        start(*link->shared_);
        return link;
    }
    Link(const Link &) = delete;
    Link &operator=(const Link &) = delete;
    ~Link() {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->stop = true; shared_->notify = nullptr; shared_->request.reset();
        if (shared_->wakeEvent) SetEvent(shared_->wakeEvent.get());
    }
    State state() const {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (manual_ && shared_->state == State::Ready && revokedNow(shared_->driver.get())) {
            shared_->driver.reset();
            shared_->state = State::Reclaimed;
        }
        return shared_->state;
    }
    std::string identity() const { std::lock_guard<std::mutex> lock(shared_->mutex); return shared_->identity; }
    uint64_t sequence() const { std::lock_guard<std::mutex> lock(shared_->mutex); return shared_->sequence; }
    void submit(Request request) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->request = std::move(request);
        if (shared_->wakeEvent) SetEvent(shared_->wakeEvent.get());
    }
    std::optional<Reply> take() {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->replies.empty()) return {};
        auto reply = std::move(shared_->replies.front()); shared_->replies.pop_front();
        return reply;
    }
    // Manual links only: serve the waiting request on this thread.
    bool pump() {
        if (!manual_) return false;
        std::optional<Request> request;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->state != State::Ready) return false;
            request.swap(shared_->request);
        }
        if (!request) return false;
        serve(*shared_, std::move(*request));
        return true;
    }
};

// The engine leaves out a native candidate with no text before its literal
// tail or that consumes nothing; the in-process list keeps it.
inline bool representable(const MPNSession *session) {
    for (size_t i = 0, count = mpn_candidate_count(session); i < count; ++i) {
        const size_t size = std::string_view(mpn_candidate(session, i)).size(), tail = mpn_candidate_literal_tail(session, i);
        if (tail >= size || !mpn_candidate_consumed(session, i)) return false;
    }
    return true;
}

// What the in-process path would have listed for this reading, rebuilt from
// the engine's round-0 candidates and checked against the service's own
// session (same base data, so the same native candidates). Null when the
// page cannot be shown as is: an id gap or a native candidate the engine
// leaves out (no text after its literal tail, or nothing consumed), an index
// or text that does not match.
inline std::optional<std::vector<WindowsCandidate>> rebuild(const MPNSession *session, const std::vector<Candidate> &words) {
    const std::string input = mpn_input(session);
    const size_t count = mpn_candidate_count(session);
    if (!representable(session)) return {};
    std::vector<WindowsCandidate> result;
    uint64_t expected = 1;
    for (const auto &word : words) {
        if (word.id != expected++ || engine::candidateRound(word)) return {};
        WindowsCandidate candidate;
        candidate.japanese = (word.flags & engine::candidate_flag::Japanese) != 0;
        candidate.strong = (word.flags & engine::candidate_flag::Strong) != 0;
        candidate.directJapanese = (word.flags & engine::candidate_flag::Direct) != 0;
        candidate.annotation = word.reading;
        if (word.core) {
            const size_t index = word.core - 1;
            if (index >= count) return {};
            std::string text = mpn_candidate(session, index);
            const size_t tail = mpn_candidate_literal_tail(session, index);
            if (text.compare(0, text.size() - tail, word.text) != 0 || text.size() - tail != word.text.size() ||
                mpn_candidate_consumed(session, index) != word.consumed) return {};
            candidate.text = std::move(text);
            candidate.coreIndex = index;
        } else {
            if (word.consumed != input.size()) return {};
            candidate.text = word.text;
        }
        result.push_back(std::move(candidate));
    }
    // Empty is a real list: after a chosen segment (locked) the merge lists
    // only native candidates, and there may be none (step 6c).
    return result;
}

// TSF thread only. Owns the link, the ticket of the request in flight, the
// fallback policy and the counters shown by Diagnose.
class Client {
public:
    using Factory = std::function<std::unique_ptr<Link>()>;
    using SequenceFactory = std::function<std::unique_ptr<Link>(uint64_t sequence)>;
    static constexpr uint64_t UpgradeCheckMs = 2000;
    static constexpr uint64_t CutoverCooldownMs = 60 * 1000;
    static constexpr uint64_t FocusYieldMs = 15 * 1000;
    static constexpr uint64_t IdleYieldMs = 180 * 1000;
    bool enabled = false;
    Factory factory;
    SequenceFactory sequenceFactory; // Optional; tests may inject per-sequence links.
    struct Stats {
        uint64_t served = 0, fallbacks = 0, stale = 0, failures = 0;
        uint64_t memoryBehind = 0;    // Step 6b: replies "behind" the client's learned state.
        uint64_t sequence = 0;        // Step 7a: active engine release sequence (0 if unversioned/none).
        uint64_t cutovers = 0;        // Step 7a: completed cutovers at empty boundaries.
        uint64_t cutoverFailures = 0; // Step 7a: failed standby connections (old link kept).
        bool incompatible = false;    // Step 7a: installed release speaks another protocol/format.
        uint64_t yields = 0;          // Step 7b: voluntary idle/unfocused slot releases.
        uint64_t reclaimed = 0;       // Step 7b: idle slots reclaimed by owner/host (zero failure penalty).
        const char *path = "off";
        std::string reason = "none";
    } stats;
private:
    std::unique_ptr<Link> link_;
    std::unique_ptr<Link> standby_;   // Step 7a: background connection to a newer same-protocol release.
    uint64_t targetSequence_ = 0;
    uint64_t failedSequence_ = 0;
    uint64_t failedSequenceAt_ = 0;
    uint64_t lastUpgradeCheck_ = 0;
    uint64_t ticket_ = 0;
    struct Pending { uint64_t ticket; std::string input; bool simplified; uint64_t sent; };
    std::optional<Pending> pending_;
    unsigned consecutive_ = 0;
    uint64_t retryAt_ = 0;
    bool blocked_ = false; // Failed in this composition: in-process until the next boundary.
    uint64_t identityProbe_ = 0; // Last request sent to an engine with other data (step 6d).
    std::string behind_;   // memoryKey() of the last "behind" reply,
    uint64_t behindAt_ = 0; // and when it came.
    void resetReclaimed() {
        ++stats.reclaimed;
        link_.reset(); standby_.reset(); targetSequence_ = 0;
        stats.sequence = 0; behind_.clear(); identityProbe_ = 0;
    }
    bool finishStandby(uint64_t now, uint64_t activeSeq, uint64_t nextSeq) {
        if (!standby_) return false;
        const auto s = standby_->state();
        if (s == Link::State::Connecting) return false;
        if (s == Link::State::Reclaimed) {
            standby_.reset(); targetSequence_ = 0; ++stats.reclaimed;
            return false;
        }
        const auto seq = standby_->sequence();
        if (s == Link::State::Ready && (!seq || seq >= nextSeq) && (!seq || seq > activeSeq)) {
            link_ = std::move(standby_);
            targetSequence_ = 0;
            stats.sequence = seq ? seq : nextSeq;
            ++stats.cutovers;
            failedSequence_ = 0; failedSequenceAt_ = 0;
            behind_.clear(); identityProbe_ = 0; consecutive_ = 0; blocked_ = false;
            return true;
        }
        standby_.reset();
        failedSequence_ = nextSeq; failedSequenceAt_ = now; targetSequence_ = 0;
        ++stats.cutoverFailures;
        return false;
    }
public:
    bool pending() const { return pending_.has_value(); }
    Link *link() const { return link_.get(); }
    Link *standby() const { return standby_.get(); }
    void inProcess(const std::string &reason) { stats.path = enabled ? "in_process" : "off"; stats.reason = enabled ? reason : "none"; }
    // Step 7b: voluntarily release the connection slot when unfocused or idle
    // at an empty composition boundary. Never treated as a failure.
    bool yield() {
        if (pending_ || (!link_ && !standby_)) return false;
        link_.reset(); standby_.reset(); targetSequence_ = 0;
        stats.sequence = 0; behind_.clear(); identityProbe_ = 0;
        ++stats.yields;
        return true;
    }
    // Whether the engine may serve now; creates the link on first use.
    bool ready(uint64_t now, const std::string &identity) {
        if (!enabled) { inProcess("off"); link_.reset(); standby_.reset(); targetSequence_ = 0; stats.sequence = 0; return false; }
        if (blocked_) { inProcess("fallback"); return false; }
        if (now < retryAt_) { inProcess("sticky"); return false; }
        if (link_ && link_->state() == Link::State::Reclaimed) resetReclaimed();
        if (!link_) {
            if (stats.incompatible) { inProcess("incompatible"); return false; }
            if (factory) try { link_ = factory(); } catch (...) { link_.reset(); }
            if (!link_) { inProcess("unavailable"); return false; }
        }
        const auto state = link_->state();
        if (state == Link::State::Connecting) { inProcess("connecting"); return false; }
        if (state == Link::State::Reclaimed) { resetReclaimed(); inProcess("reclaimed"); return false; }
        if (state == Link::State::Failed) { fail(now, "unavailable"); return false; }
        stats.sequence = link_->sequence();
        if (identity.empty()) { inProcess("identity"); return false; }
        // Step 6d: the engine may have adopted newer data since its last
        // reply: ask at most once a second; each reply names its data.
        if (link_->identity() != identity) {
            if (identityProbe_ && now < identityProbe_ + 1000) { inProcess("identity"); return false; }
            identityProbe_ = now;
        }
        return true;
    }
    // Step 7a: evaluate the installed release at an empty composition boundary.
    // Never called mid-composition. Pre-connects a newer same-protocol release
    // in standby_, atomically swaps once Ready, keeps the old link if standby_
    // fails, and marks incompatible on ProtocolMismatch without mixing.
    bool checkUpgrade(uint64_t now, const engine::release::Evaluation &eval) {
        if (!enabled || pending_) return false;
        if (eval.kind == engine::release::Compatibility::ProtocolMismatch) {
            stats.incompatible = true;
            standby_.reset(); targetSequence_ = 0;
            return false;
        }
        stats.incompatible = false;
        if (eval.kind != engine::release::Compatibility::Match || !eval.selected) {
            standby_.reset(); targetSequence_ = 0;
            return false;
        }
        if (!link_ || link_->state() != Link::State::Ready) {
            standby_.reset(); targetSequence_ = 0;
            return false;
        }
        const uint64_t activeSeq = link_->sequence();
        stats.sequence = activeSeq;
        const uint64_t nextSeq = eval.selected->sequence;
        if (!activeSeq || nextSeq <= activeSeq) {
            standby_.reset(); targetSequence_ = 0;
            return false;
        }
        if (standby_ && targetSequence_ != nextSeq) {
            standby_.reset(); targetSequence_ = 0;
        }
        if (standby_) return finishStandby(now, activeSeq, nextSeq);
        if (failedSequence_ == nextSeq && now < failedSequenceAt_ + CutoverCooldownMs) return false;
        try {
            standby_ = sequenceFactory ? sequenceFactory(nextSeq) : (factory ? factory() : nullptr);
        } catch (...) { standby_.reset(); }
        if (!standby_) {
            failedSequence_ = nextSeq; failedSequenceAt_ = now; targetSequence_ = 0;
            ++stats.cutoverFailures;
            return false;
        }
        targetSequence_ = nextSeq;
        return finishStandby(now, activeSeq, nextSeq);
    }
    bool pollUpgrade(uint64_t now, const std::function<engine::release::Evaluation()> &probe, bool force = false) {
        if (!enabled || pending_) return false;
        if (!force && lastUpgradeCheck_ && now < lastUpgradeCheck_ + UpgradeCheckMs) return false;
        lastUpgradeCheck_ = now;
        if (!probe) return false;
        engine::release::Evaluation eval;
        try { eval = probe(); } catch (...) { return false; }
        return checkUpgrade(now, eval);
    }
    void send(const std::string &input, bool simplified, uint64_t now, std::optional<engine::QuerySettings> settings = {}) {
        pending_ = Pending{++ticket_, input, simplified, now};
        link_->submit({ticket_, input, simplified, std::move(settings)});
    }
    // Step 6b: the engine answered "behind" for exactly this learned state;
    // do not ask again for BehindMs unless the client's state changes (its
    // next poll or learning) or the link is replaced.
    static std::string memoryKey(const engine::QuerySettings &q) {
        return std::string(q.memory ? "1" : "0") + q.phrases + q.words + q.user;
    }
    static constexpr uint64_t BehindMs = 2000;
    bool knownBehind(const engine::QuerySettings &q, uint64_t now) const {
        return !behind_.empty() && now < behindAt_ + BehindMs && behind_ == memoryKey(q);
    }
    void behind(const engine::QuerySettings &q, uint64_t now) {
        behind_ = memoryKey(q); behindAt_ = now; ++stats.memoryBehind; decline("memory_behind");
    }
    // A newer local list replaces whatever the engine is computing.
    void cancel() { pending_.reset(); }
    // Deactivation: drop the link (its worker ends on its own) and any request.
    void close() { pending_.reset(); link_.reset(); standby_.reset(); targetSequence_ = 0; stats.sequence = 0; behind_.clear(); }
    // The reading became empty: a failed engine may be tried again.
    void boundary() { if (!pending_) blocked_ = false; }
    void fail(uint64_t now, const std::string &reason) {
        ++stats.failures; ++stats.fallbacks;
        stats.path = "fallback"; stats.reason = reason;
        pending_.reset(); link_.reset(); standby_.reset(); targetSequence_ = 0; stats.sequence = 0; blocked_ = true; behind_.clear();
        if (++consecutive_ >= StickyFailures) { retryAt_ = now + StickyMs; consecutive_ = 0; }
    }
    // Pending without failure (a list the engine cannot represent yet).
    void decline(const std::string &reason) { ++stats.fallbacks; stats.path = "in_process"; stats.reason = reason; pending_.reset(); }
    void served() { ++stats.served; consecutive_ = 0; stats.path = "engine"; stats.reason = "none"; pending_.reset(); }
    enum class Collect { Waiting, Ready, Failed, Declined };
    // Drains finished replies. Ready: out holds the reply for the current
    // request with the current reading and script. Failed: the link failed or
    // the request is overdue; Declined: the reply no longer fits the reading.
    // In both the caller computes the list in-process.
    Collect collect(const std::string &input, bool simplified, uint64_t now, Fetched &out, std::string &reason) {
        while (link_) {
            auto reply = link_->take();
            if (!reply) break;
            if (!pending_ || reply->ticket != pending_->ticket) {
                if (!reply->ok) {
                    if (reply->reclaimed) {
                        resetReclaimed(); reason = "reclaimed";
                        if (pending_) { decline(reason); return Collect::Declined; }
                        return Collect::Waiting;
                    }
                    fail(now, reply->timeout ? "timeout" : "unavailable"); reason = stats.reason; return pending_ ? Collect::Failed : Collect::Waiting;
                }
                ++stats.stale; continue;
            }
            if (!reply->ok) {
                if (reply->reclaimed) { resetReclaimed(); reason = "reclaimed"; decline(reason); return Collect::Declined; }
                reason = reply->timeout ? "timeout" : "unavailable"; fail(now, reason); return Collect::Failed;
            }
            if (reply->input != input || reply->simplified != simplified) { ++stats.stale; reason = "stale"; decline(reason); return Collect::Declined; }
            out = std::move(reply->fetched);
            return Collect::Ready;
        }
        if (pending_ && now >= pending_->sent + DeadlineMs) { reason = "timeout"; fail(now, reason); return Collect::Failed; }
        return Collect::Waiting;
    }
    std::string diagnosticFields() const {
        std::string reason;
        for (char c : stats.reason) if ((c >= 'a' && c <= 'z') || c == '_') reason += c;
        return ",\"engine_path\":\"" + std::string(stats.path) + "\",\"engine_served\":" + std::to_string(stats.served) +
            ",\"engine_fallbacks\":" + std::to_string(stats.fallbacks) + ",\"engine_reason\":\"" + reason + "\"" +
            ",\"engine_memory_behind\":" + std::to_string(stats.memoryBehind) +
            ",\"engine_sequence\":" + std::to_string(stats.sequence) +
            ",\"engine_cutovers\":" + std::to_string(stats.cutovers) +
            ",\"engine_cutover_failures\":" + std::to_string(stats.cutoverFailures) +
            ",\"engine_incompatible\":" + std::string(stats.incompatible ? "true" : "false") +
            ",\"engine_yields\":" + std::to_string(stats.yields) +
            ",\"engine_reclaimed\":" + std::to_string(stats.reclaimed);
    }
};
} // namespace harukoto::tsf_engine
