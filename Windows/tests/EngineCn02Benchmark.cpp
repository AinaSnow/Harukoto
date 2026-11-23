// WIN-03 step 8 (CN-02): end-to-end latency and memory comparison between
// standalone engine (thin-client TextService) and in-process TextService
// (docs/ENGINE_CN02.md).
//
// Runs the production Service class (TextService.cpp) in real isolated child
// processes across 5 scenarios:
//   1. startup: in-process full load vs thin-client cold start (resident mode,
//      Explorer-started owner) and warm start (engine already running);
//   2. keys: full key-to-candidate-visible latency (mpn_key -> engine IPC /
//      local merge -> visible page formatting) + selection (KeyAction::Inspect)
//      across 1/2/4/8 concurrent active processes plus idle standby apps;
//   3. deep_page: repeated PageDown / expandCandidates() through all 3 rounds
//      (extraRounds + PageNext fetchMore) with exact candidate count/text parity;
//   4. large_personal: 5,000 personal Japanese + 5,000 phrase memories +
//      5,000 word frequencies + 1,000 user words (load, steady keys, and
//      post-learn flush + engine reload latency).
//
// Modes:
//   --mode resident : uses real harukoto_engine_owner_test + Explorer broker
//                     with an HKCU test release key (requires Medium IL).
//   --mode direct   : spawns real harukoto_engine_host via EngineProcess and
//                     hands allocated pipe endpoints to child workers (runs in
//                     any integrity level; cold Explorer start is skipped).
//
// Never touches HKLM, Program Files, or any installed engine.
#include "../TextService.cpp"
#include "../EngineProcess.h"
#include "../EngineResident.h"
#include <psapi.h>
#include <atomic>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

using namespace harukoto;
using engine::EngineProcess;
using engine::ProtocolVersion;
namespace ep = engine::pipe;
namespace rs = engine::resident;
namespace hf = engine::handoff;
namespace release = engine::release;

namespace {
quick_input::Moment fixedMoment() { return {2026, 11, 23, 1, 30, 0}; }

void require(bool value, const char *what) {
    if (!value) {
        std::cerr << "cn02 benchmark failed: " << what << std::endl;
        std::abort();
    }
}

double nowMs() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}

std::filesystem::path selfExe() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    require(n && n < buf.size(), "GetModuleFileNameW");
    buf.resize(n);
    return buf;
}

std::wstring quoteArg(const std::wstring &value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

std::string jsonEscape(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) out += ' ';
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

ep::Handle makeEvent(bool manual = true) {
    ep::Handle h(CreateEventW(nullptr, manual ? TRUE : FALSE, FALSE, nullptr));
    require(bool(h), "CreateEventW");
    return h;
}

HANDLE parseHandle(const wchar_t *arg) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(arg)));
}

struct Stats {
    size_t n = 0;
    double median = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
};

Stats computeStats(std::vector<double> values) {
    Stats s;
    s.n = values.size();
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    auto rank = [&](double p) {
        const size_t i = static_cast<size_t>(std::ceil(p * values.size()));
        return values[(std::min)(values.size(), (std::max)(i, size_t(1))) - 1];
    };
    s.median = values.size() % 2 ? values[values.size() / 2] : (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2.0;
    s.p95 = rank(0.95);
    s.p99 = rank(0.99);
    s.max = values.back();
    double sum = 0;
    for (double v : values) sum += v;
    s.mean = sum / values.size();
    return s;
}

std::string statsJson(const Stats &s) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(4)
      << "{\"n\":" << s.n << ",\"median\":" << s.median << ",\"p95\":" << s.p95
      << ",\"p99\":" << s.p99 << ",\"max\":" << s.max << ",\"mean\":" << s.mean << "}";
    return o.str();
}

struct MemoryInfo {
    uint64_t priv = 0, ws = 0, peakWs = 0;
    DWORD handles = 0;
};

MemoryInfo readMemory(HANDLE process) {
    MemoryInfo m;
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof(c);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&c), sizeof(c))) {
        m.priv = c.PrivateUsage;
        m.ws = c.WorkingSetSize;
        m.peakWs = c.PeakWorkingSetSize;
    }
    GetProcessHandleCount(process, &m.handles);
    return m;
}

std::string memJson(const MemoryInfo &m) {
    std::ostringstream o;
    o << "{\"private\":" << m.priv << ",\"working_set\":" << m.ws
      << ",\"peak_working_set\":" << m.peakWs << ",\"handles\":" << m.handles << "}";
    return o.str();
}

ep::Handle openProcess(DWORD pid) {
    ep::Handle p(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
    require(bool(p), "OpenProcess");
    return p;
}

bool processAlive(DWORD pid) {
    if (!pid) return false;
    ep::Handle p(OpenProcess(SYNCHRONIZE, FALSE, pid));
    return p && WaitForSingleObject(p.get(), 0) == WAIT_TIMEOUT;
}

template <class F>
bool waitFor(F condition, uint64_t limitMs) {
    const auto deadline = ep::now() + limitMs;
    for (;;) {
        if (condition()) return true;
        if (ep::now() >= deadline) return false;
        Sleep(10);
    }
}

struct CorpusEntry {
    uint16_t cls = 0;
    std::string text;
};

struct Corpus {
    std::vector<std::string> names;
    std::vector<CorpusEntry> items;
    static Corpus load(const std::filesystem::path &path) {
        Corpus c;
        std::map<std::string, uint16_t> index;
        std::ifstream in(path);
        require(bool(in), "open corpus");
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto tab = line.find('\t');
            if (tab == std::string::npos || !tab || tab + 1 >= line.size()) continue;
            const auto name = line.substr(0, tab), text = line.substr(tab + 1);
            auto [it, added] = index.emplace(name, static_cast<uint16_t>(c.names.size()));
            if (added) c.names.push_back(name);
            c.items.push_back({it->second, text});
        }
        require(!c.items.empty(), "empty corpus");
        return c;
    }
};

struct KeySample {
    uint16_t cls;
    uint16_t pos;
    float ms;
};

struct WorkerSummary {
    uint64_t served = 0;
    uint64_t fallbacks = 0;
    uint64_t memoryBehind = 0;
    uint64_t thinVerified = 0;
    uint64_t visibleDigest = 0;
    uint64_t keyCount = 0;
    uint64_t selectCount = 0;
};

// Declared as `friend struct EngineTsfTest;` inside Service (Windows/TextService.cpp),
// allowing direct benchmark access without modifying production TextService.cpp.
struct EngineTsfTest {
    static std::unique_ptr<Service> makeInproc(const std::filesystem::path &base,
                                               const std::filesystem::path &themes,
                                               const std::filesystem::path &personal,
                                               const std::filesystem::path &memoryFile,
                                               bool simplified = true) {
        auto s = std::make_unique<Service>();
        s->simplified = simplified;
        s->clock = &fixedMoment;
        s->installFolder = base;
        s->conjugations.loadFile(base / L"japanese-conjugations.bin");
        s->baseSnapshot = base_dictionary::Snapshot::load(base);
        s->session = s->baseSnapshot->session(simplified).release();
        require(s->session != nullptr, "inproc session");
        auto baseWords = s->baseSnapshot->words();
        std::vector<JapaneseWord> merged = baseWords;
        std::shared_ptr<const PersonalDictionarySnapshot> personalSnap;
        if (!personal.empty()) {
            personalSnap = readPersonalDictionary(personal);
            if (personalSnap) merged.insert(merged.end(), personalSnap->words.begin(), personalSnap->words.end());
        }
        std::optional<JapanesePackageSnapshot> themeSnap;
        if (!themes.empty()) {
            themeSnap = readJapanesePackageSnapshot(themes);
            if (themeSnap) merged.insert(merged.end(), themeSnap->words.begin(), themeSnap->words.end());
        }
        std::stable_sort(merged.begin(), merged.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        s->japaneseWords = std::move(merged);
        if (!themes.empty() || !personal.empty()) {
            s->themeReload = std::make_unique<ThemeReload>(themes, baseWords,
                themeSnap ? themeSnap->generation : std::string(), personal, personalSnap, std::filesystem::path());
        }
        if (!memoryFile.empty()) {
            s->memory = std::make_unique<selection_memory::Store>(memoryFile);
            s->memoryEnabled = true;
            s->memory->poll(GetTickCount64(), 0);
            s->memory->pollUserWords(GetTickCount64(), 0);
            s->attachPersonal();
        }
        s->localLexiconLoaded = true;
        s->engineClient.enabled = false;
        s->engineClient.stats.localLexicon = true;
        s->engineReleaseProbe = [] { return release::Evaluation{}; };
        return s;
    }

    static std::unique_ptr<Service> makeThin(const std::filesystem::path &base,
                                             const std::filesystem::path &memoryFile,
                                             bool simplified = true) {
        auto s = std::make_unique<Service>();
        s->simplified = simplified;
        s->clock = &fixedMoment;
        s->installFolder = base;
        s->conjugations.loadFile(base / L"japanese-conjugations.bin");
        s->lexicon = Service::createStubLexicon();
        s->session = mpn_session_create(s->lexicon);
        require(s->session != nullptr, "thin session");
        mpn_set_simplified(s->session, simplified);
        if (!memoryFile.empty()) {
            s->memory = std::make_unique<selection_memory::Store>(memoryFile);
            s->memoryEnabled = true;
            s->memory->poll(GetTickCount64(), 0);
            s->memory->pollUserWords(GetTickCount64(), 0);
            s->attachPersonal();
        }
        s->localLexiconLoaded = false;
        s->engineClient.enabled = true;
        s->engineClient.stats.localLexicon = false;
        s->engineReleaseProbe = [] { return release::Evaluation{}; };
        return s;
    }

    // Mirrors the visible page-0 payload formatting of Service::updatePopup().
    static uint64_t formatVisiblePage(const Service &s) {
        uint64_t digest = s.preeditText().size();
        const size_t start = s.page * pageSize;
        const size_t total = s.candidateCount();
        for (size_t i = 0; i < pageSize && start + i < total; ++i) {
            const auto text = s.candidateText(start + i);
            const auto reading = s.candidateReading(start + i);
            const bool typo = typoCandidate(s.session, s.choices[start + i]);
            digest = digest * 131 + text.size() + reading.size() + (typo ? 1 : 0);
            if (!text.empty()) digest ^= static_cast<uint64_t>(text[0]);
        }
        return digest;
    }

    static void attachDirectFactory(Service &s, const std::wstring &pipeName, HANDLE hostProcess, HANDLE readyEvent) {
        s.engineClient.factory = [pipeName, hostProcess, readyEvent] {
            return tsf_engine::Link::threaded([pipeName, hostProcess](uint64_t) -> std::unique_ptr<tsf_engine::Transport> {
                return std::make_unique<tsf_engine::PipeTransport>(ep::Channel::connect(pipeName, hostProcess));
            }, [readyEvent] { SetEvent(readyEvent); }, 60000);
        };
    }

    static void attachResidentFactory(Service &s, const std::wstring &scope, const std::wstring &releaseKey,
                                      const std::wstring &storeSubkey, const std::vector<std::wstring> &ownerArgs,
                                      HANDLE readyEvent) {
        const release::Source source{HKEY_CURRENT_USER, releaseKey, false};
        rs::EnsureOptions opts;
        opts.handoff.discovery.scope = scope;
        opts.store = rs::Store::under(storeSubkey);
        opts.budgetMs = 60000;
        opts.startWaitMs = 60000;
        opts.policy.maxStarts = 1000;
        opts.policy.spacingMs = {0, 0, 0};
        opts.ownerArguments = ownerArgs;
        s.engineReleaseProbe = [source] { return release::evaluate(release::read(source), ProtocolVersion); };
        s.engineClient.factory = [source, opts, readyEvent] {
            return tsf_engine::Link::threaded(tsf_engine::residentConnect(source, opts),
                [readyEvent] { SetEvent(readyEvent); }, 60000);
        };
    }

    static void waitConnected(Service &s, HANDLE readyEvent, uint64_t timeoutMs = 60000) {
        mpn_clear(s.session);
        s.resetCandidates();
        require(mpn_key(s.session, 'n') != 0, "warmup key");
        s.refreshCandidates(true);
        const auto deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            if (!s.engineClient.link()) break;
            const auto st = s.engineClient.link()->state();
            if (st == tsf_engine::Link::State::Ready) break;
            require(st == tsf_engine::Link::State::Connecting, "link failed while connecting");
            WaitForSingleObject(readyEvent, 200);
        }
        require(s.engineClient.link() && s.engineClient.link()->state() == tsf_engine::Link::State::Ready, "link not ready");
        s.onEngineReply();
        while (s.engineClient.pending() && GetTickCount64() < deadline) {
            WaitForSingleObject(readyEvent, 100);
            s.deliverEngine();
        }
        require(!s.engineClient.pending() && !s.choices.empty() && !s.localLexiconLoaded, "warmup reply");
        mpn_clear(s.session);
        s.resetCandidates();
    }

    static double stepKey(Service &s, bool engineMode, HANDLE readyEvent, char c, uint64_t &digest) {
        const double t0 = nowMs();
        require(mpn_key(s.session, c) != 0, "mpn_key");
        s.refreshCandidates(engineMode);
        if (engineMode) {
            const auto deadline = GetTickCount64() + 10000;
            while ((s.engineClient.pending() || s.engineDeferred) && GetTickCount64() < deadline) {
                if (s.engineDeferred) s.onEngineReply();
                else s.deliverEngine();
                if (s.engineClient.pending() || s.engineDeferred) WaitForSingleObject(readyEvent, 100);
            }
            require(!s.engineClient.pending() && !s.engineDeferred, "key engine timeout");
        }
        digest ^= formatVisiblePage(s);
        return nowMs() - t0;
    }

    static double stepSelect(Service &s, bool learn, std::wstring &out) {
        out.clear();
        const double t0 = nowMs();
        require(s.chooseCandidate(0, out, learn), "chooseCandidate");
        if (learn && s.memory) s.memory->flush();
        return nowMs() - t0;
    }

    // Worker 1: startup (cold / warm / inproc).
    static int workerStartup(int argc, wchar_t **argv) {
        // --worker-startup <kind> <mode> <base> <themes|-> <personal|-> <memory|-> <scope_or_pipe> <release_or_host> <store|-> <out>
        require(argc == 12, "workerStartup args");
        const std::wstring kind = argv[2], mode = argv[3];
        const std::filesystem::path base = argv[4];
        const std::filesystem::path themes = std::wstring(argv[5]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[5]);
        const std::filesystem::path personal = std::wstring(argv[6]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[6]);
        const std::filesystem::path memoryFile = std::wstring(argv[7]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[7]);
        const std::wstring target = argv[8], extra = argv[9], storeSubkey = argv[10];
        const std::filesystem::path outPath = argv[11];

        const double tInit0 = nowMs();
        std::unique_ptr<Service> s;
        ep::Handle ready = makeEvent(false);
        if (kind == L"inproc") {
            s = makeInproc(base, themes, personal, memoryFile, true);
        } else {
            s = makeThin(base, memoryFile, true);
            if (mode == L"direct") {
                attachDirectFactory(*s, target, parseHandle(extra.c_str()), ready.get());
            } else {
                std::vector<std::wstring> args{L"--scope", target, L"--release-subkey", extra, L"--store-subkey", storeSubkey,
                    L"--base", base.wstring()};
                if (!themes.empty()) { args.push_back(L"--themes"); args.push_back(themes.wstring()); }
                if (!personal.empty()) { args.push_back(L"--personal"); args.push_back(personal.wstring()); }
                if (!memoryFile.empty()) { args.push_back(L"--memory"); args.push_back(memoryFile.wstring()); }
                attachResidentFactory(*s, target, extra, storeSubkey, args, ready.get());
            }
        }
        const double initMs = nowMs() - tInit0;
        const auto memAfterInit = readMemory(GetCurrentProcess());

        uint64_t digest = 0;
        const bool engineMode = (kind != L"inproc");
        const double tFirst0 = nowMs();
        require(mpn_key(s->session, 'n') != 0, "startup first key");
        s->refreshCandidates(engineMode);
        if (engineMode) {
            const auto deadline = GetTickCount64() + 60000;
            while ((s->engineDeferred || s->engineClient.pending()) && GetTickCount64() < deadline) {
                WaitForSingleObject(ready.get(), 100);
                s->onEngineReply();
            }
            require(!s->engineDeferred && !s->engineClient.pending(), "startup first key engine reply");
            require(!s->localLexiconLoaded && !s->engineClient.stats.fallbacks, "startup thin no fallback");
        }
        digest ^= formatVisiblePage(*s);
        const double firstKeyMs = nowMs() - tFirst0;
        require(!s->choices.empty(), "startup choices non-empty");
        const auto memAfterFirst = readMemory(GetCurrentProcess());

        // Type "ihao" to confirm steady-state hot path immediately afterwards.
        std::vector<double> nextKeys;
        for (char c : std::string("ihao")) nextKeys.push_back(stepKey(*s, engineMode, ready.get(), c, digest));
        const double nextKeyMedian = computeStats(nextKeys).median;
        const auto memAfterSteady = readMemory(GetCurrentProcess());
        if (engineMode) require(!s->localLexiconLoaded && !s->engineClient.stats.fallbacks, "startup steady no fallback");

        std::ofstream out(outPath);
        require(bool(out), "startup output");
        out << std::fixed << std::setprecision(4)
            << "{\"init_ms\":" << initMs
            << ",\"first_key_ms\":" << firstKeyMs
            << ",\"total_to_first_ms\":" << (initMs + firstKeyMs)
            << ",\"next_key_median_ms\":" << nextKeyMedian
            << ",\"thin\":" << (!s->localLexiconLoaded ? "true" : "false")
            << ",\"fallbacks\":" << s->engineClient.stats.fallbacks
            << ",\"digest\":" << digest
            << ",\"mem_init\":" << memJson(memAfterInit)
            << ",\"mem_first\":" << memJson(memAfterFirst)
            << ",\"mem_steady\":" << memJson(memAfterSteady) << "}\n";
        s->engineClient.close();
        return 0;
    }

    // Worker 2: concurrent key-to-candidate-visible + selection (Inspect).
    static int workerKeys(int argc, wchar_t **argv) {
        // --worker-keys <inproc|engine> <direct|resident> <base> <themes|-> <personal|-> <memory|-> <target> <extra> <store|-> <corpus> <outBin> <start> <ready> <done> <release>
        require(argc == 17, "workerKeys args");
        const bool engineMode = std::wstring(argv[2]) == L"engine";
        const std::wstring mode = argv[3];
        const std::filesystem::path base = argv[4];
        const std::filesystem::path themes = std::wstring(argv[5]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[5]);
        const std::filesystem::path personal = std::wstring(argv[6]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[6]);
        const std::filesystem::path memoryFile = std::wstring(argv[7]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[7]);
        const std::wstring target = argv[8], extra = argv[9], storeSubkey = argv[10];
        const auto corpus = Corpus::load(argv[11]);
        const std::filesystem::path outPath = argv[12];
        const HANDLE startEv = parseHandle(argv[13]);
        const HANDLE readyEv = parseHandle(argv[14]);
        const HANDLE doneEv = parseHandle(argv[15]);
        const HANDLE releaseEv = parseHandle(argv[16]);

        ep::Handle replyEv = makeEvent(false);
        std::unique_ptr<Service> s;
        if (!engineMode) {
            s = makeInproc(base, themes, personal, memoryFile, true);
        } else {
            s = makeThin(base, memoryFile, true);
            if (mode == L"direct") {
                attachDirectFactory(*s, target, parseHandle(extra.c_str()), replyEv.get());
            } else {
                std::vector<std::wstring> args{L"--scope", target, L"--release-subkey", extra, L"--store-subkey", storeSubkey,
                    L"--base", base.wstring()};
                if (!themes.empty()) { args.push_back(L"--themes"); args.push_back(themes.wstring()); }
                if (!personal.empty()) { args.push_back(L"--personal"); args.push_back(personal.wstring()); }
                if (!memoryFile.empty()) { args.push_back(L"--memory"); args.push_back(memoryFile.wstring()); }
                attachResidentFactory(*s, target, extra, storeSubkey, args, replyEv.get());
            }
            waitConnected(*s, replyEv.get());
        }

        SetEvent(readyEv);
        require(WaitForSingleObject(startEv, 120000) == WAIT_OBJECT_0, "worker start wait");

        std::vector<KeySample> keySamples;
        std::vector<float> selectSamples;
        keySamples.reserve(corpus.items.size() * 12);
        selectSamples.reserve(corpus.items.size());
        uint64_t digest = 0;
        std::wstring committed;

        for (const auto &item : corpus.items) {
            mpn_clear(s->session);
            s->resetCandidates();
            for (size_t i = 0; i < item.text.size(); ++i) {
                const double elapsed = stepKey(*s, engineMode, replyEv.get(), item.text[i], digest);
                keySamples.push_back({item.cls, static_cast<uint16_t>(i + 1), static_cast<float>(elapsed)});
            }
            if (!s->choices.empty()) {
                selectSamples.push_back(static_cast<float>(stepSelect(*s, false, committed)));
            }
        }
        mpn_clear(s->session);
        s->resetCandidates();

        WorkerSummary summary;
        summary.served = s->engineClient.stats.served;
        summary.fallbacks = s->engineClient.stats.fallbacks;
        summary.memoryBehind = s->engineClient.stats.memoryBehind;
        summary.thinVerified = (!engineMode || !s->localLexiconLoaded) ? 1 : 0;
        summary.visibleDigest = digest;
        summary.keyCount = keySamples.size();
        summary.selectCount = selectSamples.size();

        {
            std::ofstream out(outPath, std::ios::binary);
            require(bool(out), "worker bin open");
            out.write(reinterpret_cast<const char *>(&summary), sizeof(summary));
            out.write(reinterpret_cast<const char *>(keySamples.data()), static_cast<std::streamsize>(keySamples.size() * sizeof(KeySample)));
            out.write(reinterpret_cast<const char *>(selectSamples.data()), static_cast<std::streamsize>(selectSamples.size() * sizeof(float)));
            require(bool(out), "worker bin write");
        }

        SetEvent(doneEv);
        require(WaitForSingleObject(releaseEv, 120000) == WAIT_OBJECT_0, "worker release wait");
        s->engineClient.close();
        return 0;
    }

    // Worker 3: idle standby app (DLL activated, never typed or already yielded).
    static int workerStandby(int argc, wchar_t **argv) {
        // --worker-standby <inproc|engine> <base> <themes|-> <personal|-> <memory|-> <ready> <release>
        require(argc == 9, "workerStandby args");
        const bool engineMode = std::wstring(argv[2]) == L"engine";
        const std::filesystem::path base = argv[3];
        const std::filesystem::path themes = std::wstring(argv[4]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[4]);
        const std::filesystem::path personal = std::wstring(argv[5]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[5]);
        const std::filesystem::path memoryFile = std::wstring(argv[6]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[6]);
        const HANDLE readyEv = parseHandle(argv[7]);
        const HANDLE releaseEv = parseHandle(argv[8]);

        auto s = engineMode ? makeThin(base, memoryFile, true) : makeInproc(base, themes, personal, memoryFile, true);
        SetEvent(readyEv);
        require(WaitForSingleObject(releaseEv, 180000) == WAIT_OBJECT_0, "standby release");
        s->engineClient.close();
        return 0;
    }

    // Worker 4: deep paging & multi-round expansion comparison (inproc vs thin engine).
    static int workerDeepPage(int argc, wchar_t **argv) {
        // --worker-deep <inproc|engine> <direct|resident> <base> <themes|-> <personal|-> <memory|-> <target> <extra> <store|-> <out>
        require(argc == 12, "workerDeepPage args");
        const bool engineMode = std::wstring(argv[2]) == L"engine";
        const std::wstring mode = argv[3];
        const std::filesystem::path base = argv[4];
        const std::filesystem::path themes = std::wstring(argv[5]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[5]);
        const std::filesystem::path personal = std::wstring(argv[6]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[6]);
        const std::filesystem::path memoryFile = std::wstring(argv[7]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[7]);
        const std::wstring target = argv[8], extra = argv[9], storeSubkey = argv[10];
        const std::filesystem::path outPath = argv[11];

        static const std::vector<std::string> readings{
            "shi", "yi", "ji", "li", "zhi", "zhongguo", "wozaiditieshang", "xianzai", "wenti", "keyi"
        };

        ep::Handle replyEv = makeEvent(false);
        auto s = engineMode ? makeThin(base, memoryFile, true) : makeInproc(base, themes, personal, memoryFile, true);
        if (engineMode) {
            if (mode == L"direct") {
                attachDirectFactory(*s, target, parseHandle(extra.c_str()), replyEv.get());
            } else {
                std::vector<std::wstring> args{L"--scope", target, L"--release-subkey", extra, L"--store-subkey", storeSubkey,
                    L"--base", base.wstring()};
                if (!themes.empty()) { args.push_back(L"--themes"); args.push_back(themes.wstring()); }
                if (!personal.empty()) { args.push_back(L"--personal"); args.push_back(personal.wstring()); }
                if (!memoryFile.empty()) { args.push_back(L"--memory"); args.push_back(memoryFile.wstring()); }
                attachResidentFactory(*s, target, extra, storeSubkey, args, replyEv.get());
            }
            waitConnected(*s, replyEv.get());
        }

        std::vector<double> initialMs, expandMs, pageStepMs;
        std::ostringstream perInput;
        perInput << "[";
        bool first = true;
        uint64_t digest = 0;

        for (const auto &reading : readings) {
            mpn_clear(s->session);
            s->resetCandidates();
            double lastKey = 0;
            for (char c : reading) lastKey = stepKey(*s, engineMode, replyEv.get(), c, digest);
            initialMs.push_back(lastKey);
            const size_t initialCount = s->candidateCount();

            size_t expansions = 0, pages = 1;
            std::vector<double> thisExpand;
            for (;;) {
                const double tPage = nowMs();
                if ((s->page + 1) * pageSize >= s->candidateCount()) {
                    if (!s->canExpandCandidates()) break;
                    const double tExp = nowMs();
                    s->expandCandidates();
                    const double expElapsed = nowMs() - tExp;
                    expandMs.push_back(expElapsed);
                    thisExpand.push_back(expElapsed);
                    ++expansions;
                }
                if ((s->page + 1) * pageSize < s->candidateCount()) {
                    ++s->page;
                    ++pages;
                    digest ^= formatVisiblePage(*s);
                    pageStepMs.push_back(nowMs() - tPage);
                } else {
                    break;
                }
            }
            // Hash all expanded candidate texts for exact parity checking.
            uint64_t listHash = 1469598103934665603ULL;
            for (const auto &c : s->choices) {
                for (unsigned char b : c.text) { listHash ^= b; listHash *= 1099511628211ULL; }
                listHash ^= 0xff; listHash *= 1099511628211ULL;
            }
            if (!first) perInput << ",";
            first = false;
            perInput << std::fixed << std::setprecision(4)
                     << "{\"reading\":" << jsonEscape(reading)
                     << ",\"initial_count\":" << initialCount
                     << ",\"final_count\":" << s->candidateCount()
                     << ",\"pages\":" << pages
                     << ",\"expansions\":" << expansions
                     << ",\"initial_ms\":" << lastKey
                     << ",\"expand_total_ms\":" << [&] { double sum = 0; for (double v : thisExpand) sum += v; return sum; }()
                     << ",\"list_hash\":" << listHash << "}";
        }
        perInput << "]";
        if (engineMode) require(!s->localLexiconLoaded && !s->engineClient.stats.fallbacks, "deep page no fallback");

        std::ofstream out(outPath);
        require(bool(out), "deep output");
        out << "{\"mode\":" << jsonEscape(engineMode ? "engine" : "inproc")
            << ",\"thin\":" << (!s->localLexiconLoaded ? "true" : "false")
            << ",\"fallbacks\":" << s->engineClient.stats.fallbacks
            << ",\"initial_ms\":" << statsJson(computeStats(initialMs))
            << ",\"expand_ms\":" << statsJson(computeStats(expandMs))
            << ",\"page_step_ms\":" << statsJson(computeStats(pageStepMs))
            << ",\"inputs\":" << perInput.str() << "}\n";
        s->engineClient.close();
        return 0;
    }

    // Worker 5: large personal table + heavy selection memory + post-learn reload.
    static int workerLargePersonal(int argc, wchar_t **argv) {
        // --worker-large <inproc|engine> <direct|resident> <base> <themes|-> <personal> <memory> <target> <extra> <store|-> <out>
        require(argc == 12, "workerLargePersonal args");
        const bool engineMode = std::wstring(argv[2]) == L"engine";
        const std::wstring mode = argv[3];
        const std::filesystem::path base = argv[4];
        const std::filesystem::path themes = std::wstring(argv[5]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[5]);
        const std::filesystem::path personal = argv[6];
        const std::filesystem::path memoryFile = argv[7];
        const std::wstring target = argv[8], extra = argv[9], storeSubkey = argv[10];
        const std::filesystem::path outPath = argv[11];

        ep::Handle replyEv = makeEvent(false);
        const double tInit0 = nowMs();
        auto s = engineMode ? makeThin(base, memoryFile, true) : makeInproc(base, themes, personal, memoryFile, true);
        if (engineMode) {
            if (mode == L"direct") {
                attachDirectFactory(*s, target, parseHandle(extra.c_str()), replyEv.get());
            } else {
                std::vector<std::wstring> args{L"--scope", target, L"--release-subkey", extra, L"--store-subkey", storeSubkey,
                    L"--base", base.wstring(), L"--personal", personal.wstring(), L"--memory", memoryFile.wstring()};
                if (!themes.empty()) { args.push_back(L"--themes"); args.push_back(themes.wstring()); }
                attachResidentFactory(*s, target, extra, storeSubkey, args, replyEv.get());
            }
            waitConnected(*s, replyEv.get());
        }
        const double readyMs = nowMs() - tInit0;
        const auto memReady = readMemory(GetCurrentProcess());

        // Mix of daily Chinese, synthetic personal Japanese, synthetic user Chinese words, and remembered phrases.
        static const std::vector<std::string> steadyInputs{
            "nihao", "zhongguo", "wozaiditieshang", "xianzai", "wenti", "keyi",
            "jpkaaa", "jpkaab", "jpkaac", "jpkaad", "jpkaae",
            "usrkaaa", "usrkaab", "usrkaac", "usrkaad", "usrkaae",
            "memkaaa", "memkaab", "memkaac", "memkaad", "memkaae"
        };
        std::vector<double> steadyKeyMs, learnCommitMs, postLearnFirstKeyMs;
        uint64_t digest = 0;

        for (int round = 0; round < 3; ++round) {
            for (const auto &input : steadyInputs) {
                mpn_clear(s->session);
                s->resetCandidates();
                for (char c : input) steadyKeyMs.push_back(stepKey(*s, engineMode, replyEv.get(), c, digest));
            }
        }

        // Measure 10 learn + flush cycles followed immediately by a next-word query
        // (forces engine to detect updated selection-memory digest and reload the large file).
        static const std::vector<std::string> learnInputs{
            "nihao", "zhongguo", "shijie", "gongzuo", "xuexi",
            "pengyou", "tianqi", "diannao", "shijian", "wenti"
        };
        std::wstring committed;
        for (size_t i = 0; i < learnInputs.size(); ++i) {
            mpn_clear(s->session);
            s->resetCandidates();
            for (char c : learnInputs[i]) stepKey(*s, engineMode, replyEv.get(), c, digest);
            learnCommitMs.push_back(stepSelect(*s, true, committed));
            // Re-sync personal table on inproc (as OnKeyDown does on the next key).
            if (s->memory) { s->memory->poll(GetTickCount64(), 0); s->memory->pollUserWords(GetTickCount64(), 0); }
            s->attachPersonal();
            mpn_clear(s->session);
            s->resetCandidates();
            const std::string &nextWord = learnInputs[(i + 1) % learnInputs.size()];
            postLearnFirstKeyMs.push_back(stepKey(*s, engineMode, replyEv.get(), nextWord[0], digest));
        }

        const auto memEnd = readMemory(GetCurrentProcess());
        if (engineMode) require(!s->localLexiconLoaded && !s->engineClient.stats.fallbacks, "large personal no fallback");

        std::ofstream out(outPath);
        require(bool(out), "large output");
        out << std::fixed << std::setprecision(4)
            << "{\"mode\":" << jsonEscape(engineMode ? "engine" : "inproc")
            << ",\"thin\":" << (!s->localLexiconLoaded ? "true" : "false")
            << ",\"ready_ms\":" << readyMs
            << ",\"fallbacks\":" << s->engineClient.stats.fallbacks
            << ",\"memory_behind\":" << s->engineClient.stats.memoryBehind
            << ",\"steady_key_ms\":" << statsJson(computeStats(steadyKeyMs))
            << ",\"learn_commit_ms\":" << statsJson(computeStats(learnCommitMs))
            << ",\"post_learn_first_key_ms\":" << statsJson(computeStats(postLearnFirstKeyMs))
            << ",\"mem_ready\":" << memJson(memReady)
            << ",\"mem_end\":" << memJson(memEnd) << "}\n";
        s->engineClient.close();
        return 0;
    }
};

struct Env {
    bool residentMode = true;
    std::filesystem::path root, version, ownerExe, hostExe, base, themes, personal, memoryFile;
    std::wstring testKey, releaseKey, scope;
    rs::Store store;
    std::vector<DWORD> tracked;
    std::unique_ptr<EngineProcess> directHost;

    release::Snapshot snapshot() const { return release::read({HKEY_CURRENT_USER, releaseKey, false}); }
    rs::Status status() const { return rs::inspect(store, scope); }

    std::vector<std::wstring> ownerArgs(const std::filesystem::path &usePersonal,
                                        const std::filesystem::path &useMemory) const {
        std::vector<std::wstring> args{L"--scope", scope, L"--release-subkey", releaseKey, L"--store-subkey", testKey,
            L"--base", base.wstring()};
        if (!themes.empty()) { args.push_back(L"--themes"); args.push_back(themes.wstring()); }
        if (!usePersonal.empty()) { args.push_back(L"--personal"); args.push_back(usePersonal.wstring()); }
        if (!useMemory.empty()) { args.push_back(L"--memory"); args.push_back(useMemory.wstring()); }
        return args;
    }

    rs::EnsureOptions ensureOptions(const std::filesystem::path &usePersonal,
                                    const std::filesystem::path &useMemory) const {
        rs::EnsureOptions o;
        o.handoff.discovery.scope = scope;
        o.store = store;
        o.budgetMs = 60000;
        o.startWaitMs = 60000;
        o.policy.maxStarts = 1000;
        o.policy.spacingMs = {0, 0, 0};
        o.ownerArguments = ownerArgs(usePersonal, useMemory);
        return o;
    }

    void startDirectHost(const std::filesystem::path &usePersonal, const std::filesystem::path &useMemory) {
        stopDirectHost();
        EngineProcess::Config cfg{hostExe, base, themes, usePersonal};
        cfg.memory = useMemory;
        cfg.dynamicEndpoints = true;
        cfg.connections = 8;
        cfg.threads = rs::HostThreads;
        cfg.hostReadMs = rs::HostReadMs;
        cfg.hostIdleEvictMs = rs::SlotIdleEvictMs;
        directHost = std::make_unique<EngineProcess>(cfg);
        const auto until = ep::now() + 60000;
        while (directHost->poll(ep::now()) == EngineProcess::State::Starting) {
            auto evs = directHost->waitHandles();
            require(ep::now() < until, "direct host start timeout");
            if (!evs.empty()) WaitForMultipleObjects(static_cast<DWORD>(evs.size()), evs.data(), FALSE, 200);
        }
        require(directHost->status().state == EngineProcess::State::Ready, "direct host ready");
    }

    void stopDirectHost() {
        if (!directHost) return;
        directHost->requestStop(ep::now());
        const auto until = ep::now() + 15000;
        while (directHost->poll(ep::now()) != EngineProcess::State::Exited && ep::now() < until) {
            auto evs = directHost->waitHandles();
            if (!evs.empty()) WaitForMultipleObjects(static_cast<DWORD>(evs.size()), evs.data(), FALSE, 200);
        }
        directHost.reset();
    }

    engine::EngineRegistration::Endpoint allocateDirect() {
        require(directHost != nullptr, "directHost active");
        require(directHost->requestControl(engine::control::Operation::Allocate, ep::now(), 10000), "direct allocate request");
        const auto until = ep::now() + 15000;
        while (directHost->status().controlState == engine::control::State::Pending) {
            auto evs = directHost->waitHandles();
            require(ep::now() < until, "direct allocate timeout");
            if (!evs.empty()) WaitForMultipleObjects(static_cast<DWORD>(evs.size()), evs.data(), FALSE, 200);
            directHost->poll(ep::now());
        }
        auto res = directHost->takeControl(ep::now());
        require(res && res->endpoint, "direct allocate result");
        return std::move(*res->endpoint);
    }

    void stopResidentOwner() {
        if (!residentMode) return;
        const auto st = status();
        if (st.ownerPid && processAlive(st.ownerPid)) {
            std::pair<DWORD, HWND> found(st.ownerPid, nullptr);
            EnumWindows([](HWND hwnd, LPARAM p) -> BOOL {
                auto *f = reinterpret_cast<std::pair<DWORD, HWND> *>(p);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                wchar_t cls[64]{};
                GetClassNameW(hwnd, cls, 64);
                if (pid == f->first && std::wstring(cls) == rs::WindowClass) { f->second = hwnd; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&found));
            if (found.second) PostMessageW(found.second, WM_CLOSE, 0, 0);
            if (!waitFor([&] { return !processAlive(st.ownerPid) && (!st.hostPid || !processAlive(st.hostPid)); }, 15000)) {
                for (DWORD pid : {st.ownerPid, st.hostPid}) {
                    if (pid && processAlive(pid)) {
                        ep::Handle p(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
                        if (p) TerminateProcess(p.get(), 9);
                    }
                }
            }
        }
    }

    void cleanup() noexcept {
        try { stopDirectHost(); } catch (...) {}
        try { stopResidentOwner(); } catch (...) {}
        for (DWORD pid : tracked) {
            if (pid && processAlive(pid)) {
                ep::Handle p(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
                if (p) TerminateProcess(p.get(), 9);
            }
        }
        if (!testKey.empty()) {
            RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
            RegDeleteKeyW(HKEY_CURRENT_USER, testKey.c_str());
        }
        std::error_code ignored;
        if (!root.empty()) std::filesystem::remove_all(root, ignored);
    }
};

Env *gEnv = nullptr;

struct ChildProcess {
    ep::Handle process, job;
    std::vector<ep::Handle> inheritedCopies;

    static ChildProcess spawn(const std::vector<std::wstring> &args, const std::vector<HANDLE> &inheritList,
                              std::vector<size_t> handleArgIndices = {}) {
        ChildProcess child;
        child.job.reset(CreateJobObjectW(nullptr, nullptr));
        require(bool(child.job), "CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0, "SetInformationJobObject");

        std::vector<HANDLE> dupHandles;
        dupHandles.reserve(inheritList.size() + 1);
        for (HANDLE h : inheritList) {
            HANDLE raw = nullptr;
            require(DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &raw, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0, "DuplicateHandle");
            child.inheritedCopies.emplace_back(raw);
            dupHandles.push_back(raw);
        }
        HANDLE errOut = nullptr;
        HANDLE parentErr = GetStdHandle(STD_ERROR_HANDLE);
        if (parentErr && parentErr != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), parentErr, GetCurrentProcess(), &errOut, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            child.inheritedCopies.emplace_back(errOut);
            dupHandles.push_back(errOut);
        }

        std::vector<std::wstring> finalArgs = args;
        for (size_t i = 0; i < handleArgIndices.size(); ++i) {
            finalArgs[handleArgIndices[i]] = std::to_wstring(reinterpret_cast<uintptr_t>(dupHandles[i]));
        }
        std::wstring cmd;
        for (const auto &a : finalArgs) {
            if (!cmd.empty()) cmd += L' ';
            cmd += quoteArg(a);
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        if (errOut) {
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdError = errOut;
        }
        std::vector<uint8_t> attrBytes;
        LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
        if (!dupHandles.empty()) {
            SIZE_T sz = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
            attrBytes.resize(sz);
            attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBytes.data());
            require(InitializeProcThreadAttributeList(attrList, 1, 0, &sz) != 0, "InitializeProcThreadAttributeList");
            require(UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                dupHandles.data(), dupHandles.size() * sizeof(HANDLE), nullptr, nullptr) != 0, "UpdateProcThreadAttribute");
            si.lpAttributeList = attrList;
        }
        struct AttrGuard { LPPROC_THREAD_ATTRIBUTE_LIST p; ~AttrGuard() { if (p) DeleteProcThreadAttributeList(p); } } guard{attrList};

        PROCESS_INFORMATION pi{};
        const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (!dupHandles.empty() ? EXTENDED_STARTUPINFO_PRESENT : 0);
        require(CreateProcessW(selfExe().c_str(), cmd.data(), nullptr, nullptr, !dupHandles.empty() ? TRUE : FALSE,
            flags, nullptr, nullptr, &si.StartupInfo, &pi) != 0, "CreateProcessW child");
        child.process.reset(pi.hProcess);
        ep::Handle thread(pi.hThread);
        if (!AssignProcessToJobObject(child.job.get(), child.process.get())) {
            TerminateProcess(child.process.get(), 99);
            require(false, "AssignProcessToJobObject");
        }
        ResumeThread(thread.get());
        return child;
    }

    void waitSuccess(DWORD timeoutMs = 300000) {
        require(WaitForSingleObject(process.get(), timeoutMs) == WAIT_OBJECT_0, "child timeout");
        DWORD code = 1;
        require(GetExitCodeProcess(process.get(), &code) && code == 0, "child failed");
    }
};

std::string readWholeFile(const std::filesystem::path &path) {
    std::ifstream in(path);
    require(bool(in), "readWholeFile");
    std::string data{std::istreambuf_iterator<char>(in), {}};
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) data.pop_back();
    return data;
}

std::wstring pathOrDash(const std::filesystem::path &p) {
    return p.empty() ? L"-" : p.wstring();
}

void writeReleaseEntry(const Env &e) {
    auto sha = [](const std::filesystem::path &path) {
        const auto hex = base_dictionary::StableFile(path).read(true);
        return std::wstring(hex.begin(), hex.end());
    };
    const std::wstring id = e.version.filename().wstring();
    std::wstring multi;
    for (const auto &line : {
            std::wstring(L"format=harukoto-engine-release/1"), L"id=" + id, std::wstring(L"sequence=1"),
            L"protocol=" + std::to_wstring(ProtocolVersion),
            L"host=" + e.hostExe.wstring(), L"host-sha256=" + sha(e.hostExe),
            L"owner=" + e.ownerExe.wstring(), L"owner-sha256=" + sha(e.ownerExe)}) {
        multi += line + L'\0';
    }
    multi += L'\0';
    HKEY key = nullptr;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, e.releaseKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create release key");
    require(RegSetValueExW(key, id.c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE *>(multi.data()),
        static_cast<DWORD>(multi.size() * sizeof(wchar_t))) == ERROR_SUCCESS, "write release value");
    RegCloseKey(key);
}

MemoryInfo engineOwnerMemory(const Env &e) {
    if (!e.residentMode) return {};
    const auto st = e.status();
    if (!st.ownerPid || !processAlive(st.ownerPid)) return {};
    return readMemory(openProcess(st.ownerPid).get());
}

MemoryInfo engineHostMemory(const Env &e) {
    if (e.residentMode) {
        const auto st = e.status();
        if (!st.hostPid || !processAlive(st.hostPid)) return {};
        return readMemory(openProcess(st.hostPid).get());
    }
    return e.directHost && e.directHost->engineHandle() ? readMemory(e.directHost->engineHandle()) : MemoryInfo{};
}

// Section 1: startup (inproc, engine-cold, engine-warm).
void runStartupSection(Env &e, int coldRuns, int warmRuns) {
    auto runOne = [&](const std::wstring &kind, int runIdx) {
        const auto outFile = e.root / (L"startup-" + kind + L"-" + std::to_wstring(runIdx) + L".json");
        if (kind == L"engine-cold") e.stopResidentOwner();

        std::vector<std::wstring> args{
            selfExe().wstring(), L"--worker-startup", kind, e.residentMode ? L"resident" : L"direct",
            e.base.wstring(), pathOrDash(e.themes), pathOrDash(e.personal), pathOrDash(e.memoryFile),
            L"-", L"-", e.residentMode ? e.testKey : L"-", outFile.wstring()
        };
        std::vector<HANDLE> inherit;
        std::vector<size_t> handleIdx;
        std::optional<engine::EngineRegistration::Endpoint> ep;
        if (kind != L"inproc") {
            if (e.residentMode) {
                args[8] = e.scope;
                args[9] = e.releaseKey;
            } else {
                ep = e.allocateDirect();
                args[8] = ep->name;
                inherit.push_back(ep->process.get());
                handleIdx.push_back(9);
            }
        }
        auto child = ChildProcess::spawn(args, inherit, handleIdx);
        child.waitSuccess(120000);
        const std::string payload = readWholeFile(outFile);
        if (kind == L"engine-cold") {
            const auto st = e.status();
            if (st.ownerPid) e.tracked.push_back(st.ownerPid);
            if (st.hostPid) e.tracked.push_back(st.hostPid);
        }
        const auto ownerMem = (kind == L"inproc") ? MemoryInfo{} : engineOwnerMemory(e);
        const auto hostMem = (kind == L"inproc") ? MemoryInfo{} : engineHostMemory(e);
        std::string kindNarrow;
        for (wchar_t ch : kind) kindNarrow += static_cast<char>(ch < 128 ? ch : '?');
        std::cout << "{\"type\":\"startup\",\"kind\":" << jsonEscape(kindNarrow)
                  << ",\"run\":" << runIdx
                  << ",\"client\":" << payload
                  << ",\"owner\":" << memJson(ownerMem)
                  << ",\"host\":" << memJson(hostMem) << "}\n";
        if (kind == L"engine-cold") e.stopResidentOwner();
    };

    for (int i = 0; i < warmRuns; ++i) runOne(L"inproc", i);
    if (e.residentMode) {
        for (int i = 0; i < coldRuns; ++i) runOne(L"engine-cold", i);
        // Ensure resident engine is up for warm runs.
        auto ensured = rs::ensure(e.snapshot(), e.ensureOptions(e.personal, e.memoryFile));
        require(ensured.attach.outcome.result == hf::Result::Admitted, "warm resident ensure");
        e.tracked.push_back(ensured.attach.outcome.session->engine().ownerPid);
        e.tracked.push_back(ensured.attach.outcome.session->engine().hostPid);
        ensured.attach.outcome.session.reset();
    } else {
        e.startDirectHost(e.personal, e.memoryFile);
    }
    for (int i = 0; i < warmRuns; ++i) runOne(L"engine-warm", i);
}

// Section 2: concurrent key-to-candidate-visible + selection + multi-app memory.
void runKeysSection(Env &e, const std::filesystem::path &corpusPath, const Corpus &corpus,
                    const std::vector<int> &counts, int standbyCount) {
    auto runGroup = [&](bool engineMode, int activeCount, int standbys) {
        if (engineMode) {
            if (e.residentMode) {
                auto ensured = rs::ensure(e.snapshot(), e.ensureOptions(e.personal, e.memoryFile));
                require(ensured.attach.outcome.result == hf::Result::Admitted, "keys resident ensure");
                e.tracked.push_back(ensured.attach.outcome.session->engine().ownerPid);
                e.tracked.push_back(ensured.attach.outcome.session->engine().hostPid);
                ensured.attach.outcome.session.reset();
            } else {
                e.startDirectHost(e.personal, e.memoryFile);
            }
        }

        ep::Handle startEv = makeEvent(true), releaseEv = makeEvent(true);
        struct ActiveWorker {
            ep::Handle ready = makeEvent(true), done = makeEvent(true);
            std::filesystem::path outFile;
            ChildProcess proc;
        };
        struct StandbyWorker {
            ep::Handle ready = makeEvent(true);
            ChildProcess proc;
        };
        std::vector<ActiveWorker> actives;
        actives.reserve(activeCount);
        std::vector<StandbyWorker> idleWorkers;
        idleWorkers.reserve(standbys);

        for (int i = 0; i < standbys; ++i) {
            StandbyWorker sw;
            std::vector<std::wstring> args{
                selfExe().wstring(), L"--worker-standby", engineMode ? L"engine" : L"inproc",
                e.base.wstring(), pathOrDash(e.themes), pathOrDash(e.personal), pathOrDash(e.memoryFile),
                L"0", L"0"
            };
            sw.proc = ChildProcess::spawn(args, {sw.ready.get(), releaseEv.get()}, {7, 8});
            idleWorkers.push_back(std::move(sw));
        }

        std::vector<engine::EngineRegistration::Endpoint> endpoints;
        for (int i = 0; i < activeCount; ++i) {
            ActiveWorker aw;
            aw.outFile = e.root / (L"keys-" + std::wstring(engineMode ? L"e" : L"i") + L"-" +
                std::to_wstring(activeCount) + L"-" + std::to_wstring(i) + L".bin");
            std::vector<std::wstring> args{
                selfExe().wstring(), L"--worker-keys", engineMode ? L"engine" : L"inproc",
                e.residentMode ? L"resident" : L"direct",
                e.base.wstring(), pathOrDash(e.themes), pathOrDash(e.personal), pathOrDash(e.memoryFile),
                L"-", L"-", e.residentMode ? e.testKey : L"-",
                corpusPath.wstring(), aw.outFile.wstring(), L"0", L"0", L"0", L"0"
            };
            std::vector<HANDLE> inherit{startEv.get(), aw.ready.get(), aw.done.get(), releaseEv.get()};
            std::vector<size_t> idx{13, 14, 15, 16};
            if (engineMode) {
                if (e.residentMode) {
                    args[8] = e.scope;
                    args[9] = e.releaseKey;
                } else {
                    endpoints.push_back(e.allocateDirect());
                    args[8] = endpoints.back().name;
                    inherit.push_back(endpoints.back().process.get());
                    idx.push_back(9);
                }
            }
            aw.proc = ChildProcess::spawn(args, inherit, idx);
            actives.push_back(std::move(aw));
        }

        for (auto &sw : idleWorkers) require(WaitForSingleObject(sw.ready.get(), 120000) == WAIT_OBJECT_0, "standby ready");
        for (auto &aw : actives) require(WaitForSingleObject(aw.ready.get(), 120000) == WAIT_OBJECT_0, "active ready");

        auto sampleMem = [&] {
            uint64_t activePriv = 0, standbyPriv = 0;
            std::vector<double> perClientMib;
            for (const auto &aw : actives) {
                const auto m = readMemory(aw.proc.process.get());
                activePriv += m.priv;
                perClientMib.push_back(double(m.priv) / 1048576.0);
            }
            for (const auto &sw : idleWorkers) {
                standbyPriv += readMemory(sw.proc.process.get()).priv;
            }
            const uint64_t ownerPriv = engineMode ? engineOwnerMemory(e).priv : 0;
            const uint64_t hostPriv = engineMode ? engineHostMemory(e).priv : 0;
            const uint64_t total = activePriv + standbyPriv + ownerPriv + hostPriv;
            std::ostringstream o;
            o << std::fixed << std::setprecision(4)
              << "{\"active_clients\":" << activePriv
              << ",\"standby_clients\":" << standbyPriv
              << ",\"owner\":" << ownerPriv
              << ",\"host\":" << hostPriv
              << ",\"total\":" << total
              << ",\"per_active_client_mib\":" << statsJson(computeStats(perClientMib)) << "}";
            return o.str();
        };

        const std::string memReady = sampleMem();
        const double wall0 = nowMs();
        SetEvent(startEv.get());
        for (auto &aw : actives) require(WaitForSingleObject(aw.done.get(), 600000) == WAIT_OBJECT_0, "active done");
        const double wallMs = nowMs() - wall0;
        const std::string memHeld = sampleMem();

        SetEvent(releaseEv.get());
        for (auto &aw : actives) aw.proc.waitSuccess(60000);
        for (auto &sw : idleWorkers) sw.proc.waitSuccess(60000);

        std::vector<double> overall, selects;
        std::map<std::string, std::vector<double>> byClass, byPos;
        uint64_t totalServed = 0, totalFallbacks = 0, totalBehind = 0;
        bool allThin = true;
        for (const auto &aw : actives) {
            std::ifstream in(aw.outFile, std::ios::binary);
            require(bool(in), "read worker bin");
            WorkerSummary ws{};
            in.read(reinterpret_cast<char *>(&ws), sizeof(ws));
            require(bool(in), "read worker header");
            totalServed += ws.served;
            totalFallbacks += ws.fallbacks;
            totalBehind += ws.memoryBehind;
            allThin = allThin && (ws.thinVerified != 0);
            std::vector<KeySample> ks(ws.keyCount);
            in.read(reinterpret_cast<char *>(ks.data()), static_cast<std::streamsize>(ks.size() * sizeof(KeySample)));
            std::vector<float> ss(ws.selectCount);
            in.read(reinterpret_cast<char *>(ss.data()), static_cast<std::streamsize>(ss.size() * sizeof(float)));
            require(bool(in), "read worker samples");
            for (const auto &s : ks) {
                overall.push_back(s.ms);
                byClass[corpus.names[s.cls]].push_back(s.ms);
                const char *bucket = s.pos <= 4 ? "1-4" : s.pos <= 8 ? "5-8" : s.pos <= 16 ? "9-16" : "17+";
                byPos[bucket].push_back(s.ms);
            }
            for (float v : ss) selects.push_back(v);
        }

        auto mapJson = [](const std::map<std::string, std::vector<double>> &m) {
            std::ostringstream o;
            o << "{";
            bool first = true;
            for (const auto &[k, v] : m) {
                if (!first) o << ",";
                first = false;
                o << jsonEscape(k) << ":" << statsJson(computeStats(v));
            }
            o << "}";
            return o.str();
        };

        std::cout << std::fixed << std::setprecision(4)
                  << "{\"type\":\"keys\",\"mode\":" << jsonEscape(engineMode ? "engine" : "inproc")
                  << ",\"clients\":" << activeCount
                  << ",\"standbys\":" << standbys
                  << ",\"wall_ms\":" << wallMs
                  << ",\"served\":" << totalServed
                  << ",\"fallbacks\":" << totalFallbacks
                  << ",\"memory_behind\":" << totalBehind
                  << ",\"thin_verified\":" << (allThin ? "true" : "false")
                  << ",\"overall\":" << statsJson(computeStats(overall))
                  << ",\"select_ms\":" << statsJson(computeStats(selects))
                  << ",\"classes\":" << mapJson(byClass)
                  << ",\"positions\":" << mapJson(byPos)
                  << ",\"memory\":{\"ready\":" << memReady << ",\"held\":" << memHeld << "}}\n";
    };

    for (int n : counts) {
        const int sb = (n == counts.back()) ? standbyCount : 0;
        runGroup(false, n, sb);
        runGroup(true, n, sb);
    }
}

// Section 3: deep paging & expansion.
void runDeepPageSection(Env &e) {
    if (e.residentMode) {
        auto ensured = rs::ensure(e.snapshot(), e.ensureOptions(e.personal, e.memoryFile));
        require(ensured.attach.outcome.result == hf::Result::Admitted, "deep resident ensure");
        e.tracked.push_back(ensured.attach.outcome.session->engine().ownerPid);
        e.tracked.push_back(ensured.attach.outcome.session->engine().hostPid);
        ensured.attach.outcome.session.reset();
    } else {
        e.startDirectHost(e.personal, e.memoryFile);
    }

    auto runOne = [&](bool engineMode) {
        const auto outFile = e.root / (engineMode ? L"deep-engine.json" : L"deep-inproc.json");
        std::vector<std::wstring> args{
            selfExe().wstring(), L"--worker-deep", engineMode ? L"engine" : L"inproc",
            e.residentMode ? L"resident" : L"direct",
            e.base.wstring(), pathOrDash(e.themes), pathOrDash(e.personal), pathOrDash(e.memoryFile),
            L"-", L"-", e.residentMode ? e.testKey : L"-", outFile.wstring()
        };
        std::vector<HANDLE> inherit;
        std::vector<size_t> idx;
        std::optional<engine::EngineRegistration::Endpoint> ep;
        if (engineMode) {
            if (e.residentMode) {
                args[8] = e.scope;
                args[9] = e.releaseKey;
            } else {
                ep = e.allocateDirect();
                args[8] = ep->name;
                inherit.push_back(ep->process.get());
                idx.push_back(9);
            }
        }
        auto child = ChildProcess::spawn(args, inherit, idx);
        child.waitSuccess(180000);
        std::cout << "{\"type\":\"deep_page\",\"result\":" << readWholeFile(outFile) << "}\n";
    };
    runOne(false);
    runOne(true);
}

// Section 4: large personal table + selection memory.
void runLargePersonalSection(Env &e, const std::filesystem::path &largeDir) {
    const auto largePersonal = largeDir / L"japanese-custom.tsv";
    const auto largeMemoryInproc = e.root / L"large-inproc-memory.tsv";
    const auto largeMemoryEngine = e.root / L"large-engine-memory.tsv";
    auto copyMemorySet = [&](const std::filesystem::path &dst) {
        for (const auto &[srcName, dstPath] : std::vector<std::pair<std::wstring, std::filesystem::path>>{
                {L"selection-memory.tsv", dst},
                {L"selection-memory-words.tsv", dst.parent_path() / (dst.stem().wstring() + L"-words" + dst.extension().wstring())},
                {L"selection-memory-user.tsv", dst.parent_path() / (dst.stem().wstring() + L"-user" + dst.extension().wstring())}}) {
            std::filesystem::copy_file(largeDir / srcName, dstPath, std::filesystem::copy_options::overwrite_existing);
        }
    };
    copyMemorySet(largeMemoryInproc);
    copyMemorySet(largeMemoryEngine);

    // 1. In-process worker with large personal + memory.
    {
        const auto outFile = e.root / L"large-inproc.json";
        std::vector<std::wstring> args{
            selfExe().wstring(), L"--worker-large", L"inproc", e.residentMode ? L"resident" : L"direct",
            e.base.wstring(), pathOrDash(e.themes), largePersonal.wstring(), largeMemoryInproc.wstring(),
            L"-", L"-", L"-", outFile.wstring()
        };
        auto child = ChildProcess::spawn(args, {});
        child.waitSuccess(180000);
        std::cout << "{\"type\":\"large_personal\",\"result\":" << readWholeFile(outFile)
                  << ",\"owner\":" << memJson({}) << ",\"host\":" << memJson({}) << "}\n";
    }

    // 2. Restart engine with the large personal + memory files, then run thin-client worker.
    if (e.residentMode) {
        e.stopResidentOwner();
        auto ensured = rs::ensure(e.snapshot(), e.ensureOptions(largePersonal, largeMemoryEngine));
        require(ensured.attach.outcome.result == hf::Result::Admitted, "large resident ensure");
        e.tracked.push_back(ensured.attach.outcome.session->engine().ownerPid);
        e.tracked.push_back(ensured.attach.outcome.session->engine().hostPid);
        ensured.attach.outcome.session.reset();
    } else {
        e.startDirectHost(largePersonal, largeMemoryEngine);
    }

    {
        const auto outFile = e.root / L"large-engine.json";
        std::vector<std::wstring> args{
            selfExe().wstring(), L"--worker-large", L"engine", e.residentMode ? L"resident" : L"direct",
            e.base.wstring(), pathOrDash(e.themes), largePersonal.wstring(), largeMemoryEngine.wstring(),
            L"-", L"-", e.residentMode ? e.testKey : L"-", outFile.wstring()
        };
        std::vector<HANDLE> inherit;
        std::vector<size_t> idx;
        std::optional<engine::EngineRegistration::Endpoint> ep;
        if (e.residentMode) {
            args[8] = e.scope;
            args[9] = e.releaseKey;
        } else {
            ep = e.allocateDirect();
            args[8] = ep->name;
            inherit.push_back(ep->process.get());
            idx.push_back(9);
        }
        auto child = ChildProcess::spawn(args, inherit, idx);
        child.waitSuccess(180000);
        std::cout << "{\"type\":\"large_personal\",\"result\":" << readWholeFile(outFile)
                  << ",\"owner\":" << memJson(engineOwnerMemory(e))
                  << ",\"host\":" << memJson(engineHostMemory(e)) << "}\n";
    }
}

std::vector<int> parseInts(const std::wstring &csv) {
    std::vector<int> out;
    size_t pos = 0;
    while (pos < csv.size()) {
        const auto comma = csv.find(L',', pos);
        const auto part = csv.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        if (!part.empty()) out.push_back(std::stoi(part));
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return out;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    tsf_engine::standaloneEngineDefault = false; // Never reach the installed engine unless a test asks.
    std::cout << std::unitbuf;
    try {
        if (argc >= 2) {
            const std::wstring flag = argv[1];
            if (flag == L"--worker-startup") return EngineTsfTest::workerStartup(argc, argv);
            if (flag == L"--worker-keys") return EngineTsfTest::workerKeys(argc, argv);
            if (flag == L"--worker-standby") return EngineTsfTest::workerStandby(argc, argv);
            if (flag == L"--worker-deep") return EngineTsfTest::workerDeepPage(argc, argv);
            if (flag == L"--worker-large") return EngineTsfTest::workerLargePersonal(argc, argv);
        }
        // Master:
        // <mode:resident|direct> <owner_or_host.exe> <base> <themes|-> <personal|-> <memory|-> <corpus> <counts_csv> <standby> <cold_runs> <warm_runs> <large_dir|->
        require(argc == 13, "usage: <mode> <owner_or_host> <base> <themes|-> <personal|-> <memory|-> <corpus> <counts> <standby> <cold> <warm> <large_dir|->");
        Env e;
        const std::wstring mode = argv[1];
        require(mode == L"resident" || mode == L"direct", "invalid mode");
        e.residentMode = (mode == L"resident");
        const std::filesystem::path program = argv[2];
        e.base = argv[3];
        e.themes = std::wstring(argv[4]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[4]);
        e.personal = std::wstring(argv[5]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[5]);
        e.memoryFile = std::wstring(argv[6]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[6]);
        const std::filesystem::path corpusPath = argv[7];
        const auto counts = parseInts(argv[8]);
        const int standbyCount = std::stoi(argv[9]);
        const int coldRuns = std::stoi(argv[10]);
        const int warmRuns = std::stoi(argv[11]);
        const std::filesystem::path largeDir = std::wstring(argv[12]) == L"-" ? std::filesystem::path() : std::filesystem::path(argv[12]);
        require(!counts.empty(), "counts empty");

        const auto unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        e.root = std::filesystem::temp_directory_path() / (L"harukoto-cn02-" + unique);
        e.version = e.root / (L"cn02-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(e.version);

        gEnv = &e;
        std::signal(SIGABRT, [](int) { if (gEnv) gEnv->cleanup(); });
        struct Guard { Env &env; ~Guard() { env.cleanup(); gEnv = nullptr; } } guard{e};

        if (e.residentMode) {
            e.ownerExe = e.version / L"harukoto_engine_owner.exe";
            e.hostExe = e.version / L"harukoto_engine_host.exe";
            std::filesystem::copy_file(program, e.ownerExe, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(program.parent_path() / L"harukoto_engine_host.exe", e.hostExe, std::filesystem::copy_options::overwrite_existing);
            e.testKey = L"Software\\HarukotoTest-cn02-" + unique;
            e.releaseKey = e.testKey + L"\\EngineReleases";
            e.scope = L"cn02-" + unique;
            e.store = rs::Store::under(e.testKey);
            writeReleaseEntry(e);
        } else {
            e.hostExe = program;
        }

        const auto corpus = Corpus::load(corpusPath);
        size_t totalKeys = 0;
        for (const auto &it : corpus.items) totalKeys += it.text.size();
        std::cout << "{\"type\":\"setup\",\"mode\":" << jsonEscape(e.residentMode ? "resident" : "direct")
                  << ",\"protocol\":" << ProtocolVersion
                  << ",\"host_threads\":" << rs::HostThreads
                  << ",\"corpus_items\":" << corpus.items.size()
                  << ",\"corpus_keys\":" << totalKeys << "}\n";

        runStartupSection(e, coldRuns, warmRuns);
        runKeysSection(e, corpusPath, corpus, counts, standbyCount);
        runDeepPageSection(e);
        if (!largeDir.empty()) runLargePersonalSection(e, largeDir);

        std::cout << "{\"type\":\"done\"}\n";
        return 0;
    } catch (const ep::Error &err) {
        std::cerr << "cn02 pipe error reason=" << static_cast<int>(err.reason) << " win32=" << err.code << std::endl;
    } catch (const std::exception &ex) {
        std::cerr << "cn02 error: " << ex.what() << std::endl;
    }
    return 1;
}
