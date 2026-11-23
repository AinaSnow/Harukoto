// DICT-03: the built-in dictionary manager (ThemePackages.cs, DictionaryManager.cs).
// Ports the cases of test_japanese_packages.py, test_japanese_dictionary.py and
// test_japanese_manager_gui.py. Everything runs in a temporary folder.
//   DictionaryManagerTests.exe <repo root>            run all tests
//   DictionaryManagerTests.exe --serialize <in> <out>  write a parsed word list (parity checks)
//   DictionaryManagerTests.exe --change <root> <op> <value>
//   DictionaryManagerTests.exe --dictionaries ...       the command line of MeowpuutGlassPreview.exe
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

internal static class DictionaryManagerTests
{
    static string folder, store;
    static int failures;
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);

    static void Check(bool condition, string message)
    {
        if (condition) return;
        failures++;
        Console.Error.WriteLine("FAIL: " + message);
    }

    static void Throws(Action action, string contains, string message)
    {
        try { action(); }
        catch (Exception error) {
            Check(contains == null || error.Message.Contains(contains), message + "（错误信息：" + error.Message + "）");
            return;
        }
        Check(false, message + "（未抛出异常）");
    }

    static string Package(string key = "example", string version = "1.0", string text = "日本 | にほん | pinyin: riben", string name = "测试主题")
    {
        var directory = Path.Combine(folder, key + "-" + version);
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "words.txt"), "# 测试\n" + text + "\n", Utf8);
        var path = Path.Combine(directory, "manifest.json");
        File.WriteAllText(path, "{\"format\": 1, \"id\": \"" + key + "\", \"name\": \"" + name + "\", \"version\": \"" + version
            + "\", \"source\": \"人工编写的测试数据\", \"license\": \"MIT\", \"words\": \"words.txt\"}", Utf8);
        return path;
    }

    static string Pointer() { return File.ReadAllText(Path.Combine(store, "CURRENT"), Encoding.ASCII); }
    static string CompiledText(ThemeState state) { return Utf8.GetString(ThemePackages.Compiled(state)); }

    static List<ThemeEntry> Parse(string content)
    {
        var path = Path.Combine(folder, "words-" + Guid.NewGuid().ToString("N") + ".txt");
        File.WriteAllText(path, content, new UTF8Encoding(true));
        return WordList.Parse(path);
    }

    static void Fresh()
    {
        if (folder != null && Directory.Exists(folder)) Directory.Delete(folder, true);
        folder = Path.Combine(Path.GetTempPath(), "harukoto-dictionary-manager-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        store = Path.Combine(folder, "store");
    }

    static void PackagesTests()
    {
        Fresh();
        // Preview does not write.
        var preview = ThemePackages.LoadPackage(Package());
        ThemePackages.Conflicts(ThemePackages.ReadState(store), preview);
        Check(!Directory.Exists(store), "preview must not create the store");

        // Import, enable, update, rollback, disable.
        var first = Package();
        var state = ThemePackages.Change(store, "import", first);
        Check(ThemePackages.Compiled(state).Length == 0 && !state.Packages["example"].Enabled, "imported packages start disabled");
        state = ThemePackages.Change(store, "enable", "example");
        Check(CompiledText(state).Contains("日本"), "enabled package is compiled");
        var oldGeneration = Pointer();
        var oldPayload = File.ReadAllBytes(Path.Combine(store, oldGeneration, "enabled.tsv"));
        state = ThemePackages.Change(store, "import", Package("example", "2.0", "東京 | とうきょう | pinyin: dongjing"));
        Check(state.Packages["example"].Enabled && CompiledText(state).Contains("東京") && !CompiledText(state).Contains("日本"), "update keeps enabled and replaces words");
        Check(File.ReadAllBytes(Path.Combine(store, oldGeneration, "enabled.tsv")).SequenceEqual(oldPayload), "old snapshot is immutable");
        state = ThemePackages.Change(store, "rollback", "example");
        Check(ThemePackages.Compiled(state).SequenceEqual(oldPayload) && state.Packages["example"].Current.Manifest.Version == "1.0", "rollback restores the previous version");
        Check(ThemePackages.Compiled(ThemePackages.Change(store, "disable", "example")).Length == 0, "disable empties the compiled list");
        Check(ThemePackages.StateJson(ThemePackages.ReadState(store)) == ThemePackages.StateJson(ThemePackages.Change(store, "import", first)), "re-importing the current version changes nothing");

        // Failed parse and same version with new content keep the previous pointer.
        Fresh();
        var path = Package();
        ThemePackages.Change(store, "import", path);
        var original = Pointer();
        File.WriteAllText(Path.Combine(Path.GetDirectoryName(path), "words.txt"), "# 测试\n日本 | pinyin: riben", Utf8);
        Throws(() => ThemePackages.Change(store, "import", path), null, "a broken word list is rejected");
        Package("example", "1.0", "東京 | とうきょう | pinyin: dongjing");
        Throws(() => ThemePackages.Change(store, "import", path), "版本", "same version, new content is rejected");
        Check(Pointer() == original, "failed imports keep the pointer");

        // A failed activation preserves the previous state.
        var before = ThemePackages.StateJson(ThemePackages.ReadState(store));
        ThemePackages.BeforeActivate = () => { throw new IOException("locked"); };
        try { Throws(() => ThemePackages.Change(store, "enable", "example"), "locked", "activation failure surfaces"); }
        finally { ThemePackages.BeforeActivate = null; }
        Check(Pointer() == original && ThemePackages.StateJson(ThemePackages.ReadState(store)) == before, "failed activation keeps the old snapshot");
        Check(Directory.GetFiles(store, "tmp*").Length == 0, "temporary pointer is removed");

        // Conflicts coexist; packages are independent; the personal list is untouched.
        Fresh();
        ThemePackages.Change(store, "import", Package());
        var other = Package("alternative", "1.0", "二本 | にほん | pinyin: riben");
        var report = ThemePackages.Conflicts(ThemePackages.ReadState(store), ThemePackages.LoadPackage(other));
        Check(report.Items.Count == 1 && report.Items[0].Kind == "alternative" && report.Items[0].Package == "example", "alternative conflict reported");
        ThemePackages.Change(store, "import", other);
        ThemePackages.Change(store, "enable", "example");
        state = ThemePackages.Change(store, "enable", "alternative");
        Check(CompiledText(state).Split('\n').Length == 3, "both meanings are compiled");
        state = ThemePackages.Change(store, "disable", "alternative");
        Check(CompiledText(state).Contains("日本") && !CompiledText(state).Contains("二本"), "disabling one package keeps the other");
        var legacy = Path.Combine(folder, "japanese-custom.tsv");
        File.WriteAllText(legacy, "legacy");
        ThemePackages.Change(store, "disable", "example");
        Check(File.ReadAllText(legacy) == "legacy", "theme changes leave the personal list alone");

        // Invalid manifests and pointer.
        Fresh();
        path = Package();
        var manifest = File.ReadAllText(path, Utf8);
        foreach (var bad in new[] { manifest.Replace("\"id\": \"example\"", "\"id\": \"../escape\""), manifest.Replace("\"words.txt\"", "\"../words.txt\""),
                manifest.Replace("\"words.txt\"", "\"C:/words.txt\""), manifest.Replace("\"format\": 1", "\"format\": true"),
                manifest.Replace("\"format\": 1", "\"format\": 1.0"), manifest.Replace("人工编写的测试数据", " "),
                manifest.Replace("\"license\": \"MIT\", ", ""), manifest.Replace("}", ", \"extra\": 1}") }) {
            File.WriteAllText(path, bad, Utf8);
            Throws(() => ThemePackages.LoadPackage(path), null, "invalid manifest rejected: " + bad);
        }
        Directory.CreateDirectory(store);
        File.WriteAllText(Path.Combine(store, "CURRENT"), "../escape", Encoding.ASCII);
        Throws(() => ThemePackages.ReadState(store), "指针", "invalid pointer rejected");

        // Writer contention and release.
        Fresh();
        using (new ThemePackages.WriterLock(store))
            Throws(() => ThemePackages.Change(store, "import", Package()), "其他词库", "a held lock blocks writers");
        ThemePackages.Change(store, "import", Package());
        Check(ThemePackages.ReadState(store).Packages.ContainsKey("example"), "lock is released");

        // Missing rollback does not mutate.
        original = Pointer();
        Throws(() => ThemePackages.Change(store, "rollback", "example"), "回退", "no previous version");
        Throws(() => ThemePackages.Change(store, "enable", "missing"), "找不到", "unknown package");
        Check(Pointer() == original, "rejected operations keep the pointer");

        // A corrupt state is never overwritten.
        var stateFile = Path.Combine(store, original, "state.json");
        File.WriteAllText(stateFile, File.ReadAllText(stateFile, Utf8).Replace("\"riben\"", "\"../escape\""), Utf8);
        Throws(() => ThemePackages.Change(store, "enable", "example"), "损坏", "corrupt state rejected");
        Check(Pointer() == original, "corrupt state keeps the pointer");

        // Duplicate JSON keys and the conflict report limit.
        Fresh();
        path = Package();
        File.WriteAllText(path, "{\"format\":1,\"format\":1}", Utf8);
        Throws(() => ThemePackages.LoadPackage(path), "重复", "duplicate keys rejected");
        ThemePackages.Change(store, "import", Package());
        var second = ThemePackages.LoadPackage(Package("second"));
        var many = new List<ThemeEntry>();
        for (int i = 0; i < 101; i++) many.AddRange(second.Entries);
        second.Entries = many;
        report = ThemePackages.Conflicts(ThemePackages.ReadState(store), second);
        Check(report.Limited && report.Items.Count == 100 && report.Items[0].Kind == "duplicate", "conflict report is limited to 100");
    }

    static void WordListTests(string repo)
    {
        Fresh();
        var words = Parse("# 游戏\nリムサ・ロミンサ | - | pinyin: limusaluominsa\n");
        Check(words[0].Kana == "", "spelling-only entry has no reading");
        var longName = "オーケストリオン譜:" + string.Concat(Enumerable.Repeat("Song, Name ", 5));
        words = Parse("# 游戏\n" + longName + " | - | english: " + string.Concat(Enumerable.Repeat("longname", 10)) + "\n");
        Check(words[0].Text == longName.Trim(), "long game names are kept");
        Throws(() => Parse("# 游戏\n来る | - | pos: vk | pinyin: lai\n"), "无读音", "spelling-only verbs rejected");
        Throws(() => Parse("# 游戏\n日本 | | pinyin: riben\n"), null, "empty reading rejected");

        words = Parse("# 测试\n魔土器:自己強化 | まどきじこきょうか | chinese: 魔陶器：强化自身 | pinyin：motaqiqianghuazishen\n");
        Check(words[0].Text == "魔土器:自己強化" && words[0].Chinese == "魔陶器：强化自身", "full-width colon kept in names");
        words = Parse("# 测试\n魔土器:自己強化｜まどきじこきょうか｜chinese：魔陶器:强化自身｜pinyin：motaqiqianghuazishen\n");
        Check(words[0].Chinese == "魔陶器:强化自身", "full-width separators");

        words = Parse("# 测试\n新宿｜しんじゅく｜pinyin：xinsu｜priority: 9000\n新宿 | にいじゅく | pinyin: xinsu\nポイントカード | english: point card | english: point-card\n");
        Check(words.Count == 3, "field variants merge");
        Check(words.Any(w => w.Reading == "pointcard" && w.Text == "ポイントカード" && w.Weight == 5000 && w.Kana == "ぽいんとかーど" && w.Source == "english" && w.Chinese == ""), "katakana reading and english trigger");
        Check(string.Join(",", words.Where(w => w.Reading == "xinsu").Select(w => w.Kana)) == "しんじゅく,にいじゅく", "priority order within a trigger");
        Check(WordList.Trigger("nǚ xìng") == "nvxing", "toned u-umlaut");
        Check(WordList.Trigger("nu" + (char)0x308 + (char)0x30C + " xing") == "nvxing", "decomposed u-umlaut");

        foreach (var line in new[] { "日本 | pinyin: riben", "日本 | nihon | pinyin: riben", "日本 | にほん | priority: 0", "日本 | にほん | priority: 5000x",
                "日本 | にほん | pos: unknown", "食べる | たべる | pos: v5k", "日本 | にほん | pinyin: a", "日本 | にほん | priority: 1 | priority: 2",
                "日\t本 | にほん | pinyin: riben", "=日本 | にほん | pinyin: riben", "日本 | にほん | pinyin riben" })
            Throws(() => Parse("# 测试\n" + line), "words-", "invalid record rejected with its line: " + line);
        Throws(() => Parse("no header\n日本 | にほん | pinyin: riben\n"), "首行", "header required");

        var bridge = WordList.Parse(Path.Combine(repo, "Resources/Lexicon/japanese-bridge.txt"));
        Check(bridge.Any(w => w.Reading == "tushuguan" && w.Text == "図書館") && bridge.Any(w => w.Reading == "coffee" && w.Text == "コーヒー"), "bundled bridge list parses");

        // Personal list: whole-list replace and explicit empty clear.
        var personal = Path.Combine(folder, "japanese-custom.tsv");
        ThemePackages.WritePersonal(personal, Parse("# 测试\nコーヒー | english: coffee\n"));
        Check(File.ReadAllText(personal, Utf8) == "coffee|コーヒー|5000|こーひー|english|\n", "personal list written in runtime format");
        ThemePackages.WritePersonal(personal, new List<ThemeEntry>());
        Check(File.Exists(personal) && new FileInfo(personal).Length == 0, "clear writes an empty list");
    }

    static void Pump(DictionaryManagerWindow window)
    {
        var deadline = DateTime.UtcNow.AddSeconds(60);
        do {
            var frame = new DispatcherFrame();
            Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() => frame.Continue = false));
            Dispatcher.PushFrame(frame);
            if (window.Busy) Thread.Sleep(10);
        } while (window.Busy && DateTime.UtcNow < deadline);
        Check(!window.Busy, "window operation finished");
    }

    static void Select(DictionaryManagerWindow window, string id)
    {
        foreach (ThemeRow row in window.Table.Items) if (row.Id == id) window.Table.SelectedItem = row;
    }

    static void WindowTests()
    {
        Fresh();
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext());
        var local = Path.Combine(folder, "local");
        var window = new DictionaryManagerWindow(local);
        bool answer = true;
        window.Confirm = message => answer;
        Pump(window);
        Check(!window.Buttons["import"].IsEnabled && window.Buttons["preview"].IsEnabled, "nothing to import yet");
        Check(window.PersonalText.Text.Contains("尚未导入"), "no personal list yet");

        window.PreviewFile(Package("gui-test", "1.0", "日本 | にほん | pinyin: riben", "窗口测试"));
        Pump(window);
        Check(!Directory.Exists(Path.Combine(local, "Dictionaries")), "preview does not write");
        Check(window.Details.Text.Contains("来源：人工编写的测试数据") && window.Buttons["import"].IsEnabled, "preview shows the manifest");

        // Every button stays inside the window at the minimum size.
        window.Width = window.MinWidth; window.Height = window.MinHeight;
        window.Show(); window.UpdateLayout();
        foreach (var pair in window.Buttons) {
            var right = pair.Value.TranslatePoint(new Point(pair.Value.ActualWidth, pair.Value.ActualHeight), (UIElement)window.Content);
            var content = (FrameworkElement)window.Content;
            Check(right.X <= content.ActualWidth + 1 && right.Y <= content.ActualHeight + 1, "button fits at minimum size: " + pair.Key);
        }
        window.Hide();

        window.ImportPreview(); Pump(window);
        Check(window.State.Packages.ContainsKey("gui-test") && !window.State.Packages["gui-test"].Enabled, "imported disabled");
        Check(window.Buttons["enable"].IsEnabled && !window.Buttons["disable"].IsEnabled && !window.Buttons["rollback"].IsEnabled, "imported package is selected");
        window.Modify("enable"); Pump(window);
        Check(window.State.Packages["gui-test"].Enabled, "enabled from the window");
        window.PreviewFile(Package("gui-test", "2.0", "東京 | とうきょう | pinyin: dongjing", "窗口测试")); Pump(window);
        window.ImportPreview(); Pump(window);
        Check(window.State.Packages["gui-test"].Previous != null && window.Buttons["rollback"].IsEnabled, "update keeps a previous version");
        window.Modify("rollback"); Pump(window);
        Check(window.State.Packages["gui-test"].Current.Manifest.Version == "1.0", "rollback from the window");
        Select(window, "gui-test");
        window.Modify("disable"); Pump(window);
        Check(!window.State.Packages["gui-test"].Enabled, "disabled from the window");
        window.Refresh(); Pump(window);
        Check(window.StatusText.Text.StartsWith("共 1 个主题"), "refresh lists the store");

        // Errors are shown and leave the window usable.
        window.PreviewFile(Path.Combine(folder, "missing.json")); Pump(window);
        Check(window.StatusText.Text.StartsWith("操作未完成") && window.Buttons["preview"].IsEnabled && window.Preview == null, "failed preview reported");

        // Personal list: cancel keeps the file, confirm replaces, clear empties.
        var source = Path.Combine(folder, "personal.txt");
        File.WriteAllText(source, "# 个人\nコーヒー | english: coffee\n", Utf8);
        var personal = Path.Combine(local, "japanese-custom.tsv");
        answer = false;
        window.ImportPersonal(source); Pump(window);
        Check(!File.Exists(personal), "cancelled import writes nothing");
        answer = true;
        window.ImportPersonal(source); Pump(window);
        Check(File.Exists(personal) && File.ReadAllText(personal, Utf8).StartsWith("coffee|") && window.PersonalText.Text.Contains("当前 1 组"), "personal import");
        File.WriteAllText(source, "# 个人\n日本 | pinyin: riben\n", Utf8);
        window.ImportPersonal(source); Pump(window);
        Check(window.StatusText.Text.StartsWith("操作未完成") && File.ReadAllText(personal, Utf8).StartsWith("coffee|"), "a broken list keeps the previous file");
        window.ClearPersonal(); Pump(window);
        Check(new FileInfo(personal).Length == 0 && window.PersonalText.Text.Contains("已清空"), "personal clear");
        window.Close();
    }

    [STAThread]
    static int Main(string[] args)
    {
        try {
            if (args.Length == 3 && args[0] == "--serialize") {
                File.WriteAllText(args[2], ThemePackages.Serialize(WordList.Parse(args[1])), Utf8);
                return 0;
            }
            if (args.Length > 1 && args[0] == "--dictionaries")
                return DictionaryCli.Run(args, false);
            if (args.Length == 4 && args[0] == "--change") {
                ThemePackages.Change(args[1], args[2], args[3]);
                return 0;
            }
            if (args.Length != 1) { Console.Error.WriteLine("usage: DictionaryManagerTests.exe <repo root>"); return 2; }
            PackagesTests();
            WordListTests(args[0]);
            WindowTests();
        } catch (Exception error) {
            failures++;
            Console.Error.WriteLine("FAIL: " + error);
        } finally {
            if (folder != null && Directory.Exists(folder)) Directory.Delete(folder, true);
        }
        if (args.Length == 1) Console.WriteLine(failures == 0 ? "dictionary manager tests passed" : failures + " failure(s)");
        return failures == 0 ? 0 : 1;
    }
}
