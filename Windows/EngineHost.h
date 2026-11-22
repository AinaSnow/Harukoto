#pragma once
#include "EnginePipe.h"
#include "EngineControl.h"
#include "EngineTrace.h"
#include <mutex>
#include <optional>

namespace harukoto::engine {
// Experimental service owner. Endpoints are supplied by a trusted launcher;
// each is single-use. Optional private owner control allocates fresh names.
// No TSF/COM, public discovery, restart, or dictionary publication.
class Host {
public:
    using Observer = std::function<void(const EngineTrace &)>;
    static constexpr size_t MaxConnections = 8;
    struct Options {
        size_t threads = 2, capacity = 32, replies = 8;
        uint64_t listenMs = 30000, readMs = 30000, writeMs = 10000, queryMs = 10000, terminalMs = 1000;
        // WIN-03 step 7b: when non-zero and all connection slots are occupied,
        // Allocate evicts the oldest idle connected endpoint with no in-flight
        // work that has been quiet for at least idleEvictMs.
        uint64_t idleEvictMs = 0;
        size_t connections = MaxConnections;
        std::string identity; // Data identity in Open replies (empty: none).
    };
private:
    struct Endpoint {
        std::unique_ptr<pipe::Listener> listener;
        std::unique_ptr<pipe::Channel> channel;
        ServerConnection server;
        std::deque<Frame> replies;
        uint64_t listenDeadline = 0, readDeadline = 0, writeDeadline = 0, terminalDeadline = 0, lastActivity = 0;
        bool reading = false, writing = false, ending = false, closed = false;
        size_t index;
        Endpoint(uint64_t generation, std::shared_ptr<const WindowsDictionary> dictionary, size_t number, const std::string &identity)
            : server(generation,std::move(dictionary),identity), index(number) {}
    };
    struct Job { Endpoint *owner; ServerConnection::Ticket work; bool cancelled = false; };
    Options options_;
    static pipe::Handle completionEvent() {
        pipe::Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr)); pipe::check(bool(event)); return event;
    }
    pipe::Handle completed_ = completionEvent();
    Observer observer_;
    Executor executor_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    std::vector<Job> jobs_;
    std::unique_ptr<control::Server> control_;
    std::shared_ptr<const WindowsDictionary> dictionary_;
    uint64_t generation_;
    // Step 6d: data adopted by the reload thread, handed over through offer().
    pipe::Handle offered_ = completionEvent();
    std::mutex offerMutex_;
    std::optional<std::pair<std::shared_ptr<const WindowsDictionary>, std::string>> offer_;
    void adoptOffered() {
        std::optional<std::pair<std::shared_ptr<const WindowsDictionary>, std::string>> next;
        { std::lock_guard<std::mutex> lock(offerMutex_); next.swap(offer_); }
        if (!next) return;
        dictionary_ = next->first; options_.identity = next->second;
        for (auto &endpoint : endpoints_) if (!endpoint->closed) endpoint->server.setDictionary(dictionary_, options_.identity);
    }
    size_t nextEndpoint_ = 0;
    bool started_ = false;
    static Options validate(Options options, const std::vector<std::wstring> &names, bool controlled) {
        if ((!controlled && names.empty()) || !options.connections || options.connections > MaxConnections ||
                names.size() > options.connections || !options.replies || options.replies > 64)
            throw std::invalid_argument("invalid engine host limits");
        for (auto duration : {options.listenMs,options.readMs,options.writeMs,options.queryMs,options.terminalMs})
            if (!duration || duration > 300000) throw std::invalid_argument("invalid engine host deadline");
        if (options.idleEvictMs > 300000) throw std::invalid_argument("invalid engine host idle eviction");
        for (size_t i = 0; i < names.size(); ++i)
            if (!pipe::validName(names[i]) || std::find(names.begin(),names.begin() + i,names[i]) != names.begin() + i)
                throw std::invalid_argument("invalid engine host endpoint");
        return options;
    }
    void listen(const std::wstring &name, uint64_t time) {
        if (nextEndpoint_ == std::numeric_limits<size_t>::max()) throw std::overflow_error("engine endpoint identity exhausted");
        auto endpoint = std::make_unique<Endpoint>(generation_,dictionary_,nextEndpoint_,options_.identity);
        endpoint->listenDeadline = time + options_.listenMs;
        endpoint->lastActivity = time;
        endpoint->listener = std::make_unique<pipe::Listener>(name,time,endpoint->listenDeadline);
        endpoints_.push_back(std::move(endpoint)); ++nextEndpoint_;
    }
    void reap() {
        endpoints_.erase(std::remove_if(endpoints_.begin(),endpoints_.end(),[&](const auto &endpoint) {
            if (!endpoint->closed || (endpoint->channel && !endpoint->channel->drain())) return false;
            return std::none_of(jobs_.begin(),jobs_.end(),[&](const auto &job) { return job.owner == endpoint.get(); });
        }),endpoints_.end());
    }
    // Step 7b: evict the least-recently-used idle endpoint when all slots are full.
    bool reclaimIdle(uint64_t time) {
        if (!options_.idleEvictMs || endpoints_.size() < options_.connections) return false;
        Endpoint *victim = nullptr;
        for (const auto &ep : endpoints_) {
            if (ep->closed || ep->listener || ep->ending || ep->writing || !ep->replies.empty() || ep->server.busy()) continue;
            if (std::any_of(jobs_.begin(),jobs_.end(),[&](const auto &job) { return job.owner == ep.get(); })) continue;
            if (time < ep->lastActivity || time - ep->lastActivity < options_.idleEvictMs) continue;
            if (!victim || ep->lastActivity < victim->lastActivity) victim = ep.get();
        }
        if (!victim) return false;
        close(*victim);
        if (victim->channel) victim->channel->drain(true);
        reap();
        return endpoints_.size() < options_.connections;
    }
    void controlStep(uint64_t time) {
        if (!control_) return;
        const auto request = control_->take(); if (!request) return;
        control::Response response; response.request = *request; response.generation = generation_;
        response.limit = static_cast<uint32_t>(options_.connections);
        if (request->operation == control::Operation::Allocate) {
            reap();
            if (endpoints_.size() >= options_.connections) reclaimIdle(time);
            if (endpoints_.size() >= options_.connections) response.code = control::Code::Full;
            else {
                try { response.name = pipe::newName(); listen(response.name,time); }
                catch (...) { response.name.clear(); response.code = control::Code::Backend; }
            }
        }
        for (const auto &endpoint : endpoints_) {
            if (endpoint->closed) ++response.retiring;
            else if (endpoint->listener) ++response.listening;
            else ++response.connected;
        }
        response.jobs = static_cast<uint32_t>(jobs_.size()); control_->respond(response);
    }
    void cancel(Endpoint &endpoint, std::optional<uint64_t> session = {}) {
        for (auto &job : jobs_) if (job.owner == &endpoint && !job.cancelled &&
                (!session || job.work->request.id.session == *session)) {
            job.cancelled = true; executor_.cancel(job.work);
        }
    }
    void close(Endpoint &endpoint) {
        endpoint.closed = true; endpoint.server.disconnect(); endpoint.replies.clear();
        cancel(endpoint);
        if (endpoint.channel) { endpoint.channel->close(); endpoint.channel->drain(); }
        endpoint.listener.reset();
    }
    void respond(Endpoint &endpoint, Frame response, bool terminal, uint64_t time) {
        if (endpoint.replies.size() + (endpoint.writing ? 1 : 0) >= options_.replies) { close(endpoint); return; }
        endpoint.replies.push_back(std::move(response));
        if (terminal) {
            endpoint.ending = true; endpoint.terminalDeadline = time + options_.terminalMs;
            cancel(endpoint);
        }
    }
    void result(Endpoint &endpoint, ServerConnection::Result received, uint64_t time, bool completion = false) {
        using Status = ServerConnection::Status;
        if (received.response) respond(endpoint,std::move(*received.response),received.status == Status::Failed,time);
        if (endpoint.closed) return;
        if (received.status == Status::Scheduled) {
            // Track ownership BEFORE the worker can publish. Allocation/admission
            // failure must not orphan a task from its original endpoint.
            if (jobs_.size() >= options_.capacity) {
                result(endpoint,endpoint.server.fail(received.work,ErrorCode::Overloaded,time),time); return;
            }
            jobs_.push_back({&endpoint,received.work});
            const auto admission = executor_.submit(received.work);
            if (admission != Executor::Admission::Accepted) {
                jobs_.pop_back();
                const auto code = admission == Executor::Admission::Full ? ErrorCode::Overloaded : ErrorCode::Stopped;
                result(endpoint,endpoint.server.fail(received.work,code,time),time);
            }
        } else if (received.status == Status::Closed || (received.status == Status::Rejected &&
                !(completion && received.reason == ServerConnection::Reason::Stale))) close(endpoint);
    }
    bool step(Endpoint &endpoint, uint64_t time) {
        if (endpoint.closed) {
            if (endpoint.channel) endpoint.channel->poll(time); // Reap cancellation even while other peers remain alive.
            return false;
        }
        bool progress = false;
        if (endpoint.listener) {
            endpoint.listener->poll(time);
            if (auto channel = endpoint.listener->take()) {
                endpoint.channel = std::move(channel); endpoint.listener.reset(); endpoint.lastActivity = time; progress = true;
            } else if (endpoint.listener->closed()) { close(endpoint); return true; }
            else return false;
        }
        auto &channel = *endpoint.channel;
        if ((!endpoint.ending && endpoint.server.expire(time)) ||
                (endpoint.ending && time >= endpoint.terminalDeadline)) { close(endpoint); return true; }
        channel.poll(time);
        if (channel.closed()) { close(endpoint); return true; }
        if (endpoint.writing && channel.writeDone()) { endpoint.writing = false; progress = true; }
        if (auto frame = channel.take()) {
            endpoint.reading = false; endpoint.lastActivity = time; progress = true;
            if (!endpoint.ending) {
                const auto received = endpoint.server.receive(*frame,time,time + options_.queryMs);
                if ((frame->kind == Kind::Cancel || frame->kind == Kind::Close) &&
                        received.status == ServerConnection::Status::Reply) cancel(endpoint,frame->id.session);
                result(endpoint,received,time);
            }
        }
        if (endpoint.closed) return true;
        if (!endpoint.writing && !endpoint.replies.empty()) {
            endpoint.writeDeadline = time + options_.writeMs;
            if (!channel.beginWrite(endpoint.replies.front(),time,endpoint.writeDeadline)) { close(endpoint); return true; }
            endpoint.replies.pop_front(); endpoint.writing = true; progress = true;
        }
        if (!endpoint.reading) {
            endpoint.readDeadline = time + options_.readMs;
            if (!channel.beginRead(time,endpoint.readDeadline)) { close(endpoint); return true; }
            endpoint.reading = true; progress = true;
        }
        channel.poll(time);
        if (channel.closed()) { close(endpoint); return true; }
        return progress || channel.needsPoll();
    }
    void shutdown() {
        for (auto &endpoint : endpoints_) close(*endpoint);
        executor_.stop(); executor_.join();
        while (executor_.tryTake()) {}
        jobs_.clear();
        endpoints_.clear(); // Destruct/reap OVERLAPPED on this I/O owner before run returns.
        dictionary_.reset();
    }
public:
    Host(const std::vector<std::wstring> &names, std::shared_ptr<const WindowsDictionary> dictionary,
            uint64_t generation, Executor::Query query, Options options, Observer observer = {},
            std::unique_ptr<control::Server> control = {})
        : options_(validate(options,names,bool(control))), observer_(std::move(observer)),
          executor_(options_.threads,options_.capacity,std::move(query),Executor::now,
            [event = completed_.get()] { pipe::check(SetEvent(event)); },observer_ ? Executor::traceNow : nullptr),
          control_(std::move(control)), dictionary_(std::move(dictionary)), generation_(generation) {
        if (!generation_) throw std::invalid_argument("missing engine generation");
        pipe::check(bool(completed_));
        jobs_.reserve(options_.capacity);
        const auto time = pipe::now();
        for (const auto &name : names) listen(name,time);
    }
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
    // Any thread: the dictionary to serve from the next request on, with its
    // data identity. The I/O thread adopts it between requests.
    void offer(std::shared_ptr<const WindowsDictionary> dictionary, std::string identity) {
        { std::lock_guard<std::mutex> lock(offerMutex_); offer_.emplace(std::move(dictionary), std::move(identity)); }
        pipe::check(SetEvent(offered_.get()));
    }
    ~Host() { shutdown(); }
    // Ready is signaled only after complete dictionary preparation and every
    // listener creation. Events are borrowed from the trusted launcher.
    void run(HANDLE stop, HANDLE ready) {
        if (started_ || !stop || !ready || stop == ready) throw std::invalid_argument("invalid engine host control");
        started_ = true;
        pipe::check(SetEvent(ready));
        try {
            for (;;) {
                const auto stopped = WaitForSingleObject(stop,0);
                if (stopped == WAIT_OBJECT_0) break;
                if (stopped != WAIT_TIMEOUT) throw pipe::Error(pipe::Failure::Io,GetLastError());
                bool progress = false, active = false;
                for (auto &endpoint : endpoints_) {
                    try { progress = step(*endpoint,pipe::now()) || progress; }
                    catch (...) { close(*endpoint); progress = true; }
                    active = active || !endpoint->closed;
                }
                // Drain a bounded mailbox; canceled/disconnected tickets retain
                // capacity until this exact object has been reclaimed.
                while (auto completion = executor_.tryTake()) {
                    const auto found = std::find_if(jobs_.begin(),jobs_.end(),[&](const Job &job) { return job.work == completion->work; });
                    if (found == jobs_.end()) throw std::logic_error("unowned engine completion");
                    const auto job = *found; jobs_.erase(found); progress = true;
                    if (!job.cancelled && !job.owner->closed && !job.owner->ending) {
                        try {
                            const auto doneTime = pipe::now();
                            job.owner->lastActivity = doneTime;
                            auto received = completion->deliver(job.owner->server,doneTime);
                            const bool replied = received.status == ServerConnection::Status::Reply;
                            result(*job.owner,std::move(received),doneTime,true);
                            if (observer_ && completion->timings && replied && !job.owner->closed) {
                                const EngineTrace trace{job.owner->index,job.work->request.id,*completion->timings,Executor::traceNow()};
                                // Like Wake, the optional observer must not block,
                                // throw, re-enter Host or access TSF objects.
                                try { observer_(trace); } catch (...) { std::terminate(); }
                            }
                        }
                        catch (...) { close(*job.owner); }
                    }
                }
                adoptOffered();
                if (control_) { reap(); controlStep(pipe::now()); }
                if (!active && !control_) break;
                if (progress) continue;
                std::vector<HANDLE> events{stop,completed_.get(),offered_.get()};
                if (control_) events.push_back(control_->event());
                uint64_t deadline = pipe::now() + 300000;
                for (const auto &endpoint : endpoints_) {
                    if (endpoint->closed) {
                        if (endpoint->channel) for (auto event : {endpoint->channel->readEvent(),endpoint->channel->writeEvent()})
                            if (event) events.push_back(event);
                        continue;
                    }
                    if (endpoint->listener) {
                        if (auto event = endpoint->listener->event()) events.push_back(event);
                        deadline = std::min(deadline,endpoint->listenDeadline);
                    } else {
                        for (auto event : {endpoint->channel->readEvent(),endpoint->channel->writeEvent()}) if (event) events.push_back(event);
                        if (endpoint->reading) deadline = std::min(deadline,endpoint->readDeadline);
                        if (endpoint->writing) deadline = std::min(deadline,endpoint->writeDeadline);
                        if (endpoint->ending) deadline = std::min(deadline,endpoint->terminalDeadline);
                    }
                }
                for (const auto &job : jobs_) if (!job.cancelled) deadline = std::min(deadline,job.work->deadline);
                const auto time = pipe::now();
                const auto status = WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,
                    static_cast<DWORD>(deadline > time ? deadline - time : 0));
                if (status == WAIT_FAILED) throw pipe::Error(pipe::Failure::Io,GetLastError());
            }
        } catch (...) { shutdown(); throw; }
        shutdown();
    }
};
} // namespace harukoto::engine
