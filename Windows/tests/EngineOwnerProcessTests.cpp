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
static EngineRegistration::Result request(EngineRegistration &registration, control::Operation operation) {
    assert(registration.request(operation,ep::now(),Timeout));
    while (registration.poll(ep::now()) == control::State::Pending)
        wait(registration.waitHandles(),registration.deadline());
    auto result = registration.take(ep::now()); assert(result); return std::move(*result);
}
static int worker(int argc, wchar_t **argv) {
    const bool registered = std::wstring(argv[1]) == L"--registered-worker";
    assert(argc == (registered ? 15 : 10)); const std::wstring mode = argv[2];
    std::unique_ptr<EngineRegistration> registration;
    ep::Handle engine; std::wstring name;
    const auto generation = std::stoull(argv[registered ? 3 : 4]);
    if (registered) {
        std::array<ep::Handle,3> port{inherited(argv[5]),inherited(argv[6]),inherited(argv[7])};
        auto owner = inherited(argv[8]), host = inherited(argv[9]), revoked = inherited(argv[10]);
        registration = std::make_unique<EngineRegistration>(std::move(port),owner.get(),host.get(),revoked.get(),std::stoull(argv[4]));
        assert(registration->id() == std::stoull(argv[4]));
        DWORD flags = 0; ep::check(GetHandleInformation(registration->revocationHandle(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        assert(!SetEvent(registration->revocationHandle()) && GetLastError() == ERROR_ACCESS_DENIED);
        assert(!ResetEvent(registration->revocationHandle()) && GetLastError() == ERROR_ACCESS_DENIED);
        assert(!TerminateProcess(registration->ownerHandle(),1) && GetLastError() == ERROR_ACCESS_DENIED);
        const auto status = request(*registration,control::Operation::Inspect); assert(status.metadata.generation == generation);
        auto allocated = request(*registration,control::Operation::Allocate); assert(allocated.endpoint);
        name = allocated.endpoint->name; engine = std::move(allocated.endpoint->process);
    } else { name = argv[3]; engine = inherited(argv[5]); }
    const auto offset = registered ? 11 : 6;
    auto ready = inherited(argv[offset]), go = inherited(argv[offset+1]), done = inherited(argv[offset+2]), finish = inherited(argv[offset+3]);
    assert(!TerminateProcess(engine.get(),1) && GetLastError() == ERROR_ACCESS_DENIED);
    Client client(name,engine.get()); assert(client.key(KeyAction::Refresh).input.empty());
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
    assert(WaitForSingleObject(finish.get(),Timeout) == WAIT_OBJECT_0); client.closed();
    if (registration) assert(registration->poll(ep::now()) == control::State::Failed && !registration->take(ep::now()));
    return 0;
}
// Test-only host. By default it exits on the first request; the trace-named
// fixture gates one request to make in-flight revocation deterministic.
static int faultHost(int argc, wchar_t **argv) {
    ep::Handle ready, stop; std::array<ep::Handle,3> handles;
    std::wstring gate;
    for (int i=1;i<argc;i+=2) {
        assert(i+1 < argc); const std::wstring key = argv[i];
        if (key == L"--ready") ready = inherited(argv[i+1]);
        else if (key == L"--stop") stop = inherited(argv[i+1]);
        else if (key == L"--control-section") handles[0] = inherited(argv[i+1]);
        else if (key == L"--control-request") handles[1] = inherited(argv[i+1]);
        else if (key == L"--control-response") handles[2] = inherited(argv[i+1]);
        else if (key == L"--trace") gate = std::filesystem::path(argv[i+1]).filename().wstring();
    }
    ep::Handle seen, release;
    if (gate.rfind(L"registration-gate-",0) == 0) {
        seen.reset(OpenEventW(EVENT_MODIFY_STATE,FALSE,(L"Local\\harukoto-"+gate+L"-seen").c_str()));
        release.reset(OpenEventW(SYNCHRONIZE,FALSE,(L"Local\\harukoto-"+gate+L"-release").c_str()));
        assert(seen && release);
    }
    assert(ready && stop); control::Server channel(std::move(handles)); ep::check(SetEvent(ready.get()));
    const HANDLE events[]{stop.get(),channel.event()};
    uint32_t requests = 0;
    for (;;) {
        const auto status = WaitForMultipleObjects(2,events,FALSE,Timeout);
        if (status == WAIT_OBJECT_0) return 0;
        assert(status == WAIT_OBJECT_0+1); auto request = channel.take(); assert(request);
        if (!seen) return 88;
        if (!requests) {
            ep::check(SetEvent(seen.get())); const HANDLE blocked[]{stop.get(),release.get()};
            const auto released = WaitForMultipleObjects(2,blocked,FALSE,Timeout);
            if (released == WAIT_OBJECT_0) return 0;
            assert(released == WAIT_OBJECT_0+1);
        }
        assert(request->operation == control::Operation::Inspect);
        control::Response response; response.request = *request; response.generation = 42; response.limit = 2;
        response.jobs = ++requests; channel.respond(response);
    }
}
struct Worker {
    ep::Handle ready = event(), go = event(), done = event(), finish = event(), process, job;
    Worker(EngineProcess::Endpoint endpoint, uint64_t generation, const wchar_t *mode)
        : Worker({L"--worker",mode,endpoint.name,std::to_wstring(generation)},{endpoint.process.get()}) {}
    Worker(std::unique_ptr<EngineRegistration> registration, uint64_t generation, const wchar_t *mode)
        : Worker({L"--registered-worker",mode,std::to_wstring(generation),std::to_wstring(registration->id())},registrationHandles(*registration)) {}
    static std::vector<HANDLE> registrationHandles(const EngineRegistration &registration) {
        const auto port = registration.handles();
        return {port[0],port[1],port[2],registration.ownerHandle(),registration.engineHandle(),registration.revocationHandle()};
    }
    Worker(std::vector<std::wstring> arguments, std::vector<HANDLE> sources) {
        job.reset(CreateJobObjectW(nullptr,nullptr)); ep::check(bool(job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ep::check(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
        std::vector<ep::Handle> copies;
        sources.insert(sources.end(),{ready.get(),go.get(),done.get(),finish.get()});
        for (const auto handle : sources) {
            HANDLE raw = nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
            copies.emplace_back(raw);
        }
        std::vector<HANDLE> handles; for (const auto &copy : copies) handles.push_back(copy.get());
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size); std::vector<uint8_t> bytes(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        ep::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),handles.size()*sizeof(HANDLE),nullptr,nullptr));
        const auto executable = self(); arguments.insert(arguments.begin(),executable.wstring());
        for (auto handle : handles) arguments.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(handle)));
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
static void lifetime(EngineProcess::Config value, bool registered = false) {
    value.ownerRegistrations = registered ? 4 : 0;
    EngineProcess owner(value); assert(!owner.engineHandle()); until(owner,State::Ready);
    assert(owner.engineHandle() && GetProcessId(owner.engineHandle()) != GetProcessId(owner.processHandle()));
    const auto initial = request(owner,control::Operation::Inspect);
    assert(!initial.metadata.connected && !initial.metadata.listening && initial.metadata.limit == 2);
    std::vector<std::unique_ptr<EngineRegistration>> registrations;
    if (registered) for (size_t i=0;i<4;++i) { auto port = owner.takeRegistration(ep::now()); assert(port); registrations.push_back(std::move(port)); }
    assert(!owner.takeRegistration(ep::now())); size_t next = 0; std::wstring firstName;
    auto launch = [&](const wchar_t *mode) {
        if (registered) {
            auto &port = registrations[next++]; const auto id = port->id();
            auto worker = std::make_unique<Worker>(std::move(port),initial.metadata.generation,mode);
            assert(owner.bindRegistration(id,worker->process.get()));
            assert(!owner.bindRegistration(id,worker->process.get())); return worker;
        }
        auto endpoint = request(owner,control::Operation::Allocate); assert(endpoint.endpoint);
        if (firstName.empty()) firstName = endpoint.endpoint->name;
        else if (std::wstring(mode) == L"replacement") assert(firstName != endpoint.endpoint->name);
        return std::make_unique<Worker>(std::move(*endpoint.endpoint),initial.metadata.generation,mode);
    };
    auto one = launch(L"first"); one->await(one->ready.get());
    auto two = launch(L"survivor"); two->await(two->ready.get());
    if (registered) {
        const auto handles = registrations[3]->handles();
        auto *view = static_cast<uint8_t *>(MapViewOfFile(handles[0],FILE_MAP_WRITE,0,0,control::PacketSize)); assert(view);
        std::fill_n(view,control::PacketSize,uint8_t(0)); UnmapViewOfFile(view); ep::check(SetEvent(handles[1]));
        inspectUntil(owner,[&](const auto &) { return WaitForSingleObject(registrations[3]->revocationHandle(),0) == WAIT_OBJECT_0; });
        assert(!registrations[3]->request(control::Operation::Inspect,ep::now(),Timeout));
        assert(registrations[3]->poll(ep::now()) == control::State::Failed);
        assert(registrations[3]->failure() == ep::Failure::Closed && !registrations[3]->take(ep::now()));
        assert(owner.status().registrationBound == 2 && owner.status().registrationRevoked == 1);
    }
    const auto full = request(owner,control::Operation::Allocate);
    assert(full.metadata.code == control::Code::Full && !full.endpoint && full.metadata.connected == 2);
    ep::check(SetEvent(one->go.get())); one->exited();
    inspectUntil(owner,[](const auto &r) { return r.connected == 1 && !r.retiring && !r.listening && !r.jobs; });
    if (registered) assert(owner.status().registrationBound == 1 && owner.status().registrationRevoked == 2);
    assert(owner.poll(ep::now()) == State::Ready && !writable(value.base/L"language-evidence.sqlite"));
    auto three = launch(L"replacement"); three->await(three->ready.get());
    ep::check(SetEvent(two->go.get())); two->await(two->done.get());
    ep::check(SetEvent(three->go.get())); three->await(three->done.get());
    assert(request(owner,control::Operation::Inspect).metadata.connected == 2);
    assert(owner.requestControl(control::Operation::Inspect,ep::now(),Timeout));
    owner.requestStop(ep::now()); until(owner,State::Exited); assert(!owner.takeControl(ep::now()));
    assert(owner.status().failure == EngineProcess::Failure::None && owner.status().exitCode == DWORD(0));
    assert(WaitForSingleObject(owner.engineHandle(),0) == WAIT_OBJECT_0 && writable(value.base/L"language-evidence.sqlite"));
    ep::check(SetEvent(two->finish.get())); ep::check(SetEvent(three->finish.get())); two->exited(); three->exited();
}
static void registrationRequests(EngineProcess::Config value, bool crash) {
    value.ownerRegistrations = 8;
    EngineProcess owner(value); until(owner,State::Ready);
    std::vector<std::unique_ptr<EngineRegistration>> registrations;
    for (size_t i=0;i<8;++i) { auto registration = owner.takeRegistration(ep::now()); assert(registration); registrations.push_back(std::move(registration)); }
    assert(!owner.takeRegistration(ep::now()));
    uint64_t generation = 0;
    // Equal client-local sequences must remain independent. All nine private
    // mailboxes are pending together, including the supervisor's own port.
    for (size_t round=0;round<4;++round) {
        for (auto &port : registrations) assert(port->request(round == 1 ? control::Operation::Allocate : control::Operation::Inspect,ep::now(),Timeout));
        assert(owner.requestControl(control::Operation::Inspect,ep::now(),Timeout));
        size_t allocations = 0, full = 0;
        for (auto &port : registrations) {
            assert(!port->request(control::Operation::Inspect,ep::now(),Timeout));
            while (port->poll(ep::now()) == control::State::Pending) wait(port->waitHandles(),port->deadline());
            auto response = port->take(ep::now()); assert(response && response->metadata.request.sequence == round+1);
            if (!generation) generation = response->metadata.generation;
            assert(response->metadata.generation == generation);
            if (round == 1) {
                if (response->metadata.code == control::Code::Ok) { ++allocations; assert(response->endpoint); }
                else { ++full; assert(response->metadata.code == control::Code::Full && !response->endpoint); }
            }
        }
        if (round == 1) assert(allocations == 2 && full == 6);
        while (owner.poll(ep::now()) == State::Ready && owner.status().controlState == control::State::Pending)
            wait(owner.waitHandles(),owner.deadline());
        auto supervisor = owner.takeControl(ep::now()); assert(supervisor && supervisor->metadata.generation == generation);
    }
    // Process exit must invalidate a reply even when its event is already set.
    assert(registrations[0]->request(control::Operation::Inspect,ep::now(),Timeout));
    while (registrations[0]->poll(ep::now()) == control::State::Pending)
        wait(registrations[0]->waitHandles(),registrations[0]->deadline());
    assert(registrations[0]->state() == control::State::Ready);
    assert(registrations[1]->request(control::Operation::Inspect,ep::now(),Timeout));
    if (crash) ep::check(TerminateProcess(owner.processHandle(),77));
    else owner.requestStop(ep::now());
    until(owner,State::Exited);
    assert(owner.status().failure == (crash ? EngineProcess::Failure::UnexpectedExit : EngineProcess::Failure::None));
    for (auto &port : registrations) {
        assert(port->poll(ep::now()) == control::State::Failed && port->failure() == ep::Failure::Closed);
        assert(!port->take(ep::now()) && !port->request(control::Operation::Inspect,ep::now()) && port->waitHandles().empty());
    }
    assert(WaitForSingleObject(owner.engineHandle(),Timeout) == WAIT_OBJECT_0);
}
static void registrationRevocation(EngineProcess::Config value) {
    const auto random = ep::newName(); const auto gate = L"registration-gate-"+random.substr(random.size()-32);
    ep::Handle seen(CreateEventW(nullptr,TRUE,FALSE,(L"Local\\harukoto-"+gate+L"-seen").c_str()));
    ep::Handle release(CreateEventW(nullptr,TRUE,FALSE,(L"Local\\harukoto-"+gate+L"-release").c_str()));
    assert(seen && release);
    value.executable = self(); value.trace = value.base.parent_path()/gate; value.ownerRegistrations = 2;
    EngineProcess owner(value); until(owner,State::Ready);
    auto running = owner.takeRegistration(ep::now()), queued = owner.takeRegistration(ep::now()); assert(running && queued);
    assert(running->request(control::Operation::Inspect,ep::now(),Timeout));
    assert(WaitForSingleObject(seen.get(),Timeout) == WAIT_OBJECT_0); // Host really owns this request.
    assert(queued->request(control::Operation::Allocate,ep::now(),Timeout));
    assert(!owner.revokeRegistration(0) && !owner.revokeRegistration(3));
    for (auto *port : {running.get(),queued.get()}) {
        assert(owner.revokeRegistration(port->id()) && !owner.revokeRegistration(port->id()));
        assert(port->poll(ep::now()) == control::State::Failed && port->failure() == ep::Failure::Closed);
        assert(!port->take(ep::now()) && !port->request(control::Operation::Inspect,ep::now()) && port->waitHandles().empty());
    }
    ep::check(SetEvent(release.get()));
    // The first late reply must be drained without touching a retired port;
    // the queued Allocate must never reach this Inspect-only test host.
    const auto result = request(owner,control::Operation::Inspect);
    assert(result.metadata.jobs == 2 && result.metadata.generation == 42);
    // Retained stale mailbox handles cannot reactivate either old slot.
    const auto handles = queued->handles();
    auto *view = static_cast<uint8_t *>(MapViewOfFile(handles[0],FILE_MAP_WRITE,0,0,control::PacketSize)); assert(view);
    const auto packet = control::packet(control::Request{2,control::Operation::Allocate});
    std::copy(packet.begin(),packet.end(),view); UnmapViewOfFile(view); ep::check(SetEvent(handles[1]));
    assert(request(owner,control::Operation::Inspect).metadata.jobs == 3);
    assert(owner.status().registrationConfigured == 2 && owner.status().registrationIssued == 2 && owner.status().registrationRevoked == 2);
    assert(!owner.takeRegistration(ep::now())); owner.requestStop(ep::now()); until(owner,State::Exited);
    assert(owner.status().failure == EngineProcess::Failure::None && owner.status().exitCode == DWORD(0));
}
static void registrationLiveness(EngineProcess::Config value) {
    value.ownerRegistrations = 4; EngineProcess owner(value); until(owner,State::Ready);
    auto ready = owner.takeRegistration(ep::now()), live = owner.takeRegistration(ep::now()), exited = owner.takeRegistration(ep::now());
    assert(ready && live && exited);
    assert(!owner.bindRegistration(0,GetCurrentProcess()) && !owner.bindRegistration(4,GetCurrentProcess()));
    auto wrong = event(); ep::check(SetEvent(wrong.get()));
    bool rejected = false;
    try { owner.bindRegistration(live->id(),wrong.get()); } catch (const ep::Error &e) { rejected = e.reason == ep::Failure::Authentication; }
    assert(rejected && !owner.status().registrationBound && !owner.status().registrationRevoked);
    auto allocation = request(*ready,control::Operation::Allocate); assert(allocation.endpoint);
    Client client(allocation.endpoint->name,allocation.endpoint->process.get()); client.type("riben");
    assert(ready->request(control::Operation::Inspect,ep::now(),Timeout));
    while (ready->poll(ep::now()) == control::State::Pending) wait(ready->waitHandles(),ready->deadline());
    assert(ready->state() == control::State::Ready && owner.revokeRegistration(ready->id()));
    assert(ready->poll(ep::now()) == control::State::Failed && !ready->take(ep::now()));
    assert(!owner.bindRegistration(ready->id(),GetCurrentProcess()));
    // Revocation withdraws control capability, preserving an existing query.
    assert(client.key(KeyAction::Refresh).input == "riben");
    assert(client.choose(find(client.current,"にほん")).text == "にほん");
    Worker active({L"--liveness-worker"},{}); active.await(active.ready.get());
    assert(owner.bindRegistration(live->id(),active.process.get()) && !owner.bindRegistration(live->id(),active.process.get()));
    assert(owner.status().registrationBound == 1);
    ep::check(TerminateProcess(active.process.get(),77));
    wait(owner.waitHandles(),ep::now()+Timeout); assert(owner.poll(ep::now()) == State::Ready);
    assert(live->poll(ep::now()) == control::State::Failed && live->failure() == ep::Failure::Closed);
    assert(owner.status().registrationBound == 0 && owner.status().registrationRevoked == 2);
    const auto waits = owner.waitHandles();
    assert(WaitForMultipleObjects(static_cast<DWORD>(waits.size()),waits.data(),FALSE,0) == WAIT_TIMEOUT);
    Worker finished({L"--liveness-worker"},{}); finished.await(finished.ready.get());
    ep::check(SetEvent(finished.go.get())); finished.exited();
    assert(owner.bindRegistration(exited->id(),finished.process.get()));
    assert(exited->poll(ep::now()) == control::State::Failed && !owner.bindRegistration(exited->id(),finished.process.get()));
    assert(owner.status().registrationRevoked == 3);
    // Issuing the remaining fresh slot works, but retired slots are never reused.
    auto fresh = owner.takeRegistration(ep::now()); assert(fresh && fresh->id() == 4);
    assert(request(*fresh,control::Operation::Inspect).metadata.connected == 1 && !owner.takeRegistration(ep::now()));
    assert(owner.status().registrationIssued == 4);
    owner.requestStop(ep::now()); until(owner,State::Exited); client.closed();
    assert(owner.status().registrationRevoked == 4 && owner.status().failure == EngineProcess::Failure::None);
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
        auto fault = value; fault.executable = self(); fault.ownerRegistrations = 1;
        EngineProcess owner(fault); until(owner,State::Ready);
        auto registration = owner.takeRegistration(ep::now()); assert(registration);
        assert(!TerminateProcess(owner.engineHandle(),88) && GetLastError() == ERROR_ACCESS_DENIED);
        assert(registration->request(control::Operation::Inspect,ep::now(),Timeout)); until(owner,State::Exited);
        assert(owner.status().failure == EngineProcess::Failure::UnexpectedExit && owner.status().exitCode == DWORD(1));
        assert(WaitForSingleObject(owner.engineHandle(),0) == WAIT_OBJECT_0 && !owner.takeControl(ep::now()));
        assert(registration->poll(ep::now()) == control::State::Failed && !registration->take(ep::now()));
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
    if (argc > 1 && std::wstring(argv[1]) == L"--liveness-worker") {
        assert(argc == 6); auto ready = inherited(argv[2]), go = inherited(argv[3]), done = inherited(argv[4]), finish = inherited(argv[5]);
        ep::check(SetEvent(ready.get())); assert(WaitForSingleObject(go.get(),Timeout) == WAIT_OBJECT_0); return 0;
    }
    if (argc > 1 && (std::wstring(argv[1]) == L"--worker" || std::wstring(argv[1]) == L"--registered-worker")) return worker(argc,argv);
    if (argc > 1 && std::wstring(argv[1]) == L"--base") return faultHost(argc,argv);
    try {
        const auto value = config(argc,argv); lifetime(value); lifetime(value,true);
        registrationRequests(value,false); registrationRequests(value,true); registrationRevocation(value); registrationLiveness(value);
        failures(value); singleInstance(value);
        std::cout << "engine_owner_process: separate owner/host, private registrations, revocation, process liveness, isolated requests, query/selection and cleanup passed\n";
        return 0;
    } catch (const ep::Error &error) { std::cerr << "owner process failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
}
