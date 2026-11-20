// CN-07 batch 2: Shift tap detection and the per-user ShiftToggle setting.
#include "../ShiftToggle.h"
#include <cassert>
#include <string>

static void registry() {
    const std::wstring path = L"Software\\HarukotoShiftToggleTests-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    assert(readShiftToggle(root) == shiftToggleEither); // absent
    HKEY input = nullptr;
    assert(RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &input, nullptr) == ERROR_SUCCESS);
    for (DWORD value : {0u, 1u, 2u, 3u, 0xffffffffu}) {
        assert(RegSetValueExW(input, L"ShiftToggle", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
        assert(readShiftToggle(root) == (value <= 2 ? value : DWORD(shiftToggleEither)));
    }
    assert(RegSetValueExW(input, L"ShiftToggle", 0, REG_SZ, reinterpret_cast<const BYTE *>(L"0"), 4) == ERROR_SUCCESS);
    assert(readShiftToggle(root) == shiftToggleEither); // another type keeps the default
    RegCloseKey(input);
    RegCloseKey(root);
    assert(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS);
}

int main() {
    const auto tap = [](ShiftTap &shift, bool right, unsigned long long held, DWORD setting = shiftToggleEither) {
        shift.down(right, false, false, 0, 1000);
        return shift.up(right, false, 0, 1000 + held, setting);
    };
    ShiftTap shift;
    // A quick tap of either Shift, within the limit.
    assert(tap(shift, false, 80) && tap(shift, true, 80));
    assert(tap(shift, false, shiftTapLimit) && !tap(shift, false, shiftTapLimit + 1));
    // Settings: off, left only.
    assert(!tap(shift, false, 80, shiftToggleOff) && !tap(shift, true, 80, shiftToggleOff));
    assert(tap(shift, false, 80, shiftToggleLeft) && !tap(shift, true, 80, shiftToggleLeft));
    // Another key in between (Shift+A, Shift+1, Shift+Tab…): not a tap.
    shift.down(false, false, false, 0, 1000); shift.other();
    assert(!shift.up(false, false, 0, 1010, shiftToggleEither));
    // Held with Ctrl/Alt/Win (Ctrl+Shift, Alt+Shift layout switches).
    shift.down(false, false, true, 0, 1000);
    assert(!shift.up(false, false, 0, 1010, shiftToggleEither));
    shift.down(false, false, false, 0, 1000);
    assert(!shift.up(false, true, 0, 1010, shiftToggleEither));
    // A mouse click while Shift was held (Shift+click selects text).
    shift.down(false, false, false, 0, 1000);
    assert(!shift.up(false, false, 1, 1010, shiftToggleEither));
    // Both Shift keys together.
    shift.down(false, false, false, 0, 1000); shift.down(true, false, false, 0, 1005);
    assert(!shift.up(true, false, 0, 1010, shiftToggleEither) && !shift.up(false, false, 0, 1012, shiftToggleEither));
    // Duplicate notifications (test and real callbacks) count once, and an
    // auto-repeat does not restart the clock.
    shift.down(false, false, false, 0, 1000); shift.down(false, false, false, 0, 1001);
    shift.down(false, true, false, 0, 1400);
    assert(shift.up(false, false, 0, 1450, shiftToggleEither) && !shift.up(false, false, 0, 1451, shiftToggleEither));
    shift.down(false, false, false, 0, 1000); shift.down(false, true, false, 0, 1600);
    assert(!shift.up(false, false, 0, 1700, shiftToggleEither)); // held 700 ms
    // A repeat after another key (Shift still held) does not re-arm.
    shift.down(false, false, false, 0, 1000); shift.other(); shift.down(false, true, false, 0, 1300);
    assert(!shift.up(false, false, 0, 1350, shiftToggleEither));
    // A release without a press (focus arrived while Shift was held).
    ShiftTap fresh;
    assert(!fresh.up(false, false, 0, 10, shiftToggleEither));
    // The right Shift scan code.
    assert(rightShiftKey(LPARAM(0x36) << 16) && !rightShiftKey(LPARAM(0x2a) << 16));
    assert(rightShiftKey((LPARAM(0x36) << 16) | (LPARAM(1) << 30) | 1));
    registry();
}
