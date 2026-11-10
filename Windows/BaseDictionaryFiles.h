#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace base_dictionary {
class Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    static void check(NTSTATUS status) {
        if (status < 0) throw std::runtime_error("SHA256 failed");
    }
public:
    Sha256() {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        const auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
        if (status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr; check(status); }
    }
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    ~Sha256() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void add(const char *bytes, ULONG size) {
        check(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes)), size, 0));
    }
    std::string finish() {
        unsigned char bytes[32];
        check(BCryptFinishHash(hash, bytes, sizeof(bytes), 0));
        std::string result;
        for (auto byte : bytes) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
        return result;
    }
};
inline std::string sha256(const std::string &bytes) {
    Sha256 hash; hash.add(bytes.data(), static_cast<ULONG>(bytes.size())); return hash.finish();
}

// Keep the handle for the entire loaded object's lifetime, especially SQLite:
// readers may coexist, but in-place writes and path replacement must fail.
class StableFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
public:
    explicit StableFile(const std::filesystem::path &path, uint64_t maximum = 512ull * 1024 * 1024) {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("snapshot file missing or busy: " + path.filename().u8string());
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > maximum) {
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error("invalid snapshot file: " + path.filename().u8string());
        }
        size_ = static_cast<uint64_t>(size.QuadPart);
    }
    StableFile(const StableFile &) = delete;
    StableFile &operator=(const StableFile &) = delete;
    ~StableFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    uint64_t size() const { return size_; }
    std::string prefix(DWORD size) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) throw std::runtime_error("snapshot seek failed");
        std::string bytes(size, '\0');
        DWORD count = 0;
        if (!ReadFile(handle, bytes.data(), size, &count, nullptr) || count != size)
            throw std::runtime_error("snapshot header truncated");
        return bytes;
    }
    std::string read(bool hashOnly = false) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) throw std::runtime_error("snapshot seek failed");
        Sha256 hash;
        std::string bytes;
        if (!hashOnly) bytes.reserve(static_cast<size_t>(size_));
        std::array<char, 65536> buffer{};
        uint64_t total = 0;
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                throw std::runtime_error("snapshot read failed");
            if (!count) break;
            total += count;
            if (total > size_) throw std::runtime_error("snapshot size changed");
            if (hashOnly) hash.add(buffer.data(), count);
            else bytes.append(buffer.data(), count);
        }
        if (total != size_) throw std::runtime_error("snapshot read truncated");
        return hashOnly ? hash.finish() : bytes;
    }
};
} // namespace base_dictionary
