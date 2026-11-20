#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msctf.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <unordered_map>
#include "PendingDelivery.h"
#include "PinyinPunctuation.h"
#include "PunctuationMode.h"
#include "ScriptMode.h"
#include "ScriptPreference.h"
#include "FuzzyPinyin.h"
#include "TypoCorrection.h"
#include "SelectionMemory.h"
#include "JapaneseConjugation.h"
#include "ConjugationPanel.h"
#include "JapanesePackages.h"
#include "ThemeReload.h"
#include "RuntimeStatus.h"
#include <new>
#include <string>
#include "WordEvidence.h"

namespace {
const CLSID serviceId = {0x961c0ada,0x4dc6,0x488f,{0x95,0x63,0x4f,0xc1,0x44,0xef,0xae,0x27}};
const GUID profileId = {0x8fde097d,0x3513,0x486a,{0x9a,0xb5,0x2d,0x1b,0x2c,0x67,0xc3,0x59}};
constexpr size_t pageSize = 7;
constexpr LANGID language = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
constexpr LANGID legacyLanguage = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
constexpr wchar_t name[] = L"晴词 Harukoto";
constexpr wchar_t classKey[] = L"Software\\Classes\\CLSID\\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}";
HMODULE module;
LONG objects;

std::wstring modulePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length < path.size() ? length : 0);
    return path;
}

std::wstring wide(const char *text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), length);
    result.pop_back();
    return result;
}

class RefCount {
public:
    RefCount() { InterlockedIncrement(&objects); }
    virtual ~RefCount() { InterlockedDecrement(&objects); }
    ULONG retain() { return InterlockedIncrement(&refs); }
    ULONG release() {
        const LONG remaining = InterlockedDecrement(&refs);
        if (!remaining) delete this;
        return remaining;
    }
private:
    LONG refs = 1;
};

struct DeliveryState;

class EditSession final : public ITfEditSession, public RefCount {
public:
    EditSession(ITfContext *context, std::wstring text, std::shared_ptr<DeliveryState> delivery = {}) : context(context), text(std::move(text)), delivery(std::move(delivery)) { context->AddRef(); }
    ~EditSession() override { context->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_ITfEditSession) return E_NOINTERFACE;
        *out = static_cast<ITfEditSession *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return retain(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    RECT caret{};

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override;
private:
    HRESULT insertText(TfEditCookie cookie) {
        if (text.empty()) return readCaret(cookie);
        ITfInsertAtSelection *selection = nullptr;
        HRESULT result = context->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void **>(&selection));
        if (FAILED(result)) return result;
        ITfRange *inserted = nullptr;
        result = selection->InsertTextAtSelection(cookie, 0, text.c_str(),
                                                 static_cast<LONG>(text.size()), &inserted);
        selection->Release();
        if (SUCCEEDED(result) && inserted) {
            // Move the insertion point past the committed text in rich-text hosts.
            if (SUCCEEDED(inserted->Collapse(cookie, TF_ANCHOR_END))) {
                TF_SELECTION caret{inserted, {TF_AE_NONE, FALSE}};
                context->SetSelection(cookie, 1, &caret);
            }
        }
        if (inserted) inserted->Release();
        return result;
    }
private:
    HRESULT readCaret(TfEditCookie cookie) {
        ITfContextView *view = nullptr;
        HRESULT result = context->GetActiveView(&view);
        if (FAILED(result)) return result;
        TF_SELECTION selection{};
        ULONG fetched = 0;
        result = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (SUCCEEDED(result) && fetched) {
            BOOL clipped = FALSE;
            const TfAnchor edge = selection.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END;
            result = selection.range->Collapse(cookie, edge);
            if (SUCCEEDED(result)) result = view->GetTextExt(cookie, selection.range, &caret, &clipped);
            selection.range->Release();
            if (clipped || caret.bottom <= caret.top) result = S_FALSE;
        } else if (SUCCEEDED(result)) {
            result = S_FALSE;
        }
        view->Release();
        return result;
    }

    ITfContext *context;
    std::wstring text;
    std::shared_ptr<DeliveryState> delivery;
};

struct DeliveryState : std::enable_shared_from_this<DeliveryState> {
    DeliveryState(ITfContext *context, TfClientId client) : context(context), client(client) { context->AddRef(); }
    ~DeliveryState() { context->Release(); }
    void enqueue(std::wstring text) { queue.append(std::move(text)); }
    void pump() {
        const auto *next = queue.begin();
        if (!next) return;
        // RequestEditSession can reenter the service and retire this context.
        // Keep the state alive until the request/result handling has finished.
        auto lifetime = shared_from_this();
        auto *edit = new (std::nothrow) EditSession(context, *next, lifetime);
        if (!edit) { queue.finish(false); return; }
        HRESULT result = E_FAIL;
        HRESULT request = context->RequestEditSession(client, edit, TF_ES_SYNC | TF_ES_READWRITE, &result);
        if (request == TF_E_SYNCHRONOUS || result == TF_E_SYNCHRONOUS)
            request = context->RequestEditSession(client, edit, TF_ES_ASYNC | TF_ES_READWRITE, &result);
        edit->Release();
        if ((FAILED(request) || FAILED(result)) && queue.inFlight()) queue.finish(false);
    }
    void completed(HRESULT result) {
        queue.finish(result == S_OK);
        if (result == S_OK) pump();
    }
    ITfContext *context;
    TfClientId client;
    PendingDelivery queue;
};

HRESULT EditSession::DoEditSession(TfEditCookie cookie) {
    if (delivery && delivery->queue.closed()) return TF_E_DISCONNECTED;
    if (delivery && !delivery->queue.canDeliver()) {
        delivery->completed(S_FALSE);
        return S_FALSE;
    }
    const HRESULT result = insertText(cookie);
    if (delivery) delivery->completed(result);
    return result;
}

class Service final : public ITfTextInputProcessor, public ITfKeyEventSink, public ITfThreadMgrEventSink, public ITfThreadFocusSink, public RefCount {
    friend struct ServiceFocusTest;
    friend struct ServiceReloadTest;
public:
    ~Service() override { Deactivate(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_ITfTextInputProcessor)
            *out = static_cast<ITfTextInputProcessor *>(this);
        else if (id == IID_ITfKeyEventSink)
            *out = static_cast<ITfKeyEventSink *>(this);
        else if (id == IID_ITfThreadMgrEventSink)
            *out = static_cast<ITfThreadMgrEventSink *>(this);
        else if (id == IID_ITfThreadFocusSink)
            *out = static_cast<ITfThreadFocusSink *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return retain(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }

    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr *manager, TfClientId id) override {
        try {
            simplified = readSimplifiedPreference();
            const auto folder = std::filesystem::path(modulePath()).parent_path();
            // Optional auxiliary data: invalid/missing tables must not disable Chinese input.
            conjugations.loadFile(folder / L"japanese-conjugations.bin");
            std::ifstream file(folder / L"native-pinyin.tsv", std::ios::binary);
            std::ifstream japanese(folder / L"japanese.tsv", std::ios::binary);
            japaneseWords = readJapaneseWords(japanese);
            appendConjugationWords(conjugations, japaneseWords);
            wchar_t localAppData[32768];
            const DWORD localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
            if (localLength && localLength < ARRAYSIZE(localAppData)) {
                memory = std::make_unique<selection_memory::Store>(std::filesystem::path(localAppData) / L"Meowpuut/selection-memory.tsv");
                const auto customPath = std::filesystem::path(localAppData) / L"Meowpuut/japanese-custom.tsv";
                auto personal = readPersonalDictionary(customPath);
                const auto themeRoot = std::filesystem::path(localAppData) / L"Meowpuut/Dictionaries";
                auto snapshot = readJapanesePackageSnapshot(themeRoot);
                themeReload = std::make_unique<ThemeReload>(themeRoot, japaneseWords, snapshot ? snapshot->generation : std::string(),
                    customPath, personal, std::filesystem::path(localAppData) / L"Meowpuut/BaseDictionaries");
                if (personal) japaneseWords.insert(japaneseWords.end(), personal->words.begin(), personal->words.end());
                if (snapshot) japaneseWords.insert(japaneseWords.end(), std::make_move_iterator(snapshot->words.begin()), std::make_move_iterator(snapshot->words.end()));
            }
            std::stable_sort(japaneseWords.begin(), japaneseWords.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
            evidence.open(folder / L"language-evidence.sqlite");
            // Read by size: copying tens of megabytes one character at a time
            // costs more than parsing them.
            std::string bytes;
            file.seekg(0, std::ios::end);
            if (const auto size = file.tellg(); size > 0) {
                bytes.resize(static_cast<size_t>(size));
                file.seekg(0);
                if (!file.read(bytes.data(), size)) bytes.clear();
            }
            lexicon = mpn_lexicon_create(bytes.data(), bytes.size());
            session = mpn_session_create(lexicon);
            if (session) mpn_set_simplified(session, simplified);
            if (session) applyFuzzyPinyin(session, readFuzzyPinyin());
            if (session) applyTypoCorrection(session, readTypoCorrection());
        } catch (...) { Deactivate(); return E_FAIL; }
        if (!session) { Deactivate(); return E_FAIL; }
        HRESULT result = manager->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void **>(&keys));
        client = id;
        if (SUCCEEDED(result)) result = keys->AdviseKeyEventSink(client, this, TRUE);
        if (SUCCEEDED(result)) result = manager->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&events));
        if (SUCCEEDED(result)) result = events->AdviseSink(IID_ITfThreadMgrEventSink,
            static_cast<ITfThreadMgrEventSink *>(this), &eventCookie);
        if (SUCCEEDED(result)) result = events->AdviseSink(IID_ITfThreadFocusSink,
            static_cast<ITfThreadFocusSink *>(this), &focusCookie);
        if (FAILED(result)) Deactivate();
        else { runtimeStatus.open(); reportRuntimeStatus(); }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Deactivate() override {
        runtimeStatus.close();
        if (memory) { memory->flush(); memory.reset(); }
        wordBonuses.reset();
        themeReload.reset();
        conjugationPanel.clear();
        if (delivery) delivery->queue.close();
        for (auto &entry : suspended)
            if (entry.second.delivery) entry.second.delivery->queue.close();
        if (events) {
            auto *source = events;
            events = nullptr;
            const DWORD cookie = eventCookie;
            eventCookie = TF_INVALID_COOKIE;
            if (cookie != TF_INVALID_COOKIE) source->UnadviseSink(cookie);
            const DWORD focus = focusCookie;
            focusCookie = TF_INVALID_COOKIE;
            if (focus != TF_INVALID_COOKIE) source->UnadviseSink(focus);
            source->Release();
        }
        if (lastForeground) sendGlass({});
        if (keys) {
            auto *manager = keys;
            keys = nullptr;
            manager->UnadviseKeyEventSink(client);
            manager->Release();
        }
        if (renderer) {
            if (HWND target = rendererWindow()) PostMessageW(target, WM_CLOSE, 0, 0);
            CloseHandle(renderer);
            renderer = nullptr;
        }
        if (popup) {
            DestroyWindow(popup);
            popup = nullptr;
        }
        if (activeContext) {
            activeContext->Release();
            activeContext = nullptr;
        }
        mpn_session_destroy(session);
        session = nullptr;
        for (auto &entry : suspended) {
            mpn_session_destroy(entry.second.session);
            entry.first->Release();
        }
        suspended.clear();
        delivery.reset();
        mpn_lexicon_destroy(lexicon);
        lexicon = nullptr;
        baseSnapshot.reset();
        evidence.disconnect();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override {
        if (!foreground) cancelForFocusChange();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetThreadFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnKillThreadFocus() override {
        cancelForFocusChange();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr *) override {
        // Each removed context is disposed through OnPopContext.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr *next, ITfDocumentMgr *previous) override {
        if (next != previous) cancelForFocusChange();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext *) override {
        cancelForFocusChange();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext *context) override {
        if (context == activeContext) {
            if (delivery) delivery->queue.close();
            delivery.reset();
            mpn_clear(session);
            resetCandidates();
            activeContext = nullptr;
            context->Release();
            hideCandidates();
        }
        const auto saved = suspended.find(context);
        if (saved != suspended.end()) {
            auto state = std::move(saved->second);
            suspended.erase(saved);
            if (state.delivery) state.delivery->queue.close();
            mpn_session_destroy(state.session);
            context->Release();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext *context, WPARAM key, LPARAM, BOOL *eaten) override {
        *eaten = accepts(context, key);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext *, WPARAM, LPARAM, BOOL *eaten) override {
        *eaten = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext *, WPARAM, LPARAM, BOOL *eaten) override {
        *eaten = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) override {
        *eaten = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext *context, WPARAM key, LPARAM flags, BOOL *eaten) override {
        *eaten = FALSE;
        if (!accepts(context, key)) return S_OK;
        try {
            if (activeContext != context && !switchContext(context)) return S_OK;
            if (delivery) {
                delivery->queue.resume();
                delivery->pump();
            }
            if (activeContext != context || !session || !delivery || !delivery->queue.canDeliver()) return S_OK;
            pollDictionaries();
            // Selection memory is per user and can be switched off at any time.
            memoryEnabled = memory && selection_memory::enabled();
            if (memoryEnabled) memory->poll();
            // DICT-02b: the user's own words apply whether or not learning is on.
            if (memory) memory->pollUserWords();
            attachPersonal();
            // Fuzzy pinyin pairs are per user and apply from the next key.
            applyFuzzy(readFuzzyPinyin());
            // So is typo correction (CN-05).
            applyTypo(readTypoCorrection());
            const bool preference = readSimplifiedPreference();
            if (preference != simplified) conjugationPanel.close(choices, page, highlighted, expanded);
            if (applyPinyinScript(session, simplified, preference)) {
                simplified = preference;
                refreshCandidates();
            }
            // CN-07: punctuation mode, and what this key types in it.
            englishPunctuation = readEnglishPunctuation(preferenceRoot);
            const bool toggle = punctuationToggle(key);
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            PinyinMark mark = toggle ? PinyinMark::none
                : pinyinMarkKind(static_cast<unsigned int>(key), shift, englishPunctuation, *mpn_input(session) != '\0');
            // Punctuation otherwise left to the host reaches here only when
            // accepts() kept it behind a pending insertion (which pump() above
            // may have delivered since): type it literally, in order.
            if (mark == PinyinMark::none && !toggle && !shift && !*mpn_input(session)
                && pinyinAsciiPunctuation(static_cast<unsigned int>(key)))
                mark = PinyinMark::literal;
            // Editing returns to normal composition before handling the key.
            if (conjugationPanel.active() && (scriptShortcut(key) || key == VK_SPACE ||
                (key >= 'A' && key <= 'Z') || mark != PinyinMark::none || key == VK_OEM_7))
                conjugationPanel.close(choices, page, highlighted, expanded);
            const size_t start = page * pageSize;
            const size_t total = candidateCount();
            const size_t count = total > start ? (std::min)(pageSize, total - start) : 0;
            std::wstring text;
            if (key == VK_F6 || (conjugationPanel.active() && (key == VK_ESCAPE || key == VK_BACK))) {
                if (!(static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30))) {
                    if (conjugationPanel.active()) conjugationPanel.close(choices, page, highlighted, expanded);
                    else conjugationPanel.open(currentConjugations(), choices, mpn_input(session), page, highlighted, expanded);
                }
                *eaten = TRUE;
            } else if (conjugationPanel.active() && key == VK_F4) {
                *eaten = TRUE; // Keep all forms visible; F4 resumes its normal role after leaving.
            } else if (conjugationPanel.active() && key >= '0' && key <= '9' &&
                       !(key >= '1' && key <= '7' && start + key - '1' < total)) {
                *eaten = TRUE; // An unused number must not commit an unrelated form.
            } else if (scriptShortcut(key)) {
                // Consume repeats without repeatedly toggling while held.
                if (!(static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30))) {
                    const bool next = !simplified;
                    if (writeSimplifiedPreference(next)) {
                        applyPinyinScript(session, simplified, next);
                        simplified = next;
                        refreshCandidates();
                    }
                }
                *eaten = TRUE;
            } else if (toggle) {
                // Like the script shortcut: one toggle per press, persisted
                // for every context and the next session.
                if (!(static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30))
                    && writeEnglishPunctuation(!englishPunctuation, preferenceRoot))
                    englishPunctuation = !englishPunctuation;
                *eaten = TRUE;
            } else if (mark != PinyinMark::none) {
                finishCandidateText(text, start + highlighted);
                text += pinyinMarkText(static_cast<unsigned int>(key), shift, mark, quotes);
                highlighted = 0;
                *eaten = TRUE;
            } else if (key == VK_F4) {
                expanded = !expanded;
                *eaten = TRUE;
            } else if (key == VK_SPACE) {
                text = wide(mpn_input(session)) + L" ";
                mpn_clear(session);
                resetCandidates();
                *eaten = TRUE;
            } else if (key == VK_PRIOR || key == VK_NEXT) {
                if (key == VK_PRIOR && page) --page;
                if (key == VK_NEXT) {
                    if (!conjugationPanel.active() && (page + 1) * pageSize >= candidateCount()) expandCandidates();
                    if ((page + 1) * pageSize < candidateCount()) ++page;
                }
                highlighted = 0;
                *eaten = TRUE;
            } else if (key >= '0' && key <= '9' && !pinyinDigitSelects(static_cast<unsigned int>(key), start, total)) {
                finishCandidateText(text, start + highlighted);
                text += static_cast<wchar_t>(key);
                *eaten = TRUE;
            } else if (navigation(key) && count) {
                const bool backward = key == VK_LEFT || key == VK_UP || (key == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                const size_t offset = backward ? count - 1 : 1;
                highlighted = (highlighted + offset) % count;
                *eaten = TRUE;
            } else {
                if (key >= '1' && key <= '7')
                    *eaten = chooseCandidate(start + key - '1', text);
                else if (key == VK_RETURN && count)
                    *eaten = chooseCandidate(start + (std::min)(highlighted, count - 1), text);
                else
                    *eaten = mpn_key(session, translate(key));
                refreshCandidates();
            }

            text += wide(mpn_committed(session));
            if (!text.empty()) {
                // Ownership transfers to the same-context queue before core commit is cleared.
                delivery->enqueue(std::move(text));
                mpn_clear_commit(session);
                delivery->pump();
            }
            if (activeContext == context && session && delivery && delivery->queue.canDeliver()) updatePopup();
        } catch (...) {
            // Do not discard a composition or replay an already handled key.
            sendGlass({});
            if (popup) ShowWindow(popup, SW_HIDE);
        }
        return S_OK;
    }
private:
    void suspendDeliveries() {
        if (delivery) delivery->queue.suspend();
        for (auto &entry : suspended)
            if (entry.second.delivery) entry.second.delivery->queue.suspend();
    }

    void hideCandidates() {
        popupForeground = nullptr;
        sendGlass({});
        if (popup) ShowWindow(popup, SW_HIDE);
    }

    static bool scriptShortcut(WPARAM key) {
        return pinyinScriptShortcut(static_cast<unsigned int>(key), GetKeyState(VK_CONTROL) < 0,
            GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_LMENU) < 0, GetKeyState(VK_RMENU) < 0,
            GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0);
    }

    static bool navigation(WPARAM key) {
        return key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_TAB;
    }

    static int translate(WPARAM key) {
        switch (key) {
        case VK_BACK: return 0xff08;
        case VK_ESCAPE: return 0xff1b;
        case VK_RETURN: return 0xff0d;
        case VK_PRIOR: return 0xff55;
        case VK_NEXT: return 0xff56;
        case VK_SPACE: return ' ';
        case VK_OEM_7: return '\'';
        default: return key >= 'A' && key <= 'Z' ? static_cast<int>(key - 'A' + 'a') : 0;
        }
    }

    static bool disabled(ITfContext *context) {
        ITfCompartmentMgr *manager = nullptr;
        if (FAILED(context->QueryInterface(IID_ITfCompartmentMgr, reinterpret_cast<void **>(&manager)))) return false;
        ITfCompartment *compartment = nullptr;
        HRESULT result = manager->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED, &compartment);
        manager->Release();
        if (FAILED(result)) return false;
        VARIANT value{};
        result = compartment->GetValue(&value);
        compartment->Release();
        const bool disabled = SUCCEEDED(result) && value.vt == VT_I4 && value.lVal != 0;
        VariantClear(&value);
        return disabled;
    }

    // CN-07: Ctrl+. unless switched off in the settings.
    bool punctuationToggle(WPARAM key) const {
        return pinyinPunctuationShortcut(static_cast<unsigned int>(key), GetKeyState(VK_CONTROL) < 0,
            GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0,
            GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) && readPunctuationHotkey(preferenceRoot);
    }

    bool accepts(ITfContext *context, WPARAM key) const {
        const bool shortcut = scriptShortcut(key) || punctuationToggle(key);
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        // Of the Shift layer only Chinese punctuation is taken; Shift+letters,
        // unmapped Shift keys and English mode pass to the host as before.
        const bool shiftMark = shift && pinyinShiftMark(static_cast<unsigned int>(key))
            && !readEnglishPunctuation(preferenceRoot);
        if (!session || !context || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0
            || (!shortcut && (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0
            || (key != VK_TAB && shift && !shiftMark) || (GetKeyState(VK_CAPITAL) & 1)))) return false;
        TF_STATUS status{};
        if (FAILED(context->GetStatus(&status)) || (status.dwDynamicFlags & TF_SD_READONLY)) return false;
        if (disabled(context)) return false;
        HWND focus = GetFocus();
        wchar_t className[16]{};
        if (focus && GetClassNameW(focus, className, ARRAYSIZE(className))
            && lstrcmpiW(className, L"Edit") == 0 && (GetWindowLongPtrW(focus, GWL_STYLE) & ES_PASSWORD))
            return false;
        if (shortcut || shiftMark) return true;
        if (key >= 'A' && key <= 'Z') return true;
        auto saved = suspended.find(context);
        MPNSession *current = context == activeContext ? session : (saved == suspended.end() ? nullptr : saved->second.session);
        const bool composing = current && *mpn_input(current);
        if (!shift && (pinyinAsciiPunctuation(static_cast<unsigned int>(key)))
            && pinyinMarkKind(static_cast<unsigned int>(key), false, readEnglishPunctuation(preferenceRoot), composing) != PinyinMark::none)
            return true;
        if (!composing) {
            const auto queued = context == activeContext ? delivery
                : saved == suspended.end() ? nullptr : saved->second.delivery;
            // Selection may already have cleared the engine while TSF still
            // owes its text to the host. Keep literal digits/spaces and
            // punctuation behind it.
            return !shift && queued && !queued->queue.empty() && (pinyinQueuedLiteral(static_cast<unsigned int>(key))
                || pinyinAsciiPunctuation(static_cast<unsigned int>(key)));
        }
        const size_t currentPage = context == activeContext ? page : saved->second.page;
        const size_t currentCount = context == activeContext ? candidateCount() : saved->second.choices.size();
        if (key == VK_F6) {
            if (context != activeContext) return false;
            const size_t index = page * pageSize + highlighted;
            return conjugationPanel.active() || (index < choices.size() && conjugationFor(currentConjugations(), choices[index], mpn_input(current)));
        }
        if (key >= '0' && key <= '9') return true;
        if (navigation(key) || key == VK_F4)
            return currentPage * pageSize < currentCount;
        return translate(key) != 0;
    }

    std::unique_ptr<selection_memory::Store> memory;
    bool memoryEnabled = false;
    // CN-06c: word bonuses handed to the core, rebuilt from memory->words()
    // when another process changed them or the hour (decay) moved on.
    std::unique_ptr<MPNPersonal, decltype(&mpn_personal_destroy)> wordBonuses{nullptr, &mpn_personal_destroy};
    uint64_t personalRevision = 0;
    int64_t personalHour = 0;
    // DICT-02b: revision of the user words last handed to the core table.
    uint64_t userWordsRevision = 0;
    // Segments selected so far in the composition being completed.
    std::string composedReading, composedText;
    size_t composedSegments = 0;
    struct SavedContext {
        MPNSession *session = nullptr;
        std::vector<WindowsCandidate> choices;
        std::shared_ptr<DeliveryState> delivery;
        bool chineseLocked = false, expanded = false, simplified = true;
        size_t page = 0, highlighted = 0;
    };
    const JapaneseConjugations &currentConjugations() const {
        return baseSnapshot ? baseSnapshot->conjugations() : conjugations;
    }
    const WordEvidence &currentEvidence() const {
        return baseSnapshot ? baseSnapshot->evidence() : evidence;
    }
    MPNSession *newSession() const {
        return baseSnapshot ? baseSnapshot->session(simplified).release() : mpn_session_create(lexicon);
    }
    bool dictionaryBoundary() const {
        const auto idle = [](MPNSession *value) {
            return value && !*mpn_input(value) && !*mpn_committed(value);
        };
        if (!idle(session) || conjugationPanel.active()) return false;
        for (const auto &entry : suspended) if (!idle(entry.second.session)) return false;
        return true;
    }
    bool adoptBase(std::unique_ptr<base_dictionary::Snapshot> &next, base_dictionary::Lexicon &retiredInstallation) {
        if (!next || !dictionaryBoundary()) return false;
        // Allocate every replacement before changing anything. Pending TSF
        // delivery queues belong to contexts, not engine sessions, and remain
        // untouched. Uncollected engine commits above postpone the swap.
        auto active = next->session(simplified);
        mpn_set_segment_context(active.get(), mpn_segment_context(session));
        std::vector<base_dictionary::Session> replacements;
        replacements.reserve(suspended.size());
        for (const auto &entry : suspended) {
            auto replacement = next->session(entry.second.simplified);
            mpn_set_segment_context(replacement.get(), mpn_segment_context(entry.second.session));
            replacements.push_back(std::move(replacement));
        }
        // From here on only non-throwing ownership swaps and cleanup.
        mpn_session_destroy(session);
        session = active.release();
        size_t index = 0;
        for (auto &entry : suspended) {
            auto &saved = entry.second;
            mpn_session_destroy(saved.session);
            saved.session = replacements[index++].release();
            saved.choices.clear(); saved.page = saved.highlighted = 0;
            saved.chineseLocked = saved.expanded = false;
        }
        resetCandidates();
        baseSnapshot.swap(next);
        // All contexts now own sessions from the new base. Release the original
        // installation's large lexicon rather than retaining it indefinitely.
        retiredInstallation.reset(lexicon); lexicon = nullptr;
        evidence.disconnect();
        return true;
    }
    bool pollDictionaries(ULONGLONG now = GetTickCount64()) {
        const bool changed = themeReload && themeReload->poll(dictionaryBoundary(), japaneseWords,
            [this](auto &next, auto &retired) { return adoptBase(next, retired); }, now);
        reportRuntimeStatus();
        return changed;
    }
    void reportRuntimeStatus() noexcept {
        try {
            using base_dictionary::Metadata;
            runtimeStatus.publish("\"service_version\":\"0.4.4-alpha.26\",\"base_version\":" +
                Metadata::quote(baseSnapshot ? baseSnapshot->version() : std::string()) + "," +
                (themeReload ? themeReload->diagnosticFields() :
                    "\"base_generation\":\"\",\"theme_generation\":\"\",\"pending_base_generation\":\"\",\"phase\":\"unavailable\",\"last_apply_failed\":false"));
        } catch (...) {}
    }
    bool switchContext(ITfContext *context) {
        // Some hosts change the supplied context without a preceding document
        // focus event. Never save an unfinished composition in that case.
        cancelForFocusChange();
        auto found = suspended.find(context);
        MPNSession *next = found == suspended.end() ? (activeContext ? newSession() : session) : found->second.session;
        if (!next) return false;
        // A popped active context leaves a cleared reusable session. Restoring
        // another saved context replaces it, so release that idle session.
        if (!activeContext && found != suspended.end()) mpn_session_destroy(session);
        if (activeContext) {
            if (delivery) delivery->queue.suspend();
            if (!*mpn_input(session) && !*mpn_committed(session) && (!delivery || delivery->queue.empty())) {
                mpn_session_destroy(session);
                activeContext->Release();
            } else {
                suspended.emplace(activeContext, SavedContext{session, std::move(choices), delivery, chineseLocked, expanded, simplified, page, highlighted});
            }
        }
        activeContext = context;
        // emplace may rehash the map, so reacquire the iterator.
        found = suspended.find(context);
        if (found != suspended.end()) {
            auto old = std::move(found->second);
            suspended.erase(found);
            session = old.session;
            choices = std::move(old.choices);
            delivery = std::move(old.delivery);
            chineseLocked = old.chineseLocked; expanded = old.expanded;
            page = old.page; highlighted = old.highlighted;
            if (applyPinyinScript(session, old.simplified, simplified)) refreshCandidates();
        } else {
            context->AddRef();
            session = next;
            // A new core session starts in traditional mode. Reapplying the
            // current mode to the initial session is a harmless no-op.
            mpn_set_simplified(session, simplified);
            delivery = std::make_shared<DeliveryState>(context, client);
            resetCandidates();
        }
        return true;
    }

    void resetCandidates() {
        composedReading.clear(); composedText.clear(); composedSegments = 0;
        conjugationPanel.clear();
        choices.clear();
        page = highlighted = 0;
        chineseLocked = expanded = false;
    }

    void cancelForFocusChange() {
        // Already-selected text still belongs to its original TSF context.
        // Suspend its delivery queue; cancel only the unselected reading.
        suspendDeliveries();
        mpn_cancel_composition(session);
        resetCandidates();
        for (auto &entry : suspended) {
            auto &saved = entry.second;
            mpn_cancel_composition(saved.session);
            saved.choices.clear();
            saved.page = saved.highlighted = 0;
            saved.chineseLocked = saved.expanded = false;
        }
        hideCandidates();
    }

    void refreshCandidates() {
        conjugationPanel.clear();
        page = highlighted = 0;
        if (!*mpn_input(session)) {
            resetCandidates();
            return;
        }
        choices = windowsCandidates(session, japaneseWords, [&](const std::string &input) {
            return currentEvidence().dictionary(input, simplified);
        }, chineseLocked);
        if (memoryEnabled) {
            // What the user chose for this exact reading comes first.
            const std::string input = mpn_input(session);
            selection_memory::apply(choices, input, memory->recall(input, simplified), [&](const WindowsCandidate &c) {
                return c.coreIndex ? mpn_candidate_consumed(session, *c.coreIndex) : input.size();
            });
        }
    }

    size_t candidateCount() const { return choices.size(); }

    void expandCandidates() {
        const auto before = choices.size();
        while (choices.size() == before && mpn_candidates_can_expand(session)) {
            mpn_expand_candidates(session);
            const auto more = windowsCandidates(session, japaneseWords, [&](const std::string &input) {
                return currentEvidence().dictionary(input, simplified);
            }, chineseLocked);
            appendCandidateExpansion(session, choices, more);
        }
    }

    void finishCandidateText(std::wstring &text, size_t choice) {
        bool learn = true;
        while (*mpn_input(session)) {
            const std::string remaining = mpn_input(session);
            // Only the highlighted candidate was confirmed by the user; the
            // rest of the reading is completed automatically and not learned.
            if (!chooseCandidate(choice, text, learn)) mpn_commit(session);
            learn = false;
            text += wide(mpn_committed(session));
            mpn_clear_commit(session);
            if (remaining == mpn_input(session)) {
                text += wide(remaining.c_str());
                mpn_clear(session);
                break;
            }
            refreshCandidates();
            choice = 0;
        }
    }

    std::wstring candidateText(size_t index) const { return wide(choices[index].text.c_str()); }

    std::wstring candidateReading(size_t index) const {
        if (!choices[index].annotation.empty()) return wide(choices[index].annotation.c_str());
        if (!choices[index].coreIndex) return {};
        const auto consumed = mpn_candidate_consumed(session, *choices[index].coreIndex);
        const std::string input = mpn_input(session);
        std::wstring reading = consumed < input.size() ? wide(input.substr(0, consumed).c_str()) : std::wstring();
        // CN-05: a typo correction says so on its reading line.
        if (typoCandidate(session, choices[index]))
            reading += (reading.empty() ? L"" : L" · ") + wide(typoMarker());
        return reading;
    }

    bool chooseCandidate(size_t index, std::wstring &text, bool learn = true) {
        if (index >= candidateCount()) return false;
        const std::string input = mpn_input(session), chosen = choices[index].text;
        const size_t consumed = choices[index].coreIndex ? mpn_candidate_consumed(session, *choices[index].coreIndex) : input.size();
        const bool form = conjugationPanel.active(); // A conjugated form is not a reading of the input.
        // CN-06c: the dictionary words making up a core candidate, read
        // before mpn_choose replaces the candidate list. Japanese, English
        // and other non-core candidates are not learned word by word.
        const std::string words = memoryEnabled && learn && !form && choices[index].coreIndex
            ? std::string(mpn_candidate_words(session, *choices[index].coreIndex)) : std::string();
        if (choices[index].coreIndex) {
            if (!mpn_choose(session, *choices[index].coreIndex)) return false;
            chineseLocked = *mpn_input(session) != '\0';
        } else {
            text += candidateText(index);
            mpn_clear(session);
        }
        if (memoryEnabled && learn && !form && consumed && consumed <= input.size()) remember(input, consumed, chosen, words);
        else { composedReading.clear(); composedText.clear(); composedSegments = 0; }
        return true;
    }

    // CN-06: record the confirmed selection, and the whole phrase once a
    // reading has been completed by selecting several segments.
    void remember(const std::string &input, size_t consumed, const std::string &chosen, const std::string &words = {}) {
        const auto reading = input.substr(0, consumed);
        if (chosen != reading) memory->record(reading, chosen, simplified);
        learnWords(words);
        composedReading += reading; composedText += chosen; ++composedSegments;
        if (consumed != input.size()) return;
        if (composedSegments > 1) memory->record(composedReading, composedText, simplified);
        composedReading.clear(); composedText.clear(); composedSegments = 0;
        memory->flush();
    }

    // CN-06c: count each confirmed word, then update just its reading in the
    // core's table when nothing else changed since the last sync.
    void learnWords(const std::string &words) {
        if (words.empty()) return;
        const bool current = wordBonuses && memory->words().revision() == personalRevision;
        const auto hour = selection_memory::currentHour();
        std::vector<std::string> readings;
        for (size_t start = 0; start < words.size();) {
            const auto end = words.find('\n', start), tab = words.find('\t', start);
            if (end == words.npos || tab == words.npos || tab > end) break;
            readings.push_back(words.substr(start, tab - start));
            memory->recordWord(readings.back(), std::string_view(words).substr(tab + 1, end - tab - 1), simplified, hour);
            start = end + 1;
        }
        if (!current || hour != personalHour) return; // attachPersonal rebuilds everything.
        for (const auto &reading : readings) {
            mpn_personal_set(wordBonuses.get(), simplified, reading.c_str(), nullptr, 0);
            memory->words().each(reading, simplified, hour, [&](const std::string &text, double bonus) {
                mpn_personal_set(wordBonuses.get(), simplified, reading.c_str(), text.c_str(), bonus);
            });
        }
        personalRevision = memory->words().revision();
    }

    // CN-03: gives the active session the user's fuzzy pinyin pairs and
    // rebuilds an open candidate list when they changed. Sessions created
    // later (another context, a dictionary reload) receive them on their
    // first key, before it is handled.
    void applyFuzzy(uint32_t mask) {
        if (applyFuzzyPinyin(session, mask) && *mpn_input(session)) refreshCandidates();
    }

    // CN-05: gives the active session the user's typo correction switch,
    // like applyFuzzy.
    void applyTypo(bool enabled) {
        if (applyTypoCorrection(session, enabled) && *mpn_input(session)) refreshCandidates();
    }

    // Hands the learned word bonuses and the user's own words (DICT-02b) to
    // the active session. While selection memory is off the bonuses are
    // removed; without user words either, the table is detached so the core
    // behaves exactly as without.
    void attachPersonal() {
        const bool userWords = memory && !memory->userWords().empty();
        if ((!memoryEnabled || !memory) && !userWords) {
            if (session) mpn_set_personal(session, nullptr);
            if (wordBonuses && userWordsRevision) { mpn_personal_clear_words(wordBonuses.get()); userWordsRevision = 0; }
            return;
        }
        const auto hour = selection_memory::currentHour();
        if (!wordBonuses) {
            wordBonuses.reset(mpn_personal_create());
            if (!wordBonuses) { if (session) mpn_set_personal(session, nullptr); return; }
            personalRevision = 0; userWordsRevision = 0;
        }
        if (memory->userWordsRevision() != userWordsRevision) {
            mpn_personal_clear_words(wordBonuses.get());
            for (const auto &entry : memory->userWords().entries())
                mpn_personal_add_word(wordBonuses.get(), entry.reading.c_str(), entry.text.c_str(), entry.traditional.c_str());
            userWordsRevision = memory->userWordsRevision();
        }
        if (!memoryEnabled) {
            // Learning is off: no bonuses, and a rebuild when it comes back on.
            if (personalRevision) { mpn_personal_clear(wordBonuses.get()); personalRevision = 0; }
        } else if (memory->words().revision() != personalRevision || hour != personalHour) {
            mpn_personal_clear(wordBonuses.get());
            memory->words().each(hour, [&](bool script, std::string_view reading, const std::string &text, double bonus) {
                mpn_personal_set(wordBonuses.get(), script, std::string(reading).c_str(), text.c_str(), bonus);
            });
            personalRevision = memory->words().revision();
            personalHour = hour;
        }
        if (session) mpn_set_personal(session, wordBonuses.get());
    }

    HWND rendererWindow() const {
        if (!renderer) return nullptr;
        HWND window = FindWindowW(nullptr, rendererName.c_str());
        DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        return pid && pid == GetProcessId(renderer) ? window : nullptr;
    }

    HWND desktopRendererWindow() const {
        HWND window = FindWindowW(nullptr, L"MeowpuutCandidates-desktop");
        DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        if (!pid) return nullptr;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return nullptr;
        wchar_t image[32768];
        DWORD length = ARRAYSIZE(image);
        const BOOL found = QueryFullProcessImageNameW(process, 0, image, &length);
        CloseHandle(process);
        const auto expected = std::filesystem::path(modulePath()).parent_path() / L"MeowpuutGlassPreview.exe";
        return found && lstrcmpiW(image, expected.c_str()) == 0 ? window : nullptr;
    }

    bool sendGlass(const std::wstring &text) {
        if (text.size() >= 4096) return false;
        HWND target = desktopRendererWindow();
        if (!target && !text.empty() && (!renderer || WaitForSingleObject(renderer, 0) == WAIT_OBJECT_0)) {
            if (renderer) CloseHandle(renderer);
            renderer = nullptr;
            const auto id = std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(reinterpret_cast<uintptr_t>(this));
            rendererName = L"MeowpuutCandidates-" + id;
            auto path = std::filesystem::path(modulePath()).parent_path() / L"MeowpuutGlassPreview.exe";
            auto command = L"\"" + path.wstring() + L"\" --ime " + id;
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &startup, &process)) {
                renderer = process.hProcess;
                CloseHandle(process.hThread);
            }
        }

        if (!target) target = rendererWindow();
        if (!target) return false;
        if (!text.empty()) lastForeground = GetForegroundWindow();
        COPYDATASTRUCT packet{0x4D50, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)),
                              const_cast<wchar_t *>(text.c_str())};
        DWORD_PTR accepted = 0;
        return SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(lastForeground),
                                   reinterpret_cast<LPARAM>(&packet), SMTO_ABORTIFHUNG | SMTO_BLOCK, 35,
                                   &accepted) && accepted;
    }

    static LRESULT CALLBACK candidateWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto *service = reinterpret_cast<Service *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_LBUTTONUP && service && service->activeContext
            && GetForegroundWindow() == service->popupForeground) {
            const int y = static_cast<short>(HIWORD(lParam)) - 1;
            const int candidate = y / service->popupRowHeight - 1;
            if (y >= service->popupRowHeight && candidate >= 0 && candidate < static_cast<int>(pageSize)) {
                BOOL eaten = FALSE;
                // Shift+digit is punctuation (CN-07); a Shift-click selects nothing, as before.
                if (static_cast<size_t>(candidate) < service->popupIndices.size() && GetKeyState(VK_SHIFT) >= 0)
                    service->OnKeyDown(service->activeContext, '1' + service->popupIndices[candidate], 0, &eaten);
            }
            return 0;
        }
        return service && service->originalPopupProc
            ? CallWindowProcW(service->originalPopupProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    POINT candidateAnchor() const {
        if (activeContext) {
            auto *edit = new (std::nothrow) EditSession(activeContext, {});
            if (edit) {
                HRESULT result = E_FAIL;
                const HRESULT request = activeContext->RequestEditSession(client, edit, TF_ES_SYNC | TF_ES_READ, &result);
                const RECT caret = edit->caret;
                edit->Release();
                if (request == S_OK && result == S_OK) return {caret.left, caret.bottom};
            }
        }

        GUITHREADINFO gui{};
        gui.cbSize = sizeof(gui);
        POINT anchor{};
        if (GetGUIThreadInfo(0, &gui) && gui.hwndCaret) {
            anchor = {gui.rcCaret.left, gui.rcCaret.bottom};
            ClientToScreen(gui.hwndCaret, &anchor);
        } else {
            GetCursorPos(&anchor);
        }
        return anchor;
    }

    void updatePopup() {
        if (!*mpn_input(session)) {
            sendGlass({});
            if (popup) ShowWindow(popup, SW_HIDE);
            return;
        }
        auto display = wide(mpn_preedit(session)) + (simplified ? L"  简" : L"  繁") + (englishPunctuation ? L" · 英文标点" : L"");
        const bool canConjugate = !conjugationPanel.active() && page * pageSize + highlighted < choices.size() &&
            conjugationFor(currentConjugations(), choices[page * pageSize + highlighted], mpn_input(session));
        const std::wstring conjugationHint = conjugationPanel.active() ? L"  活用 · F6/Esc 返回" : canConjugate ? L"  F6 活用" : L"";
        display += conjugationHint;
        const auto start = page * pageSize;
        popupIndices.clear();
        std::optional<size_t> japanese;
        for (size_t i = 0; i < pageSize && start + i < candidateCount(); ++i) {
            if (!expanded && choices[start + i].japanese && !choices[start + i].directJapanese) {
                if (!japanese || i == highlighted) japanese = i;
            } else {
                popupIndices.push_back(i);
            }
        }
        if (japanese) {
            popupIndices.push_back(*japanese);
            std::sort(popupIndices.begin(), popupIndices.end());
        }
        for (const auto i : popupIndices) {
            display += std::wstring(i == highlighted ? L"\r\n> " : L"\r\n  ") + std::to_wstring(i + 1)
                + L"  " + candidateText(start + i);
            if (choices[start + i].japanese) display += L"  [日]";
            if (typoCandidate(session, choices[start + i])) display += L"  [" + wide(typoMarker()) + L"]";
            if (!choices[start + i].annotation.empty()) display += L"  " + wide(choices[start + i].annotation.c_str());
        }

        auto *context = activeContext;
        POINT anchor = candidateAnchor();
        if (!session || activeContext != context) return;
        HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            const auto work = monitorInfo.rcWork;
            anchor.x = (std::max)(work.left, (std::min)(anchor.x, work.right - 850));
            if (anchor.y + 155 > work.bottom)
                anchor.y = (std::max)(work.top, anchor.y - 155);
        }
        auto payload = std::to_wstring(anchor.x) + L"\n" + std::to_wstring(anchor.y + 7) + L"\n"
            + wide(mpn_preedit(session)) + L"\t" + std::to_wstring(highlighted);
        const size_t pageCount = (candidateCount() + pageSize - 1) / pageSize;
        payload += simplified ? L"\t简" : L"\t繁";
        if (englishPunctuation) payload += L" · 英文标点";
        if (themeReload && themeReload->failed()) payload += L" · 词库更新失败，沿用旧版";
        payload += conjugationHint;
        if (pageCount > 1 || (!conjugationPanel.active() && mpn_candidates_can_expand(session)))
            payload += L"  ·  " + std::to_wstring(page + 1) + L" / " + std::to_wstring(pageCount);
        if (!conjugationPanel.active() && mpn_candidates_can_expand(session)) payload += L"+";
        payload += conjugationPanel.active() ? L"\t2" : expanded ? L"\t1" : L"\t0";
        for (size_t i = 0; i < pageSize && start + i < candidateCount(); ++i)
            payload += L"\n" + candidateText(start + i) + L"\t" + candidateReading(start + i)
                + (choices[start + i].directJapanese ? L"\tja-direct" : choices[start + i].japanese ? L"\tja" : L"");
        if (sendGlass(payload)) {
            if (popup) ShowWindow(popup, SW_HIDE);
            return;
        }
        if (!popup) {
            popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                    L"STATIC", nullptr, WS_POPUP | WS_BORDER | SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_NOTIFY,
                                    0, 0, 0, 0, nullptr, nullptr, module, nullptr);
            if (!popup) return;
            SendMessageW(popup, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), 0);
            SetWindowLongPtrW(popup, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            originalPopupProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                popup, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&candidateWindowProc)));
        }

        SetWindowTextW(popup, display.c_str());
        RECT size{};
        HDC dc = GetDC(popup);
        HGDIOBJ previous = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, display.c_str(), -1, &size, DT_CALCRECT | DT_NOPREFIX);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0) popupRowHeight = metrics.tmHeight;
        popupForeground = GetForegroundWindow();
        SelectObject(dc, previous);
        ReleaseDC(popup, dc);
        SetWindowPos(popup, HWND_TOPMOST, anchor.x, anchor.y + 7, (std::max)(size.right + 16, 180L), size.bottom + 8,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    ITfKeystrokeMgr *keys = nullptr;
    ITfSource *events = nullptr;
    DWORD eventCookie = TF_INVALID_COOKIE;
    DWORD focusCookie = TF_INVALID_COOKIE;
    ITfContext *activeContext = nullptr;
    TfClientId client = 0;
    MPNSession *session = nullptr;
    MPNLexicon *lexicon = nullptr;
    std::shared_ptr<DeliveryState> delivery;
    std::unordered_map<ITfContext *, SavedContext> suspended;
    std::vector<JapaneseWord> japaneseWords;
    std::unique_ptr<ThemeReload> themeReload;
    RuntimeStatus runtimeStatus;
    std::unique_ptr<base_dictionary::Snapshot> baseSnapshot;
    JapaneseConjugations conjugations;
    ConjugationPanel conjugationPanel;
    WordEvidence evidence;
    std::vector<WindowsCandidate> choices;
    bool chineseLocked = false, expanded = false, simplified = true;
    size_t page = 0, highlighted = 0;
    // CN-07: punctuation mode as read at the last key, the quote pairing,
    // and where the per-user preferences live (tests use a scratch key).
    bool englishPunctuation = false;
    PinyinQuotes quotes;
    HKEY preferenceRoot = HKEY_CURRENT_USER;
    HWND popup = nullptr;
    HWND popupForeground = nullptr;
    WNDPROC originalPopupProc = nullptr;
    std::vector<size_t> popupIndices;
    int popupRowHeight = 16;
    HWND lastForeground = nullptr;
    HANDLE renderer = nullptr;
    std::wstring rendererName;
};

class Factory final : public IClassFactory, public RefCount {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IClassFactory) return E_NOINTERFACE;
        *out = static_cast<IClassFactory *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return retain(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID id, void **out) override {
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto *service = new (std::nothrow) Service;
        if (!service) return E_OUTOFMEMORY;
        const HRESULT result = service->QueryInterface(id, out);
        service->Release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&objects);
        else InterlockedDecrement(&objects);
        return S_OK;
    }
};

HRESULT registerProfile(bool install) {
    ITfInputProcessorProfiles *profiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&profiles));
    if (FAILED(hr)) return hr;
    if (install) {
        hr = profiles->Register(serviceId);
        if (SUCCEEDED(hr)) hr = profiles->AddLanguageProfile(serviceId, language, profileId, name,
                                                            ARRAYSIZE(name) - 1, nullptr, 0, 0);
        if (SUCCEEDED(hr)) hr = profiles->EnableLanguageProfileByDefault(serviceId, language, profileId, TRUE);
        if (SUCCEEDED(hr)) hr = profiles->EnableLanguageProfile(serviceId, language, profileId, TRUE);
    } else {
        profiles->RemoveLanguageProfile(serviceId, language, profileId);
        profiles->RemoveLanguageProfile(serviceId, legacyLanguage, profileId);
        hr = profiles->Unregister(serviceId);
    }
    profiles->Release();
    if (install && FAILED(hr)) return hr;
    ITfCategoryMgr *categories = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr, reinterpret_cast<void **>(&categories));
    if (FAILED(hr)) return hr;
    hr = install ? categories->RegisterCategory(serviceId, GUID_TFCAT_TIP_KEYBOARD, serviceId)
                 : categories->UnregisterCategory(serviceId, GUID_TFCAT_TIP_KEYBOARD, serviceId);
    categories->Release();
    // Remove only our old language profile, after the new profile and category
    // are registered. Re-registering the old DLL can still restore it on failure.
    if (install && SUCCEEDED(hr)) {
        ITfInputProcessorProfiles *cleanup = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&cleanup)))) {
            cleanup->RemoveLanguageProfile(serviceId, legacyLanguage, profileId);
            cleanup->Release();
        }
    }
    return hr;
}
}

BOOL WINAPI DllMain(HMODULE handle, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) module = handle;
    return TRUE;
}

#ifdef _MSC_VER
#define MEOWPUUT_EXPORT STDAPI
#else
#define MEOWPUUT_EXPORT extern "C" __declspec(dllexport) HRESULT WINAPI
#endif

MEOWPUUT_EXPORT DllGetClassObject(REFCLSID id, REFIID iid, void **out) {
    *out = nullptr;
    if (id != serviceId) return CLASS_E_CLASSNOTAVAILABLE;
    auto *factory = new (std::nothrow) Factory;
    if (!factory) return E_OUTOFMEMORY;
    HRESULT result = factory->QueryInterface(iid, out);
    factory->Release();
    return result;
}

MEOWPUUT_EXPORT DllCanUnloadNow() {
    return InterlockedCompareExchange(&objects, 0, 0) == 0 ? S_OK : S_FALSE;
}

MEOWPUUT_EXPORT DllRegisterServer() {
    const auto path = modulePath();
    if (path.empty()) return E_FAIL;
    HKEY server = nullptr;
    const std::wstring key = std::wstring(classKey) + L"\\InprocServer32";
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                   nullptr, &server, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(server, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(path.c_str()),
                            static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS) {
        constexpr wchar_t model[] = L"Apartment";
        status = RegSetValueExW(server, L"ThreadingModel", 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(model), sizeof(model));
    }
    RegCloseKey(server);
    HRESULT result = status == ERROR_SUCCESS ? registerProfile(true) : HRESULT_FROM_WIN32(status);
    if (FAILED(result)) {
        registerProfile(false);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, classKey);
    }
    return result;
}

MEOWPUUT_EXPORT DllUnregisterServer() {
    HRESULT result = registerProfile(false);
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, classKey);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ? result : HRESULT_FROM_WIN32(status);
}
