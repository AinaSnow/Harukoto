#include "../EngineExecutor.h"
#include "../PendingDelivery.h"
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <set>
using namespace harukoto::engine;
using Ticket = ServerConnection::Ticket;
using Admission = Executor::Admission;
using Status = ServerConnection::Status;
using Reason = ServerConnection::Reason;
static auto deadline() { return std::chrono::steady_clock::now() + std::chrono::seconds(5); }
static CandidatePage page(const Ticket &work) { return {work->input,false,{{1,1,"字",""}}}; }
static Ticket ticket(char input = 'a', uint64_t expires = 100) {
    auto work = std::make_shared<ServerConnection::Work>();
    work->request = {Kind::Key,{1,42,1,1,1},encodePayload(KeyMessage{KeyAction::Text,static_cast<uint8_t>(input),true})};
    work->input = input; work->deadline = expires; return work;
}
static Executor::Completion take(Executor &executor) {
    auto result = executor.takeUntil(deadline()); assert(result); return std::move(*result);
}
struct Block {
    std::promise<void> entered, release;
    std::shared_future<void> go = release.get_future().share();
    void run() { entered.set_value(); go.wait(); }
    void wait() { assert(entered.get_future().wait_until(deadline()) == std::future_status::ready); }
};
struct Peer {
    ServerConnection server{42};
    uint64_t seq = 0, composition = 1;
    Peer() {
        assert(server.receive({Kind::Hello,{1,0,0,0,++seq},{}},0,100).response);
        assert(server.receive(frame(Kind::Open),0,100).response);
    }
    Frame frame(Kind kind) { return {kind,{1,42,1,composition,++seq},{}}; }
    Ticket key(char c) {
        auto request = frame(Kind::Key);
        request.payload = encodePayload(KeyMessage{KeyAction::Text,static_cast<uint8_t>(c),true});
        auto result = server.receive(request,0,100); assert(result.work); return result.work;
    }
    void cancel() { ++composition; assert(server.receive(frame(Kind::Cancel),0,100).response); }
};
static void boundedMailbox() {
    Block blocked;
    Executor executor(1,1,[&](const Ticket &work) { blocked.run(); return page(work); },[] { return 0; });
    auto first = ticket(), other = ticket('b');
    assert(executor.submit({}) == Admission::Invalid);
    assert(executor.submit(first) == Admission::Accepted); blocked.wait();
    assert(executor.submit(first) == Admission::Duplicate && executor.submit(other) == Admission::Full);
    blocked.release.set_value();
    assert(executor.waitUntilReady(deadline()) && executor.completed() == 1);
    // Completed but uncollected results still occupy capacity.
    assert(executor.outstanding() == 1 && executor.submit(other) == Admission::Full);
    auto result = take(executor); assert(result.work == first && result.page);
    assert(executor.outstanding() == 0 && !executor.tryTake());
    executor.stop(); executor.join(); assert(executor.submit(other) == Admission::Stopped);
    assert(!executor.waitUntilReady(deadline()));
    for (const auto limits : {std::pair<size_t,size_t>{0,1},{1,0},{17,17},{1,257},{2,1}}) {
        bool rejected = false;
        try { Executor invalid(limits.first,limits.second,page); } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
}
static void queueCancellationAndDeadline() {
    Block blocked; std::atomic<uint64_t> clock{0}; std::atomic<int> calls{0};
    auto first = ticket('a'), queued = ticket('b',10), cancelled = ticket('c');
    Executor executor(1,3,[&](const Ticket &work) {
        ++calls; if (work == first) blocked.run(); return page(work);
    },[&] { return clock.load(); });
    assert(executor.submit(first) == Admission::Accepted); blocked.wait();
    assert(executor.submit(queued) == Admission::Accepted && executor.submit(cancelled) == Admission::Accepted);
    assert(!executor.cancel(std::make_shared<const ServerConnection::Work>(*cancelled)));
    assert(executor.cancel(cancelled));
    auto removed = take(executor); assert(removed.work == cancelled && !removed.page && removed.error == ErrorCode::Cancelled);
    clock = 10; blocked.release.set_value();
    std::set<Ticket> received;
    for (int i=0;i<2;++i) {
        auto result = take(executor); received.insert(result.work);
        if (result.work == first) assert(result.page);
        else assert(result.work == queued && !result.page && result.error == ErrorCode::Deadline);
    }
    assert(received.size() == 2 && calls == 1); executor.stop(); executor.join();
}
static void cancellationAndOwnerIsolation() {
    Peer owner, foreign;
    auto old = owner.key('a'), other = foreign.key('a'); assert(old->request.id == other->request.id);
    assert(owner.server.fail(other,ErrorCode::Backend,1).reason == Reason::Stale && !owner.server.closed());
    assert(owner.server.fail(std::make_shared<const ServerConnection::Work>(*old),ErrorCode::Backend,1).reason == Reason::Stale);
    Block blocked;
    Executor executor(1,2,[&](const Ticket &work) {
        if (work == old) { blocked.run(); throw std::runtime_error("private backend detail"); }
        return page(work);
    },[] { return 0; });
    assert(executor.submit(old) == Admission::Accepted); blocked.wait();
    owner.cancel(); auto fresh = owner.key('b');
    assert(executor.cancel(old) && executor.submit(fresh) == Admission::Accepted);
    blocked.release.set_value();
    for (int i=0;i<2;++i) {
        auto result = take(executor);
        if (result.work == old) {
            assert(!result.page && result.error == ErrorCode::Cancelled);
            assert(result.deliver(owner.server,1).reason == Reason::Stale && !owner.server.closed());
        } else {
            assert(result.work == fresh && result.page);
            assert(result.deliver(foreign.server,1).reason == Reason::Stale);
            assert(result.deliver(owner.server,1).status == Status::Reply);
        }
    }
    // A late uncancelled failure also cannot terminate the fresh connection state.
    assert(owner.server.fail(old,ErrorCode::Backend,1000).reason == Reason::Stale && !owner.server.closed());
    executor.stop(); executor.join();
}
static void failuresAndWorkerSurvival() {
    Executor executor(1,2,[](const Ticket &work) {
        if (work->input == "a") throw std::runtime_error("do not expose input or exception text");
        auto result = page(work);
        if (work->input == "b") result.input = "wrong";
        if (work->input == "c") result.words[0].text = std::string("\xff",1);
        if (work->input == "d") {
            result.words.clear();
            for (uint64_t i=1;i<=40;++i) result.words.push_back({i,1,std::string(1024,'x'),std::string(1024,'y')});
        }
        return result;
    },[] { return 0; });
    for (char c : std::string("abcdz")) {
        Peer peer; auto work = peer.key(c); assert(executor.submit(work) == Admission::Accepted);
        auto result = take(executor); assert(result.work == work);
        auto response = result.deliver(peer.server,1);
        if (c == 'z') assert(result.page && response.status == Status::Reply && !peer.server.closed());
        else {
            ErrorMessage error;
            assert(!result.page && response.status == Status::Failed && peer.server.closed());
            assert(response.response->kind == Kind::Error && response.response->payload.size() == 4);
            assert(decodePayload(response.response->payload,error) && error.code == ErrorCode::Backend);
        }
    }
    // An admission failure is explicitly terminalized by the owner, not an empty page.
    Peer overloaded; auto work = overloaded.key('x');
    auto failure = overloaded.server.fail(work,ErrorCode::Overloaded,1); ErrorMessage error;
    assert(failure.status == Status::Failed && decodePayload(failure.response->payload,error) && error.code == ErrorCode::Overloaded);
    executor.stop(); executor.join();
}
static void runningDeadlineAndShutdown() {
    Block blocked; std::atomic<uint64_t> clock{0};
    Executor executor(1,1,[&](const Ticket &work) { blocked.run(); return page(work); },[&] { return clock.load(); });
    Peer peer; auto work = peer.key('a'); assert(executor.submit(work) == Admission::Accepted); blocked.wait();
    clock = 100; blocked.release.set_value(); auto result = take(executor);
    assert(!result.page && result.error == ErrorCode::Deadline);
    auto failure = result.deliver(peer.server,100); ErrorMessage error;
    assert(failure.status == Status::Failed && decodePayload(failure.response->payload,error) && error.code == ErrorCode::Deadline);
    executor.stop(); executor.join();

    Block stopping; std::atomic<int> calls{0};
    Executor shutdown(1,2,[&](const Ticket &item) { ++calls; stopping.run(); return page(item); },[] { return 0; });
    auto running = ticket('a'), queued = ticket('b'); std::weak_ptr<const ServerConnection::Work> weak = running;
    assert(shutdown.submit(running) == Admission::Accepted); stopping.wait();
    assert(shutdown.submit(queued) == Admission::Accepted); running.reset(); queued.reset();
    shutdown.stop(); shutdown.stop();
    assert(shutdown.submit(ticket()) == Admission::Stopped && !weak.expired());
    auto skipped = take(shutdown); assert(!skipped.page && skipped.error == ErrorCode::Stopped);
    stopping.release.set_value(); shutdown.join();
    {
        auto last = take(shutdown); assert(!last.page && last.error == ErrorCode::Stopped && !weak.expired());
    }
    assert(weak.expired() && calls == 1 && shutdown.outstanding() == 0);
    // stop also revokes an already completed, untaken success.
    Executor completed(1,1,page,[] { return 0; });
    assert(completed.submit(ticket()) == Admission::Accepted && completed.waitUntilReady(deadline()));
    completed.stop(); completed.join(); auto revoked = take(completed);
    assert(!revoked.page && revoked.error == ErrorCode::Stopped);
}
static void parallelAndClientFailure() {
    std::promise<void> both, release; auto go = release.get_future().share(); std::atomic<int> started{0};
    Executor executor(2,4,[&](const Ticket &work) {
        if (++started == 2) both.set_value(); go.wait(); return page(work);
    },[] { return 0; });
    std::set<Ticket> submitted, received;
    for (char c : std::string("abcd")) { auto work = ticket(c); submitted.insert(work); assert(executor.submit(work) == Admission::Accepted); }
    assert(both.get_future().wait_until(deadline()) == std::future_status::ready);
    assert(started == 2 && executor.submit(ticket('z')) == Admission::Full); release.set_value();
    for (int i=0;i<4;++i) { auto result = take(executor); assert(result.page); received.insert(result.work); }
    assert(submitted == received && started == 4); executor.stop(); executor.join();

    ClientGate client; ServerConnection server(42);
    auto hello = client.connect(0,100); auto ack = server.receive(hello,0,100);
    assert(client.acknowledge(*ack.response,0));
    auto open = client.open(0,100); auto opened = server.receive(*open,0,100); assert(client.accept(*opened.response,0));
    auto key = client.request(open->id.session,Kind::Key,0,100); key->payload = encodePayload(KeyMessage{KeyAction::Text,'a',true});
    auto pending = server.receive(*key,0,100); auto failed = server.fail(pending.work,ErrorCode::Backend,1);
    PendingDelivery retained; retained.append(L"已确认");
    assert(!client.accept(*failed.response,1) && !client.ready() && client.size() == 0);
    assert(!client.accept(*failed.response,1) && *retained.begin() == L"已确认");
}
int main() {
    boundedMailbox(); queueCancellationAndDeadline(); cancellationAndOwnerIsolation();
    failuresAndWorkerSurvival(); runningDeadlineAndShutdown(); parallelAndClientFailure();
    std::cout << "PASS: bounded workers/results, cancellation, deadlines, terminal errors, isolation and shutdown\n";
}
