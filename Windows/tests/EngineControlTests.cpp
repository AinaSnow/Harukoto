#include "../EngineControl.h"
#include <cassert>
#include <iostream>

using namespace harukoto::engine;
namespace ec = harukoto::engine::control;
namespace ep = harukoto::engine::pipe;
static std::array<ep::Handle,3> duplicate(const ec::Client &client) {
    std::array<ep::Handle,3> handles; const auto source = client.handles();
    for (size_t i=0;i<source.size();++i) {
        HANDLE raw = nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),source[i],GetCurrentProcess(),&raw,0,FALSE,DUPLICATE_SAME_ACCESS));
        handles[i].reset(raw);
    }
    return handles;
}
static ec::Response answer(ec::Request request) {
    ec::Response response; response.request = request; response.generation = 42; response.limit = 2;
    if (request.operation == ec::Operation::Allocate) { response.name = ep::newName(); response.listening = 1; }
    return response;
}
static void codec() {
    ec::Request request{1,ec::Operation::Allocate}, decoded{};
    assert(ec::decode(ec::packet(request),decoded) && decoded.sequence == 1 && decoded.operation == request.operation);
    auto response = answer(request); ec::Response result;
    assert(ec::decode(ec::packet(request,&response),result) && result.name == response.name && result.generation == 42);
    for (size_t offset : {size_t(0),size_t(4),size_t(6),size_t(16),size_t(255)}) {
        auto bad = ec::packet(request); bad[offset] = 255; decoded.sequence = 99;
        assert(!ec::decode(bad,decoded) && decoded.sequence == 99);
    }
    for (size_t offset : {size_t(0),size_t(4),size_t(6),size_t(16),size_t(20),size_t(32),size_t(36),size_t(48),size_t(52),size_t(56),size_t(255)}) {
        auto bad = ec::packet(request,&response); bad[offset] = 255; result.generation = 99;
        // jobs=255 is still legal; make the next byte nonzero as well.
        if (offset == 48) bad[49] = 1;
        assert(!ec::decode(bad,result) && result.generation == 99);
    }
    response.code = ec::Code::Full; response.name.clear(); response.listening = 0;
    assert(ec::decode(ec::packet(request,&response),result) && result.code == ec::Code::Full);
    response.request.operation = ec::Operation::Inspect; assert(!ec::valid(response));
    response.code = ec::Code::Ok; assert(ec::valid(response));
}
static void exchangeAndTimeout() {
    ec::Client client; ec::Server server(duplicate(client));
    for (const auto handle : client.handles()) {
        DWORD flags = 0; ep::check(GetHandleInformation(handle,&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
    }
    assert(!server.take() && !client.take(0) && !client.event());
    for (uint64_t i=1;i<=20;++i) {
        assert(client.begin(ec::Operation::Inspect,100,500)); assert(client.deadline() == 600);
        assert(!client.begin(ec::Operation::Allocate,101,500));
        assert(WaitForSingleObject(server.event(),0) == WAIT_OBJECT_0); // Outer event-loop wait must not consume the request.
        const auto request = server.take(); assert(request && request->sequence == i);
        assert(WaitForSingleObject(server.event(),0) == WAIT_TIMEOUT);
        assert(client.poll(102) == ec::State::Pending);
        server.respond(answer(*request));
        assert(client.poll(103) == ec::State::Ready && !client.event() && !client.deadline());
        assert(!client.begin(ec::Operation::Allocate,104,500)); // Unconsumed response is capacity.
        const auto response = client.take(104); assert(response && response->request.sequence == i);
        assert(!client.take(105) && client.state() == ec::State::Idle);
    }
    assert(client.begin(ec::Operation::Allocate,200,50)); const auto request = server.take(); assert(request);
    server.respond(answer(*request)); // Already signalled at the exact deadline still expires.
    assert(client.poll(250) == ec::State::Failed && client.failure() == ep::Failure::Deadline);
    assert(!client.take(251) && !client.event() && !client.deadline() && !client.begin(ec::Operation::Allocate,252,500));

    ec::Client late; ec::Server delayed(duplicate(late));
    assert(late.begin(ec::Operation::Allocate,0,10)); const auto pending = delayed.take(); assert(pending);
    assert(late.poll(10) == ec::State::Failed); delayed.respond(answer(*pending));
    assert(!late.take(11) && !late.begin(ec::Operation::Inspect,12,500));
}
static void corruptAndStale() {
    for (int variant=0;variant<5;++variant) {
        ec::Client client; ec::Port raw(duplicate(client));
        assert(client.begin(ec::Operation::Inspect,0,100));
        assert(WaitForSingleObject(raw.handles()[1],0) == WAIT_OBJECT_0);
        ec::Request request{}; assert(ec::decode(raw.read(),request));
        auto response = answer(request);
        if (variant == 0) ++response.request.sequence;
        if (variant == 1) { response.request.operation = ec::Operation::Allocate; response.name = ep::newName(); response.listening = 1; }
        if (variant == 2) {
            raw.write(ec::packet(response.request,&response)); ep::check(SetEvent(raw.handles()[2]));
            assert(client.take(1)); assert(client.begin(ec::Operation::Inspect,2,100));
            assert(WaitForSingleObject(raw.handles()[1],0) == WAIT_OBJECT_0);
            assert(ec::decode(raw.read(),request)); response = answer(request); response.generation = 43;
        }
        auto bytes = ec::packet(response.request,&response);
        if (variant == 3) bytes[255] = 1;
        if (variant == 4) bytes[4] = 2;
        raw.write(bytes); ep::check(SetEvent(raw.handles()[2]));
        assert(client.poll(3) == ec::State::Failed && client.failure() == ep::Failure::Protocol);
        assert(!client.take(4) && !client.begin(ec::Operation::Inspect,5,100));
    }
    ec::Client client; ec::Port raw(duplicate(client)); ec::Server server(duplicate(client));
    assert(client.begin(ec::Operation::Inspect,0,100));
    auto bytes = raw.read(); bytes[8] = 2; raw.write(bytes);
    bool rejected = false;
    try { server.take(); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    client.abandon(); assert(client.state() == ec::State::Failed && !client.take(1));
}
int main() {
    codec(); exchangeAndTimeout(); corruptAndStale();
    std::cout << "engine_control: bounded private mailbox, sequence/generation, stale response and absolute deadline passed\n";
}
