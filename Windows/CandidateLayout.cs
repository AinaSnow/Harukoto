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
    private static double width = 860;
    internal static LayoutOptions Current = new LayoutOptions();
    internal static double ClampFont(double value) { return double.IsNaN(value) || double.IsInfinity(value) ? 18 : Math.Max(14, Math.Min(32, value)); }
    internal static double ClampWidth(double value) { return double.IsNaN(value) || double.IsInfinity(value) ? 860 : Math.Max(480, Math.Min(1000, value)); }
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
        var source = HwndSource.FromHwnd(new WindowInteropHelper(window).Handle);
        var transform = source == null ? Matrix.Identity : source.CompositionTarget.TransformFromDevice;
        var work = SystemParameters.WorkArea;
        var info = new MonitorInfo { Size = Marshal.SizeOf(typeof(MonitorInfo)) };
        if (GetMonitorInfo(MonitorFromPoint(new PointI { X = x, Y = y }, 2), ref info))
            work = DeviceWorkArea(new Rect(info.Work.Left, info.Work.Top, info.Work.Right - info.Work.Left, info.Work.Bottom - info.Work.Top), transform);
        // Retain a readable logical canvas even on an unusually narrow work area;
        // FitToWorkArea scales the entire result, including the header.
        // Only compute the maximum here: resizing the visible window to the maximum and
        // back on every key would flash. FitToWorkArea sets the final width once.
        double maximum = Math.Max(480, Math.Min(width, work.Width));
        ((FrameworkElement)window.Content).LayoutTransform = Transform.Identity;
        Apply((FrameworkElement)window.Content, panel, Current, maximum);
        return work;
    }

    internal static void Apply(FrameworkElement root, Panel panel, LayoutOptions options, double width)
    {
        ((Border)root).Padding = new Thickness(options.Outer);
        // The configured width is a maximum; CandidateView.Render narrows Width to the row.
        panel.MaxWidth = Math.Max(100, width - 2 * options.Outer - 2);
        panel.Width = panel.MaxWidth;
    }

    internal static void Show(Window window, Rect work, int x, int y)
    {
        var source = HwndSource.FromHwnd(new WindowInteropHelper(window).Handle);
        var transform = source == null ? Matrix.Identity : source.CompositionTarget.TransformFromDevice;
        var point = transform.Transform(new Point(x, y));
        var content = (FrameworkElement)window.Content;
        FitToWorkArea(window, work.Size);
        // The window includes the shadow margin; keep the visible surface at the anchor.
        point.Offset(-content.Margin.Left, -content.Margin.Top);
        var location = Place(work, point, new Size(window.Width, content.DesiredSize.Height));
        if (window.Left != location.X) window.Left = location.X;
        if (window.Top != location.Y) window.Top = location.Y;
        window.Show();
    }

    internal static Rect DeviceWorkArea(Rect pixels, Matrix fromDevice)
    {
        return new Rect(fromDevice.Transform(pixels.TopLeft), fromDevice.Transform(pixels.BottomRight));
    }

    internal static Point Place(Rect work, Point anchor, Size content)
    {
        return new Point(Math.Max(work.Left, Math.Min(anchor.X, work.Right - content.Width)),
                         Math.Max(work.Top, Math.Min(anchor.Y, work.Bottom - content.Height)));
    }

    // Keep all seven choices visible on a short work area. This only scales the
    // current presentation; the user's stored font/width/layout stays untouched.
    internal static void FitHeight(Window window, double availableHeight)
    {
        FitToWorkArea(window, new Size(double.PositiveInfinity, availableHeight));
    }

    internal static void FitToWorkArea(Window window, Size available)
    {
        var content = (FrameworkElement)window.Content;
        var panel = (Panel)window.FindName("Candidates");
        var border = (Border)content;
        // Derive the unscaled canvas anew. Repeated fitting must not shrink it again.
        // LayoutTransform scales the surface but not its shadow margin.
        double marginX = border.Margin.Left + border.Margin.Right, marginY = border.Margin.Top + border.Margin.Bottom;
        double surfaceWidth = panel.Width + border.Padding.Left + border.Padding.Right + border.BorderThickness.Left + border.BorderThickness.Right;
        double logicalWidth = surfaceWidth + marginX;
        content.LayoutTransform = Transform.Identity;
        content.Measure(new Size(logicalWidth, double.PositiveInfinity));
        double surfaceHeight = Math.Max(1, content.DesiredSize.Height - marginY);
        double factor = Math.Min(1, Math.Min(Math.Max(1, available.Width - 1 - marginX) / surfaceWidth, Math.Max(1, available.Height - 1 - marginY) / surfaceHeight));
        double finalWidth = logicalWidth;
        if (factor < 1) {
            content.LayoutTransform = new ScaleTransform(factor, factor);
            finalWidth = surfaceWidth * factor + marginX;
            content.Measure(new Size(finalWidth, double.PositiveInfinity));
        }
        // One width change per update; skip it when nothing changed.
        if (window.Width != finalWidth) window.Width = finalWidth;
    }
}
