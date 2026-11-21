#pragma once
#include "EnginePipe.h"
#include "BaseDictionaryFiles.h"
#include <cstring>
#include <map>
#include <mutex>

namespace harukoto::engine {
// A caller-supplied digest is the trust root. Never obtain it from a candidate
// process, diagnostic mapping or the same untrusted directory being checked.
// Keep the file AND its resolved local ancestor directories pinned: an image
// path lookup must not race a renamed parent or a retargeted junction.
class EngineDeployment {
    std::vector<pipe::Handle> directories_;
    pipe::Handle file_;
    FILE_ID_INFO id_{};
    std::filesystem::path path_;
    std::wstring native_;
    std::string digest_;
    static void reject(DWORD error = ERROR_INVALID_IMAGE_HASH) {
        throw pipe::Error(pipe::Failure::Authentication,error);
    }
    static std::wstring finalPath(HANDLE file, DWORD volume) {
        std::wstring value(32768,L'\0');
        const auto size=GetFinalPathNameByHandleW(file,value.data(),static_cast<DWORD>(value.size()),FILE_NAME_NORMALIZED | volume);
        if (!size) reject(GetLastError());
        if (size>=value.size()) reject(ERROR_FILENAME_EXCED_RANGE);
        value.resize(size); return value;
    }
    static FILE_ID_INFO fileId(HANDLE file) {
        FILE_ID_INFO result{};
        if (!GetFileInformationByHandleEx(file,FileIdInfo,&result,sizeof(result))) reject(GetLastError());
        return result;
    }
    static bool equal(const FILE_ID_INFO &a, const FILE_ID_INFO &b) {
        return a.VolumeSerialNumber==b.VolumeSerialNumber && !std::memcmp(a.FileId.Identifier,b.FileId.Identifier,sizeof(a.FileId.Identifier));
    }
    static pipe::Handle open(const std::filesystem::path &path) {
        pipe::Handle result(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
        if (!result) reject(GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(result.get(),&info)) reject(GetLastError());
        if (GetFileType(result.get())!=FILE_TYPE_DISK || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) reject(ERROR_BAD_FILE_TYPE);
        return result;
    }
    void pinDirectories(const std::wstring &resolved) {
        const std::wstring prefix=L"\\\\?\\Volume{";
        const auto end=resolved.find(L"}\\",prefix.size());
        if (resolved.compare(0,prefix.size(),prefix)!=0 || end==std::wstring::npos || end+2>=resolved.size() ||
                resolved.find(L':',end+2)!=std::wstring::npos) reject(ERROR_BAD_PATHNAME);
        const auto root=end+2;
        for (size_t length=root;;) {
            pipe::Handle directory(CreateFileW(resolved.substr(0,length).c_str(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if (!directory) reject(GetLastError());
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(directory.get(),&info)) reject(GetLastError());
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) reject(ERROR_BAD_PATHNAME);
            directories_.push_back(std::move(directory));
            length=resolved.find(L'\\',length==root ? root : length+1);
            if (length==std::wstring::npos) break;
        }
    }
public:
    static std::string digest(std::string value) {
        if (value.size()!=64) throw std::invalid_argument("invalid engine deployment digest");
        for (auto &c : value) {
            if (c>='A' && c<='F') c=char(c-'A'+'a');
            if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) throw std::invalid_argument("invalid engine deployment digest");
        }
        return value;
    }
    EngineDeployment(const std::filesystem::path &path, const std::string &expected) {
        const auto approved=digest(expected);
        if (!path.is_absolute() || path.native().find(L'\0')!=std::wstring::npos) throw std::invalid_argument("invalid engine deployment path");
        pipe::processIdentityOnly();
        file_=open(path); id_=fileId(file_.get());
        const auto resolved=finalPath(file_.get(),VOLUME_NAME_GUID);
        pinDirectories(resolved);
        // Parent renames before the pins were acquired cannot redirect launch.
        auto reopened=open(resolved);
        if (!equal(id_,fileId(reopened.get()))) reject();
        // The DOS image locator is not a trust source. Verify the suspended
        // process against the pinned native path and file ID before execution.
        path_=finalPath(file_.get(),VOLUME_NAME_DOS);
        native_=finalPath(file_.get(),VOLUME_NAME_NT);
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(),&size)) reject(GetLastError());
        if (size.QuadPart<=0) reject(ERROR_BAD_LENGTH);
        if (size.QuadPart>512ll*1024*1024) reject(ERROR_FILE_TOO_LARGE);
        base_dictionary::Sha256 hash; std::array<char,65536> buffer{};
        uint64_t total=0;
        for (;;) {
            DWORD count=0;
            if (!ReadFile(file_.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr)) reject(GetLastError());
            if (!count) break;
            total+=count; if (total>static_cast<uint64_t>(size.QuadPart)) reject();
            hash.add(buffer.data(),count);
        }
        digest_=hash.finish();
        if (total!=static_cast<uint64_t>(size.QuadPart) || digest_!=approved) reject();
    }
    // Reuse a pin that something in this process still holds (a live handoff
    // session holds both), instead of hashing the file again (ENGINE_COST.md).
    // While any holder keeps it, the file is open without write/delete sharing
    // and its ancestor directories cannot be renamed, so the bytes hashed then
    // are the bytes on disk now; nothing outlives the last holder. Keyed by
    // the exact path and approved digest: a different digest pins afresh.
    static std::shared_ptr<const EngineDeployment> shared(const std::filesystem::path &path, const std::string &expected) {
        const auto key=std::make_pair(path.native(),digest(expected));
        static std::mutex lock;
        static std::map<std::pair<std::wstring,std::string>,std::weak_ptr<const EngineDeployment>> pins;
        {
            std::lock_guard<std::mutex> guard(lock);
            for (auto it=pins.begin();it!=pins.end();) it=it->second.expired() ? pins.erase(it) : std::next(it);
            const auto found=pins.find(key);
            if (found!=pins.end()) if (auto live=found->second.lock()) return live;
        }
        auto fresh=std::make_shared<const EngineDeployment>(path,key.second); // Hash outside the lock.
        std::lock_guard<std::mutex> guard(lock);
        auto &slot=pins[key];
        if (auto live=slot.lock()) return live;
        slot=fresh; return fresh;
    }
    EngineDeployment(const EngineDeployment &)=delete;
    EngineDeployment &operator=(const EngineDeployment &)=delete;
    const std::filesystem::path &path() const { return path_; }
    const std::string &digest() const { return digest_; }
    bool sameFile(const EngineDeployment &other) const { return equal(id_,other.id_); }
    // Only examine the caller's retained real process object. This API never
    // takes a PID, duplicates capabilities, attaches or reconstructs trust.
    void verify(HANDLE process) const {
        pipe::processIdentityOnly();
        const auto alive=WaitForSingleObject(process,0);
        if (alive!=WAIT_TIMEOUT) reject(alive==WAIT_FAILED ? GetLastError() : ERROR_PROCESS_ABORTED);
        std::wstring actual(32768,L'\0'); DWORD size=static_cast<DWORD>(actual.size());
        if (!QueryFullProcessImageNameW(process,PROCESS_NAME_NATIVE,actual.data(),&size)) reject(GetLastError());
        actual.resize(size);
        if (CompareStringOrdinal(actual.data(),static_cast<int>(actual.size()),native_.data(),static_cast<int>(native_.size()),TRUE)!=CSTR_EQUAL) reject();
        size=32768; actual.resize(size);
        if (!QueryFullProcessImageNameW(process,0,actual.data(),&size)) reject(GetLastError());
        actual.resize(size); auto loaded=open(actual);
        if (!equal(id_,fileId(loaded.get()))) reject();
        if (WaitForSingleObject(process,0)!=WAIT_TIMEOUT) reject(ERROR_PROCESS_ABORTED);
    }
};
} // namespace harukoto::engine
