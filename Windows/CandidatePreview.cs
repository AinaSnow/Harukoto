using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;

// Only synthetic local samples. The detached content never creates an IME HWND.
internal sealed class CandidatePreview
{
    internal readonly FrameworkElement Root;
    internal readonly WrapPanel Panel;
    private readonly TextBlock preedit, page;
    internal static readonly string[] Names = { "短中文", "FF14 长名称", "日文与假名", "同形异读", "候选第二页", "F6 活用" };
    internal int Selected;

    internal CandidatePreview()
    {
        Window host;
        using (var stream = typeof(CandidatePreview).Assembly.GetManifestResourceStream("CandidateWindow.xaml"))
            host = (Window)XamlReader.Load(stream);
        Root = (FrameworkElement)host.Content;
        Panel = (WrapPanel)host.FindName("Candidates");
        preedit = (TextBlock)host.FindName("Preedit");
        page = (TextBlock)host.FindName("PageIndicator");
        // Preserve font inheritance when detaching the production XAML content.
        TextBlock.SetFontFamily(Root, host.FontFamily);
        host.Content = null;
        Root.HorizontalAlignment = HorizontalAlignment.Left;
        Root.VerticalAlignment = VerticalAlignment.Top;
    }

    internal static string[] Fields(int sample, bool traditional)
    {
        switch (sample) {
        case 1: return new[] { "0", "0", "ff14", "乌尔达哈·来生回廊", "利姆萨·罗敏萨上层甲板", "格里达尼亚新街",
            "这是用于检查换行的很长物品名称（布局示例）", "ウルダハ\tうるだは · 拼\tja-direct", "リムサ・ロミンサ\t拼\tja-direct", "恢复药" };
        case 2: return new[] { "0", "0", "riben", "日本", "日本\tにほん · 拼\tja-direct", "にほん\t日本 · 假名\tja-direct",
            "日文", "日语", "日本語\tにほんご · 拼\tja", "にほんご\t日本語 · 假名\tja-direct" };
        case 3: return new[] { "0", "0", "sheng", "生", "生\tせい · 音读\tja", "生\tしょう · 音读\tja",
            "生\tなま · 训读\tja", "生活\tせいかつ · 拼\tja", "学生\tがくせい · 拼\tja", "生日" };
        case 4: return new[] { "0", "0", "yedui", "也堆", "野堆", "夜队", "页对", "业队", "叶对", "夜对" };
        case 5: return new[] { "0", "0", "chi", "食べる\tたべる · 辞书形\tja", "食べます\tたべます · ます形\tja",
            "食べて\tたべて · て形\tja", "食べた\tたべた · た形\tja", "食べない\tたべない · ない形\tja",
            "食べられる\tたべられる · 可能形\tja", "食べられる\tたべられる · 被动形\tja" };
        default: return traditional
            ? new[] { "0", "0", "yedui", "也對", "野隊", "也隊", "夜對", "葉對", "業對", "也堆" }
            : new[] { "0", "0", "yedui", "也对", "野队", "也队", "夜对", "叶对", "业对", "也堆" };
        }
    }

    internal void Update(int sample, double font, double width, bool expanded, bool traditional)
    {
        width = CandidateLayout.ClampWidth(width);
        Root.Width = width; Panel.Width = width - 26;
        var fields = Fields(sample, traditional);
        if (traditional) {
            // Convert only known Chinese fixture labels, never Japanese spellings/readings.
            for (int i = 3; i < fields.Length; ++i) {
                if (fields[i].EndsWith("\tja", StringComparison.Ordinal) || fields[i].EndsWith("\tja-direct", StringComparison.Ordinal)) continue;
                fields[i] = fields[i].Replace("乌尔达哈", "烏爾達哈").Replace("来生", "來生").Replace("罗敏萨", "羅敏薩")
                    .Replace("利姆萨", "利姆薩").Replace("上层", "上層").Replace("格里达尼亚", "格里達尼亞")
                    .Replace("这是用于检查换行的很长物品名称（布局示例）", "這是用於檢查換行的很長物品名稱（布局示例）")
                    .Replace("恢复药", "恢復藥").Replace("日语", "日語").Replace("队", "隊").Replace("页", "頁").Replace("叶", "葉").Replace("对", "對").Replace("业", "業");
            }
        }
        preedit.Text = fields[2];
        page.Text = (traditional ? "繁" : "简") + (sample == 5 ? " · 活用 · F6/Esc 返回 · 1 / 2" : sample == 4 ? " · 2 / 3" : " · 1 / 3");
        if (sample == 2 || sample == 3) page.Text += expanded ? " · F4 收起" : " · F4 展开";
        CandidateView.Render(Panel, fields, Selected, -1, expanded || sample == 5, font);
        foreach (Button button in Panel.Children) {
            int index = (int)button.Tag;
            button.Click += (s, e) => { Selected = index; Update(sample, font, width, expanded, traditional); };
        }
    }
}
