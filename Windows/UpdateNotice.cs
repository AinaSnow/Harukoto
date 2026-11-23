using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.Win32;

// UPD-01 user interface: the "检查更新" section of the settings window and the
// sign-in check (MeowpuutGlassPreview.exe --check-update). Logic lives in UpdateCheck.cs.
internal static class UpdateNotice
{
    internal static void OpenReleasePage(string url)
    {
        try { Process.Start(new ProcessStartInfo(UpdateCheck.SafeReleaseUrl(url)) { UseShellExecute = true }); }
        catch (Exception) { MessageBox.Show("无法打开浏览器，请手动访问 " + UpdateCheck.ReleasesPage, "晴词 · 检查更新"); }
    }

    // Adds the section to the settings body and returns the automatic-check box,
    // which the settings window saves together with its other options.
    internal static CheckBox AddSettingsSection(Panel body, bool autoCheck)
    {
        body.Children.Add(new TextBlock { Text = "检查更新", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,8) });
        body.Children.Add(new TextBlock { Text = "当前版本：" + UpdateCheck.CurrentVersion, Margin = new Thickness(0,0,0,8) });
        var check = new Button { Name = "CheckUpdate", Content = "检查更新", Padding = new Thickness(12,7,12,7), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0,0,0,8) };
        var result = new TextBlock { Name = "UpdateResult", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,8) };
        AutomationProperties.SetLiveSetting(result, AutomationLiveSetting.Polite);
        var notes = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,8), Visibility = Visibility.Collapsed };
        var open = new Button { Name = "OpenReleasePage", Content = "打开发布页", Padding = new Thickness(12,7,12,7), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0,0,0,8), Visibility = Visibility.Collapsed };
        string releaseUrl = UpdateCheck.ReleasesPage;
        open.Click += (s,e) => OpenReleasePage(releaseUrl);
        check.Click += async (s,e) => {
            check.IsEnabled = false; open.Visibility = Visibility.Collapsed; notes.Visibility = Visibility.Collapsed;
            result.Text = "正在检查…";
            var outcome = await Task.Run(() => UpdateCheck.Check(UpdateCheck.CurrentVersion, UpdateCheck.FetchFromGitHub, DateTime.UtcNow));
            result.Text = outcome.Message;
            if (outcome.Status == UpdateStatus.Available) {
                releaseUrl = outcome.Latest.Url;
                notes.Text = outcome.Latest.Notes.Length > 0 ? "更新说明：\n" + outcome.Latest.Notes : "";
                notes.Visibility = notes.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
                open.Visibility = Visibility.Visible;
            }
            if (outcome.Status != UpdateStatus.NetworkError) Record(outcome);
            check.IsEnabled = true;
        };
        body.Children.Add(check); body.Children.Add(result); body.Children.Add(notes); body.Children.Add(open);
        var auto = new CheckBox { Name = "AutoUpdateCheck", Content = "每天自动检查一次", IsChecked = autoCheck, Margin = new Thickness(0,8,0,8) };
        body.Children.Add(auto);
        body.Children.Add(new TextBlock { Text = "开启后每次登录 Windows 时检查，每天最多一次，只在有新版本时提示。检查只向 GitHub 请求晴词的发布信息，不发送输入内容、个人词库或设备标识；晴词不会自动下载或安装，由你在发布页自行下载。默认关闭。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        return auto;
    }

    // Saves the automatic-check choice and its sign-in entry.
    internal static void Save(RegistryKey input, bool enabled)
    {
        UpdateCheckOptions.WriteAuto(input, enabled);
        string exe = UpdateCheckOptions.InstalledExe();
        if (exe == null || !File.Exists(exe)) exe = Assembly.GetEntryAssembly().Location;
        using (var run = Registry.CurrentUser.CreateSubKey(UpdateCheckOptions.RunKeyPath))
            UpdateCheckOptions.SetStartup(run, exe, enabled);
    }

    // A manual check also counts as today's check, and a version already shown is not announced again.
    private static void Record(UpdateResult outcome)
    {
        try {
            using (var input = Registry.CurrentUser.CreateSubKey(UpdateCheckOptions.InputKeyPath)) {
                UpdateCheckOptions.MarkChecked(input, DateTime.UtcNow);
                if (outcome.Status == UpdateStatus.Available) UpdateCheckOptions.WriteNotified(input, outcome.Latest.Version.ToString());
            }
        } catch (Exception) { /* Bookkeeping only. */ }
    }

    // --check-update, started at sign-in while automatic checking is on.
    internal static void RunScheduled()
    {
        bool created;
        using (var single = new Mutex(true, @"Local\HarukotoUpdateCheck", out created)) {
            if (!created) return;
            try { RunScheduledOnce(); } catch (Exception) { /* Never bother the user with a failed background check. */ }
        }
    }

    private static void RunScheduledOnce()
    {
        string own = Assembly.GetEntryAssembly().Location;
        UpdateResult outcome;
        using (var input = Registry.CurrentUser.OpenSubKey(UpdateCheckOptions.InputKeyPath, true)) {
            if (input == null || !UpdateCheckOptions.ReadAuto(input)) return;
            // After an update the newest renderer does the check; an uninstall removes the entry.
            string installed = UpdateCheckOptions.InstalledExe();
            using (var run = Registry.CurrentUser.CreateSubKey(UpdateCheckOptions.RunKeyPath)) {
                if (installed == null) { UpdateCheckOptions.SetStartup(run, null, false); return; }
                if (File.Exists(installed) && !string.Equals(Path.GetFullPath(installed), Path.GetFullPath(own), StringComparison.OrdinalIgnoreCase)) {
                    UpdateCheckOptions.SetStartup(run, installed, true);
                    Process.Start(new ProcessStartInfo(installed, "--check-update") { UseShellExecute = false });
                    return;
                }
            }
            if (!UpdateCheckOptions.IsDue(input, DateTime.UtcNow)) return;
            // Give the network a moment after sign-in.
            Thread.Sleep(TimeSpan.FromSeconds(30));
            outcome = UpdateCheck.Check(UpdateCheck.CurrentVersion, UpdateCheck.FetchFromGitHub, DateTime.UtcNow);
            // Without a connection the day is not used up; the next sign-in tries again.
            if (outcome.Status == UpdateStatus.NetworkError) return;
            UpdateCheckOptions.MarkChecked(input, DateTime.UtcNow);
            if (outcome.Status != UpdateStatus.Available) return;
            string version = outcome.Latest.Version.ToString();
            if (UpdateCheckOptions.ReadNotified(input) == version) return;
            UpdateCheckOptions.WriteNotified(input, version);
        }
        new Application().Run(CreateNoticeWindow(outcome));
    }

    internal static Window CreateNoticeWindow(UpdateResult outcome)
    {
        var window = new Window { Title = "晴词 · 有新版本", Width = 460, SizeToContent = SizeToContent.Height, ResizeMode = ResizeMode.NoResize,
            WindowStartupLocation = WindowStartupLocation.CenterScreen, ShowActivated = false,
            FontFamily = new System.Windows.Media.FontFamily("Microsoft YaHei UI"), FontSize = 14 };
        var body = new StackPanel { Margin = new Thickness(20), Background = SystemColors.WindowBrush };
        window.Content = body;
        body.Children.Add(new TextBlock { Text = outcome.Message, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,12) });
        if (outcome.Latest.Notes.Length > 0)
            body.Children.Add(new ScrollViewer { MaxHeight = 220, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Margin = new Thickness(0,0,0,12),
                Content = new TextBlock { Text = "更新说明：\n" + outcome.Latest.Notes, TextWrapping = TextWrapping.Wrap } });
        body.Children.Add(new TextBlock { Text = "可在“晴词候选窗设置”中关闭每日自动检查。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,12) });
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        var open = new Button { Content = "打开发布页", Padding = new Thickness(16,6,16,6), Margin = new Thickness(0,0,10,0), IsDefault = true };
        var later = new Button { Content = "以后再说", Padding = new Thickness(16,6,16,6), IsCancel = true };
        open.Click += (s,e) => { OpenReleasePage(outcome.Latest.Url); window.Close(); };
        later.Click += (s,e) => window.Close();
        actions.Children.Add(open); actions.Children.Add(later); body.Children.Add(actions);
        return window;
    }
}
