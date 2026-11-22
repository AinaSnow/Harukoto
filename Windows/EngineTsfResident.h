#pragma once
#include "EngineResident.h"
#include "EngineTsf.h"

// WIN-03 step 6a/6c: how a TSF link reaches the resident engine. Runs on the
// link's worker thread only: discovery, handoff (and, when nothing answers,
// the shell-started owner of step 4), one private endpoint, a verified pipe.
// The release source is HKLM in production; tests pass an HKCU test key.
// options.ownerArguments carry the data layout the owner should serve
// (--base/--themes/--personal/--memory), chosen by the starting client.
namespace harukoto::tsf_engine {
inline Link::Connect residentConnect(engine::release::Source source = {}, engine::resident::EnsureOptions base = {}) {
    return [source = std::move(source), base = std::move(base)](uint64_t deadline) -> std::unique_ptr<Transport> {
        namespace resident = engine::resident;
        namespace pipe = engine::pipe;
        const auto time = pipe::now();
        if (deadline <= time) throw Timeout();
        auto options = base;
        options.budgetMs = deadline - time;
        options.startWaitMs = std::min<uint64_t>(options.startWaitMs, options.budgetMs);
        const auto snapshot = engine::release::read(source);
        const auto eval = engine::release::evaluate(snapshot, options.handoff.discovery.protocol);
        if (eval.kind != engine::release::Compatibility::Match) throw std::runtime_error("engine release incompatible");
        auto outcome = resident::ensure(snapshot, options);
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
        return std::make_unique<PipeTransport>(result->endpoint->connect(), session, outcome.sequence, session->revokeEvent());
    };
}

// Step 6c: the owner arguments for the data a TSF service ranks with, so a
// started engine reports the same data identity. base is the version
// folder (a snapshot since the package carries base-dictionary.json) or the
// published snapshot the service adopted; themes only when packages are on.
// Step 6d: the engine then follows publications itself: --base is the
// version folder, --base-store the published snapshots, --themes the package
// root (served while a package snapshot is current).
inline std::vector<std::wstring> layoutArguments(const std::filesystem::path &base, const std::filesystem::path &dataRoot, bool themes) {
    std::vector<std::wstring> result{L"--base", base.wstring()};
    if (dataRoot.empty()) return result;
    (void)themes;
    result.push_back(L"--base-store"); result.push_back((dataRoot / L"BaseDictionaries").wstring());
    result.push_back(L"--themes"); result.push_back((dataRoot / L"Dictionaries").wstring());
    result.push_back(L"--personal"); result.push_back((dataRoot / L"japanese-custom.tsv").wstring());
    result.push_back(L"--memory"); result.push_back((dataRoot / L"selection-memory.tsv").wstring());
    return result;
}
} // namespace harukoto::tsf_engine
