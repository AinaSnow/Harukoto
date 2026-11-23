using System;
using System.Globalization;
using Microsoft.Win32;

// CN-03: fuzzy pinyin pairs, one bit each in the DWORD FuzzyPinyin under
// Software\Meowpuut\Input. Bit order matches MPN_FUZZY_* in Core/MeowpuutCore.h;
// the input service reads the value on every key. Absent or of another type = all off.
internal static class FuzzyPinyinOptions
{
    internal const string ValueName = "FuzzyPinyin";
    internal static readonly string[] Pairs = { "z = zh", "c = ch", "s = sh", "n = l", "f = h", "r = l",
        "an = ang", "en = eng", "in = ing", "ian = iang", "uan = uang" };
    internal const int All = (1 << 11) - 1;
    internal static int Read(RegistryKey input)
    {
        if (input == null) return 0;
        var value = input.GetValue(ValueName);
        return value is int ? (int)value & All : 0;
    }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    internal static void Write(RegistryKey input, int mask)
    {
        mask &= All;
        if (input.GetValue(ValueName) is int && Read(input) == mask) return;
        if (input.GetValue(ValueName) == null && mask == 0) return;
        input.SetValue(ValueName, mask, RegistryValueKind.DWord);
    }
}

// CN-05: typo correction, the DWORD TypoCorrection under Software\Meowpuut\Input;
// the input service reads it on every key. On unless the value is a DWORD 0.
internal static class TypoCorrectionOptions
{
    internal const string ValueName = "TypoCorrection";
    internal static bool Read(RegistryKey input)
    {
        if (input == null) return true;
        var value = input.GetValue(ValueName);
        return !(value is int) || (int)value != 0;
    }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    internal static void Write(RegistryKey input, bool enabled)
    {
        if (Read(input) == enabled && (input.GetValue(ValueName) is int || enabled)) return;
        input.SetValue(ValueName, enabled ? 1 : 0, RegistryValueKind.DWord);
    }
}

// CN-07: punctuation, two DWORDs under Software\Meowpuut\Input read on every key.
// EnglishPunctuation: 1 = English (literal) punctuation, otherwise Chinese; the
// input service also writes it when Ctrl+. toggles the mode.
// PunctuationHotkey: Ctrl+. toggles the mode unless this is a DWORD 0.
internal static class PunctuationOptions
{
    internal const string EnglishValue = "EnglishPunctuation", HotkeyValue = "PunctuationHotkey";
    internal static bool ReadEnglish(RegistryKey input)
    {
        if (input == null) return false;
        var value = input.GetValue(EnglishValue);
        return value is int && (int)value == 1;
    }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    // The settings window calls it only when its checkbox changed, so a Ctrl+.
    // toggle made while the window is open is not undone by saving.
    internal static void WriteEnglish(RegistryKey input, bool english)
    {
        if (ReadEnglish(input) == english && (input.GetValue(EnglishValue) is int || !english)) return;
        input.SetValue(EnglishValue, english ? 1 : 0, RegistryValueKind.DWord);
    }
    internal static bool ReadHotkey(RegistryKey input)
    {
        if (input == null) return true;
        var value = input.GetValue(HotkeyValue);
        return !(value is int) || (int)value != 0;
    }
    internal static void WriteHotkey(RegistryKey input, bool enabled)
    {
        if (ReadHotkey(input) == enabled && (input.GetValue(HotkeyValue) is int || enabled)) return;
        input.SetValue(HotkeyValue, enabled ? 1 : 0, RegistryValueKind.DWord);
    }
}

// CN-07 batch 2: which Shift keys switch Chinese/English when tapped alone,
// the DWORD ShiftToggle under Software\Meowpuut\Input read at every Shift
// release: 0 none, 1 either (default), 2 left only; anything else is the default.
internal static class ShiftToggleOptions
{
    internal const string ValueName = "ShiftToggle";
    internal const int Off = 0, Either = 1, LeftOnly = 2;
    internal static readonly string[] Names = { "关闭", "左右 Shift", "仅左 Shift" };
    internal static int Read(RegistryKey input)
    {
        if (input == null) return Either;
        var value = input.GetValue(ValueName);
        return value is int && (int)value >= Off && (int)value <= LeftOnly ? (int)value : Either;
    }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    internal static void Write(RegistryKey input, int setting)
    {
        if (setting < Off || setting > LeftOnly) setting = Either;
        if (Read(input) == setting && (input.GetValue(ValueName) is int || setting == Either)) return;
        input.SetValue(ValueName, setting, RegistryValueKind.DWord);
    }
}

// CN-07 batch 7: Shift+Space toggles full-width output only when the DWORD
// WIN-03: StandaloneEngine under Software\Meowpuut\Input. On unless it is the
// DWORD 0 (default on since alpha.34), matching tsf_engine::readStandaloneEngine.
internal static class StandaloneEngineOptions
{
    internal const string ValueName = "StandaloneEngine";
    internal static bool Read(RegistryKey input)
    {
        if (input == null) return true;
        var value = input.GetValue(ValueName);
        return !(value is int) || (int)value != 0;
    }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    internal static void Write(RegistryKey input, bool enabled)
    {
        if (Read(input) == enabled && (input.GetValue(ValueName) is int || enabled)) return;
        input.SetValue(ValueName, enabled ? 1 : 0, RegistryValueKind.DWord);
    }
}

// FullWidthHotkey under Software\Meowpuut\Input is 1. Off by default (games
// use Shift+Space); switching it off also ends full-width output.
internal static class FullWidthOptions
{
    internal const string ValueName = "FullWidthHotkey";
    internal static bool Read(RegistryKey input)
    {
        if (input == null) return false;
        var value = input.GetValue(ValueName);
        return value is int && (int)value == 1;
    }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    internal static void Write(RegistryKey input, bool enabled)
    {
        if (Read(input) == enabled && (input.GetValue(ValueName) is int || !enabled)) return;
        input.SetValue(ValueName, enabled ? 1 : 0, RegistryValueKind.DWord);
    }
}

// Layout is published as one registry value. Readers never observe half a layout.
internal sealed class LayoutOptions
{
    internal const string KeyPath = @"Software\Meowpuut\CandidateWindow";
    // Compact single-row defaults (UI-04). Width is the maximum; the window shrinks to its candidates.
    internal int Font = 18, Width = 860, Reading = 0, GapX = 2, GapY = 2, PaddingX = 8, PaddingY = 4, Outer = 8;
    // Kept only for LayoutV1 compatibility: candidates are always one horizontal row now.
    internal bool Vertical;
    // UI-04 appearance, stored apart from LayoutV1 so older versions still read the layout:
    // Theme 0 晴空 Fluent, 1 晴空胶囊; ColorMode 0 follow system, 1 light, 2 dark.
    internal const int ThemeFluent = 0, ThemeCapsule = 1;
    internal int Theme, ColorMode;
    internal LayoutOptions Copy() { return (LayoutOptions)MemberwiseClone(); }
    internal double ReadingSize { get { return Reading == 0 ? Math.Max(11, Font * 0.6) : Reading; } }
    private static int Clamp(int value, int min, int max) { return Math.Max(min, Math.Min(max, value)); }
    internal LayoutOptions Validated()
    {
        var v = Copy();
        v.Font = Clamp(Font, 14, 32); v.Width = Clamp(Width, 480, 1000);
        v.Reading = Reading == 0 ? 0 : Clamp(Reading, 10, 24);
        v.GapX = Clamp(GapX, 0, 24); v.GapY = Clamp(GapY, 0, 24);
        v.PaddingX = Clamp(PaddingX, 4, 24); v.PaddingY = Clamp(PaddingY, 2, 20); v.Outer = Clamp(Outer, 4, 24);
        v.Theme = Clamp(Theme, 0, 1); v.ColorMode = Clamp(ColorMode, 0, 2);
        return v;
    }
    internal string Encode()
    {
        var v = Validated();
        return string.Join(";", new[] { "1", v.Font.ToString(CultureInfo.InvariantCulture), v.Width.ToString(CultureInfo.InvariantCulture),
            v.Reading.ToString(CultureInfo.InvariantCulture), v.GapX.ToString(CultureInfo.InvariantCulture), v.GapY.ToString(CultureInfo.InvariantCulture),
            v.PaddingX.ToString(CultureInfo.InvariantCulture), v.PaddingY.ToString(CultureInfo.InvariantCulture), v.Outer.ToString(CultureInfo.InvariantCulture), v.Vertical ? "1" : "0" });
    }
    internal static LayoutOptions Decode(string text)
    {
        if (text == null || text.Length > 256) return null;
        var parts = text.Split(';'); var n = new int[10];
        if (parts.Length != 10) return null;
        for (int i = 0; i < n.Length; ++i)
            if (!int.TryParse(parts[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out n[i])) return null;
        if (n[0] != 1 || (n[9] != 0 && n[9] != 1)) return null;
        return new LayoutOptions { Font = n[1], Width = n[2], Reading = n[3], GapX = n[4], GapY = n[5], PaddingX = n[6], PaddingY = n[7], Outer = n[8], Vertical = n[9] == 1 }.Validated();
    }
    internal string EncodeAppearance()
    {
        var v = Validated();
        return "1;" + v.Theme.ToString(CultureInfo.InvariantCulture) + ";" + v.ColorMode.ToString(CultureInfo.InvariantCulture);
    }
    // Unknown or damaged appearance leaves the defaults (Fluent, follow system).
    internal static bool DecodeAppearance(string text, LayoutOptions into)
    {
        if (text == null || text.Length > 64) return false;
        var parts = text.Split(';'); var n = new int[3];
        if (parts.Length != 3) return false;
        for (int i = 0; i < n.Length; ++i)
            if (!int.TryParse(parts[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out n[i])) return false;
        if (n[0] != 1 || n[1] < 0 || n[1] > 1 || n[2] < 0 || n[2] > 2) return false;
        into.Theme = n[1]; into.ColorMode = n[2];
        return true;
    }
    internal static LayoutOptions Read(RegistryKey key)
    {
        var v = new LayoutOptions();
        if (key == null) return v;
        var decoded = Decode(key.GetValue("LayoutV1") as string);
        if (decoded != null) v = decoded;
        else {
            int number;
            if (int.TryParse(Convert.ToString(key.GetValue("FontSize", 18)), out number)) v.Font = number;
            if (int.TryParse(Convert.ToString(key.GetValue("Width", 860)), out number)) v.Width = number;
            v = v.Validated();
        }
        DecodeAppearance(key.GetValue("AppearanceV1") as string, v);
        return v;
    }
    internal static LayoutOptions Load()
    {
        try { using (var key = Registry.CurrentUser.OpenSubKey(KeyPath)) return Read(key); }
        catch { return new LayoutOptions(); }
    }
    internal static void Write(RegistryKey key, LayoutOptions options)
    {
        // No legacy mirrors: a failed write leaves the previous layout intact.
        key.SetValue("LayoutV1", options.Encode(), RegistryValueKind.String);
        // Appearance is its own value; an unchanged or default appearance is not created.
        string appearance = options.EncodeAppearance();
        var stored = key.GetValue("AppearanceV1") as string;
        if (stored == appearance || (stored == null && appearance == new LayoutOptions().EncodeAppearance())) return;
        key.SetValue("AppearanceV1", appearance, RegistryValueKind.String);
    }
    internal static LayoutOptions Preset(int index)
    {
        if (index == 0) return new LayoutOptions { GapX = 0, GapY = 0, PaddingX = 6, PaddingY = 2, Outer = 6 };
        if (index == 2) return new LayoutOptions { GapX = 6, GapY = 6, PaddingX = 12, PaddingY = 8, Outer = 12 };
        return new LayoutOptions();
    }
    internal static void SavePreferences(RegistryKey input, RegistryKey layout, LayoutOptions options, bool traditional)
    {
        var previous = input.GetValue("TraditionalOutput");
        var previousKind = previous == null ? RegistryValueKind.DWord : input.GetValueKind("TraditionalOutput");
        bool changed = traditional != (Convert.ToString(previous ?? 0) == "1");
        if (changed) input.SetValue("TraditionalOutput", traditional ? 1 : 0, RegistryValueKind.DWord);
        try { Write(layout, options); }
        catch {
            if (changed) {
                try { if (previous == null) input.DeleteValue("TraditionalOutput", false); else input.SetValue("TraditionalOutput", previous, previousKind); }
                catch (Exception restore) { throw new InvalidOperationException("布局未保存，简繁设置恢复失败，请重新确认简繁开关：" + restore.Message); }
            }
            throw;
        }
    }
}
