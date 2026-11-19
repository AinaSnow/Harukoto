#pragma once
#include <windows.h>
#include "../Core/MeowpuutCore.h"

// CN-05: per-user typo correction switch, stored beside the other input
// preferences as the DWORD TypoCorrection. On unless the value is a DWORD 0:
// absent, unreadable or of another type keeps the default.
inline bool readTypoCorrection(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 1, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"TypoCorrection",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result != ERROR_SUCCESS || value != 0;
}

inline bool writeTypoCorrection(bool enabled, HKEY root = HKEY_CURRENT_USER) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    DWORD value = enabled ? 1 : 0;
    const auto result = RegSetValueExW(key, L"TypoCorrection", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// Give a session the current switch. True when it changed, so the caller
// rebuilds its own candidate list; the core already rebuilt its candidates.
inline bool applyTypoCorrection(MPNSession *session, bool enabled) {
    if (!session || (mpn_typo_correction(session) != 0) == enabled) return false;
    mpn_set_typo_correction(session, enabled ? 1 : 0);
    return true;
}
