#pragma once
#include <windows.h>

// CN-07: per-user punctuation preferences beside the other input settings
// under Software\Meowpuut\Input, read on every key.
//  - EnglishPunctuation: DWORD 1 = English (literal) punctuation; anything
//    else, absent or of another type = Chinese punctuation.
//  - PunctuationHotkey: Ctrl+. toggles the mode unless this is a DWORD 0.
inline bool readEnglishPunctuation(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 0, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"EnglishPunctuation",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result == ERROR_SUCCESS && value == 1;
}

inline bool writeEnglishPunctuation(bool english, HKEY root = HKEY_CURRENT_USER) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    DWORD value = english ? 1 : 0;
    const auto result = RegSetValueExW(key, L"EnglishPunctuation", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

inline bool readPunctuationHotkey(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 1, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"PunctuationHotkey",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result != ERROR_SUCCESS || value != 0;
}
