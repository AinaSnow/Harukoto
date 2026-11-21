#pragma once
#include "EngineProcess.h"
#include "EngineDiscovery.h"
#include <iostream>

namespace harukoto::engine {
// Independent owner's I/O loop. One launcher port and up to eight separately
// provisioned client ports share one bounded host request fairly. With a
// discovery scope it also publishes the discovery pipe (step 2) and hands
// registration slots to verified clients over it (step 3, ENGINE_HANDOFF.md).
class EngineOwner {
public:
    // process: handoff slots only, the verified client's process (SYNCHRONIZE);
    // its exit retires the slot like a revoke.
    struct Registration { std::unique_ptr<control::Server> port; pipe::Handle revoked; uint64_t id = 0; pipe::Handle process = pipe::Handle(); };
    // Handoff ids live in their own space so they never equal a launcher id.
    static constexpr uint64_t HandoffIds = uint64_t(1) << 63;
private:
    EngineProcess engine_;
    control::Server &upstream_;
    EngineOwnerAnchor &anchor_;
    std::vector<Registration> clients_;
    admission::Server *admission_;
    std::wstring discoveryScope_; // Empty: not discoverable (all existing launches).
    uint64_t handoffNext_ = 0;
    // Fresh unnamed slot objects; the client gets restricted duplicates. The
    // release event is the slot's revoke event: the owner sets it to revoke,
    // the client may set it to give the slot back.
    struct Fresh { std::array<pipe::Handle,4> local; std::array<uint64_t,4> remote{}; };
    static Fresh provision(HANDLE client) {
        Fresh fresh;
        fresh.local[0].reset(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,control::PacketSize,nullptr));
        pipe::check(bool(fresh.local[0]));
        for (size_t i=1;i<4;++i) { fresh.local[i].reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(fresh.local[i])); }
        constexpr DWORD rights[4]{FILE_MAP_READ | FILE_MAP_WRITE,EVENT_MODIFY_STATE | SYNCHRONIZE,EVENT_MODIFY_STATE | SYNCHRONIZE,EVENT_MODIFY_STATE | SYNCHRONIZE};
        size_t done = 0;
        try {
            for (;done<4;++done) {
                HANDLE remote = nullptr;
                pipe::check(DuplicateHandle(GetCurrentProcess(),fresh.local[done].get(),client,&remote,rights[done],FALSE,0));
                fresh.remote[done] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote));
            }
        } catch (...) {
            // Nobody in the client knows these values yet: safe to close remotely.
            for (size_t i=0;i<done;++i)
                DuplicateHandle(client,reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fresh.remote[i])),nullptr,nullptr,0,FALSE,DUPLICATE_CLOSE_SOURCE);
            throw;
        }
        return fresh;
    }
    static EngineProcess::Config validate(EngineProcess::Config config) {
        if (!config.dynamicEndpoints || !config.ownerExecutable.empty()) throw std::invalid_argument("invalid engine owner configuration");
        return config;
    }
public:
    EngineOwner(EngineProcess::Config config, control::Server &upstream, EngineOwnerAnchor &anchor,
            std::vector<Registration> clients = {}, admission::Server *admission = nullptr, std::wstring discoveryScope = {})
        : engine_(validate(std::move(config))), upstream_(upstream), anchor_(anchor), clients_(std::move(clients)), admission_(admission),
          discoveryScope_(std::move(discoveryScope)) {
        if (!discoveryScope_.empty() && !EngineInstance::validScope(discoveryScope_)) throw std::invalid_argument("invalid discovery scope");
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
        // Handoff slots between Grant and Commit: hold a position, never scheduled.
        std::vector<bool> reserved(ports.size(),false);
        size_t next = 0;
        auto release = [&](size_t index) {
            if (!active[index] && !reserved[index]) return;
            auto &client = clients_[index-1];
            pipe::check(SetEvent(client.revoked.get()));
            active[index] = false; reserved[index] = false; ports[index] = nullptr; client.port.reset(); client.process.reset();
            // Pending keeps only an index and copied request. Host completion
            // will be drained and discarded, never written through this port.
        };
        auto observeRevocations = [&] {
            for (size_t i=1;i<ports.size();++i) if (active[i] || reserved[i]) {
                const auto status = WaitForSingleObject(clients_[i-1].revoked.get(),0);
                pipe::check(status != WAIT_FAILED);
                bool gone = status == WAIT_OBJECT_0;
                if (!gone && clients_[i-1].process) {
                    const auto exited = WaitForSingleObject(clients_[i-1].process.get(),0);
                    pipe::check(exited != WAIT_FAILED); gone = exited == WAIT_OBJECT_0;
                }
                if (gone) release(i);
            }
        };
        auto handoffIndex = [&](uint64_t id) -> size_t {
            for (size_t i=1;i<ports.size();++i) if ((active[i] || reserved[i]) && clients_[i-1].id == id) return i;
            return 0;
        };
        // Step 3 slots for verified discovery clients (ENGINE_HANDOFF.md).
        discovery::Admitter admitter;
        admitter.reserve = [&](HANDLE client) {
            discovery::Offer offer;
            if (stopping) { offer.code = discovery::Grant::Closing; return offer; }
            observeRevocations();
            size_t index = 0;
            for (size_t i=1;i<ports.size();++i) if (!active[i] && !reserved[i]) { index = i; break; }
            if (!index) { offer.code = discovery::Grant::Full; return offer; }
            if (handoffNext_ >= HandoffIds-1) { offer.code = discovery::Grant::Full; return offer; } // Never wrap.
            HANDLE raw = nullptr;
            pipe::check(DuplicateHandle(GetCurrentProcess(),client,GetCurrentProcess(),&raw,SYNCHRONIZE,FALSE,0));
            pipe::Handle process(raw);
            auto fresh = provision(client);
            const auto id = HandoffIds | ++handoffNext_;
            clients_[index-1] = Registration{std::make_unique<control::Server>(std::array<pipe::Handle,3>{
                std::move(fresh.local[0]),std::move(fresh.local[1]),std::move(fresh.local[2])}),std::move(fresh.local[3]),id,std::move(process)};
            reserved[index] = true;
            offer.code = discovery::Grant::Ok; offer.registration = id; offer.handles = fresh.remote;
            return offer;
        };
        admitter.activate = [&](uint64_t id) {
            const auto index = handoffIndex(id);
            if (!index || !reserved[index]) return false;
            if (!stopping) observeRevocations(); // Released or exited meanwhile.
            if (stopping || !reserved[index]) { release(index); return false; }
            reserved[index] = false; active[index] = true; ports[index] = clients_[index-1].port.get();
            return true;
        };
        admitter.discard = [&](uint64_t id) { if (const auto index = handoffIndex(id)) release(index); };
        // Public discovery pipe (step 2). Published only once the host is
        // Ready, closed first when stopping. Failure to publish (squatted
        // name, unsupported token) never stops the engine: clients then stay
        // on their in-process path. Declared after the state the admitter
        // touches, so its destructor (which discards a half-done handoff)
        // runs first.
        std::unique_ptr<discovery::Server> discoverable;
        struct Close { std::unique_ptr<discovery::Server> &server; ~Close() { server.reset(); } } close{discoverable};
        auto dropDiscovery = [&](const char *reason, DWORD code) {
            std::cerr << "engine_owner: discovery " << reason << " win32=" << code << '\n'; discoverable.reset();
        };
        auto stopHost = [&] {
            if (!stopping) {
                stopping = true; pending.reset(); discoverable.reset();
                for (size_t i=1;i<ports.size();++i) release(i);
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
                    anchor_.publish(engine_.processHandle());
                    if (!discoveryScope_.empty()) {
                        try {
                            discoverable = std::make_unique<discovery::Server>(discoveryScope_,engine_.processHandle(),
                                ProtocolVersion,discovery::DefaultConnectionMs,admitter);
                        }
                        catch (const discovery::Server::Squatted &error) { dropDiscovery("name already exists",error.code); }
                        catch (const pipe::Error &error) { dropDiscovery("unavailable",error.code); }
                    }
                    pipe::check(SetEvent(ready)); announced = true;
                }
                if (discoverable) {
                    try { discoverable->poll(pipe::now()); }
                    catch (const pipe::Error &error) { dropDiscovery("failed",error.code); }
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
                    size_t free = 0;
                    for (size_t i=1;i<ports.size();++i) if (!active[i] && !reserved[i]) { free = i; break; }
                    if (revoked == WAIT_OBJECT_0) admission_->respond(admission::Code::Closed);
                    else if (!free) { pipe::check(SetEvent(handles[3].get())); admission_->respond(admission::Code::Full); }
                    else {
                        clients_[free-1] = Registration{std::move(port),std::move(handles[3]),offer->id};
                        ports[free] = clients_[free-1].port.get(); active[free] = true;
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
                            release(index); continue; // Retire only this malformed client.
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
                for (size_t i=1;i<ports.size();++i) if (active[i] || reserved[i]) {
                    events.push_back(clients_[i-1].revoked.get());
                    if (clients_[i-1].process) events.push_back(clients_[i-1].process.get());
                }
            if (announced && !stopping && admission_) events.push_back(admission_->event());
            if (announced && !stopping && !pending)
                for (size_t i=0;i<ports.size();++i) if (active[i]) events.push_back(ports[i]->event());
            auto deadline = engine_.deadline();
            if (discoverable && !stopping) {
                if (auto event = discoverable->event()) events.push_back(event);
                const auto own = discoverable->deadline();
                if (own && (!deadline || own < deadline)) deadline = own;
            }
            const auto time = pipe::now();
            const auto timeout = deadline ? static_cast<DWORD>(deadline > time ? deadline-time : 0) : INFINITE;
            const auto status = WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,timeout);
            pipe::check(status != WAIT_FAILED);
        }
    }
};
} // namespace harukoto::engine
