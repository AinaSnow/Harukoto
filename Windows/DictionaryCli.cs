// DICT-03: command-line dictionary management without Python.
//   MeowpuutGlassPreview.exe --dictionaries [--root DIR] list|preview M|import M|enable ID|disable ID|rollback ID
//   MeowpuutGlassPreview.exe --dictionaries [--output FILE] personal WORDS.txt [--check] | personal --clear [--check]
// Dictionaries.cmd passes its arguments here and waits. Exit code 0 on success, 2 on errors.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class DictionaryCli
{
    [DllImport("kernel32.dll")] static extern bool AttachConsole(int process);
    [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int handle);
    [DllImport("kernel32.dll")] static extern int GetFileType(IntPtr handle);

    // A GUI-subsystem program has no console of its own: write UTF-8 to redirected
    // output, otherwise attach to the parent console and use its code page.
    static void OpenConsole()
    {
        var output = GetStdHandle(-11);
        if (output == IntPtr.Zero || output == new IntPtr(-1)) {
            AttachConsole(-1);
            return;
        }
        if (GetFileType(output) == 2) return; // FILE_TYPE_CHAR: a console
        var utf8 = new UTF8Encoding(false);
        Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), utf8) { AutoFlush = true });
        Console.SetError(new StreamWriter(Console.OpenStandardError(), utf8) { AutoFlush = true });
    }

    const string Usage = "用法：--dictionaries [--root 目录] list | preview 清单.json | import 清单.json | enable ID | disable ID | rollback ID\n"
        + "      --dictionaries [--output 文件] personal 词表.txt [--check] | personal --clear [--check]";

    // args[0] is "--dictionaries".
    internal static int Run(string[] args, bool attach = true)
    {
        if (attach) OpenConsole();
        try {
            return Execute(new List<string>(args).GetRange(1, args.Length - 1));
        } catch (Exception error) {
            if (error is ThemeError || error is IOException || error is UnauthorizedAccessException || error is ArgumentException) {
                Console.Error.WriteLine("错误：" + error.Message);
                return 2;
            }
            Console.Error.WriteLine("错误：" + error);
            return 2;
        } finally {
            Console.Out.Flush(); Console.Error.Flush();
        }
    }

    static string Option(List<string> args, string name)
    {
        int index = args.IndexOf(name);
        if (index < 0) return null;
        if (index + 1 >= args.Count) throw new ArgumentException("缺少 " + name + " 的值\n" + Usage);
        var value = args[index + 1];
        args.RemoveRange(index, 2);
        return value;
    }

    static string Local()
    {
        var local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
        if (string.IsNullOrEmpty(local)) throw new ArgumentException("缺少 LOCALAPPDATA，请用 --root 或 --output 指定位置");
        return Path.Combine(local, "Meowpuut");
    }

    static int Execute(List<string> args)
    {
        var root = Option(args, "--root");
        var output = Option(args, "--output");
        bool check = args.Remove("--check");
        bool clear = args.Remove("--clear");
        if (args.Count == 0) throw new ArgumentException(Usage);
        var command = args[0];
        if (command == "personal") {
            if (args.Count != (clear ? 1 : 2)) throw new ArgumentException("请指定词表文件，或单独使用 --clear\n" + Usage);
            var entries = clear ? new List<ThemeEntry>() : WordList.Parse(args[1]);
            if (entries.Count == 0 && !clear)
                throw new ThemeError("没有可导入的 pinyin 或 english 触发词；本阶段不生成罗马字或动词变形");
            if (!check) {
                var destination = output ?? Path.Combine(Local(), "japanese-custom.tsv");
                ThemePackages.WritePersonal(destination, entries);
                Console.WriteLine("已写入 " + destination + "；输入法后台加载后在新一段输入时生效。");
            }
            Console.WriteLine("校验通过：" + entries.Count + " 组触发词／日语读音。");
            return 0;
        }
        if (clear || output != null || check) throw new ArgumentException(Usage);
        root = root ?? Path.Combine(Local(), "Dictionaries");
        bool needsValue = command != "list";
        if (args.Count != (needsValue ? 2 : 1) || Array.IndexOf(new[] { "list", "preview", "import", "enable", "disable", "rollback" }, command) < 0)
            throw new ArgumentException(Usage);
        var state = ThemePackages.ReadState(root);
        if (command == "preview" || command == "import") {
            var package = ThemePackages.LoadPackage(args[1]);
            var m = package.Manifest;
            Console.WriteLine(m.Name + "\nID：" + m.Id + "    版本：" + m.Version + "\n来源：" + m.Source + "\n许可：" + m.License + "\n触发记录：" + package.Entries.Count);
            var report = ThemePackages.Conflicts(state, package);
            Console.WriteLine("主题包之间的冲突：" + (report.Items.Count == 0 ? "未发现" : report.Items.Count + (report.Limited ? "+ 项（仅显示前 100 项）" : " 项")));
            foreach (var item in report.Items)
                Console.WriteLine("  " + item.Trigger + "：" + item.Incoming + " ↔ " + item.Existing + "（" + item.Package + "，" + (item.Enabled ? "启用" : "关闭") + "）");
            if (command == "preview") {
                Console.WriteLine("仅预览，未写入。冲突仅比较已导入主题包；基础词库和个人词表不在此报告内。");
                return 0;
            }
            state = ThemePackages.Change(root, "import", args[1], package);
        } else if (command != "list") {
            state = ThemePackages.Change(root, command, args[1]);
        }
        if (command != "list")
            Console.WriteLine("已保存。新主题默认关闭，需执行 enable 才参与候选；输入法后台加载后在新一段输入时生效。");
        foreach (var pair in state.Packages) {
            var manifest = pair.Value.Current.Manifest;
            Console.WriteLine(pair.Key + " | " + manifest.Name + " | " + manifest.Version + " | " + (pair.Value.Enabled ? "启用" : "关闭")
                + " | 回退：" + (pair.Value.Previous != null ? pair.Value.Previous.Manifest.Version : "无"));
        }
        if (state.Packages.Count == 0) Console.WriteLine("尚未导入主题包。");
        return 0;
    }
}
