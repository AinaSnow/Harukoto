#define NOMINMAX
#include "../ConjugationPanel.h"
#include "../WordEvidence.h"
#include "../JapanesePackages.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 4);
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
    if (argc == 4) {
        auto themes = readEnabledJapanesePackages(argv[2]);
        assert(themes.size() == std::stoul(argv[3]));
        auto merged = words; merged.insert(merged.end(), themes.begin(), themes.end());
        std::stable_sort(merged.begin(), merged.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        for (bool simplified : {false, true}) {
            auto lookup = [&](const std::string &reading) { return evidence.dictionary(reading, simplified); };
            mpn_set_simplified(session, simplified);
            for (const auto &word : themes) {
                mpn_clear(session);
                for (char c : word.reading) assert(mpn_key(session, c));
                const auto choices = windowsCandidates(session, merged, lookup, false);
                const auto end = choices.begin() + std::min(size_t(7), choices.size());
                for (const auto &output : {word.text, word.kana}) {
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
                assert(before.size() == after.size());
                for (size_t i = 0; i < before.size(); ++i)
                    assert(before[i].text == after[i].text && before[i].annotation == after[i].annotation && before[i].coreIndex == after[i].coreIndex);
            }
        }
        std::cout << "PASS: FF14 real SQLite first-page spelling/kana, both scripts, Chinese isolation\n";
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
