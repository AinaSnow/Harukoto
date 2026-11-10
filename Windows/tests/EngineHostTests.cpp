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
    explicit Runner(Executor::Query query = page, Host::Options options = {}, size_t count = 2) {
        for (size_t i=0;i<count;++i) names.push_back(ep::newName());
        service = std::async(std::launch::async,[this,query,options] {
            Host host(names,{},42,query,options); host.run(stop.get(),ready.get());
        });
        assert(WaitForSingleObject(ready.get(),Timeout) == WAIT_OBJECT_0);
    }
    ~Runner() { SetEvent(stop.get()); if (service.valid()) service.wait(); }
    void finish() { SetEvent(stop.get()); assert(service.wait_for(std::chrono::milliseconds(Timeout)) == std::future_status::ready); service.get(); }
};
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
static void limits() {
    const auto name = ep::newName();
    for (int variant=0;variant<5;++variant) {
        Host::Options options; std::vector<std::wstring> names{name};
        if (variant == 0) names.clear();
        if (variant == 1) names.push_back(name);
        if (variant == 2) options.replies = 0;
        if (variant == 3) options.queryMs = 0;
        if (variant == 4) names.resize(Host::MaxConnections+1,name);
        bool rejected = false;
        try { Host host(names,{},42,page,options); } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
}
int main() {
    try {
        limits(); std::cerr << "host: ownership and wake\n"; connectionOwnershipAndWake();
        std::cerr << "host: cancellation and disconnect\n"; cancelRunningAndDisconnect();
        std::cerr << "host: overload and failure\n"; overloadAndFailure();
        std::cerr << "host: expiry and malformed\n"; expiryAndMalformed();
        std::cout << "engine_host: bounded event loop, owner wake, cancellation, overload, failures and shutdown passed\n";
        return 0;
    } catch (const ep::Error &error) { std::cerr << "host failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
}
