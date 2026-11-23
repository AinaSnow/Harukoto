using System;
using System.Globalization;

// RH-1: versioned WM_COPYDATA payload specification and pure parser (docs/RENDERER_HANDOFF.md).
// v1 uses tag 0x4D50; vN (N >= 2) uses 0x4D500000 + N. Within a version, new
// fields are appended after tab separators so older renderers ignore them and
// missing fields take their defaults.
internal sealed class CandidatePayloadItem
{
    internal string Text { get; private set; }
    internal string Reading { get; private set; }
    internal bool Japanese { get; private set; }
    internal bool DirectJapanese { get; private set; }

    internal CandidatePayloadItem(string text, string reading, bool japanese, bool directJapanese)
    {
        Text = text;
        Reading = reading;
        Japanese = japanese;
        DirectJapanese = directJapanese;
    }

    internal string ToRenderField()
    {
        if (DirectJapanese) return Text + "\t" + Reading + "\tja-direct";
        if (Japanese) return Text + "\t" + Reading + "\tja";
        if (Reading.Length > 0) return Text + "\t" + Reading;
        return Text;
    }
}

internal sealed class CandidatePayload
{
    // Must match HarukotoRendererPayloadMin / Max in Windows/InstallSupport.ps1
    // and cover PayloadVersion in Windows/RendererRelease.h (checked by tests).
    internal const int MinVersion = 1;
    internal const int MaxVersion = 1;
    internal const long TagV1 = 0x4D50;
    internal const long TagBase = 0x4D500000;
    internal const int MaxBytes = 8192;

    internal int Version { get; private set; }
    internal bool Hide { get; private set; }
    internal int X { get; private set; }
    internal int Y { get; private set; }
    internal string Preedit { get; private set; }
    internal int Highlighted { get; private set; }
    internal string Status { get; private set; }
    internal int Mode { get; private set; }
    internal bool Conjugating { get { return Mode == 2; } }
    internal bool Expanded { get { return Mode == 1 || Mode == 2; } }
    internal string PageIndicator { get; private set; }
    internal CandidatePayloadItem[] Candidates { get; private set; }
    internal string[] RenderFields { get; private set; }

    internal static int VersionFromTag(long tag)
    {
        if (tag == TagV1) return 1;
        if (tag >= TagBase + 2 && tag <= TagBase + 65535) return (int)(tag - TagBase);
        return 0;
    }

    internal static bool SupportsTag(long tag)
    {
        int version = VersionFromTag(tag);
        return version >= MinVersion && version <= MaxVersion;
    }

    internal static bool ValidPacket(long tag, int bytes)
    {
        return SupportsTag(tag) && bytes >= 2 && bytes <= MaxBytes && (bytes % 2) == 0;
    }

    internal static bool TryParse(long tag, string text, out CandidatePayload payload)
    {
        payload = null;
        int version = VersionFromTag(tag);
        if (version < MinVersion || version > MaxVersion || text == null)
            return false;
        if ((text.Length + 1) * 2 > MaxBytes)
            return false;
        if (text.Length == 0)
        {
            payload = new CandidatePayload
            {
                Version = version,
                Hide = true,
                Preedit = "",
                Status = "",
                PageIndicator = "",
                Candidates = new CandidatePayloadItem[0],
                RenderFields = new string[0]
            };
            return true;
        }

        string[] fields = text.Split('\n');
        int x, y;
        if (fields.Length < 3 || fields.Length > 10
            || !int.TryParse(fields[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out x)
            || !int.TryParse(fields[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out y))
            return false;

        string[] state = fields[2].Split('\t');
        int highlighted;
        if (state.Length < 2 || !int.TryParse(state[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out highlighted))
            highlighted = 0;

        string preedit = state[0];
        string status = state.Length > 2 ? state[2] : "";
        int mode = 0;
        if (state.Length > 3)
        {
            if (state[3] == "2") mode = 2;
            else if (state[3] == "1") mode = 1;
        }
        bool conjugating = mode == 2;
        bool expanded = mode == 1 || mode == 2;

        int count = fields.Length - 3;
        var candidates = new CandidatePayloadItem[count];
        var renderFields = new string[fields.Length];
        renderFields[0] = fields[0];
        renderFields[1] = fields[1];
        renderFields[2] = preedit;
        int collapsibleJapanese = 0;
        for (int i = 0; i < count; i++)
        {
            string[] parts = fields[i + 3].Split('\t');
            string word = parts[0];
            string reading = parts.Length > 1 ? parts[1] : "";
            string kind = parts.Length > 2 ? parts[2] : "";
            bool directJapanese = kind == "ja-direct";
            bool japanese = directJapanese || kind == "ja";
            if (japanese && !directJapanese) collapsibleJapanese++;
            candidates[i] = new CandidatePayloadItem(word, reading, japanese, directJapanese);
            renderFields[i + 3] = candidates[i].ToRenderField();
        }

        string indicator = status;
        if (!conjugating && (expanded || collapsibleJapanese > 1))
            indicator += (indicator.Length > 0 ? "   ·   " : "") + (expanded ? "F4 收起" : "F4 展开");

        payload = new CandidatePayload
        {
            Version = version,
            Hide = false,
            X = x,
            Y = y,
            Preedit = preedit,
            Highlighted = highlighted,
            Status = status,
            Mode = mode,
            PageIndicator = indicator,
            Candidates = candidates,
            RenderFields = renderFields
        };
        return true;
    }
}
