#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// WIN-03 step 1: the only trusted source of approved engine deployments.
// The elevated installer writes one REG_MULTI_SZ value per installed version
// under HKLM (64-bit view). Readers never take paths or digests from a
// candidate process, diagnostics, environment variables, HKCU or a file next
// to the candidate. Anything unexpected is not approved (fail closed).
// All calls are blocking system calls: run them on a service / I/O thread,
// never inside a TSF foreground callback.
namespace harukoto::engine::release {
constexpr wchar_t DefaultKey[]=L"SOFTWARE\\Harukoto\\EngineReleases";
constexpr wchar_t FormatLine[]=L"format=harukoto-engine-release/1";
constexpr wchar_t FormatPrefix[]=L"format=harukoto-engine-release/";
constexpr wchar_t HostName[]=L"harukoto_engine_host.exe";
constexpr wchar_t OwnerName[]=L"harukoto_engine_owner.exe";
constexpr size_t MaxEntries=8, MaxIdLength=240, MaxPathLength=1024, MaxValueBytes=8192, MaxLines=16;
constexpr uint64_t MaxSequence=9007199254740991ull;

struct Entry {
    std::wstring id;
    uint64_t sequence=0;
    uint16_t protocol=0;
    std::filesystem::path host, owner;
    std::string hostSha256, ownerSha256;
};
enum class Parse { Ok, Unsupported, Invalid };
enum class Status { Ready, Missing, Unprotected, Invalid };
struct Snapshot {
    Status status=Status::Invalid;
    DWORD error=0;
    std::vector<Entry> entries;
    size_t rejected=0, unsupported=0;
};
// Production callers use the defaults. Tests inject an HKCU subkey and may
// disable the protection checks, which a user-writable location cannot pass.
struct Source {
    HKEY root=HKEY_LOCAL_MACHINE;
    std::wstring subkey=DefaultKey;
    bool requireProtected=true;
};
enum class Object { RegistryKey, Directory };

namespace detail {
struct Key {
    HKEY value=nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~File() { if (value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Local {
    void *value=nullptr;
    ~Local() { if (value) LocalFree(value); }
};
inline bool equalNoCase(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
}
inline bool trustedWriter(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid,WinLocalSystemSid) || IsWellKnownSid(sid,WinBuiltinAdministratorsSid)) return true;
    Local installer;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&installer.value)) return false;
    return EqualSid(sid,installer.value)!=FALSE;
}
inline bool decimal(std::wstring_view text, uint64_t limit, uint64_t &out) {
    if (text.empty() || text.size()>16 || text[0]==L'0') return false;
    uint64_t value=0;
    for (auto c : text) {
        if (c<L'0' || c>L'9') return false;
        value=value*10+static_cast<uint64_t>(c-L'0');
    }
    if (value>limit) return false;
    out=value; return true;
}
inline bool digest(std::wstring_view text, std::string &out) {
    if (text.size()!=64) return false;
    std::string value;
    for (auto c : text) {
        if (!((c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f'))) return false;
        value.push_back(static_cast<char>(c));
    }
    out=std::move(value); return true;
}
} // namespace detail

// Version directory names written by the installer, e.g. upper-case hash
// groups joined by '-'.
inline bool validId(std::wstring_view id) {
    if (id.empty() || id.size()>MaxIdLength) return false;
    for (size_t i=0;i<id.size();++i) {
        const auto c=id[i];
        const bool alnum=(c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'z') || (c>=L'A' && c<=L'Z');
        if (!alnum && (i==0 || (c!=L'.' && c!=L'-' && c!=L'_'))) return false;
    }
    return true;
}
// Canonical local DOS path only: drive letter, backslashes, no relative,
// device, UNC, stream or trailing-dot/space segments.
inline bool validPath(std::wstring_view path) {
    if (path.size()<4 || path.size()>MaxPathLength) return false;
    const auto drive=path[0];
    if (!((drive>=L'A' && drive<=L'Z') || (drive>=L'a' && drive<=L'z')) || path[1]!=L':' || path[2]!=L'\\') return false;
    size_t start=3;
    for (;;) {
        const auto end=path.find(L'\\',start);
        const auto segment=path.substr(start,end==std::wstring_view::npos ? std::wstring_view::npos : end-start);
        if (segment.empty() || segment==L"." || segment==L".." || segment.back()==L'.' || segment.back()==L' ') return false;
        for (auto c : segment) {
            if (c<0x20 || c==0x7f || c==L'/' || c==L':' || c==L'*' || c==L'?' || c==L'"' || c==L'<' || c==L'>' || c==L'|') return false;
        }
        if (end==std::wstring_view::npos) break;
        start=end+1;
    }
    return true;
}

// Strict, fixed-order parse of one value. `name` is the registry value name.
// A different format generation is Unsupported (skipped, not an error) so an
// older reader tolerates entries from a newer installer.
inline Parse parse(std::wstring_view name, const std::vector<std::wstring> &lines, Entry &out) {
    if (lines.empty() || lines.size()>MaxLines) return Parse::Invalid;
    if (lines[0]!=FormatLine) {
        const std::wstring_view prefix=FormatPrefix;
        uint64_t generation=0;
        if (std::wstring_view(lines[0]).substr(0,prefix.size())==prefix &&
                detail::decimal(std::wstring_view(lines[0]).substr(prefix.size()),65535,generation)) return Parse::Unsupported;
        return Parse::Invalid;
    }
    static const wchar_t *const keys[]={L"format=",L"id=",L"sequence=",L"protocol=",L"host=",L"host-sha256=",L"owner=",L"owner-sha256="};
    constexpr size_t count=sizeof(keys)/sizeof(keys[0]);
    if (lines.size()!=count) return Parse::Invalid;
    std::wstring_view field[count];
    for (size_t i=0;i<count;++i) {
        const std::wstring_view key=keys[i], line=lines[i];
        if (line.size()<=key.size() || line.substr(0,key.size())!=key) return Parse::Invalid;
        field[i]=line.substr(key.size());
    }
    Entry entry;
    uint64_t value=0;
    if (!validId(name) || field[1]!=name) return Parse::Invalid;
    entry.id=std::wstring(name);
    if (!detail::decimal(field[2],MaxSequence,value)) return Parse::Invalid;
    entry.sequence=value;
    if (!detail::decimal(field[3],65535,value)) return Parse::Invalid;
    entry.protocol=static_cast<uint16_t>(value);
    if (!validPath(field[4]) || !validPath(field[6])) return Parse::Invalid;
    if (!detail::digest(field[5],entry.hostSha256) || !detail::digest(field[7],entry.ownerSha256)) return Parse::Invalid;
    const std::filesystem::path host{std::wstring(field[4])}, owner{std::wstring(field[6])};
    // Both programs live directly in the version directory named by id.
    if (host.filename().native()!=HostName || owner.filename().native()!=OwnerName) return Parse::Invalid;
    const auto folder=host.parent_path().native();
    if (!detail::equalNoCase(folder,owner.parent_path().native()) ||
            !detail::equalNoCase(host.parent_path().filename().native(),entry.id) || folder.size()<=3) return Parse::Invalid;
    entry.host=host; entry.owner=owner;
    out=std::move(entry);
    return Parse::Ok;
}

// REG_MULTI_SZ bytes: even length, strings terminated by an empty string, no
// trailing bytes after the double NUL.
inline bool splitMulti(const std::vector<BYTE> &data, std::vector<std::wstring> &lines) {
    if (data.size()<4 || data.size()%2 || data.size()>MaxValueBytes) return false;
    std::wstring text(data.size()/2,L'\0');
    std::memcpy(text.data(),data.data(),data.size());
    std::vector<std::wstring> result;
    size_t start=0;
    for (;;) {
        const auto end=text.find(L'\0',start);
        if (end==std::wstring::npos) return false;
        if (end==start) { if (end+1!=text.size()) return false; break; }
        if (result.size()==MaxLines) return false;
        result.emplace_back(text.substr(start,end-start));
        start=end+1;
    }
    if (result.empty()) return false;
    lines=std::move(result); return true;
}

// Owner and every effective (not inherit-only) allow ACE that grants a write
// right must be SYSTEM, Administrators or TrustedInstaller. Deny ACEs never
// widen access and are ignored; a missing or NULL DACL is unprotected.
inline bool protectedDescriptor(PSECURITY_DESCRIPTOR descriptor, Object object) {
    if (!descriptor || !IsValidSecurityDescriptor(descriptor)) return false;
    PSID owner=nullptr; BOOL defaulted=FALSE;
    if (!GetSecurityDescriptorOwner(descriptor,&owner,&defaulted) || !detail::trustedWriter(owner)) return false;
    BOOL present=FALSE; PACL dacl=nullptr;
    if (!GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted) || !present || !dacl) return false;
    const ACCESS_MASK write=GENERIC_WRITE | GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER |
        (object==Object::RegistryKey ? (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_CREATE_LINK)
            : (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_WRITE_EA | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES));
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl,&info,sizeof(info),AclSizeInformation)) return false;
    for (DWORD i=0;i<info.AceCount;++i) {
        void *raw=nullptr;
        if (!GetAce(dacl,i,&raw)) return false;
        const auto header=static_cast<ACE_HEADER *>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        switch (header->AceType) {
        case ACCESS_ALLOWED_ACE_TYPE: {
            const auto ace=static_cast<ACCESS_ALLOWED_ACE *>(raw);
            // CREATOR OWNER / GROUP are inheritance placeholders that no
            // token contains, so on the object itself they grant nothing.
            const PSID sid=&ace->SidStart;
            if (IsWellKnownSid(sid,WinCreatorOwnerSid) || IsWellKnownSid(sid,WinCreatorGroupSid)) break;
            if ((ace->Mask & write) && !detail::trustedWriter(sid)) return false;
            break;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
        case ACCESS_ALLOWED_COMPOUND_ACE_TYPE:
            // The mask follows the header for every allow type. Do not
            // interpret conditions or object GUIDs: any write here rejects.
            if (static_cast<ACCESS_ALLOWED_ACE *>(raw)->Mask & write) return false;
            break;
        default: break;
        }
    }
    return true;
}
inline bool protectedHandle(HANDLE handle, SE_OBJECT_TYPE type, Object object) {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if (GetSecurityInfo(handle,type,OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            nullptr,nullptr,nullptr,nullptr,&descriptor)!=ERROR_SUCCESS) return false;
    detail::Local owned; owned.value=descriptor;
    return protectedDescriptor(descriptor,object);
}
// The version directory and each ancestor below the volume root must be
// directories (not reparse points) writable only by trusted principals, so no
// other user can plant DLLs beside the approved programs or swap the folder.
inline bool protectedDirectory(const std::filesystem::path &directory) {
    const auto &path=directory.native();
    if (!validPath(path)) return false;
    for (size_t end=path.find(L'\\',3);;end=path.find(L'\\',end+1)) {
        const auto prefix=path.substr(0,end);
        detail::File handle;
        handle.value=CreateFileW(prefix.c_str(),READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (handle.value==INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.value,&info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (!protectedHandle(handle.value,SE_FILE_OBJECT,Object::Directory)) return false;
        if (end==std::wstring::npos) break;
    }
    return true;
}

inline Snapshot read(const Source &source=Source{}) {
    Snapshot snapshot;
    auto fail=[&](Status status, DWORD error) { snapshot.status=status; snapshot.error=error; snapshot.entries.clear(); return snapshot; };
    if (source.subkey.empty() || source.subkey.size()>512 || source.subkey.find(L'\0')!=std::wstring::npos) return fail(Status::Invalid,ERROR_INVALID_PARAMETER);
    detail::Key key;
    auto opened=RegOpenKeyExW(source.root,source.subkey.c_str(),0,KEY_QUERY_VALUE | READ_CONTROL | KEY_WOW64_64KEY,&key.value);
    if (opened==ERROR_FILE_NOT_FOUND || opened==ERROR_PATH_NOT_FOUND) return fail(Status::Missing,opened);
    if (opened!=ERROR_SUCCESS) return fail(Status::Invalid,opened);
    if (source.requireProtected) {
        if (!protectedHandle(key.value,SE_REGISTRY_KEY,Object::RegistryKey)) return fail(Status::Unprotected,ERROR_ACCESS_DENIED);
        // Whoever can write an ancestor could delete and recreate this key.
        for (size_t end=source.subkey.find(L'\\');end!=std::wstring::npos;end=source.subkey.find(L'\\',end+1)) {
            detail::Key ancestor;
            const auto prefix=source.subkey.substr(0,end);
            const auto status=RegOpenKeyExW(source.root,prefix.c_str(),0,READ_CONTROL | KEY_WOW64_64KEY,&ancestor.value);
            if (status!=ERROR_SUCCESS) return fail(Status::Unprotected,status);
            if (!protectedHandle(ancestor.value,SE_REGISTRY_KEY,Object::RegistryKey)) return fail(Status::Unprotected,ERROR_ACCESS_DENIED);
        }
    }
    DWORD values=0, longestName=0, longestData=0;
    const auto info=RegQueryInfoKeyW(key.value,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&values,&longestName,&longestData,nullptr,nullptr);
    if (info!=ERROR_SUCCESS) return fail(Status::Invalid,info);
    if (values>MaxEntries) return fail(Status::Invalid,ERROR_TOO_MANY_NAMES);
    for (DWORD index=0;index<values;++index) {
        std::wstring name(MaxIdLength+2,L'\0');
        DWORD nameSize=static_cast<DWORD>(name.size()), type=0;
        std::vector<BYTE> data(MaxValueBytes+2);
        DWORD dataSize=static_cast<DWORD>(data.size());
        const auto status=RegEnumValueW(key.value,index,name.data(),&nameSize,nullptr,&type,data.data(),&dataSize);
        if (status==ERROR_NO_MORE_ITEMS) break;
        if (status==ERROR_MORE_DATA) { ++snapshot.rejected; continue; }
        if (status!=ERROR_SUCCESS) return fail(Status::Invalid,status);
        name.resize(nameSize); data.resize(dataSize);
        std::vector<std::wstring> lines; Entry entry;
        if (type!=REG_MULTI_SZ || !splitMulti(data,lines)) { ++snapshot.rejected; continue; }
        const auto result=parse(name,lines,entry);
        if (result==Parse::Unsupported) { ++snapshot.unsupported; continue; }
        if (result!=Parse::Ok) { ++snapshot.rejected; continue; }
        if (source.requireProtected && !protectedDirectory(entry.host.parent_path())) { ++snapshot.rejected; continue; }
        snapshot.entries.push_back(std::move(entry));
    }
    snapshot.status=Status::Ready;
    return snapshot;
}

// Highest install sequence with the exact protocol the caller speaks. A tie
// cannot come from the installer and is treated as ambiguous: no approval.
inline std::optional<Entry> select(const Snapshot &snapshot, uint16_t protocol) {
    if (snapshot.status!=Status::Ready) return std::nullopt;
    const Entry *best=nullptr; bool tie=false;
    for (const auto &entry : snapshot.entries) {
        if (entry.protocol!=protocol) continue;
        if (!best || entry.sequence>best->sequence) { best=&entry; tie=false; }
        else if (entry.sequence==best->sequence) tie=true;
    }
    if (!best || tie) return std::nullopt;
    return *best;
}
} // namespace harukoto::engine::release
