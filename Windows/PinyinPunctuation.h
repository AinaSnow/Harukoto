#pragma once

#include <cstddef>
#include <string>

// Windows virtual-key codes for unshifted US-layout punctuation in Chinese
// punctuation mode. `/` is not here: it stays literal (CN-07), so chat
// commands such as FF14's /say work; Shift+/ gives ？ instead.
constexpr wchar_t pinyinPunctuation(unsigned int key) {
    switch (key) {
    case 0xbc: return L'，'; // VK_OEM_COMMA
    case 0xbe: return L'。'; // VK_OEM_PERIOD
    case 0xba: return L'；'; // VK_OEM_1
    case 0xdb: return L'「'; // VK_OEM_4
    case 0xdd: return L'」'; // VK_OEM_6
    case 0xdc: return L'、'; // VK_OEM_5
    default: return 0;
    }
}

// The literal (US-layout) character of the unshifted punctuation keys the
// IME may type itself: in English punctuation mode, for `/`, and behind a
// pending insertion.
constexpr wchar_t pinyinAsciiPunctuation(unsigned int key) {
    switch (key) {
    case 0xbc: return L',';
    case 0xbe: return L'.';
    case 0xba: return L';';
    case 0xbf: return L'/';
    case 0xdb: return L'[';
    case 0xdd: return L']';
    case 0xdc: return L'\\';
    case 0xde: return L'\'';
    default: return 0;
    }
}

// CN-07: the Shift layer in Chinese punctuation mode. Quotes (Shift+') are
// paired by PinyinQuotes. Every other Shift key passes to the host as before.
constexpr const wchar_t *pinyinShiftPunctuation(unsigned int key) {
    switch (key) {
    case '1': return L"！";
    case '4': return L"￥";
    case '6': return L"……";
    case '9': return L"（";
    case '0': return L"）";
    case 0xbd: return L"——"; // VK_OEM_MINUS
    case 0xba: return L"：";
    case 0xbc: return L"《";
    case 0xbe: return L"》";
    case 0xbf: return L"？";
    case 0xdb: return L"『";
    case 0xdd: return L"』";
    default: return nullptr;
    }
}

constexpr bool pinyinShiftMark(unsigned int key) { return key == 0xde || pinyinShiftPunctuation(key) != nullptr; }

// The US-layout character of Shift with a digit or punctuation key.
constexpr wchar_t pinyinShiftedAscii(unsigned int key) {
    switch (key) {
    case '1': return L'!'; case '2': return L'@'; case '3': return L'#'; case '4': return L'$'; case '5': return L'%';
    case '6': return L'^'; case '7': return L'&'; case '8': return L'*'; case '9': return L'('; case '0': return L')';
    case 0xba: return L':'; case 0xbb: return L'+'; case 0xbc: return L'<'; case 0xbd: return L'_'; case 0xbe: return L'>';
    case 0xbf: return L'?'; case 0xc0: return L'~'; case 0xdb: return L'{'; case 0xdc: return L'|'; case 0xdd: return L'}';
    case 0xde: return L'"';
    default: return 0;
    }
}

// What a printable key types on a US layout with Caps Lock off: letters,
// digits, space and punctuation, with or without Shift. 0 for anything else.
constexpr wchar_t pinyinLiteral(unsigned int key, bool shift) {
    if (key >= 'A' && key <= 'Z') return static_cast<wchar_t>(shift ? key : key - 'A' + 'a');
    if (key == 0x20) return L' ';
    if (shift) return pinyinShiftedAscii(key);
    if (key >= '0' && key <= '9') return static_cast<wchar_t>(key);
    switch (key) {
    case 0xbb: return L'=';
    case 0xbd: return L'-';
    case 0xc0: return L'`';
    default: return pinyinAsciiPunctuation(key);
    }
}

// Chinese quotes open and close alternately, one state per quote kind.
struct PinyinQuotes {
    bool singleOpen = false, doubleOpen = false;
    wchar_t next(bool isDouble) {
        bool &open = isDouble ? doubleOpen : singleOpen;
        open = !open;
        return isDouble ? (open ? L'“' : L'”') : (open ? L'‘' : L'’');
    }
};

enum class PinyinMark { none, chinese, literal };

// What the IME types for a punctuation key, before any pending-insertion
// rule. none: not punctuation here (the key passes to the host when idle, or
// is the syllable separator ' while composing).
//  - Chinese mode: , . ; [ ] \ and the Shift layer always; ' gives ‘’ when
//    idle; / passes through when idle and is a literal / while composing.
//  - English mode: unshifted punctuation passes through when idle and is
//    literal while composing; the Shift layer passes through as before.
//  - Either mode, composing: any other Shift symbol (@ # % … in Chinese
//    mode, all of them in English mode) is literal rather than reaching the
//    host ahead of the composition (batch 2).
// Composing punctuation first commits the highlighted candidate.
constexpr PinyinMark pinyinMarkKind(unsigned int key, bool shift, bool english, bool composing) {
    if (shift) return !english && pinyinShiftMark(key) ? PinyinMark::chinese
        : composing && pinyinShiftedAscii(key) ? PinyinMark::literal : PinyinMark::none;
    if (key == 0xde) return composing || english ? PinyinMark::none : PinyinMark::chinese;
    if (!pinyinAsciiPunctuation(key)) return PinyinMark::none;
    if (english || key == 0xbf) return composing ? PinyinMark::literal : PinyinMark::none;
    return PinyinMark::chinese;
}

inline std::wstring pinyinMarkText(unsigned int key, bool shift, PinyinMark mark, PinyinQuotes &quotes) {
    if (mark == PinyinMark::literal) return std::wstring(1, shift ? pinyinShiftedAscii(key) : pinyinAsciiPunctuation(key));
    if (mark != PinyinMark::chinese) return {};
    if (key == 0xde) return std::wstring(1, quotes.next(shift));
    if (shift) return pinyinShiftPunctuation(key);
    return std::wstring(1, pinyinPunctuation(key));
}

// Ctrl+. toggles Chinese/English punctuation. Any other modifier passes.
constexpr bool pinyinPunctuationShortcut(unsigned int key, bool control, bool shift, bool alt, bool windows) {
    return key == 0xbe && control && !shift && !alt && !windows;
}

// CN-07 batch 3: while composing, - and = page back and forward like Page Up
// and Page Down (in both punctuation modes). Idle they stay the host's, and
// Shift+- / Shift+= follow the punctuation rules.
constexpr bool pinyinPagingKey(unsigned int key, bool shift, bool composing) {
    return composing && !shift && (key == 0xbd || key == 0xbb); // VK_OEM_MINUS, VK_OEM_PLUS
}

// CN-07 batch 5: Ctrl+Delete or Shift+Delete (as in Rime and other IMEs)
// forgets what was learned for the highlighted candidate. Only one of Ctrl
// and Shift, no Alt or Windows key; taken only while composing.
constexpr bool pinyinForgetShortcut(unsigned int key, bool control, bool shift, bool alt, bool windows) {
    return key == 0x2e && control != shift && !alt && !windows; // VK_DELETE
}

// CN-07 batch 4, 以词定字: while composing, [ and ] commit the first and the
// last character of the highlighted candidate. Idle they stay 「」, and
// Shift+[ ] stay 『』.
constexpr bool pinyinWordCharacterKey(unsigned int key, bool shift, bool composing) {
    return composing && !shift && (key == 0xdb || key == 0xdd); // VK_OEM_4, VK_OEM_6
}

// The first or last code point of UTF-8 text (one to four bytes, so
// characters outside the BMP stay whole). Empty for empty text.
inline std::string pinyinWordCharacter(const std::string &text, bool last) {
    if (text.empty()) return {};
    const auto continuation = [&](size_t i) { return (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80; };
    size_t begin = 0, end = 1;
    if (last) {
        end = text.size();
        begin = end - 1;
        while (begin > 0 && continuation(begin)) --begin;
    } else {
        while (end < text.size() && continuation(end)) ++end;
    }
    return text.substr(begin, end - begin);
}

// The other digits remain literal input when no corresponding candidate is
// displayed, rather than reaching the editor ahead of an active composition.
constexpr bool pinyinDigitSelects(unsigned int key, std::size_t start, std::size_t count) {
    return key >= '1' && key <= '7' && start < count && key - '1' < count - start;
}

// These unmodified keys are literal text after a selection has cleared the
// reading, but must not overtake a pending asynchronous TSF insertion.
// Unshifted punctuation the IME leaves to the host joins them
// (pinyinAsciiPunctuation).
constexpr bool pinyinQueuedLiteral(unsigned int key) {
    return key == 0x20 || (key >= '0' && key <= '9');
}
