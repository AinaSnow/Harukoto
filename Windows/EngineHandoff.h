#pragma once
#include "EngineDiscovery.h"
#include "EngineRegistration.h"

// WIN-03 step 3: handoff after discovery (docs/ENGINE_HANDOFF.md). A client
// that finds an engine running - including one that just lost the
// single-instance race - gets a registration slot on that engine through the
// SAME verified discovery connection: Admit -> Grant -> Commit -> Committed.
// The owner duplicates the slot's handles into the verified client process
// and binds the slot to that process. No other public object, no second
// engine, no query before the handoff completes, nothing replayed.
// Blocking calls: run on a service / I/O thread, never in a TSF callback.
namespace harukoto::engine::handoff {
enum class Result {
    Admitted,    // Session usable; no request has been sent yet.
    NotRunning,  // No engine in this logon session.
    Busy,        // Discovery pipe busy until the deadline, or the starter lock was.
    Full,        // All 8 client slots in use.
    Closing,     // The engine is stopping.
    Unavailable, // Connection lost mid-handoff (owner exited/stopped, owner gave up on a slow client).
    Timeout,     // Our own deadline passed.
    Stale,       // Older approved engine running, or a reply from another owner incarnation.
    Duplicate,   // The owner had already seen this request id.
    Rejected,    // The owner refused this client.
    Protocol,    // Malformed or mismatched message.
    Untrusted,   // Verification failed (release, deployment, identity, integrity, image...).
    Failed       // Unexpected local failure.
};
// Test-only fault injection points, called on the calling thread.
enum class Stage { Verified, Granted, Committed };
struct Options {
    discovery::Options discovery; // timeoutMs bounds the whole exchange (verification included).
    std::function<void(Stage)> testHook;
    uint64_t testRequestId=0;     // Tests only: 0 picks a fresh random id (always, in production).
    Options() { discovery.timeoutMs=3000; }
};

// A handed-off slot. Destroying it (or release()) gives the slot back: the
// owner retires it at once instead of waiting for this process to exit.
class Session {
    discovery::Engine engine_;
    std::unique_ptr<EngineRegistration> registration_;
    pipe::Handle release_; // EVENT_MODIFY_STATE | SYNCHRONIZE on the slot's revoke event.
public:
    Session(discovery::Engine engine, std::unique_ptr<EngineRegistration> registration, pipe::Handle release)
        : engine_(std::move(engine)), registration_(std::move(registration)), release_(std::move(release)) {}
    Session(const Session &)=delete;
    Session &operator=(const Session &)=delete;
    ~Session() { release(); }
    EngineRegistration &registration() { return *registration_; }
    const discovery::Engine &engine() const { return engine_; }
    uint64_t id() const { return registration_->id(); }
    void release() { if (release_) { SetEvent(release_.get()); release_.reset(); } }
};
struct Outcome {
    Result result=Result::Failed;
    discovery::Result verification=discovery::Result::Failed;
    DWORD win32=ERROR_SUCCESS;
    std::unique_ptr<Session> session;
};

inline Result fromDiscovery(discovery::Result result) {
    using R=discovery::Result;
    switch (result) {
    case R::Verified: return Result::Admitted;
    case R::NotRunning: return Result::NotRunning;
    case R::Busy: return Result::Busy;
    case R::Unavailable: return Result::Unavailable;
    case R::Timeout: return Result::Timeout;
    case R::Stale: return Result::Stale;
    case R::Rejected: return Result::Rejected;
    case R::Protocol: return Result::Protocol;
    case R::Failed: return Result::Failed;
    default: return Result::Untrusted;
    }
}
// The client's check of a Grant against its own Admit. A reply that does not
// echo this connection's nonce, this attempt's request id and the owner
// incarnation the client verified is never accepted.
inline Result check(const discovery::Admission &admit, const discovery::Admission &grant, DWORD ownerPid) {
    using G=discovery::Grant;
    if (grant.type!=discovery::Type::Grant || grant.protocol!=admit.protocol || grant.nonce!=admit.nonce ||
            grant.request!=admit.request || grant.pid!=ownerPid) return Result::Protocol;
    if (grant.incarnation!=admit.incarnation || grant.code==G::Stale) return Result::Stale;
    switch (grant.code) {
    case G::Ok: return Result::Admitted;
    case G::Full: return Result::Full;
    case G::Closing: return Result::Closing;
    case G::Duplicate: return Result::Duplicate;
    default: return Result::Rejected;
    }
}
inline Result check(const discovery::Admission &commit, const discovery::Admission &ack) {
    if (ack.type!=discovery::Type::Committed || ack.protocol!=commit.protocol || ack.nonce!=commit.nonce ||
            ack.request!=commit.request || ack.incarnation!=commit.incarnation || ack.value!=commit.value) return Result::Protocol;
    return ack.code==discovery::Grant::Ok ? Result::Admitted : Result::Closing;
}

namespace detail {
struct Stop { Result result; DWORD win32; };
[[noreturn]] inline void stop(Result result, DWORD win32=ERROR_SUCCESS) { throw Stop{result,win32}; }
// One message each way; a closed or broken connection means the owner went
// away or gave up on us (Unavailable), our own deadline is Timeout.
inline void exchange(HANDLE file, bool write, discovery::Message &message, uint64_t deadline, HANDLE owner) {
    try { discovery::detail::transfer(file,write,message.data(),message.size(),deadline); }
    catch (const discovery::detail::Stop &failure) {
        if (failure.result==discovery::Result::Timeout) stop(Result::Timeout,failure.win32);
        const auto error=failure.win32;
        if (error==ERROR_BROKEN_PIPE || error==ERROR_NO_DATA || error==ERROR_PIPE_NOT_CONNECTED || error==ERROR_HANDLE_EOF ||
                WaitForSingleObject(owner,0)!=WAIT_TIMEOUT)
            stop(Result::Unavailable,error);
        stop(Result::Protocol,error);
    }
}
inline uint64_t random() {
    uint64_t value=0;
    while (!value)
        if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&value),sizeof(value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) stop(Result::Failed,ERROR_GEN_FAILURE);
    return value;
}
} // namespace detail

// Discover, verify, and take a slot over the same connection. Any result but
// Admitted leaves the caller on its in-process path with nothing sent to the
// engine beyond the handoff itself, and no slot held (ENGINE_HANDOFF.md).
inline Outcome admit(const release::Snapshot &snapshot, const Options &options=Options{}) {
    Outcome outcome;
    discovery::Connection connection;
    auto found=discovery::detail::open(snapshot,options.discovery,&connection);
    outcome.verification=found.result; outcome.win32=found.win32;
    if (found.result!=discovery::Result::Verified) { outcome.result=fromDiscovery(found.result); return outcome; }
    auto engine=std::move(*found.engine);
    std::array<pipe::Handle,4> handles;
    // Once a grant is in hand, every exit except success hands the slot back,
    // whether or not the owner has already activated it.
    struct Give { std::array<pipe::Handle,4> &handles; bool armed=false; ~Give() { if (armed && handles[3]) SetEvent(handles[3].get()); } } give{handles};
    try {
        auto hook=[&](Stage stage) { if (options.testHook) options.testHook(stage); };
        hook(Stage::Verified);
        discovery::Admission request; request.type=discovery::Type::Admit; request.protocol=options.discovery.protocol;
        request.pid=GetCurrentProcessId(); request.nonce=connection.request.nonce;
        request.request=options.testRequestId ? options.testRequestId : detail::random();
        try { request.incarnation=discovery::created(engine.owner.get()); request.value=discovery::created(GetCurrentProcess()); }
        catch (const pipe::Error &error) { detail::stop(Result::Failed,error.code); }
        auto bytes=discovery::encode(request);
        detail::exchange(connection.file.get(),true,bytes,connection.deadline,engine.owner.get());
        discovery::Message reply{};
        detail::exchange(connection.file.get(),false,reply,connection.deadline,engine.owner.get());
        discovery::Admission grant;
        if (!discovery::decode(reply.data(),reply.size(),grant)) detail::stop(Result::Protocol,ERROR_INVALID_DATA);
        const auto granted=check(request,grant,engine.ownerPid);
        if (granted!=Result::Admitted) detail::stop(granted,ERROR_SUCCESS);
        // The owner (a verified approved image) duplicated these into this
        // process for this attempt only. Own them at once so that every
        // failure below closes them here.
        for (size_t i=0;i<handles.size();++i) {
            handles[i].reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(grant.handles[i])));
            DWORD flags=0;
            if (!GetHandleInformation(handles[i].get(),&flags)) {
                for (size_t j=0;j<=i;++j) handles[j].release(); // Not ours to close.
                detail::stop(Result::Protocol,ERROR_INVALID_HANDLE);
            }
        }
        give.armed=true;
        hook(Stage::Granted);
        discovery::Admission commit=request; commit.type=discovery::Type::Commit; commit.value=grant.value;
        bytes=discovery::encode(commit);
        detail::exchange(connection.file.get(),true,bytes,connection.deadline,engine.owner.get());
        hook(Stage::Committed);
        detail::exchange(connection.file.get(),false,reply,connection.deadline,engine.owner.get());
        discovery::Admission ack;
        if (!discovery::decode(reply.data(),reply.size(),ack) || ack.pid!=engine.ownerPid) detail::stop(Result::Protocol,ERROR_INVALID_DATA);
        const auto committed=check(commit,ack);
        if (committed!=Result::Admitted) detail::stop(committed,ERROR_SUCCESS);
        // Active on the owner. The client checks the owner and host images
        // again, independently, before the first request.
        EngineRegistration::DeploymentPolicy policy{engine.ownerDeployment,engine.hostDeployment};
        std::unique_ptr<EngineRegistration> registration;
        auto gone=[&] { return WaitForSingleObject(engine.owner.get(),0)!=WAIT_TIMEOUT || WaitForSingleObject(engine.host.get(),0)!=WAIT_TIMEOUT; };
        if (gone()) detail::stop(Result::Unavailable,ERROR_PROCESS_ABORTED); // Acknowledged, then died.
        try {
            registration=std::make_unique<EngineRegistration>(std::array<pipe::Handle,3>{std::move(handles[0]),std::move(handles[1]),std::move(handles[2])},
                engine.owner.get(),engine.host.get(),handles[3].get(),grant.value,std::move(policy));
        } catch (const pipe::Error &error) { detail::stop(gone() ? Result::Unavailable : Result::Untrusted,error.code); }
        catch (const std::invalid_argument &) { detail::stop(Result::Protocol,ERROR_INVALID_DATA); }
        connection.file.reset(); // The owner sees EOF and serves the next client.
        give.armed=false;
        outcome.session=std::make_unique<Session>(std::move(engine),std::move(registration),std::move(handles[3]));
        outcome.result=Result::Admitted; outcome.win32=ERROR_SUCCESS;
    } catch (const detail::Stop &failure) { outcome.result=failure.result; outcome.win32=failure.win32; }
    catch (const pipe::Error &error) { outcome.result=Result::Failed; outcome.win32=error.code; }
    catch (const std::exception &) { outcome.result=Result::Failed; outcome.win32=ERROR_INVALID_PARAMETER; }
    return outcome;
}
// Production entry: the protected HKLM release source only.
inline Outcome admit(const Options &options=Options{}) { return admit(release::read(),options); }

// Serializes starters of one scope in this logon session so that N
// concurrent starters launch one owner, not N owners racing for the
// instance lease. Not an authentication object: it only orders starters.
// Abandonment (a starter died mid-start) is harmless; the next holder
// discovers again before starting anything.
class StarterLock {
    pipe::Handle mutex_;
    bool held_=false;
public:
    static std::wstring name(const std::wstring &scope) {
        auto result=EngineInstance::name(scope);
        result.replace(result.find(L"OwnerLease"),10,L"Starter");
        return result;
    }
    explicit StarterLock(const std::wstring &scope) {
        const auto objectName=name(scope);
        const auto identity=pipe::LogonIdentity::fromProcess(GetCurrentProcess());
        constexpr DWORD access=SYNCHRONIZE | MUTEX_MODIFY_STATE;
        std::vector<uint8_t> bytes(sizeof(ACL)+sizeof(ACCESS_ALLOWED_ACE)-sizeof(DWORD)+identity.logon.size());
        auto *acl=reinterpret_cast<ACL *>(bytes.data());
        pipe::check(InitializeAcl(acl,static_cast<DWORD>(bytes.size()),ACL_REVISION));
        pipe::check(AddAccessAllowedAce(acl,ACL_REVISION,access,const_cast<uint8_t *>(identity.logon.data())));
        SECURITY_DESCRIPTOR descriptor{};
        pipe::check(InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION));
        pipe::check(SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE));
        pipe::check(SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),&descriptor,FALSE};
        mutex_.reset(CreateMutexExW(&attributes,objectName.c_str(),0,access));
        if (!mutex_) throw pipe::Error(pipe::Failure::Io,GetLastError());
    }
    StarterLock(const StarterLock &)=delete;
    StarterLock &operator=(const StarterLock &)=delete;
    ~StarterLock() { if (held_) ReleaseMutex(mutex_.get()); }
    bool acquire(DWORD ms) {
        const auto status=WaitForSingleObject(mutex_.get(),ms);
        if (status==WAIT_OBJECT_0 || status==WAIT_ABANDONED) { held_=true; return true; }
        if (status==WAIT_FAILED) throw pipe::Error(pipe::Failure::Io,GetLastError());
        return false;
    }
};

// Explicit starter for development and tests (the resident lifecycle is
// step 4): discover/admit; if nothing runs, take the starter lock, check
// again, start an owner with --discovery and admit again; transient results
// retry with bounded exponential backoff. Never spins, never starts while an
// engine answers, and never starts more than once per call.
enum class Start { Started, Exists, Failed };
struct AttachOptions {
    Options handoff;
    uint64_t budgetMs=10000;
    uint32_t maxAttempts=32;
    // Starts an owner publishing discovery under handoff.discovery.scope and
    // returns once it is Ready (Started), lost the instance race (Exists) or
    // failed. Empty: never start anything.
    std::function<Start(uint64_t deadline)> start;
};
struct AttachOutcome {
    Outcome outcome;
    uint32_t attempts=0, starts=0, waits=0;
};
inline bool retryable(Result result) {
    return result==Result::Busy || result==Result::Closing || result==Result::Unavailable || result==Result::Timeout;
}
inline AttachOutcome attach(const release::Snapshot &snapshot, const AttachOptions &options) {
    AttachOutcome out;
    if (!options.budgetMs || options.budgetMs>120000 || !options.maxAttempts) { out.outcome.win32=ERROR_INVALID_PARAMETER; return out; }
    const auto deadline=pipe::now()+options.budgetMs;
    auto attempt=[&]() {
        auto opts=options.handoff;
        const auto left=deadline>pipe::now() ? deadline-pipe::now() : 0;
        opts.discovery.timeoutMs=std::max<uint64_t>(1,std::min(opts.discovery.timeoutMs,left));
        ++out.attempts; return admit(snapshot,opts);
    };
    uint64_t backoff=10;
    std::unique_ptr<StarterLock> lock;
    while (out.attempts<options.maxAttempts && pipe::now()<deadline) {
        out.outcome=attempt();
        auto result=out.outcome.result;
        if (result==Result::NotRunning && options.start && !out.starts) {
            try {
                if (!lock) lock=std::make_unique<StarterLock>(options.handoff.discovery.scope);
                ++out.waits;
                if (!lock->acquire(discovery::detail::remaining(deadline))) { out.outcome=Outcome{}; out.outcome.result=Result::Busy; return out; }
            } catch (const pipe::Error &error) { out.outcome=Outcome{}; out.outcome.win32=error.code; return out; }
            // Another starter may have finished while we waited.
            out.outcome=attempt(); result=out.outcome.result;
            if (result==Result::NotRunning) {
                ++out.starts;
                const auto started=options.start(deadline);
                if (started==Start::Failed) { lock.reset(); out.outcome=Outcome{}; out.outcome.result=Result::NotRunning; return out; }
                out.outcome=attempt(); result=out.outcome.result;
            }
            lock.reset();
        }
        if (result==Result::Admitted || !(retryable(result) || (result==Result::NotRunning && out.starts))) return out;
        // Bounded exponential backoff with jitter, inside the budget.
        const auto left=deadline>pipe::now() ? deadline-pipe::now() : 0;
        if (!left) break;
        uint64_t jitter=0; BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&jitter),sizeof(jitter),BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        Sleep(static_cast<DWORD>(std::min<uint64_t>(backoff+jitter%backoff,left)));
        backoff=std::min<uint64_t>(backoff*2,200);
    }
    return out; // Budget or attempts exhausted: the last transient result.
}
} // namespace harukoto::engine::handoff
