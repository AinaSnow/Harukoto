#define NOMINMAX
#include "../ConjugationPanel.h"
#include "../WordEvidence.h"
#include "../JapanesePackages.h"
#include <cassert>
#include <array>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3 || argc == 4 || argc == 5);
    const std::filesystem::path dir(argv[1]);
    std::ifstream file(dir / "native-pinyin.tsv", std::ios::binary);
    assert(file);
    const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto *lexicon = mpn_lexicon_create(data.data(), data.size()); assert(lexicon);
    auto *session = mpn_session_create(lexicon); assert(session);
    std::ifstream dictionary(dir / "japanese.tsv"); assert(dictionary);
    auto words = readJapaneseWords(dictionary);
    JapaneseConjugations table; assert(table.loadFile(dir / "japanese-conjugations.bin"));
    appendConjugationWords(table, words);
    std::stable_sort(words.begin(), words.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
    WordEvidence evidence; evidence.open(dir / "language-evidence.sqlite");
    assert(!evidence.dictionary("chi", true).empty());
    if (argc == 4 && std::string(argv[2]) == "--daily") {
        std::ifstream source(argv[3]); assert(source);
        size_t count = 0;
        for (std::string line; std::getline(source, line);) {
            if (line.empty() || line.front() == '#') continue;
            if (line.back() == '\r') line.pop_back();
            std::vector<std::string> fields; std::istringstream row(line);
            for (std::string field; std::getline(row, field, '|');) fields.push_back(field);
            assert(fields.size() == 4);
            std::string input; for (char c : fields[2]) if (c >= 'a' && c <= 'z') input += c;
            for (bool simplified : {false, true}) {
                mpn_clear(session); mpn_set_simplified(session, simplified);
                for (char c : input) assert(mpn_key(session, c));
                const auto choices = windowsCandidates(session, words, [&](const std::string &reading) { return evidence.dictionary(reading, simplified); }, false);
                const auto &expected = fields[simplified ? 1 : 0];
                if (choices.empty() || choices.front().text != expected) std::cerr << input << " expected " << expected << std::endl;
                assert(!choices.empty() && choices.front().text == expected && choices.front().coreIndex);
                assert(mpn_choose(session, *choices.front().coreIndex));
                assert(std::string(mpn_committed(session)) == expected && !*mpn_input(session));
                mpn_clear_commit(session);
            }
            ++count;
        }
        assert(count >= 270);
        std::cout << "PASS: daily Chinese first choice and commit in both scripts: " << count << std::endl;
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon); return 0;
    }
    // A common exact word must not hide a newly recorded gaming homophone,
    // and short-input search must expose usable choices beyond five paths.
    for (bool simplified : {false, true}) {
        mpn_set_simplified(session, simplified);
        mpn_clear(session);
        for (char c : std::string("yedui")) assert(mpn_key(session, c));
        const auto choices = windowsCandidates(session, words, [&](const std::string &reading) { return evidence.dictionary(reading, simplified); }, false);
        assert(choices.size() > 7);
        assert(choices.front().text == (simplified ? "也对" : "也對"));
        const auto firstPage = choices.begin() + 7;
        const auto team = std::find_if(choices.begin(), firstPage, [&](const auto &c) { return c.text == (simplified ? "野队" : "野隊"); });
        assert(team != firstPage && team->coreIndex);
        assert(mpn_choose(session, *team->coreIndex));
        assert(std::string(mpn_committed(session)) == team->text && !*mpn_input(session));
        mpn_clear_commit(session);
        mpn_clear(session); for (char c : std::string("yedui")) assert(mpn_key(session, c));
        assert(choices[7].coreIndex && mpn_choose(session, *choices[7].coreIndex));
        assert(std::string(mpn_committed(session)) == choices[7].text);
        mpn_clear_commit(session);
    }
    // Ordinary Chinese regression corpus, with themes absent. Full readings
    // must beat an overlapping unfinished-prefix interpretation.
    mpn_set_simplified(session, true);
    for (const auto &sample : std::vector<std::pair<std::string, std::string>>{
            {"zhidaole", "知道了"}, {"wozhidaole", "我知道了"},
            {"xinkule", "辛苦了"}, {"dengyixia", "等一下"},
            {"wokanyixia", "我看一下"}, {"wobuzhidao", "我不知道"},
            {"meiwenti", "没问题"}, {"woyaohuijia", "我要回家"},
            {"zhidaol", "知道"}, {"wozhidaol", "我知道"}}) {
        mpn_clear(session); for (char c : sample.first) assert(mpn_key(session, c));
        const auto choices = windowsCandidates(session, words, [&](const std::string &reading) { return evidence.dictionary(reading, true); }, false);
        if (choices.empty() || choices.front().text != sample.second)
            std::cerr << "Chinese ranking regression: " << sample.first << " expected " << sample.second << " actual " << (choices.empty() ? "empty" : choices.front().text) << std::endl;
        assert(!choices.empty() && choices.front().text == sample.second && choices.front().coreIndex);
        if (sample.first == "zhidaole") {
            assert(mpn_choose(session, *choices.front().coreIndex));
            assert(std::string(mpn_committed(session)) == "知道了" && !*mpn_input(session));
            mpn_clear_commit(session);
        }
    }
    if (argc == 3 && std::string(argv[2]) == "--diagnose") {
        mpn_set_simplified(session, true);
        for (std::string input; std::getline(std::cin, input);) {
            if (!input.empty() && input.back() == '\r') input.pop_back();
            mpn_clear(session); for (char c : input) mpn_key(session, c);
            const auto choices = windowsCandidates(session, words, [&](const std::string &reading) { return evidence.dictionary(reading, true); }, false);
            std::cout << input;
            for (size_t i = 0; i < std::min(size_t(7), choices.size()); ++i)
                std::cout << " | " << choices[i].text;
            std::cout << std::endl;
        }
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon); return 0;
    }
    // Reusable short context must improve composed phrases in both scripts;
    // these sentences are not whole-word entries in the authored dictionaries.
    for (bool simplified : {false, true}) {
        mpn_set_simplified(session, simplified);
        for (const auto &sample : std::vector<std::array<std::string, 3>>{
            {"womenmingtianjian", "我们明天见", "我們明天見"},
            {"womenwanshangjian", "我们晚上见", "我們晚上見"},
            {"tamenmingtianjian", "他们明天见", "他們明天見"},
            {"qingbangwokanyixia", "请帮我看一下", "請幫我看一下"},
            {"qingbangwomen", "请帮我们", "請幫我們"},
            {"zuduishijian", "组队时间", "組隊時間"},
            {"woduanxianle", "我断线了", "我斷線了"},
            {"taduanxianle", "他断线了", "他斷線了"},
            {"womenkaishiba", "我们开始吧", "我們開始吧"},
            {"nimenkaishiba", "你们开始吧", "你們開始吧"},
            {"zhegezenmeda", "这个怎么打", "這個怎麼打"}}) {
            mpn_clear(session);
            for (char c : sample[0]) assert(mpn_key(session, c));
            const auto choices = windowsCandidates(session, words, [&](const std::string &reading) { return evidence.dictionary(reading, simplified); }, false);
            const auto &expected = sample[simplified ? 1 : 2];
            if (choices.empty() || choices.front().text != expected) std::cerr << "Context: " << sample[0] << " expected " << expected << std::endl;
            assert(!choices.empty() && choices.front().text == expected && choices.front().coreIndex);
            assert(mpn_choose(session, *choices.front().coreIndex));
            assert(std::string(mpn_committed(session)) == expected && !*mpn_input(session));
            mpn_clear_commit(session);
        }
    }
    if (argc >= 4) {
        const bool bulk = argc == 5;
        auto themes = readEnabledJapanesePackages(argv[2]);
        assert(themes.size() == std::stoul(argv[3]));
        auto merged = words; merged.insert(merged.end(), themes.begin(), themes.end());
        std::stable_sort(merged.begin(), merged.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        for (bool simplified : {false, true}) {
            auto lookup = [&](const std::string &reading) { return evidence.dictionary(reading, simplified); };
            mpn_set_simplified(session, simplified);
            if (bulk) {
                for (const auto &input : {"xinkule", "zaoshanghao"}) {
                    mpn_clear(session); for (char c : std::string(input)) assert(mpn_key(session, c));
                    const auto before = windowsCandidates(session, words, lookup, false);
                    const auto after = windowsCandidates(session, merged, lookup, false);
                    assert(!before.empty() && !after.empty() && before.front().text == after.front().text);
                    assert(after.front().coreIndex && after.front().coreIndex == before.front().coreIndex);
                    assert(std::any_of(after.begin(), after.end(), [](const auto &c) { return c.annotation == "主题词库 · 中文"; }));
                    assert(mpn_choose(session, *after.front().coreIndex));
                    assert(std::string(mpn_committed(session)) == before.front().text && !*mpn_input(session));
                    mpn_clear_commit(session);
                }
                for (const auto &input : {"wuerdaha", "xinkule"}) {
                    mpn_clear(session);
                    for (char c : std::string(input)) assert(mpn_key(session, c));
                    const auto choices = windowsCandidates(session, merged, lookup, false);
                    const auto pageEnd = choices.begin() + std::min(size_t(7), choices.size());
                    if (std::string(input) == "wuerdaha") {
                        assert(choices.front().text == (simplified ? "乌尔达哈" : "烏爾達哈"));
                        assert(!choices.front().japanese && !choices.front().coreIndex);
                        assert(std::any_of(choices.begin(), pageEnd, [](const auto &c) { return c.text == "ウルダハ"; }));
                    } else {
                        assert(std::any_of(choices.begin(), pageEnd, [](const auto &c) { return c.text == "お疲れ様でした。"; }));
                        assert(std::any_of(choices.begin(), pageEnd, [](const auto &c) { return c.text == "おつかれさまでした"; }));
                    }
                }
                const std::string longName = "乌尔达哈·高级恢复药（测试）";
                const std::string converted = mpn_custom_chinese(session, longName.c_str());
                assert(converted == (simplified ? longName : "烏爾達哈·高級恢復藥（測試）"));
            }
            size_t checked = 0;
            for (const auto &word : themes) {
                // Load validates every row. Sample across the entire large table
                // for expensive real-lexicon candidate/keystroke integration.
                if (bulk && checked++ % 2000 != 0 && word.reading != "huifuyao" && word.reading != "limusaluominsa") continue;
                mpn_clear(session);
                for (char c : word.reading) assert(mpn_key(session, c));
                const auto choices = windowsCandidates(session, merged, lookup, false);
                const auto end = bulk ? choices.end() : choices.begin() + std::min(size_t(7), choices.size());
                for (const auto &output : {word.text, word.kana}) {
                    if (output.empty()) continue;
                    const auto found = std::find_if(choices.begin(), end, [&](const auto &c) { return c.text == output && c.directJapanese && !c.coreIndex; });
                    if (found == end) std::cerr << "Missing first-page theme choice: " << word.reading << " -> " << output << std::endl;
                    assert(found != end);
                }
                const auto locked = windowsCandidates(session, merged, lookup, true);
                assert(std::none_of(locked.begin(), locked.end(), [](const auto &c) { return c.japanese; }));
            }
            for (const auto &input : {"nihao", "chi", "riben", "zhongguo", "jintian"}) {
                mpn_clear(session); for (char c : std::string(input)) assert(mpn_key(session, c));
                const auto before = windowsCandidates(session, words, lookup, false);
                const auto after = windowsCandidates(session, merged, lookup, false);
                if (bulk) {
                    // Full Completion includes greetings such as nihao. Verify
                    // Chinese choices survive; legitimate new theme choices may coexist.
                    for (const auto &choice : before) if (choice.coreIndex)
                        assert(std::any_of(after.begin(), after.end(), [&](const auto &c) { return c.text == choice.text && c.coreIndex == choice.coreIndex; }));
                    continue;
                }
                assert(before.size() == after.size());
                for (size_t i = 0; i < before.size(); ++i)
                    assert(before[i].text == after[i].text && before[i].annotation == after[i].annotation && before[i].coreIndex == after[i].coreIndex);
            }
        }
        std::cout << (bulk ? "PASS: complete bulk load; sampled candidates, both scripts, Chinese preservation\n" : "PASS: FF14 real SQLite first-page spelling/kana, both scripts, Chinese isolation\n");
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon); return 0;
    }
    for (bool simplified : {false, true})
    for (const auto &input : {"chi", "eat", "riben", "qu", "lai", "hao", "xie", "you", "xuexi", "gao", "keai", "anjing"}) {
        mpn_clear(session); mpn_set_simplified(session, simplified);
        for (char c : std::string(input)) assert(mpn_key(session, c));
        auto choices = windowsCandidates(session, words, [&](const std::string &reading) {
            return evidence.dictionary(reading, simplified);
        }, false);
        const auto pageEnd = choices.begin() + std::min(size_t(7), choices.size());
        if (std::string(input) == "chi") assert(choices.front().text == "吃" && choices.front().coreIndex);
        if (std::string(input) == "riben") {
            for (const auto &text : {"日本", "にほん"}) {
                const auto found = std::find_if(choices.begin(), pageEnd, [&](const auto &c) {
                    return c.text == text && c.directJapanese && !c.coreIndex;
                });
                assert(found != pageEnd);
            }
        } else {
            const auto found = std::find_if(choices.begin(), pageEnd, [&](const auto &c) {
                return conjugationFor(table, c, input) != nullptr;
            });
            assert(found != pageEnd && found->directJapanese);
            const auto *entry = conjugationFor(table, *found, input);
            const auto kana = std::find_if(choices.begin(), pageEnd, [&](const auto &c) {
                return c.text == entry->reading && c.directJapanese && !c.coreIndex;
            });
            assert(kana != pageEnd);
            size_t page = 0, highlighted = static_cast<size_t>(found - choices.begin()); bool expanded = false;
            ConjugationPanel panel;
            assert(panel.open(table, choices, input, page, highlighted, expanded));
            assert(panel.active() && choices.front().text == entry->text);
        }
    }
    std::cout << "PASS: production lexicon and SQLite, both scripts, direct spelling/kana pairs and F6 on first page\n";
    mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
}
