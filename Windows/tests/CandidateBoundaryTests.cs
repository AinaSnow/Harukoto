using System;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;

class CandidateBoundaryTests
{
    const BindingFlags Flags = BindingFlags.Static | BindingFlags.Instance | BindingFlags.NonPublic;
    static void Check(bool condition, string reason) { if (!condition) throw new Exception(reason); }
    static void Close(double a, double b, string reason) { Check(Math.Abs(a - b) < 0.05, reason); }
    [STAThread] static void Main(string[] args)
    {
        try { Run(args); }
        catch (Exception error) {
            while (error.InnerException != null) error = error.InnerException;
            Console.Error.WriteLine(error.GetType().FullName + ": " + error.Message + "\n" + error.StackTrace);
            Environment.ExitCode = 1;
        }
    }
    static void Run(string[] args)
    {
        var assembly = Assembly.LoadFrom(args[0]);
        var layout = assembly.GetType("CandidateLayout");
        var optionsType = assembly.GetType("LayoutOptions");
        var previewType = assembly.GetType("CandidatePreview");
        var view = assembly.GetType("CandidateView");
        Window window;
        using (var stream = assembly.GetManifestResourceStream("CandidateWindow.xaml")) window = (Window)XamlReader.Load(stream);
        var root = (FrameworkElement)window.Content;
        var panel = (Panel)window.FindName("Candidates");
        int cases = 0;
        foreach (double scale in new[] { 1.0, 1.25, 1.5, 2.0 })
        foreach (Rect pixels in new[] { new Rect(0, 0, 1920, 1032), new Rect(-1920, -300, 1920, 1032), new Rect(1920, 40, 1280, 680), new Rect(-120, -40, 120, 160) })
        foreach (bool capsule in new[] { false, true })
        foreach (int sample in new[] { 1, 3, 5 })
        foreach (int font in new[] { 18, 32 }) {
            var matrix = new Matrix(1 / scale, 0, 0, 1 / scale, 0, 0);
            var work = (Rect)layout.GetMethod("DeviceWorkArea", Flags).Invoke(null, new object[] { pixels, matrix });
            Close(work.Left * scale, pixels.Left, "negative monitor origin");
            Close(work.Height * scale, pixels.Height, "work area scale");
            var options = Activator.CreateInstance(optionsType, true);
            optionsType.GetField("Font", Flags).SetValue(options, font);
            optionsType.GetField("Width", Flags).SetValue(options, 1000);
            optionsType.GetField("Theme", Flags).SetValue(options, capsule ? 1 : 0);
            if (font == 32) {
                foreach (string name in new[] { "Reading", "Outer", "PaddingX", "GapX", "GapY" }) optionsType.GetField(name, Flags).SetValue(options, 24);
                optionsType.GetField("PaddingY", Flags).SetValue(options, 20);
            }
            window.Width = Math.Max(480, Math.Min(1000, work.Width));
            root.LayoutTransform = Transform.Identity;
            layout.GetMethod("Apply", Flags).Invoke(null, new object[] { root, panel, options, window.Width });
            var fields = (string[])previewType.GetMethod("Fields", Flags).Invoke(null, new object[] { sample, false });
            ((TextBlock)window.FindName("Preedit")).Text = new string('a', 64);
            ((TextBlock)window.FindName("PageIndicator")).Text = "繁 · 活用 · F6/Esc 返回 · 999 / 999";
            view.GetMethod("Render", Flags).Invoke(null, new object[] { panel, fields, 0, -1, true, options });
            layout.GetMethod("FitToWorkArea", Flags).Invoke(null, new object[] { window, work.Size });
            double fittedWidth = window.Width, fittedHeight = root.DesiredSize.Height;
            Check(fittedWidth <= work.Width + .05 && fittedHeight <= work.Height + .05, "fitted bounds");
            layout.GetMethod("FitToWorkArea", Flags).Invoke(null, new object[] { window, work.Size });
            Close(window.Width, fittedWidth, "repeated fit shrank width"); Close(root.DesiredSize.Height, fittedHeight, "repeated fit changed height");
            foreach (Point anchor in new[] { work.TopLeft, work.TopRight, work.BottomLeft, work.BottomRight, new Point(work.Left - 300, work.Top - 300), new Point(work.Right + 300, work.Bottom + 300) }) {
                var location = (Point)layout.GetMethod("Place", Flags).Invoke(null, new object[] { work, anchor, new Size(fittedWidth, fittedHeight) });
                Check(location.X >= work.Left - .05 && location.Y >= work.Top - .05 && location.X + fittedWidth <= work.Right + .05 && location.Y + fittedHeight <= work.Bottom + .05, "edge position");
            }
            root.Arrange(new Rect(0, 0, fittedWidth, fittedHeight)); root.UpdateLayout();
            Check(panel.Children.Count == 7, "candidate lost at boundary");
            for (int i = 0; i < 7; ++i) {
                var child = (FrameworkElement)panel.Children[i];
                Check((int)child.Tag == i, "candidate index changed");
                var position = child.TranslatePoint(new Point(), root);
                double factor = root.LayoutTransform.Value.M11;
                Check(position.X >= -.05 && position.Y >= -.05 && (position.X + child.ActualWidth) * factor <= fittedWidth + .1 &&
                    (position.Y + child.ActualHeight) * factor <= fittedHeight + .1, "candidate clipped after fitting");
            }
            if (capsule && sample == 5 && font == 18 && pixels.Left == 0) {
                var bitmap = new RenderTargetBitmap((int)Math.Ceiling(fittedWidth * scale), (int)Math.Ceiling(fittedHeight * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
                bitmap.Render(root); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var output = File.Create(Path.Combine(args[1], "boundary-" + (int)(scale * 100) + ".png"))) encoder.Save(output);
            }
            layout.GetMethod("FitToWorkArea", Flags).Invoke(null, new object[] { window, new Size(4000, 4000) });
            Close(window.Width, panel.Width + 2 * (int)optionsType.GetField("Outer", Flags).GetValue(options) + 2 + root.Margin.Left + root.Margin.Right, "large work area did not restore natural width");
            ++cases;
        }
        Console.WriteLine("PASS: " + cases + " simulated scale/layout/work-area cases, six anchors each, repeated fit and large-area recovery, seven indices retained.");

        var preview = Activator.CreateInstance(previewType, true);
        var previewRoot = (FrameworkElement)previewType.GetField("Root", Flags).GetValue(preview);
        previewType.GetMethod("Update", Flags).Invoke(preview, new object[] { 3, 18.0, 672.0, false, false });
        var select = previewType.GetMethod("SelectKey", Flags);
        Action<Key, int> key = (k, expected) => {
            Check((bool)select.Invoke(preview, new object[] { k }), "key not handled");
            Check((int)previewType.GetField("Selected", Flags).GetValue(preview) == expected, "wrong preview selection");
        };
        key(Key.Right, 1); key(Key.Right, 6); key(Key.Left, 1); key(Key.D3, 1); // hidden candidate must not be selected
        key(Key.NumPad7, 6); key(Key.Home, 0); key(Key.End, 6); key(Key.Down, 6);
        Check(!(bool)select.Invoke(preview, new object[] { Key.Tab }), "Tab trapped");
        using (var source = new HwndSource(new HwndSourceParameters("HarukotoKeyTest") { Width = 1, Height = 1, WindowStyle = 0 })) {
            foreach (Key k in new[] { Key.Enter, Key.Space, Key.D1 }) {
                var e = new KeyEventArgs(Keyboard.PrimaryDevice, source, 0, k) { RoutedEvent = UIElement.KeyDownEvent };
                previewRoot.RaiseEvent(e); Check(e.Handled, "preview routed key escaped to save");
            }
        }
        Check(previewRoot.Focusable, "preview not keyboard focusable");
        Console.WriteLine("PASS: preview keyboard selection skips hidden rows, keypad/home/end, Tab escape, routed Enter/Space isolation.");

        // Hidden HWND integration against the current desktop; never display or install.
        new WindowInteropHelper(window).EnsureHandle();
        var sourceWindow = HwndSource.FromHwnd(new WindowInteropHelper(window).Handle);
        var fromDevice = sourceWindow.CompositionTarget.TransformFromDevice;
        int monitors = 0;
        foreach (var screen in System.Windows.Forms.Screen.AllScreens) {
            int x = screen.WorkingArea.Left + 10, y = screen.WorkingArea.Top + 10;
            var actual = (Rect)layout.GetMethod("Prepare", Flags).Invoke(null, new object[] { window, panel, x, y });
            var expected = new Rect(fromDevice.Transform(new Point(screen.WorkingArea.Left, screen.WorkingArea.Top)), fromDevice.Transform(new Point(screen.WorkingArea.Right, screen.WorkingArea.Bottom)));
            Close(actual.Left, expected.Left, "monitor selection origin"); Close(actual.Width, expected.Width, "monitor selection width");
            Close(actual.Top, expected.Top, "monitor taskbar origin"); Close(actual.Height, expected.Height, "monitor taskbar height");
            ++monitors;
        }
        window.Close();
        Console.WriteLine("PASS: hidden HWND Prepare selected " + monitors + " current desktop work areas; renderer transform " + fromDevice.M11 + "/" + fromDevice.M22 + ". Not a mixed-DPI or streaming visual sign-off.");
    }
}
