#include "../MixedComposition.h"
#include "../PendingDelivery.h"
#include <cassert>
#include <cmath>
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
    // DICT-02b on the real lexicon: a hand-added word leads its reading and
    // joins sentences, a single syllable does not take over its common
    // character, and unrelated input is untouched.
    auto *plain = mpn_session_create(lexicon);
    auto *personal = mpn_personal_create();
    mpn_set_simplified(session, 1); mpn_set_simplified(plain, 1);
    const auto firstOf = [](MPNSession *value, const char *keys) {
        mpn_clear(value); type(value, keys); return std::string(mpn_candidate(value, 0));
    };
    std::vector<std::string> before;
    const char *unrelated[] = {"wozaigongsi", "shijianhenchang", "nihao", "zhongguoren", "woshixuesheng"};
    for (const char *keys : unrelated) before.push_back(firstOf(plain, keys));
    mpn_set_personal(session, personal);
    assert(mpn_personal_add_word(personal, "qing'ci", "晴词", nullptr) && mpn_personal_add_word(personal, "shi", "式", nullptr));
    assert(firstOf(session, "qingci") == "晴词" && mpn_candidate_personal(session, 0));
    assert(firstOf(session, "qingcishurufa") == "晴词输入法");
    assert(firstOf(session, "shi") == "式" && firstOf(plain, "shi") != "式" && firstOf(session, "woshi") == firstOf(plain, "woshi"));
    // The user guide's examples: a name, in a sentence and by its initials.
    assert(mpn_personal_add_word(personal, "zhang'shan", "张珊", nullptr));
    assert(firstOf(plain, "zhangshan") != "张珊" && firstOf(session, "zhangshan") == "张珊");
    assert(firstOf(session, "wozhaozhangshan") == "我找张珊");
    {
        mpn_clear(session); type(session, "zhangs");
        // Mixed abbreviation leads; two bare initials (zs) compete with
        // hundreds of common words at weight 3000 and stay off the first page.
        assert(std::string(mpn_candidate(session, 0)) == "张珊" && mpn_candidate_personal(session, 0));
    }
    assert(mpn_personal_remove_word(personal, "zhangshan", "张珊"));
    for (size_t i = 0; i < std::size(unrelated); ++i) assert(firstOf(session, unrelated[i]) == before[i]);
    mpn_set_simplified(session, 0);
    assert(firstOf(session, "qingci") == "晴詞");
    mpn_set_personal(session, nullptr);
    mpn_personal_destroy(personal); mpn_session_destroy(plain);
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

// CN-01b/CN-01e: optional corpus statistics steer sentence composition only.
static void verifyCompositionStatistics() {
    const std::string words = "MEOWPUUT-PINYIN-2\n"
                              "ba\tba3\t把\t把\t8000\t8000\n"
                              "ba\tba5\t吧\t吧\t7000\t7000\n"
                              "chi\tchi1\t吃\t吃\t8000\t8000\n"
                              "chifan\tchi1fan4\t吃飯\t吃饭\t7000\t7000\n"
                              "dao\tdao4\t到\t到\t8000\t8000\n"
                              "fan\tfan4\t飯\t饭\t8000\t8000\n"
                              "le\tle5\t了\t了\t9000\t9000\n"
                              "zhi\tzhi2\t直\t直\t6000\t6000\n"
                              "zhidao\tzhi1dao4\t知道\t知道\t7000\t7000\n"
                              "zhidao\tzhi2dao4\t直到\t直到\t5600\t5600\n"
                              "zou\tzou3\t走\t走\t8000\t8000\n"
                              "# context\n"
                              "到了\t9000\t9000\n";
    const auto first = [](const std::string &data, const char *keys) {
        auto *lexicon = makeLexicon(data);
        auto *session = mpn_session_create(lexicon);
        mpn_set_simplified(session, 1);
        type(session, keys);
        assert(mpn_candidate_count(session) > 0);
        const std::string text = mpn_candidate(session, 0);
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
        return text;
    };
    // Pairs name their words by line number in "# pairwords" (byte order):
    // 1 "$", 2 "^", 3 了, 4 吧, 5 直到, 6 知道, 7 走.
    const std::string statistics = "# statistics\n", pairWords = "# pairwords\n$\n^\n了\n吧\n直到\n知道\n走\n# pairs\n";
    // Without statistics the more frequent homophone wins everywhere, and a
    // boundary that spells a dictionary word (到了) earns the full bonus.
    assert(first(words, "chifanba") == "吃饭把" && first(words, "zouba") == "走把" && first(words, "zhidaole") == "直到了");
    assert(first(words + statistics, "chifanba") == "吃饭把");
    // A phrase-final particle is recognised by the end marker ...
    assert(first(words + statistics + pairWords + "4\t1\t300\n", "chifanba") == "吃饭吧");
    // ... an opener by the start marker, and neighbours by their own pair.
    assert(first(words + statistics + pairWords + "2\t6\t400\n", "zhidaole") == "知道了");
    assert(first(words + statistics + pairWords + "7\t4\t300\n", "zouba") == "走吧");
    assert(first(words + statistics + pairWords + "7\t4\t300\n", "chifanba") == "吃饭把");
    // One pair cannot outvote everything: its score is capped at six nats, so
    // a seven-nat correction on the other reading still decides.
    assert(first(words + statistics + pairWords + "5\t3\t999\n", "zhidaole") == "直到了");
    assert(first(words + statistics + "# priors\n知道\t700\n" + pairWords + "5\t3\t999\n", "zhidaole") == "知道了");
    // A correction moves a word inside sentences but not on its own.
    const auto corrected = words + statistics + "# priors\n吧\t200\n";
    assert(first(corrected, "zouba") == "走吧" && first(corrected, "ba") == "把");
    // With pairs present, the dictionary-word boundary counts for half, while
    // an authored context phrase counts for more than before.
    assert(first(words + statistics + pairWords + "7\t4\t300\n", "zhidaole") == "知道了");
    assert(first(words + statistics + "# authored\n到了\n" + pairWords + "7\t4\t300\n", "zhidaole") == "直到了");
    // Sections are optional but ordered, sorted and validated.
    for (const std::string broken : {pairWords + "4\t1\t0\n", pairWords + "4\t1\t1000\n", pairWords + "7\t4\t1\n4\t1\t1\n",
                                     pairWords + "4\t1\t1\n4\t1\t2\n", pairWords + "4\t1\n", pairWords + "4\t8\t1\n",
                                     pairWords + "0\t1\t1\n", pairWords + "吧\t$\t1\n", std::string("# pairs\n1\t1\t1\n"),
                                     std::string("# pairwords\n走\n吧\n"), std::string("# priors\n吧\t0\n"),
                                     std::string("# priors\n走\t5\n吧\t5\n"), std::string("# priors\n吧\t1000\n"),
                                     std::string("# authored\n到\n"), std::string("# authored\n到了\t1\n"),
                                     pairWords + "4\t1\t1\n# priors\n吧\t5\n", std::string("# priors\n吧\t5\n# authored\n到了\n"),
                                     std::string("# unknown\n"), std::string("吧\t5\n")}) {
        const auto data = words + statistics + broken;
        assert(!mpn_lexicon_create(data.data(), data.size()));
    }
}

// CN-06c: personal word bonuses in whole-word order and sentence composition.
struct Listed { std::string text; size_t consumed; uint32_t kind, scoreKind; double score; };
static bool operator==(const Listed &a, const Listed &b) {
    return a.text == b.text && a.consumed == b.consumed && a.kind == b.kind && a.scoreKind == b.scoreKind && a.score == b.score;
}
static std::vector<Listed> listed(MPNSession *session) {
    std::vector<Listed> result;
    for (size_t i = 0; i < mpn_candidate_count(session); ++i) {
        MPNCandidateInfo info{};
        assert(mpn_candidate_info(session, i, &info, sizeof(info)));
        result.push_back({mpn_candidate(session, i), mpn_candidate_consumed(session, i), info.kind, info.score_kind, info.score});
    }
    return result;
}

static void verifyPersonalWords() {
    const std::string data = "MEOWPUUT-PINYIN-1\n"
        "chang\tchang2\t長\t长\t8000\n"
        "da\tda4\t大\t大\t8000\n"
        "gongshi\tgong1shi4\t公式\t公式\t6000\n"
        "gongshi\tgong1shi4\t攻勢\t攻势\t5900\n"
        "gongsi\tgong1si1\t攻絲\t攻丝\t6000\n"
        "gongsi\tgong1si1\t公司\t公司\t5800\n"
        "gongsi\tgong1si1\t公私\t公私\t1000\n"
        "hen\then3\t很\t很\t8000\n"
        "jian\tjian4\t見\t见\t8000\n"
        "jian\tjian1\t間\t间\t7000\n"
        "shi\tshi4\t是\t是\t9000\n"
        "shi\tshi2\t時\t时\t8000\n"
        "shijian\tshi2jian1\t時間\t时间\t7000\n"
        "wo\two3\t我\t我\t9000\n"
        "xian\txian1\t先\t先\t8000\n"
        "xian\txi1an1\t西安\t西安\t7000\n"
        "zai\tzai4\t在\t在\t9000\n";
    auto *lexicon = makeLexicon(data);
    auto *plain = mpn_session_create(lexicon), *learning = mpn_session_create(lexicon);
    auto *personal = mpn_personal_create();
    assert(plain && learning && personal);
    mpn_set_simplified(plain, 1); mpn_set_simplified(learning, 1);
    mpn_set_personal(learning, personal);
    const auto run = [](MPNSession *session, const char *keys) {
        mpn_clear(session); type(session, keys);
        auto result = listed(session);
        while (mpn_candidates_can_expand(session)) mpn_expand_candidates(session);
        return std::make_pair(result, listed(session));
    };
    const auto first = [&](MPNSession *session, const char *keys) { return run(session, keys).first.front().text; };
    const char *inputs[] = {"gongsi", "wozaigongsi", "gongsihenda", "gongshi", "shijianhenchang", "shi", "jian",
                            "xian", "xi'an", "gongs", "wozaigong", "shijianh", "gong"};
    // An empty table, or one without these readings, changes nothing at all.
    const auto unchanged = [&] {
        for (const char *keys : inputs) assert(run(plain, keys) == run(learning, keys));
    };
    unchanged();
    assert(mpn_personal_set(personal, 1, "ma", "妈", 2) && mpn_personal_set(personal, 0, "gongsi", "公司", 2));
    unchanged(); // Another reading, and the other script.
    mpn_personal_clear(personal);
    assert(first(plain, "gongsi") == "攻丝" && first(plain, "wozaigongsi") == "我在攻丝" && first(plain, "gongsihenda") == "攻丝很大");

    // One selection's worth of bonus (log 2) lifts a close homophone alone,
    // in another sentence and at the start of one. The table is shared: the
    // change applies at the next refresh.
    assert(mpn_personal_set(personal, 1, "gongsi", "公司", std::log1p(1.0)));
    const auto single = run(learning, "gongsi").first;
    assert(single[0].text == "公司" && single[1].text == "攻丝" && single[2].text == "公私" && single.size() == run(plain, "gongsi").first.size());
    assert(first(learning, "wozaigongsi") == "我在公司" && first(learning, "gongsihenda") == "公司很大");
    // Other readings are untouched, including one that shares a prefix.
    assert(run(plain, "gongshi") == run(learning, "gongshi") && run(plain, "shijianhenchang") == run(learning, "shijianhenchang"));
    // Traditional output keeps its own table.
    mpn_set_simplified(learning, 0); mpn_set_simplified(plain, 0);
    assert(run(plain, "gongsi") == run(learning, "gongsi") && first(learning, "gongsi") == "攻絲");
    mpn_set_simplified(learning, 1); mpn_set_simplified(plain, 1);

    // Capped: even an absurd bonus counts as three nats, which cannot carry a
    // word seven nats down the list past its homophones.
    mpn_personal_clear(personal);
    assert(mpn_personal_set(personal, 1, "gongsi", "公私", 100));
    assert(first(learning, "gongsi") == "攻丝" && first(learning, "wozaigongsi") == "我在攻丝");
    // Below the leader a word rises by its full bonus; 时 trails 是 by 1.43 nats.
    mpn_personal_clear(personal);
    assert(mpn_personal_set(personal, 1, "shi", "时", 1.1) && first(learning, "shi") == "是");
    assert(mpn_personal_set(personal, 1, "shi", "时", 1.5) && first(learning, "shi") == "时");
    // A longer word is not split apart: 是 and 见 at the cap are already the
    // best of their readings, so they gain a fifth of it, 0.6 nats each,
    // against the 4.7 nats that 时间 leads by. (At full value, 2 x 3 nats
    // would turn 时间很长 into 是见很长.)
    mpn_personal_clear(personal);
    assert(mpn_personal_set(personal, 1, "shi", "是", 3) && mpn_personal_set(personal, 1, "jian", "见", 3));
    assert(first(plain, "shijianhenchang") == "时间很长" && first(learning, "shijianhenchang") == "时间很长");
    assert(first(learning, "shijian") == "时间");
    // Removing entries restores the original lists exactly.
    assert(mpn_personal_set(personal, 1, "shi", "是", 0) && mpn_personal_set(personal, 1, "jian", nullptr, 0));
    unchanged();
    // The handle can go; attached sessions keep the table until detached.
    assert(mpn_personal_set(personal, 1, "gongsi", "公司", 1));
    mpn_personal_destroy(personal);
    assert(first(learning, "gongsi") == "公司");
    mpn_set_personal(learning, nullptr);
    assert(run(plain, "gongsi") == run(learning, "gongsi"));
    assert(!mpn_personal_set(nullptr, 1, "gongsi", "公司", 1));

    // The words of a candidate, for learning: fewest dictionary words, untoned
    // readings, separators dropped; nothing for invalid indices.
    mpn_clear(learning); type(learning, "wozaigongsi");
    assert(std::string(mpn_candidate(learning, 0)) == "我在攻丝");
    assert(std::string(mpn_candidate_words(learning, 0)) == "wo\t我\nzai\t在\ngongsi\t攻丝\n");
    assert(!*mpn_candidate_words(learning, mpn_candidate_count(learning)) && !*mpn_candidate_words(nullptr, 0));
    for (size_t i = 0; i < mpn_candidate_count(learning); ++i) {
        // A leading segment covers only the keys it consumes.
        if (std::string(mpn_candidate(learning, i)) == "我" && mpn_candidate_consumed(learning, i) == 2)
            assert(std::string(mpn_candidate_words(learning, i)) == "wo\t我\n");
    }
    mpn_clear(learning); type(learning, "xi'an");
    assert(std::string(mpn_candidate(learning, 0)) == "西安" && std::string(mpn_candidate_words(learning, 0)) == "xian\t西安\n");
    mpn_clear(learning); type(learning, "shijian");
    assert(std::string(mpn_candidate_words(learning, 0)) == "shijian\t时间\n");
    mpn_set_simplified(learning, 0);
    assert(std::string(mpn_candidate(learning, 0)) == "時間" && std::string(mpn_candidate_words(learning, 0)) == "shijian\t時間\n");
    // A keyboard correction is not a spelling of the typed keys.
    mpn_set_simplified(learning, 1);
    mpn_clear(learning); type(learning, "gonsgi");
    for (size_t i = 0; i < mpn_candidate_count(learning); ++i)
        if (mpn_candidate_consumed(learning, i) == 6) assert(!*mpn_candidate_words(learning, i));
    mpn_session_destroy(plain); mpn_session_destroy(learning); mpn_lexicon_destroy(lexicon);
}

// DICT-02b: words the user added by hand.
static void verifyUserWords() {
    const std::string data = "MEOWPUUT-PINYIN-1\n"
        "an\tan1\t安\t安\t7000\n"
        "ci\tci2\t詞\t词\t7000\n"
        "ci\tci2\t瓷\t瓷\t6000\n"
        "de\tde5\t的\t的\t9000\n"
        "feng\tfeng1\t風\t风\t7000\n"
        "guo\tguo2\t國\t国\t8000\n"
        "hao\thao3\t好\t好\t9000\n"
        "ni\tni3\t你\t你\t9000\n"
        "nihao\tni3hao3\t你好\t你好\t9000\n"
        "qing\tqing1\t青\t青\t7000\n"
        "qing\tqing2\t晴\t晴\t6500\n"
        "qingci\tqing1ci2\t青瓷\t青瓷\t3479\n"
        "san\tsan1\t三\t三\t8000\n"
        "shi\tshi4\t是\t是\t9000\n"
        "shi\tshi1\t詩\t诗\t6000\n"
        "shiyan\tshi2yan4\t實驗\t实验\t6059\n"
        "wo\two3\t我\t我\t9000\n"
        "xi\txi1\t西\t西\t7000\n"
        "xian\txian1\t先\t先\t8000\n"
        "xian\txi1an1\t西安\t西安\t7000\n"
        "yan\tyan2\t岩\t岩\t5000\n"
        "zhang\tzhang1\t張\t张\t8000\n"
        "zhao\tzhao3\t找\t找\t8000\n"
        "zhong\tzhong1\t中\t中\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t10000\n"
        "zuo\tzuo4\t做\t做\t8000\n";
    auto *lexicon = makeLexicon(data);
    auto *plain = mpn_session_create(lexicon), *user = mpn_session_create(lexicon);
    auto *personal = mpn_personal_create();
    assert(plain && user && personal);
    mpn_set_simplified(plain, 1); mpn_set_simplified(user, 1);
    mpn_set_personal(user, personal);
    const auto run = [](MPNSession *session, const char *keys) {
        mpn_clear(session); type(session, keys);
        auto result = listed(session);
        while (mpn_candidates_can_expand(session)) mpn_expand_candidates(session);
        return std::make_pair(result, listed(session));
    };
    const auto first = [&](MPNSession *session, const char *keys) { return run(session, keys).first.front().text; };
    const auto count = [&](MPNSession *session, const char *keys, const char *text) {
        const auto all = run(session, keys).second;
        return std::count_if(all.begin(), all.end(), [&](const Listed &item) { return item.text == text && item.consumed == std::strlen(keys); });
    };
    const char *inputs[] = {"zhangsan", "wozhaozhangsan", "qingci", "woqingci", "shiyan", "zuoshiyan", "shi", "woshi",
                            "nihao", "xian", "xi'an", "zs", "zhang1san", "zhang3san", "ci", "zhongguo", "zhangs"};
    const auto unchanged = [&] {
        for (const char *keys : inputs) assert(run(plain, keys) == run(user, keys));
    };
    unchanged();
    assert(first(plain, "zhangsan") == "张三" && first(plain, "woqingci") == "我青瓷" && first(plain, "zuoshiyan") == "做实验");

    // Validation: readings of letters, tone digits and single apostrophes;
    // non-empty bounded text without control characters.
    for (const char *reading : {"", "zhang san", "1zhang", "zh''ang", "ZHANG", "zhang-san", "'zhang", "zhang11"})
        assert(!mpn_personal_add_word(personal, reading, "张珊", nullptr));
    for (const char *text : {"", "张\t珊", "张\n珊", "\x7f", "\xff", "\xe5\xbc"})
        assert(!mpn_personal_add_word(personal, "zhangsan", text, nullptr));
    assert(!mpn_personal_add_word(personal, "zhangsan", std::string(193, 'a').c_str(), nullptr));
    assert(!mpn_personal_add_word(personal, "zhangsan", "张珊", "張\x01珊"));
    assert(!mpn_personal_add_word(nullptr, "zhangsan", "张珊", nullptr) && !mpn_personal_add_word(personal, nullptr, "张珊", nullptr));
    assert(mpn_personal_word_count(personal) == 0);
    // Accepted but not syllables of this lexicon: ignored, nothing changes.
    assert(mpn_personal_add_word(personal, "xyzq", "某词", nullptr) && mpn_personal_word_count(personal) == 1);
    unchanged();
    assert(mpn_personal_remove_word(personal, "xyzq", "某词") && !mpn_personal_remove_word(personal, "xyzq", "某词"));

    // A name the lexicon lacks leads its reading, in either script, and wins
    // in a sentence over spelling it character by character.
    assert(mpn_personal_add_word(personal, "zhang'san", "张珊", nullptr));
    auto list = run(user, "zhangsan").first;
    assert(list[0].text == "张珊" && list[0].kind == MPN_CANDIDATE_EXACT && list[0].consumed == 8 && mpn_candidate_personal(user, 0));
    assert(list[1].text == "张三" && !mpn_candidate_personal(user, 1));
    assert(first(user, "wozhaozhangsan") == "我找张珊" && mpn_candidate_personal(user, 0));
    assert(mpn_choose(user, 0) && std::string(mpn_committed(user)) == "我找张珊" && !*mpn_input(user));
    mpn_clear_commit(user);
    // Unspelled letters split into syllables: the same word.
    assert(mpn_personal_add_word(personal, "zhangsan", "张珊", nullptr) && mpn_personal_word_count(personal) == 1);
    assert(first(user, "zhangsan") == "张珊" && count(user, "zhangsan", "张珊") == 1);
    // Untoned syllables accept any typed tone; toned ones only their own.
    assert(first(user, "zhang3san") == "张珊" && first(user, "zhang'san") == "张珊");
    assert(mpn_personal_add_word(personal, "zhang1'san", "张珊", nullptr) && mpn_personal_word_count(personal) == 1);
    assert(first(user, "zhang1san") == "张珊" && count(user, "zhang3san", "张珊") == 0);
    assert(mpn_personal_add_word(personal, "zhang'san", "张珊", nullptr));
    // Traditional output: converted through the lexicon, or as given.
    mpn_set_simplified(user, 0);
    assert(first(user, "zhangsan") == "張珊");
    assert(mpn_personal_add_word(personal, "zhang'san", "张珊", "章珊") && mpn_personal_word_count(personal) == 1);
    assert(first(user, "zhangsan") == "章珊" && first(user, "wozhaozhangsan") == "我找章珊");
    mpn_set_simplified(user, 1);
    assert(first(user, "zhangsan") == "张珊");
    // Through its initials (CN-04), alone and in a sentence.
    assert(first(user, "zs") == "张珊" && mpn_candidate_personal(user, 0) && mpn_candidate_abbreviated(user, 0) == 2);
    assert(first(user, "wozhaozs") == "我找张珊");
    // Other readings are untouched.
    for (const char *keys : {"nihao", "xian", "xi'an", "zhongguo", "qingci", "shi", "ci"}) assert(run(plain, keys) == run(user, keys));
    // Longer than any lexicon reading, inside a sentence.
    assert(mpn_personal_add_word(personal, "zhangsanfeng", "张三丰", nullptr));
    assert(first(user, "zhangsanfeng") == "张三丰" && first(user, "wozhaozhangsanfeng") == "我找张三丰");
    assert(mpn_personal_remove_word(personal, "zhang'san'feng", "张三丰"));

    // A rare homophone yields in sentences too; a common one does not, and a
    // single syllable never displaces its common character in a sentence.
    assert(mpn_personal_add_word(personal, "qingci", "晴词", nullptr) && mpn_personal_add_word(personal, "shiyan", "诗岩", nullptr) &&
           mpn_personal_add_word(personal, "shi", "式", nullptr));
    list = run(user, "qingci").first;
    assert(list[0].text == "晴词" && list[1].text == "青瓷" && first(user, "woqingci") == "我晴词");
    assert(first(user, "shiyan") == "诗岩" && first(user, "zuoshiyan") == "做实验");
    assert(first(user, "shi") == "式" && first(user, "woshi") == "我是");
    // A lexicon word added by the user moves up instead of appearing twice.
    assert(first(plain, "ci") == "词" && mpn_personal_add_word(personal, "ci", "瓷", nullptr));
    assert(first(user, "ci") == "瓷" && count(user, "ci", "瓷") == 1);
    // Several words of one reading: in the order added.
    assert(mpn_personal_add_word(personal, "ci", "辞", nullptr));
    list = run(user, "ci").first;
    assert(list[0].text == "瓷" && list[1].text == "辞" && list[2].text == "词");
    // Evidence for another word cannot push it down in the Windows list.
    const auto evidence = [](const std::string &) { return ChineseEvidence{{"张三", 9000}}; };
    run(user, "zhangsan");
    assert(windowsCandidates(user, {}, evidence, false).front().text == "张珊");
    run(plain, "zhangsan");
    assert(windowsCandidates(plain, {}, evidence, false).front().text == "张三");

    // Learned bonuses and user words are cleared separately.
    assert(mpn_personal_set(personal, 1, "gongsi", "公司", 1));
    mpn_personal_clear(personal);
    assert(mpn_personal_word_count(personal) == 6 && first(user, "zhangsan") == "张珊");
    assert(mpn_personal_remove_word(personal, "zhang1san1", "张珊") && mpn_personal_word_count(personal) == 5);
    assert(first(user, "zhangsan") == "张三");
    mpn_personal_clear_words(personal);
    assert(mpn_personal_word_count(personal) == 0);
    unchanged();
    // A table shared by sessions of another lexicon is prepared for each.
    assert(mpn_personal_add_word(personal, "zhang'san", "张珊", nullptr));
    auto *other = makeLexicon(data);
    auto *second = mpn_session_create(other);
    mpn_set_simplified(second, 0); mpn_set_personal(second, personal);
    assert(first(second, "zhangsan") == "張珊" && first(user, "zhangsan") == "张珊");
    mpn_session_destroy(second); mpn_lexicon_destroy(other);
    mpn_personal_destroy(personal);
    assert(first(user, "zhangsan") == "张珊");
    mpn_set_personal(user, nullptr);
    unchanged();
    assert(!mpn_candidate_personal(nullptr, 0) && !mpn_candidate_personal(user, 1000));
    mpn_session_destroy(plain); mpn_session_destroy(user); mpn_lexicon_destroy(lexicon);
}

// CN-03: fuzzy pinyin pairs in the core decoder.
static void verifyFuzzyPinyin() {
    const std::string data = "MEOWPUUT-PINYIN-1\n"
        "chi\tchi1\t吃\t吃\t8000\n"
        "chifan\tchi1fan4\t吃飯\t吃饭\t7000\n"
        "ci\tci3\t此\t此\t7000\n"
        "cifan\tci3fan1\t此番\t此番\t3000\n"
        "dian\tdian3\t點\t点\t7080\n"
        "dianzhan\tdian4zhan4\t電站\t电站\t3952\n"
        "fan\tfan4\t飯\t饭\t7000\n"
        "fan\tfan1\t番\t番\t5000\n"
        "fen\tfen1\t分\t分\t7000\n"
        "feng\tfeng1\t風\t风\t8000\n"
        "guan\tguan1\t關\t关\t7000\n"
        "guang\tguang1\t光\t光\t8000\n"
        "guo\tguo2\t國\t国\t8000\n"
        "guo\tguo4\t過\t过\t7000\n"
        "hao\thao3\t好\t好\t9000\n"
        "hu\thu2\t湖\t湖\t7000\n"
        "jian\tjian4\t見\t见\t7000\n"
        "lan\tlan2\t藍\t蓝\t6000\n"
        "lang\tlang2\t狼\t狼\t7000\n"
        "li\tli3\t李\t李\t7000\n"
        "lihao\tli4hao3\t利好\t利好\t6000\n"
        "ni\tni3\t你\t你\t9000\n"
        "nihao\tni3hao3\t你好\t你好\t9000\n"
        "ren\tren2\t人\t人\t8000\n"
        "shi\tshi4\t是\t是\t9000\n"
        "shijian\tshi2jian1\t時間\t时间\t8000\n"
        "si\tsi4\t四\t四\t7000\n"
        "sijian\tsi4jian4\t四件\t四件\t2710\n"
        "wan\twan4\t萬\t万\t7000\n"
        "wang\twang2\t王\t王\t8000\n"
        "xian\txian1\t先\t先\t7000\n"
        "xiang\txiang3\t想\t想\t8000\n"
        "xin\txin1\t新\t新\t8000\n"
        "xing\txing2\t行\t行\t8000\n"
        "yan\tyan2\t言\t言\t7000\n"
        "yang\tyang2\t羊\t羊\t8000\n"
        "zan\tzan4\t贊\t赞\t5000\n"
        "zg\tz1g2\t中國\t中国\t1000\n"
        "zhan\tzhan4\t站\t站\t6000\n"
        "zhong\tzhong1\t中\t中\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t10000\n"
        "zong\tzong3\t總\t总\t7000\n";
    auto *lexicon = makeLexicon(data);
    auto *plain = mpn_session_create(lexicon), *fuzzy = mpn_session_create(lexicon);
    assert(plain && fuzzy);
    mpn_set_simplified(plain, 1); mpn_set_simplified(fuzzy, 1);
    const auto run = [](MPNSession *session, const char *keys) {
        mpn_clear(session); type(session, keys);
        auto result = listed(session);
        while (mpn_candidates_can_expand(session)) mpn_expand_candidates(session);
        return std::make_pair(result, listed(session));
    };
    const auto index = [](const std::vector<Listed> &items, const char *text) {
        size_t i = 0;
        while (i < items.size() && items[i].text != text) ++i;
        return i;
    };
    const char *inputs[] = {"zongguo", "zhongguo", "cifan", "chifan", "lihao", "nihao", "si", "shi", "fu", "len",
                            "lan", "fen", "xin", "xian3", "yan", "guan1", "wan", "sijian", "dianzan", "zg", "zhg", "nihaozongguo", "zong1guo2", "zong3guo2", "zon"};
    // Off by default, and off again after use: exactly the lists without the feature.
    assert(mpn_fuzzy_pinyin(fuzzy) == 0 && mpn_fuzzy_pinyin(nullptr) == 0);
    mpn_set_fuzzy_pinyin(nullptr, MPN_FUZZY_ALL);
    const auto unchanged = [&] { for (const char *keys : inputs) assert(run(plain, keys) == run(fuzzy, keys)); };
    unchanged();
    mpn_set_fuzzy_pinyin(fuzzy, MPN_FUZZY_ALL | (1u << 20));
    assert(mpn_fuzzy_pinyin(fuzzy) == MPN_FUZZY_ALL); // Unknown bits are dropped.
    mpn_set_fuzzy_pinyin(fuzzy, 0);
    unchanged();

    // Each pair on its own reaches its word; every other pair together does not.
    struct Pair { uint32_t bit; const char *keys, *word; };
    const Pair pairs[] = {
        {MPN_FUZZY_Z_ZH, "zongguo", "中国"}, {MPN_FUZZY_C_CH, "cifan", "吃饭"}, {MPN_FUZZY_S_SH, "si", "是"},
        {MPN_FUZZY_N_L, "lihao", "你好"}, {MPN_FUZZY_F_H, "fu", "湖"}, {MPN_FUZZY_R_L, "len", "人"},
        {MPN_FUZZY_AN_ANG, "lan", "狼"}, {MPN_FUZZY_EN_ENG, "fen", "风"}, {MPN_FUZZY_IN_ING, "xin", "行"},
        // Four-letter n/ng endings are already offered as corrections, so a
        // typed tone (which turns those off) shows the pair itself.
        {MPN_FUZZY_IAN_IANG, "xian3", "想"}, {MPN_FUZZY_IAN_IANG, "yan", "羊"},
        {MPN_FUZZY_UAN_UANG, "guan1", "光"}, {MPN_FUZZY_UAN_UANG, "wan", "王"},
    };
    // CN-05 typo correction can reach the same words (zongguo is one missing
    // h from zhongguo), so it is off while the pairs are told apart.
    mpn_set_typo_correction(plain, 0); mpn_set_typo_correction(fuzzy, 0);
    for (const auto &pair : pairs) {
        mpn_set_fuzzy_pinyin(fuzzy, pair.bit);
        const auto on = run(fuzzy, pair.keys).second;
        assert(index(on, pair.word) < on.size());
        mpn_set_fuzzy_pinyin(fuzzy, MPN_FUZZY_ALL & ~pair.bit);
        const auto others = run(fuzzy, pair.keys).second;
        assert(index(others, pair.word) == others.size());
        assert(index(run(plain, pair.keys).second, pair.word) == run(plain, pair.keys).second.size());
    }
    mpn_set_typo_correction(plain, 1); mpn_set_typo_correction(fuzzy, 1);

    mpn_set_fuzzy_pinyin(fuzzy, MPN_FUZZY_ALL);
    // Without a good exact reading the fuzzy word leads; it is a correction,
    // and it is not offered as words to learn.
    auto list = run(fuzzy, "zongguo").first;
    assert(list.front().text == "中国" && list.front().kind == MPN_CANDIDATE_CORRECTION && list.front().consumed == 7);
    assert(!*mpn_candidate_words(fuzzy, 0));
    assert(run(fuzzy, "fu").first.front().text == "湖");
    // A fuzzy word competes on frequency: clearly more common than the exact
    // words (5 nats a change), it leads: 时间 (8000) over 四件 (2710), 吃饭
    // (7000) over 此番 (3000). Exact readings win near-ties: 利好 (6000)
    // before 你好 (9000), 四 (7000) before 是 (9000).
    list = run(fuzzy, "sijian").first;
    assert(list.front().text == "时间" && list.front().kind == MPN_CANDIDATE_CORRECTION && mpn_candidate_fuzzy(fuzzy, 0));
    assert(index(list, "四件") == 1 && !mpn_candidate_fuzzy(fuzzy, 1));
    list = run(fuzzy, "cifan").first;
    assert(list.front().text == "吃饭" && index(list, "此番") == 1);
    list = run(fuzzy, "lihao").first;
    assert(list.front().text == "利好" && list.front().kind != MPN_CANDIDATE_CORRECTION && index(list, "你好") == 1);
    list = run(fuzzy, "si").first;
    assert(list.front().text == "四" && index(list, "是") < list.size());
    // Without an exact word, a rare fuzzy word pays the full 8 nats, so a
    // composed exact reading (a word missing from the dictionary) stays first.
    list = run(fuzzy, "dianzan").first;
    assert(list.front().text == "点赞" && index(list, "电站") > 0 && index(list, "电站") < list.size());
    assert(!mpn_candidate_fuzzy(fuzzy, list.size()) && !mpn_candidate_fuzzy(nullptr, 0));
    list = run(fuzzy, "zhongguo").first;
    assert(list.front().text == "中国" && list.front().kind == MPN_CANDIDATE_EXACT);
    // Sentence composition: a fuzzy word inside a sentence.
    list = run(fuzzy, "nihaozongguo").first;
    assert(list.front().text == "你好中国" && list.front().kind == MPN_CANDIDATE_CORRECTION);
    assert(index(run(plain, "nihaozongguo").second, "你好中国") == run(plain, "nihaozongguo").second.size());
    // Typed tones must agree with the word; abbreviations are never substituted.
    assert(run(fuzzy, "zong1guo2").first.front().text == "中国");
    assert(index(run(fuzzy, "zong3guo2").second, "中国") == run(fuzzy, "zong3guo2").second.size());
    assert(run(plain, "zg") == run(fuzzy, "zg") && run(plain, "zhg") == run(fuzzy, "zhg"));
    // Traditional output.
    mpn_set_simplified(fuzzy, 0);
    assert(run(fuzzy, "zongguo").first.front().text == "中國");
    mpn_set_simplified(fuzzy, 1);
    // Selecting the fuzzy word consumes every key.
    mpn_clear(fuzzy); type(fuzzy, "zongguo");
    assert(mpn_choose(fuzzy, 0) && std::string(mpn_committed(fuzzy)) == "中国" && !*mpn_input(fuzzy));
    // Changing the pairs rebuilds an open composition at once.
    mpn_clear(fuzzy); mpn_set_fuzzy_pinyin(fuzzy, 0); type(fuzzy, "zongguo");
    const auto before = listed(fuzzy);
    mpn_set_fuzzy_pinyin(fuzzy, MPN_FUZZY_Z_ZH);
    assert(std::string(mpn_candidate(fuzzy, 0)) == "中国" && std::string(mpn_input(fuzzy)) == "zongguo");
    mpn_set_fuzzy_pinyin(fuzzy, 0);
    assert(listed(fuzzy) == before);
    // The Windows merge puts words with dictionary evidence for the typed
    // reading first, but keeps a fuzzy word the core already ranked above them.
    mpn_set_fuzzy_pinyin(fuzzy, MPN_FUZZY_ALL);
    mpn_clear(fuzzy); type(fuzzy, "sijian");
    const auto sijianEvidence = [](const std::string &) { return ChineseEvidence{{"四件", 2710}}; };
    auto merged = windowsCandidates(fuzzy, {}, sijianEvidence, false);
    assert(merged.size() > 1 && merged[0].text == "时间" && merged[1].text == "四件");
    // Off: the evidence order is exactly as before.
    mpn_set_fuzzy_pinyin(fuzzy, 0);
    mpn_clear(plain); type(plain, "sijian");
    merged = windowsCandidates(fuzzy, {}, sijianEvidence, false);
    const auto unmerged = windowsCandidates(plain, {}, sijianEvidence, false);
    assert(merged.size() == unmerged.size() && merged[0].text == "四件");
    for (size_t i = 0; i < merged.size(); ++i) assert(merged[i].text == unmerged[i].text);
    mpn_session_destroy(plain); mpn_session_destroy(fuzzy); mpn_lexicon_destroy(lexicon);
}

// CN-04: abbreviated and mixed pinyin in the core decoder.
static void verifyAbbreviatedPinyin() {
    const std::string data = "MEOWPUUT-PINYIN-1\n"
        "an\tan1\t安\t安\t7000\n"
        "bu\tbu4\t不\t不\t9000\n"
        "en\ten1\t恩\t恩\t5000\n"
        "guo\tguo2\t國\t国\t8000\n"
        "hao\thao3\t好\t好\t9000\n"
        "haome\thao3me5\t好麼\t好么\t5000\n"
        "ma\tma5\t嗎\t吗\t8000\n"
        "nanhai\tnan2hai2\t男孩\t男孩\t7000\n"
        "nh\tn3h3\t你好\t你好\t1125\n"
        "nh\tn2h2\t男孩\t男孩\t875\n"
        "ni\tni3\t你\t你\t9000\n"
        "nihao\tni3hao3\t你好\t你好\t9000\n"
        "nihaoma\tni3hao3ma5\t你好嗎\t你好吗\t4500\n"
        "ren\tren2\t人\t人\t8000\n"
        "wo\two3\t我\t我\t9000\n"
        "xi\txi1\t西\t西\t7000\n"
        "xian\txian1\t先\t先\t8000\n"
        "xian\txi1an1\t西安\t西安\t7000\n"
        "zg\tz1g2\t中國\t中国\t1250\n"
        "zhege\tzhe4ge5\t這個\t这个\t9000\n"
        "zhidao\tzhi1dao4\t知道\t知道\t7500\n"
        "zhong\tzhong1\t中\t中\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t10000\n"
        "zhongguoren\tzhong1guo2ren2\t中國人\t中国人\t5500\n"
        "zhuguanren\tzhu3guan3ren2\t主管人\t主管人\t1000\n";
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    assert(session);
    mpn_set_simplified(session, 1);
    const auto run = [&](const char *keys) { mpn_clear(session); type(session, keys); return listed(session); };
    const auto index = [](const std::vector<Listed> &items, const char *text) {
        size_t i = 0;
        while (i < items.size() && items[i].text != text) ++i;
        return i;
    };

    // Whole-word initials: frequency order, a correction consuming every key,
    // and nothing to learn word by word.
    auto list = run("nh");
    assert(list[0].text == "你好" && list[0].kind == MPN_CANDIDATE_CORRECTION && list[0].consumed == 2);
    assert(mpn_candidate_abbreviated(session, 0) == 2 && index(list, "男孩") < list.size() && index(list, "男孩") > 0);
    assert(!*mpn_candidate_words(session, 0));
    assert(mpn_choose(session, 0) && std::string(mpn_committed(session)) == "你好" && !*mpn_input(session));
    mpn_clear_commit(session);
    // zh/ch/sh as initials, and a lone z covering zh.
    assert(run("zg")[0].text == "中国" && run("zhg")[0].text == "中国" && run("zhgr")[0].text == "中国人");
    // Mixed: typed syllables and initials within one word; more syllables
    // typed in full win (中国人, not 中国人+恩), and frequency decides among
    // words of one pattern (中国人 5500 over 主管人 1000).
    list = run("zgren");
    assert(list[0].text == "中国人" && list[0].kind == MPN_CANDIDATE_CORRECTION && mpn_candidate_abbreviated(session, 0) == 2);
    assert(index(list, "主管人") > 0 && index(list, "主管人") < list.size());
    assert(mpn_choose(session, 0) && std::string(mpn_committed(session)) == "中国人" && !*mpn_input(session));
    mpn_clear_commit(session);
    // A lone initial inside a sentence may be one common character.
    assert(run("wobzd")[0].text == "我不知道" && run("nihaozg")[0].text == "你好中国");
    // A single initial after complete syllables may still be the next
    // syllable: the exact prefix stays first, the completion follows it.
    list = run("nihaom");
    assert(list[0].text == "你好" && list[0].consumed == 5 && list[0].kind == MPN_CANDIDATE_PARTIAL);
    assert(list[1].text == "你好吗" && list[1].consumed == 6 && mpn_candidate_abbreviated(session, 1) == 1);
    // Typed tones and separators must agree with the word.
    assert(run("z1g2")[0].text == "中国" && run("z'g")[0].text == "中国");
    list = run("z3g2");
    assert(index(list, "中国") == list.size());
    // Complete pinyin is decoded as before: no abbreviated candidate at all,
    // so an exact full-pinyin result is never displaced.
    for (const char *keys : {"nihao", "xian", "xi'an", "nihaoma", "zhongguo", "zhongguoren", "zhidao", "wo"}) {
        list = run(keys);
        for (size_t i = 0; i < list.size(); ++i) assert(!mpn_candidate_abbreviated(session, i));
    }
    assert(run("nihao")[0].text == "你好" && run("nihao")[0].kind == MPN_CANDIDATE_EXACT);
    assert(run("xian")[0].text == "先" && run("zhongguoren")[0].text == "中国人");
    assert(!mpn_candidate_abbreviated(nullptr, 0) && !mpn_candidate_abbreviated(session, mpn_candidate_count(session)));
    // Traditional output.
    mpn_set_simplified(session, 0);
    assert(run("zgren")[0].text == "中國人");
    mpn_set_simplified(session, 1);
    // A learned word keeps its bonus when typed by its initials (CN-06c).
    auto *personal = mpn_personal_create();
    assert(personal && mpn_personal_set(personal, 1, "nanhai", "男孩", 3));
    mpn_set_personal(session, personal);
    assert(run("nh")[0].text == "男孩" && run("nihao")[0].text == "你好");
    mpn_set_personal(session, nullptr);
    mpn_personal_destroy(personal);
    // Fuzzy pairs leave initials alone (CN-03).
    mpn_set_fuzzy_pinyin(session, MPN_FUZZY_ALL);
    assert(run("zgren")[0].text == "中国人" && run("nh")[0].text == "你好");
    mpn_set_fuzzy_pinyin(session, 0);
    // A long consonant string stays bounded.
    std::string keys;
    while (keys.size() + 2 <= mpn_max_input_bytes()) keys += "zg";
    list = run(keys.c_str());
    assert(!list.empty() && list.size() <= 256 && list[0].consumed <= keys.size());
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

// CN-05: typo correction in the core decoder.
static void verifyTypoCorrection() {
    const std::string data = "MEOWPUUT-PINYIN-1\n"
        "gou\tgou4\t夠\t够\t6000\n"
        "guo\tguo2\t國\t国\t8000\n"
        "hao\thao3\t好\t好\t9000\n"
        "men\tmen5\t們\t们\t7000\n"
        "nh\tn3h3\t你好\t你好\t1125\n"
        "ni\tni3\t你\t你\t9000\n"
        "nihao\tni3hao3\t你好\t你好\t9000\n"
        "ren\tren2\t人\t人\t8000\n"
        "wo\two3\t我\t我\t9000\n"
        "women\two3men5\t我們\t我们\t8500\n"
        "zai\tzai4\t在\t在\t9000\n"
        "zg\tz1g2\t中國\t中国\t1250\n"
        "zhong\tzhong1\t中\t中\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t10000\n"
        "zhongguoren\tzhong1guo2ren2\t中國人\t中国人\t5500\n";
    auto *lexicon = makeLexicon(data);
    auto *session = mpn_session_create(lexicon);
    assert(session && mpn_typo_correction(session) == 1);
    mpn_set_simplified(session, 1);
    const auto run = [&](const char *keys) { mpn_clear(session); type(session, keys); return listed(session); };
    const auto index = [](const std::vector<Listed> &items, const char *text) {
        size_t i = 0;
        while (i < items.size() && items[i].text != text) ++i;
        return i;
    };
    const auto typos = [&](const std::vector<Listed> &items) {
        size_t count = 0;
        for (size_t i = 0; i < items.size(); ++i) count += mpn_candidate_typo(session, i);
        return count;
    };
    // Swapped neighbours, a missing letter and a doubled letter, each read as
    // one slip that consumes every key; a correction, not learned word by word.
    for (const auto &[keys, text] : {std::pair{"zhogn", "中"}, std::pair{"zhonguo", "中国"}, std::pair{"zhonggguo", "中国"},
                                     std::pair{"nihoa", "你好"}, std::pair{"nihhao", "你好"}, std::pair{"womne", "我们"},
                                     std::pair{"wozaizhognguo", "我在中国"}}) {
        const auto list = run(keys);
        assert(list[0].text == text && list[0].kind == MPN_CANDIDATE_CORRECTION && list[0].consumed == std::strlen(keys));
        assert(mpn_candidate_typo(session, 0) && !mpn_candidate_abbreviated(session, 0) && !*mpn_candidate_words(session, 0));
    }
    assert(mpn_choose(session, 0) && std::string(mpn_committed(session)) == "我在中国" && !*mpn_input(session));
    mpn_clear_commit(session);
    // At most one slip per candidate.
    auto list = run("nihoazhogn");
    assert(index(list, "你好中") == list.size());
    // Abbreviations keep their readings; an unfinished final syllable is not
    // a slip, so the last key is never dropped (zhongg).
    list = run("zhongg");
    assert(list[0].text == "中国" && mpn_candidate_abbreviated(session, 0) && !mpn_candidate_typo(session, 0));
    assert(run("zgren")[0].text == "中国人" && run("nh")[0].text == "你好" && !mpn_candidate_typo(session, 0));
    // Typed tones turn the in-sentence correction off.
    list = run("zho1gn");
    assert(typos(list) == 0);
    // Complete syllables are read exactly: the same candidates as with the
    // switch off, apart from a common whole word one slip away that follows
    // the first three (zhong+gou -> 中国).
    mpn_set_typo_correction(session, 0);
    const auto exactOff = run("zhongguo");
    const auto gouOff = run("zhonggou");
    mpn_set_typo_correction(session, 1);
    assert(run("zhongguo") == exactOff && typos(exactOff) == 0);
    list = run("zhonggou");
    assert(!mpn_candidate_typo(session, 0) && list[0].text == gouOff[0].text);
    const size_t at = index(list, "中国");
    assert(at == std::min<size_t>(3, gouOff.size()) && mpn_candidate_typo(session, at) && list[at].kind == MPN_CANDIDATE_CORRECTION);
    for (size_t i = 0; i < at; ++i) assert(list[i] == gouOff[i]);
    // Off: no candidate rests on a slip, and the list is rebuilt at once.
    list = run("zhogn");
    assert(list[0].text == "中" && mpn_candidate_typo(session, 0));
    mpn_set_typo_correction(session, 0);
    assert(mpn_typo_correction(session) == 0 && std::string(mpn_input(session)) == "zhogn");
    list = listed(session);
    assert(typos(list) == 0);
    for (const auto &item : list) assert(item.text != "中" || item.consumed < 5);
    for (const char *keys : {"zhonguo", "nihoa", "nihhao", "wozaizhognguo"}) assert(typos(run(keys)) == 0);
    mpn_set_typo_correction(session, 1);
    assert(run("zhogn")[0].text == "中");
    assert(!mpn_candidate_typo(nullptr, 0) && !mpn_candidate_typo(session, mpn_candidate_count(session)));
    mpn_set_typo_correction(nullptr, 0);
    assert(mpn_typo_correction(nullptr) == 0);
    // The window label: corrections only, not abbreviations or exact words.
    const auto noEvidence = [](const std::string &) { return ChineseEvidence{}; };
    run("zhonguo");
    auto choices = windowsCandidates(session, {}, noEvidence, false);
    assert(choices[0].text == "中国" && typoCandidate(session, choices[0]) && std::string(typoMarker()) == "纠错");
    run("zg");
    choices = windowsCandidates(session, {}, noEvidence, false);
    assert(choices[0].text == "中国" && !typoCandidate(session, choices[0]));
    run("zhongguo");
    choices = windowsCandidates(session, {}, noEvidence, false);
    for (const auto &choice : choices) assert(!typoCandidate(session, choice));
    // Traditional output.
    mpn_set_simplified(session, 0);
    assert(run("zhonguo")[0].text == "中國");
    mpn_set_simplified(session, 1);
    // Long strings stay bounded: 39 keys still corrected, longer ones not.
    list = run("wozaizhognguowozaizhognguowozaizhognguo");
    assert(!list.empty() && list.size() <= 256);
    std::string keys;
    while (keys.size() + 5 <= mpn_max_input_bytes()) keys += "zhogn";
    list = run(keys.c_str());
    assert(list.size() <= 256 && typos(list) == 0);
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

// CN-07 batch 6: generated numbers and dates in the merge layer.
static void verifyQuickCandidates() {
    const std::string data = "MEOWPUUT-PINYIN-2\n"
        "qun\tqun2\t群\t群\t7000\t7000\nren\tren2\t人\t人\t8000\t8000\nrenqun\tren2qun2\t人群\t人群\t9000\t9000\n";
    auto *lexicon = mpn_lexicon_create(data.data(), data.size());
    auto *session = mpn_session_create(lexicon);
    assert(lexicon && session);
    const auto lookup = [](const std::string &) { return ChineseEvidence{}; };
    const quick_input::Moment now{2026, 11, 20, 14, 30, 5};
    const auto list = [&](const char *keys) {
        mpn_clear(session);
        for (const char *c = keys; *c; ++c) assert(mpn_key(session, *c));
        return windowsCandidates(session, {}, lookup, false);
    };
    // Not a trigger: nothing generated, the list is untouched.
    for (const char *keys : {"rq", "renqun", "v", "vr", "vrqa"}) {
        auto choices = list(keys), merged = choices;
        assert(quickCandidates(keys, {}, now).empty());
        mergeQuickCandidates(merged, quickCandidates(keys, {}, now));
        assert(merged.size() == choices.size());
        for (size_t i = 0; i < merged.size(); ++i) assert(merged[i].text == choices[i].text && !merged[i].generated);
    }
    // vrq: the dates first, generated, then the ordinary list in its order.
    auto choices = list("vrq");
    auto merged = choices;
    mergeQuickCandidates(merged, quickCandidates("vrq", {}, now));
    assert(merged.size() == choices.size() + 6 && merged[0].text == "2026年11月20日" && merged[0].annotation == "日期");
    for (size_t i = 0; i < 6; ++i) assert(merged[i].generated && !merged[i].coreIndex);
    for (size_t i = 0; i < choices.size(); ++i) assert(merged[6 + i].text == choices[i].text && !merged[6 + i].generated);
    // A generated text the list already had as a non-core entry is not repeated.
    std::vector<WindowsCandidate> duplicate{{"14:30", std::nullopt}, {"x", std::nullopt}};
    mergeQuickCandidates(duplicate, quickCandidates("vsj", {}, now));
    assert(std::count_if(duplicate.begin(), duplicate.end(), [](const auto &c) { return c.text == "14:30"; }) == 1);
    assert(duplicate.front().text == "14:30" && duplicate.front().generated && duplicate.back().text == "x");
    // Number mode: the forms, then what was typed; only after a lone v.
    const auto numbers = quickCandidates("v", "123", now);
    assert(numbers.size() == 6 && numbers.front().text == "一百二十三" && numbers.back().text == "v123" && numbers.back().annotation == "原样");
    assert(std::all_of(numbers.begin(), numbers.end(), [](const auto &c) { return c.generated && !c.coreIndex; }));
    assert(quickCandidates("vr", "123", now).empty() && quickCandidates("v", {}, now).empty());
    // Symbols: the same rule as vrq. Close misses and other inputs are untouched.
    for (const char *keys : {"vb", "vbdd", "vfhx", "bd", "vx"}) {
        auto plain = list(keys), combined = plain;
        mergeQuickCandidates(combined, quickCandidates(keys, {}, now));
        assert(combined.size() == plain.size());
        for (size_t i = 0; i < combined.size(); ++i) assert(combined[i].text == plain[i].text && !combined[i].generated);
    }
    for (const char *keys : {"vbd", "vxh", "vfh"}) {
        auto plain = list(keys), combined = plain;
        const auto symbols = quickCandidates(keys, {}, now);
        assert(symbols.size() == quick_input::symbolCandidates(keys).size() && symbols.size() >= 10);
        mergeQuickCandidates(combined, symbols);
        assert(combined.size() == plain.size() + symbols.size());
        for (size_t i = 0; i < symbols.size(); ++i)
            assert(combined[i].generated && !combined[i].coreIndex && combined[i].text == symbols[i].text && !combined[i].annotation.empty());
        for (size_t i = 0; i < plain.size(); ++i) assert(combined[symbols.size() + i].text == plain[i].text && !combined[symbols.size() + i].generated);
    }
    assert(quickCandidates("vxh", {}, now).front().text == "①" && quickCandidates("vxh", {}, now).front().annotation == "序号");
    assert(quickCandidates("vbd", "1", now).empty()); // number mode only after a lone v
    mpn_session_destroy(session);
    mpn_lexicon_destroy(lexicon);
}

int main(int argc, char **argv) {
    verifyQuickCandidates();
    verifyTypoCorrection();
    verifyAbbreviatedPinyin();
    verifyFuzzyPinyin();
    verifyPersonalWords();
    verifyUserWords();
    verifyLazyExpansion();
    verifyLeadingWords();
    verifyCompositionStatistics();
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
