using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows.Interop;
using System.Windows.Threading;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;

internal static class GlassPreview
{
    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--settings")
            new Application().Run(CandidateSettings.Create());
        else if (args.Length == 1 && args[0] == "--dictionaries")
            new Application().Run(DictionaryLauncher.Create());
        else if (args.Length == 2 && args[0] == "--ime")
            RunCandidateHost(args[1]);
        else
            new Application().Run(CreateWindow());
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CopyData
    {
        internal IntPtr Tag;
        internal int Bytes;
        internal IntPtr Text;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong(IntPtr window, int index);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    private static extern int SetWindowLong(IntPtr window, int index, int value);

    [DllImport("user32.dll")]
    private static extern bool ChangeWindowMessageFilterEx(IntPtr window, uint message, uint action, IntPtr change);

    private static void RunCandidateHost(string parent)
    {
        Process owner = null;
        if (parent != "desktop")
        {
            int pid;
            if (!int.TryParse(parent.Split('-')[0], out pid))
                return;
            try { owner = Process.GetProcessById(pid); }
            catch (ArgumentException) { return; }
        }

        AppContext.SetSwitch("Switch.System.Windows.Input.Stylus.DisableStylusAndTouchSupport", true);
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        var receiverParameters = new HwndSourceParameters("MeowpuutCandidates-" + parent)
        {
            WindowStyle = unchecked((int)0x80000000),
            Width = 1,
            Height = 1
        };
        using (owner)
        using (var receiver = new HwndSource(receiverParameters))
        {
            Window window;
            using (var layout = typeof(GlassPreview).Assembly.GetManifestResourceStream("CandidateWindow.xaml"))
                window = (Window)XamlReader.Load(layout);
            window.ShowActivated = false;
            window.ShowInTaskbar = false;
            window.Topmost = true;
            window.WindowStartupLocation = WindowStartupLocation.Manual;
            window.IsHitTestVisible = false;
            window.SizeToContent = SizeToContent.Height;
            window.SourceInitialized += (sender, args) =>
            {
                var handle = new WindowInteropHelper(window).Handle;
                const int extendedStyle = -20;
                const int transparentNoActivate = 0x08000020;
                SetWindowLong(handle, extendedStyle, GetWindowLong(handle, extendedStyle) | transparentNoActivate);
            };
            ChangeWindowMessageFilterEx(receiver.Handle, 0x004A, 1, IntPtr.Zero);
            var strip = CandidateLayout.Create(window);
            var header = (FrameworkElement)window.FindName("CompositionHeader");
            var pageChip = (Border)window.FindName("PageChip");
            var preedit = (TextBlock)window.FindName("Preedit");
            var pageIndicator = (TextBlock)window.FindName("PageIndicator");
            Geometry paw = null;
            // The plain candidate window remains visible in screen sharing.

            IntPtr foreground = IntPtr.Zero;
            receiver.AddHook(delegate(IntPtr hwnd, int message, IntPtr sender, IntPtr data, ref bool handled)
            {
                if (message == 0x0010)
                {
                    handled = true;
                    app.Dispatcher.BeginInvoke(new Action(app.Shutdown));
                    return IntPtr.Zero;
                }
                const int copyDataMessage = 0x004A;
                if (message != copyDataMessage)
                    return IntPtr.Zero;
                var packet = (CopyData)Marshal.PtrToStructure(data, typeof(CopyData));
                if (packet.Tag.ToInt64() != 0x4D50 || packet.Bytes < 2 || packet.Bytes > 8192 || packet.Bytes % 2 != 0)
                    return IntPtr.Zero;

                string text = Marshal.PtrToStringUni(packet.Text, packet.Bytes / 2 - 1);
                handled = true;
                if (text.Length == 0)
                {
                    app.Dispatcher.BeginInvoke(new Action(() =>
                    {
                        if (sender == foreground)
                            window.Hide();
                    }));
                    return new IntPtr(1);
                }

                string[] fields = text.Split('\n');
                int x, y;
                if (fields.Length < 3 || fields.Length > 10 || !int.TryParse(fields[0], out x)
                    || !int.TryParse(fields[1], out y) || sender != GetForegroundWindow())
                    return IntPtr.Zero;

                app.Dispatcher.BeginInvoke(new Action(() =>
                {
                    if (sender != GetForegroundWindow())
                        return;
                    foreground = sender;
                    string[] state = fields[2].Split('\t');
                    int highlighted;
                    if (state.Length < 2 || !int.TryParse(state[1], out highlighted))
                        highlighted = 0;
                    preedit.Text = state[0];
                    header.Visibility = Visibility.Visible;
                    pageIndicator.Text = state.Length > 2 ? state[2] : "";
                    bool conjugating = state.Length > 3 && state[3] == "2";
                    bool expanded = conjugating || (state.Length > 3 && state[3] == "1");
                    int japaneseCount = 0;
                    for (int i = 3; i < fields.Length; i++)
                        if (IsJapanese(fields[i]) && !fields[i].EndsWith("\tja-direct", StringComparison.Ordinal)) japaneseCount++;
                    if (!conjugating && (expanded || japaneseCount > 1))
                        pageIndicator.Text += (pageIndicator.Text.Length > 0 ? "   ·   " : "") + (expanded ? "F4 收起" : "F4 展开");
                    pageChip.Visibility = pageIndicator.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
                    var work = CandidateLayout.Prepare(window, strip, x, y);
                    UpdateCandidates(strip, fields, highlighted, -1, expanded, paw);

                    CandidateLayout.Show(window, work, x, y);
                }));
                return new IntPtr(1);
            });
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
            timer.Tick += (sender, args) =>
            {
                if (owner != null && owner.HasExited)
                {
                    timer.Stop();
                    app.Shutdown();
                }
                else if (window.IsVisible && GetForegroundWindow() != foreground)
                    window.Hide();
            };
            timer.Start();
            app.Run();
        }
    }

    private static void UpdateCandidates(Panel strip, string[] fields, int highlighted,
                                         int mainIndex, bool expanded, Geometry paw)
    {
        CandidateView.Render(strip, fields, highlighted, mainIndex, expanded, CandidateLayout.FontSize);
    }

    private static bool IsJapanese(string candidate)
    {
        return candidate.EndsWith("\tja", StringComparison.Ordinal) || candidate.EndsWith("\tja-direct", StringComparison.Ordinal);
    }

    private static int MainCandidate(string[] fields, int highlighted)
    {
        int count = fields.Length - 3;
        if (highlighted >= 0 && highlighted < count)
        {
            string[] selected = fields[highlighted + 3].Split('	');
            if (!IsJapanese(fields[highlighted + 3]) && selected[0].Length > 10 &&
                (selected.Length <= 1 || selected[1].Length == 0))
                return highlighted;
        }
        if (count > 0 && !IsJapanese(fields[3]) && fields[3].Split('	')[0].Length > 10)
            return 0;
        return -1;
    }

    private static void SetSentence(TextBlock label, string text, string reading)
    {
        label.Inlines.Clear();
        int tail = text.Length;
        while (tail > 0 && text[tail - 1] < 128)
            tail--;
        string raw = text.Substring(tail);
        if (raw.Length == 0 || !reading.EndsWith(raw, StringComparison.Ordinal))
        {
            label.Inlines.Add(new Run(text));
            return;
        }
        label.Inlines.Add(new Run(text.Substring(0, tail)));
        var unconverted = new Run(raw) { FontStyle = FontStyles.Italic };
        if (!SystemParameters.HighContrast)
            unconverted.Foreground = Brushes.BurlyWood;
        label.Inlines.Add(unconverted);
    }

    private static Window CreateWindow()
    {
        Window window;
        using (var layout = typeof(GlassPreview).Assembly.GetManifestResourceStream("GlassPreview.xaml"))
            window = (Window)XamlReader.Load(layout);

        var strip = (StackPanel)window.FindName("Candidates");
        string[] samples = { "我", "握", "窩", "臥", "斡", "を" };
        int selected = 0;
        bool highContrast = SystemParameters.HighContrast;
        var rose = new LinearGradientBrush(Color.FromRgb(106, 75, 67), Color.FromRgb(126, 95, 87), 15);
        Action<int> select = index =>
        {
            var previous = (Button)strip.Children[selected];
            previous.Background = Brushes.Transparent;
            if (highContrast)
                previous.Foreground = SystemColors.WindowTextBrush;
            selected = index;
            var current = (Button)strip.Children[selected];
            current.Background = highContrast ? (Brush)SystemColors.HighlightBrush : rose;
            if (highContrast)
                current.Foreground = SystemColors.HighlightTextBrush;
        };

        for (int i = 0; i < samples.Length; i++)
        {
            int index = i;
            int number = i < 5 ? i + 1 : 7;
            var content = new StackPanel { Orientation = Orientation.Horizontal };
            content.Children.Add(new TextBlock
            {
                Text = number.ToString(),
                FontSize = 20,
                Opacity = 0.62,
                Margin = new Thickness(0, 0, 18, 0),
                VerticalAlignment = VerticalAlignment.Center
            });
            content.Children.Add(new TextBlock { Text = samples[i], FontSize = 27 });
            var button = new Button { Content = content };
            AutomationProperties.SetName(button, number + " " + samples[i]);
            button.Click += (sender, args) => select(index);
            strip.Children.Add(button);
        }
        if (highContrast)
        {
            ((System.Windows.Shapes.Rectangle)window.FindName("GlassSurface")).Fill = SystemColors.WindowBrush;
            foreach (Button button in strip.Children)
                button.Foreground = SystemColors.WindowTextBrush;
        }
        if (!highContrast)
            new GlassBackdrop(window);
        select(0);

        window.PreviewKeyDown += (sender, args) =>
        {
            Key key = args.Key;
            if (key == Key.Escape)
                window.Close();
            else if (key >= Key.D1 && key <= Key.D5)
                select(key - Key.D1);
            else if (key == Key.D7)
                select(samples.Length - 1);
            else if (key == Key.Left || key == Key.Right)
                select((selected + samples.Length + (key == Key.Left ? -1 : 1)) % samples.Length);
            else
                return;

            args.Handled = true;
        };
        window.MouseLeftButtonDown += (sender, args) => window.DragMove();
        return window;
    }
}
