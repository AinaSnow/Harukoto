#include "../EngineProcess.h"
#include "EngineHostTestClient.h"

using namespace host_test;
using State = EngineProcess::State;
static ep::Handle event() { ep::Handle result(CreateEventW(nullptr,TRUE,FALSE,nullptr)); ep::check(bool(result)); return result; }
static std::filesystem::path self() {
    std::wstring value(32768,L'\0'); const auto size = GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    assert(size && size < value.size()); value.resize(size); return value;
}
static std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes*2+1 : slashes,L'\\'); slashes = 0; result += c;
    }
    result.append(slashes*2,L'\\'); return result+L"\"";
}
static ep::Handle inherited(const wchar_t *value) {
    ep::Handle result(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value))));
    DWORD flags = 0; ep::check(GetHandleInformation(result.get(),&flags)); assert(flags & HANDLE_FLAG_INHERIT);
    ep::check(SetHandleInformation(result.get(),HANDLE_FLAG_INHERIT,0)); return result;
}
static int worker(int argc, wchar_t **argv) {
    assert(argc == 10); const std::wstring mode = argv[2];
    auto engine = inherited(argv[4]); const auto generation = std::stoull(argv[5]);
    auto ready = inherited(argv[6]), go = inherited(argv[7]), done = inherited(argv[8]), finish = inherited(argv[9]);
    assert(!TerminateProcess(engine.get(),1) && GetLastError() == ERROR_ACCESS_DENIED);
    Client client(argv[3],engine.get()); assert(client.key(KeyAction::Refresh).input.empty());
    assert(client.last.id.engine == generation);
    if (mode == L"first") client.type("zhongguo");
    else if (mode == L"survivor") client.type("riben");
    else if (mode == L"replacement") {
        client.type("zhongguo",false); assert(client.choose(find(client.current,"中國")).text == "中國");
    } else assert(false);
    ep::check(SetEvent(ready.get())); assert(WaitForSingleObject(go.get(),Timeout) == WAIT_OBJECT_0);
    if (mode == L"first") return 0;
    if (mode == L"survivor") {
        assert(client.key(KeyAction::Refresh).input == "riben");
        assert(client.choose(find(client.current,"にほん")).text == "にほん");
        client.type("tejihuifuyao"); assert(client.choose(find(client.current,"特级恢复药")).text == "特级恢复药");
        client.type("nihaoz"); const auto selection = client.selection(find(client.current,"你好"));
        const auto response = client.exchange(selection); Commit commit;
        assert(client.candidates.confirm(selection,response,commit) && client.gate.accept(response,ep::now()));
        assert(commit.text == "你好" && commit.remaining == "z");
        const auto duplicate = client.exchange(selection); assert(duplicate.id == response.id && duplicate.payload == response.payload);
        assert(!client.gate.accept(duplicate,ep::now()) && client.key(KeyAction::Refresh).input == "z");
    }
    assert(client.last.id.engine == generation); ep::check(SetEvent(done.get()));
    assert(WaitForSingleObject(finish.get(),Timeout) == WAIT_OBJECT_0); client.closed(); return 0;
}
// A real fault child speaking only the inherited startup contract. Exit after
// the first control request, so the owner must notice host death without a reply.
static int faultHost(int argc, wchar_t **argv) {
    ep::Handle ready, stop; std::array<ep::Handle,3> handles;
    for (int i=1;i<argc;i+=2) {
        assert(i+1 < argc); const std::wstring key = argv[i];
        if (key == L"--ready") ready = inherited(argv[i+1]);
        else if (key == L"--stop") stop = inherited(argv[i+1]);
        else if (key == L"--control-section") handles[0] = inherited(argv[i+1]);
        else if (key == L"--control-request") handles[1] = inherited(argv[i+1]);
        else if (key == L"--control-response") handles[2] = inherited(argv[i+1]);
    }
    assert(ready && stop); control::Server channel(std::move(handles)); ep::check(SetEvent(ready.get()));
    const HANDLE events[]{stop.get(),channel.event()};
    const auto status = WaitForMultipleObjects(2,events,FALSE,Timeout);
    if (status == WAIT_OBJECT_0) return 0;
    assert(status == WAIT_OBJECT_0+1 && channel.take()); return 88;
}
struct Worker {
    ep::Handle ready = event(), go = event(), done = event(), finish = event(), process, job;
    Worker(EngineProcess::Endpoint endpoint, uint64_t generation, const wchar_t *mode) {
        job.reset(CreateJobObjectW(nullptr,nullptr)); ep::check(bool(job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ep::check(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
        std::vector<ep::Handle> copies;
        for (const auto handle : {endpoint.process.get(),ready.get(),go.get(),done.get(),finish.get()}) {
            HANDLE raw = nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
            copies.emplace_back(raw);
        }
        std::vector<HANDLE> handles; for (const auto &copy : copies) handles.push_back(copy.get());
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size); std::vector<uint8_t> bytes(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        ep::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),handles.size()*sizeof(HANDLE),nullptr,nullptr));
        const auto executable = self(); std::vector<std::wstring> arguments{executable.wstring(),L"--worker",mode,endpoint.name,
            std::to_wstring(reinterpret_cast<uintptr_t>(handles[0])),std::to_wstring(generation)};
        for (size_t i=1;i<handles.size();++i) arguments.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(handles[i])));
        std::wstring command; for (const auto &argument : arguments) { if (!command.empty()) command += L' '; command += quote(argument); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        ep::check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,executable.parent_path().c_str(),&startup.StartupInfo,&info));
        process.reset(info.hProcess); ep::Handle thread(info.hThread);
        try {
            ep::check(AssignProcessToJobObject(job.get(),process.get())); copies.clear();
            if (ResumeThread(thread.get()) == DWORD(-1)) throw ep::Error(ep::Failure::Io,GetLastError());
        } catch (...) { TerminateProcess(process.get(),99); throw; }
    }
    ~Worker() { job.reset(); }
    void await(HANDLE signal) { wait({process.get(),signal},ep::now()+Timeout); assert(WaitForSingleObject(signal,0) == WAIT_OBJECT_0); }
    void exited() {
        assert(WaitForSingleObject(process.get(),Timeout) == WAIT_OBJECT_0);
        DWORD code = 0; ep::check(GetExitCodeProcess(process.get(),&code)); assert(code == 0);
    }
};
static void until(EngineProcess &manager, State target) {
    const auto limit = ep::now()+Timeout;
    while (manager.poll(ep::now()) != target) {
        assert(manager.status().state != State::Exited && ep::now() < limit);
        const auto deadline = manager.deadline() ? std::min(manager.deadline(),limit) : limit;
        wait(manager.waitHandles(),deadline);
    }
}
static EngineProcess::ControlResult request(EngineProcess &manager, control::Operation operation) {
    assert(manager.requestControl(operation,ep::now(),Timeout));
    while (manager.status().controlState == control::State::Pending) {
        wait(manager.waitHandles(),manager.deadline()); assert(manager.poll(ep::now()) == State::Ready);
    }
    auto result = manager.takeControl(ep::now()); assert(result);
    if (result->endpoint) {
        DWORD flags = 0; ep::check(GetHandleInformation(result->endpoint->process.get(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        assert(GetProcessId(result->endpoint->process.get()) == GetProcessId(manager.engineHandle()));
        assert(GetProcessId(result->endpoint->process.get()) != GetProcessId(manager.processHandle()));
    }
    return std::move(*result);
}
template<class Predicate> static control::Response inspectUntil(EngineProcess &manager, Predicate predicate) {
    const auto deadline = ep::now()+Timeout;
    for (;;) {
        assert(ep::now() < deadline); auto result = request(manager,control::Operation::Inspect);
        if (predicate(result.metadata)) return std::move(result.metadata);
    }
}
static EngineProcess::Config config(int argc, wchar_t **argv) {
    assert(argc == 5);
    const std::filesystem::path owner = argv[1];
    EngineProcess::Config result{owner.parent_path()/L"harukoto_engine_host.exe",argv[2],argv[3],argv[4]};
    result.ownerExecutable = owner; result.dynamicEndpoints = true; result.connections = 2; return result;
}
static ep::Handle retain(HANDLE handle) {
    HANDLE raw = nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&raw,
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0)); return ep::Handle(raw);
}
static bool writable(const std::filesystem::path &path) {
    ep::Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)); return bool(file);
}
static void lifetime(const EngineProcess::Config &value) {
    EngineProcess owner(value); assert(!owner.engineHandle()); until(owner,State::Ready);
    assert(owner.engineHandle() && GetProcessId(owner.engineHandle()) != GetProcessId(owner.processHandle()));
    const auto initial = request(owner,control::Operation::Inspect);
    assert(!initial.metadata.connected && !initial.metadata.listening && initial.metadata.limit == 2);
    auto first = request(owner,control::Operation::Allocate); const auto originalName = first.endpoint->name;
    Worker one(std::move(*first.endpoint),initial.metadata.generation,L"first"); one.await(one.ready.get());
    auto second = request(owner,control::Operation::Allocate);
    Worker two(std::move(*second.endpoint),initial.metadata.generation,L"survivor"); two.await(two.ready.get());
    const auto full = request(owner,control::Operation::Allocate);
    assert(full.metadata.code == control::Code::Full && !full.endpoint && full.metadata.connected == 2);
    ep::check(SetEvent(one.go.get())); one.exited();
    inspectUntil(owner,[](const auto &r) { return r.connected == 1 && !r.retiring && !r.listening && !r.jobs; });
    assert(owner.poll(ep::now()) == State::Ready && !writable(value.base/L"language-evidence.sqlite"));
    auto third = request(owner,control::Operation::Allocate); assert(third.endpoint && third.endpoint->name != originalName);
    Worker three(std::move(*third.endpoint),initial.metadata.generation,L"replacement"); three.await(three.ready.get());
    ep::check(SetEvent(two.go.get())); two.await(two.done.get());
    ep::check(SetEvent(three.go.get())); three.await(three.done.get());
    assert(request(owner,control::Operation::Inspect).metadata.connected == 2);
    assert(owner.requestControl(control::Operation::Inspect,ep::now(),Timeout));
    owner.requestStop(ep::now()); until(owner,State::Exited); assert(!owner.takeControl(ep::now()));
    assert(owner.status().failure == EngineProcess::Failure::None && owner.status().exitCode == DWORD(0));
    assert(WaitForSingleObject(owner.engineHandle(),0) == WAIT_OBJECT_0 && writable(value.base/L"language-evidence.sqlite"));
    ep::check(SetEvent(two.finish.get())); ep::check(SetEvent(three.finish.get())); two.exited(); three.exited();
}
static void failures(const EngineProcess::Config &value) {
    {
        EngineProcess owner(value); until(owner,State::Ready);
        auto allocation = request(owner,control::Operation::Allocate); Client client(allocation.endpoint->name,allocation.endpoint->process.get());
        ep::check(TerminateProcess(owner.processHandle(),77)); until(owner,State::Exited); client.closed();
        assert(owner.status().failure == EngineProcess::Failure::UnexpectedExit && owner.status().exitCode == DWORD(77));
        assert(WaitForSingleObject(owner.engineHandle(),Timeout) == WAIT_OBJECT_0 && !owner.requestControl(control::Operation::Inspect,ep::now()));
    }
    {
        auto fault = value; fault.executable = self();
        EngineProcess owner(fault); until(owner,State::Ready);
        assert(!TerminateProcess(owner.engineHandle(),88) && GetLastError() == ERROR_ACCESS_DENIED);
        assert(owner.requestControl(control::Operation::Inspect,ep::now(),Timeout)); until(owner,State::Exited);
        assert(owner.status().failure == EngineProcess::Failure::UnexpectedExit && owner.status().exitCode == DWORD(1));
        assert(WaitForSingleObject(owner.engineHandle(),0) == WAIT_OBJECT_0 && !owner.takeControl(ep::now()));
    }
    {
        auto invalid = value; invalid.base /= L"missing";
        EngineProcess owner(invalid); until(owner,State::Exited);
        assert(owner.status().failure == EngineProcess::Failure::StartupExit && !owner.engineHandle());
    }
    {
        EngineProcess owner(value); owner.requestStop(ep::now()); until(owner,State::Exited);
        assert(!owner.takeControl(ep::now()) && owner.status().failure == EngineProcess::Failure::None);
    }
    {
        ep::Handle root, engine;
        { EngineProcess owner(value); until(owner,State::Ready); root = retain(owner.processHandle()); engine = retain(owner.engineHandle()); }
        assert(WaitForSingleObject(root.get(),Timeout) == WAIT_OBJECT_0 && WaitForSingleObject(engine.get(),Timeout) == WAIT_OBJECT_0);
    }
    assert(writable(value.base/L"language-evidence.sqlite"));
}
static void singleInstance(EngineProcess::Config value) {
    const auto random = ep::newName(); value.ownerInstance = L"test-"+random.substr(random.size()-32);
    std::vector<std::unique_ptr<EngineProcess>> contenders;
    for (size_t i=0;i<4;++i) {
        auto contender = value; contender.trace = value.base.parent_path()/(L"instance-race-"+std::to_wstring(i)+L".jsonl");
        contenders.push_back(std::make_unique<EngineProcess>(contender));
    }
    const auto deadline = ep::now()+Timeout; EngineProcess *winner = nullptr;
    for (;;) {
        size_t settled = 0; std::vector<HANDLE> handles;
        for (auto &candidate : contenders) {
            const auto state = candidate->poll(ep::now());
            if (state == State::Ready) { assert(!winner || winner == candidate.get()); winner = candidate.get(); ++settled; }
            else if (state == State::Exited) {
                assert(candidate->status().failure == EngineProcess::Failure::InstanceBusy && candidate->status().exitCode == DWORD(ERROR_ALREADY_EXISTS));
                assert(!candidate->engineHandle()); ++settled;
            } else { const auto events = candidate->waitHandles(); handles.insert(handles.end(),events.begin(),events.end()); }
        }
        if (settled == contenders.size()) break;
        assert(ep::now() < deadline && !handles.empty()); wait(handles,deadline);
    }
    assert(winner && request(*winner,control::Operation::Inspect).metadata.limit == 2);
    for (size_t i=0;i<contenders.size();++i)
        assert(std::filesystem::exists(value.base.parent_path()/(L"instance-race-"+std::to_wstring(i)+L".jsonl")) == (contenders[i].get() == winner));
    // Existing object is only a refusal, never evidence for a trusted handoff.
    { EngineProcess blocked(value); until(blocked,State::Exited); assert(blocked.status().failure == EngineProcess::Failure::InstanceBusy); }
    auto otherValue = value; otherValue.ownerInstance += L"-other";
    { EngineProcess other(otherValue); until(other,State::Ready); other.requestStop(ep::now()); until(other,State::Exited); }
    winner->requestStop(ep::now()); until(*winner,State::Exited);
    assert(winner->status().failure == EngineProcess::Failure::None && WaitForSingleObject(winner->engineHandle(),0) == WAIT_OBJECT_0);
    // Old management and exited process objects remain retained during reuse.
    { EngineProcess successor(value); until(successor,State::Ready);
      ep::check(TerminateProcess(successor.processHandle(),77)); until(successor,State::Exited);
      assert(WaitForSingleObject(successor.engineHandle(),Timeout) == WAIT_OBJECT_0); }
    { auto invalid = value; invalid.base /= L"missing"; EngineProcess bad(invalid); until(bad,State::Exited);
      assert(bad.status().failure == EngineProcess::Failure::StartupExit); }
    { EngineProcess successor(value); until(successor,State::Ready); successor.requestStop(ep::now()); until(successor,State::Exited); }
    assert(writable(value.base/L"language-evidence.sqlite"));
}
int wmain(int argc, wchar_t **argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--worker") return worker(argc,argv);
    if (argc > 1 && std::wstring(argv[1]) == L"--base") return faultHost(argc,argv);
    try {
        const auto value = config(argc,argv); lifetime(value); failures(value); singleInstance(value);
        std::cout << "engine_owner_process: separate owner/host, independent client exit/rejoin, restricted handoff, query/selection, stop/crash and Job cleanup passed\n";
        return 0;
    } catch (const ep::Error &error) { std::cerr << "owner process failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
}
