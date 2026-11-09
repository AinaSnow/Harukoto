using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

internal static class Setup
{
    internal const string ProductVersion = "0.4.4-alpha.6";
    private const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\Meowpuut";
    private static string work;
    [STAThread] private static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        bool uninstall = args.Length == 1 && args[0] == "--uninstall";
        if (args.Length == 1 && args[0] == "--verify") {
            try { Extract(); Run("Install.ps1", "-Prebuilt -VerifyOnly"); Run("Uninstall.ps1", "-VerifyOnly"); Environment.ExitCode = 0; }
            catch (Exception e) { Console.Error.WriteLine(e); Environment.ExitCode = 1; }
            finally { if (work != null) Directory.Delete(work, true); }
            return;
        }
        var form = new Form { Text = "晴词输入法安装", ClientSize = new System.Drawing.Size(520, 280),
            Font = new System.Drawing.Font("Microsoft YaHei UI", 10), AutoScaleMode = AutoScaleMode.None,
            StartPosition = FormStartPosition.CenterScreen, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false };
        var title = new Label { Text = uninstall ? "卸载晴词输入法" : "安装晴词输入法", Left = 24, Top = 24, Width = 470, Height = 36,
            Font = new System.Drawing.Font("Microsoft YaHei UI", 19) };
        var description = new Label { Left = 24, Top = 78, Width = 470, Height = 94, Text = uninstall
            ? "移除系统输入法注册和当前用户快捷方式。个人词表及外观设置保留。其他登录账户的语言列表需由对应账户清理。"
            : "版本 " + ProductVersion + " · Windows x64\n\n安装或更新输入法，保留个人词表与外观设置。\n继续时 Windows 将请求管理员授权。内置 PowerShell 7。" };
        var status = new Label { Left = 24, Top = 178, Width = 470, Height = 44, AutoEllipsis = true };
        var action = new Button { Text = uninstall ? "卸载" : "安装 / 更新", Left = 365, Top = 232, Width = 130, Height = 30 };
        var license = new LinkLabel { Text = "查看许可证", Left = 24, Top = 239, Width = 120 };
        license.LinkClicked += async (s,e) => {
            try { await Task.Run((Action)Extract); Process.Start(new ProcessStartInfo(Path.Combine(work,"LICENSE")) { UseShellExecute = true }); }
            catch (Exception error) { MessageBox.Show(form, error.Message, "许可证"); }
        };
        bool busy = false;
        form.FormClosing += (s,e) => { if (busy) e.Cancel = true; };
        action.Click += async (s,e) => {
            if (action.Text == "完成") { form.Close(); return; }
            busy = true; action.Enabled = false; license.Enabled = false;
            status.Text = "正在准备文件；请确认 Windows 管理员授权提示…";
            try {
                await Task.Run(() => {
                    Extract();
                    Run(uninstall ? "Uninstall.ps1" : "Install.ps1", uninstall ? "" : "-Prebuilt");
                    if (uninstall) Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
                    else {
                        string launcher = Path.Combine(work, "HarukotoSetup.exe");
                        string current = Assembly.GetExecutingAssembly().Location;
                        if (!String.Equals(current, launcher, StringComparison.OrdinalIgnoreCase)) File.Copy(current, launcher, true);
                        using (var key = Registry.CurrentUser.CreateSubKey(UninstallKey)) {
                            key.SetValue("DisplayName", "晴词 Harukoto 输入法");
                            key.SetValue("DisplayVersion", ProductVersion);
                            key.SetValue("Publisher", "Harukoto contributors");
                            key.SetValue("UninstallString", "\"" + launcher + "\" --uninstall");
                            key.SetValue("NoModify", 1); key.SetValue("NoRepair", 1);
                        }
                    }
                });
                status.Text = uninstall ? "卸载完成。个人数据及旧版本文件已保留。" : "安装完成！按 Win + 空格切换到 Meowpuut。请重新打开输入应用。";
                action.Text = "完成";
            } catch (Exception error) {
                status.Text = "未完成，可查看日志后重试。";
                MessageBox.Show(form, error.Message + "\n\n日志：" + Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Meowpuut", "Logs"), "晴词安装", MessageBoxButtons.OK, MessageBoxIcon.Error);
            } finally { busy = false; action.Enabled = true; license.Enabled = true; }
        };
        form.Controls.AddRange(new Control[] { title, description, status, action, license });
        if (args.Length == 2 && args[0] == "--render") {
            form.ShowInTaskbar = false;
            form.StartPosition = FormStartPosition.Manual;
            form.Location = new System.Drawing.Point(-10000, -10000);
            form.Show();
            form.PerformLayout();
            using (var bitmap = new System.Drawing.Bitmap(form.Width, form.Height)) {
                form.DrawToBitmap(bitmap, new System.Drawing.Rectangle(0, 0, form.Width, form.Height));
                bitmap.Save(args[1], System.Drawing.Imaging.ImageFormat.Png);
            }
            form.Dispose();
            return;
        }
        Application.Run(form);
    }
    private static readonly object extractionLock = new object();
    private static void Extract()
    {
        lock (extractionLock) {
            if (work != null) return;
            string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Meowpuut", "Setup", ProductVersion);
            string destination = Path.Combine(root, Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(destination);
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Read)) {
                foreach (var entry in archive.Entries) {
                    string path = Path.GetFullPath(Path.Combine(destination, entry.FullName));
                    if (!path.StartsWith(destination + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Invalid archive path.");
                    if (entry.Name.Length == 0) { Directory.CreateDirectory(path); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    entry.ExtractToFile(path);
                }
            }
            work = destination;
        }
    }
    private static void Run(string script, string arguments)
    {
        var info = new ProcessStartInfo(Path.Combine(work, "PowerShell", "pwsh.exe"),
            "-NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(work, script) + "\" " + arguments) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        using (var process = Process.Start(info)) {
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            process.WaitForExit(); Task.WaitAll(output, error);
            if (process.ExitCode != 0) throw new InvalidOperationException("安装操作失败（" + process.ExitCode + "）\n" + error.Result);
        }
    }
}
