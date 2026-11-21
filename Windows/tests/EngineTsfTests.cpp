// WIN-03 steps 6a/6b: the production text service with the standalone engine
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
            ",\"engine_path\":\"off\",\"engine_served\":0,\"engine_fallbacks\":0,\"engine_reason\":\"none\",\"engine_memory_behind\":0");
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
        // Step 6b: fuzzy pinyin and the typo switch travel with the request.
        c->engineClient.close();
        c->engineClient.factory = loopback(dictionary);
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

    // Step 6b: the per-key preparation OnKeyDown does before a key: the
    // memory switch, polling, the personal table, fuzzy pinyin and typo switch.
    struct Scenario { std::string name; uint32_t fuzzy = 0; bool typo = true, memory = false, learn = false; };
    static void prepare(Service &s, const Scenario &scenario) {
        s.memoryEnabled = s.memory && scenario.memory;
        if (s.memoryEnabled) s.memory->poll(GetTickCount64(), 0);
        if (s.memory) s.memory->pollUserWords(GetTickCount64(), 0);
        s.attachPersonal();
        applyFuzzyPinyin(s.session, scenario.fuzzy);
        applyTypoCorrection(s.session, scenario.typo);
    }
    struct Result {
        size_t prefixes = 0, served = 0, chosen = 0, learned = 0;
        std::map<std::string, size_t> fallbacks;
    };
    // Types one reading on both services key by key and compares every list.
    static void compare(Service &a, Service &b, const Scenario &scenario, const std::string &input, Result &result) {
        prepare(a, scenario); prepare(b, scenario);
        mpn_clear(a.session); mpn_clear(b.session); a.refreshCandidates(); b.refreshCandidates(true);
        for (size_t i = 0; i < input.size(); ++i) {
            prepare(a, scenario); prepare(b, scenario);
            assert(mpn_key(a.session, input[i]) && mpn_key(b.session, input[i]));
            a.refreshCandidates(); b.refreshCandidates(true);
            const auto before = b.engineClient.stats.served;
            if (b.engineClient.pending()) serve(b);
            ++result.prefixes;
            if (b.engineClient.stats.served > before) ++result.served;
            else ++result.fallbacks[b.engineClient.stats.reason];
            if (!same(a.choices, b.choices)) { report("MISMATCH " + scenario.name, input.substr(0, i + 1), a, b); assert(false); }
        }
        assert(!b.engineClient.stats.failures);
    }
    // Chooses index, then the first candidate of every remaining segment, as
    // a user completing the reading: learning is flushed at its end.
    static void finish(Service &a, Service &b, const Scenario &scenario, size_t index) {
        std::wstring ta, tb;
        for (bool first = true; *mpn_input(a.session); first = false) {
            const size_t pick = first && index < a.choices.size() ? index : 0;
            if (pick >= a.choices.size()) break;
            assert(a.chooseCandidate(pick, ta) == b.chooseCandidate(pick, tb));
            ta += wide(mpn_committed(a.session)); tb += wide(mpn_committed(b.session));
            mpn_clear_commit(a.session); mpn_clear_commit(b.session);
            assert(ta == tb && std::string(mpn_input(a.session)) == mpn_input(b.session));
            prepare(a, scenario); prepare(b, scenario);
            a.refreshCandidates(); b.refreshCandidates(true);
            if (b.engineClient.pending()) serve(b);
            if (!same(a.choices, b.choices)) { report("SEGMENT " + scenario.name, mpn_input(a.session), a, b); assert(false); }
        }
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
            auto a = service(data, simplified, folderA / L"selection-memory.tsv"), b = service(data, simplified, folderB / L"selection-memory.tsv");
            b->engineClient.enabled = true;
            b->engineClient.factory = loopback(dictionary);
            for (const auto &input : inputs) {
                compare(*a, *b, scenario, input, result);
                // Deeper pages continue from either list in the same way.
                for (int k = 0; k < 2; ++k) {
                    a->expandCandidates(); b->expandCandidates();
                    if (!same(a->choices, b->choices)) { report("EXPANSION " + scenario.name, input, *a, *b); assert(false); }
                }
                if (!a->choices.empty()) {
                    const size_t index = a->choices.size() > 1 ? 1 : 0;
                    if (scenario.learn) { finish(*a, *b, scenario, index); ++result.learned; }
                    else {
                        // Without learning: the same commit, nothing recorded.
                        a->memoryEnabled = b->memoryEnabled = false;
                        std::wstring ta, tb;
                        assert(a->chooseCandidate(index, ta) == b->chooseCandidate(index, tb));
                        assert(ta == tb && std::string(mpn_committed(a->session)) == mpn_committed(b->session) &&
                               std::string(mpn_input(a->session)) == mpn_input(b->session));
                    }
                    ++result.chosen;
                }
                mpn_clear(a->session); mpn_clear(b->session); a->resetCandidates(); b->resetCandidates();
            }
            if (scenario.learn) {
                // Both sides learned the same: the same models, byte for byte.
                assert(a->memory->model().state() == b->memory->model().state());
                assert(a->memory->words().state() == b->memory->words().state());
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
        Scenario scenario{"steps"}; scenario.memory = true;
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
                if (command == "learn") finish(*a, *b, scenario, index);
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
        EngineTsfTest::pipeHost(data, argv[6]);
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
        Scenario learn{"learn_then_type"}; learn.memory = true; learn.learn = true; runs.emplace_back(learn, &both, true);
        Scenario everything{"memory_fuzzy_typo_off"}; everything.memory = true; everything.fuzzy = MPN_FUZZY_ALL; everything.typo = false;
        runs.emplace_back(everything, &both, true);
        std::cout << "{";
        bool firstRun = true;
        for (const auto &[scenario, inputs, seeded] : runs) {
            EngineTsfTest::Result result;
            EngineTsfTest::parity(data, scenario, *inputs, seeded ? root / L"seed" : std::filesystem::path(), root, result);
            std::cout << (firstRun ? "" : ",") << "\"" << scenario.name << "\":{\"prefixes\":" << result.prefixes << ",\"served\":" << result.served
                      << ",\"chosen\":" << result.chosen << ",\"learned\":" << result.learned << ",\"mismatches\":0,\"fallbacks\":{";
            bool firstReason = true;
            for (const auto &[reason, count] : result.fallbacks) { std::cout << (firstReason ? "" : ",") << "\"" << reason << "\":" << count; firstReason = false; }
            std::cout << "}}";
            firstRun = false;
        }
        std::cout << "}" << std::endl;
    }
    return 0;
}
