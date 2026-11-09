using System;
using System.Globalization;
using Microsoft.Win32;

// Layout is published as one registry value. Readers never observe half a layout.
internal sealed class LayoutOptions
{
    internal const string KeyPath = @"Software\Meowpuut\CandidateWindow";
    internal int Font = 18, Width = 672, Reading = 0, GapX = 8, GapY = 8, PaddingX = 10, PaddingY = 8, Outer = 12;
    internal bool Vertical;
    internal LayoutOptions Copy() { return (LayoutOptions)MemberwiseClone(); }
    internal double ReadingSize { get { return Reading == 0 ? Math.Max(11, Font * 0.6) : Reading; } }
    private static int Clamp(int value, int min, int max) { return Math.Max(min, Math.Min(max, value)); }
    internal LayoutOptions Validated()
    {
        var v = Copy();
        v.Font = Clamp(Font, 14, 32); v.Width = Clamp(Width, 480, 1000);
        v.Reading = Reading == 0 ? 0 : Clamp(Reading, 10, 24);
        v.GapX = Clamp(GapX, 0, 24); v.GapY = Clamp(GapY, 0, 24);
        v.PaddingX = Clamp(PaddingX, 4, 24); v.PaddingY = Clamp(PaddingY, 4, 20); v.Outer = Clamp(Outer, 4, 24);
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
    internal static LayoutOptions Read(RegistryKey key)
    {
        var v = new LayoutOptions();
        if (key == null) return v;
        var decoded = Decode(key.GetValue("LayoutV1") as string);
        if (decoded != null) return decoded;
        int number;
        if (int.TryParse(Convert.ToString(key.GetValue("FontSize", 18)), out number)) v.Font = number;
        if (int.TryParse(Convert.ToString(key.GetValue("Width", 672)), out number)) v.Width = number;
        return v.Validated();
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
    }
    internal static LayoutOptions Preset(int index)
    {
        if (index == 0) return new LayoutOptions { GapX = 4, GapY = 4, PaddingX = 6, PaddingY = 4, Outer = 8 };
        if (index == 2) return new LayoutOptions { GapX = 12, GapY = 12, PaddingX = 14, PaddingY = 12, Outer = 16 };
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
