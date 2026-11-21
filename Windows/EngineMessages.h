#pragma once
#include "EngineProtocol.h"
#include <algorithm>
#include <string>
#include <unordered_set>

namespace harukoto::engine {
constexpr size_t MaxInput = 256, MaxText = 1024, MaxCandidates = 128;
constexpr uint32_t MaxCoreIndex = 1u << 24;
enum class Payload : uint16_t { Key = 1, Candidates = 2, Select = 3, Commit = 4, Error = ErrorPayloadTag, Opened = 6, Query = 7 };
enum class KeyAction : uint8_t { Text = 1, Backspace, PageNext, PagePrevious, Refresh };
// WIN-03 step 6b: the per-user ranking state of a TSF client, sent with each
// key (Payload::Query). fuzzy/typo set the core session; memory says whether
// selection memory and word frequency apply (at hour, the hour of the
// client's personal table); the digests (EngineMemory.h) name the exact
// phrase, word and user-word models the client ranks with. Phrase and word
// digests are empty when memory is off; the user digest is always given.
struct QuerySettings {
    uint32_t fuzzy = 0;
    bool typo = true, memory = false;
    int64_t hour = 0;
    std::string phrases, words, user;
};
struct KeyMessage {
    KeyAction action = KeyAction::Text; uint8_t text = 0; bool simplified = true;
    bool extended = false; // false: the step 6a key, ranked with engine defaults.
    QuerySettings settings;
};
// WIN-03 step 6a: core is the native candidate index + 1 (0: not a core
// candidate); flags describe how the Windows merge placed it, so a TSF client
// can rebuild exactly the in-process list. The round is the expansion pass
// that produced the candidate (0: before any mpn_expand_candidates).
namespace candidate_flag {
constexpr uint8_t Japanese = 1, Strong = 2, Direct = 4, RoundShift = 4, RoundMask = 0x30, Known = 0x37;
}
struct Candidate { uint64_t id = 0; uint32_t consumed = 0; std::string text, reading; uint32_t core = 0; uint8_t flags = 0; };
inline unsigned candidateRound(const Candidate &c) { return (c.flags & candidate_flag::RoundMask) >> candidate_flag::RoundShift; }
// The Open reply may name the data the engine serves (EngineIdentity.h).
constexpr size_t IdentitySize = 64;
struct Opened { std::string identity; };
static_assert(2 + 2 + IdentitySize == OpenedPayloadSize, "Opened payload layout");
// behind (step 6b): the engine could not reach the client's memory state; no
// candidates, the client lists in-process.
struct CandidatePage { std::string input; bool more = false; std::vector<Candidate> words; bool behind = false; };
struct Selection { uint64_t sourceRequest = 0, candidate = 0; };
struct Commit { Selection selection; uint32_t consumed = 0; std::string text, remaining; };

inline bool inputByte(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == 39; }
inline bool inputValid(const std::string &s) {
    if (s.size() > MaxInput) return false;
    for (unsigned char c : s) if (!inputByte(c)) return false;
    return true;
}
// Strict scalar UTF-8, no embedded NUL or C0/C1 controls. Reading may be empty.
inline bool textValid(const std::string &s) {
    if (s.size() > MaxText) return false;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = static_cast<uint8_t>(s[i++]);
        size_t extra = 0; uint32_t minimum = 0;
        if (cp < 0x80) {}
        else if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; extra = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; extra = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; extra = 3; minimum = 0x10000; }
        else return false;
        if (extra > s.size() - i) return false;
        while (extra--) {
            const auto b = static_cast<uint8_t>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) return false;
    }
    return true;
}
inline bool digestValid(const std::string &s) {
    return s.empty() || (s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
}
inline bool valid(const QuerySettings &q) {
    return q.fuzzy <= 0xffff && q.hour >= 0 && q.hour <= 100000000 && digestValid(q.phrases) && digestValid(q.words) &&
        q.user.size() == 64 && digestValid(q.user) && (q.memory || (q.phrases.empty() && q.words.empty()));
}
inline bool valid(const KeyMessage &m) {
    if (m.extended && !valid(m.settings)) return false;
    if (m.action == KeyAction::Text) return inputByte(m.text);
    return m.text == 0 && (m.action == KeyAction::Backspace || m.action == KeyAction::PageNext ||
                          m.action == KeyAction::PagePrevious || m.action == KeyAction::Refresh);
}
inline bool valid(const CandidatePage &p) {
    if (!inputValid(p.input) || p.words.size() > MaxCandidates || (p.more && p.words.empty()) ||
        (p.behind && (p.more || !p.words.empty()))) return false;
    std::unordered_set<uint64_t> ids;
    for (const auto &word : p.words)
        if (!word.id || !ids.insert(word.id).second || !word.consumed || word.consumed > p.input.size() ||
            word.text.empty() || !textValid(word.text) || !textValid(word.reading) ||
            (word.flags & ~candidate_flag::Known) || word.core > MaxCoreIndex) return false;
    return true;
}
inline bool valid(const Selection &s) { return s.sourceRequest && s.candidate; }
inline bool valid(const Commit &c) {
    return valid(c.selection) && c.consumed && c.consumed <= MaxInput &&
           !c.text.empty() && textValid(c.text) && inputValid(c.remaining) && c.remaining.size() + c.consumed <= MaxInput;
}
class PayloadReader {
    const std::vector<uint8_t> &bytes_; size_t cursor_ = 0; bool ok_;
public:
    explicit PayloadReader(const std::vector<uint8_t> &bytes) : bytes_(bytes), ok_(bytes.size() <= MaxPayload) {}
    uint64_t number(size_t count) {
        if (!ok_ || count > 8 || count > bytes_.size() - cursor_) { ok_ = false; return 0; }
        auto n = get(bytes_.data(), cursor_, count); cursor_ += count; return n;
    }
    std::string string(size_t maximum) {
        const auto count = static_cast<size_t>(number(2));
        if (!ok_ || count > maximum || count > bytes_.size() - cursor_) { ok_ = false; return {}; }
        std::string s(bytes_.begin() + cursor_, bytes_.begin() + cursor_ + count); cursor_ += count; return s;
    }
    bool done() const { return ok_ && cursor_ == bytes_.size(); }
};
inline void putString(std::vector<uint8_t> &bytes, const std::string &s) {
    put(bytes, s.size(), 2); bytes.insert(bytes.end(), s.begin(), s.end());
}
inline std::vector<uint8_t> encodePayload(const ErrorMessage &error) { return encodeError(error); }
inline bool decodePayload(const std::vector<uint8_t> &bytes, ErrorMessage &out) { return decodeError(bytes, out); }
inline std::vector<uint8_t> encodePayload(const KeyMessage &m) {
    if (!valid(m)) throw std::invalid_argument("invalid engine key");
    std::vector<uint8_t> b; put(b, static_cast<uint16_t>(m.extended ? Payload::Query : Payload::Key), 2);
    put(b, static_cast<uint8_t>(m.action), 1); put(b, m.text, 1); put(b, m.simplified, 1);
    if (m.extended) {
        const auto &q = m.settings;
        put(b, q.fuzzy, 4); put(b, q.typo, 1); put(b, q.memory, 1); put(b, static_cast<uint64_t>(q.hour), 8);
        putString(b, q.phrases); putString(b, q.words); putString(b, q.user);
    }
    return b;
}
inline std::vector<uint8_t> encodePayload(const CandidatePage &p) {
    if (!valid(p)) throw std::invalid_argument("invalid candidate page");
    std::vector<uint8_t> b; put(b, static_cast<uint16_t>(Payload::Candidates), 2);
    putString(b, p.input); put(b, (p.more ? 1 : 0) | (p.behind ? 2 : 0), 1); put(b, p.words.size(), 2);
    for (const auto &w : p.words) {
        put(b, w.id, 8); put(b, w.consumed, 4); putString(b, w.text); putString(b, w.reading);
        put(b, w.core, 4); put(b, w.flags, 1);
        if (b.size() > MaxPayload) throw std::invalid_argument("candidate frame too large");
    }
    return b;
}
inline std::vector<uint8_t> encodePayload(const Selection &s) {
    if (!valid(s)) throw std::invalid_argument("invalid selection");
    std::vector<uint8_t> b; put(b, static_cast<uint16_t>(Payload::Select), 2);
    put(b, s.sourceRequest, 8); put(b, s.candidate, 8); return b;
}
inline std::vector<uint8_t> encodePayload(const Commit &c) {
    if (!valid(c)) throw std::invalid_argument("invalid commit");
    std::vector<uint8_t> b; put(b, static_cast<uint16_t>(Payload::Commit), 2);
    put(b, c.selection.sourceRequest, 8); put(b, c.selection.candidate, 8); put(b, c.consumed, 4);
    putString(b, c.text); putString(b, c.remaining); return b;
}
inline bool decodePayload(const std::vector<uint8_t> &b, KeyMessage &out) {
    PayloadReader r(b); const auto tag = r.number(2);
    if (tag != static_cast<uint16_t>(Payload::Key) && tag != static_cast<uint16_t>(Payload::Query)) return false;
    KeyMessage m; m.action = static_cast<KeyAction>(r.number(1)); m.text = static_cast<uint8_t>(r.number(1));
    const auto simplified = r.number(1); m.simplified = simplified != 0;
    uint64_t typo = 0, memory = 0;
    if (tag == static_cast<uint16_t>(Payload::Query)) {
        auto &q = m.settings; m.extended = true;
        q.fuzzy = static_cast<uint32_t>(r.number(4)); typo = r.number(1); memory = r.number(1);
        q.typo = typo != 0; q.memory = memory != 0; q.hour = static_cast<int64_t>(r.number(8));
        q.phrases = r.string(64); q.words = r.string(64); q.user = r.string(64);
    }
    if (simplified > 1 || typo > 1 || memory > 1 || !r.done() || !valid(m)) return false; out = std::move(m); return true;
}
inline bool decodePayload(const std::vector<uint8_t> &b, CandidatePage &out) {
    PayloadReader r(b); if (r.number(2) != static_cast<uint16_t>(Payload::Candidates)) return false;
    CandidatePage p; p.input = r.string(MaxInput); const auto more = r.number(1); p.more = (more & 1) != 0; p.behind = (more & 2) != 0;
    const auto count = r.number(2); if (count > MaxCandidates || more > 3) return false;
    for (size_t i = 0; i < count; ++i) {
        Candidate w; w.id = r.number(8); w.consumed = static_cast<uint32_t>(r.number(4));
        w.text = r.string(MaxText); w.reading = r.string(MaxText);
        w.core = static_cast<uint32_t>(r.number(4)); w.flags = static_cast<uint8_t>(r.number(1));
        p.words.push_back(std::move(w));
    }
    if (!r.done() || !valid(p)) return false; out = std::move(p); return true;
}
inline bool decodePayload(const std::vector<uint8_t> &b, Selection &out) {
    PayloadReader r(b); if (r.number(2) != static_cast<uint16_t>(Payload::Select)) return false;
    Selection s{r.number(8), r.number(8)};
    if (!r.done() || !valid(s)) return false; out = s; return true;
}
inline bool decodePayload(const std::vector<uint8_t> &b, Commit &out) {
    PayloadReader r(b); if (r.number(2) != static_cast<uint16_t>(Payload::Commit)) return false;
    Commit c; c.selection = {r.number(8), r.number(8)}; c.consumed = static_cast<uint32_t>(r.number(4));
    c.text = r.string(MaxText); c.remaining = r.string(MaxInput);
    if (!r.done() || !valid(c)) return false; out = std::move(c); return true;
}

inline bool identityValid(const std::string &s) {
    return s.size() == IdentitySize && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
inline std::vector<uint8_t> encodePayload(const Opened &o) {
    if (!identityValid(o.identity)) throw std::invalid_argument("invalid engine identity");
    std::vector<uint8_t> b; put(b, static_cast<uint16_t>(Payload::Opened), 2); putString(b, o.identity); return b;
}
inline bool decodePayload(const std::vector<uint8_t> &b, Opened &out) {
    PayloadReader r(b); if (r.number(2) != static_cast<uint16_t>(Payload::Opened)) return false;
    Opened o{r.string(IdentitySize)};
    if (!r.done() || !identityValid(o.identity)) return false; out = std::move(o); return true;
}

// One snapshot per context, on its owner thread. Caller must first validate frame
// ownership with ClientGate/server scheduling. This class does not deliver text.
class CandidateSnapshot {
    std::optional<Identity> source_;
    CandidatePage page_;
    bool active_ = false;
    static bool sameSession(const Identity &a, const Identity &b) {
        return a.connection == b.connection && a.engine == b.engine && a.session == b.session;
    }
public:
    void invalidate() { active_ = false; page_ = {}; } // keep high-water identity
    bool publish(const Frame &response) {
        CandidatePage next;
        if (response.kind != Kind::Reply || !valid(response) || !decodePayload(response.payload, next)) return false;
        if (source_ && (!sameSession(*source_, response.id) || response.id.composition < source_->composition ||
                        response.id.request <= source_->request)) return false;
        page_ = std::move(next); source_ = response.id; active_ = true; return true;
    }
    std::optional<Candidate> resolve(const Frame &request) const {
        Selection selection;
        if (!active_ || !source_ || request.kind != Kind::Select || !valid(request) ||
            !sameSession(*source_, request.id) || request.id.composition != source_->composition ||
            request.id.request <= source_->request || !decodePayload(request.payload, selection) ||
            selection.sourceRequest != source_->request) return {};
        for (const auto &word : page_.words) if (word.id == selection.candidate) return word;
        return {};
    }
    // Server-side pure preview. Execution/confirmation remains a separate phase.
    std::optional<Commit> preview(const Frame &request) const {
        const auto word = resolve(request); if (!word) return {};
        return Commit{{source_->request, word->id}, word->consumed, word->text, page_.input.substr(word->consumed)};
    }
    // Validate against the exact issued selection and displayed snapshot before
    // consuming it. Caller also checks ClientGate; only then enqueue to original context.
    bool validateConfirmation(const Frame &request, const Frame &response, Commit &out) const {
        const auto expected = preview(request); Commit actual;
        if (!expected || response.kind != Kind::Reply || !valid(response) || !(request.id == response.id) ||
            !decodePayload(response.payload, actual) || actual.selection.sourceRequest != expected->selection.sourceRequest ||
            actual.selection.candidate != expected->selection.candidate || actual.consumed != expected->consumed ||
            actual.text != expected->text || actual.remaining != expected->remaining) return false;
        out = std::move(actual); return true;
    }
    bool confirm(const Frame &request, const Frame &response, Commit &out) {
        Commit actual;
        if (!validateConfirmation(request, response, actual)) return false;
        source_ = response.id; invalidate(); out = std::move(actual); return true;
    }
};
} // namespace harukoto::engine
