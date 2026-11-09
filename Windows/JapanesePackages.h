#pragma once
#include "MixedComposition.h"
#include <filesystem>
#include <fstream>
#include <cstdint>

inline bool validPackageUnicode(const std::string &text, bool kana = false, bool empty = false) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = static_cast<unsigned char>(text[i++]);
        unsigned n = 0; uint32_t minimum = 0;
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 31; n = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 15; n = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; n = 3; minimum = 0x10000; }
        else if (cp >= 0x80) return false;
        if (i + n > text.size()) return false;
        while (n--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 || cp == 127) return false;
        if (kana && !((cp >= 0x3041 && cp <= 0x3096) || cp == 0x30fc)) return false;
        if (++count > 128) return false;
    }
    return count != 0 || empty;
}

// CURRENT is replaced only after both files in its immutable snapshot are complete.
// A failed optional package load must never prevent the base IME from starting.
inline std::vector<JapaneseWord> readEnabledJapanesePackages(const std::filesystem::path &root) noexcept {
    try {
        std::ifstream pointer(root / "CURRENT", std::ios::binary);
        char generation[33]{};
        pointer.read(generation, sizeof(generation));
        if (pointer.gcount() != 32 || !std::all_of(generation, generation + 32, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) return {};
        const auto path = root / std::string(generation, 32) / "enabled.tsv";
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > 256 * 1024 * 1024) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        std::string bytes(static_cast<size_t>(size), '\0');
        if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || file.peek() != EOF) return {};
        std::istringstream input(bytes);
        size_t count = 0;
        for (std::string line; std::getline(input, line);) {
            if (++count > 500000 || line.empty() || line.size() > 4096 || line.front() == '#' ||
                std::count(line.begin(), line.end(), '|') != 5 || line.find_first_of("\t\r") != line.npos) return {};
            const auto end = line.find('|');
            if (end < 2 || end > 256 || !std::all_of(line.begin(), line.begin() + end, [](char c) { return c >= 'a' && c <= 'z'; })) return {};
        }
        std::istringstream rows(bytes);
        auto words = readJapaneseWords(rows);
        if (words.size() != count || std::any_of(words.begin(), words.end(), [](const auto &word) {
            return word.source.empty() || !validPackageUnicode(word.text) ||
                !validPackageUnicode(word.kana, true, true) || !validPackageUnicode(word.chinese, false, true);
        })) return {};
        return words;
    } catch (...) { return {}; }
}
