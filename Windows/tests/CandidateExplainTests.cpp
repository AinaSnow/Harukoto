// DICT-02a: candidate source and conflict diagnostics on synthetic data.
// Every user file is checked byte for byte, with its write time, after the
// explainer and the shipped executable have read it.
#include "../CandidateExplain.h"
#include <cassert>
#include <iostream>

namespace fs = std::filesystem;

static void put(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << bytes;
}
static bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
static void expect(bool ok, const std::string &what, const std::string &output) {
    if (ok) return;
    std::cerr << "FAILED: " << what << "\n----\n" << output << "\n----\n";
    std::exit(1);
}
// The block of one numbered candidate: its header line and the indented
// detail lines under it.
static std::string block(const std::string &report, const std::string &header) {
    const auto start = report.find(header);
    if (start == std::string::npos) return {};
    size_t end = start;
    for (;;) {
        end = report.find('\n', end);
        if (end == std::string::npos || end + 2 >= report.size() || report[end + 1] != ' ' || report[end + 2] != ' ') break;
        ++end;
    }
    return report.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

struct State {
    uintmax_t size; fs::file_time_type time; std::string bytes;
    bool operator==(const State &other) const { return size == other.size && time == other.time && bytes == other.bytes; }
};
static std::map<fs::path, State> snapshot(const fs::path &root) {
    std::map<fs::path, State> result;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) { result[entry.path()] = {0, {}, "<dir>"}; continue; }
        std::ifstream file(entry.path(), std::ios::binary);
        result[entry.path()] = {entry.file_size(), entry.last_write_time(), std::string(std::istreambuf_iterator<char>(file), {})};
    }
    return result;
}

static void verifyCoreReadOnly() {
    const std::string data = "MEOWPUUT-PINYIN-2\n"
        "gong\tgong1\t公\t公\t8000\t8000\n"
        "gongsi\tgong1si1\t公司\t公司\t9000\t9000\n"
        "si\tsi1\t司\t司\t8000\t8000\n";
    auto *lexicon = mpn_lexicon_create(data.data(), data.size());
    auto *session = mpn_session_create(lexicon);
    assert(lexicon && session);
    mpn_set_simplified(session, 1);
    for (const char *c = "gongsi"; *c; ++c) assert(mpn_key(session, *c));
    std::vector<std::string> before;
    for (size_t i = 0; i < mpn_candidate_count(session); ++i) before.push_back(mpn_candidate(session, i));
    const std::string first = mpn_candidate_explain(session, 0);
    assert(first.rfind("gong1si1\t公司\tlexicon\t9000\t0.0000\t", 0) == 0);
    assert(std::string(mpn_explain_text(session, "公司")).rfind("lexicon\tgong1si1\t公司\t公司\t9000\t9000\t0.00\t0\n", 0) == 0);
    assert(std::string(mpn_explain_text(session, "没有")).empty());
    assert(std::string(mpn_candidate_explain(session, 9999)).empty());
    std::vector<std::string> after;
    for (size_t i = 0; i < mpn_candidate_count(session); ++i) after.push_back(mpn_candidate(session, i));
    assert(before == after);
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

int main(int argc, char **argv) {
    if (argc != 3) { std::cerr << "usage: candidate_explain_test BUILD_DIR EXPLAIN_EXE\n"; return 2; }
    verifyCoreReadOnly();
    const auto root = fs::u8path(argv[1]) / "candidate-explain-test";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const auto program = root / "program", local = root / "local";
    put(program / "native-pinyin.tsv", "MEOWPUUT-PINYIN-2\n"
        "gong\tgong1\t公\t公\t8000\t8000\n"
        "gongsi\tgong1si1\t公司\t公司\t9000\t9000\n"
        "gongsi\tgong1si1\t攻絲\t攻丝\t5000\t5000\n"
        "gongsi\tgong1si1\t公私\t公私\t4000\t4000\n"
        "san\tsan1\t三\t三\t8000\t8000\n"
        "shan\tshan1\t山\t山\t8000\t8000\n"
        "shan\tshan1\t珊\t珊\t5000\t5000\n"
        "si\tsi1\t司\t司\t8000\t8000\n"
        "si\tsi1\t絲\t丝\t7000\t7000\n"
        "zhang\tzhang1\t張\t张\t8000\t8000\n"
        "zhangsan\tzhang1san1\t張三\t张三\t6000\t6000\n");
    // The base Japanese table defines 会社 for gongsi, as the theme does too.
    put(program / "japanese.tsv", "gongsi|会社|4000|かいしゃ|pinyin|\n");
    put(local / "japanese-custom.tsv", "zhangsan|ジャンサン|6000||english|\n");
    const std::string generation = "0123456789abcdef0123456789abcdef";
    put(local / "Dictionaries/CURRENT", generation);
    put(local / ("Dictionaries/" + generation + "/enabled.tsv"),
        "gongsi|会社|5000|かいしゃ|pinyin|\n"
        "zhangsan|チャンサン|5000||english|张三丰\n");
    put(local / ("Dictionaries/" + generation + "/state.json"), R"({
  "format": 1,
  "packages": {
    "offpack": {"current": {"entries": [["gongsi", "攻撃", 5000, "こうげき", "pinyin", "公司"]],
                 "manifest": {"id": "offpack", "name": "Off 主题", "version": "1.0.0"}},
                "enabled": false, "previous": null},
    "testpack": {"current": {"entries": [["gongsi", "会社", 5000, "かいしゃ", "pinyin", ""],
                                         ["zhangsan", "チャンサン", 5000, "", "english", "张三丰"]],
                 "manifest": {"id": "testpack", "name": "Test", "version": "0.1.0"}},
                 "enabled": true, "previous": {"entries": [], "manifest": {}}}
  }
})");
    const auto hour = std::to_string(selection_memory::currentHour());
    // CN-06a: 攻丝 chosen three times for gongsi, 功思 once (not a lexicon candidate).
    put(local / "selection-memory.tsv", "3\ts\tgongsi\t攻丝\n1\ts\tgongsi\t功思\n");
    // CN-06c: the word 丝 for si, weight 3 (bonus log1p(3) = 1.39).
    put(local / "selection-memory-words.tsv", "=300\ts\tsi\t丝\t" + hour + "\n");
    // DICT-02b: a name, a duplicate of a lexicon word, and a reading that does not split.
    put(local / "selection-memory-user.tsv", "zhang'shan\t张珊\ngongsi\t公司\nxqz\t某\n");
    const auto before = snapshot(root);

    candidate_explain::Options options;
    options.program = program; options.local = local;
    options.simplified = true; options.fuzzy = 0; options.typo = true; options.memory = true;
    options.limit = 0;
    {
        candidate_explain::Explainer explainer(options);
        const auto sources = explainer.sources();
        expect(has(sources, "testpack「Test」0.1.0：启用，2 条") && has(sources, "offpack「Off 主题」1.0.0：未启用，1 条"), "theme packages", sources);
        expect(has(sources, "选词记忆（CN-06a/b）：2 条") && has(sources, "个人词（DICT-02b）：3 个") && has(sources, "日语自定义词表 japanese-custom.tsv：1 条"), "personal data", sources);

        const auto report = explainer.explain("gongsi");
        // Selection memory leads with its two texts, the inserted one marked.
        expect(has(report, " 1. 攻丝\n") && has(report, " 2. 功思\n"), "memory order", report);
        expect(has(block(report, " 1. 攻丝"), "选词记忆：选过 3 次") && has(block(report, " 1. 攻丝"), "选词记忆把它从第 2 项提到第 1 项"), "memory moved", block(report, " 1. 攻丝"));
        expect(has(block(report, " 2. 功思"), "选词记忆把它作为整串候选插入") && has(block(report, " 2. 功思"), "无个人数据时不在列表中"), "memory inserted", block(report, " 2. 功思"));
        // The user's duplicate of 公司: source, components and the conflict.
        const auto company = block(report, ". 公司\n");
        expect(has(company, "基础词库·整词（精确读音）") && has(company, "个人词（DICT-02b）") &&
               has(company, "组成：gong1si1 公司（词库+个人词）权重 9000 · 先验 +0.00 · 基础 -6.14 · 个人词加分 +3.00"), "user duplicate source", company);
        expect(has(report, "个人词「公司」（gong1si1）与基础词库同读音同文字重复（词库权重 9000）"), "user duplicate conflict", report);
        // The learned word 丝 in a composed path, and the score adding up.
        const auto silk = block(report, ". 公丝\n");
        expect(has(silk, "组句") && has(silk, "si1 丝（词库）权重 7000") && has(silk, "个人词频加分 +1.39"), "learned word", silk);
        const auto total = silk.find("合计 "), generated = silk.find("（生成分 ");
        expect(total != std::string::npos && generated != std::string::npos &&
               std::abs(std::atof(silk.c_str() + total + 7) - std::atof(silk.c_str() + generated + 13)) < 0.011,
               "path score adds up", silk);
        // 会社 is defined twice (base Japanese table and the theme package) and
        // ranks ahead of the whole base word 公私.
        const auto kaisha = block(report, ". 会社");
        expect(has(kaisha, "基础日语词表：拼音触发 gongsi → 会社，权重 4000") && has(kaisha, "主题包 testpack：拼音触发 gongsi → 会社，权重 5000"), "japanese origins", kaisha);
        expect(has(report, "「会社」（第 ") && has(report, " 处定义："), "duplicate definition conflict", report);
        expect(has(report, "主题／自定义触发「会社」") && has(report, "之前"), "theme shadows base word", report);
        expect(has(report, "未启用的主题 offpack「Off 主题」也有触发词 gongsi：攻撃（中文 公司）（启用后会出现）"), "disabled theme", report);
        expect(has(report, "选词记忆记录「功思」不在词库候选中"), "memory note", report);

        const auto name = explainer.explain("zhangshan");
        expect(has(name, " 1. 张珊\n") && has(block(name, " 1. 张珊"), "个人词（DICT-02b）") && has(block(name, " 1. 张珊"), "（个人词）权重 3000"), "user word", name);
        expect(has(block(name, " 1. 张珊"), "无个人数据时第 "), "user word baseline", name);

        const auto theme = explainer.explain("zhangsan");
        expect(has(theme, ". 张三丰") && has(block(theme, ". 张三丰"), "主题中文名：主题包 testpack（チャンサン）"), "theme chinese name", theme);
        expect(has(block(theme, ". ジャンサン"), "日语自定义词表：英文触发 zhangsan → ジャンサン"), "custom origin", theme);

        const auto lookup = explainer.lookup("公司");
        expect(has(lookup, "基础词库：gong1si1  公司／公司  权重 繁 9000／简 9000") && has(lookup, "个人词表 selection-memory-user.tsv：gongsi  公司") &&
               has(lookup, "主题 offpack（未启用）：触发 gongsi（拼音触发）→ 攻撃  中文 公司"), "reverse lookup", lookup);
        const auto silkLookup = explainer.lookup("丝");
        expect(has(silkLookup, "个人词频：si（简体）加分 +1.39"), "reverse lookup learned", silkLookup);
        const auto chosen = explainer.lookup("攻丝");
        expect(has(chosen, "选词记忆：gongsi（简体）选过 3 次"), "reverse lookup memory", chosen);
        expect(has(explainer.lookup("没有的词"), "没有找到"), "reverse lookup miss", "");

        const auto audit = explainer.audit();
        expect(has(audit, "个人词「某」（xqz）的读音拆不成本词库的音节") && has(audit, "个人词「公司」（gongsi）与基础词库重复") &&
               has(audit, "触发词 zhangsan：日语自定义词表「ジャンサン」与主题「チャンサン」同时匹配"), "audit", audit);
    }
    {
        // Selection memory off: no learned bonuses or reordering, user words stay.
        auto off = options; off.memory = false;
        candidate_explain::Explainer explainer(off);
        const auto report = explainer.explain("gongsi");
        expect(!has(report, " 1. 攻丝\n") && !has(report, "功思") && has(report, "选词记忆开关已关闭"), "memory off", report);
        expect(!has(report, "个人词频加分") && has(report, "个人词（DICT-02b）"), "memory off keeps user words", report);
    }
    expect(snapshot(root) == before, "explainer left every file unchanged", "");

    // The shipped executable, end to end, writing its report outside the watched tree.
    const auto output = fs::u8path(argv[1]) / "candidate-explain-output.txt";
    const std::wstring command = L"\"\"" + fs::u8path(argv[2]).wstring() + L"\" --data=\"" + program.wstring() + L"\" --local=\"" + local.wstring() +
        L"\" --hans --fuzzy=0 --typo=1 --memory=1 gongsi --text=" + fs::u8path("公司").wstring() + L" --audit > \"" + output.wstring() + L"\"\"";
    expect(_wsystem(command.c_str()) == 0, "executable exit code", "");
    std::ifstream file(output, std::ios::binary);
    const std::string printed(std::istreambuf_iterator<char>(file), {});
    expect(has(printed, "【候选】gongsi") && has(printed, "【反查】公司") && has(printed, "【检查】") && has(printed, "冲突："), "executable report", printed);
    expect(snapshot(root) == before, "executable left every file unchanged", "");
    fs::remove_all(root, ignored);
    std::cout << "PASS: candidate explain sources, components, personal effects, conflicts, reverse lookup, audit, read-only\n";
    return 0;
}
