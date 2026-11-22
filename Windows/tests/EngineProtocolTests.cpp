#include "../EngineProtocol.h"
#include "../PendingDelivery.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace harukoto::engine;

static Frame reply(Frame request, Kind kind = Kind::Reply) { request.kind = kind; return request; }
static void connect(ClientGate &gate, uint64_t now = 0, uint64_t engine = 42) {
    auto hello = gate.connect(now, now + 100);
    assert(!gate.ready());
    auto ack = reply(hello, Kind::HelloAck); ack.id.engine = engine;
    assert(gate.acknowledge(ack, now));
    assert(!gate.acknowledge(ack, now));
    assert(gate.ready());
}
static uint64_t open(ClientGate &gate, uint64_t now = 0) {
    auto frame = gate.open(now, now + 100); assert(frame);
    assert(!gate.request(frame->id.session, Kind::Key, now, now + 100));
    assert(gate.accept(reply(*frame), now));
    return frame->id.session;
}
static void codec() {
    Frame f{Kind::Key, {1, 2, 3, 4, 5}, {0, 0xff, 0x7f}};
    auto bytes = encode(f);
    assert(bytes.size() == 59);
    // Fixed little-endian layout, independent of ABI/structure packing.
    assert(bytes[0] == 'H' && bytes[3] == 'P' && bytes[4] == ProtocolVersion && bytes[6] == 4);
    assert(bytes[8] == 3 && bytes[16] == 1 && bytes[24] == 2 && bytes[32] == 3 && bytes[40] == 4 && bytes[48] == 5);
    Frame out{Kind::Close, {9,9,9,9,9}, {8}}; size_t consumed = 999;
    for (size_t length = 0; length < bytes.size(); ++length) {
        assert(decode(bytes.data(), length, out, consumed) == Decode::Incomplete);
        assert(consumed == 0 && out.kind == Kind::Close && out.payload == std::vector<uint8_t>{8});
    }
    assert(decode(bytes.data(), bytes.size(), out, consumed) == Decode::Complete);
    assert(consumed == bytes.size() && out.id == f.id && out.kind == f.kind && out.payload == f.payload);
    auto doubled = bytes; doubled.insert(doubled.end(), bytes.begin(), bytes.end());
    assert(decode(doubled.data(), doubled.size(), out, consumed) == Decode::Complete && consumed == bytes.size());
    size_t second = 0;
    assert(decode(doubled.data() + consumed, doubled.size() - consumed, out, second) == Decode::Complete && second == consumed);
    for (auto offset : {0u, 4u, 6u, 10u, 12u, 16u, 24u, 32u, 40u, 48u}) {
        auto bad = bytes; bad[offset] = offset < 16 ? 0xff : 0;
        assert(decode(bad.data(), bad.size(), out, consumed) == Decode::Invalid && consumed == 0);
        assert(out.id == f.id && out.payload == f.payload);
    }
    assert(decode(nullptr, HeaderSize, out, consumed) == Decode::Invalid);
    f.payload.resize(MaxPayload, 0xab); bytes = encode(f);
    assert(decode(bytes.data(), bytes.size(), out, consumed) == Decode::Complete && out.payload == f.payload);
    f.payload.push_back(0);
    bool rejected = false; try { encode(f); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    std::mt19937 random(9217);
    for (int n = 0; n < 2000; ++n) {
        Frame sample{Kind::Reply, {uint64_t(random()) + 1, uint64_t(random()) + 1,
                     uint64_t(random()) + 1, uint64_t(random()) + 1, uint64_t(random()) + 1}, {}};
        sample.payload.resize(random() % 257);
        for (auto &b : sample.payload) b = static_cast<uint8_t>(random());
        auto wire = encode(sample);
        assert(decode(wire.data(), wire.size(), out, consumed) == Decode::Complete);
        assert(out.id == sample.id && out.payload == sample.payload);
        wire[4] = static_cast<uint8_t>(ProtocolVersion + 1);
        assert(decode(wire.data(), wire.size(), out, consumed) == Decode::Invalid);
    }
}
static void isolation() {
    ClientGate gate; connect(gate);
    const auto a = open(gate), b = open(gate);
    auto ka = gate.request(a, Kind::Key, 1, 50), kb = gate.request(b, Kind::Key, 1, 50);
    assert(ka && kb && ka->id.request != kb->id.request);
    assert(!gate.request(a, Kind::Select, 2, 50));
    auto wrong = reply(*ka); wrong.id.session = b;
    assert(!gate.accept(wrong, 2));
    for (auto member : {&Identity::connection, &Identity::engine, &Identity::composition, &Identity::request}) {
        wrong = reply(*ka); ++(wrong.id.*member); assert(!gate.accept(wrong, 2));
    }
    assert(!gate.accept(*ka, 2));
    assert(gate.accept(reply(*kb), 2)); // B can finish first.
    auto cancel = gate.cancel(a, 3, 60); assert(cancel && cancel->id.composition > ka->id.composition);
    assert(!gate.accept(reply(*ka), 4));
    assert(!gate.request(a, Kind::Key, 4, 60));
    auto repeated = gate.cancel(a, 4, 60); assert(repeated);
    assert(!gate.accept(reply(*cancel), 5));
    assert(gate.accept(reply(*repeated), 5));
    auto select = gate.request(a, Kind::Select, 6, 70); assert(select);
    PendingDelivery original, other;
    if (gate.accept(reply(*select), 7)) original.append(L"你好");
    assert(!gate.accept(reply(*select), 7));
    original.suspend();
    cancel = gate.cancel(a, 8, 70); assert(cancel);
    assert(!original.empty() && original.begin() == nullptr && other.empty());
    assert(gate.accept(reply(*cancel), 9));
    original.resume(); assert(*original.begin() == L"你好"); original.finish(true); assert(original.empty());
    auto pending = gate.request(a, Kind::Key, 10, 80); assert(pending);
    auto close = gate.close(a, 11, 80); assert(close);
    assert(!gate.accept(reply(*pending), 12));
    assert(!gate.cancel(a, 12, 80));
    assert(gate.accept(reply(*close), 12));
    assert(!gate.accept(reply(*close), 12));
    assert(!gate.request(a, Kind::Key, 12, 80));
    assert(gate.size() == 1 && open(gate, 12) != a);
    // Focus loss during Open invalidates its delayed reply as well.
    auto opening = gate.open(13, 80); assert(opening);
    auto earlyCancel = gate.cancel(opening->id.session, 14, 80); assert(earlyCancel);
    assert(!gate.accept(reply(*opening), 14));
    auto invalidAck = reply(*earlyCancel); invalidAck.payload = {1};
    assert(!gate.accept(invalidAck, 15));
    assert(gate.accept(reply(*earlyCancel), 15));
    assert(gate.request(opening->id.session, Kind::Key, 16, 80));
}
static void reconnectAndBounds() {
    ClientGate gate;
    assert(!gate.open(0, 10));
    auto hello = gate.connect(0, 10);
    auto ack = reply(hello, Kind::HelloAck); ack.id.engine = 9;
    auto wrong = ack; ++wrong.id.connection; assert(!gate.acknowledge(wrong, 1));
    wrong = ack; ++wrong.id.request; assert(!gate.acknowledge(wrong, 1));
    wrong = ack; wrong.id.session = 1; assert(!gate.acknowledge(wrong, 1));
    wrong = ack; wrong.id.engine = 0; assert(!gate.acknowledge(wrong, 1));
    wrong = ack; wrong.payload = {1}; assert(!gate.acknowledge(wrong, 1));
    assert(!gate.acknowledge(ack, 10) && !gate.ready());
    connect(gate, 10); const auto oldSession = open(gate, 10);
    auto key = gate.request(oldSession, Kind::Select, 11, 20); assert(key);
    assert(!gate.expire(19));
    assert(!gate.accept(reply(*key), 20) && !gate.ready() && gate.size() == 0);
    connect(gate, 21); const auto fresh = open(gate, 21);
    assert(fresh != oldSession);
    assert(!gate.accept(reply(*key), 22));
    assert(!gate.acknowledge(ack, 22));
    auto newer = gate.request(fresh, Kind::Key, 22, 30); assert(newer);
    assert(newer->id.connection != key->id.connection && newer->id.request > key->id.request);
    assert(!gate.accept(reply(*key, Kind::Error), 22) && gate.ready());
    auto failure = reply(*newer, Kind::Error);
    assert(!gate.accept(failure, 23) && gate.ready()); // empty error is malformed
    failure.payload = encodeError({ErrorCode::Backend});
    auto malformed = failure; malformed.payload.push_back(0);
    assert(!gate.accept(malformed, 23) && gate.ready());
    malformed = failure; malformed.payload[2] = 99;
    assert(!gate.accept(malformed, 23) && gate.ready());
    auto stale = failure; stale.id = key->id;
    assert(!gate.accept(stale, 23) && gate.ready());
    assert(!gate.accept(failure, 23) && !gate.ready());
    // Locally accepted text belongs to its retained context even after disconnect.
    PendingDelivery retained; retained.append(L"已确认"); retained.suspend();
    gate.disconnect(); assert(!retained.empty() && retained.begin() == nullptr);
    retained.resume(); assert(*retained.begin() == L"已确认"); retained.finish(true);
    connect(gate, 24);
    for (size_t n = 0; n < MaxSessions; ++n) open(gate, 24);
    assert(!gate.open(24, 40) && gate.size() == MaxSessions);
    gate.disconnect(); gate.disconnect(); assert(gate.size() == 0 && !gate.ready());
    connect(gate, 25); auto opening = gate.open(25, 40); assert(opening);
    auto closing = gate.close(opening->id.session, 26, 40); assert(closing);
    assert(!gate.accept(reply(*opening), 27));
    assert(gate.accept(reply(*closing), 27) && gate.size() == 0);
}
int main() {
    codec(); isolation(); reconnectAndBounds();
    std::cout << "PASS: bounded codec, ownership, cancellation, duplicates, reconnect and timeout\n";
}
