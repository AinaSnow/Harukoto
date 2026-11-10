#pragma once
#include "EngineProcess.h"

namespace harukoto::engine {
// Independent owner's I/O loop. One launcher port and up to eight separately
// provisioned client ports share one bounded host request fairly. No discovery.
class EngineOwner {
public:
    struct Registration { std::unique_ptr<control::Server> port; pipe::Handle revoked; uint64_t id = 0; };
private:
    EngineProcess engine_;
    control::Server &upstream_;
    EngineOwnerAnchor &anchor_;
    std::vector<Registration> clients_;
    admission::Server *admission_;
    static EngineProcess::Config validate(EngineProcess::Config config) {
        if (!config.dynamicEndpoints || !config.ownerExecutable.empty()) throw std::invalid_argument("invalid engine owner configuration");
        return config;
    }
public:
    EngineOwner(EngineProcess::Config config, control::Server &upstream, EngineOwnerAnchor &anchor,
            std::vector<Registration> clients = {}, admission::Server *admission = nullptr)
        : engine_(validate(std::move(config))), upstream_(upstream), anchor_(anchor), clients_(std::move(clients)), admission_(admission) {
        if (clients_.size() > 8 || std::any_of(clients_.begin(),clients_.end(),[](const auto &client) { return !client.port || !client.revoked; }))
            throw std::invalid_argument("invalid owner registrations");
        for (size_t i=0;i<clients_.size();++i) clients_[i].id = i+1;
        clients_.resize(admission::Limit);
    }
    EngineOwner(const EngineOwner &) = delete;
    EngineOwner &operator=(const EngineOwner &) = delete;
    // No query clients, text, candidate caches or TSF objects are owned here.
    // Explicit stop must observe host exit before this loop can report success.
    bool run(HANDLE stop, HANDLE ready) {
        if (!stop || !ready || stop == ready) throw std::invalid_argument("invalid engine owner events");
        bool stopping = false, failed = false, announced = false;
        struct Pending { size_t port; uint64_t registration; control::Request request; };
        std::optional<Pending> pending;
        std::vector<control::Server *> ports{&upstream_};
        for (const auto &client : clients_) ports.push_back(client.port.get());
        std::vector<bool> active; for (auto *port : ports) active.push_back(port != nullptr);
        size_t next = 0;
        auto retire = [&](size_t index) {
            if (!active[index]) return;
            auto &client = clients_[index-1];
            pipe::check(SetEvent(client.revoked.get()));
            active[index] = false; ports[index] = nullptr; client.port.reset();
            // Pending keeps only an index and copied request. Host completion
            // will be drained and discarded, never written through this port.
        };
        auto observeRevocations = [&] {
            for (size_t i=1;i<ports.size();++i) if (active[i]) {
                const auto status = WaitForSingleObject(clients_[i-1].revoked.get(),0);
                pipe::check(status != WAIT_FAILED);
                if (status == WAIT_OBJECT_0) retire(i);
            }
        };
        auto stopHost = [&] {
            if (!stopping) {
                stopping = true; pending.reset();
                for (size_t i=1;i<ports.size();++i) retire(i);
                engine_.requestStop(pipe::now());
            }
        };
        for (;;) {
            const auto stopped = WaitForSingleObject(stop,0); pipe::check(stopped != WAIT_FAILED);
            if (stopped == WAIT_OBJECT_0) stopHost();
            const auto state = engine_.poll(pipe::now());
            if (state == EngineProcess::State::Exited) {
                const auto status = engine_.status();
                return stopping && !failed && status.failure == EngineProcess::Failure::None && status.exitCode == DWORD(0);
            }
            if (!stopping && state == EngineProcess::State::Ready) {
                // Revocation supersedes a queued request or an unclaimed host
                // reply, including while another host operation is in flight.
                observeRevocations();
                if (!announced) {
                    anchor_.publish(engine_.processHandle()); pipe::check(SetEvent(ready)); announced = true;
                }
                if (admission_) if (auto offer = admission_->take()) {
                    // The trusted launcher already duplicated these fresh
                    // handles into this process. Ownership transferred when
                    // it signalled the private request, including Full/Closed.
                    std::array<pipe::Handle,4> handles;
                    for (size_t i=0;i<handles.size();++i) handles[i].reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(offer->handles[i])));
                    auto port = std::make_unique<control::Server>(std::array<pipe::Handle,3>{std::move(handles[0]),std::move(handles[1]),std::move(handles[2])});
                    pipe::check(SetHandleInformation(handles[3].get(),HANDLE_FLAG_INHERIT,0));
                    const auto revoked = WaitForSingleObject(handles[3].get(),0); pipe::check(revoked != WAIT_FAILED);
                    // A revoke may have arrived after the loop's first scan
                    // but before this offer was published. Refresh capacity
                    // after observing the offer, rather than returning Full
                    // from an older snapshot of the client positions.
                    observeRevocations();
                    auto free = std::find(active.begin()+1,active.end(),false);
                    if (revoked == WAIT_OBJECT_0) admission_->respond(admission::Code::Closed);
                    else if (free == active.end()) { pipe::check(SetEvent(handles[3].get())); admission_->respond(admission::Code::Full); }
                    else {
                        const auto index = static_cast<size_t>(free-active.begin());
                        clients_[index-1] = Registration{std::move(port),std::move(handles[3]),offer->id};
                        ports[index] = clients_[index-1].port.get(); active[index] = true;
                        admission_->respond(admission::Code::Ok);
                    }
                }
                if (pending) {
                    if (auto result = engine_.takeControl(pipe::now())) {
                        // Correlate the outer request independently of the host's
                        // private sequence. Never forward the local HANDLE value.
                        result->metadata.request = pending->request;
                        if (active[pending->port] && (!pending->port || clients_[pending->port-1].id == pending->registration))
                            ports[pending->port]->respond(result->metadata);
                        pending.reset();
                    }
                }
                if (engine_.status().controlState == control::State::Failed) { failed = true; stopHost(); }
                if (!stopping && !pending) {
                    for (size_t offset=0;offset<ports.size();++offset) {
                        const auto index = (next+offset)%ports.size();
                        if (!active[index]) continue;
                        std::optional<control::Request> request;
                        try { request = ports[index]->take(); }
                        catch (const std::invalid_argument &) {
                            if (!index) throw; // Preserve the trusted supervisor's failure contract.
                            retire(index); continue; // Retire only this malformed client.
                        }
                        if (!request) continue;
                        pending = Pending{index,index ? clients_[index-1].id : 0,*request}; next = (index+1)%ports.size();
                        if (!engine_.requestControl(request->operation,pipe::now())) { failed = true; stopHost(); }
                        break;
                    }
                }
            }
            auto events = engine_.waitHandles();
            if (!stopping) events.push_back(stop);
            if (announced && !stopping)
                for (size_t i=1;i<ports.size();++i) if (active[i]) events.push_back(clients_[i-1].revoked.get());
            if (announced && !stopping && admission_) events.push_back(admission_->event());
            if (announced && !stopping && !pending)
                for (size_t i=0;i<ports.size();++i) if (active[i]) events.push_back(ports[i]->event());
            const auto time = pipe::now(), deadline = engine_.deadline();
            const auto timeout = deadline ? static_cast<DWORD>(deadline > time ? deadline-time : 0) : INFINITE;
            const auto status = WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,timeout);
            pipe::check(status != WAIT_FAILED);
        }
    }
};
} // namespace harukoto::engine
