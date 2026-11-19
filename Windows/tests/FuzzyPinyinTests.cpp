#include "../FuzzyPinyin.h"
#include <cassert>
#include <string>
#include <string_view>

// CN-03: the per-user fuzzy pinyin value and how the service hands it to a session.
int main() {
    // Use an isolated subtree, never the real user preference.
    auto path = L"Software\\HarukotoFuzzyPinyinTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                           KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    // Default: every pair off.
    assert(readFuzzyPinyin(root) == 0);
    assert(writeFuzzyPinyin(MPN_FUZZY_Z_ZH | MPN_FUZZY_IN_ING, root));
    assert(readFuzzyPinyin(root) == (MPN_FUZZY_Z_ZH | MPN_FUZZY_IN_ING));
    assert(writeFuzzyPinyin(MPN_FUZZY_ALL, root) && readFuzzyPinyin(root) == MPN_FUZZY_ALL);
    assert(writeFuzzyPinyin(0, root) && readFuzzyPinyin(root) == 0);
    HKEY key = nullptr;
    assert(RegOpenKeyExW(root, L"Software\\Meowpuut\\Input", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS);
    // Unknown bits are ignored; another value type means off.
    DWORD future = 0xffffffff;
    assert(RegSetValueExW(key, L"FuzzyPinyin", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&future), sizeof(future)) == ERROR_SUCCESS);
    assert(readFuzzyPinyin(root) == MPN_FUZZY_ALL);
    const wchar_t text[] = L"2047";
    assert(RegSetValueExW(key, L"FuzzyPinyin", 0, REG_SZ, reinterpret_cast<const BYTE *>(text), sizeof(text)) == ERROR_SUCCESS);
    assert(readFuzzyPinyin(root) == 0);
    RegCloseKey(key);
    RegCloseKey(root);
    assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);

    // Applying: only a change rebuilds, and the open composition is kept.
    constexpr std::string_view data =
        "MEOWPUUT-PINYIN-1\n"
        "guo\tguo2\t國\t国\t8000\n"
        "zhong\tzhong1\t中\t中\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t10000\n"
        "zong\tzong3\t總\t总\t7000\n";
    auto *lexicon = mpn_lexicon_create(data.data(), data.size());
    assert(lexicon);
    auto *session = mpn_session_create(lexicon);
    assert(session);
    mpn_set_simplified(session, 1);
    assert(!applyFuzzyPinyin(nullptr, MPN_FUZZY_ALL));
    assert(!applyFuzzyPinyin(session, 0));
    for (char c : std::string_view("zongguo")) assert(mpn_key(session, c));
    const std::string before = mpn_candidate(session, 0);
    assert(before != "中国");
    assert(applyFuzzyPinyin(session, MPN_FUZZY_Z_ZH));
    assert(!applyFuzzyPinyin(session, MPN_FUZZY_Z_ZH | (1u << 30)));
    assert(std::string(mpn_input(session)) == "zongguo" && std::string(mpn_candidate(session, 0)) == "中国");
    assert(applyFuzzyPinyin(session, 0) && std::string(mpn_candidate(session, 0)) == before);
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}
