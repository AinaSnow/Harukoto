using System;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

internal static class CandidateSettings
{
    internal static Window Create()
    {
        var window = new Window { Title = "晴词 · 输入设置", Width = 440, SizeToContent = SizeToContent.Height,
            ResizeMode = ResizeMode.NoResize, WindowStartupLocation = WindowStartupLocation.CenterScreen,
            FontFamily = new System.Windows.Media.FontFamily("Microsoft YaHei UI"), FontSize = 14 };
        var body = new StackPanel { Margin = new Thickness(24) };
        window.Content = body;
        body.Children.Add(new TextBlock { Text = "输入与候选窗", FontSize = 23, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0,0,0,16) });
        var traditional = new CheckBox { Content = "输出繁体中文", Margin = new Thickness(0,0,0,8) };
        using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\Input")) {
            traditional.IsChecked = key != null && Convert.ToString(key.GetValue("TraditionalOutput", 0)) == "1";
        }
        body.Children.Add(traditional);
        body.Children.Add(new TextBlock { Text = "开启输出繁体，关闭输出简体。日语词形保持原样。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,18) });
        int font = 18, width = 672;
        using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\CandidateWindow")) {
            if (key != null) {
                int.TryParse(Convert.ToString(key.GetValue("FontSize", 18)), out font);
                int.TryParse(Convert.ToString(key.GetValue("Width", 672)), out width);
            }
        }
        var fontLabel = new TextBlock();
        var fontSlider = new Slider { Minimum = 14, Maximum = 32, TickFrequency = 1, IsSnapToTickEnabled = true, Value = Math.Max(14, Math.Min(32,font)), Margin = new Thickness(0,8,0,18) };
        var widthLabel = new TextBlock();
        var widthSlider = new Slider { Minimum = 480, Maximum = 1000, TickFrequency = 8, IsSnapToTickEnabled = true, Value = Math.Max(480, Math.Min(1000,width)), Margin = new Thickness(0,8,0,18) };
        body.Children.Add(fontLabel); body.Children.Add(fontSlider); body.Children.Add(widthLabel); body.Children.Add(widthSlider);
        var preview = new TextBlock { Text = "你好　図書館　コーヒー", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,16) };
        body.Children.Add(preview);
        Action refresh = () => { fontLabel.Text = "候选字号：" + (int)fontSlider.Value; widthLabel.Text = "窗口宽度：" + (int)widthSlider.Value; preview.FontSize = fontSlider.Value; };
        fontSlider.ValueChanged += (s,e) => refresh(); widthSlider.ValueChanged += (s,e) => refresh(); refresh();
        var status = new TextBlock { Text = "保存后，下次输入按键时生效。候选自动换行。", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,16) };
        body.Children.Add(status);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        var reset = new Button { Content = "恢复默认", Padding = new Thickness(14,7,14,7), Margin = new Thickness(0,0,10,0) };
        var save = new Button { Content = "保存", Padding = new Thickness(20,7,20,7), IsDefault = true };
        reset.Click += (s,e) => { fontSlider.Value = 18; widthSlider.Value = 672; traditional.IsChecked = false; status.Text = "已恢复默认，点击保存后生效。"; };
        save.Click += (s,e) => {
            try {
                using (var key = Registry.CurrentUser.CreateSubKey(@"Software\Meowpuut\Input")) {
                    key.SetValue("TraditionalOutput", traditional.IsChecked == true ? 1 : 0, RegistryValueKind.DWord);
                }
                using (var key = Registry.CurrentUser.CreateSubKey(@"Software\Meowpuut\CandidateWindow")) {
                    key.SetValue("FontSize", (int)fontSlider.Value, RegistryValueKind.DWord);
                    key.SetValue("Width", (int)widthSlider.Value, RegistryValueKind.DWord);
                }
                status.Text = "已保存，下次输入按键时生效。";
            } catch (Exception error) { status.Text = "保存失败：" + error.Message; }
        };
        actions.Children.Add(reset); actions.Children.Add(save); body.Children.Add(actions);
        return window;
    }
}
