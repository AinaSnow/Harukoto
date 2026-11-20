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
// Composing punctuation first commits the highlighted candidate.
constexpr PinyinMark pinyinMarkKind(unsigned int key, bool shift, bool english, bool composing) {
    if (shift) return !english && pinyinShiftMark(key) ? PinyinMark::chinese : PinyinMark::none;
    if (key == 0xde) return composing || english ? PinyinMark::none : PinyinMark::chinese;
    if (!pinyinAsciiPunctuation(key)) return PinyinMark::none;
    if (english || key == 0xbf) return composing ? PinyinMark::literal : PinyinMark::none;
    return PinyinMark::chinese;
}

inline std::wstring pinyinMarkText(unsigned int key, bool shift, PinyinMark mark, PinyinQuotes &quotes) {
    if (mark == PinyinMark::literal) return std::wstring(1, pinyinAsciiPunctuation(key));
    if (mark != PinyinMark::chinese) return {};
    if (key == 0xde) return std::wstring(1, quotes.next(shift));
    if (shift) return pinyinShiftPunctuation(key);
    return std::wstring(1, pinyinPunctuation(key));
}

// Ctrl+. toggles Chinese/English punctuation. Any other modifier passes.
constexpr bool pinyinPunctuationShortcut(unsigned int key, bool control, bool shift, bool alt, bool windows) {
    return key == 0xbe && control && !shift && !alt && !windows;
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
