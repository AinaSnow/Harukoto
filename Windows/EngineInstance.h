#pragma once
#include "EnginePipe.h"

namespace harukoto::engine {
// Existence lease only: never wait on or own the mutex, and never use its name
// to authenticate/discover a server. Scope is trusted deployment configuration.
class EngineInstance {
    pipe::Handle lease_;
public:
    class Busy : public std::runtime_error {
    public: Busy() : std::runtime_error("engine instance already exists") {}
    };
    static bool validScope(const std::wstring &scope) {
        return !scope.empty() && scope.size() <= 64 &&
            std::all_of(scope.begin(),scope.end(),[](wchar_t c) {
                return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-';
            });
    }
    static std::wstring name(const std::wstring &scope) {
        if (!validScope(scope)) throw std::invalid_argument("invalid engine instance scope");
        pipe::processIdentityOnly();
        const auto identity = pipe::LogonIdentity::fromProcess(GetCurrentProcess());
        std::wstring result = L"Local\\Harukoto.OwnerLease.v1.";
        auto hex = [&](uint32_t value) {
            for (int shift=28;shift>=0;shift-=4) result += L"0123456789abcdef"[(value >> shift) & 15];
        };
        hex(identity.session); hex(static_cast<uint32_t>(identity.authentication.HighPart)); hex(identity.authentication.LowPart);
        result += L'.';
        for (auto byte : identity.logon) { result += L"0123456789abcdef"[byte >> 4]; result += L"0123456789abcdef"[byte & 15]; }
        result += L'.'; result += scope;
        if (result.size() >= MAX_PATH) throw std::invalid_argument("engine instance name too long");
        return result;
    }
    explicit EngineInstance(const std::wstring &scope) {
        const auto objectName = name(scope);
        auto identity = pipe::LogonIdentity::fromProcess(GetCurrentProcess());
        constexpr DWORD access = SYNCHRONIZE | READ_CONTROL;
        std::vector<uint8_t> bytes(sizeof(ACL)+sizeof(ACCESS_ALLOWED_ACE)-sizeof(DWORD)+identity.logon.size());
        auto *acl = reinterpret_cast<ACL *>(bytes.data());
        pipe::check(InitializeAcl(acl,static_cast<DWORD>(bytes.size()),ACL_REVISION));
        pipe::check(AddAccessAllowedAce(acl,ACL_REVISION,access,identity.logon.data()));
        SECURITY_DESCRIPTOR descriptor{};
        pipe::check(InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION));
        pipe::check(SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE));
        pipe::check(SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),&descriptor,FALSE};
        SetLastError(ERROR_SUCCESS);
        lease_.reset(CreateMutexExW(&attributes,objectName.c_str(),0,access));
        const auto error = GetLastError();
        if (!lease_) throw pipe::Error(pipe::Failure::Io,error);
        if (error == ERROR_ALREADY_EXISTS) throw Busy(); // Never adopt an existing object.
    }
    HANDLE handle() const { return lease_.get(); }
};
} // namespace harukoto::engine
