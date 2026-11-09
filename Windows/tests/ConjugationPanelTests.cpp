#include "../ConjugationPanel.h"
#include <cassert>
#include <iostream>

int main(int argc, char **argv) {
    assert(argc == 2);
    JapaneseConjugations table; assert(table.loadFile(argv[1]));
    std::vector<JapaneseWord> words; appendConjugationWords(table, words);
    assert(!words.empty());
    const std::string data = "MEOWPUUT-PINYIN-2\nchi\tchi1\t吃\t吃\t9000\t9000\nhao\thao3\t好\t好\t9000\t9000\n";
    auto *lexicon = mpn_lexicon_create(data.data(), data.size()); assert(lexicon);
    auto *session = mpn_session_create(lexicon); assert(session);
    mpn_set_simplified(session, 1);
    for (char c : std::string("chi")) assert(mpn_key(session, c));
    auto lookup = [](const std::string &) { return ChineseEvidence{}; };
    auto choices = windowsCandidates(session, words, lookup, false);
    size_t index = 0;
    while (index < choices.size() && choices[index].text != "食べる") ++index;
    assert(index < choices.size());
    size_t page = index / 7, highlighted = index % 7; bool expanded = false;
    const auto original = choices;
    ConjugationPanel panel;
    assert(panel.open(table, choices, "chi", page, highlighted, expanded));
    assert(panel.active() && expanded && page == 0 && highlighted == 0);
    assert(choices.size() == 10);
    assert(choices[1].text == "食べます");
    assert(choices[1].annotation == "たべます · ます形");
    assert(std::string(mpn_input(session)) == "chi" && std::string(mpn_committed(session)).empty());
    assert(!panel.open(table, choices, "chi", page, highlighted, expanded));
    panel.close(choices, page, highlighted, expanded);
    assert(!panel.active() && !expanded && page == index / 7 && highlighted == index % 7);
    assert(choices.size() == original.size());
    for (size_t i = 0; i < choices.size(); ++i) assert(choices[i].text == original[i].text && choices[i].coreIndex == original[i].coreIndex);
    // Only exact whole-input triggers are eligible. Partial/mixed input is untouched.
    assert(!panel.open(table, choices, "chihao", page, highlighted, expanded));
    assert(std::string(mpn_input(session)) == "chi");
    auto paginated = choices;
    const auto eligible = choices[index];
    paginated.assign(16, WindowsCandidate{"placeholder", {}, false, false, ""});
    paginated[15] = eligible;
    size_t laterPage = 2, laterHighlight = 1; bool wasExpanded = true;
    assert(panel.open(table, paginated, "chi", laterPage, laterHighlight, wasExpanded));
    laterPage = 1; laterHighlight = 2;
    panel.close(paginated, laterPage, laterHighlight, wasExpanded);
    assert(laterPage == 2 && laterHighlight == 1 && wasExpanded);
    assert(paginated[15].text == "食べる");
    for (char c : std::string("hao")) assert(mpn_key(session, c));
    auto mixedChoices = windowsCandidates(session, words, lookup, false);
    for (const auto &choice : mixedChoices) assert(!conjugationFor(table, choice, "chihao"));
    assert(std::string(mpn_input(session)) == "chihao");
    mpn_clear(session);
    for (char c : std::string("chi")) assert(mpn_key(session, c));
    auto chinese = WindowsCandidate{"吃", 0, false, true, ""};
    assert(!conjugationFor(table, chinese, "chi"));
    auto wrongReading = WindowsCandidate{"食べる", {}, true, false, "wrong · 拼"};
    assert(!conjugationFor(table, wrongReading, "chi"));
    assert(panel.open(table, choices, "chi", page, highlighted, expanded));
    // Whole-input forms use the service existing non-core candidate commit path.
    const auto selected = choices[2].text;
    assert(selected == "食べて" && !choices[2].coreIndex);
    mpn_clear(session); panel.clear();
    assert(!panel.active() && std::string(mpn_input(session)).empty());
    for (char c : std::string("hao")) assert(mpn_key(session, c));
    assert(std::string(mpn_candidate(session, 0)) == "好");
    mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
    std::cout << "PASS: real candidates, panel entry/return, exact-trigger boundary and subsequent composition\n";
}
