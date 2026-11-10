#include "EngineOwner.h"
#include <iostream>

using namespace harukoto::engine;
// Developer-only independent owner; not installed, registered or auto-started.
int wmain(int argc, wchar_t **argv) {
    try {
        EngineProcess::Config config; config.dynamicEndpoints = true;
        std::array<pipe::Handle,6> inherited; // ready, stop, section, request, response, anchor
        std::vector<std::array<pipe::Handle,3>> registrations;
        bool threadsSet = false, connectionsSet = false;
        std::wstring instanceScope;
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
            else if (key == L"--base" && config.base.empty()) config.base = value;
            else if (key == L"--themes" && config.themes.empty()) config.themes = value;
            else if (key == L"--personal" && config.personal.empty()) config.personal = value;
            else if (key == L"--trace" && config.trace.empty()) config.trace = value;
            else if (key == L"--instance" && instanceScope.empty() && EngineInstance::validScope(value)) instanceScope = value;
            else if (key == L"--registration" && registrations.size() < 8) {
                std::array<pipe::Handle,3> port; size_t position = 0;
                for (size_t j=0;j<port.size();++j) {
                    const auto end = value.find(L',',position);
                    if ((j < 2) != (end != std::wstring::npos)) throw std::invalid_argument("invalid owner registration");
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
            else throw std::invalid_argument("invalid owner option");
        }
        if (!connectionsSet || std::any_of(inherited.begin(),inherited.end(),[](const auto &entry) { return !entry; }))
            throw std::invalid_argument("incomplete owner startup");
        pipe::processIdentityOnly();
        for (auto &handle : inherited) pipe::check(SetHandleInformation(handle.get(),HANDLE_FLAG_INHERIT,0));
        std::vector<std::unique_ptr<control::Server>> clientPorts;
        for (auto &port : registrations) clientPorts.push_back(std::make_unique<control::Server>(std::move(port)));
        std::unique_ptr<EngineInstance> instance;
        if (!instanceScope.empty()) {
            instance = std::make_unique<EngineInstance>(instanceScope);
            config.ownerLease = instance->handle();
        }
        control::Server upstream({std::move(inherited[2]),std::move(inherited[3]),std::move(inherited[4])});
        EngineOwnerAnchor anchor(std::move(inherited[5]));
        EngineOwner owner(std::move(config),upstream,anchor,std::move(clientPorts));
        return owner.run(inherited[1].get(),inherited[0].get()) ? 0 : 1;
    } catch (const EngineInstance::Busy &) {
        std::cerr << "engine_owner: instance already exists\n"; return ERROR_ALREADY_EXISTS;
    } catch (const pipe::Error &error) {
        std::cerr << "engine_owner failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
    } catch (...) { std::cerr << "engine_owner: startup or execution failed\n"; }
    return 1;
}
