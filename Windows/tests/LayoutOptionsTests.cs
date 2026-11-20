using System;
using Microsoft.Win32;

internal static class LayoutOptionsTests
{
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    static void Main()
    {
        try { Run(); }
        catch (Exception e) { Console.Error.WriteLine(e.GetType().FullName + ": " + e.Message); Environment.ExitCode = 1; }
    }
    static void Run()
    {
        string path = @"Software\HarukotoLayoutTests-" + Guid.NewGuid().ToString("N");
        try {
            using (var key = Registry.CurrentUser.CreateSubKey(path)) {
                key.SetValue("FontSize", 26); key.SetValue("Width", 784);
                var legacy = LayoutOptions.Read(key);
                Check(legacy.Font == 26 && legacy.Width == 784 && legacy.Reading == 0 && !legacy.Vertical, "Legacy migration");
                var changed = new LayoutOptions { Font = 28, Width = 520, Reading = 21, GapX = 3, GapY = 6, PaddingX = 7, PaddingY = 9, Outer = 14, Vertical = true };
                LayoutOptions.Write(key, changed);
                Check(LayoutOptions.Read(key).Encode() == changed.Encode(), "All fields persisted");
                Check((int)key.GetValue("FontSize") == 26, "Legacy values must not be overwritten");
            }
            using (var readOnly = Registry.CurrentUser.OpenSubKey(path, false)) {
                string before = (string)readOnly.GetValue("LayoutV1");
                bool failed = false;
                try { LayoutOptions.Write(readOnly, new LayoutOptions()); } catch (UnauthorizedAccessException) { failed = true; }
                Check(failed && (string)readOnly.GetValue("LayoutV1") == before, "Failed write replaced old layout");
                Check(LayoutOptions.Read(readOnly).Font == 28, "Reopen lost settings");
                using (var input = Registry.CurrentUser.CreateSubKey(path + @"\Input")) {
                    foreach (bool absent in new[] { true, false }) {
                        if (!absent) input.SetValue("TraditionalOutput", 0, RegistryValueKind.DWord);
                        failed = false;
                        try { LayoutOptions.SavePreferences(input, readOnly, new LayoutOptions(), true); } catch (UnauthorizedAccessException) { failed = true; }
                        Check(failed && (string)readOnly.GetValue("LayoutV1") == before, "Failed grouped save changed layout");
                        Check(absent ? input.GetValue("TraditionalOutput") == null : (int)input.GetValue("TraditionalOutput") == 0, "Script rollback failed");
                    }
                }
            }
            using (var key = Registry.CurrentUser.OpenSubKey(path, true)) {
                foreach (string invalid in new[] { "broken", "2;18;672;0;8;8;10;8;12;0", "1;18;672;0;8;8;10;8;12;2", "1;NaN;672;0;8;8;10;8;12;0" }) {
                    key.SetValue("LayoutV1", invalid);
                    Check(LayoutOptions.Read(key).Font == 26, "Invalid snapshot did not fall back to legacy");
                }
                key.SetValue("LayoutV1", 42, RegistryValueKind.DWord);
                Check(LayoutOptions.Read(key).Width == 784, "Wrong registry type");
                var bounded = LayoutOptions.Decode("1;-5;5000;99;-1;99;0;99;99;1");
                Check(bounded.Font == 14 && bounded.Width == 1000 && bounded.Reading == 24 && bounded.GapX == 0 && bounded.GapY == 24 && bounded.PaddingX == 4 && bounded.PaddingY == 20 && bounded.Outer == 24, "Bounds");
            }
            for (int i = 0; i < 3; ++i) Check(LayoutOptions.Decode(LayoutOptions.Preset(i).Encode()).Encode() == LayoutOptions.Preset(i).Encode(), "Preset round trip");
            using (var key = Registry.CurrentUser.OpenSubKey(path, true)) LayoutOptions.Write(key, new LayoutOptions { Vertical = true, Reading = 20, GapX = 5 });
            var cli = LayoutOptions.Decode(HarukotoAppearanceBridge.Update(path, 24, 800, false));
            Check(cli.Vertical && cli.Reading == 20 && cli.GapX == 5 && cli.Font == 24 && cli.Width == 800, "CLI lost extended layout fields");
            Check(HarukotoAppearanceBridge.Update(path, 0, 0, true) == new LayoutOptions().Encode(), "CLI reset");
            // CN-03: fuzzy pinyin pairs, one DWORD bit mask; off unless saved.
            Check(FuzzyPinyinOptions.Pairs.Length == 11 && FuzzyPinyinOptions.All == 2047, "Eleven fuzzy pairs");
            Check(FuzzyPinyinOptions.Read(null) == 0, "Fuzzy default without key");
            using (var input = Registry.CurrentUser.CreateSubKey(path + @"\FuzzyInput")) {
                Check(FuzzyPinyinOptions.Read(input) == 0, "Fuzzy default off");
                FuzzyPinyinOptions.Write(input, 0);
                Check(input.GetValue(FuzzyPinyinOptions.ValueName) == null, "Saving all-off must not create the value");
                FuzzyPinyinOptions.Write(input, (1 << 0) | (1 << 8));
                Check(input.GetValueKind(FuzzyPinyinOptions.ValueName) == RegistryValueKind.DWord && FuzzyPinyinOptions.Read(input) == 257, "Fuzzy pairs persisted as DWORD");
                FuzzyPinyinOptions.Write(input, -1);
                Check(FuzzyPinyinOptions.Read(input) == 2047, "Unknown fuzzy bits dropped");
                FuzzyPinyinOptions.Write(input, 0);
                Check((int)input.GetValue(FuzzyPinyinOptions.ValueName) == 0, "Fuzzy pairs switched off");
                input.SetValue(FuzzyPinyinOptions.ValueName, "2047");
                Check(FuzzyPinyinOptions.Read(input) == 0, "Wrong fuzzy registry type means off");
            }
            // CN-05: typo correction, on unless saved as DWORD 0.
            Check(TypoCorrectionOptions.Read(null), "Typo correction default without key");
            using (var input = Registry.CurrentUser.CreateSubKey(path + @"\TypoInput")) {
                Check(TypoCorrectionOptions.Read(input), "Typo correction default on");
                TypoCorrectionOptions.Write(input, true);
                Check(input.GetValue(TypoCorrectionOptions.ValueName) == null, "Saving the default must not create the value");
                TypoCorrectionOptions.Write(input, false);
                Check(input.GetValueKind(TypoCorrectionOptions.ValueName) == RegistryValueKind.DWord && !TypoCorrectionOptions.Read(input), "Typo correction off persisted as DWORD");
                TypoCorrectionOptions.Write(input, true);
                Check((int)input.GetValue(TypoCorrectionOptions.ValueName) == 1 && TypoCorrectionOptions.Read(input), "Typo correction switched back on");
                input.SetValue(TypoCorrectionOptions.ValueName, "0");
                Check(TypoCorrectionOptions.Read(input), "Wrong typo registry type means on");
            }
            // CN-07: Chinese punctuation and the Ctrl+. hotkey unless saved otherwise.
            Check(!PunctuationOptions.ReadEnglish(null) && PunctuationOptions.ReadHotkey(null), "Punctuation defaults without key");
            using (var input = Registry.CurrentUser.CreateSubKey(path + @"\PunctuationInput")) {
                Check(!PunctuationOptions.ReadEnglish(input) && PunctuationOptions.ReadHotkey(input), "Punctuation defaults");
                PunctuationOptions.WriteEnglish(input, false); PunctuationOptions.WriteHotkey(input, true);
                Check(input.GetValue(PunctuationOptions.EnglishValue) == null && input.GetValue(PunctuationOptions.HotkeyValue) == null, "Saving the defaults must not create the values");
                PunctuationOptions.WriteEnglish(input, true); PunctuationOptions.WriteHotkey(input, false);
                Check(input.GetValueKind(PunctuationOptions.EnglishValue) == RegistryValueKind.DWord && (int)input.GetValue(PunctuationOptions.EnglishValue) == 1
                    && PunctuationOptions.ReadEnglish(input), "English punctuation persisted as DWORD 1");
                Check(input.GetValueKind(PunctuationOptions.HotkeyValue) == RegistryValueKind.DWord && !PunctuationOptions.ReadHotkey(input), "Hotkey off persisted as DWORD 0");
                PunctuationOptions.WriteEnglish(input, false); PunctuationOptions.WriteHotkey(input, true);
                Check((int)input.GetValue(PunctuationOptions.EnglishValue) == 0 && !PunctuationOptions.ReadEnglish(input), "Chinese punctuation again");
                Check((int)input.GetValue(PunctuationOptions.HotkeyValue) == 1 && PunctuationOptions.ReadHotkey(input), "Hotkey on again");
                input.SetValue(PunctuationOptions.EnglishValue, 2, RegistryValueKind.DWord);
                Check(!PunctuationOptions.ReadEnglish(input), "Only DWORD 1 means English");
                input.SetValue(PunctuationOptions.EnglishValue, "1"); input.SetValue(PunctuationOptions.HotkeyValue, "0");
                Check(!PunctuationOptions.ReadEnglish(input) && PunctuationOptions.ReadHotkey(input), "Wrong punctuation registry types keep the defaults");
            }
            Console.WriteLine("PASS: legacy migration, every field persistence, reopen, denied-write preservation, corrupt/future/type fallback, bounds, presets, fuzzy pinyin pairs, typo correction and punctuation.");
        } finally { Registry.CurrentUser.DeleteSubKeyTree(path, false); }
    }
}
