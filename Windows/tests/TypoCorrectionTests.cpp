#include "../TypoCorrection.h"
#include <cassert>
#include <string>
#include <string_view>

// CN-05: the per-user typo correction switch and how the service hands it to a session.
int main() {
    // Use an isolated subtree, never the real user preference.
    auto path = L"Software\\HarukotoTypoCorrectionTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                           KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    // Default: on.
    assert(readTypoCorrection(root));
    assert(writeTypoCorrection(false, root) && !readTypoCorrection(root));
    assert(writeTypoCorrection(true, root) && readTypoCorrection(root));
    HKEY key = nullptr;
    assert(RegOpenKeyExW(root, L"Software\\Meowpuut\\Input", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS);
    // Any non-zero DWORD is on; another value type keeps the default.
    DWORD value = 7;
    assert(RegSetValueExW(key, L"TypoCorrection", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
    assert(readTypoCorrection(root));
    const wchar_t text[] = L"0";
    assert(RegSetValueExW(key, L"TypoCorrection", 0, REG_SZ, reinterpret_cast<const BYTE *>(text), sizeof(text)) == ERROR_SUCCESS);
    assert(readTypoCorrection(root));
    RegCloseKey(key);
    RegCloseKey(root);
    assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);

    // Applying: only a change rebuilds, and the open composition is kept.
    constexpr std::string_view data =
        "MEOWPUUT-PINYIN-1\n"
        "guo\tguo2\t國\t国\t8000\n"
        "zhong\tzhong1\t中\t中\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t10000\n";
    auto *lexicon = mpn_lexicon_create(data.data(), data.size());
    assert(lexicon);
    auto *session = mpn_session_create(lexicon);
    assert(session);
    mpn_set_simplified(session, 1);
    assert(!applyTypoCorrection(nullptr, false));
    assert(!applyTypoCorrection(session, true));
    for (char c : std::string_view("zhonguo")) assert(mpn_key(session, c));
    assert(std::string(mpn_candidate(session, 0)) == "中国" && mpn_candidate_typo(session, 0));
    assert(applyTypoCorrection(session, false));
    assert(!applyTypoCorrection(session, false));
    assert(std::string(mpn_input(session)) == "zhonguo");
    for (size_t i = 0; i < mpn_candidate_count(session); ++i) assert(!mpn_candidate_typo(session, i));
    assert(applyTypoCorrection(session, true) && std::string(mpn_candidate(session, 0)) == "中国");
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}
