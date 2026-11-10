#pragma once
#include "EngineProcess.h"

namespace harukoto::engine {
// Independent owner's I/O loop. One launcher port and up to eight separately
// provisioned client ports share one bounded host request fairly. No discovery.
class EngineOwner {
    EngineProcess engine_;
    control::Server &upstream_;
    EngineOwnerAnchor &anchor_;
    std::vector<std::unique_ptr<control::Server>> clients_;
    static EngineProcess::Config validate(EngineProcess::Config config) {
        if (!config.dynamicEndpoints || !config.ownerExecutable.empty()) throw std::invalid_argument("invalid engine owner configuration");
        return config;
    }
public:
    EngineOwner(EngineProcess::Config config, control::Server &upstream, EngineOwnerAnchor &anchor,
            std::vector<std::unique_ptr<control::Server>> clients = {})
        : engine_(validate(std::move(config))), upstream_(upstream), anchor_(anchor), clients_(std::move(clients)) {
        if (clients_.size() > 8 || std::any_of(clients_.begin(),clients_.end(),[](const auto &port) { return !port; }))
            throw std::invalid_argument("invalid owner registrations");
    }
    EngineOwner(const EngineOwner &) = delete;
    EngineOwner &operator=(const EngineOwner &) = delete;
    // No query clients, text, candidate caches or TSF objects are owned here.
    // Explicit stop must observe host exit before this loop can report success.
    bool run(HANDLE stop, HANDLE ready) {
        if (!stop || !ready || stop == ready) throw std::invalid_argument("invalid engine owner events");
        bool stopping = false, failed = false, announced = false;
        struct Pending { size_t port; control::Request request; };
        std::optional<Pending> pending;
        std::vector<control::Server *> ports{&upstream_};
        for (const auto &port : clients_) ports.push_back(port.get());
        std::vector<bool> active(ports.size(),true); size_t next = 0;
        auto stopHost = [&] {
            if (!stopping) { stopping = true; pending.reset(); engine_.requestStop(pipe::now()); }
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
                if (!announced) {
                    anchor_.publish(engine_.processHandle()); pipe::check(SetEvent(ready)); announced = true;
                }
                if (pending) {
                    if (auto result = engine_.takeControl(pipe::now())) {
                        // Correlate the outer request independently of the host's
                        // private sequence. Never forward the local HANDLE value.
                        result->metadata.request = pending->request;
                        ports[pending->port]->respond(result->metadata); pending.reset();
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
                            active[index] = false; continue; // Retire only this malformed client.
                        }
                        if (!request) continue;
                        pending = Pending{index,*request}; next = (index+1)%ports.size();
                        if (!engine_.requestControl(request->operation,pipe::now())) { failed = true; stopHost(); }
                        break;
                    }
                }
            }
            auto events = engine_.waitHandles();
            if (!stopping) events.push_back(stop);
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
