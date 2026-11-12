#include "../SelectionMemory.h"
#include <cassert>
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

int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    model(); candidates(); preference();
    const auto folder = std::filesystem::path(argv[1]) / (L"selection-memory-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    store(folder);
    std::error_code ignored; std::filesystem::remove_all(folder, ignored);
    std::cout << "PASS: selection memory ranking, journal sharing, clearing, compaction and switch\n";
}
