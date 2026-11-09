#include "../ScriptPreference.h"
#include <cassert>
#include <string>

int main() {
    // Use an isolated subtree, never the real user preference.
    auto path = L"Software\\HarukotoScriptPreferenceTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                          KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    assert(readSimplifiedPreference(root));
    assert(writeSimplifiedPreference(false, root));
    assert(!readSimplifiedPreference(root));
    assert(writeSimplifiedPreference(true, root));
    assert(readSimplifiedPreference(root));
    HKEY key = nullptr;
    assert(RegOpenKeyExW(root, L"Software\\Meowpuut\\Input", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS);
    DWORD malformed = 7;
    assert(RegSetValueExW(key, L"TraditionalOutput", 0, REG_DWORD,
        reinterpret_cast<const BYTE *>(&malformed), sizeof(malformed)) == ERROR_SUCCESS);
    assert(readSimplifiedPreference(root));
    RegCloseKey(key);
    RegCloseKey(root);
    assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
}
