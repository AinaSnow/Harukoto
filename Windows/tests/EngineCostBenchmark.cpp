#include "../EngineWindows.h"
#include "../EngineResident.h"
#include "../BaseDictionaryFiles.h"
#include "EngineHostTestClient.h"
#include <psapi.h>
#include <atomic>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

// WIN-03 step 5: cost of the resident engine (docs/ENGINE_COST.md). Real
// resident owners (test build, copied with the host into a temporary version
// directory, approved through an HKCU test release key) started through the
// session Explorer, real client processes holding real handoff slots.
// Measures: cold ensure() timeline, pin/hash, discovery/handoff, first query,
// owner/host memory (private, working set, after trim), per-session overhead,
// state-ledger writes, idle CPU, and per-keystroke latency in-process vs
// through the engine for 1/2/4/8 client processes and several host worker
// thread counts. Writes JSON lines to stdout. No HKLM, Run key, task or COM
// registration; owners it started, the HKCU test key and the directory are
// removed at the end (also on a failed assertion).
using namespace host_test;
namespace rs=harukoto::engine::resident;
namespace hf=harukoto::engine::handoff;
namespace dc=harukoto::engine::discovery;

static void require(bool value, const char *what) { if (!value) { std::cerr << "cost benchmark: " << what << '\n'; std::abort(); } }
static double ms() {
    static const double frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart)/1000.0; }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return double(c.QuadPart)/frequency;
}
static uint64_t precise() { FILETIME t{}; GetSystemTimePreciseAsFileTime(&t); return (uint64_t(t.dwHighDateTime)<<32) | t.dwLowDateTime; }
static double since(uint64_t a, uint64_t b) { return double(int64_t(b-a))/10000.0; }
static std::filesystem::path self() {
    std::wstring value(32768,L'\0'); const auto size=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    require(size && size<value.size(),"module path"); value.resize(size); return value;
}
static std::wstring wide(const std::string &value) { return std::wstring(value.begin(),value.end()); }
static std::string narrow(const std::wstring &value) { std::string out; for (auto c : value) out+=static_cast<char>(c<128 ? c : '?'); return out; }
static std::wstring quote(const std::wstring &value) {
    std::wstring output=L"\""; size_t slashes=0;
    for (auto c : value) {
        if (c==L'\\') { ++slashes; continue; }
        output.append(c==L'\"' ? slashes*2+1 : slashes,L'\\'); slashes=0; output+=c;
    }
    output.append(slashes*2,L'\\'); return output+L"\"";
}
static pipe::Handle event() { pipe::Handle value(CreateEventW(nullptr,TRUE,FALSE,nullptr)); require(bool(value),"event"); return value; }

// ---------------------------------------------------------------- stats
struct Stats { size_t n=0; double median=0, p95=0, p99=0, max=0, mean=0; };
static Stats stats(std::vector<double> values) {
    Stats s; s.n=values.size(); if (values.empty()) return s;
    std::sort(values.begin(),values.end());
    auto rank=[&](double p) { const auto i=static_cast<size_t>(std::ceil(p*values.size())); return values[std::min(values.size(),std::max<size_t>(i,1))-1]; };
    s.median=values.size()%2 ? values[values.size()/2] : (values[values.size()/2-1]+values[values.size()/2])/2;
    s.p95=rank(0.95); s.p99=rank(0.99); s.max=values.back();
    double sum=0; for (auto v : values) sum+=v; s.mean=sum/values.size(); return s;
}
static std::string json(const Stats &s) {
    std::ostringstream o; o << std::setprecision(6) << std::fixed;
    o << "{\"n\":" << s.n << ",\"median\":" << s.median << ",\"p95\":" << s.p95 << ",\"p99\":" << s.p99 << ",\"max\":" << s.max << ",\"mean\":" << s.mean << '}';
    return o.str();
}
struct Series {
    std::map<std::string,std::vector<double>> values;
    void add(const std::string &name, double value) { values[name].push_back(value); }
    std::string json() const {
        std::string out="{"; bool first=true;
        for (const auto &[name,v] : values) { if (!first) out+=','; first=false; out+="\""+name+"\":"+::json(stats(v)); }
        return out+"}";
    }
};

// ---------------------------------------------------------------- processes
struct Memory { uint64_t priv=0, ws=0, peak=0; DWORD handles=0; };
static Memory memory(HANDLE process) {
    Memory m; PROCESS_MEMORY_COUNTERS_EX c{}; c.cb=sizeof(c);
    if (GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&c),sizeof(c))) { m.priv=c.PrivateUsage; m.ws=c.WorkingSetSize; m.peak=c.PeakWorkingSetSize; }
    GetProcessHandleCount(process,&m.handles); return m;
}
static std::string json(const Memory &m) {
    std::ostringstream o; o << "{\"private\":" << m.priv << ",\"working_set\":" << m.ws << ",\"peak_working_set\":" << m.peak << ",\"handles\":" << m.handles << '}';
    return o.str();
}
static pipe::Handle open(DWORD pid) {
    pipe::Handle p(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SET_QUOTA | PROCESS_TERMINATE | SYNCHRONIZE,FALSE,pid));
    require(bool(p),"open engine process"); return p;
}
static uint64_t createdAt(HANDLE process) { FILETIME c{}, e{}, k{}, u{}; require(GetProcessTimes(process,&c,&e,&k,&u),"process times"); return (uint64_t(c.dwHighDateTime)<<32) | c.dwLowDateTime; }
static double cpuMs(HANDLE process) {
    FILETIME c{}, e{}, k{}, u{}; require(GetProcessTimes(process,&c,&e,&k,&u),"process times");
    return double(((uint64_t(k.dwHighDateTime)<<32) | k.dwLowDateTime)+((uint64_t(u.dwHighDateTime)<<32) | u.dwLowDateTime))/10000.0;
}
static bool alive(DWORD pid) { pipe::Handle p(OpenProcess(SYNCHRONIZE,FALSE,pid)); return p && WaitForSingleObject(p.get(),0)==WAIT_TIMEOUT; }
template<class F> static bool waitFor(F condition, uint64_t limit) {
    const auto deadline=pipe::now()+limit;
    for (;;) { if (condition()) return true; if (pipe::now()>=deadline) return false; Sleep(10); }
}

// ---------------------------------------------------------------- fixture
struct Env {
    std::filesystem::path root, version, owner, host, base, themes;
    std::wstring testKey, releaseKey, scope;
    rs::Store store;
    std::vector<DWORD> tracked;
    release::Snapshot snapshot() const { return release::read({HKEY_CURRENT_USER,releaseKey,false}); }
    rs::Status status() const { return rs::inspect(store,scope); }
    rs::EnsureOptions ensure(size_t threads) const {
        rs::EnsureOptions o; o.handoff.discovery.scope=scope; o.budgetMs=60000; o.startWaitMs=60000; o.store=store;
        o.policy.maxStarts=1000; o.policy.spacingMs={0,0,0}; // The benchmark restarts on purpose.
        o.ownerArguments={L"--scope",scope,L"--release-subkey",releaseKey,L"--store-subkey",testKey,L"--base",base.wstring()};
        if (!themes.empty()) { o.ownerArguments.push_back(L"--themes"); o.ownerArguments.push_back(themes.wstring()); }
        if (threads) { o.ownerArguments.push_back(L"--threads"); o.ownerArguments.push_back(std::to_wstring(threads)); }
        return o;
    }
};
static Env *active=nullptr;
static void reap(Env &env) {
    const auto last=env.status(); if (last.ownerPid) env.tracked.push_back(last.ownerPid); if (last.hostPid) env.tracked.push_back(last.hostPid);
    for (auto pid : env.tracked) if (alive(pid)) {
        pipe::Handle p(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,FALSE,pid));
        if (p) { TerminateProcess(p.get(),9); WaitForSingleObject(p.get(),5000); }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER,env.testKey.c_str()); RegDeleteKeyW(HKEY_CURRENT_USER,env.testKey.c_str());
    for (int i=0;i<50;++i) {
        std::error_code error; std::filesystem::remove_all(env.root,error);
        if (!std::filesystem::exists(env.root,error)) return;
        Sleep(100);
    }
    std::cerr << "warning: could not remove temporary directory\n";
}
struct Cleanup {
    Env &env;
    explicit Cleanup(Env &e) : env(e) { active=&e; signal(SIGABRT,[](int) { if (auto *e=active) { active=nullptr; reap(*e); } }); }
    ~Cleanup() { if (active) { active=nullptr; reap(env); } }
};
static void writeRelease(const Env &env, const std::wstring &id) {
    auto sha=[](const std::filesystem::path &p) { return wide(base_dictionary::StableFile(p).read(true)); };
    std::wstring value;
    for (const auto &line : {std::wstring(L"format=harukoto-engine-release/1"),L"id="+id,std::wstring(L"sequence=1"),
            L"protocol="+std::to_wstring(ProtocolVersion),L"host="+env.host.wstring(),L"host-sha256="+sha(env.host),
            L"owner="+env.owner.wstring(),L"owner-sha256="+sha(env.owner)})
        value+=line+L'\0';
    value+=L'\0';
    HKEY key=nullptr;
    require(RegCreateKeyExW(HKEY_CURRENT_USER,env.releaseKey.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS,"release key");
    require(RegSetValueExW(key,id.c_str(),0,REG_MULTI_SZ,reinterpret_cast<const BYTE *>(value.data()),static_cast<DWORD>(value.size()*sizeof(wchar_t)))==ERROR_SUCCESS,"release value");
    RegCloseKey(key);
}
static HWND window(DWORD pid) {
    struct Find { DWORD pid; HWND found; } find{pid,nullptr};
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        auto *f=reinterpret_cast<Find *>(parameter); DWORD owner=0; GetWindowThreadProcessId(hwnd,&owner);
        wchar_t name[64]{}; GetClassNameW(hwnd,name,64);
        if (owner==f->pid && std::wstring(name)==rs::WindowClass) { f->found=hwnd; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&find));
    return find.found;
}
// Graceful stop (WM_CLOSE, as the uninstaller does), then wait for the host
// to release the instance lease.
static void stopOwner(Env &e) {
    const auto s=e.status();
    if (!s.ownerPid || !alive(s.ownerPid)) return;
    pipe::Handle owner(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE,FALSE,s.ownerPid));
    pipe::Handle host(s.hostPid ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE,FALSE,s.hostPid) : nullptr);
    HWND w=nullptr; waitFor([&] { return (w=window(s.ownerPid))!=nullptr; },5000);
    if (w) PostMessageW(w,WM_CLOSE,0,0);
    if (owner && WaitForSingleObject(owner.get(),15000)!=WAIT_OBJECT_0) TerminateProcess(owner.get(),9);
    if (host && WaitForSingleObject(host.get(),5000)!=WAIT_OBJECT_0) TerminateProcess(host.get(),9);
}
static std::optional<EngineRegistration::Endpoint> allocate(hf::Session &session) {
    auto &r=session.registration();
    if (!r.request(control::Operation::Allocate,pipe::now(),Timeout)) return {};
    const auto deadline=pipe::now()+Timeout;
    while (r.poll(pipe::now())==control::State::Pending) {
        auto events=r.waitHandles(); if (events.empty() || pipe::now()>=deadline) return {};
        WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,static_cast<DWORD>(deadline-pipe::now()));
    }
    auto result=r.take(pipe::now());
    if (!result || !result->endpoint) return {};
    return std::move(result->endpoint);
}
static hf::Outcome admit(const Env &e, double *verified=nullptr) {
    hf::Options o; o.discovery.scope=e.scope;
    const auto began=ms();
    if (verified) o.testHook=[&](hf::Stage stage) { if (stage==hf::Stage::Verified) *verified=ms()-began; };
    return hf::admit(e.snapshot(),o);
}
static const char *launchName(rs::Launch value) {
    static const char *names[]{"NotNeeded","Started","Pending","Failed","Backoff","Tripped","NoRelease"};
    return names[static_cast<size_t>(value)];
}

// ---------------------------------------------------------------- corpus
struct Input { size_t cls; std::string text; };
struct Corpus { std::vector<std::string> classes; std::vector<Input> inputs; };
static Corpus readCorpus(const std::filesystem::path &path) {
    Corpus c; std::ifstream in(path); require(bool(in),"corpus");
    std::string line;
    while (std::getline(in,line)) {
        if (line.empty() || line[0]=='#') continue;
        const auto tab=line.find('\t'); require(tab!=std::string::npos,"corpus line");
        const auto cls=line.substr(0,tab), text=line.substr(tab+1);
        require(!text.empty() && text.size()<=64 && std::all_of(text.begin(),text.end(),[](char ch) { return ch>='a' && ch<='z'; }),"corpus input");
        auto it=std::find(c.classes.begin(),c.classes.end(),cls);
        if (it==c.classes.end()) { c.classes.push_back(cls); it=c.classes.end()-1; }
        c.inputs.push_back({static_cast<size_t>(it-c.classes.begin()),text});
    }
    require(!c.inputs.empty(),"empty corpus");
    return c;
}
static pipe::Handle inherited(const wchar_t *text) {
    const auto handle=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(text)));
    DWORD flags=0; require(GetHandleInformation(handle,&flags) && (flags & HANDLE_FLAG_INHERIT),"inherited handle");
    SetHandleInformation(handle,HANDLE_FLAG_INHERIT,0); return pipe::Handle(handle);
}
// --worker <inproc|engine> base themes|- scope releaseKey corpus output runs start ready done release
static int worker(int argc, wchar_t **argv) {
    require(argc==14,"worker arguments");
    const std::wstring mode=argv[2];
    const std::filesystem::path base=argv[3], themes=std::wstring(argv[4])==L"-" ? L"" : argv[4];
    Env e; e.scope=argv[5]; e.releaseKey=argv[6];
    const auto corpus=readCorpus(argv[7]);
    const std::filesystem::path output=argv[8];
    const auto runs=std::stoi(argv[9]);
    auto start=inherited(argv[10]), ready=inherited(argv[11]), done=inherited(argv[12]), release=inherited(argv[13]);
    std::shared_ptr<const WindowsDictionary> dictionary;
    std::unique_ptr<ServerConnection> server;
    std::unique_ptr<hf::Session> session;
    std::unique_ptr<Client> client;
    if (mode==L"engine") {
        auto out=admit(e); require(out.result==hf::Result::Admitted,"worker admit");
        session=std::move(out.session);
        auto endpoint=allocate(*session); require(endpoint.has_value(),"worker allocate");
        client=std::make_unique<Client>(endpoint->connect());
    } else {
        require(mode==L"inproc","worker mode");
        dictionary=WindowsDictionary::load(base,themes);
        server=std::make_unique<ServerConnection>(42,dictionary);
        client=std::make_unique<Client>(std::function<Frame(const Frame &)>([&](const Frame &frame) {
            auto result=server->receive(frame,pipe::now(),pipe::now()+Timeout);
            if (result.work) { const auto page=dictionary->query(result.work); result=server->complete(result.work,page,pipe::now()); }
            require(result.response.has_value(),"local response"); return *result.response;
        }));
    }
    SetEvent(ready.get()); require(WaitForSingleObject(start.get(),120000)==WAIT_OBJECT_0,"start");
    struct Sample { uint16_t cls, key; float ms; };
    std::vector<Sample> samples; samples.reserve(corpus.inputs.size()*12*runs);
    for (int round=0;round<runs;++round) for (const auto &input : corpus.inputs) {
        client->cancel();
        for (size_t key=0;key<input.text.size();++key) {
            const auto began=ms(); client->key(KeyAction::Text,static_cast<uint8_t>(input.text[key]));
            const auto elapsed=ms()-began;
            require(client->current.input==input.text.substr(0,key+1),"prefix echoed");
            samples.push_back({static_cast<uint16_t>(input.cls),static_cast<uint16_t>(key+1),static_cast<float>(elapsed)});
        }
    }
    client->cancel();
    {
        std::ofstream out(output,std::ios::binary); require(bool(out),"worker output");
        out.write(reinterpret_cast<const char *>(samples.data()),static_cast<std::streamsize>(samples.size()*sizeof(Sample)));
        require(bool(out),"worker write");
    }
    SetEvent(done.get());
    require(WaitForSingleObject(release.get(),120000)==WAIT_OBJECT_0,"release");
    return 0;
}
struct Worker {
    pipe::Handle ready=event(), done=event(), process, job;
    std::filesystem::path output;
    Worker(const Env &e, const std::wstring &mode, const std::filesystem::path &corpus, int runs, int index, HANDLE start, HANDLE release)
            : output(e.root/(L"w"+std::to_wstring(index)+L".bin")) {
        job.reset(CreateJobObjectW(nullptr,nullptr)); require(bool(job),"job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"job limits");
        std::vector<pipe::Handle> copies;
        for (const auto handle : {start,ready.get(),done.get(),release}) {
            HANDLE raw=nullptr;
            require(DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS),"duplicate");
            copies.emplace_back(raw);
        }
        std::vector<HANDLE> handles; for (const auto &copy : copies) handles.push_back(copy.get());
        SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size); auto *attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        require(InitializeProcThreadAttributeList(attributes,1,0,&size),"attributes");
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(HANDLE)*handles.size(),nullptr,nullptr),"handle list");
        std::vector<std::wstring> args{self().wstring(),L"--worker",mode,e.base.wstring(),e.themes.empty() ? L"-" : e.themes.wstring(),
            e.scope,e.releaseKey,corpus.wstring(),output.wstring(),std::to_wstring(runs)};
        for (auto h : handles) args.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(h)));
        std::wstring command; for (const auto &arg : args) { if (!command.empty()) command+=L' '; command+=quote(arg); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.lpAttributeList=attributes;
        PROCESS_INFORMATION info{};
        require(CreateProcessW(self().c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,nullptr,&startup.StartupInfo,&info),"create worker");
        process.reset(info.hProcess); pipe::Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job.get(),process.get())) { TerminateProcess(process.get(),99); require(false,"assign job"); }
        ResumeThread(thread.get());
    }
};

// ---------------------------------------------------------------- sections
static void hashes(Env &e, int reps) {
    const auto entry=*release::select(e.snapshot(),ProtocolVersion);
    Series s;
    for (int i=0;i<reps;++i) {
        auto t=ms(); { EngineDeployment d(entry.owner,entry.ownerSha256); } s.add("pin_owner_ms",ms()-t);
        t=ms(); { EngineDeployment d(entry.host,entry.hostSha256); } s.add("pin_host_ms",ms()-t);
    }
    std::cout << "{\"type\":\"pin\",\"owner_bytes\":" << std::filesystem::file_size(entry.owner) << ",\"host_bytes\":" << std::filesystem::file_size(entry.host)
        << ",\"stats\":" << s.json() << "}\n";
}
// One on-demand start through Explorer until the first candidate page.
static void cold(Env &e, int runs, size_t threads) {
    const auto pipeName=dc::name(e.scope);
    for (int run=0;run<runs;++run) {
        require(!e.status().ownerPid || !alive(e.status().ownerPid),"owner already running");
        std::atomic<bool> stop{false}; std::atomic<uint64_t> seen{0};
        std::thread watcher([&] {
            while (!stop) {
                if (WaitNamedPipeW(pipeName.c_str(),1) || GetLastError()==ERROR_SEM_TIMEOUT) { seen=precise(); return; }
                Sleep(1);
            }
        });
        const auto t0=precise(); const auto q0=ms();
        auto out=rs::ensure(e.snapshot(),e.ensure(threads));
        const auto ensureMs=ms()-q0;
        stop=true; watcher.join();
        require(out.attach.outcome.result==hf::Result::Admitted,"cold ensure admitted");
        auto &session=*out.attach.outcome.session;
        const auto ownerPid=session.engine().ownerPid, hostPid=session.engine().hostPid;
        e.tracked.push_back(ownerPid); e.tracked.push_back(hostPid);
        auto owner=open(ownerPid), host=open(hostPid);
        auto t=ms(); auto endpoint=allocate(session); const auto allocateMs=ms()-t; require(endpoint.has_value(),"allocate");
        t=ms(); Client client(endpoint->connect()); const auto connectMs=ms()-t;
        t=ms(); client.key(KeyAction::Text,'n'); const auto firstKeyMs=ms()-t;
        std::vector<double> rest;
        for (char c : std::string("ihao")) { t=ms(); client.key(KeyAction::Text,static_cast<uint8_t>(c)); rest.push_back(ms()-t); }
        client.cancel();
        t=ms(); client.type(e.themes.empty() ? "xinkule" : "tejihuifuyao"); const auto longMs=ms()-t;
        const auto ownerMem=memory(owner.get()), hostMem=memory(host.get());
        std::cout << std::setprecision(6) << std::fixed << "{\"type\":\"cold\",\"run\":" << run << ",\"threads\":" << threads << ",\"launch\":\"" << launchName(out.launch)
            << "\",\"attempts\":" << out.attach.attempts << ",\"ensure_ms\":" << ensureMs
            << ",\"owner_created_ms\":" << since(t0,createdAt(owner.get())) << ",\"host_created_ms\":" << since(t0,createdAt(host.get()))
            << ",\"pipe_visible_ms\":" << (seen ? since(t0,seen) : -1.0) << ",\"allocate_ms\":" << allocateMs << ",\"connect_ms\":" << connectMs
            << ",\"first_key_ms\":" << firstKeyMs << ",\"next_keys_ms\":" << json(stats(rest)) << ",\"long_word_ms\":" << longMs
            << ",\"owner\":" << json(ownerMem) << ",\"host\":" << json(hostMem) << "}\n";
        client.channel.reset(); out.attach.outcome.session.reset();
        stopOwner(e);
        require(waitFor([&] { return !alive(ownerPid) && !alive(hostPid); },15000),"engine stopped");
    }
}
static std::unique_ptr<hf::Session> start(Env &e, size_t threads) {
    auto out=rs::ensure(e.snapshot(),e.ensure(threads));
    require(out.attach.outcome.result==hf::Result::Admitted,"ensure");
    e.tracked.push_back(out.attach.outcome.session->engine().ownerPid); e.tracked.push_back(out.attach.outcome.session->engine().hostPid);
    return std::move(out.attach.outcome.session);
}
// Discovery, handoff and first query against a running engine. held: another
// session of this process stays open meanwhile (as a TSF client reconnecting
// while it still has a session would).
static void handshake(Env &e, int reps, bool held) {
    std::unique_ptr<hf::Session> keep;
    if (held) { auto out=admit(e); require(out.result==hf::Result::Admitted,"held admit"); keep=std::move(out.session); }
    Series s;
    for (int i=0;i<reps;++i) {
        auto t=ms(); auto found=dc::discover(e.snapshot(),[&] { dc::Options o; o.scope=e.scope; return o; }()); s.add("discover_ms",ms()-t);
        require(found.result==dc::Result::Verified,"discover");
        found.engine.reset();
        double verified=0; t=ms(); auto out=admit(e,&verified); const auto total=ms()-t;
        require(out.result==hf::Result::Admitted,"admit");
        s.add("admit_ms",total); s.add("admit_verified_ms",verified); s.add("admit_handoff_ms",total-verified);
        t=ms(); auto endpoint=allocate(*out.session); s.add("allocate_ms",ms()-t); require(endpoint.has_value(),"allocate");
        t=ms(); Client client(endpoint->connect()); s.add("connect_ms",ms()-t);
        t=ms(); client.key(KeyAction::Text,'n'); s.add("first_key_ms",ms()-t);
        client.channel.reset(); out.session.reset();
        Sleep(20); // Let the owner retire the slot.
    }
    std::cout << "{\"type\":\"handshake\",\"held\":" << (held ? "true" : "false") << ",\"reps\":" << reps << ",\"stats\":" << s.json() << "}\n";
}
static void ledger(Env &e, int reps) {
    Series s;
    for (int i=0;i<reps;++i) {
        auto t=ms(); rs::Ledger l(e.store,e.scope+L"-probe"); s.add("open_ms",ms()-t);
        t=ms(); l.clients(static_cast<size_t>(i%8),1234); s.add("clients_write_ms",ms()-t);
        t=ms(); l.state(rs::State::Ready); s.add("state_write_ms",ms()-t);
        t=ms(); auto st=rs::inspect(e.store,e.scope); s.add("inspect_ms",ms()-t); require(st.present,"inspect");
        t=ms(); auto snap=e.snapshot(); s.add("release_read_ms",ms()-t); require(snap.status==release::Status::Ready,"release read");
    }
    std::cout << "{\"type\":\"ledger\",\"reps\":" << reps << ",\"stats\":" << s.json() << "}\n";
}
// Owner / host / this process as sessions are added (each: admit, allocate, connect).
static void sessions(Env &e) {
    const auto st=e.status(); auto owner=open(st.ownerPid), host=open(st.hostPid);
    std::vector<std::unique_ptr<hf::Session>> held; std::vector<std::unique_ptr<Client>> clients;
    auto report=[&](size_t n) {
        Sleep(200);
        std::cout << "{\"type\":\"sessions\",\"count\":" << n << ",\"owner\":" << json(memory(owner.get())) << ",\"host\":" << json(memory(host.get()))
            << ",\"client\":" << json(memory(GetCurrentProcess())) << "}\n";
    };
    report(0);
    for (size_t n=1;n<=8;++n) {
        auto out=admit(e); require(out.result==hf::Result::Admitted,"session admit");
        auto endpoint=allocate(*out.session); require(endpoint.has_value(),"session allocate");
        clients.push_back(std::make_unique<Client>(endpoint->connect())); clients.back()->type("nihao"); clients.back()->cancel();
        held.push_back(std::move(out.session));
        if (n==1 || n==2 || n==4 || n==8) report(n);
    }
    { auto out=admit(e); require(out.result==hf::Result::Full,"ninth session is Full"); }
    clients.clear(); held.clear();
    Sleep(300); report(0);
}
// Idle CPU with one session attached; then trimmed working set and the first keys afterwards.
static void idle(Env &e, int seconds, int trims) {
    const auto st=e.status(); auto owner=open(st.ownerPid), host=open(st.hostPid);
    auto out=admit(e); require(out.result==hf::Result::Admitted,"idle admit");
    auto endpoint=allocate(*out.session); require(endpoint.has_value(),"idle allocate");
    Client client(endpoint->connect()); client.type("nihao"); client.cancel();
    const auto o0=cpuMs(owner.get()), h0=cpuMs(host.get()), w0=ms();
    Sleep(static_cast<DWORD>(seconds*1000));
    const auto wall=ms()-w0;
    std::cout << std::setprecision(6) << std::fixed << "{\"type\":\"idle\",\"wall_ms\":" << wall << ",\"owner_cpu_ms\":" << cpuMs(owner.get())-o0
        << ",\"host_cpu_ms\":" << cpuMs(host.get())-h0 << ",\"owner\":" << json(memory(owner.get())) << ",\"host\":" << json(memory(host.get())) << "}\n";
    for (int i=0;i<trims;++i) {
        require(EmptyWorkingSet(host.get()) && EmptyWorkingSet(owner.get()),"trim");
        const auto trimmedHost=memory(host.get()), trimmedOwner=memory(owner.get());
        auto t=ms(); client.key(KeyAction::Text,'n'); const auto first=ms()-t;
        t=ms(); client.type("ihao"); const auto rest=ms()-t; client.cancel();
        t=ms(); client.type(e.themes.empty() ? "xinkule" : "tejihuifuyao"); const auto longMs=ms()-t; client.cancel();
        std::cout << "{\"type\":\"trim\",\"rep\":" << i << ",\"first_key_ms\":" << first << ",\"rest_ms\":" << rest << ",\"long_word_ms\":" << longMs
            << ",\"host_trimmed\":" << json(trimmedHost) << ",\"owner_trimmed\":" << json(trimmedOwner)
            << ",\"host_after\":" << json(memory(host.get())) << "}\n";
        Sleep(500);
    }
}
static void keys(Env &e, const Corpus &corpus, const std::filesystem::path &corpusPath, const std::wstring &mode, size_t threads, int count, int runs) {
    auto go=event(), release=event();
    std::vector<std::unique_ptr<Worker>> workers;
    const auto began=ms();
    for (int i=0;i<count;++i) workers.push_back(std::make_unique<Worker>(e,mode,corpusPath,runs,i,go.get(),release.get()));
    for (auto &w : workers) {
        HANDLE events[2]{w->ready.get(),w->process.get()};
        require(WaitForMultipleObjects(2,events,FALSE,180000)==WAIT_OBJECT_0,"worker ready");
    }
    const auto prepareMs=ms()-began;
    std::optional<pipe::Handle> owner, host;
    if (mode==L"engine") { const auto st=e.status(); owner=open(st.ownerPid); host=open(st.hostPid); }
    auto sweep=[&] {
        uint64_t clients=0, o=0, h=0;
        for (auto &w : workers) clients+=memory(w->process.get()).priv;
        if (owner) o=memory(owner->get()).priv;
        if (host) h=memory(host->get()).priv;
        std::ostringstream s; s << "{\"clients\":" << clients << ",\"owner\":" << o << ",\"host\":" << h << ",\"total\":" << clients+o+h << '}'; return s.str();
    };
    const auto ready=sweep();
    const double hostCpu0=host ? cpuMs(host->get()) : 0;
    const auto w0=ms(); SetEvent(go.get());
    for (auto &w : workers) {
        HANDLE events[2]{w->done.get(),w->process.get()};
        require(WaitForMultipleObjects(2,events,FALSE,600000)==WAIT_OBJECT_0,"worker done");
    }
    const auto wall=ms()-w0; const auto held=sweep();
    const double hostCpu=host ? cpuMs(host->get())-hostCpu0 : 0;
    SetEvent(release.get());
    for (auto &w : workers) {
        require(WaitForSingleObject(w->process.get(),30000)==WAIT_OBJECT_0,"worker exit");
        DWORD code=1; GetExitCodeProcess(w->process.get(),&code); require(code==0,"worker exit code");
    }
    struct Sample { uint16_t cls, key; float ms; };
    std::vector<double> all; std::vector<std::vector<double>> byClass(corpus.classes.size());
    std::map<std::string,std::vector<double>> byPosition;
    for (auto &w : workers) {
        std::ifstream in(w->output,std::ios::binary); require(bool(in),"worker samples");
        Sample s{};
        while (in.read(reinterpret_cast<char *>(&s),sizeof(s))) {
            require(s.cls<corpus.classes.size() && s.key>=1,"sample");
            all.push_back(s.ms); byClass[s.cls].push_back(s.ms);
            byPosition[s.key<=4 ? "01-04" : s.key<=8 ? "05-08" : s.key<=16 ? "09-16" : "17+"].push_back(s.ms);
        }
        std::error_code error; std::filesystem::remove(w->output,error);
    }
    size_t expected=0; for (const auto &input : corpus.inputs) expected+=input.text.size();
    require(all.size()==expected*runs*count,"sample count");
    std::cout << std::setprecision(6) << std::fixed << "{\"type\":\"keys\",\"mode\":\"" << narrow(mode) << "\",\"threads\":" << threads << ",\"clients\":" << count
        << ",\"runs\":" << runs << ",\"prepare_ms\":" << prepareMs << ",\"wall_ms\":" << wall << ",\"host_cpu_ms\":" << hostCpu
        << ",\"memory\":{\"ready\":" << ready << ",\"held\":" << held << "},\"overall\":" << json(stats(all)) << ",\"classes\":{";
    for (size_t i=0;i<corpus.classes.size();++i) std::cout << (i ? "," : "") << '"' << corpus.classes[i] << "\":" << json(stats(byClass[i]));
    std::cout << "},\"positions\":{"; bool first=true;
    for (const auto &[name,v] : byPosition) { std::cout << (first ? "" : ",") << '"' << name << "\":" << json(stats(v)); first=false; }
    std::cout << "}}\n";
}

static std::vector<int> list(const std::wstring &text) {
    std::vector<int> out; std::wstringstream in(text); std::wstring item;
    while (std::getline(in,item,L',')) { require(!item.empty() && item.size()<3 && item.find_first_not_of(L"0123456789")==std::wstring::npos,"list"); out.push_back(std::stoi(item)); }
    return out;
}
// harukoto_engine_cost_benchmark OWNER_TEST BASE THEMES|- CORPUS COLD HANDSHAKE RUNS COUNTS THREADS [SECTIONS]
// COUNTS, THREADS: comma lists (clients 1..8; host worker threads 1..16; 0 = owner default).
int wmain(int argc, wchar_t **argv) {
    std::cout << std::unitbuf;
    if (argc>1 && std::wstring(argv[1])==L"--worker") return worker(argc,argv);
    if (argc!=10 && argc!=11) { std::cerr << "usage: harukoto_engine_cost_benchmark OWNER_TEST BASE THEMES|- CORPUS COLD HANDSHAKE RUNS COUNTS THREADS [cold,warm,keys]\n"; return 2; }
    Env e;
    const std::filesystem::path ownerBuild=argv[1], hostBuild=ownerBuild.parent_path()/L"harukoto_engine_host.exe";
    e.base=argv[2]; if (std::wstring(argv[3])!=L"-") e.themes=argv[3];
    const std::filesystem::path corpusPath=argv[4];
    const int coldRuns=std::stoi(argv[5]), reps=std::stoi(argv[6]), runs=std::stoi(argv[7]);
    const auto counts=list(argv[8]), threadList=list(argv[9]);
    const std::wstring sections=argc==11 ? argv[10] : L"cold,warm,keys";
    require(coldRuns>=0 && coldRuns<=50 && reps>=1 && reps<=1000 && runs>=1 && runs<=10,"arguments");
    for (auto n : counts) require(n>=1 && n<=8,"client count");
    for (auto n : threadList) require(n>=0 && n<=16,"thread count");
    const auto corpus=readCorpus(corpusPath);
    std::wstring temp(MAX_PATH+1,L'\0'); temp.resize(GetTempPathW(static_cast<DWORD>(temp.size()),temp.data()));
    std::wstring longTemp(32768,L'\0'); longTemp.resize(GetLongPathNameW(temp.c_str(),longTemp.data(),static_cast<DWORD>(longTemp.size())));
    require(!longTemp.empty(),"temp path");
    const auto unique=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    e.root=std::filesystem::path(longTemp)/(L"harukoto-cost-"+unique);
    const auto id=L"cost-"+std::to_wstring(GetCurrentProcessId());
    e.version=e.root/id; std::filesystem::create_directories(e.version);
    e.owner=e.version/L"harukoto_engine_owner.exe"; e.host=e.version/L"harukoto_engine_host.exe";
    std::filesystem::copy_file(ownerBuild,e.owner); std::filesystem::copy_file(hostBuild,e.host);
    e.testKey=L"Software\\HarukotoTest-cost-"+unique; e.releaseKey=e.testKey+L"\\EngineReleases";
    e.scope=L"cost-"+unique; e.store=rs::Store::under(e.testKey);
    Cleanup cleanup(e);
    writeRelease(e,id);
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    MEMORYSTATUSEX physical{}; physical.dwLength=sizeof(physical); GlobalMemoryStatusEx(&physical);
    std::cout << "{\"type\":\"setup\",\"logical_cpus\":" << info.dwNumberOfProcessors << ",\"physical_memory\":" << physical.ullTotalPhys
        << ",\"corpus_inputs\":" << corpus.inputs.size() << ",\"themes\":" << (e.themes.empty() ? "false" : "true") << "}\n";
    hashes(e,reps);
    if (sections.find(L"cold")!=std::wstring::npos) cold(e,coldRuns,0);
    if (sections.find(L"warm")!=std::wstring::npos) {
        auto first=start(e,0);
        handshake(e,reps,false);
        handshake(e,reps,true);
        ledger(e,reps);
        first.reset();
        sessions(e);
        idle(e,10,5);
        stopOwner(e);
    }
    if (sections.find(L"keys")!=std::wstring::npos) {
        for (auto n : counts) keys(e,corpus,corpusPath,L"inproc",0,n,runs);
        for (auto threads : threadList) {
            auto held=start(e,static_cast<size_t>(threads)); held.reset();
            for (auto n : counts) keys(e,corpus,corpusPath,L"engine",static_cast<size_t>(threads),n,runs);
            stopOwner(e);
        }
    }
    std::cout << "{\"type\":\"done\"}\n";
    return 0;
}
