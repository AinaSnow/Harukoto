using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.Win32;

internal static class CandidateSettings
{
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
        int font = 18, width = 672;
        try {
            using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\Input")) {
                traditional.IsChecked = key != null && Convert.ToString(key.GetValue("TraditionalOutput", 0)) == "1";
            }
            using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\CandidateWindow")) {
                int parsed;
                if (key != null) {
                    if (int.TryParse(Convert.ToString(key.GetValue("FontSize", 18)), out parsed)) font = parsed;
                    if (int.TryParse(Convert.ToString(key.GetValue("Width", 672)), out parsed)) width = parsed;
                }
            }
        } catch { /* Keep usable defaults if preferences are unavailable. */ }
        body.Children.Add(traditional);
        body.Children.Add(new TextBlock { Text = "开启输出繁体，关闭输出简体。日语词形保持原样。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,24) });
        var fontLabel = new Label { Padding = new Thickness(0) };
        var fontSlider = new Slider { Name = "FontSizeSlider", Minimum = 14, Maximum = 32, TickFrequency = 1, IsSnapToTickEnabled = true, Value = CandidateLayout.ClampFont(font), Margin = new Thickness(0,8,0,20) };
        var widthLabel = new Label { Padding = new Thickness(0) };
        var widthSlider = new Slider { Name = "WidthSlider", Minimum = 480, Maximum = 1000, TickFrequency = 8, IsSnapToTickEnabled = true, Value = CandidateLayout.ClampWidth(width), Margin = new Thickness(0,8,0,20) };
        fontLabel.Target = fontSlider; widthLabel.Target = widthSlider;
        AutomationProperties.SetName(fontSlider, "候选字号"); AutomationProperties.SetName(widthSlider, "窗口宽度");
        body.Children.Add(fontLabel); body.Children.Add(fontSlider); body.Children.Add(widthLabel); body.Children.Add(widthSlider);
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
        var explanation = new TextBlock { Text = "仅用示例演示布局；点击候选可预览高亮，不会输入文字。\n按当前显示缩放展示，宽度超出预览区时可横向滚动。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,16) };
        Grid.SetRow(explanation, 1); previewArea.Children.Add(explanation);
        var preview = new CandidatePreview();
        var scroll = new ScrollViewer { Name = "PreviewScroll", Content = preview.Root, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(12), Background = SystemColors.ControlBrush };
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
        Action refresh = () => {
            fontLabel.Content = "候选字号：" + (int)fontSlider.Value;
            widthLabel.Content = "窗口宽度：" + (int)widthSlider.Value;
            expanded.IsEnabled = sample.SelectedIndex == 2 || sample.SelectedIndex == 3;
            preview.Update(sample.SelectedIndex, fontSlider.Value, widthSlider.Value, expanded.IsChecked == true, traditional.IsChecked == true);
        };
        fontSlider.ValueChanged += (s,e) => refresh(); widthSlider.ValueChanged += (s,e) => refresh();
        sample.SelectionChanged += (s,e) => { preview.Selected = 0; refresh(); };
        expanded.Checked += (s,e) => refresh(); expanded.Unchecked += (s,e) => refresh();
        traditional.Checked += (s,e) => refresh(); traditional.Unchecked += (s,e) => refresh();
        reset.Click += (s,e) => { fontSlider.Value = 18; widthSlider.Value = 672; status.Text = "已恢复默认布局，点击保存后生效。简繁与词库设置保持。"; };
        cancel.Click += (s,e) => window.Close();
        save.Click += (s,e) => {
            try {
                using (var key = Registry.CurrentUser.CreateSubKey(@"Software\Meowpuut\Input")) {
                    key.SetValue("TraditionalOutput", traditional.IsChecked == true ? 1 : 0, RegistryValueKind.DWord);
                }
                using (var key = Registry.CurrentUser.CreateSubKey(@"Software\Meowpuut\CandidateWindow")) {
                    key.SetValue("FontSize", (int)fontSlider.Value, RegistryValueKind.DWord);
                    key.SetValue("Width", (int)widthSlider.Value, RegistryValueKind.DWord);
                }
                status.Text = "已保存，下次输入按键时生效。取消只丢弃此后未保存的调整。";
            } catch (Exception error) { status.Text = "保存失败：" + error.Message; }
        };
        refresh();
        return window;
    }
}