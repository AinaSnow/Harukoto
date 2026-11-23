using System;

internal static class CandidatePayloadTests
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception("CandidatePayload test failed: " + message);
    }

    private static void Main()
    {
        // Tag and version rules (§3.2).
        Check(CandidatePayload.MinVersion == 1 && CandidatePayload.MaxVersion == 1, "expected v1 bounds");
        Check(CandidatePayload.VersionFromTag(0x4D50) == 1, "v1 tag");
        Check(CandidatePayload.VersionFromTag(0x4D500002) == 2, "v2 tag decoding");
        Check(CandidatePayload.VersionFromTag(0x4D50FFFF) == 65535, "max tag decoding");
        Check(CandidatePayload.VersionFromTag(0x4D500001) == 0, "v1 must only use 0x4D50");
        Check(CandidatePayload.VersionFromTag(0x4D500000) == 0, "zero version invalid");
        Check(CandidatePayload.VersionFromTag(0x4D510000) == 0, "overflow tag invalid");
        Check(CandidatePayload.VersionFromTag(0x1234) == 0, "unknown tag invalid");
        Check(CandidatePayload.SupportsTag(0x4D50), "v1 supported");
        Check(!CandidatePayload.SupportsTag(0x4D500002), "v2 not yet supported");
        Check(!CandidatePayload.SupportsTag(0), "zero tag rejected");

        Check(CandidatePayload.ValidPacket(0x4D50, 2), "empty NUL string packet valid");
        Check(CandidatePayload.ValidPacket(0x4D50, 8192), "8192-byte packet valid");
        Check(!CandidatePayload.ValidPacket(0x4D50, 0), "0 bytes rejected");
        Check(!CandidatePayload.ValidPacket(0x4D50, 1), "odd 1 byte rejected");
        Check(!CandidatePayload.ValidPacket(0x4D50, 3), "odd 3 bytes rejected");
        Check(!CandidatePayload.ValidPacket(0x4D50, 8194), "oversize packet rejected");
        Check(!CandidatePayload.ValidPacket(0x4D500002, 2), "unknown version packet rejected");

        // 1. Empty payload -> hide.
        CandidatePayload p;
        Check(CandidatePayload.TryParse(0x4D50, "", out p), "empty payload accepted");
        Check(p.Version == 1 && p.Hide && p.Candidates.Length == 0 && p.RenderFields.Length == 0, "hide payload fields");

        // 2. Mode hint (3 lines, no candidates).
        Check(CandidatePayload.TryParse(0x4D50, "-120\n480\n中\t0\t中文 · 中文标点\t0", out p), "mode hint accepted");
        Check(!p.Hide && p.X == -120 && p.Y == 480 && p.Preedit == "中" && p.Highlighted == 0, "mode hint header");
        Check(p.Status == "中文 · 中文标点" && p.PageIndicator == "中文 · 中文标点" && p.Mode == 0 && !p.Expanded && !p.Conjugating, "mode hint status");
        Check(p.Candidates.Length == 0 && p.RenderFields.Length == 3, "mode hint candidate count");

        // 3. Standard candidates with collapsible Japanese (>= 2 triggers F4 展开) and direct Japanese (excluded).
        string standard = "100\n200\nriben\t1\t简  ·  1 / 2+\t0\n"
            + "日本\t\n"
            + "日本\tにほん · 拼\tja-direct\n"
            + "にほん\t日本 · 假名\tja-direct\n"
            + "日本語\tにほんご · 拼\tja\n"
            + "日本海\tにほんかい · 拼\tja";
        Check(CandidatePayload.TryParse(0x4D50, standard, out p), "standard candidates accepted");
        Check(p.X == 100 && p.Y == 200 && p.Preedit == "riben" && p.Highlighted == 1 && !p.Expanded && !p.Conjugating, "standard header");
        Check(p.PageIndicator == "简  ·  1 / 2+   ·   F4 展开", "F4 expand hint appended: " + p.PageIndicator);
        Check(p.Candidates.Length == 5, "5 candidates");
        Check(p.Candidates[0].Text == "日本" && p.Candidates[0].Reading == "" && !p.Candidates[0].Japanese && !p.Candidates[0].DirectJapanese, "cn candidate");
        Check(p.Candidates[1].DirectJapanese && p.Candidates[1].Japanese && p.Candidates[1].Reading == "にほん · 拼", "ja-direct candidate");
        Check(p.Candidates[3].Japanese && !p.Candidates[3].DirectJapanese, "ja candidate");

        // Direct Japanese alone does not trigger F4 expand.
        string directOnly = "10\n20\nriben\t0\t简\t0\n日本\tにほん · 拼\tja-direct\nにほん\t日本 · 假名\tja-direct\n日语\t";
        Check(CandidatePayload.TryParse(0x4D50, directOnly, out p), "direct-only accepted");
        Check(p.PageIndicator == "简", "ja-direct does not trigger F4 hint: " + p.PageIndicator);

        // Expanded mode (mode=1) shows F4 收起 even without status text.
        string expandedNoStatus = "10\n20\nchi\t2\t\t1\n吃\t\n食べる\tたべる\tja";
        Check(CandidatePayload.TryParse(0x4D50, expandedNoStatus, out p), "expanded mode accepted");
        Check(p.Mode == 1 && p.Expanded && !p.Conjugating && p.PageIndicator == "F4 收起", "F4 collapse hint without separator");

        // 4. Conjugation panel (mode=2): Expanded=true, Conjugating=true, no F4 hint appended.
        string conjugation = "50\n60\nchi\t1\t简  活用 · F6/Esc 返回  ·  1 / 2\t2\n"
            + "食べる\tたべる · 辞书形\tja\n"
            + "食べます\tたべます · ます形\tja\n"
            + "食べて\tたべて · て形\tja";
        Check(CandidatePayload.TryParse(0x4D50, conjugation, out p), "conjugation panel accepted");
        Check(p.Mode == 2 && p.Conjugating && p.Expanded, "conjugation flags");
        Check(p.PageIndicator == "简  活用 · F6/Esc 返回  ·  1 / 2", "conjugation must not append F4 hint");

        // 5. Missing optional tab fields use defaults.
        Check(CandidatePayload.TryParse(0x4D50, "0\n0\nnihao\n你好", out p), "minimal header and candidate accepted");
        Check(p.Preedit == "nihao" && p.Highlighted == 0 && p.Status == "" && p.Mode == 0 && p.PageIndicator == "", "header defaults");
        Check(p.Candidates.Length == 1 && p.Candidates[0].Text == "你好" && p.Candidates[0].Reading == "" && !p.Candidates[0].Japanese, "candidate defaults");
        Check(CandidatePayload.TryParse(0x4D50, "0\n0\nnihao\tbadint\t简\t9\n你好", out p), "non-int highlighted and unknown mode default");
        Check(p.Highlighted == 0 && p.Mode == 0 && !p.Expanded, "default fallback values");

        // 6. Forward-compatible extra tab fields are ignored (§3.3 rule 2).
        string withExtras = "12\n34\nchi\t1\t简\t0\tfuture-header-1\tfuture-header-2\n"
            + "吃\t\t\tfuture-cn-1\n"
            + "食べる\tたべる · 拼\tja\tfuture-ja-1\tfuture-ja-2\n"
            + "食べます\tたべます · 拼\tja\textra\n"
            + "日本\tにほん · 拼\tja-direct\tscore=99";
        Check(CandidatePayload.TryParse(0x4D50, withExtras, out p), "extra trailing tab fields accepted");
        Check(p.Highlighted == 1 && p.Status == "简" && p.Mode == 0, "header ignores trailing fields");
        Check(p.PageIndicator == "简   ·   F4 展开", "collapsible ja recognized despite trailing fields");
        Check(p.Candidates[0].Text == "吃" && !p.Candidates[0].Japanese, "cn ignores trailing fields");
        Check(p.Candidates[1].Japanese && !p.Candidates[1].DirectJapanese, "ja recognized with trailing fields");
        Check(p.Candidates[3].DirectJapanese && p.Candidates[3].Japanese, "ja-direct recognized with trailing fields");
        Check(p.RenderFields[3] == "吃" && p.RenderFields[4] == "食べる\tたべる · 拼\tja"
            && p.RenderFields[6] == "日本\tにほん · 拼\tja-direct", "RenderFields strips unknown trailing fields");

        // 7. Unknown tags, malformed coordinates, line counts and size bounds rejected.
        Check(!CandidatePayload.TryParse(0x4D500002, "", out p), "unknown tag rejected on empty");
        Check(!CandidatePayload.TryParse(0x4D500002, standard, out p), "unknown tag rejected on non-empty");
        Check(!CandidatePayload.TryParse(0x9999, standard, out p), "arbitrary tag rejected");
        Check(!CandidatePayload.TryParse(0x4D50, null, out p), "null text rejected");
        Check(!CandidatePayload.TryParse(0x4D50, "10\n20", out p), "too few lines rejected");
        Check(!CandidatePayload.TryParse(0x4D50, "x\n20\na", out p), "bad X rejected");
        Check(!CandidatePayload.TryParse(0x4D50, "10\ny\na", out p), "bad Y rejected");
        string seven = "0\n0\na\n1\n2\n3\n4\n5\n6\n7";
        Check(CandidatePayload.TryParse(0x4D50, seven, out p) && p.Candidates.Length == 7, "7 candidates (10 lines) accepted");
        Check(!CandidatePayload.TryParse(0x4D50, seven + "\n8", out p), "8 candidates (11 lines) rejected");
        string maxAllowed = "0\n0\n" + new string('a', 4091); // length 4095 -> (4095+1)*2 = 8192 bytes
        Check(CandidatePayload.TryParse(0x4D50, maxAllowed, out p), "exact 8192-byte payload accepted");
        Check(!CandidatePayload.TryParse(0x4D50, maxAllowed + "a", out p), "8194-byte payload rejected");

        Console.WriteLine("PASS: candidate payload v1 specification, defaults, trailing fields, tags and bounds.");
    }
}
