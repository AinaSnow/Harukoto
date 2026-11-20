#include "../SelectionMemory.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>

using namespace selection_memory;
using Texts = std::vector<std::string>;

struct Choice { std::string text; std::optional<size_t> coreIndex; bool japanese = false, strong = false; std::string annotation; size_t consumed = 0; };

static std::string bytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

static void model() {
    Model memory;
    assert(memory.recall("shi", true).empty());
    // One correction replaces one mistake; a habit needs more than one slip.
    assert(memory.record("shi", "市", true) && memory.recall("shi", true) == Texts{"市"});
    assert(memory.record("shi", "是", true) && (memory.recall("shi", true) == Texts{"是", "市"}));
    for (int i = 0; i < 3; ++i) memory.record("shi", "是", true);
    memory.record("shi", "市", true);
    assert((memory.recall("shi", true) == Texts{"是", "市"}));
    // The count saturates, so a changed habit wins after a bounded number of selections.
    for (int i = 0; i < 20; ++i) memory.record("shi", "是", true);
    for (uint32_t i = 0; i < MaxCount; ++i) memory.record("shi", "市", true);
    assert(memory.recall("shi", true).front() == "市");
    // Scripts and readings are separate.
    assert(memory.recall("shi", false).empty() && memory.recall("sh", true).empty());
    memory.record("shi", "時", false);
    assert(memory.recall("shi", false) == Texts{"時"} && memory.recall("shi", true).size() == 2);
    // Rejected input never reaches the journal.
    assert(!memory.record("", "是", true) && !memory.record("shi", "", true) && !memory.record("Shi", "是", true));
    assert(!memory.record("shi", "a\tb", true) && !memory.record("shi", "a\nb", true) && !memory.record("shi 1", "是", true));
    assert(!memory.record(std::string(MaxReading + 1, 'a'), "是", true) && !memory.record("shi", std::string(MaxText + 1, 'x'), true));
    assert(memory.record("xi'an", "西安", true));
    // Each reading keeps a bounded number of alternatives; the weakest goes first.
    Model bounded;
    for (size_t i = 0; i < MaxPerReading; ++i) bounded.record("yi", "text" + std::to_string(i), true);
    bounded.record("yi", "text0", true);
    bounded.record("yi", "new", true);
    const auto kept = bounded.recall("yi", true);
    assert(kept.size() == MaxPerReading && kept.front() == "text0");
    assert(std::find(kept.begin(), kept.end(), "text1") == kept.end() && std::find(kept.begin(), kept.end(), "new") != kept.end());
    // The total is bounded as well; the oldest selections are dropped together.
    Model large;
    for (size_t i = 0; i <= MaxEntries; ++i) {
        std::string reading;
        for (size_t value = i; reading.size() < 4; value /= 26) reading += static_cast<char>('a' + value % 26);
        large.record(reading, "x", true);
    }
    assert(large.size() < MaxEntries && large.size() > MaxEntries * 8 / 10);
    assert(large.recall("aaaa", true).empty());
    // A snapshot replays to the same ranking, and broken rows are skipped singly.
    Model copy;
    const auto rows = memory.snapshot();
    assert(copy.replay(rows) == rows.size() && copy.recall("shi", true) == memory.recall("shi", true));
    assert(copy.recall("xi'an", true) == Texts{"西安"} && copy.recall("shi", false) == Texts{"時"});
    Model damaged;
    const std::string mixed = "1\ts\tni\t你\nbroken\n0\ts\tni\t尼\n1\tx\tni\t泥\n\t\t\t\n1\ts\tNI\t你\n2\tt\tni\t妳\r\n1\ts\tni\t拟";
    assert(damaged.replay(mixed) == mixed.rfind('\n') + 1); // The unterminated last row waits for its newline.
    assert(damaged.recall("ni", true) == Texts{"你"} && damaged.recall("ni", false) == Texts{"妳"} && damaged.size() == 2);
}

// CN-06c word weights: growth, cap, correction, decay, bounds and rows.
static void words() {
    constexpr int64_t hour = 480000; // Any hour since 1970.
    const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    WordModel model;
    assert(model.bonus("gongsi", "公司", true, hour) == 0 && model.size() == 0);
    // One selection: log 2 nats. Growth is logarithmic and capped.
    const auto revision = model.revision();
    assert(model.select("gongsi", "公司", true, hour) && model.revision() != revision);
    assert(same(model.bonus("gongsi", "公司", true, hour), std::log1p(1.0)));
    assert(model.select("gongsi", "公司", true, hour) && same(model.bonus("gongsi", "公司", true, hour), std::log1p(2.0)));
    for (int i = 0; i < 50; ++i) model.select("gongsi", "公司", true, hour);
    assert(same(model.weight("gongsi", "公司", true, hour), MaxWeight) && model.bonus("gongsi", "公司", true, hour) == BonusCap);
    // Decay: half after HalfLifeHours, forgotten once the weight falls below MinWeight.
    assert(same(model.weight("gongsi", "公司", true, hour + int64_t(HalfLifeHours)), MaxWeight / 2));
    assert(model.bonus("gongsi", "公司", true, hour + int64_t(HalfLifeHours) * 9) == 0);
    // Scripts and readings are separate; separators and bad text are refused.
    assert(model.bonus("gongsi", "公司", false, hour) == 0 && model.bonus("gongshi", "公司", true, hour) == 0);
    assert(!model.select("xi'an", "西安", true, hour) && !model.select("", "是", true, hour) && !model.select("shi", "a\tb", true, hour));
    assert(!model.select("Shi", "是", true, hour) && !model.select("shi", "是", true, 0));

    // A single slip fades and is undone by a single correction: the other
    // word of the reading halves while the correction gains one.
    WordModel slip;
    slip.select("shiyan", "试验", true, hour);
    assert(same(slip.bonus("shiyan", "试验", true, hour + int64_t(HalfLifeHours)), std::log1p(0.5)));
    slip.select("shiyan", "实验", true, hour + 1);
    assert(slip.bonus("shiyan", "实验", true, hour + 1) > slip.bonus("shiyan", "试验", true, hour + 1));
    // Even a habit at the cap is overturned by a few corrections.
    WordModel habit;
    for (int i = 0; i < 40; ++i) habit.select("shiyan", "试验", true, hour);
    int corrections = 0;
    while (habit.bonus("shiyan", "实验", true, hour) <= habit.bonus("shiyan", "试验", true, hour)) {
        habit.select("shiyan", "实验", true, hour); ++corrections;
    }
    assert(corrections == 3);
    // A word of another reading is not a competitor.
    habit.select("shi", "是", true, hour);
    assert(same(habit.weight("shiyan", "实验", true, hour), 3));
    // Bounded per reading and in total.
    WordModel bounded;
    for (size_t i = 0; i < MaxPerReading + 3; ++i) bounded.select("yi", "text" + std::to_string(i), true, hour);
    assert(bounded.size() <= MaxPerReading && bounded.bonus("yi", "text" + std::to_string(MaxPerReading + 2), true, hour) > 0);
    WordModel large;
    for (size_t i = 0; i <= MaxEntries; ++i) {
        std::string reading;
        for (size_t value = i; reading.size() < 4; value /= 26) reading += static_cast<char>('a' + value % 26);
        large.select(reading, "x", true, hour);
    }
    assert(large.size() < MaxEntries && large.size() > MaxEntries * 8 / 10 && large.bonus("aaaa", "x", true, hour) == 0);
    // Every bonus at an hour, for the core's table.
    size_t listed = 0;
    model.each(hour, [&](bool simplified, std::string_view reading, const std::string &text, double bonus) {
        assert(simplified && reading == "gongsi" && text == "公司" && bonus == BonusCap); ++listed;
    });
    assert(listed == 1);

    // Rows: selections replay with the same competition; a compacted
    // snapshot replays to the same weights; broken rows are skipped singly.
    const std::string events = WordModel::event("shiyan", "试验", true, hour) + WordModel::event("shiyan", "试验", true, hour) +
        WordModel::event("shiyan", "实验", true, hour) + WordModel::event("gongsi", "公司", false, hour + 2);
    assert(events.substr(0, 25) == "+\ts\tshiyan\t试验\t480000\n");
    WordModel replayed;
    assert(replayed.replay(events) == events.size());
    assert(same(replayed.weight("shiyan", "试验", true, hour), 1) && same(replayed.weight("shiyan", "实验", true, hour), 1));
    assert(replayed.bonus("gongsi", "公司", false, hour) > 0 && replayed.bonus("gongsi", "公司", true, hour) == 0);
    WordModel restored;
    const auto rows = replayed.snapshot();
    assert(rows.find("=100\ts\tshiyan\t实验\t480002\n") != rows.npos);
    assert(restored.replay(rows) == rows.size());
    for (const auto *text : {"试验", "实验"})
        assert(std::abs(restored.weight("shiyan", text, true, hour + 9) - replayed.weight("shiyan", text, true, hour + 9)) < 0.01);
    WordModel damaged;
    const std::string mixed = "+\ts\tni\t你\t480000\nbroken\n+\tx\tni\t泥\t480000\n+\ts\tni\t泥\t0\n+\ts\tni\t泥\t48x\n"
        "=0\ts\tni\t泥\t480000\n=9999\ts\tni\t泥\t480000\n+\ts\tni\t泥\t480000\textra\n1\ts\tni\t泥\n+\tt\tni\t妳\t480000\r\n+\ts\tni\t拟\t480000";
    assert(damaged.replay(mixed) == mixed.rfind('\n') + 1);
    assert(damaged.size() == 2 && damaged.bonus("ni", "你", true, hour) > 0 && damaged.bonus("ni", "妳", false, hour) > 0);
    // Neither format reads the other's rows, so an older version that met a
    // word row (or this version a phrase row) would skip it, not fail.
    Model old;
    assert(old.replay(events + rows) == (events + rows).size() && old.size() == 0);
    WordModel fresh;
    assert(fresh.replay("1\ts\tni\t你\n3\tt\tshi\t是\n") > 0 && fresh.size() == 0);
}

static void candidates() {
    const std::string input = "qingchashou";
    const auto consumed = [&](const Choice &c) { return c.coreIndex ? c.consumed : input.size(); };
    std::vector<Choice> list{{"清查收", 0, false, false, {}, 11}, {"请", 1, false, false, {}, 4}, {"请查收", 2, false, false, {}, 11},
                             {"请查收", 3, false, false, {}, 7}, {"青茶手", 4, false, false, {}, 11}};
    apply(list, input, {}, consumed);
    assert(list.front().text == "清查收");
    // An existing whole-input candidate moves, keeping its core index.
    apply(list, input, {"请查收"}, consumed);
    assert(list[0].text == "请查收" && list[0].coreIndex == 2 && list[1].text == "清查收" && list.size() == 5);
    // A shorter candidate with the same text is not the remembered selection.
    assert(list[3].text == "请查收" && list[3].consumed == 7);
    // A composed phrase that the decoder does not offer is added in front.
    apply(list, input, {"情查收", "请查收"}, consumed);
    assert(list[0].text == "情查收" && !list[0].coreIndex && list[0].strong && list[1].text == "请查收" && list.size() == 6);
    // At most three remembered selections lead the page.
    std::vector<Choice> many{{"a", 0, false, false, {}, 11}, {"b", 1, false, false, {}, 11}};
    apply(many, input, {"w", "x", "y", "z", "b"}, consumed);
    assert(many.size() == 5 && many[0].text == "w" && many[2].text == "y" && many[3].text == "a" && many[4].text == "b");
}

static void store(const std::filesystem::path &folder) {
    const auto path = folder / L"记忆" / L"selection-memory.tsv";
    {
        Store first(path);
        first.poll(1000);
        assert(first.recall("ni", true).empty());
        first.record("ni", "你", true);
        // Visible in this process at once; on disk after the composition ends.
        assert(first.recall("ni", true) == Texts{"你"} && !std::filesystem::exists(path));
        assert(first.flush() && bytes(path) == "1\ts\tni\t你\n");
        // A second process sees it, and its own rows are not counted twice.
        Store second(path);
        second.poll(1000);
        assert(second.recall("ni", true) == Texts{"你"});
        second.record("ni", "尼", true); second.record("ni", "尼", true);
        assert(second.flush());
        first.poll(1500); assert(first.recall("ni", true) == Texts{"你"}); // Within the interval: no file access.
        first.poll(4000); assert((first.recall("ni", true) == Texts{"尼", "你"}));
        // Both write between looks; neither loses the other's rows.
        first.record("hao", "好", true); second.record("hao", "号", true); second.record("hao", "号", true);
        assert(second.flush() && first.flush());
        first.poll(8000); second.poll(8000);
        assert((first.recall("hao", true) == Texts{"号", "好"}) && first.recall("hao", true) == second.recall("hao", true));
        assert((second.recall("ni", true) == Texts{"尼", "你"}));
        // Clearing the file clears every process at its next look.
        assert(std::filesystem::remove(path));
        first.poll(12000);
        assert(first.recall("ni", true).empty() && first.recall("hao", true).empty());
        std::ofstream(path, std::ios::binary) << "3\tt\tshi\t是\n";
        second.poll(12000);
        assert(second.recall("ni", true).empty() && second.recall("shi", false) == Texts{"是"});
        // A file being rewritten elsewhere keeps the rows pending.
        const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        assert(locked != INVALID_HANDLE_VALUE);
        second.record("wo", "我", true);
        assert(!second.flush() && second.recall("wo", true) == Texts{"我"});
        CloseHandle(locked);
        assert(second.flush() && bytes(path) == "3\tt\tshi\t是\n1\ts\two\t我\n");
    }
    // A long journal is rewritten as one row per selection on the next start.
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        std::string rows;
        while (rows.size() < CompactAt + 100) rows += "1\ts\tnihao\t你好\n1\ts\tshi\t是\n";
        file << rows << "junk\n";
    }
    Store compacted(path);
    compacted.poll(1000);
    const auto text = bytes(path);
    assert(text.size() < 100 && text.find("junk") == text.npos && text.find(std::to_string(MaxCount) + "\ts\tnihao\t你好\n") != text.npos);
    assert(compacted.recall("nihao", true) == Texts{"你好"});
    compacted.record("nihao", "拟好", true);
    assert(compacted.flush() && bytes(path).size() == text.size() + std::string("1\ts\tnihao\t拟好\n").size());
    Store reopened(path); reopened.poll(1000);
    assert((reopened.recall("nihao", true) == Texts{"你好", "拟好"}));
    // An oversized file is neither read nor grown.
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << std::string(MaxFile + 1, '\n');
    }
    Store oversized(path); oversized.poll(1000);
    oversized.record("ni", "你", true);
    assert(!oversized.flush() && std::filesystem::file_size(path) == MaxFile + 1);
}

using Clock = std::chrono::steady_clock;
static double since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

// CN-06c journal beside the phrase journal: sharing, clearing, compaction,
// and the cost of the reads and writes made on the input thread.
static void storeWords(const std::filesystem::path &folder) {
    constexpr int64_t hour = 480000;
    const auto path = folder / L"selection-memory.tsv", words = folder / L"selection-memory-words.tsv";
    {
        Store first(path), second(path);
        assert(first.wordsFile() == words);
        first.poll(1000); second.poll(1000);
        first.recordWord("gongsi", "公司", true, hour);
        assert(first.words().bonus("gongsi", "公司", true, hour) > 0 && !std::filesystem::exists(words));
        assert(first.flush() && bytes(words) == "+\ts\tgongsi\t公司\t480000\n" && !std::filesystem::exists(path));
        // A phrase row goes to its own file; the word file is untouched.
        first.record("wozai", "我在", true);
        assert(first.flush() && bytes(path) == "1\ts\twozai\t我在\n" && bytes(words) == "+\ts\tgongsi\t公司\t480000\n");
        // Another process sees the word once, and its own rows are not counted twice.
        second.poll(4000);
        assert(second.words().weight("gongsi", "公司", true, hour) == 1 && second.recall("wozai", true) == Texts{"我在"});
        second.recordWord("gongsi", "攻丝", true, hour);
        first.recordWord("gongsi", "公司", true, hour);
        assert(second.flush() && first.flush());
        first.poll(8000); second.poll(8000);
        assert(second.words().weight("gongsi", "攻丝", true, hour) > 0 && first.words().weight("gongsi", "攻丝", true, hour) > 0);
        assert(second.words().weight("gongsi", "公司", true, hour) > 1);
        // Clearing (the management tool truncates both files) clears every process.
        const auto revision = first.words().revision();
        std::filesystem::resize_file(words, 0); std::filesystem::resize_file(path, 0);
        first.poll(12000); second.poll(12000);
        assert(first.words().size() == 0 && second.words().size() == 0 && first.recall("wozai", true).empty());
        assert(first.words().revision() != revision);
        std::filesystem::remove(words);
        first.recordWord("shi", "是", true, hour);
        assert(first.flush() && bytes(words) == "+\ts\tshi\t是\t480000\n");
    }
    // A long word journal is rewritten as current weights on the next start.
    {
        std::ofstream file(words, std::ios::binary | std::ios::trunc);
        std::string rows;
        for (int i = 0; rows.size() < CompactAt + 100; ++i)
            rows += WordModel::event("gongsi", i % 3 ? "公司" : "攻丝", true, hour + i / 1000);
        file << rows << "junk\n";
    }
    Store compacted(path);
    WordModel expected;
    expected.replay(bytes(words));
    compacted.poll(1000);
    const auto text = bytes(words);
    assert(text.size() < 200 && text.find("junk") == text.npos && text.find("\ts\tgongsi\t公司\t") != text.npos && text[0] == '=');
    for (const auto *word : {"公司", "攻丝"})
        assert(std::abs(compacted.words().weight("gongsi", word, true, hour + 100) - expected.weight("gongsi", word, true, hour + 100)) < 0.01);

    // Measured cost with thousands of learned words (not asserted; reported).
    {
        std::filesystem::remove(words);
        Store writer(path);
        writer.poll(1000);
        std::string reading;
        for (size_t i = 0; i < 5000; ++i) {
            reading.clear();
            for (size_t value = i; reading.size() < 5; value /= 26) reading += static_cast<char>('a' + value % 26);
            writer.recordWord(reading, "词" + std::to_string(i % 7), true, hour);
            if (i % 1000 == 999) assert(writer.flush()); // One append stays below 64 KiB.
        }
        const auto size = std::filesystem::file_size(words);
        auto start = Clock::now();
        Store reader(path); reader.poll(1000);
        const double load = since(start);
        assert(reader.words().size() == 5000);
        double append = 0, refresh = 0, worstAppend = 0, worstRefresh = 0;
        constexpr int rounds = 200;
        for (int i = 0; i < rounds; ++i) {
            // One finished composition of four words, then the other process's next look.
            start = Clock::now();
            for (int w = 0; w < 4; ++w) writer.recordWord("ceshi", "测试" + std::to_string(w), true, hour);
            assert(writer.flush());
            const double a = since(start);
            start = Clock::now();
            reader.poll(10000 + 3000 * ULONGLONG(i));
            const double r = since(start);
            append += a; refresh += r; worstAppend = (std::max)(worstAppend, a); worstRefresh = (std::max)(worstRefresh, r);
        }
        start = Clock::now();
        size_t listed = 0;
        reader.words().each(hour, [&](bool, std::string_view, const std::string &, double) { ++listed; });
        const double enumerate = since(start);
        std::cout << "MEASURE: " << reader.words().size() << " words, journal " << size << " bytes; first load " << load
                  << " ms; append of 4 words mean " << append / rounds << " ms (max " << worstAppend << "); other-process refresh mean "
                  << refresh / rounds << " ms (max " << worstRefresh << "); enumerate " << listed << " bonuses " << enumerate << " ms\n";
    }
}

// DICT-02b: the user's own word table, its rows and its reloading.
static void userWords(const std::filesystem::path &folder) {
    UserWordModel model;
    model.parse(std::string("\xEF\xBB\xBF# comment\r\nzhang'san\t张珊\r\n\nqingci\t晴词\t晴詞\n"
        "bad reading\t坏\nZHANG\t坏\n'zhang\t坏\nzhang''san\t坏\nzhang11\t坏\nzhang\t\nzhang\t坏\x01\n"
        "zhang\tA\tB\tC\nonlyreading\n") + std::string(129, 'a') + "\t长\nzhang\t" + std::string(193, 'x') + "\n"
        "zhang1san1\t张珊\t張珊\nshi\t式");
    // Malformed rows are skipped one by one; a later row for the same untoned
    // reading and text updates the earlier one in place; the last row needs no newline.
    assert(model.size() == 3);
    assert(model.entries()[0].reading == "zhang1san1" && model.entries()[0].text == "张珊" && model.entries()[0].traditional == "張珊");
    assert(model.entries()[1].text == "晴词" && model.entries()[1].traditional == "晴詞" && model.entries()[2].text == "式");
    assert(UserWordModel::key("zhang1'san") == "zhangsan" && UserWordModel::key("xi'an") == "xian" && UserWordModel::key("zhang'") == "zhang");
    assert(UserWordModel::key("").empty() && UserWordModel::key("1a").empty() && UserWordModel::key("a b").empty());
    UserWordModel full;
    for (size_t i = 0; i < UserWordModel::MaxWords + 5; ++i) full.add("shi", "式" + std::to_string(i));
    assert(full.size() == UserWordModel::MaxWords);

    // The table beside the journals: missing is empty; a replacement, an edit
    // in place and a deletion are each seen at the next look, and an
    // unchanged file is not read again.
    const auto path = folder / L"selection-memory.tsv";
    Store store(path);
    assert(store.userWordsFile() == folder / L"selection-memory-user.tsv");
    assert(!store.pollUserWords(1000) && store.userWords().empty() && store.userWordsRevision() == 0);
    const auto write = [](const std::filesystem::path &file, const std::string &data) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc); out << data;
    };
    const auto temporary = folder / L"selection-memory-user.tmp";
    write(temporary, "zhang'san\t张珊\n");
    assert(MoveFileExW(temporary.c_str(), store.userWordsFile().c_str(), MOVEFILE_REPLACE_EXISTING));
    assert(!store.pollUserWords(1500) && store.userWords().empty()); // Within the interval: not looked at.
    assert(store.pollUserWords(4000) && store.userWords().size() == 1);
    const auto first = store.userWordsRevision();
    assert(first && !store.pollUserWords(7000) && store.userWordsRevision() == first);
    // Same size, replaced: the identity changed.
    write(temporary, "zhang'san\t张姗\n");
    assert(MoveFileExW(temporary.c_str(), store.userWordsFile().c_str(), MOVEFILE_REPLACE_EXISTING));
    assert(store.pollUserWords(10000) && store.userWords().entries()[0].text == "张姗" && store.userWordsRevision() != first);
    // Edited in place.
    write(store.userWordsFile(), "zhang'san\t张姗\nqing'ci\t晴词\n");
    assert(store.pollUserWords(13000) && store.userWords().size() == 2);
    // Locked against reading: the last good table stays and is retried.
    {
        const HANDLE lock = CreateFileW(store.userWordsFile().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(lock != INVALID_HANDLE_VALUE);
        assert(!store.pollUserWords(16000) && store.userWords().size() == 2);
        CloseHandle(lock);
    }
    // Too large: ignored, the last good table stays.
    write(temporary, std::string(UserWordModel::MaxFile + 1, '#'));
    assert(MoveFileExW(temporary.c_str(), store.userWordsFile().c_str(), MOVEFILE_REPLACE_EXISTING));
    assert(!store.pollUserWords(19000) && store.userWords().size() == 2);
    write(temporary, "shi\t式\n");
    assert(MoveFileExW(temporary.c_str(), store.userWordsFile().c_str(), MOVEFILE_REPLACE_EXISTING));
    assert(store.pollUserWords(22000) && store.userWords().size() == 1);
    // Deleted: empty.
    std::filesystem::remove(store.userWordsFile());
    assert(store.pollUserWords(25000) && store.userWords().empty() && !store.pollUserWords(28000));
    // Clearing the journals leaves the table alone.
    write(store.userWordsFile(), "shi\t式\n");
    assert(store.pollUserWords(31000) && store.userWords().size() == 1);
    store.record("shi", "是", true); store.flush();
    std::filesystem::resize_file(path, 0);
    store.poll(40000);
    assert(store.model().size() == 0 && store.userWords().size() == 1);
    // Reading cost of a 10,000-word table, paid once per change.
    std::string large;
    for (size_t i = 0; i < UserWordModel::MaxWords; ++i) large += "zhang'san\t张" + std::to_string(i) + "\n";
    write(temporary, large);
    assert(MoveFileExW(temporary.c_str(), store.userWordsFile().c_str(), MOVEFILE_REPLACE_EXISTING));
    const auto started = Clock::now();
    assert(store.pollUserWords(50000) && store.userWords().size() == UserWordModel::MaxWords);
    const double readMs = since(started);
    const auto unchanged = Clock::now();
    for (int i = 0; i < 100; ++i) assert(!store.pollUserWords(60000 + 3000 * ULONGLONG(i)));
    std::cout << "user words: read " << large.size() << " bytes, " << UserWordModel::MaxWords << " words in " << readMs
              << " ms; unchanged look " << since(unchanged) / 100 << " ms\n";
}

static void preference() {
    const std::wstring name = L"Software\\HarukotoSelectionMemoryTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    assert(enabled(root)); // Missing: on.
    HKEY key = nullptr;
    assert(RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS);
    DWORD value = 0;
    assert(RegSetValueExW(key, L"SelectionMemory", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
    assert(!enabled(root));
    value = 1;
    assert(RegSetValueExW(key, L"SelectionMemory", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS);
    assert(enabled(root));
    RegCloseKey(key); RegCloseKey(root);
    assert(RegDeleteTreeW(HKEY_CURRENT_USER, name.c_str()) == ERROR_SUCCESS);
}

// CN-07 batch 5: forgetting a selection or a learned word, in the models,
// the journals (shared with other processes) and compaction.
static void forgetting(const std::filesystem::path &folder) {
    Model model;
    model.record("hao", "号", true); model.record("hao", "好", true); model.record("hao", "号", false);
    assert(model.forget("hao", "号", true) && !model.forget("hao", "号", true) && model.size() == 2);
    assert(model.recall("hao", true) == Texts{"好"} && model.recall("hao", false) == Texts{"号"}); // the other script stays
    assert(model.forget("hao", "好", true) && model.recall("hao", true).empty() && !model.forget("ni", "你", true));
    // Rows: a forget removes what came before it; a later selection starts again from one.
    Model replayed;
    const std::string rows = Model::row("hao", "号", true, 5) + Model::forgetRow("hao", "号", true) + Model::row("hao", "号", true) +
        Model::forgetRow("ni", "你", true) + "-\tx\thao\t号\n-\ts\tHAO\t号\n";
    assert(Model::forgetRow("hao", "号", false) == "-\tt\thao\t号\n");
    assert(replayed.replay(rows) == rows.size());
    assert(replayed.recall("hao", true) == Texts{"号"} && replayed.snapshot() == "1\ts\thao\t号\n");

    constexpr int64_t hour = 480000;
    WordModel words;
    words.select("gongsi", "公司", true, hour); words.select("gongsi", "公司", true, hour); words.select("shi", "是", true, hour);
    const auto revision = words.revision();
    assert(words.forget("gongsi", "公司", true, hour) && words.revision() != revision);
    assert(words.bonus("gongsi", "公司", true, hour) == 0 && words.size() == 1 && !words.forget("gongsi", "公司", true, hour));
    assert(!words.forget("shi", "是", false, hour) && words.weight("shi", "是", true, hour) == 1);
    WordModel wordsReplayed;
    const std::string wordRows = WordModel::event("gongsi", "公司", true, hour) + WordModel::event("gongsi", "公司", true, hour, '-') +
        WordModel::event("gongsi", "攻丝", true, hour) + "-\ts\tgongsi\t攻丝\t0\n";
    assert(WordModel::event("gongsi", "公司", true, hour, '-') == "-\ts\tgongsi\t公司\t480000\n");
    assert(wordsReplayed.replay(wordRows) == wordRows.size());
    assert(wordsReplayed.weight("gongsi", "公司", true, hour) == 0 && wordsReplayed.weight("gongsi", "攻丝", true, hour) == 1);
    assert(wordsReplayed.snapshot().find("公司") == std::string::npos);

    // Journals: another process applies the forget at its next look, and a
    // later selection is learned again.
    const auto path = folder / L"selection-memory.tsv", wordsPath = folder / L"selection-memory-words.tsv";
    {
        Store first(path), second(path);
        first.poll(1000); second.poll(1000);
        first.record("hao", "号", true); first.recordWord("hao", "号", true, hour);
        assert(first.flush());
        second.poll(4000);
        assert(second.recall("hao", true) == Texts{"号"} && second.words().weight("hao", "号", true, hour) == 1);
        assert(!second.forget("hao", "好", true) && second.forget("hao", "号", true));
        assert(second.forgetWord("hao", "号", true, hour) && !second.forgetWord("hao", "号", true, hour));
        assert(second.flush());
        assert(bytes(path) == "1\ts\thao\t号\n-\ts\thao\t号\n");
        assert(bytes(wordsPath) == "+\ts\thao\t号\t480000\n-\ts\thao\t号\t480000\n");
        const auto before = first.words().revision();
        first.poll(8000);
        assert(first.recall("hao", true).empty() && first.words().weight("hao", "号", true, hour) == 0 && first.words().revision() != before);
        first.record("hao", "号", true);
        assert(first.flush());
        second.poll(12000);
        assert(second.recall("hao", true) == Texts{"号"});
        // Nothing to forget writes nothing.
        const auto size = bytes(path).size();
        assert(!first.forget("ni", "你", true) && first.flush() && bytes(path).size() == size);
    }
    // Compaction keeps neither the forgotten entries nor the forget rows.
    {
        std::string longRows, longWordRows;
        while (longRows.size() < CompactAt + 100) longRows += "1\ts\tnihao\t你好\n1\ts\tshi\t是\n";
        longRows += Model::forgetRow("shi", "是", true);
        for (int i = 0; longWordRows.size() < CompactAt + 100; ++i)
            longWordRows += WordModel::event("gongsi", i % 2 ? "公司" : "攻丝", true, hour);
        longWordRows += WordModel::event("gongsi", "攻丝", true, hour, '-');
        std::ofstream(path, std::ios::binary | std::ios::trunc) << longRows;
        std::ofstream(wordsPath, std::ios::binary | std::ios::trunc) << longWordRows;
    }
    Store compacted(path);
    compacted.poll(1000);
    const auto text = bytes(path), wordText = bytes(wordsPath);
    assert(text == std::to_string(MaxCount) + "\ts\tnihao\t你好\n");
    assert(wordText.find("攻丝") == std::string::npos && wordText.find("公司") != std::string::npos && wordText.find('-') == std::string::npos);
    assert(compacted.recall("shi", true).empty() && compacted.words().weight("gongsi", "攻丝", true, hour) == 0);
}

int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    model(); words(); candidates(); preference();
    const auto folder = std::filesystem::path(argv[1]) / (L"selection-memory-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    store(folder);
    const auto wordsFolder = folder / L"words";
    std::filesystem::create_directories(wordsFolder);
    storeWords(wordsFolder);
    const auto userFolder = folder / L"user";
    std::filesystem::create_directories(userFolder);
    userWords(userFolder);
    const auto forgetFolder = folder / L"forget";
    std::filesystem::create_directories(forgetFolder);
    forgetting(forgetFolder);
    std::error_code ignored; std::filesystem::remove_all(folder, ignored);
    std::cout << "PASS: selection memory ranking, word weights, journal sharing, clearing, compaction, switch, user words and forgetting\n";
}
