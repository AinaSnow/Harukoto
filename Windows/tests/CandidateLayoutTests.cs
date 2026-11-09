using System;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;

class CandidateLayoutTests
{
    [STAThread] static void Main(string[] args)
    {
        var assembly = Assembly.LoadFrom(args[0]);
        Window window;
        using (var stream = assembly.GetManifestResourceStream("GlassPreview.xaml"))
            window = (Window)XamlReader.Load(stream);
        var layout = assembly.GetType("CandidateLayout");
        var flags = BindingFlags.Static | BindingFlags.NonPublic;
        var panel = (WrapPanel)layout.GetMethod("Create", flags).Invoke(null, new object[] { window });
        var paw = ((System.Windows.Shapes.Path)window.FindName("JapanesePaw")).Data;
        ((FrameworkElement)window.FindName("JapanesePaw")).Visibility = Visibility.Collapsed;
        ((FrameworkElement)window.FindName("CompositionHeader")).Visibility = Visibility.Visible;
        ((TextBlock)window.FindName("Preedit")).Text = "jiankangbaoxian";
        string[] fields = { "0", "0", "jiankangbaoxian", "健康保险", "健康保険\tけんこうほけん · 拼\tja",
            "这是一个需要完整显示而不应该挤成省略号的长候选词", "健康保险制度", "健康保险公司", "健康保险费用", "健康保険証\tけんこうほけんしょう · 拼\tja" };
        var update = assembly.GetType("GlassPreview").GetMethod("UpdateCandidates", flags);
        foreach (int width in new[] { 480, 672, 1000 })
        foreach (int size in new[] { 18, 26, 32 }) {
            window.Width = width; panel.Width = width - 42;
            layout.GetField("FontSize", flags).SetValue(null, (double)size);
            update.Invoke(null, new object[] { panel, fields, 1, -1, true, paw });
            var root = (FrameworkElement)window.Content;
            root.Measure(new Size(width, double.PositiveInfinity));
            root.Arrange(new Rect(0, 0, width, root.DesiredSize.Height));
            root.UpdateLayout();
            if (panel.Children.Count != 7 || panel.ActualHeight <= 44) throw new Exception("Missing candidates or wrapping.");
            for (int i = 0; i < panel.Children.Count; i++) {
                var child = (FrameworkElement)panel.Children[i];
                var point = child.TranslatePoint(new Point(), panel);
                var bounds = new Rect(point, child.RenderSize);
                if (bounds.Right > panel.Width + 1) throw new Exception("Candidate exceeds panel.");
                for (int j = 0; j < i; j++) {
                    var other = (FrameworkElement)panel.Children[j];
                    var previous = new Rect(other.TranslatePoint(new Point(), panel), other.RenderSize);
                    if (bounds.IntersectsWith(previous)) throw new Exception("Candidates overlap.");
                }
            }
            if (width == 672 && size == 18) {
                var bitmap = new RenderTargetBitmap(width, (int)Math.Ceiling(root.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                bitmap.Render(root);
                var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var file = File.Create(args[1])) encoder.Save(file);
            }
        }
        Console.WriteLine("PASS: 9 width/font combinations, seven visible candidates, no overlaps.");
        var settings = (Window)assembly.GetType("CandidateSettings").GetMethod("Create", flags).Invoke(null, null);
        var settingsRoot = (FrameworkElement)settings.Content;
        ((Panel)settingsRoot).Background = SystemColors.WindowBrush;
        settingsRoot.Measure(new Size(settings.Width - 32, double.PositiveInfinity));
        settingsRoot.Arrange(new Rect(new Point(), settingsRoot.DesiredSize));
        settingsRoot.UpdateLayout();
        var settingsImage = new RenderTargetBitmap((int)settings.Width, (int)Math.Ceiling(settingsRoot.ActualHeight + 48), 96, 96, PixelFormats.Pbgra32);
        settingsImage.Render(settingsRoot);
        var settingsEncoder = new PngBitmapEncoder();
        settingsEncoder.Frames.Add(BitmapFrame.Create(settingsImage));
        using (var file = File.Create(Path.Combine(Path.GetDirectoryName(args[1]), "candidate-settings.png"))) settingsEncoder.Save(file);
        Console.WriteLine("PASS: settings window created and rendered without changing user preferences.");
    }
}
