#include "../MixedComposition.h"
#include "../PendingDelivery.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

static MPNLexicon *makeLexicon(const std::string &text) {
    auto *lexicon = mpn_lexicon_create(text.data(), text.size());
    assert(lexicon);
    return lexicon;
}

static void type(MPNSession *session, const std::string &text) {
    for (unsigned char ch : text) assert(mpn_key(session, ch));
}

static std::string numbered(const char *prefix, size_t number) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%s%03zu", prefix, number);
    return buffer;
}

static void verifyLongCandidateList() {
    std::string data = "MEOWPUUT-PINYIN-1\nni\tni3\t你\t你\t18000\n";
    for (size_t i = 0; i < 400; ++i) {
        const auto text = numbered("native", i);
        data += "ni\tni3\t" + text + "\t" + text + "\t" + std::to_string(16000 - i) + "\n";
    }
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    assert(session);
    type(session, "ni");

    ChineseEvidence evidence{{"你", 9000}};
    for (size_t i = 0; i < 320; ++i) evidence[numbered("extra", i)] = 1900 - static_cast<int>(i);
    const auto lookup = [&](const std::string &) { return evidence; };
    const std::vector<JapaneseWord> japanese{{"ni", "に", 6000}, {"ni", "你", 5000}};
    const auto choices = windowsCandidates(session, japanese, lookup, false);
    assert(choices.size() > 256);
    assert(choices.front().text == "你" && !choices.front().japanese);
    assert(choices.front().coreIndex && mpn_candidate_consumed(session, *choices.front().coreIndex) == 2);
    std::set<std::string> unique;
    for (const auto &choice : choices) {
        assert(unique.insert(choice.text).second);
        if (choice.coreIndex) {
            assert(*choice.coreIndex < mpn_candidate_count(session));
            assert(choice.text == mpn_candidate(session, *choice.coreIndex));
        }
    }
    size_t previous = 0;
    for (size_t i = 0; i < 320; ++i) {
        const auto text = numbered("extra", i);
        auto found = std::find_if(choices.begin(), choices.end(), [&](const auto &choice) { return choice.text == text; });
        assert(found != choices.end() && !found->coreIndex);
        const auto at = static_cast<size_t>(found - choices.begin());
        if (i < 7 && i > 0) assert(previous < at); // retain the leading supplements' weight order
        if (i < 7) previous = at;
        if (i == 319) {
            assert(at >= 256);
            const size_t page = at / 7, key = at % 7;
            assert(choices[page * 7 + key].text == text); // same mapping used by TextService
        }
    }
    const auto late = std::find_if(choices.begin(), choices.end(), [&](const auto &choice) {
        return choice.coreIndex && *choice.coreIndex >= 300;
    });
    assert(late != choices.end());
    const size_t absolute = static_cast<size_t>(late - choices.begin());
    assert(absolute >= 256);
    const size_t page = absolute / 7, key = absolute % 7;
    const auto &selected = choices[page * 7 + key];
    assert(selected.coreIndex && mpn_choose(session, *selected.coreIndex));
    assert(selected.text == mpn_committed(session));
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

static void verifySameTextDifferentConsumption() {
    const std::string data = "MEOWPUUT-PINYIN-1\n"
        "hao\thao3\t好\t好\t15000\n"
        "ni\tni3\t你\t你\t15000\n"
        "nihao\tni3hao3\t你好\t你好\t15000\n"
        "nihaoni\tni3hao3ni3\t你好\t你好\t16000\n";
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    assert(session);
    type(session, "nihaoni");
    const auto choices = windowsCandidates(session, {}, [](const std::string &) { return ChineseEvidence{}; }, false);
    std::set<size_t> consumed;
    std::optional<size_t> partial;
    for (const auto &choice : choices) {
        if (choice.text == "你好" && choice.coreIndex) {
            const auto length = mpn_candidate_consumed(session, *choice.coreIndex);
            consumed.insert(length);
            if (length == 5) partial = *choice.coreIndex;
        }
    }
    assert(consumed == (std::set<size_t>{5, 7}));
    assert(partial && mpn_choose(session, *partial));
    assert(std::strcmp(mpn_committed(session), "你好") == 0);
    assert(std::strcmp(mpn_input(session), "ni") == 0);
    PendingDelivery delivery;
    delivery.append(L"你好");
    mpn_clear_commit(session); // same ownership transfer used by TSF
    assert(*delivery.begin() == L"你好");
    delivery.finish(false); // first segment is rejected by the host
    const auto tail = windowsCandidates(session, {}, [](const std::string &) { return ChineseEvidence{}; }, true);
    assert(!tail.empty() && tail.front().text == "你" && tail.front().coreIndex);
    assert(mpn_candidate_consumed(session, *tail.front().coreIndex) == 2);
    assert(mpn_choose(session, *tail.front().coreIndex));
    assert(std::strcmp(mpn_input(session), "") == 0);
    assert(std::strcmp(mpn_committed(session), "你") == 0);
    delivery.append(L"你");
    mpn_clear_commit(session);
    delivery.suspend();
    assert(delivery.begin() == nullptr);
    delivery.resume();
    assert(*delivery.begin() == L"你好"); delivery.finish(true);
    assert(*delivery.begin() == L"你"); delivery.finish(true);
    assert(delivery.empty()); // rejected prefix never disappears or overtakes its tail
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

static void verifyRealLexicon(const char *path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    assert(session);
    type(session, "ji");
    const auto choices = windowsCandidates(session, {}, [](const std::string &) { return ChineseEvidence{}; }, false);
    const auto found = std::find_if(choices.begin(), choices.end(), [](const auto &choice) { return choice.text == "蹟"; });
    assert(found != choices.end() && found->coreIndex);
    const auto position = static_cast<size_t>(found - choices.begin());
    const size_t page = position / 7, key = position % 7;
    const auto &selected = choices[page * 7 + key];
    assert(selected.coreIndex && selected.text == mpn_candidate(session, *selected.coreIndex));
    assert(mpn_choose(session, *selected.coreIndex));
    assert(std::strcmp(mpn_committed(session), "蹟") == 0 && std::strcmp(mpn_input(session), "") == 0);
    mpn_clear(session);
    type(session, "lue");
    const auto aliasChoices = windowsCandidates(session, {}, [](const std::string &reading) {
        ChineseEvidence evidence;
        for (const auto &alias : pinyinEvidenceReadings(reading)) {
            if (alias == "lue") evidence.emplace("蓼莪", 1388);
            if (alias == "lve") evidence.emplace("略", 5000);
        }
        return evidence;
    }, false);
    assert(!aliasChoices.empty() && aliasChoices.front().text == "略");
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

static void verifyRealBridge(const char *lexiconPath, const char *dictionaryPath) {
    std::ifstream file(lexiconPath, std::ios::binary), dictionaryFile(dictionaryPath, std::ios::binary);
    assert(file && dictionaryFile);
    const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    mpn_set_simplified(session, 1);
    const auto dictionary = readJapaneseWords(dictionaryFile);
    size_t checked = 0;
    for (const auto &word : dictionary) {
        if (word.source.empty()) continue;
        mpn_clear(session);
        type(session, word.reading);
        const auto choices = windowsCandidates(session, dictionary,
            [](const std::string &) { return ChineseEvidence{}; }, false);
        const auto found = std::find_if(choices.begin(), choices.end(), [&](const auto &c) {
            return c.text == word.text && c.japanese && c.annotation.find(word.kana) == 0;
        });
        assert(found != choices.end() && !found->coreIndex);
        assert(found - choices.begin() < 7);
        ++checked;
    }
    assert(checked >= 41);
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

static void verifyJapaneseBridge() {
    std::istringstream table(
        "nihon|日本|9000\n"
        "riben|日本|5000|にほん|pinyin|\r\n"
        "xinsu|新宿|5000|しんじゅく|pinyin|\n"
        "xinsu|新宿|4900|にいじゅく|pinyin|\n"
        "coffee|コーヒー|5000|こーひー|english|\n"
        "bad|bad|oops\n"
        "bad|bad|5000junk\n");
    const auto dictionary = readJapaneseWords(table);
    assert(dictionary.size() == 5);
    auto *lexicon = makeLexicon("MEOWPUUT-PINYIN-1\nriben\tri4ben3\t日本\t日本\t18000\n");
    auto *session = mpn_session_create(lexicon);
    const auto lookup = [](const std::string &input) {
        return input == "riben" ? ChineseEvidence{{"日本", 9000}} : ChineseEvidence{};
    };
    type(session, "riben");
    const auto choices = windowsCandidates(session, dictionary, lookup, false);
    assert(choices.front().text == "日本" && !choices.front().japanese && choices.front().coreIndex);
    const auto ja = std::find_if(choices.begin(), choices.end(), [](const auto &c) { return c.japanese; });
    assert(ja != choices.end() && ja->text == "日本" && ja->annotation == "にほん · 拼" && !ja->coreIndex);
    assert(ja - choices.begin() < 7);
    const auto kana = std::find_if(choices.begin(), choices.end(), [](const auto &c) { return c.text == "にほん"; });
    assert(kana != choices.end() && kana->directJapanese && !kana->coreIndex);
    assert(kana - choices.begin() < 7 && ja->directJapanese);
    const auto locked = windowsCandidates(session, dictionary, lookup, true);
    assert(std::none_of(locked.begin(), locked.end(), [](const auto &c) { return c.japanese; }));
    mpn_clear(session);
    type(session, "xinsu");
    const auto readings = windowsCandidates(session, dictionary, lookup, false);
    assert(std::count_if(readings.begin(), readings.end(), [](const auto &c) { return c.text == "新宿" && c.japanese; }) == 2);
    mpn_clear(session);
    type(session, "coffee");
    const auto english = windowsCandidates(session, dictionary, lookup, false);
    const auto coffee = std::find_if(english.begin(), english.end(), [](const auto &c) { return c.text == "コーヒー"; });
    assert(coffee != english.end() && coffee->annotation == "こーひー · EN coffee" && coffee->japanese);
    assert(coffee - english.begin() < 7);
    assert(std::any_of(english.begin(), english.end(), [](const auto &c) { return c.text == "coffee" && !c.japanese; }));
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

static void verifyLazyExpansion() {
    std::string data = "MEOWPUUT-PINYIN-1\n";
    for (size_t i = 0; i < 400; ++i) {
        const auto text = numbered("match", i);
        data += "nihao\tni3hao3\t" + text + "\t" + text + "\t" + std::to_string(16000 - i) + "\n";
    }
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    type(session, "nihao");
    const auto lookup = [](const std::string &) { return ChineseEvidence{}; };
    const std::vector<JapaneseWord> words;
    auto choices = windowsCandidates(session, words, lookup, false);
    const auto before = choices;
    assert(choices.size() == 256 && mpn_candidates_can_expand(session));
    assert(mpn_expand_candidates(session) == 144);
    appendCandidateExpansion(session, choices, windowsCandidates(session, words, lookup, false));
    assert(choices.size() == 400);
    for (size_t i = 0; i < before.size(); ++i) {
        assert(choices[i].text == before[i].text && choices[i].coreIndex == before[i].coreIndex);
    }
    while (mpn_candidates_can_expand(session)) assert(mpn_expand_candidates(session) == 0);
    const auto repeated = choices.size();
    appendCandidateExpansion(session, choices, windowsCandidates(session, words, lookup, false));
    assert(choices.size() == repeated && !mpn_candidates_can_expand(session));
    assert(choices.back().coreIndex && mpn_choose(session, *choices.back().coreIndex));
    assert(std::string(mpn_committed(session)) == "match399" && !*mpn_input(session));
    mpn_clear_commit(session);
    type(session, "nihao");
    assert(mpn_candidates_can_expand(session) && mpn_candidate_count(session) == 256);
    mpn_cancel_composition(session);
    assert(!mpn_candidates_can_expand(session));
    mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
}

// CN-08: when no decoded sentence is right, leading words and characters keep
// every dictionary text reachable by selecting segment after segment.
static void verifyLeadingWords() {
    // Buckets are sorted by reading, and by descending weight within one.
    std::string data = "MEOWPUUT-PINYIN-1\n"
                       "an\tan1\t安\t安\t9000\n"
                       "cha\tcha2\t查\t查\t9000\n"
                       "chashou\tcha2shou1\t查收\t查收\t6000\n"
                       "qi\tqi1\t七\t七\t9500\n";
    const char *characters[] = {"请", "清", "情", "轻", "青", "晴", "氢", "倾", "卿"};
    for (size_t i = 0; i < 9; ++i)
        data += std::string("qing\tqing") + (i ? "1" : "3") + "\t" + characters[i] + "\t" + characters[i] + "\t" + std::to_string(9000 - i) + "\n";
    data += "qingcha\tqing1cha2\t清查\t清查\t5000\n";
    for (size_t i = 0; i < 9; ++i) {
        const auto text = numbered("whole", i);
        data += "qingchashou\tqing3cha2shou1\t" + text + "\t" + text + "\t" + std::to_string(16000 - i) + "\n";
    }
    data += "shou\tshou1\t收\t收\t9000\n"
            "xi\txi1\t西\t西\t9000\n"
            "xian\txian1\t先\t先\t9000\n";
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    const auto find = [&](const std::string &text, size_t consumed) {
        for (size_t i = 0; i < mpn_candidate_count(session); ++i)
            if (text == mpn_candidate(session, i) && mpn_candidate_consumed(session, i) == consumed) return i;
        return mpn_candidate_count(session);
    };
    const auto missing = [&](const std::string &text, size_t consumed) { return find(text, consumed) == mpn_candidate_count(session); };

    // The first page is untouched; building blocks start on the second.
    type(session, "qingchashou");
    // Existing order: three whole matches, the decoded leading phrase, then more.
    const size_t firstPage[] = {0, 1, 2, 99, 3, 4, 5};
    for (size_t i = 0; i < 7; ++i)
        assert((firstPage[i] == 99 ? std::string("清查") : numbered("whole", firstPage[i])) == mpn_candidate(session, i));
    assert(find("清查", 7) == 3 && find("请", 4) == 7 && find("清", 4) == 8);
    MPNCandidateInfo info{};
    assert(mpn_candidate_info(session, 7, &info, sizeof(info)) && info.kind == MPN_CANDIDATE_PARTIAL &&
           info.consumed_bytes == 4 && info.remaining_offset_bytes == 4);
    // The page holds seven building blocks; whole-input matches resume after it.
    assert(find("氢", 4) == 13 && find(numbered("whole", 6), 11) == 14 && missing("倾", 4));
    // qi|ngchashou is not a syllable boundary.
    assert(missing("七", 2));
    std::vector<std::string> before;
    for (size_t i = 0; i < mpn_candidate_count(session); ++i) before.push_back(mpn_candidate(session, i));

    // The first expansion completes the ladder without moving earlier indices.
    assert(mpn_candidates_can_expand(session) && mpn_expand_candidates(session) >= 2);
    for (size_t i = 0; i < before.size(); ++i) assert(before[i] == mpn_candidate(session, i));
    assert(find("倾", 4) == before.size() && find("卿", 4) == before.size() + 1);
    assert(missing("七", 2));
    const auto total = mpn_candidate_count(session);
    while (mpn_candidates_can_expand(session)) mpn_expand_candidates(session);
    assert(find("倾", 4) == before.size() && mpn_candidate_count(session) >= total);

    // Selecting a block keeps the remaining keys for the next selection.
    assert(mpn_choose(session, find("请", 4)));
    assert(std::string(mpn_committed(session)) == "请" && std::string(mpn_input(session)) == "chashou");
    assert(mpn_choose(session, find("查收", 7)));
    assert(std::string(mpn_committed(session)) == "请查收" && !*mpn_input(session));
    mpn_clear_commit(session);

    // A list that fits one page gains nothing until the user pages onward.
    type(session, "xian");
    assert(mpn_candidate_count(session) <= 7 && missing("西", 2));
    mpn_expand_candidates(session);
    assert(!missing("西", 2));
    assert(mpn_choose(session, find("西", 2)) && std::string(mpn_input(session)) == "an");
    mpn_clear(session); mpn_clear_commit(session);

    // An explicit separator belongs to the syllable it closes.
    type(session, "xi'an");
    while (mpn_candidates_can_expand(session)) mpn_expand_candidates(session);
    assert(!missing("西", 3) && missing("先", 4) && missing("先", 5));
    assert(mpn_choose(session, find("西", 3)) && std::string(mpn_input(session)) == "an");
    mpn_clear(session); mpn_clear_commit(session);

    // A single syllable already lists its characters; nothing is repeated.
    type(session, "qing");
    const auto single = mpn_candidate_count(session);
    assert(single == 9);
    while (mpn_candidates_can_expand(session)) mpn_expand_candidates(session);
    assert(mpn_candidate_count(session) == single);
    mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
}

int main(int argc, char **argv) {
    verifyLazyExpansion();
    verifyLeadingWords();
    verifyJapaneseBridge();
    assert((pinyinEvidenceReadings("lue") == std::vector<std::string>{"lue", "lve"}));
    assert((pinyinEvidenceReadings("nue") == std::vector<std::string>{"nue", "nve"}));
    assert((pinyinEvidenceReadings("shenglue") == std::vector<std::string>{"shenglue", "shenglve"}));
    assert((pinyinEvidenceReadings("lu'e") == std::vector<std::string>{"lue"}));
    assert((pinyinEvidenceReadings("nu'e") == std::vector<std::string>{"nue"}));
    verifyLongCandidateList();
    verifySameTextDifferentConsumption();
    if (argc > 1) verifyRealLexicon(argv[1]);
    if (argc > 2) verifyRealBridge(argv[1], argv[2]);
    std::puts("PASS: Windows candidate pagination, indices and distinct consumption");
}
