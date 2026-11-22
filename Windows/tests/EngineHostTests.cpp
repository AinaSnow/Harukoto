#include "../EngineHost.h"
#include "EngineHostTestClient.h"
#include <future>
#include <iostream>

using namespace harukoto::engine;
using namespace host_test;
static CandidatePage page(const ServerConnection::Ticket &work) {
    CandidatePage value{work->input,false,{}};
    if (!work->input.empty()) value.words.push_back({1,static_cast<uint32_t>(work->input.size()),work->input,""});
    return value;
}
struct Latch {
    ep::Handle entered{CreateEventW(nullptr,TRUE,FALSE,nullptr)}, release{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    void block() { ep::check(SetEvent(entered.get())); assert(WaitForSingleObject(release.get(),Timeout) == WAIT_OBJECT_0); }
    void wait() { assert(WaitForSingleObject(entered.get(),Timeout) == WAIT_OBJECT_0); }
    void open() { ep::check(SetEvent(release.get())); }
};
struct Runner {
    ep::Handle ready{CreateEventW(nullptr,TRUE,FALSE,nullptr)}, stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::vector<std::wstring> names;
    std::future<void> service;
    explicit Runner(Executor::Query query = page, Host::Options options = {}, size_t count = 2, control::Client *owner = nullptr) {
        for (size_t i=0;i<count;++i) names.push_back(ep::newName());
        std::unique_ptr<control::Server> endpointControl;
        if (owner) {
            std::array<ep::Handle,3> handles; const auto source = owner->handles();
            for (size_t i=0;i<source.size();++i) {
                HANDLE raw = nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),source[i],GetCurrentProcess(),&raw,0,FALSE,DUPLICATE_SAME_ACCESS));
                handles[i].reset(raw);
            }
            endpointControl = std::make_unique<control::Server>(std::move(handles));
        }
        service = std::async(std::launch::async,[this,query,options,endpointControl = std::move(endpointControl)]() mutable {
            Host host(names,{},42,query,options,{},std::move(endpointControl)); host.run(stop.get(),ready.get());
        });
        assert(WaitForSingleObject(ready.get(),Timeout) == WAIT_OBJECT_0);
    }
    ~Runner() { SetEvent(stop.get()); if (service.valid()) service.wait(); }
    void finish() { SetEvent(stop.get()); assert(service.wait_for(std::chrono::milliseconds(Timeout)) == std::future_status::ready); service.get(); }
};
static control::Response request(control::Client &owner, control::Operation operation) {
    assert(owner.begin(operation,ep::now(),Timeout));
    wait({owner.event()},owner.deadline());
    auto response = owner.take(ep::now()); assert(response); return std::move(*response);
}
template<class Predicate> static control::Response inspectUntil(control::Client &owner, Predicate predicate) {
    const auto deadline = ep::now()+Timeout;
    for (;;) {
        assert(ep::now() < deadline);
        auto response = request(owner,control::Operation::Inspect);
        if (predicate(response)) return response;
    }
}
static void dynamicOwnership() {
    control::Client owner; Host::Options options; options.connections = 2;
    Runner runner(page,options,0,&owner);
    const auto empty = request(owner,control::Operation::Inspect);
    assert(empty.generation == 42 && empty.limit == 2 && !empty.listening && !empty.connected && !empty.retiring && !empty.jobs);
    const auto first = request(owner,control::Operation::Allocate);
    assert(first.code == control::Code::Ok && first.listening == 1);
    Client client(first.name,GetCurrentProcess()); assert(client.key(KeyAction::Text,'x').input == "x");
    const auto second = request(owner,control::Operation::Allocate);
    assert(second.code == control::Code::Ok && second.name != first.name && second.listening == 1 && second.connected == 1);
    Client survivor(second.name,GetCurrentProcess());
    for (int i=0;i<5;++i) {
        const auto full = request(owner,control::Operation::Allocate);
        assert(full.code == control::Code::Full && full.name.empty() && full.connected == 2 && !full.jobs);
    }
    client.channel.reset();
    inspectUntil(owner,[](const auto &r) { return r.connected == 1 && !r.retiring && !r.listening; });
    const auto third = request(owner,control::Operation::Allocate);
    assert(third.code == control::Code::Ok && third.name != first.name && third.name != second.name);
    { Client replacement(third.name,GetCurrentProcess()); assert(replacement.key(KeyAction::Refresh).input.empty()); }
    survivor.channel.reset();
    inspectUntil(owner,[](const auto &r) { return !r.connected && !r.retiring && !r.listening; });
    assert(runner.service.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
    const auto later = request(owner,control::Operation::Allocate);
    Client renewed(later.name,GetCurrentProcess()); assert(renewed.key(KeyAction::Text,'z').input == "z");
    runner.finish(); renewed.closed();
}
static void dynamicRetirementAndExpiry() {
    control::Client owner; Latch latch;
    Host::Options options; options.connections = 2; options.threads = 1;
    Runner runner([&](const auto &work) { if (work->input == "a") latch.block(); return page(work); },options,0,&owner);
    const auto first = request(owner,control::Operation::Allocate); Client old(first.name,GetCurrentProcess());
    const auto second = request(owner,control::Operation::Allocate); Client survivor(second.name,GetCurrentProcess());
    old.send(old.keyFrame(KeyAction::Text,'a')); latch.wait(); old.channel.reset();
    const auto retired = inspectUntil(owner,[](const auto &r) { return r.retiring == 1; });
    assert(retired.jobs == 1 && retired.connected == 1);
    assert(request(owner,control::Operation::Allocate).code == control::Code::Full); // Still owns its exact task.
    latch.open(); inspectUntil(owner,[](const auto &r) { return !r.retiring && !r.jobs; });
    const auto replacement = request(owner,control::Operation::Allocate);
    assert(replacement.code == control::Code::Ok && replacement.name != first.name);
    Client fresh(replacement.name,GetCurrentProcess()); assert(fresh.key(KeyAction::Refresh).input.empty());
    assert(survivor.key(KeyAction::Text,'x').input == "x"); runner.finish();

    control::Client idleOwner; options.listenMs = 25; options.connections = 1;
    Runner idle(page,options,0,&idleOwner);
    const auto unused = request(idleOwner,control::Operation::Allocate); assert(unused.listening == 1);
    inspectUntil(idleOwner,[](const auto &r) { return !r.listening && !r.retiring; });
    const auto next = request(idleOwner,control::Operation::Allocate); assert(next.code == control::Code::Ok && next.name != unused.name);
    idle.finish();
}
static void connectionOwnershipAndWake() {
    Runner runner;
    Client first(runner.names[0],GetCurrentProcess()), second(runner.names[1],GetCurrentProcess());
    assert(first.session == second.session);
    // No further I/O after dispatch: the worker notification must wake the owner.
    first.send(first.keyFrame(KeyAction::Text,'a')); second.send(second.keyFrame(KeyAction::Text,'b'));
    assert(first.accept(first.receive()).input == "a"); assert(second.accept(second.receive()).input == "b");
    const auto word = find(first.current,"a"); const auto selection = first.selection(word);
    const auto commit = first.exchange(selection); Commit decoded;
    assert(decodePayload(commit.payload,decoded) && decoded.text == "a");
    assert(first.gate.accept(commit,ep::now()));
    const auto replay = first.exchange(selection); assert(replay.id == commit.id && replay.payload == commit.payload);
    assert(!first.gate.accept(replay,ep::now()));
    first.cancel(); assert(first.key(KeyAction::Refresh).input.empty());
    assert(second.key(KeyAction::Refresh).input == "b");
    first.channel.reset(); assert(second.key(KeyAction::Text,'c').input == "bc");
    runner.finish(); second.closed();
}
static void cancelRunningAndDisconnect() {
    Latch latch;
    Host::Options options; options.threads = 1; options.capacity = 2;
    Runner runner([&](const auto &work) { if (work->input == "a") latch.block(); return page(work); },options);
    Client first(runner.names[0],GetCurrentProcess()), second(runner.names[1],GetCurrentProcess());
    first.send(first.keyFrame(KeyAction::Text,'a')); latch.wait();
    first.cancel(); first.send(first.keyFrame(KeyAction::Text,'b'));
    latch.open(); assert(first.accept(first.receive()).input == "b"); // No canceled 'a' reply precedes this.
    assert(second.key(KeyAction::Text,'z').input == "z");
    runner.finish();

    Latch abandoned;
    Runner disconnect([&](const auto &work) { if (work->input == "a") abandoned.block(); return page(work); },options);
    Client old(disconnect.names[0],GetCurrentProcess()), survivor(disconnect.names[1],GetCurrentProcess());
    old.send(old.keyFrame(KeyAction::Text,'a')); abandoned.wait(); old.channel.reset();
    survivor.send(survivor.keyFrame(KeyAction::Text,'x')); abandoned.open();
    assert(survivor.accept(survivor.receive()).input == "x"); disconnect.finish();
}
static void overloadAndFailure() {
    Latch latch;
    Host::Options options; options.threads = 1; options.capacity = 1;
    Runner runner([&](const auto &work) { latch.block(); return page(work); },options);
    Client first(runner.names[0],GetCurrentProcess()), second(runner.names[1],GetCurrentProcess());
    first.send(first.keyFrame(KeyAction::Text,'a')); latch.wait();
    first.cancel(); // Canceled work still occupies the global slot until return/reclamation.
    second.send(second.keyFrame(KeyAction::Text,'b')); const auto response = second.receive(); ErrorMessage error;
    assert(response.kind == Kind::Error && decodePayload(response.payload,error) && error.code == ErrorCode::Overloaded);
    assert(!second.gate.accept(response,ep::now()) && !second.gate.ready()); second.channel.reset();
    latch.open(); runner.finish();

    Runner broken([](const auto &work) { if (work->input == "a") throw std::runtime_error("private backend details"); return page(work); });
    Client failed(broken.names[0],GetCurrentProcess()), survivor(broken.names[1],GetCurrentProcess());
    failed.send(failed.keyFrame(KeyAction::Text,'a')); const auto failure = failed.receive();
    assert(failure.kind == Kind::Error && failure.payload.size() == 4 && decodePayload(failure.payload,error) && error.code == ErrorCode::Backend);
    failed.channel.reset(); assert(survivor.key(KeyAction::Text,'b').input == "b"); broken.finish();
}
static void expiryAndMalformed() {
    Latch latch; Host::Options options; options.queryMs = 50;
    Runner runner([&](const auto &work) { latch.block(); return page(work); },options);
    Client client(runner.names[0],GetCurrentProcess());
    client.send(client.keyFrame(KeyAction::Text,'a')); latch.wait(); client.closed(); latch.open(); runner.finish();

    Runner malformed;
    Client bad(malformed.names[0],GetCurrentProcess()), good(malformed.names[1],GetCurrentProcess());
    auto frame = bad.keyFrame(KeyAction::Text,'x'); frame.payload = {1}; bad.send(frame); bad.closed();
    assert(good.key(KeyAction::Text,'y').input == "y"); malformed.finish();
}
// WIN-03 step 7b: LRU idle endpoint reclamation when all slots are occupied,
// while strictly preserving active endpoints and any endpoint with in-flight work.
static void idleEviction() {
    control::Client owner;
    Host::Options options; options.connections = 2; options.threads = 2; options.idleEvictMs = 100;
    Runner runner(page,options,0,&owner);
    const auto first = request(owner,control::Operation::Allocate);
    assert(first.code == control::Code::Ok);
    Client oldest(first.name,GetCurrentProcess()); assert(oldest.key(KeyAction::Text,'a').input == "a");
    Sleep(50);
    const auto second = request(owner,control::Operation::Allocate);
    assert(second.code == control::Code::Ok);
    Client newer(second.name,GetCurrentProcess()); assert(newer.key(KeyAction::Text,'b').input == "b");
    // Before idleEvictMs has elapsed, Allocate must still return Full.
    assert(request(owner,control::Operation::Allocate).code == control::Code::Full);
    // Keep newer active while oldest ages past idleEvictMs.
    Sleep(80);
    assert(newer.key(KeyAction::Text,'c').input == "bc");
    // Allocate evicts oldest (LRU) while newer stays completely alive.
    const auto third = request(owner,control::Operation::Allocate);
    assert(third.code == control::Code::Ok && third.name != first.name && third.name != second.name);
    oldest.closed();
    Client replacement(third.name,GetCurrentProcess());
    assert(replacement.key(KeyAction::Text,'d').input == "d");
    assert(newer.key(KeyAction::Text,'e').input == "bce");
    newer.channel.reset(); replacement.channel.reset();
    runner.finish();

    control::Client busyOwner; Latch busyLatch;
    Host::Options busyOpts; busyOpts.connections = 1; busyOpts.threads = 1; busyOpts.idleEvictMs = 40;
    Runner busyRunner([&](const auto &work) { if (work->input == "x") busyLatch.block(); return page(work); },busyOpts,0,&busyOwner);
    const auto only = request(busyOwner,control::Operation::Allocate);
    Client active(only.name,GetCurrentProcess());
    active.send(active.keyFrame(KeyAction::Text,'x')); busyLatch.wait();
    Sleep(80);
    assert(request(busyOwner,control::Operation::Allocate).code == control::Code::Full);
    busyLatch.open();
    assert(active.accept(active.receive()).input == "x");
    Sleep(80);
    // Once idle past idleEvictMs with no in-flight job, the slot is reclaimed cleanly.
    const auto next = request(busyOwner,control::Operation::Allocate);
    assert(next.code == control::Code::Ok && next.name != only.name);
    active.closed();
    Client fresh(next.name,GetCurrentProcess());
    assert(fresh.key(KeyAction::Text,'z').input == "z");
    busyRunner.finish();
}
static void limits() {
    const auto name = ep::newName();
    for (int variant=0;variant<8;++variant) {
        Host::Options options; std::vector<std::wstring> names{name};
        if (variant == 0) names.clear();
        if (variant == 1) names.push_back(name);
        if (variant == 2) options.replies = 0;
        if (variant == 3) options.queryMs = 0;
        if (variant == 4) names.resize(Host::MaxConnections+1,name);
        if (variant == 5) options.connections = 0;
        if (variant == 6) options.connections = Host::MaxConnections+1;
        if (variant == 7) { options.connections = 1; names.push_back(ep::newName()); }
        bool rejected = false;
        try { Host host(names,{},42,page,options); } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
}
int main() {
    try {
        limits(); std::cerr << "host: ownership and wake\n"; connectionOwnershipAndWake();
        std::cerr << "host: dynamic owner and retirement\n"; dynamicOwnership(); dynamicRetirementAndExpiry();
        std::cerr << "host: cancellation and disconnect\n"; cancelRunningAndDisconnect();
        std::cerr << "host: overload and failure\n"; overloadAndFailure();
        std::cerr << "host: expiry and malformed\n"; expiryAndMalformed();
        std::cerr << "host: step 7b idle LRU eviction\n"; idleEviction();
        std::cout << "engine_host: bounded event loop, owner wake, cancellation, overload, failures and shutdown passed\n";
        return 0;
    } catch (const ep::Error &error) { std::cerr << "host failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
}
