using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using Microsoft.Win32;

// UI-04: colours and shapes of one candidate-window look. Resolved per render
// from LayoutOptions.Theme / ColorMode, the system light/dark setting and the
// Windows accent colour. High contrast always uses system colours.
internal sealed class CandidatePalette
{
    internal bool Capsule, Dark, Contrast;
    // Idle: background of every unselected item (the capsule look draws each candidate as a pill).
    internal Brush Surface, Border, Text, Muted, Number, Idle, Highlight, HighlightText, HighlightMuted, HighlightNumber, Bar;
    internal Color Shadow;
    internal double ShadowOpacity, Corner, ItemCorner;
}

internal static class CandidateTheme
{
    internal static readonly string[] Names = { "晴空 Fluent", "晴空胶囊" };
    internal static readonly string[] ColorModes = { "跟随系统", "浅色", "深色" };
    // Room around the surface for its shadow; the surface itself sits at the caret anchor.
    internal static readonly Thickness ShadowMargin = new Thickness(12, 8, 12, 16);
    private static readonly Dictionary<double, ControlTemplate> templates = new Dictionary<double, ControlTemplate>();

    private static SolidColorBrush Brush(Color color) { var brush = new SolidColorBrush(color); brush.Freeze(); return brush; }
    private static SolidColorBrush Brush(uint argb)
    {
        return Brush(Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb));
    }
    // t = share of b.
    private static Color Mix(Color a, Color b, double t)
    {
        Func<byte, byte, byte> channel = (x, y) => (byte)Math.Round(x * (1 - t) + y * t);
        return Color.FromRgb(channel(a.R, b.R), channel(a.G, b.G), channel(a.B, b.B));
    }

    internal static bool SystemDark()
    {
        try {
            using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")) {
                var value = key == null ? null : key.GetValue("AppsUseLightTheme");
                return value is int && (int)value == 0;
            }
        } catch { return false; }
    }

    // DWM AccentColor is 0xAABBGGRR. Falls back to the Windows default blue.
    internal static Color Accent()
    {
        try {
            using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\DWM")) {
                var value = key == null ? null : key.GetValue("AccentColor");
                if (value is int) {
                    uint abgr = unchecked((uint)(int)value);
                    return Color.FromRgb((byte)abgr, (byte)(abgr >> 8), (byte)(abgr >> 16));
                }
            }
        } catch { }
        return Color.FromRgb(0x0F, 0x6C, 0xBD);
    }

    internal static CandidatePalette Resolve(int theme, int colorMode)
    {
        var p = new CandidatePalette { Capsule = theme == LayoutOptions.ThemeCapsule, Contrast = SystemParameters.HighContrast };
        p.Dark = colorMode == 2 || (colorMode == 0 && SystemDark());
        if (p.Contrast) {
            p.Surface = SystemColors.WindowBrush; p.Border = SystemColors.WindowTextBrush;
            p.Text = p.Muted = p.Number = SystemColors.WindowTextBrush;
            p.Highlight = SystemColors.HighlightBrush;
            p.HighlightText = p.HighlightMuted = p.HighlightNumber = SystemColors.HighlightTextBrush;
            p.Bar = p.Idle = Brushes.Transparent;
            return p;
        }
        if (p.Capsule) {
            // Every candidate is a faint pill; the selected one turns solid blue.
            p.Corner = 16; p.ItemCorner = 999; p.Bar = Brushes.Transparent;
            // White on #2A78D0 keeps about 4.6:1 contrast in both modes.
            p.Highlight = Brush(0xFF2A78D0); p.HighlightText = Brushes.White;
            p.HighlightMuted = p.HighlightNumber = Brush(0xFFDCEBFF);
            if (p.Dark) {
                p.Surface = Brush(0xFF26303B); p.Border = Brush(0x1FFFFFFF);
                p.Text = Brush(0xFFE8EEF5); p.Muted = Brush(0xFFA9B6C4); p.Number = Brush(0xFF7D8B99);
                p.Idle = Brush(0xFF2F3A46);
                p.Shadow = Colors.Black; p.ShadowOpacity = 0.5;
            } else {
                p.Surface = Brush(0xFFFFFFFF); p.Border = Brush(0x1A2A6FB0);
                p.Text = Brush(0xFF1C2733); p.Muted = Brush(0xFF4C5A6A); p.Number = Brush(0xFF7A8796);
                p.Idle = Brush(0xFFF0F3F7);
                p.Shadow = Color.FromRgb(0x1E, 0x3C, 0x64); p.ShadowOpacity = 0.24;
            }
            return p;
        }
        p.Corner = 8; p.ItemCorner = 4;
        var accent = Accent();
        p.Idle = Brushes.Transparent;
        if (p.Dark) {
            var surface = Color.FromRgb(0x2B, 0x2C, 0x31);
            var light = Mix(accent, Colors.White, 0.45);
            p.Surface = Brush(surface); p.Border = Brush(0x24FFFFFF);
            p.Text = Brush(0xFFF1F1F3); p.Muted = Brush(0xFFA3A6AD); p.Number = Brush(0xFF8D9098);
            p.Highlight = Brush(Mix(surface, light, 0.24)); p.Bar = p.HighlightNumber = Brush(light);
            p.HighlightText = p.Text; p.HighlightMuted = Brush(0xFFC4C7CE);
            p.Shadow = Colors.Black; p.ShadowOpacity = 0.45;
        } else {
            var surface = Color.FromRgb(0xF9, 0xF9, 0xFB);
            p.Surface = Brush(surface); p.Border = Brush(0x1F000000);
            p.Text = Brush(0xFF1B1B1F); p.Muted = Brush(0xFF5C5F66); p.Number = Brush(0xFF7E8188);
            p.Highlight = Brush(Mix(surface, accent, 0.18)); p.Bar = p.HighlightNumber = Brush(accent);
            p.HighlightText = p.Text; p.HighlightMuted = p.Muted;
            p.Shadow = Colors.Black; p.ShadowOpacity = 0.18;
        }
        return p;
    }

    // Item chrome: rounded background plus an optional left accent bar drawn
    // with the button's BorderBrush. No hover/press states (the live window is
    // click-through; the preview selects on click).
    internal static ControlTemplate ItemTemplate(double corner)
    {
        ControlTemplate template;
        if (templates.TryGetValue(corner, out template)) return template;
        var grid = new FrameworkElementFactory(typeof(Grid));
        var background = new FrameworkElementFactory(typeof(Border));
        background.SetValue(Border.CornerRadiusProperty, new CornerRadius(corner));
        background.SetValue(Border.BackgroundProperty, new TemplateBindingExtension(Control.BackgroundProperty));
        grid.AppendChild(background);
        var bar = new FrameworkElementFactory(typeof(Border));
        bar.SetValue(FrameworkElement.WidthProperty, 3.0);
        bar.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Left);
        bar.SetValue(FrameworkElement.MarginProperty, new Thickness(0, 7, 0, 7));
        bar.SetValue(Border.CornerRadiusProperty, new CornerRadius(1.5));
        bar.SetValue(Border.BackgroundProperty, new TemplateBindingExtension(Control.BorderBrushProperty));
        grid.AppendChild(bar);
        var content = new FrameworkElementFactory(typeof(ContentPresenter));
        content.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Left);
        content.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
        grid.AppendChild(content);
        template = new ControlTemplate(typeof(Button)) { VisualTree = grid };
        template.Seal();
        templates[corner] = template;
        return template;
    }

    // Styles the window surface and header around the candidate strip. Shared
    // by the live window and the settings preview, so both look identical.
    internal static FrameworkElement ApplyChrome(Panel strip, CandidatePalette p, LayoutOptions options)
    {
        var stack = strip.Parent as Panel;
        var surface = stack == null ? null : stack.Parent as Border;
        if (surface == null) return null;
        surface.Background = p.Surface; surface.BorderBrush = p.Border; surface.BorderThickness = new Thickness(1);
        surface.CornerRadius = new CornerRadius(p.Corner);
        if (p.ShadowOpacity > 0) {
            surface.Margin = ShadowMargin;
            var shadow = surface.Effect as DropShadowEffect;
            if (shadow == null || shadow.Color != p.Shadow || shadow.Opacity != p.ShadowOpacity)
                surface.Effect = new DropShadowEffect { Color = p.Shadow, Opacity = p.ShadowOpacity, BlurRadius = 18, ShadowDepth = 3, Direction = 270 };
        } else {
            surface.Margin = new Thickness(0); surface.Effect = null;
        }
        var header = LogicalTreeHelper.FindLogicalNode(surface, "CompositionHeader") as FrameworkElement;
        var preedit = LogicalTreeHelper.FindLogicalNode(surface, "Preedit") as TextBlock;
        var page = LogicalTreeHelper.FindLogicalNode(surface, "PageIndicator") as TextBlock;
        double small = Math.Max(12, Math.Round(options.Font * 0.68));
        if (header != null) header.Margin = new Thickness(options.PaddingX + (p.Capsule ? 2 : 0), 0, options.PaddingX, 4);
        if (preedit != null) { preedit.Foreground = p.Muted; preedit.FontSize = Math.Max(13, Math.Round(options.Font * 0.78)); }
        if (page != null) { page.Foreground = p.Muted; page.FontSize = small; }
        return header;
    }
}
