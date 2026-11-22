#pragma once
#include "EnginePipe.h"
#include "EngineControl.h"
#include "EngineOwnerAnchor.h"
#include "EngineInstance.h"
#include "EngineRegistration.h"
#include "EngineAdmission.h"
#include "EngineDiagnostics.h"
#include "EngineDeployment.h"
#include <filesystem>

namespace harukoto::engine {
// Experimental launch owner. All calls belong to one service/I/O thread, never
// a TSF foreground callback. No discovery, restart, request replay or version selection.
class EngineProcess {
public:
    enum class State { Starting, Ready, Stopping, Terminating, Exited };
    enum class Failure { None, StartupExit, UnexpectedExit, StartupTimeout, StopTimeout, ControlIo, InstanceBusy, DeploymentIdentity };
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
        size_t ownerRegistrations = 0; // Bounded private client capabilities, owner mode only.
        bool dynamicRegistrations = false; // Optional private launcher admission, owner mode only.
        bool diagnostics = false; // Optional observational mapping; never trusted discovery.
        std::string hostSha256, ownerSha256; // Optional trusted deployment policy; owner mode requires both.
        bool discovery = false; // Owner publishes the step-2 discovery pipe under ownerInstance.
        std::filesystem::path memory; // Step 6b: the user's selection-memory.tsv, read-only.
        std::filesystem::path baseStore; // Step 6d: published base snapshots the host adopts.
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
        size_t registrationConfigured = 0, registrationIssued = 0, registrationBound = 0, registrationRevoked = 0;
        size_t registrationCreated = 0, registrationActive = 0;
        std::optional<admission::State> admissionState;
        pipe::Failure admissionFailure = pipe::Failure::None;
        DWORD admissionWin32 = ERROR_SUCCESS;
        DWORD diagnosticsWin32 = ERROR_SUCCESS; // Publication failures never stop the engine.
        bool deploymentRequired = false, deploymentVerified = false;
    };
    // Trusted owner-to-client handoff, never reconstructed from a public PID.
    // The retained process copy is query/wait only and non-inheritable. A trusted
    // launcher may explicitly duplicate it into its own client's handle list.
    using Endpoint = EngineRegistration::Endpoint;
    struct ControlResult { control::Response metadata; std::optional<Endpoint> endpoint; };
    struct RegistrationResult { admission::Code code; uint64_t id; std::unique_ptr<EngineRegistration> registration; };
private:
    std::shared_ptr<const EngineDeployment> hostDeployment_, ownerDeployment_;
    bool deploymentVerified_ = false;
    pipe::Handle ready_, stop_, process_, job_;
    std::vector<std::wstring> names_;
    std::unique_ptr<control::Client> control_;
    std::unique_ptr<admission::Client> admission_;
    std::unique_ptr<EngineOwnerAnchor> ownerAnchor_;
    pipe::Handle engine_;
    struct RegistrationSlot {
        std::unique_ptr<control::Client> client = std::make_unique<control::Client>();
        pipe::Handle revoked{CreateEventW(nullptr,TRUE,FALSE,nullptr)}, process;
        uint64_t id;
        bool issued = false, bound = false, retired = false;
        explicit RegistrationSlot(uint64_t value) : id(value) { pipe::check(bool(revoked)); }
    };
    std::vector<std::unique_ptr<RegistrationSlot>> registrations_;
    std::unique_ptr<RegistrationSlot> offered_;
    uint64_t registrationNext_ = 0;
    size_t registrationConfigured_ = 0, registrationIssued_ = 0, registrationRevoked_ = 0;
    State state_ = State::Starting;
    Failure failure_ = Failure::None;
    DWORD win32_ = ERROR_SUCCESS;
    std::optional<DWORD> exitCode_;
    size_t claimed_ = 0, configured_ = 0;
    uint64_t deadline_ = 0, stopMs_ = 0;
    std::unique_ptr<diagnostics::Writer> diagnostics_;
    diagnostics::Record diagnosticRecord_;
    DWORD diagnosticsWin32_ = ERROR_SUCCESS;
    uint64_t diagnosticPoll_ = 0;
    void publishDiagnostics() noexcept {
        if (!diagnostics_) return;
        try {
            const auto s = status();
            auto &r = diagnosticRecord_;
            if (!r.host.pid && engineHandle()) r.host = diagnostics::identity(engineHandle());
            r.published = GetTickCount64(); r.polled = diagnosticPoll_;
            r.state = static_cast<uint32_t>(s.state); r.failure = static_cast<uint32_t>(s.failure);
            r.win32 = s.win32; r.exitCode = s.exitCode; r.configured = static_cast<uint32_t>(s.configured); r.claimed = s.claimed;
            r.deploymentRequired = s.deploymentRequired; r.deploymentVerified = s.deploymentVerified;
            r.controlState = s.controlState ? 1+static_cast<uint32_t>(*s.controlState) : 0;
            r.controlFailure = static_cast<uint32_t>(s.controlFailure); r.controlWin32 = s.controlWin32;
            r.admissionState = s.admissionState ? 1+static_cast<uint32_t>(*s.admissionState) : 0;
            r.admissionFailure = static_cast<uint32_t>(s.admissionFailure); r.admissionWin32 = s.admissionWin32;
            r.registrationConfigured = s.registrationConfigured; r.registrationIssued = s.registrationIssued;
            r.registrationBound = s.registrationBound; r.registrationRevoked = s.registrationRevoked;
            r.registrationCreated = s.registrationCreated; r.registrationActive = s.registrationActive;
            diagnostics_->publish(r);
        } catch (const pipe::Error &error) { diagnosticsWin32_ = error.code; diagnostics_.reset(); }
        catch (...) { diagnosticsWin32_ = ERROR_INVALID_DATA; diagnostics_.reset(); }
    }
    struct DiagnosticObservation {
        EngineProcess *process;
        ~DiagnosticObservation() { process->publishDiagnostics(); }
    };
    void revoke(RegistrationSlot &slot) {
        if (slot.retired) return;
        pipe::check(SetEvent(slot.revoked.get())); slot.retired = true; slot.process.reset(); ++registrationRevoked_;
    }
    void observeRegistrations() {
        auto observe = [&](auto &slot) { if (slot && !slot->retired) {
            const auto revoked = WaitForSingleObject(slot->revoked.get(),0); pipe::check(revoked != WAIT_FAILED);
            if (revoked == WAIT_OBJECT_0) { slot->retired = true; slot->process.reset(); ++registrationRevoked_; return; }
            if (slot->process) {
                const auto exited = WaitForSingleObject(slot->process.get(),0); pipe::check(exited != WAIT_FAILED);
                if (exited == WAIT_OBJECT_0) revoke(*slot);
            }
        }};
        for (auto &slot : registrations_) observe(slot);
        observe(offered_);
    }
    RegistrationSlot *registration(uint64_t id) {
        for (auto &slot : registrations_) if (slot->id == id && slot->issued) return slot.get();
        return nullptr;
    }
    void closeAdmission() {
        if (admission_) admission_->abandon();
        if (offered_) { revoke(*offered_); offered_.reset(); }
    }

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
        closeAdmission();
    }
    void launch(const Config &config) {
        if (!absolute(config.executable) || !absolute(config.base) ||
                (!config.themes.empty() && !absolute(config.themes)) ||
                (!config.personal.empty() && !absolute(config.personal)) ||
                (!config.memory.empty() && !absolute(config.memory)) ||
                (!config.baseStore.empty() && !absolute(config.baseStore)) ||
                (!config.ownerExecutable.empty() && (!absolute(config.ownerExecutable) || !config.dynamicEndpoints || config.ownerExecutable == config.executable)) ||
                (!config.ownerInstance.empty() && (config.ownerExecutable.empty() || !EngineInstance::validScope(config.ownerInstance))) ||
                (config.ownerLease && (!config.ownerExecutable.empty() || !config.dynamicEndpoints || !config.ownerInstance.empty())) ||
                (config.ownerRegistrations && (config.ownerExecutable.empty() || config.ownerRegistrations > 8)) ||
                (config.dynamicRegistrations && config.ownerExecutable.empty()) ||
                (config.discovery && config.ownerInstance.empty()) ||
                (!config.trace.empty() && !absolute(config.trace)) || !config.threads || config.threads > 16 ||
                !config.connections || config.connections > 8 || !config.startupMs || config.startupMs > 300000 ||
                !config.stopMs || config.stopMs > 300000)
            throw std::invalid_argument("invalid engine process configuration");
        pipe::processIdentityOnly();
        if (!config.hostSha256.empty() || !config.ownerSha256.empty()) {
            EngineDeployment::digest(config.hostSha256);
            if (!config.ownerExecutable.empty()) EngineDeployment::digest(config.ownerSha256);
            else if (!config.ownerSha256.empty()) throw std::invalid_argument("owner digest requires owner mode");
            hostDeployment_=std::make_shared<EngineDeployment>(config.executable,config.hostSha256);
            if (!config.ownerExecutable.empty()) {
                ownerDeployment_=std::make_shared<EngineDeployment>(config.ownerExecutable,config.ownerSha256);
                if (hostDeployment_->sameFile(*ownerDeployment_)) throw std::invalid_argument("host and owner use the same file");
            }
        }
        stopMs_ = config.stopMs;
        configured_ = config.connections;
        deadline_ = pipe::now() + config.startupMs;
        if (config.dynamicEndpoints) control_ = std::make_unique<control::Client>();
        else for (size_t i = 0; i < config.connections; ++i) names_.push_back(pipe::newName());
        if (!config.ownerExecutable.empty()) ownerAnchor_ = std::make_unique<EngineOwnerAnchor>();
        if (config.dynamicRegistrations) admission_ = std::make_unique<admission::Client>();
        registrationConfigured_ = config.ownerRegistrations;
        for (size_t i=0;i<config.ownerRegistrations;++i) registrations_.push_back(std::make_unique<RegistrationSlot>(++registrationNext_));
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
        std::array<pipe::Handle,3> childAdmission;
        std::vector<pipe::Handle> childRegistrations;
        for (const auto &registration : registrations_) {
            const auto port = registration->client->handles();
            for (auto handle : {port[0],port[1],port[2],registration->revoked.get()}) {
                pipe::check(DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
                childRegistrations.emplace_back(raw);
            }
        }
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
        if (admission_) {
            const auto handles = admission_->handles();
            for (size_t i=0;i<handles.size();++i) {
                pipe::check(DuplicateHandle(GetCurrentProcess(),handles[i],GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
                childAdmission[i].reset(raw);
            }
        }
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        pipe::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        std::vector<HANDLE> inherited{childReady.get(),childStop.get()};
        if (control_) for (const auto &handle : childControl) inherited.push_back(handle.get());
        if (admission_) for (const auto &handle : childAdmission) inherited.push_back(handle.get());
        if (childAnchor) inherited.push_back(childAnchor.get());
        if (childLease) inherited.push_back(childLease.get());
        for (const auto &handle : childRegistrations) inherited.push_back(handle.get());
        pipe::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited.data(),inherited.size()*sizeof(HANDLE),nullptr,nullptr));
        const auto hostExecutable = hostDeployment_ ? hostDeployment_->path() : config.executable;
        const auto executable = ownerDeployment_ ? ownerDeployment_->path() : ownerAnchor_ ? config.ownerExecutable : hostExecutable;
        std::vector<std::wstring> arguments{executable.wstring(),L"--base",config.base.wstring()};
        if (ownerAnchor_) arguments.insert(arguments.end(),{L"--host",hostExecutable.wstring(),L"--anchor",
            std::to_wstring(reinterpret_cast<uintptr_t>(childAnchor.get()))});
        if (ownerDeployment_) arguments.insert(arguments.end(),{L"--host-sha256",std::wstring(hostDeployment_->digest().begin(),hostDeployment_->digest().end())});
        if (!config.ownerInstance.empty()) arguments.insert(arguments.end(),{L"--instance",config.ownerInstance});
        if (config.discovery) arguments.insert(arguments.end(),{L"--discovery",L"1"});
        if (admission_) {
            const wchar_t *keys[]{L"--admission-section",L"--admission-request",L"--admission-response"};
            for (size_t i=0;i<childAdmission.size();++i)
                arguments.insert(arguments.end(),{keys[i],std::to_wstring(reinterpret_cast<uintptr_t>(childAdmission[i].get()))});
        }
        if (childLease) arguments.insert(arguments.end(),{L"--owner-lease",std::to_wstring(reinterpret_cast<uintptr_t>(childLease.get()))});
        for (size_t i=0;i<childRegistrations.size();i+=4) {
            std::wstring value;
            for (size_t j=0;j<4;++j) { if (j) value += L','; value += std::to_wstring(reinterpret_cast<uintptr_t>(childRegistrations[i+j].get())); }
            arguments.insert(arguments.end(),{L"--registration",value});
        }
        if (!config.themes.empty()) { arguments.push_back(L"--themes"); arguments.push_back(config.themes.wstring()); }
        if (!config.personal.empty()) { arguments.push_back(L"--personal"); arguments.push_back(config.personal.wstring()); }
        if (!config.memory.empty()) { arguments.push_back(L"--memory"); arguments.push_back(config.memory.wstring()); }
        if (!config.baseStore.empty()) { arguments.push_back(L"--base-store"); arguments.push_back(config.baseStore.wstring()); }
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
        auto workingDirectory=executable.parent_path().wstring();
        if (hostDeployment_) {
            // All engine data paths are absolute. Verified launches use the
            // system directory as their explicit working directory.
            std::wstring system(32768,L'\0');
            const auto length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
            pipe::check(length && length<system.size()); system.resize(length); workingDirectory=std::move(system);
        }
        pipe::check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,
            workingDirectory.c_str(),&startup.StartupInfo,&info));
        process_.reset(info.hProcess); pipe::Handle thread(info.hThread);
        try {
            pipe::check(AssignProcessToJobObject(job_.get(),process_.get()));
            if (ownerDeployment_) ownerDeployment_->verify(process_.get());
            else if (hostDeployment_) hostDeployment_->verify(process_.get());
            // Only the child's copies remain inheritable while it starts.
            childReady.reset(); childStop.reset();
            childAnchor.reset();
            childLease.reset();
            childRegistrations.clear();
            for (auto &handle : childControl) handle.reset();
            for (auto &handle : childAdmission) handle.reset();
            if (ResumeThread(thread.get()) == DWORD(-1)) throw pipe::Error(pipe::Failure::Io,GetLastError());
        } catch (...) {
            // Even assignment failure must not leave our suspended child behind.
            TerminateProcess(process_.get(),ERROR_PROCESS_ABORTED); throw;
        }
    }
public:
    explicit EngineProcess(const Config &config) {
        launch(config);
        if (config.diagnostics) {
            try {
                diagnosticRecord_.root = diagnostics::identity(process_.get());
                diagnosticRecord_.publisher = diagnostics::identity(GetCurrentProcess());
                diagnosticRecord_.owner = bool(ownerAnchor_);
                diagnostics_ = std::make_unique<diagnostics::Writer>(diagnosticRecord_.root);
                publishDiagnostics();
            } catch (const pipe::Error &error) { diagnosticsWin32_ = error.code; }
            catch (...) { diagnosticsWin32_ = ERROR_NOT_ENOUGH_MEMORY; }
        }
    }
    EngineProcess(const EngineProcess &) = delete;
    EngineProcess &operator=(const EngineProcess &) = delete;
    // No blocking wait. Explicit stop/poll until Exited is the normal release
    // path; abandonment closes our own Job and kills only its child tree.
    ~EngineProcess() { job_.reset(); }
    State poll(uint64_t time) {
        DiagnosticObservation observation{this};
        if (diagnostics_) diagnosticPoll_ = GetTickCount64();
        if (state_ == State::Exited) return state_;
        observeRegistrations();
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
            for (auto &slot : registrations_) revoke(*slot);
            if (control_) control_->abandon();
            closeAdmission();
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
                    try {
                        auto candidate = ownerAnchor_->pin(process_.get());
                        if (hostDeployment_) {
                            try { hostDeployment_->verify(candidate.get()); }
                            catch (const pipe::Error &error) { terminate(Failure::DeploymentIdentity,error.code); return state_; }
                        }
                        engine_ = std::move(candidate);
                    }
                    catch (const pipe::Error &error) { terminate(Failure::ControlIo,error.code); return state_; }
                }
                deploymentVerified_ = bool(hostDeployment_);
                state_ = State::Ready; deadline_ = 0;
            }
        }
        if (state_ == State::Ready && control_) control_->poll(time);
        if (state_ == State::Ready && admission_ && admission_->poll(time) == admission::State::Failed && offered_) {
            revoke(*offered_); offered_.reset(); // Remote copies belong to the owner after publication.
        }
        return state_;
    }
    void requestStop(uint64_t time) {
        DiagnosticObservation observation{this};
        poll(time);
        if (state_ != State::Starting && state_ != State::Ready) return;
        if (!SetEvent(stop_.get())) { const auto code = GetLastError(); terminate(Failure::ControlIo,code); return; }
        state_ = State::Stopping; deadline_ = time + stopMs_;
        if (control_) control_->abandon();
        closeAdmission();
    }
    // Single attempt per random endpoint, consumed even if connect throws.
    // No WaitNamedPipe/sleep/retry; Hello remains the client's responsibility.
    std::optional<Endpoint> takeEndpoint(uint64_t time) {
        DiagnosticObservation observation{this};
        if (poll(time) != State::Ready || claimed_ >= names_.size()) return {};
        const auto index = claimed_++;
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),process_.get(),GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
        pipe::Handle process(raw);
        return Endpoint{names_[index],std::move(process),hostDeployment_};
    }
    std::unique_ptr<pipe::Channel> claim(uint64_t time) {
        auto endpoint = takeEndpoint(time);
        return endpoint ? endpoint->connect() : nullptr;
    }
    // A fixed preallocated slot is handed out once; it is not the launcher's
    // upstream stop/control port. The client allocates its own query endpoint.
    std::unique_ptr<EngineRegistration> takeRegistration(uint64_t time) {
        DiagnosticObservation observation{this};
        if (poll(time) != State::Ready) return {};
        for (auto &slot : registrations_) if (!slot->issued && !slot->retired) {
            std::unique_ptr<EngineRegistration> result;
            try { result = std::make_unique<EngineRegistration>(std::move(slot->client),process_.get(),engineHandle(),slot->revoked.get(),slot->id,
                EngineRegistration::DeploymentPolicy{ownerDeployment_,hostDeployment_}); }
            catch (...) { revoke(*slot); throw; } // A consumed import is never reissued.
            slot->issued = true; ++registrationIssued_; return result;
        }
        return {};
    }
    bool requestRegistration(uint64_t time, uint64_t timeoutMs = 5000) {
        DiagnosticObservation observation{this};
        if (poll(time) != State::Ready || !admission_ || admission_->state() != admission::State::Idle) return false;
        if (!timeoutMs || timeoutMs > 300000 || time > UINT64_MAX-timeoutMs || registrationNext_ == UINT64_MAX)
            throw std::invalid_argument("invalid registration deadline or id");
        registrations_.erase(std::remove_if(registrations_.begin(),registrations_.end(),[](const auto &slot) { return slot->retired; }),registrations_.end());
        // Keep at most eight admitted slots plus one single-flight offer. The
        // owner returns Full explicitly; no local queue or automatic retry.
        registrations_.reserve(admission::Limit);
        auto slot = std::make_unique<RegistrationSlot>(++registrationNext_);
        const auto port = slot->client->handles();
        admission::Transfer transfer(process_.get(),{port[0],port[1],port[2],slot->revoked.get()});
        if (!admission_->begin(slot->id,transfer.handles(),time,timeoutMs)) return false;
        transfer.release(); offered_ = std::move(slot); return true;
    }
    std::optional<RegistrationResult> takeRegistrationResult(uint64_t time) {
        DiagnosticObservation observation{this};
        if (poll(time) != State::Ready || !admission_) return {};
        auto response = admission_->take(time); if (!response) return {};
        if (!offered_ || response->id != offered_->id) throw std::logic_error("missing registration offer");
        RegistrationResult result{response->code,response->id,{}};
        if (result.code == admission::Code::Ok && !offered_->retired) {
            registrations_.erase(std::remove_if(registrations_.begin(),registrations_.end(),[](const auto &slot) { return slot->retired; }),registrations_.end());
            try { result.registration = std::make_unique<EngineRegistration>(std::move(offered_->client),process_.get(),engineHandle(),offered_->revoked.get(),offered_->id,
                EngineRegistration::DeploymentPolicy{ownerDeployment_,hostDeployment_}); }
            catch (...) { revoke(*offered_); offered_.reset(); throw; }
            offered_->issued = true; ++registrationIssued_; registrations_.push_back(std::move(offered_));
        } else {
            if (result.code == admission::Code::Ok) result.code = admission::Code::Closed;
            revoke(*offered_); offered_.reset();
        }
        return result;
    }
    // Bind only a retained process object from a trusted launch/handoff, never
    // an OpenProcess(public PID) reconstruction. A slot cannot be rebound.
    bool bindRegistration(uint64_t id, HANDLE process) {
        DiagnosticObservation observation{this};
        auto *found = registration(id);
        if (state_ != State::Ready || !found) return false;
        auto &slot = *found;
        if (slot.retired || slot.bound) return false;
        pipe::processIdentityOnly();
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),process,GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0),pipe::Failure::Authentication);
        pipe::Handle retained(raw);
        if (!GetProcessId(retained.get()))
            throw pipe::Error(pipe::Failure::Authentication,ERROR_ACCESS_DENIED);
        const auto exited = WaitForSingleObject(retained.get(),0); pipe::check(exited != WAIT_FAILED);
        if (exited == WAIT_OBJECT_0) { slot.bound = true; revoke(slot); return true; }
        if (!(pipe::LogonIdentity::fromProcess(retained.get()) == pipe::LogonIdentity::fromProcess(GetCurrentProcess())))
            throw pipe::Error(pipe::Failure::Authentication,ERROR_ACCESS_DENIED);
        slot.bound = true; slot.process = std::move(retained); return true;
    }
    bool revokeRegistration(uint64_t id) {
        DiagnosticObservation observation{this};
        auto *found = registration(id); if (!found) return false;
        auto &slot = *found;
        if (slot.retired) return false;
        revoke(slot); return true;
    }
    // Single in-flight private control request, with no retry/replay. Full and
    // backend failure are explicit results; a timeout retires only the mailbox.
    bool requestControl(control::Operation operation, uint64_t time, uint64_t timeoutMs = 5000) {
        DiagnosticObservation observation{this};
        return poll(time) == State::Ready && control_ && control_->begin(operation,time,timeoutMs);
    }
    std::optional<ControlResult> takeControl(uint64_t time) {
        DiagnosticObservation observation{this};
        if (poll(time) != State::Ready || !control_) return {};
        auto response = control_->take(time); if (!response) return {};
        ControlResult result{std::move(*response),{}};
        if (diagnostics_ && result.metadata.request.operation == control::Operation::Inspect) {
            const auto &m = result.metadata;
            diagnosticRecord_.capacity = {GetTickCount64(),m.generation,m.limit,m.listening,m.connected,m.retiring,m.jobs};
        }
        if (result.metadata.request.operation == control::Operation::Allocate && result.metadata.code == control::Code::Ok) {
            HANDLE raw = nullptr;
            pipe::check(DuplicateHandle(GetCurrentProcess(),engineHandle(),GetCurrentProcess(),&raw,
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
            result.endpoint = Endpoint{result.metadata.name,pipe::Handle(raw),hostDeployment_}; ++claimed_;
        }
        return result;
    }
    std::vector<HANDLE> waitHandles() const {
        if (state_ == State::Exited) return {};
        if (state_ == State::Starting) return {process_.get(),ready_.get()};
        std::vector<HANDLE> handles{process_.get()};
        if (state_ == State::Ready && control_ && control_->event()) handles.push_back(control_->event());
        if (state_ == State::Ready && admission_ && admission_->event()) handles.push_back(admission_->event());
        if (state_ == State::Ready) for (const auto &slot : registrations_) if (!slot->retired) {
            handles.push_back(slot->revoked.get());
            if (slot->process) handles.push_back(slot->process.get());
        }
        return handles; // A signalled ready/consumed response must not spin.
    }
    uint64_t deadline() const {
        const auto controlDeadline = state_ == State::Ready && control_ ? control_->deadline() : 0;
        const auto admissionDeadline = state_ == State::Ready && admission_ ? admission_->deadline() : 0;
        uint64_t result = deadline_;
        for (auto value : {controlDeadline,admissionDeadline}) if (value && (!result || value < result)) result = value;
        return result;
    } // Zero means no manager deadline.
    Status status() const {
        Status result{state_,failure_,win32_,exitCode_,claimed_,configured_,control_ ? std::optional<control::State>(control_->state()) : std::nullopt,
            control_ ? control_->failure() : pipe::Failure::None,control_ ? control_->error() : ERROR_SUCCESS};
        result.registrationConfigured = registrationConfigured_; result.registrationIssued = registrationIssued_;
        result.registrationCreated = static_cast<size_t>(registrationNext_); result.registrationRevoked = registrationRevoked_;
        for (const auto &slot : registrations_) {
            result.registrationBound += bool(slot->process); result.registrationActive += !slot->retired;
        }
        if (admission_) { result.admissionState = admission_->state(); result.admissionFailure = admission_->failure(); result.admissionWin32 = admission_->error(); }
        result.diagnosticsWin32 = diagnosticsWin32_;
        result.deploymentRequired = bool(hostDeployment_); result.deploymentVerified = deploymentVerified_;
        return result;
    }
    HANDLE processHandle() const { return process_.get(); } // Borrowed; never close or change it.
    // In owner mode the managed root above is the broker; query authentication
    // always pins the separate host object. Never give clients the broker's
    // PROCESS_DUP_HANDLE capability or its control/Job handles.
    HANDLE engineHandle() const { return engine_ ? engine_.get() : ownerAnchor_ ? nullptr : process_.get(); }
};
} // namespace harukoto::engine
