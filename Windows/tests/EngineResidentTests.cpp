#include "../EngineOwner.h"
#include "../EngineResident.h"
#include "../BaseDictionaryFiles.h"
#include "EngineHostTestClient.h"
#include <fstream>
#include <csignal>
#include <tlhelp32.h>

// WIN-03 step 4: resident lifecycle. Real resident owners (the test build of
// the owner, copied with the host into a temporary version directory and
// approved through an HKCU test release key) started through the real
// session Explorer (the broker), real starter child processes in
// kill-on-close Jobs without breakaway, crash / host-crash / breaker / idle /
// session-end / installer-stop paths. No HKLM, no Run key, no scheduled task,
// no COM registration; every owner this test started is terminated if still
// running, the HKCU test key and the directory are removed.
using namespace host_test;
namespace rs=harukoto::engine::resident;
namespace hf=harukoto::engine::handoff;
using Result=hf::Result;

static std::vector<DWORD> tracked; // Owner and host PIDs to reap at the end.
static std::filesystem::path self() {
    std::wstring value(32768,L'\0'); const auto size=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    assert(size && size<value.size()); value.resize(size); return value;
}
static std::wstring wide(const std::string &value) { return std::wstring(value.begin(),value.end()); }
static const char *launchName(rs::Launch value) {
    static const char *names[]{"NotNeeded","Started","Pending","Failed","Backoff","Tripped","NoRelease"};
    return names[static_cast<size_t>(value)];
}
static const char *resultName(Result result) {
    static const char *names[]{"Admitted","NotRunning","Busy","Full","Closing","Unavailable","Timeout","Stale","Duplicate",
        "Rejected","Protocol","Untrusted","Failed"};
    return names[static_cast<size_t>(result)];
}
template<class F> static bool waitFor(F condition, uint64_t ms) {
    const auto deadline=ep::now()+ms;
    for (;;) { if (condition()) return true; if (ep::now()>=deadline) return false; Sleep(25); }
}
static bool alive(DWORD pid) {
    ep::Handle process(OpenProcess(SYNCHRONIZE,FALSE,pid));
    return process && WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT;
}
static DWORD exitCode(HANDLE process, DWORD ms) {
    assert(WaitForSingleObject(process,ms)==WAIT_OBJECT_0);
    DWORD code=0; ep::check(GetExitCodeProcess(process,&code)); return code;
}
static ep::Handle open(DWORD pid) {
    ep::Handle process(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)); assert(process); return process;
}
static void kill(DWORD pid) {
    auto process=open(pid); ep::check(TerminateProcess(process.get(),9));
    assert(WaitForSingleObject(process.get(),static_cast<DWORD>(Timeout))==WAIT_OBJECT_0);
}
static std::wstring quote(const std::wstring &value) {
    std::wstring output=L"\""; size_t slashes=0;
    for (auto c : value) {
        if (c==L'\\') { ++slashes; continue; }
        output.append(c==L'\"' ? slashes*2+1 : slashes,L'\\'); slashes=0; output+=c;
    }
    output.append(slashes*2,L'\\'); return output+L"\"";
}

// ---------------------------------------------------------------- fixture
struct Env {
    std::filesystem::path root, version, owner, host, base, themes, personal;
    std::wstring testKey, releaseKey, badReleaseKey, scope;
    rs::Store store;
    release::Snapshot snapshot() const { return release::read({HKEY_CURRENT_USER,releaseKey,false}); }
    release::Entry entry() const { auto approved=release::select(snapshot(),ProtocolVersion); assert(approved); return *approved; }
    std::vector<std::wstring> arguments(const std::wstring &release={}, std::vector<std::wstring> extra={}) const {
        std::vector<std::wstring> result{L"--scope",scope,L"--release-subkey",release.empty() ? releaseKey : release,L"--store-subkey",testKey,
            L"--base",base.wstring(),L"--themes",themes.wstring(),L"--personal",personal.wstring()};
        result.insert(result.end(),extra.begin(),extra.end()); return result;
    }
    rs::EnsureOptions ensure(rs::Policy policy=rs::Policy{}, std::vector<std::wstring> extra={}, const std::wstring &release={}) const {
        rs::EnsureOptions options; options.handoff.discovery.scope=scope; options.budgetMs=60000; options.policy=policy;
        options.store=store; options.ownerArguments=arguments(release,std::move(extra)); return options;
    }
    rs::Status status() const { return rs::inspect(store,scope); }
};
static void writeRelease(const std::wstring &subkey, const std::wstring &id, const Env &env, const std::string &ownerSha, const std::string &hostSha) {
    std::wstring value;
    for (const auto &line : {std::wstring(L"format=harukoto-engine-release/1"),L"id="+id,std::wstring(L"sequence=1"),
            L"protocol="+std::to_wstring(ProtocolVersion),L"host="+env.host.wstring(),L"host-sha256="+wide(hostSha),
            L"owner="+env.owner.wstring(),L"owner-sha256="+wide(ownerSha)})
        value+=line+L'\0';
    value+=L'\0';
    HKEY key=nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER,subkey.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS);
    assert(RegSetValueExW(key,id.c_str(),0,REG_MULTI_SZ,reinterpret_cast<const BYTE *>(value.data()),static_cast<DWORD>(value.size()*sizeof(wchar_t)))==ERROR_SUCCESS);
    RegCloseKey(key);
}
// Owners are started outside this process tree on purpose, so nothing reaps
// them for us: the destructor and a failed assertion (abort) both do.
static void reap(Env &env);
static Env *active=nullptr;
struct Cleanup {
    Env &env;
    explicit Cleanup(Env &e) : env(e) { active=&e; signal(SIGABRT,[](int) { if (auto *e=active) { active=nullptr; reap(*e); } }); }
    ~Cleanup() { if (active) { active=nullptr; reap(env); } }
};
static void reap(Env &env) {
    {
        const auto last=env.status(); if (last.ownerPid) tracked.push_back(last.ownerPid); if (last.hostPid) tracked.push_back(last.hostPid);
        for (auto pid : tracked) if (alive(pid)) {
            ep::Handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,FALSE,pid));
            if (process) { TerminateProcess(process.get(),9); WaitForSingleObject(process.get(),5000); }
        }
        RegDeleteTreeW(HKEY_CURRENT_USER,env.testKey.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER,env.testKey.c_str());
        for (int i=0;i<50;++i) {
            std::error_code error; std::filesystem::remove_all(env.root,error);
            if (!std::filesystem::exists(env.root,error)) return;
            Sleep(100);
        }
        std::cout << "warning: could not remove temporary directory\n";
    }
}

// ---------------------------------------------------------------- jobs
class Job {
    ep::Handle job_{CreateJobObjectW(nullptr,nullptr)};
public:
    explicit Job(DWORD extra) {
        ep::check(bool(job_));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | extra;
        ep::check(SetInformationJobObject(job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
    }
    ~Job() { close(); }
    // Closing the last handle of a kill-on-close Job is what happens when
    // a game or browser that put its children in such a Job exits.
    void close() {
        if (!job_) return;
        std::vector<ep::Handle> members;
        std::vector<uint8_t> bytes(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)+64*sizeof(ULONG_PTR));
        auto *list=reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(bytes.data());
        if (QueryInformationJobObject(job_.get(),JobObjectBasicProcessIdList,list,static_cast<DWORD>(bytes.size()),nullptr))
            for (DWORD i=0;i<list->NumberOfProcessIdsInList;++i) members.emplace_back(OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(list->ProcessIdList[i])));
        job_.reset();
        for (auto &member : members) if (member) assert(WaitForSingleObject(member.get(),static_cast<DWORD>(Timeout))==WAIT_OBJECT_0);
    }
    ep::Handle start(const std::vector<std::wstring> &arguments, std::vector<HANDLE> inherit={}) const {
        std::wstring command=quote(self().wstring());
        for (const auto &argument : arguments) command+=L" "+quote(argument);
        SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size);
        auto *attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        ep::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        if (!inherit.empty())
            ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherit.data(),inherit.size()*sizeof(HANDLE),nullptr,nullptr));
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.lpAttributeList=attributes;
        PROCESS_INFORMATION info{};
        ep::check(CreateProcessW(self().c_str(),command.data(),nullptr,nullptr,!inherit.empty(),
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,nullptr,&startup.StartupInfo,&info));
        ep::Handle process(info.hProcess), thread(info.hThread);
        try { ep::check(AssignProcessToJobObject(job_.get(),process.get())); } catch (...) { TerminateProcess(process.get(),1); throw; }
        if (ResumeThread(thread.get())==DWORD(-1)) { TerminateProcess(process.get(),1); throw ep::Error(ep::Failure::Io,GetLastError()); }
        return process;
    }
};

// ---------------------------------------------------------------- client child
// --client <go handle|0> <report> <scope> <testKey> <releaseKey> <base> <themes> <personal>
// ensure() from inside the Job, Inspect over the slot, report, then wait to be killed.
struct Report { rs::Launch launch=rs::Launch::Failed; Result result=Result::Failed; DWORD owner=0; bool usable=false; };
static Report readReport(const std::filesystem::path &path) {
    Report r; int launch=0, result=0; std::ifstream in(path); assert(in >> launch >> result >> r.owner >> r.usable);
    r.launch=static_cast<rs::Launch>(launch); r.result=static_cast<Result>(result); return r;
}
static bool inspect(hf::Session &session) {
    auto &registration=session.registration();
    if (!registration.request(control::Operation::Inspect,ep::now(),Timeout)) return false;
    const auto deadline=ep::now()+Timeout;
    while (registration.poll(ep::now())==control::State::Pending) {
        auto events=registration.waitHandles(); if (events.empty() || ep::now()>=deadline) break;
        WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,static_cast<DWORD>(deadline-ep::now()));
    }
    auto result=registration.take(ep::now());
    return result && result->metadata.code==control::Code::Ok;
}
static int clientMode(wchar_t **argv) {
    const auto go=std::stoull(argv[2]);
    const std::filesystem::path report=argv[3];
    Env env; env.scope=argv[4]; env.testKey=argv[5]; env.releaseKey=argv[6]; env.base=argv[7]; env.themes=argv[8]; env.personal=argv[9];
    env.store=rs::Store::under(env.testKey);
    if (go) { ep::Handle event(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(go))); assert(WaitForSingleObject(event.get(),static_cast<DWORD>(Timeout))==WAIT_OBJECT_0); }
    auto outcome=rs::ensure(env.snapshot(),env.ensure());
    auto &session=outcome.attach.outcome.session;
    const bool usable=session && inspect(*session);
    {
        std::ofstream out(report.wstring()+L".tmp");
        out << static_cast<int>(outcome.launch) << ' ' << static_cast<int>(outcome.attach.outcome.result) << ' '
            << (session ? session->engine().ownerPid : 0) << ' ' << usable << '\n';
    }
    std::filesystem::rename(report.wstring()+L".tmp",report);
    Sleep(120000); // Killed with its Job.
    return 0;
}
static std::vector<std::wstring> clientArguments(const Env &env, const std::filesystem::path &report, HANDLE go=nullptr) {
    return {L"--client",std::to_wstring(reinterpret_cast<uintptr_t>(go)),report.wstring(),env.scope,env.testKey,env.releaseKey,
        env.base.wstring(),env.themes.wstring(),env.personal.wstring()};
}
static Report runClient(const Env &env, Job &job, const wchar_t *name) {
    const auto report=env.root/name;
    auto process=job.start(clientArguments(env,report));
    assert(waitFor([&] { return std::filesystem::exists(report) || WaitForSingleObject(process.get(),0)==WAIT_OBJECT_0; },60000));
    assert(std::filesystem::exists(report));
    auto r=readReport(report);
    if (r.owner) tracked.push_back(r.owner);
    return r;
}

// ---------------------------------------------------------------- helpers
static hf::Outcome admit(const Env &env) { hf::Options options; options.discovery.scope=env.scope; return hf::admit(env.snapshot(),options); }
static rs::EnsureOutcome ensure(const Env &env, rs::Policy policy=rs::Policy{}, std::vector<std::wstring> extra={}) {
    auto out=rs::ensure(env.snapshot(),env.ensure(policy,std::move(extra)));
    std::cout << "  ensure: launch=" << launchName(out.launch) << " hr=0x" << std::hex << static_cast<uint32_t>(out.hr) << std::dec
        << " result=" << resultName(out.attach.outcome.result) << '\n';
    if (out.attach.outcome.session) tracked.push_back(out.attach.outcome.session->engine().ownerPid);
    if (const auto pid=env.status().ownerPid) tracked.push_back(pid);
    return out;
}
// Direct start of the owner (not through COM), for exit-code checks.
static ep::Handle direct(const Env &env, const std::wstring &release={}) {
    auto line=quote(env.owner.wstring())+L" --resident demand";
    for (const auto &argument : env.arguments(release)) line+=L" "+quote(argument);
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION info{};
    ep::check(CreateProcessW(env.owner.c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info));
    ep::Handle process(info.hProcess), thread(info.hThread); tracked.push_back(info.dwProcessId); return process;
}
static DWORD ownerOf(const rs::EnsureOutcome &out) { assert(out.attach.outcome.session); return out.attach.outcome.session->engine().ownerPid; }
static DWORD readyHost(const Env &env, DWORD owner, DWORD previous=0) {
    DWORD host=0;
    assert(waitFor([&] { const auto s=env.status(); host=s.hostPid; return s.ownerPid==owner && s.state==rs::State::Ready && host && host!=previous; },30000));
    tracked.push_back(host); return host;
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
static HWND waitWindow(DWORD pid) { HWND w=nullptr; assert(waitFor([&] { return (w=window(pid))!=nullptr; },10000)); return w; }
static DWORD run(const std::wstring &command, std::string *output=nullptr) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE readRaw=nullptr, writeRaw=nullptr; ep::check(CreatePipe(&readRaw,&writeRaw,&inherit,0));
    ep::Handle reader(readRaw), writer(writeRaw); ep::check(SetHandleInformation(reader.get(),HANDLE_FLAG_INHERIT,0));
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=writer.get(); startup.hStdError=writer.get(); startup.hStdInput=nullptr;
    PROCESS_INFORMATION info{}; auto line=command;
    ep::check(CreateProcessW(nullptr,line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info));
    ep::Handle process(info.hProcess), thread(info.hThread); writer.reset();
    std::string text; char buffer[4096]; DWORD count=0;
    while (ReadFile(reader.get(),buffer,sizeof(buffer),&count,nullptr) && count) text.append(buffer,count);
    const auto code=exitCode(process.get(),120000);
    if (output) *output=text; else std::cout << text;
    return code;
}
static std::wstring env(const wchar_t *name) {
    std::wstring value(32768,L'\0'); const auto size=GetEnvironmentVariableW(name,value.data(),static_cast<DWORD>(value.size()));
    value.resize(size<value.size() ? size : 0); return value;
}

// ---------------------------------------------------------------- unit checks
static void units(const Env &env) {
    rs::Policy p;
    using G=rs::Gate;
    assert(rs::decide(p,{},false,100000).gate==G::Allow);
    assert(rs::decide(p,{},true,100000).gate==G::Tripped);
    auto d=rs::decide(p,{100000},false,101000); assert(d.gate==G::Backoff && d.retryAt==102000 && d.recent==1);
    assert(rs::decide(p,{100000},false,102000).gate==G::Allow);
    d=rs::decide(p,{100000,103000},false,110000); assert(d.gate==G::Backoff && d.retryAt==113000);
    assert(rs::decide(p,{100000,103000},false,113000).gate==G::Allow);
    assert(rs::decide(p,{100000,103000,120000},false,130000).gate==G::Tripped);
    d=rs::decide(p,{100000,103000,120000},false,100000+p.windowMs+1); assert(d.gate==G::Allow && d.recent==2); // Oldest aged out.
    assert(rs::decide(p,{100000},false,100000+p.windowMs).gate==G::Allow);  // Aged out.
    assert(rs::decide(p,{200000},false,100000).gate==G::Allow);             // Future tick (other boot) ignored.
    std::deque<uint64_t> restarts;
    assert(rs::hostRestart(p,restarts,1000)==1000 && rs::hostRestart(p,restarts,2000)==2000 && rs::hostRestart(p,restarts,3000)==4000);
    assert(!rs::hostRestart(p,restarts,4000) && rs::hostRestart(p,restarts,1000+p.windowMs)==4000);
    { rs::EnsureOptions defaults; assert(defaults.startWaitMs<=defaults.budgetMs && rs::HostThreads==admission::Limit && rs::KeyDeadlineMs==1000); }
    assert(rs::clampIdle(0)==0 && rs::clampIdle(1)==5 && rs::clampIdle(30)==30 && rs::clampIdle(5000)==1440);
    assert(rs::idleMinutes(env.store)==0); // Missing value: never.
    {
        HKEY key=nullptr; assert(RegCreateKeyExW(HKEY_CURRENT_USER,env.store.settings.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS);
        for (auto [stored,expected] : {std::pair<DWORD,uint32_t>{0,0},{3,5},{45,45},{99999,1440}}) {
            assert(RegSetValueExW(key,L"IdleMinutes",0,REG_DWORD,reinterpret_cast<const BYTE *>(&stored),sizeof(stored))==ERROR_SUCCESS);
            assert(rs::idleMinutes(env.store)==expected);
        }
        const wchar_t text[]=L"30"; // Wrong type: default.
        assert(RegSetValueExW(key,L"IdleMinutes",0,REG_SZ,reinterpret_cast<const BYTE *>(text),sizeof(text))==ERROR_SUCCESS);
        assert(rs::idleMinutes(env.store)==0);
        RegDeleteValueW(key,L"IdleMinutes"); RegCloseKey(key);
    }
    const auto name=rs::sessionName(env.scope);
    assert(name.size()==24+1+env.scope.size() && name.substr(25)==env.scope);
    {
        rs::Ledger ledger(env.store,env.scope);
        // Volatile: a persistent child cannot be created under it.
        HKEY key=nullptr, child=nullptr;
        assert(RegOpenKeyExW(HKEY_CURRENT_USER,(env.store.state+L"\\"+name).c_str(),0,KEY_CREATE_SUB_KEY,&key)==ERROR_SUCCESS);
        assert(RegCreateKeyExW(key,L"probe",0,nullptr,REG_OPTION_NON_VOLATILE,KEY_READ,nullptr,&child,nullptr)==ERROR_CHILD_MUST_BE_VOLATILE);
        RegCloseKey(key);
        // The settings key next to it stays persistent.
        assert(RegOpenKeyExW(HKEY_CURRENT_USER,env.testKey.c_str(),0,KEY_CREATE_SUB_KEY,&key)==ERROR_SUCCESS);
        assert(RegCreateKeyExW(key,L"probe",0,nullptr,REG_OPTION_NON_VOLATILE,KEY_READ,nullptr,&child,nullptr)==ERROR_SUCCESS);
        RegCloseKey(child); RegDeleteKeyW(key,L"probe"); RegCloseKey(key);
        assert(ledger.starts({1,2,3,4,5,6,7,8,9,10}) && ledger.read().starts.size()==8 && ledger.read().starts.front()==3);
        assert(ledger.trip(77) && ledger.read().tripped && ledger.read().trippedTick==77);
        assert(rs::reset(env.store,env.scope) && !ledger.read().tripped && ledger.read().starts.empty());
    }
    assert(!rs::inspect(env.store,L"never-used").present);
    { // A tampered approval never reaches the shell.
        auto bad=env.entry(); bad.ownerSha256=std::string(64,'0');
        assert(rs::start(bad,ep::now()+Timeout,env.scope,env.arguments()).result==rs::Launch::Failed);
    }
    std::cout << "unit checks passed\n";
}

// ---------------------------------------------------------------- real processes
static void real(Env &e) {
    std::cout << "[not approved] the owner checks the release it reads itself\n";
    {
        auto process=direct(e,e.badReleaseKey);
        assert(exitCode(process.get(),static_cast<DWORD>(Timeout))==rs::ExitNotApproved);
        assert(!e.status().ownerPid); // Never got as far as the ledger.
        // Through the broker: the shell starts it, it exits, nothing ever answers.
        auto options=e.ensure(rs::Policy{},{},e.badReleaseKey); options.budgetMs=6000; options.startWaitMs=3000;
        auto out=rs::ensure(e.snapshot(),options);
        std::cout << "  unapproved via broker: launch=" << launchName(out.launch) << " result=" << resultName(out.attach.outcome.result) << '\n';
        assert(out.launch==rs::Launch::Pending && out.attach.outcome.result==Result::NotRunning && !e.status().ownerPid);
        assert(rs::reset(e.store,e.scope));
        assert(admit(e).result==Result::NotRunning);
    }
    std::cout << "[restrictive Job] a starter in a kill-on-close Job without breakaway starts the owner through the shell\n";
    DWORD owner=0;
    {
        Job job(0);
        const auto r=runClient(e,job,L"demand.txt");
        std::cout << "  launch=" << launchName(r.launch) << " result=" << resultName(r.result) << " owner=" << r.owner << '\n';
        assert(r.launch==rs::Launch::Started && r.result==Result::Admitted && r.usable && r.owner);
        owner=r.owner;
        // Explorer's child, not the starter's. (Explorer may keep its children
        // in a Job of its own; what matters is that it is not the starter's.)
        job.close(); // The starter, and only the starter, dies with its Job.
    }
    assert(alive(owner));
    auto host=readyHost(e,owner);
    { // The resident host runs one query worker per client slot (ENGINE_COST.md).
        size_t threads=0; ep::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0)); assert(snapshot);
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL more=Thread32First(snapshot.get(),&entry);more;more=Thread32Next(snapshot.get(),&entry)) threads+=entry.th32OwnerProcessID==host;
        std::cout << "  host threads: " << threads << '\n';
        assert(threads>rs::HostThreads);
    }
    assert(waitFor([&] { return e.status().clients==0; },Timeout));
    {
        auto s=e.status(); assert(s.alive && s.reason==rs::Reason::Demand && s.starts.size()==1 && !s.tripped);
        auto again=admit(e); assert(again.result==Result::Admitted && again.session->engine().ownerPid==owner && inspect(*again.session));
        assert(waitFor([&] { return e.status().clients==1; },Timeout));
    }
    std::cout << "[client in restrictive Job] attaches, dies with its Job, owner keeps serving\n";
    {
        Job job(0);
        const auto r=runClient(e,job,L"attached.txt");
        assert(r.launch==rs::Launch::NotNeeded && r.result==Result::Admitted && r.owner==owner && r.usable);
        job.close();
        assert(alive(owner) && alive(host));
        auto again=admit(e); assert(again.result==Result::Admitted && again.session->engine().ownerPid==owner && inspect(*again.session));
        assert(e.status().starts.size()==1);
    }
    std::cout << "[owner crash] clients see Closed; next ensure restarts once for 4 concurrent starters\n";
    {
        auto held=admit(e); assert(held.result==Result::Admitted);
        auto &registration=held.session->registration();
        assert(registration.request(control::Operation::Allocate,ep::now(),Timeout));
        while (registration.poll(ep::now())==control::State::Pending) wait(registration.waitHandles(),ep::now()+Timeout);
        auto allocated=registration.take(ep::now()); assert(allocated && allocated->endpoint);
        Client client(allocated->endpoint->connect());
        client.type("nihao"); find(client.current,"你好");
        kill(owner);
        assert(waitFor([&] { return !alive(host); },Timeout));
        assert(registration.poll(ep::now())==control::State::Failed); // Closed: the caller falls back in-process.
        { // The pending composition is gone with the host, never replayed elsewhere.
            const auto deadline=ep::now()+Timeout;
            if (client.channel->beginRead(ep::now(),deadline))
                while (!client.channel->closed() && ep::now()<deadline) {
                    client.channel->poll(ep::now()); assert(!client.channel->take());
                    if (!client.channel->closed() && !client.channel->needsPoll()) WaitForSingleObject(client.channel->readEvent(),50);
                }
            assert(client.channel->closed());
        }
        auto s=e.status(); assert(!s.alive && s.state!=rs::State::Exited); // No exit record: it crashed.
        Job job(JOB_OBJECT_LIMIT_BREAKAWAY_OK);
        ep::Handle go(CreateEventW(nullptr,TRUE,FALSE,nullptr)); ep::check(bool(go));
        HANDLE raw=nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),go.get(),GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
        ep::Handle childGo(raw);
        std::vector<ep::Handle> children;
        for (int i=0;i<4;++i) children.push_back(job.start(clientArguments(e,e.root/(L"race"+std::to_wstring(i)+L".txt"),childGo.get()),{childGo.get()}));
        Sleep(2100); // Past the 2 s spacing after the first on-demand start.
        ep::check(SetEvent(go.get()));
        int started=0; DWORD next=0;
        for (int i=0;i<4;++i) {
            const auto report=e.root/(L"race"+std::to_wstring(i)+L".txt");
            assert(waitFor([&] { return std::filesystem::exists(report); },60000));
            const auto r=readReport(report); if (r.owner) tracked.push_back(r.owner);
            std::cout << "  starter " << i << ": launch=" << launchName(r.launch) << " result=" << resultName(r.result) << " owner=" << r.owner << '\n';
            assert(r.result==Result::Admitted && r.usable && r.owner && r.owner!=owner && (!next || next==r.owner));
            next=r.owner; started+=r.launch==rs::Launch::Started;
            assert(r.launch==rs::Launch::Started || r.launch==rs::Launch::NotNeeded);
        }
        assert(started==1 && e.status().starts.size()==2);
        job.close();
        owner=next; host=readyHost(e,owner);
        auto fresh=admit(e); assert(fresh.result==Result::Admitted);
        auto &r2=fresh.session->registration();
        assert(r2.request(control::Operation::Allocate,ep::now(),Timeout));
        while (r2.poll(ep::now())==control::State::Pending) wait(r2.waitHandles(),ep::now()+Timeout);
        auto endpoint=r2.take(ep::now()); assert(endpoint && endpoint->endpoint);
        Client second(endpoint->endpoint->connect());
        assert(second.key(KeyAction::Refresh).input.empty()); // Fresh composition on the new host.
        second.type("nihao"); assert(second.choose(find(second.current,"你好")).text=="你好");
    }
    std::cout << "[breaker] spacing, trip, persistence, reset\n";
    {
        // Spacing longer than one start + kill, so the second try is too soon.
        rs::Policy tight; tight.maxStarts=2; tight.spacingMs={0,8000,8000};
        assert(rs::reset(e.store,e.scope));
        kill(owner);
        auto a=ensure(e,tight); assert(a.launch==rs::Launch::Started && a.attach.outcome.result==Result::Admitted);
        kill(ownerOf(a));
        auto b=ensure(e,tight); assert(b.launch==rs::Launch::Backoff && b.attach.outcome.result==Result::NotRunning);
        assert(e.status().starts.size()==1);
        Sleep(8100);
        auto c=ensure(e,tight); assert(c.launch==rs::Launch::Started && c.attach.outcome.result==Result::Admitted);
        kill(ownerOf(c));
        auto d=ensure(e,tight); // Two recent starts: the third trips at once. assert(d.launch==rs::Launch::Tripped && d.attach.outcome.result==Result::NotRunning);
        auto f=ensure(e,tight); assert(f.launch==rs::Launch::Tripped);
        assert(e.status().tripped && admit(e).result==Result::NotRunning);
        assert(rs::reset(e.store,e.scope));
        auto g=ensure(e,tight); assert(g.launch==rs::Launch::Started && g.attach.outcome.result==Result::Admitted);
        owner=ownerOf(g); host=readyHost(e,owner);
    }
    std::cout << "[host crash] owner restarts the host 3 times, then exits as a crash loop\n";
    {
        auto ownerProcess=open(owner);
        for (DWORD i=1;i<=3;++i) {
            kill(host);
            host=readyHost(e,owner,host);
            assert(e.status().hostRestarts==i && alive(owner));
            auto again=admit(e); assert(again.result==Result::Admitted && again.session->engine().ownerPid==owner && inspect(*again.session));
        }
        kill(host);
        assert(exitCode(ownerProcess.get(),30000)==rs::ExitCrashLoop);
        auto s=e.status(); assert(s.lastExit==rs::Exit::CrashLoop && s.tripped && s.state==rs::State::Exited && !s.alive);
        auto blocked=ensure(e); assert(blocked.launch==rs::Launch::Tripped && blocked.attach.outcome.result==Result::NotRunning);
        assert(rs::reset(e.store,e.scope));
    }
    std::cout << "[idle] exits after the idle time with no clients; resume restarts the clock\n";
    {
        auto a=ensure(e,rs::Policy{},{L"--idle-ms",L"3000"}); assert(a.launch==rs::Launch::Started);
        owner=ownerOf(a); readyHost(e,owner);
        auto ownerProcess=open(owner);
        Sleep(4000);
        assert(alive(owner)); // A client holds a slot.
        a.attach.outcome.session.reset();
        assert(waitFor([&] { return e.status().clients==0; },Timeout));
        Sleep(1500);
        ep::check(PostMessageW(waitWindow(owner),WM_POWERBROADCAST,PBT_APMRESUMEAUTOMATIC,0));
        Sleep(2300);
        if (!alive(owner)) {
            DWORD code=0; GetExitCodeProcess(ownerProcess.get(),&code); const auto s=e.status();
            std::cout << "  owner gone early: code=" << code << " last_exit=" << static_cast<int>(s.lastExit) << " age=" << GetTickCount64()-s.lastExitTick << '\n';
        }
        assert(alive(owner)); // Would have gone at ~3 s without the resume.
        assert(exitCode(ownerProcess.get(),10000)==rs::ExitClean);
        auto s=e.status(); assert(s.lastExit==rs::Exit::Idle && s.starts.empty() && !s.tripped); // Clean exit forgives.
    }
    std::cout << "[session end] WM_QUERYENDSESSION allowed, WM_ENDSESSION stops the host and exits\n";
    {
        auto a=ensure(e); assert(a.launch==rs::Launch::Started);
        owner=ownerOf(a); host=readyHost(e,owner);
        auto ownerProcess=open(owner); auto hostProcess=open(host);
        // Diagnostics while running.
        assert(waitFor([&] { return e.status().clients==1; },Timeout));
        const auto diagnose=env(L"HARUKOTO_DIAGNOSE");
        if (!diagnose.empty()) {
            std::string json;
            assert(run(quote(diagnose)+L" --resident --scope "+e.scope+L" --store-subkey "+quote(e.testKey)+L" --json",&json)==0);
            std::cout << "  " << json;
            assert(json.find("\"owner_pid\":"+std::to_string(owner)+",")!=std::string::npos && json.find("\"alive\":true")!=std::string::npos);
            assert(json.find("\"start_reason\":\"demand\"")!=std::string::npos && json.find("\"clients\":1")!=std::string::npos);
            assert(json.find("\"host_pid\":"+std::to_string(host)+",")!=std::string::npos);
        } else std::cout << "  diagnose tool not given; skipped\n";
        const auto w=waitWindow(owner);
        DWORD_PTR answer=0;
        assert(SendMessageTimeoutW(w,WM_QUERYENDSESSION,0,ENDSESSION_LOGOFF,SMTO_ABORTIFHUNG,5000,&answer) && answer==TRUE);
        assert(alive(owner));
        assert(SendMessageTimeoutW(w,WM_ENDSESSION,TRUE,ENDSESSION_LOGOFF,SMTO_ABORTIFHUNG,10000,&answer));
        assert(exitCode(ownerProcess.get(),10000)==rs::ExitClean);
        assert(WaitForSingleObject(hostProcess.get(),0)==WAIT_OBJECT_0);
        assert(e.status().lastExit==rs::Exit::Logoff);
        assert(a.attach.outcome.session->registration().poll(ep::now())==control::State::Failed);
    }
    std::cout << "[installer stop] Stop-HarukotoEngineOwners closes it gracefully\n";
    {
        auto a=ensure(e); assert(a.launch==rs::Launch::Started);
        owner=ownerOf(a); host=readyHost(e,owner);
        auto ownerProcess=open(owner); auto hostProcess=open(host);
        waitWindow(owner);
        const auto pwsh=env(L"HARUKOTO_PWSH"), support=env(L"HARUKOTO_INSTALL_SUPPORT");
        if (!pwsh.empty() && !support.empty()) {
            const auto script=L". '"+support+L"'; $r = Stop-HarukotoEngineOwners -Root '"+e.root.wstring()+L"' -TimeoutSeconds 15; "
                L"Write-Output \"graceful=$($r.Graceful) forced=$($r.Forced)\"; exit ($r.Graceful * 10 + $r.Forced)";
            const auto code=run(quote(pwsh)+L" -NoProfile -NonInteractive -Command "+quote(script));
            assert(code==10);
        } else {
            std::cout << "  pwsh not given; posting WM_CLOSE directly\n";
            ep::check(PostMessageW(window(owner),WM_CLOSE,0,0));
        }
        assert(exitCode(ownerProcess.get(),15000)==rs::ExitClean);
        assert(WaitForSingleObject(hostProcess.get(),5000)==WAIT_OBJECT_0);
        assert(e.status().lastExit==rs::Exit::Stopped && admit(e).result==Result::NotRunning);
    }
    std::cout << "[single instance] a second owner of the same logon session gives up after waiting for the lease\n";
    {
        auto a=ensure(e); assert(a.launch==rs::Launch::Started);
        owner=ownerOf(a); readyHost(e,owner);
        auto ownerProcess=open(owner);
        const auto began=ep::now();
        auto second=direct(e);
        assert(exitCode(second.get(),static_cast<DWORD>(Timeout*2))==rs::ExitExists && ep::now()-began>=4500);
        assert(e.status().ownerPid==owner && alive(owner));
        ep::check(PostMessageW(waitWindow(owner),WM_CLOSE,0,0));
        assert(exitCode(ownerProcess.get(),15000)==rs::ExitClean);
    }
}

static std::string sha(const std::filesystem::path &path) { return base_dictionary::StableFile(path).read(true); }
int wmain(int argc, wchar_t **argv) {
    std::cout << std::unitbuf;
    try {
        if (argc==10 && std::wstring(argv[1])==L"--client") return clientMode(argv);
        if (argc!=5) { std::cerr << "usage: harukoto_engine_resident_test OWNER_TEST BASE THEMES PERSONAL\n"; return 1; }
        Env e;
        const std::filesystem::path ownerBuild=argv[1], hostBuild=ownerBuild.parent_path()/L"harukoto_engine_host.exe";
        e.base=argv[2]; e.themes=argv[3]; e.personal=argv[4];
        std::wstring temp(MAX_PATH+1,L'\0'); temp.resize(GetTempPathW(static_cast<DWORD>(temp.size()),temp.data()));
        std::wstring longTemp(32768,L'\0'); longTemp.resize(GetLongPathNameW(temp.c_str(),longTemp.data(),static_cast<DWORD>(longTemp.size())));
        assert(!longTemp.empty());
        const auto unique=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        e.root=std::filesystem::path(longTemp)/(L"harukoto-resident-"+unique);
        const auto id=L"res-"+std::to_wstring(GetCurrentProcessId());
        e.version=e.root/id; std::filesystem::create_directories(e.version);
        e.owner=e.version/L"harukoto_engine_owner.exe"; e.host=e.version/L"harukoto_engine_host.exe";
        std::filesystem::copy_file(ownerBuild,e.owner); std::filesystem::copy_file(hostBuild,e.host);
        e.testKey=L"Software\\HarukotoTest-resident-"+unique;
        e.releaseKey=e.testKey+L"\\EngineReleases"; e.badReleaseKey=e.testKey+L"\\BadReleases";
        e.scope=L"rt-"+unique; e.store=rs::Store::under(e.testKey);
        Cleanup cleanup(e);
        writeRelease(e.releaseKey,id,e,sha(e.owner),sha(e.host));
        writeRelease(e.badReleaseKey,id,e,std::string(64,'0'),sha(e.host));
        {
            BOOL jobbed=FALSE; ep::check(IsProcessInJob(GetCurrentProcess(),nullptr,&jobbed));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            const bool known=jobbed && QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr);
            std::cout << "test process in job: " << (jobbed ? "yes" : "no") << " limits=0x" << std::hex
                << (known ? limits.BasicLimitInformation.LimitFlags : 0) << std::dec << '\n';
        }
        units(e);
        real(e);
        std::cout << "engine resident tests passed\n";
        return 0;
    } catch (const ep::Error &error) {
        std::cerr << "pipe error reason=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
    } catch (const std::exception &error) { std::cerr << "error: " << error.what() << '\n'; }
    return 1;
}
