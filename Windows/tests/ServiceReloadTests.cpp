// Drive the production service's reload boundary without registering an IME.
#include "../TextService.cpp"
#include <cassert>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <psapi.h>

using ReloadClock = std::chrono::steady_clock;
static double elapsedMs(ReloadClock::time_point start) {
    return std::chrono::duration<double, std::milli>(ReloadClock::now() - start).count();
}
static size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX info{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&info), sizeof(info)))
        throw std::runtime_error("cannot sample process private bytes");
    return info.PrivateUsage;
}

struct ThemeReloadTests {
    static void wait(ThemeReload &reload) {
        const auto limit = GetTickCount64() + 30000;
        assert(reload.job_);
        while (!reload.job_->done.load(std::memory_order_acquire) && GetTickCount64() < limit) Sleep(1);
        assert(reload.job_->done.load(std::memory_order_acquire));
    }
    static bool started(const ThemeReload &reload) { return bool(reload.job_); }
    static void waitCleanup(ThemeReload &reload) {
        const auto limit = GetTickCount64() + 30000;
        while (reload.cleanup_ && !reload.cleanup_->done.load(std::memory_order_acquire) && GetTickCount64() < limit) Sleep(1);
        assert(!reload.cleanup_ || reload.cleanup_->done.load(std::memory_order_acquire));
    }
    static size_t sampleUntilReady(ThemeReload &reload) {
        const auto limit = GetTickCount64() + 30000;
        size_t peak = privateBytes();
        assert(reload.job_);
        while (!reload.job_->done.load(std::memory_order_acquire) && GetTickCount64() < limit) {
            peak = std::max(peak, privateBytes()); Sleep(1);
        }
        assert(reload.job_->done.load(std::memory_order_acquire));
        return std::max(peak, privateBytes());
    }
};

namespace {
// Keep COM identity and queue ownership realistic, but do not simulate host
// insertion. Actual cross-application TSF delivery remains a manual check.
class Context final : public ITfContext {
public:
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { assert(refs); return --refs; }
    HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId, ITfEditSession *, DWORD, HRESULT *out) override { *out = E_FAIL; return E_FAIL; }
    HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION *, ULONG *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie, ULONG, const TF_SELECTION *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie, ITfRange **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie, ITfRange **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumViews(IEnumTfContextViews **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, ITfProperty **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAppProperty(REFGUID, ITfReadOnlyProperty **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TrackProperties(const GUID **, ULONG, const GUID **, ULONG, ITfReadOnlyProperty **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumProperties(IEnumTfProperties **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRangeBackup(TfEditCookie, ITfRange *, ITfRangeBackup **) override { return E_NOTIMPL; }
};

struct ServiceReloadTest {
    static void run(const std::filesystem::path &root, const std::filesystem::path &initial) {
        Context context, savedContext;
        {
            Service service;
            service.simplified = true;
            std::ifstream input(initial / "native-pinyin.tsv", std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(input), {}};
            service.lexicon = mpn_lexicon_create(bytes.data(), bytes.size()); assert(service.lexicon);
            // The production Activate local buffer is gone before the first key.
            std::string().swap(bytes); input.close();
            service.session = mpn_session_create(service.lexicon); assert(service.session);
            mpn_set_simplified(service.session, true);
            service.evidence.open(initial / "language-evidence.sqlite");
            assert(service.conjugations.loadFile(initial / "japanese-conjugations.bin"));
            std::ifstream japanese(initial / "japanese.tsv");
            service.japaneseWords = readJapaneseWords(japanese);
            appendConjugationWords(service.conjugations, service.japaneseWords);
            service.themeReload = std::make_unique<ThemeReload>(root / "themes", service.japaneseWords, "",
                root / "personal.tsv", nullptr, root / "store");
            assert(service.runtimeStatus.open());
            service.reportRuntimeStatus();
            service.delivery = std::make_shared<DeliveryState>(&context, 1);
            service.delivery->enqueue(L"原字段待上屏");
            const auto *queued = service.delivery->queue.begin(); assert(queued && *queued == L"原字段待上屏");
            auto saved = Service::SavedContext{};
            saved.session = mpn_session_create(service.lexicon); assert(saved.session);
            saved.simplified = false;
            saved.delivery = std::make_shared<DeliveryState>(&savedContext, 1);
            saved.delivery->enqueue(L"后台字段待上屏");
            saved.delivery->queue.suspend();
            savedContext.AddRef();
            service.suspended.emplace(&savedContext, std::move(saved));
            const auto activeDelivery = service.delivery;
            const auto savedDelivery = service.suspended.at(&savedContext).delivery;
            ULONGLONG now = 0;
            std::cout << "ready" << std::endl;
            for (std::string command; std::getline(std::cin, command);) {
                auto &background = service.suspended.at(&savedContext);
                if (command == "exit") break;
                if (command == "measure") {
                    assert(service.dictionaryBoundary());
                    ThemeReloadTests::waitCleanup(*service.themeReload);
                    now += 1000;
                    const auto before = privateBytes();
                    auto start = ReloadClock::now();
                    service.pollDictionaries(now);
                    const auto dispatch = elapsedMs(start);
                    start = ReloadClock::now();
                    const auto peak = ThemeReloadTests::sampleUntilReady(*service.themeReload);
                    const auto wait = elapsedMs(start);
                    const auto ready = privateBytes();
                    start = ReloadClock::now();
                    const bool changed = service.pollDictionaries(now);
                    const auto apply = elapsedMs(start);
                    const auto after = privateBytes();
                    start = ReloadClock::now();
                    ThemeReloadTests::waitCleanup(*service.themeReload);
                    const auto cleanup = elapsedMs(start);
                    const auto settled = privateBytes();
                    assert(!service.themeReload->failed());
                    assert(service.delivery == activeDelivery && background.delivery == savedDelivery);
                    assert(service.delivery->queue.inFlight() && !background.delivery->queue.empty());
                    using base_dictionary::Metadata;
                    std::cout << std::fixed << std::setprecision(4)
                        << "{\"generation\":" << Metadata::quote(service.themeReload->activeBaseGeneration())
                        << ",\"changed\":" << (changed ? "true" : "false")
                        << ",\"dispatch_ms\":" << dispatch << ",\"background_wait_ms\":" << wait
                        << ",\"apply_ms\":" << apply << ",\"private_before\":" << before
                        << ",\"private_ready\":" << ready << ",\"private_after\":" << after
                        << ",\"cleanup_wait_ms\":" << cleanup << ",\"private_settled\":" << settled
                        << ",\"sampled_peak_private\":" << std::max({before, peak, ready, after})
                        << ",\"merged_rows\":" << service.japaneseWords.size() << "}" << std::endl;
                    continue;
                } else if (command == "poll") {
                    now += 1000;
                    service.pollDictionaries(now); ThemeReloadTests::wait(*service.themeReload);
                    service.pollDictionaries(now);
                } else if (command == "type") {
                    for (char c : std::string("nihao")) assert(mpn_key(service.session, c));
                    service.refreshCandidates();
                } else if (command == "choose") {
                    assert(mpn_choose(service.session, 0));
                } else if (command == "clear") {
                    mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
                } else if (command == "saved-type") {
                    for (char c : std::string("nihao")) assert(mpn_key(background.session, c));
                } else if (command == "saved-choose") {
                    assert(mpn_choose(background.session, 0));
                } else if (command == "saved-clear") {
                    mpn_clear(background.session); mpn_clear_commit(background.session);
                } else if (command == "focus") {
                    service.OnKillThreadFocus();
                } else if (command == "cleanup") {
                    ThemeReloadTests::waitCleanup(*service.themeReload);
                } else if (command == "query") {
                    const bool started = ThemeReloadTests::started(*service.themeReload);
                    BOOL eaten = FALSE;
                    service.OnTestKeyDown(&context, 'A', 0, &eaten);
                    assert(started == ThemeReloadTests::started(*service.themeReload));
                } else if (command == "start-exit") {
                    // Destroy the service with its worker still potentially running.
                    now += 1000; service.pollDictionaries(now);
                    break;
                } else assert(command == "status");
                assert(service.delivery == activeDelivery && background.delivery == savedDelivery);
                assert(!service.delivery->queue.empty() && !background.delivery->queue.empty());
                assert(service.delivery->queue.inFlight());
                assert(service.delivery->context == &context && background.delivery->context == &savedContext);
                auto probe = base_dictionary::Session(service.newSession(), &mpn_session_destroy);
                mpn_set_simplified(probe.get(), true);
                for (char c : std::string("nihao")) assert(mpn_key(probe.get(), c));
                const std::string first = mpn_candidate(probe.get(), 0);
                assert(service.currentEvidence().dictionary("nihao", true).count(first));
                using base_dictionary::Metadata;
                std::cout << "{\"generation\":" << Metadata::quote(service.themeReload->activeBaseGeneration())
                    << ",\"failed\":" << (service.themeReload->failed() ? "true" : "false")
                    << ",\"first\":" << Metadata::quote(first)
                    << ",\"input\":" << Metadata::quote(mpn_input(service.session))
                    << ",\"commit\":" << Metadata::quote(mpn_committed(service.session))
                    << ",\"saved_commit\":" << Metadata::quote(mpn_committed(background.session))
                    << ",\"conjugation\":" << Metadata::quote(service.currentConjugations().entries().front().text)
                    << ",\"words\":[";
                bool comma = false;
                for (const auto &word : service.japaneseWords) {
                    if (comma) std::cout << ',';
                    comma = true; std::cout << Metadata::quote(word.text);
                }
                std::cout << "],\"diagnostic\":{" << service.themeReload->diagnosticFields() << "}}" << std::endl;
            }
        }
        assert(context.refs == 1 && savedContext.refs == 1);
    }
};
}
int wmain(int argc, wchar_t **argv) {
    assert(argc == 3);
    ServiceReloadTest::run(argv[1], argv[2]);
}
