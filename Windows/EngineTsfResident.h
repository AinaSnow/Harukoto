#pragma once
#include "EngineResident.h"
#include "EngineTsf.h"

// WIN-03 step 6a: how a TSF link reaches the resident engine. Runs on the
// link's worker thread only: discovery, handoff (and, when nothing answers,
// the shell-started owner of step 4), one private endpoint, a verified pipe.
// The release source is HKLM in production; tests pass an HKCU test key.
namespace harukoto::tsf_engine {
inline Link::Connect residentConnect(engine::release::Source source = {}, std::wstring scope = engine::resident::DefaultScope) {
    return [source = std::move(source), scope = std::move(scope)](uint64_t deadline) -> std::unique_ptr<Transport> {
        namespace resident = engine::resident;
        namespace pipe = engine::pipe;
        const auto time = pipe::now();
        if (deadline <= time) throw Timeout();
        resident::EnsureOptions options;
        options.handoff.discovery.scope = scope;
        options.budgetMs = deadline - time;
        options.startWaitMs = std::min<uint64_t>(options.startWaitMs, options.budgetMs);
        auto outcome = resident::ensure(engine::release::read(source), options);
        auto &admitted = outcome.attach.outcome;
        if (admitted.result != engine::handoff::Result::Admitted || !admitted.session) throw std::runtime_error("engine not admitted");
        std::shared_ptr<engine::handoff::Session> session(std::move(admitted.session));
        auto &registration = session->registration();
        if (!registration.request(engine::control::Operation::Allocate, pipe::now(), DeadlineMs)) throw std::runtime_error("engine allocation refused");
        const auto limit = pipe::now() + DeadlineMs;
        while (registration.poll(pipe::now()) == engine::control::State::Pending) {
            auto events = registration.waitHandles();
            const auto now = pipe::now();
            if (events.empty() || now >= limit) throw Timeout();
            WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, static_cast<DWORD>(limit - now));
        }
        auto result = registration.take(pipe::now());
        if (!result || !result->endpoint) throw std::runtime_error("engine allocation failed");
        // The slot stays ours while the transport lives; destroying it hands it back.
        return std::make_unique<PipeTransport>(result->endpoint->connect(), session);
    };
}
} // namespace harukoto::tsf_engine
