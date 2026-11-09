using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;

internal static class CandidateLayout
{
    internal static double FontSize = 18;
    private static double width = 672;
    [StructLayout(LayoutKind.Sequential)] private struct PointI { internal int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct RectI { internal int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { internal int Size; internal RectI Monitor, Work; internal uint Flags; }
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(PointI point, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    internal static WrapPanel Create(Window window)
    {
        var original = (StackPanel)window.FindName("Candidates");
        original.Visibility = Visibility.Collapsed;
        var panel = new WrapPanel { Resources = original.Resources };
        var body = new Border { Child = panel, Padding = new Thickness(12), Margin = new Thickness(8, 0, 8, 8),
            CornerRadius = new CornerRadius(18), BorderThickness = new Thickness(1),
            Background = SystemParameters.HighContrast ? SystemColors.WindowBrush : new SolidColorBrush(Color.FromArgb(245, 48, 42, 46)),
            BorderBrush = SystemParameters.HighContrast ? SystemColors.WindowTextBrush : new SolidColorBrush(Color.FromArgb(120, 223, 192, 180)) };
        ((StackPanel)window.Content).Children.Add(body);
        Canvas.SetTop((FrameworkElement)window.FindName("CompositionHeader"), 82);
        return panel;
    }

    internal static Rect Prepare(Window window, Panel panel, int x, int y)
    {
        FontSize = 18; width = 672;
        try {
            using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Meowpuut\CandidateWindow")) {
                if (key != null) {
                    FontSize = Math.Max(14, Math.Min(32, Convert.ToDouble(key.GetValue("FontSize", 18))));
                    width = Math.Max(480, Math.Min(1000, Convert.ToDouble(key.GetValue("Width", 672))));
                }
            }
        } catch { FontSize = 18; width = 672; }
        new WindowInteropHelper(window).EnsureHandle();
        var source = PresentationSource.FromVisual(window) as HwndSource;
        var transform = source == null ? Matrix.Identity : source.CompositionTarget.TransformFromDevice;
        var work = SystemParameters.WorkArea;
        var info = new MonitorInfo { Size = Marshal.SizeOf(typeof(MonitorInfo)) };
        if (GetMonitorInfo(MonitorFromPoint(new PointI { X = x, Y = y }, 2), ref info))
            work = new Rect(transform.Transform(new Point(info.Work.Left, info.Work.Top)),
                            transform.Transform(new Point(info.Work.Right, info.Work.Bottom)));
        window.Width = Math.Min(width, work.Width);
        panel.Width = Math.Max(100, window.Width - 42);
        return work;
    }

    internal static void Show(Window window, Rect work, int x, int y)
    {
        var source = PresentationSource.FromVisual(window) as HwndSource;
        var transform = source == null ? Matrix.Identity : source.CompositionTarget.TransformFromDevice;
        var point = transform.Transform(new Point(x, y));
        var content = (FrameworkElement)window.Content;
        content.Measure(new Size(window.Width, double.PositiveInfinity));
        window.Left = Math.Max(work.Left, Math.Min(point.X, work.Right - window.Width));
        window.Top = Math.Max(work.Top, Math.Min(point.Y, work.Bottom - content.DesiredSize.Height));
        window.Show();
    }
}
