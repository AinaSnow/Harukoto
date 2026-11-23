// DICT-03: theme packages and the personal word list without Python.
// A port of scripts/japanese_packages.py and scripts/japanese_dictionary.py:
// the same manifest and word-list rules, the same immutable snapshot layout
// (CURRENT -> <generation>/state.json + enabled.tsv) read by the input service
// and ThemeReload, and the same .lock byte, so this and the Python tools
// exclude each other. state.json and enabled.tsv are written byte for byte
// as the Python version writes them (tests/DictionaryManagerTests.ps1).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

internal sealed class ThemeError : Exception
{
    internal ThemeError(string message) : base(message) { }
}

public sealed class ThemeEntry
{
    public string Reading, Text, Kana, Source, Chinese;
    public int Weight;

    internal bool SameAs(ThemeEntry other)
    {
        return Reading == other.Reading && Text == other.Text && Weight == other.Weight && Kana == other.Kana
            && Source == other.Source && Chinese == other.Chinese;
    }

    internal string Line()
    {
        return Reading + "|" + Text + "|" + Weight.ToString(CultureInfo.InvariantCulture) + "|" + Kana + "|" + Source + "|" + Chinese + "\n";
    }
}

public sealed class ThemeManifest
{
    public string Id, Name, Version, Source, License, Words;

    internal bool SameAs(ThemeManifest other)
    {
        return Id == other.Id && Name == other.Name && Version == other.Version && Source == other.Source
            && License == other.License && Words == other.Words;
    }
}

public sealed class ThemePackage
{
    public ThemeManifest Manifest;
    public List<ThemeEntry> Entries;

    internal bool SameAs(ThemePackage other)
    {
        if (other == null || !Manifest.SameAs(other.Manifest) || Entries.Count != other.Entries.Count) return false;
        for (int i = 0; i < Entries.Count; i++)
            if (!Entries[i].SameAs(other.Entries[i])) return false;
        return true;
    }
}

public sealed class ThemeItem
{
    public bool Enabled;
    public ThemePackage Current, Previous;
}

public sealed class ThemeState
{
    // Package ids are ASCII, so ordinal order is Python's sorted() order.
    public SortedDictionary<string, ThemeItem> Packages = new SortedDictionary<string, ThemeItem>(StringComparer.Ordinal);
}

public sealed class ThemeConflict
{
    public string Trigger, Package, Kind, Incoming, Existing;
    public bool Enabled;
}

public sealed class ThemeConflicts
{
    public List<ThemeConflict> Items = new List<ThemeConflict>();
    public bool Limited;
}

internal static class ThemePackages
{
    internal const long MaxBytes = 256L * 1024 * 1024;
    internal const int MaxEntries = 500000;
    static readonly Regex Id = new Regex(@"\A[a-z][a-z0-9-]{0,63}\z");
    static readonly Regex Generation = new Regex(@"\A[0-9a-f]{32}\z");
    static readonly Regex VersionPattern = new Regex(@"\A[A-Za-z0-9][A-Za-z0-9.+-]{0,63}\z");
    static readonly Regex WordsPattern = new Regex(@"\A[A-Za-z0-9_-][A-Za-z0-9_.-]{0,79}[.]txt\z");
    static readonly Regex ReadingPattern = new Regex(@"\A[a-z]{2,256}\z");
    static readonly Regex KanaPattern = new Regex(@"\A[ぁ-ゖー]{0,128}\z");
    static readonly string[] ManifestFields = { "format", "id", "name", "version", "source", "license", "words" };

    // Test hook: runs just before CURRENT is replaced (simulates a failed activation).
    internal static Action BeforeActivate;

    internal static ThemeState Empty() { return new ThemeState(); }

    // ---- manifest and package ----

    static ThemeManifest ValidateManifest(object value)
    {
        var manifest = value as Dictionary<string, object>;
        if (manifest == null || !Json.HasExactly(manifest, ManifestFields))
            throw new ThemeError("清单字段必须为 format、id、name、version、source、license、words");
        if (!(manifest["format"] is long) || (long)manifest["format"] != 1)
            throw new ThemeError("不支持的主题包格式");
        foreach (var field in ManifestFields) {
            if (field == "format") continue;
            var text = manifest[field] as string;
            if (text == null || Py.Strip(text).Length == 0 || Py.Length(text) > 1024 || HasControl(text))
                throw new ThemeError("清单字段无效：" + field);
        }
        var result = new ThemeManifest { Id = (string)manifest["id"], Name = (string)manifest["name"], Version = (string)manifest["version"],
            Source = (string)manifest["source"], License = (string)manifest["license"], Words = (string)manifest["words"] };
        if (!Id.IsMatch(result.Id))
            throw new ThemeError("id 须以小写字母开头，只含小写字母、数字和短横线，最多 64 字");
        if (Py.Length(result.Name) > 80 || !VersionPattern.IsMatch(result.Version))
            throw new ThemeError("名称最多 80 字；版本须为 1–64 个字母、数字、点、加号或短横线");
        // Only a sibling plain-text file; never interpret package data as a path outside its directory.
        if (!WordsPattern.IsMatch(result.Words))
            throw new ThemeError("words 须为同目录下的 .txt 文件名，不能包含路径");
        return result;
    }

    internal static ThemePackage LoadPackage(string path)
    {
        path = Path.GetFullPath(path);
        var manifest = ValidateManifest(Json.Parse(ReadLimited(path, 64 * 1024)));
        var words = Path.Combine(Path.GetDirectoryName(path), manifest.Words);
        if (File.Exists(words) && (File.GetAttributes(words) & FileAttributes.ReparsePoint) != 0)
            throw new ThemeError("词表不能链接到包目录之外");
        var entries = WordList.Parse(words);
        if (entries.Count == 0)
            throw new ThemeError("主题包没有有效的拼音／英语触发词");
        var package = new ThemePackage { Manifest = manifest, Entries = entries };
        ValidatePackage(package);
        return package;
    }

    static void ValidatePackage(ThemePackage package)
    {
        if (package.Entries.Count < 1 || package.Entries.Count > MaxEntries)
            throw new ThemeError("主题包条目数无效");
        foreach (var row in package.Entries) {
            int textLength = Py.Length(row.Text);
            if (!ReadingPattern.IsMatch(row.Reading) || textLength < 1 || textLength > 128 || !KanaPattern.IsMatch(row.Kana)
                    || (row.Source != "pinyin" && row.Source != "english") || Py.Length(row.Chinese) > 128
                    || row.Weight < 1 || row.Weight > 9999
                    || (row.Text + row.Chinese).IndexOfAny(new[] { '|', '\t', '\r', '\n' }) >= 0
                    || HasControl(row.Text + row.Chinese))
                throw new ThemeError("主题包候选内容损坏");
        }
    }

    static bool HasControl(string text)
    {
        foreach (char c in text) if (c < 32) return true;
        return false;
    }

    // ---- state.json <-> objects ----

    static ThemePackage PackageFromJson(object value)
    {
        var package = value as Dictionary<string, object>;
        if (package == null || !Json.HasExactly(package, "manifest", "entries"))
            throw new ThemeError("主题包快照损坏");
        var manifest = ValidateManifest(package["manifest"]);
        var rows = package["entries"] as List<object>;
        if (rows == null || rows.Count < 1 || rows.Count > MaxEntries)
            throw new ThemeError("主题包条目数无效");
        var entries = new List<ThemeEntry>(rows.Count);
        foreach (var item in rows) {
            var row = item as List<object>;
            if (row == null || row.Count != 6)
                throw new ThemeError("主题包候选结构损坏");
            if (!(row[0] is string) || !(row[1] is string) || !(row[2] is long) || !(row[3] is string) || !(row[4] is string) || !(row[5] is string))
                throw new ThemeError("主题包候选内容损坏");
            long weight = (long)row[2];
            if (weight < 1 || weight > 9999)
                throw new ThemeError("主题包候选内容损坏");
            entries.Add(new ThemeEntry { Reading = (string)row[0], Text = (string)row[1], Weight = (int)weight,
                Kana = (string)row[3], Source = (string)row[4], Chinese = (string)row[5] });
        }
        var result = new ThemePackage { Manifest = manifest, Entries = entries };
        ValidatePackage(result);
        return result;
    }

    static ThemeState StateFromJson(object value)
    {
        var state = value as Dictionary<string, object>;
        if (state == null || !Json.HasExactly(state, "format", "packages") || !(state["format"] is long) || (long)state["format"] != 1)
            throw new ThemeError("词库状态格式损坏");
        var packages = state["packages"] as Dictionary<string, object>;
        if (packages == null || packages.Count > 128)
            throw new ThemeError("主题包不能超过 128 个");
        var result = new ThemeState();
        foreach (var pair in packages) {
            var item = pair.Value as Dictionary<string, object>;
            if (item == null || !Json.HasExactly(item, "enabled", "current", "previous") || !(item["enabled"] is bool))
                throw new ThemeError("主题包状态损坏");
            var entry = new ThemeItem { Enabled = (bool)item["enabled"] };
            foreach (var name in new[] { "current", "previous" }) {
                if (item[name] == null) continue;
                var package = PackageFromJson(item[name]);
                if (package.Manifest.Id != pair.Key)
                    throw new ThemeError("主题包 ID 与索引不一致");
                if (name == "current") entry.Current = package; else entry.Previous = package;
            }
            if (entry.Current == null)
                throw new ThemeError("主题包缺少当前版本");
            result.Packages.Add(pair.Key, entry);
        }
        ValidateState(result);
        return result;
    }

    static void ValidateState(ThemeState state)
    {
        if (state.Packages.Count > 128)
            throw new ThemeError("主题包不能超过 128 个");
        long count = 0;
        foreach (var pair in state.Packages) {
            if (pair.Value.Current == null)
                throw new ThemeError("主题包缺少当前版本");
            foreach (var package in new[] { pair.Value.Current, pair.Value.Previous }) {
                if (package == null) continue;
                ValidatePackage(package);
                if (package.Manifest.Id != pair.Key)
                    throw new ThemeError("主题包 ID 与索引不一致");
                count += package.Entries.Count;
            }
        }
        if (count > MaxEntries)
            throw new ThemeError("主题包总条目（含回退版本）不能超过 500000 条");
    }

    static object PackageToJson(ThemePackage package)
    {
        if (package == null) return null;
        var m = package.Manifest;
        var manifest = new Dictionary<string, object> { { "format", 1L }, { "id", m.Id }, { "name", m.Name }, { "version", m.Version },
            { "source", m.Source }, { "license", m.License }, { "words", m.Words } };
        var rows = new List<object>(package.Entries.Count);
        foreach (var e in package.Entries)
            rows.Add(new List<object> { e.Reading, e.Text, (long)e.Weight, e.Kana, e.Source, e.Chinese });
        return new Dictionary<string, object> { { "manifest", manifest }, { "entries", rows } };
    }

    internal static string StateJson(ThemeState state)
    {
        var packages = new Dictionary<string, object>();
        foreach (var pair in state.Packages)
            packages.Add(pair.Key, new Dictionary<string, object> { { "enabled", pair.Value.Enabled },
                { "current", PackageToJson(pair.Value.Current) }, { "previous", PackageToJson(pair.Value.Previous) } });
        return Json.Write(new Dictionary<string, object> { { "format", 1L }, { "packages", packages } }) + "\n";
    }

    // ---- store ----

    internal static ThemeState ReadState(string root)
    {
        byte[] raw;
        try {
            using (var pointer = new FileStream(Path.Combine(root, "CURRENT"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
                raw = new byte[34];
                int length = 0, read;
                while (length < raw.Length && (read = pointer.Read(raw, length, raw.Length - length)) > 0) length += read;
                Array.Resize(ref raw, length);
            }
        } catch (FileNotFoundException) { return Empty(); }
        catch (DirectoryNotFoundException) { return Empty(); }
        foreach (byte b in raw) if (b > 127) throw new ThemeError("主题包激活指针损坏");
        var generation = Encoding.ASCII.GetString(raw);
        if (!Generation.IsMatch(generation))
            throw new ThemeError("主题包激活指针损坏");
        return StateFromJson(Json.Parse(ReadLimited(Path.Combine(root, generation, "state.json"), MaxBytes)));
    }

    internal static byte[] Compiled(ThemeState state)
    {
        var rows = new List<ThemeEntry>();
        foreach (var pair in state.Packages)
            if (pair.Value.Enabled) rows.AddRange(pair.Value.Current.Entries);
        // Exact duplicates across packages need only one runtime row. Conflicting meanings coexist.
        rows.Sort(CompileOrder);
        var builder = new StringBuilder();
        ThemeEntry last = null;
        foreach (var row in rows) {
            if (last != null && last.SameAs(row)) continue;
            builder.Append(row.Line()); last = row;
        }
        var result = new UTF8Encoding(false, true).GetBytes(builder.ToString());
        if (result.LongLength > MaxBytes)
            throw new ThemeError("合并词库超过 256 MB");
        return result;
    }

    static int CompileOrder(ThemeEntry a, ThemeEntry b)
    {
        int c = b.Weight.CompareTo(a.Weight);
        if (c == 0) c = Py.Compare(a.Reading, b.Reading);
        if (c == 0) c = Py.Compare(a.Text, b.Text);
        if (c == 0) c = Py.Compare(a.Kana, b.Kana);
        if (c == 0) c = Py.Compare(a.Source, b.Source);
        if (c == 0) c = Py.Compare(a.Chinese, b.Chinese);
        return c;
    }

    internal sealed class WriterLock : IDisposable
    {
        FileStream file;

        internal WriterLock(string root)
        {
            Directory.CreateDirectory(root);
            file = new FileStream(Path.Combine(root, ".lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete);
            try {
                if (file.Length == 0) { file.Seek(0, SeekOrigin.End); file.WriteByte((byte)'0'); file.Flush(); }
                // The same LockFile byte the Python tool takes with msvcrt.locking.
                file.Lock(0, 1);
            } catch (IOException) {
                file.Dispose(); file = null;
                throw new ThemeError("其他词库管理操作正在执行，请稍后重试");
            }
        }

        public void Dispose()
        {
            if (file == null) return;
            try { file.Unlock(0, 1); } catch (IOException) { }
            file.Dispose(); file = null;
        }
    }

    static void WriteDurable(string path, byte[] data)
    {
        using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, FileOptions.WriteThrough)) {
            stream.Write(data, 0, data.Length);
            stream.Flush(true);
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileEx(string existing, string replacement, int flags);

    // Atomic replace, like os.replace (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
    internal static void Replace(string source, string destination)
    {
        if (!MoveFileEx(source, destination, 0x1 | 0x8))
            throw new IOException("无法替换文件：" + destination, Marshal.GetHRForLastWin32Error());
    }

    static void WriteState(string root, ThemeState state)
    {
        ValidateState(state);
        var payload = Compiled(state);
        var metadata = new UTF8Encoding(false, true).GetBytes(StateJson(state));
        if (metadata.LongLength > MaxBytes)
            throw new ThemeError("主题包状态超过 256 MB");
        var generation = Guid.NewGuid().ToString("N");
        var folder = Path.Combine(root, generation);
        if (Directory.Exists(folder)) throw new IOException("快照目录已存在：" + folder);
        Directory.CreateDirectory(folder);
        // Readers use one immutable snapshot, even while the next update is being prepared.
        WriteDurable(Path.Combine(folder, "state.json"), metadata);
        WriteDurable(Path.Combine(folder, "enabled.tsv"), payload);
        var temporary = Path.Combine(root, "tmp" + Guid.NewGuid().ToString("N"));
        try {
            WriteDurable(temporary, Encoding.ASCII.GetBytes(generation));
            var hook = BeforeActivate;
            if (hook != null) hook();
            Replace(temporary, Path.Combine(root, "CURRENT"));
        } finally {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    internal static ThemeConflicts Conflicts(ThemeState state, ThemePackage package)
    {
        var index = new Dictionary<string, List<KeyValuePair<string, ThemeEntry>>>(StringComparer.Ordinal);
        var enabled = new Dictionary<string, bool>(StringComparer.Ordinal);
        foreach (var pair in state.Packages) {
            if (pair.Key == package.Manifest.Id) continue;
            enabled[pair.Key] = pair.Value.Enabled;
            foreach (var row in pair.Value.Current.Entries) {
                List<KeyValuePair<string, ThemeEntry>> list;
                if (!index.TryGetValue(row.Reading, out list)) index.Add(row.Reading, list = new List<KeyValuePair<string, ThemeEntry>>());
                list.Add(new KeyValuePair<string, ThemeEntry>(pair.Key, row));
            }
        }
        var result = new ThemeConflicts();
        foreach (var row in package.Entries) {
            List<KeyValuePair<string, ThemeEntry>> list;
            if (!index.TryGetValue(row.Reading, out list)) continue;
            foreach (var other in list) {
                result.Items.Add(new ThemeConflict { Trigger = row.Reading, Package = other.Key, Enabled = enabled[other.Key],
                    Kind = row.Text == other.Value.Text && row.Kana == other.Value.Kana ? "duplicate" : "alternative",
                    Incoming = row.Text, Existing = other.Value.Text });
                if (result.Items.Count == 100) { result.Limited = true; return result; }
            }
        }
        return result;
    }

    // operation: import (value = manifest path), enable, disable, rollback (value = id).
    internal static ThemeState Change(string root, string operation, string value, ThemePackage expected = null)
    {
        ThemePackage incoming = operation == "import" ? LoadPackage(value) : null;
        if (expected != null && !expected.SameAs(incoming))
            throw new ThemeError("词库文件在预览后发生变化，请重新预览再导入");
        using (new WriterLock(root)) {
            var state = ReadState(root);
            var packages = state.Packages;
            if (incoming != null) {
                var key = incoming.Manifest.Id;
                ThemeItem old;
                packages.TryGetValue(key, out old);
                if (old != null && old.Current.SameAs(incoming))
                    return state;
                if (old != null && old.Current.Manifest.Version == incoming.Manifest.Version)
                    throw new ThemeError("同一版本的内容发生变化，请先更新版本号");
                packages[key] = new ThemeItem { Enabled = old != null && old.Enabled, Current = incoming, Previous = old != null ? old.Current : null };
            } else {
                ThemeItem item;
                if (value == null || !packages.TryGetValue(value, out item))
                    throw new ThemeError("找不到主题包：" + value);
                if (operation == "enable" || operation == "disable")
                    item.Enabled = operation == "enable";
                else if (operation == "rollback") {
                    if (item.Previous == null)
                        throw new ThemeError("此主题包没有可回退版本");
                    var current = item.Current; item.Current = item.Previous; item.Previous = current;
                } else
                    throw new ThemeError("未知操作：" + operation);
            }
            WriteState(root, state);
            return state;
        }
    }

    // ---- personal word list (japanese-custom.tsv) ----

    internal static string Serialize(List<ThemeEntry> entries)
    {
        var builder = new StringBuilder();
        foreach (var entry in entries) builder.Append(entry.Line());
        return builder.ToString();
    }

    // Replaces the whole personal list; an empty list is an explicit clear.
    internal static void WritePersonal(string destination, List<ThemeEntry> entries)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destination)));
        var temporary = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(destination)), "tmp" + Guid.NewGuid().ToString("N"));
        try {
            WriteDurable(temporary, new UTF8Encoding(false, true).GetBytes(Serialize(entries)));
            Replace(temporary, destination);
        } finally {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    // ---- files ----

    internal static byte[] ReadLimited(string path, long limit)
    {
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            if (stream.Length > limit) throw new ThemeError("文件超过大小限制：" + path);
            var data = new byte[stream.Length];
            int length = 0, read;
            while (length < data.Length && (read = stream.Read(data, length, data.Length - length)) > 0) length += read;
            if (length != data.Length || stream.ReadByte() >= 0) throw new ThemeError("文件在读取期间发生变化：" + path);
            return data;
        }
    }

    internal static string DecodeUtf8Sig(byte[] data, string path)
    {
        try {
            int start = data.Length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ? 3 : 0;
            return new UTF8Encoding(false, true).GetString(data, start, data.Length - start);
        } catch (DecoderFallbackException) {
            throw new ThemeError("文件不是有效的 UTF-8：" + path);
        }
    }
}

// Word lists: "# name" header, then "word | reading | key: value ..." lines.
internal static class WordList
{
    static readonly HashSet<string> Pos = new HashSet<string> { "n", "v1", "vk", "vs", "adj-i", "adj-na", "adv", "prt", "other",
        "v5k", "v5g", "v5s", "v5t", "v5n", "v5b", "v5m", "v5r", "v5u" };
    static readonly string[] Godan = { "v5k", "v5g", "v5s", "v5t", "v5n", "v5b", "v5m", "v5r", "v5u" };
    const string GodanEndings = "くぐすつぬぶむるう";
    const string IchidanStems = "いきぎしじちぢにひびぴみりえけげせぜてでねへべぺめれ";
    static readonly Regex TriggerPattern = new Regex(@"\A[a-z]{2,256}\z");
    static readonly Regex KanaPattern = new Regex(@"\A[ぁ-ゖー]{1,128}\z");
    static readonly Regex PriorityPattern = new Regex(@"\A[0-9]{1,4}\z");

    internal static string Kana(string text)
    {
        var chars = text.ToCharArray();
        for (int i = 0; i < chars.Length; i++)
            if (chars[i] >= 'ァ' && chars[i] <= 'ヶ') chars[i] = (char)(chars[i] - 0x60);
        return new string(chars);
    }

    internal static string Trigger(string text)
    {
        text = text.ToLowerInvariant().Normalize(NormalizationForm.FormC);
        text = text.Replace("ü", "v").Replace("u:", "v");
        foreach (var toned in new[] { "ǖ", "ǘ", "ǚ", "ǜ" }) text = text.Replace(toned, "v");
        var decomposed = text.Normalize(NormalizationForm.FormD);
        var builder = new StringBuilder();
        for (int i = 0; i < decomposed.Length; i++) {
            if (CharUnicodeInfo.GetUnicodeCategory(decomposed, i) == UnicodeCategory.NonSpacingMark) {
                if (char.IsHighSurrogate(decomposed[i])) i++;
                continue;
            }
            char c = decomposed[i];
            if (c == '-' || Py.IsSpace(c)) continue;
            builder.Append(c);
        }
        text = builder.ToString();
        if (!TriggerPattern.IsMatch(text))
            throw new ThemeError("触发词须为 2–256 个字母，可含空格、连字符或拼音声调符号");
        return text;
    }

    static string Label(string text)
    {
        int length = Py.Length(text);
        if (length < 1 || length > 128 || "=+-@".IndexOf(text[0]) >= 0)
            throw new ThemeError("词或中文名须为 1–128 字，且不能以 = + - @ 开头");
        foreach (char c in text)
            if (c == '|' || c < 32)
                throw new ThemeError("词或中文名含不支持的分隔符或控制字符");
        return text;
    }

    sealed class Key : IEquatable<Key>
    {
        internal string Trigger, Word, Reading, Kind, Chinese;
        public bool Equals(Key o) { return Trigger == o.Trigger && Word == o.Word && Reading == o.Reading && Kind == o.Kind && Chinese == o.Chinese; }
        public override bool Equals(object o) { var k = o as Key; return k != null && Equals(k); }
        public override int GetHashCode() { return (Trigger + "\0" + Word + "\0" + Reading + "\0" + Kind + "\0" + Chinese).GetHashCode(); }
    }

    internal static List<ThemeEntry> Parse(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists) throw new FileNotFoundException("找不到词表：" + path, path);
        if (info.Length > 32 * 1024 * 1024)
            throw new ThemeError("词表不得超过 32 MB");
        var lines = Py.SplitLines(ThemePackages.DecodeUtf8Sig(ThemePackages.ReadLimited(path, 32 * 1024 * 1024), path));
        if (lines.Count == 0 || !lines[0].StartsWith("# ", StringComparison.Ordinal)) throw new ThemeError("首行须为 # 加空格及 1–40 字的词库名称");
        int nameLength = Py.Length(Py.Strip(lines[0].Substring(2)));
        if (nameLength < 1 || nameLength > 40)
            throw new ThemeError("首行须为 # 加空格及 1–40 字的词库名称");
        var entries = new Dictionary<Key, int>();
        var utf8 = new UTF8Encoding(false, false);
        int count = 0;
        for (int index = 1; index < lines.Count; index++) {
            var line = lines[index];
            if (Py.Strip(line).Length == 0 || Py.StripStart(line).StartsWith("#", StringComparison.Ordinal)) continue;
            try {
                if (utf8.GetByteCount(line) > 4096)
                    throw new ThemeError("每行不得超过 4096 字节");
                count++;
                if (count > 100000)
                    throw new ThemeError("词表不得超过 100000 行词条");
                var parts = new List<string>();
                foreach (var part in line.Replace('｜', '|').Split('|')) parts.Add(Py.Strip(part));
                var word = Label(parts[0]); parts.RemoveAt(0);
                string reading;
                if (parts.Count > 0 && parts[0].IndexOf(':') < 0 && parts[0].IndexOf('：') < 0) { reading = parts[0]; parts.RemoveAt(0); }
                else reading = Kana(word);
                bool spellingOnly = reading == "-";
                if (spellingOnly) reading = "";
                if (!spellingOnly && !KanaPattern.IsMatch(reading))
                    throw new ThemeError("须填写平假名读音（最多 128 字），或用 - 明确表示仅输出表记");
                var fields = new Dictionary<string, List<string>>(StringComparer.Ordinal);
                foreach (var part in parts) {
                    int colon = part.IndexOfAny(new[] { ':', '：' });
                    if (colon < 0)
                        throw new ThemeError("属性须使用 key: value 格式");
                    string key = Py.Strip(part.Substring(0, colon)), value = Py.Strip(part.Substring(colon + 1));
                    if (key != "pinyin" && key != "english" && key != "chinese" && key != "pos" && key != "priority") continue;
                    if (key != "pinyin" && key != "english" && fields.ContainsKey(key))
                        throw new ThemeError("属性重复：" + key);
                    List<string> list;
                    if (!fields.TryGetValue(key, out list)) fields.Add(key, list = new List<string>());
                    list.Add(value);
                }
                var pos = fields.ContainsKey("pos") ? fields["pos"][0] : "n";
                if (spellingOnly && pos != "n" && pos != "other")
                    throw new ThemeError("无读音词条只能使用 n 或 other，不支持活用");
                if (!Pos.Contains(pos))
                    throw new ThemeError("未知词性：" + pos);
                int godan = Array.IndexOf(Godan, pos);
                if (godan >= 0 && !reading.EndsWith(GodanEndings[godan].ToString(), StringComparison.Ordinal))
                    throw new ThemeError("读音结尾与五段动词词性不符");
                if (pos == "v1" && (reading.Length < 2 || reading[reading.Length - 1] != 'る' || IchidanStems.IndexOf(reading[reading.Length - 2]) < 0))
                    throw new ThemeError("一段动词读音须以い段或え段假名加る结尾");
                if (pos == "vk" && !reading.EndsWith("くる", StringComparison.Ordinal) || pos == "adj-i" && !reading.EndsWith("い", StringComparison.Ordinal))
                    throw new ThemeError("读音结尾与词性不符");
                var priorityText = fields.ContainsKey("priority") ? fields["priority"][0] : "5000";
                if (!PriorityPattern.IsMatch(priorityText) || int.Parse(priorityText, CultureInfo.InvariantCulture) < 1)
                    throw new ThemeError("priority 须为 1–9999 的整数");
                int priority = int.Parse(priorityText, CultureInfo.InvariantCulture);
                var chinese = fields.ContainsKey("chinese") ? Label(fields["chinese"][0]) : "";
                foreach (var kind in new[] { "pinyin", "english" }) {
                    if (!fields.ContainsKey(kind)) continue;
                    foreach (var raw in fields[kind]) {
                        var key = new Key { Trigger = Trigger(raw), Word = word, Reading = reading, Kind = kind, Chinese = chinese };
                        int previous;
                        entries[key] = entries.TryGetValue(key, out previous) ? Math.Max(previous, priority) : priority;
                    }
                }
            } catch (ThemeError error) {
                throw new ThemeError(Path.GetFileName(path) + ":" + (index + 1) + ": " + error.Message);
            }
        }
        var result = new List<ThemeEntry>(entries.Count);
        foreach (var pair in entries)
            result.Add(new ThemeEntry { Reading = pair.Key.Trigger, Text = pair.Key.Word, Weight = pair.Value, Kana = pair.Key.Reading,
                Source = pair.Key.Kind, Chinese = pair.Key.Chinese });
        result.Sort((a, b) => {
            int c = Py.Compare(a.Reading, b.Reading);
            if (c == 0) c = b.Weight.CompareTo(a.Weight);
            if (c == 0) c = Py.Compare(a.Text, b.Text);
            if (c == 0) c = Py.Compare(a.Kana, b.Kana);
            if (c == 0) c = Py.Compare(a.Source, b.Source);
            if (c == 0) c = Py.Compare(a.Chinese, b.Chinese);
            return c;
        });
        return result;
    }
}

// Python str semantics the formats depend on: code-point length and order,
// str.isspace()/strip() and str.splitlines().
internal static class Py
{
    internal static bool IsSpace(char c)
    {
        int v = c;
        return (v >= 9 && v <= 13) || (v >= 0x1c && v <= 0x20) || v == 0x85 || v == 0xa0 || v == 0x1680
            || (v >= 0x2000 && v <= 0x200a) || v == 0x2028 || v == 0x2029 || v == 0x202f || v == 0x205f || v == 0x3000;
    }

    internal static string Strip(string text)
    {
        int start = 0, end = text.Length;
        while (start < end && IsSpace(text[start])) start++;
        while (end > start && IsSpace(text[end - 1])) end--;
        return text.Substring(start, end - start);
    }

    internal static string StripStart(string text)
    {
        int start = 0;
        while (start < text.Length && IsSpace(text[start])) start++;
        return text.Substring(start);
    }

    internal static int Length(string text)
    {
        int length = 0;
        for (int i = 0; i < text.Length; i++) {
            if (char.IsHighSurrogate(text[i]) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1])) i++;
            length++;
        }
        return length;
    }

    // Code-point order (UTF-16 ordinal order differs only around surrogates).
    internal static int Compare(string a, string b)
    {
        int n = Math.Min(a.Length, b.Length);
        for (int i = 0; i < n; i++) {
            if (a[i] == b[i]) continue;
            return Fix(a[i]).CompareTo(Fix(b[i]));
        }
        return a.Length.CompareTo(b.Length);
    }

    static int Fix(char c)
    {
        if (c >= 0xD800 && c <= 0xDFFF) return c + 0x2000;
        if (c >= 0xE000) return c - 0x800;
        return c;
    }

    internal static List<string> SplitLines(string text)
    {
        var lines = new List<string>();
        int start = 0, i = 0;
        while (i < text.Length) {
            char c = text[i];
            int v = c;
            bool breaks = (v >= 10 && v <= 13) || v == 0x1c || v == 0x1d || v == 0x1e || v == 0x85 || v == 0x2028 || v == 0x2029;
            if (!breaks) { i++; continue; }
            lines.Add(text.Substring(start, i - start));
            i += c == '\r' && i + 1 < text.Length && text[i + 1] == '\n' ? 2 : 1;
            start = i;
        }
        if (start < text.Length) lines.Add(text.Substring(start));
        return lines;
    }
}

// Strict JSON as Python's json module reads it (duplicate keys rejected), and
// json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) output.
// Objects are Dictionary<string, object>, arrays List<object>, integers long.
internal static class Json
{
    internal static bool HasExactly(Dictionary<string, object> value, params string[] keys)
    {
        if (value.Count != keys.Length) return false;
        foreach (var key in keys) if (!value.ContainsKey(key)) return false;
        return true;
    }

    internal static object Parse(byte[] data)
    {
        var text = ThemePackages.DecodeUtf8Sig(data, "JSON");
        int position = 0;
        var value = Value(text, ref position, 0);
        Space(text, ref position);
        if (position != text.Length) throw Error("JSON 末尾有多余内容");
        return value;
    }

    static ThemeError Error(string message) { return new ThemeError(message); }

    static void Space(string s, ref int p)
    {
        while (p < s.Length && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    }

    static object Value(string s, ref int p, int depth)
    {
        if (depth > 64) throw Error("JSON 嵌套过深");
        Space(s, ref p);
        if (p >= s.Length) throw Error("JSON 不完整");
        char c = s[p];
        if (c == '{') {
            p++;
            var result = new Dictionary<string, object>(StringComparer.Ordinal);
            Space(s, ref p);
            if (p < s.Length && s[p] == '}') { p++; return result; }
            while (true) {
                Space(s, ref p);
                if (p >= s.Length || s[p] != '"') throw Error("JSON 对象属性名无效");
                var key = String(s, ref p);
                Space(s, ref p);
                if (p >= s.Length || s[p] != ':') throw Error("JSON 缺少冒号");
                p++;
                var item = Value(s, ref p, depth + 1);
                if (result.ContainsKey(key)) throw Error("JSON 属性重复：" + key);
                result.Add(key, item);
                Space(s, ref p);
                if (p < s.Length && s[p] == ',') { p++; continue; }
                if (p < s.Length && s[p] == '}') { p++; return result; }
                throw Error("JSON 对象格式无效");
            }
        }
        if (c == '[') {
            p++;
            var result = new List<object>();
            Space(s, ref p);
            if (p < s.Length && s[p] == ']') { p++; return result; }
            while (true) {
                result.Add(Value(s, ref p, depth + 1));
                Space(s, ref p);
                if (p < s.Length && s[p] == ',') { p++; continue; }
                if (p < s.Length && s[p] == ']') { p++; return result; }
                throw Error("JSON 数组格式无效");
            }
        }
        if (c == '"') return String(s, ref p);
        if (Literal(s, ref p, "true")) return true;
        if (Literal(s, ref p, "false")) return false;
        if (Literal(s, ref p, "null")) return null;
        return Number(s, ref p);
    }

    static bool Literal(string s, ref int p, string word)
    {
        if (string.CompareOrdinal(s, p, word, 0, word.Length) != 0) return false;
        p += word.Length; return true;
    }

    static object Number(string s, ref int p)
    {
        int start = p;
        if (p < s.Length && s[p] == '-') p++;
        if (p < s.Length && s[p] == '0') p++;
        else if (p < s.Length && s[p] >= '1' && s[p] <= '9') while (p < s.Length && s[p] >= '0' && s[p] <= '9') p++;
        else throw Error("JSON 值无效");
        bool integer = true;
        if (p < s.Length && s[p] == '.') {
            int digits = ++p;
            while (p < s.Length && s[p] >= '0' && s[p] <= '9') p++;
            if (p == digits) throw Error("JSON 数字无效");
            integer = false;
        }
        if (p < s.Length && (s[p] == 'e' || s[p] == 'E')) {
            p++;
            if (p < s.Length && (s[p] == '+' || s[p] == '-')) p++;
            int digits = p;
            while (p < s.Length && s[p] >= '0' && s[p] <= '9') p++;
            if (p == digits) throw Error("JSON 数字无效");
            integer = false;
        }
        var text = s.Substring(start, p - start);
        long whole;
        if (integer && long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out whole)) return whole;
        return double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture);
    }

    static string String(string s, ref int p)
    {
        p++;
        var builder = new StringBuilder();
        while (true) {
            if (p >= s.Length) throw Error("JSON 字符串未结束");
            char c = s[p++];
            if (c == '"') break;
            if (c < 32) throw Error("JSON 字符串含控制字符");
            if (c != '\\') { builder.Append(c); continue; }
            if (p >= s.Length) throw Error("JSON 字符串未结束");
            char e = s[p++];
            switch (e) {
                case '"': builder.Append('"'); break;
                case '\\': builder.Append('\\'); break;
                case '/': builder.Append('/'); break;
                case 'b': builder.Append('\b'); break;
                case 'f': builder.Append('\f'); break;
                case 'n': builder.Append('\n'); break;
                case 'r': builder.Append('\r'); break;
                case 't': builder.Append('\t'); break;
                case 'u':
                    int code;
                    if (p + 4 > s.Length || !int.TryParse(s.Substring(p, 4), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out code))
                        throw Error("JSON 转义无效");
                    p += 4; builder.Append((char)code); break;
                default: throw Error("JSON 转义无效");
            }
        }
        var result = builder.ToString();
        for (int i = 0; i < result.Length; i++) {
            if (char.IsHighSurrogate(result[i]) && i + 1 < result.Length && char.IsLowSurrogate(result[i + 1])) { i++; continue; }
            if (char.IsSurrogate(result[i])) throw Error("JSON 字符串含无效代理项");
        }
        return result;
    }

    internal static string Write(object value)
    {
        var builder = new StringBuilder();
        Write(builder, value, 0);
        return builder.ToString();
    }

    static void Write(StringBuilder b, object value, int level)
    {
        if (value == null) { b.Append("null"); return; }
        if (value is bool) { b.Append((bool)value ? "true" : "false"); return; }
        if (value is long) { b.Append(((long)value).ToString(CultureInfo.InvariantCulture)); return; }
        var text = value as string;
        if (text != null) { Quote(b, text); return; }
        var map = value as Dictionary<string, object>;
        if (map != null) {
            if (map.Count == 0) { b.Append("{}"); return; }
            var keys = new List<string>(map.Keys);
            keys.Sort(Py.Compare);
            b.Append("{\n");
            for (int i = 0; i < keys.Count; i++) {
                b.Append(' ', 2 * (level + 1));
                Quote(b, keys[i]); b.Append(": ");
                Write(b, map[keys[i]], level + 1);
                b.Append(i + 1 < keys.Count ? ",\n" : "\n");
            }
            b.Append(' ', 2 * level).Append('}');
            return;
        }
        var list = value as List<object>;
        if (list != null) {
            if (list.Count == 0) { b.Append("[]"); return; }
            b.Append("[\n");
            for (int i = 0; i < list.Count; i++) {
                b.Append(' ', 2 * (level + 1));
                Write(b, list[i], level + 1);
                b.Append(i + 1 < list.Count ? ",\n" : "\n");
            }
            b.Append(' ', 2 * level).Append(']');
            return;
        }
        throw new ArgumentException("不支持的 JSON 值");
    }

    static void Quote(StringBuilder b, string text)
    {
        b.Append('"');
        foreach (char c in text) {
            switch (c) {
                case '"': b.Append("\\\""); break;
                case '\\': b.Append("\\\\"); break;
                case '\n': b.Append("\\n"); break;
                case '\r': b.Append("\\r"); break;
                case '\t': b.Append("\\t"); break;
                case '\b': b.Append("\\b"); break;
                case '\f': b.Append("\\f"); break;
                default:
                    if (c < 32) b.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else b.Append(c);
                    break;
            }
        }
        b.Append('"');
    }
}
