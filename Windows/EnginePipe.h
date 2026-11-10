#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include "EngineProtocol.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>

namespace harukoto::engine::pipe {
enum class Failure { None, Closed, Io, Protocol, Authentication, Deadline };
class Error : public std::runtime_error {
public:
    Failure reason;
    DWORD code;
    Error(Failure failure, DWORD win32) : std::runtime_error("engine pipe failure"), reason(failure), code(win32) {}
};
class Handle {
    HANDLE value_ = nullptr;
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept { if (this != &other) reset(other.release()); return *this; }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { const auto value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) { if (*this) CloseHandle(value_); value_ = value; }
};
// Match Executor's default millisecond clock. Never mix absolute clock epochs.
inline uint64_t now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void check(BOOL ok, Failure reason = Failure::Io) { if (!ok) throw Error(reason, GetLastError()); }
inline void processIdentityOnly() {
    HANDLE raw = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) {
        CloseHandle(raw); throw Error(Failure::Authentication, ERROR_BAD_IMPERSONATION_LEVEL);
    }
    if (GetLastError() != ERROR_NO_TOKEN) throw Error(Failure::Authentication, GetLastError());
}
inline std::vector<uint8_t> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || size > 65536)
        throw Error(Failure::Authentication, ERROR_INVALID_DATA);
    std::vector<uint8_t> result(size);
    check(GetTokenInformation(token, kind, result.data(), size, &size), Failure::Authentication); return result;
}
struct LogonIdentity {
    std::vector<uint8_t> user, logon;
    LUID authentication{};
    DWORD session = 0;
    static std::vector<uint8_t> copy(PSID sid) {
        if (!IsValidSid(sid)) throw Error(Failure::Authentication, ERROR_INVALID_SID);
        std::vector<uint8_t> bytes(GetLengthSid(sid));
        check(CopySid(static_cast<DWORD>(bytes.size()), bytes.data(), sid), Failure::Authentication); return bytes;
    }
    static LogonIdentity fromToken(HANDLE token) {
        LogonIdentity id;
        const auto user = tokenInfo(token, TokenUser), groups = tokenInfo(token, TokenGroups);
        id.user = copy(reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid);
        const auto *list = reinterpret_cast<const TOKEN_GROUPS *>(groups.data());
        for (DWORD i = 0; i < list->GroupCount; ++i)
            if ((list->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
                id.logon = copy(list->Groups[i].Sid); break;
            }
        if (id.logon.empty()) throw Error(Failure::Authentication, ERROR_NO_SUCH_LOGON_SESSION);
        const auto stats = tokenInfo(token, TokenStatistics), session = tokenInfo(token, TokenSessionId);
        id.authentication = reinterpret_cast<const TOKEN_STATISTICS *>(stats.data())->AuthenticationId;
        id.session = *reinterpret_cast<const DWORD *>(session.data()); return id;
    }
    static LogonIdentity fromProcess(HANDLE process) {
        HANDLE raw = nullptr; check(OpenProcessToken(process, TOKEN_QUERY, &raw), Failure::Authentication);
        Handle token(raw); return fromToken(token.get());
    }
    bool operator==(const LogonIdentity &other) const {
        return user == other.user && logon == other.logon && session == other.session &&
            authentication.HighPart == other.authentication.HighPart && authentication.LowPart == other.authentication.LowPart;
    }
};
constexpr DWORD ClientAccess = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
inline std::wstring newName() {
    std::array<uint8_t,32> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw Error(Failure::Io, ERROR_GEN_FAILURE);
    std::wstring name = L"\\\\.\\pipe\\Harukoto.Engine.v1.";
    for (auto byte : nonce) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
    return name;
}
inline bool validName(const std::wstring &name) {
    const std::wstring prefix = L"\\\\.\\pipe\\Harukoto.Engine.v1.";
    return name.size() == prefix.size() + 64 && name.compare(0, prefix.size(), prefix) == 0 &&
        std::all_of(name.begin() + prefix.size(), name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
}
struct Operation {
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED overlapped{};
    bool pending = false;
    Operation() { check(bool(event)); overlapped.hEvent = event.get(); }
    void reset() { ResetEvent(event.get()); overlapped = {}; overlapped.hEvent = event.get(); }
    void cancel(HANDLE pipe) { if (pending) CancelIoEx(pipe, &overlapped); }
    bool drain(HANDLE pipe, bool wait) {
        if (!pending) return true;
        DWORD transferred = 0;
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, wait) && GetLastError() == ERROR_IO_INCOMPLETE) return false;
        pending = false; return true;
    }
};
class Listener;
// One service I/O owner thread. Nonmovable: kernel OVERLAPPED pointers stay stable.
class Channel {
    friend class Listener;
    Handle pipe_, serverProcess_;
    LogonIdentity local_;
    Operation read_, write_;
    std::vector<uint8_t> incoming_, outgoing_;
    size_t readPosition_ = 0, writePosition_ = 0;
    uint64_t readDeadline_ = 0, writeDeadline_ = 0;
    bool reading_ = false, writing_ = false, writeDone_ = false, server_ = false, authenticated_ = false;
    std::optional<Frame> received_;
    Failure failure_ = Failure::None;
    DWORD error_ = ERROR_SUCCESS;
    explicit Channel(Handle pipe, LogonIdentity identity, bool server, Handle process = Handle{})
        : pipe_(std::move(pipe)), serverProcess_(std::move(process)), local_(std::move(identity)),
          server_(server), authenticated_(!server) { incoming_.reserve(HeaderSize + MaxPayload); }
    bool authenticate() {
        try {
            processIdentityOnly();
            check(ImpersonateNamedPipeClient(pipe_.get()), Failure::Authentication);
            struct Revert { ~Revert() { if (!RevertToSelf()) std::terminate(); } } revert;
            HANDLE raw = nullptr; check(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw), Failure::Authentication);
            Handle token(raw); return LogonIdentity::fromToken(token.get()) == local_;
        } catch (...) { return false; }
    }
    bool completeRead() {
        Frame frame; size_t consumed = 0;
        const auto status = decode(incoming_.data(), incoming_.size(), frame, consumed);
        if (status == Decode::Invalid) { close(Failure::Protocol); return false; }
        if (status == Decode::Incomplete) {
            // decode already checked the entire header before this allocation.
            incoming_.resize(HeaderSize + static_cast<size_t>(get(incoming_.data(), 8, 4))); return true;
        }
        if (consumed != incoming_.size()) { close(Failure::Protocol); return false; }
        if (server_ && !authenticated_) {
            if (frame.kind != Kind::Hello || !frame.payload.empty()) { close(Failure::Protocol); return false; }
            if (!authenticate()) { close(Failure::Authentication); return false; }
            authenticated_ = true;
        }
        received_ = std::move(frame); reading_ = false; return true;
    }
    void pumpRead() {
        for (int budget = 0; reading_ && !closed() && budget < 8; ++budget) {
            if (!read_.pending) {
                read_.reset();
                const auto issued = ReadFile(pipe_.get(), incoming_.data() + readPosition_,
                    static_cast<DWORD>(incoming_.size() - readPosition_), nullptr, &read_.overlapped);
                if (!issued && GetLastError() != ERROR_IO_PENDING) { close(Failure::Io, GetLastError()); return; }
                read_.pending = true;
            }
            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe_.get(), &read_.overlapped, &transferred, FALSE)) {
                const auto error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) return;
                read_.pending = false; close(Failure::Io, error); return;
            }
            read_.pending = false;
            if (!transferred || transferred > incoming_.size() - readPosition_) { close(Failure::Io, ERROR_BROKEN_PIPE); return; }
            readPosition_ += transferred;
            if (readPosition_ == incoming_.size() && !completeRead()) return;
        }
    }
    void pumpWrite() {
        for (int budget = 0; writing_ && !closed() && budget < 8; ++budget) {
            if (!write_.pending) {
                write_.reset();
                const auto issued = WriteFile(pipe_.get(), outgoing_.data() + writePosition_,
                    static_cast<DWORD>(outgoing_.size() - writePosition_), nullptr, &write_.overlapped);
                if (!issued && GetLastError() != ERROR_IO_PENDING) { close(Failure::Io, GetLastError()); return; }
                write_.pending = true;
            }
            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe_.get(), &write_.overlapped, &transferred, FALSE)) {
                const auto error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) return;
                write_.pending = false; close(Failure::Io, error); return;
            }
            write_.pending = false;
            if (!transferred || transferred > outgoing_.size() - writePosition_) { close(Failure::Io, ERROR_BROKEN_PIPE); return; }
            writePosition_ += transferred;
            if (writePosition_ == outgoing_.size()) { writing_ = false; writeDone_ = true; }
        }
    }
public:
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    ~Channel() { close(); read_.drain(pipe_.get(), true); write_.drain(pipe_.get(), true); }
    // expectedServer is a retained trusted launch handle, NOT a PID received
    // from the pipe/registry. Pin its kernel object before connecting by name.
    static std::unique_ptr<Channel> connect(const std::wstring &name, HANDLE expectedServer) {
        if (!validName(name)) throw Error(Failure::Authentication, ERROR_INVALID_NAME);
        processIdentityOnly();
        HANDLE rawProcess = nullptr;
        check(DuplicateHandle(GetCurrentProcess(), expectedServer, GetCurrentProcess(), &rawProcess, 0, FALSE, DUPLICATE_SAME_ACCESS), Failure::Authentication);
        Handle process(rawProcess);
        const auto pid = GetProcessId(process.get());
        if (!pid || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) throw Error(Failure::Authentication, ERROR_PROCESS_ABORTED);
        auto local = LogonIdentity::fromProcess(GetCurrentProcess());
        if (!(LogonIdentity::fromProcess(process.get()) == local)) throw Error(Failure::Authentication, ERROR_ACCESS_DENIED);
        Handle handle(CreateFileW(name.c_str(), ClientAccess, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        check(bool(handle));
        ULONG actual = 0;
        check(GetNamedPipeServerProcessId(handle.get(), &actual), Failure::Authentication);
        if (actual != pid || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) throw Error(Failure::Authentication, ERROR_ACCESS_DENIED);
        return std::unique_ptr<Channel>(new Channel(std::move(handle), std::move(local), false, std::move(process)));
    }
    bool beginRead(uint64_t time, uint64_t deadline) {
        if (closed() || reading_ || received_) return false;
        if (deadline <= time) { close(Failure::Deadline); return false; }
        incoming_.resize(HeaderSize); readPosition_ = 0; readDeadline_ = deadline; reading_ = true; return true;
    }
    bool beginWrite(const Frame &frame, uint64_t time, uint64_t deadline) {
        if (closed() || writing_ || !authenticated_) return false;
        if (deadline <= time) { close(Failure::Deadline); return false; }
        auto bytes = encode(frame);
        outgoing_ = std::move(bytes); writePosition_ = 0; writeDeadline_ = deadline; writing_ = true; writeDone_ = false; return true;
    }
    void poll(uint64_t time) {
        if (closed()) { drain(); return; }
        if ((reading_ && time >= readDeadline_) || (writing_ && time >= writeDeadline_)) { close(Failure::Deadline); return; }
        try { pumpRead(); pumpWrite(); } catch (...) { close(Failure::Io, ERROR_NOT_ENOUGH_MEMORY); }
    }
    std::optional<Frame> take() { auto result = std::move(received_); received_.reset(); return result; }
    bool authenticated() const { return authenticated_; }
    bool closed() const { return failure_ != Failure::None; }
    Failure failure() const { return failure_; }
    DWORD error() const { return error_; }
    bool writeDone() const { return writeDone_; }
    HANDLE readEvent() const { return read_.pending ? read_.event.get() : nullptr; }
    HANDLE writeEvent() const { return write_.pending ? write_.event.get() : nullptr; }
    HANDLE nativeHandle() const { return pipe_.get(); } // Borrowed for diagnostics/fault-injection tests.
    void close(Failure reason = Failure::Closed, DWORD code = ERROR_SUCCESS) {
        if (!closed()) { failure_ = reason == Failure::None ? Failure::Closed : reason; error_ = code; }
        received_.reset(); reading_ = writing_ = false;
        read_.cancel(pipe_.get()); write_.cancel(pipe_.get());
    }
    bool drain() {
        if (!closed()) return false; // Never consume a live operation's completion here.
        const bool read = read_.drain(pipe_.get(), false), write = write_.drain(pipe_.get(), false);
        if (closed() && read && write) pipe_.reset();
        return read && write;
    }
};
class Listener {
    Handle pipe_;
    LogonIdentity local_;
    Operation connect_;
    uint64_t deadline_;
    bool connected_ = false, closed_ = false;
public:
    Listener(const std::wstring &name, uint64_t time, uint64_t deadline) : deadline_(deadline) {
        if (!validName(name) || deadline <= time) throw Error(Failure::Protocol, ERROR_INVALID_PARAMETER);
        processIdentityOnly(); local_ = LogonIdentity::fromProcess(GetCurrentProcess());
        std::vector<uint8_t> aclBytes(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + local_.logon.size());
        auto *acl = reinterpret_cast<ACL *>(aclBytes.data());
        check(InitializeAcl(acl, static_cast<DWORD>(aclBytes.size()), ACL_REVISION));
        check(AddAccessAllowedAce(acl, ACL_REVISION, ClientAccess, local_.logon.data()));
        SECURITY_DESCRIPTOR descriptor{};
        check(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
        check(SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE));
        check(SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
        pipe_.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
            static_cast<DWORD>(HeaderSize + MaxPayload), static_cast<DWORD>(HeaderSize + MaxPayload), 0, &attributes));
        check(bool(pipe_));
        if (ConnectNamedPipe(pipe_.get(), &connect_.overlapped)) connected_ = true;
        else {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) connected_ = true;
            else if (error == ERROR_IO_PENDING) connect_.pending = true;
            else throw Error(Failure::Io, error);
        }
    }
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;
    ~Listener() { connect_.cancel(pipe_.get()); connect_.drain(pipe_.get(), true); }
    void poll(uint64_t time) {
        if (closed_ || connected_) return;
        if (time >= deadline_) { closed_ = true; connect_.cancel(pipe_.get()); return; }
        DWORD transferred = 0;
        if (GetOverlappedResult(pipe_.get(), &connect_.overlapped, &transferred, FALSE)) { connect_.pending = false; connected_ = true; }
        else if (GetLastError() != ERROR_IO_INCOMPLETE) { connect_.pending = false; closed_ = true; }
    }
    bool closed() const { return closed_; }
    HANDLE event() const { return connect_.pending ? connect_.event.get() : nullptr; }
    HANDLE nativeHandle() const { return pipe_.get(); }
    std::unique_ptr<Channel> take() {
        if (!connected_ || closed_) return {};
        closed_ = true;
        return std::unique_ptr<Channel>(new Channel(std::move(pipe_), std::move(local_), true));
    }
};
} // namespace harukoto::engine::pipe
