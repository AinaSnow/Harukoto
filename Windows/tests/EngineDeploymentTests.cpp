#include "../EngineProcess.h"
#include <cassert>
#include <fstream>
#include <iostream>

using namespace harukoto::engine;
namespace ep=harukoto::engine::pipe;
using State=EngineProcess::State;
static std::filesystem::path self() {
    std::wstring value(32768,L'\0'); const auto size=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    assert(size && size<value.size()); value.resize(size); return value;
}
static std::string hash(const std::filesystem::path &path) { return base_dictionary::StableFile(path).read(true); }
template<class Function> static void rejected(Function function) {
    bool failed=false;
    try { function(); } catch (const ep::Error &error) { assert(error.reason==ep::Failure::Authentication); failed=true; }
    assert(failed);
}
template<class Function> static void invalid(Function function) {
    bool failed=false; try { function(); } catch (const std::invalid_argument &) { failed=true; } assert(failed);
}
static void until(EngineProcess &manager, State target) {
    const auto limit=ep::now()+10000;
    while (manager.poll(ep::now())!=target) {
        assert(ep::now()<limit && manager.status().state!=State::Exited);
        const auto handles=manager.waitHandles(); assert(!handles.empty());
        const auto time=ep::now();
        const auto deadline=manager.deadline() ? std::min(limit,manager.deadline()) : limit;
        assert(WaitForMultipleObjects(static_cast<DWORD>(handles.size()),handles.data(),FALSE,
            static_cast<DWORD>(deadline>time ? deadline-time : 0))!=WAIT_FAILED);
    }
}
// Deliberately small real child. The wrong-host mode publishes its own owner
// object instead of the approved host: a valid private anchor alone must fail.
static int probe(int argc, wchar_t **argv) {
    HANDLE ready=nullptr, stop=nullptr, anchor=nullptr;
    std::filesystem::path base;
    for (int i=1;i<argc;i+=2) {
        assert(i+1<argc); const std::wstring key=argv[i], value=argv[i+1];
        auto handle=[&] { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value))); };
        if (key==L"--base") base=value;
        else if (key==L"--ready") ready=handle();
        else if (key==L"--stop") stop=handle();
        else if (key==L"--anchor") anchor=handle();
        else assert(key==L"--pipe" || key==L"--host" || key==L"--host-sha256" || key==L"--control-section" ||
            key==L"--control-request" || key==L"--control-response" || key==L"--connections");
    }
    assert(ready && stop);
    std::unique_ptr<EngineOwnerAnchor> publication;
    if (base.filename()==L"wrong-host") {
        assert(anchor); publication=std::make_unique<EngineOwnerAnchor>(ep::Handle(anchor)); publication->publish(GetCurrentProcess());
    }
    ep::check(SetEvent(ready)); assert(WaitForSingleObject(stop,10000)==WAIT_OBJECT_0); return 0;
}
static void files(const std::filesystem::path &root) {
    const auto folder=root/L"部署 目录";
    std::filesystem::create_directory(folder);
    const auto executable=folder/L"引擎 程序.exe", copy=folder/L"copy.exe", alias=folder/L"hardlink.exe";
    std::filesystem::copy_file(self(),executable); std::filesystem::copy_file(self(),copy);
    const auto approved=hash(executable);
    assert(approved.size()==64);
    invalid([&] { EngineDeployment p(executable,""); });
    invalid([&] { EngineDeployment p(executable,std::string(64,'g')); });
    invalid([&] { EngineDeployment p(L"relative.exe",approved); });
    invalid([&] { EngineDeployment p(std::wstring(L"C:\\good\0bad",11),approved); });
    rejected([&] { EngineDeployment p(executable,std::string(64,'0')); });
    rejected([&] { EngineDeployment p(folder,approved); });
    const auto empty=folder/L"empty.exe"; std::ofstream(empty).close();
    rejected([&] { EngineDeployment p(empty,approved); });
    assert(CreateHardLinkW(alias.c_str(),executable.c_str(),nullptr));
    {
        std::string uppercase=approved;
        for (auto &c : uppercase) if (c>='a' && c<='f') c=char(c-'a'+'A');
        EngineDeployment pin(executable,uppercase), same(alias,approved), different(copy,approved);
        assert(pin.digest()==approved && pin.sameFile(same) && !pin.sameFile(different));
        assert(pin.path().native().find(L"\\\\?\\")==0);
        ep::Handle write(CreateFileW(executable.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        assert(!write && GetLastError()==ERROR_SHARING_VIOLATION);
        assert(!DeleteFileW(executable.c_str()));
        assert(!MoveFileW(executable.c_str(),(folder/L"renamed.exe").c_str()));
        assert(!MoveFileW(folder.c_str(),(root/L"renamed-parent").c_str()));
        std::ofstream(folder/L"other.txt") << "ordinary sibling creation still works";
        assert(std::filesystem::exists(folder/L"other.txt"));
        rejected([&] { pin.verify(GetCurrentProcess()); }); // Same bytes, another actual file.
        ep::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr)); assert(event);
        rejected([&] { pin.verify(event.get()); });
        EngineProcess::Config config{executable,root/L"normal"}; config.hostSha256=uppercase; config.diagnostics=true;
        EngineProcess process(config);
        assert(process.status().deploymentRequired && !process.status().deploymentVerified);
        until(process,State::Ready); assert(process.status().deploymentVerified);
        pin.verify(process.processHandle());
        rejected([&] { different.verify(process.processHandle()); });
        auto observation=diagnostics::read(process.processHandle());
        assert(observation.record->deploymentRequired && observation.record->deploymentVerified);
        process.requestStop(ep::now()); until(process,State::Exited);
        rejected([&] { pin.verify(process.processHandle()); });
    }
    // Every guard from success, failure and actual exit has now been released.
    const auto moved=root/L"释放后的目录";
    assert(MoveFileW(folder.c_str(),moved.c_str()));
    assert(DeleteFileW((moved/executable.filename()).c_str()));
}
static void processes(const std::filesystem::path &root) {
    const auto owner=root/L"owner.exe", host=root/L"host.exe";
    std::filesystem::copy_file(self(),owner); std::filesystem::copy_file(self(),host);
    EngineProcess::Config config{host,root/L"normal"};
    const auto approved=hash(host);
    config.ownerSha256=approved; invalid([&] { EngineProcess p(config); });
    config.hostSha256=approved; invalid([&] { EngineProcess p(config); }); // No owner mode.
    config.dynamicEndpoints=true; config.ownerExecutable=owner; config.ownerSha256.clear();
    invalid([&] { EngineProcess p(config); }); // Never partly verify an owner tree.
    config.ownerSha256=std::string(64,'0'); rejected([&] { EngineProcess p(config); });
    config.ownerSha256=approved; config.base=root/L"wrong-host"; config.diagnostics=true;
    {
        EngineProcess p(config); until(p,State::Exited);
        assert(p.status().failure==EngineProcess::Failure::DeploymentIdentity && p.status().win32==ERROR_INVALID_IMAGE_HASH);
        assert(p.status().deploymentRequired && !p.status().deploymentVerified && !p.engineHandle());
        assert(!p.takeRegistration(ep::now()) && !p.takeControl(ep::now()));
        auto o=diagnostics::read(p.processHandle());
        assert(o.record->failure==7 && o.record->deploymentRequired && !o.record->deploymentVerified);
    }
    const auto alias=root/L"owner-alias.exe";
    assert(CreateHardLinkW(alias.c_str(),host.c_str(),nullptr));
    config.ownerExecutable=alias; invalid([&] { EngineProcess p(config); });
    config.ownerExecutable.clear(); config.ownerSha256.clear(); config.hostSha256.clear(); config.base=root/L"normal";
    EngineProcess legacy(config); until(legacy,State::Ready);
    assert(!legacy.status().deploymentRequired && !legacy.status().deploymentVerified);
    legacy.requestStop(ep::now()); until(legacy,State::Exited);
}
int wmain(int argc, wchar_t **argv) {
    if (argc>1 && std::wstring(argv[1])==L"--base") return probe(argc,argv);
    assert(argc==2);
    try {
        files(argv[1]); processes(argv[1]);
        std::cout << "engine_deployment: trusted digest, stable paths, retained image, wrong-host refusal and release passed\n"; return 0;
    } catch (const ep::Error &error) { std::cerr << "deployment failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
}
