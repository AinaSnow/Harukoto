#include "../EngineProcess.h"
#include <cassert>
#include <iostream>

using namespace harukoto::engine;
namespace ep = harukoto::engine::pipe;
using State = EngineProcess::State;
using Failure = EngineProcess::Failure;

static std::filesystem::path self() {
    std::wstring buffer(32768,L'\0');
    const auto size = GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    assert(size && size < buffer.size()); buffer.resize(size); return buffer;
}
static std::filesystem::path quotedPath() { return self().parent_path()/L"包含 空格\\双\"引号\\末尾\\"; }

// A real child with the same control argv contract, deliberately omitting the
// dictionary. Faults remain isolated in our own kill-on-close Job.
static int probe(int argc, wchar_t **argv) {
    HANDLE ready = nullptr, stop = nullptr, anchor = nullptr;
    std::filesystem::path base, themes, personal;
    for (int i=1;i<argc;i+=2) {
        assert(i+1 < argc); const std::wstring key = argv[i], value = argv[i+1];
        if (key == L"--base") base = value;
        else if (key == L"--themes") themes = value;
        else if (key == L"--personal") personal = value;
        else if (key == L"--ready") ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value)));
        else if (key == L"--stop") stop = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value)));
        else if (key == L"--anchor") anchor = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value)));
        else if (key == L"--host") assert(std::filesystem::path(value).is_absolute());
        else if (key == L"--control-section" || key == L"--control-request" || key == L"--control-response") {
            const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value)));
            DWORD flags = 0; ep::check(GetHandleInformation(handle,&flags)); assert(flags & HANDLE_FLAG_INHERIT);
        }
        else if (key == L"--connections") assert(value == L"2");
        else assert(key == L"--pipe" && ep::validName(value));
    }
    assert(ready && stop && ready != stop);
    DWORD flags = 0;
    ep::check(GetHandleInformation(ready,&flags)); assert(flags & HANDLE_FLAG_INHERIT);
    ep::check(GetHandleInformation(stop,&flags)); assert(flags & HANDLE_FLAG_INHERIT);
    const auto mode = base.filename();
    std::unique_ptr<EngineOwnerAnchor> ownerAnchor;
    if (mode == L"owner-valid") {
        assert(anchor); ownerAnchor = std::make_unique<EngineOwnerAnchor>(ep::Handle(anchor)); ownerAnchor->publish(GetCurrentProcess());
    }
    if (mode == L"early-exit") return 0;
    if (mode == L"hang-start") { WaitForSingleObject(stop,INFINITE); return 0; }
    if (mode == L"controls") {
        assert(themes == quotedPath());
        // If a parent's unrelated inheritable event leaked, this signals it.
        const auto decoy = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(personal.filename().wstring())));
        SetEvent(decoy);
    }
    ep::check(SetEvent(ready));
    if (mode == L"ready-exit") return 23;
    if (mode == L"stubborn") {
        ep::Handle never(CreateEventW(nullptr,TRUE,FALSE,nullptr)); ep::check(bool(never));
        WaitForSingleObject(never.get(),INFINITE); return 0;
    }
    assert(WaitForSingleObject(stop,INFINITE) == WAIT_OBJECT_0); return 0;
}
static EngineProcess::Config config(const wchar_t *mode) {
    return {self(),self().parent_path()/mode,{},{},2,10000,1000};
}
static void until(EngineProcess &process, State target) {
    const auto limit = ep::now()+10000;
    while (process.poll(ep::now()) != target) {
        assert(process.status().state != State::Exited && ep::now() < limit);
        const auto events = process.waitHandles(); assert(!events.empty());
        const auto deadline = process.deadline() ? std::min(limit,process.deadline()) : limit;
        const auto time = ep::now();
        assert(WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,
            static_cast<DWORD>(deadline > time ? deadline-time : 0)) != WAIT_FAILED);
    }
}
static void invalid(const EngineProcess::Config &value) {
    bool rejected = false;
    try { EngineProcess process(value); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
}
static void anchors() {
    HANDLE escrow = nullptr;
    {
        EngineOwnerAnchor anchor;
        auto *view = static_cast<uint8_t *>(MapViewOfFile(anchor.section(),FILE_MAP_READ | FILE_MAP_WRITE,0,0,32));
        ep::check(view != nullptr);
        bool rejected = false;
        try { anchor.pin(GetCurrentProcess()); } catch (const ep::Error &) { rejected = true; }
        assert(rejected); // Unpublished / malformed bootstrap never becomes Ready.
        anchor.publish(GetCurrentProcess());
        escrow = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(get(view,8,8)));
        DWORD flags = 0; ep::check(GetHandleInformation(escrow,&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        auto pinned = anchor.pin(GetCurrentProcess()); assert(GetProcessId(pinned.get()) == GetCurrentProcessId());
        assert(!TerminateProcess(pinned.get(),77) && GetLastError() == ERROR_ACCESS_DENIED);
        rejected = false;
        try { anchor.publish(GetCurrentProcess()); } catch (const std::logic_error &) { rejected = true; }
        assert(rejected && reinterpret_cast<uintptr_t>(escrow) == get(view,8,8));
        for (size_t offset : {size_t(0),size_t(4),size_t(16),size_t(31)}) {
            const auto before = view[offset]; view[offset] = 255; rejected = false;
            try { anchor.pin(GetCurrentProcess()); } catch (const ep::Error &) { rejected = true; }
            assert(rejected); view[offset] = before;
        }
        UnmapViewOfFile(view);
    }
    // Escrow slot is deliberately retained through mapping destruction and all
    // host/owner teardown code; only OS process exit reclaims this one handle.
    assert(GetProcessId(escrow) == GetCurrentProcessId());
}
int wmain(int argc, wchar_t **argv) {
    if (argc > 1) return probe(argc,argv);
    try {
        anchors();
        auto value = config(L"normal"); value.executable = L"relative.exe"; invalid(value);
        value = config(L"normal"); value.base.clear(); invalid(value);
        value = config(L"normal"); value.connections = 0; invalid(value);
        value.connections = 9; invalid(value);
        value = config(L"normal"); value.startupMs = 0; invalid(value);
        value.startupMs = 300001; invalid(value);
        value = config(L"normal"); value.stopMs = 0; invalid(value);
        value.stopMs = 300001; invalid(value);
        value = config(L"normal"); value.themes = L"relative"; invalid(value);
        value = config(L"normal"); value.threads = 0; invalid(value); value.threads = 17; invalid(value);
        value = config(L"normal"); value.trace = L"relative.jsonl"; invalid(value);
        value = config(L"normal"); value.ownerExecutable = L"relative.exe"; value.dynamicEndpoints = true; invalid(value);
        value.ownerExecutable = self(); invalid(value); // Host and owner cannot recurse into the same executable.
        value.executable = self().parent_path()/L"different.exe"; value.dynamicEndpoints = false; invalid(value);
        value = config(L"normal"); value.personal = std::wstring(L"C:\\valid\0hidden",15); invalid(value);
        value = config(L"normal"); value.personal = L"C:\\"+std::wstring(33000,L'a'); invalid(value);
        value = config(L"normal"); value.executable = self().parent_path()/L"missing-engine.exe";
        bool failed = false;
        try { EngineProcess process(value); } catch (const ep::Error &) { failed = true; }
        assert(failed);
        {
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            ep::Handle decoy(CreateEventW(&security,TRUE,FALSE,nullptr)); ep::check(bool(decoy));
            value = config(L"controls"); value.themes = quotedPath();
            value.personal = self().parent_path()/std::to_wstring(reinterpret_cast<uintptr_t>(decoy.get()));
            EngineProcess process(value); until(process,State::Ready);
            assert(WaitForSingleObject(decoy.get(),0) == WAIT_TIMEOUT);
            assert(process.waitHandles().size() == 1 && !process.deadline());
            DWORD flags = 0; ep::check(GetHandleInformation(process.processHandle(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
            // No listeners in this fault probe: an attempted endpoint is burned,
            // and does not silently retry or make a replacement process.
            for (size_t i=0;i<2;++i) {
                failed = false;
                try { process.claim(ep::now()); } catch (const ep::Error &) { failed = true; }
                assert(failed && process.status().claimed == i+1 && process.status().state == State::Ready);
            }
            assert(!process.claim(ep::now()));
            process.requestStop(ep::now()); const auto deadline = process.deadline();
            process.requestStop(ep::now()); assert(process.status().state == State::Exited || process.deadline() == deadline);
            assert(!process.claim(ep::now())); until(process,State::Exited);
            const auto status = process.status();
            assert(status.failure == Failure::None && status.exitCode == DWORD(0) && process.waitHandles().empty() && !process.deadline());
            process.requestStop(ep::now()); assert(process.poll(ep::now()) == State::Exited);
        }
        {
            EngineProcess process(config(L"early-exit")); until(process,State::Exited);
            assert(process.status().failure == Failure::StartupExit && process.status().exitCode == DWORD(0));
        }
        {
            value = config(L"owner-valid"); value.dynamicEndpoints = true; value.ownerExecutable = self();
            value.executable = self().parent_path()/L"different.exe";
            EngineProcess process(value); assert(!process.engineHandle()); until(process,State::Ready);
            assert(process.engineHandle() && GetProcessId(process.engineHandle()) == GetProcessId(process.processHandle()));
            DWORD flags = 0; ep::check(GetHandleInformation(process.engineHandle(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
            assert(!TerminateProcess(process.engineHandle(),77) && GetLastError() == ERROR_ACCESS_DENIED);
            process.requestStop(ep::now()); until(process,State::Exited);
        }
        {
            value = config(L"normal"); value.dynamicEndpoints = true; value.ownerExecutable = self();
            value.executable = self().parent_path()/L"different.exe";
            EngineProcess process(value); until(process,State::Exited); // Probe signals ready without publishing an anchor.
            assert(process.status().failure == Failure::ControlIo && !process.engineHandle() && !process.takeControl(ep::now()));
        }
        {
            value = config(L"normal"); value.dynamicEndpoints = true;
            EngineProcess process(value); assert(!process.requestControl(control::Operation::Inspect,ep::now()));
            until(process,State::Ready); assert(process.status().controlState == control::State::Idle && !process.claim(ep::now()));
            assert(process.requestControl(control::Operation::Allocate,ep::now(),10000));
            const auto deadline = process.deadline(); assert(deadline && process.waitHandles().size() == 2);
            assert(!process.requestControl(control::Operation::Inspect,ep::now(),10000) && process.deadline() == deadline);
            process.poll(deadline); // Probe deliberately ignores the private mailbox.
            assert(process.status().state == State::Ready && process.status().controlState == control::State::Failed &&
                process.status().controlFailure == ep::Failure::Deadline && !process.deadline() && process.waitHandles().size() == 1);
            assert(!process.takeControl(ep::now()) && !process.requestControl(control::Operation::Allocate,ep::now()));
            process.requestStop(ep::now()); until(process,State::Exited);
            assert(process.status().failure == Failure::None && process.status().controlFailure == ep::Failure::Deadline);
        }
        {
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            ep::Handle decoy(CreateEventW(&security,TRUE,FALSE,nullptr)); ep::check(bool(decoy));
            value = config(L"controls"); value.dynamicEndpoints = true; value.themes = quotedPath();
            value.personal = self().parent_path()/std::to_wstring(reinterpret_cast<uintptr_t>(decoy.get()));
            EngineProcess process(value); until(process,State::Ready);
            assert(WaitForSingleObject(decoy.get(),0) == WAIT_TIMEOUT);
            process.requestStop(ep::now()); until(process,State::Exited);
        }
        {
            value = config(L"normal"); value.dynamicEndpoints = true;
            EngineProcess process(value); until(process,State::Ready);
            assert(process.requestControl(control::Operation::Inspect,ep::now(),10000));
            ep::check(TerminateProcess(process.processHandle(),77)); until(process,State::Exited);
            assert(process.status().failure == Failure::UnexpectedExit && process.status().controlState == control::State::Failed &&
                !process.takeControl(ep::now()) && !process.requestControl(control::Operation::Allocate,ep::now()));
        }
        {
            EngineProcess process(config(L"ready-exit"));
            assert(WaitForSingleObject(process.processHandle(),10000) == WAIT_OBJECT_0);
            assert(process.poll(ep::now()) == State::Exited);
            assert(process.status().failure == Failure::StartupExit && process.status().exitCode == DWORD(23));
            assert(!process.claim(ep::now())); // Both signals: exit wins, no endpoint exposed.
        }
        {
            EngineProcess process(config(L"hang-start")); assert(!process.claim(ep::now()));
            for (const auto handle : process.waitHandles()) {
                DWORD flags = 0; ep::check(GetHandleInformation(handle,&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
            }
            process.poll(process.deadline()); until(process,State::Exited);
            assert(process.status().failure == Failure::StartupTimeout && process.status().exitCode);
        }
        {
            EngineProcess process(config(L"normal"));
            assert(WaitForSingleObject(process.waitHandles()[1],10000) == WAIT_OBJECT_0);
            // A ready not observed until the exact deadline cannot extend it.
            process.poll(process.deadline()); until(process,State::Exited);
            assert(process.status().failure == Failure::StartupTimeout);
        }
        {
            EngineProcess process(config(L"hang-start"));
            process.requestStop(ep::now()); until(process,State::Exited);
            assert(process.status().failure == Failure::None && process.status().exitCode == DWORD(0));
        }
        {
            EngineProcess process(config(L"stubborn")); until(process,State::Ready);
            process.requestStop(ep::now()); const auto deadline = process.deadline();
            process.requestStop(ep::now()+100); assert(process.deadline() == deadline);
            process.poll(deadline); until(process,State::Exited);
            assert(process.status().failure == Failure::StopTimeout && process.status().exitCode);
        }
        {
            EngineProcess process(config(L"normal")); until(process,State::Ready);
            ep::check(TerminateProcess(process.processHandle(),77)); until(process,State::Exited);
            assert(process.status().failure == Failure::UnexpectedExit && process.status().exitCode == DWORD(77));
        }
        {
            EngineProcess process(config(L"normal")); until(process,State::Ready);
            auto endpoint = process.takeEndpoint(ep::now()); assert(endpoint && ep::validName(endpoint->name));
            assert(GetProcessId(endpoint->process.get()) == GetProcessId(process.processHandle()));
            DWORD flags = 0; ep::check(GetHandleInformation(endpoint->process.get(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
            assert(!TerminateProcess(endpoint->process.get(),77) && GetLastError() == ERROR_ACCESS_DENIED);
            process.requestStop(ep::now()); until(process,State::Exited);
            assert(WaitForSingleObject(endpoint->process.get(),0) == WAIT_OBJECT_0 && process.status().failure == Failure::None);
        }
        {
            ep::Handle retained;
            {
                value = config(L"stubborn"); value.dynamicEndpoints = true;
                EngineProcess process(value); until(process,State::Ready);
                HANDLE raw = nullptr;
                ep::check(DuplicateHandle(GetCurrentProcess(),process.processHandle(),GetCurrentProcess(),&raw,0,FALSE,DUPLICATE_SAME_ACCESS));
                retained.reset(raw); // Abandon the owner: Job closes without a wait.
            }
            assert(WaitForSingleObject(retained.get(),10000) == WAIT_OBJECT_0);
        }
        std::cout << "engine_process: controlled argv/inheritance, state/deadlines, endpoint attempts, stop/crash and Job cleanup passed\n";
        return 0;
    } catch (const ep::Error &error) {
        std::cerr << "engine_process failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1;
    }
}
