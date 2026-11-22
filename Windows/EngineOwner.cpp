#include "EngineOwner.h"
#include "EngineResident.h"
#include <iostream>

using namespace harukoto::engine;
namespace rs = harukoto::engine::resident;
static_assert(rs::HostThreads == admission::Limit, "one host worker per client slot");
#ifdef HARUKOTO_RESIDENT_TESTING
constexpr bool Testing = true;  // harukoto_engine_owner_test only: injectable roots, never packaged.
#else
constexpr bool Testing = false;
#endif

// --resident <logon|demand> [--base D] [--themes D] [--personal F]
// Resident owner (step 4, docs/ENGINE_LIFECYCLE.md). Started by the HKLM Run
// value at logon or by resident::ensure(); takes no inherited handles. It
// re-checks that it is the approved release, holds the instance lease, keeps
// one host running (restarting it within the breaker), publishes discovery
// and exits on logoff/shutdown, WM_CLOSE, idle (if configured) or a crash loop.
static int residentMain(int argc, wchar_t **argv) {
    if (argc < 3) throw std::invalid_argument("missing resident reason");
    const std::wstring why = argv[2];
    const auto reason = why == L"logon" ? rs::Reason::Logon : why == L"demand" ? rs::Reason::Demand : throw std::invalid_argument("invalid resident reason");
    std::wstring scope = rs::DefaultScope;
    release::Source source;
    rs::Store store;
    rs::Policy policy;
    std::filesystem::path base, themes, personal, memory, baseStore;
    std::optional<uint64_t> idleMs;
    std::optional<size_t> threads;
    for (int i=3;i<argc;i+=2) {
        if (i+1 >= argc) throw std::invalid_argument("missing resident option");
        const std::wstring key = argv[i], value = argv[i+1];
        auto path = [&](std::filesystem::path &target) {
            if (!target.empty() || !std::filesystem::path(value).is_absolute()) throw std::invalid_argument("invalid resident path");
            target = value;
        };
        if (key == L"--base") path(base);
        else if (key == L"--themes") path(themes);
        else if (key == L"--personal") path(personal);
        else if (key == L"--memory") path(memory);
        else if (key == L"--base-store") path(baseStore);
        else if (Testing && key == L"--scope" && EngineInstance::validScope(value)) scope = value;
        else if (Testing && key == L"--release-subkey") { source.root = HKEY_CURRENT_USER; source.subkey = value; source.requireProtected = false; }
        else if (Testing && key == L"--store-subkey") store = rs::Store::under(value);
        else if (Testing && key == L"--idle-ms" && !value.empty() && value.size() < 10 && value.find_first_not_of(L"0123456789") == std::wstring::npos) idleMs = std::stoull(value);
        else if (Testing && key == L"--threads" && value.size() <= 2 && !value.empty() && value.find_first_not_of(L"0123456789") == std::wstring::npos &&
                std::stoul(value) >= 1 && std::stoul(value) <= 16) threads = std::stoul(value);
        else throw std::invalid_argument("invalid resident option");
    }
    pipe::processIdentityOnly();
    // Only the approved release may serve, whoever started this process.
    const auto approved = release::select(release::read(source),ProtocolVersion);
    if (!approved) return static_cast<int>(rs::ExitNotApproved);
    std::unique_ptr<EngineDeployment> self;
    try { self = std::make_unique<EngineDeployment>(approved->owner,approved->ownerSha256); self->verify(GetCurrentProcess()); }
    catch (const pipe::Error &) { return static_cast<int>(rs::ExitNotApproved); }
    if (base.empty()) base = approved->owner.parent_path(); // Step 6 decides the packaged data layout.
    // A previous owner may still be finishing its stop (its host keeps the
    // lease until it exits): wait a little, then give up with Busy.
    std::unique_ptr<EngineInstance> instance;
    for (const auto until = pipe::now()+5000;;) {
        try { instance = std::make_unique<EngineInstance>(scope); break; }
        catch (const EngineInstance::Busy &) { if (pipe::now() >= until) throw; Sleep(100); }
    }
    std::unique_ptr<rs::Ledger> ledger;
    try { ledger = std::make_unique<rs::Ledger>(store,scope); } catch (...) {} // Diagnostics and breaker are best effort.
    rs::SessionWatch watch;
    const auto pid = GetCurrentProcessId();
    if (ledger) ledger->started(pid,rs::created(GetCurrentProcess()),reason,GetTickCount64());
    const uint64_t idle = idleMs ? *idleMs : uint64_t(rs::idleMinutes(store))*60000;
    std::deque<uint64_t> restarts;
    DWORD restartCount = 0;
    rs::Exit result = rs::Exit::Failed;
    for (bool first=true;;first=false) {
        if (ledger) ledger->state(first ? rs::State::Starting : rs::State::Restarting);
        EngineOwner::Exit how = EngineOwner::Exit::HostExited;
        try {
            EngineProcess::Config config{approved->host,base,themes,personal};
            config.dynamicEndpoints = true; config.connections = 8; config.hostSha256 = approved->hostSha256; config.memory = memory; config.baseStore = baseStore;
            config.ownerLease = instance->handle(); config.stopMs = 3000;
            config.threads = threads ? *threads : rs::HostThreads;
            EngineOwnerAnchor anchor;
            pipe::Handle ready(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(ready));
            EngineOwner::Lifecycle lifecycle{idle,watch.resume(),[&](size_t clients, DWORD host) { if (ledger) ledger->clients(clients,host); }};
            EngineOwner owner(std::move(config),nullptr,anchor,{},nullptr,scope,std::move(lifecycle));
            owner.run(watch.stop(),ready.get());
            how = owner.exitReason();
        } catch (const pipe::Error &error) {
            std::cerr << "engine_owner: host generation failed win32=" << error.code << '\n';
        }
        if (how == EngineOwner::Exit::Stopped) { result = watch.reason(); break; }
        if (how == EngineOwner::Exit::Idle) { result = rs::Exit::Idle; break; }
        if (WaitForSingleObject(watch.stop(),0) == WAIT_OBJECT_0) { result = watch.reason(); break; }
        // Host crashed, failed to start or its control failed. Clients see
        // Closed and fall back; they re-admit to the next host. Nothing is replayed.
        const auto backoff = rs::hostRestart(policy,restarts,pipe::now());
        if (!backoff) { result = rs::Exit::CrashLoop; break; }
        if (ledger) ledger->hostRestarts(++restartCount);
        if (WaitForSingleObject(watch.stop(),static_cast<DWORD>(*backoff)) == WAIT_OBJECT_0) { result = watch.reason(); break; }
    }
    if (ledger) {
        ledger->state(rs::State::Stopping);
        if (result == rs::Exit::CrashLoop) ledger->trip(GetTickCount64());
        else {
            // A clean exit (idle, logoff, asked to stop) forgives earlier on-demand starts.
            try { handoff::StarterLock lock(scope); if (lock.acquire(2000)) ledger->starts({}); } catch (...) {}
        }
        ledger->exited(result,pid,GetTickCount64());
    }
    watch.finished();
    return static_cast<int>(result == rs::Exit::CrashLoop ? rs::ExitCrashLoop : rs::ExitClean);
}

// Developer-only independent owner (launcher mode) unless --resident.
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"--resident") return residentMain(argc,argv);
        EngineProcess::Config config; config.dynamicEndpoints = true;
        std::array<pipe::Handle,9> inherited; // ready, stop, control3, anchor, optional admission3
        std::vector<std::array<pipe::Handle,4>> registrations;
        bool threadsSet = false, connectionsSet = false;
        std::wstring instanceScope;
        bool discoverable = false;
        auto number = [](const std::wstring &value) {
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) throw std::invalid_argument("invalid owner number");
            return std::stoull(value);
        };
        auto own = [&](size_t index, const std::wstring &value) {
            const auto n = number(value);
            if (!n || n >= std::numeric_limits<uintptr_t>::max()) throw std::invalid_argument("invalid owner handle");
            const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n));
            if (inherited[index] || std::any_of(inherited.begin(),inherited.end(),[&](const auto &entry) { return entry.get() == handle; }) ||
                    std::any_of(registrations.begin(),registrations.end(),[&](const auto &port) {
                        return std::any_of(port.begin(),port.end(),[&](const auto &entry) { return entry.get() == handle; });
                    }))
                throw std::invalid_argument("duplicate owner handle");
            DWORD flags = 0; pipe::check(GetHandleInformation(handle,&flags));
            if (!(flags & HANDLE_FLAG_INHERIT)) throw std::invalid_argument("owner handle is not inherited");
            inherited[index].reset(handle);
        };
        for (int i=1;i<argc;i+=2) {
            if (i+1 >= argc) throw std::invalid_argument("missing owner option");
            const std::wstring key = argv[i], value = argv[i+1];
            if (key == L"--host" && config.executable.empty()) config.executable = value;
            else if (key == L"--host-sha256" && config.hostSha256.empty() && value.size()==64 &&
                    std::all_of(value.begin(),value.end(),[](wchar_t c) { return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f') || (c>=L'A' && c<=L'F'); }))
            {
                for (auto c : value) config.hostSha256.push_back(static_cast<char>(c));
                config.hostSha256=EngineDeployment::digest(config.hostSha256);
            }
            else if (key == L"--base" && config.base.empty()) config.base = value;
            else if (key == L"--themes" && config.themes.empty()) config.themes = value;
            else if (key == L"--personal" && config.personal.empty()) config.personal = value;
            else if (key == L"--memory" && config.memory.empty()) config.memory = value;
            else if (key == L"--base-store" && config.baseStore.empty()) config.baseStore = value;
            else if (key == L"--trace" && config.trace.empty()) config.trace = value;
            else if (key == L"--instance" && instanceScope.empty() && EngineInstance::validScope(value)) instanceScope = value;
            else if (key == L"--discovery" && !discoverable && value == L"1") discoverable = true;
            else if (key == L"--registration" && registrations.size() < 8) {
                std::array<pipe::Handle,4> port; size_t position = 0;
                for (size_t j=0;j<port.size();++j) {
                    const auto end = value.find(L',',position);
                    if ((j+1 < port.size()) != (end != std::wstring::npos)) throw std::invalid_argument("invalid owner registration");
                    const auto n = number(value.substr(position,end == std::wstring::npos ? end : end-position));
                    if (!n || n >= std::numeric_limits<uintptr_t>::max()) throw std::invalid_argument("invalid registration handle");
                    const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n));
                    auto contains = [&](const auto &handles) {
                        return std::any_of(handles.begin(),handles.end(),[&](const auto &entry) { return entry.get() == handle; });
                    };
                    if (contains(inherited) || contains(port) || std::any_of(registrations.begin(),registrations.end(),contains))
                        throw std::invalid_argument("duplicate registration handle");
                    DWORD flags = 0; pipe::check(GetHandleInformation(handle,&flags));
                    if (!(flags & HANDLE_FLAG_INHERIT)) throw std::invalid_argument("registration handle is not inherited");
                    port[j].reset(handle); position = end == std::wstring::npos ? value.size() : end+1;
                }
                registrations.push_back(std::move(port));
            }
            else if (key == L"--threads" && !threadsSet) {
                const auto n = number(value); if (!n || n > 16) throw std::invalid_argument("invalid owner threads");
                config.threads = static_cast<size_t>(n); threadsSet = true;
            } else if (key == L"--connections" && !connectionsSet) {
                const auto n = number(value); if (!n || n > 8) throw std::invalid_argument("invalid owner connections");
                config.connections = static_cast<size_t>(n); connectionsSet = true;
            }
            else if (key == L"--ready") own(0,value);
            else if (key == L"--stop") own(1,value);
            else if (key == L"--control-section") own(2,value);
            else if (key == L"--control-request") own(3,value);
            else if (key == L"--control-response") own(4,value);
            else if (key == L"--anchor") own(5,value);
            else if (key == L"--admission-section") own(6,value);
            else if (key == L"--admission-request") own(7,value);
            else if (key == L"--admission-response") own(8,value);
            else throw std::invalid_argument("invalid owner option");
        }
        const bool admissionSet = bool(inherited[6]) || bool(inherited[7]) || bool(inherited[8]);
        if (!connectionsSet || std::any_of(inherited.begin(),inherited.begin()+6,[](const auto &entry) { return !entry; }) ||
                (admissionSet && std::any_of(inherited.begin()+6,inherited.end(),[](const auto &entry) { return !entry; })))
            throw std::invalid_argument("incomplete owner startup");
        // Discovery is scoped by the same deployment identifier as the lease.
        if (discoverable && instanceScope.empty()) throw std::invalid_argument("discovery requires an instance scope");
        pipe::processIdentityOnly();
        for (auto &handle : inherited) if (handle) pipe::check(SetHandleInformation(handle.get(),HANDLE_FLAG_INHERIT,0));
        std::unique_ptr<admission::Server> admissionPort;
        if (admissionSet) admissionPort = std::make_unique<admission::Server>(std::array<pipe::Handle,3>{
            std::move(inherited[6]),std::move(inherited[7]),std::move(inherited[8])},registrations.size());
        std::vector<EngineOwner::Registration> clientPorts;
        for (auto &port : registrations) {
            pipe::check(SetHandleInformation(port[3].get(),HANDLE_FLAG_INHERIT,0));
            clientPorts.push_back({std::make_unique<control::Server>(std::array<pipe::Handle,3>{
                std::move(port[0]),std::move(port[1]),std::move(port[2])}),std::move(port[3])});
        }
        std::unique_ptr<EngineInstance> instance;
        if (!instanceScope.empty()) {
            instance = std::make_unique<EngineInstance>(instanceScope);
            config.ownerLease = instance->handle();
        }
        control::Server upstream({std::move(inherited[2]),std::move(inherited[3]),std::move(inherited[4])});
        EngineOwnerAnchor anchor(std::move(inherited[5]));
        EngineOwner owner(std::move(config),&upstream,anchor,std::move(clientPorts),admissionPort.get(),
            discoverable ? instanceScope : std::wstring{});
        return owner.run(inherited[1].get(),inherited[0].get()) ? 0 : 1;
    } catch (const EngineInstance::Busy &) {
        std::cerr << "engine_owner: instance already exists\n"; return ERROR_ALREADY_EXISTS;
    } catch (const pipe::Error &error) {
        std::cerr << "engine_owner failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
    } catch (...) { std::cerr << "engine_owner: startup or execution failed\n"; }
    return 1;
}
