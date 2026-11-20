#include "../PinyinPunctuation.h"
#include <cassert>
#include <string>

static std::wstring type(unsigned int key, bool shift, bool english, bool composing, PinyinQuotes &quotes) {
    return pinyinMarkText(key, shift, pinyinMarkKind(key, shift, english, composing), quotes);
}


int main() {
    assert(pinyinPunctuation(0xbc) == L'，');
    assert(pinyinPunctuation(0xbe) == L'。');
    assert(pinyinPunctuation(0xba) == L'；');
    assert(!pinyinPunctuation(0xbf)); // CN-07: / stays literal
    assert(pinyinPunctuation(0xdb) == L'「');
    assert(pinyinPunctuation(0xdd) == L'」');
    assert(pinyinPunctuation(0xdc) == L'、');
    assert(!pinyinPunctuation('A'));
    assert(!pinyinPunctuation(0xde)); // apostrophe splits pinyin syllables
    assert(pinyinQueuedLiteral(0x20));
    assert(!pinyinQueuedLiteral(0x0d)); // editor actions must not become literal newlines
    assert(!pinyinQueuedLiteral(0x08)); // do not swallow host deletion
    assert(!pinyinQueuedLiteral(0x25)); // host cursor movement
    assert(!pinyinQueuedLiteral('A'));
    for (unsigned int key = '0'; key <= '9'; ++key) {
        assert(pinyinQueuedLiteral(key));
        assert(pinyinDigitSelects(key, 0, 3) == (key >= '1' && key <= '3'));
        assert(pinyinDigitSelects(key, 7, 9) == (key >= '1' && key <= '2'));
        assert(!pinyinDigitSelects(key, 0, 0));
    }

    // CN-07, Chinese mode, idle and composing alike.
    PinyinQuotes quotes;
    for (bool composing : {false, true}) {
        assert(type(0xbc, false, false, composing, quotes) == L"，");
        assert(type(0xbe, false, false, composing, quotes) == L"。");
        assert(type(0xba, false, false, composing, quotes) == L"；");
        assert(type(0xdb, false, false, composing, quotes) == L"「");
        assert(type(0xdd, false, false, composing, quotes) == L"」");
        assert(type(0xdc, false, false, composing, quotes) == L"、");
        // Shift layer.
        assert(type('1', true, false, composing, quotes) == L"！");
        assert(type('4', true, false, composing, quotes) == L"￥");
        assert(type('6', true, false, composing, quotes) == L"……");
        assert(type('9', true, false, composing, quotes) == L"（");
        assert(type('0', true, false, composing, quotes) == L"）");
        assert(type(0xbd, true, false, composing, quotes) == L"——");
        assert(type(0xba, true, false, composing, quotes) == L"：");
        assert(type(0xbc, true, false, composing, quotes) == L"《");
        assert(type(0xbe, true, false, composing, quotes) == L"》");
        assert(type(0xbf, true, false, composing, quotes) == L"？");
        assert(type(0xdb, true, false, composing, quotes) == L"『");
        assert(type(0xdd, true, false, composing, quotes) == L"』");
        assert(type(0xde, true, false, composing, quotes) == L"“");
        assert(type(0xde, true, false, composing, quotes) == L"”");
        // Unmapped Shift keys are left to the host when idle and literal
        // while composing (batch 2); Shift+letters are not punctuation.
        for (unsigned int key : {unsigned('2'), unsigned('3'), unsigned('5'), unsigned('7'), unsigned('8'), 0xbbu, 0xc0u, 0xdcu}) {
            assert(!pinyinShiftMark(key));
            assert(pinyinMarkKind(key, true, false, composing) == (composing ? PinyinMark::literal : PinyinMark::none));
            if (composing) assert(type(key, true, false, true, quotes) == std::wstring(1, pinyinShiftedAscii(key)));
        }
        for (unsigned int key : {unsigned('A'), unsigned('Z')}) {
            assert(!pinyinShiftMark(key));
            assert(pinyinMarkKind(key, true, false, composing) == PinyinMark::none);
        }
    }
    // / passes to the host when idle and is literal while composing.
    assert(pinyinMarkKind(0xbf, false, false, false) == PinyinMark::none);
    assert(type(0xbf, false, false, true, quotes) == L"/");
    // ' gives paired single quotes when idle and separates syllables otherwise.
    assert(type(0xde, false, false, false, quotes) == L"‘");
    assert(type(0xde, false, false, false, quotes) == L"’");
    assert(type(0xde, false, false, false, quotes) == L"‘");
    assert(pinyinMarkKind(0xde, false, false, true) == PinyinMark::none);
    // The two quote kinds pair independently.
    assert(type(0xde, true, false, false, quotes) == L"“");
    assert(type(0xde, false, false, false, quotes) == L"’");
    assert(type(0xde, true, false, false, quotes) == L"”");
    // Letters, digits and other keys are not punctuation.
    for (unsigned int key : {unsigned('A'), unsigned('1'), 0x20u, 0x0du, 0xbbu, 0xbdu, 0xc0u})
        assert(pinyinMarkKind(key, false, false, true) == PinyinMark::none);

    // English mode: everything literal. Idle keys pass to the host; while
    // composing they commit the candidate and type the character.
    const std::wstring ascii = L",.;/[]\\";
    const unsigned int keys[] = {0xbc, 0xbe, 0xba, 0xbf, 0xdb, 0xdd, 0xdc};
    for (size_t i = 0; i < ascii.size(); ++i) {
        assert(pinyinAsciiPunctuation(keys[i]) == ascii[i]);
        assert(pinyinMarkKind(keys[i], false, true, false) == PinyinMark::none);
        assert(type(keys[i], false, true, true, quotes) == std::wstring(1, ascii[i]));
        assert(pinyinMarkKind(keys[i], true, true, false) == PinyinMark::none);
        assert(type(keys[i], true, true, true, quotes) == std::wstring(1, pinyinShiftedAscii(keys[i])));
    }
    assert(pinyinAsciiPunctuation(0xde) == L'\'');
    assert(pinyinMarkKind(0xde, false, true, false) == PinyinMark::none);
    assert(pinyinMarkKind(0xde, false, true, true) == PinyinMark::none); // still the separator
    assert(type('1', true, true, true, quotes) == L"!" && type(0xde, true, true, true, quotes) == L"\"");
    assert(pinyinMarkKind('A', true, true, true) == PinyinMark::none);

    // Literal characters (US layout), used in English mode behind a pending insertion.
    const std::wstring shifted = L")!@#$%^&*(";
    for (unsigned int key = '0'; key <= '9'; ++key) {
        assert(pinyinLiteral(key, false) == static_cast<wchar_t>(key));
        assert(pinyinLiteral(key, true) == shifted[key - '0'] && pinyinShiftedAscii(key) == shifted[key - '0']);
    }
    assert(pinyinLiteral('A', false) == L'a' && pinyinLiteral('A', true) == L'A' && pinyinLiteral('Z', false) == L'z');
    assert(pinyinLiteral(0x20, false) == L' ' && pinyinLiteral(0x20, true) == L' ');
    const std::wstring plain = L";=,-./`[\\]'", upper = L":+<_>?~{|}\"";
    const unsigned int punctuation[] = {0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xdb, 0xdc, 0xdd, 0xde};
    for (size_t i = 0; i < plain.size(); ++i)
        assert(pinyinLiteral(punctuation[i], false) == plain[i] && pinyinLiteral(punctuation[i], true) == upper[i]);
    for (unsigned int key : {0x0du, 0x08u, 0x25u, 0x09u, 0x70u}) assert(!pinyinLiteral(key, false) && !pinyinLiteral(key, true));
    assert(!pinyinAsciiPunctuation('A') && !pinyinAsciiPunctuation(0x20));

    // Batch 4: [ ] pick a character only while composing and without Shift.
    assert(pinyinWordCharacterKey(0xdb, false, true) && pinyinWordCharacterKey(0xdd, false, true));
    assert(!pinyinWordCharacterKey(0xdb, false, false) && !pinyinWordCharacterKey(0xdd, true, true));
    assert(!pinyinWordCharacterKey(0xdc, false, true) && !pinyinWordCharacterKey(0xbd, false, true));
    assert(pinyinWordCharacter("中国人", false) == "中" && pinyinWordCharacter("中国人", true) == "人");
    assert(pinyinWordCharacter("好", false) == "好" && pinyinWordCharacter("好", true) == "好");
    assert(pinyinWordCharacter("", false).empty() && pinyinWordCharacter("", true).empty());
    assert(pinyinWordCharacter("ab", false) == "a" && pinyinWordCharacter("ab", true) == "b");
    // Four-byte UTF-8 (a surrogate pair in UTF-16) stays whole at either end.
    const std::string astral = "\xf0\xaa\x9a\xa5", dragon = "\xe9\xbe\x98"; // U+2A6A5, U+9F98
    assert(pinyinWordCharacter(astral + dragon, false) == astral && pinyinWordCharacter(dragon + astral, true) == astral);
    assert(pinyinWordCharacter(astral, false) == astral && pinyinWordCharacter(astral, true) == astral);

    // Batch 3: - and = page only while composing and without Shift.
    assert(pinyinPagingKey(0xbd, false, true) && pinyinPagingKey(0xbb, false, true));
    assert(!pinyinPagingKey(0xbd, false, false) && !pinyinPagingKey(0xbb, false, false));
    assert(!pinyinPagingKey(0xbd, true, true) && !pinyinPagingKey(0xbb, true, true));
    for (unsigned int key : {0xbcu, 0xbeu, 0x21u, 0x22u, unsigned('A')}) assert(!pinyinPagingKey(key, false, true));
    assert(pinyinMarkKind(0xbd, false, false, true) == PinyinMark::none && pinyinMarkKind(0xbb, false, true, true) == PinyinMark::none);

    // Ctrl+. alone toggles; other modifiers leave the key to the host.
    assert(pinyinPunctuationShortcut(0xbe, true, false, false, false));
    assert(!pinyinPunctuationShortcut(0xbe, false, false, false, false));
    assert(!pinyinPunctuationShortcut(0xbe, true, true, false, false));
    assert(!pinyinPunctuationShortcut(0xbe, true, false, true, false));
    assert(!pinyinPunctuationShortcut(0xbe, true, false, false, true));
    assert(!pinyinPunctuationShortcut(0xbc, true, false, false, false));

}
