#pragma once
#include "../EnginePipe.h"
#include "../EngineMessages.h"
#include <cassert>
#include <iostream>

namespace host_test {
using namespace harukoto::engine;
namespace ep = harukoto::engine::pipe;
constexpr uint64_t Timeout = 10000;
inline void wait(const std::vector<HANDLE> &events, uint64_t deadline) {
    const auto time = ep::now(); assert(time < deadline);
    if (events.empty()) return;
    const auto status = WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,static_cast<DWORD>(deadline-time));
    assert(status != WAIT_TIMEOUT && status != WAIT_FAILED);
}
struct Client {
    std::unique_ptr<ep::Channel> channel;
    ClientGate gate;
    uint64_t session = 0;
    CandidateSnapshot candidates;
    CandidatePage current;
    Frame last;
    Client(const std::wstring &name, HANDLE process) : Client(ep::Channel::connect(name,process)) {}
    explicit Client(std::unique_ptr<ep::Channel> connection) : channel(std::move(connection)) {
        assert(channel);
        const auto time = ep::now(); auto hello = gate.connect(time,time+Timeout);
        auto ack = exchange(hello); assert(gate.acknowledge(ack,ep::now()));
        auto open = gate.open(ep::now(),ep::now()+Timeout); assert(open); session = open->id.session;
        const auto opened = exchange(*open); assert(gate.accept(opened,ep::now()));
    }
    void send(const Frame &frame) {
        const auto deadline = ep::now()+Timeout;
        assert(channel->beginWrite(frame,ep::now(),deadline));
        for (;;) {
            channel->poll(ep::now()); assert(!channel->closed());
            if (channel->writeDone()) return;
            if (!channel->needsPoll()) wait({channel->writeEvent()},deadline);
        }
    }
    Frame receive() {
        const auto deadline = ep::now()+Timeout;
        assert(channel->beginRead(ep::now(),deadline));
        for (;;) {
            channel->poll(ep::now());
            if (auto frame = channel->take()) return *frame;
            assert(!channel->closed());
            if (!channel->needsPoll()) wait({channel->readEvent()},deadline);
        }
    }
    Frame exchange(const Frame &frame) { send(frame); return receive(); }
    Frame keyFrame(KeyAction action, uint8_t text = 0, bool hans = true) {
        auto request = gate.request(session,Kind::Key,ep::now(),ep::now()+Timeout); assert(request);
        request->payload = encodePayload(KeyMessage{action,text,hans}); return *request;
    }
    CandidatePage accept(const Frame &response) {
        CandidatePage page; assert(decodePayload(response.payload,page));
        assert(candidates.publish(response) && gate.accept(response,ep::now()));
        last = response; current = page; return page;
    }
    CandidatePage key(KeyAction action, uint8_t text = 0, bool hans = true) { return accept(exchange(keyFrame(action,text,hans))); }
    CandidatePage type(const std::string &input, bool hans = true) {
        for (unsigned char c : input) key(KeyAction::Text,c,hans); return current;
    }
    void cancel() {
        auto request = gate.cancel(session,ep::now(),ep::now()+Timeout); assert(request);
        const auto response = exchange(*request); assert(gate.accept(response,ep::now())); candidates.invalidate(); current = {};
    }
    Frame selection(const Candidate &word) {
        auto request = gate.request(session,Kind::Select,ep::now(),ep::now()+Timeout); assert(request);
        request->payload = encodePayload(Selection{last.id.request,word.id}); return *request;
    }
    Commit choose(const Candidate &word) {
        auto request = selection(word), response = exchange(request); Commit commit;
        assert(candidates.confirm(request,response,commit) && gate.accept(response,ep::now())); return commit;
    }
    void closed() {
        const auto deadline = ep::now()+Timeout;
        assert(channel->beginRead(ep::now(),deadline));
        while (!channel->closed()) {
            channel->poll(ep::now()); assert(!channel->take());
            if (!channel->closed() && !channel->needsPoll()) wait({channel->readEvent()},deadline);
        }
        gate.disconnect();
    }
};
inline Candidate find(const CandidatePage &page, const std::string &text) {
    for (const auto &word : page.words) if (word.text == text) return word;
    std::cerr << "test input=" << page.input << " expected=" << text << " actual:";
    for (const auto &word : page.words) std::cerr << ' ' << word.text;
    std::cerr << '\n';
    assert(false && "missing expected candidate"); return {};
}
} // namespace host_test
