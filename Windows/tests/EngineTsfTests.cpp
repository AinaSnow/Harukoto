// WIN-03 steps 6a/6b: the production text service with the standalone engine
// switch off and on (docs/ENGINE_TSF.md). Two Service objects load the same
// base snapshot, theme packages and personal table; one lists in-process,
// the other through the engine protocol (loopback ServerConnection +
// WindowsDictionary, or a real engine host process over its pipe).
#include "../TextService.cpp"
#include "../EngineWindows.h"
#include "../EngineProcess.h"
#include <psapi.h>
#include <cassert>
#include <csignal>
#include <iostream>
#include <map>
#include <sstream>

using namespace harukoto;
using engine::ServerConnection;
using engine::WindowsDictionary;
using engine::EngineProcess;
namespace ep = engine::pipe;

namespace {
quick_input::Moment fixedMoment() { return {2026, 11, 21, 9, 30, 0}; }

struct Data {
    std::filesystem::path base, themes, personal;
};

// The server side of the protocol in this process. Every frame goes through
// the wire encoding both ways. A fault hook may throw before the exchange.
class Loopback final : public tsf_engine::Transport {
public:
    std::shared_ptr<const WindowsDictionary> dictionary;
    ServerConnection server;
    std::function<void(const engine::Frame &)> fault;
    std::function<void()> onDestroy;
    size_t exchanges = 0;
    uint64_t sequence_ = 0;
    HANDLE revoke_ = nullptr;
    explicit Loopback(std::shared_ptr<const WindowsDictionary> value, uint64_t seq = 0, HANDLE revoke = nullptr)
        : dictionary(value), server(42, value, value->identity()), sequence_(seq), revoke_(revoke) {}
    ~Loopback() override { if (onDestroy) onDestroy(); }
    uint64_t sequence() const override { return sequence_; }
    HANDLE revokeEvent() const override { return revoke_; }
    static engine::Frame wire(const engine::Frame &frame) {
        const auto bytes = engine::encode(frame);
        engine::Frame out; size_t used = 0;
        if (engine::decode(bytes.data(), bytes.size(), out, used) != engine::Decode::Complete || used != bytes.size())
            throw std::runtime_error("wire");
        return out;
    }
    engine::Frame exchange(const engine::Frame &request, uint64_t deadline) override {
        ++exchanges;
        if (fault) fault(request);
        auto result = server.receive(wire(request), ep::now(), deadline);
        if (result.status == ServerConnection::Status::Scheduled) {
            const auto page = dictionary->query(result.work);
            result = server.complete(result.work, page, ep::now());
        }
        if (!result.response) throw std::runtime_error("loopback rejected");
        return wire(*result.response);
    }
};

struct EngineTsfTest {
    static std::unique_ptr<Service> service(const Data &data, bool simplified, const std::filesystem::path &memory = {}) {
        auto s = std::make_unique<Service>();
        if (!memory.empty()) s->memory = std::make_unique<selection_memory::Store>(memory);
        s->simplified = simplified;
        s->clock = &fixedMoment;
        s->baseSnapshot = base_dictionary::Snapshot::load(data.base);
        s->session = s->baseSnapshot->session(simplified).release();
        auto personal = readPersonalDictionary(data.personal);
        assert(personal);
        auto themes = readJapanesePackageSnapshot(data.themes);
        auto words = s->baseSnapshot->words();
        const auto base = words;
        words.insert(words.end(), personal->words.begin(), personal->words.end());
        if (themes) words.insert(words.end(), themes->words.begin(), themes->words.end());
        std::stable_sort(words.begin(), words.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        s->japaneseWords = std::move(words);
        s->themeReload = std::make_unique<ThemeReload>(data.themes, base, themes ? themes->generation : std::string(),
            data.personal, personal, std::filesystem::path());
        s->engineReleaseProbe = [] { return engine::release::Evaluation{}; };
        return s;
    }
    // WIN-03 step 7d: thin-client service with only the 35-byte stub lexicon
    // and ~4 KiB conjugation table (zero base snapshot, SQLite or theme words).
    static std::unique_ptr<Service> thinService(const Data &data, bool simplified, const std::filesystem::path &memory = {}) {
        auto s = std::make_unique<Service>();
        if (!memory.empty()) s->memory = std::make_unique<selection_memory::Store>(memory);
        s->simplified = simplified;
        s->clock = &fixedMoment;
        s->installFolder = data.base;
        s->conjugations.loadFile(data.base / L"japanese-conjugations.bin");
        s->lexicon = Service::createStubLexicon();
        s->session = mpn_session_create(s->lexicon);
        assert(s->session);
        mpn_set_simplified(s->session, simplified);
        s->localLexiconLoaded = false;
        s->engineClient.stats.localLexicon = false;
        s->engineReleaseProbe = [] { return engine::release::Evaluation{}; };
        return s;
    }
    static bool same(const WindowsCandidate &a, const WindowsCandidate &b) {
        return a.text == b.text && a.coreIndex == b.coreIndex && a.annotation == b.annotation && a.japanese == b.japanese &&
            a.strong == b.strong && a.directJapanese == b.directJapanese && a.generated == b.generated;
    }
    static bool same(const std::vector<WindowsCandidate> &a, const std::vector<WindowsCandidate> &b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto &x, const auto &y) { return same(x, y); });
    }
    static void report(const std::string &what, const std::string &input, const Service &a, const Service &b) {
        std::cerr << what << " input=" << input << " in-process=" << a.choices.size() << " engine=" << b.choices.size() << '\n';
        for (size_t i = 0; i < std::max(a.choices.size(), b.choices.size()) && i < 20; ++i)
            std::cerr << "  " << i << ' ' << (i < a.choices.size() ? a.choices[i].text : "-") << " | "
                      << (i < b.choices.size() ? b.choices[i].text : "-") << '\n';
    }
    // Loopback link factory over one shared immutable dictionary.
    static tsf_engine::Client::Factory loopback(std::shared_ptr<const WindowsDictionary> dictionary, Loopback **last = nullptr) {
        return [dictionary, last] {
            return tsf_engine::Link::manual([dictionary, last](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                auto transport = std::make_unique<Loopback>(dictionary);
                if (last) *last = transport.get();
                return transport;
            });
        };
    }
    static void type(Service &s, const std::string &input, bool engine) {
        mpn_clear(s.session); s.refreshCandidates(engine);
        for (char c : input) { assert(mpn_key(s.session, c)); s.refreshCandidates(engine); }
    }
    static void serve(Service &s) {
        if (auto *link = s.engineClient.link()) link->pump();
        s.deliverEngine();
    }

    // Switch off (default): no link, no factory call, the same list.
    static void offPath(const Data &data, std::shared_ptr<const WindowsDictionary> dictionary) {
        HKEY key = nullptr;
        const std::wstring path = L"Software\\HarukotoEngineTsf-" + std::to_wstring(GetCurrentProcessId());
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS);
        HKEY input = nullptr;
        assert(RegCreateKeyExW(key, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &input, nullptr) == ERROR_SUCCESS);
        // Absent or of another type: the default (on in the product, off in tests).
        assert(!tsf_engine::readStandaloneEngine(key));
        tsf_engine::standaloneEngineDefault = true;
        assert(tsf_engine::readStandaloneEngine(key));
        DWORD one = 1, zero = 0;
        assert(RegSetValueExW(input, L"StandaloneEngine", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof(zero)) == ERROR_SUCCESS);
        assert(!tsf_engine::readStandaloneEngine(key));
        assert(RegSetValueExW(input, L"StandaloneEngine", 0, REG_SZ, reinterpret_cast<const BYTE *>(L"0"), 4) == ERROR_SUCCESS);
        assert(tsf_engine::readStandaloneEngine(key));
        tsf_engine::standaloneEngineDefault = false;
        assert(!tsf_engine::readStandaloneEngine(key));
        assert(RegSetValueExW(input, L"StandaloneEngine", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one), sizeof(one)) == ERROR_SUCCESS);
        assert(tsf_engine::readStandaloneEngine(key));
        RegCloseKey(input); RegCloseKey(key);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);

        auto a = service(data, true), b = service(data, true);
        bool called = false;
        b->engineClient.factory = [&] { called = true; return std::unique_ptr<tsf_engine::Link>(); };
        for (const std::string input : {"nihao", "riben", "zhongguo"}) {
            type(*a, input, false); type(*b, input, true);
            assert(!b->engineClient.pending() && !b->engineClient.link() && same(a->choices, b->choices));
        }
        assert(!called && std::string(b->engineClient.stats.path) == "off" && !b->engineClient.stats.served);
        assert(b->engineClient.diagnosticFields() ==
            ",\"engine_path\":\"off\",\"engine_served\":0,\"engine_fallbacks\":0,\"engine_reason\":\"none\",\"engine_memory_behind\":0"
            ",\"engine_sequence\":0,\"engine_cutovers\":0,\"engine_cutover_failures\":0,\"engine_incompatible\":false"
            ",\"engine_yields\":0,\"engine_reclaimed\":0,\"engine_local_lexicon\":true");
        // Step 7a: when off, upgrade probe is never invoked.
        bool probed = false;
        b->engineReleaseProbe = [&] { probed = true; return engine::release::Evaluation{}; };
        assert(!b->pollEngineUpgrade(10000, true) && !probed);
        (void)dictionary;
        std::cout << "off path: identical, no link" << std::endl;
    }

    // Asynchronous delivery, stale replies, a key while pending, timeouts,
    // sticky fallback, identity and capability gates.
    static void behaviour(const Data &data, std::shared_ptr<const WindowsDictionary> dictionary,
                          std::shared_ptr<const WindowsDictionary> other) {
        auto a = service(data, true), b = service(data, true);
        Loopback *transport = nullptr;
        b->engineClient.enabled = true;
        b->engineClient.factory = loopback(dictionary, &transport);
        // Generation: a reply for "ni" arrives after "nih" was typed.
        type(*a, "nih", false);
        mpn_clear(b->session);
        assert(mpn_key(b->session, 'n')); b->refreshCandidates(true); assert(b->engineClient.pending() && b->choices.empty());
        // While an engine reply is in flight, updatePopup() keeps the previous
        // visual frame instead of flashing an empty candidate list.
        b->popupIndices = {0, 1, 2};
        b->updatePopup();
        assert(b->popupIndices == (std::vector<size_t>{0, 1, 2}) && !b->popup);
        serve(*b); assert(!b->engineClient.pending() && !b->choices.empty());
        assert(mpn_key(b->session, 'i')); b->refreshCandidates(true);
        b->engineClient.link()->pump();               // reply for "ni" is waiting
        assert(mpn_key(b->session, 'h')); b->refreshCandidates(true); // newer ticket
        assert(!b->deliverEngine() && b->engineClient.pending() && b->engineClient.stats.stale == 1);
        serve(*b); assert(!b->engineClient.pending() && same(a->choices, b->choices));
        // Latest request wins: two keys before the worker runs cost one reply
        // and exactly one Set exchange (never Cancel + N-key replay).
        type(*a, "nihao", false);
        const auto exchangesBeforeJump = transport->exchanges;
        assert(mpn_key(b->session, 'a')); b->refreshCandidates(true);
        assert(mpn_key(b->session, 'o')); b->refreshCandidates(true);
        serve(*b);
        assert(!b->engineClient.pending() && same(a->choices, b->choices));
        assert(transport->exchanges == exchangesBeforeJump + 1);
        // A key that reads the candidates while they are owed: computed here.
        type(*a, "zhongwen", false); type(*b, "zhongwe", true); serve(*b);
        assert(mpn_key(b->session, 'n')); b->refreshCandidates(true); assert(b->engineClient.pending());
        b->settleEngine(VK_DOWN);
        assert(!b->engineClient.pending() && same(a->choices, b->choices) && b->engineClient.stats.reason == "pending_key");
        // Expansion continues from an engine list exactly as in-process.
        type(*a, "zhongguo", false); type(*b, "zhongguo", true); serve(*b);
        assert(std::string(b->engineClient.stats.path) == "engine" && same(a->choices, b->choices));
        for (int i = 0; i < 3; ++i) { a->expandCandidates(); b->expandCandidates(); assert(same(a->choices, b->choices)); }

        // Timeout reported by the worker: in-process now, again until the boundary.
        transport->fault = [](const engine::Frame &) { throw tsf_engine::Timeout(); };
        type(*a, "riben", false);
        mpn_clear(b->session); b->refreshCandidates(true);
        for (char c : std::string("riben")) { assert(mpn_key(b->session, c)); b->refreshCandidates(true); if (b->engineClient.pending()) serve(*b); }
        assert(same(a->choices, b->choices) && b->engineClient.stats.reason == "fallback" && b->engineClient.stats.failures == 1);
        assert(!b->engineClient.link());
        // Overdue on the TSF side (no reply at all): the same.
        type(*b, "", true); // Boundary: a new link is made on the next reading.
        assert(mpn_key(b->session, 'n')); b->refreshCandidates(true); assert(b->engineClient.pending());
        Sleep(static_cast<DWORD>(tsf_engine::DeadlineMs + 50));
        assert(b->deliverEngine() && !b->engineClient.pending() && b->engineClient.stats.reason == "timeout");
        assert(b->engineClient.stats.failures == 2 && !b->choices.empty());
        // A third consecutive failure: left alone for StickyMs.
        type(*b, "", true);
        assert(mpn_key(b->session, 'n')); b->refreshCandidates(true); assert(b->engineClient.pending());
        transport->fault = [](const engine::Frame &) { throw std::runtime_error("broken pipe"); };
        serve(*b); assert(b->engineClient.stats.reason == "unavailable" && b->engineClient.stats.failures == 3);
        type(*b, "", true); type(*b, "ni", true);
        assert(!b->engineClient.pending() && b->engineClient.stats.reason == "sticky");

        // Identity: an engine serving other data is never used.
        auto c = service(data, false);
        c->engineClient.enabled = true; c->engineClient.factory = loopback(other);
        type(*c, "riben", true);
        assert(!c->engineClient.pending() && c->engineClient.stats.reason == "identity" && !c->choices.empty());
        // Step 6b: fuzzy pinyin and the typo switch travel with the request.
        Loopback *cTransport = nullptr;
        c->engineClient.close();
        c->engineClient.factory = loopback(dictionary, &cTransport);
        auto d = service(data, false);
        type(*c, "nihao", true); serve(*c); assert(std::string(c->engineClient.stats.path) == "engine");
        mpn_set_fuzzy_pinyin(c->session, 1); mpn_set_fuzzy_pinyin(d->session, 1);
        type(*c, "nihao", true); serve(*c); type(*d, "nihao", false);
        assert(std::string(c->engineClient.stats.path) == "engine" && same(c->choices, d->choices));
        mpn_set_fuzzy_pinyin(c->session, 0); mpn_set_fuzzy_pinyin(d->session, 0);
        mpn_set_typo_correction(c->session, 0); mpn_set_typo_correction(d->session, 0);
        type(*c, "nihoa", true); serve(*c); type(*d, "nihoa", false);
        assert(std::string(c->engineClient.stats.path) == "engine" && same(c->choices, d->choices));
        mpn_set_typo_correction(c->session, 1); mpn_set_typo_correction(d->session, 1);
        // Learned state the engine cannot read (no memory file given): "behind".
        c->memoryEnabled = true; c->personalHour = 1;
        c->memory = std::make_unique<selection_memory::Store>(std::filesystem::temp_directory_path() / L"harukoto-engine-tsf-absent" / L"m.tsv");
        type(*c, "nihao", true); serve(*c);
        assert(c->engineClient.stats.reason == "memory_behind" && c->engineClient.stats.memoryBehind == 1 && !c->choices.empty());
        type(*c, "nihao", true); // Known behind for this state: not asked again.
        assert(!c->engineClient.pending() && c->engineClient.stats.reason == "memory_behind" && c->engineClient.stats.memoryBehind == 1);
        c->memoryEnabled = false; c->memory.reset();
        type(*c, "nihao", true); serve(*c);
        std::wstring text; const auto split = std::find_if(c->choices.begin(), c->choices.end(), [&](const auto &x) {
            return x.coreIndex && mpn_candidate_consumed(c->session, *x.coreIndex) < 5; });
        if (split != c->choices.end()) {
            // Step 6c: the rest after a chosen segment, with its context, through the engine.
            const auto index = static_cast<size_t>(split - c->choices.begin());
            type(*d, "nihao", false);
            std::wstring other;
            assert(c->chooseCandidate(index, text) && d->chooseCandidate(index, other));
            text += wide(mpn_committed(c->session)); other += wide(mpn_committed(d->session));
            mpn_clear_commit(c->session); mpn_clear_commit(d->session);
            assert(text == other);
            const auto exchangesBeforePartial = cTransport->exchanges;
            c->refreshCandidates(true); d->refreshCandidates();
            assert(c->engineClient.pending()); serve(*c);
            assert(std::string(c->engineClient.stats.path) == "engine" && same(c->choices, d->choices));
            assert(cTransport->exchanges == exchangesBeforePartial + 1);
        }
        // rebuild() refuses what it cannot show exactly.
        type(*a, "nihao", false);
        engine::Candidate wrong{1, 5, "错", "", 1, 0};
        assert(!tsf_engine::rebuild(a->session, {wrong}));
        engine::Candidate gap{2, 5, "nihao", "", 0, 0};
        assert(!tsf_engine::rebuild(a->session, {gap}));
        const auto none = tsf_engine::rebuild(a->session, {}); assert(none && none->empty());
        std::cout << "behaviour: async, stale, pending key, timeout, sticky, identity, gates" << std::endl;
    }

    // WIN-03 step 7a: versioned cutover at empty boundaries, async standby
    // pre-connect while old engine serves, keep-old-on-failure with cooldown,
    // and explicit protocol-mismatch notice without silent mixing.
    static void cutover(const Data &data, std::shared_ptr<const WindowsDictionary> dictionary) {
        using engine::release::Compatibility;
        using engine::release::Entry;
        using engine::release::Evaluation;
        auto makeMatch = [](uint64_t seq, uint16_t proto = engine::ProtocolVersion) {
            Evaluation ev; ev.kind = Compatibility::Match; ev.latestProtocol = proto; ev.latestSequence = seq;
            Entry e; e.id = L"V" + std::to_wstring(seq); e.sequence = seq; e.protocol = proto; ev.selected = e;
            return ev;
        };
        auto a = service(data, true), b = service(data, true);
        b->engineClient.enabled = true;
        bool v1Closed = false;
        size_t v1Exchanges = 0, v2Exchanges = 0, v3Attempts = 0;
        Loopback *v1 = nullptr, *v2 = nullptr;
        b->engineClient.factory = [&] {
            return tsf_engine::Link::manual([&](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                auto t = std::make_unique<Loopback>(dictionary, 1);
                t->onDestroy = [&] { v1Closed = true; };
                v1 = t.get();
                return t;
            });
        };
        Evaluation currentEval = makeMatch(1);
        b->engineReleaseProbe = [&] { return currentEval; };

        // Start on v1 (sequence 1).
        type(*a, "nihao", false); type(*b, "nihao", true); serve(*b);
        assert(std::string(b->engineClient.stats.path) == "engine" && same(a->choices, b->choices));
        assert(b->engineClient.stats.sequence == 1 && b->engineClient.stats.cutovers == 0);
        v1Exchanges = v1->exchanges; assert(v1Exchanges > 0);

        // Mid-composition: v2 appears, must NOT pre-connect or switch while typing.
        currentEval = makeMatch(2);
        ep::Handle gateV2(CreateEventW(nullptr, TRUE, FALSE, nullptr)); assert(gateV2);
        ep::Handle notifyV2(CreateEventW(nullptr, FALSE, FALSE, nullptr)); assert(notifyV2);
        b->engineClient.sequenceFactory = [&](uint64_t seq) -> std::unique_ptr<tsf_engine::Link> {
            if (seq == 2) {
                return tsf_engine::Link::threaded([&, gate = gateV2.get()](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                    assert(WaitForSingleObject(gate, 10000) == WAIT_OBJECT_0);
                    auto t = std::make_unique<Loopback>(dictionary, 2);
                    v2 = t.get();
                    return t;
                }, [ev = notifyV2.get()] { SetEvent(ev); });
            }
            if (seq == 3) {
                ++v3Attempts;
                return tsf_engine::Link::manual([](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                    throw std::runtime_error("broken v3 engine");
                });
            }
            return {};
        };
        assert(!b->dictionaryBoundary());
        assert(!b->pollEngineUpgrade(10000, true));
        assert(!b->engineClient.standby() && b->engineClient.link()->sequence() == 1);

        // Empty boundary: standby starts connecting to v2 asynchronously while v1 keeps serving.
        mpn_clear(b->session); b->resetCandidates();
        assert(b->dictionaryBoundary());
        assert(!b->pollEngineUpgrade(10000, true)); // Connecting, not swapped yet.
        assert(b->engineClient.standby() && b->engineClient.standby()->state() == tsf_engine::Link::State::Connecting);
        assert(!v1Closed && b->engineClient.link()->sequence() == 1);

        // While v2 is still connecting, v1 serves another composition cleanly.
        type(*a, "zhongwen", false); type(*b, "zhongwen", true); serve(*b);
        assert(same(a->choices, b->choices) && v1->exchanges > v1Exchanges && !v1Closed);

        // Let v2 finish connecting; mid-composition must STILL hold off the swap!
        assert(SetEvent(gateV2.get()));
        assert(WaitForSingleObject(notifyV2.get(), 10000) == WAIT_OBJECT_0);
        assert(b->engineClient.standby()->state() == tsf_engine::Link::State::Ready);
        b->onEngineReply();
        assert(b->engineClient.standby() && !v1Closed && b->engineClient.stats.cutovers == 0);

        // Reach empty boundary: onEngineReply atomically swaps to v2 and closes v1.
        mpn_clear(b->session); b->resetCandidates();
        assert(b->dictionaryBoundary());
        b->onEngineReply();
        assert(!b->engineClient.standby() && v1Closed);
        assert(b->engineClient.link() && b->engineClient.link()->sequence() == 2);
        assert(b->engineClient.stats.sequence == 2 && b->engineClient.stats.cutovers == 1 && !b->engineClient.stats.cutoverFailures);

        // Next composition is served by v2 (threaded link).
        type(*a, "riben", false); type(*b, "riben", true);
        assert(b->engineClient.pending());
        const auto deadline = GetTickCount64() + 5000;
        while (b->engineClient.pending() && GetTickCount64() < deadline) {
            WaitForSingleObject(notifyV2.get(), 200);
            b->deliverEngine();
        }
        assert(!b->engineClient.pending() && same(a->choices, b->choices) && std::string(b->engineClient.stats.path) == "engine");
        v2Exchanges = v2->exchanges; assert(v2Exchanges > 0);

        // Failing v3: standby fails, v2 is kept untouched, cooldown suppresses immediate retry.
        mpn_clear(b->session); b->resetCandidates();
        currentEval = makeMatch(3);
        assert(!b->pollEngineUpgrade(20000, true));
        assert(v3Attempts == 1 && !b->engineClient.standby());
        assert(b->engineClient.stats.cutoverFailures == 1 && b->engineClient.stats.cutovers == 1);
        assert(b->engineClient.link() && b->engineClient.link()->sequence() == 2);
        // Within 60 s cooldown: does not retry v3.
        assert(!b->pollEngineUpgrade(25000, true) && v3Attempts == 1);
        // v2 still serves cleanly after v3 failed.
        type(*a, "nihao", false); type(*b, "nihao", true);
        while (b->engineClient.pending() && GetTickCount64() < deadline + 5000) {
            WaitForSingleObject(notifyV2.get(), 200);
            b->deliverEngine();
        }
        assert(!b->engineClient.pending() && same(a->choices, b->choices) && v2->exchanges > v2Exchanges);

        // Protocol mismatch (v4, protocol 2): marks incompatible, clears standby, never mixes.
        mpn_clear(b->session); b->resetCandidates();
        Evaluation mismatch; mismatch.kind = Compatibility::ProtocolMismatch; mismatch.latestProtocol = engine::ProtocolVersion + 1; mismatch.latestSequence = 4;
        currentEval = mismatch;
        assert(!b->pollEngineUpgrade(90000, true));
        assert(b->engineClient.stats.incompatible && !b->engineClient.standby());
        const auto diag = b->engineClient.diagnosticFields();
        assert(diag.find("\"engine_sequence\":2") != std::string::npos);
        assert(diag.find("\"engine_cutovers\":1") != std::string::npos);
        assert(diag.find("\"engine_cutover_failures\":1") != std::string::npos);
        assert(diag.find("\"engine_incompatible\":true") != std::string::npos);

        // Fresh service with ProtocolMismatch: never invokes factory, stays in-process with reason "incompatible".
        auto fresh = service(data, true);
        bool freshFactoryCalled = false;
        fresh->engineClient.enabled = true;
        fresh->engineClient.factory = [&] { freshFactoryCalled = true; return std::unique_ptr<tsf_engine::Link>(); };
        fresh->engineReleaseProbe = [&] { return mismatch; };
        assert(!fresh->pollEngineUpgrade(10000, true) && fresh->engineClient.stats.incompatible);
        type(*fresh, "nihao", true);
        assert(!freshFactoryCalled && !fresh->engineClient.link() && fresh->engineClient.stats.reason == "incompatible" && same(a->choices, fresh->choices));

        b->engineClient.close();
        std::cout << "cutover: boundary swap, async standby, keep-old-on-failure, protocol mismatch" << std::endl;
    }

    // WIN-03 step 7b: voluntary focus-loss/idle slot yield and graceful
    // reclamation (revokeEvent) without failure penalty or sticky fallback.
    static void slotLifecycle(const Data &data, std::shared_ptr<const WindowsDictionary> dictionary) {
        auto a = service(data, true), b = service(data, true);
        b->engineClient.enabled = true;
        b->engineClient.factory = loopback(dictionary);
        type(*a, "nihao", false); type(*b, "nihao", true); serve(*b);
        assert(b->engineClient.link() && std::string(b->engineClient.stats.path) == "engine" && same(a->choices, b->choices));

        // Mid-composition: idle timer must NEVER yield the slot.
        assert(!b->dictionaryBoundary());
        assert(!b->onEngineIdleTimer() && b->engineClient.link() && !b->engineClient.stats.yields);

        // Focus loss clears composition; idle timer then yields the slot cleanly with 0 failures.
        b->OnSetFocus(FALSE);
        assert(!b->engineFocused && b->dictionaryBoundary());
        assert(b->onEngineIdleTimer() && !b->engineClient.link());
        assert(b->engineClient.stats.yields == 1 && !b->engineClient.stats.failures);

        // Next typing reconnects cleanly and serves from engine with 0 failures.
        type(*b, "zhongwen", true); serve(*b);
        type(*a, "zhongwen", false);
        assert(same(a->choices, b->choices) && std::string(b->engineClient.stats.path) == "engine" && !b->engineClient.stats.failures);

        // Manual link: 5 consecutive idle reclaims (exceeding StickyFailures=3)
        // never count as failures and never trigger sticky fallback.
        mpn_clear(b->session); b->resetCandidates(); b->engineClient.close();
        ep::Handle revoke(CreateEventW(nullptr, TRUE, FALSE, nullptr)); assert(revoke);
        size_t created = 0;
        b->engineClient.factory = [&] {
            ++created;
            return tsf_engine::Link::manual([&](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                return std::make_unique<Loopback>(dictionary, 1, revoke.get());
            });
        };
        type(*a, "riben", false);
        for (int i = 0; i < 5; ++i) {
            assert(ResetEvent(revoke.get()));
            type(*b, "riben", true); serve(*b);
            assert(std::string(b->engineClient.stats.path) == "engine" && same(a->choices, b->choices));
            mpn_clear(b->session); b->resetCandidates();
            assert(SetEvent(revoke.get()));
            assert(b->engineClient.link()->state() == tsf_engine::Link::State::Reclaimed);
        }
        assert(ResetEvent(revoke.get()));
        type(*b, "riben", true); serve(*b);
        assert(std::string(b->engineClient.stats.path) == "engine" && same(a->choices, b->choices));
        assert(b->engineClient.stats.reclaimed == 5 && !b->engineClient.stats.failures && created == 6);

        // Threaded link: signaling revokeEvent immediately wakes the worker thread,
        // destroys the transport (closing the EngineHost pipe), and enters Reclaimed.
        mpn_clear(b->session); b->resetCandidates(); b->engineClient.close();
        ep::Handle tRevoke(CreateEventW(nullptr, TRUE, FALSE, nullptr)); assert(tRevoke);
        ep::Handle tNotify(CreateEventW(nullptr, FALSE, FALSE, nullptr)); assert(tNotify);
        ep::Handle tDestroyed(CreateEventW(nullptr, TRUE, FALSE, nullptr)); assert(tDestroyed);
        auto tLink = tsf_engine::Link::threaded([&](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
            auto t = std::make_unique<Loopback>(dictionary, 1, tRevoke.get());
            t->onDestroy = [ev = tDestroyed.get()] { SetEvent(ev); };
            return t;
        }, [ev = tNotify.get()] { SetEvent(ev); });
        assert(tLink && WaitForSingleObject(tNotify.get(), 5000) == WAIT_OBJECT_0);
        assert(tLink->state() == tsf_engine::Link::State::Ready);
        assert(SetEvent(tRevoke.get()));
        assert(WaitForSingleObject(tDestroyed.get(), 5000) == WAIT_OBJECT_0);
        assert(WaitForSingleObject(tNotify.get(), 5000) == WAIT_OBJECT_0 || tLink->state() == tsf_engine::Link::State::Reclaimed);
        assert(tLink->state() == tsf_engine::Link::State::Reclaimed);
        tLink.reset();

        const auto diag = b->engineClient.diagnosticFields();
        assert(diag.find("\"engine_yields\":1") != std::string::npos);
        assert(diag.find("\"engine_reclaimed\":5") != std::string::npos);
        b->engineClient.close();
        std::cout << "slot lifecycle: focus yield, immediate threaded revoke teardown, zero-penalty reclaim" << std::endl;
    }

    // WIN-03 step 7c/7d: thin-client mode (zero startup base-lexicon load),
    // deferred connect without fallback, bounded sync wait on selection keys,
    // engine-owned expansion/selection/typo/pick/forget, lazy fallback and memory.
    static void thinClient(const Data &data, std::shared_ptr<const WindowsDictionary> dictionary) {
        auto privateBytes = []() -> size_t {
            PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
            assert(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc)));
            return pmc.PrivateUsage;
        };
        const size_t beforeThin = privateBytes();
        auto thin = thinService(data, true);
        const size_t afterThin = privateBytes();
        const double thinDeltaMiB = afterThin > beforeThin ? double(afterThin - beforeThin) / (1024.0 * 1024.0) : 0.0;
        assert(!thin->localLexiconLoaded && !thin->engineClient.stats.localLexicon && thinDeltaMiB < 10.0);
        assert(thin->engineClient.diagnosticFields().find("\"engine_local_lexicon\":false") != std::string::npos);

        auto ref = service(data, true);
        thin->engineClient.enabled = true;

        // 1. Connecting state in thin-client mode defers instead of loading 185 MiB local lexicon.
        ep::Handle gate(CreateEventW(nullptr, TRUE, FALSE, nullptr)); assert(gate);
        ep::Handle notify(CreateEventW(nullptr, FALSE, FALSE, nullptr)); assert(notify);
        thin->engineClient.factory = [&] {
            return tsf_engine::Link::threaded([&, g = gate.get()](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                assert(WaitForSingleObject(g, 10000) == WAIT_OBJECT_0);
                return std::make_unique<Loopback>(dictionary, 1);
            }, [ev = notify.get()] { SetEvent(ev); });
        };
        mpn_clear(thin->session);
        assert(mpn_key(thin->session, 'n')); thin->refreshCandidates(true);
        assert(mpn_key(thin->session, 'i')); thin->refreshCandidates(true);
        assert(thin->engineDeferred && !thin->localLexiconLoaded && thin->choices.empty());

        // Release connect gate; onEngineReply sends the deferred query without loading local lexicon.
        assert(SetEvent(gate.get()));
        assert(WaitForSingleObject(notify.get(), 10000) == WAIT_OBJECT_0);
        thin->onEngineReply();
        assert(!thin->engineDeferred && thin->engineClient.pending() && !thin->localLexiconLoaded);

        // 2. Selection key (Space) while reply is in flight waits synchronously instead of falling back.
        thin->settleEngine(VK_SPACE);
        type(*ref, "ni", false);
        assert(!thin->engineClient.pending() && !thin->localLexiconLoaded);
        assert(same(ref->choices, thin->choices) && std::string(thin->engineClient.stats.path) == "engine" && !thin->engineClient.stats.fallbacks);

        // 3. Typo flag, multi-round expansion (with no local lexicon), and candidateReading match in-process.
        type(*ref, "nihoa", false);
        mpn_clear(thin->session); thin->resetCandidates();
        for (char c : std::string("nihoa")) { assert(mpn_key(thin->session, c)); thin->refreshCandidates(true); }
        thin->settleEngine(VK_SPACE);
        assert(!thin->localLexiconLoaded && same(ref->choices, thin->choices));
        for (size_t i = 0; i < ref->choices.size(); ++i)
            assert(typoCandidate(ref->session, ref->choices[i]) == typoCandidate(thin->session, thin->choices[i]));

        type(*ref, "zhongguo", false);
        mpn_clear(thin->session); thin->resetCandidates();
        for (char c : std::string("zhongguo")) { assert(mpn_key(thin->session, c)); thin->refreshCandidates(true); }
        thin->settleEngine(VK_SPACE);
        assert(same(ref->choices, thin->choices) && thin->canExpandCandidates() == (mpn_candidates_can_expand(ref->session) != 0));
        for (int k = 0; k < 3; ++k) {
            ref->expandCandidates(); thin->expandCandidates();
            assert(same(ref->choices, thin->choices) && !thin->localLexiconLoaded);
        }
        assert(!thin->canExpandCandidates());

        // 4. True fallback (switch off / unavailable) lazily loads local lexicon and serves exact full candidates.
        thin->engineClient.close();
        thin->engineClient.enabled = false;
        mpn_clear(thin->session); thin->resetCandidates();
        for (char c : std::string("nihao")) { assert(mpn_key(thin->session, c)); thin->refreshCandidates(true); }
        type(*ref, "nihao", false);
        assert(thin->localLexiconLoaded && thin->engineClient.stats.localLexicon);
        assert(!thin->choices.empty() && thin->choices[0].text == ref->choices[0].text);
        std::cout << "thin client: delta=" << thinDeltaMiB << " MiB (<10 MiB), deferred connect, sync settle, expansion/typo, lazy fallback" << std::endl;
    }

    // Step 6b: the per-key preparation OnKeyDown does before a key: the
    // memory switch, polling, the personal table, fuzzy pinyin and typo switch.
    struct Scenario { std::string name; uint32_t fuzzy = 0; bool typo = true, memory = false, learn = false; };
    static void prepare(Service &s, const Scenario &scenario) {
        s.engineChosenWords = false; // As at the start of OnKeyDown.
        s.memoryEnabled = s.memory && scenario.memory;
        if (s.memoryEnabled) s.memory->poll(GetTickCount64(), 0);
        if (s.memory) s.memory->pollUserWords(GetTickCount64(), 0);
        s.attachPersonal();
        applyFuzzyPinyin(s.session, scenario.fuzzy);
        applyTypoCorrection(s.session, scenario.typo);
    }
    struct Result {
        size_t prefixes = 0, segments = 0, served = 0, chosen = 0, learned = 0, picks = 0, f6 = 0, quick = 0, forgets = 0;
        std::map<std::string, size_t> fallbacks;
    };
    // Who listed b's candidates last, and that they equal a's.
    static void check(Service &a, Service &b, const Scenario &scenario, const std::string &what, uint64_t servedBefore, Result &result) {
        if (b.engineClient.pending()) serve(b);
        if (b.engineClient.stats.served > servedBefore) ++result.served;
        else if (*mpn_input(b.session)) ++result.fallbacks[b.engineClient.stats.reason];
        if (!same(a.choices, b.choices)) { report("MISMATCH " + scenario.name + " " + what, mpn_input(a.session), a, b); assert(false); }
        if (b.engineClient.stats.failures) {
            std::cerr << "FAILURE " << scenario.name << " " << what << " input=" << mpn_input(a.session) << " reason=" << b.engineClient.stats.reason << std::endl;
            // What a fresh session with the same context and keys lists, beside a's.
            auto fresh = a.baseSnapshot->session(a.simplified);
            mpn_set_fuzzy_pinyin(fresh.get(), mpn_fuzzy_pinyin(a.session)); mpn_set_typo_correction(fresh.get(), mpn_typo_correction(a.session));
            mpn_set_segment_context(fresh.get(), mpn_segment_context(a.session));
            for (const char *c = mpn_input(a.session); *c; ++c) mpn_key(fresh.get(), *c);
            std::cerr << "context a=" << mpn_segment_context(a.session) << " fresh=" << mpn_segment_context(fresh.get())
                      << " count a=" << mpn_candidate_count(a.session) << " fresh=" << mpn_candidate_count(fresh.get()) << std::endl;
            for (size_t i = 0; i < 12 && (i < mpn_candidate_count(a.session) || i < mpn_candidate_count(fresh.get())); ++i)
                std::cerr << "  " << i << " " << (i < mpn_candidate_count(a.session) ? mpn_candidate(a.session, i) : "-") << " | "
                          << (i < mpn_candidate_count(fresh.get()) ? mpn_candidate(fresh.get(), i) : "-") << std::endl;
            assert(false);
        }
    }
    // Types one reading on both services key by key and compares every list.
    static void compare(Service &a, Service &b, const Scenario &scenario, const std::string &input, Result &result) {
        prepare(a, scenario); prepare(b, scenario);
        mpn_clear(a.session); mpn_clear(b.session); a.resetCandidates(); b.resetCandidates();
        for (size_t i = 0; i < input.size(); ++i) {
            prepare(a, scenario); prepare(b, scenario);
            assert(mpn_key(a.session, input[i]) && mpn_key(b.session, input[i]));
            a.refreshCandidates(); const auto before = b.engineClient.stats.served; b.refreshCandidates(true);
            ++result.prefixes;
            check(a, b, scenario, "typing", before, result);
        }
    }
    // Step 6c: a choose sequence. The first choice is rank `first`, each
    // later segment rank (segment % 3), capped by the list; every remaining
    // reading is listed again (through the engine on b) and compared.
    static void finish(Service &a, Service &b, const Scenario &scenario, size_t first, Result &result) {
        std::wstring ta, tb;
        for (size_t segment = 0; *mpn_input(a.session); ++segment) {
            if (a.choices.empty()) break;
            const size_t pick = (std::min)(segment ? segment % 3 : first, a.choices.size() - 1);
            if (!scenario.learn) { a.memoryEnabled = b.memoryEnabled = false; a.attachPersonal(); b.attachPersonal(); } // As at a key with learning off.
            assert(a.chooseCandidate(pick, ta) == b.chooseCandidate(pick, tb));
            ta += wide(mpn_committed(a.session)); tb += wide(mpn_committed(b.session));
            mpn_clear_commit(a.session); mpn_clear_commit(b.session);
            assert(ta == tb && std::string(mpn_input(a.session)) == mpn_input(b.session));
            assert(std::string(mpn_segment_context(a.session)) == mpn_segment_context(b.session));
            a.refreshCandidates(); const auto before = b.engineClient.stats.served; b.refreshCandidates(true);
            if (*mpn_input(a.session)) ++result.segments;
            check(a, b, scenario, "segment", before, result);
            prepare(a, scenario); prepare(b, scenario);
        }
        ++result.chosen;
    }
    // 以词定字: [ or ] on rank `index`, then the rest is listed again.
    static void pick(Service &a, Service &b, const Scenario &scenario, size_t index, bool last, Result &result) {
        if (index >= a.choices.size() || !a.choices[index].coreIndex) return;
        std::wstring ta, tb;
        if (!scenario.learn) { a.memoryEnabled = b.memoryEnabled = false; a.attachPersonal(); b.attachPersonal(); } // As at a key with learning off.
        const auto before = b.engineClient.stats.served;
        assert(a.pickCharacter(index, last, ta) == b.pickCharacter(index, last, tb));
        assert(ta == tb && std::string(mpn_input(a.session)) == mpn_input(b.session));
        ++result.picks;
        check(a, b, scenario, "pick", before, result);
        prepare(a, scenario); prepare(b, scenario);
    }
    static void parity(const Data &data, const Scenario &scenario, const std::vector<std::string> &inputs,
                       const std::filesystem::path &seed, const std::filesystem::path &root, Result &result) {
        for (bool simplified : {true, false}) {
            // Each side owns a copy of the same learned state; the engine reads B's.
            const auto folderA = root / (scenario.name + (simplified ? "-s-a" : "-t-a")), folderB = root / (scenario.name + (simplified ? "-s-b" : "-t-b"));
            for (const auto &folder : {folderA, folderB}) {
                std::filesystem::create_directories(folder);
                if (!seed.empty()) for (const auto &file : std::filesystem::directory_iterator(seed))
                    std::filesystem::copy_file(file.path(), folder / file.path().filename(), std::filesystem::copy_options::overwrite_existing);
            }
            auto dictionary = WindowsDictionary::load(data.base, data.themes, data.personal, folderB / L"selection-memory.tsv");
            // Step 7c/7d: compare full in-process service A against true thin-client
            // service B (zero local base snapshot, SQLite or FF14 theme words!).
            auto a = service(data, simplified, folderA / L"selection-memory.tsv"), b = thinService(data, simplified, folderB / L"selection-memory.tsv");
            b->engineClient.enabled = true;
            b->engineClient.factory = loopback(dictionary);
            for (size_t n = 0; n < inputs.size(); ++n) {
                const auto &input = inputs[n];
                compare(*a, *b, scenario, input, result);
                // Deeper pages continue from either list in the same way.
                for (int k = 0; k < 2; ++k) {
                    a->expandCandidates(); b->expandCandidates();
                    if (!same(a->choices, b->choices)) { report("EXPANSION " + scenario.name, input, *a, *b); assert(false); }
                }
                if (n % 5 == 0) pick(*a, *b, scenario, n % 3, n % 2 == 1, result);
                if (!a->choices.empty() && *mpn_input(a->session)) {
                    // Every other reading starts with a partial segment (the
                    // (n % 3)-th native candidate that leaves keys), so the
                    // rest is chosen segment by segment.
                    size_t first = n % 4;
                    if (n % 2 == 0) {
                        size_t seen = 0;
                        for (size_t i = 0; i < a->choices.size(); ++i) {
                            const auto &c = a->choices[i];
                            if (c.coreIndex && mpn_candidate_consumed(a->session, *c.coreIndex) < input.size() && seen++ == n % 3) { first = i; break; }
                        }
                    }
                    finish(*a, *b, scenario, first, result);
                    if (scenario.learn) ++result.learned;
                }
                mpn_clear(a->session); mpn_clear(b->session); a->resetCandidates(); b->resetCandidates();
            }
            extras(*a, *b, scenario, result);
            assert(!b->localLexiconLoaded); // Step 7d: thin-client never fell back to local lexicon!
            if (scenario.learn) {
                // Both sides learned the same: the same models, byte for byte.
                assert(a->memory->model().state() == b->memory->model().state());
                assert(a->memory->words().state() == b->memory->words().state());
            }
        }
    }
    // Generated candidates, number mode, F6 forms and (with learning) forget.
    static void extras(Service &a, Service &b, const Scenario &scenario, Result &result) {
        for (const std::string input : {"vrq", "vsj", "vxq", "vfh", "vbd", "vjt", "vxl"}) {
            compare(a, b, scenario, input, result); ++result.quick;
        }
        // Number mode stays in the service: digits after a lone v.
        compare(a, b, scenario, "v", result);
        a.numberInput = b.numberInput = "1234.5";
        a.refreshCandidates(); b.refreshCandidates(true);
        assert(!b.engineClient.pending() && same(a.choices, b.choices) && !a.choices.empty()); ++result.quick;
        a.resetCandidates(); b.resetCandidates();
        // F6: the forms of a Japanese candidate, opened on either list.
        for (const std::string input : {"taberu", "iku", "kaku", "suru", "kuru", "aru", "takai", "kawaii", "shizuka", "benkyou",
                                         "eat", "go", "write", "study", "come", "chi", "qu", "xie", "lai", "xuexi"}) {
            compare(a, b, scenario, input, result);
            for (size_t i = 0; i < a.choices.size(); ++i) {
                if (!conjugationFor(a.currentConjugations(), a.choices[i], input)) continue;
                a.page = b.page = i / 7; a.highlighted = b.highlighted = i % 7;
                assert(a.conjugationPanel.open(a.currentConjugations(), a.choices, input, a.page, a.highlighted, a.expanded));
                assert(b.conjugationPanel.open(b.currentConjugations(), b.choices, input, b.page, b.highlighted, b.expanded));
                assert(same(a.choices, b.choices));
                a.memoryEnabled = b.memoryEnabled = false;
                std::wstring ta, tb;
                assert(a.chooseCandidate(0, ta) == b.chooseCandidate(0, tb) && ta == tb);
                ++result.f6;
                break;
            }
            mpn_clear(a.session); mpn_clear(b.session); a.resetCandidates(); b.resetCandidates();
        }
        if (scenario.memory) {
            // Ctrl+Delete after learning: the same notice, the same list again.
            for (const std::string input : {"zhongguo", "shijian", "keyi"}) {
                compare(a, b, scenario, input, result);
                if (a.choices.size() < 3) continue;
                finish(a, b, Scenario{scenario.name, scenario.fuzzy, scenario.typo, true, true}, 2, result);
                mpn_clear(a.session); mpn_clear(b.session); a.resetCandidates(); b.resetCandidates();
                compare(a, b, scenario, input, result);
                const auto before = b.engineClient.stats.served;
                assert(a.forgetCandidate(0) == b.forgetCandidate(0));
                check(a, b, scenario, "forget", before, result); ++result.forgets;
                mpn_clear(a.session); mpn_clear(b.session); a.resetCandidates(); b.resetCandidates();
            }
        }
    }
    // A learned state built from real candidates: for every third reading the
    // third candidate was chosen twice (phrase and words, both scripts), and
    // every seventh reading has a user word.
    static void seed(const Data &data, const std::vector<std::string> &inputs, const std::filesystem::path &folder) {
        std::filesystem::create_directories(folder);
        selection_memory::Store store(folder / L"selection-memory.tsv");
        std::string user = "# synthetic user words\n";
        const auto hour = selection_memory::currentHour();
        for (bool simplified : {true, false}) {
            auto s = service(data, simplified);
            for (size_t i = 0; i < inputs.size(); i += 3) {
                type(*s, inputs[i], false);
                if (s->choices.size() < 3 || !s->choices[2].coreIndex) continue;
                const auto &choice = s->choices[2];
                for (int k = 0; k < 2; ++k) store.record(inputs[i], choice.text, simplified);
                const std::string words = mpn_candidate_words(s->session, *choice.coreIndex);
                for (size_t start = 0; start < words.size();) {
                    const auto end = words.find('\n', start), tab = words.find('\t', start);
                    if (end == words.npos || tab == words.npos || tab > end) break;
                    store.recordWord(words.substr(start, tab - start), words.substr(tab + 1, end - tab - 1), simplified, hour - int64_t(i % 50));
                    start = end + 1;
                }
                if (simplified && i % 7 == 0 && s->choices.size() > 4) user += inputs[i] + "\t" + s->choices[4].text + "\n";
            }
        }
        assert(store.flush());
        std::ofstream(folder / L"selection-memory-user.tsv", std::ios::binary) << user;
    }

    // Script-driven checks (test_engine_tsf.py): the switch, learning, forget,
    // and the management script's Clear / AddWord / RemoveWord / ClearWords.
    static void steps(const Data &data, const std::filesystem::path &folderA, const std::filesystem::path &folderB) {
        auto dictionary = WindowsDictionary::load(data.base, data.themes, data.personal, folderB / L"selection-memory.tsv");
        auto a = service(data, true, folderA / L"selection-memory.tsv"), b = service(data, true, folderB / L"selection-memory.tsv");
        b->engineClient.enabled = true;
        b->engineClient.factory = loopback(dictionary);
        Scenario scenario{"steps"}; scenario.memory = true; scenario.learn = true;
        std::cout << "ready" << std::endl;
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream words(line);
            std::string command, input; size_t index = 0;
            words >> command >> input >> index;
            if (command == "exit") break;
            Result result;
            std::string extra;
            if (command == "memory") { scenario.memory = input == "on"; std::cout << "{}" << std::endl; continue; }
            if (command == "check" || command == "learn" || command == "forget") {
                compare(*a, *b, scenario, input, result);
                if (command == "learn") finish(*a, *b, scenario, index, result);
                if (command == "forget") {
                    const auto na = a->forgetCandidate(index), nb = b->forgetCandidate(index);
                    assert(na == nb); extra = na == L"已忘记" ? "forgot" : "nothing";
                }
            } else assert(false && "unknown command");
            std::string first = a->choices.empty() ? std::string() : a->choices[0].text;
            if (command != "check") first.clear();
            std::string texts;
            for (size_t i = 0; i < a->choices.size() && i < 9; ++i) texts += (i ? "|" : "") + a->choices[i].text;
            std::cout << "{\"served\":" << result.served << ",\"prefixes\":" << result.prefixes << ",\"path\":\"" << b->engineClient.stats.path
                      << "\",\"reason\":\"" << b->engineClient.stats.reason << "\",\"behind\":" << b->engineClient.stats.memoryBehind
                      << ",\"extra\":\"" << extra << "\",\"texts\":\"" << (command == "check" ? texts : std::string()) << "\"}" << std::endl;
            mpn_clear(a->session); mpn_clear(b->session); a->resetCandidates(); b->resetCandidates();
        }
    }

    // A real engine host process: threaded link, pipe transport, Open identity.
    static void pipeHost(const Data &data, const std::filesystem::path &executable) {
        EngineProcess::Config config{executable, data.base, data.themes, data.personal};
        config.dynamicEndpoints = true;
        EngineProcess manager(config);
        const auto wait = [&](uint64_t deadline) {
            auto events = manager.waitHandles();
            const auto now = ep::now(); assert(now < deadline);
            if (!events.empty()) WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, static_cast<DWORD>(deadline - now));
        };
        const auto started = ep::now() + 60000;
        while (manager.poll(ep::now()) == EngineProcess::State::Starting) wait(started);
        assert(manager.status().state == EngineProcess::State::Ready);
        assert(manager.requestControl(engine::control::Operation::Allocate, ep::now(), 10000));
        while (manager.status().controlState == engine::control::State::Pending) { wait(ep::now() + 10000); manager.poll(ep::now()); }
        auto allocated = manager.takeControl(ep::now()); assert(allocated && allocated->endpoint);
        auto endpoint = std::make_shared<engine::EngineRegistration::Endpoint>(std::move(*allocated->endpoint));
        auto a = service(data, true), b = service(data, true);
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr); assert(ready);
        b->engineClient.enabled = true;
        b->engineClient.factory = [endpoint, ready] {
            return tsf_engine::Link::threaded([endpoint](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                return std::make_unique<tsf_engine::PipeTransport>(endpoint->connect());
            }, [ready] { SetEvent(ready); });
        };
        // The first reading creates the link; it serves once connected.
        mpn_clear(b->session); b->refreshCandidates(true);
        assert(mpn_key(b->session, 'n')); b->refreshCandidates(true);
        assert(b->engineClient.pending() || b->engineClient.stats.reason == "connecting");
        while (b->engineClient.link()->state() == tsf_engine::Link::State::Connecting) WaitForSingleObject(ready, 10000);
        assert(b->engineClient.link()->state() == tsf_engine::Link::State::Ready);
        size_t served = 0;
        for (const std::string input : {"nihao", "riben", "zhongguoren", "shi", "wozaiditieshang"}) {
            type(*a, input, false);
            mpn_clear(b->session); b->refreshCandidates(true);
            for (char c : input) {
                assert(mpn_key(b->session, c)); b->refreshCandidates(true);
            }
            const auto deadline = GetTickCount64() + 5000;
            while (b->engineClient.pending() && GetTickCount64() < deadline) { WaitForSingleObject(ready, 100); b->deliverEngine(); }
            assert(!b->engineClient.pending() && same(a->choices, b->choices));
            if (std::string(b->engineClient.stats.path) == "engine") ++served;
            b->resetCandidates();
        }
        assert(served >= 3 && !b->engineClient.stats.failures);
        b.reset(); CloseHandle(ready);
        manager.requestStop(ep::now());
        const auto stop = ep::now() + 15000;
        while (manager.poll(ep::now()) != EngineProcess::State::Exited) wait(stop);
        std::cout << "pipe host: " << served << "/5 readings served by the engine process" << std::endl;
    }

    // ---------------------------------------------------------------- step 6c
    // An installed layout: the version folder's own files (now a snapshot by
    // its base-dictionary.json) and %LOCALAPPDATA%\Meowpuut, set up exactly as
    // Service::Activate does.
    static std::unique_ptr<Service> installed(const std::filesystem::path &version, const std::filesystem::path &dataRoot, bool simplified) {
        auto s = std::make_unique<Service>();
        s->simplified = simplified;
        s->clock = &fixedMoment;
        s->installFolder = version; s->dataRoot = dataRoot;
        s->conjugations.loadFile(version / L"japanese-conjugations.bin");
        std::ifstream japanese(version / L"japanese.tsv", std::ios::binary);
        s->japaneseWords = readJapaneseWords(japanese);
        appendConjugationWords(s->conjugations, s->japaneseWords);
        s->memory = std::make_unique<selection_memory::Store>(dataRoot / L"selection-memory.tsv");
        const auto customPath = dataRoot / L"japanese-custom.tsv";
        auto personal = readPersonalDictionary(customPath);
        const auto themeRoot = dataRoot / L"Dictionaries";
        auto snapshot = readJapanesePackageSnapshot(themeRoot);
        s->themeReload = std::make_unique<ThemeReload>(themeRoot, s->japaneseWords, snapshot ? snapshot->generation : std::string(),
            customPath, personal, dataRoot / L"BaseDictionaries");
        if (personal) s->japaneseWords.insert(s->japaneseWords.end(), personal->words.begin(), personal->words.end());
        if (snapshot) s->japaneseWords.insert(s->japaneseWords.end(), std::make_move_iterator(snapshot->words.begin()), std::make_move_iterator(snapshot->words.end()));
        std::stable_sort(s->japaneseWords.begin(), s->japaneseWords.end(), [](const auto &x, const auto &y) { return x.weight > y.weight; });
        s->evidence.open(version / L"language-evidence.sqlite");
        std::ifstream file(version / L"native-pinyin.tsv", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file), {}};
        s->lexicon = mpn_lexicon_create(bytes.data(), bytes.size());
        s->session = mpn_session_create(s->lexicon);
        assert(s->session);
        mpn_set_simplified(s->session, simplified);
        s->engineReleaseProbe = [] { return engine::release::Evaluation{}; };
        return s;
    }
    struct Layout { std::filesystem::path base, baseStore, themes, personal, memory; };
    static Layout parseLayout(const std::vector<std::wstring> &arguments) {
        Layout result;
        for (size_t i = 0; i + 1 < arguments.size(); i += 2) {
            if (arguments[i] == L"--base") result.base = arguments[i + 1];
            else if (arguments[i] == L"--themes") result.themes = arguments[i + 1];
            else if (arguments[i] == L"--personal") result.personal = arguments[i + 1];
            else if (arguments[i] == L"--memory") result.memory = arguments[i + 1];
            else if (arguments[i] == L"--base-store") result.baseStore = arguments[i + 1];
            else assert(false && "unexpected layout argument");
        }
        return result;
    }
    // Waits for b's link to connect, then types readings on a (in-process)
    // and b (engine); returns how many final lists the engine served.
    static size_t served(Service &a, Service &b, HANDLE ready, const std::vector<std::string> &inputs) {
        mpn_clear(b.session); b.resetCandidates();
        assert(mpn_key(b.session, 'n')); b.refreshCandidates(true);
        const auto connect = GetTickCount64() + 90000;
        while (b.engineClient.link() && b.engineClient.link()->state() == tsf_engine::Link::State::Connecting && GetTickCount64() < connect)
            WaitForSingleObject(ready, 500);
        assert(b.engineClient.link() && b.engineClient.link()->state() == tsf_engine::Link::State::Ready);
        size_t count = 0;
        for (const auto &input : inputs) {
            type(a, input, false);
            mpn_clear(b.session); b.resetCandidates();
            for (char c : input) { assert(mpn_key(b.session, c)); b.refreshCandidates(true); }
            const auto deadline = GetTickCount64() + 5000;
            while (b.engineClient.pending() && GetTickCount64() < deadline) { WaitForSingleObject(ready, 100); b.deliverEngine(); }
            assert(!b.engineClient.pending() && same(a.choices, b.choices));
            if (std::string(b.engineClient.stats.path) == "engine") ++count;
        }
        return count;
    }
    struct Host {
        std::unique_ptr<EngineProcess> manager;
        std::shared_ptr<engine::EngineRegistration::Endpoint> endpoint;
        Host(const std::filesystem::path &executable, const Layout &layout) {
            EngineProcess::Config config{executable, layout.base, layout.themes, layout.personal};
            config.memory = layout.memory; config.baseStore = layout.baseStore; config.dynamicEndpoints = true;
            manager = std::make_unique<EngineProcess>(config);
            const auto until = ep::now() + 60000;
            while (manager->poll(ep::now()) == EngineProcess::State::Starting) wait(until);
            assert(manager->status().state == EngineProcess::State::Ready);
            endpoint = allocate();
        }
        // Each endpoint serves one connection: a new link needs a new one.
        std::shared_ptr<engine::EngineRegistration::Endpoint> allocate() {
            assert(manager->requestControl(engine::control::Operation::Allocate, ep::now(), 10000));
            while (manager->status().controlState == engine::control::State::Pending) { wait(ep::now() + 10000); manager->poll(ep::now()); }
            auto allocated = manager->takeControl(ep::now()); assert(allocated && allocated->endpoint);
            return std::make_shared<engine::EngineRegistration::Endpoint>(std::move(*allocated->endpoint));
        }
        void wait(uint64_t deadline) {
            auto events = manager->waitHandles();
            const auto now = ep::now(); assert(now < deadline);
            if (!events.empty()) WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, static_cast<DWORD>(deadline - now));
        }
        tsf_engine::Client::Factory factory(HANDLE ready) {
            return [this, ready] {
                auto target = endpoint ? std::move(endpoint) : allocate();
                return tsf_engine::Link::threaded([target](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                    return std::make_unique<tsf_engine::PipeTransport>(target->connect());
                }, [ready] { SetEvent(ready); });
            };
        }
        struct Memory { uint64_t priv = 0, peakWs = 0; };
        Memory memory() const {
            Memory m;
            PROCESS_MEMORY_COUNTERS_EX c{}; c.cb = sizeof(c);
            if (manager && manager->engineHandle() && GetProcessMemoryInfo(manager->engineHandle(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&c), sizeof(c))) {
                m.priv = c.PrivateUsage; m.peakWs = c.PeakWorkingSetSize;
            }
            return m;
        }
        ~Host() {
            manager->requestStop(ep::now());
            const auto until = ep::now() + 15000;
            while (manager->poll(ep::now()) != EngineProcess::State::Exited && ep::now() < until) wait(until);
        }
    };
    static void adopt(Service &s) {
        mpn_clear(s.session); s.resetCandidates(); // A boundary.
        const auto until = GetTickCount64() + 60000;
        while (!s.baseSnapshot && GetTickCount64() < until) { s.pollDictionaries(GetTickCount64()); Sleep(20); }
        assert(s.baseSnapshot);
    }
    static std::string adoptIdentity(Service &s, const std::string &previous) {
        mpn_clear(s.session); s.resetCandidates();
        const auto until = GetTickCount64() + 60000;
        while (s.engineIdentity() == previous && GetTickCount64() < until) { s.pollDictionaries(GetTickCount64()); Sleep(20); }
        assert(s.engineIdentity() != previous && !s.engineIdentity().empty());
        return s.engineIdentity();
    }
    static bool hasCandidate(const Service &s, const std::string &text) {
        return std::any_of(s.choices.begin(), s.choices.end(), [&](const auto &c) { return c.text == text; });
    }
    static void converge(Service &a, Service &b, HANDLE ready, const std::string &target,
                         const std::vector<std::string> &inputs, size_t minServed, size_t &waited, size_t &outServed) {
        const auto until = GetTickCount64() + 60000;
        for (;;) {
            const auto count = served(a, b, ready, inputs);
            if (count >= minServed && b.engineClient.link()->identity() == target) { outServed = count; return; }
            assert(GetTickCount64() < until && "the engine did not adopt the publication");
            ++waited; Sleep(1100);
        }
    }
    // layout <version> <dataRoot> <host.exe> [<owner_test.exe>] ; stdin drives
    // base publish, personal edit, theme disable/re-enable and corrupt publish.
    static void layout(const std::filesystem::path &version, const std::filesystem::path &dataRoot,
                       const std::filesystem::path &hostExe, const std::filesystem::path &ownerExe) {
        const std::vector<std::string> inputs{"nihao", "zhongguo", "riben", "wozaiditieshang", "shi"};
        auto a = installed(version, dataRoot, true), b = installed(version, dataRoot, true);
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr); assert(ready);
        b->engineClient.enabled = true;
        // 1. The version folder's manifest gives the service a data identity;
        //    the owner arguments it would start an engine with give the same.
        const auto identity = b->engineIdentity();
        assert(!identity.empty() && b->installGeneration == base_dictionary::manifestGeneration(version));
        auto arguments = parseLayout(b->engineLayout());
        assert(arguments.base == version && arguments.baseStore == dataRoot / L"BaseDictionaries" && arguments.themes == dataRoot / L"Dictionaries" &&
               arguments.personal == dataRoot / L"japanese-custom.tsv" && arguments.memory == dataRoot / L"selection-memory.tsv");
        assert(WindowsDictionary::load(arguments.base, arguments.themes, arguments.personal, arguments.memory)->identity() == identity);
        size_t first = 0, waited = 0, converged = 0, kept = 0, personalWaited = 0, personalServed = 0;
        size_t themeOffWaited = 0, themeOffServed = 0, themeOnWaited = 0, themeOnServed = 0;
        auto mib = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
        Host::Memory memBefore{}, memAfterBase{}, memAfterPersonal{}, memAfterThemeOff{}, memAfterThemeOn{};
        {
            Host host(hostExe, arguments);
            b->engineClient.factory = host.factory(ready);
            first = served(*a, *b, ready, inputs);
            assert(first >= 4);
            memBefore = host.memory();
            std::cout << "installed layout: identity equal, " << first << "/" << inputs.size() << " served" << std::endl;
            // 2. Step 6d: a newer base snapshot is published. Both services adopt
            //    it at a boundary and the running engine adopts it too; until
            //    both use the same data the service lists in-process ("identity").
            std::cout << "ready-publish" << std::endl;
            std::string line; std::getline(std::cin, line); assert(line == "published");
            adopt(*a); adopt(*b);
            const auto published = b->engineIdentity();
            assert(published != identity && !published.empty());
            converge(*a, *b, ready, published, inputs, 4, waited, converged);
            memAfterBase = host.memory();
            // 3. Pre-step-7: personal table edit hot-reloaded by the engine
            //    while reusing the pinned base snapshot.
            std::cout << "ready-personal" << std::endl;
            std::getline(std::cin, line); assert(line == "personal-updated");
            const auto personalId = adoptIdentity(*a, published);
            assert(adoptIdentity(*b, published) == personalId);
            converge(*a, *b, ready, personalId, {"testcoffee"}, 1, personalWaited, personalServed);
            assert(hasCandidate(*b, "コーヒー") && !hasCandidate(*b, "珈琲"));
            memAfterPersonal = host.memory();
            // 4. Pre-step-7: theme package disabled then re-enabled, hot-reloaded
            //    by the engine while reusing the pinned base snapshot.
            std::cout << "ready-theme-off" << std::endl;
            std::getline(std::cin, line); assert(line == "theme-disabled");
            const auto themeOffId = adoptIdentity(*a, personalId);
            assert(adoptIdentity(*b, personalId) == themeOffId);
            converge(*a, *b, ready, themeOffId, {"tejihuifuyao"}, 1, themeOffWaited, themeOffServed);
            assert(!hasCandidate(*b, "特级恢复药"));
            memAfterThemeOff = host.memory();
            std::cout << "ready-theme-on" << std::endl;
            std::getline(std::cin, line); assert(line == "theme-enabled");
            const auto themeOnId = adoptIdentity(*a, themeOffId);
            assert(adoptIdentity(*b, themeOffId) == themeOnId);
            converge(*a, *b, ready, themeOnId, {"tejihuifuyao"}, 1, themeOnWaited, themeOnServed);
            assert(hasCandidate(*b, "特级恢复药"));
            memAfterThemeOn = host.memory();
            // The formal resident path now, while the store's current snapshot
            // is the one both services use: discovery, handoff and an owner
            // started through the shell, from an injected (HKCU) release source.
            if (!ownerExe.empty()) {
                resident(*a, *b, version, ownerExe, hostExe, ready, inputs);
                b->engineClient.factory = host.factory(ready);
            }
            // 5. A damaged publication (a file changed after staging): every
            //    loader refuses it, the services and the engine keep what they
            //    have, identities stay equal and the engine keeps serving.
            std::cout << "ready-corrupt" << std::endl;
            std::getline(std::cin, line); assert(line == "corrupted");
            for (auto *s : {a.get(), b.get()}) {
                mpn_clear(s->session); s->resetCandidates();
                const auto stop = GetTickCount64() + 5000;
                while (GetTickCount64() < stop) { s->pollDictionaries(GetTickCount64()); Sleep(50); }
                assert(s->engineIdentity() == themeOnId);
            }
            Sleep(3000); // The engine's reload thread looks once a second.
            kept = served(*a, *b, ready, inputs);
            assert(kept >= 4 && b->engineClient.link()->identity() == themeOnId);
            b->engineClient.close();
        }
        std::cout << "hot reload: published snapshot adopted after " << waited << " round(s) (" << converged << "/" << inputs.size()
                  << "), personal adopted after " << personalWaited << " round(s) (" << personalServed << "/1)"
                  << ", theme off/on adopted after " << themeOffWaited << "/" << themeOnWaited << " round(s) ("
                  << themeOffServed << "/1, " << themeOnServed << "/1); damaged publication refused by both, "
                  << kept << "/" << inputs.size() << " served"
                  << "; host private MiB initial=" << static_cast<int>(mib(memBefore.priv))
                  << " after-base=" << static_cast<int>(mib(memAfterBase.priv))
                  << " after-personal=" << static_cast<int>(mib(memAfterPersonal.priv))
                  << " after-theme-off=" << static_cast<int>(mib(memAfterThemeOff.priv))
                  << " after-theme-on=" << static_cast<int>(mib(memAfterThemeOn.priv))
                  << " peak-ws=" << static_cast<int>(mib(memAfterThemeOn.peakWs)) << std::endl;
        b.reset(); CloseHandle(ready);
    }
    static void resident(Service &a, Service &b, const std::filesystem::path &dataVersion, const std::filesystem::path &ownerExe,
                         const std::filesystem::path &hostExe, HANDLE ready, const std::vector<std::string> &inputs) {
        namespace rs = engine::resident;
        const auto unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const std::wstring id = L"tsf-" + std::to_wstring(GetCurrentProcessId());
        const auto programs = dataVersion.parent_path() / id;
        std::filesystem::create_directories(programs);
        const auto owner = programs / L"harukoto_engine_owner.exe", host = programs / L"harukoto_engine_host.exe";
        std::filesystem::copy_file(ownerExe, owner, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(hostExe, host, std::filesystem::copy_options::overwrite_existing);
        const std::wstring testKey = L"Software\\HarukotoTest-tsf-" + unique, releaseKey = testKey + L"\\EngineReleases", scope = L"tsf-" + unique;
        struct Cleanup {
            // An assert aborts without unwinding: stop the owner and remove the
            // test key from the SIGABRT handler too.
            static Cleanup *&active() { static Cleanup *value = nullptr; return value; }
            std::wstring key, scope; rs::Store store;
            ~Cleanup() {
                const auto status = rs::inspect(store, scope);
                for (DWORD pid : {status.ownerPid, status.hostPid}) {
                    if (!pid) continue;
                    HWND window = nullptr;
                    engine::pipe::Handle process(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid));
                    std::pair<DWORD, HWND> find(pid, nullptr);
                    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
                        auto *found = reinterpret_cast<std::pair<DWORD, HWND> *>(parameter); DWORD owner = 0;
                        GetWindowThreadProcessId(hwnd, &owner); wchar_t name[64]{}; GetClassNameW(hwnd, name, 64);
                        if (owner == found->first && std::wstring(name) == rs::WindowClass) { found->second = hwnd; return FALSE; }
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&find));
                    window = find.second;
                    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
                    if (process && WaitForSingleObject(process.get(), 15000) != WAIT_OBJECT_0) TerminateProcess(process.get(), 9);
                }
                RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
            }
        } cleanup{testKey, scope, rs::Store::under(testKey)};
        Cleanup::active() = &cleanup;
        std::signal(SIGABRT, [](int) { if (auto *c = Cleanup::active()) { Cleanup::active() = nullptr; c->~Cleanup(); } });
        struct Disarm { ~Disarm() { Cleanup::active() = nullptr; } } disarm;
        // The release value, as the elevated installer writes it under HKLM.
        const auto sha = [](const std::filesystem::path &path) { const auto hex = base_dictionary::StableFile(path).read(true); return std::wstring(hex.begin(), hex.end()); };
        std::wstring value;
        for (const auto &line : {std::wstring(L"format=harukoto-engine-release/1"), L"id=" + id, std::wstring(L"sequence=1"),
                L"protocol=" + std::to_wstring(engine::ProtocolVersion), L"host=" + host.wstring(), L"host-sha256=" + sha(host),
                L"owner=" + owner.wstring(), L"owner-sha256=" + sha(owner)})
            value += line + L'\0';
        value += L'\0';
        HKEY key = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, releaseKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS);
        assert(RegSetValueExW(key, id.c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE *>(value.data()), static_cast<DWORD>(value.size() * sizeof(wchar_t))) == ERROR_SUCCESS);
        RegCloseKey(key);
        rs::EnsureOptions options;
        options.handoff.discovery.scope = scope; options.store = rs::Store::under(testKey);
        options.ownerArguments = b.engineLayout();
        for (const auto &argument : {std::wstring(L"--scope"), scope, std::wstring(L"--release-subkey"), releaseKey, std::wstring(L"--store-subkey"), testKey})
            options.ownerArguments.push_back(argument);
        const engine::release::Source source{HKEY_CURRENT_USER, releaseKey, false};
        b.engineClient.close();
        b.engineReleaseProbe = [source] {
            return engine::release::evaluate(engine::release::read(source), engine::ProtocolVersion);
        };
        b.engineClient.factory = [source, options, ready] {
            return tsf_engine::Link::threaded(tsf_engine::residentConnect(source, options), [ready] { SetEvent(ready); }, 60000);
        };
        const auto count = served(a, b, ready, inputs);
        assert(count >= 4);
        const auto status = rs::inspect(cleanup.store, scope);
        assert(status.present && status.alive);
        b.engineClient.close();
        b.engineReleaseProbe = [] { return engine::release::Evaluation{}; };
        std::cout << "resident: owner started through the shell, " << count << "/" << inputs.size() << " served" << std::endl;
    }
};
} // namespace

static std::vector<std::string> corpus(const std::filesystem::path &path) {
    std::ifstream file(path);
    assert(file);
    std::vector<std::string> inputs;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#') continue;
        size_t start = 0;
        for (int column = 0; column < 2; ++column) start = line.find('\t', start) + 1;
        const auto input = line.substr(start, line.find('\t', start) - start);
        if (!input.empty() && std::all_of(input.begin(), input.end(), [](char c) { return c >= 'a' && c <= 'z'; })) inputs.push_back(input);
    }
    return inputs;
}

// unit <base> <themes> <personal> <other-personal> <host.exe>
// parity <base> <themes> <personal> <scratch> <tuned> <words> <fuzzy> <typo> <abbrev>
// steps <base> <themes> <personal> <memory-a-folder> <memory-b-folder>
static int run(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv) {
    tsf_engine::standaloneEngineDefault = false; // Never reach the installed engine unless a test asks.
    renderer_handoff::rendererHandoffDefault = false; // Never probe HKLM renderer releases unless a test asks.
    try { return run(argc, argv); }
    catch (const engine::pipe::Error &error) { std::cerr << "pipe failure=" << static_cast<int>(error.reason) << " win32=" << error.code << std::endl; }
    catch (const std::exception &error) { std::cerr << "exception: " << error.what() << std::endl; }
    return 1;
}
static int run(int argc, wchar_t **argv) {
    assert(argc >= 5);
    const std::wstring mode = argv[1];
    Data data{argv[2], argv[3], argv[4]};
    if (mode == L"unit") {
        assert(argc == 7);
        auto dictionary = WindowsDictionary::load(data.base, data.themes, data.personal);
        auto other = WindowsDictionary::load(data.base, data.themes, argv[5]);
        assert(other->identity() != dictionary->identity());
        EngineTsfTest::offPath(data, dictionary);
        EngineTsfTest::behaviour(data, dictionary, other);
        EngineTsfTest::cutover(data, dictionary);
        EngineTsfTest::slotLifecycle(data, dictionary);
        EngineTsfTest::thinClient(data, dictionary);
        EngineTsfTest::pipeHost(data, argv[6]);
    } else if (mode == L"layout") {
        assert(argc == 5 || argc == 6);
        EngineTsfTest::layout(argv[2], argv[3], argv[4], argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path());
        return 0;
    } else if (mode == L"steps") {
        assert(argc == 7);
        EngineTsfTest::steps(data, argv[5], argv[6]);
    } else {
        assert(mode == L"parity" && argc == 11);
        const std::filesystem::path root = argv[5];
        const auto tuned = corpus(argv[6]), words = corpus(argv[7]), fuzzy = corpus(argv[8]), typo = corpus(argv[9]), abbrev = corpus(argv[10]);
        auto both = tuned; both.insert(both.end(), words.begin(), words.end());
        auto typing = typo; typing.insert(typing.end(), abbrev.begin(), abbrev.end());
        auto fuzzyInputs = tuned; fuzzyInputs.insert(fuzzyInputs.end(), fuzzy.begin(), fuzzy.end());
        EngineTsfTest::seed(data, both, root / L"seed");
        using Scenario = EngineTsfTest::Scenario;
        std::vector<std::tuple<Scenario, const std::vector<std::string> *, bool>> runs;
        Scenario plain{"plain"}; runs.emplace_back(plain, &both, false);
        Scenario allFuzzy{"fuzzy_all"}; allFuzzy.fuzzy = MPN_FUZZY_ALL; runs.emplace_back(allFuzzy, &fuzzyInputs, false);
        Scenario typoOn{"typo_on"}; runs.emplace_back(typoOn, &typing, false);
        Scenario typoOff{"typo_off"}; typoOff.typo = false; runs.emplace_back(typoOff, &typing, false);
        Scenario memoryOn{"memory"}; memoryOn.memory = true; runs.emplace_back(memoryOn, &both, true);
        Scenario userOnly{"user_words_only"}; runs.emplace_back(userOnly, &both, true);
        Scenario learn{"learn_while_choosing"}; learn.memory = true; learn.learn = true; runs.emplace_back(learn, &both, true);
        Scenario everything{"learn_fuzzy_typo_off"}; everything.memory = true; everything.learn = true; everything.fuzzy = MPN_FUZZY_ALL; everything.typo = false;
        runs.emplace_back(everything, &both, true);
        std::cout << "{";
        bool firstRun = true;
        for (const auto &[scenario, inputs, seeded] : runs) {
            EngineTsfTest::Result result;
            EngineTsfTest::parity(data, scenario, *inputs, seeded ? root / L"seed" : std::filesystem::path(), root, result);
            std::cout << (firstRun ? "" : ",") << "\"" << scenario.name << "\":{\"prefixes\":" << result.prefixes << ",\"segments\":" << result.segments
                      << ",\"served\":" << result.served << ",\"chosen\":" << result.chosen << ",\"learned\":" << result.learned
                      << ",\"picks\":" << result.picks << ",\"f6\":" << result.f6 << ",\"quick\":" << result.quick << ",\"forgets\":" << result.forgets
                      << ",\"mismatches\":0,\"fallbacks\":{";
            bool firstReason = true;
            for (const auto &[reason, count] : result.fallbacks) { std::cout << (firstReason ? "" : ",") << "\"" << reason << "\":" << count; firstReason = false; }
            std::cout << "}}";
            firstRun = false;
        }
        std::cout << "}" << std::endl;
    }
    return 0;
}
