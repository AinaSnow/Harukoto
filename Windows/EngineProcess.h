#pragma once
#include "EnginePipe.h"
#include <filesystem>

namespace harukoto::engine {
// Experimental launch owner. All calls belong to one service/I/O thread, never
// a TSF foreground callback. No discovery, restart, request replay or version selection.
class EngineProcess {
public:
    enum class State { Starting, Ready, Stopping, Terminating, Exited };
    enum class Failure { None, StartupExit, UnexpectedExit, StartupTimeout, StopTimeout, ControlIo };
    struct Config {
        std::filesystem::path executable, base, themes, personal;
        size_t connections = 2;
        uint64_t startupMs = 30000, stopMs = 5000;
    };
    struct Status {
        State state;
        Failure failure;
        DWORD win32;
        std::optional<DWORD> exitCode;
        size_t claimed, configured;
    };
private:
    pipe::Handle ready_, stop_, process_, job_;
    std::vector<std::wstring> names_;
    State state_ = State::Starting;
    Failure failure_ = Failure::None;
    DWORD win32_ = ERROR_SUCCESS;
    std::optional<DWORD> exitCode_;
    size_t claimed_ = 0;
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
    }
    void launch(const Config &config) {
        if (!absolute(config.executable) || !absolute(config.base) ||
                (!config.themes.empty() && !absolute(config.themes)) ||
                (!config.personal.empty() && !absolute(config.personal)) ||
                !config.connections || config.connections > 8 || !config.startupMs || config.startupMs > 300000 ||
                !config.stopMs || config.stopMs > 300000)
            throw std::invalid_argument("invalid engine process configuration");
        pipe::processIdentityOnly();
        stopMs_ = config.stopMs;
        deadline_ = pipe::now() + config.startupMs;
        for (size_t i = 0; i < config.connections; ++i) names_.push_back(pipe::newName());
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
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        pipe::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        HANDLE inherited[]{childReady.get(),childStop.get()};
        pipe::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr));
        std::vector<std::wstring> arguments{config.executable.wstring(),L"--base",config.base.wstring()};
        if (!config.themes.empty()) { arguments.push_back(L"--themes"); arguments.push_back(config.themes.wstring()); }
        if (!config.personal.empty()) { arguments.push_back(L"--personal"); arguments.push_back(config.personal.wstring()); }
        for (const auto &name : names_) { arguments.push_back(L"--pipe"); arguments.push_back(name); }
        arguments.insert(arguments.end(),{L"--ready",std::to_wstring(reinterpret_cast<uintptr_t>(childReady.get())),
            L"--stop",std::to_wstring(reinterpret_cast<uintptr_t>(childStop.get()))});
        std::wstring command;
        for (const auto &argument : arguments) { if (!command.empty()) command += L' '; command += quote(argument); }
        if (command.size() >= 32767) throw std::invalid_argument("engine command line too long");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        pipe::check(CreateProcessW(config.executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,
            config.executable.parent_path().c_str(),&startup.StartupInfo,&info));
        process_.reset(info.hProcess); pipe::Handle thread(info.hThread);
        try {
            pipe::check(AssignProcessToJobObject(job_.get(),process_.get()));
            // Only the child's copies remain inheritable while it starts.
            childReady.reset(); childStop.reset();
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
                if (state_ == State::Starting) failure_ = Failure::StartupExit;
                else if (code != 0) failure_ = Failure::UnexpectedExit;
            }
            state_ = State::Exited; deadline_ = 0; return state_;
        }
        if (deadline_ && time >= deadline_) {
            terminate(state_ == State::Starting ? Failure::StartupTimeout : Failure::StopTimeout);
            return state_;
        }
        if (state_ == State::Starting) {
            const auto ready = WaitForSingleObject(ready_.get(),0); pipe::check(ready != WAIT_FAILED);
            if (ready == WAIT_OBJECT_0) { state_ = State::Ready; deadline_ = 0; }
        }
        return state_;
    }
    void requestStop(uint64_t time) {
        poll(time);
        if (state_ != State::Starting && state_ != State::Ready) return;
        if (!SetEvent(stop_.get())) { const auto code = GetLastError(); terminate(Failure::ControlIo,code); return; }
        state_ = State::Stopping; deadline_ = time + stopMs_;
    }
    // Single attempt per random endpoint, consumed even if connect throws.
    // No WaitNamedPipe/sleep/retry; Hello remains the client's responsibility.
    std::unique_ptr<pipe::Channel> claim(uint64_t time) {
        if (poll(time) != State::Ready || claimed_ == names_.size()) return {};
        const auto index = claimed_++;
        return pipe::Channel::connect(names_[index],process_.get());
    }
    std::vector<HANDLE> waitHandles() const {
        if (state_ == State::Exited) return {};
        if (state_ == State::Starting) return {process_.get(),ready_.get()};
        return {process_.get()}; // A signalled ready must not spin in Ready/Stopping.
    }
    uint64_t deadline() const { return deadline_; } // Zero means no manager deadline.
    Status status() const { return {state_,failure_,win32_,exitCode_,claimed_,names_.size()}; }
    HANDLE processHandle() const { return process_.get(); } // Borrowed; never close or change it.
};
} // namespace harukoto::engine
