#pragma once
#include "EngineRelease.h"
#include "EngineDeployment.h"
#include "EngineInstance.h"
#include <deque>
#include <functional>
#include <thread>

// WIN-03 step 2: discovery handshake. An ordinary application finds the
// engine owner of its own logon session through one well-known pipe and
// proves who is serving it. The pipe name, PIDs and anything in the reply are
// locators only. Identity comes from the kernel: the pipe's server process
// (held open for the whole check), its token and its image against the
// approved release (step 1). Discovery grants no capability by itself.
// Blocking calls: run on a service / I/O thread, never in a TSF callback.
namespace harukoto::engine::discovery {
constexpr wchar_t DefaultScope[]=L"harukoto";
constexpr size_t MessageSize=128;
constexpr uint16_t Version=1;
constexpr uint64_t DefaultConnectionMs=2000, DefaultTimeoutMs=2000;
enum class Code : uint16_t { Ok=1, Rejected=2, ProtocolMismatch=3, Unavailable=4 };
struct Request {
    uint16_t protocol=0;
    uint32_t clientPid=0;
    std::array<uint8_t,32> nonce{};
};
struct Response {
    uint16_t protocol=0;
    Code code=Code::Rejected;
    uint32_t ownerPid=0, hostPid=0;
    uint64_t hostCreated=0; // FILETIME; binds the host PID locator to one process instance.
    std::array<uint8_t,32> nonce{};
};
using Message=std::array<uint8_t,MessageSize>;

namespace detail {
inline void put(uint8_t *out, uint64_t value, size_t bytes) { for (size_t i=0;i<bytes;++i) out[i]=static_cast<uint8_t>(value>>(8*i)); }
inline uint64_t get(const uint8_t *in, size_t bytes) { uint64_t value=0; for (size_t i=0;i<bytes;++i) value|=uint64_t(in[i])<<(8*i); return value; }
inline bool zero(const uint8_t *in, size_t from, size_t to) { return std::all_of(in+from,in+to,[](uint8_t b) { return b==0; }); }
inline bool header(const uint8_t *in, uint16_t type) {
    return in[0]=='H' && in[1]=='K' && in[2]=='D' && in[3]=='V' && get(in+4,2)==Version && get(in+6,2)==type;
}
inline void header(uint8_t *out, uint16_t type) { out[0]='H'; out[1]='K'; out[2]='D'; out[3]='V'; put(out+4,Version,2); put(out+6,type,2); }
} // namespace detail

// Fixed little-endian layout, all unused bytes zero:
// request  0 magic "HKDV" | 4 version | 6 type 1 | 8 protocol | 12 client pid | 16 nonce[32]
// response 0 magic "HKDV" | 4 version | 6 type 2 | 8 protocol | 10 code | 12 owner pid |
//          16 nonce[32] | 48 host pid | 56 host creation FILETIME
inline Message encode(const Request &request) {
    Message out{}; detail::header(out.data(),1);
    detail::put(out.data()+8,request.protocol,2); detail::put(out.data()+12,request.clientPid,4);
    std::copy(request.nonce.begin(),request.nonce.end(),out.begin()+16); return out;
}
inline Message encode(const Response &response) {
    Message out{}; detail::header(out.data(),2);
    detail::put(out.data()+8,response.protocol,2); detail::put(out.data()+10,static_cast<uint16_t>(response.code),2);
    detail::put(out.data()+12,response.ownerPid,4);
    std::copy(response.nonce.begin(),response.nonce.end(),out.begin()+16);
    detail::put(out.data()+48,response.hostPid,4); detail::put(out.data()+56,response.hostCreated,8); return out;
}
inline bool decode(const uint8_t *in, size_t size, Request &out) {
    if (!in || size!=MessageSize || !detail::header(in,1) || !detail::zero(in,10,12) || !detail::zero(in,48,MessageSize)) return false;
    Request value; value.protocol=static_cast<uint16_t>(detail::get(in+8,2)); value.clientPid=static_cast<uint32_t>(detail::get(in+12,4));
    std::copy(in+16,in+48,value.nonce.begin());
    if (!value.protocol || !value.clientPid) return false;
    out=value; return true;
}
inline bool decode(const uint8_t *in, size_t size, Response &out) {
    if (!in || size!=MessageSize || !detail::header(in,2) || !detail::zero(in,52,56) || !detail::zero(in,64,MessageSize)) return false;
    Response value; value.protocol=static_cast<uint16_t>(detail::get(in+8,2));
    const auto code=detail::get(in+10,2);
    if (code<1 || code>4 || !value.protocol) return false;
    value.code=static_cast<Code>(code); value.ownerPid=static_cast<uint32_t>(detail::get(in+12,4));
    std::copy(in+16,in+48,value.nonce.begin());
    value.hostPid=static_cast<uint32_t>(detail::get(in+48,4)); value.hostCreated=detail::get(in+56,8);
    // Only Ok carries locators; every other code must leave them empty.
    if ((value.code==Code::Ok) != (value.ownerPid && value.hostPid && value.hostCreated)) return false;
    out=value; return true;
}

// Step 3 handoff, on the SAME connection after an Ok reply (ENGINE_HANDOFF.md):
// Admit (3) -> Grant (4) -> Commit (5) -> Committed (6). Every message repeats
// the hello nonce, the client's request id and the owner incarnation (the
// owner process creation time), so nothing from another connection, another
// attempt or a previous owner incarnation can be mistaken for this one.
enum class Type : uint16_t { Admit=3, Grant=4, Commit=5, Committed=6 };
enum class Grant : uint16_t { Ok=1, Full=2, Closing=3, Rejected=4, Stale=5, Duplicate=6 };
struct Admission {
    Type type=Type::Admit;
    uint16_t protocol=0;
    Grant code=Grant::Ok;          // Grant / Committed only; zero bytes otherwise.
    uint32_t pid=0;                // Admit/Commit: client PID. Grant/Committed: owner PID.
    std::array<uint8_t,32> nonce{};
    uint64_t request=0;            // Client-chosen, non-zero, never reused for a new attempt.
    uint64_t incarnation=0;        // Owner creation FILETIME.
    uint64_t value=0;              // Admit: client creation FILETIME. Others: registration id.
    std::array<uint64_t,4> handles{}; // Grant Ok only: section, request, response, release.
};
// 0 magic | 4 version | 6 type | 8 protocol | 10 code | 12 pid | 16 nonce[32] |
// 48 request | 56 incarnation | 64 value | 72 four handle values | rest zero.
inline Message encode(const Admission &message) {
    Message out{}; detail::header(out.data(),static_cast<uint16_t>(message.type));
    detail::put(out.data()+8,message.protocol,2);
    if (message.type==Type::Grant || message.type==Type::Committed) detail::put(out.data()+10,static_cast<uint16_t>(message.code),2);
    detail::put(out.data()+12,message.pid,4);
    std::copy(message.nonce.begin(),message.nonce.end(),out.begin()+16);
    detail::put(out.data()+48,message.request,8); detail::put(out.data()+56,message.incarnation,8); detail::put(out.data()+64,message.value,8);
    for (size_t i=0;i<message.handles.size();++i) detail::put(out.data()+72+i*8,message.handles[i],8);
    return out;
}
inline bool decode(const uint8_t *in, size_t size, Admission &out) {
    if (!in || size!=MessageSize || in[0]!='H' || in[1]!='K' || in[2]!='D' || in[3]!='V' || detail::get(in+4,2)!=Version) return false;
    const auto type=detail::get(in+6,2);
    if (type<3 || type>6 || !detail::zero(in,104,MessageSize)) return false;
    Admission value; value.type=static_cast<Type>(type);
    value.protocol=static_cast<uint16_t>(detail::get(in+8,2));
    const auto code=detail::get(in+10,2);
    value.pid=static_cast<uint32_t>(detail::get(in+12,4));
    std::copy(in+16,in+48,value.nonce.begin());
    value.request=detail::get(in+48,8); value.incarnation=detail::get(in+56,8); value.value=detail::get(in+64,8);
    for (size_t i=0;i<value.handles.size();++i) value.handles[i]=detail::get(in+72+i*8,8);
    if (!value.protocol || !value.pid || !value.request || !value.incarnation) return false;
    const bool none=std::all_of(value.handles.begin(),value.handles.end(),[](uint64_t h) { return h==0; });
    switch (value.type) {
    case Type::Admit: case Type::Commit:
        if (code || !value.value || !none) return false;
        break;
    case Type::Grant:
        if (code<1 || code>6) return false;
        value.code=static_cast<Grant>(code);
        if (value.code==Grant::Ok) {
            if (!value.value) return false;
            for (size_t i=0;i<value.handles.size();++i) {
                if (!value.handles[i] || value.handles[i]>=std::numeric_limits<uintptr_t>::max()) return false;
                for (size_t j=0;j<i;++j) if (value.handles[i]==value.handles[j]) return false;
            }
        } else if (value.value || !none) return false; // Only Ok carries a capability.
        break;
    case Type::Committed:
        if (code!=static_cast<uint16_t>(Grant::Ok) && code!=static_cast<uint16_t>(Grant::Closing)) return false;
        value.code=static_cast<Grant>(code);
        if (!value.value || !none) return false;
        break;
    }
    out=value; return true;
}

// Supported peers on both sides: same user, logon SID, logon session and
// Windows session; Medium integrity; no AppContainer; no restricted SIDs.
// Elevated, low-integrity and sandboxed hosts stay on the in-process path.
struct TokenPolicy {
    pipe::LogonIdentity identity;
    DWORD integrity=0;
    bool appContainer=false, restricted=false;
    bool supported() const { return integrity==SECURITY_MANDATORY_MEDIUM_RID && !appContainer && !restricted; }
    bool peerOf(const TokenPolicy &other) const {
        return supported() && other.supported() && identity==other.identity && integrity==other.integrity;
    }
};
inline TokenPolicy inspect(HANDLE token) {
    TokenPolicy result;
    result.identity=pipe::LogonIdentity::fromToken(token);
    const auto label=pipe::tokenInfo(token,TokenIntegrityLevel);
    const auto sid=reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(label.data())->Label.Sid;
    if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) throw pipe::Error(pipe::Failure::Authentication,ERROR_INVALID_SID);
    result.integrity=*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
    DWORD container=0, size=0;
    pipe::check(GetTokenInformation(token,TokenIsAppContainer,&container,sizeof(container),&size),pipe::Failure::Authentication);
    result.appContainer=container!=0;
    result.restricted=IsTokenRestricted(token)!=FALSE;
    return result;
}
inline TokenPolicy inspectProcess(HANDLE process) {
    HANDLE raw=nullptr; pipe::check(OpenProcessToken(process,TOKEN_QUERY,&raw),pipe::Failure::Authentication);
    pipe::Handle token(raw); return inspect(token.get());
}
inline uint64_t created(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    pipe::check(GetProcessTimes(process,&creation,&exit,&kernel,&user),pipe::Failure::Authentication);
    return (uint64_t(creation.dwHighDateTime)<<32) | creation.dwLowDateTime;
}

// Pipe names are machine-global, so the name carries the Windows session,
// logon session LUID and logon SID of the caller, plus the deployment scope.
inline std::wstring name(const std::wstring &scope=DefaultScope) {
    if (!EngineInstance::validScope(scope)) throw std::invalid_argument("invalid discovery scope");
    pipe::processIdentityOnly();
    const auto identity=pipe::LogonIdentity::fromProcess(GetCurrentProcess());
    std::wstring result=L"\\\\.\\pipe\\Harukoto.Discovery.v1.";
    auto hex=[&](uint32_t value) { for (int shift=28;shift>=0;shift-=4) result+=L"0123456789abcdef"[(value>>shift)&15]; };
    hex(identity.session); hex(static_cast<uint32_t>(identity.authentication.HighPart)); hex(identity.authentication.LowPart);
    result+=L'.';
    for (auto byte : identity.logon) { result+=L"0123456789abcdef"[byte>>4]; result+=L"0123456789abcdef"[byte&15]; }
    result+=L'.'; result+=scope;
    if (result.size()>=256) throw std::invalid_argument("discovery name too long");
    return result;
}

// Owner side. One pipe instance, reused with Disconnect/Connect, so nobody
// (not even the owner) needs FILE_CREATE_PIPE_INSTANCE on the DACL. The first
// instance flag detects a squatter instead of joining its pipe. One
// connection at a time, each bounded by connectionMs (hello and the optional
// step-3 handoff together); clients retry on busy.
//
// Step 3: an Admitter lets a verified client ask for a registration slot on
// the same connection. The owner duplicates the slot's handles into the
// verified client process; the slot stays reserved (never scheduled) until
// the client commits, and is discarded on any failure before that.
struct Offer {
    Grant code=Grant::Full;
    uint64_t registration=0;
    std::array<uint64_t,4> handles{}; // Values valid in the client process.
};
struct Admitter {
    // Owner thread, inside Server::poll. `client` is the verified pipe client
    // with PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION.
    // Ok: handles already duplicated into it, slot reserved but not scheduled.
    std::function<Offer(HANDLE client)> reserve;
    std::function<bool(uint64_t)> activate; // false: slot already gone; it must not be activated.
    std::function<void(uint64_t)> discard;  // Release a reserved or just-activated slot.
};
class Server {
public:
    class Squatted : public pipe::Error {
    public: explicit Squatted(DWORD code) : pipe::Error(pipe::Failure::Authentication,code) {}
    };
    struct Stats { uint64_t accepted=0, rejected=0, aborted=0, granted=0, committed=0, discarded=0, reclaimed=0; };
private:
    enum class Phase { Idle, Listening, Reading, Writing, AwaitAdmit, WritingGrant, AwaitCommit, WritingAck, Closing };
    struct Reservation {
        uint64_t registration=0;
        std::array<uint64_t,4> handles{};
        pipe::Handle client;      // Dup-capable; dropped once the grant is delivered.
        bool delivered=false, activated=false;
    };
    pipe::Handle pipe_, host_;
    pipe::Operation operation_;
    TokenPolicy policy_;
    uint16_t protocol_;
    uint64_t connectionMs_, deadline_=0, hostCreated_=0, incarnation_=0;
    DWORD hostPid_=0;
    Phase phase_=Phase::Idle;
    Message buffer_{};
    size_t position_=0;
    uint8_t scratch_=0;
    Stats stats_;
    Admitter admitter_;
    std::optional<Request> hello_;  // Set only after an Ok reply on this connection.
    Admission admit_{};
    std::optional<Reservation> reservation_;
    std::deque<uint64_t> recent_;   // Request ids already seen by this owner incarnation.
    std::optional<TokenPolicy> client() {
        try {
            pipe::processIdentityOnly();
            pipe::check(ImpersonateNamedPipeClient(pipe_.get()),pipe::Failure::Authentication);
            struct Revert { ~Revert() { if (!RevertToSelf()) std::terminate(); } } revert;
            HANDLE raw=nullptr; pipe::check(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&raw),pipe::Failure::Authentication);
            pipe::Handle token(raw); return inspect(token.get());
        } catch (...) { return std::nullopt; }
    }
    Response respond(const Request &request) {
        Response response; response.protocol=protocol_; response.nonce=request.nonce;
        ULONG pid=0, session=0;
        wchar_t computer[256]{};
        if (!GetNamedPipeClientProcessId(pipe_.get(),&pid) || pid!=request.clientPid ||
                !GetNamedPipeClientSessionId(pipe_.get(),&session) || session!=policy_.identity.session) return response;
        // Local clients fail with ERROR_PIPE_LOCAL; PIPE_REJECT_REMOTE_CLIENTS
        // should already have refused remote ones. Defense in depth.
        if (GetNamedPipeClientComputerNameW(pipe_.get(),computer,sizeof(computer)) || GetLastError()!=ERROR_PIPE_LOCAL) return response;
        const auto peer=client();
        if (!peer || !peer->peerOf(policy_)) return response;
        if (request.protocol!=protocol_) { response.code=Code::ProtocolMismatch; return response; }
        if (WaitForSingleObject(host_.get(),0)!=WAIT_TIMEOUT) { response.code=Code::Unavailable; return response; }
        response.code=Code::Ok; response.ownerPid=GetCurrentProcessId();
        response.hostPid=hostPid_; response.hostCreated=hostCreated_; return response;
    }
    // The client process behind this connection, opened by the pipe's client
    // PID and bound to the creation time the verified client stated. It must
    // still be alive and still be a peer token.
    pipe::Handle verifiedClient(const Admission &admit) {
        ULONG pid=0;
        if (!GetNamedPipeClientProcessId(pipe_.get(),&pid) || pid!=admit.pid) return pipe::Handle();
        pipe::Handle process(OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        if (!process || WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT) return pipe::Handle();
        try {
            if (created(process.get())!=admit.value || !inspectProcess(process.get()).peerOf(policy_)) return pipe::Handle();
        } catch (const pipe::Error &) { return pipe::Handle(); }
        return process;
    }
    Admission grant(const Admission &admit) {
        Admission reply; reply.type=Type::Grant; reply.protocol=protocol_; reply.pid=GetCurrentProcessId();
        reply.nonce=admit.nonce; reply.request=admit.request; reply.incarnation=incarnation_; reply.code=Grant::Rejected;
        // A request id is accepted once per owner incarnation, whatever the
        // outcome: a replayed or retried Admit can never take a second slot.
        if (std::find(recent_.begin(),recent_.end(),admit.request)!=recent_.end()) { reply.code=Grant::Duplicate; return reply; }
        recent_.push_back(admit.request); if (recent_.size()>256) recent_.pop_front();
        if (admit.incarnation!=incarnation_) { reply.code=Grant::Stale; return reply; }
        if (!admitter_.reserve) return reply;
        auto process=verifiedClient(admit);
        if (!process) return reply;
        // A client that vanishes mid-duplication only fails its own handoff;
        // it must never take the discovery pipe down.
        Offer offer;
        try { offer=admitter_.reserve(process.get()); } catch (const pipe::Error &) { return reply; }
        reply.code=offer.code;
        if (offer.code==Grant::Ok) {
            if (!offer.registration) throw std::logic_error("missing registration id");
            reservation_=Reservation{offer.registration,offer.handles,std::move(process)};
            reply.value=offer.registration; reply.handles=offer.handles; ++stats_.granted;
        }
        return reply;
    }
    // Undelivered grant: the client never learned the values, so close them in
    // its process. Once delivered, only the client may close them (it may
    // already have closed and reused those values).
    void releaseReservation(bool writeFailed) {
        if (!reservation_) return;
        auto reservation=std::move(*reservation_); reservation_.reset();
        if (!reservation.delivered && writeFailed && reservation.client) {
            for (auto value : reservation.handles)
                DuplicateHandle(reservation.client.get(),reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),nullptr,nullptr,0,FALSE,DUPLICATE_CLOSE_SOURCE);
            ++stats_.reclaimed;
        }
        ++stats_.discarded;
        if (admitter_.discard) admitter_.discard(reservation.registration);
    }
    void recycle(bool aborted, bool writeFailed=false) {
        if (operation_.pending) {
            CancelIoEx(pipe_.get(),&operation_.overlapped);
            DWORD ignored=0; GetOverlappedResult(pipe_.get(),&operation_.overlapped,&ignored,TRUE);
            operation_.pending=false;
        }
        releaseReservation(writeFailed);
        hello_.reset();
        if (aborted) ++stats_.aborted;
        DisconnectNamedPipe(pipe_.get()); // Discards anything unread; a new client starts clean.
        phase_=Phase::Idle;
    }
    void begin(uint64_t time) { phase_=Phase::Reading; position_=0; deadline_=time+connectionMs_; hello_.reset(); }
    bool reading() const { return phase_==Phase::Reading || phase_==Phase::AwaitAdmit || phase_==Phase::AwaitCommit; }
    bool writing() const { return phase_==Phase::Writing || phase_==Phase::WritingGrant || phase_==Phase::WritingAck; }
    bool issue() {
        operation_.reset();
        BOOL ok=FALSE;
        if (reading()) ok=ReadFile(pipe_.get(),buffer_.data()+position_,static_cast<DWORD>(MessageSize-position_),nullptr,&operation_.overlapped);
        else if (writing()) ok=WriteFile(pipe_.get(),buffer_.data()+position_,static_cast<DWORD>(MessageSize-position_),nullptr,&operation_.overlapped);
        else ok=ReadFile(pipe_.get(),&scratch_,1,nullptr,&operation_.overlapped);
        if (!ok && GetLastError()!=ERROR_IO_PENDING) return false;
        operation_.pending=true; return true;
    }
    void listen(uint64_t time) {
        operation_.reset();
        if (ConnectNamedPipe(pipe_.get(),&operation_.overlapped)) { begin(time); return; }
        const auto error=GetLastError();
        if (error==ERROR_IO_PENDING) { operation_.pending=true; phase_=Phase::Listening; }
        else if (error==ERROR_PIPE_CONNECTED) begin(time);
        else if (error==ERROR_NO_DATA) DisconnectNamedPipe(pipe_.get()); // Client already left; listen again.
        else throw pipe::Error(pipe::Failure::Io,error);
    }
    void send(const Message &message, Phase phase) { buffer_=message; position_=0; phase_=phase; }
    void expect(Phase phase) { position_=0; phase_=phase; }
    // A failed I/O: a step-2 client closing after its reply is the normal end.
    void failed() {
        if (phase_==Phase::Closing || (phase_==Phase::AwaitAdmit && !position_)) { recycle(false); return; }
        recycle(true,phase_==Phase::WritingGrant);
    }
    void complete() {
        switch (phase_) {
        case Phase::Reading: {
            Request request;
            if (!decode(buffer_.data(),buffer_.size(),request)) { ++stats_.rejected; recycle(false); return; }
            const auto response=respond(request);
            ++(response.code==Code::Ok ? stats_.accepted : stats_.rejected);
            if (response.code==Code::Ok) hello_=request;
            send(encode(response),Phase::Writing); return;
        }
        case Phase::Writing:
            // Only a verified client may continue to the handoff.
            expect(hello_ ? Phase::AwaitAdmit : Phase::Closing); return;
        case Phase::AwaitAdmit: {
            Admission admit;
            if (!decode(buffer_.data(),buffer_.size(),admit) || admit.type!=Type::Admit || admit.protocol!=protocol_ ||
                    admit.pid!=hello_->clientPid || admit.nonce!=hello_->nonce) { ++stats_.rejected; recycle(false); return; }
            admit_=admit;
            const auto reply=grant(admit);
            send(encode(reply),reply.code==Grant::Ok ? Phase::WritingGrant : Phase::Writing);
            if (reply.code!=Grant::Ok) hello_.reset(); // Write, then wait for EOF.
            return;
        }
        case Phase::WritingGrant:
            reservation_->delivered=true; reservation_->client.reset();
            expect(Phase::AwaitCommit); return;
        case Phase::AwaitCommit: {
            Admission commit;
            if (!decode(buffer_.data(),buffer_.size(),commit) || commit.type!=Type::Commit || commit.protocol!=protocol_ ||
                    commit.pid!=admit_.pid || commit.nonce!=admit_.nonce || commit.request!=admit_.request ||
                    commit.incarnation!=incarnation_ || commit.value!=reservation_->registration) { ++stats_.rejected; recycle(true); return; }
            Admission ack; ack.type=Type::Committed; ack.protocol=protocol_; ack.pid=GetCurrentProcessId();
            ack.nonce=commit.nonce; ack.request=commit.request; ack.incarnation=incarnation_; ack.value=commit.value;
            const bool active=admitter_.activate && admitter_.activate(commit.value);
            if (active) reservation_->activated=true; else { reservation_.reset(); ++stats_.discarded; }
            ack.code=active ? Grant::Ok : Grant::Closing;
            send(encode(ack),Phase::WritingAck); return;
        }
        case Phase::WritingAck:
            if (reservation_) { reservation_.reset(); ++stats_.committed; } // Delivered: the client owns the slot now.
            expect(Phase::Closing); return;
        default: return;
        }
    }
public:
    // host: the owner's retained host process object; its PID and creation
    // time are the only locators returned to verified clients.
    Server(const std::wstring &scope, HANDLE host, uint16_t protocol=ProtocolVersion, uint64_t connectionMs=DefaultConnectionMs,
            Admitter admitter={})
        : protocol_(protocol), connectionMs_(connectionMs), admitter_(std::move(admitter)) {
        if (!host || !protocol || !connectionMs || connectionMs>60000 ||
                (bool(admitter_.reserve) != bool(admitter_.activate) || bool(admitter_.reserve) != bool(admitter_.discard)))
            throw std::invalid_argument("invalid discovery server");
        const auto objectName=name(scope);
        policy_=inspectProcess(GetCurrentProcess());
        incarnation_=created(GetCurrentProcess());
        // An elevated, low-integrity or sandboxed owner is never discoverable.
        if (!policy_.supported()) throw pipe::Error(pipe::Failure::Authentication,ERROR_ACCESS_DENIED);
        HANDLE raw=nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),host,GetCurrentProcess(),&raw,PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
        host_.reset(raw); hostPid_=GetProcessId(host_.get()); hostCreated_=created(host_.get());
        if (!hostPid_ || hostPid_==GetCurrentProcessId() || WaitForSingleObject(host_.get(),0)!=WAIT_TIMEOUT)
            throw pipe::Error(pipe::Failure::Authentication,ERROR_PROCESS_ABORTED);
        // Only this logon SID may open it, read/write only (no create-instance,
        // no generic write). The default Medium no-write-up label keeps
        // low-integrity processes out; AppContainers are not in the DACL.
        constexpr DWORD access=pipe::ClientAccess;
        std::vector<uint8_t> aclBytes(sizeof(ACL)+sizeof(ACCESS_ALLOWED_ACE)-sizeof(DWORD)+policy_.identity.logon.size());
        auto *acl=reinterpret_cast<ACL *>(aclBytes.data());
        pipe::check(InitializeAcl(acl,static_cast<DWORD>(aclBytes.size()),ACL_REVISION));
        pipe::check(AddAccessAllowedAce(acl,ACL_REVISION,access,policy_.identity.logon.data()));
        SECURITY_DESCRIPTOR descriptor{};
        pipe::check(InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION));
        pipe::check(SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE));
        pipe::check(SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),&descriptor,FALSE};
        pipe_.reset(CreateNamedPipeW(objectName.c_str(),PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,1,
            static_cast<DWORD>(MessageSize*2),static_cast<DWORD>(MessageSize*2),0,&attributes));
        if (!pipe_) {
            const auto error=GetLastError();
            // Someone already owns the name: never join or adopt that pipe.
            if (error==ERROR_ACCESS_DENIED || error==ERROR_PIPE_BUSY) throw Squatted(error);
            throw pipe::Error(pipe::Failure::Io,error);
        }
        listen(pipe::now());
    }
    Server(const Server &)=delete;
    Server &operator=(const Server &)=delete;
    // Closing the pipe mid-handoff (owner stopping) discards an uncommitted slot.
    ~Server() {
        if (operation_.pending) { CancelIoEx(pipe_.get(),&operation_.overlapped); DWORD n=0; GetOverlappedResult(pipe_.get(),&operation_.overlapped,&n,TRUE); }
        try { releaseReservation(false); } catch (...) {}
    }
    HANDLE event() const { return operation_.pending ? operation_.event.get() : nullptr; }
    // Absolute deadline for the active connection; 1 asks for an immediate poll.
    uint64_t deadline() const {
        if (!operation_.pending) return 1;
        return phase_==Phase::Listening ? 0 : deadline_;
    }
    const Stats &stats() const { return stats_; }
    uint64_t incarnation() const { return incarnation_; }
    void poll(uint64_t time) {
        for (int budget=0;budget<16;++budget) {
            if (phase_==Phase::Idle) { listen(time); continue; }
            if (phase_!=Phase::Listening && time>=deadline_) { recycle(true); continue; }
            if (!operation_.pending && !issue()) { failed(); continue; }
            DWORD transferred=0;
            if (!GetOverlappedResult(pipe_.get(),&operation_.overlapped,&transferred,FALSE)) {
                const auto error=GetLastError();
                if (error==ERROR_IO_INCOMPLETE) return;
                operation_.pending=false;
                failed(); continue;
            }
            operation_.pending=false;
            if (phase_==Phase::Listening) { begin(time); continue; }
            if (phase_==Phase::Closing) { recycle(false); continue; } // Unexpected extra byte: drop.
            if (!transferred || transferred>MessageSize-position_) { failed(); continue; }
            position_+=transferred;
            if (position_<MessageSize) continue;
            complete();
        }
    }
};

enum class Result {
    Verified,          // Owner and host proven; engine is usable for step 3.
    NoRelease,         // Release metadata missing/unprotected/invalid or nothing approved.
    DeploymentInvalid, // Approved files cannot be pinned or no longer match.
    UnsupportedClient, // This process is elevated, low-integrity, AppContainer or restricted.
    NotRunning,        // No discovery pipe in this logon session.
    Busy,              // The single instance stayed busy until the deadline.
    Denied,            // The OS refused to open the pipe (DACL, integrity label).
    Remote,            // The handle is not a local named pipe.
    Identity,          // Server/host not openable, other user/logon/session, PID mismatch.
    Integrity,         // Server/host token has another integrity level or is sandboxed.
    Image,             // Server/host image is not an approved program.
    Stale,             // Server/host is an older approved release than the selected one.
    Protocol,          // Malformed reply, nonce mismatch, protocol mismatch or early close.
    Rejected,          // The engine refused this client.
    Unavailable,       // The engine reported its host already exited.
    Timeout,           // The handshake did not finish before the deadline.
    Failed             // Unexpected local failure.
};
struct Engine {
    release::Entry release;
    std::shared_ptr<const EngineDeployment> ownerDeployment, hostDeployment;
    pipe::Handle owner, host; // Non-inheritable, query/wait only.
    DWORD ownerPid=0, hostPid=0;
};
struct Outcome {
    Result result=Result::Failed;
    DWORD win32=ERROR_SUCCESS;
    std::optional<Engine> engine;
};
struct Options {
    std::wstring scope=DefaultScope;
    uint16_t protocol=ProtocolVersion;
    uint64_t timeoutMs=DefaultTimeoutMs;
};

namespace detail {
struct Stop { Result result; DWORD win32; };
[[noreturn]] inline void stop(Result result, DWORD win32=ERROR_SUCCESS) { throw Stop{result,win32}; }
inline DWORD remaining(uint64_t deadline) { const auto time=pipe::now(); return deadline>time ? static_cast<DWORD>(std::min<uint64_t>(deadline-time,60000)) : 0; }
inline void transfer(HANDLE file, bool write, uint8_t *data, size_t size, uint64_t deadline) {
    pipe::Operation operation;
    for (size_t done=0;done<size;) {
        operation.reset();
        const auto count=static_cast<DWORD>(size-done);
        const auto ok=write ? WriteFile(file,data+done,count,nullptr,&operation.overlapped) : ReadFile(file,data+done,count,nullptr,&operation.overlapped);
        if (!ok && GetLastError()!=ERROR_IO_PENDING) stop(Result::Protocol,GetLastError());
        DWORD transferred=0;
        if (WaitForSingleObject(operation.event.get(),remaining(deadline))!=WAIT_OBJECT_0) {
            CancelIoEx(file,&operation.overlapped); GetOverlappedResult(file,&operation.overlapped,&transferred,TRUE);
            stop(Result::Timeout,WAIT_TIMEOUT);
        }
        if (!GetOverlappedResult(file,&operation.overlapped,&transferred,FALSE)) stop(Result::Protocol,GetLastError());
        if (!transferred || transferred>count) stop(Result::Protocol,ERROR_BROKEN_PIPE);
        done+=transferred;
    }
}
inline pipe::Handle openProcess(DWORD pid) {
    if (!pid) stop(Result::Identity,ERROR_INVALID_PARAMETER);
    pipe::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,pid));
    if (!process) stop(Result::Identity,GetLastError());
    if (WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT) stop(Result::Identity,ERROR_PROCESS_ABORTED);
    return process;
}
inline void peer(HANDLE process, const TokenPolicy &self) {
    TokenPolicy other;
    try { other=inspectProcess(process); } catch (const pipe::Error &error) { stop(Result::Identity,error.code); }
    if (!(other.identity==self.identity)) stop(Result::Identity,ERROR_ACCESS_DENIED);
    if (!other.peerOf(self)) stop(Result::Integrity,ERROR_ACCESS_DENIED);
}
// Distinguish an older approved release (upgrade window, step 7) from an
// unapproved image. Only entries from the same trusted snapshot count.
inline void image(HANDLE process, const EngineDeployment &expected, const release::Snapshot &snapshot,
        const release::Entry &selected, bool owner) {
    try { expected.verify(process); return; }
    catch (const pipe::Error &error) {
        if (WaitForSingleObject(process,0)!=WAIT_TIMEOUT) stop(Result::Identity,ERROR_PROCESS_ABORTED);
        for (const auto &entry : snapshot.entries) {
            if (entry.protocol!=selected.protocol || entry.id==selected.id) continue;
            try {
                EngineDeployment older(owner ? entry.owner : entry.host,owner ? entry.ownerSha256 : entry.hostSha256);
                older.verify(process);
                stop(Result::Stale,ERROR_INVALID_IMAGE_HASH);
            } catch (const pipe::Error &) {} catch (const std::exception &) {}
        }
        stop(Result::Image,error.code);
    }
}
} // namespace detail

// The verified connection, kept open for the step-3 handoff (EngineHandoff.h).
struct Connection {
    pipe::Handle file;
    Request request;     // The hello: its nonce binds every later message.
    uint64_t deadline=0; // Absolute, for the whole exchange.
};
namespace detail {
inline Outcome open(const release::Snapshot &snapshot, const Options &options, Connection *keep);
}
// Client side. `snapshot` must come from release::read(); tests may pass an
// in-memory snapshot. Never feed values from the engine, HKCU or diagnostics.
// Closes the connection: identity only, no capability.
inline Outcome discover(const release::Snapshot &snapshot, const Options &options=Options{}) { return detail::open(snapshot,options,nullptr); }
inline Outcome detail::open(const release::Snapshot &snapshot, const Options &options, Connection *keep) {
    Outcome outcome;
    try {
        if (!options.protocol || !options.timeoutMs || options.timeoutMs>60000) detail::stop(Result::Failed,ERROR_INVALID_PARAMETER);
        try { pipe::processIdentityOnly(); } catch (const pipe::Error &error) { detail::stop(Result::UnsupportedClient,error.code); }
        const auto self=inspectProcess(GetCurrentProcess());
        if (!self.supported()) detail::stop(Result::UnsupportedClient,ERROR_ACCESS_DENIED);
        const auto selected=release::select(snapshot,options.protocol);
        if (!selected) detail::stop(Result::NoRelease,snapshot.status==release::Status::Ready ? ERROR_NOT_FOUND : snapshot.error);
        Engine engine; engine.release=*selected;
        try {
            // Reused while still pinned in this process (EngineDeployment::shared).
            engine.ownerDeployment=EngineDeployment::shared(selected->owner,selected->ownerSha256);
            engine.hostDeployment=EngineDeployment::shared(selected->host,selected->hostSha256);
        } catch (const pipe::Error &error) { detail::stop(Result::DeploymentInvalid,error.code); }
        catch (const std::invalid_argument &) { detail::stop(Result::DeploymentInvalid,ERROR_INVALID_DATA); }
        const auto objectName=name(options.scope);
        const auto deadline=pipe::now()+options.timeoutMs;
        pipe::Handle file;
        for (;;) {
            file.reset(CreateFileW(objectName.c_str(),pipe::ClientAccess,0,nullptr,OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,nullptr));
            if (file) break;
            const auto error=GetLastError();
            if (error==ERROR_FILE_NOT_FOUND) detail::stop(Result::NotRunning,error);
            if (error==ERROR_ACCESS_DENIED) detail::stop(Result::Denied,error);
            if (error!=ERROR_PIPE_BUSY) detail::stop(Result::Failed,error);
            const auto wait=detail::remaining(deadline);
            if (!wait) detail::stop(Result::Busy,error);
            if (!WaitNamedPipeW(objectName.c_str(),wait) && GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_SEM_TIMEOUT)
                detail::stop(Result::Failed,GetLastError());
            if (!detail::remaining(deadline)) detail::stop(Result::Busy,ERROR_PIPE_BUSY);
        }
        // A local named pipe client end in our own Windows session.
        DWORD flags=0; FILE_REMOTE_PROTOCOL_INFO remote{};
        if (GetFileType(file.get())!=FILE_TYPE_PIPE || !GetNamedPipeInfo(file.get(),&flags,nullptr,nullptr,nullptr) ||
                (flags & PIPE_SERVER_END) || GetFileInformationByHandleEx(file.get(),FileRemoteProtocolInfo,&remote,sizeof(remote)))
            detail::stop(Result::Remote,ERROR_BAD_PIPE);
        ULONG session=0, pid=0;
        if (!GetNamedPipeServerSessionId(file.get(),&session) || session!=self.identity.session) detail::stop(Result::Identity,ERROR_ACCESS_DENIED);
        if (!GetNamedPipeServerProcessId(file.get(),&pid)) detail::stop(Result::Identity,GetLastError());
        // Hold the server process object from here on: while we hold it the
        // PID cannot be reused, so a later PID match means the same process.
        engine.owner=detail::openProcess(pid); engine.ownerPid=pid;
        detail::peer(engine.owner.get(),self);
        detail::image(engine.owner.get(),*engine.ownerDeployment,snapshot,*selected,true);
        Request request; request.protocol=options.protocol; request.clientPid=GetCurrentProcessId();
        if (BCryptGenRandom(nullptr,request.nonce.data(),static_cast<ULONG>(request.nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
            detail::stop(Result::Failed,ERROR_GEN_FAILURE);
        auto bytes=encode(request);
        detail::transfer(file.get(),true,bytes.data(),bytes.size(),deadline);
        Message reply{};
        detail::transfer(file.get(),false,reply.data(),reply.size(),deadline);
        Response response;
        if (!decode(reply.data(),reply.size(),response) || response.nonce!=request.nonce) detail::stop(Result::Protocol,ERROR_INVALID_DATA);
        if (response.code==Code::Rejected) detail::stop(Result::Rejected,ERROR_ACCESS_DENIED);
        if (response.code==Code::Unavailable) detail::stop(Result::Unavailable,ERROR_PROCESS_ABORTED);
        if (response.code==Code::ProtocolMismatch || response.protocol!=options.protocol) detail::stop(Result::Protocol,ERROR_REVISION_MISMATCH);
        if (response.ownerPid!=pid) detail::stop(Result::Protocol,ERROR_INVALID_DATA);
        // The server PID attribute is fixed per pipe instance and the process
        // object has been held since before the request, so the PID cannot
        // have been reused; the server may already have disconnected, so do
        // not query the pipe again. Re-check that the owner is still alive
        // and still the approved image.
        detail::image(engine.owner.get(),*engine.ownerDeployment,snapshot,*selected,true);
        // The host PID is a locator from a now-verified owner. Bind it to the
        // exact instance with its creation time, then prove it like the owner.
        if (response.hostPid==pid || response.hostPid==GetCurrentProcessId()) detail::stop(Result::Identity,ERROR_INVALID_DATA);
        engine.host=detail::openProcess(response.hostPid); engine.hostPid=response.hostPid;
        try { if (created(engine.host.get())!=response.hostCreated) detail::stop(Result::Identity,ERROR_INVALID_DATA); }
        catch (const pipe::Error &error) { detail::stop(Result::Identity,error.code); }
        detail::peer(engine.host.get(),self);
        detail::image(engine.host.get(),*engine.hostDeployment,snapshot,*selected,false);
        if (WaitForSingleObject(engine.owner.get(),0)!=WAIT_TIMEOUT) detail::stop(Result::Identity,ERROR_PROCESS_ABORTED);
        if (keep) { keep->file=std::move(file); keep->request=request; keep->deadline=deadline; }
        outcome.result=Result::Verified; outcome.engine=std::move(engine);
    } catch (const detail::Stop &stop) { outcome.result=stop.result; outcome.win32=stop.win32; outcome.engine.reset(); }
    catch (const pipe::Error &error) { outcome.result=Result::Failed; outcome.win32=error.code; outcome.engine.reset(); }
    catch (const std::exception &) { outcome.result=Result::Failed; outcome.win32=ERROR_INVALID_PARAMETER; outcome.engine.reset(); }
    return outcome;
}
// Production entry: the protected HKLM release source only.
inline Outcome discover(const Options &options=Options{}) { return discover(release::read(),options); }
} // namespace harukoto::engine::discovery
