#include "EngineHost.h"
#include "EngineWindows.h"
#include <iostream>

using namespace harukoto::engine;
// This is an experimental developer executable, not installed or auto-started.
int wmain(int argc, wchar_t **argv) {
    try {
        std::filesystem::path base, themes, personal;
        std::vector<std::wstring> names;
        pipe::Handle ready, stop;
        auto control = [](const std::wstring &value) {
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::invalid_argument("invalid inherited control handle");
            const auto number = std::stoull(value);
            if (!number || number >= std::numeric_limits<uintptr_t>::max()) throw std::invalid_argument("invalid inherited control handle");
            const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number));
            DWORD flags = 0; pipe::check(GetHandleInformation(handle,&flags));
            if (!(flags & HANDLE_FLAG_INHERIT)) throw std::invalid_argument("control handle is not inherited");
            return handle;
        };
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (++i >= argc) throw std::invalid_argument("missing host option");
            const std::wstring value = argv[i];
            if (option == L"--base" && base.empty()) base = value;
            else if (option == L"--themes" && themes.empty()) themes = value;
            else if (option == L"--personal" && personal.empty()) personal = value;
            else if (option == L"--pipe" && names.size() < Host::MaxConnections) names.push_back(value);
            else if (option == L"--ready" && !ready) {
                const auto handle = control(value); if (handle == stop.get()) throw std::invalid_argument("duplicate host control"); ready.reset(handle);
            } else if (option == L"--stop" && !stop) {
                const auto handle = control(value); if (handle == ready.get()) throw std::invalid_argument("duplicate host control"); stop.reset(handle);
            }
            else throw std::invalid_argument("invalid host option");
        }
        if (base.empty() || !base.is_absolute() || (!themes.empty() && !themes.is_absolute()) ||
                (!personal.empty() && !personal.is_absolute()) || !ready || !stop || ready.get() == stop.get())
            throw std::invalid_argument("invalid host startup");
        // Prevent accidental leakage if future code launches a child. Never use
        // these command-line handle values as a substitute for caller trust.
        pipe::check(SetHandleInformation(ready.get(),HANDLE_FLAG_INHERIT,0));
        pipe::check(SetHandleInformation(stop.get(),HANDLE_FLAG_INHERIT,0));
        uint64_t generation = 0;
        do {
            if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&generation),sizeof(generation),BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("engine generation failure");
        } while (!generation);
        auto dictionary = WindowsDictionary::load(base,themes,personal);
        Host host(names,dictionary,generation,[](const ServerConnection::Ticket &work) {
            if (!work->dictionary) throw std::invalid_argument("missing engine snapshot");
            return work->dictionary->query(work);
        },Host::Options{});
        host.run(stop.get(),ready.get());
        return 0;
    } catch (const pipe::Error &error) {
        std::cerr << "engine_host failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
    } catch (...) { std::cerr << "engine_host: startup or execution failed\n"; }
    return 1;
}
