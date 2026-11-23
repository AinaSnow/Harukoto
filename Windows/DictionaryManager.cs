// DICT-03: the dictionary manager window (MeowpuutGlassPreview.exe --dictionaries).
// Same operations as the former Python window (dictionary-manager.pyw, removed), plus the personal word
// list that import-japanese-dictionary.py used to replace. All file work runs in
// the background through ThemePackages; nothing here needs Python.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using Microsoft.Win32;

public sealed class ThemeRow
{
    public string Id { get; set; }
    public string Name { get; set; }
    public string Version { get; set; }
    public string Status { get; set; }
    public string Previous { get; set; }
}

internal sealed class DictionaryManagerWindow : Window
{
    readonly string themeRoot, personalPath;
    internal ThemeState State = ThemePackages.Empty();
    internal ThemePackage Preview;
    string previewPath;
    internal bool Busy;
    internal readonly Dictionary<string, Button> Buttons = new Dictionary<string, Button>();
    internal readonly ListView Table = new ListView { SelectionMode = SelectionMode.Single };
    readonly ObservableCollection<ThemeRow> rows = new ObservableCollection<ThemeRow>();
    internal readonly TextBox Details = new TextBox { IsReadOnly = true, TextWrapping = TextWrapping.Wrap, AcceptsReturn = true,
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(10), FontSize = 13 };
    internal readonly TextBlock StatusText = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 12, 0, 0) };
    internal readonly TextBlock PersonalText = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 8) };
    // Tests answer the personal-list confirmation instead of a dialog.
    internal Func<string, bool> Confirm;

    // local: the per-user data folder, normally %LOCALAPPDATA%\Meowpuut.
    internal DictionaryManagerWindow(string local)
    {
        themeRoot = Path.Combine(local, "Dictionaries");
        personalPath = Path.Combine(local, "japanese-custom.tsv");
        Title = "晴词 · 主题词库";
        Width = 860; Height = 760; MinWidth = 720; MinHeight = 600; FontSize = 14;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Confirm = message => MessageBox.Show(this, message, "晴词 · 个人词表", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK;

        var grid = new Grid { Margin = new Thickness(20) };
        Content = grid;
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, new GridLength(2, GridUnitType.Star), GridLength.Auto,
                new GridLength(3, GridUnitType.Star), GridLength.Auto, GridLength.Auto })
            grid.RowDefinitions.Add(new RowDefinition { Height = height });
        Add(grid, new TextBlock { Text = "主题词库", FontSize = 24 }, 0);
        Add(grid, new TextBlock { Text = "先预览再导入，新主题默认关闭。启用、关闭、更新或回退后，输入法在后台加载，下一段输入时生效。",
            TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 16) }, 1);

        var view = new GridView();
        foreach (var column in new[] { new[] { "主题", "Name", "300" }, new[] { "版本", "Version", "130" },
                new[] { "状态", "Status", "100" }, new[] { "可回退版本", "Previous", "140" } })
            view.Columns.Add(new GridViewColumn { Header = column[0], DisplayMemberBinding = new System.Windows.Data.Binding(column[1]),
                Width = double.Parse(column[2]) });
        Table.View = view; Table.ItemsSource = rows;
        Table.SelectionChanged += (s, e) => Selected();
        Add(grid, Table, 2);

        var bar = new UniformGrid { Columns = 3, Margin = new Thickness(0, 12, 0, 12) };
        AddButton(bar, "preview", "选择词库并预览", Choose);
        AddButton(bar, "import", "导入此预览", ImportPreview);
        AddButton(bar, "enable", "启用", () => Modify("enable"));
        AddButton(bar, "disable", "关闭", () => Modify("disable"));
        AddButton(bar, "rollback", "回退上一版", () => Modify("rollback"));
        AddButton(bar, "refresh", "刷新", Refresh);
        AddButton(bar, "ff14", "预览 FF14 词库", PreviewFf14);
        Add(grid, bar, 3);
        Add(grid, Details, 4);

        var personal = new StackPanel();
        personal.Children.Add(new TextBlock { Text = "个人日语触发词表", FontSize = 18, Margin = new Thickness(0, 16, 0, 0) });
        personal.Children.Add(PersonalText);
        var personalBar = new UniformGrid { Columns = 3 };
        AddButton(personalBar, "personal", "导入个人词表…", ChoosePersonal);
        AddButton(personalBar, "clear", "清空个人词表", ClearPersonal);
        personal.Children.Add(personalBar);
        Add(grid, personal, 5);
        Add(grid, StatusText, 6);

        ShowDetails("选择 manifest.json 可查看名称、来源、许可和冲突。\n随附 FF14 职业与物品术语包可点击按钮预览；导入后默认关闭。");
        StatusText.Text = "正在读取本地词库…";
        Closing += (s, e) => {
            if (!Busy) return;
            e.Cancel = true;
            StatusText.Text = "正在完成词库操作，请稍候再关闭。";
        };
        ShowPersonal();
        Refresh();
    }

    static void Add(Grid grid, UIElement element, int row)
    {
        Grid.SetRow(element, row);
        grid.Children.Add(element);
    }

    void AddButton(Panel panel, string key, string text, Action click)
    {
        var button = new Button { Content = text, Padding = new Thickness(12, 7, 12, 7), Margin = new Thickness(0, 3, 8, 3) };
        button.Click += (s, e) => click();
        Buttons.Add(key, button);
        panel.Children.Add(button);
    }

    void ShowDetails(string text) { Details.Text = text; Details.ScrollToHome(); }

    string SelectedId()
    {
        var row = Table.SelectedItem as ThemeRow;
        return row == null ? null : row.Id;
    }

    ThemeItem SelectedItem()
    {
        var id = SelectedId();
        ThemeItem item;
        return id != null && State.Packages.TryGetValue(id, out item) ? item : null;
    }

    void UpdateButtons()
    {
        var item = SelectedItem();
        foreach (var pair in Buttons) {
            bool enabled = !Busy;
            if (pair.Key == "import") enabled = enabled && Preview != null;
            if (pair.Key == "enable") enabled = enabled && item != null && !item.Enabled;
            if (pair.Key == "disable") enabled = enabled && item != null && item.Enabled;
            if (pair.Key == "rollback") enabled = enabled && item != null && item.Previous != null;
            pair.Value.IsEnabled = enabled;
        }
    }

    int generation;

    async void Run<T>(Func<T> work, Action<T> done)
    {
        if (Busy) return;
        Busy = true; int mine = ++generation; UpdateButtons(); StatusText.Text = "正在处理…";
        try {
            T result = await Task.Run(work);
            Busy = false;
            done(result);
        } catch (Exception error) {
            StatusText.Text = "操作未完成：" + error.Message;
        } finally {
            // done() may start the next step; leave that one busy.
            if (generation == mine) Busy = false;
            UpdateButtons();
        }
    }

    void ShowState(ThemeState state)
    {
        State = state;
        var selected = SelectedId();
        rows.Clear();
        foreach (var pair in state.Packages) {
            var manifest = pair.Value.Current.Manifest;
            rows.Add(new ThemeRow { Id = pair.Key, Name = manifest.Name, Version = manifest.Version, Status = pair.Value.Enabled ? "启用" : "关闭",
                Previous = pair.Value.Previous != null ? pair.Value.Previous.Manifest.Version : "无" });
        }
        Select(selected);
        StatusText.Text = "共 " + state.Packages.Count + " 个主题。主题变更后台加载后，在新一段输入时生效。";
    }

    void Select(string id)
    {
        foreach (var row in rows)
            if (row.Id == id) { Table.SelectedItem = row; return; }
    }

    internal void Refresh()
    {
        Run(() => ThemePackages.ReadState(themeRoot), ShowState);
    }

    void Selected()
    {
        var item = SelectedItem();
        if (item != null && Preview == null) ShowDetails(Describe(item.Current));
        UpdateButtons();
    }

    static string Describe(ThemePackage package)
    {
        var m = package.Manifest;
        return m.Name + "\nID：" + m.Id + "    版本：" + m.Version + "\n来源：" + m.Source + "\n许可：" + m.License + "\n触发记录：" + package.Entries.Count;
    }

    void Choose()
    {
        var dialog = new OpenFileDialog { Title = "选择主题词库清单", Filter = "词库清单 (*.json)|*.json" };
        if (dialog.ShowDialog(this) == true) PreviewFile(dialog.FileName);
    }

    // The bundled FF14 package sits beside the program once installed; in a
    // source checkout it is found under Resources/.
    internal static string BundledFf14()
    {
        var folder = AppDomain.CurrentDomain.BaseDirectory;
        var beside = Path.Combine(folder, "ff14-manifest.json");
        if (File.Exists(beside)) return beside;
        var directory = new DirectoryInfo(folder);
        for (int i = 0; i < 5 && directory != null; i++, directory = directory.Parent) {
            var candidate = Path.Combine(directory.FullName, "Resources", "DictionaryPackages", "ff14", "full", "manifest.json");
            if (File.Exists(candidate)) return candidate;
        }
        return beside;
    }

    void PreviewFf14() { PreviewFile(BundledFf14()); }

    sealed class PreviewResult { internal ThemePackage Package; internal ThemeConflicts Report; }

    internal void PreviewFile(string path)
    {
        Preview = null; previewPath = null;
        Run(() => {
            var package = ThemePackages.LoadPackage(path);
            return new PreviewResult { Package = package, Report = ThemePackages.Conflicts(ThemePackages.ReadState(themeRoot), package) };
        }, result => {
            Preview = result.Package; previewPath = path;
            var text = new StringBuilder(Describe(Preview) + "\n\n主题包之间的冲突（不含基础词库和个人词表）：\n");
            foreach (var item in result.Report.Items)
                text.Append(item.Trigger + "：" + item.Incoming + " ↔ " + item.Existing + "（" + item.Package + "，" + (item.Enabled ? "启用" : "关闭") + "）\n");
            if (result.Report.Items.Count == 0) text.Append("未发现冲突。\n");
            if (result.Report.Limited) text.Append("仅显示前 100 项。\n");
            ShowDetails(text.ToString());
            StatusText.Text = "预览完成，尚未写入。点击“导入此预览”保存。";
        });
    }

    internal void ImportPreview()
    {
        if (Preview == null) return;
        var package = Preview; var path = previewPath;
        Run(() => ThemePackages.Change(themeRoot, "import", path, package), state => {
            Preview = null; previewPath = null; ShowState(state);
            Select(package.Manifest.Id); Selected();
            StatusText.Text = "已导入。新主题默认关闭；选择主题后点击“启用”。";
        });
    }

    internal void Modify(string operation)
    {
        var id = SelectedId();
        if (id == null) return;
        Preview = null; previewPath = null;
        Run(() => ThemePackages.Change(themeRoot, operation, id), ShowState);
    }

    // ---- personal word list ----

    void ShowPersonal()
    {
        string state;
        try {
            if (!File.Exists(personalPath)) state = "尚未导入。";
            else {
                int count = 0;
                foreach (var line in File.ReadLines(personalPath, Encoding.UTF8)) if (line.Length > 0) count++;
                state = count == 0 ? "已清空（0 组）。" : "当前 " + count + " 组触发词／日语读音。";
            }
        } catch (Exception error) { state = "无法读取：" + error.Message; }
        PersonalText.Text = "导入会整表替换此前的个人词条，不影响主题包；请自己保留词表源文件。" + state;
    }

    void ChoosePersonal()
    {
        var dialog = new OpenFileDialog { Title = "选择个人词表", Filter = "词表 (*.txt)|*.txt|所有文件 (*.*)|*.*" };
        if (dialog.ShowDialog(this) == true) ImportPersonal(dialog.FileName);
    }

    internal void ImportPersonal(string path)
    {
        Run(() => WordList.Parse(path), entries => {
            if (entries.Count == 0) {
                StatusText.Text = "操作未完成：没有可导入的 pinyin 或 english 触发词；本阶段不生成罗马字或动词变形";
                return;
            }
            ShowDetails("个人词表：" + Path.GetFileName(path) + "\n校验通过：" + entries.Count + " 组触发词／日语读音。");
            if (!Confirm("校验通过：" + entries.Count + " 组触发词／日语读音。\n导入会整表替换当前个人词表，确定继续？")) {
                StatusText.Text = "已取消，个人词表未改变。";
                return;
            }
            Run(() => {
                var again = WordList.Parse(path);
                if (again.Count != entries.Count || ThemePackages.Serialize(again) != ThemePackages.Serialize(entries))
                    throw new ThemeError("词表在校验后发生变化，请重新选择");
                ThemePackages.WritePersonal(personalPath, again);
                return again.Count;
            }, count => {
                ShowPersonal();
                StatusText.Text = "已导入 " + count + " 组个人触发词；输入法后台加载后在新一段输入时生效。";
            });
        });
    }

    internal void ClearPersonal()
    {
        if (!Confirm("清空个人日语触发词表？主题包不受影响。")) return;
        // An empty file is the explicit clear; deleting the file is not (docs/USER_GUIDE.md).
        Run(() => { ThemePackages.WritePersonal(personalPath, new List<ThemeEntry>()); return 0; }, count => {
            ShowPersonal();
            StatusText.Text = "已清空个人词表；输入法后台加载后在新一段输入时生效。";
        });
    }
}
