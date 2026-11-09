#pragma once
#include <windows.h>

// Keep the existing per-user namespace for upgrade compatibility.
inline bool readSimplifiedPreference(HKEY root = HKEY_CURRENT_USER) {
    DWORD traditional = 0, bytes = sizeof(traditional);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input",
        L"TraditionalOutput", RRF_RT_REG_DWORD, nullptr, &traditional, &bytes);
    return result != ERROR_SUCCESS || traditional != 1;
}

inline bool writeSimplifiedPreference(bool simplified, HKEY root = HKEY_CURRENT_USER) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0,
                       KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    DWORD value = simplified ? 0 : 1;
    auto result = RegSetValueExW(key, L"TraditionalOutput", 0, REG_DWORD,
                                reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
