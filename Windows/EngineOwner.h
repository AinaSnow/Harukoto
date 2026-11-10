#pragma once
#include "EngineProcess.h"

namespace harukoto::engine {
// Independent owner process's I/O loop. The trusted launcher has ONE private
// request slot; client registration/discovery belongs to a later broker layer.
class EngineOwner {
    EngineProcess engine_;
    control::Server &upstream_;
    EngineOwnerAnchor &anchor_;
    static EngineProcess::Config validate(EngineProcess::Config config) {
        if (!config.dynamicEndpoints || !config.ownerExecutable.empty()) throw std::invalid_argument("invalid engine owner configuration");
        return config;
    }
public:
    EngineOwner(EngineProcess::Config config, control::Server &upstream, EngineOwnerAnchor &anchor)
        : engine_(validate(std::move(config))), upstream_(upstream), anchor_(anchor) {}
    EngineOwner(const EngineOwner &) = delete;
    EngineOwner &operator=(const EngineOwner &) = delete;
    // No query clients, text, candidate caches or TSF objects are owned here.
    // Explicit stop must observe host exit before this loop can report success.
    bool run(HANDLE stop, HANDLE ready) {
        if (!stop || !ready || stop == ready) throw std::invalid_argument("invalid engine owner events");
        bool stopping = false, failed = false, announced = false;
        std::optional<control::Request> pending;
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
                        result->metadata.request = *pending; upstream_.respond(result->metadata); pending.reset();
                    }
                }
                if (engine_.status().controlState == control::State::Failed) { failed = true; stopHost(); }
                if (!stopping && !pending) {
                    pending = upstream_.take();
                    if (pending && !engine_.requestControl(pending->operation,pipe::now())) { failed = true; stopHost(); }
                }
            }
            auto events = engine_.waitHandles();
            if (!stopping) events.push_back(stop);
            if (announced && !stopping && !pending) events.push_back(upstream_.event());
            const auto time = pipe::now(), deadline = engine_.deadline();
            const auto timeout = deadline ? static_cast<DWORD>(deadline > time ? deadline-time : 0) : INFINITE;
            const auto status = WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,timeout);
            pipe::check(status != WAIT_FAILED);
        }
    }
};
} // namespace harukoto::engine
