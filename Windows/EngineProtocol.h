#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <utility>

// Experimental envelope and client-side ownership gate. No transport or TSF I/O.
namespace harukoto::engine {
constexpr uint16_t ProtocolVersion = 1;
constexpr size_t HeaderSize = 56, MaxPayload = 65536;
constexpr size_t MaxSessions = 64;
enum class Kind : uint16_t { Hello = 1, HelloAck, Open, Key, Select, Cancel, Close, Reply, Error };
struct Identity {
    uint64_t connection = 0, engine = 0, session = 0, composition = 0, request = 0;
    bool operator==(const Identity &o) const {
        return connection == o.connection && engine == o.engine && session == o.session &&
               composition == o.composition && request == o.request;
    }
};
struct Frame {
    Kind kind = Kind::Hello;
    Identity id;
    std::vector<uint8_t> payload;
};
inline bool valid(const Frame &f) {
    const auto kind = static_cast<uint16_t>(f.kind);
    if (kind < 1 || kind > static_cast<uint16_t>(Kind::Error) || f.payload.size() > MaxPayload ||
        !f.id.connection || !f.id.request) return false;
    if (f.kind == Kind::Hello || f.kind == Kind::HelloAck)
        return !f.id.session && !f.id.composition &&
               (f.kind == Kind::Hello ? !f.id.engine : f.id.engine != 0);
    return f.id.engine && f.id.session && f.id.composition;
}
inline void put(std::vector<uint8_t> &out, uint64_t n, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(n >> (i * 8)));
}
inline uint64_t get(const uint8_t *data, size_t offset, size_t bytes) {
    uint64_t result = 0;
    for (size_t i = 0; i < bytes; ++i) result |= uint64_t(data[offset + i]) << (i * 8);
    return result;
}
inline std::vector<uint8_t> encode(const Frame &f) {
    if (!valid(f)) throw std::invalid_argument("invalid engine envelope");
    std::vector<uint8_t> out{'H', 'K', 'I', 'P'};
    put(out, ProtocolVersion, 2); put(out, static_cast<uint16_t>(f.kind), 2);
    put(out, f.payload.size(), 4); put(out, 0, 4); // reserved, must stay zero
    for (auto n : {f.id.connection, f.id.engine, f.id.session, f.id.composition, f.id.request}) put(out, n, 8);
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    return out;
}
enum class Decode { Incomplete, Complete, Invalid };
// Exactly one bounded frame. A transport may use consumed to process coalesced frames.
// On failure, output remains unchanged; no allocation based on unchecked length.
inline Decode decode(const uint8_t *data, size_t size, Frame &out, size_t &consumed) {
    consumed = 0;
    if (size < HeaderSize) return Decode::Incomplete;
    if (!data || data[0] != 'H' || data[1] != 'K' || data[2] != 'I' || data[3] != 'P' ||
        get(data, 4, 2) != ProtocolVersion || get(data, 12, 4)) return Decode::Invalid;
    const auto length = get(data, 8, 4);
    if (length > MaxPayload) return Decode::Invalid;
    Frame f;
    f.kind = static_cast<Kind>(get(data, 6, 2));
    f.id = {get(data, 16, 8), get(data, 24, 8), get(data, 32, 8), get(data, 40, 8), get(data, 48, 8)};
    if (!valid(f)) return Decode::Invalid;
    if (size < HeaderSize + length) return Decode::Incomplete;
    f.payload.assign(data + HeaderSize, data + HeaderSize + length);
    out = std::move(f); consumed = HeaderSize + static_cast<size_t>(length);
    return Decode::Complete;
}

// One instance per authenticated transport connection lineage, owner-thread only.
// Session IDs represent retained contexts, never HWNDs or raw COM addresses.
// This gate grants response ownership; typed payload validation must precede accept.
class ClientGate {
    struct Session {
        uint64_t composition = 1;
        bool opened = false, closing = false;
        std::optional<Frame> pending;
        uint64_t deadline = 0;
    };
    uint64_t connection_ = 0, engine_ = 0, session_ = 0, request_ = 0;
    bool ready_ = false;
    std::optional<Frame> hello_;
    uint64_t helloDeadline_ = 0;
    std::unordered_map<uint64_t, Session> sessions_;
    static uint64_t next(uint64_t &value) {
        if (value == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("engine identity exhausted");
        return ++value;
    }
    Frame issue(uint64_t id, Session &s, Kind kind, uint64_t deadline) {
        Frame f{kind, {connection_, engine_, id, s.composition, next(request_)}, {}};
        s.pending = f; s.deadline = deadline;
        return f;
    }
public:
    ClientGate() = default;
    ClientGate(const ClientGate &) = delete;
    ClientGate &operator=(const ClientGate &) = delete;
    ClientGate(ClientGate &&) = delete;
    ClientGate &operator=(ClientGate &&) = delete;
    bool ready() const { return ready_; }
    size_t size() const { return sessions_.size(); }
    void disconnect() { ready_ = false; engine_ = 0; hello_.reset(); sessions_.clear(); }
    // Call only after authenticated transport creation. Counters never reset on reconnect.
    Frame connect(uint64_t now, uint64_t deadline) {
        if (deadline <= now) throw std::invalid_argument("expired handshake deadline");
        disconnect();
        hello_ = Frame{Kind::Hello, {next(connection_), 0, 0, 0, next(request_)}, {}};
        helloDeadline_ = deadline;
        return *hello_;
    }
    bool expire(uint64_t now) {
        bool expired = hello_ && now >= helloDeadline_;
        for (const auto &entry : sessions_)
            expired = expired || (entry.second.pending && now >= entry.second.deadline);
        if (expired) disconnect(); // execution may be unknown; never replay a key or selection
        return expired;
    }
    bool acknowledge(const Frame &f, uint64_t now) {
        if (expire(now) || !hello_ || !valid(f) || f.kind != Kind::HelloAck || !f.payload.empty() ||
            f.id.connection != hello_->id.connection || f.id.request != hello_->id.request) return false;
        engine_ = f.id.engine; ready_ = true; hello_.reset();
        return true;
    }
    std::optional<Frame> open(uint64_t now, uint64_t deadline) {
        if (expire(now) || !ready_ || deadline <= now || sessions_.size() >= MaxSessions) return {};
        const auto id = next(session_);
        return issue(id, sessions_[id], Kind::Open, deadline);
    }
    std::optional<Frame> request(uint64_t id, Kind kind, uint64_t now, uint64_t deadline) {
        if (expire(now) || !ready_ || deadline <= now) return {};
        auto found = sessions_.find(id);
        if (found == sessions_.end()) return {};
        auto &s = found->second;
        if (!s.opened || s.closing || s.pending || (kind != Kind::Key && kind != Kind::Select)) return {};
        return issue(id, s, kind, deadline);
    }
    // Ordered transport must send this even when it supersedes an in-flight request.
    // The server must advance composition and discard older work before acknowledging.
    std::optional<Frame> cancel(uint64_t id, uint64_t now, uint64_t deadline) {
        if (expire(now) || !ready_ || deadline <= now) return {};
        auto found = sessions_.find(id);
        if (found == sessions_.end() || found->second.closing) return {};
        auto &s = found->second; next(s.composition);
        return issue(id, s, Kind::Cancel, deadline);
    }
    std::optional<Frame> close(uint64_t id, uint64_t now, uint64_t deadline) {
        if (expire(now) || !ready_ || deadline <= now) return {};
        auto found = sessions_.find(id);
        if (found == sessions_.end() || found->second.closing) return {};
        auto &s = found->second; next(s.composition); s.closing = true;
        return issue(id, s, Kind::Close, deadline);
    }
    bool accept(const Frame &f, uint64_t now) {
        if (expire(now) || !ready_ || !valid(f) || (f.kind != Kind::Reply && f.kind != Kind::Error)) return false;
        auto found = sessions_.find(f.id.session);
        if (found == sessions_.end()) return false;
        auto &s = found->second;
        if (!s.pending || !(s.pending->id == f.id)) return false;
        if (f.kind == Kind::Error) { disconnect(); return false; }
        const auto kind = s.pending->kind;
        if ((kind == Kind::Open || kind == Kind::Cancel || kind == Kind::Close) && !f.payload.empty()) return false;
        s.pending.reset();
        if (kind == Kind::Open || kind == Kind::Cancel) s.opened = true;
        if (kind == Kind::Close) sessions_.erase(found);
        return true; // one grant only; caller retains the original context for delivery
    }
};
} // namespace harukoto::engine
