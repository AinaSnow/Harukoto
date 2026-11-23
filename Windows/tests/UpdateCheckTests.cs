using System;
using System.IO;
using System.Net;
using System.Text.RegularExpressions;
using Microsoft.Win32;

// UPD-01: fake responses and a fake clock; no network, only a throwaway HKCU test key.
internal static class UpdateCheckTests
{
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    static int Main(string[] args)
    {
        try { Run(args[0]); Console.WriteLine("UpdateCheckTests passed."); return 0; }
        catch (Exception e) { Console.Error.WriteLine(e.GetType().FullName + ": " + e.Message); return 1; }
    }

    static readonly DateTime Now = new DateTime(2026, 11, 23, 12, 0, 0, DateTimeKind.Utc);
    static ReleaseResponse Ok(string body) { return new ReleaseResponse { Status = 200, Body = body }; }
    static string Release(string tag, bool prerelease, bool draft = false, string url = null, string body = "")
    {
        return "{\"tag_name\":\"" + tag + "\",\"prerelease\":" + (prerelease ? "true" : "false") + ",\"draft\":" + (draft ? "true" : "false") +
            ",\"html_url\":\"" + (url ?? "https://github.com/AinaSnow/Harukoto/releases/tag/" + tag) + "\",\"body\":\"" + body + "\"}";
    }
    static UpdateResult Eval(string current, ReleaseResponse response) { return UpdateCheck.Evaluate(current, response, Now); }

    static void Run(string windows)
    {
        // Versions.
        Func<string, ReleaseVersion> v = ReleaseVersion.Parse;
        Check(v("0.4.4-alpha.36").CompareTo(v("0.4.4-alpha.37")) < 0, "alpha.36 < alpha.37");
        Check(v("0.4.4-alpha.9").CompareTo(v("0.4.4-alpha.10")) < 0, "numeric prerelease order");
        Check(v("0.4.4-alpha.40").CompareTo(v("0.4.4")) < 0, "prerelease < stable");
        Check(v("0.4.5-alpha.1").CompareTo(v("0.4.4")) > 0, "next patch prerelease > stable");
        Check(v("v0.4.4-alpha.37").ToString() == "0.4.4-alpha.37", "v prefix");
        Check(v("0.4.4-alpha.37").CompareTo(v("0.4.4-alpha.37")) == 0, "equal");
        Check(v("0.4.4-beta.1").CompareTo(v("0.4.4-alpha.99")) > 0, "beta > alpha");
        foreach (var bad in new[] { null, "", "latest", "0.4", "0.4.4-", "0.4.4-alpha..1", "1.2.3 extra", new string('9', 80) })
            Check(v(bad) == null, "rejects " + bad);

        // Newer prerelease available; drafts and unknown tags ignored; order of the list does not matter.
        string list = "[" + Release("v0.4.4-alpha.36", true) + "," + Release("v0.4.4-alpha.38", true, true) + "," + Release("nightly", true) + "," +
            Release("v0.4.4-alpha.37", true, false, null, "## 变化\\n- **候选窗** 热换代 [说明](https://example.com)\\n\\n```\\ncode\\n```") + "]";
        var r = Eval("0.4.4-alpha.36", Ok(list));
        Check(r.Status == UpdateStatus.Available && r.Latest.Version.ToString() == "0.4.4-alpha.37", "Available alpha.37");
        Check(r.Latest.Prerelease && r.Message.Contains("0.4.4-alpha.37") && r.Message.Contains("预发布") && r.Message.Contains("不会自动下载"), "Available message: " + r.Message);
        Check(r.Latest.Url == "https://github.com/AinaSnow/Harukoto/releases/tag/v0.4.4-alpha.37", "release page url");
        Check(r.Latest.Notes == "变化\n· 候选窗 热换代 说明", "notes summary: " + r.Latest.Notes);

        // Same or older only.
        Check(Eval("0.4.4-alpha.37", Ok(list)).Status == UpdateStatus.UpToDate, "same version up to date");
        var older = Eval("0.4.4-alpha.40", Ok(list));
        Check(older.Status == UpdateStatus.UpToDate && older.Message.Contains("已是最新") && older.Latest != null, "newer local up to date");

        // A stable build is offered stable releases only.
        string mixed = "[" + Release("v0.4.5-alpha.1", true) + "," + Release("v0.4.4", false) + "]";
        Check(Eval("0.4.4", Ok(mixed)).Status == UpdateStatus.UpToDate, "stable ignores prerelease");
        Check(Eval("0.4.4-alpha.36", Ok(mixed)).Latest.Version.ToString() == "0.4.5-alpha.1", "prerelease follows prereleases");
        var stable = Eval("0.4.4", Ok("[" + Release("v0.4.5", false) + "," + Release("v0.4.6-alpha.1", false) + "]"));
        Check(stable.Status == UpdateStatus.Available && stable.Latest.Version.ToString() == "0.4.5" && !stable.Message.Contains("预发布"), "stable update; tag with prerelease suffix is a prerelease");

        // Only this repository's release pages are opened.
        var foreign = Eval("0.4.4-alpha.36", Ok("[" + Release("v0.4.4-alpha.37", true, false, "https://evil.example/releases/tag/v0.4.4-alpha.37") + "]"));
        Check(foreign.Latest.Url == UpdateCheck.ReleasesPage, "foreign url replaced");
        Check(UpdateCheck.SafeReleaseUrl("https://github.com/AinaSnow/Harukoto/releases/tag/x/../../y") == UpdateCheck.ReleasesPage, "path traversal rejected");
        Check(UpdateCheck.SafeReleaseUrl(null) == UpdateCheck.ReleasesPage, "missing url");

        // Failures never throw and carry a clear message.
        Check(Eval("0.4.4-alpha.36", Ok("[]")).Status == UpdateStatus.NoRelease, "empty list");
        Check(Eval("0.4.4-alpha.36", Ok("[" + Release("nightly", true) + "]")).Status == UpdateStatus.NoRelease, "no usable release");
        foreach (var body in new[] { "not json", "{}", "{\"message\":\"x\"}", "", null, "[1,2", "[\"x\", null]" }) {
            var bad = Eval("0.4.4-alpha.36", Ok(body));
            Check(bad.Latest == null && (bad.Status == UpdateStatus.BadResponse || bad.Status == UpdateStatus.NoRelease) && bad.Message.Length > 0, "bad body " + body);
        }
        var limited = Eval("0.4.4-alpha.36", new ReleaseResponse { Status = 403, RateLimitRemaining = "0", RateLimitReset = "1795000000" });
        Check(limited.Status == UpdateStatus.RateLimited && limited.Message.Contains("限制") && limited.Message.Contains("后重试"), "rate limited: " + limited.Message);
        var retry = Eval("0.4.4-alpha.36", new ReleaseResponse { Status = 429, RetryAfter = "120" });
        Check(retry.Status == UpdateStatus.RateLimited && retry.Message.Contains(Now.AddSeconds(120).ToLocalTime().ToString("HH:mm")), "retry-after: " + retry.Message);
        Check(Eval("0.4.4-alpha.36", new ReleaseResponse { Status = 429 }).Message == "GitHub 暂时限制了检查次数，请稍后重试。", "rate limit without time");
        var forbidden = Eval("0.4.4-alpha.36", new ReleaseResponse { Status = 403, RateLimitRemaining = "12" });
        Check(forbidden.Status == UpdateStatus.BadResponse && forbidden.Message.Contains("403"), "plain 403");
        Check(Eval("0.4.4-alpha.36", new ReleaseResponse { Status = 500 }).Message.Contains("500"), "server error");
        Check(Eval("0.4.4-alpha.36", null).Status == UpdateStatus.NetworkError, "no response");
        var thrown = UpdateCheck.Check("0.4.4-alpha.36", () => { throw new WebException("timeout", WebExceptionStatus.Timeout); }, Now);
        Check(thrown.Status == UpdateStatus.NetworkError && thrown.Message.Contains("无法连接"), "network exception");
        Check(Eval("dev", Ok(list)).Status == UpdateStatus.BadResponse, "unknown local version");

        // Summary limits length and strips control characters.
        Check(UpdateCheck.Summary(new string('字', 700), 600).Length == 601, "summary truncated");
        Check(UpdateCheck.Summary("a\u0007b", 10) == "ab", "control characters");
        Check(UpdateCheck.Summary(null, 10) == "", "null notes");

        // The request carries no personal data: fixed URL, product-only agent, no cookies or credentials.
        var request = UpdateCheck.CreateRequest();
        Check(request.RequestUri.AbsoluteUri == "https://api.github.com/repos/AinaSnow/Harukoto/releases?per_page=20", "request url");
        Check(request.UserAgent == "Harukoto-UpdateCheck" && request.CookieContainer == null && !request.UseDefaultCredentials && request.Credentials == null, "request identity");
        Check(request.Headers["Cookie"] == null && request.Headers["Authorization"] == null, "no extra headers");

        // The local version matches the input service and installer.
        string service = File.ReadAllText(Path.Combine(windows, "TextService.cpp"));
        var serviceVersion = Regex.Match(service, "service_version\\\\\":\\\\\"([^\\\\]+)\\\\\"");
        Check(serviceVersion.Success && serviceVersion.Groups[1].Value == UpdateCheck.CurrentVersion, "UpdateCheck.CurrentVersion differs from service_version");
        var setupVersion = Regex.Match(File.ReadAllText(Path.Combine(windows, "Setup.cs")), "ProductVersion = \"([^\"]+)\"");
        Check(setupVersion.Success && setupVersion.Groups[1].Value == UpdateCheck.CurrentVersion, "UpdateCheck.CurrentVersion differs from Setup.ProductVersion");

        // Options and the sign-in entry, in a throwaway HKCU key.
        string path = @"Software\HarukotoUpdateCheckTests-" + Guid.NewGuid().ToString("N");
        try {
            using (var input = Registry.CurrentUser.CreateSubKey(path + @"\Input"))
            using (var run = Registry.CurrentUser.CreateSubKey(path + @"\Run")) {
                Check(!UpdateCheckOptions.ReadAuto(null) && !UpdateCheckOptions.ReadAuto(input) && !UpdateCheckOptions.HasChoice(input), "off by default");
                UpdateCheckOptions.WriteAuto(input, false);
                Check(input.GetValue(UpdateCheckOptions.AutoValue) == null, "saving off does not create the value");
                input.SetValue(UpdateCheckOptions.AutoValue, "1");
                Check(!UpdateCheckOptions.ReadAuto(input), "string value is off");
                UpdateCheckOptions.WriteAuto(input, true);
                Check(UpdateCheckOptions.ReadAuto(input) && UpdateCheckOptions.HasChoice(input), "on");
                UpdateCheckOptions.WriteAuto(input, false);
                Check((int)input.GetValue(UpdateCheckOptions.AutoValue) == 0, "turned off");
                input.DeleteValue(UpdateCheckOptions.AutoValue);
                UpdateCheckOptions.RecordChoice(input, false);
                Check(UpdateCheckOptions.HasChoice(input) && !UpdateCheckOptions.ReadAuto(input), "installer answer recorded");

                // Fake clock: at most once a day.
                Check(UpdateCheckOptions.IsDue(input, Now), "never checked is due");
                UpdateCheckOptions.MarkChecked(input, Now);
                Check(UpdateCheckOptions.ReadLastCheck(input).Value == Now, "last check stored");
                Check(!UpdateCheckOptions.IsDue(input, Now.AddHours(23)), "not due within a day");
                Check(UpdateCheckOptions.IsDue(input, Now.AddHours(24)), "due after a day");
                Check(UpdateCheckOptions.IsDue(input, Now.AddMinutes(-5)), "clock moved back is due");
                input.SetValue(UpdateCheckOptions.LastCheckValue, "garbage");
                Check(UpdateCheckOptions.IsDue(input, Now), "damaged last check is due");
                Check(UpdateCheckOptions.ReadNotified(input) == null, "nothing notified");
                UpdateCheckOptions.WriteNotified(input, "0.4.4-alpha.37");
                Check(UpdateCheckOptions.ReadNotified(input) == "0.4.4-alpha.37", "notified version");

                string exe = @"C:\Program Files\Meowpuut\IME\abc\MeowpuutGlassPreview.exe";
                UpdateCheckOptions.SetStartup(run, exe, true);
                Check((string)run.GetValue(UpdateCheckOptions.RunValue) == "\"" + exe + "\" --check-update", "sign-in entry");
                UpdateCheckOptions.SetStartup(run, exe, false);
                Check(run.GetValue(UpdateCheckOptions.RunValue) == null, "entry removed when off");
                UpdateCheckOptions.SetStartup(run, null, true);
                Check(run.GetValue(UpdateCheckOptions.RunValue) == null, "no entry without an executable");
                Check(UpdateCheckOptions.InstalledExe(@"C:\Program Files\Meowpuut\IME\abc\MeowpuutTip.dll") == exe, "renderer beside the service");
                Check(UpdateCheckOptions.InstalledExe(null) == null && UpdateCheckOptions.InstalledExe("") == null, "not registered");
            }
        } finally { Registry.CurrentUser.DeleteSubKeyTree(path, false); }
    }
}
