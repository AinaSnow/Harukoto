#pragma once
#include "EngineControl.h"

namespace harukoto::engine {
// One capability-provisioned client, one private mailbox. No public discovery,
// owner control/Job capability, process lookup by PID, or automatic replay.
// All methods run on the client's service/I/O thread, never a TSF callback.
class EngineRegistration {
    std::unique_ptr<control::Client> client_;
    pipe::Handle owner_, engine_;
    static pipe::Handle retain(HANDLE source) {
        pipe::processIdentityOnly();
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0),pipe::Failure::Authentication);
        pipe::Handle result(raw);
        if (WaitForSingleObject(result.get(),0) != WAIT_TIMEOUT ||
                !(pipe::LogonIdentity::fromProcess(result.get()) == pipe::LogonIdentity::fromProcess(GetCurrentProcess())))
            throw pipe::Error(pipe::Failure::Authentication,ERROR_ACCESS_DENIED);
        return result;
    }
public:
    struct Endpoint { std::wstring name; pipe::Handle process; };
    struct Result { control::Response metadata; std::optional<Endpoint> endpoint; };
    EngineRegistration(std::unique_ptr<control::Client> client, HANDLE owner, HANDLE engine)
        : client_(std::move(client)), owner_(retain(owner)), engine_(retain(engine)) {
        if (!client_ || client_->state() != control::State::Idle) throw std::invalid_argument("invalid engine registration");
    }
    // Import only fresh handles handed off by a trusted launcher. Numeric
    // values from a public source do not establish this contract.
    EngineRegistration(std::array<pipe::Handle,3> handles, HANDLE owner, HANDLE engine)
        : EngineRegistration(std::make_unique<control::Client>(std::move(handles)),owner,engine) {}
    control::State poll(uint64_t time) {
        if (client_->state() != control::State::Failed) {
            for (auto handle : {owner_.get(),engine_.get()}) {
                const auto status = WaitForSingleObject(handle,0);
                pipe::check(status != WAIT_FAILED);
                if (status != WAIT_TIMEOUT) { client_->abandon(); break; }
            }
        }
        return client_->poll(time); // Actual process exit supersedes unconsumed replies.
    }
    bool request(control::Operation operation, uint64_t time, uint64_t timeoutMs = 5000) {
        return poll(time) == control::State::Idle && client_->begin(operation,time,timeoutMs);
    }
    std::optional<Result> take(uint64_t time) {
        if (poll(time) != control::State::Ready) return {};
        auto response = client_->take(time); if (!response) return {};
        Result result{std::move(*response),{}};
        if (result.metadata.request.operation == control::Operation::Allocate && result.metadata.code == control::Code::Ok)
            result.endpoint = Endpoint{result.metadata.name,retain(engine_.get())};
        return result;
    }
    std::vector<HANDLE> waitHandles() const {
        if (client_->state() == control::State::Failed) return {};
        std::vector<HANDLE> result{owner_.get(),engine_.get()};
        if (client_->event()) result.push_back(client_->event());
        return result;
    }
    uint64_t deadline() const { return client_->deadline(); }
    control::State state() const { return client_->state(); }
    pipe::Failure failure() const { return client_->failure(); }
    DWORD error() const { return client_->error(); }
    // Export for one explicit trusted handoff BEFORE the first request only.
    std::array<HANDLE,3> handles() const { return client_->handles(); }
    HANDLE ownerHandle() const { return owner_.get(); }
    HANDLE engineHandle() const { return engine_.get(); }
};
} // namespace harukoto::engine
