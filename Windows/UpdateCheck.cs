using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using Microsoft.Win32;

// UPD-01: check GitHub Releases for a newer Harukoto. Only the public release
// list is requested; nothing typed, no personal words and no device identifier
// is sent. Harukoto never downloads or runs an installer: a newer release is
// shown and its page opened in the browser. No WPF here so tests can compile it alone.

// A version such as 0.4.4-alpha.37, compared the semantic-versioning way.
internal sealed class ReleaseVersion : IComparable<ReleaseVersion>
{
    internal int Major, Minor, Patch;
    internal string[] Pre = new string[0];
    internal bool IsPrerelease { get { return Pre.Length > 0; } }
    private static readonly Regex Pattern = new Regex(@"^v?(\d{1,6})\.(\d{1,6})\.(\d{1,6})(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$", RegexOptions.CultureInvariant);
    internal static ReleaseVersion Parse(string text)
    {
        if (text == null || text.Length > 64) return null;
        var m = Pattern.Match(text.Trim());
        if (!m.Success) return null;
        var v = new ReleaseVersion { Major = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture),
            Minor = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture), Patch = int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture) };
        if (m.Groups[4].Success) v.Pre = m.Groups[4].Value.Split('.');
        return v;
    }
    public int CompareTo(ReleaseVersion other)
    {
        int c = Major.CompareTo(other.Major); if (c != 0) return c;
        c = Minor.CompareTo(other.Minor); if (c != 0) return c;
        c = Patch.CompareTo(other.Patch); if (c != 0) return c;
        if (Pre.Length == 0 || other.Pre.Length == 0) return other.Pre.Length.CompareTo(Pre.Length);
        for (int i = 0; i < Math.Min(Pre.Length, other.Pre.Length); ++i) {
            int a, b;
            bool an = int.TryParse(Pre[i], NumberStyles.None, CultureInfo.InvariantCulture, out a);
            bool bn = int.TryParse(other.Pre[i], NumberStyles.None, CultureInfo.InvariantCulture, out b);
            if (an && bn) c = a.CompareTo(b);
            else if (an != bn) c = an ? -1 : 1;
            else c = string.CompareOrdinal(Pre[i], other.Pre[i]);
            if (c != 0) return c;
        }
        return Pre.Length.CompareTo(other.Pre.Length);
    }
    public override string ToString()
    {
        string core = Major.ToString(CultureInfo.InvariantCulture) + "." + Minor.ToString(CultureInfo.InvariantCulture) + "." + Patch.ToString(CultureInfo.InvariantCulture);
        return Pre.Length == 0 ? core : core + "-" + string.Join(".", Pre);
    }
}

internal sealed class ReleaseInfo
{
    internal ReleaseVersion Version;
    internal string Url, Notes;
    internal bool Prerelease;
}

// What the request returned. Status 0 means no HTTP response at all.
internal sealed class ReleaseResponse
{
    internal int Status;
    internal string Body, RateLimitRemaining, RateLimitReset, RetryAfter;
}

internal enum UpdateStatus { UpToDate, Available, NoRelease, NetworkError, RateLimited, BadResponse }

internal sealed class UpdateResult
{
    internal UpdateStatus Status;
    internal ReleaseVersion Current;
    internal ReleaseInfo Latest;
    internal string Message;
}

internal static class UpdateCheck
{
    // Must equal service_version in TextService.cpp and Setup.ProductVersion;
    // UpdateCheckTests fails the build when a version bump misses this file.
    internal const string CurrentVersion = "0.4.4-alpha.36";
    internal const string ReleasesPage = "https://github.com/AinaSnow/Harukoto/releases";
    internal const string ReleasesApi = "https://api.github.com/repos/AinaSnow/Harukoto/releases?per_page=20";
    private const int MaxBody = 2 * 1024 * 1024;

    // Builds the request without sending it: no cookies, no credentials, a fixed
    // product-only User-Agent (GitHub rejects requests without one).
    internal static HttpWebRequest CreateRequest()
    {
        var request = (HttpWebRequest)WebRequest.Create(ReleasesApi);
        request.Method = "GET";
        request.UserAgent = "Harukoto-UpdateCheck";
        request.Accept = "application/vnd.github+json";
        request.Headers["X-GitHub-Api-Version"] = "2022-11-28";
        request.Timeout = 15000; request.ReadWriteTimeout = 15000;
        request.AllowAutoRedirect = true; request.UseDefaultCredentials = false; request.CookieContainer = null;
        request.CachePolicy = new System.Net.Cache.RequestCachePolicy(System.Net.Cache.RequestCacheLevel.NoCacheNoStore);
        return request;
    }

    internal static ReleaseResponse FetchFromGitHub()
    {
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        HttpWebResponse response = null;
        try {
            try { response = (HttpWebResponse)CreateRequest().GetResponse(); }
            catch (WebException e) {
                response = e.Response as HttpWebResponse;
                if (response == null) throw;
            }
            var result = new ReleaseResponse { Status = (int)response.StatusCode,
                RateLimitRemaining = response.Headers["X-RateLimit-Remaining"], RateLimitReset = response.Headers["X-RateLimit-Reset"],
                RetryAfter = response.Headers["Retry-After"] };
            using (var stream = response.GetResponseStream())
            using (var buffer = new MemoryStream()) {
                var chunk = new byte[16384]; int read;
                while ((read = stream.Read(chunk, 0, chunk.Length)) > 0) {
                    if (buffer.Length + read > MaxBody) { result.Body = null; return result; }
                    buffer.Write(chunk, 0, read);
                }
                result.Body = Encoding.UTF8.GetString(buffer.ToArray());
            }
            return result;
        } finally { if (response != null) response.Close(); }
    }

    // Never throws: any failure of the fetch becomes a Chinese message.
    internal static UpdateResult Check(string current, Func<ReleaseResponse> fetch, DateTime utcNow)
    {
        ReleaseResponse response;
        try { response = fetch(); }
        catch (Exception) { response = null; }
        return Evaluate(current, response, utcNow);
    }

    internal static UpdateResult Evaluate(string current, ReleaseResponse response, DateTime utcNow)
    {
        var result = new UpdateResult { Current = ReleaseVersion.Parse(current) };
        if (result.Current == null) return Fail(result, UpdateStatus.BadResponse, "无法识别本机晴词版本（" + current + "），请重新安装后再试。");
        if (response == null || response.Status == 0)
            return Fail(result, UpdateStatus.NetworkError, "无法连接 GitHub，请检查网络或代理后重试。");
        if (response.Status == 429 || (response.Status == 403 && response.RateLimitRemaining == "0"))
            return Fail(result, UpdateStatus.RateLimited, RateLimitMessage(response, utcNow));
        if (response.Status != 200)
            return Fail(result, UpdateStatus.BadResponse, "GitHub 返回异常（状态 " + response.Status.ToString(CultureInfo.InvariantCulture) + "），请稍后重试。");
        object[] list = null;
        try { list = new JavaScriptSerializer().DeserializeObject(response.Body ?? "") as object[]; } catch (Exception) { }
        if (list == null) return Fail(result, UpdateStatus.BadResponse, "GitHub 返回的数据无法识别，请稍后重试。");
        // A prerelease build follows prereleases; a stable build only offers stable releases.
        bool acceptPrerelease = result.Current.IsPrerelease;
        foreach (var item in list) {
            var release = item as Dictionary<string, object>;
            if (release == null || Flag(release, "draft")) continue;
            var version = ReleaseVersion.Parse(Text(release, "tag_name"));
            if (version == null) continue;
            bool prerelease = Flag(release, "prerelease") || version.IsPrerelease;
            if (prerelease && !acceptPrerelease) continue;
            if (result.Latest == null || version.CompareTo(result.Latest.Version) > 0)
                result.Latest = new ReleaseInfo { Version = version, Prerelease = prerelease,
                    Url = SafeReleaseUrl(Text(release, "html_url")), Notes = Summary(Text(release, "body"), 600) };
        }
        if (result.Latest == null) return Fail(result, UpdateStatus.NoRelease, "GitHub 上暂未找到可用的晴词发布版本。");
        if (result.Latest.Version.CompareTo(result.Current) > 0) {
            result.Status = UpdateStatus.Available;
            result.Message = "有新版本 " + result.Latest.Version + (result.Latest.Prerelease ? "（预发布）" : "") + "，当前为 " + result.Current + "。晴词不会自动下载，请在发布页下载安装。";
        } else {
            result.Status = UpdateStatus.UpToDate;
            result.Message = "已是最新版本（" + result.Current + "）。";
        }
        return result;
    }

    private static UpdateResult Fail(UpdateResult result, UpdateStatus status, string message)
    {
        result.Status = status; result.Message = message; result.Latest = null; return result;
    }

    private static string RateLimitMessage(ReleaseResponse response, DateTime utcNow)
    {
        long seconds;
        DateTime? until = null;
        if (long.TryParse(response.RateLimitReset, NumberStyles.None, CultureInfo.InvariantCulture, out seconds) && seconds > 0 && seconds < 253402300799L)
            until = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(seconds);
        else if (long.TryParse(response.RetryAfter, NumberStyles.None, CultureInfo.InvariantCulture, out seconds) && seconds > 0 && seconds < 86400 * 7)
            until = utcNow.AddSeconds(seconds);
        if (until.HasValue && until.Value > utcNow)
            return "GitHub 暂时限制了检查次数，请在 " + until.Value.ToLocalTime().ToString("HH:mm", CultureInfo.InvariantCulture) + " 后重试。";
        return "GitHub 暂时限制了检查次数，请稍后重试。";
    }

    private static string Text(Dictionary<string, object> map, string name)
    {
        object value; return map.TryGetValue(name, out value) ? value as string : null;
    }
    private static bool Flag(Dictionary<string, object> map, string name)
    {
        object value; return map.TryGetValue(name, out value) && value is bool && (bool)value;
    }

    // Only a page of this repository's releases may be opened.
    internal static string SafeReleaseUrl(string url)
    {
        const string prefix = ReleasesPage + "/tag/";
        if (url == null || !url.StartsWith(prefix, StringComparison.Ordinal) || url.Length > 256) return ReleasesPage;
        string tag = url.Substring(prefix.Length);
        return Regex.IsMatch(tag, @"^[0-9A-Za-z._-]+$") ? url : ReleasesPage;
    }

    // Plain-text summary of Markdown release notes.
    internal static string Summary(string markdown, int max)
    {
        if (string.IsNullOrEmpty(markdown)) return "";
        var lines = new List<string>();
        bool fence = false;
        foreach (var raw in markdown.Replace("\r\n", "\n").Split('\n')) {
            string line = raw.Trim();
            if (line.StartsWith("```", StringComparison.Ordinal)) { fence = !fence; continue; }
            if (fence || line.Length == 0 || line.StartsWith("|", StringComparison.Ordinal)) continue;
            line = Regex.Replace(line, @"^#{1,6}\s*", "");
            line = Regex.Replace(line, @"^[-*+]\s+", "· ");
            line = Regex.Replace(line, @"!?\[([^\]]*)\]\([^)]*\)", "$1");
            line = line.Replace("**", "").Replace("`", "");
            line = Regex.Replace(line, @"[\u0000-\u0008\u000B-\u001F\u007F]", "");
            if (line.Length > 0) lines.Add(line);
        }
        string text = string.Join("\n", lines);
        return text.Length <= max ? text : text.Substring(0, max).TrimEnd() + "…";
    }
}

// Options under HKCU\Software\Meowpuut\Input, plus the sign-in entry that runs
// the daily check. Keys are passed in so tests use a throwaway HKCU subkey.
internal static class UpdateCheckOptions
{
    internal const string InputKeyPath = @"Software\Meowpuut\Input";
    internal const string AutoValue = "AutoUpdateCheck", LastCheckValue = "UpdateLastCheck", NotifiedValue = "UpdateNotifiedVersion";
    internal const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run", RunValue = "HarukotoUpdateCheck";
    internal const string TipServerKey = @"Software\Classes\CLSID\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\InprocServer32";
    internal const string ExeName = "MeowpuutGlassPreview.exe";
    internal static readonly TimeSpan Interval = TimeSpan.FromHours(24);

    // Off unless the DWORD is 1.
    internal static bool ReadAuto(RegistryKey input)
    {
        if (input == null) return false;
        var value = input.GetValue(AutoValue);
        return value is int && (int)value == 1;
    }
    internal static bool HasChoice(RegistryKey input) { return input != null && input.GetValue(AutoValue) is int; }
    // Leaves an unchanged value alone, so saving other settings never creates it.
    internal static void WriteAuto(RegistryKey input, bool enabled)
    {
        if (ReadAuto(input) == enabled && (input.GetValue(AutoValue) is int || !enabled)) return;
        input.SetValue(AutoValue, enabled ? 1 : 0, RegistryValueKind.DWord);
    }
    // The installer records the first-install answer either way so it asks once.
    internal static void RecordChoice(RegistryKey input, bool enabled)
    {
        input.SetValue(AutoValue, enabled ? 1 : 0, RegistryValueKind.DWord);
    }

    internal static DateTime? ReadLastCheck(RegistryKey input)
    {
        var value = input == null ? null : input.GetValue(LastCheckValue);
        if (!(value is long)) return null;
        long ticks = (long)value;
        if (ticks < DateTime.MinValue.Ticks || ticks > DateTime.MaxValue.Ticks) return null;
        return new DateTime(ticks, DateTimeKind.Utc);
    }
    // Due when never checked, a day has passed, or the clock went backwards.
    internal static bool IsDue(RegistryKey input, DateTime utcNow)
    {
        var last = ReadLastCheck(input);
        return !last.HasValue || utcNow < last.Value || utcNow - last.Value >= Interval;
    }
    internal static void MarkChecked(RegistryKey input, DateTime utcNow)
    {
        input.SetValue(LastCheckValue, utcNow.ToUniversalTime().Ticks, RegistryValueKind.QWord);
    }
    internal static string ReadNotified(RegistryKey input)
    {
        return input == null ? null : input.GetValue(NotifiedValue) as string;
    }
    internal static void WriteNotified(RegistryKey input, string version)
    {
        input.SetValue(NotifiedValue, version, RegistryValueKind.String);
    }

    internal static string StartupCommand(string exe) { return "\"" + exe + "\" --check-update"; }
    // The sign-in entry exists only while automatic checking is on.
    internal static void SetStartup(RegistryKey run, string exe, bool enabled)
    {
        if (!enabled || string.IsNullOrEmpty(exe)) { if (run.GetValue(RunValue) != null) run.DeleteValue(RunValue, false); return; }
        string command = StartupCommand(exe);
        if (run.GetValue(RunValue) as string != command) run.SetValue(RunValue, command, RegistryValueKind.String);
    }

    // The renderer beside the registered input service, or null when Harukoto is not registered.
    internal static string InstalledExe(string registeredDll)
    {
        if (string.IsNullOrEmpty(registeredDll)) return null;
        try {
            string folder = Path.GetDirectoryName(registeredDll);
            return string.IsNullOrEmpty(folder) ? null : Path.Combine(folder, ExeName);
        } catch (ArgumentException) { return null; }
    }
    internal static string InstalledExe()
    {
        try {
            using (var key = Registry.LocalMachine.OpenSubKey(TipServerKey))
                return InstalledExe(key == null ? null : key.GetValue("") as string);
        } catch { return null; }
    }
}
