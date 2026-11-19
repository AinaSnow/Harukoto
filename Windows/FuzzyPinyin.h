#pragma once
#include <windows.h>
#include <cstdint>
#include "../Core/MeowpuutCore.h"

// CN-03: per-user fuzzy pinyin pairs, one bit per pair (MPN_FUZZY_*), stored
// beside the other input preferences. Absent, unreadable or of another type
// means every pair is off; unknown bits are ignored.
inline uint32_t readFuzzyPinyin(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 0, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"FuzzyPinyin",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result == ERROR_SUCCESS ? static_cast<uint32_t>(value) & MPN_FUZZY_ALL : 0;
}

inline bool writeFuzzyPinyin(uint32_t mask, HKEY root = HKEY_CURRENT_USER) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    DWORD value = mask & MPN_FUZZY_ALL;
    const auto result = RegSetValueExW(key, L"FuzzyPinyin", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// Give a session the current pairs. True when they changed, so the caller
// rebuilds its own candidate list; the core already rebuilt its candidates.
inline bool applyFuzzyPinyin(MPNSession *session, uint32_t mask) {
    mask &= MPN_FUZZY_ALL;
    if (!session || mpn_fuzzy_pinyin(session) == mask) return false;
    mpn_set_fuzzy_pinyin(session, mask);
    return true;
}
