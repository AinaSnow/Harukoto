#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msctf.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <unordered_map>
#include "PendingDelivery.h"
#include "PinyinPunctuation.h"
#include "PunctuationMode.h"
#include "ShiftToggle.h"
#include <olectl.h>
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
#include "GameInput.h"
#include <new>
#include <string>
#include "WordEvidence.h"
#include "EngineTsfResident.h"
#include "RendererHandoff.h"
namespace tsf_engine = harukoto::tsf_engine;
namespace renderer_handoff = harukoto::renderer::handoff;

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

// CN-07 batch 2: the input mode item Windows shows beside the language
// indicator in the taskbar (GUID_LBI_INPUTMODE; the TIP is registered with
// the system tray capability). Values from ctffunc.h / msctf.h, defined
// here so builds without them in uuid.lib still link; the focus test checks
// them against the SDK.
const GUID inputModeItem = {0x2c77a81e,0x41cc,0x4178,{0xa3,0xa7,0x5f,0x8a,0x98,0x75,0x68,0xe6}};
const GUID systrayCapability = {0x25504fb4,0x7bab,0x4bc1,{0x9c,0x69,0xcf,0x81,0x89,0x0f,0x0e,0xf5}};
// WIN-08: GUID_TFCAT_TIPCAP_UIELEMENTENABLED (the TIP offers its UI as UI
// elements, so hosts may draw it themselves), and our candidate list's own id.
const GUID uiElementCapability = {0x49d2f9cf,0x1f5e,0x11d7,{0xa6,0xd3,0x00,0x06,0x5b,0x84,0x43,0x5c}};
const GUID candidateElementGuid = {0x6c1a3f2e,0x8b0d,0x4e57,{0x9a,0x41,0x2d,0x7f,0x3b,0x9c,0x5e,0x18}};

// A taskbar icon showing one or two characters: white on the dark taskbar,
// black on a light one. The caller owns the icon.
HICON modeIcon(const wchar_t *text) {
    DWORD light = 0, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    constexpr int size = 32;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    const BYTE zeros[size * size / 8]{};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros);
    HICON icon = nullptr;
    if (color && mask && bits) {
        const HGDIOBJ previous = SelectObject(dc, color);
        HFONT font = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        const HGDIOBJ previousFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT area{0, 0, size, size};
        DrawTextW(dc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        // Grey-scale coverage becomes alpha over a single colour.
        const DWORD ink = light == 1 ? 0 : 0xffffff;
        auto *pixel = static_cast<DWORD *>(bits);
        for (int i = 0; i < size * size; ++i) {
            const DWORD value = pixel[i];
            const DWORD alpha = (std::max)({value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff});
            pixel[i] = alpha ? (alpha << 24) | ink : 0;
        }
        if (previousFont) SelectObject(dc, previousFont);
        if (font) DeleteObject(font);
        SelectObject(dc, previous);
        ICONINFO parts{TRUE, 0, 0, mask, color};
        icon = CreateIconIndirect(&parts);
    }
    if (mask) DeleteObject(mask);
    if (color) DeleteObject(color);
    if (dc) DeleteDC(dc);
    return icon;
}

class Service;
// The service whose mode hint the thread's timer hides.
thread_local Service *hintOwner = nullptr;
// Calls back into the service on the input thread; the service clears the
// owner before releasing the item.
class ModeButton final : public ITfLangBarItemButton, public ITfSource, public RefCount {
public:
    explicit ModeButton(Service *owner) : owner(owner) {}
    ~ModeButton() override { if (sink) sink->Release(); }
    Service *owner;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_ITfLangBarItem || id == IID_ITfLangBarItemButton)
            *out = static_cast<ITfLangBarItemButton *>(this);
        else if (id == IID_ITfSource) *out = static_cast<ITfSource *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return retain(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO *info) override {
        *info = {};
        info->clsidService = serviceId;
        info->guidItem = inputModeItem;
        info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
        lstrcpynW(info->szDescription, L"晴词 中／英", ARRAYSIZE(info->szDescription));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD *status) override { *status = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Show(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR *tip) override;
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT, const RECT *) override;
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIcon(HICON *icon) override;
    HRESULT STDMETHODCALLTYPE GetText(BSTR *text) override;
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID id, IUnknown *unknown, DWORD *cookie) override {
        if (id != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
        if (sink) return CONNECT_E_ADVISELIMIT;
        if (FAILED(unknown->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void **>(&sink)))) {
            sink = nullptr;
            return E_NOINTERFACE;
        }
        *cookie = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
        if (cookie != 1 || !sink) return CONNECT_E_NOCONNECTION;
        sink->Release();
        sink = nullptr;
        return S_OK;
    }
    void changed() { if (sink) sink->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP); }
private:
    ITfLangBarItemSink *sink = nullptr;
};

// WIN-08: the candidate list as a TSF UI element. Hosts that draw their own
// IME UI (games) read the candidates through it; TSF's IMM32 layer also
// serves ImmGetCandidateList from it. Strings are the candidate texts as
// shown, without readings or markers; indices are those of the full list,
// seven to a page, so digit keys select the same entries. Calls back into
// the service on the input thread; the service clears the owner before
// releasing the element.
class CandidateElement final : public ITfCandidateListUIElementBehavior, public RefCount {
public:
    explicit CandidateElement(Service *owner) : owner(owner) {}
    Service *owner;
    DWORD updated = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING
        | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_ITfUIElement || id == IID_ITfCandidateListUIElement
            || id == IID_ITfCandidateListUIElementBehavior)
            *out = static_cast<ITfCandidateListUIElementBehavior *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return retain(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR *text) override {
        if (!text) return E_INVALIDARG;
        *text = SysAllocString(L"晴词候选");
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID *id) override {
        if (!id) return E_INVALIDARG;
        *id = candidateElementGuid;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL *shown) override;
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD *flags) override {
        if (!flags) return E_INVALIDARG;
        *flags = updated;
        updated = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr **manager) override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT *count) override;
    HRESULT STDMETHODCALLTYPE GetSelection(UINT *index) override;
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR *text) override;
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT *index, UINT size, UINT *pages) override;
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT *, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT *page) override;
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override;
    HRESULT STDMETHODCALLTYPE Finalize() override;
    HRESULT STDMETHODCALLTYPE Abort() override;
};

// WIN-08: writes the reading into the document as a TSF composition, or ends
// one (ending: the composition to end, already detached from the service).
class CompositionEdit final : public ITfEditSession, public RefCount {
public:
    CompositionEdit(Service *service, ITfContext *context, ITfComposition *ending);
    ~CompositionEdit() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_ITfEditSession) return E_NOINTERFACE;
        *out = static_cast<ITfEditSession *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return retain(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override;
private:
    Service *service;
    ITfContext *context;
    ITfComposition *ending;
};

class Service final : public ITfTextInputProcessor, public ITfKeyEventSink, public ITfThreadMgrEventSink, public ITfThreadFocusSink,
    public ITfCompartmentEventSink, public ITfCompositionSink, public RefCount {
    friend struct ServiceFocusTest;
    friend struct ServiceReloadTest;
    friend struct EngineTsfTest;
    friend class ModeButton;
    friend class CandidateElement;
    friend class CompositionEdit;
public:
    // Tests only (WIN-03 step 6d): run a feature suite with the standalone
    // engine switch forced on and an injected link. Unset in production.
    static inline std::function<void(Service &)> testHook;
    Service() { if (testHook) testHook(*this); }
    ~Service() override { destroying = true; Deactivate(); }
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
        else if (id == IID_ITfCompartmentEventSink)
            *out = static_cast<ITfCompartmentEventSink *>(this);
        else if (id == IID_ITfCompositionSink)
            *out = static_cast<ITfCompositionSink *>(this);
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
            installFolder = folder;
            // Optional auxiliary data (~4 KiB): needed for F6 conjugation panel
            // even in thin-client mode (WIN-03 step 7d).
            conjugations.loadFile(folder / L"japanese-conjugations.bin");
            wchar_t localAppData[32768];
            const DWORD localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
            if (localLength && localLength < ARRAYSIZE(localAppData)) {
                dataRoot = std::filesystem::path(localAppData) / L"Meowpuut";
                memory = std::make_unique<selection_memory::Store>(dataRoot / L"selection-memory.tsv");
            }
            localLexiconLoaded = false;
            if (canStartThinClient()) {
                // WIN-03 step 7d: standalone engine is on with a compatible
                // approved release and supported token — start with a tiny
                // stub lexicon (< 2 KiB) and lazy-load local data only on fallback.
                lexicon = createStubLexicon();
                session = mpn_session_create(lexicon);
                if (session) {
                    mpn_set_simplified(session, simplified);
                    applyFuzzyPinyin(session, readFuzzyPinyin());
                    applyTypoCorrection(session, readTypoCorrection());
                }
                engineClient.enabled = true;
                engineClient.stats.localLexicon = false;
            } else {
                if (!ensureLocalLexicon(false)) { Deactivate(); return E_FAIL; }
                if (session) {
                    applyFuzzyPinyin(session, readFuzzyPinyin());
                    applyTypoCorrection(session, readTypoCorrection());
                }
            }
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
        else { runtimeStatus.open(); pollRendererHandoff(GetTickCount64()); reportRuntimeStatus(); attachMode(manager); }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Deactivate() override {
        // While the thread manager is still known: the host's UI element ends.
        endCandidateElement();
        endComposition();
        detachMode();
        runtimeStatus.close();
        if (memory) { memory->flush(); memory.reset(); }
        engineClient.cancel();
        engineClient.factory = nullptr;
        engineClient.sequenceFactory = nullptr;
        engineClient.enabled = false;
        engineClient.close(); // Its worker thread, if any, ends on its own.
        if (engineWindow) { KillTimer(engineWindow, EngineIdleTimerId); DestroyWindow(engineWindow); engineWindow = nullptr; }
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
        if (rendererReady && rendererHost.lastForeground()) sendGlass({});
        if (keys) {
            auto *manager = keys;
            keys = nullptr;
            manager->UnadviseKeyEventSink(client);
            manager->Release();
        }
        if (rendererReady) {
            rendererHandoff.shutdown();
            rendererHost.reset({}, {}, harukoto::renderer::release::PayloadVersion, false);
            rendererReady = false;
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
        engineFocused = foreground != FALSE;
        if (!foreground) cancelForFocusChange();
        armEngineIdleTimer();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetThreadFocus() override {
        engineFocused = true;
        armEngineIdleTimer();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKillThreadFocus() override {
        engineFocused = false;
        cancelForFocusChange();
        armEngineIdleTimer();
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
        // A composition in a removed context ends with it; no edit session.
        if (composition && compositionContext == context) releaseComposition();
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
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext *context, WPARAM key, LPARAM flags, BOOL *eaten) override {
        trackKeyDown(key, flags);
        // WIN-03 step 6a: these keys are taken only with candidates to act on.
        if (context == activeContext && (navigation(key) || key == VK_F4 || key == VK_F6)) {
            try { settleEngine(key); } catch (...) {}
        }
        *eaten = accepts(context, key);
        return S_OK;
    }
    // Shift is never eaten: hosts keep seeing it as a modifier. A tap is
    // acted on at its release, from whichever callback sees it first.
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext *, WPARAM key, LPARAM flags, BOOL *eaten) override {
        *eaten = FALSE;
        trackKeyUp(key, flags);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext *, WPARAM key, LPARAM flags, BOOL *eaten) override {
        *eaten = FALSE;
        trackKeyUp(key, flags);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnChange(REFGUID guid) override {
        // Windows, the host or another service changed the open state
        // (WIN-08: see OpenCloseRule). Our own writes come back as echoes.
        if (IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
            bool open = true;
            if (readOpenClose(open) && openRule.observed(open)) {
                try { setMode(openRule.chinese, false); } catch (...) {}
                reportRuntimeStatus();
            }
        }
        return S_OK;
    }
    // WIN-08: the host or TSF ended our composition (focus moved, a game
    // cancelled it). What it contains stays the host's; the reading is dropped.
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition *ended) override {
        if (!ended || ended != composition) return S_OK;
        releaseComposition();
        ++game.compositionsTerminated;
        try {
            if (session) mpn_cancel_composition(session);
            resetCandidates();
            endCandidateElement();
            hideOwnWindow();
        } catch (...) {}
        reportRuntimeStatus();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) override {
        *eaten = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext *context, WPARAM key, LPARAM flags, BOOL *eaten) override {
        *eaten = FALSE;
        trackKeyDown(key, flags);
        if (!accepts(context, key)) return S_OK;
        try {
            if (activeContext != context && !switchContext(context)) return S_OK;
            if (delivery) {
                delivery->queue.resume();
                delivery->pump();
            }
            if (activeContext != context || !session || !delivery || !delivery->queue.canDeliver()) return S_OK;
            engineChosenWords = false; // Any refresh from this key on uses the current bonuses.
            pollDictionaries();
            // Selection memory is per user and can be switched off at any time.
            memoryEnabled = memory && selection_memory::enabled(preferenceRoot);
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
                refreshCandidates(engineClient.enabled);
            }
            // WIN-08: when the reading is also written as a composition.
            inlineSetting = readInlineComposition(preferenceRoot);
            // CN-07: punctuation mode, and what this key types in it.
            if (readEnglishPunctuation(preferenceRoot) != englishPunctuation) {
                englishPunctuation = !englishPunctuation;
                if (button) button->changed();
            }
            // WIN-03 step 6a/7d: the standalone engine switch, and the candidates
            // it may still owe for the reading.
            engineClient.enabled = engineSwitch ? *engineSwitch : tsf_engine::readStandaloneEngine(preferenceRoot);
            engineFocused = true;
            settleEngine(key);
            if (!engineClient.enabled) {
                if (engineClient.link() || engineClient.standby() || engineDeferred) {
                    // Switched off: give the slot back; load local fallback if thin-client.
                    engineDeferred = false;
                    const bool wasPending = engineClient.pending();
                    engineClient.close();
                    if (!localLexiconLoaded) ensureLocalLexicon(true);
                    if (wasPending) refreshCandidates();
                } else if (!localLexiconLoaded) {
                    ensureLocalLexicon(true);
                }
            }
            pollEngineUpgrade(GetTickCount64(), engineClient.standby() != nullptr);
            std::wstring text;
            // CN-07 batch 6, number mode: digits (and one decimal point) typed
            // after a lone "v" are kept here beside the core's "v" and offered
            // as Chinese numerals, capitals, amounts. Space or Enter commits
            // the highlighted form, Backspace removes a digit, Esc cancels;
            // punctuation commits it and is typed after it as usual; a letter,
            // ' [ or ] commits it and then starts again as if idle.
            bool numberDone = false;
            if (chineseMode && GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_CONTROL) >= 0) {
                const char typed = key >= '0' && key <= '9' ? static_cast<char>(key) : key == VK_OEM_PERIOD ? '.' : '\0';
                const bool numbering = !numberInput.empty();
                if (typed && std::string(mpn_input(session)) == "v" && (numbering || typed != '.')) {
                    // Past the limit or a second point: taken and ignored.
                    if (quick_input::accepts(numberInput, typed)) { numberInput += typed; refreshCandidates(); }
                    numberDone = true;
                } else if (numbering && key == VK_BACK) {
                    numberInput.pop_back();
                    refreshCandidates(engineClient.enabled);
                    numberDone = true;
                } else if (numbering && key == VK_SPACE) {
                    chooseCandidate(page * pageSize + highlighted, text);
                    refreshCandidates();
                    numberDone = true;
                } else if (numbering && ((key >= 'A' && key <= 'Z') || key == VK_OEM_7 || key == VK_OEM_4 || key == VK_OEM_6)) {
                    chooseCandidate(page * pageSize + highlighted, text);
                    refreshCandidates();
                }
            }
            const bool toggle = chineseMode && punctuationToggle(key);
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            // CN-07 batch 7: full-width output. What the IME types literally
            // (raw letters, digits, spaces, ASCII punctuation) takes its
            // full-width form; candidates and Chinese punctuation do not change.
            const bool widthToggle = fullWidthToggle(key);
            const bool full = fullWidthOn();
            const auto literal = [&](std::wstring value) { return full ? pinyinFullWidth(std::move(value)) : value; };
            // CN-07 batch 4: [ ] pick a character (以词定字) while composing.
            const bool wordKey = chineseMode && !toggle
                && pinyinWordCharacterKey(static_cast<unsigned int>(key), shift, *mpn_input(session) != '\0');
            PinyinMark mark = toggle || !chineseMode || wordKey ? PinyinMark::none
                : pinyinMarkKind(static_cast<unsigned int>(key), shift, englishPunctuation, *mpn_input(session) != '\0');
            // Punctuation otherwise left to the host reaches here only when
            // accepts() kept it behind a pending insertion (which pump() above
            // may have delivered since): type it literally, in order.
            if (chineseMode && !wordKey && mark == PinyinMark::none && !toggle && !shift && !*mpn_input(session)
                && pinyinAsciiPunctuation(static_cast<unsigned int>(key)))
                mark = PinyinMark::literal;
            // Editing returns to normal composition before handling the key.
            if (conjugationPanel.active() && (scriptShortcut(key) || key == VK_SPACE ||
                (key >= 'A' && key <= 'Z') || mark != PinyinMark::none || key == VK_OEM_7))
                conjugationPanel.close(choices, page, highlighted, expanded);
            notice.clear(); // A notice lasts until the next key.
            const size_t start = page * pageSize;
            const size_t total = candidateCount();
            const size_t count = total > start ? (std::min)(pageSize, total - start) : 0;
            if (numberDone) {
                *eaten = TRUE;
            } else if (key == VK_F6 || (conjugationPanel.active() && (key == VK_ESCAPE || key == VK_BACK))) {
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
                        refreshCandidates(engineClient.enabled);
                    }
                }
                *eaten = TRUE;
            } else if (toggle) {
                // Like the script shortcut: one toggle per press, persisted
                // for every context and the next session.
                if (!(static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30))
                    && writeEnglishPunctuation(!englishPunctuation, preferenceRoot)) {
                    englishPunctuation = !englishPunctuation;
                    if (button) button->changed();
                    if (!*mpn_input(session)) showModeHint();
                }
                *eaten = TRUE;
            } else if (chineseMode && forgetShortcut(key) && *mpn_input(session)) {
                // CN-07 batch 5: forget what was learned for the highlighted
                // candidate; the composition stays open, rebuilt at once.
                if (!(static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30)))
                    notice = count ? forgetCandidate(start + (std::min)(highlighted, count - 1)) : L"没有学到的记录";
                *eaten = TRUE;
            } else if (widthToggle) {
                if (!(static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30))) {
                    fullWidth = !fullWidth;
                    if (button) button->changed();
                    if (!*mpn_input(session)) showModeHint();
                }
                *eaten = TRUE;
            } else if (!chineseMode) {
                // English mode takes a key only to keep it behind a pending
                // insertion, or to type it full-width (accepts()).
                if (const wchar_t typed = pinyinLiteral(static_cast<unsigned int>(key), shift)) text = literal(std::wstring(1, typed));
                *eaten = TRUE;
            } else if (wordKey) {
                // Only for a core (Chinese) candidate in the normal list; with
                // a Japanese, English or F6 form highlighted, or no candidate,
                // the key is taken and does nothing.
                if (!conjugationPanel.active() && count)
                    pickCharacter(start + (std::min)(highlighted, count - 1), key == VK_OEM_6, text);
                *eaten = TRUE;
            } else if (mark != PinyinMark::none) {
                finishCandidateText(text, start + highlighted);
                const std::wstring typed = pinyinMarkText(static_cast<unsigned int>(key), shift, mark, quotes);
                text += mark == PinyinMark::literal ? literal(typed) : typed;
                highlighted = 0;
                *eaten = TRUE;
            } else if (full && !*mpn_input(session) && (shift || key < 'A' || key > 'Z')
                       && pinyinLiteral(static_cast<unsigned int>(key), shift)) {
                // Idle in full-width output: digits, space, Shift+letters and
                // the other symbols the host would have typed.
                text = literal(std::wstring(1, pinyinLiteral(static_cast<unsigned int>(key), shift)));
                *eaten = TRUE;
            } else if (shift && key >= 'A' && key <= 'Z') {
                // A capital ends the reading as typed, like Space: the raw
                // letters and the capital, so "i" Shift+P "hone" Space gives
                // "iPhone ". Before batch 2 the capital reached the host
                // ahead of the composition.
                text = wide(mpn_committed(session)) + literal(wide(mpn_input(session)) + static_cast<wchar_t>(key));
                mpn_clear(session);
                mpn_clear_commit(session);
                resetCandidates();
                *eaten = TRUE;
            } else if (key == VK_F4) {
                expanded = !expanded;
                *eaten = TRUE;
            } else if (key == VK_SPACE) {
                text = literal(wide(mpn_input(session)) + L" ");
                mpn_clear(session);
                resetCandidates();
                *eaten = TRUE;
            } else if (key == VK_PRIOR || key == VK_NEXT || pinyinPagingKey(static_cast<unsigned int>(key), shift, *mpn_input(session) != '\0')) {
                // CN-07 batch 3: - and = page like Page Up / Page Down while composing.
                const bool next = key == VK_NEXT || key == VK_OEM_PLUS;
                if (!next && page) --page;
                if (next) {
                    if (!conjugationPanel.active() && (page + 1) * pageSize >= candidateCount()) expandCandidates();
                    if ((page + 1) * pageSize < candidateCount()) ++page;
                }
                highlighted = 0;
                *eaten = TRUE;
            } else if (key >= '0' && key <= '9' && !pinyinDigitSelects(static_cast<unsigned int>(key), start, total)) {
                finishCandidateText(text, start + highlighted);
                text += literal(std::wstring(1, static_cast<wchar_t>(key)));
                *eaten = TRUE;
            } else if (navigation(key) && count) {
                const bool backward = key == VK_LEFT || key == VK_UP || (key == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                // Moving past the last candidate of a page turns to the next
                // page (expanding the list when needed) and highlights its
                // first item; moving before the first turns back to the
                // previous page's last item. At either end of the whole list
                // the highlight stays where it is.
                if (backward) {
                    if (highlighted) --highlighted;
                    else if (page) { --page; highlighted = pageSize - 1; }
                } else if (highlighted + 1 < count) {
                    ++highlighted;
                } else {
                    if (!conjugationPanel.active() && (page + 1) * pageSize >= candidateCount()) expandCandidates();
                    if ((page + 1) * pageSize < candidateCount()) { ++page; highlighted = 0; }
                }
                *eaten = TRUE;
            } else {
                if (key >= '1' && key <= '7')
                    *eaten = chooseCandidate(start + key - '1', text);
                else if (key == VK_RETURN && count)
                    *eaten = chooseCandidate(start + (std::min)(highlighted, count - 1), text);
                else
                    *eaten = mpn_key(session, translate(key));
                // The edited reading, or what remains after a chosen
                // segment (step 6c), may be listed by the engine.
                refreshCandidates(true);
            }

            text += wide(mpn_committed(session));
            if (!text.empty()) {
                // WIN-08: an inline reading leaves the document before the
                // text replacing it arrives; what remains starts a new one.
                endComposition();
                // Ownership transfers to the same-context queue before core commit is cleared.
                delivery->enqueue(std::move(text));
                mpn_clear_commit(session);
                delivery->pump();
            }
            if (activeContext == context && session && delivery && delivery->queue.canDeliver()) {
                updatePopup();
                syncComposition();
            }
            const auto boundaryNow = GetTickCount64();
            pollEngineUpgrade(boundaryNow, engineClient.standby() != nullptr);
            pollRendererHandoff(boundaryNow);
            armEngineIdleTimer();
            reportRuntimeStatus();
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

    // The candidates are gone: our window, the host's UI element and an
    // inline reading.
    void hideCandidates() {
        endCandidateElement();
        endComposition();
        hideOwnWindow();
    }
    void hideOwnWindow() {
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

    static bool forgetShortcut(WPARAM key) {
        return pinyinForgetShortcut(static_cast<unsigned int>(key), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
            GetKeyState(VK_MENU) < 0, GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0);
    }

    // CN-07: Ctrl+. unless switched off in the settings.
    bool punctuationToggle(WPARAM key) const {
        return pinyinPunctuationShortcut(static_cast<unsigned int>(key), GetKeyState(VK_CONTROL) < 0,
            GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0,
            GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) && readPunctuationHotkey(preferenceRoot);
    }

    // ---- CN-07 batch 2: Chinese/English mode ----------------------------
    // The mode belongs to this service instance (one per input thread, so
    // per application window thread) and starts Chinese on activation; it is
    // not saved. It mirrors the thread's keyboard open/close compartment, so
    // Windows (Ctrl+Space), the host and the taskbar indicator agree with it.

    // Two bits per mouse button: pressed (odd bits) and the toggle bit that
    // flips with every press this thread processes.
    static constexpr unsigned int mousePressed = 0x2aa;
    static unsigned int mouseState() {
        unsigned int state = 0, offset = 0;
        for (int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
            const SHORT value = GetKeyState(button);
            state |= ((value < 0 ? 2u : 0u) | (value & 1 ? 1u : 0u)) << offset;
            offset += 2;
        }
        return state;
    }
    static bool otherModifiers() {
        return GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    }
    void trackKeyDown(WPARAM key, LPARAM flags) {
        if (key == VK_SHIFT)
            shiftTap.down(rightShiftKey(flags), (static_cast<ULONG_PTR>(flags) & (ULONG_PTR(1) << 30)) != 0,
                otherModifiers(), mouseState(), GetTickCount64());
        else shiftTap.other();
    }
    void trackKeyUp(WPARAM key, LPARAM flags) {
        if (key != VK_SHIFT) return;
        const unsigned int mouse = mouseState();
        if (shiftTap.up(rightShiftKey(flags), otherModifiers() || (mouse & mousePressed), mouse, GetTickCount64(),
                        readShiftToggle(preferenceRoot)) && session) {
            try { setMode(!chineseMode); } catch (...) {}
        }
    }

    ITfCompartment *openClose() const {
        if (!threadManager) return nullptr;
        ITfCompartmentMgr *manager = nullptr;
        if (FAILED(threadManager->QueryInterface(IID_ITfCompartmentMgr, reinterpret_cast<void **>(&manager)))) return nullptr;
        ITfCompartment *compartment = nullptr;
        if (FAILED(manager->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &compartment))) compartment = nullptr;
        manager->Release();
        return compartment;
    }
    bool readOpenClose(bool &open) const {
        ITfCompartment *compartment = openClose();
        if (!compartment) return false;
        VARIANT value{};
        const HRESULT result = compartment->GetValue(&value);
        compartment->Release();
        const bool known = result == S_OK && value.vt == VT_I4;
        if (known) open = value.lVal != 0;
        VariantClear(&value);
        return known;
    }
    void writeOpenClose(bool open) {
        ITfCompartment *compartment = openClose();
        if (!compartment) return;
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = open ? 1 : 0;
        compartment->SetValue(client, &value); // Reenters OnChange as an echo (OpenCloseRule).
        compartment->Release();
    }

    // Switching to English while composing commits the raw letters, as Space
    // does (mainstream behaviour): nothing typed is lost or converted.
    // user: the user's own switch (Shift, taskbar), which sets the preferred
    // mode, is written to the open/close compartment and shows the hint. A
    // change from outside (WIN-08) does none of these.
    void setMode(bool chinese, bool user = true) {
        if (user) {
            const bool wasOpen = openRule.open;
            openRule.user(chinese);
            if (chinese == chineseMode) {
                if (wasOpen != chinese) writeOpenClose(chinese);
                return;
            }
        }
        if (chinese == chineseMode) return;
        chineseMode = chinese;
        shiftTap.other();
        if (!chinese && session && *mpn_input(session) && delivery && activeContext) {
            std::wstring text = wide(mpn_committed(session)) + (fullWidthOn() ? pinyinFullWidth(wide(mpn_input(session))) : wide(mpn_input(session)));
            mpn_clear(session);
            mpn_clear_commit(session);
            resetCandidates();
            hideCandidates(); // ends an inline reading before its letters arrive
            delivery->enqueue(std::move(text));
            delivery->pump();
        }
        if (user) writeOpenClose(chinese);
        if (button) button->changed();
        if (user) showModeHint();
    }

    void attachMode(ITfThreadMgr *manager) {
        threadManager = manager;
        threadManager->AddRef();
        chineseMode = true;
        openRule.activate();
        writeOpenClose(true);
        if (ITfCompartment *compartment = openClose()) {
            ITfSource *source = nullptr;
            if (SUCCEEDED(compartment->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&source)))) {
                if (FAILED(source->AdviseSink(IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink *>(this), &openCookie)))
                    openCookie = TF_INVALID_COOKIE;
                source->Release();
            }
            compartment->Release();
        }
        ITfLangBarItemMgr *items = nullptr;
        if (SUCCEEDED(threadManager->QueryInterface(IID_ITfLangBarItemMgr, reinterpret_cast<void **>(&items)))) {
            button = new (std::nothrow) ModeButton(this);
            if (button && FAILED(items->AddItem(button))) { button->owner = nullptr; button->Release(); button = nullptr; }
            items->Release();
        }
    }
    void detachMode() {
        if (hintTimer) { KillTimer(nullptr, hintTimer); hintTimer = 0; }
        if (hintOwner == this) hintOwner = nullptr;
        if (!threadManager) return;
        if (openCookie != TF_INVALID_COOKIE) {
            if (ITfCompartment *compartment = openClose()) {
                ITfSource *source = nullptr;
                if (SUCCEEDED(compartment->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&source)))) {
                    source->UnadviseSink(openCookie);
                    source->Release();
                }
                compartment->Release();
            }
            openCookie = TF_INVALID_COOKIE;
        }
        if (button) {
            ITfLangBarItemMgr *items = nullptr;
            if (SUCCEEDED(threadManager->QueryInterface(IID_ITfLangBarItemMgr, reinterpret_cast<void **>(&items)))) {
                items->RemoveItem(button);
                items->Release();
            }
            button->owner = nullptr;
            button->Release();
            button = nullptr;
        }
        threadManager->Release();
        threadManager = nullptr;
    }

    std::wstring modeTooltip() const {
        return std::wstring(chineseMode ? (englishPunctuation ? L"晴词：中文 · 英文标点" : L"晴词：中文 · 中文标点") : L"晴词：英文")
            + (fullWidthOn() ? L" · 全角" : L" · 半角") + L"（Shift 切换中英）";
    }

    // A brief mode hint near the caret after a switch, also when idle, in the
    // candidate renderer (or the fallback popup); neither takes focus. Only
    // in a real TSF activation (tests run without a keystroke manager).
    static void CALLBACK hideHint(HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        Service *service = hintOwner;
        if (!service || service->hintTimer != id) return;
        service->hintTimer = 0;
        if (service->session && !*mpn_input(service->session)) {
            service->hideCandidates();
            service->pollRendererHandoff();
            service->reportRuntimeStatus();
        }
    }
    void showModeHint() {
        if (!keys || !session || *mpn_input(session)) return;
        POINT anchor = candidateAnchor();
        const std::wstring mode = chineseMode ? L"中" : L"英";
        const std::wstring detail = std::wstring(chineseMode ? (englishPunctuation ? L"中文 · 英文标点" : L"中文 · 中文标点") : L"英文")
            + (fullWidthOn() ? L" · 全角" : L"");
        const auto payload = std::to_wstring(anchor.x) + L"\n" + std::to_wstring(anchor.y + 7) + L"\n"
            + mode + L"\t0\t" + detail + L"\t0";
        if (sendGlass(payload)) {
            if (popup) ShowWindow(popup, SW_HIDE);
        } else {
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
            const std::wstring text = mode + L"  " + detail;
            SetWindowTextW(popup, text.c_str());
            popupIndices.clear();
            SetWindowPos(popup, HWND_TOPMOST, anchor.x, anchor.y + 7, 140, 24, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        hintOwner = this;
        if (hintTimer) KillTimer(nullptr, hintTimer);
        hintTimer = SetTimer(nullptr, 0, 1000, &Service::hideHint);
    }

    // CN-07 batch 7: Shift+Space, only when the option is on; full-width
    // output is on only while the option is.
    bool fullWidthToggle(WPARAM key) const {
        return pinyinFullWidthShortcut(static_cast<unsigned int>(key), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
            GetKeyState(VK_MENU) < 0, GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) && readFullWidthHotkey(preferenceRoot);
    }
    bool fullWidthOn() const { return fullWidth && readFullWidthHotkey(preferenceRoot); }

    bool accepts(ITfContext *context, WPARAM key) const {
        const auto code = static_cast<unsigned int>(key);
        // In English mode Ctrl+. is the host's (CN-07 batch 2).
        const bool shortcut = scriptShortcut(key) || (chineseMode && punctuationToggle(key)) || fullWidthToggle(key);
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        // Ctrl/Shift+Delete passes the modifier check here and is taken only
        // while composing (below).
        const bool forget = forgetShortcut(key);
        if (!session || !context || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0
            || (!shortcut && !forget && (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0
            || (GetKeyState(VK_CAPITAL) & 1)))) return false;
        TF_STATUS status{};
        if (FAILED(context->GetStatus(&status)) || (status.dwDynamicFlags & TF_SD_READONLY)) return false;
        if (disabled(context)) return false;
        HWND focus = GetFocus();
        wchar_t className[16]{};
        if (focus && GetClassNameW(focus, className, ARRAYSIZE(className))
            && lstrcmpiW(className, L"Edit") == 0 && (GetWindowLongPtrW(focus, GWL_STYLE) & ES_PASSWORD))
            return false;
        if (shortcut) return true;
        auto saved = suspended.find(context);
        MPNSession *current = context == activeContext ? session : (saved == suspended.end() ? nullptr : saved->second.session);
        const bool composing = current && *mpn_input(current);
        const auto queued = context == activeContext ? delivery
            : saved == suspended.end() ? nullptr : saved->second.delivery;
        const bool pending = queued && !queued->queue.empty();
        if (forget) return chineseMode && composing;
        // English mode: every key is the host's, except that typed text must
        // not overtake an insertion still owed to the host (such as the
        // letters committed by switching mid-composition).
        // CN-07 batch 7: with full-width output on, every printable key is
        // taken and typed in its full-width form.
        if (!chineseMode) return (pending || fullWidthOn()) && pinyinLiteral(code, shift);
        if (shift && key != VK_TAB) {
            // While composing no Shift key may reach the host ahead of the
            // composition (batch 2); idle, only Chinese punctuation is taken
            // and Shift+letters and other Shift keys pass as before.
            if (composing) return (code >= 'A' && code <= 'Z') || pinyinShiftedAscii(code);
            return (pinyinShiftMark(code) && !readEnglishPunctuation(preferenceRoot)) || (fullWidthOn() && pinyinLiteral(code, true));
        }
        if (key >= 'A' && key <= 'Z') return true;
        if (pinyinAsciiPunctuation(code)
            && pinyinMarkKind(code, false, readEnglishPunctuation(preferenceRoot), composing) != PinyinMark::none)
            return true;
        if (!composing) {
            // Selection may already have cleared the engine while TSF still
            // owes its text to the host. Keep literal digits/spaces and
            // punctuation behind it.
            return !shift && ((pending && (pinyinQueuedLiteral(code) || pinyinAsciiPunctuation(code)))
                || (fullWidthOn() && pinyinLiteral(code, false)));
        }
        const size_t currentPage = context == activeContext ? page : saved->second.page;
        const size_t currentCount = context == activeContext ? candidateCount() : saved->second.choices.size();
        if (key == VK_F6) {
            if (context != activeContext) return false;
            const size_t index = page * pageSize + highlighted;
            return conjugationPanel.active() || (index < choices.size() && conjugationFor(currentConjugations(), choices[index], mpn_input(current)));
        }
        if (key >= '0' && key <= '9') return true;
        // Like Page Up / Page Down: taken while composing, never reaching the host.
        if (pinyinPagingKey(code, shift, composing)) return true;
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
    static MPNLexicon *createStubLexicon() {
        static constexpr char Stub[] = "MEOWPUUT-PINYIN-2\na\ta1\t\xe5\x95\x8a\t\xe5\x95\x8a\t1\t1\n";
        return mpn_lexicon_create(Stub, sizeof(Stub) - 1);
    }
    bool canStartThinClient() const {
        const bool on = engineSwitch ? *engineSwitch : tsf_engine::readStandaloneEngine(preferenceRoot);
        if (!on) return false;
        try {
            if (!engineThinOverride && !harukoto::engine::discovery::inspectProcess(GetCurrentProcess()).supported())
                return false;
            const auto probe = engineReleaseProbe ? engineReleaseProbe : [] {
                return harukoto::engine::release::evaluate(harukoto::engine::release::read(), harukoto::engine::ProtocolVersion);
            };
            return probe().kind == harukoto::engine::release::Compatibility::Match;
        } catch (...) { return false; }
    }
    // WIN-03 step 7d: load local base lexicon, SQLite evidence and theme words
    // strictly on demand (at startup when switch is off / unsupported token /
    // no compatible release, or lazily if thin-client mode falls back).
    bool ensureLocalLexicon(bool adoptPublishedBase = true) {
        if (localLexiconLoaded) return true;
        try {
            const auto folder = installFolder.empty() ? std::filesystem::path(modulePath()).parent_path() : installFolder;
            installFolder = folder;
            if (!baseSnapshot && adoptPublishedBase && !dataRoot.empty()) {
                try {
                    const auto gen = base_dictionary::currentGeneration(dataRoot / L"BaseDictionaries");
                    if (!gen.empty()) baseSnapshot = base_dictionary::Snapshot::load(dataRoot / L"BaseDictionaries" / gen);
                } catch (...) { baseSnapshot.reset(); }
            }
            std::vector<JapaneseWord> baseWords;
            if (baseSnapshot) {
                baseWords = baseSnapshot->words();
            } else {
                std::ifstream japanese(folder / L"japanese.tsv", std::ios::binary);
                baseWords = readJapaneseWords(japanese);
                appendConjugationWords(conjugations, baseWords);
            }
            japaneseWords = baseWords;
            if (!dataRoot.empty()) {
                const auto customPath = dataRoot / L"japanese-custom.tsv";
                auto personal = readPersonalDictionary(customPath);
                const auto themeRoot = dataRoot / L"Dictionaries";
                auto snapshot = readJapanesePackageSnapshot(themeRoot);
                themeReload = std::make_unique<ThemeReload>(themeRoot, baseWords, snapshot ? snapshot->generation : std::string(),
                    customPath, personal, dataRoot / L"BaseDictionaries");
                if (personal) japaneseWords.insert(japaneseWords.end(), personal->words.begin(), personal->words.end());
                if (snapshot) japaneseWords.insert(japaneseWords.end(), std::make_move_iterator(snapshot->words.begin()), std::make_move_iterator(snapshot->words.end()));
            }
            std::stable_sort(japaneseWords.begin(), japaneseWords.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
            MPNLexicon *loadedLexicon = nullptr;
            if (!baseSnapshot) {
                evidence.open(folder / L"language-evidence.sqlite");
                std::ifstream file(folder / L"native-pinyin.tsv", std::ios::binary);
                std::string bytes;
                file.seekg(0, std::ios::end);
                if (const auto size = file.tellg(); size > 0) {
                    bytes.resize(static_cast<size_t>(size));
                    file.seekg(0);
                    if (!file.read(bytes.data(), size)) bytes.clear();
                }
                loadedLexicon = mpn_lexicon_create(bytes.data(), bytes.size());
                if (!loadedLexicon) return false;
            }
            auto migrate = [&](MPNSession *oldSession, bool script) -> MPNSession * {
                MPNSession *fresh = baseSnapshot ? baseSnapshot->session(script).release() : mpn_session_create(loadedLexicon);
                if (!fresh) return nullptr;
                mpn_set_simplified(fresh, script);
                if (oldSession) {
                    mpn_set_fuzzy_pinyin(fresh, mpn_fuzzy_pinyin(oldSession));
                    mpn_set_typo_correction(fresh, mpn_typo_correction(oldSession));
                    if (*mpn_segment_context(oldSession)) mpn_set_segment_context(fresh, mpn_segment_context(oldSession));
                    if (*mpn_input(oldSession)) mpn_set_input(fresh, mpn_input(oldSession));
                }
                return fresh;
            };
            MPNSession *freshActive = migrate(session, simplified);
            if (!freshActive) { mpn_lexicon_destroy(loadedLexicon); return false; }
            std::vector<MPNSession *> freshSuspended;
            freshSuspended.reserve(suspended.size());
            for (const auto &entry : suspended) {
                MPNSession *s = migrate(entry.second.session, entry.second.simplified);
                if (!s) {
                    mpn_session_destroy(freshActive);
                    for (auto *item : freshSuspended) mpn_session_destroy(item);
                    mpn_lexicon_destroy(loadedLexicon);
                    return false;
                }
                freshSuspended.push_back(s);
            }
            mpn_session_destroy(session);
            session = freshActive;
            size_t idx = 0;
            for (auto &entry : suspended) {
                mpn_session_destroy(entry.second.session);
                entry.second.session = freshSuspended[idx++];
            }
            mpn_lexicon_destroy(lexicon);
            lexicon = loadedLexicon;
            localLexiconLoaded = true;
            engineClient.stats.localLexicon = true;
            engineIdentityCache = {};
            attachPersonal();
            return true;
        } catch (...) { return false; }
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
        pollEngineUpgrade(now);
        pollRendererHandoff(now);
        reportRuntimeStatus();
        return changed;
    }
    void reportRuntimeStatus() noexcept {
        try {
            using base_dictionary::Metadata;
            // WIN-08: how the host handles input (no text): the input mode,
            // open/close changes from outside, who draws the candidates and
            // inline compositions.
            const auto count = [](unsigned value) { return std::to_string(value); };
            runtimeStatus.publish("\"service_version\":\"0.4.4-alpha.37\",\"base_version\":" +
                Metadata::quote(baseSnapshot ? baseSnapshot->version() : std::string()) + "," +
                (themeReload ? themeReload->diagnosticFields() :
                    "\"base_generation\":\"\",\"theme_generation\":\"\",\"pending_base_generation\":\"\",\"phase\":\"unavailable\",\"last_apply_failed\":false")
                + ",\"input_mode\":\"" + (chineseMode ? "chinese" : openRule.hostClosed ? "host_closed" : "english")
                + "\",\"open_close_changes\":" + count(openRule.changes) + ",\"host_closes\":" + count(openRule.hostCloses)
                + ",\"candidate_ui\":\"" + game.candidateUi + "\",\"ui_elements\":" + count(game.elementsBegun)
                + ",\"ui_elements_host_drawn\":" + count(game.elementsHostDrawn) + ",\"ui_element_failures\":" + count(game.elementFailures)
                + ",\"compositions\":" + count(game.compositions) + ",\"composition_failures\":" + count(game.compositionFailures)
                + ",\"compositions_terminated\":" + count(game.compositionsTerminated)
                + engineClient.diagnosticFields()
                + rendererHandoff.diagnosticFields());
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
            if (applyPinyinScript(session, old.simplified, simplified)) refreshCandidates(engineClient.enabled);
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
        engineChosenWords = false;
        engineChoicesActive = false;
        engineExpansionRound = 0;
        engineMorePages = false;
        engineFetchedMaxRound = 0;
        engineDeferred = false;
        for (auto &r : engineExtraRounds) r.clear();
        engineClient.cancel();
        engineClient.boundary();
        numberInput.clear();
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

    void refreshCandidates(bool allowEngine = false) {
        engineClient.cancel();
        engineChoicesActive = false;
        engineExpansionRound = 0;
        engineMorePages = false;
        engineFetchedMaxRound = 0;
        engineDeferred = false;
        for (auto &r : engineExtraRounds) r.clear();
        conjugationPanel.clear();
        page = highlighted = 0;
        if (!*mpn_input(session)) {
            resetCandidates();
            return;
        }
        // CN-07 batch 6: in number mode the generated forms are the list.
        if (!numberInput.empty()) {
            choices = quickCandidates(mpn_input(session), numberInput, clock());
            return;
        }
        // WIN-03 step 6a/7d: the engine lists the reading; the candidates stay
        // empty until its reply (deliverEngine) or a key that needs them.
        if (allowEngine && engineClient.enabled) {
            if (engineRequest()) {
                choices.clear();
                return;
            }
            // Step 7d: in thin-client mode, while the worker is completing its
            // ~1 ms connect/handshake, defer instead of loading the 185 MiB
            // local lexicon; onEngineReply() sends as soon as Ready (or loads
            // fallback if connect fails).
            if (!localLexiconLoaded && engineClient.stats.reason == "connecting") {
                engineDeferred = true;
                choices.clear();
                return;
            }
        }
        if (!localLexiconLoaded && !ensureLocalLexicon(true)) {
            choices = quickCandidates(mpn_input(session), {}, clock());
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
        // vrq / vsj / vxq: the date, time or weekday first; vfh, vbd ...: symbols.
        mergeQuickCandidates(choices, quickCandidates(mpn_input(session), {}, clock()));
    }

    size_t candidateCount() const { return choices.size(); }

    // WIN-03 step 6a (docs/ENGINE_TSF.md). The data identity of what this
    // service lists from: its base snapshot, theme packages and personal
    // table. Empty (never equal to an engine's) on the installation folder.
    const std::string &engineIdentity() {
        auto &cache = engineIdentityCache;
        // Step 6c: on the version folder's own files, its manifest names them.
        if (!baseSnapshot && !installGenerationRead) {
            installGenerationRead = true;
            try { if (!installFolder.empty()) installGeneration = base_dictionary::manifestGeneration(installFolder); }
            catch (...) { installGeneration.clear(); }
        }
        const std::string &base = baseSnapshot ? baseSnapshot->generation() : installGeneration;
        if (base.empty() || !themeReload || !themeReload->personal()) { cache = {}; return cache.value; }
        const auto *personal = themeReload->personal().get();
        if (cache.value.empty() || cache.base != base || cache.theme != themeReload->activeGeneration() ||
            cache.personal != personal) {
            cache.base = base; cache.theme = themeReload->activeGeneration(); cache.personal = personal;
            cache.value = harukoto::engine::dataIdentity(cache.base, cache.theme,
                harukoto::engine::personalIdentity(personal->present, personal->bytes));
        }
        return cache.value;
    }
    // Capability gate: what the engine cannot rank yet stays in-process.
    const char *engineUnsupported() const {
        const std::string input = mpn_input(session);
        if (!harukoto::engine::inputValid(input)) return "input";
        // Step 6c: partial selection travels with the request; only a
        // segment context longer than the protocol carries stays here.
        if (std::string_view(mpn_segment_context(session)).size() > 64) return "context";
        if (localLexiconLoaded && !tsf_engine::representable(session)) return "unrepresentable";
        return nullptr;
    }
    std::unique_ptr<tsf_engine::Link> engineLink() {
        if (!engineWindow) {
            engineWindow = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
            if (engineWindow) {
                SetWindowLongPtrW(engineWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                originalEngineProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                    engineWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&engineWindowProc)));
            }
        }
        const HWND target = engineWindow;
        // Step 6c: an owner started by this service serves the data it uses.
        harukoto::engine::resident::EnsureOptions options;
        options.ownerArguments = engineLayout();
        return tsf_engine::Link::threaded(tsf_engine::residentConnect({}, options), [target] { if (target) PostMessageW(target, WM_APP + 0x31, 0, 0); });
    }
    std::vector<std::wstring> engineLayout() const {
        // Step 6d: the version folder and the roots; the engine picks the
        // published snapshot and packages that are current, as this service does.
        return tsf_engine::layoutArguments(installFolder, dataRoot, themeReload && !themeReload->activeGeneration().empty());
    }
    static LRESULT CALLBACK engineWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto *service = reinterpret_cast<Service *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_APP + 0x31 && service) {
            try { service->onEngineReply(); } catch (...) {}
            return 0;
        }
        if (message == WM_TIMER && wParam == EngineIdleTimerId && service) {
            try { service->onEngineIdleTimer(); } catch (...) {}
            return 0;
        }
        return service && service->originalEngineProc
            ? CallWindowProcW(service->originalEngineProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }
    // WIN-03 step 6b: what this service ranks with besides the data: fuzzy
    // pinyin and typo switch as its session has them, and the exact state of
    // the learning it applies (EngineMemory.h). Digests are recomputed only
    // when a model's revision moved.
    harukoto::engine::QuerySettings engineSettings() {
        namespace engine = harukoto::engine;
        engine::QuerySettings q;
        q.fuzzy = mpn_fuzzy_pinyin(session);
        q.typo = mpn_typo_correction(session) != 0;
        q.memory = memoryEnabled;
        q.hour = memoryEnabled ? personalHour : 0;
        q.context = mpn_segment_context(session);
        q.locked = chineseLocked;
        auto &cache = engineMemoryCache;
        const uint64_t user = memory ? memory->userWordsRevision() : 0;
        if (cache.user.empty() || cache.userRevision != user || cache.userStore != memory.get()) {
            cache.user = memory ? engine::memoryDigest(memory->userWords()) : engine::memoryDigest(selection_memory::UserWordModel{});
            cache.userRevision = user; cache.userStore = memory.get();
        }
        q.user = cache.user;
        if (memoryEnabled) {
            // Step 6d: our own recall for this reading (pending learning
            // included) instead of a phrase digest; the words the core ranked
            // with: as before the segment just chosen, if one was.
            q.remembered = memory->recall(mpn_input(session), simplified);
            if (q.remembered.size() > engine::MaxRemembered) q.remembered.resize(engine::MaxRemembered);
            q.words = engineChosenWords ? engineChosenDigest : engineWordsDigest();
        }
        return q;
    }
    const std::string &engineWordsDigest() {
        auto &cache = engineMemoryCache;
        if (cache.words.empty() || cache.wordsRevision != memory->wordsRevision() || cache.store != memory.get()) {
            cache.words = harukoto::engine::memoryDigest(memory->words()); cache.wordsRevision = memory->wordsRevision();
            cache.store = memory.get();
        }
        return cache.words;
    }
    bool engineRequest() {
        if (const char *reason = engineUnsupported()) { engineClient.inProcess(reason); return false; }
        if (!engineClient.factory) engineClient.factory = [this] { return engineLink(); };
        const auto now = GetTickCount64();
        const std::string emptyIdentity;
        if (!engineClient.ready(now, localLexiconLoaded ? engineIdentity() : emptyIdentity, !localLexiconLoaded)) return false;
        auto settings = engineSettings();
        if (engineClient.knownBehind(settings, now)) { engineClient.inProcess("memory_behind"); return false; }
        engineSent = settings;
        engineClient.send(mpn_input(session), simplified, now, std::move(settings));
        return true;
    }
    // Takes the engine's reply when it has arrived. True when the list
    // changed: the engine's, or the in-process one after a failure.
    bool deliverEngine() {
        if (!session || !engineClient.link()) return false;
        const bool waiting = engineClient.pending();
        const std::string input = mpn_input(session);
        tsf_engine::Fetched fetched;
        std::string reason;
        const auto now = GetTickCount64();
        switch (engineClient.collect(input, simplified, now, fetched, reason)) {
        case tsf_engine::Client::Collect::Waiting: return false;
        case tsf_engine::Client::Collect::Failed:
        case tsf_engine::Client::Collect::Declined:
            if (!waiting) return false;
            refreshCandidates();
            return true;
        case tsf_engine::Client::Collect::Ready: break;
        }
        if (fetched.behind) { engineClient.behind(engineSent, now); refreshCandidates(); return true; }
        // Step 6d: in full-lexicon mode verify identity matches local data; in
        // step 7d thin-client mode the engine's own identity is authoritative.
        if (localLexiconLoaded && !fetched.identity.empty() && fetched.identity != engineIdentity()) { engineClient.decline("identity"); refreshCandidates(); return true; }
        if (!fetched.complete) { engineClient.decline("long_list"); refreshCandidates(); return true; }
        auto rebuilt = localLexiconLoaded
            ? tsf_engine::rebuild(session, fetched.words)
            : tsf_engine::materialize(input, fetched.words, true);
        if (!rebuilt) { engineClient.fail(now, "mismatch"); refreshCandidates(); return true; }
        choices = std::move(*rebuilt);
        mergeQuickCandidates(choices, quickCandidates(input, {}, clock()));
        engineExtraRounds = std::move(fetched.extraRounds);
        engineMorePages = fetched.morePages;
        engineFetchedMaxRound = fetched.roundComplete ? fetched.maxRound
            : (fetched.maxRound ? static_cast<uint8_t>(fetched.maxRound - 1) : 0);
        engineExpansionRound = 0;
        engineChoicesActive = true;
        engineClient.served();
        return true;
    }
    // Before a key: a key that reads the candidates must not act on a list
    // still owed by the engine. Letters and Backspace replace it anyway.
    // Step 7d: in thin-client mode wait briefly synchronously for an in-flight
    // connect/reply before ever falling back to lazy-loading local lexicon.
    void settleEngine(WPARAM key) {
        if (!session) return;
        const bool edit = ((key >= 'A' && key <= 'Z') || key == VK_BACK) && GetKeyState(VK_SHIFT) >= 0 &&
            GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0;
        if (engineDeferred) {
            if (edit) return;
            engineDeferred = false;
            if (engineClient.enabled && engineClient.waitReady(1500) && engineRequest()) {
                engineClient.waitPending(tsf_engine::DeadlineMs);
                deliverEngine();
            }
            if (engineClient.pending() || choices.empty()) {
                if (engineClient.pending()) engineClient.decline("pending_key");
                if (*mpn_input(session)) refreshCandidates();
            }
            return;
        }
        if (!engineClient.pending()) return;
        deliverEngine();
        if (!engineClient.pending() || edit) return;
        if (!localLexiconLoaded) {
            engineClient.waitPending(tsf_engine::DeadlineMs);
            deliverEngine();
            if (!engineClient.pending()) return;
        }
        engineClient.decline("pending_key");
        refreshCandidates();
    }
    bool pollEngineUpgrade(ULONGLONG now = GetTickCount64(), bool force = false) {
        if (!engineClient.enabled || !dictionaryBoundary() || engineClient.pending()) return false;
        if (!engineClient.factory) engineClient.factory = [this] { return engineLink(); };
        const auto probe = engineReleaseProbe ? engineReleaseProbe : [] {
            return harukoto::engine::release::evaluate(harukoto::engine::release::read(), harukoto::engine::ProtocolVersion);
        };
        return engineClient.pollUpgrade(now, probe, force);
    }
    void armEngineIdleTimer() {
        if (!engineWindow) return;
        if (!engineClient.enabled || (!engineClient.link() && !engineClient.standby())) {
            KillTimer(engineWindow, EngineIdleTimerId);
            return;
        }
        const uint64_t delay = engineFocused ? engineIdleYieldMs : engineFocusYieldMs;
        if (!delay) { KillTimer(engineWindow, EngineIdleTimerId); return; }
        SetTimer(engineWindow, EngineIdleTimerId, static_cast<UINT>(delay), nullptr);
    }
    bool onEngineIdleTimer() {
        if (engineWindow) KillTimer(engineWindow, EngineIdleTimerId);
        if (!engineClient.enabled || !dictionaryBoundary() || engineClient.pending()) return false;
        if (!engineClient.yield()) return false;
        reportRuntimeStatus();
        return true;
    }
    void onEngineReply() {
        bool updated = deliverEngine();
        if (!updated && engineDeferred && !engineClient.pending() && session && *mpn_input(session)) {
            engineDeferred = false;
            refreshCandidates(true);
            updated = !engineClient.pending() && !engineDeferred;
        }
        if (updated && activeContext && session && delivery && delivery->queue.canDeliver()) {
            updatePopup();     // Our window, or the host's UI element (UI-less).
            syncComposition(); // WIN-08: as after a key.
        }
        const auto now = GetTickCount64();
        if (engineClient.standby() && dictionaryBoundary() && !engineClient.pending())
            pollEngineUpgrade(now, true);
        pollRendererHandoff(now);
        armEngineIdleTimer();
        reportRuntimeStatus();
    }

    bool canExpandCandidates() const {
        if (!session || !*mpn_input(session)) return false;
        if (engineChoicesActive) {
            if (engineExpansionRound >= 3) return false;
            if (engineMorePages) return true;
            for (size_t r = engineExpansionRound; r < 3; ++r)
                if (!engineExtraRounds[r].empty()) return true;
            return false;
        }
        return mpn_candidates_can_expand(session) != 0;
    }

    void expandCandidates() {
        const auto before = choices.size();
        if (engineChoicesActive) {
            const std::string input = mpn_input(session);
            while (choices.size() == before && engineExpansionRound < 3) {
                ++engineExpansionRound;
                size_t pagesFetched = 0;
                while (engineMorePages && engineFetchedMaxRound < engineExpansionRound && pagesFetched < tsf_engine::Driver::MaxPages) {
                    ++pagesFetched;
                    auto more = engineClient.fetchMore(input, simplified, engineSent, GetTickCount64());
                    if (!more || more->behind) {
                        engineMorePages = false;
                        break;
                    }
                    const uint8_t done = more->roundComplete ? more->maxRound
                        : (more->maxRound ? static_cast<uint8_t>(more->maxRound - 1) : 0);
                    if (done > engineFetchedMaxRound) engineFetchedMaxRound = done;
                    for (size_t r = 0; r < 3; ++r)
                        engineExtraRounds[r].insert(engineExtraRounds[r].end(),
                            std::make_move_iterator(more->extraRounds[r].begin()),
                            std::make_move_iterator(more->extraRounds[r].end()));
                    engineMorePages = more->morePages;
                }
                auto &roundWords = engineExtraRounds[engineExpansionRound - 1];
                if (!roundWords.empty()) {
                    if (auto batch = tsf_engine::materialize(input, roundWords, false)) {
                        choices.insert(choices.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
                    }
                    roundWords.clear();
                }
            }
            return;
        }
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
            refreshCandidates(engineClient.enabled);
            if (engineClient.pending() || engineDeferred) settleEngine(VK_RETURN);
            choice = 0;
        }
    }

    // The reading as shown: the core's, then any number typed after the v.
    std::wstring preeditText() const { return wide(mpn_preedit(session)) + std::wstring(numberInput.begin(), numberInput.end()); }

    static quick_input::Moment localMoment() {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        return {now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond};
    }

    std::wstring candidateText(size_t index) const { return wide(choices[index].text.c_str()); }

    std::wstring candidateReading(size_t index) const {
        if (!choices[index].annotation.empty()) return wide(choices[index].annotation.c_str());
        if (!choices[index].coreIndex) return {};
        const auto consumed = choices[index].consumed ? choices[index].consumed
            : mpn_candidate_consumed(session, *choices[index].coreIndex);
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
        const bool form = conjugationPanel.active() || choices[index].generated;
        // WIN-03 step 7c: candidates produced by the engine carry their own
        // consumed/literalTail metadata and query decomposition + context from
        // the engine so selection never touches a local MPNSession.
        if (choices[index].engineId) {
            const auto &choice = choices[index];
            const size_t consumed = choice.consumed ? choice.consumed : input.size();
            const std::string committed = (choice.literalTail && choice.literalTail <= choice.text.size())
                ? choice.text.substr(0, choice.text.size() - choice.literalTail) : choice.text;
            const std::string remaining = consumed <= input.size() ? input.substr(consumed) : std::string();
            const bool needWords = memoryEnabled && learn && !form && choice.coreIndex.has_value();
            const bool needContext = !remaining.empty() && choice.coreIndex.has_value();
            std::string words, nextContext;
            if (needWords || needContext) {
                if (auto info = engineClient.inspect(input.substr(0, (std::min)(consumed, input.size())),
                        committed, mpn_segment_context(session), simplified, GetTickCount64())) {
                    words = std::move(info->decomposed);
                    nextContext = std::move(info->nextContext);
                } else if (localLexiconLoaded && choice.coreIndex) {
                    words = needWords ? std::string(mpn_candidate_words(session, *choice.coreIndex)) : std::string();
                    if (engineClient.enabled && memoryEnabled) { engineChosenDigest = engineWordsDigest(); engineChosenWords = true; }
                    if (!mpn_choose(session, *choice.coreIndex)) return false;
                    chineseLocked = *mpn_input(session) != '\0';
                    if (memoryEnabled && learn && !form && consumed && consumed <= input.size()) remember(input, consumed, chosen, words);
                    else { composedReading.clear(); composedText.clear(); composedSegments = 0; }
                    return true;
                }
            }
            if (choice.coreIndex) {
                if (engineClient.enabled && memoryEnabled) { engineChosenDigest = engineWordsDigest(); engineChosenWords = true; }
                text += wide(committed.c_str());
                mpn_clear(session);
                if (!remaining.empty()) {
                    if (!nextContext.empty()) mpn_set_segment_context(session, nextContext.c_str());
                    mpn_set_input(session, remaining.c_str());
                }
                chineseLocked = !remaining.empty();
            } else {
                text += candidateText(index);
                mpn_clear(session);
                chineseLocked = false;
            }
            if (memoryEnabled && learn && !form && consumed && consumed <= input.size()) remember(input, consumed, chosen, words);
            else { composedReading.clear(); composedText.clear(); composedSegments = 0; }
            return true;
        }
        const size_t consumed = choices[index].coreIndex ? mpn_candidate_consumed(session, *choices[index].coreIndex) : input.size();
        // CN-06c: the dictionary words making up a core candidate, read
        // before mpn_choose replaces the candidate list. Japanese, English
        // and other non-core candidates are not learned word by word.
        const std::string words = memoryEnabled && learn && !form && choices[index].coreIndex
            ? std::string(mpn_candidate_words(session, *choices[index].coreIndex)) : std::string();
        if (choices[index].coreIndex) {
            // Step 6d: the rest is listed now, with the bonuses from before
            // this choice teaches anything; the engine is told so.
            if (engineClient.enabled && memoryEnabled) { engineChosenDigest = engineWordsDigest(); engineChosenWords = true; }
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

    // CN-07 batch 5: forget what the IME learned for a candidate, never the
    // base dictionary:
    std::wstring forgetCandidate(size_t index) {
        if (index >= candidateCount() || conjugationPanel.active()) return L"没有学到的记录";
        const WindowsCandidate candidate = choices[index];
        const std::string input = mpn_input(session);
        const size_t consumed = candidate.consumed ? candidate.consumed
            : (candidate.coreIndex ? mpn_candidate_consumed(session, *candidate.coreIndex) : input.size());
        const bool personal = candidate.engineId ? candidate.personal
            : (candidate.coreIndex && mpn_candidate_personal(session, *candidate.coreIndex));
        bool forgot = false;
        if (memory) {
            memory->poll();
            if (consumed && consumed <= input.size()) forgot = memory->forget(input.substr(0, consumed), candidate.text, simplified);
            if (candidate.coreIndex) {
                std::string words;
                if (candidate.engineId) {
                    const std::string committed = (candidate.literalTail && candidate.literalTail <= candidate.text.size())
                        ? candidate.text.substr(0, candidate.text.size() - candidate.literalTail) : candidate.text;
                    if (auto info = engineClient.inspect(input.substr(0, (std::min)(consumed, input.size())),
                            committed, "", simplified, GetTickCount64()))
                        words = std::move(info->decomposed);
                    else if (localLexiconLoaded)
                        words = mpn_candidate_words(session, *candidate.coreIndex);
                } else {
                    words = mpn_candidate_words(session, *candidate.coreIndex);
                }
                const auto hour = selection_memory::currentHour();
                for (size_t start = 0; start < words.size();) {
                    const auto end = words.find('\n', start), tab = words.find('\t', start);
                    if (end == words.npos || tab == words.npos || tab > end) break;
                    forgot = memory->forgetWord(std::string_view(words).substr(start, tab - start),
                        std::string_view(words).substr(tab + 1, end - tab - 1), simplified, hour) || forgot;
                    start = end + 1;
                }
            }
            if (forgot) memory->flush();
        }
        if (forgot) {
            // New bonuses for the core, then the same reading ranked again.
            attachPersonal();
            const std::string context = mpn_segment_context(session);
            mpn_set_segment_context(session, context.c_str());
            refreshCandidates(true); // Step 6c: the same reading, through the engine when on.
        }
        if (personal) return forgot ? L"已忘记学习记录；个人词请用 manage-selection-memory.ps1 -RemoveWord 删除"
                                    : L"这是手动添加的个人词，请用 manage-selection-memory.ps1 -RemoveWord 删除";
        return forgot ? L"已忘记" : L"没有学到的记录";
    }

    // CN-07 batch 4, 以词定字:
    bool pickCharacter(size_t index, bool last, std::wstring &text) {
        if (index >= candidateCount() || !choices[index].coreIndex) return false;
        if (choices[index].engineId) {
            const auto choice = choices[index];
            const std::string input = mpn_input(session);
            const size_t consumed = choice.consumed ? choice.consumed : input.size();
            const std::string committed = (choice.literalTail && choice.literalTail <= choice.text.size())
                ? choice.text.substr(0, choice.text.size() - choice.literalTail) : choice.text;
            const std::string remaining = consumed <= input.size() ? input.substr(consumed) : std::string();
            const std::string character = pinyinWordCharacter(committed, last);
            std::string nextContext;
            if (!remaining.empty()) {
                if (auto info = engineClient.inspect(input.substr(0, (std::min)(consumed, input.size())),
                        character, mpn_segment_context(session), simplified, GetTickCount64()))
                    nextContext = std::move(info->nextContext);
                else
                    nextContext = std::string(mpn_segment_context(session)) + character;
            }
            text += wide(mpn_committed(session)) + wide(character.c_str());
            mpn_clear_commit(session);
            mpn_clear(session);
            chineseLocked = !remaining.empty();
            if (chineseLocked) {
                if (!nextContext.empty()) mpn_set_segment_context(session, nextContext.c_str());
                mpn_set_input(session, remaining.c_str());
            }
            composedReading.clear(); composedText.clear(); composedSegments = 0;
            refreshCandidates(true);
            return true;
        }
        const std::string context = mpn_segment_context(session);
        const size_t before = std::string(mpn_committed(session)).size();
        if (!mpn_choose(session, *choices[index].coreIndex)) return false;
        const std::string committed = mpn_committed(session);
        const std::string character = pinyinWordCharacter(committed.substr((std::min)(before, committed.size())), last);
        text += wide(committed.substr(0, (std::min)(before, committed.size())).c_str()) + wide(character.c_str());
        mpn_clear_commit(session);
        chineseLocked = *mpn_input(session) != '\0';
        if (chineseLocked) mpn_set_segment_context(session, (context + character).c_str());
        composedReading.clear(); composedText.clear(); composedSegments = 0;
        refreshCandidates(true); // Step 6c: the rest may be listed by the engine.
        return true;
    }

    // CN-06: record the confirmed selection, and the whole phrase once a
    // reading has been completed by selecting several segments.
    void remember(const std::string &input, size_t consumed, const std::string &chosen, const std::string &words = {}) {
        const auto reading = input.substr(0, consumed);
        if (chosen != reading) memory->record(reading, chosen, simplified);
        learnWords(words);
        composedReading += reading; composedText += chosen; ++composedSegments;
        if (consumed != input.size()) {
            // Step 6d: with the engine on, write each segment's learning at
            // once, so the engine can rank the following keys with it.
            if (engineClient.enabled) memory->flush();
            return;
        }
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
        if (applyFuzzyPinyin(session, mask) && *mpn_input(session)) refreshCandidates(engineClient.enabled);
    }

    // CN-05: gives the active session the user's typo correction switch,
    // like applyFuzzy.
    void applyTypo(bool enabled) {
        if (applyTypoCorrection(session, enabled) && *mpn_input(session)) refreshCandidates(engineClient.enabled);
    }

    // Hands the learned word bonuses and the user's own words (DICT-02b) to
    // the active session. While selection memory is off the bonuses are
    // removed; without user words either, the table is detached so the core
    // behaves exactly as without.
    void attachPersonal() {
        if (!localLexiconLoaded) {
            if (memoryEnabled) personalHour = selection_memory::currentHour();
            return;
        }
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

    void ensureRendererInitialized() {
        if (rendererReady) return;
        const auto folder = installFolder.empty()
            ? std::filesystem::path(modulePath()).parent_path() : installFolder;
        const auto id = std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(reinterpret_cast<uintptr_t>(this));
        rendererHost.reset(folder / L"MeowpuutGlassPreview.exe",
            rendererSourceOverride.value_or(harukoto::renderer::release::Source{}),
            harukoto::renderer::release::PayloadVersion, false);
        rendererHandoff.configure(rendererHost.ops(), id, harukoto::renderer::release::PayloadVersion);
        rendererReady = true;
    }

    bool rendererHandoffEnabled() const {
        if (rendererSwitchOverride.has_value()) return *rendererSwitchOverride;
        return renderer_handoff::readRendererHandoff(preferenceRoot);
    }

    bool rendererBoundary() const {
        return dictionaryBoundary() && !engineClient.pending() && !engineDeferred && rendererHandoff.lastSentEmpty();
    }

    void pollRendererHandoff(ULONGLONG now = GetTickCount64()) {
        if (destroying) return;
        ensureRendererInitialized();
        rendererHandoff.onBoundary(now, rendererBoundary(), rendererHandoffEnabled());
    }

    bool sendGlass(const std::wstring &text) {
        if (text.size() >= 4096) return false;
        if (text.empty() && !rendererReady) return false;
        ensureRendererInitialized();
        if (text.empty() && !rendererHost.lastForeground() && !rendererHandoff.hasCurrent()) return false;
        return rendererHandoff.send(text, GetTickCount64(), rendererBoundary(), rendererHandoffEnabled());
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
            endCandidateElement();
            sendGlass({});
            if (popup) ShowWindow(popup, SW_HIDE);
            return;
        }
        // Keep the previous popup / host UI element steady while an async
        // engine reply (or initial thin-client connect) is in flight instead of
        // flashing an empty candidate list on every keystroke.
        if (engineClient.pending() || engineDeferred) return;
        // WIN-08: the host may draw the candidates itself.
        if (!presentCandidates()) {
            hideOwnWindow();
            return;
        }
        auto display = preeditText() + (simplified ? L"  简" : L"  繁") + (englishPunctuation ? L" · 英文标点" : L"") + (fullWidthOn() ? L" · 全角" : L"") + (engineClient.stats.incompatible ? L" · 输入法已更新，请重开应用" : L"") + (notice.empty() ? L"" : L" · " + notice);
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
            + preeditText() + L"\t" + std::to_wstring(highlighted);
        const size_t pageCount = (candidateCount() + pageSize - 1) / pageSize;
        payload += simplified ? L"\t简" : L"\t繁";
        if (englishPunctuation) payload += L" · 英文标点";
        if (fullWidthOn()) payload += L" · 全角";
        if (themeReload && themeReload->failed()) payload += L" · 词库更新失败，沿用旧版";
        if (engineClient.stats.incompatible) payload += L" · 输入法已更新，请重开应用";
        if (!notice.empty()) payload += L" · " + notice;
        payload += conjugationHint;
        if (pageCount > 1 || (!conjugationPanel.active() && canExpandCandidates()))
            payload += L"  ·  " + std::to_wstring(page + 1) + L" / " + std::to_wstring(pageCount);
        if (!conjugationPanel.active() && canExpandCandidates()) payload += L"+";
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

    // ---- WIN-08: candidate UI element and inline composition ------------

    ITfUIElementMgr *uiElements() const {
        if (!threadManager) return nullptr;
        ITfUIElementMgr *manager = nullptr;
        if (FAILED(threadManager->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void **>(&manager)))) return nullptr;
        return manager;
    }

    // Begins the candidate element when candidates appear, or tells the host
    // what changed. False: the host draws them and our window stays hidden.
    // Without a UI element manager (or when it fails) our window is used.
    bool presentCandidates() {
        std::vector<std::string> texts;
        texts.reserve(choices.size());
        for (const auto &choice : choices) texts.push_back(choice.text);
        const size_t selection = page * pageSize + highlighted;
        if (!candidateElement) {
            ITfUIElementMgr *manager = uiElements();
            if (!manager) {
                game.candidateUi = "own";
                return true;
            }
            auto *element = new (std::nothrow) CandidateElement(this);
            BOOL show = TRUE;
            DWORD id = TF_INVALID_UIELEMENTID;
            // The host may read the element during the call: the state is ready.
            const HRESULT result = element ? manager->BeginUIElement(element, &show, &id) : E_OUTOFMEMORY;
            manager->Release();
            if (FAILED(result) || id == TF_INVALID_UIELEMENTID) {
                if (element) { element->owner = nullptr; element->Release(); }
                ++game.elementFailures;
                game.candidateUi = "own_fallback";
                return true;
            }
            candidateElement = element;
            candidateElementId = id;
            candidateShown = show != FALSE;
            ++game.elementsBegun;
            if (!candidateShown) ++game.elementsHostDrawn;
        } else {
            DWORD flags = 0;
            if (texts != shownTexts) flags |= TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX;
            if (selection != shownSelection) flags |= TF_CLUIE_SELECTION;
            if (page != shownPage) flags |= TF_CLUIE_CURRENTPAGE;
            if (flags) {
                candidateElement->updated |= flags;
                if (ITfUIElementMgr *manager = uiElements()) {
                    manager->UpdateUIElement(candidateElementId);
                    manager->Release();
                }
            }
        }
        shownTexts = std::move(texts);
        shownSelection = selection;
        shownPage = page;
        game.candidateUi = candidateShown ? "own" : "host";
        return candidateShown;
    }

    void endCandidateElement() {
        if (!candidateElement) return;
        auto *element = candidateElement;
        const DWORD id = candidateElementId;
        candidateElement = nullptr;
        candidateElementId = TF_INVALID_UIELEMENTID;
        shownTexts.clear();
        if (ITfUIElementMgr *manager = uiElements()) {
            manager->EndUIElement(id);
            manager->Release();
        }
        element->owner = nullptr;
        element->Release();
    }

    // The host's Show() on the element: it hands the candidates back to us
    // or takes them over.
    void showCandidatesByHost(bool show) {
        if (!candidateElement || candidateShown == show) return;
        candidateShown = show;
        if (show) {
            if (session && activeContext) updatePopup();
        } else {
            hideOwnWindow();
        }
        try { syncComposition(); } catch (...) {}
    }

    bool inlineWanted() const {
        if (!session || !*mpn_input(session) || inlineSetting == inlineCompositionNever) return false;
        return inlineSetting == inlineCompositionAlways || (candidateElement && !candidateShown);
    }

    void releaseComposition() {
        if (composition) composition->Release();
        if (compositionContext) compositionContext->Release();
        composition = nullptr;
        compositionContext = nullptr;
    }

    // Edit sessions are synchronous where the host allows, else queued in order.
    void requestComposition(ITfContext *context, ITfComposition *ending) {
        auto *edit = new (std::nothrow) CompositionEdit(this, context, ending);
        if (!edit) return;
        HRESULT result = E_FAIL;
        const HRESULT request = context->RequestEditSession(client, edit, TF_ES_SYNC | TF_ES_READWRITE, &result);
        if (request == TF_E_SYNCHRONOUS || result == TF_E_SYNCHRONOUS)
            context->RequestEditSession(client, edit, TF_ES_ASYNC | TF_ES_READWRITE, &result);
        edit->Release();
    }

    // Ends the inline reading: it is detached at once, its text removed
    // whenever the edit session runs.
    void endComposition() {
        if (!composition) return;
        // No edit session from the destructor: it would hold a reference
        // to an object being destroyed. TSF ends the composition itself.
        if (destroying) { releaseComposition(); return; }
        ITfComposition *ending = composition;
        ITfContext *context = compositionContext;
        composition = nullptr;
        compositionContext = nullptr;
        requestComposition(context, ending);
        ending->Release();
        context->Release();
    }

    // After a key: the document's inline reading follows the current one.
    void syncComposition() {
        if (composition && compositionContext != activeContext) endComposition();
        if (inlineWanted() && activeContext) requestComposition(activeContext, nullptr);
        else endComposition();
    }

    static void finishComposition(TfEditCookie cookie, ITfComposition *ending) {
        ITfRange *range = nullptr;
        if (SUCCEEDED(ending->GetRange(&range)) && range) {
            range->SetText(cookie, 0, L"", 0);
            range->Release();
        }
        ending->EndComposition(cookie);
    }

    // Runs in an edit session: the reading as it is when the session runs.
    HRESULT applyComposition(TfEditCookie cookie, ITfContext *context) {
        if (context != activeContext) return S_OK;
        const std::wstring text = inlineWanted() ? preeditText() : std::wstring();
        if (text.empty()) {
            if (composition && compositionContext == context) {
                ITfComposition *ending = composition;
                ending->AddRef();
                releaseComposition();
                finishComposition(cookie, ending);
                ending->Release();
            }
            return S_OK;
        }
        if (composition && compositionContext != context) releaseComposition();
        HRESULT result = S_OK;
        bool startedNow = false;
        if (!composition) {
            ITfInsertAtSelection *insert = nullptr;
            ITfContextComposition *compose = nullptr;
            ITfRange *start = nullptr;
            ITfComposition *started = nullptr;
            result = context->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void **>(&insert));
            if (SUCCEEDED(result)) result = insert->InsertTextAtSelection(cookie, TF_IAS_QUERYONLY, nullptr, 0, &start);
            if (SUCCEEDED(result) && !start) result = E_FAIL;
            if (SUCCEEDED(result)) result = context->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void **>(&compose));
            if (SUCCEEDED(result)) result = compose->StartComposition(cookie, start, static_cast<ITfCompositionSink *>(this), &started);
            if (SUCCEEDED(result) && !started) result = E_FAIL;
            if (start) start->Release();
            if (compose) compose->Release();
            if (insert) insert->Release();
            if (FAILED(result)) {
                ++game.compositionFailures;
                return result;
            }
            composition = started;
            compositionContext = context;
            context->AddRef();
            ++game.compositions;
            startedNow = true;
        }
        ITfRange *range = nullptr;
        result = composition->GetRange(&range);
        if (SUCCEEDED(result) && range) {
            result = range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
            // The caret after the reading, where the host expects it.
            ITfRange *caret = nullptr;
            if (SUCCEEDED(result) && SUCCEEDED(range->Clone(&caret)) && caret) {
                if (SUCCEEDED(caret->Collapse(cookie, TF_ANCHOR_END))) {
                    TF_SELECTION selection{caret, {TF_AE_NONE, FALSE}};
                    context->SetSelection(cookie, 1, &selection);
                }
                caret->Release();
            }
            range->Release();
        }
        // A host may only take the list while a composition exists: once the
        // first one does, it is told to read everything again.
        if (startedNow && candidateElement) {
            candidateElement->updated |= TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING
                | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
            if (ITfUIElementMgr *manager = uiElements()) {
                manager->UpdateUIElement(candidateElementId);
                manager->Release();
            }
        }
        return result;
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
    // WIN-03 step 6a: the standalone engine client, the window its worker
    // posts to, and the data identity last computed for it.
    tsf_engine::Client engineClient;
    std::optional<bool> engineSwitch; // Tests: overrides StandaloneEngine.
    std::function<harukoto::engine::release::Evaluation()> engineReleaseProbe; // Step 7a: tests override HKLM release probe.
    // Step 7c/7d: thin-client state & engine-backed expansion buffer.
    bool localLexiconLoaded = true;   // Default true for manual test fixtures; Activate() clears in thin-client mode.
    bool engineThinOverride = false;  // Tests only: allow thin-client Activate() in elevated shells.
    bool engineChoicesActive = false;
    bool engineMorePages = false;
    bool engineDeferred = false;
    uint8_t engineExpansionRound = 0, engineFetchedMaxRound = 0;
    std::array<std::vector<harukoto::engine::Candidate>, 3> engineExtraRounds;
    static constexpr UINT_PTR EngineIdleTimerId = 0x32;
    bool engineFocused = true;
    uint64_t engineFocusYieldMs = tsf_engine::Client::FocusYieldMs;
    uint64_t engineIdleYieldMs = tsf_engine::Client::IdleYieldMs;
    HWND engineWindow = nullptr;
    WNDPROC originalEngineProc = nullptr;
    struct { std::string base, theme, value; const void *personal = nullptr; } engineIdentityCache;
    // Step 6c: where this service's data lives (install folder, %LOCALAPPDATA%\Meowpuut)
    // and the generation the install folder's manifest names.
    std::filesystem::path installFolder, dataRoot;
    std::string installGeneration;
    bool installGenerationRead = false;
    // Step 6b: digests of the learning last sent, by model revision, and the
    // settings of the request in flight.
    struct {
        std::string phrases, words, user;
        uint64_t phrasesRevision = 0, wordsRevision = 0, userRevision = 0;
        const void *store = nullptr, *userStore = nullptr;
    } engineMemoryCache;
    harukoto::engine::QuerySettings engineSent;
    // Step 6d: the core listed the rest of the reading after a chosen segment
    // with the word bonuses from before the choice taught anything.
    bool engineChosenWords = false;
    std::string engineChosenDigest;
    // CN-07: punctuation mode as read at the last key, the quote pairing,
    // and where the per-user preferences live (tests use a scratch key).
    bool englishPunctuation = false;
    PinyinQuotes quotes;
    HKEY preferenceRoot = HKEY_CURRENT_USER;
    // CN-07 batch 5: shown in the candidate window's status until the next key.
    std::wstring notice;
    // CN-07 batch 6: digits typed after a lone "v" (number mode), and the
    // clock for dates (tests set a fixed one).
    std::string numberInput;
    // CN-07 batch 7: full-width output, per service instance like the input
    // mode; starts half-width and is not saved.
    bool fullWidth = false;
    quick_input::Moment (*clock)() = &Service::localMoment;
    // CN-07 batch 2: input mode, Shift tap, open/close compartment, taskbar item, hint timer.
    bool chineseMode = true;
    ShiftTap shiftTap;
    ITfThreadMgr *threadManager = nullptr;
    DWORD openCookie = TF_INVALID_COOKIE;
    ModeButton *button = nullptr;
    UINT_PTR hintTimer = 0;
    HWND popup = nullptr;
    HWND popupForeground = nullptr;
    WNDPROC originalPopupProc = nullptr;
    std::vector<size_t> popupIndices;
    int popupRowHeight = 16;
    renderer_handoff::LiveHost rendererHost;
    renderer_handoff::Machine rendererHandoff;
    bool rendererReady = false;
    std::optional<harukoto::renderer::release::Source> rendererSourceOverride;
    std::optional<bool> rendererSwitchOverride;
    // WIN-08: the open/close rule, the candidate element (with what the host
    // was last told), the inline reading and counters for Diagnose.
    OpenCloseRule openRule;
    bool destroying = false;
    CandidateElement *candidateElement = nullptr;
    DWORD candidateElementId = TF_INVALID_UIELEMENTID;
    bool candidateShown = true;
    std::vector<std::string> shownTexts;
    size_t shownSelection = 0, shownPage = 0;
    DWORD inlineSetting = inlineCompositionAuto;
    ITfComposition *composition = nullptr;
    ITfContext *compositionContext = nullptr;
    struct {
        const char *candidateUi = "none";
        unsigned elementsBegun = 0, elementsHostDrawn = 0, elementFailures = 0;
        unsigned compositions = 0, compositionFailures = 0, compositionsTerminated = 0;
    } game;
};

CompositionEdit::CompositionEdit(Service *service, ITfContext *context, ITfComposition *ending)
    : service(service), context(context), ending(ending) {
    service->AddRef();
    context->AddRef();
    if (ending) ending->AddRef();
}
CompositionEdit::~CompositionEdit() {
    if (ending) ending->Release();
    context->Release();
    service->Release();
}
HRESULT CompositionEdit::DoEditSession(TfEditCookie cookie) {
    try {
        if (ending) {
            Service::finishComposition(cookie, ending);
            return S_OK;
        }
        return service->applyComposition(cookie, context);
    } catch (...) { return E_FAIL; }
}

HRESULT CandidateElement::Show(BOOL show) {
    if (owner) {
        try { owner->showCandidatesByHost(show != FALSE); } catch (...) {}
    }
    return S_OK;
}
HRESULT CandidateElement::IsShown(BOOL *shown) {
    if (!shown) return E_INVALIDARG;
    *shown = owner && owner->candidateElement == this && owner->candidateShown;
    return S_OK;
}
HRESULT CandidateElement::GetDocumentMgr(ITfDocumentMgr **manager) {
    if (!manager) return E_INVALIDARG;
    *manager = nullptr;
    if (!owner || !owner->activeContext) return E_FAIL;
    return owner->activeContext->GetDocumentMgr(manager);
}
HRESULT CandidateElement::GetCount(UINT *count) {
    if (!count) return E_INVALIDARG;
    *count = owner ? static_cast<UINT>(owner->candidateCount()) : 0;
    return S_OK;
}
HRESULT CandidateElement::GetSelection(UINT *index) {
    if (!index) return E_INVALIDARG;
    *index = owner ? static_cast<UINT>(owner->page * pageSize + owner->highlighted) : 0;
    return S_OK;
}
HRESULT CandidateElement::GetString(UINT index, BSTR *text) {
    if (!text) return E_INVALIDARG;
    *text = nullptr;
    if (!owner || index >= owner->candidateCount()) return E_INVALIDARG;
    try {
        *text = SysAllocString(owner->candidateText(index).c_str());
    } catch (...) { return E_OUTOFMEMORY; }
    return *text ? S_OK : E_OUTOFMEMORY;
}
HRESULT CandidateElement::GetPageIndex(UINT *index, UINT size, UINT *pages) {
    if (!pages) return E_INVALIDARG;
    const size_t count = owner ? owner->candidateCount() : 0;
    const UINT total = static_cast<UINT>(count ? (count + pageSize - 1) / pageSize : 1);
    *pages = total;
    if (!index) return S_OK;
    for (UINT i = 0; i < size && i < total; ++i) index[i] = static_cast<UINT>(i * pageSize);
    return S_OK;
}
HRESULT CandidateElement::GetCurrentPage(UINT *page) {
    if (!page) return E_INVALIDARG;
    *page = owner ? static_cast<UINT>(owner->page) : 0;
    return S_OK;
}
HRESULT CandidateElement::SetSelection(UINT index) {
    if (!owner || index >= owner->candidateCount()) return E_INVALIDARG;
    owner->page = index / pageSize;
    owner->highlighted = index % pageSize;
    try { owner->updatePopup(); } catch (...) {}
    return S_OK;
}
// Like Enter and Esc in the candidate window.
HRESULT CandidateElement::Finalize() {
    if (!owner || !owner->activeContext) return E_FAIL;
    BOOL eaten = FALSE;
    return owner->OnKeyDown(owner->activeContext, VK_RETURN, 0, &eaten);
}
HRESULT CandidateElement::Abort() {
    if (!owner || !owner->activeContext) return E_FAIL;
    BOOL eaten = FALSE;
    return owner->OnKeyDown(owner->activeContext, VK_ESCAPE, 0, &eaten);
}

HRESULT ModeButton::GetTooltipString(BSTR *tip) {
    *tip = SysAllocString(owner ? owner->modeTooltip().c_str() : L"晴词");
    return *tip ? S_OK : E_OUTOFMEMORY;
}
HRESULT ModeButton::GetText(BSTR *text) {
    *text = SysAllocString(!owner || owner->chineseMode ? L"中" : L"英");
    return *text ? S_OK : E_OUTOFMEMORY;
}
HRESULT ModeButton::GetIcon(HICON *icon) {
    *icon = modeIcon(!owner || owner->chineseMode ? L"中" : L"英");
    return *icon ? S_OK : E_FAIL;
}
HRESULT ModeButton::OnClick(TfLBIClick click, POINT, const RECT *) {
    if (owner && click == TF_LBI_CLK_LEFT) {
        try { owner->setMode(!owner->chineseMode); } catch (...) {}
    }
    return S_OK;
}

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
    // CN-07 batch 2: lets Windows show the 中／英 item in the taskbar.
    if (install && SUCCEEDED(hr)) hr = categories->RegisterCategory(serviceId, systrayCapability, serviceId);
    else if (!install) categories->UnregisterCategory(serviceId, systrayCapability, serviceId);
    // WIN-08: the candidate list is offered as a UI element (games draw it).
    if (install && SUCCEEDED(hr)) hr = categories->RegisterCategory(serviceId, uiElementCapability, serviceId);
    else if (!install) categories->UnregisterCategory(serviceId, uiElementCapability, serviceId);
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
