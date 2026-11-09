using Microsoft.Win32;

public static class HarukotoAppearanceBridge
{
    public static string Update(string keyPath, int font, int width, bool reset)
    {
        using (var key = Registry.CurrentUser.CreateSubKey(keyPath)) {
            var options = reset ? new LayoutOptions() : LayoutOptions.Read(key);
            if (!reset) {
                if (font != 0) options.Font = font;
                if (width != 0) options.Width = width;
            }
            if (reset || font != 0 || width != 0) LayoutOptions.Write(key, options);
            return options.Encode();
        }
    }
}
