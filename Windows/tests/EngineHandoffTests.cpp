#include "../EngineOwner.h"
#include "../EngineHandoff.h"
#include "EngineHostTestClient.h"
#include <fstream>
#include <set>
#include <thread>

// WIN-03 step 3: handoff after discovery. Real owner + host processes (the
// built programs, approved by an in-memory release snapshot: no HKLM, no
// registry writes), real client child processes that die mid-handoff, and 8
// concurrent starter processes that must converge on one owner. Every child
// runs in a kill-on-close Job; the temporary directory is removed at the end.
using namespace host_test;
namespace dv=harukoto::engine::discovery;
namespace hf=harukoto::engine::handoff;
using Result=hf::Result;
constexpr int DieCode=77, StarterBase=100;

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
    static const char *names[]{"Admitted","NotRunning","Busy","Full","Closing","Unavailable","Timeout","Stale","Duplicate",
        "Rejected","Protocol","Untrusted","Failed"};
    return names[static_cast<size_t>(result)];
}
static std::string hash(const std::filesystem::path &path) { return base_dictionary::StableFile(path).read(true); }
static std::wstring wide(const std::string &value) { return std::wstring(value.begin(),value.end()); }
static std::string narrow(const wchar_t *value) { std::string r; for (; *value; ++value) r.push_back(static_cast<char>(*value & 0x7f)); return r; }
static release::Snapshot snapshot(const std::filesystem::path &owner, const std::string &ownerSha, const std::filesystem::path &host, const std::string &hostSha) {
    release::Entry entry; entry.id=L"build"; entry.sequence=1; entry.protocol=ProtocolVersion;
    entry.owner=owner; entry.host=host; entry.ownerSha256=ownerSha; entry.hostSha256=hostSha;
    release::Snapshot result; result.status=release::Status::Ready; result.entries={entry}; return result;
}
static hf::Options options(const std::wstring &scope) { hf::Options result; result.discovery.scope=scope; return result; }
static hf::Outcome admit(const release::Snapshot &approved, const std::wstring &scope, const char *what, Result expected) {
    auto outcome=hf::admit(approved,options(scope));
    std::cout << what << ": " << label(outcome.result) << " win32=" << outcome.win32 << '\n';
    assert(outcome.result==expected && bool(outcome.session)==(expected==Result::Admitted));
    return outcome;
}
static DWORD handles(HANDLE process) { DWORD count=0; ep::check(GetProcessHandleCount(process,&count)); return count; }
static ep::Handle event(bool inherit) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,inherit};
    ep::Handle result(CreateEventW(&attributes,TRUE,FALSE,nullptr)); ep::check(bool(result)); return result;
}
static EngineRegistration::Result request(EngineRegistration &registration, control::Operation operation) {
    assert(registration.request(operation,ep::now(),Timeout));
    while (registration.poll(ep::now())==control::State::Pending) wait(registration.waitHandles(),registration.deadline());
    auto result=registration.take(ep::now()); assert(result); return std::move(*result);
}

// ---------------------------------------------------------------- processes
class Job {
    ep::Handle job_{CreateJobObjectW(nullptr,nullptr)};
public:
    Job() {
        ep::check(bool(job_));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ep::check(SetInformationJobObject(job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
    }
    ~Job() {
        TerminateJobObject(job_.get(),0);
        for (int i=0;i<200;++i) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if (!QueryInformationJobObject(job_.get(),JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr) || !info.ActiveProcesses) break;
            Sleep(50);
        }
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
static DWORD finish(HANDLE process) {
    assert(WaitForSingleObject(process,static_cast<DWORD>(Timeout*3))==WAIT_OBJECT_0);
    DWORD code=0; ep::check(GetExitCodeProcess(process,&code)); return code;
}
// Owner + host started by this test (EngineProcess, discovery published).
struct Fixture {
    std::filesystem::path owner, host, base, themes, personal;
    std::string ownerSha, hostSha;
    release::Snapshot approved() const { return snapshot(owner,ownerSha,host,hostSha); }
    EngineProcess::Config config(const std::wstring &scope) const {
        EngineProcess::Config config{host,base,themes,personal};
        config.ownerExecutable=owner; config.dynamicEndpoints=true; config.connections=4;
        config.ownerInstance=scope; config.discovery=true;
        config.hostSha256=hostSha; config.ownerSha256=ownerSha;
        return config;
    }
    std::vector<std::wstring> arguments() const {
        return {owner.wstring(),wide(ownerSha),host.wstring(),wide(hostSha),base.wstring(),themes.wstring(),personal.wstring()};
    }
    static Fixture parse(wchar_t **argv) { // owner ownerSha host hostSha base themes personal
        Fixture f; f.owner=argv[0]; f.ownerSha=EngineDeployment::digest(narrow(argv[1])); f.host=argv[2];
        f.hostSha=EngineDeployment::digest(narrow(argv[3])); f.base=argv[4]; f.themes=argv[5]; f.personal=argv[6]; return f;
    }
};
// Ready, or exited (InstanceBusy etc.).
static EngineProcess::State settle(EngineProcess &process, uint64_t deadline) {
    for (;;) {
        const auto state=process.poll(ep::now());
        if (state==EngineProcess::State::Ready || state==EngineProcess::State::Exited) return state;
        if (ep::now()>=deadline) return state;
        auto events=process.waitHandles();
        if (events.empty()) Sleep(10); else WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,100);
    }
}
struct Owner {
    std::unique_ptr<EngineProcess> process;
    bool ready=false;
    Owner(const Fixture &fixture, const std::wstring &scope, bool required=true) : process(std::make_unique<EngineProcess>(fixture.config(scope))) {
        ready=settle(*process,ep::now()+Timeout*4)==EngineProcess::State::Ready;
        assert(ready || !required);
    }
    DWORD pid() const { return GetProcessId(process->processHandle()); }
    void stop() {
        process->requestStop(ep::now());
        while (process->poll(ep::now())!=EngineProcess::State::Exited) {
            auto events=process->waitHandles(); if (events.empty()) break;
            WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,100);
        }
    }
    void crash() {
        ep::Handle killer(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,FALSE,pid())); ep::check(bool(killer));
        ep::check(TerminateProcess(killer.get(),9)); assert(WaitForSingleObject(killer.get(),static_cast<DWORD>(Timeout))==WAIT_OBJECT_0);
    }
};

// ---------------------------------------------------------------- child modes
// --die <stage> <scope> <fixture...>: dies at the given point of a handoff.
static int dieMode(wchar_t **argv) {
    const std::wstring stage=argv[2];
    const auto fixture=Fixture::parse(argv+4);
    auto opts=options(argv[3]);
    opts.testHook=[&](hf::Stage at) {
        const bool now=(stage==L"verified" && at==hf::Stage::Verified) || (stage==L"granted" && at==hf::Stage::Granted) ||
            (stage==L"committed" && at==hf::Stage::Committed);
        if (now) TerminateProcess(GetCurrentProcess(),DieCode);
    };
    const auto outcome=hf::admit(fixture.approved(),opts);
    return 200+static_cast<int>(outcome.result); // Not reached when the hook fires.
}
// --starter <scope> <go> <done> <finish> <report file> <fixture...>
static int starterMode(wchar_t **argv) {
    const std::wstring scope=argv[2];
    ep::Handle go(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[3]))));
    ep::Handle done(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[4]))));
    ep::Handle finished(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[5]))));
    const std::filesystem::path report=argv[6];
    const auto fixture=Fixture::parse(argv+7);
    std::unique_ptr<EngineProcess> started;
    hf::AttachOptions attach; attach.handoff=options(scope); attach.budgetMs=60000;
    attach.start=[&](uint64_t deadline) {
        started=std::make_unique<EngineProcess>(fixture.config(scope));
        const auto state=settle(*started,deadline);
        if (state==EngineProcess::State::Ready) return hf::Start::Started;
        if (started->status().failure==EngineProcess::Failure::InstanceBusy) return hf::Start::Exists;
        return hf::Start::Failed;
    };
    assert(WaitForSingleObject(go.get(),static_cast<DWORD>(Timeout))==WAIT_OBJECT_0);
    const auto outcome=hf::attach(fixture.approved(),attach);
    DWORD ownerPid=outcome.outcome.session ? outcome.outcome.session->engine().ownerPid : 0;
    bool usable=false;
    if (outcome.outcome.session) {
        const auto status=request(outcome.outcome.session->registration(),control::Operation::Inspect);
        usable=status.metadata.code==control::Code::Ok;
    }
    {
        std::ofstream out(report);
        out << label(outcome.outcome.result) << ' ' << ownerPid << ' ' << outcome.starts << ' ' << outcome.attempts << ' ' << outcome.waits << ' '
            << (started ? 1 : 0) << ' ' << usable << '\n';
    }
    ep::check(SetEvent(done.get()));
    // Keep the owner (in this starter's Job, if it started it) alive until all report.
    assert(WaitForSingleObject(finished.get(),static_cast<DWORD>(Timeout*6))==WAIT_OBJECT_0);
    return StarterBase+static_cast<int>(outcome.outcome.result);
}

// ---------------------------------------------------------------- unit checks
static void units() {
    dv::Admission admit; admit.type=dv::Type::Admit; admit.protocol=1; admit.pid=42; admit.nonce[0]=7; admit.request=99; admit.incarnation=1234; admit.value=55;
    auto bytes=dv::encode(admit); dv::Admission back;
    assert(dv::decode(bytes.data(),bytes.size(),back) && back.type==dv::Type::Admit && back.request==99 && back.value==55 && back.nonce==admit.nonce);
    dv::Request hello; assert(!dv::decode(bytes.data(),bytes.size(),hello)); // Never confused with a hello.
    for (size_t index : {size_t(0),size_t(4),size_t(6),size_t(10),size_t(72),size_t(127)}) {
        auto bad=bytes; bad[index]^=0x40; assert(!dv::decode(bad.data(),bad.size(),back));
    }
    for (int field=0;field<3;++field) {
        auto copy=admit; (field==0 ? copy.request : field==1 ? copy.incarnation : copy.value)=0;
        auto raw=dv::encode(copy); assert(!dv::decode(raw.data(),raw.size(),back));
    }
    dv::Admission grant=admit; grant.type=dv::Type::Grant; grant.code=dv::Grant::Ok; grant.pid=7; grant.value=(uint64_t(1)<<63)|1;
    grant.handles={0x10,0x14,0x18,0x1c};
    bytes=dv::encode(grant); assert(dv::decode(bytes.data(),bytes.size(),back) && back.handles==grant.handles && back.code==dv::Grant::Ok);
    auto dup=grant; dup.handles[3]=dup.handles[0]; bytes=dv::encode(dup); assert(!dv::decode(bytes.data(),bytes.size(),back));
    auto missing=grant; missing.handles[2]=0; bytes=dv::encode(missing); assert(!dv::decode(bytes.data(),bytes.size(),back));
    auto full=grant; full.code=dv::Grant::Full; bytes=dv::encode(full); assert(!dv::decode(bytes.data(),bytes.size(),back)); // Capability only with Ok.
    full.value=0; full.handles={}; bytes=dv::encode(full); assert(dv::decode(bytes.data(),bytes.size(),back) && back.code==dv::Grant::Full);
    auto handlesInAdmit=admit; handlesInAdmit.handles[0]=4; bytes=dv::encode(handlesInAdmit); assert(!dv::decode(bytes.data(),bytes.size(),back));
    bytes=dv::encode(grant); bytes[10]=7; assert(!dv::decode(bytes.data(),bytes.size(),back));
    auto ack=grant; ack.type=dv::Type::Committed; ack.handles={}; bytes=dv::encode(ack); assert(dv::decode(bytes.data(),bytes.size(),back));
    ack.code=dv::Grant::Full; bytes=dv::encode(ack); assert(!dv::decode(bytes.data(),bytes.size(),back));
    // The client's checks: everything is bound to this connection, attempt and owner incarnation.
    assert(hf::check(admit,grant,7)==Result::Admitted);
    assert(hf::check(admit,grant,8)==Result::Protocol);
    auto other=grant; other.nonce[0]^=1; assert(hf::check(admit,other,7)==Result::Protocol);
    other=grant; other.request+=1; assert(hf::check(admit,other,7)==Result::Protocol);
    other=grant; other.incarnation+=1; assert(hf::check(admit,other,7)==Result::Stale); // A previous owner incarnation.
    other=full; assert(hf::check(admit,other,7)==Result::Full);
    other=full; other.code=dv::Grant::Duplicate; assert(hf::check(admit,other,7)==Result::Duplicate);
    other=full; other.code=dv::Grant::Closing; assert(hf::check(admit,other,7)==Result::Closing);
    other=full; other.code=dv::Grant::Stale; assert(hf::check(admit,other,7)==Result::Stale);
    other=grant; other.type=dv::Type::Committed; assert(hf::check(admit,other,7)==Result::Protocol);
    auto commit=admit; commit.type=dv::Type::Commit; commit.value=grant.value;
    ack=grant; ack.type=dv::Type::Committed; ack.handles={}; ack.code=dv::Grant::Ok;
    assert(hf::check(commit,ack)==Result::Admitted);
    other=ack; other.value+=1; assert(hf::check(commit,other)==Result::Protocol);
    other=ack; other.code=dv::Grant::Closing; assert(hf::check(commit,other)==Result::Closing);
    assert(hf::StarterLock::name(L"abc").find(L"Harukoto.Starter.v1.")!=std::wstring::npos);
    std::cout << "units ok\n";
}

// ---------------------------------------------------------------- real owner
// Take slots until refused; the refusal must be Full. Sessions are released on return.
static size_t capacity(const release::Snapshot &approved, const std::wstring &scope) {
    std::vector<std::unique_ptr<hf::Session>> held;
    for (;;) {
        auto outcome=hf::admit(approved,options(scope));
        if (outcome.result!=Result::Admitted) {
            if (outcome.result!=Result::Full) std::cout << "capacity probe ended with " << label(outcome.result) << '\n';
            assert(outcome.result==Result::Full); break;
        }
        held.push_back(std::move(outcome.session)); assert(held.size()<=admission::Limit);
    }
    return held.size();
}
static void baseline(const release::Snapshot &approved, const std::wstring &scope, const char *after) {
    const auto count=capacity(approved,scope);
    std::cout << "slots free after " << after << ": " << count << '\n';
    assert(count==admission::Limit);
}
// Raw protocol on a verified connection, for faults a well-behaved client never sends.
struct Raw {
    dv::Connection connection;
    dv::Engine engine;
    dv::Admission admit;
    Raw(const release::Snapshot &approved, const std::wstring &scope) {
        dv::Options o; o.scope=scope; o.timeoutMs=3000;
        auto found=dv::detail::open(approved,o,&connection); assert(found.result==dv::Result::Verified);
        engine=std::move(*found.engine);
        admit.type=dv::Type::Admit; admit.protocol=ProtocolVersion; admit.pid=GetCurrentProcessId(); admit.nonce=connection.request.nonce;
        admit.request=hf::detail::random(); admit.incarnation=dv::created(engine.owner.get()); admit.value=dv::created(GetCurrentProcess());
    }
    void send(const dv::Admission &message) { auto bytes=dv::encode(message); dv::detail::transfer(connection.file.get(),true,bytes.data(),bytes.size(),connection.deadline); }
    std::optional<dv::Admission> receive() {
        dv::Message bytes{};
        try { dv::detail::transfer(connection.file.get(),false,bytes.data(),bytes.size(),connection.deadline); }
        catch (const dv::detail::Stop &) { return {}; }
        dv::Admission result; assert(dv::decode(bytes.data(),bytes.size(),result)); return result;
    }
};
static void raw(const release::Snapshot &approved, const std::wstring &scope) {
    { // Admit claiming another owner incarnation.
        Raw r(approved,scope); auto stale=r.admit; stale.incarnation+=1; r.send(stale);
        auto reply=r.receive(); assert(reply && reply->code==dv::Grant::Stale && reply->incarnation==r.admit.incarnation);
        std::cout << "raw: admit for another owner incarnation -> Stale\n";
    }
    { // Admit not bound to this connection's hello.
        Raw r(approved,scope); auto wrong=r.admit; wrong.nonce[5]^=1; r.send(wrong);
        assert(!r.receive()); std::cout << "raw: admit with foreign nonce -> connection dropped, no grant\n";
    }
    { // Grant, then give up without committing.
        Raw r(approved,scope); r.send(r.admit); auto grant=r.receive(); assert(grant && grant->code==dv::Grant::Ok);
        for (auto value : grant->handles) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
        std::cout << "raw: grant then close without commit\n";
    }
    baseline(approved,scope,"uncommitted grant");
    { // Commit naming another registration id.
        Raw r(approved,scope); r.send(r.admit); auto grant=r.receive(); assert(grant && grant->code==dv::Grant::Ok);
        for (auto value : grant->handles) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
        auto commit=r.admit; commit.type=dv::Type::Commit; commit.value=grant->value+1; r.send(commit);
        assert(!r.receive()); std::cout << "raw: commit for another registration -> dropped\n";
    }
    baseline(approved,scope,"mismatched commit");
    { // A second admit on one connection is never served.
        Raw r(approved,scope); r.send(r.admit); auto grant=r.receive(); assert(grant && grant->code==dv::Grant::Ok);
        for (auto value : grant->handles) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
        auto again=r.admit; again.request+=1; r.send(again);
        assert(!r.receive()); std::cout << "raw: second admit on one connection -> dropped\n";
    }
    baseline(approved,scope,"second admit");
}
static void usable(hf::Session &session, uint64_t *generation=nullptr) {
    auto &registration=session.registration();
    assert(registration.deploymentVerified() && (session.id() & EngineOwner::HandoffIds));
    const auto status=request(registration,control::Operation::Inspect);
    assert(status.metadata.code==control::Code::Ok && !status.endpoint);
    if (generation) *generation=status.metadata.generation;
    auto allocated=request(registration,control::Operation::Allocate); assert(allocated.endpoint);
    Client client(allocated.endpoint->connect());
    assert(client.key(KeyAction::Refresh).input.empty());
    // Exactly-once on the query channel: a selection replay returns the cached
    // reply for the same request id and the gate refuses to apply it twice.
    client.type("nihaoz"); const auto selection=client.selection(find(client.current,"你好"));
    const auto response=client.exchange(selection); Commit commit;
    assert(client.candidates.confirm(selection,response,commit) && client.gate.accept(response,ep::now()));
    assert(commit.text=="你好" && commit.remaining=="z");
    const auto duplicate=client.exchange(selection); assert(duplicate.id==response.id && duplicate.payload==response.payload);
    assert(!client.gate.accept(duplicate,ep::now()) && client.key(KeyAction::Refresh).input=="z");
}
static void real(const Fixture &fixture, const std::filesystem::path &folder) {
    Job job;
    const auto scope=L"handoff-"+std::to_wstring(GetCurrentProcessId());
    const auto approved=fixture.approved();
    admit(approved,scope,"nothing running",Result::NotRunning);
    hf::AttachOptions lone; lone.handoff=options(scope);
    auto none=hf::attach(approved,lone);
    std::cout << "attach without starter: " << label(none.outcome.result) << " attempts=" << none.attempts << '\n';
    assert(none.outcome.result==Result::NotRunning && none.attempts==1 && !none.starts);

    auto owner=std::make_unique<Owner>(fixture,scope);
    ep::Handle ownerQuery(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,owner->pid())); ep::check(bool(ownerQuery));
    { // A usable slot, then a query session through it.
        auto outcome=admit(approved,scope,"admit",Result::Admitted);
        assert(outcome.session->engine().ownerPid==owner->pid());
        assert(outcome.session->engine().hostPid==GetProcessId(owner->process->engineHandle()));
        uint64_t generation=0; usable(*outcome.session,&generation); assert(generation);
        std::cout << "query through handed-off slot: ok (id=0x" << std::hex << outcome.session->id() << std::dec << ")\n";
        // The client gives the slot back; its retained wrapper is closed for good.
        outcome.session->release();
        assert(outcome.session->registration().poll(ep::now())==control::State::Failed);
        assert(!outcome.session->registration().request(control::Operation::Inspect,ep::now()));
    }
    baseline(approved,scope,"release");
    { // Two clients race for the last slot.
        std::vector<std::unique_ptr<hf::Session>> held;
        for (size_t i=0;i+1<admission::Limit;++i) held.push_back(std::move(admit(approved,scope,"fill",Result::Admitted).session));
        Result results[2]{Result::Failed,Result::Failed}; std::unique_ptr<hf::Session> won[2];
        std::thread a([&] { auto o=hf::admit(approved,options(scope)); results[0]=o.result; won[0]=std::move(o.session); });
        std::thread b([&] { auto o=hf::admit(approved,options(scope)); results[1]=o.result; won[1]=std::move(o.session); });
        a.join(); b.join();
        std::cout << "race for last slot: " << label(results[0]) << " / " << label(results[1]) << '\n';
        assert((results[0]==Result::Admitted)+(results[1]==Result::Admitted)==1);
        assert((results[0]==Result::Full)+(results[1]==Result::Full)==1);
    }
    baseline(approved,scope,"last-slot race");
    { // 8 concurrent clients all get a slot from the single serialized pipe.
        std::vector<std::thread> threads; std::vector<hf::Outcome> outcomes(admission::Limit);
        for (size_t i=0;i<outcomes.size();++i) threads.emplace_back([&,i] { outcomes[i]=hf::admit(approved,options(scope)); });
        for (auto &thread : threads) thread.join();
        std::set<uint64_t> ids;
        for (auto &o : outcomes) { assert(o.result==Result::Admitted); ids.insert(o.session->id()); }
        assert(ids.size()==admission::Limit);
        std::cout << "8 concurrent clients: 8 Admitted, distinct ids\n";
        usable(*outcomes[3].session);
    }
    baseline(approved,scope,"8 concurrent clients");
    { // A request id is served once per owner incarnation.
        auto opts=options(scope); opts.testRequestId=0x5eed5eed5eedull;
        auto first=hf::admit(approved,opts); assert(first.result==Result::Admitted);
        auto again=hf::admit(approved,opts);
        std::cout << "same request id again: " << label(again.result) << '\n';
        assert(again.result==Result::Duplicate && !again.session);
    }
    baseline(approved,scope,"duplicate request id");
    raw(approved,scope);
    { // Slow client: the owner gives up after its connection deadline and discards the slot.
        auto opts=options(scope); opts.discovery.timeoutMs=6000;
        opts.testHook=[](hf::Stage stage) { if (stage==hf::Stage::Granted) Sleep(static_cast<DWORD>(dv::DefaultConnectionMs+700)); };
        const auto before=handles(GetCurrentProcess());
        auto slow=hf::admit(approved,opts);
        std::cout << "slow client: " << label(slow.result) << " win32=" << slow.win32 << '\n';
        assert(slow.result==Result::Unavailable && !slow.session);
        assert(handles(GetCurrentProcess())<=before+2); // The granted handles were closed here.
    }
    baseline(approved,scope,"slow client");
    // Client processes that die mid-handoff.
    for (const wchar_t *stage : {L"verified",L"granted",L"committed"}) {
        std::vector<std::wstring> arguments{L"--die",stage,scope};
        for (auto &value : fixture.arguments()) arguments.push_back(value);
        const auto code=finish(job.start(arguments).get());
        std::wcout << L"client died at " << stage << L": exit=" << code << L'\n';
        assert(code==DieCode);
        baseline(approved,scope,"client death");
    }
    { // No leaked handles across many handoffs.
        { auto warm=hf::admit(approved,options(scope)); assert(warm.result==Result::Admitted); }
        baseline(approved,scope,"warm-up");
        const auto ownerBefore=handles(ownerQuery.get()), selfBefore=handles(GetCurrentProcess());
        for (int i=0;i<24;++i) {
            auto o=hf::admit(approved,options(scope)); assert(o.result==Result::Admitted);
            request(o.session->registration(),control::Operation::Inspect);
        }
        baseline(approved,scope,"24 cycles");
        const auto ownerAfter=handles(ownerQuery.get()), selfAfter=handles(GetCurrentProcess());
        std::cout << "handles owner " << ownerBefore << "->" << ownerAfter << ", client " << selfBefore << "->" << selfAfter << '\n';
        assert(ownerAfter<=ownerBefore+6 && selfAfter<=selfBefore+6);
    }
    { // Owner stops between Grant and Commit.
        std::vector<std::unique_ptr<hf::Session>> held;
        held.push_back(std::move(admit(approved,scope,"held across stop",Result::Admitted).session));
        auto opts=options(scope);
        opts.testHook=[&](hf::Stage stage) { if (stage==hf::Stage::Granted) owner->stop(); };
        auto stopped=hf::admit(approved,opts);
        std::cout << "owner stopping mid-handoff: " << label(stopped.result) << '\n';
        assert((stopped.result==Result::Unavailable || stopped.result==Result::Closing) && !stopped.session);
        assert(owner->process->status().failure==EngineProcess::Failure::None);
        // The session held across the stop is closed, not silently retargeted.
        assert(held[0]->registration().poll(ep::now())==control::State::Failed);
        admit(approved,scope,"after stop",Result::NotRunning);
    }
    owner=std::make_unique<Owner>(fixture,scope);
    { // Owner stops right after verification.
        auto opts=options(scope);
        opts.testHook=[&](hf::Stage stage) { if (stage==hf::Stage::Verified) owner->stop(); };
        auto stopped=hf::admit(approved,opts);
        std::cout << "owner stopping after verification: " << label(stopped.result) << '\n';
        assert(stopped.result==Result::Unavailable && !stopped.session);
    }
    owner=std::make_unique<Owner>(fixture,scope);
    { // Owner crashes between Verified and Admit.
        auto opts=options(scope);
        opts.testHook=[&](hf::Stage stage) { if (stage==hf::Stage::Verified) owner->crash(); };
        auto crashed=hf::admit(approved,opts);
        std::cout << "owner crash after verification: " << label(crashed.result) << '\n';
        assert(crashed.result==Result::Unavailable && !crashed.session);
        settle(*owner->process,ep::now()+Timeout);
        // The host dies with the owner's Job; the lease goes with it.
        const auto deadline=ep::now()+Timeout;
        hf::Outcome after;
        do { after=hf::admit(approved,options(scope)); } while (after.result!=Result::NotRunning && ep::now()<deadline && (Sleep(50),true));
        assert(after.result==Result::NotRunning);
    }
    owner.reset();
    for (int attempt=0;;++attempt) { // The crashed owner's host may still hold the lease briefly.
        owner=std::make_unique<Owner>(fixture,scope,false);
        if (owner->ready) break;
        assert(owner->process->status().failure==EngineProcess::Failure::InstanceBusy && attempt<50); Sleep(100);
    }
    { // Owner crashes after Commit, before the client reads the acknowledgement.
        auto opts=options(scope);
        opts.testHook=[&](hf::Stage stage) { if (stage==hf::Stage::Committed) owner->crash(); };
        auto crashed=hf::admit(approved,opts);
        std::cout << "owner crash after commit: " << label(crashed.result) << '\n';
        assert(crashed.result==Result::Unavailable && !crashed.session);
    }
    owner.reset();

    // N concurrent starters converge on one owner.
    const auto shared=L"handoff-start-"+std::to_wstring(GetCurrentProcessId());
    constexpr size_t Starters=8;
    auto go=event(true), finished=event(true);
    std::vector<ep::Handle> done, processes; std::vector<std::filesystem::path> reports;
    for (size_t i=0;i<Starters;++i) {
        done.push_back(event(true)); reports.push_back(folder/(L"starter-"+std::to_wstring(i)+L".txt"));
        std::vector<std::wstring> arguments{L"--starter",shared,std::to_wstring(reinterpret_cast<uintptr_t>(go.get())),
            std::to_wstring(reinterpret_cast<uintptr_t>(done.back().get())),std::to_wstring(reinterpret_cast<uintptr_t>(finished.get())),reports.back().wstring()};
        for (auto &value : fixture.arguments()) arguments.push_back(value);
        processes.push_back(job.start(arguments,{go.get(),done.back().get(),finished.get()}));
    }
    ep::check(SetEvent(go.get()));
    for (size_t i=0;i<Starters;++i) {
        HANDLE pair[]{done[i].get(),processes[i].get()};
        const auto status=WaitForMultipleObjects(2,pair,FALSE,static_cast<DWORD>(Timeout*9));
        assert(status==WAIT_OBJECT_0);
    }
    std::set<DWORD> owners; size_t starts=0, launched=0, admitted=0, attempts=0;
    for (const auto &path : reports) {
        std::ifstream in(path); std::string result; DWORD pid=0; size_t start=0, tries=0, waits=0, made=0; int ok=0;
        in >> result >> pid >> start >> tries >> waits >> made >> ok;
        std::cout << "starter: " << result << " owner=" << pid << " starts=" << start << " attempts=" << tries << " lock waits=" << waits << '\n';
        admitted+=result=="Admitted" && ok; owners.insert(pid); starts+=start; launched+=made; attempts+=tries;
    }
    std::cout << "concurrent starters: " << admitted << "/" << Starters << " admitted, owners=" << owners.size() << ", owner launches=" << launched
        << ", attempts=" << attempts << '\n';
    assert(admitted==Starters && owners.size()==1 && !owners.count(0) && starts==1 && launched==1 && attempts<=Starters*3);
    { // A further starter finds it running and starts nothing.
        hf::AttachOptions again; again.handoff=options(shared);
        again.start=[](uint64_t) { assert(false && "must not start"); return hf::Start::Failed; };
        auto outcome=hf::attach(approved,again);
        assert(outcome.outcome.result==Result::Full && !outcome.starts); // 8 starters hold all slots.
        std::cout << "extra starter with 8 slots held: Full, no start\n";
    }
    ep::check(SetEvent(finished.get()));
    for (auto &process : processes) assert(finish(process.get())==StarterBase+static_cast<DWORD>(Result::Admitted));
    { // The owner lived in the winning starter's Job.
        const auto deadline=ep::now()+Timeout; hf::Outcome after;
        do { after=hf::admit(approved,options(shared)); } while (after.result!=Result::NotRunning && ep::now()<deadline && (Sleep(50),true));
        assert(after.result==Result::NotRunning);
    }
    { // A starter whose start fails gives up at once, without spinning.
        hf::AttachOptions failing; failing.handoff=options(L"handoff-fail-"+std::to_wstring(GetCurrentProcessId()));
        failing.start=[](uint64_t) { return hf::Start::Failed; };
        auto outcome=hf::attach(approved,failing);
        std::cout << "failing starter: " << label(outcome.outcome.result) << " attempts=" << outcome.attempts << " starts=" << outcome.starts << '\n';
        assert(outcome.outcome.result==Result::NotRunning && outcome.starts==1 && outcome.attempts==2);
    }
}

struct Root {
    std::filesystem::path path;
    Root() {
        std::wstring temp(MAX_PATH+1,L'\0'); temp.resize(GetTempPathW(static_cast<DWORD>(temp.size()),temp.data()));
        path=std::filesystem::path(temp)/(L"harukoto-handoff-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(path);
    }
    ~Root() {
        for (int i=0;i<50;++i) {
            std::error_code error; std::filesystem::remove_all(path,error);
            if (!std::filesystem::exists(path,error)) return;
            Sleep(100);
        }
        std::cout << "warning: could not remove temporary directory\n";
    }
};

int wmain(int argc, wchar_t **argv) {
    std::cout << std::unitbuf;
    try {
        if (argc>=2) {
            const std::wstring mode=argv[1];
            if (mode==L"--die" && argc==11) return dieMode(argv);
            if (mode==L"--starter" && argc==14) return starterMode(argv);
        }
        units();
        if (argc!=5) { std::cout << "real owner skipped (no fixture arguments)\n"; return 0; }
        Fixture fixture;
        fixture.owner=argv[1]; fixture.host=fixture.owner.parent_path()/L"harukoto_engine_host.exe";
        fixture.base=argv[2]; fixture.themes=argv[3]; fixture.personal=argv[4];
        fixture.ownerSha=hash(fixture.owner); fixture.hostSha=hash(fixture.host);
        {
            Root root;
            real(fixture,root.path);
        }
        std::cout << "engine handoff tests passed\n";
        return 0;
    } catch (const ep::Error &error) {
        std::cerr << "pipe error reason=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
    } catch (const std::exception &error) { std::cerr << "error: " << error.what() << '\n'; }
    return 1;
}
