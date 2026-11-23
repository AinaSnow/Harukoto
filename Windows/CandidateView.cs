using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;

// Shared by the live host and isolated settings preview. No registry or IPC
// beyond reading the system colour settings in CandidateTheme.
// UI-04: one compact horizontal row like the system IME. Each candidate is a
// single line (number, word, reading); the strip shrinks to its row and only
// wraps when the row is wider than the configured maximum width.
internal static class CandidateView
{
    internal static void Render(Panel strip, string[] fields, int highlighted,
                                         int mainIndex, bool expanded, LayoutOptions options)
    {
        options = options.Validated();
        var palette = CandidateTheme.Resolve(options.Theme, options.ColorMode);
        var header = CandidateTheme.ApplyChrome(strip, palette, options);
        double fontSize = options.Font;
        // Apply() publishes the maximum as MaxWidth; callers that only set Width use it as the maximum.
        double available = double.IsInfinity(strip.MaxWidth) ? strip.Width : strip.MaxWidth;
        if (double.IsNaN(available) || available <= 0) available = 640;
        var indices = new System.Collections.Generic.List<int>();
        int reservedJapanese = -1;
        for (int index = 0; index < fields.Length - 3; index++)
        {
            if (index == mainIndex) continue;
            if (!expanded && IsJapanese(fields[index + 3]) && !IsDirectJapanese(fields[index + 3])) {
                if (reservedJapanese < 0 || index == highlighted) reservedJapanese = index;
            } else {
                indices.Add(index);
            }
        }
        if (reservedJapanese >= 0)
        {
            indices.Add(reservedJapanese);
            indices.Sort();
        }
        int count = indices.Count;
        var buttons = new Button[count];
        var preferred = new double[count];
        double numberSize = Math.Max(11, Math.Round(fontSize * 0.68));
        // Pills need a little room for their rounded ends and must not touch each other.
        double paddingX = options.PaddingX + (palette.Capsule ? 2 : 0);
        double gapX = palette.Capsule ? Math.Max(4, options.GapX) : options.GapX;
        double gapY = palette.Capsule ? Math.Max(4, options.GapY) : options.GapY;
        strip.Children.Clear();

        for (int i = 0; i < count; i++)
        {
            int index = indices[i];
            bool selected = index == highlighted;
            bool japanese = IsJapanese(fields[index + 3]);
            string[] candidate = fields[index + 3].Split('\t');
            var text = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(paddingX, options.PaddingY, paddingX, options.PaddingY)
            };
            text.Inlines.Add(new Run((index + 1).ToString()) { FontSize = numberSize, Foreground = selected ? palette.HighlightNumber : palette.Number });
            text.Inlines.Add(new Run(" ") { FontSize = numberSize });
            text.Inlines.Add(new Run(candidate[0]) { FontSize = fontSize, Foreground = selected ? palette.HighlightText : palette.Text });
            if (candidate.Length > 1 && candidate[1].Length > 0)
                text.Inlines.Add(new Run(" " + candidate[1]) { FontSize = options.ReadingSize, Foreground = selected ? palette.HighlightMuted : palette.Muted });
            var button = new Button
            {
                Content = text,
                Height = double.NaN, Width = double.NaN,
                HorizontalContentAlignment = HorizontalAlignment.Left,
                Foreground = palette.Text,
                Background = selected ? palette.Highlight : palette.Idle,
                BorderBrush = selected ? palette.Bar : Brushes.Transparent,
                Focusable = false, Tag = index,
                Margin = new Thickness(0, 0, gapX, gapY), VerticalAlignment = VerticalAlignment.Center
            };
            AutomationProperties.SetName(button, (index + 1) + " " + candidate[0] + (japanese ? " 日语" : ""));
            strip.Children.Add(button);
            text.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            // WPF scales an oversized CornerRadius per axis, which turns a "pill" into an
            // ellipse. A true pill uses half of the single-line item height.
            double corner = palette.ItemCorner >= 999 ? Math.Round(text.DesiredSize.Height / 2, 1) : palette.ItemCorner;
            button.Template = CandidateTheme.ItemTemplate(corner);
            preferred[i] = Math.Min(available - gapX, Math.Ceiling(text.DesiredSize.Width) + 1);
            buttons[i] = button;
        }

        double row = 0;
        for (int i = 0; i < count; i++)
        {
            buttons[i].Width = preferred[i];
            row += preferred[i] + gapX;
        }
        // Shrink to the single row (or the header, whichever is wider); wrap only past the maximum.
        double needed = row;
        if (header != null)
        {
            header.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            needed = Math.Max(needed, header.DesiredSize.Width);
        }
        strip.Width = Math.Min(available, Math.Max(120, Math.Ceiling(needed) + 1));
    }

    private static string CandidateKind(string candidate)
    {
        string[] parts = candidate.Split('\t');
        return parts.Length > 2 ? parts[2] : "";
    }

    private static bool IsJapanese(string candidate)
    {
        string kind = CandidateKind(candidate);
        return kind == "ja" || kind == "ja-direct";
    }

    private static bool IsDirectJapanese(string candidate)
    {
        return CandidateKind(candidate) == "ja-direct";
    }

}
