#pragma once
#include "EnginePipe.h"
#include "EngineTrace.h"

namespace harukoto::engine {
// Experimental service owner. Endpoints are supplied by a trusted launcher;
// each is single-use. No TSF/COM, discovery, restart, or dictionary publication.
class Host {
public:
    using Observer = std::function<void(const EngineTrace &)>;
    static constexpr size_t MaxConnections = 8;
    struct Options {
        size_t threads = 2, capacity = 32, replies = 8;
        uint64_t listenMs = 30000, readMs = 30000, writeMs = 10000, queryMs = 10000, terminalMs = 1000;
    };
private:
    struct Endpoint {
        std::unique_ptr<pipe::Listener> listener;
        std::unique_ptr<pipe::Channel> channel;
        ServerConnection server;
        std::deque<Frame> replies;
        uint64_t listenDeadline = 0, readDeadline = 0, writeDeadline = 0, terminalDeadline = 0;
        bool reading = false, writing = false, ending = false, closed = false;
        size_t index;
        Endpoint(uint64_t generation, std::shared_ptr<const WindowsDictionary> dictionary, size_t number)
            : server(generation,std::move(dictionary)), index(number) {}
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
    bool started_ = false;
    static Options validate(Options options, const std::vector<std::wstring> &names) {
        if (names.empty() || names.size() > MaxConnections || !options.replies || options.replies > 64)
            throw std::invalid_argument("invalid engine host limits");
        for (auto duration : {options.listenMs,options.readMs,options.writeMs,options.queryMs,options.terminalMs})
            if (!duration || duration > 300000) throw std::invalid_argument("invalid engine host deadline");
        for (size_t i = 0; i < names.size(); ++i)
            if (!pipe::validName(names[i]) || std::find(names.begin(),names.begin() + i,names[i]) != names.begin() + i)
                throw std::invalid_argument("invalid engine host endpoint");
        return options;
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
                endpoint.channel = std::move(channel); endpoint.listener.reset(); progress = true;
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
            endpoint.reading = false; progress = true;
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
    }
public:
    Host(const std::vector<std::wstring> &names, std::shared_ptr<const WindowsDictionary> dictionary,
            uint64_t generation, Executor::Query query, Options options, Observer observer = {})
        : options_(validate(options,names)), observer_(std::move(observer)),
          executor_(options_.threads,options_.capacity,std::move(query),Executor::now,
            [event = completed_.get()] { pipe::check(SetEvent(event)); },observer_ ? Executor::traceNow : nullptr) {
        pipe::check(bool(completed_));
        jobs_.reserve(options_.capacity);
        const auto time = pipe::now();
        for (const auto &name : names) {
            auto endpoint = std::make_unique<Endpoint>(generation,dictionary,endpoints_.size());
            endpoint->listenDeadline = time + options_.listenMs;
            endpoint->listener = std::make_unique<pipe::Listener>(name,time,endpoint->listenDeadline);
            endpoints_.push_back(std::move(endpoint));
        }
    }
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
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
                            auto received = completion->deliver(job.owner->server,pipe::now());
                            const bool replied = received.status == ServerConnection::Status::Reply;
                            result(*job.owner,std::move(received),pipe::now(),true);
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
                if (!active) break;
                if (progress) continue;
                std::vector<HANDLE> events{stop,completed_.get()};
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
