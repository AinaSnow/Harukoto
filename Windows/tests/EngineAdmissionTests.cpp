#include "../EngineAdmission.h"
#include <cassert>
#include <iostream>

using namespace harukoto::engine;
namespace ea = harukoto::engine::admission;
namespace ep = harukoto::engine::pipe;
static std::array<ep::Handle,3> duplicate(const ea::Client &client) {
    std::array<ep::Handle,3> result; const auto source = client.handles();
    for (size_t i=0;i<source.size();++i) {
        HANDLE raw = nullptr; ep::check(DuplicateHandle(GetCurrentProcess(),source[i],GetCurrentProcess(),&raw,0,FALSE,DUPLICATE_SAME_ACCESS));
        result[i].reset(raw);
    }
    return result;
}
static void write(ea::Client &client, const control::Packet &packet) {
    auto *view = static_cast<uint8_t *>(MapViewOfFile(client.handles()[0],FILE_MAP_WRITE,0,0,control::PacketSize)); assert(view);
    std::copy(packet.begin(),packet.end(),view); UnmapViewOfFile(view);
}
static void codec() {
    ea::Request request{1,9,{20,24,28,32}}, decoded;
    assert(ea::decode(ea::packet(request),decoded) && decoded.id == 9 && decoded.handles == request.handles);
    for (size_t offset : {size_t(0),size_t(4),size_t(6),size_t(56),size_t(255)}) {
        auto bad = ea::packet(request); bad[offset] = 255; decoded.id = 77;
        assert(!ea::decode(bad,decoded) && decoded.id == 77);
    }
    auto invalid = request; invalid.handles[1] = invalid.handles[0]; assert(!ea::valid(invalid));
    invalid = request; invalid.handles[1] = 0; assert(!ea::valid(invalid));
    invalid.handles[1] = UINT64_MAX; assert(!ea::valid(invalid));
    invalid = request; invalid.id = 0; assert(!ea::valid(invalid));
    ea::Response response;
    for (auto code : {ea::Code::Ok,ea::Code::Full,ea::Code::Closed}) {
        const auto bytes = ea::packet(ea::Response{1,9,code});
        assert(ea::decode(bytes,response) && response.code == code && response.sequence == 1 && response.id == 9);
        for (size_t offset : {size_t(0),size_t(4),size_t(6),size_t(24),size_t(28),size_t(255)}) {
            auto bad = bytes; bad[offset] = 255; response.id = 77;
            assert(!ea::decode(bad,response) && response.id == 77);
        }
    }
}
static void exchange() {
    ea::Client client; ea::Server server(duplicate(client),8);
    std::array<ep::Handle,4> objects; std::array<uint64_t,4> handles{};
    for (size_t i=0;i<objects.size();++i) {
        objects[i].reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); assert(objects[i]);
        handles[i] = reinterpret_cast<uintptr_t>(objects[i].get());
    }
    for (auto handle : client.handles()) {
        DWORD flags = 0; ep::check(GetHandleInformation(handle,&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
    }
    for (uint64_t id=9;id<12;++id) {
        assert(client.begin(id,handles,100,50));
        assert(!client.begin(id+1,handles,100,50));
        assert(WaitForSingleObject(server.event(),0) == WAIT_OBJECT_0); // Outer waits cannot consume it.
        auto request = server.take(); assert(request && request->id == id && request->sequence == id-8);
        const auto code = id == 10 ? ea::Code::Full : ea::Code::Ok; server.respond(code);
        assert(client.poll(149) == ea::State::Ready && !client.begin(id+1,handles,149,50));
        auto response = client.take(149); assert(response && response->id == id && response->code == code && !client.take(149));
    }
    // A queued response cannot extend the exact deadline, and cannot replay.
    assert(client.begin(12,handles,100,50)); assert(server.take()); server.respond(ea::Code::Ok);
    assert(client.poll(150) == ea::State::Failed && client.failure() == ep::Failure::Deadline && !client.take(150));
    assert(!client.begin(13,handles,151,50) && !client.event()); client.abandon();
    assert(client.failure() == ep::Failure::Deadline);
    for (bool wrongSequence : {false,true}) {
        ea::Client bad; assert(bad.begin(9,handles,100,50));
        write(bad,ea::packet(ea::Response{wrongSequence ? 2u : 1u,wrongSequence ? 9u : 10u,ea::Code::Ok}));
        ep::check(SetEvent(bad.handles()[2]));
        assert(bad.poll(101) == ea::State::Failed && bad.failure() == ep::Failure::Protocol);
    }
    ea::Client closed; assert(closed.begin(9,handles,100,50)); closed.abandon();
    assert(closed.poll(101) == ea::State::Failed && closed.failure() == ep::Failure::Closed && !closed.take(101));
    for (auto request : {ea::Request{2,9,handles},ea::Request{1,8,handles}}) {
        ea::Client bad; ea::Server guarded(duplicate(bad),8); write(bad,ea::packet(request));
        ep::check(SetEvent(bad.handles()[1])); bool rejected = false;
        try { guarded.take(); } catch (const std::invalid_argument &) { rejected = true; } assert(rejected);
    }
}
static void transfer() {
    control::Client port; ep::Handle revoked(CreateEventW(nullptr,TRUE,FALSE,nullptr)); assert(revoked);
    const auto source = port.handles(); std::array<uint64_t,4> remote{};
    {
        ea::Transfer draft(GetCurrentProcess(),{source[0],source[1],source[2],revoked.get()}); remote = draft.handles();
        for (auto value : remote) {
            DWORD flags = 0; ep::check(GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        }
    }
    for (auto value : remote) {
        DWORD flags = 0; assert(!GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),&flags) && GetLastError() == ERROR_INVALID_HANDLE);
    }
    {
        ea::Transfer published(GetCurrentProcess(),{source[0],source[1],source[2],revoked.get()}); remote = published.handles(); published.release();
    }
    for (auto value : remote) { ep::Handle adopted(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value))); DWORD flags = 0; ep::check(GetHandleInformation(adopted.get(),&flags)); }
    DWORD before = 0, after = 0; ep::check(GetProcessHandleCount(GetCurrentProcess(),&before));
    bool failed = false;
    try { ea::Transfer partial(GetCurrentProcess(),{source[0],source[1],nullptr,revoked.get()}); }
    catch (const ep::Error &) { failed = true; }
    ep::check(GetProcessHandleCount(GetCurrentProcess(),&after)); assert(failed && after == before);
    ep::check(SetEvent(revoked.get())); // Closing remote duplicates never closes the sources.
}
int main() {
    codec(); exchange(); transfer();
    std::cout << "engine_admission: strict private packets, correlation/deadlines, single-flight and remote-handle ownership passed\n";
}
