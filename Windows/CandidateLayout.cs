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
    internal static LayoutOptions Current = new LayoutOptions();
    internal static double ClampFont(double value) { return double.IsNaN(value) || double.IsInfinity(value) ? 18 : Math.Max(14, Math.Min(32, value)); }
    internal static double ClampWidth(double value) { return double.IsNaN(value) || double.IsInfinity(value) ? 672 : Math.Max(480, Math.Min(1000, value)); }
    [StructLayout(LayoutKind.Sequential)] private struct PointI { internal int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct RectI { internal int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { internal int Size; internal RectI Monitor, Work; internal uint Flags; }
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(PointI point, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    internal static WrapPanel Create(Window window)
    {
        var panel = (WrapPanel)window.FindName("Candidates");
        return panel;
    }

    internal static Rect Prepare(Window window, Panel panel, int x, int y)
    {
        Current = LayoutOptions.Load(); FontSize = Current.Font; width = Current.Width;
        new WindowInteropHelper(window).EnsureHandle();
        var source = PresentationSource.FromVisual(window) as HwndSource;
        var transform = source == null ? Matrix.Identity : source.CompositionTarget.TransformFromDevice;
        var work = SystemParameters.WorkArea;
        var info = new MonitorInfo { Size = Marshal.SizeOf(typeof(MonitorInfo)) };
        if (GetMonitorInfo(MonitorFromPoint(new PointI { X = x, Y = y }, 2), ref info))
            work = new Rect(transform.Transform(new Point(info.Work.Left, info.Work.Top)),
                            transform.Transform(new Point(info.Work.Right, info.Work.Bottom)));
        window.Width = Math.Min(width, work.Width);
        Apply((FrameworkElement)window.Content, panel, Current, window.Width);
        return work;
    }

    internal static void Apply(FrameworkElement root, Panel panel, LayoutOptions options, double width)
    {
        ((Border)root).Padding = new Thickness(options.Outer);
        panel.Width = Math.Max(100, width - 2 * options.Outer - 2);
    }

    internal static void Show(Window window, Rect work, int x, int y)
    {
        var source = PresentationSource.FromVisual(window) as HwndSource;
        var transform = source == null ? Matrix.Identity : source.CompositionTarget.TransformFromDevice;
        var point = transform.Transform(new Point(x, y));
        var content = (FrameworkElement)window.Content;
        FitHeight(window, work.Height);
        window.Left = Math.Max(work.Left, Math.Min(point.X, work.Right - window.Width));
        window.Top = Math.Max(work.Top, Math.Min(point.Y, work.Bottom - content.DesiredSize.Height));
        window.Show();
    }

    // Keep all seven choices visible on a short work area. This only scales the
    // current presentation; the user's stored font/width/layout stays untouched.
    internal static void FitHeight(Window window, double availableHeight)
    {
        var content = (FrameworkElement)window.Content;
        content.LayoutTransform = Transform.Identity;
        content.Measure(new Size(window.Width, double.PositiveInfinity));
        double height = content.DesiredSize.Height;
        if (height > availableHeight && availableHeight > 2) {
            double factor = (availableHeight - 1) / height;
            content.LayoutTransform = new ScaleTransform(factor, factor);
            window.Width *= factor;
            content.Measure(new Size(window.Width, double.PositiveInfinity));
        }
    }
}
