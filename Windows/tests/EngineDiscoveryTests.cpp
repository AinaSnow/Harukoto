#include "../EngineOwner.h"
#include <userenv.h>
#include <cassert>
#include <iostream>
#include <thread>

// WIN-03 step 2. Real processes: approved fixture copies of this test program
// act as discovery servers / squatters / fake hosts, low-integrity and
// AppContainer children act as sandboxed clients, and the real owner + host
// run with the published discovery pipe. Nothing is installed; HKLM is never
// read here (in-memory release snapshots); the AppContainer profile and the
// temporary directory are removed at the end.
using namespace harukoto::engine;
namespace ep=harukoto::engine::pipe;
namespace dv=harukoto::engine::discovery;
using Result=dv::Result;
constexpr uint64_t Timeout=15000;
constexpr int ResultBase=100;
constexpr wchar_t ContainerName[]=L"Harukoto.EngineDiscoveryTest";

static std::filesystem::path self() {
    std::wstring value(32768,L'\0'); const auto size=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    assert(size && size<value.size()); value.resize(size); return value;
}
static std::wstring quote(const std::wstring &value) {
    std::wstring result=L"\""; size_t slashes=0;
    for (auto c : value) {
        if (c==L'\\') { ++slashes; continue; }
        result.append(c==L'\"' ? slashes*2+1 : slashes,L'\\'); slashes=0; result+=c;
    }
    result.append(slashes*2,L'\\'); return result+L"\"";
}
static const char *label(Result result) {
    static const char *names[]{"Verified","NoRelease","DeploymentInvalid","UnsupportedClient","NotRunning","Busy","Denied","Remote",
        "Identity","Integrity","Image","Stale","Protocol","Rejected","Unavailable","Timeout","Failed"};
    return names[static_cast<size_t>(result)];
}
static void expect(const dv::Outcome &outcome, Result result, const char *what) {
    std::cout << what << ": " << label(outcome.result) << " win32=" << outcome.win32 << '\n';
    assert(outcome.result==result);
    assert(outcome.engine.has_value()==(result==Result::Verified));
}
static std::string hash(const std::filesystem::path &path) { return base_dictionary::StableFile(path).read(true); }
static std::wstring wide(const std::string &value) { return std::wstring(value.begin(),value.end()); }
static release::Entry entry(const std::wstring &id, uint64_t sequence, const std::filesystem::path &owner,
        const std::filesystem::path &host, uint16_t protocol=ProtocolVersion) {
    release::Entry result; result.id=id; result.sequence=sequence; result.protocol=protocol;
    result.owner=owner; result.host=host; result.ownerSha256=hash(owner); result.hostSha256=hash(host); return result;
}
static release::Snapshot snapshot(std::vector<release::Entry> entries) {
    release::Snapshot result; result.status=release::Status::Ready; result.entries=std::move(entries); return result;
}
static dv::Options options(const std::wstring &scope, uint64_t timeout=dv::DefaultTimeoutMs, uint16_t protocol=ProtocolVersion) {
    dv::Options result; result.scope=scope; result.timeoutMs=timeout; result.protocol=protocol; return result;
}
static ep::Handle event(bool inherit) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,inherit};
    ep::Handle result(CreateEventW(&attributes,TRUE,FALSE,nullptr)); ep::check(bool(result)); return result;
}

// ---------------------------------------------------------------- children
class Job {
    ep::Handle job_{CreateJobObjectW(nullptr,nullptr)};
public:
    Job() {
        ep::check(bool(job_));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ep::check(SetInformationJobObject(job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
    }
    void add(HANDLE process) const { ep::check(AssignProcessToJobObject(job_.get(),process)); }
    // Kill everything and wait, so the temporary images can be deleted.
    ~Job() {
        TerminateJobObject(job_.get(),0);
        for (int i=0;i<200;++i) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if (!QueryInformationJobObject(job_.get(),JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr) || !info.ActiveProcesses) break;
            Sleep(50);
        }
    }
};
enum class Sandbox { None, Low, Container };
struct Launch {
    Sandbox sandbox=Sandbox::None;
    PSID container=nullptr;
    std::vector<HANDLE> inherit;
};
// job==nullptr: the child stays in the caller's job (fixture children; a
// low-integrity process cannot open the test's Medium job to nest another).
static ep::Handle start(const Job *job, const std::filesystem::path &exe, const std::vector<std::wstring> &arguments, const Launch &how={}) {
    std::wstring command=quote(exe.wstring());
    for (const auto &argument : arguments) command+=L" "+quote(argument);
    SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,2,0,&size);
    std::vector<uint8_t> bytes(size);
    auto *attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
    ep::check(InitializeProcThreadAttributeList(attributes,2,0,&size));
    struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
    auto handles=how.inherit;
    if (!handles.empty())
        ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),handles.size()*sizeof(HANDLE),nullptr,nullptr));
    SECURITY_CAPABILITIES capabilities{how.container,nullptr,0,0};
    if (how.sandbox==Sandbox::Container)
        ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,&capabilities,sizeof(capabilities),nullptr,nullptr));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.lpAttributeList=attributes;
    PROCESS_INFORMATION info{};
    const DWORD flags=EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED;
    const BOOL inherit=!handles.empty();
    if (how.sandbox==Sandbox::Low) {
        HANDLE raw=nullptr;
        ep::check(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,&raw));
        ep::Handle own(raw);
        ep::check(DuplicateTokenEx(own.get(),0,nullptr,SecurityImpersonation,TokenPrimary,&raw));
        ep::Handle low(raw);
        SID_IDENTIFIER_AUTHORITY authority=SECURITY_MANDATORY_LABEL_AUTHORITY; PSID sid=nullptr;
        ep::check(AllocateAndInitializeSid(&authority,1,SECURITY_MANDATORY_LOW_RID,0,0,0,0,0,0,0,&sid));
        TOKEN_MANDATORY_LABEL mandatory{{sid,SE_GROUP_INTEGRITY}};
        const auto set=SetTokenInformation(low.get(),TokenIntegrityLevel,&mandatory,sizeof(mandatory)+GetLengthSid(sid));
        FreeSid(sid); ep::check(set);
        ep::check(CreateProcessAsUserW(low.get(),exe.c_str(),command.data(),nullptr,nullptr,inherit,flags,nullptr,nullptr,&startup.StartupInfo,&info));
    } else {
        ep::check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,inherit,flags,nullptr,nullptr,&startup.StartupInfo,&info));
    }
    ep::Handle process(info.hProcess), thread(info.hThread);
    if (job) try { job->add(process.get()); } catch (...) { TerminateProcess(process.get(),1); throw; }
    if (ResumeThread(thread.get())==DWORD(-1)) { TerminateProcess(process.get(),1); throw ep::Error(ep::Failure::Io,GetLastError()); }
    return process;
}
static DWORD finish(HANDLE process, uint64_t ms=Timeout) {
    assert(WaitForSingleObject(process,static_cast<DWORD>(ms))==WAIT_OBJECT_0);
    DWORD code=0; ep::check(GetExitCodeProcess(process,&code)); return code;
}
static void kill(HANDLE process) { TerminateProcess(process,0); assert(WaitForSingleObject(process,static_cast<DWORD>(Timeout))==WAIT_OBJECT_0); }
// Start a fixture server and wait until its pipe exists (or it exits).
static ep::Handle serve(const Job &job, const std::filesystem::path &exe, std::vector<std::wstring> arguments, Sandbox sandbox=Sandbox::None) {
    auto ready=event(true);
    arguments.insert(arguments.begin()+2,std::to_wstring(reinterpret_cast<uintptr_t>(ready.get())));
    Launch how; how.sandbox=sandbox; how.inherit={ready.get()};
    auto process=start(&job,exe,arguments,how);
    HANDLE both[]{ready.get(),process.get()};
    const auto status=WaitForMultipleObjects(2,both,FALSE,static_cast<DWORD>(Timeout));
    assert(status==WAIT_OBJECT_0 || status==WAIT_OBJECT_0+1);
    return process;
}

// ---------------------------------------------------------------- fixture modes
static ep::Handle inherited(const wchar_t *value) {
    ep::Handle result(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value))));
    ep::check(SetHandleInformation(result.get(),HANDLE_FLAG_INHERIT,0)); return result;
}
static int idle() { Sleep(INFINITE); return 0; }
// --serve <scope> <ready> <host exe> <protocol>
static int serveMode(wchar_t **argv) {
    auto ready=inherited(argv[3]);
    auto host=start(nullptr,argv[4],{L"--idle"});
    try {
        dv::Server server(argv[2],host.get(),static_cast<uint16_t>(std::stoul(argv[5])),1000);
        ep::check(SetEvent(ready.get()));
        for (;;) {
            server.poll(ep::now());
            const auto time=ep::now(), deadline=server.deadline();
            const DWORD wait=deadline ? (deadline>time ? static_cast<DWORD>(deadline-time) : 0) : INFINITE;
            if (auto handle=server.event()) WaitForSingleObject(handle,wait); else if (wait) Sleep(wait==INFINITE ? 10 : std::min<DWORD>(wait,10));
        }
    } catch (const dv::Server::Squatted &) { return ERROR_ALREADY_EXISTS; }
}
// --squat <scope> <ready> <host exe> <mode>: a raw pipe on the discovery name
// that answers like a server would, with a scripted fault.
static int squatMode(wchar_t **argv) {
    auto ready=inherited(argv[3]);
    const std::wstring mode=argv[5];
    ep::Handle host;
    // Mode "self" (used at low integrity, where starting the fake host is
    // refused with ACCESS_DENIED) reports itself; clients reject it earlier.
    if (mode==L"self") {
        HANDLE raw=nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&raw,0,FALSE,DUPLICATE_SAME_ACCESS));
        host.reset(raw);
    } else host=start(nullptr,argv[4],{L"--idle"});
    ep::Handle pipe(CreateNamedPipeW(dv::name(argv[2]).c_str(),PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,1,256,256,0,nullptr));
    if (!pipe) return static_cast<int>(GetLastError());
    ep::check(SetEvent(ready.get()));
    for (;;) {
        if (!ConnectNamedPipe(pipe.get(),nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED) continue;
        dv::Message bytes{}; DWORD count=0, total=0;
        while (total<bytes.size() && ReadFile(pipe.get(),bytes.data()+total,static_cast<DWORD>(bytes.size()-total),&count,nullptr) && count) total+=count;
        dv::Request request;
        if (total==bytes.size() && dv::decode(bytes.data(),bytes.size(),request) && mode!=L"silent") {
            dv::Response response; response.protocol=request.protocol; response.nonce=request.nonce; response.code=dv::Code::Ok;
            response.ownerPid=GetCurrentProcessId(); response.hostPid=GetProcessId(host.get()); response.hostCreated=dv::created(host.get());
            if (mode==L"badnonce") response.nonce[0]^=1;
            if (mode==L"selfhost") response.hostPid=GetCurrentProcessId();
            if (mode==L"wrongcreated") response.hostCreated+=1;
            if (mode==L"reject" || mode==L"unavailable") {
                response.code=mode==L"reject" ? dv::Code::Rejected : dv::Code::Unavailable; response.ownerPid=response.hostPid=0; response.hostCreated=0;
            }
            const auto reply=dv::encode(response);
            WriteFile(pipe.get(),reply.data(),static_cast<DWORD>(reply.size()),&count,nullptr); FlushFileBuffers(pipe.get());
        } else if (mode==L"silent") Sleep(3000);
        DisconnectNamedPipe(pipe.get());
    }
}
// --client <scope> <owner> <owner sha> <host> <host sha>: exit code ResultBase+Result.
static std::string narrow(const wchar_t *value) {
    std::string result; for (; *value; ++value) result.push_back(static_cast<char>(*value & 0x7f)); return result;
}
static int clientMode(wchar_t **argv) {
    release::Entry value; value.id=L"fixture"; value.sequence=1; value.protocol=ProtocolVersion;
    value.owner=argv[3]; value.ownerSha256=EngineDeployment::digest(narrow(argv[4]));
    value.host=argv[5]; value.hostSha256=EngineDeployment::digest(narrow(argv[6]));
    return ResultBase+static_cast<int>(dv::discover(snapshot({value}),options(argv[2])).result);
}
static int openMode(wchar_t **argv) {
    ep::Handle file(CreateFileW(dv::name(argv[2]).c_str(),ep::ClientAccess,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,nullptr));
    return file ? 0 : static_cast<int>(GetLastError());
}
static int openProcessMode(wchar_t **argv) {
    ep::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,static_cast<DWORD>(std::stoul(argv[2]))));
    if (!process) return static_cast<int>(GetLastError());
    HANDLE raw=nullptr;
    if (!OpenProcessToken(process.get(),TOKEN_QUERY,&raw)) return 1000+static_cast<int>(GetLastError());
    CloseHandle(raw); return 0;
}

// ---------------------------------------------------------------- unit checks
static void units() {
    dv::Request request; request.protocol=1; request.clientPid=42; request.nonce[0]=7; request.nonce[31]=9;
    auto bytes=dv::encode(request); dv::Request decoded;
    assert(dv::decode(bytes.data(),bytes.size(),decoded) && decoded.protocol==1 && decoded.clientPid==42 && decoded.nonce==request.nonce);
    assert(!dv::decode(bytes.data(),bytes.size()-1,decoded));
    for (size_t index : {size_t(0),size_t(4),size_t(6),size_t(10),size_t(48),size_t(127)}) {
        auto bad=bytes; bad[index]^=0x40; assert(!dv::decode(bad.data(),bad.size(),decoded));
    }
    dv::Response response; response.protocol=1; response.code=dv::Code::Ok; response.ownerPid=1; response.hostPid=2; response.hostCreated=3;
    response.nonce=request.nonce;
    auto reply=dv::encode(response); dv::Response back;
    assert(dv::decode(reply.data(),reply.size(),back) && back.code==dv::Code::Ok && back.hostCreated==3 && back.nonce==request.nonce);
    assert(!dv::decode(bytes.data(),bytes.size(),back) && !dv::decode(reply.data(),reply.size(),decoded)); // Type confusion.
    response.code=dv::Code::Rejected; reply=dv::encode(response); assert(!dv::decode(reply.data(),reply.size(),back)); // Locators only with Ok.
    response.ownerPid=response.hostPid=0; response.hostCreated=0; reply=dv::encode(response);
    assert(dv::decode(reply.data(),reply.size(),back) && back.code==dv::Code::Rejected);
    reply[10]=9; assert(!dv::decode(reply.data(),reply.size(),back));
    // Names: per logon session and scope, local only.
    const auto name=dv::name(L"abc");
    assert(name.rfind(L"\\\\.\\pipe\\Harukoto.Discovery.v1.",0)==0 && name.size()>40 && name.substr(name.size()-4)==L".abc");
    assert(dv::name(L"abc")==name && dv::name(L"abd")!=name);
    bool invalid=false; try { dv::name(L"Bad Scope"); } catch (const std::invalid_argument &) { invalid=true; } assert(invalid);
    // Token policy: this test runs at Medium; derived low-integrity and
    // restricted tokens keep the same logon identity but are never peers.
    const auto own=dv::inspectProcess(GetCurrentProcess());
    std::cout << "own token integrity=0x" << std::hex << own.integrity << std::dec << " appcontainer=" << own.appContainer << '\n';
    assert(own.supported() && own.peerOf(own));
    HANDLE raw=nullptr; ep::check(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT,&raw));
    ep::Handle token(raw);
    ep::check(DuplicateTokenEx(token.get(),0,nullptr,SecurityImpersonation,TokenPrimary,&raw)); ep::Handle low(raw);
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_MANDATORY_LABEL_AUTHORITY; PSID sid=nullptr;
    ep::check(AllocateAndInitializeSid(&authority,1,SECURITY_MANDATORY_LOW_RID,0,0,0,0,0,0,0,&sid));
    TOKEN_MANDATORY_LABEL mandatory{{sid,SE_GROUP_INTEGRITY}};
    ep::check(SetTokenInformation(low.get(),TokenIntegrityLevel,&mandatory,sizeof(mandatory)+GetLengthSid(sid))); FreeSid(sid);
    const auto lowPolicy=dv::inspect(low.get());
    assert(lowPolicy.identity==own.identity && lowPolicy.integrity==SECURITY_MANDATORY_LOW_RID && !lowPolicy.supported());
    assert(!lowPolicy.peerOf(own) && !own.peerOf(lowPolicy));
    const auto user=pipe::tokenInfo(token.get(),TokenUser);
    SID_AND_ATTRIBUTES restricting{reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid,0};
    ep::check(CreateRestrictedToken(token.get(),0,0,nullptr,0,nullptr,1,&restricting,&raw)); ep::Handle restricted(raw);
    const auto restrictedPolicy=dv::inspect(restricted.get());
    assert(restrictedPolicy.restricted && restrictedPolicy.identity==own.identity && !restrictedPolicy.supported() && !own.peerOf(restrictedPolicy));
    auto other=own; other.identity.authentication.LowPart^=1; assert(!own.peerOf(other)); // Another logon session (e.g. the elevated half).
    other=own; other.identity.session^=1; assert(!own.peerOf(other));
    other=own; other.integrity=SECURITY_MANDATORY_HIGH_RID; assert(!other.supported() && !own.peerOf(other));
    other=own; other.appContainer=true; assert(!own.peerOf(other));
    std::cout << "units ok\n";
}

// ---------------------------------------------------------------- main checks
struct Root {
    std::filesystem::path path;
    Root() {
        std::wstring temp(MAX_PATH+1,L'\0'); temp.resize(GetTempPathW(static_cast<DWORD>(temp.size()),temp.data()));
        path=std::filesystem::path(temp)/(L"harukoto-discovery-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(path);
    }
    ~Root() {
        for (int i=0;i<50;++i) {
            std::error_code error; std::filesystem::remove_all(path,error);
            if (!std::filesystem::exists(path,error)) return;
            Sleep(100); // Image sections of just-killed fixtures may linger briefly.
        }
        std::cout << "warning: could not remove temporary directory\n";
    }
};
static std::filesystem::path copy(const std::filesystem::path &folder, const wchar_t *name, size_t extra) {
    std::filesystem::create_directories(folder);
    const auto target=folder/name; std::filesystem::copy_file(self(),target);
    if (extra) {
        // Trailing overlay bytes: still a runnable image, different SHA-256.
        ep::Handle file(CreateFileW(target.c_str(),FILE_APPEND_DATA,0,nullptr,OPEN_EXISTING,0,nullptr)); ep::check(bool(file));
        std::vector<uint8_t> bytes(extra,static_cast<uint8_t>(extra)); DWORD count=0;
        ep::check(WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr));
    }
    return target;
}
struct Container {
    PSID sid=nullptr;
    explicit Container(const std::filesystem::path &grant) {
        DeleteAppContainerProfile(ContainerName); // Leftover from an interrupted run.
        const auto created=CreateAppContainerProfile(ContainerName,L"Harukoto discovery test",L"Temporary test profile",nullptr,0,&sid);
        if (FAILED(created)) throw ep::Error(ep::Failure::Io,static_cast<DWORD>(created));
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions=GENERIC_READ | GENERIC_EXECUTE; access.grfAccessMode=GRANT_ACCESS;
        access.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm=TRUSTEE_IS_SID; access.Trustee.TrusteeType=TRUSTEE_IS_WELL_KNOWN_GROUP; access.Trustee.ptstrName=static_cast<LPWSTR>(sid);
        PACL current=nullptr, updated=nullptr; PSECURITY_DESCRIPTOR descriptor=nullptr;
        auto status=GetNamedSecurityInfoW(grant.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&current,nullptr,&descriptor);
        assert(status==ERROR_SUCCESS);
        status=SetEntriesInAclW(1,&access,current,&updated); assert(status==ERROR_SUCCESS);
        status=SetNamedSecurityInfoW(const_cast<LPWSTR>(grant.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,updated,nullptr);
        assert(status==ERROR_SUCCESS);
        LocalFree(updated); LocalFree(descriptor);
    }
    ~Container() { if (sid) FreeSid(sid); DeleteAppContainerProfile(ContainerName); }
};
static void concurrent(const release::Snapshot &approved, const std::wstring &scope) {
    std::vector<std::thread> threads; std::vector<Result> results(4,Result::Failed);
    for (size_t i=0;i<results.size();++i)
        threads.emplace_back([&,i] { results[i]=dv::discover(approved,options(scope,Timeout)).result; });
    for (auto &thread : threads) thread.join();
    for (auto result : results) assert(result==Result::Verified);
    std::cout << "concurrent clients: 4 Verified\n";
}
static void sandboxed(const Job &job, const Root &root, const std::filesystem::path &server, const std::filesystem::path &host,
        const std::wstring &scope, DWORD serverPid, Sandbox sandbox, PSID container) {
    const auto client=root.path/L"a"/L"server.exe";
    const char *kind=sandbox==Sandbox::Low ? "low-integrity" : "appcontainer";
    Launch how; how.sandbox=sandbox; how.container=container;
    const auto result=finish(start(&job,client,{L"--client",scope,server.wstring(),wide(hash(server)),host.wstring(),wide(hash(host))},how).get());
    std::cout << kind << " client discover: " << (result>=ResultBase ? label(static_cast<Result>(result-ResultBase)) : "exit") << " (" << result << ")\n";
    assert(result==ResultBase+static_cast<DWORD>(Result::UnsupportedClient));
    const auto open=finish(start(&job,client,{L"--open",scope},how).get());
    std::cout << kind << " raw pipe open: win32=" << open << '\n';
    assert(open==ERROR_ACCESS_DENIED); // DACL (AppContainer) or Medium no-write-up label (low).
    const auto process=finish(start(&job,client,{L"--openprocess",std::to_wstring(serverPid)},how).get());
    std::cout << kind << " open engine process/token: code=" << process << '\n';
    if (sandbox==Sandbox::Container) assert(process!=0); // Cannot even verify the engine identity.
}
static void fixtures(const Root &root) {
    Job job;
    const auto scope=L"discovery-test-"+std::to_wstring(GetCurrentProcessId());
    const auto aServer=copy(root.path/L"a",L"server.exe",0), aHost=copy(root.path/L"a",L"host.exe",1);
    const auto bServer=copy(root.path/L"b",L"server.exe",2), bHost=copy(root.path/L"b",L"host.exe",3);
    const auto cServer=copy(root.path/L"c",L"server.exe",4);
    const auto a=entry(L"a",1,aServer,aHost), b=entry(L"b",2,bServer,bHost), c=entry(L"c",1,cServer,aHost);
    const auto approved=snapshot({a});

    // No trusted release: never touch the pipe.
    expect(dv::discover(release::Snapshot{},options(scope)),Result::NoRelease,"invalid snapshot");
    release::Snapshot missing; missing.status=release::Status::Missing; missing.error=ERROR_FILE_NOT_FOUND;
    expect(dv::discover(missing,options(scope)),Result::NoRelease,"missing release key");
    const auto protoArg=std::to_wstring(ProtocolVersion);
    const auto nextProtocol=static_cast<uint16_t>(ProtocolVersion+1);
    expect(dv::discover(snapshot({a}),options(scope,1000,nextProtocol)),Result::NoRelease,"no entry for protocol");
    auto tie=b; tie.sequence=1; expect(dv::discover(snapshot({a,tie}),options(scope)),Result::NoRelease,"ambiguous release");
    auto wrong=a; wrong.ownerSha256=b.ownerSha256; expect(dv::discover(snapshot({wrong}),options(scope)),Result::DeploymentInvalid,"approved hash mismatch");
    expect(dv::discover(approved,options(scope)),Result::NotRunning,"nothing running");

    // Approved server: verified, reusable, concurrent clients serialize.
    auto server=serve(job,aServer,{L"--serve",scope,aHost.wstring(),protoArg});
    assert(WaitForSingleObject(server.get(),0)==WAIT_TIMEOUT);
    for (int i=0;i<5;++i) {
        auto outcome=dv::discover(approved,options(scope));
        expect(outcome,Result::Verified,"approved server");
        const auto &engine=*outcome.engine;
        assert(engine.ownerPid==GetProcessId(server.get()) && engine.hostPid && engine.hostPid!=engine.ownerPid);
        assert(GetProcessId(engine.owner.get())==engine.ownerPid && GetProcessId(engine.host.get())==engine.hostPid);
        DWORD flags=1; ep::check(GetHandleInformation(engine.owner.get(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        assert(engine.release.id==L"a" && engine.ownerDeployment && engine.hostDeployment);
    }
    concurrent(approved,scope);
    // A protocol the running engine does not speak (selected entry says ProtocolVersion + 1).
    auto newer=a; newer.protocol=nextProtocol;
    expect(dv::discover(snapshot({newer}),options(scope,dv::DefaultTimeoutMs,nextProtocol)),Result::Protocol,"protocol mismatch");
    expect(dv::discover(snapshot({a,b}),options(scope)),Result::Stale,"older approved release running");
    expect(dv::discover(snapshot({c}),options(scope)),Result::Image,"unapproved image");
    // Remote access to the same pipe through the loopback SMB redirector.
    const auto local=dv::name(scope);
    const auto remoteName=L"\\\\localhost\\pipe\\"+local.substr(9);
    ep::Handle remote(CreateFileW(remoteName.c_str(),ep::ClientAccess,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,nullptr));
    const auto remoteError=remote ? ERROR_SUCCESS : GetLastError();
    std::cout << "remote (\\\\localhost) open: win32=" << remoteError << '\n';
    assert(!remote);
    expect(dv::discover(approved,options(scope)),Result::Verified,"after remote attempt");

    // Sandboxed clients cannot reach or verify the engine: stay in-process.
    sandboxed(job,root,aServer,aHost,scope,GetProcessId(server.get()),Sandbox::Low,nullptr);
    try {
        Container container(root.path);
        sandboxed(job,root,aServer,aHost,scope,GetProcessId(server.get()),Sandbox::Container,container.sid);
    } catch (const ep::Error &error) {
        std::cout << "appcontainer: unavailable win32=" << error.code << " (unverified)\n";
    }
    // Squatting: a second server must not join an existing name.
    auto second=serve(job,aServer,{L"--serve",scope,aHost.wstring(),protoArg});
    assert(finish(second.get())==ERROR_ALREADY_EXISTS);
    expect(dv::discover(approved,options(scope)),Result::Verified,"first server unaffected");
    kill(server.get());
    expect(dv::discover(approved,options(scope)),Result::NotRunning,"stale after exit");

    // This test program squats the name: wrong image, and the real server refuses to start.
    {
        ep::Handle squat(CreateNamedPipeW(local.c_str(),PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE | PIPE_WAIT,1,256,256,0,nullptr));
        ep::check(bool(squat));
        auto blocked=serve(job,aServer,{L"--serve",scope,aHost.wstring(),protoArg});
        assert(finish(blocked.get())==ERROR_ALREADY_EXISTS);
        expect(dv::discover(approved,options(scope)),Result::Image,"squatted by unapproved process");
    }
    // Low-integrity squatter running the APPROVED image: image alone is not enough.
    {
        auto squat=serve(job,aServer,{L"--squat",scope,aHost.wstring(),L"self"},Sandbox::Low);
        assert(WaitForSingleObject(squat.get(),0)==WAIT_TIMEOUT);
        expect(dv::discover(approved,options(scope)),Result::Integrity,"low-integrity approved image");
        auto blocked=serve(job,aServer,{L"--serve",scope,aHost.wstring(),protoArg});
        assert(finish(blocked.get())==ERROR_ALREADY_EXISTS); // Denial of service only; detected.
        kill(squat.get());
    }
    // Approved-image servers that answer wrongly.
    const std::pair<const wchar_t *,Result> faults[]{{L"ok",Result::Verified},{L"badnonce",Result::Protocol},
        {L"selfhost",Result::Identity},{L"wrongcreated",Result::Identity},{L"reject",Result::Rejected},
        {L"unavailable",Result::Unavailable},{L"silent",Result::Timeout}};
    for (const auto &[mode,result] : faults) {
        auto squat=serve(job,aServer,{L"--squat",scope,aHost.wstring(),mode});
        assert(WaitForSingleObject(squat.get(),0)==WAIT_TIMEOUT);
        std::string what="scripted reply "; for (auto ch=mode;*ch;++ch) what+=static_cast<char>(*ch);
        expect(dv::discover(approved,options(scope,500)),result,what.c_str());
        kill(squat.get());
    }
}
static void owner(int argc, wchar_t **argv) {
    assert(argc==5);
    const std::filesystem::path ownerExe=argv[1];
    EngineProcess::Config config{ownerExe.parent_path()/L"harukoto_engine_host.exe",argv[2],argv[3],argv[4]};
    config.ownerExecutable=ownerExe; config.dynamicEndpoints=true; config.connections=2;
    const auto scope=L"discovery-owner-"+std::to_wstring(GetCurrentProcessId());
    config.discovery=true;
    bool invalid=false; try { EngineProcess bad(config); } catch (const std::invalid_argument &) { invalid=true; } assert(invalid);
    config.ownerInstance=scope;
    config.hostSha256=hash(config.executable); config.ownerSha256=hash(config.ownerExecutable);
    const auto approved=snapshot({entry(L"build",1,config.ownerExecutable,config.executable)});
    EngineProcess process(config);
    const auto deadline=ep::now()+Timeout*4;
    while (process.poll(ep::now())!=EngineProcess::State::Ready) {
        assert(process.status().state!=EngineProcess::State::Exited && ep::now()<deadline);
        auto handles=process.waitHandles(); assert(!handles.empty());
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,100);
    }
    for (int i=0;i<3;++i) {
        auto outcome=dv::discover(approved,options(scope));
        expect(outcome,Result::Verified,"real owner");
        assert(outcome.engine->ownerPid==GetProcessId(process.processHandle()));
        assert(outcome.engine->hostPid==GetProcessId(process.engineHandle()));
        process.poll(ep::now());
    }
    concurrent(approved,scope);
    auto swapped=approved; std::swap(swapped.entries[0].owner,swapped.entries[0].host); std::swap(swapped.entries[0].ownerSha256,swapped.entries[0].hostSha256);
    expect(dv::discover(swapped,options(scope)),Result::Image,"real owner, owner/host swapped in metadata");
    process.requestStop(ep::now());
    while (process.poll(ep::now())!=EngineProcess::State::Exited) {
        auto handles=process.waitHandles(); if (handles.empty()) break;
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,100);
    }
    assert(process.status().failure==EngineProcess::Failure::None);
    expect(dv::discover(approved,options(scope)),Result::NotRunning,"real owner stopped");
}

int wmain(int argc, wchar_t **argv) {
    std::cout << std::unitbuf;
    try {
        if (argc>=2) {
            const std::wstring mode=argv[1];
            if (mode==L"--idle") return idle();
            if (mode==L"--serve" && argc==6) return serveMode(argv);
            if (mode==L"--squat" && argc==6) return squatMode(argv);
            if (mode==L"--client" && argc==7) return clientMode(argv);
            if (mode==L"--open" && argc==3) return openMode(argv);
            if (mode==L"--openprocess" && argc==3) return openProcessMode(argv);
        }
        units();
        {
            Root root;
            fixtures(root);
        }
        if (argc==5) owner(argc,argv);
        else std::cout << "real owner skipped (no fixture arguments)\n";
        std::cout << "engine discovery tests passed\n";
        return 0;
    } catch (const ep::Error &error) {
        std::cerr << "pipe error reason=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
        if (argc>=2 && argv[1][0]==L'-') return 4000+static_cast<int>(error.code); // Fixture child: report the cause.
    } catch (const std::exception &error) { std::cerr << "error: " << error.what() << '\n'; }
    return 1;
}
