#include "../EngineProcess.h"
#include <cassert>
#include <iostream>

using namespace harukoto::engine;
namespace ep=harukoto::engine::pipe;
using State=EngineProcess::State;
constexpr uint64_t Timeout=10000;
static std::filesystem::path self() {
    std::wstring value(32768,L'\0'); const auto size=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    assert(size && size<value.size()); value.resize(size); return value;
}
static void wait(const std::vector<HANDLE> &handles, uint64_t deadline) {
    const auto time=ep::now(); assert(time<deadline && !handles.empty());
    const auto result=WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,static_cast<DWORD>(deadline-time));
    assert(result!=WAIT_FAILED && result!=WAIT_TIMEOUT);
}
static void until(EngineProcess &process, State state) {
    const auto deadline=ep::now()+Timeout;
    while (process.poll(ep::now())!=state) {
        assert(process.status().state!=State::Exited);
        wait(process.waitHandles(),deadline);
    }
}
template<class Function> static void rejected(Function function, ep::Failure reason=ep::Failure::Authentication) {
    bool failed=false;
    try { function(); } catch (const ep::Error &error) { assert(error.reason==reason); failed=true; }
    assert(failed);
}
template<class Function> static void invalid(Function function) {
    bool failed=false; try { function(); } catch (const std::invalid_argument &) { failed=true; } assert(failed);
}
static std::array<ep::Handle,3> copies(const std::array<HANDLE,3> &handles) {
    std::array<ep::Handle,3> result;
    for (size_t i=0;i<3;++i) {
        HANDLE raw=nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),handles[i],GetCurrentProcess(),&raw,0,FALSE,DUPLICATE_SAME_ACCESS));
        result[i].reset(raw);
    }
    return result;
}
static Frame exchange(ep::Channel &channel, Frame frame) {
    const auto time=ep::now(), deadline=time+Timeout;
    assert(channel.beginRead(time,deadline) && channel.beginWrite(frame,time,deadline));
    std::optional<Frame> received;
    for (;;) {
        channel.poll(ep::now()); assert(!channel.closed());
        if (!received) received=channel.take();
        if (received && channel.writeDone()) return std::move(*received);
        std::vector<HANDLE> events;
        for (auto handle : {channel.readEvent(),channel.writeEvent()}) if (handle) events.push_back(handle);
        wait(events,deadline);
    }
}
static void handshake(ep::Channel &channel) {
    Frame hello{Kind::Hello,{1,0,0,0,1},{}};
    auto response=exchange(channel,hello);
    hello.id.engine=42;
    assert(response.kind==Kind::HelloAck && response.id==hello.id && response.payload.empty());
    Frame key{Kind::Key,{1,42,1,1,2},{1,2,3,4}};
    response=exchange(channel,key);
    assert(response.kind==Kind::Reply && response.id==key.id && response.payload==key.payload);
}
// Real child endpoint, not the full dictionary engine. All I/O runs on this
// thread; rejected clients may disconnect without sending even a Hello.
static int probe(int argc, wchar_t **argv) {
    ep::Handle ready, stop; std::vector<std::unique_ptr<ep::Listener>> listeners;
    std::vector<std::unique_ptr<ep::Channel>> clients;
    const auto time=ep::now(), deadline=time+Timeout;
    for (int i=1;i<argc;i+=2) {
        assert(i+1<argc); const std::wstring key=argv[i], value=argv[i+1];
        auto handle=[&] { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value))); };
        if (key==L"--ready") ready.reset(handle());
        else if (key==L"--stop") stop.reset(handle());
        else if (key==L"--pipe") listeners.push_back(std::make_unique<ep::Listener>(value,time,deadline));
        else assert(key==L"--base");
    }
    assert(ready && stop && !listeners.empty()); ep::check(SetEvent(ready.get()));
    for (;;) {
        if (WaitForSingleObject(stop.get(),0)==WAIT_OBJECT_0) return 0;
        std::vector<HANDLE> events{stop.get()};
        for (auto &listener : listeners) if (!listener->closed()) {
            listener->poll(ep::now());
            if (auto channel=listener->take()) {
                assert(channel->beginRead(ep::now(),deadline)); clients.push_back(std::move(channel));
            } else if (listener->event()) events.push_back(listener->event());
        }
        for (auto &client : clients) if (!client->closed()) {
            client->poll(ep::now());
            if (auto frame=client->take()) {
                if (frame->kind==Kind::Hello) { frame->kind=Kind::HelloAck; frame->id.engine=42; }
                else frame->kind=Kind::Reply;
                assert(client->beginWrite(*frame,ep::now(),deadline));
            }
            if (client->writeDone()) client->beginRead(ep::now(),deadline);
            for (auto handle : {client->readEvent(),client->writeEvent()}) if (handle) events.push_back(handle);
        }
        // beginRead/beginWrite have not issued their overlapped operation yet.
        bool immediate=false;
        for (auto &client : clients) immediate=immediate || client->needsPoll();
        if (!immediate) wait(events,deadline);
    }
}
static void exercise(const std::filesystem::path &root) {
    const auto folder=root/L"批准的部署", otherFolder=root/L"另一个部署", renamed=root/L"释放后的部署";
    std::filesystem::create_directory(folder); std::filesystem::create_directory(otherFolder);
    const auto executable=folder/L"查询宿主.exe", otherExecutable=otherFolder/L"owner.exe";
    std::filesystem::copy_file(self(),executable); std::filesystem::copy_file(self(),otherExecutable);
    // This fixture's hashes are known privately to the test launcher only.
    const auto digest=base_dictionary::StableFile(executable).read(true);
    auto deployment=std::make_shared<EngineDeployment>(executable,digest);
    auto otherDeployment=std::make_shared<EngineDeployment>(otherExecutable,digest);
    std::weak_ptr<const EngineDeployment> lifetime=deployment;
    std::unique_ptr<ep::Channel> first, second;
    {
        EngineProcess::Config config{executable,root/L"probe"}; config.connections=2;
        EngineProcess host(config); until(host,State::Ready);
        config.executable=otherExecutable; config.connections=2;
        EngineProcess other(config); until(other,State::Ready);
        auto firstEndpoint=host.takeEndpoint(ep::now()), secondEndpoint=host.takeEndpoint(ep::now());
        auto wrongEndpoint=other.takeEndpoint(ep::now()), legacyEndpoint=other.takeEndpoint(ep::now());
        assert(firstEndpoint && secondEndpoint && wrongEndpoint && legacyEndpoint);
        invalid([&] { EngineVerifiedPeer peer({},host.processHandle()); });
        rejected([&] { EngineVerifiedPeer peer(otherDeployment,host.processHandle()); }); // Equal bytes, different image file.
        ep::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr)); assert(event);
        rejected([&] { EngineVerifiedPeer peer(deployment,event.get()); });
        EngineVerifiedPeer peer(deployment,host.processHandle());
        DWORD flags=0; ep::check(GetHandleInformation(peer.processHandle(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        assert(!TerminateProcess(peer.processHandle(),1) && GetLastError()==ERROR_ACCESS_DENIED);
        HANDLE forbidden=nullptr;
        assert(!DuplicateHandle(peer.processHandle(),GetCurrentProcess(),GetCurrentProcess(),&forbidden,0,FALSE,DUPLICATE_SAME_ACCESS));
        assert(GetLastError()==ERROR_ACCESS_DENIED);
        ep::check(ImpersonateSelf(SecurityIdentification));
        rejected([&] { EngineVerifiedPeer impersonated(deployment,host.processHandle()); });
        rejected([&] { peer.connect(firstEndpoint->name); });
        ep::check(RevertToSelf());
        rejected([&] { peer.connect(wrongEndpoint->name); }); // Approved process cannot authenticate another pipe's creator.
        rejected([&] { peer.connect(L"\\\\remote\\pipe\\anything"); });
        first=peer.connect(firstEndpoint->name); handshake(*first);
        EngineRegistration::DeploymentPolicy policy{otherDeployment,deployment};
        // Reject a partial pair, either swapped image, and a single-file pair
        // before any request is published to the freshly handed-off mailbox.
        auto failedImport=[&](EngineRegistration::DeploymentPolicy bad, bool badFormat) {
            auto port=std::make_unique<control::Client>(); const auto handles=port->handles();
            control::Server server(copies(handles));
            auto attempt=[&] { EngineRegistration registration(std::move(port),other.processHandle(),host.processHandle(),nullptr,0,std::move(bad)); };
            if (badFormat) invalid(attempt); else rejected(attempt);
            assert(!server.take());
        };
        failedImport({otherDeployment,{}},true); failedImport({{},deployment},true);
        failedImport({deployment,otherDeployment},false);
        failedImport({otherDeployment,otherDeployment},true);
        // Each half fails independently while the other approved file remains correct.
        auto thirdFolder=root/L"第三个部署"; std::filesystem::create_directory(thirdFolder);
        auto thirdExe=thirdFolder/L"same.exe"; std::filesystem::copy_file(self(),thirdExe);
        auto third=std::make_shared<EngineDeployment>(thirdExe,digest);
        failedImport({third,deployment},false); failedImport({otherDeployment,third},false);
        auto port=std::make_unique<control::Client>(); control::Server server(copies(port->handles()));
        auto registration=std::make_unique<EngineRegistration>(std::move(port),other.processHandle(),host.processHandle(),nullptr,0,policy);
        assert(registration->deploymentVerified() && registration->state()==control::State::Idle && !server.take());
        assert(registration->request(control::Operation::Allocate,ep::now(),Timeout));
        auto request=server.take(); assert(request);
        control::Response response; response.request=*request; response.generation=42; response.limit=2; response.listening=1; response.name=secondEndpoint->name;
        server.respond(response);
        auto result=registration->take(ep::now()); assert(result && result->endpoint && !registration->take(ep::now()));
        assert(result->endpoint->deployment==deployment);
        second=result->endpoint->connect(); handshake(*second);
        registration.reset(); result.reset(); policy={}; // Channels must retain their own file guard.
        EngineRegistration legacy(std::make_unique<control::Client>(),other.processHandle(),host.processHandle());
        assert(!legacy.deploymentVerified());
        EngineRegistration::Endpoint legacyQuery{legacyEndpoint->name,std::move(legacyEndpoint->process),{}};
        auto channel=legacyQuery.connect(); handshake(*channel);
        host.requestStop(ep::now()); until(host,State::Exited);
        rejected([&] { peer.connect(firstEndpoint->name); }); // No replay / rediscovery after actual exit.
        rejected([&] { EngineVerifiedPeer dead(deployment,host.processHandle()); });
        assert(registration==nullptr);
        other.requestStop(ep::now()); until(other,State::Exited);
    }
    deployment.reset(); otherDeployment.reset();
    assert(!lifetime.expired() && !MoveFileW(folder.c_str(),renamed.c_str()));
    first.reset(); assert(!lifetime.expired() && !MoveFileW(folder.c_str(),renamed.c_str()));
    second.reset(); assert(lifetime.expired() && MoveFileW(folder.c_str(),renamed.c_str()));
    assert(DeleteFileW((renamed/executable.filename()).c_str()));
}
int wmain(int argc, wchar_t **argv) {
    if (argc>1 && std::wstring(argv[1])==L"--base") return probe(argc,argv);
    assert(argc==2);
    try {
        exercise(argv[1]);
        std::cout << "engine_verified_peer: private object, independent import, pipe binding and channel guard lifetime passed\n"; return 0;
    } catch (const ep::Error &error) { std::cerr << "peer failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
}
