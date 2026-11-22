#pragma once

#include "../Core/MeowpuutCore.h"
#include "QuickInput.h"
#include <algorithm>
#include <cstdlib>
#include <istream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>

struct JapaneseWord {
    std::string reading, text;
    int weight;
    std::string kana, source, chinese;
    bool theme = false;
};

inline std::vector<JapaneseWord> readJapaneseWords(std::istream &file) {
    std::vector<JapaneseWord> words;
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const auto first = line.find('|');
        if (first == line.npos) continue;
        const auto second = line.find('|', first + 1);
        if (second == line.npos) continue;
        std::vector<std::string> fields;
        std::istringstream row(line);
        for (std::string field; std::getline(row, field, '|');) fields.push_back(field);
        if (fields.size() < 3 || fields.size() > 6) continue;
        const auto &text = fields[1];
        int weight = 0;
        try {
            size_t used = 0;
            weight = std::stoi(fields[2], &used);
            if (used != fields[2].size() || weight <= 0 || weight > 10000) continue;
        } catch (...) { continue; }
        const std::string kana = fields.size() > 3 ? fields[3] : "";
        const std::string source = fields.size() > 4 ? fields[4] : "";
        const std::string chinese = fields.size() > 5 ? fields[5] : "";
        if (text.empty() || text.size() > 512 || kana.size() > 512 || chinese.size() > 512 ||
            (source != "" && source != "pinyin" && source != "english")) continue;
        if (text.find_first_of("\t\r") != text.npos || kana.find_first_of("\t\r") != kana.npos ||
            chinese.find_first_of("\t\r") != chinese.npos) continue;
        for (size_t begin = 0; begin < first;) {
            const auto comma = std::min(line.find(',', begin), first);
            const auto reading = line.substr(begin, comma - begin);
            if (!reading.empty() && reading.size() <= 256 &&
                std::all_of(reading.begin(), reading.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || c == '\'' || c == '-';
                })) words.push_back({reading, text, weight, kana, source, chinese});
            begin = comma + 1;
        }
    }
    std::stable_sort(words.begin(), words.end(), [](const auto &a, const auto &b) {
        return a.weight > b.weight;
    });
    return words;
}

using ChineseEvidence = std::map<std::string, int>;

// Query both valid spellings without treating an explicit lu'e as lüe.
inline std::vector<std::string> pinyinEvidenceReadings(const std::string &input) {
    auto canonical = input;
    for (size_t i = 0; i + 2 < canonical.size(); ++i)
        if ((canonical[i] == 'l' || canonical[i] == 'n') && canonical[i + 1] == 'u' && canonical[i + 2] == 'e')
            canonical[i + 1] = 'v';
    auto withoutSeparators = [](std::string reading) {
        reading.erase(std::remove_if(reading.begin(), reading.end(), [](char c) {
            return c == '\'' || (c >= '1' && c <= '5');
        }), reading.end());
        return reading;
    };
    auto original = withoutSeparators(input);
    canonical = withoutSeparators(std::move(canonical));
    if (canonical == original) return {std::move(original)};
    return {std::move(original), std::move(canonical)};
}

// Pinyin evidence is indexed without delimiters; verify explicit boundaries
// and tones against the original reading before treating it as exact.
inline bool pinyinEvidenceMatches(const std::string &input, const std::string &toned) {
    if (input.find_first_of("'12345") == std::string::npos) return true;
    std::map<size_t, char> required;
    size_t position = 0;
    for (const char c : input) {
        if (c >= 'a' && c <= 'z') ++position;
        else if (c == '\'' || (c >= '1' && c <= '5')) {
            if (!position || !required.emplace(position, c).second) return false;
        } else return false;
    }
    if (required.empty()) return true;
    position = 0;
    for (const char c : toned) {
        if (c >= 'a' && c <= 'z') ++position;
        else if (c >= '1' && c <= '5') {
            const auto item = required.find(position);
            if (item != required.end()) {
                if (item->second != '\'' && item->second != c) return false;
                required.erase(item);
            }
        } else return false;
    }
    return required.empty();
}

struct WindowsCandidate {
    std::string text;
    std::optional<size_t> coreIndex;
    bool japanese = false;
    bool strong = false;
    std::string annotation;
    bool directJapanese = false; // Explicit bridge choices stay visible when collapsed.
    bool generated = false;      // CN-07: a number, date, time or symbol; never learned.
    // WIN-03 step 7c: self-contained metadata supplied by the standalone engine
    // so thin-client selection, forgetting, picking and display need no local lexicon.
    size_t consumed = 0;         // 0: unset (read from local session / full input).
    size_t literalTail = 0;      // Trailing literal bytes shown in candidate text, omitted on commit.
    bool typo = false;           // CN-05: typo correction marker.
    bool personal = false;       // DICT-02b: user-added personal word.
    uint64_t engineId = 0;       // Non-zero when produced by the standalone engine.
};

// CN-07 batch 6: the generated candidates of the merge layer. In number
// mode (v then digits, kept by the host beside the core's "v") they are the
// whole list, followed by what was typed; for an exact vrq / vsj / vxq they
// come first and the ordinary candidates follow; so do the symbols of an
// exact symbol trigger (vfh, vbd, vsx ...: QuickInput.h). Nothing else
// changes, so a-z input other than the triggers is ranked exactly as before.
inline std::vector<WindowsCandidate> quickCandidates(const std::string &input, const std::string &number, const quick_input::Moment &now) {
    std::vector<WindowsCandidate> result;
    const auto items = !number.empty() && input == "v" ? quick_input::numberCandidates(number)
        : !number.empty() ? std::vector<quick_input::Item>{}
        : quick_input::trigger(input) ? quick_input::dateCandidates(input, now) : quick_input::symbolCandidates(input);
    for (const auto &item : items) {
        WindowsCandidate candidate{item.text, {}};
        candidate.annotation = item.label;
        candidate.generated = true;
        result.push_back(std::move(candidate));
    }
    if (!number.empty() && input == "v") {
        WindowsCandidate raw{input + number, {}};
        raw.annotation = "原样";
        raw.generated = true;
        result.push_back(std::move(raw));
    }
    return result;
}

inline void mergeQuickCandidates(std::vector<WindowsCandidate> &choices, std::vector<WindowsCandidate> quick) {
    if (quick.empty()) return;
    // A generated text the list already has keeps only its first place.
    choices.erase(std::remove_if(choices.begin(), choices.end(), [&](const WindowsCandidate &c) {
        return std::any_of(quick.begin(), quick.end(), [&](const WindowsCandidate &q) { return q.text == c.text && !c.coreIndex; });
    }), choices.end());
    choices.insert(choices.begin(), std::make_move_iterator(quick.begin()), std::make_move_iterator(quick.end()));
}

// CN-05: the label shown on the reading line of a core candidate that rests
// on a typo correction (mpn_candidate_typo: in-sentence and whole-input
// corrections, including the older keyboard-neighbour and n/ng ones the same
// switch governs). Fuzzy pinyin and abbreviations are readings the user chose
// to type and carry no label. Display only: never part of committed text.
inline const char *typoMarker() { return "\xe7\xba\xa0\xe9\x94\x99"; } // 纠错 in UTF-8, whatever the source charset
inline bool typoCandidate(const MPNSession *session, const WindowsCandidate &candidate) {
    return candidate.typo || (!candidate.engineId && candidate.coreIndex && mpn_candidate_typo(session, *candidate.coreIndex));
}

inline void appendCandidateExpansion(MPNSession *session, std::vector<WindowsCandidate> &current,
                                     const std::vector<WindowsCandidate> &more) {
    std::set<std::tuple<std::string, size_t, std::string>> seen;
    const auto key = [&](const WindowsCandidate &c) {
        return std::make_tuple(c.text, c.coreIndex ? mpn_candidate_consumed(session, *c.coreIndex) : std::string(mpn_input(session)).size(), c.annotation);
    };
    for (const auto &c : current) seen.insert(key(c));
    for (const auto &c : more) if (seen.insert(key(c)).second) current.push_back(c);
}

// Mirrors the macOS mixed-reading gate: one exact Japanese word surrounded by
// complete, high-confidence Chinese dictionary readings.
template<class Lookup>
std::string mixedReading(const std::string &input, const std::vector<JapaneseWord> &dictionary, Lookup lookup) {
    if (input.size() < 5 || input.size() > 24 ||
        !std::all_of(input.begin(), input.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
        return {};
    std::unordered_map<std::string, const JapaneseWord *> japanese;
    // Explicit bridge aliases are whole-word lookups until Japanese sentence
    // decoding exists; do not treat their editorial priority as mixed evidence.
    for (const auto &word : dictionary)
        if (word.source.empty()) japanese.emplace(word.reading, &word);
    if (japanese.count(input)) return {};

    using Evidence = std::optional<std::pair<std::string, int>>;
    std::unordered_map<std::string, Evidence> cache;
    auto bestChinese = [&](const std::string &reading) -> Evidence {
        if (reading.empty()) return std::make_pair(std::string(), 0);
        const auto found = cache.find(reading);
        if (found != cache.end()) return found->second;
        Evidence best;
        for (const auto &[text, weight] : lookup(reading)) {
            if (weight >= 2000 && (!best || weight > best->second))
                best = std::make_pair(text, weight);
        }
        cache.emplace(reading, best);
        return best;
    };

    int bestScore = -1;
    std::string mixed;
    for (size_t start = 0; start + 3 <= input.size(); ++start) {
        for (size_t end = start + 3; end <= std::min(input.size(), start + 10); ++end) {
            const auto match = japanese.find(input.substr(start, end - start));
            if (match == japanese.end() || match->second->weight < 4000) continue;
            const auto before = bestChinese(input.substr(0, start));
            const auto after = bestChinese(input.substr(end));
            if (!before || !after) continue;
            const int score = before->second + match->second->weight + after->second;
            if (score > bestScore) {
                bestScore = score;
                mixed = before->first + match->second->text + after->first;
            }
        }
    }
    return mixed;
}

inline std::vector<WindowsCandidate> interleaveCandidates(std::vector<WindowsCandidate> choices) {
    if (std::none_of(choices.begin(), choices.end(), [](const auto &choice) { return choice.japanese; }))
        return choices;
    std::vector<WindowsCandidate> strong, weak, japanese, direct;
    for (auto &choice : choices)
        (choice.directJapanese ? direct : choice.japanese ? japanese : choice.strong ? strong : weak).push_back(std::move(choice));
    const size_t lead = strong.empty() ? std::min(size_t(2), weak.size())
        : std::min(direct.empty() ? size_t(6) : size_t(4), strong.size());
    strong.insert(strong.end(), std::make_move_iterator(weak.begin()), std::make_move_iterator(weak.end()));
    std::vector<WindowsCandidate> result;
    result.insert(result.end(), std::make_move_iterator(strong.begin()), std::make_move_iterator(strong.begin() + lead));
    // Keep the first explicit spelling/reading pair together on the first page.
    result.insert(result.end(), std::make_move_iterator(direct.begin()), std::make_move_iterator(direct.end()));
    for (size_t i = 0; i < japanese.size() || i + lead < strong.size(); ++i) {
        if (i < japanese.size()) result.push_back(std::move(japanese[i]));
        if (i + lead < strong.size()) result.push_back(std::move(strong[i + lead]));
    }
    return result;
}

template<class Lookup>
std::vector<WindowsCandidate> windowsCandidates(MPNSession *session, const std::vector<JapaneseWord> &dictionary,
                                                 Lookup lookup, bool chineseLocked) {
    const std::string input = mpn_input(session);
    if (input.empty()) return {};
    std::vector<WindowsCandidate> choices;
    std::set<std::tuple<std::string, size_t, std::string>> seen;
    const auto evidence = chineseLocked ? ChineseEvidence{} : lookup(input);
    auto append = [&](std::string text, std::optional<size_t> index, bool japanese, std::string annotation = {}, bool direct = false) {
        const size_t consumed = index ? mpn_candidate_consumed(session, *index) : input.size();
        if (text.empty() || !seen.emplace(text, consumed, annotation).second)
            return;
        const auto found = evidence.find(text);
        const bool strong = found != evidence.end() && found->second >= 2000;
        const bool explicitJapanese = !annotation.empty();
        choices.push_back({std::move(text), index, japanese && (explicitJapanese || found == evidence.end()),
                           strong && !explicitJapanese, std::move(annotation), direct});
    };

    std::vector<size_t> chinese;
    std::set<std::pair<std::string, size_t>> nativeSelections;
    for (size_t i = 0; i < mpn_candidate_count(session); ++i) {
        chinese.push_back(i);
        nativeSelections.emplace(mpn_candidate(session, i), mpn_candidate_consumed(session, i));
    }
    // CN-03: a fuzzy pinyin candidate that the core ranked ahead of an
    // evidence-backed one keeps its place: the core already compared it with
    // the exact words of the input. Without fuzzy candidates nothing changes.
    size_t lastEvidence = 0;
    for (size_t i = 0; i < chinese.size(); ++i)
        if (evidence.count(mpn_candidate(session, i))) lastEvidence = i + 1;
    // DICT-02b: a word the user added for exactly this input counts as backed,
    // so evidence for other words cannot push it off the lead the core gave it.
    const auto backed = [&](size_t i) {
        return evidence.count(mpn_candidate(session, i)) != 0 || (i + 1 < lastEvidence && mpn_candidate_fuzzy(session, i)) ||
            (mpn_candidate_personal(session, i) && mpn_candidate_consumed(session, i) == input.size());
    };
    std::stable_partition(chinese.begin(), chinese.end(), backed);
    if (chineseLocked) {
        for (auto i : chinese) append(mpn_candidate(session, i), i, false);
        return choices;
    }

    std::vector<std::pair<std::string, int>> additional;
    for (const auto &entry : evidence) {
        if (!nativeSelections.count({entry.first, input.size()})) additional.push_back(entry);
    }
    std::stable_sort(additional.begin(), additional.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    const bool supplementFirst = chinese.empty() || !backed(chinese.front());
    auto supplement = [&] {
        for (size_t i = 0; i < std::min(size_t(7), additional.size()); ++i) append(additional[i].first, {}, false);
    };
    if (supplementFirst) supplement();
    for (size_t i = 0; i < std::min(size_t(2), chinese.size()); ++i)
        append(mpn_candidate(session, chinese[i]), chinese[i], false);
    append(mixedReading(input, dictionary, lookup), {}, true);
    auto appendWord = [&](const JapaneseWord &word) {
        std::string annotation = word.kana;
        if (word.source == "english") annotation += " · EN " + word.reading;
        else if (word.source == "pinyin") annotation += " · 拼";
        if (!word.chinese.empty()) annotation += " · " + word.chinese;
        const bool direct = !word.source.empty();
        append(word.text, {}, true, std::move(annotation), direct);
        if (direct && !word.kana.empty() && word.kana != word.text)
            append(word.kana, {}, true, word.text + " · 假名", true);
    };
    const auto firstJapanese = std::find_if(dictionary.begin(), dictionary.end(), [&](const auto &word) {
        return word.reading == input;
    });
    if (firstJapanese != dictionary.end()) appendWord(*firstJapanese);
    if (input.size() <= 3) append(input, {}, false);
    if (chinese.size() > 2) append(mpn_candidate(session, chinese[2]), chinese[2], false);
    if (!supplementFirst) supplement();
    for (size_t i = 3; i < chinese.size(); ++i) append(mpn_candidate(session, chinese[i]), chinese[i], false);
    for (size_t i = 7; i < additional.size(); ++i) append(additional[i].first, {}, false);
    for (const auto &word : dictionary)
        if (word.reading == input) appendWord(word);
    if (std::any_of(dictionary.begin(), dictionary.end(), [&](const auto &word) {
        return word.reading == input && word.source == "english";
    })) append(input, {}, false);
    if (choices.empty()) append(input, {}, false);
    auto result = interleaveCandidates(std::move(choices));
    std::vector<WindowsCandidate> names;
    std::set<std::string> included;
    for (const auto &word : dictionary) {
        if (!word.theme || word.reading != input || word.chinese.empty()) continue;
        const std::string chineseName = mpn_custom_chinese(session, word.chinese.c_str());
        if (!included.insert(chineseName).second) continue;
        const auto existing = std::find_if(result.begin(), result.end(), [&](const auto &c) {
            return !c.japanese && c.text == chineseName && (!c.coreIndex || mpn_candidate_consumed(session, *c.coreIndex) == input.size());
        });
        // A theme name can already exist as a low-ranked composed path.
        // Promote that selection too, retaining its native consumption index.
        if (existing != result.end()) {
            auto promoted = *existing;
            promoted.strong = true;
            names.push_back(std::move(promoted));
            result.erase(existing);
        } else names.push_back({chineseName, {}, false, true, "主题词库 · 中文", false});
    }
    if (!names.empty()) {
        // Keep ordinary high-confidence Chinese first. A recorded theme name
        // precedes speculative spelling paths, with room for the Japanese pair.
        auto position = std::find_if(result.begin(), result.end(), [&](const auto &c) {
            if (c.japanese) return true;
            if (c.strong) return false;
            if (c.coreIndex) {
                MPNCandidateInfo info{};
                if (mpn_candidate_info(session, *c.coreIndex, &info, sizeof(info)) && info.consumed_bytes == input.size()) {
                    // An exact dictionary reading keeps priority even with a low
                    // source frequency. Also retain a complete phrase when the
                    // theme expands it (辛苦了 -> 打得好！辛苦了！).
                    if (info.kind == MPN_CANDIDATE_EXACT ||
                        (info.kind == MPN_CANDIDATE_PATH && names.front().text.find(c.text) != std::string::npos)) return false;
                }
            }
            return true;
        });
        result.insert(position, std::move(names.front()));
        result.insert(result.end(), std::make_move_iterator(names.begin() + 1), std::make_move_iterator(names.end()));
    }
    return result;
}
