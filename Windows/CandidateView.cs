using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;

// Shared by the live host and isolated settings preview. No registry or IPC.
internal static class CandidateView
{
    internal static void Render(Panel strip, string[] fields, int highlighted,
                                         int mainIndex, bool expanded, double fontSize)
    {
        fontSize = CandidateLayout.ClampFont(fontSize);
        double available = strip.Width;
        const double minimum = 64;
        var indices = new System.Collections.Generic.List<int>();
        int reservedJapanese = -1;
        for (int index = 0; index < fields.Length - 3; index++)
        {
            if (index == mainIndex) continue;
            if (!expanded && IsJapanese(fields[index + 3]) && !fields[index + 3].EndsWith("\tja-direct", StringComparison.Ordinal)) {
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


        bool contrast = SystemParameters.HighContrast;
        var rose = new SolidColorBrush(Color.FromRgb(228, 238, 252));
        rose.Freeze();
        var rim = new SolidColorBrush(Color.FromRgb(138, 176, 227));
        rim.Freeze();
        strip.Children.Clear();

        for (int i = 0; i < count; i++)
        {
            int index = indices[i];
            string[] candidate = fields[index + 3].Split('\t');
            var content = new DockPanel { Margin = new Thickness(10, 8, 10, 8) };
            content.Children.Add(new TextBlock
            {
                Text = (index + 1).ToString(),
                FontSize = 13,
                Opacity = contrast || index == highlighted ? 1 : 0.72,
                Margin = new Thickness(0, 0, 7, 0),
                VerticalAlignment = VerticalAlignment.Center
            });
            var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            if (IsJapanese(fields[index + 3]) && candidate.Length > 1 && candidate[1].Length > 0)
                words.Children.Add(new TextBlock { Text = candidate[1], FontSize = Math.Max(11, fontSize * 0.6), Opacity = contrast ? 1 : 0.85, Margin = new Thickness(0, 3, 0, 3),
                                                   TextWrapping = TextWrapping.Wrap });
            words.Children.Add(new TextBlock
            {
                Text = candidate[0],
                FontSize = fontSize,
                TextWrapping = TextWrapping.Wrap
            });
            if (!IsJapanese(fields[index + 3]) && candidate.Length > 1 && candidate[1].Length > 0)
                words.Children.Add(new TextBlock { Text = candidate[1], FontSize = Math.Max(11, fontSize * 0.6), Opacity = contrast ? 1 : 0.85, Margin = new Thickness(0, 3, 0, 3), TextWrapping = TextWrapping.Wrap });
            content.Children.Add(words);
            var button = new Button
            {
                Content = content,
                Height = double.NaN, MinHeight = 44, Width = double.NaN,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                Foreground = SystemColors.WindowTextBrush, Background = Brushes.Transparent, BorderBrush = Brushes.Transparent,
                Focusable = false, Tag = index,
                Margin = new Thickness(0, 0, 8, 8), VerticalAlignment = VerticalAlignment.Top
            };
            if (contrast)
                button.Foreground = index == highlighted ? SystemColors.HighlightTextBrush : SystemColors.WindowTextBrush;
            if (index == highlighted)
            {
                button.Background = contrast ? (Brush)SystemColors.HighlightBrush : rose;
                button.BorderBrush = contrast ? SystemColors.HighlightTextBrush : rim;
            }
            AutomationProperties.SetName(button, (index + 1) + " " + candidate[0] + (IsJapanese(fields[index + 3]) ? " 日语" : ""));
            strip.Children.Add(button);
            content.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            double naturalWidth = content.DesiredSize.Width + 4;
            preferred[i] = Math.Min(available - 8, Math.Max(minimum, naturalWidth));

            buttons[i] = button;
        }


        for (int i = 0; i < count; i++)
            buttons[i].Width = preferred[i];
    }

    private static bool IsJapanese(string candidate)
    {
        return candidate.EndsWith("\tja", StringComparison.Ordinal) || candidate.EndsWith("\tja-direct", StringComparison.Ordinal);
    }

}
