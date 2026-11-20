// CN-07: the per-user punctuation preferences read by the input service.
#include "../PunctuationMode.h"
#include <cassert>
#include <string>

int main() {
    const std::wstring path = L"Software\\HarukotoPunctuationTests-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    // Defaults: Chinese punctuation, Ctrl+. on.
    assert(!readEnglishPunctuation(root) && readPunctuationHotkey(root));
    assert(writeEnglishPunctuation(true, root) && readEnglishPunctuation(root));
    assert(writeEnglishPunctuation(false, root) && !readEnglishPunctuation(root));
    HKEY input = nullptr;
    assert(RegOpenKeyExW(root, L"Software\\Meowpuut\\Input", 0, KEY_SET_VALUE, &input) == ERROR_SUCCESS);
    DWORD value = 0;
    assert(RegSetValueExW(input, L"PunctuationHotkey", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
    assert(!readPunctuationHotkey(root));
    value = 2; // Only 1 means English.
    assert(RegSetValueExW(input, L"EnglishPunctuation", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
    assert(!readEnglishPunctuation(root));
    // Another type keeps the defaults.
    assert(RegSetValueExW(input, L"EnglishPunctuation", 0, REG_SZ, reinterpret_cast<const BYTE *>(L"1"), 4) == ERROR_SUCCESS);
    assert(RegSetValueExW(input, L"PunctuationHotkey", 0, REG_SZ, reinterpret_cast<const BYTE *>(L"0"), 4) == ERROR_SUCCESS);
    assert(!readEnglishPunctuation(root) && readPunctuationHotkey(root));
    // CN-07 batch 7: the full-width hotkey is off unless DWORD 1.
    assert(!readFullWidthHotkey(root));
    for (DWORD setting : {1u, 0u, 2u}) {
        assert(RegSetValueExW(input, L"FullWidthHotkey", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&setting), sizeof(setting)) == ERROR_SUCCESS);
        assert(readFullWidthHotkey(root) == (setting == 1));
    }
    assert(RegSetValueExW(input, L"FullWidthHotkey", 0, REG_SZ, reinterpret_cast<const BYTE *>(L"1"), 4) == ERROR_SUCCESS);
    assert(!readFullWidthHotkey(root));
    RegCloseKey(input);
    RegCloseKey(root);
    assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
}
