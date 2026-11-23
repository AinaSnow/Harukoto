using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using Microsoft.Win32;

internal static class CandidateSettings
{
    private static Slider Control(Panel body, string name, string title, int min, int max, int value)
    {
        var label = new Label { Padding = new Thickness(0), Content = title + "：" + value };
        var slider = new Slider { Name = name, Minimum = min, Maximum = max, Value = value, TickFrequency = 1,
            IsSnapToTickEnabled = true, Margin = new Thickness(0,4,0,12) };
        label.Target = slider; AutomationProperties.SetName(slider, title);
        slider.ValueChanged += (s,e) => label.Content = title + "：" + (int)slider.Value;
        body.Children.Add(label); body.Children.Add(slider); return slider;
    }

    internal static Window Create()
    {
        var work = SystemParameters.WorkArea;
        var window = new Window { Title = "晴词 · 输入设置", Width = Math.Min(1120, work.Width), Height = Math.Min(680, work.Height),
            MinWidth = Math.Min(700, work.Width), MinHeight = Math.Min(480, work.Height),
            WindowStartupLocation = WindowStartupLocation.CenterScreen,
            FontFamily = new System.Windows.Media.FontFamily("Microsoft YaHei UI"), FontSize = 14 };
        var root = new Grid { Margin = new Thickness(24), Background = SystemColors.WindowBrush };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        window.Content = root;
        root.Children.Add(new TextBlock { Text = "输入与候选窗", FontSize = 23, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,20) });
        var columns = new Grid(); Grid.SetRow(columns, 1); root.Children.Add(columns);
        columns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(240) });
        columns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var body = new StackPanel { Margin = new Thickness(0,0,24,0) };
        columns.Children.Add(new ScrollViewer { Content = body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled });
        var traditional = new CheckBox { Name = "TraditionalOutput", Content = "输出繁体中文", Margin = new Thickness(0,0,0,8) };
        var savedLayout = LayoutOptions.Load();
        int font = savedLayout.Font, width = savedLayout.Width, fuzzyMask = 0;
        bool typoCorrection = true, englishPunctuation = false, punctuationHotkey = true;
        int shiftToggle = ShiftToggleOptions.Either;
        bool fullWidthHotkey = false, standaloneEngine = true;
        try {
            using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\Input")) {
                traditional.IsChecked = key != null && Convert.ToString(key.GetValue("TraditionalOutput", 0)) == "1";
                fuzzyMask = FuzzyPinyinOptions.Read(key);
                typoCorrection = TypoCorrectionOptions.Read(key);
                englishPunctuation = PunctuationOptions.ReadEnglish(key);
                punctuationHotkey = PunctuationOptions.ReadHotkey(key);
                shiftToggle = ShiftToggleOptions.Read(key);
                fullWidthHotkey = FullWidthOptions.Read(key);
                standaloneEngine = StandaloneEngineOptions.Read(key);
            }
        } catch { /* Keep usable defaults if preferences are unavailable. */ }
        body.Children.Add(traditional);
        body.Children.Add(new TextBlock { Text = "开启输出繁体，关闭输出简体。日语词形保持原样。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        body.Children.Add(new TextBlock { Text = "模糊音", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,8) });
        var fuzzyPanel = new WrapPanel { Name = "FuzzyPinyin", Margin = new Thickness(0,0,0,4) };
        var fuzzyBoxes = new CheckBox[FuzzyPinyinOptions.Pairs.Length];
        for (int i = 0; i < fuzzyBoxes.Length; ++i) {
            var box = new CheckBox { Name = "Fuzzy" + i, Content = FuzzyPinyinOptions.Pairs[i], IsChecked = (fuzzyMask & (1 << i)) != 0,
                Width = 96, Margin = new Thickness(0,0,0,6) };
            AutomationProperties.SetName(box, "模糊音 " + FuzzyPinyinOptions.Pairs[i]);
            fuzzyBoxes[i] = box; fuzzyPanel.Children.Add(box);
        }
        body.Children.Add(fuzzyPanel);
        body.Children.Add(new TextBlock { Text = "勾选的两种读音互相通用，例如勾选 z = zh 后输入 zongguo 也能打出“中国”。读音完全一致的候选始终优先。默认全部关闭。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        var typo = new CheckBox { Name = "TypoCorrection", Content = "纠正常见手误", IsChecked = typoCorrection, Margin = new Thickness(0,0,0,8) };
        body.Children.Add(new TextBlock { Text = "纠错", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,8) });
        body.Children.Add(typo);
        body.Children.Add(new TextBlock { Text = "把相邻字母颠倒、漏打、多打或按到旁边的键读作想打的拼音，例如 zhonguo、nihoa 也能打出“中国”“你好”。每个候选最多纠正一处，读音完全一致的候选优先。关闭后不再给出任何纠错候选。默认开启。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        body.Children.Add(new TextBlock { Text = "中英文切换", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,8) });
        var shiftMode = new ComboBox { Name = "ShiftToggle", ItemsSource = ShiftToggleOptions.Names, SelectedIndex = shiftToggle, Margin = new Thickness(0,0,0,8) };
        AutomationProperties.SetName(shiftMode, "单按 Shift 切换中英文");
        body.Children.Add(new Label { Content = "单按 Shift 切换中英文", Target = shiftMode, Padding = new Thickness(0,0,0,5) }); body.Children.Add(shiftMode);
        body.Children.Add(new TextBlock { Text = "单独按一下 Shift（不按其他键、0.5 秒内松开）切换中文／英文；按住 Shift 打字、Shift 加鼠标点击不会切换。英文时所有按键交给应用。打拼音时切到英文，已打的字母原样上屏。每个应用各自记住中英文，重新打开应用后为中文。默认左右 Shift 都可以。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        var fullWidth = new CheckBox { Name = "FullWidthHotkey", Content = "Shift+空格 切换全角／半角", IsChecked = fullWidthHotkey, Margin = new Thickness(0,0,0,8) };
        body.Children.Add(fullWidth);
        body.Children.Add(new TextBlock { Text = "默认关闭：很多游戏用 Shift+空格跳跃或冲刺。开启后按 Shift+空格切换全角，全角时数字、空格、英文字母和符号输出全角（如 １２３、ＡＢＣ、全角空格），拼音候选和中文标点不变；每个应用各自记住，重新打开为半角。关闭此项也会回到半角。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        body.Children.Add(new TextBlock { Text = "标点", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,8) });
        var english = new CheckBox { Name = "EnglishPunctuation", Content = "使用英文标点", IsChecked = englishPunctuation, Margin = new Thickness(0,0,0,8) };
        var hotkey = new CheckBox { Name = "PunctuationHotkey", Content = "Ctrl+. 切换中英文标点", IsChecked = punctuationHotkey, Margin = new Thickness(0,0,0,8) };
        body.Children.Add(english); body.Children.Add(hotkey);
        body.Children.Add(new TextBlock { Text = "中文标点下 , . ; [ ] \\ 输出 ，。；「」、，Shift 加符号键输出 ！：“”（）《》——……？￥ 等；/ 始终输出 /。英文标点下全部按原样输出。若 Ctrl+. 与其他软件的快捷键冲突，可关闭快捷键。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        body.Children.Add(new TextBlock { Text = "独立引擎", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,8) });
        var engine = new CheckBox { Name = "StandaloneEngine", Content = "多个应用共用一个输入引擎", IsChecked = standaloneEngine, Margin = new Thickness(0,0,0,8) };
        body.Children.Add(engine);
        body.Children.Add(new TextBlock { Text = "开启后词库只在一个后台引擎里加载一份，各应用不再各自加载，同时开多个应用时省内存、打字更快。引擎不可用时自动改用应用内输入，已打的字不会丢。管理员权限或沙箱中的应用始终使用应用内输入。默认开启；关闭后对之后打开的应用生效，已打开的应用在下一次输入时切换。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        Func<int> fuzzyDraft = () => {
            int mask = 0;
            for (int i = 0; i < fuzzyBoxes.Length; ++i) if (fuzzyBoxes[i].IsChecked == true) mask |= 1 << i;
            return mask;
        };
        var fontLabel = new Label { Padding = new Thickness(0) };
        var fontSlider = new Slider { Name = "FontSizeSlider", Minimum = 14, Maximum = 32, TickFrequency = 1, IsSnapToTickEnabled = true, Value = CandidateLayout.ClampFont(font), Margin = new Thickness(0,8,0,20) };
        var widthLabel = new Label { Padding = new Thickness(0) };
        var widthSlider = new Slider { Name = "WidthSlider", Minimum = 480, Maximum = 1000, TickFrequency = 1, IsSnapToTickEnabled = true, Value = CandidateLayout.ClampWidth(width), Margin = new Thickness(0,8,0,20) };
        fontLabel.Target = fontSlider; widthLabel.Target = widthSlider;
        AutomationProperties.SetName(fontSlider, "候选字号"); AutomationProperties.SetName(widthSlider, "窗口宽度");
        body.Children.Add(fontLabel); body.Children.Add(fontSlider); body.Children.Add(widthLabel); body.Children.Add(widthSlider);
        var direction = new ComboBox { Name = "LayoutDirection", ItemsSource = new[] { "横向自动换行", "纵向列表" }, SelectedIndex = savedLayout.Vertical ? 1 : 0, Margin = new Thickness(0,0,0,12) };
        body.Children.Add(new Label { Content = "排列方向", Target = direction, Padding = new Thickness(0,0,0,5) }); body.Children.Add(direction);
        var autoReading = new CheckBox { Name = "AutoReading", Content = "读音字号跟随正文", IsChecked = savedLayout.Reading == 0, Margin = new Thickness(0,0,0,8) }; body.Children.Add(autoReading);
        var reading = Control(body, "ReadingSlider", "读音字号", 10, 24, savedLayout.Reading == 0 ? (int)savedLayout.ReadingSize : savedLayout.Reading);
        var gapX = Control(body, "GapXSlider", "候选水平间距", 0, 24, savedLayout.GapX);
        var gapY = Control(body, "GapYSlider", "候选垂直间距", 0, 24, savedLayout.GapY);
        var paddingX = Control(body, "PaddingXSlider", "候选左右留白", 4, 24, savedLayout.PaddingX);
        var paddingY = Control(body, "PaddingYSlider", "候选上下留白", 4, 20, savedLayout.PaddingY);
        var outer = Control(body, "OuterSlider", "窗口内边距", 4, 24, savedLayout.Outer);
        var presets = new ComboBox { Name = "LayoutPreset", ItemsSource = new[] { "紧凑", "标准", "宽松" }, SelectedIndex = 1, Margin = new Thickness(0,0,0,8) };
        body.Children.Add(new Label { Content = "布局预设", Target = presets, Padding = new Thickness(0,0,0,5) }); body.Children.Add(presets);
        body.Children.Add(new TextBlock { Text = "应用预设会恢复标准字号、宽度和横向排列。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,8) });
        var applyPreset = new Button { Name = "ApplyPreset", Content = "应用预设", Padding = new Thickness(12,7,12,7), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0,0,0,8) }; body.Children.Add(applyPreset);
        var reset = new Button { Name = "ResetLayout", Content = "恢复默认布局", Padding = new Thickness(12,7,12,7), HorizontalAlignment = HorizontalAlignment.Left };
        body.Children.Add(reset);
        var dictionaries = new Button { Content = "管理主题词库…", Padding = new Thickness(12,7,12,7), Margin = new Thickness(0,20,0,0), HorizontalAlignment = HorizontalAlignment.Left };
        dictionaries.Click += (s,e) => { var manager = DictionaryLauncher.Create(); manager.Owner = window; manager.Show(); };
        body.Children.Add(dictionaries);

        var previewArea = new Grid(); Grid.SetColumn(previewArea, 1); columns.Children.Add(previewArea);
        previewArea.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        previewArea.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        previewArea.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var toolbar = new WrapPanel { Margin = new Thickness(0,0,0,12) };
        var sample = new ComboBox { Name = "PreviewSample", ItemsSource = CandidatePreview.Names, SelectedIndex = 0, Width = 170, Margin = new Thickness(0,0,16,8) };
        AutomationProperties.SetName(sample, "预览示例");
        var sampleLabel = new Label { Content = "预览示例", Target = sample, Padding = new Thickness(0,4,12,0) };
        var expanded = new CheckBox { Name = "PreviewExpanded", Content = "展开日语候选", IsChecked = true, Margin = new Thickness(0,5,0,8) };
        toolbar.Children.Add(sampleLabel); toolbar.Children.Add(sample); toolbar.Children.Add(expanded); previewArea.Children.Add(toolbar);
        var explanation = new TextBlock { Text = "仅用示例演示布局，不会输入文字。点击或 Tab 聚焦预览后，用方向键、1–7 改变高亮。\n拖动右侧边条调整宽度；超出预览区域可滚动。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,16) };
        Grid.SetRow(explanation, 1); previewArea.Children.Add(explanation);
        var preview = new CandidatePreview();
        var previewFrame = new Grid { HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        previewFrame.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        previewFrame.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        previewFrame.Children.Add(preview.Root);
        var grip = new Thumb { Name = "WidthGrip", Width = 10, MinHeight = 80, Cursor = Cursors.SizeWE, Focusable = true,
            Background = SystemParameters.HighContrast ? SystemColors.HighlightBrush : System.Windows.Media.Brushes.SteelBlue, ToolTip = "拖动调整宽度，也可用左右方向键" };
        AutomationProperties.SetName(grip, "拖动候选窗口宽度"); Grid.SetColumn(grip, 1); previewFrame.Children.Add(grip);
        double dragWidth = widthSlider.Value;
        grip.DragStarted += (s,e) => dragWidth = widthSlider.Value;
        grip.DragDelta += (s,e) => { dragWidth = CandidateLayout.ClampWidth(dragWidth + e.HorizontalChange); widthSlider.Value = Math.Round(dragWidth); };
        grip.KeyDown += (s,e) => { if (e.Key == Key.Left || e.Key == Key.Right) { widthSlider.Value = CandidateLayout.ClampWidth(widthSlider.Value + (e.Key == Key.Left ? -8 : 8)); e.Handled = true; } };
        var scroll = new ScrollViewer { Name = "PreviewScroll", Content = previewFrame, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(12), Background = SystemColors.ControlBrush };
        Grid.SetRow(scroll, 2); previewArea.Children.Add(scroll);
        var footer = new Grid { Margin = new Thickness(0,20,0,0) }; Grid.SetRow(footer, 2); root.Children.Add(footer);
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var status = new TextBlock { Text = "保存后，下次输入按键时生效。未保存的调整只用于预览。", TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0,0,16,0) };
        footer.Children.Add(status);
        var actions = new StackPanel { Orientation = Orientation.Horizontal }; Grid.SetColumn(actions, 1); footer.Children.Add(actions);
        var cancel = new Button { Name = "CancelSettings", Content = "取消", Padding = new Thickness(20,7,20,7), Margin = new Thickness(0,0,10,0), IsCancel = true };
        var save = new Button { Content = "保存", Padding = new Thickness(20,7,20,7), IsDefault = true };
        actions.Children.Add(cancel); actions.Children.Add(save);
        Func<LayoutOptions> draft = () => new LayoutOptions { Font = (int)fontSlider.Value, Width = (int)widthSlider.Value,
            Reading = autoReading.IsChecked == true ? 0 : (int)reading.Value, GapX = (int)gapX.Value, GapY = (int)gapY.Value,
            PaddingX = (int)paddingX.Value, PaddingY = (int)paddingY.Value, Outer = (int)outer.Value, Vertical = direction.SelectedIndex == 1 };
        Action<LayoutOptions> setDraft = value => {
            fontSlider.Value = value.Font; widthSlider.Value = value.Width; autoReading.IsChecked = value.Reading == 0;
            reading.Value = value.Reading == 0 ? (int)value.ReadingSize : value.Reading; gapX.Value = value.GapX; gapY.Value = value.GapY;
            paddingX.Value = value.PaddingX; paddingY.Value = value.PaddingY; outer.Value = value.Outer; direction.SelectedIndex = value.Vertical ? 1 : 0;
        };
        Action refresh = () => {
            fontLabel.Content = "候选字号：" + (int)fontSlider.Value;
            widthLabel.Content = "窗口宽度：" + (int)widthSlider.Value;
            expanded.IsEnabled = sample.SelectedIndex == 2 || sample.SelectedIndex == 3;
            reading.IsEnabled = autoReading.IsChecked != true;
            preview.UpdateLayout(sample.SelectedIndex, draft(), expanded.IsChecked == true, traditional.IsChecked == true);
        };
        fontSlider.ValueChanged += (s,e) => refresh(); widthSlider.ValueChanged += (s,e) => refresh();
        sample.SelectionChanged += (s,e) => { preview.Selected = 0; refresh(); };
        expanded.Checked += (s,e) => refresh(); expanded.Unchecked += (s,e) => refresh();
        traditional.Checked += (s,e) => refresh(); traditional.Unchecked += (s,e) => refresh();
        foreach (var slider in new[] { reading, gapX, gapY, paddingX, paddingY, outer }) slider.ValueChanged += (s,e) => refresh();
        direction.SelectionChanged += (s,e) => refresh();
        autoReading.Checked += (s,e) => refresh(); autoReading.Unchecked += (s,e) => refresh();
        applyPreset.Click += (s,e) => { setDraft(LayoutOptions.Preset(presets.SelectedIndex)); status.Text = "预设已应用到草稿，保存后生效。"; };
        reset.Click += (s,e) => { setDraft(new LayoutOptions()); status.Text = "已恢复默认布局，点击保存后生效。简繁、模糊音、纠错、中英文切换、全角、标点、独立引擎与词库设置保持。"; };
        cancel.Click += (s,e) => window.Close();
        save.Click += (s,e) => {
            try {
                // The layout itself is one atomic value; script preference remains independent.
                // If publishing the layout fails, restore the previous script value.
                using (var input = Registry.CurrentUser.CreateSubKey(@"Software\Meowpuut\Input"))
                using (var layout = Registry.CurrentUser.CreateSubKey(LayoutOptions.KeyPath)) {
                    LayoutOptions.SavePreferences(input, layout, draft(), traditional.IsChecked == true);
                    FuzzyPinyinOptions.Write(input, fuzzyDraft());
                    TypoCorrectionOptions.Write(input, typo.IsChecked == true);
                    // Only a changed checkbox is saved: Ctrl+. may have toggled the mode meanwhile.
                    if ((english.IsChecked == true) != englishPunctuation) {
                        PunctuationOptions.WriteEnglish(input, english.IsChecked == true);
                        englishPunctuation = english.IsChecked == true;
                    }
                    PunctuationOptions.WriteHotkey(input, hotkey.IsChecked == true);
                    ShiftToggleOptions.Write(input, shiftMode.SelectedIndex);
                    FullWidthOptions.Write(input, fullWidth.IsChecked == true);
                    StandaloneEngineOptions.Write(input, engine.IsChecked == true);
                }
                status.Text = "已保存，下次输入按键时生效。取消只丢弃此后未保存的调整。";
            } catch (Exception error) { status.Text = "保存失败：" + error.Message; }
        };
        refresh();
        return window;
    }
}
