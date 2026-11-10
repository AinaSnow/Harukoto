#pragma once
#include "EnginePipe.h"
#include "EngineControl.h"
#include "EngineOwnerAnchor.h"
#include "EngineInstance.h"
#include <filesystem>

namespace harukoto::engine {
// Experimental launch owner. All calls belong to one service/I/O thread, never
// a TSF foreground callback. No discovery, restart, request replay or version selection.
class EngineProcess {
public:
    enum class State { Starting, Ready, Stopping, Terminating, Exited };
    enum class Failure { None, StartupExit, UnexpectedExit, StartupTimeout, StopTimeout, ControlIo, InstanceBusy };
    struct Config {
        std::filesystem::path executable, base, themes, personal;
        size_t connections = 2;
        uint64_t startupMs = 30000, stopMs = 5000;
        size_t threads = 2;
        std::filesystem::path trace;
        bool dynamicEndpoints = false;
        std::filesystem::path ownerExecutable;
        std::wstring ownerInstance; // Optional stable deployment scope, independent of version/path.
        HANDLE ownerLease = nullptr; // Borrowed only during direct dynamic host launch.
    };
    struct Status {
        State state;
        Failure failure;
        DWORD win32;
        std::optional<DWORD> exitCode;
        size_t claimed, configured;
        std::optional<control::State> controlState;
        pipe::Failure controlFailure = pipe::Failure::None;
        DWORD controlWin32 = ERROR_SUCCESS;
    };
    // Trusted owner-to-client handoff, never reconstructed from a public PID.
    // The retained process copy is query/wait only and non-inheritable. A trusted
    // launcher may explicitly duplicate it into its own client's handle list.
    struct Endpoint { std::wstring name; pipe::Handle process; };
    struct ControlResult { control::Response metadata; std::optional<Endpoint> endpoint; };
private:
    pipe::Handle ready_, stop_, process_, job_;
    std::vector<std::wstring> names_;
    std::unique_ptr<control::Client> control_;
    std::unique_ptr<EngineOwnerAnchor> ownerAnchor_;
    pipe::Handle engine_;
    State state_ = State::Starting;
    Failure failure_ = Failure::None;
    DWORD win32_ = ERROR_SUCCESS;
    std::optional<DWORD> exitCode_;
    size_t claimed_ = 0, configured_ = 0;
    uint64_t deadline_ = 0, stopMs_ = 0;

    static std::wstring quote(const std::wstring &value) {
        std::wstring output = L"\""; size_t slashes = 0;
        for (auto c : value) {
            if (c == L'\\') { ++slashes; continue; }
            output.append(c == L'\"' ? slashes*2+1 : slashes,L'\\'); slashes = 0; output += c;
        }
        output.append(slashes*2,L'\\'); return output + L"\"";
    }
    static bool absolute(const std::filesystem::path &path) {
        // Paths are trusted caller configuration, not a child's self-report.
        // Embedded NUL would truncate the executable or silently change argv.
        return path.is_absolute() && path.native().find(L'\0') == std::wstring::npos;
    }
    void terminate(Failure failure, DWORD win32 = ERROR_SUCCESS) {
        pipe::check(TerminateJobObject(job_.get(),ERROR_PROCESS_ABORTED));
        failure_ = failure; win32_ = win32; deadline_ = 0; state_ = State::Terminating;
        if (control_) control_->abandon();
    }
    void launch(const Config &config) {
        if (!absolute(config.executable) || !absolute(config.base) ||
                (!config.themes.empty() && !absolute(config.themes)) ||
                (!config.personal.empty() && !absolute(config.personal)) ||
                (!config.ownerExecutable.empty() && (!absolute(config.ownerExecutable) || !config.dynamicEndpoints || config.ownerExecutable == config.executable)) ||
                (!config.ownerInstance.empty() && (config.ownerExecutable.empty() || !EngineInstance::validScope(config.ownerInstance))) ||
                (config.ownerLease && (!config.ownerExecutable.empty() || !config.dynamicEndpoints || !config.ownerInstance.empty())) ||
                (!config.trace.empty() && !absolute(config.trace)) || !config.threads || config.threads > 16 ||
                !config.connections || config.connections > 8 || !config.startupMs || config.startupMs > 300000 ||
                !config.stopMs || config.stopMs > 300000)
            throw std::invalid_argument("invalid engine process configuration");
        pipe::processIdentityOnly();
        stopMs_ = config.stopMs;
        configured_ = config.connections;
        deadline_ = pipe::now() + config.startupMs;
        if (config.dynamicEndpoints) control_ = std::make_unique<control::Client>();
        else for (size_t i = 0; i < config.connections; ++i) names_.push_back(pipe::newName());
        if (!config.ownerExecutable.empty()) ownerAnchor_ = std::make_unique<EngineOwnerAnchor>();
        ready_.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(ready_));
        stop_.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(stop_));
        job_.reset(CreateJobObjectW(nullptr,nullptr)); pipe::check(bool(job_));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        pipe::check(SetInformationJobObject(job_.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
        pipe::Handle childReady, childStop;
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),ready_.get(),GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
        childReady.reset(raw);
        pipe::check(DuplicateHandle(GetCurrentProcess(),stop_.get(),GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
        childStop.reset(raw);
        std::array<pipe::Handle,3> childControl;
        pipe::Handle childAnchor;
        pipe::Handle childLease;
        if (config.ownerLease) {
            pipe::check(DuplicateHandle(GetCurrentProcess(),config.ownerLease,GetCurrentProcess(),&raw,
                SYNCHRONIZE | READ_CONTROL,TRUE,0));
            childLease.reset(raw);
        }
        if (ownerAnchor_) {
            pipe::check(DuplicateHandle(GetCurrentProcess(),ownerAnchor_->section(),GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
            childAnchor.reset(raw);
        }
        if (control_) {
            const auto handles = control_->handles();
            for (size_t i=0;i<handles.size();++i) {
                pipe::check(DuplicateHandle(GetCurrentProcess(),handles[i],GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
                childControl[i].reset(raw);
            }
        }
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        pipe::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        std::vector<HANDLE> inherited{childReady.get(),childStop.get()};
        if (control_) for (const auto &handle : childControl) inherited.push_back(handle.get());
        if (childAnchor) inherited.push_back(childAnchor.get());
        if (childLease) inherited.push_back(childLease.get());
        pipe::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited.data(),inherited.size()*sizeof(HANDLE),nullptr,nullptr));
        const auto executable = ownerAnchor_ ? config.ownerExecutable : config.executable;
        std::vector<std::wstring> arguments{executable.wstring(),L"--base",config.base.wstring()};
        if (ownerAnchor_) arguments.insert(arguments.end(),{L"--host",config.executable.wstring(),L"--anchor",
            std::to_wstring(reinterpret_cast<uintptr_t>(childAnchor.get()))});
        if (!config.ownerInstance.empty()) arguments.insert(arguments.end(),{L"--instance",config.ownerInstance});
        if (childLease) arguments.insert(arguments.end(),{L"--owner-lease",std::to_wstring(reinterpret_cast<uintptr_t>(childLease.get()))});
        if (!config.themes.empty()) { arguments.push_back(L"--themes"); arguments.push_back(config.themes.wstring()); }
        if (!config.personal.empty()) { arguments.push_back(L"--personal"); arguments.push_back(config.personal.wstring()); }
        if (config.threads != 2) { arguments.push_back(L"--threads"); arguments.push_back(std::to_wstring(config.threads)); }
        if (!config.trace.empty()) { arguments.push_back(L"--trace"); arguments.push_back(config.trace.wstring()); }
        for (const auto &name : names_) { arguments.push_back(L"--pipe"); arguments.push_back(name); }
        if (control_) {
            const wchar_t *keys[]{L"--control-section",L"--control-request",L"--control-response"};
            for (size_t i=0;i<childControl.size();++i) {
                arguments.push_back(keys[i]); arguments.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(childControl[i].get())));
            }
            arguments.push_back(L"--connections"); arguments.push_back(std::to_wstring(config.connections));
        }
        arguments.insert(arguments.end(),{L"--ready",std::to_wstring(reinterpret_cast<uintptr_t>(childReady.get())),
            L"--stop",std::to_wstring(reinterpret_cast<uintptr_t>(childStop.get()))});
        std::wstring command;
        for (const auto &argument : arguments) { if (!command.empty()) command += L' '; command += quote(argument); }
        if (command.size() >= 32767) throw std::invalid_argument("engine command line too long");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        pipe::check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,
            executable.parent_path().c_str(),&startup.StartupInfo,&info));
        process_.reset(info.hProcess); pipe::Handle thread(info.hThread);
        try {
            pipe::check(AssignProcessToJobObject(job_.get(),process_.get()));
            // Only the child's copies remain inheritable while it starts.
            childReady.reset(); childStop.reset();
            childAnchor.reset();
            childLease.reset();
            for (auto &handle : childControl) handle.reset();
            if (ResumeThread(thread.get()) == DWORD(-1)) throw pipe::Error(pipe::Failure::Io,GetLastError());
        } catch (...) {
            // Even assignment failure must not leave our suspended child behind.
            TerminateProcess(process_.get(),ERROR_PROCESS_ABORTED); throw;
        }
    }
public:
    explicit EngineProcess(const Config &config) { launch(config); }
    EngineProcess(const EngineProcess &) = delete;
    EngineProcess &operator=(const EngineProcess &) = delete;
    // No blocking wait. Explicit stop/poll until Exited is the normal release
    // path; abandonment closes our own Job and kills only its child tree.
    ~EngineProcess() { job_.reset(); }
    State poll(uint64_t time) {
        if (state_ == State::Exited) return state_;
        const auto exited = WaitForSingleObject(process_.get(),0);
        pipe::check(exited != WAIT_FAILED);
        // Exit always wins over ready, even if both are signalled.
        if (exited == WAIT_OBJECT_0) {
            DWORD code = 0; pipe::check(GetExitCodeProcess(process_.get(),&code)); exitCode_ = code;
            if (failure_ == Failure::None) {
                if (state_ == State::Starting) failure_ = ownerAnchor_ && code == ERROR_ALREADY_EXISTS ? Failure::InstanceBusy : Failure::StartupExit;
                else if (code != 0) failure_ = Failure::UnexpectedExit;
            }
            state_ = State::Exited; deadline_ = 0;
            if (control_) control_->abandon();
            return state_;
        }
        if (deadline_ && time >= deadline_) {
            terminate(state_ == State::Starting ? Failure::StartupTimeout : Failure::StopTimeout);
            return state_;
        }
        if (state_ == State::Starting) {
            const auto ready = WaitForSingleObject(ready_.get(),0); pipe::check(ready != WAIT_FAILED);
            if (ready == WAIT_OBJECT_0) {
                if (ownerAnchor_) {
                    try { engine_ = ownerAnchor_->pin(process_.get()); }
                    catch (const pipe::Error &error) { terminate(Failure::ControlIo,error.code); return state_; }
                }
                state_ = State::Ready; deadline_ = 0;
            }
        }
        if (state_ == State::Ready && control_) control_->poll(time);
        return state_;
    }
    void requestStop(uint64_t time) {
        poll(time);
        if (state_ != State::Starting && state_ != State::Ready) return;
        if (!SetEvent(stop_.get())) { const auto code = GetLastError(); terminate(Failure::ControlIo,code); return; }
        state_ = State::Stopping; deadline_ = time + stopMs_;
        if (control_) control_->abandon();
    }
    // Single attempt per random endpoint, consumed even if connect throws.
    // No WaitNamedPipe/sleep/retry; Hello remains the client's responsibility.
    std::optional<Endpoint> takeEndpoint(uint64_t time) {
        if (poll(time) != State::Ready || claimed_ >= names_.size()) return {};
        const auto index = claimed_++;
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),process_.get(),GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
        pipe::Handle process(raw);
        return Endpoint{names_[index],std::move(process)};
    }
    std::unique_ptr<pipe::Channel> claim(uint64_t time) {
        auto endpoint = takeEndpoint(time);
        return endpoint ? pipe::Channel::connect(endpoint->name,endpoint->process.get()) : nullptr;
    }
    // Single in-flight private control request, with no retry/replay. Full and
    // backend failure are explicit results; a timeout retires only the mailbox.
    bool requestControl(control::Operation operation, uint64_t time, uint64_t timeoutMs = 5000) {
        return poll(time) == State::Ready && control_ && control_->begin(operation,time,timeoutMs);
    }
    std::optional<ControlResult> takeControl(uint64_t time) {
        if (poll(time) != State::Ready || !control_) return {};
        auto response = control_->take(time); if (!response) return {};
        ControlResult result{std::move(*response),{}};
        if (result.metadata.request.operation == control::Operation::Allocate && result.metadata.code == control::Code::Ok) {
            HANDLE raw = nullptr;
            pipe::check(DuplicateHandle(GetCurrentProcess(),engineHandle(),GetCurrentProcess(),&raw,
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
            result.endpoint = Endpoint{result.metadata.name,pipe::Handle(raw)}; ++claimed_;
        }
        return result;
    }
    std::vector<HANDLE> waitHandles() const {
        if (state_ == State::Exited) return {};
        if (state_ == State::Starting) return {process_.get(),ready_.get()};
        std::vector<HANDLE> handles{process_.get()};
        if (state_ == State::Ready && control_ && control_->event()) handles.push_back(control_->event());
        return handles; // A signalled ready/consumed response must not spin.
    }
    uint64_t deadline() const {
        const auto controlDeadline = state_ == State::Ready && control_ ? control_->deadline() : 0;
        return deadline_ && controlDeadline ? std::min(deadline_,controlDeadline) : deadline_ ? deadline_ : controlDeadline;
    } // Zero means no manager deadline.
    Status status() const {
        return {state_,failure_,win32_,exitCode_,claimed_,configured_,control_ ? std::optional<control::State>(control_->state()) : std::nullopt,
            control_ ? control_->failure() : pipe::Failure::None,control_ ? control_->error() : ERROR_SUCCESS};
    }
    HANDLE processHandle() const { return process_.get(); } // Borrowed; never close or change it.
    // In owner mode the managed root above is the broker; query authentication
    // always pins the separate host object. Never give clients the broker's
    // PROCESS_DUP_HANDLE capability or its control/Job handles.
    HANDLE engineHandle() const { return engine_ ? engine_.get() : ownerAnchor_ ? nullptr : process_.get(); }
};
} // namespace harukoto::engine
