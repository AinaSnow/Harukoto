using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

internal static class DictionaryLauncher
{
    internal static Window Create()
    {
        var window = new Window { Title = "晴词 · 主题词库", Width = 560, SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterScreen, ResizeMode = ResizeMode.NoResize, FontSize = 14 };
        var body = new StackPanel { Margin = new Thickness(24) }; window.Content = body;
        body.Children.Add(new TextBlock { Text = "打开主题词库", FontSize = 24, Margin = new Thickness(0,0,0,16) });
        body.Children.Add(new TextBlock { Text = "词库管理需要 Python 3.10 或更新版本，并包含 tkinter。选择已安装的 python.exe；此设置只用于词库管理。", TextWrapping = TextWrapping.Wrap });
        var path = new TextBox { Margin = new Thickness(0,16,0,12) };
        using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\Dictionaries"))
            path.Text = key == null ? "" : Convert.ToString(key.GetValue("PythonPath", ""));
        if (path.Text.Length == 0) {
            foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';')) {
                try { var candidate = Path.Combine(dir.Trim().Trim('"'), "python.exe");
                    if (Path.IsPathRooted(candidate) && File.Exists(candidate) && candidate.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) < 0) { path.Text = candidate; break; }
                } catch (ArgumentException) { }
            }
        }
        body.Children.Add(path);
        var status = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,12) }; body.Children.Add(status);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        var browse = new Button { Content = "选择 Python…", Padding = new Thickness(12,7,12,7), Margin = new Thickness(0,0,12,0) };
        var open = new Button { Content = "打开词库管理", Padding = new Thickness(12,7,12,7), IsDefault = true };
        browse.Click += (s,e) => { var dialog = new OpenFileDialog { Filter = "Python (python.exe)|python.exe" }; if (dialog.ShowDialog(window) == true) path.Text = dialog.FileName; };
        open.Click += async (s,e) => {
            open.IsEnabled = browse.IsEnabled = path.IsEnabled = false; status.Text = "正在检查 Python…";
            try {
                var executable = Path.GetFullPath(path.Text);
                if (!File.Exists(executable)) throw new IOException("找不到所选 Python。");
                await Task.Run(() => {
                    var probe = new ProcessStartInfo(executable, "-I -c \"import sys,tkinter; assert sys.version_info >= (3,10)\"") { UseShellExecute = false, CreateNoWindow = true };
                    using (var process = Process.Start(probe)) {
                        if (!process.WaitForExit(10000)) { process.Kill(); throw new IOException("Python 检查超时，请选择有效的 Python 安装。"); }
                        if (process.ExitCode != 0) throw new IOException("需要 Python 3.10+ 及 tkinter，请重新选择完整的 Python 安装。");
                    }
                });
                var script = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "dictionary-manager.pyw");
                if (!File.Exists(script)) throw new IOException("缺少词库管理文件，请重新安装晴词。");
                var start = new ProcessStartInfo(executable, "-I \"" + script + "\"") { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory };
                using (var process = Process.Start(start)) { }
                using (var key = Registry.CurrentUser.CreateSubKey(@"Software\Meowpuut\Dictionaries")) key.SetValue("PythonPath", executable);
                window.Close();
            } catch (Exception error) { status.Text = error.Message; }
            finally { open.IsEnabled = browse.IsEnabled = path.IsEnabled = true; }
        };
        buttons.Children.Add(browse); buttons.Children.Add(open); body.Children.Add(buttons);
        return window;
    }
}
