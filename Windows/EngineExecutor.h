#pragma once
#include "EngineServer.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace harukoto::engine {
// Shared by connections on one owner event loop. Workers never call a connection
// or COM; the owner routes each immutable ticket back to its original object.
class Executor {
public:
    using Ticket = ServerConnection::Ticket;
    using Query = std::function<CandidatePage(const Ticket &)>;
    using Clock = std::function<uint64_t()>;
    using Wake = std::function<void()>;
    using TraceClock = uint64_t (*)() noexcept;
    struct Timings {
        uint64_t submitted = 0, started = 0, finished = 0, published = 0, taken = 0;
    };
    enum class Admission { Accepted, Full, Stopped, Duplicate, Invalid };
    struct Completion {
        Ticket work;
        std::optional<CandidatePage> page;
        ErrorCode error = ErrorCode::Backend;
        std::optional<Timings> timings;
        // Owner-thread only. Server checks exact ticket ownership again.
        ServerConnection::Result deliver(ServerConnection &server, uint64_t now) const {
            return page ? server.complete(work, *page, now) : server.fail(work, error, now);
        }
    };
    static uint64_t now() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // Optional profiling clock, distinct from the millisecond deadline clock.
    static uint64_t traceNow() noexcept {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
private:
    enum class State { Free, Queued, Running, Complete };
    struct Slot {
        State state = State::Free;
        bool cancelled = false;
        Completion result;
    };
    Query query_;
    Clock clock_;
    Wake wake_;
    TraceClock traceClock_;
    std::vector<Slot> slots_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable ready_, completed_;
    size_t occupied_ = 0, queued_ = 0, finished_ = 0, cursor_ = 0;
    bool stopping_ = false;
    // Called with mutex_ held after publication. A wake hook must be nonblocking,
    // must not throw or re-enter Executor, and must outlive every worker.
    void notifyCompleted() noexcept {
        completed_.notify_all();
        if (wake_) { try { wake_(); } catch (...) { std::terminate(); } }
    }
    std::optional<Completion> takeLocked() {
        for (auto &slot : slots_) if (slot.state == State::Complete) {
            auto result = std::move(slot.result);
            if (result.timings) result.timings->taken = traceClock_();
            slot = Slot{}; --occupied_; --finished_; return result;
        }
        return {};
    }
    void run() {
        for (;;) {
            size_t index;
            Ticket work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || queued_; });
                if (stopping_) return;
                // Round-robin selection across bounded preallocated slots.
                while (slots_[cursor_].state != State::Queued) cursor_ = (cursor_ + 1) % slots_.size();
                index = cursor_; cursor_ = (cursor_ + 1) % slots_.size();
                slots_[index].state = State::Running; --queued_;
                work = slots_[index].result.work;
                if (slots_[index].result.timings) slots_[index].result.timings->started = traceClock_();
            }
            std::optional<CandidatePage> page;
            ErrorCode error = ErrorCode::Backend;
            try {
                if (clock_() >= work->deadline) error = ErrorCode::Deadline;
                else {
                    auto result = query_(work);
                    // Validate before retaining a result in the bounded mailbox.
                    if (result.input != work->input) throw std::invalid_argument("query input mismatch");
                    (void)encodePayload(result);
                    if (clock_() >= work->deadline) error = ErrorCode::Deadline;
                    else page = std::move(result);
                }
            } catch (...) { page.reset(); error = ErrorCode::Backend; }
            const auto finished = traceClock_ ? traceClock_() : 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto &slot = slots_[index];
                if (stopping_ || slot.cancelled) {
                    page.reset(); error = stopping_ ? ErrorCode::Stopped : ErrorCode::Cancelled;
                }
                slot.result.page = std::move(page); slot.result.error = error;
                if (slot.result.timings) {
                    slot.result.timings->finished = finished;
                    slot.result.timings->published = traceClock_();
                }
                slot.state = State::Complete; ++finished_;
                notifyCompleted();
            }
        }
    }
public:
    // Query and Clock must support concurrent calls and return. They must not
    // capture a connection, TSF context or thread-affine database connection.
    explicit Executor(size_t threads, size_t capacity, Query query, Clock clock = now, Wake wake = {}, TraceClock traceClock = nullptr)
        : query_(std::move(query)), clock_(std::move(clock)), wake_(std::move(wake)), traceClock_(traceClock) {
        if (!threads || threads > 16 || !capacity || capacity > 256 || threads > capacity || !query_ || !clock_)
            throw std::invalid_argument("invalid engine executor limits");
        slots_.resize(capacity); workers_.reserve(threads);
        try { for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); }); }
        catch (...) { stop(); join(); throw; }
    }
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    ~Executor() { stop(); join(); }
    Admission submit(const Ticket &work) {
        if (!work || work->request.kind != Kind::Key || !valid(work->request) || !inputValid(work->input))
            return Admission::Invalid;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return Admission::Stopped;
        for (const auto &slot : slots_)
            if (slot.state != State::Free && slot.result.work == work) return Admission::Duplicate;
        if (occupied_ == slots_.size()) return Admission::Full;
        for (auto &slot : slots_) if (slot.state == State::Free) {
            if (traceClock_) { slot.result.timings = Timings{}; slot.result.timings->submitted = traceClock_(); }
            slot.result.work = work; slot.state = State::Queued; ++occupied_; ++queued_; break;
        }
        ready_.notify_one(); return Admission::Accepted;
    }
    // Cancel a ticket after the owner has invalidated it in ServerConnection.
    // Running queries are cooperative: their result is discarded when they return.
    bool cancel(const Ticket &work) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!work || stopping_) return false;
        for (auto &slot : slots_) if (slot.state != State::Free && slot.result.work == work) {
            slot.cancelled = true;
            if (slot.state != State::Running) {
                if (slot.state == State::Queued && slot.result.timings) slot.result.timings->published = traceClock_();
                if (slot.state == State::Queued) { --queued_; ++finished_; }
                slot.state = State::Complete; slot.result.page.reset(); slot.result.error = ErrorCode::Cancelled;
                notifyCompleted();
            }
            return true;
        }
        return false;
    }
    std::optional<Completion> tryTake() {
        std::lock_guard<std::mutex> lock(mutex_); return takeLocked();
    }
    bool waitUntilReady(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        completed_.wait_until(lock, deadline, [&] { return finished_ || (stopping_ && !occupied_); });
        return finished_ != 0;
    }
    // Only for a service event loop/test, never for a host application's TSF thread.
    std::optional<Completion> takeUntil(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        completed_.wait_until(lock, deadline, [&] { return finished_ || (stopping_ && !occupied_); });
        return takeLocked();
    }
    size_t outstanding() {
        std::lock_guard<std::mutex> lock(mutex_); return occupied_;
    }
    size_t completed() {
        std::lock_guard<std::mutex> lock(mutex_); return finished_;
    }
    // Nonblocking: retain one terminal result for every admitted ticket, including
    // queued/untaken results. Running backend calls finish before join can return.
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        for (auto &slot : slots_) if (slot.state == State::Queued || slot.state == State::Complete) {
            if (slot.state == State::Queued && slot.result.timings) slot.result.timings->published = traceClock_();
            if (slot.state == State::Queued) { --queued_; ++finished_; }
            slot.state = State::Complete; slot.result.page.reset(); slot.result.error = ErrorCode::Stopped;
        }
        ready_.notify_all(); notifyCompleted();
    }
    // Owner shutdown only, serialized with destruction/other join calls.
    void join() { for (auto &worker : workers_) if (worker.joinable()) worker.join(); }
};
} // namespace harukoto::engine
