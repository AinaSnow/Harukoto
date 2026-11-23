using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;

// Entry point for "--dictionaries" and the settings page button. Since DICT-03
// the manager is built in (DictionaryManager.cs); Python is no longer needed.
internal static class DictionaryLauncher
{
    internal static Window Create()
    {
        var local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        if (!string.IsNullOrEmpty(local))
            return new DictionaryManagerWindow(Path.Combine(local, "Meowpuut"));
        var window = new Window { Title = "晴词 · 主题词库", Width = 480, SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterScreen, ResizeMode = ResizeMode.NoResize, FontSize = 14 };
        window.Content = new TextBlock { Text = "缺少 LOCALAPPDATA，无法定位当前用户的词库目录。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(24) };
        return window;
    }
}
