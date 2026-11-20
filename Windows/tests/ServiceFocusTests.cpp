// Exercise the actual TSF callbacks without registering or installing an IME.
#include "../TextService.cpp"
#include <ctffunc.h>
#include <cassert>
#include <iostream>

namespace {
class RecordingContext;
// WIN-08: a range and a composition just rich enough for the service's inline
// reading. Heap objects, deleted at their last release.
class FakeRange final : public ITfRange {
public:
    FakeRange(RecordingContext *context, bool composition) : context(context), composition(composition) {}
    ULONG refs = 1;
    RecordingContext *context;
    bool composition;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_ITfRange) return E_NOINTERFACE;
        *out = static_cast<ITfRange *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG left = --refs; if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD, WCHAR *, ULONG, ULONG *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR *value, LONG length) override;
    HRESULT STDMETHODCALLTYPE GetFormattedText(TfEditCookie, IDataObject **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(TfEditCookie, DWORD, IDataObject *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG, LONG *, const TF_HALTCOND *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, LONG, LONG *, const TF_HALTCOND *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStartToRange(TfEditCookie, ITfRange *, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEndToRange(TfEditCookie, ITfRange *, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie, TfAnchor) override { composition = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE IsEqualStart(TfEditCookie, ITfRange *, TfAnchor, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEqualEnd(TfEditCookie, ITfRange *, TfAnchor, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange *, TfAnchor, LONG *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange *, TfAnchor, LONG *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdjustForInsert(TfEditCookie, ULONG, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGravity(TfGravity *, TfGravity *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(ITfRange **clone) override { *clone = new FakeRange(context, false); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContext(ITfContext **) override { return E_NOTIMPL; }
};

class FakeComposition final : public ITfComposition {
public:
    explicit FakeComposition(RecordingContext *context) : context(context) {}
    ULONG refs = 1;
    RecordingContext *context;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_ITfComposition) return E_NOINTERFACE;
        *out = static_cast<ITfComposition *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG left = --refs; if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE GetRange(ITfRange **range) override { *range = new FakeRange(context, true); return S_OK; }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, ITfRange *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, ITfRange *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndComposition(TfEditCookie) override;
};

// A writable host context that records what the service inserts. Edit
// sessions run synchronously; there is no compartment, so input is enabled.
// With composable set it also accepts a composition (WIN-08): its text is
// `reading` while `composing`.
class RecordingContext final : public ITfContext, public ITfInsertAtSelection, public ITfContextComposition {
public:
    ULONG refs = 1;
    std::wstring text;
    bool composable = false, composing = false;
    std::wstring reading;
    unsigned compositions = 0, compositionsEnded = 0;
    ITfCompositionSink *sink = nullptr;
    ITfComposition *current = nullptr; // not owned
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_ITfContext) *out = static_cast<ITfContext *>(this);
        else if (id == IID_ITfInsertAtSelection) *out = static_cast<ITfInsertAtSelection *>(this);
        else if (id == IID_ITfContextComposition && composable) *out = static_cast<ITfContextComposition *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { assert(refs); return --refs; }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(TfEditCookie, DWORD flags, const WCHAR *value, LONG length, ITfRange **range) override {
        if (flags & TF_IAS_QUERYONLY) {
            *range = new FakeRange(this, false);
            return S_OK;
        }
        assert(!composing); // committed text never lands inside a reading
        text.append(value, length);
        *range = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartComposition(TfEditCookie, ITfRange *, ITfCompositionSink *compositionSink, ITfComposition **out) override {
        assert(!composing);
        auto *started = new FakeComposition(this);
        composing = true;
        reading.clear();
        sink = compositionSink;
        current = started;
        ++compositions;
        *out = started;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumCompositions(IEnumITfCompositionView **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindComposition(TfEditCookie, ITfRange *, IEnumITfCompositionView **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TakeOwnership(TfEditCookie, ITfCompositionView *, ITfCompositionSink *, ITfComposition **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(TfEditCookie, DWORD, IDataObject *, ITfRange **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId, ITfEditSession *edit, DWORD, HRESULT *out) override {
        *out = edit->DoEditSession(1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION *, ULONG *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie, ULONG, const TF_SELECTION *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie, ITfRange **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie, ITfRange **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumViews(IEnumTfContextViews **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS *status) override { *status = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, ITfProperty **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAppProperty(REFGUID, ITfReadOnlyProperty **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TrackProperties(const GUID **, ULONG, const GUID **, ULONG, ITfReadOnlyProperty **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumProperties(IEnumTfProperties **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRangeBackup(TfEditCookie, ITfRange *, ITfRangeBackup **) override { return E_NOTIMPL; }
};

HRESULT FakeRange::SetText(TfEditCookie, DWORD, const WCHAR *value, LONG length) {
    if (!composition || !context->composing) return E_FAIL;
    context->reading.assign(value, length);
    return S_OK;
}
HRESULT FakeComposition::EndComposition(TfEditCookie) {
    assert(context->composing && context->current == this);
    context->composing = false;
    context->current = nullptr;
    ++context->compositionsEnded;
    return S_OK;
}

// WIN-08: the thread's keyboard open/close compartment, which the host
// (a game) may set too. Notifies synchronously, as TSF does.
class FakeCompartment final : public ITfCompartment, public ITfSource {
public:
    ULONG refs = 1;
    VARIANT value{};
    unsigned writes = 0;
    ITfCompartmentEventSink *sink = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_ITfCompartment) *out = static_cast<ITfCompartment *>(this);
        else if (id == IID_ITfSource) *out = static_cast<ITfSource *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { assert(refs); return --refs; }
    HRESULT STDMETHODCALLTYPE SetValue(TfClientId, const VARIANT *next) override {
        value = *next;
        ++writes;
        if (sink) sink->OnChange(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(VARIANT *out) override { *out = value; return value.vt == VT_EMPTY ? S_FALSE : S_OK; }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID id, IUnknown *unknown, DWORD *cookie) override {
        if (id != IID_ITfCompartmentEventSink || sink) return E_FAIL;
        if (FAILED(unknown->QueryInterface(IID_ITfCompartmentEventSink, reinterpret_cast<void **>(&sink)))) return E_FAIL;
        *cookie = 7;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
        if (cookie != 7 || !sink) return E_FAIL;
        sink->Release();
        sink = nullptr;
        return S_OK;
    }
    bool open() const { return value.vt == VT_I4 && value.lVal != 0; }
    // What ImmSetOpenStatus or a Ctrl+Space does.
    void host(bool next) {
        VARIANT changed{};
        changed.vt = VT_I4;
        changed.lVal = next ? 1 : 0;
        SetValue(99, &changed);
    }
};

// WIN-08: a thread manager offering the open/close compartment and, unless
// switched off, a UI element manager that answers `show` (a game that draws
// the candidates itself answers FALSE).
class FakeThreadManager final : public ITfThreadMgr, public ITfCompartmentMgr, public ITfUIElementMgr {
public:
    ULONG refs = 1;
    FakeCompartment openClose;
    bool uiElements = true;
    BOOL show = TRUE;
    ITfUIElement *element = nullptr;
    DWORD id = TF_INVALID_UIELEMENTID;
    unsigned begun = 0, updates = 0, ended = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfThreadMgr) *out = static_cast<ITfThreadMgr *>(this);
        else if (iid == IID_ITfCompartmentMgr) *out = static_cast<ITfCompartmentMgr *>(this);
        else if (iid == IID_ITfUIElementMgr && uiElements) *out = static_cast<ITfUIElementMgr *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { assert(refs); return --refs; }
    HRESULT STDMETHODCALLTYPE Activate(TfClientId *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Deactivate() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AssociateFocus(HWND, ITfDocumentMgr *, ITfDocumentMgr **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID, ITfFunctionProvider **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFunctionProviders(IEnumTfFunctionProviders **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCompartment(REFGUID guid, ITfCompartment **out) override {
        *out = nullptr;
        if (!IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) return E_NOTIMPL;
        openClose.AddRef();
        *out = &openClose;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ClearCompartment(TfClientId, REFGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumCompartments(IEnumGUID **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement *next, BOOL *shown, DWORD *out) override {
        assert(!element && *shown); // one list at a time; the TIP asks to show
        element = next;
        element->AddRef();
        id = *out = 40 + ++begun;
        *shown = show;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD updated) override {
        assert(element && updated == id);
        ++updates;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD ending) override {
        assert(element && ending == id);
        element->Release();
        element = nullptr;
        ++ended;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUIElement(DWORD, ITfUIElement **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements **) override { return E_NOTIMPL; }
};

// Holds modifier keys in this thread's keyboard state, as GetKeyState sees them.
static void hold(std::initializer_list<int> keys) {
    BYTE state[256]{};
    for (int key : keys) state[key] = 0x80;
    assert(SetKeyboardState(state));
}

struct ServiceFocusTest {
    static void run() {
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\nhao\thao3\t好\t好\t9000\nni\tni3\t你\t你\t9000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        auto *saved = mpn_session_create(service.lexicon);
        service.suspended.emplace(nullptr, Service::SavedContext{saved});
        const auto prepare = [&] {
            mpn_clear(service.session);
            for (char c : std::string("ni")) assert(mpn_key(service.session, c));
            assert(mpn_choose(service.session, 0)); // Accepted text must survive cancellation.
            for (char c : std::string("hao")) assert(mpn_key(service.session, c));
            mpn_set_segment_context(service.session, "你");
            service.choices = {{"好", 0}};
            service.page = service.highlighted = 2;
            service.expanded = service.chineseLocked = true;
            mpn_clear(saved);
            for (char c : std::string("ni")) assert(mpn_key(saved, c));
            auto &state = service.suspended.at(nullptr);
            state.choices = {{"你", 0}}; state.page = 1; state.expanded = true;
        };
        const auto check = [&] {
            assert(!*mpn_input(service.session) && !*mpn_preedit(service.session));
            assert(!*mpn_segment_context(service.session) && !mpn_candidate_count(service.session));
            assert(std::string(mpn_committed(service.session)) == "你");
            assert(service.choices.empty() && !service.page && !service.highlighted);
            assert(!service.expanded && !service.chineseLocked);
            assert(!*mpn_input(saved) && service.suspended.at(nullptr).choices.empty());
            service.OnSetThreadFocus(); service.OnSetFocus(TRUE);
            assert(!*mpn_input(service.session));
            assert(mpn_key(service.session, 'n') && std::string(mpn_input(service.session)) == "n");
        };
        prepare(); service.OnSetFocus(TRUE); assert(std::string(mpn_input(service.session)) == "hao");
        service.OnSetFocus(FALSE); check();
        prepare(); service.OnKillThreadFocus(); check();
        prepare(); service.OnSetFocus(reinterpret_cast<ITfDocumentMgr *>(1), nullptr); check();
        prepare(); service.OnPushContext(nullptr); check();
        service.suspended.erase(nullptr); mpn_session_destroy(saved);
    }

    // CN-06 through the production selection path: what was chosen once is
    // first the next time, and a reading completed in segments is remembered
    // as a whole. No TSF objects are needed for candidate handling.
    static void memory(const std::filesystem::path &folder) {
        const auto path = folder / (L"service-memory-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        std::filesystem::remove(path);
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "hao\thao3\t好\t好\t9000\nhao\thao4\t号\t号\t8000\n"
            "ni\tni3\t你\t你\t9000\nni\tni2\t泥\t泥\t5000\n"
            "nihao\tni3hao3\t你好\t你好\t9000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        service.memory = std::make_unique<selection_memory::Store>(path);
        service.memory->poll();
        service.memoryEnabled = true;
        const auto type = [&](const std::string &keys) {
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
        };
        const auto find = [&](const std::string &text) {
            // Leading words and characters may only appear on a later page.
            for (int pass = 0; pass < 2; ++pass) {
                for (size_t i = 0; i < service.choices.size(); ++i) if (service.choices[i].text == text) return i;
                if (!pass) service.expandCandidates();
            }
            return service.choices.size();
        };
        const auto committed = [&](std::wstring text) {
            text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
            return text;
        };
        std::wstring text;
        // A different homophone, chosen once, leads next time.
        type("hao");
        assert(service.choices.front().text == "好" && find("号") > 0 && find("号") < service.choices.size());
        assert(service.chooseCandidate(find("号"), text) && committed(text) == L"号");
        type("hao");
        assert(service.choices.front().text == "号" && service.choices.front().coreIndex && find("好") == 1);
        // Choosing the original again restores it: equal counts, latest first.
        text.clear();
        assert(service.chooseCandidate(find("好"), text));
        type("hao");
        assert(service.choices.front().text == "好");
        // Segment by segment: 泥 then 号. The phrase is then offered whole.
        type("nihao");
        assert(service.choices.front().text == "你好" && find("泥号") > 0);
        text.clear();
        if (find("泥") == service.choices.size()) service.expandCandidates(); // Leading characters follow on the next page.
        const auto mud = find("泥");
        assert(mud < service.choices.size() && service.chooseCandidate(mud, text));
        assert(std::string(mpn_input(service.session)) == "hao" && service.composedSegments == 1);
        service.refreshCandidates();
        assert(service.chooseCandidate(find("号"), text) && committed(text) == L"泥号" && !service.composedSegments);
        type("nihao");
        assert(service.choices.front().text == "泥号" && find("你好") == 1);
        text.clear();
        assert(service.chooseCandidate(0, text) && committed(text) == L"泥号" && !*mpn_input(service.session));
        // A cancelled composition leaves no phrase behind.
        type("nihao");
        text.clear();
        assert(service.chooseCandidate(find("你"), text) && service.composedSegments == 1);
        service.cancelForFocusChange();
        assert(!service.composedSegments && service.composedReading.empty());
        mpn_clear_commit(service.session);
        // Automatic completion after punctuation learns only the confirmed first choice.
        type("nihao");
        text.clear();
        service.finishCandidateText(text, find("你"));
        assert(text == L"你号" && !service.composedSegments); // 号 leads hao by now; that completion is not learned.
        type("nihao");
        assert(service.choices.front().text == "泥号" && find("你好") == 1);
        // Switched off: nothing is applied and nothing is recorded.
        const auto before = service.memory->recall("hao", true);
        assert(before.size() == 2 && before.front() == "号");
        service.memoryEnabled = false;
        type("hao");
        assert(service.choices.front().text == "好");
        text.clear();
        assert(service.chooseCandidate(0, text));
        service.memoryEnabled = true;
        assert(service.memory->recall("hao", true) == before);
        type("hao");
        assert(service.choices.front().text == "号");
        // The journal survives the service and is shared with the next one.
        service.memory->flush();
        selection_memory::Store other(path); other.poll();
        assert(other.recall("nihao", true).front() == "泥号" && other.recall("hao", true) == before);
        mpn_clear(service.session); service.resetCandidates();
        const auto words = service.memory->wordsFile();
        service.memory.reset(); std::filesystem::remove(path); std::filesystem::remove(words);
    }

    // CN-06c through the production selection path: a word chosen as one
    // segment of a sentence leads in another sentence and on its own, and
    // switching memory off restores the original lists exactly.
    static void personalWords(const std::filesystem::path &folder) {
        const auto path = folder / (L"service-words-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "da\tda4\t大\t大\t8000\n"
            "gong\tgong1\t工\t工\t3000\n"
            "gongsi\tgong1si1\t攻絲\t攻丝\t6000\ngongsi\tgong1si1\t公司\t公司\t5800\n"
            "hen\then3\t很\t很\t8000\n"
            "si\tsi1\t絲\t丝\t3000\n"
            "wo\two3\t我\t我\t9000\n"
            "zai\tzai4\t在\t在\t9000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        service.memory = std::make_unique<selection_memory::Store>(path);
        std::filesystem::remove(service.memory->wordsFile());
        service.memory->poll();
        // As OnKeyDown does: read the switch, then attach or detach the table.
        const auto type = [&](const std::string &keys, bool enabled = true) {
            service.memoryEnabled = enabled;
            service.attachPersonal();
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
            std::vector<std::string> texts;
            for (const auto &choice : service.choices) texts.push_back(choice.text);
            return texts;
        };
        const auto find = [&](const std::string &text) {
            // Leading words and characters may only appear on later pages.
            for (int pass = 0; pass < 4; ++pass) {
                for (size_t i = 0; i < service.choices.size(); ++i) if (service.choices[i].text == text) return i;
                service.expandCandidates();
            }
            return service.choices.size();
        };
        // Nothing learned yet: the lists are those of a service without memory.
        const auto single = type("gongsi", false), sentence = type("gongsihenda", false), start = type("wozaigongsi", false);
        assert(single.front() == "攻丝" && sentence.front() == "攻丝很大" && start.front() == "我在攻丝");
        assert(type("gongsi") == single && type("gongsihenda") == sentence && type("wozaigongsi") == start);
        // 我, 在, then 公司, one segment at a time.
        type("wozaigongsi");
        std::wstring text;
        for (const char *segment : {"我", "在", "公司"}) {
            const auto index = find(segment);
            assert(index < service.choices.size() && service.chooseCandidate(index, text));
            service.refreshCandidates();
        }
        text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
        assert(text == L"我在公司" && !*mpn_input(service.session));
        assert(service.memory->words().bonus("gongsi", "公司", true, selection_memory::currentHour()) > 0);
        // Another sentence and the word alone: the core itself now ranks 公司 first.
        assert(type("gongsihenda").front() == "公司很大" && std::string(mpn_candidate(service.session, 0)) == "公司很大");
        assert(type("gongsi").front() == "公司" && std::string(mpn_candidate(service.session, 0)) == "公司");
        // Switched off: exactly the original lists; on again: learned again.
        assert(type("gongsi", false) == single && type("gongsihenda", false) == sentence && type("wozaigongsi", false) == start);
        assert(type("gongsihenda").front() == "公司很大");
        // Whole-sentence selections are learned word by word too; one
        // correction undoes one slip.
        type("gongsihenda");
        text.clear();
        assert(service.chooseCandidate(find("攻丝很大"), text));
        assert(type("gongsihenda").front() == "攻丝很大" && std::string(mpn_candidate(service.session, 0)) == "攻丝很大");
        type("gongsihenda"); text.clear();
        assert(service.chooseCandidate(find("公司很大"), text));
        assert(type("gongsihenda").front() == "公司很大" && std::string(mpn_candidate(service.session, 0)) == "公司很大");
        assert(type("gongsi").front() == "公司" && std::string(mpn_candidate(service.session, 0)) == "公司");
        // The words reach the journal and the next service.
        service.memory->flush();
        selection_memory::Store other(path); other.poll();
        assert(other.words().bonus("gongsi", "公司", true, selection_memory::currentHour()) >
               other.words().bonus("gongsi", "攻丝", true, selection_memory::currentHour()));
        // Clearing the journal clears the bonuses at the next look.
        const auto words = service.memory->wordsFile();
        std::filesystem::resize_file(words, 0); std::filesystem::resize_file(path, 0);
        service.memory->poll(GetTickCount64() + 10000);
        assert(type("gongsi") == single && type("gongsihenda") == sentence);
        mpn_clear(service.session); service.resetCandidates();
        service.memory.reset(); std::filesystem::remove(path); std::filesystem::remove(words);
    }

    // DICT-02b through the service: a word added to the user's table reaches
    // the core at the next key after the table is looked at, with or without
    // selection memory; edits, the other script and deletion follow it.
    static void userWords(const std::filesystem::path &folder) {
        const auto path = folder / (L"service-user-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "san\tsan1\t三\t三\t8000\n"
            "wo\two3\t我\t我\t9000\n"
            "zhang\tzhang1\t張\t张\t8000\n"
            "zhao\tzhao3\t找\t找\t8000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        service.memory = std::make_unique<selection_memory::Store>(path);
        const auto table = service.memory->userWordsFile();
        std::filesystem::remove(table); std::filesystem::remove(service.memory->wordsFile());
        ULONGLONG now = GetTickCount64();
        // As OnKeyDown does: read the switch, look at the table, attach.
        const auto type = [&](const std::string &keys, bool enabled = true) {
            service.memoryEnabled = enabled;
            if (enabled) service.memory->poll(now);
            service.memory->pollUserWords(now += 5000);
            service.attachPersonal();
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
            std::vector<std::string> texts;
            for (const auto &choice : service.choices) texts.push_back(choice.text);
            return texts;
        };
        const auto publish = [&](const std::string &rows) {
            const auto temporary = folder / (L"service-user-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << rows; }
            assert(MoveFileExW(temporary.c_str(), table.c_str(), MOVEFILE_REPLACE_EXISTING));
        };
        const auto plain = type("zhangsan", false), sentence = type("wozhaozhangsan", false);
        assert(plain.front() == "张三" && sentence.front() == "我找张三");
        assert(type("zhangsan") == plain);
        publish("# 个人词\nzhang'san\t张珊\n");
        assert(type("zhangsan").front() == "张珊" && type("wozhaozhangsan").front() == "我找张珊");
        // Independent of the learning switch.
        assert(type("zhangsan", false).front() == "张珊" && type("wozhaozhangsan", false).front() == "我找张珊");
        // Choosing it commits it; with memory on it is also remembered as usual.
        type("zhangsan");
        std::wstring text;
        assert(service.chooseCandidate(0, text));
        text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
        assert(text == L"张珊" && !*mpn_input(service.session));
        // Revised: the new text replaces the old one at the next look.
        publish("zhang'san\t章珊\t章珊\n");
        auto list = type("zhangsan", false);
        assert(list.front() == "章珊" && std::find(list.begin(), list.end(), "张珊") == list.end());
        // Traditional output takes the given traditional text, or converts.
        service.simplified = false; mpn_set_simplified(service.session, 0);
        assert(type("zhangsan", false).front() == "章珊");
        publish("zhang'san\t张珊\n");
        assert(type("zhangsan", false).front() == "張珊");
        service.simplified = true; mpn_set_simplified(service.session, 1);
        // Removed: the original lists again, with and without memory.
        publish("");
        assert(type("zhangsan", false) == plain && type("wozhaozhangsan", false) == sentence);
        publish("zhang'san\t张珊\n");
        assert(type("zhangsan").front() == "张珊");
        std::filesystem::remove(table);
        assert(type("zhangsan", false) == plain);
        mpn_clear(service.session); service.resetCandidates();
        const auto words = service.memory->wordsFile();
        service.memory.reset(); std::filesystem::remove(path); std::filesystem::remove(words);
    }

    // CN-03 through the service: the pairs read from the user's settings are
    // applied to the active session before a key, rebuild an open candidate
    // list, reach a session created later, and switched off give back the
    // original list exactly.
    static void fuzzyPinyin() {
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "guo\tguo2\t國\t国\t8000\n"
            "zhong\tzhong1\t中\t中\t9000\n"
            "zhongguo\tzhong1guo2\t中國\t中国\t10000\n"
            "zong\tzong3\t總\t总\t7000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        const auto texts = [&] {
            std::vector<std::string> result;
            for (const auto &choice : service.choices) result.push_back(choice.text);
            return result;
        };
        const auto type = [&](const std::string &keys) {
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
            return texts();
        };
        service.applyFuzzy(0);
        const auto plain = type("zongguo");
        assert(!plain.empty() && plain.front() != "中国");
        // Turned on while the composition is open: the list is rebuilt at once.
        service.applyFuzzy(MPN_FUZZY_Z_ZH);
        assert(texts().front() == "中国" && std::string(mpn_input(service.session)) == "zongguo");
        assert(type("zongguo").front() == "中国");
        std::wstring text;
        assert(service.chooseCandidate(0, text));
        text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
        assert(text == L"中国" && !*mpn_input(service.session));
        // A fresh session (another context or a dictionary reload) starts
        // without pairs and receives them with its first key.
        mpn_session_destroy(service.session);
        service.session = mpn_session_create(service.lexicon);
        mpn_set_simplified(service.session, service.simplified);
        assert(!mpn_fuzzy_pinyin(service.session));
        service.applyFuzzy(MPN_FUZZY_Z_ZH);
        assert(mpn_fuzzy_pinyin(service.session) == MPN_FUZZY_Z_ZH && type("zongguo").front() == "中国");
        // Off again: exactly the original list.
        service.applyFuzzy(0);
        assert(texts() == plain && type("zongguo") == plain);
        mpn_clear(service.session); service.resetCandidates();
    }

    // CN-05 through the service: the switch read from the user's settings is
    // applied before a key, rebuilds an open candidate list and reaches a
    // session created later.
    static void typoCorrection() {
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "guo\tguo2\t國\t国\t8000\n"
            "zhong\tzhong1\t中\t中\t9000\n"
            "zhongguo\tzhong1guo2\t中國\t中国\t10000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        const auto texts = [&] {
            std::vector<std::string> result;
            for (const auto &choice : service.choices) result.push_back(choice.text);
            return result;
        };
        const auto type = [&](const std::string &keys) {
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
            return texts();
        };
        // On by default: the slip is read as 中国 and commits as such.
        service.applyTypo(true);
        assert(type("zhonguo").front() == "中国");
        // The candidate window labels it on the reading line; the label is
        // display only. An exact candidate carries no label.
        assert(service.candidateReading(0) == L"纠错" && service.candidateText(0) == L"中国");
        type("zhongguo");
        for (size_t i = 0; i < service.choices.size(); ++i)
            assert(service.candidateReading(i).find(L"纠错") == std::wstring::npos);
        assert(type("zhonguo").front() == "中国");
        std::wstring text;
        assert(service.chooseCandidate(0, text));
        text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
        assert(text == L"中国" && !*mpn_input(service.session));
        // Turned off while the composition is open: rebuilt at once, without it.
        const auto on = type("zhonguo");
        service.applyTypo(false);
        assert(std::string(mpn_input(service.session)) == "zhonguo" && texts() != on);
        for (size_t i = 0; i < mpn_candidate_count(service.session); ++i) assert(!mpn_candidate_typo(service.session, i));
        for (size_t i = 0; i < service.choices.size(); ++i)
            assert(service.candidateReading(i).find(L"纠错") == std::wstring::npos);
        const auto off = type("zhonguo");
        // A fresh session starts on and receives the switch with its first key.
        mpn_session_destroy(service.session);
        service.session = mpn_session_create(service.lexicon);
        mpn_set_simplified(service.session, service.simplified);
        assert(mpn_typo_correction(service.session));
        service.applyTypo(false);
        assert(!mpn_typo_correction(service.session) && type("zhonguo") == off);
        service.applyTypo(true);
        assert(texts() == on);
        mpn_clear(service.session); service.resetCandidates();
    }

    // CN-07 through the real key callbacks: which keys the service takes,
    // what reaches the host, and Ctrl+. with its persisted state and switch.
    static void punctuation() {
        const std::wstring path = L"Software\\HarukotoPunctuationService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root;
            const std::string data = "MEOWPUUT-PINYIN-1\nhao\thao3\t好\t好\t9000\nni\tni3\t你\t你\t9000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            const auto test = [&](WPARAM key) {
                BOOL eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &eaten) == S_OK);
                return eaten != FALSE;
            };
            // Returns whether the key was eaten and what reached the host.
            const auto press = [&](WPARAM key, LPARAM flags = 0) {
                const bool tested = test(key);
                context.text.clear();
                BOOL eaten = FALSE;
                assert(service.OnKeyDown(&context, key, flags, &eaten) == S_OK);
                assert(tested == (eaten != FALSE));
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto compose = [&](const char *keys) {
                for (const char *c = keys; *c; ++c) {
                    assert(mpn_key(service.session, *c));
                }
                service.refreshCandidates();
            };
            using Result = std::pair<bool, std::wstring>;
            const Result passed{false, L""};
            hold({});
            // The first key makes this the active context.
            assert(press(VK_OEM_COMMA) == Result(true, L"，"));
            // Chinese punctuation, idle.
            assert(press(VK_OEM_PERIOD) == Result(true, L"。"));
            assert(press(VK_OEM_5) == Result(true, L"、"));
            assert(press(VK_OEM_4) == Result(true, L"「"));
            assert(press(VK_OEM_2) == passed); // / reaches the host as itself (FF14 commands)
            assert(press(VK_OEM_7) == Result(true, L"‘"));
            assert(press(VK_OEM_7) == Result(true, L"’"));
            // Composing: the highlighted candidate first, then the mark.
            compose("ni");
            assert(press(VK_OEM_COMMA) == Result(true, L"你，"));
            compose("hao");
            assert(press(VK_OEM_2) == Result(true, L"好/"));
            compose("ni");
            // ' stays the syllable separator (pinyinMarkKind leaves it to the
            // reading). Only queried: an open reading would show a popup.
            assert(test(VK_OEM_7));
            mpn_clear(service.session); service.resetCandidates();
            // Shift layer.
            hold({VK_SHIFT});
            assert(press('1') == Result(true, L"！"));
            assert(press('9') == Result(true, L"（"));
            assert(press('0') == Result(true, L"）"));
            assert(press('6') == Result(true, L"……"));
            assert(press(VK_OEM_MINUS) == Result(true, L"——"));
            assert(press(VK_OEM_1) == Result(true, L"："));
            assert(press(VK_OEM_2) == Result(true, L"？"));
            assert(press(VK_OEM_7) == Result(true, L"“"));
            assert(press(VK_OEM_7) == Result(true, L"”"));
            for (WPARAM key : {WPARAM('2'), WPARAM('A'), WPARAM(VK_OEM_PLUS), WPARAM(VK_OEM_5)})
                assert(press(key) == passed); // unmapped Shift keys and Shift+letters
            hold({});
            compose("hao");
            hold({VK_SHIFT});
            assert(press(VK_OEM_PERIOD) == Result(true, L"好》"));
            assert(press('1') == Result(true, L"！"));
            // Caps Lock still leaves every key to the host.
            BYTE state[256]{}; state[VK_SHIFT] = 0x80; state[VK_CAPITAL] = 1; assert(SetKeyboardState(state));
            assert(press('1') == passed);
            // Ctrl+. toggles, once per press, and persists the mode.
            hold({VK_CONTROL});
            assert(press(VK_OEM_PERIOD) == Result(true, L"") && readEnglishPunctuation(root) && service.englishPunctuation);
            assert(press(VK_OEM_PERIOD, LPARAM(1) << 30) == Result(true, L"") && readEnglishPunctuation(root));
            assert(press(VK_OEM_COMMA) == passed); // other Ctrl keys are the host's
            hold({});
            // English mode: idle punctuation is the host's, composing types it literally.
            for (WPARAM key : {WPARAM(VK_OEM_COMMA), WPARAM(VK_OEM_PERIOD), WPARAM(VK_OEM_2), WPARAM(VK_OEM_4), WPARAM(VK_OEM_7)})
                assert(press(key) == passed);
            compose("ni");
            assert(press(VK_OEM_PERIOD) == Result(true, L"你."));
            compose("hao");
            assert(press(VK_OEM_5) == Result(true, L"好\\"));
            hold({VK_SHIFT});
            assert(press('1') == passed && press(VK_OEM_7) == passed);
            hold({});
            // Behind a pending insertion punctuation is typed in order, not passed.
            mpn_clear(service.session); service.resetCandidates();
            service.delivery->queue.suspend();
            service.delivery->enqueue(L"待");
            assert(test(VK_OEM_COMMA) && test(VK_OEM_2));
            service.delivery->queue.resume();
            assert(press(VK_OEM_COMMA) == Result(true, L"待,"));
            // Back to Chinese.
            hold({VK_CONTROL});
            assert(press(VK_OEM_PERIOD) == Result(true, L"") && !readEnglishPunctuation(root));
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，"));
            // Switched off, Ctrl+. is the host's and the mode stays.
            DWORD off = 0;
            HKEY input = nullptr;
            assert(RegOpenKeyExW(root, L"Software\\Meowpuut\\Input", 0, KEY_SET_VALUE, &input) == ERROR_SUCCESS);
            assert(RegSetValueExW(input, L"PunctuationHotkey", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&off), sizeof(off)) == ERROR_SUCCESS);
            RegCloseKey(input);
            hold({VK_CONTROL});
            assert(press(VK_OEM_PERIOD) == passed && !readEnglishPunctuation(root));
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，"));
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    }

    // CN-07 batch 2 through the real key callbacks: Shift taps switch
    // Chinese/English, English passes everything to the host, switching
    // mid-composition commits the letters, and no Shift key reaches the host
    // ahead of a composition.
    static void inputMode() {
        const std::wstring path = L"Software\\HarukotoInputModeService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        const auto setting = [&](DWORD value) {
            HKEY input = nullptr;
            assert(RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &input, nullptr) == ERROR_SUCCESS);
            assert(RegSetValueExW(input, L"ShiftToggle", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
            RegCloseKey(input);
        };
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root;
            const std::string data = "MEOWPUUT-PINYIN-1\nhao\thao3\t好\t好\t9000\nni\tni3\t你\t你\t9000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            const auto test = [&](WPARAM key) {
                BOOL eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &eaten) == S_OK);
                return eaten != FALSE;
            };
            const auto press = [&](WPARAM key) {
                const bool tested = test(key);
                context.text.clear();
                BOOL eaten = FALSE;
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == (eaten != FALSE));
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto compose = [&](const char *keys) {
                for (const char *c = keys; *c; ++c) assert(mpn_key(service.session, *c));
                service.refreshCandidates();
            };
            // Shift down and up as the TSF callbacks see them; never eaten.
            const LPARAM left = LPARAM(0x2a) << 16, right = LPARAM(0x36) << 16, release = (LPARAM(1) << 30) | (LPARAM(1) << 31);
            const auto shiftDown = [&](LPARAM side = LPARAM(0x2a) << 16) {
                hold({VK_SHIFT});
                BOOL eaten = TRUE;
                assert(service.OnTestKeyDown(&context, VK_SHIFT, side | 1, &eaten) == S_OK && !eaten);
            };
            const auto shiftUp = [&](LPARAM side = LPARAM(0x2a) << 16) {
                hold({});
                BOOL eaten = TRUE;
                assert(service.OnTestKeyUp(&context, VK_SHIFT, side | release | 1, &eaten) == S_OK && !eaten);
                eaten = TRUE;
                assert(service.OnKeyUp(&context, VK_SHIFT, side | release | 1, &eaten) == S_OK && !eaten); // counted once
            };
            const auto tap = [&](LPARAM side = LPARAM(0x2a) << 16) { shiftDown(side); shiftUp(side); };
            using Result = std::pair<bool, std::wstring>;
            const Result passed{false, L""};
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，") && service.chineseMode);
            // Left and right taps toggle.
            tap(left); assert(!service.chineseMode);
            for (WPARAM key : {WPARAM('A'), WPARAM('1'), WPARAM(VK_SPACE), WPARAM(VK_OEM_COMMA), WPARAM(VK_OEM_2), WPARAM(VK_OEM_7), WPARAM(VK_RETURN)})
                assert(press(key) == passed);
            hold({VK_SHIFT});
            assert(press('1') == passed && press('A') == passed);
            hold({VK_CONTROL});
            assert(press(VK_OEM_PERIOD) == passed && !readEnglishPunctuation(root)); // Ctrl+. is the host's in English
            hold({});
            tap(right); assert(service.chineseMode);
            assert(press(VK_OEM_COMMA) == Result(true, L"，"));
            // Switching mid-composition commits the raw letters.
            compose("ni");
            context.text.clear();
            tap(); assert(!service.chineseMode && context.text == L"ni" && !*mpn_input(service.session) && service.choices.empty());
            tap(); assert(service.chineseMode);
            // English mode keeps typing behind an insertion still owed to the host.
            tap(); assert(!service.chineseMode);
            service.delivery->queue.suspend();
            service.delivery->enqueue(L"待");
            assert(test('A') && test(VK_SPACE) && !test(VK_RETURN) && !test(VK_LEFT));
            service.delivery->queue.resume();
            assert(press('A') == Result(true, L"待a"));
            tap(); assert(service.chineseMode);
            // The leak: a Shift key while composing no longer reaches the host first.
            compose("ni");
            hold({VK_SHIFT});
            assert(press('W') == Result(true, L"niW")); // raw letters and the capital, like Space
            hold({});
            compose("ni");
            hold({VK_SHIFT});
            assert(press('2') == Result(true, L"你@") && press('2') == passed); // idle: the host's, as before
            hold({});
            // Shift used as a modifier is not a tap.
            shiftDown();
            assert(press('1') == Result(true, L"！"));
            shiftUp(); assert(service.chineseMode);
            shiftDown();
            assert(press('A') == passed); // idle Shift+letter, the host's
            shiftUp(); assert(service.chineseMode);
            hold({VK_CONTROL});
            { BOOL eaten = FALSE; service.OnTestKeyDown(&context, VK_SHIFT, left | 1, &eaten); assert(!eaten); }
            shiftUp(); assert(service.chineseMode); // Ctrl+Shift
            // A click while Shift was held (Shift+click selects text).
            shiftDown();
            { BYTE state[256]{}; state[VK_SHIFT] = 0x80; state[VK_LBUTTON] = 1; assert(SetKeyboardState(state)); }
            { BOOL eaten = TRUE; service.OnTestKeyUp(&context, VK_SHIFT, left | release | 1, &eaten); assert(!eaten); }
            assert(service.chineseMode);
            hold({});
            // Settings: off, left only, either again.
            setting(shiftToggleOff);
            tap(left); tap(right); assert(service.chineseMode);
            setting(shiftToggleLeft);
            tap(right); assert(service.chineseMode);
            tap(left); assert(!service.chineseMode);
            tap(left); assert(service.chineseMode);
            setting(shiftToggleEither);
            tap(right); assert(!service.chineseMode);
            tap(right); assert(service.chineseMode);
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
#ifdef _MSC_VER
        // The taskbar item and capability GUIDs defined in TextService.cpp.
        assert(IsEqualGUID(inputModeItem, GUID_LBI_INPUTMODE));
        assert(IsEqualGUID(systrayCapability, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT));
#endif
        // The icon renders.
        HICON icon = modeIcon(L"中");
        assert(icon);
        DestroyIcon(icon);
    }

    // CN-07 batch 3 through the real key callbacks: - and = page while
    // composing, stop at the first and last page, and are the host's when
    // idle or in English mode.
    static void paging() {
        const std::wstring path = L"Software\\HarukotoPagingService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root; // Chinese punctuation regardless of this user's settings
            std::string data = "MEOWPUUT-PINYIN-1\n";
            const char *characters[] = {"一", "以", "已", "意", "义", "议", "易", "医", "依", "亿", "艺", "益", "异", "衣", "移", "遗"};
            int weight = 9000;
            for (const char *text : characters) data += std::string("yi\tyi4\t") + text + "\t" + text + "\t" + std::to_string(weight -= 100) + "\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            const auto press = [&](WPARAM key) {
                BOOL tested = FALSE, eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &tested) == S_OK);
                context.text.clear();
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == eaten);
                return std::make_pair(eaten != FALSE, context.text);
            };
            using Result = std::pair<bool, std::wstring>;
            const Result taken{true, L""}, passed{false, L""};
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，")); // makes the context active
            // Idle: the host's, as before.
            assert(press(VK_OEM_MINUS) == passed && press(VK_OEM_PLUS) == passed);
            const auto compose = [&] {
                assert(mpn_key(service.session, 'y') && mpn_key(service.session, 'i'));
                service.refreshCandidates();
                assert(service.choices.size() > pageSize && service.page == 0);
            };
            compose();
            // = forward until the last page, which it does not pass; the same as Page Down.
            size_t last = 0;
            for (int i = 0; i < 10; ++i) {
                service.highlighted = 1;
                assert(press(VK_OEM_PLUS) == taken && !service.highlighted);
                assert(service.page >= last && service.page * pageSize < service.candidateCount());
                last = service.page;
            }
            assert(last >= 1 && (last + 1) * pageSize >= service.candidateCount());
            assert(press(VK_OEM_PLUS) == taken && service.page == last);
            // - back to the first page, which it does not pass.
            assert(press(VK_OEM_MINUS) == taken && service.page == last - 1);
            while (service.page) assert(press(VK_OEM_MINUS) == taken);
            assert(press(VK_OEM_MINUS) == taken && service.page == 0 && std::string(mpn_input(service.session)) == "yi");
            // Matches Page Up / Page Down exactly.
            assert(press(VK_NEXT) == taken && service.page == 1 && press(VK_OEM_MINUS) == taken && service.page == 0);
            assert(press(VK_OEM_PLUS) == taken && service.page == 1 && press(VK_PRIOR) == taken && service.page == 0);
            // A selection on a later page still commits that page's candidate.
            assert(press(VK_OEM_PLUS) == taken);
            const std::wstring second = service.candidateText(pageSize);
            assert(press('1') == Result(true, second) && !*mpn_input(service.session));
            // Arrows past the last candidate of a page turn to the next page
            // and highlight its first item; before the first, back to the
            // previous page's last item; at the ends of the list they stay.
            compose();
            for (size_t i = 1; i < pageSize; ++i) assert(press(VK_DOWN) == taken && service.page == 0 && service.highlighted == i);
            assert(press(VK_DOWN) == taken && service.page == 1 && service.highlighted == 0);
            assert(press(VK_UP) == taken && service.page == 0 && service.highlighted == pageSize - 1);
            assert(press(VK_RIGHT) == taken && service.page == 1 && service.highlighted == 0);
            assert(press(VK_LEFT) == taken && press(VK_UP) == taken && service.page == 0 && service.highlighted == pageSize - 2);
            while (service.highlighted) assert(press(VK_LEFT) == taken);
            assert(press(VK_UP) == taken && service.page == 0 && service.highlighted == 0);
            assert(press(VK_TAB) == taken && service.highlighted == 1);
            while (true) {
                const size_t before = service.page * pageSize + service.highlighted;
                assert(press(VK_DOWN) == taken);
                if (service.page * pageSize + service.highlighted == before) break;
                assert(service.page * pageSize + service.highlighted == before + 1);
            }
            assert(service.page * pageSize + service.highlighted + 1 == service.candidateCount());
            assert(press(VK_RETURN) == Result(true, service.candidateText(service.candidateCount() - 1)));
            // Shift+- and Shift+= keep the punctuation rules (batch 1 and 2).
            compose();
            hold({VK_SHIFT});
            assert(press(VK_OEM_MINUS) == Result(true, L"一——"));
            hold({});
            compose();
            hold({VK_SHIFT});
            assert(press(VK_OEM_PLUS) == Result(true, L"一+"));
            assert(press(VK_OEM_PLUS) == passed); // idle Shift+= stays the host's
            hold({});
            // English mode: the host's.
            service.setMode(false);
            assert(press(VK_OEM_MINUS) == passed && press(VK_OEM_PLUS) == passed);
            service.setMode(true);
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    }

    // CN-07 batch 4 through the real key callbacks: [ and ] commit the first
    // and last character of the highlighted candidate, the rest of the
    // reading keeps composing, nothing is learned, and idle, English mode,
    // Shift and non-Chinese candidates keep their own behaviour.
    static void wordCharacters(const std::filesystem::path &folder) {
        const std::wstring path = L"Software\\HarukotoWordCharacterService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        const auto memoryPath = folder / (L"service-pick-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root;
            const std::string data = "MEOWPUUT-PINYIN-1\n"
                "guo\tguo2\t國\t国\t8000\n"
                "hehe\the1he1\t\xf0\xaa\x9a\xa5\xe9\xbe\x98\t\xf0\xaa\x9a\xa5\xe9\xbe\x98\t9000\n"
                "ren\tren2\t人\t人\t9000\n"
                "zhong\tzhong1\t中\t中\t9000\n"
                "zhongguo\tzhong1guo2\t中國\t中国\t10000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            mpn_set_simplified(service.session, service.simplified);
            std::filesystem::remove(memoryPath);
            service.memory = std::make_unique<selection_memory::Store>(memoryPath);
            std::filesystem::remove(service.memory->wordsFile());
            const auto press = [&](WPARAM key) {
                BOOL tested = FALSE, eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &tested) == S_OK);
                context.text.clear();
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == eaten);
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto compose = [&](const char *keys) {
                for (const char *c = keys; *c; ++c) assert(mpn_key(service.session, *c));
                service.refreshCandidates();
            };
            // Highlights a core candidate, on a later page if need be.
            const auto highlight = [&](const std::string &text) {
                for (int pass = 0; pass < 4; ++pass) {
                    for (size_t i = 0; i < service.choices.size(); ++i)
                        if (service.choices[i].text == text && service.choices[i].coreIndex) {
                            service.page = i / pageSize; service.highlighted = i % pageSize;
                            return true;
                        }
                    service.expandCandidates();
                }
                return false;
            };
            using Result = std::pair<bool, std::wstring>;
            hold({});
            // Idle: 「」 as before (this also makes the context active).
            assert(press(VK_OEM_4) == Result(true, L"「") && press(VK_OEM_6) == Result(true, L"」"));
            // First and last character of the highlighted word.
            compose("zhongguo");
            assert(service.choices.front().text == "中国");
            assert(press(VK_OEM_4) == Result(true, L"中") && !*mpn_input(service.session) && service.choices.empty());
            compose("zhongguo");
            assert(press(VK_OEM_6) == Result(true, L"国") && !*mpn_input(service.session));
            // The highlighted one, not the first.
            compose("zhongguo");
            assert(highlight("中"));
            assert(press(VK_OEM_6) == Result(true, L"中") && std::string(mpn_input(service.session)) == "guo");
            assert(service.chineseLocked && !service.choices.empty());
            assert(press('1') == Result(true, L"国") && !*mpn_input(service.session));
            // A word that covers part of the reading: the rest keeps composing.
            mpn_set_segment_context(service.session, nullptr); // a fresh document position
            compose("zhongguoren");
            assert(highlight("中国"));
            assert(press(VK_OEM_6) == Result(true, L"国") && std::string(mpn_input(service.session)) == "ren");
            const std::string ranked = mpn_segment_context(service.session); // ranked after the character
            assert(ranked == "國"); // not 中國, the whole word
            assert(press('1') == Result(true, L"人"));
            // Characters outside the BMP stay whole (a UTF-16 surrogate pair).
            compose("hehe");
            assert(press(VK_OEM_4) == Result(true, L"\U0002A6A5"));
            compose("hehe");
            assert(press(VK_OEM_6) == Result(true, L"龘"));
            // Nothing learned, with selection memory on.
            service.memoryEnabled = true;
            compose("zhongguo");
            assert(highlight("中"));
            assert(press(VK_OEM_4) == Result(true, L"中"));
            mpn_clear(service.session); service.resetCandidates();
            assert(service.memory->recall("zhongguo", true).empty() && service.memory->recall("zhong", true).empty());
            assert(service.memory->words().bonus("zhong", "中", true, selection_memory::currentHour()) == 0);
            assert(service.memory->words().bonus("zhongguo", "中国", true, selection_memory::currentHour()) == 0);
            // A Japanese (non-core) candidate highlighted: taken, nothing happens.
            compose("zhongguo");
            WindowsCandidate japanese{"中国語", std::nullopt};
            japanese.japanese = true;
            service.choices.insert(service.choices.begin(), japanese);
            service.highlighted = 0;
            assert(press(VK_OEM_4) == Result(true, L"") && press(VK_OEM_6) == Result(true, L""));
            assert(std::string(mpn_input(service.session)) == "zhongguo");
            mpn_clear(service.session); service.resetCandidates();
            // Shift+[ ] while composing: 『』 after the highlighted candidate, as in batch 1.
            compose("zhongguo");
            hold({VK_SHIFT});
            assert(press(VK_OEM_4) == Result(true, L"中国『"));
            assert(press(VK_OEM_6) == Result(true, L"』"));
            hold({});
            // English mode: the host's.
            service.setMode(false);
            assert(press(VK_OEM_4) == Result(false, L"") && press(VK_OEM_6) == Result(false, L""));
            service.setMode(true);
            const auto words = service.memory->wordsFile();
            service.memory.reset();
            std::filesystem::remove(memoryPath); std::filesystem::remove(words);
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    }

    // CN-07 batch 5 through the real key callbacks: Ctrl+Delete or
    // Shift+Delete forgets what was learned for the highlighted candidate,
    // rebuilds the list at once and journals the forget; a candidate with
    // nothing learned is left alone, a hand-added word is not deleted, and
    // the keys are the host's when idle or in English mode.
    static void forgetting(const std::filesystem::path &folder) {
        const std::wstring path = L"Software\\HarukotoForgetService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        const auto memoryPath = folder / (L"service-forget-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root; // selection memory on (absent switch), Chinese punctuation
            const std::string data = "MEOWPUUT-PINYIN-1\n"
                "hao\thao3\t好\t好\t9000\nhao\thao4\t号\t号\t8000\n"
                "san\tsan1\t三\t三\t8000\nzhang\tzhang1\t張\t张\t8000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            mpn_set_simplified(service.session, service.simplified);
            std::filesystem::remove(memoryPath);
            service.memory = std::make_unique<selection_memory::Store>(memoryPath);
            const auto words = service.memory->wordsFile(), table = service.memory->userWordsFile();
            std::filesystem::remove(words);
            const std::string tableRows = "zhang'san\t张珊\n";
            { std::ofstream out(table, std::ios::binary | std::ios::trunc); out << tableRows; }
            const auto read = [](const std::filesystem::path &file) {
                std::ifstream in(file, std::ios::binary);
                return std::string{std::istreambuf_iterator<char>(in), {}};
            };
            const auto press = [&](WPARAM key) {
                BOOL tested = FALSE, eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &tested) == S_OK);
                context.text.clear();
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == eaten);
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto compose = [&](const char *keys) {
                for (const char *c = keys; *c; ++c) assert(mpn_key(service.session, *c));
                service.refreshCandidates();
            };
            const auto highlight = [&](const std::string &text) {
                for (size_t i = 0; i < service.choices.size() && i < pageSize; ++i)
                    if (service.choices[i].text == text) { service.highlighted = i; return true; }
                return false;
            };
            using Result = std::pair<bool, std::wstring>;
            const Result taken{true, L""}, passed{false, L""};
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，") && service.memoryEnabled); // active context, memory and table loaded
            // Learn 号 for hao: selection memory and its word weight.
            compose("hao");
            assert(service.choices.front().text == "好" && highlight("号"));
            assert(press(VK_RETURN) == Result(true, L"号"));
            const auto hour = selection_memory::currentHour();
            assert(service.memory->recall("hao", true) == std::vector<std::string>{"号"});
            assert(service.memory->words().weight("hao", "号", true, hour) > 0);
            compose("hao");
            assert(service.choices.front().text == "号");
            // Ctrl+Delete on it: forgotten, rebuilt at once, composition kept.
            hold({VK_CONTROL});
            assert(press(VK_DELETE) == taken && service.notice == L"已忘记");
            assert(std::string(mpn_input(service.session)) == "hao" && service.choices.front().text == "好");
            assert(service.memory->recall("hao", true).empty() && service.memory->words().weight("hao", "号", true, hour) == 0);
            assert(read(memoryPath).find("-\ts\thao\t号\n") != std::string::npos);
            assert(read(words).find("-\ts\thao\t号\t") != std::string::npos);
            // Another process sees the forget at its next look.
            {
                selection_memory::Store other(memoryPath);
                other.poll(1000);
                assert(other.recall("hao", true).empty() && other.words().weight("hao", "号", true, hour) == 0);
            }
            // Nothing learned for the highlighted base candidate: nothing changes.
            const auto journal = read(memoryPath), wordJournal = read(words);
            assert(press(VK_DELETE) == taken && service.notice == L"没有学到的记录");
            assert(read(memoryPath) == journal && read(words) == wordJournal && service.choices.front().text == "好");
            hold({});
            // The notice lasts until the next key.
            assert(press(VK_RIGHT) == taken && service.notice.empty());
            mpn_clear(service.session); service.resetCandidates();
            // Learned again, then Shift+Delete.
            compose("hao");
            assert(highlight("号") && press(VK_RETURN) == Result(true, L"号"));
            compose("hao");
            assert(service.choices.front().text == "号");
            hold({VK_SHIFT});
            assert(press(VK_DELETE) == taken && service.notice == L"已忘记" && service.choices.front().text == "好");
            // Both modifiers, or Delete alone, are not the shortcut (Delete stays the host's as before).
            hold({VK_SHIFT, VK_CONTROL});
            assert(press(VK_DELETE) == passed);
            hold({});
            assert(press(VK_DELETE) == passed);
            mpn_clear(service.session); service.resetCandidates();
            // Idle and English mode: the host's.
            hold({VK_CONTROL});
            assert(press(VK_DELETE) == passed);
            hold({VK_SHIFT});
            assert(press(VK_DELETE) == passed);
            hold({});
            service.setMode(false);
            hold({VK_CONTROL});
            assert(press(VK_DELETE) == passed);
            hold({});
            service.setMode(true);
            // A hand-added word is not deleted; the notice points to the script.
            compose("zhangsan");
            assert(service.choices.front().text == "张珊" && mpn_candidate_personal(service.session, *service.choices.front().coreIndex));
            hold({VK_CONTROL});
            assert(press(VK_DELETE) == taken && service.notice.find(L"-RemoveWord") != std::wstring::npos);
            assert(service.notice.find(L"已忘记") == std::wstring::npos);
            assert(read(table) == tableRows && service.choices.front().text == "张珊");
            hold({});
            // What was learned for it is forgotten; the word itself stays.
            mpn_clear(service.session); service.resetCandidates();
            compose("zhangsan");
            assert(press(VK_RETURN) == Result(true, L"张珊") && service.memory->recall("zhangsan", true) == std::vector<std::string>{"张珊"});
            compose("zhangsan");
            hold({VK_CONTROL});
            assert(press(VK_DELETE) == taken && service.notice.find(L"已忘记学习记录") == 0);
            assert(service.memory->recall("zhangsan", true).empty() && read(table) == tableRows && service.choices.front().text == "张珊");
            hold({});
            mpn_clear(service.session); service.resetCandidates();
            service.memory.reset();
            std::filesystem::remove(memoryPath); std::filesystem::remove(words); std::filesystem::remove(table);
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    }

    // CN-07 batch 6 through the real key callbacks: v then digits offers
    // numerals, capitals and amounts; vrq / vsj / vxq the date, time and
    // weekday from the (fixed) clock; nothing generated is learned.
    static void quickInput(const std::filesystem::path &folder) {
        const std::wstring path = L"Software\\HarukotoQuickInputService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        const auto memoryPath = folder / (L"service-quick-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root; // selection memory on, Chinese punctuation
            service.clock = [] { return quick_input::Moment{2026, 11, 20, 14, 30, 5}; };
            const std::string data = "MEOWPUUT-PINYIN-1\nni\tni3\t你\t你\t9000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            std::filesystem::remove(memoryPath);
            service.memory = std::make_unique<selection_memory::Store>(memoryPath);
            const auto words = service.memory->wordsFile();
            std::filesystem::remove(words);
            const auto press = [&](WPARAM key) {
                BOOL tested = FALSE, eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &tested) == S_OK);
                context.text.clear();
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == eaten);
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto type = [&](const char *keys) {
                for (const char *c = keys; *c; ++c) {
                    const WPARAM key = *c >= 'a' && *c <= 'z' ? WPARAM(*c - 'a' + 'A') : *c == '.' ? WPARAM(VK_OEM_PERIOD) : WPARAM(*c);
                    assert(press(key) == std::make_pair(true, std::wstring()));
                }
            };
            using Result = std::pair<bool, std::wstring>;
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，") && service.memoryEnabled);
            // v123: the forms of the number; Space commits the highlighted one.
            type("v123");
            assert(service.numberInput == "123" && std::string(mpn_input(service.session)) == "v" && service.preeditText() == L"v123");
            assert(service.choices.front().text == "一百二十三" && service.choices.front().generated);
            assert(service.choices[1].text == "壹佰贰拾叁" && service.choices[2].text == "壹佰贰拾叁元整" && service.choices.back().text == "v123");
            assert(service.candidateReading(1) == L"大写");
            assert(press(VK_SPACE) == Result(true, L"一百二十三") && !*mpn_input(service.session) && service.numberInput.empty());
            // An amount with a decimal; a second point is ignored; Enter commits the highlighted form.
            type("v100.5.");
            assert(service.numberInput == "100.5");
            service.highlighted = 2;
            assert(service.choices[2].text == "壹佰元伍角整" && press(VK_RETURN) == Result(true, L"壹佰元伍角整"));
            // Digits extend the number, so arrows (or Tab) choose among the forms.
            type("v8");
            assert(press(VK_RIGHT).first && service.highlighted == 1 && press(VK_RETURN) == Result(true, L"捌"));
            // Backspace removes digits, then the v as usual; Esc cancels.
            type("v12");
            assert(press(VK_BACK) == Result(true, L"") && service.numberInput == "1" && service.choices.front().text == "一");
            assert(press(VK_BACK) == Result(true, L"") && service.numberInput.empty() && std::string(mpn_input(service.session)) == "v");
            assert(!service.choices.empty() && std::none_of(service.choices.begin(), service.choices.end(), [](const auto &c) { return c.generated; }));
            assert(press(VK_BACK) == Result(true, L"") && !*mpn_input(service.session));
            type("v9");
            assert(press(VK_ESCAPE) == Result(true, L"") && !*mpn_input(service.session) && service.numberInput.empty());
            // Punctuation commits the form and follows it; a letter, [ commit it and start again.
            type("v123");
            assert(press(VK_OEM_COMMA) == Result(true, L"一百二十三，"));
            type("v12");
            assert(press('N') == Result(true, L"十二") && std::string(mpn_input(service.session)) == "n");
            assert(press('I') == Result(true, L"") && press(VK_SPACE) == Result(true, L"ni "));
            type("v5");
            assert(press(VK_OEM_4) == Result(true, L"五「"));
            type("v5");
            hold({VK_SHIFT});
            assert(press('1') == Result(true, L"五！"));
            hold({});
            // At most MaxNumber digits.
            type("v");
            for (size_t i = 0; i < quick_input::MaxNumber + 3; ++i) assert(press('9') == Result(true, L""));
            assert(service.numberInput.size() == quick_input::MaxNumber);
            assert(press(VK_ESCAPE).first);
            // Dates, times and weekdays from the clock, first in the list; the ordinary list follows.
            type("vrq");
            assert(service.choices.front().text == "2026年11月20日" && service.choices[1].text == "2026-11-20" && service.numberInput.empty());
            assert(press(VK_RETURN) == Result(true, L"2026年11月20日"));
            type("vsj");
            assert(press('3') == Result(true, L"下午2点30分"));
            type("vxq");
            assert(press(VK_SPACE) == Result(true, L"vxq ")); // Space keeps its raw-letters meaning outside number mode
            type("vxq");
            assert(service.choices.front().text == "星期五" && press('1') == Result(true, L"星期五"));
            // Nothing generated was learned.
            service.memory->flush();
            for (const char *reading : {"v", "vrq", "vsj", "vxq"}) assert(service.memory->recall(reading, true).empty());
            assert(service.memory->words().size() == 0);
            // English mode: the host's.
            service.setMode(false);
            assert(press('V') == Result(false, L"") && press('1') == Result(false, L""));
            service.setMode(true);
            service.memory.reset();
            std::filesystem::remove(memoryPath); std::filesystem::remove(words);
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    }

    // CN-07 batch 7 through the real key callbacks: Shift+Space toggles
    // full-width output only when the option is on; literal output becomes
    // full-width in both modes, candidates and Chinese punctuation do not.
    static void fullWidth() {
        const std::wstring path = L"Software\\HarukotoFullWidthService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        const auto option = [&](DWORD value) {
            HKEY input = nullptr;
            assert(RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &input, nullptr) == ERROR_SUCCESS);
            assert(RegSetValueExW(input, L"FullWidthHotkey", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
            RegCloseKey(input);
        };
        RecordingContext context;
        {
            Service service;
            service.preferenceRoot = root;
            const std::string data = "MEOWPUUT-PINYIN-1\nni\tni3\t你\t你\t9000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            const auto press = [&](WPARAM key) {
                BOOL tested = FALSE, eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &tested) == S_OK);
                context.text.clear();
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == eaten);
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto compose = [&](const char *keys) {
                for (const char *c = keys; *c; ++c) assert(mpn_key(service.session, *c));
                service.refreshCandidates();
            };
            using Result = std::pair<bool, std::wstring>;
            const Result passed{false, L""}, taken{true, L""};
            hold({});
            assert(press(VK_OEM_COMMA) == Result(true, L"，"));
            // Option off (the default): Shift+Space is the host's (a game's jump), nothing changes.
            hold({VK_SHIFT});
            assert(press(VK_SPACE) == passed && !service.fullWidth);
            hold({});
            assert(press('1') == passed && press(VK_SPACE) == passed);
            assert(service.modeTooltip().find(L"半角") != std::wstring::npos);
            // Option on: Shift+Space toggles, once per press.
            option(1);
            hold({VK_SHIFT});
            assert(press(VK_SPACE) == taken && service.fullWidth && service.fullWidthOn());
            assert(service.modeTooltip().find(L"全角") != std::wstring::npos);
            // Chinese mode, idle: digits, space and the symbols the host would have typed.
            hold({});
            assert(press('1') == Result(true, L"１") && press(VK_SPACE) == Result(true, L"　"));
            assert(press(VK_OEM_2) == Result(true, L"／") && press(VK_OEM_MINUS) == Result(true, L"－"));
            assert(press(VK_OEM_PLUS) == Result(true, L"＝") && press(VK_OEM_3) == Result(true, L"｀"));
            assert(press(VK_OEM_COMMA) == Result(true, L"，") && press(VK_OEM_7) == Result(true, L"‘")); // Chinese marks unchanged
            hold({VK_SHIFT});
            assert(press('2') == Result(true, L"＠") && press('A') == Result(true, L"Ａ") && press('1') == Result(true, L"！"));
            hold({});
            // Letters still compose; what the IME types literally is full-width, candidates are not.
            assert(press('N') == taken && press('I') == taken && press(VK_SPACE) == Result(true, L"ｎｉ　"));
            compose("ni");
            assert(press(VK_OEM_2) == Result(true, L"你／"));
            compose("ni");
            assert(service.choices.size() < 9 && press('9') == Result(true, L"你９"));
            compose("ni");
            assert(press('1') == Result(true, L"你")); // a digit that selects still selects
            compose("ni");
            hold({VK_SHIFT});
            assert(press('W') == Result(true, L"ｎｉＷ"));
            hold({});
            // English punctuation mode: punctuation is full-width ASCII.
            hold({VK_CONTROL});
            assert(press(VK_OEM_PERIOD) == taken);
            hold({});
            assert(press(VK_OEM_PERIOD) == Result(true, L"．") && press(VK_OEM_4) == Result(true, L"［"));
            hold({VK_CONTROL});
            assert(press(VK_OEM_PERIOD) == taken);
            hold({});
            // Switching to English mid-composition commits the letters full-width.
            compose("ni");
            context.text.clear();
            service.setMode(false);
            assert(context.text == L"ｎｉ");
            // English mode: every printable key full-width; others still the host's.
            assert(press('A') == Result(true, L"ａ") && press('1') == Result(true, L"１") && press(VK_SPACE) == Result(true, L"　"));
            hold({VK_SHIFT});
            assert(press('A') == Result(true, L"Ａ") && press('1') == Result(true, L"！"));
            assert(press(VK_SPACE) == taken && !service.fullWidth); // toggles in English mode too
            hold({});
            assert(press('A') == passed && press(VK_RETURN) == passed);
            hold({VK_SHIFT});
            assert(press(VK_SPACE) == taken && service.fullWidth);
            hold({});
            assert(press(VK_RETURN) == passed && press(VK_LEFT) == passed);
            service.setMode(true);
            // Option switched off while full-width: half-width again, Shift+Space the host's.
            option(0);
            assert(!service.fullWidthOn() && press('1') == passed && press(VK_SPACE) == passed);
            hold({VK_SHIFT});
            assert(press(VK_SPACE) == passed);
            hold({});
            service.Deactivate();
        }
        assert(context.refs == 1);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
    }

    // WIN-08 through the real key callbacks: a host that draws the
    // candidates itself (pbShow FALSE) gets them from the UI element and the
    // reading as a composition, and our window stays hidden; a host that
    // leaves them to us sees no change; the open/close compartment set by
    // the host switches the mode without stranding the user in English.
    static void gameInput() {
        const std::wstring path = L"Software\\HarukotoGameInputService-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr;
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
        const auto option = [&](DWORD value) {
            HKEY input = nullptr;
            assert(RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &input, nullptr) == ERROR_SUCCESS);
            assert(RegSetValueExW(input, L"InlineComposition", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
            RegCloseKey(input);
        };
        RecordingContext context;
        context.composable = true;
        FakeThreadManager manager;
        {
            Service service;
            service.preferenceRoot = root;
            const std::string data = "MEOWPUUT-PINYIN-1\nni\tni3\t你\t你\t9000\nni\tni2\t泥\t泥\t5000\n";
            service.lexicon = mpn_lexicon_create(data.data(), data.size());
            service.session = mpn_session_create(service.lexicon);
            assert(service.lexicon && service.session);
            service.attachMode(&manager);
            assert(manager.openClose.open() && manager.openClose.sink && service.chineseMode && !service.openRule.changes);
            using Result = std::pair<bool, std::wstring>;
            const Result passed{false, L""};
            const auto press = [&](WPARAM key) {
                BOOL tested = FALSE, eaten = FALSE;
                assert(service.OnTestKeyDown(&context, key, 0, &tested) == S_OK);
                context.text.clear();
                assert(service.OnKeyDown(&context, key, 0, &eaten) == S_OK);
                assert(tested == eaten);
                return std::make_pair(eaten != FALSE, context.text);
            };
            const auto type = [&] { assert(press('N').first && press('I').first && *mpn_input(service.session)); };
            const auto list = [&] {
                ITfCandidateListUIElementBehavior *element = nullptr;
                assert(manager.element && SUCCEEDED(manager.element->QueryInterface(IID_ITfCandidateListUIElementBehavior,
                    reinterpret_cast<void **>(&element))));
                return element;
            };
            const auto find = [&](const char *text) {
                for (size_t i = 0; i < service.choices.size(); ++i) if (service.choices[i].text == text) return i;
                assert(false);
                return size_t(0);
            };
            const auto ownWindowShown = [&] { return service.popup && IsWindowVisible(service.popup); };
            hold({});

            // 1. The host draws the candidates: element, inline reading, our window hidden.
            manager.show = FALSE;
            type();
            assert(manager.begun == 1 && manager.element && std::string(service.game.candidateUi) == "host" && !ownWindowShown());
            assert(context.composing && context.compositions == 1 && context.reading == service.preeditText());
            assert(context.reading.find(L"ni") != std::wstring::npos && context.text.empty());
            {
                auto *element = list();
                UINT count = 0, selection = 9, current = 9, pages = 0;
                assert(element->GetCount(&count) == S_OK && count == service.choices.size() && count >= 2);
                BSTR first = nullptr;
                assert(element->GetString(0, &first) == S_OK && std::wstring(first) == L"你");
                SysFreeString(first);
                assert(element->GetString(count, &first) == E_INVALIDARG && !first);
                assert(element->GetSelection(&selection) == S_OK && !selection);
                assert(element->GetCurrentPage(&current) == S_OK && !current);
                assert(element->GetPageIndex(nullptr, 0, &pages) == S_OK && pages == (count + 6) / 7);
                UINT starts[8]{9, 9};
                assert(element->GetPageIndex(starts, 8, &pages) == S_OK && starts[0] == 0);
                DWORD flags = 0;
                assert(element->GetUpdatedFlags(&flags) == S_OK && (flags & TF_CLUIE_STRING) && (flags & TF_CLUIE_COUNT));
                assert(element->GetUpdatedFlags(&flags) == S_OK && !flags);
                BOOL shown = TRUE;
                assert(element->IsShown(&shown) == S_OK && !shown);
                GUID id{};
                assert(element->GetGUID(&id) == S_OK && IsEqualGUID(id, candidateElementGuid));
                BSTR description = nullptr;
                assert(element->GetDescription(&description) == S_OK && description);
                SysFreeString(description);
                // Moving the highlight tells the host just that.
                const auto updates = manager.updates;
                assert(press(VK_DOWN).first && manager.updates == updates + 1);
                assert(element->GetUpdatedFlags(&flags) == S_OK && flags == TF_CLUIE_SELECTION);
                assert(element->GetSelection(&selection) == S_OK && selection == 1);
                element->Release();
            }
            // A digit commits: the reading leaves the document first, the text follows.
            assert(press(static_cast<WPARAM>('1' + find("你"))) == Result(true, L"你"));
            assert(!context.composing && context.reading.empty() && context.compositionsEnded == 1);
            assert(!manager.element && manager.ended == 1 && service.game.elementsHostDrawn == 1);
            // The host selects and finalizes, or aborts.
            type();
            {
                auto *element = list();
                assert(element->SetSelection(static_cast<UINT>(find("泥"))) == S_OK && service.highlighted == find("泥"));
                context.text.clear();
                assert(element->Finalize() == S_OK && context.text == L"泥" && !context.composing && !manager.element);
                element->Release();
            }
            type();
            {
                auto *element = list();
                context.text.clear();
                assert(element->Abort() == S_OK && context.text.empty() && !*mpn_input(service.session));
                assert(!context.composing && !manager.element);
                element->Release();
            }
            // The host ends the composition (focus moved, a game's cancel): the reading is dropped.
            type();
            assert(context.composing && context.sink);
            context.composing = false; // TSF leaves the text to the host
            assert(context.sink->OnCompositionTerminated(1, context.current) == S_OK);
            context.current = nullptr;
            assert(!*mpn_input(service.session) && service.choices.empty() && !manager.element);
            assert(service.game.compositionsTerminated == 1 && !service.composition);
            // The host hands the list back with Show(TRUE): our window, no inline reading.
            type();
            assert(context.composing);
            assert(manager.element->Show(TRUE) == S_OK && service.candidateShown && !context.composing);
            assert(std::string(service.game.candidateUi) == "own");
            assert(press(VK_ESCAPE).first && !*mpn_input(service.session) && !manager.element);

            // 2. The host leaves the candidates to us: as before, no inline reading.
            manager.show = TRUE;
            const auto compositions = context.compositions;
            type();
            assert(manager.element && std::string(service.game.candidateUi) == "own" && !context.composing);
            assert(press(VK_ESCAPE).first && !manager.element && context.compositions == compositions);
            // InlineComposition 1: the reading inline also beside our window; 2: never.
            option(inlineCompositionAlways);
            type();
            assert(context.composing && std::string(service.game.candidateUi) == "own");
            assert(press(VK_ESCAPE).first && !context.composing && !manager.element);
            option(inlineCompositionNever);
            manager.show = FALSE;
            type();
            assert(!context.composing && std::string(service.game.candidateUi) == "host");
            assert(press(VK_ESCAPE).first && !manager.element);
            option(inlineCompositionAuto);
            // Without a UI element manager: our window, as before.
            manager.uiElements = false;
            const auto begun = manager.begun;
            type();
            assert(manager.begun == begun && std::string(service.game.candidateUi) == "own" && !context.composing);
            assert(press(VK_ESCAPE).first);
            manager.uiElements = true;

            // 3. Open/close from the host. Our own writes are echoes, not changes.
            assert(!service.openRule.changes);
            manager.openClose.host(false);
            assert(!service.chineseMode && service.openRule.hostClosed && service.openRule.preferred);
            assert(press('A') == passed && press(VK_OEM_COMMA) == passed);
            manager.openClose.host(true); // the chat box again: Chinese again
            assert(service.chineseMode && !service.openRule.hostClosed && press(VK_OEM_COMMA) == Result(true, L"，"));
            // Closed mid-composition: the letters are committed, the inline reading ends first.
            type();
            assert(context.composing && manager.element);
            context.text.clear();
            manager.openClose.host(false);
            assert(context.text == L"ni" && !context.composing && !manager.element && !service.chineseMode);
            // A host that never reopens: the user's switch (Shift tap) reopens and is written.
            const auto writes = manager.openClose.writes;
            service.setMode(true);
            assert(service.chineseMode && manager.openClose.open() && manager.openClose.writes == writes + 1);
            assert(!service.openRule.hostClosed && service.openRule.changes == 3 && service.openRule.hostCloses == 2);
            // The user's English; an explicit open (Ctrl+Space) is Chinese, as before.
            service.setMode(false);
            assert(!manager.openClose.open() && !service.openRule.preferred);
            manager.openClose.host(true);
            assert(service.chineseMode && service.openRule.preferred);
            // A repeated value is no change.
            manager.openClose.host(true);
            assert(service.openRule.changes == 4);
            service.Deactivate();
        }
        assert(context.refs == 1 && manager.refs == 1 && manager.openClose.refs == 1 && !manager.openClose.sink);
        assert(!manager.element && !context.composing);
        RegCloseKey(root);
        assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
#ifdef _MSC_VER
        assert(IsEqualGUID(uiElementCapability, GUID_TFCAT_TIPCAP_UIELEMENTENABLED));
#endif
        // The rule on its own.
        OpenCloseRule rule;
        rule.activate();
        assert(!rule.observed(true) && rule.observed(false) && !rule.chinese && rule.hostClosed && rule.preferred);
        assert(std::string(rule.mode()) == "host_closed" && rule.observed(true) && rule.chinese);
        assert(!rule.user(false) && rule.observed(true) && rule.chinese && rule.preferred);
        assert(rule.user(false) == false && rule.observed(true) && rule.observed(false) && rule.observed(true) && rule.chinese);
    }
};
}
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    ServiceFocusTest::run(); ServiceFocusTest::memory(argv[1]); ServiceFocusTest::personalWords(argv[1]);
    ServiceFocusTest::userWords(argv[1]);
    ServiceFocusTest::fuzzyPinyin();
    ServiceFocusTest::typoCorrection();
    ServiceFocusTest::punctuation();
    ServiceFocusTest::inputMode();
    ServiceFocusTest::paging();
    ServiceFocusTest::wordCharacters(argv[1]);
    ServiceFocusTest::forgetting(argv[1]);
    ServiceFocusTest::quickInput(argv[1]);
    ServiceFocusTest::fullWidth();
    ServiceFocusTest::gameInput();
    std::cout << "PASS: TSF focus cancellation, fresh input, selected-text preservation, selection memory, personal words, user words, fuzzy pinyin, typo correction, punctuation, Chinese/English mode, -/= paging, [ ] character picking, forgetting, numbers, dates, full width, candidate UI element, inline composition and host open/close\n";
}
