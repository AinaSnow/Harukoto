#pragma once
#include "EnginePipe.h"

// Private, single-flight owner/host mailbox. Its unnamed section and events are
// handed off ONLY through the trusted launcher's explicit inherited handle list.
// This is not a public discovery channel and carries no input or candidate data.
namespace harukoto::engine::control {
constexpr size_t PacketSize = 256;
enum class Operation : uint32_t { Allocate = 1, Inspect };
enum class Code : uint32_t { Ok = 1, Full, Backend };
enum class State { Idle, Pending, Ready, Failed };
struct Request { uint64_t sequence; Operation operation; };
struct Response {
    Request request{};
    Code code = Code::Ok;
    uint64_t generation = 0;
    uint32_t limit = 0, listening = 0, connected = 0, retiring = 0, jobs = 0;
    std::wstring name;
};
using Packet = std::array<uint8_t,PacketSize>;
inline bool valid(Operation operation) { return operation == Operation::Allocate || operation == Operation::Inspect; }
inline bool valid(const Response &r) {
    if (!r.request.sequence || !valid(r.request.operation) || !r.generation || !r.limit || r.limit > 8 ||
            r.listening > r.limit || r.connected > r.limit || r.retiring > r.limit ||
            r.listening+r.connected+r.retiring > r.limit || r.jobs > 256) return false;
    if (r.request.operation == Operation::Inspect) return r.code == Code::Ok && r.name.empty();
    if (r.code == Code::Ok) return pipe::validName(r.name) && r.listening;
    return (r.code == Code::Full || r.code == Code::Backend) && r.name.empty();
}
inline Packet packet(Request request, const Response *response = nullptr) {
    if (!request.sequence || !valid(request.operation) || (response &&
            (!valid(*response) || response->request.sequence != request.sequence || response->request.operation != request.operation)))
        throw std::invalid_argument("invalid engine control packet");
    std::vector<uint8_t> bytes{'H','K','C','T'};
    put(bytes,1,2); put(bytes,response ? 2 : 1,2); put(bytes,request.sequence,8);
    put(bytes,static_cast<uint32_t>(request.operation),4);
    if (response) {
        put(bytes,static_cast<uint32_t>(response->code),4); put(bytes,response->generation,8);
        for (auto number : {response->limit,response->listening,response->connected,response->retiring,response->jobs}) put(bytes,number,4);
        put(bytes,response->name.size(),4);
        for (auto c : response->name) bytes.push_back(static_cast<uint8_t>(c));
    }
    Packet result{}; std::copy(bytes.begin(),bytes.end(),result.begin()); return result;
}
inline bool header(const Packet &bytes, uint16_t type, Request &request) {
    if (bytes[0] != 'H' || bytes[1] != 'K' || bytes[2] != 'C' || bytes[3] != 'T' ||
            get(bytes.data(),4,2) != 1 || get(bytes.data(),6,2) != type) return false;
    Request value{get(bytes.data(),8,8),static_cast<Operation>(get(bytes.data(),16,4))};
    if (!value.sequence || !valid(value.operation)) return false;
    request = value; return true;
}
inline bool decode(const Packet &bytes, Request &request) {
    Request value{};
    if (!header(bytes,1,value) || std::any_of(bytes.begin()+20,bytes.end(),[](auto b) { return b != 0; })) return false;
    request = value; return true;
}
inline bool decode(const Packet &bytes, Response &response) {
    Response value;
    if (!header(bytes,2,value.request)) return false;
    value.code = static_cast<Code>(get(bytes.data(),20,4)); value.generation = get(bytes.data(),24,8);
    value.limit = static_cast<uint32_t>(get(bytes.data(),32,4)); value.listening = static_cast<uint32_t>(get(bytes.data(),36,4));
    value.connected = static_cast<uint32_t>(get(bytes.data(),40,4)); value.retiring = static_cast<uint32_t>(get(bytes.data(),44,4));
    value.jobs = static_cast<uint32_t>(get(bytes.data(),48,4)); const auto length = get(bytes.data(),52,4);
    if (length > 96 || std::any_of(bytes.begin()+56+static_cast<size_t>(length),bytes.end(),[](auto b) { return b != 0; })) return false;
    for (size_t i=0;i<length;++i) value.name += static_cast<wchar_t>(bytes[56+i]);
    if (!valid(value)) return false;
    response = std::move(value); return true;
}
class Port {
    std::array<pipe::Handle,3> handles_;
    uint8_t *view_ = nullptr;
public:
    Port() {
        handles_[0].reset(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,PacketSize,nullptr));
        pipe::check(bool(handles_[0]));
        // Both events remain signalled across an outer WaitForMultipleObjects;
        // consuming an auto-reset request there would lose it before take().
        handles_[1].reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(handles_[1]));
        handles_[2].reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(handles_[2]));
        view_ = static_cast<uint8_t *>(MapViewOfFile(handles_[0].get(),FILE_MAP_READ | FILE_MAP_WRITE,0,0,PacketSize));
        pipe::check(view_ != nullptr);
    }
    explicit Port(std::array<pipe::Handle,3> handles) : handles_(std::move(handles)) {
        for (auto &handle : handles_) {
            if (!handle) throw std::invalid_argument("missing engine control handle");
            pipe::check(SetHandleInformation(handle.get(),HANDLE_FLAG_INHERIT,0));
        }
        view_ = static_cast<uint8_t *>(MapViewOfFile(handles_[0].get(),FILE_MAP_READ | FILE_MAP_WRITE,0,0,PacketSize));
        pipe::check(view_ != nullptr);
    }
    ~Port() { if (view_) UnmapViewOfFile(view_); }
    Port(const Port &) = delete;
    Port &operator=(const Port &) = delete;
    std::array<HANDLE,3> handles() const { return {handles_[0].get(),handles_[1].get(),handles_[2].get()}; }
    Packet read() const { Packet value; std::copy_n(view_,PacketSize,value.begin()); return value; }
    void write(const Packet &value) { std::copy(value.begin(),value.end(),view_); }
};
// Owner-thread only. A timeout permanently retires this mailbox: a delayed host
// response must never become an answer to a newer request or cause a replay.
class Client {
    Port port_;
    State state_ = State::Idle;
    pipe::Failure failure_ = pipe::Failure::None;
    DWORD error_ = ERROR_SUCCESS;
    uint64_t sequence_ = 0, deadline_ = 0, generation_ = 0;
    Request pending_{};
    std::optional<Response> response_;
    void fail(pipe::Failure failure, DWORD error = ERROR_SUCCESS) {
        state_ = State::Failed; failure_ = failure; error_ = error; deadline_ = 0; response_.reset();
    }
public:
    Client() = default;
    explicit Client(std::array<pipe::Handle,3> handles) : port_(std::move(handles)) {}
    std::array<HANDLE,3> handles() const { return port_.handles(); }
    bool begin(Operation operation, uint64_t time, uint64_t timeoutMs) {
        if (state_ != State::Idle) return false;
        if (!valid(operation) || !timeoutMs || timeoutMs > 300000 || time > UINT64_MAX-timeoutMs || sequence_ == UINT64_MAX)
            throw std::invalid_argument("invalid engine control request");
        pending_ = {++sequence_,operation};
        port_.write(packet(pending_)); deadline_ = time+timeoutMs; state_ = State::Pending;
        if (!ResetEvent(handles()[2]) || !SetEvent(handles()[1])) fail(pipe::Failure::Io,GetLastError());
        return state_ == State::Pending;
    }
    State poll(uint64_t time) {
        if (state_ != State::Pending) return state_;
        if (time >= deadline_) { fail(pipe::Failure::Deadline); return state_; }
        const auto status = WaitForSingleObject(handles()[2],0);
        if (status == WAIT_FAILED) fail(pipe::Failure::Io,GetLastError());
        else if (status == WAIT_OBJECT_0) {
            Response response;
            if (!decode(port_.read(),response) || response.request.sequence != pending_.sequence || response.request.operation != pending_.operation ||
                    (generation_ && response.generation != generation_))
                fail(pipe::Failure::Protocol,ERROR_INVALID_DATA);
            else { generation_ = response.generation; response_ = std::move(response); state_ = State::Ready; deadline_ = 0; }
        }
        return state_;
    }
    std::optional<Response> take(uint64_t time) {
        if (poll(time) != State::Ready) return {};
        auto response = std::move(response_); response_.reset(); state_ = State::Idle; return response;
    }
    void abandon() { if (state_ != State::Failed) fail(pipe::Failure::Closed); }
    HANDLE event() const { return state_ == State::Pending ? handles()[2] : nullptr; }
    uint64_t deadline() const { return deadline_; }
    State state() const { return state_; }
    pipe::Failure failure() const { return failure_; }
    DWORD error() const { return error_; }
};
// Host I/O-thread only. Event synchronization transfers exclusive access to the
// fixed packet; neither side may touch the section while the peer owns it.
class Server {
    Port port_;
    uint64_t sequence_ = 0;
    std::optional<Request> pending_;
public:
    explicit Server(std::array<pipe::Handle,3> handles) : port_(std::move(handles)) {}
    HANDLE event() const { return port_.handles()[1]; }
    std::optional<Request> take() {
        if (pending_) throw std::logic_error("unanswered engine control request");
        const auto status = WaitForSingleObject(event(),0);
        if (status == WAIT_TIMEOUT) return {};
        pipe::check(status == WAIT_OBJECT_0);
        pipe::check(ResetEvent(event()));
        Request request{};
        if (!decode(port_.read(),request) || sequence_ == UINT64_MAX || request.sequence != sequence_+1)
            throw std::invalid_argument("invalid engine control sequence");
        sequence_ = request.sequence; pending_ = request; return request;
    }
    void respond(const Response &response) {
        if (!pending_) throw std::logic_error("missing engine control request");
        port_.write(packet(*pending_,&response)); pending_.reset(); pipe::check(SetEvent(port_.handles()[2]));
    }
};
} // namespace harukoto::engine::control
