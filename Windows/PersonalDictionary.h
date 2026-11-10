#pragma once
#include <windows.h>
#include "JapanesePackages.h"
#include <memory>

struct PersonalDictionarySnapshot {
    bool present = false;
    std::string bytes;
    std::vector<JapaneseWord> words;
};

// Read one stable file handle. The importer publishes by atomic replacement;
// deny in-place writers until this bounded read finishes, but allow rename.
inline std::shared_ptr<const PersonalDictionarySnapshot> readPersonalDictionary(
        const std::filesystem::path &path, bool allowMissing = true,
        std::shared_ptr<const PersonalDictionarySnapshot> previous = {}) noexcept {
    HANDLE handle = INVALID_HANDLE_VALUE;
    try {
        auto snapshot = std::make_shared<PersonalDictionarySnapshot>();
        if (path.empty()) return snapshot;
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return allowMissing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? snapshot : nullptr;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > 32 * 1024 * 1024) {
            CloseHandle(handle); return {};
        }
        snapshot->present = true;
        snapshot->bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        const bool ok = ReadFile(handle, snapshot->bytes.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
        CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
        if (!ok) return {};
        if (previous && previous->present && previous->bytes == snapshot->bytes) return previous;
        std::istringstream lines(snapshot->bytes);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.front() == '#') continue;
            if (line.size() > 4096 || std::count(line.begin(), line.end(), '|') != 5) return {};
            std::istringstream row(line);
            auto words = readJapaneseWords(row);
            if (words.size() != 1) return {};
            const auto &word = words.front();
            if (word.reading.size() < 2 || word.source.empty() ||
                !std::all_of(word.reading.begin(), word.reading.end(), [](char c) { return c >= 'a' && c <= 'z'; }) ||
                !validPackageUnicode(word.text) || !validPackageUnicode(word.kana, true, true) ||
                !validPackageUnicode(word.chinese, false, true)) return {};
            snapshot->words.push_back(std::move(words.front()));
            if (snapshot->words.size() > 500000) return {};
        }
        return snapshot;
    } catch (...) { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); return {}; }
}
