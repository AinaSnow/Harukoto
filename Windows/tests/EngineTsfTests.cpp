// WIN-03 step 6a: the production text service with the standalone engine
// switch off and on (docs/ENGINE_TSF.md). Two Service objects load the same
// base snapshot, theme packages and personal table; one lists in-process,
// the other through the engine protocol (loopback ServerConnection +
// WindowsDictionary, or a real engine host process over its pipe).
#include "../TextService.cpp"
#include "../EngineWindows.h"
#include "../EngineProcess.h"
#include <cassert>
#include <csignal>
#include <iostream>

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
    size_t exchanges = 0;
    explicit Loopback(std::shared_ptr<const WindowsDictionary> value)
        : dictionary(value), server(42, value, value->identity()) {}
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
    static std::unique_ptr<Service> service(const Data &data, bool simplified) {
        auto s = std::make_unique<Service>();
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
        assert(!tsf_engine::readStandaloneEngine(key));
        DWORD one = 1, zero = 0;
        assert(RegSetValueExW(input, L"StandaloneEngine", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof(zero)) == ERROR_SUCCESS);
        assert(!tsf_engine::readStandaloneEngine(key));
        assert(RegSetValueExW(input, L"StandaloneEngine", 0, REG_SZ, reinterpret_cast<const BYTE *>(L"1"), 4) == ERROR_SUCCESS);
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
            ",\"engine_path\":\"off\",\"engine_served\":0,\"engine_fallbacks\":0,\"engine_reason\":\"none\"");
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
        serve(*b); assert(!b->engineClient.pending() && !b->choices.empty());
        assert(mpn_key(b->session, 'i')); b->refreshCandidates(true);
        b->engineClient.link()->pump();               // reply for "ni" is waiting
        assert(mpn_key(b->session, 'h')); b->refreshCandidates(true); // newer ticket
        assert(!b->deliverEngine() && b->engineClient.pending() && b->engineClient.stats.stale == 1);
        serve(*b); assert(!b->engineClient.pending() && same(a->choices, b->choices));
        // Latest request wins: two keys before the worker runs cost one reply.
        type(*a, "nihao", false);
        assert(mpn_key(b->session, 'a')); b->refreshCandidates(true);
        assert(mpn_key(b->session, 'o')); b->refreshCandidates(true);
        serve(*b); assert(!b->engineClient.pending() && same(a->choices, b->choices));
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
        // Capability gates: fuzzy pinyin, typo switch, partial selection, learning.
        c->engineClient.close();
        c->engineClient.factory = loopback(dictionary);
        type(*c, "nihao", true); serve(*c); assert(std::string(c->engineClient.stats.path) == "engine");
        mpn_set_fuzzy_pinyin(c->session, 1); type(*c, "nihao", true);
        assert(!c->engineClient.pending() && c->engineClient.stats.reason == "settings"); mpn_set_fuzzy_pinyin(c->session, 0);
        mpn_set_typo_correction(c->session, 0); type(*c, "nihao", true);
        assert(c->engineClient.stats.reason == "settings"); mpn_set_typo_correction(c->session, 1);
        c->memoryEnabled = true; // (No store here: check the gate itself.)
        assert(std::string(c->engineUnsupported()) == "personal"); c->memoryEnabled = false;
        type(*c, "nihao", true); serve(*c);
        std::wstring text; const auto split = std::find_if(c->choices.begin(), c->choices.end(), [&](const auto &x) {
            return x.coreIndex && mpn_candidate_consumed(c->session, *x.coreIndex) < 5; });
        if (split != c->choices.end()) {
            assert(c->chooseCandidate(static_cast<size_t>(split - c->choices.begin()), text));
            c->refreshCandidates(true);
            assert(!c->engineClient.pending() && c->engineClient.stats.reason == "partial");
        }
        // rebuild() refuses what it cannot show exactly.
        type(*a, "nihao", false);
        engine::Candidate wrong{1, 5, "错", "", 1, 0};
        assert(!tsf_engine::rebuild(a->session, {wrong}));
        engine::Candidate gap{2, 5, "nihao", "", 0, 0};
        assert(!tsf_engine::rebuild(a->session, {gap}) && !tsf_engine::rebuild(a->session, {}));
        std::cout << "behaviour: async, stale, pending key, timeout, sticky, identity, gates" << std::endl;
    }

    struct Totals { size_t prefixes = 0, served = 0, longList = 0, unrepresentable = 0, inputs = 0, chosen = 0, expanded = 0; };
    static void parity(const Data &data, std::shared_ptr<const WindowsDictionary> dictionary,
                       const std::vector<std::string> &inputs, bool simplified, Totals &totals) {
        auto a = service(data, simplified), b = service(data, simplified);
        b->engineClient.enabled = true;
        b->engineClient.factory = loopback(dictionary);
        for (const auto &input : inputs) {
            ++totals.inputs;
            mpn_clear(a->session); a->refreshCandidates(); mpn_clear(b->session); b->refreshCandidates(true);
            for (size_t i = 0; i < input.size(); ++i) {
                assert(mpn_key(a->session, input[i]) && mpn_key(b->session, input[i]));
                a->refreshCandidates(); b->refreshCandidates(true);
                const auto before = b->engineClient.stats;
                if (b->engineClient.pending()) serve(*b);
                ++totals.prefixes;
                const auto &after = b->engineClient.stats;
                if (after.served > before.served) ++totals.served;
                else if (after.reason == "long_list") ++totals.longList;
                else if (after.reason == "unrepresentable") ++totals.unrepresentable;
                else { std::cerr << "unexpected path " << after.path << " reason " << after.reason << '\n'; assert(false); }
                if (!same(a->choices, b->choices)) { report("MISMATCH", input.substr(0, i + 1), *a, *b); assert(false); }
            }
            assert(b->engineClient.stats.failures == 0);
            // Deeper pages continue from either list in the same way.
            for (int k = 0; k < 2; ++k) {
                a->expandCandidates(); b->expandCandidates();
                if (!same(a->choices, b->choices)) { report("EXPANSION", input, *a, *b); assert(false); }
            }
            ++totals.expanded;
            // Choosing the second candidate (or the first) commits the same text.
            if (!a->choices.empty()) {
                const size_t index = a->choices.size() > 1 ? 1 : 0;
                std::wstring ta, tb;
                assert(a->chooseCandidate(index, ta) == b->chooseCandidate(index, tb));
                assert(ta == tb && std::string(mpn_committed(a->session)) == mpn_committed(b->session) &&
                       std::string(mpn_input(a->session)) == mpn_input(b->session));
                ++totals.chosen;
            }
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
// parity <base> <themes> <personal> <corpus>...
static int run(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv) {
    try { return run(argc, argv); }
    catch (const engine::pipe::Error &error) { std::cerr << "pipe failure=" << static_cast<int>(error.reason) << " win32=" << error.code << std::endl; }
    catch (const std::exception &error) { std::cerr << "exception: " << error.what() << std::endl; }
    return 1;
}
static int run(int argc, wchar_t **argv) {
    assert(argc >= 5);
    const std::wstring mode = argv[1];
    Data data{argv[2], argv[3], argv[4]};
    auto dictionary = WindowsDictionary::load(data.base, data.themes, data.personal);
    if (mode == L"unit") {
        assert(argc == 7);
        auto other = WindowsDictionary::load(data.base, data.themes, argv[5]);
        assert(other->identity() != dictionary->identity());
        EngineTsfTest::offPath(data, dictionary);
        EngineTsfTest::behaviour(data, dictionary, other);
        EngineTsfTest::pipeHost(data, argv[6]);
    } else {
        assert(mode == L"parity");
        EngineTsfTest::Totals totals;
        for (int i = 5; i < argc; ++i) {
            const auto inputs = corpus(argv[i]);
            for (bool simplified : {true, false}) EngineTsfTest::parity(data, dictionary, inputs, simplified, totals);
        }
        std::cout << "{\"inputs\":" << totals.inputs << ",\"prefixes\":" << totals.prefixes << ",\"served\":" << totals.served
                  << ",\"long_list\":" << totals.longList << ",\"unrepresentable\":" << totals.unrepresentable
                  << ",\"expanded\":" << totals.expanded << ",\"chosen\":" << totals.chosen << ",\"mismatches\":0}" << std::endl;
    }
    return 0;
}
