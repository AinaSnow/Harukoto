#pragma once
#include <windows.h>

// WIN-08: hosts that draw their own IME UI (full-screen games such as
// FINAL FANTASY XIV). The service offers its candidate list as a TSF UI
// element; a host that answers BeginUIElement with "do not show" draws the
// list itself, and then also needs the reading as a real TSF composition
// (IMM32 games read it as the composition string).
//
// The per-user DWORD InlineComposition under Software\Meowpuut\Input picks
// when the reading is written into the document as a composition:
// 0 automatic (default): only while the host draws the candidates;
// 1 always (also when the candidate window is ours: for a game that shows
//   the composition string but leaves the candidate window to the IME);
// 2 never (the reading stays in the candidate window only, as before).
// Anything else, absent or of another type is the default. Read per key.
enum InlineCompositionSetting : DWORD { inlineCompositionAuto = 0, inlineCompositionAlways = 1, inlineCompositionNever = 2 };

inline DWORD readInlineComposition(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = inlineCompositionAuto, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"InlineComposition",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result == ERROR_SUCCESS && value <= inlineCompositionNever ? value : inlineCompositionAuto;
}

// The keyboard open/close compartment and the Chinese/English mode
// (CN-07 batch 2, made robust in WIN-08). Games commonly close the IME while
// their chat box is not focused (ImmSetOpenStatus / OPENCLOSE = 0) and may or
// may not open it again. The rule:
//  - only the user's own switch (Shift tap, taskbar item) changes the
//    preferred mode, and it is written to the compartment;
//  - a close from outside switches to English for the moment without
//    touching the preference ("closed by the host");
//  - an open from outside restores the preference after such a close;
//    otherwise (the user had chosen English and something asks to open the
//    IME, e.g. the system's Ctrl+Space) it is an explicit request: Chinese,
//    which also becomes the preference, as before WIN-08;
//  - a Shift tap always works, so a host that never reopens cannot strand
//    the user in English.
// Values equal to the last known compartment state are echoes (our own
// writes, or no change) and are ignored.
struct OpenCloseRule {
    bool chinese = true, preferred = true, hostClosed = false, open = true;
    unsigned changes = 0, hostCloses = 0, hostOpens = 0;
    // Activation: Chinese, compartment written open.
    void activate() { chinese = preferred = open = true; hostClosed = false; }
    // The user's switch; returns the value to write to the compartment.
    bool user(bool value) {
        chinese = preferred = open = value;
        hostClosed = false;
        return value;
    }
    // The compartment as read after a change notification. False: an echo.
    bool observed(bool value) {
        if (value == open) return false;
        open = value;
        ++changes;
        if (!value) {
            ++hostCloses;
            hostClosed = true;
            chinese = false;
        } else {
            ++hostOpens;
            chinese = hostClosed ? preferred : (preferred = true);
            hostClosed = false;
        }
        return true;
    }
    const char *mode() const { return chinese ? "chinese" : hostClosed ? "host_closed" : "english"; }
};
