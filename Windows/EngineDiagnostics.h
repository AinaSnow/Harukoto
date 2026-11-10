#pragma once
#include "EnginePipe.h"

// Observations only. Names, PIDs and every byte in this mapping are untrusted
// diagnostic data, NEVER a source of process/pipe/client capabilities.
namespace harukoto::engine::diagnostics {
constexpr size_t PacketSize = 256, MappingSize = PacketSize+8;
using Packet = std::array<uint8_t,PacketSize>;
struct ProcessIdentity { DWORD pid = 0; uint64_t created = 0; };
inline ProcessIdentity identity(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    const auto pid = GetProcessId(process); pipe::check(pid != 0);
    pipe::check(GetProcessTimes(process,&created,&exited,&kernel,&user));
    return {pid,(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime};
}
inline std::wstring name(ProcessIdentity root) {
    return L"Local\\Harukoto.EngineDiagnostic.v1."+std::to_wstring(root.pid)+L"."+std::to_wstring(root.created);
}
struct Capacity {
    uint64_t tick = 0, generation = 0;
    uint32_t limit = 0, listening = 0, connected = 0, retiring = 0, jobs = 0;
};
struct Record {
    ProcessIdentity root, publisher, host;
    bool owner = false;
    bool deploymentRequired = false, deploymentVerified = false;
    uint64_t published = 0, polled = 0;
    uint32_t state = 0, failure = 0, win32 = 0;
    std::optional<DWORD> exitCode;
    uint32_t configured = 0;
    uint64_t claimed = 0;
    uint32_t controlState = 0, controlFailure = 0, controlWin32 = 0;
    uint32_t admissionState = 0, admissionFailure = 0, admissionWin32 = 0;
    uint64_t registrationConfigured = 0, registrationIssued = 0, registrationBound = 0,
        registrationRevoked = 0, registrationCreated = 0, registrationActive = 0;
    Capacity capacity;
};
inline bool valid(const Record &r) {
    if (!r.root.pid || !r.root.created || !r.publisher.pid || !r.publisher.created ||
            bool(r.host.pid) != bool(r.host.created) || !r.published || r.polled > r.published ||
            r.state > 4 || r.failure > 7 || (r.state == 4) != bool(r.exitCode) ||
            (r.deploymentVerified && !r.deploymentRequired) ||
            !r.configured || r.configured > 8 || r.controlState > 4 || r.controlFailure > 5 ||
            r.admissionState > 4 || r.admissionFailure > 5 ||
            (!r.controlState && (r.controlFailure || r.controlWin32)) ||
            (!r.admissionState && (r.admissionFailure || r.admissionWin32)) ||
            r.registrationConfigured > 8 || r.registrationBound > r.registrationActive ||
            r.registrationActive > 8 || r.registrationCreated < r.registrationConfigured ||
            r.registrationIssued > r.registrationCreated || r.registrationRevoked > r.registrationCreated ||
            r.registrationActive > r.registrationCreated ||
            (!r.owner && (r.admissionState || r.registrationCreated))) return false;
    const auto &c = r.capacity;
    if (!c.tick) return !c.generation && !c.limit && !c.listening && !c.connected && !c.retiring && !c.jobs;
    return c.tick <= r.published && c.generation && c.limit == r.configured && c.limit <= 8 &&
        c.listening <= c.limit && c.connected <= c.limit && c.retiring <= c.limit &&
        c.listening+c.connected+c.retiring <= c.limit && c.jobs <= 256 && r.controlState;
}
inline Packet encode(const Record &r) {
    if (!valid(r)) throw std::invalid_argument("invalid engine diagnostic record");
    Packet p{}; size_t offset = 0;
    auto put = [&](uint64_t n, size_t bytes) { for (size_t i=0;i<bytes;++i) p[offset++] = static_cast<uint8_t>(n>>(i*8)); };
    for (auto c : {'H','K','D','S'}) put(c,1);
    put(1,2); put(PacketSize,2);
    for (auto id : {r.root,r.publisher,r.host}) { put(id.pid,4); put(id.created,8); }
    put(uint32_t(r.owner) | (uint32_t(r.deploymentRequired)<<1) | (uint32_t(r.deploymentVerified)<<2),4);
    put(r.published,8); put(r.polled,8);
    for (auto n : std::array<uint32_t,6>{r.state,r.failure,r.win32,uint32_t(bool(r.exitCode)),r.exitCode.value_or(0),r.configured}) put(n,4);
    put(r.claimed,8);
    for (auto n : {r.controlState,r.controlFailure,r.controlWin32,r.admissionState,r.admissionFailure,r.admissionWin32}) put(n,4);
    for (auto n : {r.registrationConfigured,r.registrationIssued,r.registrationBound,r.registrationRevoked,r.registrationCreated,r.registrationActive}) put(n,8);
    put(r.capacity.tick,8); put(r.capacity.generation,8);
    for (auto n : {r.capacity.limit,r.capacity.listening,r.capacity.connected,r.capacity.retiring,r.capacity.jobs}) put(n,4);
    return p;
}
inline bool decode(const Packet &p, Record &out) {
    if (p[0]!='H' || p[1]!='K' || p[2]!='D' || p[3]!='S' || get(p.data(),4,2)!=1 || get(p.data(),6,2)!=PacketSize) return false;
    size_t offset = 8;
    auto take = [&](size_t bytes) { const auto value = get(p.data(),offset,bytes); offset += bytes; return value; };
    auto word = [&] { return static_cast<uint32_t>(take(4)); };
    Record r;
    for (auto *id : {&r.root,&r.publisher,&r.host}) { id->pid=word(); id->created=take(8); }
    const auto flags=word(); if (flags>7) return false;
    r.owner=bool(flags&1); r.deploymentRequired=bool(flags&2); r.deploymentVerified=bool(flags&4);
    r.published=take(8); r.polled=take(8);
    r.state=word(); r.failure=word(); r.win32=word();
    const auto hasExit=word(), exit=word(); if (hasExit>1 || (!hasExit && exit)) return false;
    if (hasExit) r.exitCode=exit;
    r.configured=word(); r.claimed=take(8);
    r.controlState=word(); r.controlFailure=word(); r.controlWin32=word();
    r.admissionState=word(); r.admissionFailure=word(); r.admissionWin32=word();
    for (auto *n : {&r.registrationConfigured,&r.registrationIssued,&r.registrationBound,&r.registrationRevoked,&r.registrationCreated,&r.registrationActive}) *n=take(8);
    r.capacity.tick=take(8); r.capacity.generation=take(8);
    r.capacity.limit=word(); r.capacity.listening=word(); r.capacity.connected=word(); r.capacity.retiring=word(); r.capacity.jobs=word();
    if (!valid(r) || std::any_of(p.begin()+offset,p.end(),[](auto b) { return b!=0; })) return false;
    out=r; return true;
}
struct Shared { volatile LONG sequence; LONG reserved; uint8_t packet[PacketSize]; };
static_assert(sizeof(Shared)==MappingSize);
class Writer {
    pipe::Handle mapping_;
    Shared *view_ = nullptr;
public:
    explicit Writer(ProcessIdentity root) {
        pipe::processIdentityOnly();
        auto logon = pipe::LogonIdentity::fromProcess(GetCurrentProcess());
        std::vector<uint8_t> bytes(sizeof(ACL)+sizeof(ACCESS_ALLOWED_ACE)-sizeof(DWORD)+logon.logon.size());
        auto *acl = reinterpret_cast<ACL *>(bytes.data());
        pipe::check(InitializeAcl(acl,static_cast<DWORD>(bytes.size()),ACL_REVISION));
        pipe::check(AddAccessAllowedAce(acl,ACL_REVISION,SECTION_MAP_READ | READ_CONTROL,logon.logon.data()));
        SECURITY_DESCRIPTOR descriptor{};
        pipe::check(InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION));
        pipe::check(SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE));
        pipe::check(SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),&descriptor,FALSE};
        const auto objectName = name(root);
        SetLastError(ERROR_SUCCESS);
        mapping_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE,0,MappingSize,objectName.c_str()));
        const auto error = GetLastError(); pipe::check(bool(mapping_));
        if (error==ERROR_ALREADY_EXISTS) throw pipe::Error(pipe::Failure::Io,error); // Never adopt precreated data.
        view_=static_cast<Shared *>(MapViewOfFile(mapping_.get(),FILE_MAP_READ | FILE_MAP_WRITE,0,0,MappingSize));
        pipe::check(view_!=nullptr);
    }
    ~Writer() { if (view_) UnmapViewOfFile(view_); }
    Writer(const Writer &)=delete;
    Writer &operator=(const Writer &)=delete;
    void publish(const Record &record) {
        const auto packet=encode(record);
        InterlockedIncrement(&view_->sequence);
        std::copy(packet.begin(),packet.end(),view_->packet);
        InterlockedIncrement(&view_->sequence);
    }
};
enum class Availability { Observed, Unavailable, Inconsistent, Invalid, ProcessExited, PublisherUnavailable };
struct Observation { Availability availability=Availability::Unavailable; DWORD win32=0; std::optional<Record> record; };
// Bounded reads only: no events, control requests, process creation or retries
// beyond three seqlock samples. Opening PID objects here is for display only.
inline Observation read(HANDLE root) {
    Observation result;
    const auto rootId=identity(root);
    pipe::Handle mapping(OpenFileMappingW(FILE_MAP_READ,FALSE,name(rootId).c_str()));
    if (!mapping) { result.win32=GetLastError(); return result; }
    const auto *view=static_cast<const Shared *>(MapViewOfFile(mapping.get(),FILE_MAP_READ,0,0,MappingSize));
    if (!view) { result.win32=GetLastError(); return result; }
    struct Unmap { const void *view; ~Unmap() { UnmapViewOfFile(view); } } unmap{view};
    result.availability=Availability::Inconsistent;
    for (unsigned attempt=0;attempt<3;++attempt) {
        const LONG sequence=view->sequence; MemoryBarrier();
        if (!sequence || (sequence & 1)) continue;
        Packet packet; std::copy_n(view->packet,PacketSize,packet.begin());
        const auto reserved=view->reserved; MemoryBarrier();
        if (sequence!=view->sequence) continue;
        Record r;
        if (reserved || !decode(packet,r) || r.root.pid!=rootId.pid || r.root.created!=rootId.created ||
                r.published>GetTickCount64()) { result.availability=Availability::Invalid; return result; }
        result.record=r;
        const auto alive=WaitForSingleObject(root,0);
        if (alive==WAIT_FAILED) { result.availability=Availability::Unavailable; result.win32=GetLastError(); return result; }
        if (alive==WAIT_OBJECT_0) { result.availability=Availability::ProcessExited; return result; }
        pipe::Handle publisher(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,r.publisher.pid));
        result.availability=Availability::PublisherUnavailable;
        if (!publisher) { result.win32=GetLastError(); return result; }
        const auto publisherId=identity(publisher.get());
        if (publisherId.created!=r.publisher.created || WaitForSingleObject(publisher.get(),0)!=WAIT_TIMEOUT) return result;
        // Even an Observed result is self-reported metadata, not authentication,
        // a heartbeat, or proof that the I/O thread is still making progress.
        result.availability=Availability::Observed; return result;
    }
    return result;
}
} // namespace harukoto::engine::diagnostics
