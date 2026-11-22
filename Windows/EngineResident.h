#pragma once
#include "EngineHandoff.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <atomic>

// WIN-03 step 4: resident lifecycle (docs/ENGINE_LIFECYCLE.md).
// - Who starts the owner: nobody at logon. When no engine answers, ensure()
//   on a client's service thread asks the session's Explorer to start the
//   approved owner (step 1 metadata), so it is created outside the client's
//   process tree and Job; the owner then re-checks that it is the approved
//   release before serving.
// - Starts are serialized by the starter lock (step 3) and limited by a
//   breaker kept in a volatile per-logon-session registry key, which also
//   carries the owner's self-reported resident state for diagnostics. Neither
//   the key nor anything in it is ever a trust source.
// Blocking calls: run on a service / I/O thread, never in a TSF callback.
namespace harukoto::engine::resident {
constexpr wchar_t DefaultScope[]=L"harukoto";       // Same as discovery::DefaultScope.
// WIN-03 step 7a: per-release scope so a newer installed engine can run
// alongside an older one draining its remaining clients at composition boundaries.
inline std::wstring releaseScope(uint64_t sequence, const std::wstring &prefix=DefaultScope) {
    return prefix+L"-s"+std::to_wstring(sequence);
}
constexpr wchar_t WindowClass[]=L"Harukoto.ResidentOwner.v1";
constexpr uint32_t IdleNever=0, IdleMinMinutes=5, IdleMaxMinutes=1440;
// Formal budgets from the step-5 measurements (docs/ENGINE_COST.md).
// Host query workers: one per client slot (admission::Limit), so a request
// never waits in the executor behind another client's long query; idle
// workers cost no CPU and about 0.3 MiB each.
constexpr size_t HostThreads=8;
// Deadline a step-6 client puts on each keystroke request (the host drops the
// work after it). The slowest measured query, a 49-letter sentence with 8
// clients typing at once, took about 0.24 s in-process and through the engine.
constexpr uint64_t KeyDeadlineMs=1000;
// WIN-03 step 7b: resident host idle read timeout (5 min) and idle LRU slot
// reclamation threshold (5 s) so 8 slots serve dozens of desktop apps cleanly.
constexpr uint64_t HostReadMs=300000;
constexpr uint64_t SlotIdleEvictMs=5000;
// Owner process exit codes in resident mode.
constexpr DWORD ExitClean=0, ExitFailure=1, ExitNotApproved=3, ExitCrashLoop=4, ExitExists=ERROR_ALREADY_EXISTS;

enum class Reason : uint32_t { Unknown=0, Logon=1, Demand=2 };
enum class State : uint32_t { None=0, Starting=1, Ready=2, Restarting=3, Stopping=4, Exited=5 };
// Why the last owner exited. Stopped: asked to (WM_CLOSE, e.g. uninstall);
// Superseded: a newer release was installed and the last client left (step 7a).
enum class Exit : uint32_t { None=0, Stopped=1, Idle=2, Logoff=3, Shutdown=4, CrashLoop=5, Failed=6, Superseded=7 };

struct Policy {
    // On-demand starts since the last clean owner exit, counted in a window.
    uint64_t windowMs=600000;                       // 10 minutes.
    uint32_t maxStarts=3;                           // The next one trips the breaker.
    std::array<uint64_t,3> spacingMs{0,2000,10000}; // Minimum gap after 0, 1, 2 recent starts.
    // Host restarts inside one owner, same window.
    uint32_t maxHostRestarts=3;
    std::array<uint64_t,3> hostBackoffMs{1000,2000,4000};
};
// Production: HKCU\Software\Meowpuut\Engine (settings, persistent) and
// HKCU\Software\Meowpuut\EngineState.v1 (volatile). Tests inject a subkey.
struct Store {
    HKEY root=HKEY_CURRENT_USER;
    std::wstring settings=L"Software\\Meowpuut\\Engine";
    std::wstring state=L"Software\\Meowpuut\\EngineState.v1";
    static Store under(const std::wstring &subkey) { Store s; s.settings=subkey+L"\\Engine"; s.state=subkey+L"\\EngineState.v1"; return s; }
};

// IdleMinutes (REG_DWORD): 0 or missing = never exit when idle (default);
// otherwise clamped to 5..1440. Read once when the owner starts.
inline uint32_t clampIdle(uint32_t minutes) {
    if (minutes==IdleNever) return IdleNever;
    return std::min(std::max(minutes,IdleMinMinutes),IdleMaxMinutes);
}
inline uint32_t idleMinutes(const Store &store=Store{}) {
    DWORD value=0, size=sizeof(value), type=0;
    if (RegGetValueW(store.root,store.settings.c_str(),L"IdleMinutes",RRF_RT_REG_DWORD,&type,&value,&size)!=ERROR_SUCCESS) return IdleNever;
    return clampIdle(value);
}

// ---------------------------------------------------------------- breaker
enum class Gate { Allow, Backoff, Tripped };
struct Decision { Gate gate=Gate::Allow; uint64_t retryAt=0; size_t recent=0; };
// starts: GetTickCount64 values of earlier on-demand starts (system-wide,
// monotonic within a boot; the ledger dies with the logon session anyway).
inline Decision decide(const Policy &policy, std::vector<uint64_t> starts, bool tripped, uint64_t now) {
    Decision d;
    if (tripped) { d.gate=Gate::Tripped; return d; }
    starts.erase(std::remove_if(starts.begin(),starts.end(),[&](uint64_t t) { return t>now || now-t>=policy.windowMs; }),starts.end());
    d.recent=starts.size();
    if (d.recent>=policy.maxStarts) { d.gate=Gate::Tripped; return d; }
    if (!d.recent) return d;
    const auto spacing=policy.spacingMs[std::min<size_t>(d.recent,policy.spacingMs.size()-1)];
    const auto last=*std::max_element(starts.begin(),starts.end());
    if (now<last+spacing) { d.gate=Gate::Backoff; d.retryAt=last+spacing; }
    return d;
}
// Host restarts inside an owner: prune the window, then either allow the
// next restart (returns its backoff) or report a crash loop (nullopt).
inline std::optional<uint64_t> hostRestart(const Policy &policy, std::deque<uint64_t> &restarts, uint64_t now) {
    while (!restarts.empty() && now-restarts.front()>=policy.windowMs) restarts.pop_front();
    if (restarts.size()>=policy.maxHostRestarts) return std::nullopt;
    restarts.push_back(now);
    return policy.hostBackoffMs[std::min<size_t>(restarts.size()-1,policy.hostBackoffMs.size()-1)];
}

// ---------------------------------------------------------------- ledger
// Volatile key per logon session (Windows session + logon LUID) and scope,
// so a new logon never sees the previous one's state even if the user hive
// stayed loaded. Same-user programs can read and change it: it only steers
// the breaker and diagnostics, never which binary runs or who is trusted.
inline std::wstring sessionName(const std::wstring &scope) {
    if (!EngineInstance::validScope(scope)) throw std::invalid_argument("invalid resident scope");
    pipe::processIdentityOnly();
    const auto identity=pipe::LogonIdentity::fromProcess(GetCurrentProcess());
    std::wstring result;
    auto hex=[&](uint32_t value) { for (int shift=28;shift>=0;shift-=4) result+=L"0123456789abcdef"[(value>>shift)&15]; };
    hex(identity.session); hex(static_cast<uint32_t>(identity.authentication.HighPart)); hex(identity.authentication.LowPart);
    return result+L"."+scope;
}
struct Status {
    bool present=false, alive=false, tripped=false;
    DWORD ownerPid=0, hostPid=0, clients=0, hostRestarts=0, lastExitPid=0;
    uint64_t ownerCreated=0, startTick=0, lastExitTick=0, trippedTick=0;
    Reason reason=Reason::Unknown;
    State state=State::None;
    Exit lastExit=Exit::None;
    std::vector<uint64_t> starts;
};
class Ledger {
    HKEY key_=nullptr;
    static HKEY create(HKEY root, const std::wstring &path, DWORD options) {
        HKEY key=nullptr;
        const auto status=RegCreateKeyExW(root,path.c_str(),0,nullptr,options,KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY,nullptr,&key,nullptr);
        if (status!=ERROR_SUCCESS) throw pipe::Error(pipe::Failure::Io,static_cast<DWORD>(status));
        return key;
    }
    template<class T> bool put(const wchar_t *name, DWORD type, const T &value) noexcept {
        return RegSetValueExW(key_,name,0,type,reinterpret_cast<const BYTE *>(&value),sizeof(T))==ERROR_SUCCESS;
    }
    bool dword(const wchar_t *name, DWORD value) noexcept { return put(name,REG_DWORD,value); }
    bool qword(const wchar_t *name, uint64_t value) noexcept { return put(name,REG_QWORD,value); }
public:
    // Opens or creates the key. The parent of store.state is created
    // persistent first so a volatile key never swallows the settings key.
    Ledger(const Store &store, const std::wstring &scope) {
        const auto split=store.state.rfind(L'\\');
        if (split==std::wstring::npos) throw std::invalid_argument("invalid resident state key");
        RegCloseKey(create(store.root,store.state.substr(0,split),REG_OPTION_NON_VOLATILE));
        const auto state=create(store.root,store.state,REG_OPTION_VOLATILE);
        try { key_=create(state,sessionName(scope),REG_OPTION_VOLATILE); } catch (...) { RegCloseKey(state); throw; }
        RegCloseKey(state);
    }
    Ledger(const Ledger &)=delete;
    Ledger &operator=(const Ledger &)=delete;
    ~Ledger() { if (key_) RegCloseKey(key_); }
    static Status read(HKEY key) {
        Status s; s.present=true;
        auto dw=[&](const wchar_t *name) { DWORD v=0, size=sizeof(v); return RegGetValueW(key,nullptr,name,RRF_RT_REG_DWORD,nullptr,&v,&size)==ERROR_SUCCESS ? v : 0; };
        auto qw=[&](const wchar_t *name) { uint64_t v=0; DWORD size=sizeof(v); return RegGetValueW(key,nullptr,name,RRF_RT_REG_QWORD,nullptr,&v,&size)==ERROR_SUCCESS ? v : 0; };
        s.ownerPid=dw(L"OwnerPid"); s.ownerCreated=qw(L"OwnerCreated"); s.reason=static_cast<Reason>(dw(L"Reason"));
        s.state=static_cast<State>(dw(L"State")); s.startTick=qw(L"StartTick"); s.clients=dw(L"Clients"); s.hostPid=dw(L"HostPid");
        s.hostRestarts=dw(L"HostRestarts"); s.lastExit=static_cast<Exit>(dw(L"LastExit")); s.lastExitTick=qw(L"LastExitTick");
        s.lastExitPid=dw(L"LastExitPid"); s.tripped=dw(L"Tripped")!=0; s.trippedTick=qw(L"TrippedTick");
        std::array<uint64_t,8> starts{}; DWORD size=sizeof(starts);
        if (RegGetValueW(key,nullptr,L"Starts",RRF_RT_REG_BINARY,nullptr,starts.data(),&size)==ERROR_SUCCESS)
            for (size_t i=0;i<size/sizeof(uint64_t) && i<starts.size();++i) if (starts[i]) s.starts.push_back(starts[i]);
        if (static_cast<uint32_t>(s.reason)>2) s.reason=Reason::Unknown;
        if (static_cast<uint32_t>(s.state)>5) s.state=State::None;
        if (static_cast<uint32_t>(s.lastExit)>7) s.lastExit=Exit::None;
        return s;
    }
    Status read() const { return read(key_); }
    // Starter side (under the starter lock).
    bool starts(const std::vector<uint64_t> &values) noexcept {
        if (values.empty()) { RegDeleteValueW(key_,L"Starts"); return true; }
        std::array<uint64_t,8> bytes{};
        const auto count=std::min(values.size(),bytes.size());
        std::copy(values.end()-count,values.end(),bytes.begin()); // Newest eight.
        return RegSetValueExW(key_,L"Starts",0,REG_BINARY,reinterpret_cast<const BYTE *>(bytes.data()),static_cast<DWORD>(count*sizeof(uint64_t)))==ERROR_SUCCESS;
    }
    bool trip(uint64_t tick) noexcept { return dword(L"Tripped",1) && qword(L"TrippedTick",tick); }
    bool reset() noexcept { RegDeleteValueW(key_,L"Tripped"); RegDeleteValueW(key_,L"TrippedTick"); RegDeleteValueW(key_,L"Starts"); return true; }
    // Owner side (self-reported; best effort).
    bool started(DWORD pid, uint64_t created, Reason reason, uint64_t tick) noexcept {
        return dword(L"OwnerPid",pid) && qword(L"OwnerCreated",created) && dword(L"Reason",static_cast<DWORD>(reason)) &&
            qword(L"StartTick",tick) && dword(L"State",static_cast<DWORD>(State::Starting)) && dword(L"Clients",0) &&
            dword(L"HostPid",0) && dword(L"HostRestarts",0);
    }
    bool state(State value) noexcept { return dword(L"State",static_cast<DWORD>(value)); }
    bool clients(size_t count, DWORD host) noexcept {
        return dword(L"Clients",static_cast<DWORD>(count)) && (!host || (dword(L"HostPid",host) && state(State::Ready)));
    }
    bool hostRestarts(DWORD count) noexcept { return dword(L"HostRestarts",count); }
    bool exited(Exit reason, DWORD pid, uint64_t tick) noexcept {
        return dword(L"LastExit",static_cast<DWORD>(reason)) && qword(L"LastExitTick",tick) && dword(L"LastExitPid",pid) &&
            state(State::Exited) && dword(L"Clients",0) && dword(L"HostPid",0);
    }
};
inline uint64_t created(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process,&creation,&exit,&kernel,&user)) return 0;
    return (uint64_t(creation.dwHighDateTime)<<32) | creation.dwLowDateTime;
}
// Diagnostics: the self-reported state plus whether the recorded owner
// (PID and creation time) is still running. Read only; never a locator for trust.
inline Status inspect(const Store &store=Store{}, const std::wstring &scope=DefaultScope) {
    Status s;
    HKEY key=nullptr;
    if (RegOpenKeyExW(store.root,(store.state+L"\\"+sessionName(scope)).c_str(),0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS) return s;
    s=Ledger::read(key); RegCloseKey(key);
    if (s.ownerPid) {
        pipe::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,s.ownerPid));
        s.alive=process && WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT && created(process.get())==s.ownerCreated;
    }
    return s;
}
// Explicit reset (settings / diagnostics action): clears the breaker.
inline bool reset(const Store &store=Store{}, const std::wstring &scope=DefaultScope) {
    try { Ledger ledger(store,scope); return ledger.reset(); } catch (...) { return false; }
}

// ---------------------------------------------------------------- start
// The broker is the session's shell. The client asks the running Explorer
// (through its desktop automation object: ShellWindows -> desktop view ->
// IShellDispatch2::ShellExecute) to start the approved owner. Explorer
// creates it as its own child: in the user's session, with the user's
// default environment, outside the client's process tree and Job. So a game
// or browser that keeps its children in a kill-on-close Job without
// breakaway cannot take the engine down when it exits, and the host
// application never creates a process itself. No registration is needed;
// the program path comes from the approved release (pinned, hash checked),
// and the owner re-checks itself against the same metadata before serving.
// Without a shell (custom shell, Explorer restarting) nothing starts and the
// client stays on its in-process path.
enum class Launch {
    NotNeeded,     // An engine answered; nothing started.
    Started,       // The shell accepted the request and the discovery pipe appeared.
    Pending,       // Accepted, but no discovery pipe by the wait limit (still loading, or the owner gave up).
    Failed,        // No shell, the shell refused, or the approved file does not match.
    Backoff,       // Breaker spacing: too soon after the previous on-demand start.
    Tripped,       // Breaker open until the next logon or reset().
    NoRelease      // No approved release for this protocol: nothing started.
};
struct Started { Launch result=Launch::Failed; HRESULT hr=S_OK; };
namespace detail {
template<class T> struct Com {
    T *p=nullptr;
    Com()=default;
    Com(const Com &)=delete;
    Com &operator=(const Com &)=delete;
    ~Com() { if (p) p->Release(); }
    T **out() { if (p) { p->Release(); p=nullptr; } return &p; }
    T *operator->() const { return p; }
};
struct Variant {
    VARIANT v;
    Variant() { VariantInit(&v); }
    explicit Variant(const std::wstring &text) { VariantInit(&v); v.vt=VT_BSTR; v.bstrVal=SysAllocString(text.c_str()); if (!v.bstrVal) throw std::bad_alloc(); }
    explicit Variant(long value) { VariantInit(&v); v.vt=VT_I4; v.lVal=value; }
    ~Variant() { VariantClear(&v); }
    Variant(const Variant &)=delete;
    Variant &operator=(const Variant &)=delete;
};
inline std::wstring quote(const std::wstring &value) {
    std::wstring output=L"\""; size_t slashes=0;
    for (auto c : value) {
        if (c==L'\\') { ++slashes; continue; }
        output.append(c==L'\"' ? slashes*2+1 : slashes,L'\\'); slashes=0; output+=c;
    }
    output.append(slashes*2,L'\\'); return output+L"\"";
}
// Ask the desktop's Explorer to ShellExecute program with arguments.
inline HRESULT shellExecute(const std::wstring &program, const std::wstring &arguments, const std::wstring &directory) {
    Com<IShellWindows> windows;
    auto hr=CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_IShellWindows,reinterpret_cast<void **>(windows.out()));
    if (FAILED(hr)) return hr;
    Variant location(static_cast<long>(CSIDL_DESKTOP)), root;
    long window=0; Com<IDispatch> desktop;
    hr=windows->FindWindowSW(&location.v,&root.v,SWC_DESKTOP,&window,SWFO_NEEDDISPATCH,desktop.out());
    if (hr!=S_OK || !desktop.p) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    Com<IServiceProvider> provider; Com<IShellBrowser> browser; Com<IShellView> view;
    Com<IDispatch> background; Com<IShellFolderViewDual> folder; Com<IDispatch> application; Com<IShellDispatch2> shell;
    if (FAILED(hr=desktop->QueryInterface(IID_IServiceProvider,reinterpret_cast<void **>(provider.out())))) return hr;
    if (FAILED(hr=provider->QueryService(SID_STopLevelBrowser,IID_IShellBrowser,reinterpret_cast<void **>(browser.out())))) return hr;
    if (FAILED(hr=browser->QueryActiveShellView(view.out()))) return hr;
    if (FAILED(hr=view->GetItemObject(SVGIO_BACKGROUND,IID_IDispatch,reinterpret_cast<void **>(background.out())))) return hr;
    if (FAILED(hr=background->QueryInterface(IID_IShellFolderViewDual,reinterpret_cast<void **>(folder.out())))) return hr;
    if (FAILED(hr=folder->get_Application(application.out()))) return hr;
    if (FAILED(hr=application->QueryInterface(IID_IShellDispatch2,reinterpret_cast<void **>(shell.out())))) return hr;
    BSTR file=SysAllocString(program.c_str()); if (!file) return E_OUTOFMEMORY;
    struct Free { BSTR b; ~Free() { SysFreeString(b); } } freeFile{file};
    Variant args(arguments), dir(directory), operation(std::wstring(L"open")), show(static_cast<long>(SW_HIDE));
    return shell->ShellExecute(file,args.v,dir.v,operation.v,show.v);
}
} // namespace detail
// Start the approved owner through the shell and wait (at most waitMs and
// not past deadline) until the discovery pipe of scope exists. arguments:
// data paths (--base/--themes/--personal) and, for the test build of the
// owner only, its test switches; never another program. Usable from any
// thread; a thread without COM is initialized for the call only.
inline Started start(const release::Entry &approved, uint64_t deadline, const std::wstring &scope=DefaultScope,
        const std::vector<std::wstring> &arguments={}, uint64_t waitMs=15000) {
    Started out;
    const auto init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Uninit { bool armed; ~Uninit() { if (armed) CoUninitialize(); } } uninit{SUCCEEDED(init)};
    if (FAILED(init) && init!=RPC_E_CHANGED_MODE) { out.hr=init; return out; }
    try {
        pipe::processIdentityOnly();
        // Pinned (no replacement or rename while we ask) and hash checked.
        const auto owner=EngineDeployment::shared(approved.owner,approved.ownerSha256);
        std::wstring line=L"--resident demand";
        for (const auto &argument : arguments) line+=L" "+detail::quote(argument);
        std::wstring system(MAX_PATH,L'\0');
        const auto length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
        pipe::check(length && length<system.size()); system.resize(length);
        // The pinned final path is \\?\X:\...; the shell wants a plain drive path.
        auto program=owner->path().wstring();
        if (program.rfind(L"\\\\?\\",0)==0) program.erase(0,4);
        if (program.size()<3 || program[1]!=L':' || program[2]!=L'\\') throw std::invalid_argument("approved owner is not on a local drive");
        out.hr=detail::shellExecute(program,line,system);
        if (FAILED(out.hr)) return out;
        const auto pipeName=discovery::name(scope);
        const auto until=std::min(deadline,pipe::now()+waitMs);
        for (;;) {
            if (WaitNamedPipeW(pipeName.c_str(),1) || GetLastError()==ERROR_SEM_TIMEOUT) { out.result=Launch::Started; break; }
            if (pipe::now()>=until) { out.result=Launch::Pending; break; }
            Sleep(25);
        }
    } catch (const pipe::Error &error) { out.result=Launch::Failed; out.hr=HRESULT_FROM_WIN32(error.code); }
    catch (const std::exception &) { out.result=Launch::Failed; out.hr=E_INVALIDARG; }
    return out;
}

// ---------------------------------------------------------------- ensure
struct EnsureOptions {
    handoff::Options handoff;                 // discovery.scope selects the engine.
    uint64_t budgetMs=10000;
    uint32_t maxAttempts=32;
    uint64_t startWaitMs=10000;               // Never longer than budgetMs; a cold start took about 1.75 s.
    Policy policy;
    Store store;
    std::vector<std::wstring> ownerArguments; // See start().
    // Step 7a: derive "<scope>-s<sequence>" from the approved release so new
    // and draining releases never collide on one global lease/discovery pipe.
    // Enabled automatically when scope is DefaultScope (production).
    bool versionedScope=false;
};
struct EnsureOutcome {
    handoff::AttachOutcome attach;            // attach.outcome.session when Admitted.
    Launch launch=Launch::NotNeeded;
    HRESULT hr=S_OK;
    uint64_t sequence=0;                      // Approved release sequence targeted (step 7a).
};
// The formal replacement for attach()'s dev starter: discover/admit; if no
// engine answers, under the starter lock consult the breaker, ask the shell
// to start the approved owner, then admit. Never replays anything: the
// caller gets a fresh session or stays on its in-process path.
inline EnsureOutcome ensure(const release::Snapshot &snapshot, const EnsureOptions &options) {
    EnsureOutcome out;
    const auto approved=release::select(snapshot,options.handoff.discovery.protocol);
    const bool useVersioned=approved && (options.versionedScope || options.handoff.discovery.scope==DefaultScope);
    const std::wstring scope=useVersioned ? releaseScope(approved->sequence,options.handoff.discovery.scope)
        : options.handoff.discovery.scope;
    if (approved) out.sequence=approved->sequence;
    // Pin the approved files once for the whole call: every discovery attempt
    // and the start below reuse these pins instead of hashing again. A file
    // that cannot be pinned is reported by discovery itself (DeploymentInvalid).
    std::shared_ptr<const EngineDeployment> ownerPin, hostPin;
    if (approved) try { ownerPin=EngineDeployment::shared(approved->owner,approved->ownerSha256); hostPin=EngineDeployment::shared(approved->host,approved->hostSha256); }
    catch (const pipe::Error &) {} catch (const std::exception &) {}
    handoff::AttachOptions attach; attach.handoff=options.handoff; attach.handoff.discovery.scope=scope; attach.budgetMs=options.budgetMs; attach.maxAttempts=options.maxAttempts;
    if (approved) attach.start=[&](uint64_t deadline) {
        // Starter lock held; a fresh discovery just said NotRunning.
        try {
            Ledger ledger(options.store,scope);
            const auto status=ledger.read();
            const auto now=GetTickCount64();
            const auto decision=decide(options.policy,status.starts,status.tripped,now);
            if (decision.gate==Gate::Tripped) { if (!status.tripped) ledger.trip(now); out.launch=Launch::Tripped; return handoff::Start::Failed; }
            if (decision.gate==Gate::Backoff) { out.launch=Launch::Backoff; return handoff::Start::Failed; }
            auto recorded=status.starts; recorded.push_back(now); ledger.starts(recorded);
            const auto started=start(*approved,deadline,scope,options.ownerArguments,options.startWaitMs);
            out.launch=started.result; out.hr=started.hr;
            if (started.result==Launch::Failed) { ledger.starts(status.starts); return handoff::Start::Failed; } // Nothing was started.
            return handoff::Start::Started; // Pending too: keep discovering within the budget.
        } catch (const pipe::Error &error) { out.launch=Launch::Failed; out.hr=HRESULT_FROM_WIN32(error.code); }
        catch (const std::exception &) { out.launch=Launch::Failed; out.hr=E_INVALIDARG; }
        return handoff::Start::Failed;
    };
    out.attach=handoff::attach(snapshot,attach);
    if (!approved && out.attach.outcome.result!=handoff::Result::Admitted) out.launch=Launch::NoRelease;
    return out;
}
inline EnsureOutcome ensure(const EnsureOptions &options=EnsureOptions{}) { return ensure(release::read(),options); }

// ---------------------------------------------------------------- owner side
// A hidden top-level window on its own thread: a windowless process gets no
// session-end notification otherwise. WM_QUERYENDSESSION is always allowed
// (an input engine never blocks logoff); WM_ENDSESSION, WM_CLOSE (installer)
// set the stop event and record why; resume from sleep restarts the idle clock.
class SessionWatch {
    pipe::Handle stop_, resume_, done_, created_;
    std::atomic<uint32_t> reason_{0};
    std::atomic<HWND> window_{nullptr};
    DWORD win32_=ERROR_SUCCESS;
    std::thread thread_;
    static constexpr UINT Quit=WM_APP+1;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto *self=reinterpret_cast<SessionWatch *>(GetWindowLongPtrW(window,GWLP_USERDATA));
        switch (message) {
        case WM_NCCREATE:
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams));
            return TRUE;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (self && wParam) {
                self->request(lParam & ENDSESSION_LOGOFF ? Exit::Logoff : Exit::Shutdown);
                WaitForSingleObject(self->done_.get(),4000); // Let the host stop (3 s grace) before Windows ends us.
            }
            return 0;
        case WM_CLOSE: if (self) self->request(Exit::Stopped); return 0;
        case WM_POWERBROADCAST:
            if (self && (wParam==PBT_APMRESUMEAUTOMATIC || wParam==PBT_APMRESUMESUSPEND)) SetEvent(self->resume_.get());
            return TRUE;
        case Quit: DestroyWindow(window); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window,message,wParam,lParam);
    }
    void run() {
        const auto instance=GetModuleHandleW(nullptr);
        WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc=procedure; cls.hInstance=instance; cls.lpszClassName=WindowClass;
        if (!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { win32_=GetLastError(); SetEvent(created_.get()); return; }
        const auto window=CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,WindowClass,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,this);
        if (!window) { win32_=GetLastError(); SetEvent(created_.get()); return; }
        window_=window; SetEvent(created_.get());
        MSG message;
        while (GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
        window_=nullptr;
    }
public:
    SessionWatch() : stop_(CreateEventW(nullptr,TRUE,FALSE,nullptr)), resume_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),
            done_(CreateEventW(nullptr,TRUE,FALSE,nullptr)), created_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
        pipe::check(stop_ && resume_ && done_ && created_);
        thread_=std::thread([this] { run(); });
        WaitForSingleObject(created_.get(),INFINITE);
        if (!window_) { thread_.join(); throw pipe::Error(pipe::Failure::Io,win32_); }
    }
    SessionWatch(const SessionWatch &)=delete;
    SessionWatch &operator=(const SessionWatch &)=delete;
    ~SessionWatch() {
        finished();
        if (const auto window=window_.load()) PostMessageW(window,Quit,0,0);
        if (thread_.joinable()) thread_.join();
    }
    HANDLE stop() const { return stop_.get(); }
    HANDLE resume() const { return resume_.get(); }
    void request(Exit reason) { uint32_t none=0; reason_.compare_exchange_strong(none,static_cast<uint32_t>(reason)); SetEvent(stop_.get()); }
    Exit reason() const { const auto r=reason_.load(); return r ? static_cast<Exit>(r) : Exit::Stopped; }
    void finished() { SetEvent(done_.get()); }
};
} // namespace harukoto::engine::resident
