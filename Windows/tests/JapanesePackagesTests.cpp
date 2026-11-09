#include "../JapanesePackages.h"
#include <cassert>
#include <chrono>
#include <iostream>

static void write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text; assert(file);
}

int main(int argc, char **argv) {
    if (argc == 3) {
        const auto words = readEnabledJapanesePackages(argv[1]);
        assert(words.size() == std::stoul(argv[2]));
        // Verify the manager's output reaches real candidate merging, including independent kana.
        const std::string data = "MEOWPUUT-PINYIN-1\nriben\tri4ben3\t日本\t日本\t9000\n";
        auto *lexicon = mpn_lexicon_create(data.data(), data.size()); assert(lexicon);
        auto *session = mpn_session_create(lexicon); assert(session);
        for (const auto &word : words) {
            mpn_clear(session);
            for (char c : word.reading) assert(mpn_key(session, c));
            const auto choices = windowsCandidates(session, words, [](const std::string &) { return ChineseEvidence{}; }, false);
            for (const auto &output : {word.text, word.kana}) if (!output.empty())
                assert(std::any_of(choices.begin(), choices.end(), [&](const auto &c) { return c.text == output && c.directJapanese && !c.coreIndex; }));
        }
        mpn_clear(session);
        for (char c : std::string("riben")) assert(mpn_key(session, c));
        const auto lookup = [](const std::string &) { return ChineseEvidence{{"日本", 9000}}; };
        const auto chinese = windowsCandidates(session, words, lookup, false);
        assert(!chinese.empty() && chinese.front().text == "日本" && chinese.front().coreIndex);
        const auto locked = windowsCandidates(session, words, lookup, true);
        assert(std::none_of(locked.begin(), locked.end(), [](const auto &c) { return c.japanese; }));
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
        return 0;
    }
    const auto root = std::filesystem::temp_directory_path() / ("harukoto-packages-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string generation(32, 'a');
    std::filesystem::create_directories(root / generation);
    assert(readEnabledJapanesePackages(root).empty());
    write(root / "CURRENT", generation);
    const auto tsv = root / generation / "enabled.tsv";
    const std::string good = "riben|日本|5000|にほん|pinyin|\n";
    write(tsv, good); assert(readEnabledJapanesePackages(root).size() == 1);
    write(tsv, "limusa|リムサ・ロミンサ|5000||pinyin|利姆萨·罗敏萨\n");
    assert(readEnabledJapanesePackages(root).size() == 1);
    for (const auto &bad : {std::string("../escape"), generation + "\n", std::string(32, 'z'), std::string(33, 'a')}) {
        write(root / "CURRENT", bad); assert(readEnabledJapanesePackages(root).empty());
    }
    write(root / "CURRENT", generation);
    for (const auto &bad : {good + "broken\n", std::string("riben|日本|5000|nihon|pinyin|\n"),
            std::string("riben|日本|5000|にほん||\n"), std::string("riben|") + char(0xff) + "|5000|にほん|pinyin|\n",
            std::string(1025, 'x'), std::string(32 * 1024 * 1024 + 1, 'x')}) {
        write(tsv, bad); assert(readEnabledJapanesePackages(root).empty());
    }
    write(tsv, ""); assert(readEnabledJapanesePackages(root).empty());
    std::filesystem::remove_all(root);
    std::cout << "PASS: missing, malformed, oversized and empty optional package snapshots\n";
}
