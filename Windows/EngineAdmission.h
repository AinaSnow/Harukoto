#pragma once
#include "EngineControl.h"

// Trusted launcher -> retained owner object only. Handle values are already
// duplicated into that owner before publication; never accept public values.
namespace harukoto::engine::admission {
constexpr size_t Limit = 8;
using State = control::State;
enum class Code : uint32_t { Ok = 1, Full, Closed };
struct Request { uint64_t sequence = 0, id = 0; std::array<uint64_t,4> handles{}; };
struct Response { uint64_t sequence = 0, id = 0; Code code = Code::Ok; };
inline bool valid(Code code) { return code == Code::Ok || code == Code::Full || code == Code::Closed; }
inline bool valid(const Request &request) {
    if (!request.sequence || !request.id) return false;
    for (size_t i=0;i<request.handles.size();++i) {
        const auto handle = request.handles[i];
        if (!handle || handle >= std::numeric_limits<uintptr_t>::max()) return false;
        for (size_t j=0;j<i;++j) if (handle == request.handles[j]) return false;
    }
    return true;
}
inline control::Packet packet(uint16_t type, uint64_t sequence, uint64_t id) {
    std::vector<uint8_t> bytes{'H','K','R','G'};
    put(bytes,1,2); put(bytes,type,2); put(bytes,sequence,8); put(bytes,id,8);
    control::Packet result{}; std::copy(bytes.begin(),bytes.end(),result.begin()); return result;
}
inline control::Packet packet(const Request &request) {
    if (!valid(request)) throw std::invalid_argument("invalid engine admission request");
    auto result = packet(1,request.sequence,request.id);
    std::vector<uint8_t> handles; for (auto value : request.handles) put(handles,value,8);
    std::copy(handles.begin(),handles.end(),result.begin()+24); return result;
}
inline control::Packet packet(const Response &response) {
    if (!response.sequence || !response.id || !valid(response.code)) throw std::invalid_argument("invalid engine admission response");
    auto result = packet(2,response.sequence,response.id);
    std::vector<uint8_t> code; put(code,static_cast<uint32_t>(response.code),4);
    std::copy(code.begin(),code.end(),result.begin()+24); return result;
}
inline bool header(const control::Packet &bytes, uint16_t type) {
    return bytes[0] == 'H' && bytes[1] == 'K' && bytes[2] == 'R' && bytes[3] == 'G' &&
        get(bytes.data(),4,2) == 1 && get(bytes.data(),6,2) == type;
}
inline bool decode(const control::Packet &bytes, Request &request) {
    Request value{get(bytes.data(),8,8),get(bytes.data(),16,8)};
    for (size_t i=0;i<value.handles.size();++i) value.handles[i] = get(bytes.data(),24+i*8,8);
    if (!header(bytes,1) || !valid(value) || std::any_of(bytes.begin()+56,bytes.end(),[](auto b) { return b != 0; })) return false;
    request = value; return true;
}
inline bool decode(const control::Packet &bytes, Response &response) {
    Response value{get(bytes.data(),8,8),get(bytes.data(),16,8),static_cast<Code>(get(bytes.data(),24,4))};
    if (!header(bytes,2) || !value.sequence || !value.id || !valid(value.code) ||
            std::any_of(bytes.begin()+28,bytes.end(),[](auto b) { return b != 0; })) return false;
    response = value; return true;
}
class Client {
    control::Port port_;
    State state_ = State::Idle;
    pipe::Failure failure_ = pipe::Failure::None;
    DWORD error_ = ERROR_SUCCESS;
    uint64_t sequence_ = 0, id_ = 0, deadline_ = 0;
    std::optional<Response> response_;
    void fail(pipe::Failure failure, DWORD error = ERROR_SUCCESS) {
        state_ = State::Failed; failure_ = failure; error_ = error; deadline_ = 0; response_.reset();
    }
public:
    Client() = default;
    explicit Client(std::array<pipe::Handle,3> handles) : port_(std::move(handles)) {}
    std::array<HANDLE,3> handles() const { return port_.handles(); }
    bool begin(uint64_t id, std::array<uint64_t,4> incoming, uint64_t time, uint64_t timeoutMs) {
        if (state_ != State::Idle) return false;
        if (!timeoutMs || timeoutMs > 300000 || time > UINT64_MAX-timeoutMs || sequence_ == UINT64_MAX || id <= id_)
            throw std::invalid_argument("invalid engine admission request");
        const auto bytes = packet(Request{sequence_+1,id,incoming});
        port_.write(bytes); ++sequence_; id_ = id; deadline_ = time+timeoutMs; state_ = State::Pending;
        if (!ResetEvent(handles()[2]) || !SetEvent(handles()[1])) fail(pipe::Failure::Io,GetLastError());
        return state_ == State::Pending; // Only true transfers the remote handles.
    }
    State poll(uint64_t time) {
        if (state_ != State::Pending) return state_;
        if (time >= deadline_) { fail(pipe::Failure::Deadline); return state_; }
        const auto status = WaitForSingleObject(handles()[2],0);
        if (status == WAIT_FAILED) fail(pipe::Failure::Io,GetLastError());
        else if (status == WAIT_OBJECT_0) {
            Response response;
            if (!decode(port_.read(),response) || response.sequence != sequence_ || response.id != id_)
                fail(pipe::Failure::Protocol,ERROR_INVALID_DATA);
            else { response_ = response; state_ = State::Ready; deadline_ = 0; }
        }
        return state_;
    }
    std::optional<Response> take(uint64_t time) {
        if (poll(time) != State::Ready) return {};
        const auto result = response_; response_.reset(); state_ = State::Idle; return result;
    }
    void abandon() { if (state_ != State::Failed) fail(pipe::Failure::Closed); }
    State state() const { return state_; }
    pipe::Failure failure() const { return failure_; }
    DWORD error() const { return error_; }
    uint64_t deadline() const { return deadline_; }
    HANDLE event() const { return state_ == State::Pending ? handles()[2] : nullptr; }
};
class Server {
    control::Port port_;
    uint64_t sequence_ = 0, id_;
    std::optional<Request> pending_;
public:
    explicit Server(std::array<pipe::Handle,3> handles, uint64_t initialId = 0) : port_(std::move(handles)), id_(initialId) {}
    HANDLE event() const { return port_.handles()[1]; }
    std::optional<Request> take() {
        if (pending_) throw std::logic_error("unanswered engine admission request");
        const auto status = WaitForSingleObject(event(),0);
        if (status == WAIT_TIMEOUT) return {};
        pipe::check(status == WAIT_OBJECT_0); pipe::check(ResetEvent(event()));
        Request request;
        if (!decode(port_.read(),request) || sequence_ == UINT64_MAX || request.sequence != sequence_+1 || request.id <= id_)
            throw std::invalid_argument("invalid engine admission sequence");
        for (auto handle : port_.handles()) for (auto remote : request.handles)
            if (remote == reinterpret_cast<uintptr_t>(handle)) throw std::invalid_argument("aliased admission handle");
        sequence_ = request.sequence; id_ = request.id; pending_ = request; return request;
    }
    void respond(Code code) {
        if (!pending_) throw std::logic_error("missing engine admission request");
        port_.write(packet(Response{pending_->sequence,pending_->id,code})); pending_.reset(); pipe::check(SetEvent(port_.handles()[2]));
    }
};
// Before publication, this guard owns the newly created remote duplicates.
// Afterwards ONLY the owner may close them; no CLOSE_SOURCE after transfer,
// even on deadline, because the owner's numeric slots may already be reused.
class Transfer {
    HANDLE target_;
    std::array<uint64_t,4> handles_{};
    void clear() {
        for (auto value : handles_) if (value) {
            HANDLE local = nullptr;
            if (DuplicateHandle(target_,reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),GetCurrentProcess(),&local,
                    0,FALSE,DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) CloseHandle(local);
        }
        handles_ = {};
    }
public:
    Transfer(HANDLE target, std::array<HANDLE,4> sources) : target_(target) {
        try {
            for (size_t i=0;i<sources.size();++i) {
                HANDLE remote = nullptr;
                pipe::check(DuplicateHandle(GetCurrentProcess(),sources[i],target_,&remote,0,FALSE,DUPLICATE_SAME_ACCESS));
                handles_[i] = reinterpret_cast<uintptr_t>(remote);
            }
        } catch (...) { clear(); throw; }
    }
    ~Transfer() { clear(); }
    Transfer(const Transfer &) = delete;
    Transfer &operator=(const Transfer &) = delete;
    std::array<uint64_t,4> handles() const { return handles_; }
    void release() { handles_ = {}; }
};
} // namespace harukoto::engine::admission
