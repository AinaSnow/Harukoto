using System;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

class CandidateLayoutTests
{
    static T Find<T>(DependencyObject root, string name) where T : FrameworkElement
    {
        var element = root as T;
        if (element != null && element.Name == name) return element;
        foreach (var child in LogicalTreeHelper.GetChildren(root)) {
            var dependency = child as DependencyObject;
            if (dependency == null) continue;
            var found = Find<T>(dependency, name); if (found != null) return found;
        }
        return null;
    }
    static string Preferences()
    {
        string result = "";
        foreach (string path in new[] { @"Software\Meowpuut\Input", @"Software\Meowpuut\CandidateWindow" })
        using (var key = Registry.CurrentUser.OpenSubKey(path)) {
            result += path + (key == null ? "missing" : "present");
            if (key != null) foreach (string name in key.GetValueNames()) result += name + ":" + key.GetValue(name) + ":" + key.GetValueKind(name);
        }
        return result;
    }
    static void Arrange(FrameworkElement root, double width, double height)
    {
        root.Measure(new Size(width, height));
        root.Arrange(new Rect(0, 0, width, double.IsInfinity(height) ? root.DesiredSize.Height : height));
        root.UpdateLayout();
    }
    static void CheckBounds(Panel panel)
    {
        for (int i = 0; i < panel.Children.Count; i++) {
            var child = (FrameworkElement)panel.Children[i];
            var bounds = new Rect(child.TranslatePoint(new Point(), panel), child.RenderSize);
            if (bounds.Right > panel.Width + 1 || bounds.Width < 1 || bounds.Height < 1) throw new Exception("Invalid preview bounds");
            for (int j = 0; j < i; j++) {
                var other = (FrameworkElement)panel.Children[j];
                var intersection = Rect.Intersect(bounds, new Rect(other.TranslatePoint(new Point(), panel), other.RenderSize));
                if (!intersection.IsEmpty && intersection.Width > 0.01 && intersection.Height > 0.01) throw new Exception("Preview overlap");
            }
        }
    }
    static void CheckPreview(Assembly assembly, Window production, Panel panel, MethodInfo update, string directory)
    {
        var flags = BindingFlags.Static | BindingFlags.Instance | BindingFlags.NonPublic;
        var type = assembly.GetType("CandidatePreview");
        var preview = Activator.CreateInstance(type, true);
        var root = (FrameworkElement)type.GetField("Root", flags).GetValue(preview);
        var strip = (Panel)type.GetField("Panel", flags).GetValue(preview);
        var layout = assembly.GetType("CandidateLayout");
        foreach (int sample in new[] { 0, 1, 2, 3, 4, 5 })
        foreach (int width in new[] { 480, 672, 1000 })
        foreach (int size in new[] { 14, 18, 32 })
        foreach (bool expanded in new[] { false, true }) {
            layout.GetField("FontSize", flags).SetValue(null, 23.0);
            type.GetMethod("Update", flags).Invoke(preview, new object[] { sample, (double)size, (double)width, expanded, false });
            if ((double)layout.GetField("FontSize", flags).GetValue(null) != 23) throw new Exception("Preview changed live font");
            Arrange(root, width, double.PositiveInfinity); CheckBounds(strip);
            var fields = (string[])type.GetMethod("Fields", flags).Invoke(null, new object[] { sample, false });
            production.Width = width; panel.Width = width - 26;
            layout.GetField("FontSize", flags).SetValue(null, (double)size);
            update.Invoke(null, new object[] { panel, fields, 0, -1, expanded || sample == 5, null });
            Arrange((FrameworkElement)production.Content, width, double.PositiveInfinity);
            if (strip.Children.Count != panel.Children.Count) throw new Exception("Production/preview count differs");
            for (int i = 0; i < strip.Children.Count; ++i) {
                var a = (FrameworkElement)strip.Children[i]; var b = (FrameworkElement)panel.Children[i];
                if (a.RenderSize != b.RenderSize || a.TranslatePoint(new Point(), strip) != b.TranslatePoint(new Point(), panel) || !a.Tag.Equals(b.Tag))
                    throw new Exception("Production/preview geometry or selection differs");
            }
            if (sample == 5 && width == 480 && size == 32 && expanded) {
                var bitmap = new RenderTargetBitmap(width, (int)Math.Ceiling(root.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                bitmap.Render(root); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var file = File.Create(Path.Combine(directory, "preview-narrow-large.png"))) encoder.Save(file);
            }
        }
        ((Button)strip.Children[2]).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if ((int)type.GetField("Selected", flags).GetValue(preview) != 2) throw new Exception("Preview selection failed");
        Console.WriteLine("PASS: 108 preview/production combinations, identical geometry and indices, no overlaps, isolated font and local selection.");
    }
    static void CheckAdvanced(Assembly assembly, Window production, Panel panel, MethodInfo update, string directory)
    {
        var flags = BindingFlags.Static | BindingFlags.Instance | BindingFlags.NonPublic;
        var optionType = assembly.GetType("LayoutOptions"); var layout = assembly.GetType("CandidateLayout");
        var previewType = assembly.GetType("CandidatePreview"); var preview = Activator.CreateInstance(previewType, true);
        var root = (FrameworkElement)previewType.GetField("Root", flags).GetValue(preview);
        var strip = (Panel)previewType.GetField("Panel", flags).GetValue(preview);
        foreach (int sample in new[] { 0, 1, 2, 3, 4, 5 })
        foreach (int preset in new[] { 0, 1, 2, 3 })
        foreach (bool vertical in new[] { false, true })
        foreach (int width in new[] { 480, 1000 }) {
            var options = optionType.GetMethod("Preset", flags).Invoke(null, new object[] { preset });
            optionType.GetField("Width", flags).SetValue(options, width); optionType.GetField("Vertical", flags).SetValue(options, vertical);
            if (preset == 3) {
                foreach (string name in new[] { "Reading", "Outer", "PaddingX" }) optionType.GetField(name, flags).SetValue(options, 24);
                optionType.GetField("PaddingY", flags).SetValue(options, 20);
                optionType.GetField("Font", flags).SetValue(options, 32);
                optionType.GetField("GapX", flags).SetValue(options, 0); optionType.GetField("GapY", flags).SetValue(options, 0);
            }
            previewType.GetMethod("UpdateLayout", flags).Invoke(preview, new[] { (object)sample, options, true, false });
            Arrange(root, width, double.PositiveInfinity); CheckBounds(strip);
            var fields = (string[])previewType.GetMethod("Fields", flags).Invoke(null, new object[] { sample, false });
            layout.GetField("Current", flags).SetValue(null, options);
            layout.GetField("FontSize", flags).SetValue(null, (double)(int)optionType.GetField("Font", flags).GetValue(options));
            production.Width = width;
            layout.GetMethod("Apply", flags).Invoke(null, new[] { production.Content, panel, options, (object)(double)width });
            update.Invoke(null, new object[] { panel, fields, 0, -1, true, null });
            Arrange((FrameworkElement)production.Content, width, double.PositiveInfinity);
            if (strip.Children.Count != 7 || panel.Children.Count != 7) throw new Exception("Advanced layout missing candidate");
            for (int i = 0; i < 7; ++i) {
                var a = (FrameworkElement)strip.Children[i]; var b = (FrameworkElement)panel.Children[i];
                if (a.RenderSize != b.RenderSize || a.TranslatePoint(new Point(), strip) != b.TranslatePoint(new Point(), panel) || (int)a.Tag != i)
                    throw new Exception("Advanced production geometry/indices mismatch");
                if (vertical && (a.TranslatePoint(new Point(), strip).X != 0 || (i > 0 && a.TranslatePoint(new Point(), strip).Y <= ((FrameworkElement)strip.Children[i - 1]).TranslatePoint(new Point(), strip).Y)))
                    throw new Exception("Vertical candidate not on its own row");
            }
            if (sample == 5 && preset == 1 && vertical && width == 480) {
                var bitmap = new RenderTargetBitmap(width, (int)Math.Ceiling(root.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                bitmap.Render(root); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var file = File.Create(Path.Combine(directory, "preview-vertical.png"))) encoder.Save(file);
            }
            if (preset == 3 && vertical) {
                layout.GetMethod("FitHeight", flags).Invoke(null, new object[] { production, 360.0 });
                var content = (FrameworkElement)production.Content;
                if (content.DesiredSize.Height > 360.1 || production.Width > width) throw new Exception("Tall layout escaped work area");
                if ((int)optionType.GetField("Width", flags).GetValue(options) != width) throw new Exception("Fit changed saved width");
                content.LayoutTransform = Transform.Identity;
            }
        }
        layout.GetField("Current", flags).SetValue(null, Activator.CreateInstance(optionType, true));
        Console.WriteLine("PASS: 96 advanced layout combinations, presets/vertical/max padding/reading/min gaps, identical production geometry and seven indices.");
    }
    [STAThread] static void Main(string[] args)
    {
        var assembly = Assembly.LoadFrom(args[0]);
        Window window;
        using (var stream = assembly.GetManifestResourceStream("CandidateWindow.xaml"))
            window = (Window)XamlReader.Load(stream);
        var layout = assembly.GetType("CandidateLayout");
        var flags = BindingFlags.Static | BindingFlags.NonPublic;
        var panel = (WrapPanel)layout.GetMethod("Create", flags).Invoke(null, new object[] { window });
        Geometry paw = null;
        ((FrameworkElement)window.FindName("CompositionHeader")).Visibility = Visibility.Visible;
        ((TextBlock)window.FindName("Preedit")).Text = "jiankangbaoxian";
        string[] fields = { "0", "0", "jiankangbaoxian", "健康保险", "健康保険\tけんこうほけん · 拼\tja",
            "这是一个需要完整显示而不应该挤成省略号的长候选词", "健康保险制度", "健康保险公司", "健康保险费用", "健康保険証\tけんこうほけんしょう · 拼\tja" };
        var update = assembly.GetType("GlassPreview").GetMethod("UpdateCandidates", flags);
        foreach (int width in new[] { 480, 672, 1000 })
        foreach (int size in new[] { 18, 26, 32 }) {
            window.Width = width; panel.Width = width - 26;
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
        string[] pair = { "0", "0", "riben", "日本", "日本\tにほん · 拼\tja-direct",
            "にほん\t日本 · 假名\tja-direct", "日锛", "日奔", "日苯", "日笨" };
        foreach (int highlight in new[] { 0, 1, 2 }) {
            update.Invoke(null, new object[] { panel, pair, highlight, -1, false, paw });
            if (panel.Children.Count != 7) throw new Exception("Direct spelling/kana pair must stay visible when collapsed.");
        }
        Console.WriteLine("PASS: both spelling and kana stay selectable when collapsed.");
        window.Width = 672; panel.Width = 646;
        layout.GetField("FontSize", flags).SetValue(null, 18.0);
        ((TextBlock)window.FindName("Preedit")).Text = "riben";
        ((TextBlock)window.FindName("PageIndicator")).Text = "简";
        update.Invoke(null, new object[] { panel, pair, 2, -1, false, paw });
        var pairRoot = (FrameworkElement)window.Content;
        pairRoot.Measure(new Size(672, double.PositiveInfinity));
        pairRoot.Arrange(new Rect(0, 0, 672, pairRoot.DesiredSize.Height));
        pairRoot.UpdateLayout();
        var pairImage = new RenderTargetBitmap(672, (int)Math.Ceiling(pairRoot.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        pairImage.Render(pairRoot);
        var pairEncoder = new PngBitmapEncoder(); pairEncoder.Frames.Add(BitmapFrame.Create(pairImage));
        using (var file = File.Create(Path.Combine(Path.GetDirectoryName(args[1]), "kana-choice.png"))) pairEncoder.Save(file);
        ((TextBlock)window.FindName("Preedit")).Text = "chi";
        ((TextBlock)window.FindName("PageIndicator")).Text = "简 · 活用 · F6/Esc 返回 · 1 / 2";
        string[] forms = { "0", "0", "chi", "食べる\tたべる · 辞书形\tja", "食べます\tたべます · ます形\tja",
            "食べて\tたべて · て形\tja", "食べた\tたべた · た形\tja", "食べない\tたべない · ない形\tja",
            "食べられる\tたべられる · 可能形\tja", "食べられる\tたべられる · 被动形\tja" };
        update.Invoke(null, new object[] { panel, forms, 1, -1, true, paw });
        var formRoot = (FrameworkElement)window.Content;
        formRoot.Measure(new Size(672, double.PositiveInfinity));
        formRoot.Arrange(new Rect(0, 0, 672, formRoot.DesiredSize.Height));
        formRoot.UpdateLayout();
        if (panel.Children.Count != 7) throw new Exception("Conjugation forms were collapsed.");
        var formImage = new RenderTargetBitmap(672, (int)Math.Ceiling(formRoot.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        formImage.Render(formRoot);
        var formEncoder = new PngBitmapEncoder(); formEncoder.Frames.Add(BitmapFrame.Create(formImage));
        using (var file = File.Create(Path.Combine(Path.GetDirectoryName(args[1]), "conjugation-panel.png"))) formEncoder.Save(file);
        CheckPreview(assembly, window, panel, update, Path.GetDirectoryName(args[1]));
        CheckAdvanced(assembly, window, panel, update, Path.GetDirectoryName(args[1]));
        string beforePreferences = Preferences();
        var settings = (Window)assembly.GetType("CandidateSettings").GetMethod("Create", flags).Invoke(null, null);
        var settingsRoot = (FrameworkElement)settings.Content;
        ((Panel)settingsRoot).Background = SystemColors.WindowBrush;
        var fontControl = Find<Slider>(settingsRoot, "FontSizeSlider");
        var widthControl = Find<Slider>(settingsRoot, "WidthSlider");
        var traditional = Find<CheckBox>(settingsRoot, "TraditionalOutput");
        var samples = Find<ComboBox>(settingsRoot, "PreviewSample");
        var scroll = Find<ScrollViewer>(settingsRoot, "PreviewScroll");
        traditional.IsChecked = true; fontControl.Value = 32; widthControl.Value = 480;
        samples.SelectedIndex = 5;
        if (Find<FrameworkElement>(settingsRoot, "CandidatePreviewRoot").Width != 480) throw new Exception("Width slider did not refresh preview");
        Find<Button>(settingsRoot, "ResetLayout").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if (fontControl.Value != 18 || widthControl.Value != 672 || traditional.IsChecked != true) throw new Exception("Reset changed script or failed to reset layout");
        if (Find<FrameworkElement>(settingsRoot, "CandidatePreviewRoot").Width != 672) throw new Exception("Reset did not refresh preview");
        var direction = Find<ComboBox>(settingsRoot, "LayoutDirection");
        direction.SelectedIndex = 1;
        Find<CheckBox>(settingsRoot, "AutoReading").IsChecked = false;
        Find<Slider>(settingsRoot, "ReadingSlider").Value = 24;
        Find<Slider>(settingsRoot, "GapYSlider").Value = 24;
        var grip = Find<Thumb>(settingsRoot, "WidthGrip");
        grip.RaiseEvent(new DragStartedEventArgs(0, 0) { RoutedEvent = Thumb.DragStartedEvent });
        grip.RaiseEvent(new DragDeltaEventArgs(104, 0) { RoutedEvent = Thumb.DragDeltaEvent });
        if (widthControl.Value != 776 || Find<FrameworkElement>(settingsRoot, "CandidatePreviewRoot").Width != 776) throw new Exception("Drag width did not sync");
        grip.RaiseEvent(new DragDeltaEventArgs(10000, 0) { RoutedEvent = Thumb.DragDeltaEvent });
        if (widthControl.Value != 1000) throw new Exception("Drag width exceeded limit");
        Find<ComboBox>(settingsRoot, "LayoutPreset").SelectedIndex = 0;
        Find<Button>(settingsRoot, "ApplyPreset").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if (Find<Slider>(settingsRoot, "GapYSlider").Value != 4 || direction.SelectedIndex != 0 || traditional.IsChecked != true) throw new Exception("Preset applied incorrectly");
        Find<Button>(settingsRoot, "ResetLayout").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Arrange(settingsRoot, settings.Width - 16, settings.Height - 40);
        var settingsImage = new RenderTargetBitmap((int)settings.Width, (int)Math.Ceiling(settingsRoot.ActualHeight + 48), 96, 96, PixelFormats.Pbgra32);
        settingsImage.Render(settingsRoot);
        var settingsEncoder = new PngBitmapEncoder();
        settingsEncoder.Frames.Add(BitmapFrame.Create(settingsImage));
        using (var file = File.Create(Path.Combine(Path.GetDirectoryName(args[1]), "candidate-settings.png"))) settingsEncoder.Save(file);
        Arrange(settingsRoot, 700 - 16, 480 - 40);
        if (scroll.ViewportWidth <= 0 || scroll.ViewportHeight <= 0) throw new Exception("Small settings window has no preview viewport");
        Find<Button>(settingsRoot, "CancelSettings").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if (Preferences() != beforePreferences) throw new Exception("Draft editing or cancellation changed user preferences");
        Console.WriteLine("PASS: live sliders, layout-only reset, small-window scrolling and cancellation without changing user preferences.");
        var launcher = (Window)assembly.GetType("DictionaryLauncher").GetMethod("Create", flags).Invoke(null, null);
        var launcherRoot = (FrameworkElement)launcher.Content;
        ((Panel)launcherRoot).Background = SystemColors.WindowBrush;
        launcherRoot.Measure(new Size(launcher.Width - 32, double.PositiveInfinity));
        launcherRoot.Arrange(new Rect(new Point(), launcherRoot.DesiredSize)); launcherRoot.UpdateLayout();
        var launcherImage = new RenderTargetBitmap((int)launcher.Width, (int)Math.Ceiling(launcherRoot.ActualHeight + 48), 96, 96, PixelFormats.Pbgra32);
        launcherImage.Render(launcherRoot);
        var launcherEncoder = new PngBitmapEncoder(); launcherEncoder.Frames.Add(BitmapFrame.Create(launcherImage));
        using (var file = File.Create(Path.Combine(Path.GetDirectoryName(args[1]), "dictionary-launcher.png"))) launcherEncoder.Save(file);
        Console.WriteLine("PASS: dictionary launcher created and rendered without launching Python.");
    }
}
