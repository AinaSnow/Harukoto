#pragma once
#include <windows.h>

// CN-07 batch 2: tapping Shift alone switches Chinese/English input.
//
// The per-user DWORD ShiftToggle under Software\Meowpuut\Input picks which
// Shift keys do it: 0 none, 1 either (default), 2 left only. Anything else,
// absent or of another type is the default. Read on every Shift release.
enum ShiftToggleSetting : DWORD { shiftToggleOff = 0, shiftToggleEither = 1, shiftToggleLeft = 2 };

inline DWORD readShiftToggle(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = shiftToggleEither, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"ShiftToggle",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result == ERROR_SUCCESS && value <= shiftToggleLeft ? value : shiftToggleEither;
}

// A tap: Shift pressed with no other modifier held, released within
// shiftTapLimit milliseconds, with no other key pressed and no mouse click in
// between (a Shift+click selects text). Notifications may arrive twice (TSF
// test and real key callbacks) and Shift repeats while held; both are
// ignored, so the detector can be fed from every callback.
constexpr unsigned long long shiftTapLimit = 500;

struct ShiftTap {
    bool armed = false, right = false;
    unsigned long long since = 0;
    unsigned int mouse = 0;

    // mouse: a snapshot of the mouse buttons (pressed and toggle bits), which
    // changes with every click this thread sees.
    void down(bool rightKey, bool repeat, bool otherModifiers, unsigned int mouseState, unsigned long long now) {
        if (armed) {
            if (rightKey != right) armed = false; // both Shift keys: not a tap
            return;
        }
        if (repeat) return; // held, or pressed again after another key
        armed = !otherModifiers;
        right = rightKey;
        since = now;
        mouse = mouseState;
    }
    void other() { armed = false; }
    // True exactly once for a tap that the setting allows.
    bool up(bool rightKey, bool otherModifiers, unsigned int mouseState, unsigned long long now, DWORD setting) {
        if (!armed || rightKey != right) return false;
        armed = false;
        return !otherModifiers && mouseState == mouse && now - since <= shiftTapLimit
            && setting != shiftToggleOff && (setting != shiftToggleLeft || !rightKey);
    }
};

// Right Shift is scan code 0x36 in the key message's lParam.
constexpr bool rightShiftKey(LPARAM flags) { return ((static_cast<unsigned long long>(flags) >> 16) & 0xff) == 0x36; }
