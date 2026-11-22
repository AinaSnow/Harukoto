#include "MeowpuutCore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view tones = " 6347";
constexpr std::string_view keys = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/- 6347'";
constexpr std::array<std::string_view, 43> symbols = {
    "ㄅ","ㄆ","ㄇ","ㄈ","ㄉ","ㄊ","ㄋ","ㄌ","ㄍ","ㄎ","ㄏ","ㄐ","ㄑ","ㄒ","ㄓ","ㄔ","ㄕ","ㄖ","ㄗ","ㄘ","ㄙ",
    "ㄧ","ㄨ","ㄩ","ㄚ","ㄛ","ㄜ","ㄝ","ㄞ","ㄟ","ㄠ","ㄡ","ㄢ","ㄣ","ㄤ","ㄥ","ㄦ","ˉ","ˊ","ˇ","ˋ","˙"," "
};
static_assert(keys.size() == symbols.size());
constexpr size_t page_size = 7;
constexpr size_t max_input = 256;
constexpr size_t candidate_limit = 256;
// A single syllable can have hundreds of homophones. The regular sentence
// limit remains small; only exact single-character lookup may use this bound.
constexpr size_t single_character_candidate_limit = 2048;
constexpr size_t beam = 5;
// Sentence composition with corpus statistics (the optional "# statistics" trailer).
// Lexicon weights, corrections and collocation scores share one scale of nats,
// so a collocation counts in full; the cap keeps one pair from outvoting
// everything else in the sentence.
constexpr double pair_cap = 6.0;
// With real adjacency statistics available, a boundary that merely spells a
// dictionary word is weak evidence, while an editorial context phrase is a
// deliberate statement and outweighs a typical collocation.
constexpr double derived_join_scale = 0.5, authored_join_scale = 1.5;
// CN-06c: personal word preferences supplied by the host, in nats on the same
// scale as word scores. The cap bounds what any learned word can gain; above
// the best competing word of the same reading only a fifth of the bonus
// counts, so learning reorders homophones and moves a reading's best score by
// at most 0.6 nats, which cannot split a longer dictionary word apart.
constexpr double personal_cap = 3.0, personal_lead = 0.2;
// CN-04: abbreviated pinyin reaches dictionary words of at most this many syllables.
constexpr size_t abbrev_max_syllables = 8;

bool is_tone(char key, bool pinyin = false) { return pinyin ? key >= '1' && key <= '5' : tones.find(key) != std::string_view::npos; }

// Context must contain only Han text. Punctuation, emoji, ASCII and malformed
// UTF-8 end the local phrase rather than carrying it into another composition.
std::vector<std::string_view> han_characters(std::string_view text) {
    std::vector<std::string_view> result;
    for (size_t start = 0; start < text.size();) {
        const auto first = static_cast<unsigned char>(text[start]);
        const size_t length = first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!length || start + length > text.size()) return {};
        uint32_t scalar = first & (length == 3 ? 0x0f : 0x07);
        for (size_t i = 1; i < length; ++i) {
            const auto next = static_cast<unsigned char>(text[start + i]);
            if ((next & 0xc0) != 0x80) return {};
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((length == 4 && scalar < 0x10000) || !((scalar >= 0x3400 && scalar <= 0x9fff) || (scalar >= 0xf900 && scalar <= 0xfaff) ||
              (scalar >= 0x20000 && scalar <= 0x323af))) return {};
        result.push_back(text.substr(start, length));
        start += length;
    }
    return result;
}

struct Word {
    std::string_view row;
    int weight;
    int hansWeight;
    // Index of the simplified text in the collocation vocabulary; 0 = absent.
    uint32_t pair = 0;
    // Sentence-composition prior correction in hundredths of a nat; 0 = none.
    int16_t prior = 0;

    int score(bool hans) const { return hans ? hansWeight : weight; }

    std::string_view reading() const { return row.substr(0, row.find('\t')); }
    std::string_view text(bool hans) const {
        const auto start = row.find('\t') + 1;
        const auto separator = row.find('\t', start);
        return hans ? row.substr(separator + 1) : row.substr(start, separator - start);
    }
};

bool fully_spelled(const Word &word) {
    bool vowel = false;
    for (char key : word.reading()) {
        if (is_tone(key, true)) {
            if (!vowel) return false;
            vowel = false;
        } else if (std::string_view("aeiouv").find(key) != std::string_view::npos) {
            vowel = true;
        }
    }
    return true;
}

struct Lexicon {
    // The table is immutable after loading. Row and index views point into it.
    std::string storage;
    std::vector<Word> words;
    // Bucket offsets remain shared, but simplified frequency can order the
    // same readings differently without disturbing traditional candidates.
    std::vector<uint32_t> hansOrder;
    struct Bucket { std::string_view key; size_t start; };
    std::vector<Bucket> buckets;
    // Sorted independent syllable evidence; frequency and abbreviation rows
    // must not decide whether an input is a complete Pinyin syllable.
    std::vector<std::string_view> singleReadings, singleSpellings;
    struct ContextScore { std::string_view first; int weight, hansWeight; };
    std::vector<ContextScore> context;
    // Parallel to context when the statistics list authored phrases: true for
    // an editorial phrase rather than a dictionary word that happens to span
    // a boundary. Empty otherwise.
    std::vector<bool> contextAuthored;
    std::unordered_map<std::string_view, std::pair<std::string_view, int>> traditionalCharacters;
    std::unordered_map<std::string_view, std::pair<std::string_view, int>> traditionalPhrases;
    size_t longestTraditionalPhrase = 0;
    size_t longest = 0;
    bool pinyin = false;
    // DICT-02b: identifies this lexicon to caches derived from it (user words).
    const uint64_t serial = next_serial();
    static uint64_t next_serial() { static std::atomic<uint64_t> counter{0}; return ++counter; }
    std::array<std::vector<size_t>, 128> initials;
    std::array<std::vector<size_t>, 128> hansInitials;
    // CN-04: fully spelled words of 2..abbrev_max_syllables syllables, keyed
    // by the first letters of their syllables (zhong1guo2 -> "zg") packed into
    // five bits per letter, most significant first, so that every key that
    // begins with some letters forms one contiguous range. Sorted by key, then
    // by the higher of the two script weights. Parallel arrays.
    static constexpr size_t abbrevLetters = 12;
    std::vector<uint64_t> abbrevKeys;
    std::vector<uint32_t> abbrevWords;

    static uint64_t abbrev_letter(char letter, size_t position) {
        return uint64_t(letter - 'a' + 1) << (5 * (abbrevLetters - 1 - position));
    }
    // Index range of the words whose key is key over letters letters
    // (prefix: or begins with it).
    std::pair<size_t, size_t> abbrev_range(uint64_t key, size_t letters, bool prefix) const {
        const uint64_t last = prefix ? key + (uint64_t(1) << (5 * (abbrevLetters - letters))) : key + 1;
        const auto first = std::lower_bound(abbrevKeys.begin(), abbrevKeys.end(), key);
        const auto end = std::lower_bound(first, abbrevKeys.end(), last);
        return {static_cast<size_t>(first - abbrevKeys.begin()), static_cast<size_t>(end - abbrevKeys.begin())};
    }
    // Adjacent-word collocations from the optional statistics: scores in
    // hundredths of a nat (discounted pointwise mutual information), keyed by
    // the two words' collocation indices. Derived from sentence corpora; an
    // unlisted pair simply earns nothing. Keys are sorted; scores run in
    // parallel so that a million pairs cost ten bytes each.
    std::vector<uint64_t> pairs;
    std::vector<uint16_t> pairScores;
    // Collocation indices of the phrase-boundary markers "^" and "$"; 0 = absent.
    uint32_t pairBegin = 0, pairEnd = 0;

    double pair_bonus(uint32_t left, uint32_t right) const {
        if (!left || !right) return 0;
        const uint64_t key = (uint64_t(left) << 32) | right;
        const auto found = std::lower_bound(pairs.begin(), pairs.end(), key);
        return found != pairs.end() && *found == key ? std::min(pair_cap, pairScores[found - pairs.begin()] / 100.0) : 0;
    }

    const Word &word(size_t position, bool hans) const {
        return words[hans && !hansOrder.empty() ? hansOrder[position] : position];
    }

    // Optional trailer written by the lexicon compiler from sentence-corpus
    // statistics. Sections appear in this order, each sorted ascending:
    //   # authored   context phrases that are editorial, one per line
    //   # priors     word<TAB>correction (hundredths of a nat, non-zero)
    //   # pairwords  words taking part in a pair; a word's index is its line
    //   # pairs      left index<TAB>right index<TAB>score (1..999)
    // "^" and "$" in pairwords stand for the start and end of a phrase. The
    // text is read from the caller's buffer and not retained.
    bool loadStatistics(std::string_view data) {
        enum Section { None, Authored, Priors, PairWords, Pairs } section = None;
        struct Tag { uint32_t pair = 0; int16_t prior = 0; };
        std::unordered_map<std::string_view, Tag> tags;
        uint32_t pairWords = 0;
        std::string_view previous;
        while (!data.empty() && section != Pairs) {
            const auto newline = data.find('\n');
            auto line = data.substr(0, newline);
            if (newline == data.npos) data = {};
            else data.remove_prefix(newline + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;
            if (line[0] == '#') {
                const Section next = line == "# authored" ? Authored : line == "# priors" ? Priors :
                    line == "# pairwords" ? PairWords : line == "# pairs" ? Pairs : None;
                if (next <= section) return false;
                section = next;
                previous = {};
                continue;
            }
            const auto tab = line.find('\t');
            const auto word = line.substr(0, tab);
            if (section == None || word.empty() || (!previous.empty() && previous >= word)) return false;
            previous = word;
            if (section == Priors) {
                int delta = 0;
                if (tab == line.npos) return false;
                const auto parsed = std::from_chars(line.data() + tab + 1, line.data() + line.size(), delta);
                if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size() || delta == 0 || delta < -999 || delta > 999) return false;
                tags[word].prior = static_cast<int16_t>(delta);
            } else if (tab != line.npos) {
                return false;
            } else if (section == PairWords) {
                tags[word].pair = ++pairWords;
            } else {
                const auto entry = std::lower_bound(context.begin(), context.end(), word,
                    [](const ContextScore &item, std::string_view value) { return item.first < value; });
                if (entry == context.end() || entry->first != word) return false;
                contextAuthored.resize(context.size());
                contextAuthored[entry - context.begin()] = true;
            }
        }
        // The pair rows are the last and by far the largest section: three
        // unsigned numbers per line, read in one pass.
        const auto rows = static_cast<size_t>(std::count(data.begin(), data.end(), '\n')) + 1;
        pairs.reserve(rows); pairScores.reserve(rows);
        const char *at = data.data(), *const end = at + data.size();
        const auto number = [&](uint32_t limit, char terminator, uint32_t &value) {
            const char *const first = at;
            for (value = 0; at != end && *at >= '0' && *at <= '9' && at - first < 9; ++at) value = value * 10 + static_cast<uint32_t>(*at - '0');
            if (at == first || value < 1 || value > limit) return false;
            if (terminator == '\n') {
                if (at != end && *at == '\r') ++at;
                if (at == end) return true;
            }
            return at != end && *at++ == terminator;
        };
        while (at != end) {
            if (*at == '\n' || *at == '\r') { ++at; continue; }
            uint32_t left = 0, right = 0, score = 0;
            if (!number(pairWords, '\t', left) || !number(pairWords, '\t', right) || !number(999, '\n', score)) return false;
            const uint64_t key = (uint64_t(left) << 32) | right;
            if (!pairs.empty() && pairs.back() >= key) return false;
            pairs.push_back(key); pairScores.push_back(static_cast<uint16_t>(score));
        }
        if (const auto found = tags.find("^"); found != tags.end()) pairBegin = found->second.pair;
        if (const auto found = tags.find("$"); found != tags.end()) pairEnd = found->second.pair;
        if (!tags.empty()) for (auto &word : words) {
            const auto found = tags.find(word.text(true));
            if (found != tags.end()) { word.prior = found->second.prior; word.pair = found->second.pair; }
        }
        return true;
    }

    bool load(std::string_view data) {
        // Statistics are parsed from the caller's buffer and never stored.
        constexpr std::string_view statisticsMarker = "\n# statistics\n";
        const auto statisticsStart = data.find(statisticsMarker);
        const auto statistics = statisticsStart == data.npos ? std::string_view() : data.substr(statisticsStart + statisticsMarker.size());
        storage.assign(statisticsStart == data.npos ? data : data.substr(0, statisticsStart + 1));
        data = storage;
        const bool version2 = data.substr(0, std::string_view("MEOWPUUT-PINYIN-2\n").size()) == "MEOWPUUT-PINYIN-2\n" ||
                              data.substr(0, std::string_view("MEOWPUUT-ZHUYIN-2\n").size()) == "MEOWPUUT-ZHUYIN-2\n";
        pinyin = data.substr(0, std::string_view("MEOWPUUT-PINYIN-1\n").size()) == "MEOWPUUT-PINYIN-1\n" ||
                 data.substr(0, std::string_view("MEOWPUUT-PINYIN-2\n").size()) == "MEOWPUUT-PINYIN-2\n";
        const std::string_view header = pinyin ? (version2 ? "MEOWPUUT-PINYIN-2\n" : "MEOWPUUT-PINYIN-1\n")
                                                : (version2 ? "MEOWPUUT-ZHUYIN-2\n" : "MEOWPUUT-ZHUYIN-1\n");
        if (data.substr(0, header.size()) != header) return false;
        data.remove_prefix(header.size());
        // Word rows end where the context section begins.
        const auto contextStart = data.find("\n# context\n");
        words.reserve(std::count(data.begin(), contextStart == data.npos ? data.end() : data.begin() + contextStart + 1, '\n'));
        bool reading_context = false;
        while (!data.empty()) {
            const auto newline = data.find('\n');
            auto line = data.substr(0, newline);
            if (newline == data.npos) data = {};
            else data.remove_prefix(newline + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;
            if (line == "# context" && !reading_context) {
                reading_context = true;
                continue;
            }
            const auto first_tab = line.find('\t');
            if (first_tab == line.npos || first_tab == 0) return false;
            const auto bare = line.substr(0, first_tab);
            if (reading_context) {
                const auto second_tab = line.find('\t', first_tab + 1);
                if ((!version2 && second_tab != line.npos) ||
                    (second_tab != line.npos && line.find('\t', second_tab + 1) != line.npos) ||
                    (!context.empty() && context.back().first >= bare)) return false;
                const auto parseScore = [](std::string_view value, int &score) {
                    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), score);
                    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && score >= 0;
                };
                const auto scoreEnd = second_tab == line.npos ? line.size() : second_tab;
                int weight = 0, hansWeight = 0;
                if (!parseScore(line.substr(first_tab + 1, scoreEnd - first_tab - 1), weight)) return false;
                hansWeight = weight;
                if (second_tab != line.npos && !parseScore(line.substr(second_tab + 1), hansWeight)) return false;
                context.push_back({bare, weight, hansWeight});
                continue;
            }
            const auto last_tab = line.rfind('\t');
            const auto weight_tab = version2 ? line.rfind('\t', last_tab - 1) : last_tab;
            if (weight_tab == line.npos || (version2 && weight_tab <= first_tab)) return false;
            const auto number = line.substr(weight_tab + 1, (version2 ? last_tab : line.size()) - weight_tab - 1);
            int weight = 0;
            const auto end = number.data() + number.size();
            const auto parsed = std::from_chars(number.data(), end, weight);
            if (parsed.ec != std::errc{} || parsed.ptr != end || weight < 0) return false;

            if (weight_tab == first_tab) return false;
            int hansWeight = weight;
            if (version2) {
                const auto number = line.substr(last_tab + 1);
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), hansWeight);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || hansWeight < 0) return false;
            }
            const auto row = line.substr(first_tab + 1, weight_tab - first_tab - 1);
            const Word word{row, weight, hansWeight};
            const auto reading = word.reading();
            const auto first_separator = row.find('\t');
            const auto second_separator = row.find('\t', first_separator + 1);
            if (reading.empty() || first_separator == row.npos || second_separator == row.npos ||
                first_separator + 1 == second_separator || second_separator + 1 == row.size() ||
                row.find('\t', second_separator + 1) != row.npos ||
                !is_tone(reading.back(), pinyin)) return false;
            size_t position = 0;
            bool has_sound = false;
            for (char key : reading) {
                if (is_tone(key, pinyin)) {
                    if (!has_sound) return false;
                    has_sound = false;
                } else {
                    if (position == bare.size() || key != bare[position++] ||
                        (pinyin ? key < 'a' || key > 'z' : keys.find(key) == keys.npos) || key == '\'') return false;
                    has_sound = true;
                }
            }
            if (position != bare.size() || bare.size() > max_input) return false;
            if (buckets.empty() || buckets.back().key != bare) {
                if (!buckets.empty() && buckets.back().key > bare) return false;
                buckets.push_back({bare, words.size()});
                longest = std::max(longest, bare.size());
            } else if (words.back().weight < weight) return false;
            words.push_back(word);
        }
        {
            if (version2) {
                if (words.size() > UINT32_MAX) return false;
                hansOrder.resize(words.size());
                std::iota(hansOrder.begin(), hansOrder.end(), uint32_t{0});
                for (size_t i = 0; i < buckets.size(); ++i) {
                    const size_t end = i + 1 == buckets.size() ? words.size() : buckets[i + 1].start;
                    std::stable_sort(hansOrder.begin() + buckets[i].start, hansOrder.begin() + end,
                        [&](uint32_t a, uint32_t b) { return words[a].hansWeight > words[b].hansWeight; });
                }
            }
            if (pinyin) {
                for (size_t i = 0; i < buckets.size(); ++i) {
                    if (buckets[i].key.size() > 6) continue;
                    const size_t end = i + 1 == buckets.size() ? words.size() : buckets[i + 1].start;
                    for (size_t j = buckets[i].start; j < end; ++j) {
                        if (fully_spelled(words[j]) && han_characters(words[j].text(false)).size() == 1) {
                            singleReadings.push_back(buckets[i].key);
                            break;
                        }
                    }
                }
                // CN-04: the abbreviation index (see abbrevKeys).
                std::vector<std::pair<uint64_t, uint32_t>> entries;
                for (size_t i = 0; i < words.size(); ++i) {
                    uint64_t key = 0;
                    size_t syllables = 0;
                    bool opening = true, vowel = false, valid = true;
                    for (char c : words[i].reading()) {
                        if (is_tone(c, true)) {
                            if (!vowel) { valid = false; break; }
                            vowel = false; opening = true;
                            continue;
                        }
                        if (opening) {
                            if (syllables == abbrev_max_syllables) { valid = false; break; }
                            key |= abbrev_letter(c, syllables++);
                            opening = false;
                        }
                        vowel = vowel || std::string_view("aeiouv").find(c) != std::string_view::npos;
                    }
                    if (valid && syllables >= 2) entries.emplace_back(key, static_cast<uint32_t>(i));
                }
                std::sort(entries.begin(), entries.end(), [&](const auto &a, const auto &b) {
                    if (a.first != b.first) return a.first < b.first;
                    const int left = std::max(words[a.second].weight, words[a.second].hansWeight);
                    const int right = std::max(words[b.second].weight, words[b.second].hansWeight);
                    return left != right ? left > right : a.second < b.second;
                });
                abbrevKeys.reserve(entries.size());
                abbrevWords.reserve(entries.size());
                for (const auto &[key, index] : entries) { abbrevKeys.push_back(key); abbrevWords.push_back(index); }
            }
            for (size_t i = 0; i < words.size(); ++i) {
                const auto reading = words[i].reading();
                const auto traditional = words[i].text(false), simplified = words[i].text(true);
                if (simplified != traditional && simplified.size() > 3 && simplified.size() <= 384) {
                    auto found = traditionalPhrases.find(simplified);
                    if (found == traditionalPhrases.end() || found->second.second < words[i].hansWeight)
                        traditionalPhrases[simplified] = {traditional, words[i].hansWeight};
                    longestTraditionalPhrase = std::max(longestTraditionalPhrase, simplified.size());
                }
                if (pinyin && traditional.size() <= 4 && han_characters(traditional).size() == 1)
                    singleSpellings.push_back(reading);
                if (traditional.size() <= 4 && simplified.size() <= 4 &&
                    han_characters(traditional).size() == 1 && han_characters(simplified).size() == 1) {
                    auto found = traditionalCharacters.find(simplified);
                    if (found == traditionalCharacters.end() || found->second.second < words[i].weight)
                        traditionalCharacters[simplified] = {traditional, words[i].weight};
                }
                if (words[i].text(false).size() <= 4 && static_cast<unsigned char>(reading.front()) < initials.size()) {
                    const auto initial = static_cast<unsigned char>(reading.front());
                    initials[initial].push_back(i);
                    hansInitials[initial].push_back(i);
                }
            }
            std::sort(singleSpellings.begin(), singleSpellings.end());
            singleSpellings.erase(std::unique(singleSpellings.begin(), singleSpellings.end()), singleSpellings.end());
            singleSpellings.shrink_to_fit();
            singleReadings.shrink_to_fit();
            auto prepareInitials = [&](auto &groups, bool hans) {
                for (auto &items : groups) {
                    std::stable_sort(items.begin(), items.end(), [&](size_t a, size_t b) {
                        return words[a].score(hans) > words[b].score(hans);
                    });
                    std::unordered_set<std::string_view> visible;
                    size_t retained = 0;
                    for (const auto index : items) {
                        if (visible.insert(words[index].text(hans)).second) items[retained++] = index;
                        if (retained == 64) break;
                    }
                    items.resize(retained);
                }
            };
            prepareInitials(initials, false);
            prepareInitials(hansInitials, true);
        }
        if (words.empty()) return false;
        return statistics.empty() || loadStatistics(statistics);
    }

    // withStatistics: the caller also scores corpus collocations, so authored
    // phrases and plain dictionary words are weighted apart.
    double join_bonus(std::string_view left, std::string_view right, bool hans, bool distinguishStrong = false, bool withStatistics = false) const {
        if (context.empty() || right.empty()) return 0;
        // UTF-8 character boundaries for up to four characters on each side.
        std::array<size_t, 4> starts{}, ends{};
        size_t left_count = 0, right_count = 0;
        const auto is_start = [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0) != 0x80; };
        for (size_t i = left.size(); i > 0 && left_count < starts.size();) {
            if (is_start(left[--i])) starts[left_count++] = i;
        }
        for (size_t i = 1; i <= right.size() && right_count < ends.size(); ++i) {
            if (i == right.size() || is_start(right[i])) ends[right_count++] = i;
        }
        if (left_count == 0 || right_count == 0) return 0;
        const size_t offset = starts[left_count - 1];
        std::string joined(left.substr(offset));
        joined.append(right.substr(0, ends[right_count - 1]));
        int frequency = 0, authoredFrequency = 0;
        for (size_t i = 0; i < left_count; ++i) {
            const size_t begin = starts[i] - offset;
            for (size_t j = 0; j < right_count && i + j < 4; ++j) {
                const auto phrase = std::string_view(joined).substr(begin, left.size() - starts[i] + ends[j]);
                const auto entry = std::lower_bound(context.begin(), context.end(), phrase,
                    [](const auto &item, std::string_view value) { return item.first < value; });
                if (entry != context.end() && entry->first == phrase) {
                    frequency = std::max(frequency, hans ? entry->hansWeight : entry->weight);
                    if (!contextAuthored.empty() && contextAuthored[entry - context.begin()]) authoredFrequency = std::max(authoredFrequency, hans ? entry->hansWeight : entry->weight);
                }
            }
        }
        // Multi-character continuations provide more evidence than a single
        // potentially polyphonic character. For segment re-ranking, retain
        // differences between strong frequencies instead of clipping them all
        // to the same score. Decoder joins and single-character order retain
        // their existing scale.
        if (distinguishStrong && right_count > 1)
            return std::min(3.0, 3.0 * std::log1p(frequency) / std::log1p(10000.0));
        const double bonus = std::min(3.0, 0.4 * std::log1p(frequency));
        if (!withStatistics) return bonus;
        return std::max(authored_join_scale * std::min(3.0, 0.4 * std::log1p(authoredFrequency)), derived_join_scale * bonus);
    }

    std::string traditionalize(std::string_view value, bool tail) const {
        const auto characters = han_characters(value);
        if (characters.empty()) return {};
        std::string result;
        const size_t start = tail && characters.size() > 4 ? characters.size() - 4 : 0;
        const size_t end = std::min(characters.size(), start + 4);
        for (size_t i = start; i < end; ++i) {
            const auto entry = traditionalCharacters.find(characters[i]);
            result.append(entry == traditionalCharacters.end() ? characters[i] : entry->second.first);
        }
        return result;
    }

    // A simplified text in traditional characters: the longest recorded
    // phrase at each position, else character by character.
    std::string to_traditional(std::string_view input) const {
        std::string result;
        for (size_t i = 0; i < input.size();) {
            bool matched = false;
            for (size_t length = std::min(longestTraditionalPhrase, input.size() - i); length > 3; --length) {
                const auto found = traditionalPhrases.find(input.substr(i, length));
                if (found == traditionalPhrases.end()) continue;
                result.append(found->second.first);
                i += length; matched = true; break;
            }
            if (matched) continue;
            const auto first = static_cast<unsigned char>(input[i]);
            const size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            const auto character = input.substr(i, std::min(length, input.size() - i));
            const auto found = traditionalCharacters.find(character);
            result.append(found == traditionalCharacters.end() ? character : found->second.first);
            i += character.size();
        }
        return result;
    }

    std::pair<size_t, size_t> range(std::string_view key) const {
        const auto bucket = std::lower_bound(buckets.begin(), buckets.end(), key,
                                             [](const Bucket &item, std::string_view reading) {
                                                 return item.key < reading;
                                             });
        if (bucket == buckets.end() || bucket->key != key) return {0, 0};
        const auto next = bucket + 1;
        return {bucket->start, next == buckets.end() ? words.size() : next->start};
    }
};

struct Reading {
    std::string sounds;
    std::array<char, max_input + 1> boundaries{};
    std::array<size_t, max_input + 1> boundary_count{};

    bool pinyin;
    bool unambiguous = true;
    explicit Reading(std::string_view input, bool latin) : pinyin(latin) {
        for (char key : input) {
            if (is_tone(key, pinyin) || key == '\'') {
                if (sounds.empty() || boundaries[sounds.size()]) unambiguous = false;
                boundaries[sounds.size()] = key;
            }
            else sounds += key;
        }
        for (size_t i = 1; i <= sounds.size(); ++i) {
            boundary_count[i] = boundary_count[i - 1] + (boundaries[i] != 0);
        }
    }

    bool matches(const Word &word, size_t start, size_t end) const {
        const size_t required = boundary_count[end] - boundary_count[start];
        if (required == 0) return true;
        size_t matched = 0;
        for (char key : word.reading()) {
            if (!is_tone(key, pinyin)) { ++start; continue; }
            const char tone = boundaries[start];
            if (tone && tone != key && tone != '\'') return false;
            matched += tone != 0;
        }
        return matched == required;
    }
};

struct Fallback { std::string text; size_t consumed; double score; bool typo = false; /* CN-05: a local typo correction */ };

// Per-script personal bonuses keyed by a word's untoned reading (its lexicon
// bucket) and then its text in that script. Owned by the host through
// MPNPersonal; sessions only read it.
// DICT-02b: words the user added by hand, prepared for one lexicon. A word
// the lexicon lacks counts in sentence composition as a fairly common word of
// user_weight that has reached the learned-word cap (see Personal::lifted):
// it beats spelling its characters one by one and rare homophones (by at most
// personal_cap * personal_lead), but not a common lexicon word of the same
// reading, and a single syllable never displaces the common characters of
// that syllable. Whole-word lists put it first (see decode).
constexpr int user_weight = 3000;
constexpr size_t user_max_syllables = 16, user_max_reading = 128, user_max_text = 192;

struct UserWords {
    // Rows "reading<TAB>traditional<TAB>simplified" with one tone digit per
    // syllable ('5' where the user gave none); words view them.
    std::vector<std::string> rows, bare;
    std::vector<Word> words;
    std::vector<uint32_t> loose;   // Syllables without a typed tone: any tone matches.
    std::vector<uint32_t> sorted;  // Word indices by bare reading, then the order added.
    std::vector<uint64_t> abbrevKeys; // CN-04 keys of words of 2..abbrev_max_syllables syllables,
    std::vector<uint32_t> abbrevWords; // sorted, with their word indices.
    size_t longest = 0;                // Longest bare reading.

    std::pair<size_t, size_t> range(std::string_view sounds) const {
        const auto first = std::lower_bound(sorted.begin(), sorted.end(), sounds,
            [&](uint32_t index, std::string_view value) { return bare[index] < value; });
        auto last = first;
        while (last != sorted.end() && bare[*last] == sounds) ++last;
        return {static_cast<size_t>(first - sorted.begin()), static_cast<size_t>(last - sorted.begin())};
    }
    bool has(std::pair<size_t, size_t> span, std::string_view text, bool hans) const {
        for (size_t i = span.first; i < span.second; ++i) if (words[sorted[i]].text(hans) == text) return true;
        return false;
    }
    std::pair<size_t, size_t> abbrev_range(uint64_t key, size_t letters, bool prefix) const {
        const uint64_t last = prefix ? key + (uint64_t(1) << (5 * (Lexicon::abbrevLetters - letters))) : key + 1;
        const auto first = std::lower_bound(abbrevKeys.begin(), abbrevKeys.end(), key);
        const auto end = std::lower_bound(first, abbrevKeys.end(), last);
        return {static_cast<size_t>(first - abbrevKeys.begin()), static_cast<size_t>(end - abbrevKeys.begin())};
    }
};

// Reading::matches for a user word, whose untoned syllables accept any tone.
bool user_matches(const Reading &reading, const Word &word, uint32_t loose, size_t start, size_t end) {
    const size_t required = reading.boundary_count[end] - reading.boundary_count[start];
    if (required == 0) return true;
    size_t matched = 0, syllable = 0;
    for (char key : word.reading()) {
        if (!is_tone(key, true)) { ++start; continue; }
        const char tone = reading.boundaries[start];
        if (tone && tone != key && tone != '\'' && !(loose >> syllable & 1u)) return false;
        matched += tone != 0;
        ++syllable;
    }
    return matched == required;
}

struct Personal {
    using Texts = std::map<std::string, double, std::less<>>;
    std::array<std::map<std::string, Texts, std::less<>>, 2> readings; // [hans]

    // DICT-02b: user words keyed by untoned reading and simplified text, as
    // given: reading (letters, tone digits, apostrophes), traditional (may be
    // empty: converted per lexicon) and the order added.
    struct UserEntry { std::string reading, traditional; uint64_t order; };
    std::map<std::pair<std::string, std::string>, UserEntry> userWords;
    uint64_t userOrder = 0, userRevision = 0;
    mutable std::mutex userMutex;
    mutable std::shared_ptr<const UserWords> userCache;
    mutable uint64_t userCacheLexicon = 0, userCacheRevision = 0;

    // Splits letters into pinyin syllables known to the lexicon: fewest
    // syllables, then the longest first syllable. Empty when impossible.
    static std::vector<std::string_view> syllables(const Lexicon &lexicon, std::string_view letters) {
        const size_t n = letters.size();
        std::vector<size_t> best(n + 1, SIZE_MAX), next(n + 1, 0);
        best[n] = 0;
        for (size_t start = n; start-- > 0;)
            for (size_t size = std::min(size_t(6), n - start); size >= 1; --size) {
                if (best[start + size] == SIZE_MAX || best[start + size] + 1 >= best[start]) continue;
                if (!std::binary_search(lexicon.singleReadings.begin(), lexicon.singleReadings.end(), letters.substr(start, size))) continue;
                best[start] = best[start + size] + 1; next[start] = size;
            }
        std::vector<std::string_view> result;
        if (best[0] == SIZE_MAX) return result;
        for (size_t at = 0; at < n; at += next[at]) result.push_back(letters.substr(at, next[at]));
        return result;
    }

    std::shared_ptr<const UserWords> user_words(const Lexicon &lexicon) const {
        std::lock_guard<std::mutex> lock(userMutex);
        if (userWords.empty() || !lexicon.pinyin) return nullptr;
        if (userCache && userCacheLexicon == lexicon.serial && userCacheRevision == userRevision) return userCache;
        auto built = std::make_shared<UserWords>();
        std::vector<const std::pair<const std::pair<std::string, std::string>, UserEntry> *> entries;
        for (const auto &entry : userWords) entries.push_back(&entry);
        std::sort(entries.begin(), entries.end(), [](const auto *a, const auto *b) { return a->second.order < b->second.order; });
        for (const auto *entry : entries) {
            const auto &given = entry->second.reading;
            std::string reading;
            uint32_t loose = 0;
            size_t count = 0;
            bool valid = true;
            for (size_t at = 0; at < given.size() && valid;) {
                size_t stop = at;
                while (stop < given.size() && given[stop] >= 'a' && given[stop] <= 'z') ++stop;
                const char tone = stop < given.size() && given[stop] >= '1' && given[stop] <= '5' ? given[stop] : 0;
                const auto pieces = syllables(lexicon, std::string_view(given).substr(at, stop - at));
                valid = !pieces.empty() && count + pieces.size() <= user_max_syllables;
                for (size_t i = 0; valid && i < pieces.size(); ++i) {
                    reading.append(pieces[i]);
                    const bool last = i + 1 == pieces.size();
                    reading += last && tone ? tone : '5';
                    if (!(last && tone)) loose |= 1u << count;
                    ++count;
                }
                at = stop + (stop < given.size() ? 1 : 0);
                if (tone && at < given.size() && given[at] == '\'') ++at;
            }
            if (!valid || !count) continue;
            const auto &simplified = entry->first.second;
            const auto traditional = entry->second.traditional.empty() ? lexicon.to_traditional(simplified) : entry->second.traditional;
            built->rows.push_back(reading + '\t' + traditional + '\t' + simplified);
            built->bare.push_back(entry->first.first);
            built->loose.push_back(loose);
            built->longest = std::max(built->longest, entry->first.first.size());
        }
        if (built->rows.empty()) { userCache.reset(); userCacheLexicon = lexicon.serial; userCacheRevision = userRevision; return nullptr; }
        std::vector<std::pair<uint64_t, uint32_t>> abbrev;
        for (size_t i = 0; i < built->rows.size(); ++i) {
            built->words.push_back({built->rows[i], user_weight, user_weight});
            built->sorted.push_back(static_cast<uint32_t>(i));
            uint64_t key = 0;
            size_t count = 0;
            bool opening = true;
            for (char c : built->words.back().reading()) {
                if (is_tone(c, true)) { opening = true; continue; }
                if (opening && count < abbrev_max_syllables) key |= Lexicon::abbrev_letter(c, count);
                if (opening) ++count;
                opening = false;
            }
            if (count >= 2 && count <= abbrev_max_syllables) abbrev.emplace_back(key, static_cast<uint32_t>(i));
        }
        std::stable_sort(built->sorted.begin(), built->sorted.end(), [&](uint32_t a, uint32_t b) { return built->bare[a] < built->bare[b]; });
        std::stable_sort(abbrev.begin(), abbrev.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[key, index] : abbrev) { built->abbrevKeys.push_back(key); built->abbrevWords.push_back(index); }
        userCache = std::move(built);
        userCacheLexicon = lexicon.serial;
        userCacheRevision = userRevision;
        return userCache;
    }

    const Texts *find(bool hans, std::string_view sounds) const {
        const auto &table = readings[hans ? 1 : 0];
        if (table.empty()) return nullptr;
        const auto found = table.find(sounds);
        return found == table.end() ? nullptr : &found->second;
    }
    static double bonus(const Texts *texts, std::string_view text) {
        if (!texts) return 0;
        const auto found = texts->find(text);
        return found == texts->end() ? 0 : found->second;
    }
    // A learned word rises by its bonus, but past the best competing word of
    // the same reading (top) only by personal_lead of it.
    static double lifted(double base, double bonus, double top) {
        return bonus > 0 ? std::min(base + bonus, std::max(base, top) + personal_lead * bonus) : base;
    }
};

bool has_exact_match(const Lexicon &lexicon, std::string_view input) {
    const Reading reading(input, lexicon.pinyin);
    const auto range = lexicon.range(reading.sounds);
    for (size_t i = range.first; i < range.second; ++i)
        if (reading.matches(lexicon.words[i], 0, reading.sounds.size())) return true;
    return false;
}

// One bounded n/ng correction at a pinyin syllable ending. Dictionary
// validation keeps arbitrary edits out of the candidate list.
std::vector<std::string> nasal_variants(std::string_view sounds) {
    std::vector<std::string> result;
    if (sounds.size() < 4 || sounds.size() > 32) return result;
    for (size_t i = 1; i < sounds.size(); ++i) {
        if (sounds[i] != 'n' || std::string_view("aeiouv").find(sounds[i - 1]) == std::string_view::npos) continue;
        auto variant = std::string(sounds);
        if (i + 1 < sounds.size() && sounds[i + 1] == 'g') variant.erase(i + 1, 1);
        else if (i + 1 == sounds.size() || std::string_view("aeiouv").find(sounds[i + 1]) == std::string_view::npos)
            variant.insert(i + 1, 1, 'g');
        else continue;
        result.push_back(std::move(variant));
    }
    return result;
}

// When the last syllable is still being typed, keep a completed phrase
// selectable without consuming the unfinished suffix. Whole-word matches
// continue to take precedence over this partial conversion.
std::vector<Fallback> unfinished_phrase(const Lexicon &lexicon, std::string_view input, bool hans) {
    if (!lexicon.pinyin || input.size() < 3 || input.size() > 64) return {};
    if (has_exact_match(lexicon, input)) return {};

    for (size_t tail = 1; tail <= std::min(size_t(3), input.size() - 1); ++tail) {
        const auto suffix = input.substr(input.size() - tail);
        if (!std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= 'a' && c <= 'z'; })) continue;
        // A complete syllable or word can already participate in sentence decoding.
        const auto completeSuffix = lexicon.range(suffix);
        if (completeSuffix.first != completeSuffix.second) continue;
        const size_t count = input.size() - tail;
        const Reading prefix(input.substr(0, count), true);
        const auto range = lexicon.range(prefix.sounds);
        std::vector<Fallback> matches;
        std::unordered_set<std::string_view> visible;
        for (size_t i = range.first; i < range.second && matches.size() < 3; ++i) {
            const auto &word = lexicon.word(i, hans);
            const auto text = word.text(hans);
            const auto characters = std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
            // The table also contains consonant-only abbreviations (e.g. wo
            // for 玩偶). They are predictions, not a completed pinyin phrase.
            bool hasVowel = false, complete = true;
            for (char key : word.reading()) {
                if (is_tone(key, true)) { complete = complete && hasVowel; hasVowel = false; }
                else if (std::string_view("aeiouv").find(key) != std::string_view::npos) hasVowel = true;
            }
            if (complete && characters > 1 && prefix.matches(word, 0, prefix.sounds.size()) && visible.insert(text).second)
                matches.push_back({std::string(text), count, static_cast<double>(word.score(hans))});
        }
        if (!matches.empty()) return matches;
    }
    return {};
}

// Letters next to a key on a QWERTY keyboard.
std::string_view keyboard_neighbors(char key) {
    switch (key) {
        case 'q': return "wa"; case 'w': return "qeas"; case 'e': return "wrsd"; case 'r': return "etdf";
        case 't': return "ryfg"; case 'y': return "tugh"; case 'u': return "yihj"; case 'i': return "uojk";
        case 'o': return "ipkl"; case 'p': return "ol"; case 'a': return "qwsz"; case 's': return "awedzx";
        case 'd': return "serfxc"; case 'f': return "drtgcv"; case 'g': return "ftyhbv"; case 'h': return "gyujbn";
        case 'j': return "huiknm"; case 'k': return "jiolm"; case 'l': return "kop"; case 'z': return "asx";
        case 'x': return "zsdc"; case 'c': return "xdfv"; case 'v': return "cfgb"; case 'b': return "vghn";
        case 'n': return "bhjm"; case 'm': return "njk"; default: return {};
    }
}

// correct: offer the local typo corrections below (CN-05 switch); prefix
// completion and the single-character fallback do not depend on it.
std::vector<Fallback> fallback(const Lexicon &lexicon, std::string_view input, bool hans, bool allowSingle = true, bool correct = true) {
    std::vector<Fallback> matches;
    const Reading reading(input, lexicon.pinyin);
    const auto &sounds = reading.sounds;
    auto collect = [&](std::string_view key, double penalty, bool typo = false) {
        const auto range = lexicon.range(key);
        std::unordered_set<std::string_view> visible;
        // Distinct traditional entries can share one simplified spelling.
        // Bound work but count visible choices, not duplicate source rows.
        for (size_t i = range.first; i < range.second && i - range.first < 256 && visible.size() < 7; ++i) {
            const auto &word = lexicon.word(i, hans);
            const auto text = word.text(hans);
            if (visible.insert(text).second)
                matches.push_back({std::string(text), input.size(), word.score(hans) - penalty, typo});
        }
    };
    if (sounds.size() == 1 && static_cast<unsigned char>(sounds[0]) < lexicon.initials.size()) {
        const auto initial = static_cast<unsigned char>(sounds[0]);
        const auto &items = hans ? lexicon.hansInitials[initial] : lexicon.initials[initial];
        for (size_t i : items) {
            const auto &word = lexicon.words[i];
            matches.push_back({std::string(word.text(hans)), input.size(), double(word.score(hans))});
        }
    }
    if (lexicon.pinyin && sounds.size() >= 2 && sounds.size() <= 32 && input == sounds) {
        // Bounded prefix completion: binary search, then at most 256 dictionary buckets.
        auto first = std::lower_bound(lexicon.buckets.begin(), lexicon.buckets.end(), sounds,
            [](const Lexicon::Bucket &bucket, std::string_view key) { return bucket.key < key; });
        size_t scanned = 0;
        for (auto it = first; it != lexicon.buckets.end() && scanned++ < 256; ++it) {
            if (it->key.substr(0, sounds.size()) != sounds) break;
            if (it->key.size() - sounds.size() <= 6) collect(it->key, 1200.0 * (it->key.size() - sounds.size()));
        }
        // One local correction only; no full dictionary scan or arbitrary multi-edit guesses.
        if (correct && sounds.size() >= 4) {
            for (size_t i = 0; i < sounds.size(); ++i) {
                if (i + 1 < sounds.size()) { auto key = sounds; std::swap(key[i], key[i + 1]); collect(key, 800, true); }
                if (i && sounds[i] == sounds[i - 1]) { auto key = sounds; key.erase(i, 1); collect(key, 800, true); }
                for (char ch : keyboard_neighbors(sounds[i])) { auto key = sounds; key[i] = ch; collect(key, 1200, true); }
            }
            for (const auto &variant : nasal_variants(sounds)) collect(variant, 900, true);
        }
    }
    if (matches.empty() && allowSingle) {
        // Offer the first character only, retaining every unconverted key after selection.
        for (size_t count = std::min(input.size(), size_t(7)); count > 0; --count) {
            // A tone or explicit separator completes the preceding syllable.
            // Never select its untoned prefix and leave an orphaned boundary.
            if (count < input.size() && (is_tone(input[count], lexicon.pinyin) || input[count] == '\'')) continue;
            const Reading prefix(input.substr(0, count), lexicon.pinyin);
            if (prefix.sounds.empty()) continue;
            const auto range = lexicon.range(prefix.sounds);
            std::unordered_set<std::string_view> visible;
            for (size_t i = range.first; i < range.second && i - range.first < 512 && visible.size() < 28; ++i) {
                const auto &word = lexicon.word(i, hans);
                const auto text = word.text(hans);
                const auto characters = std::count_if(text.begin(), text.end(), [](unsigned char ch) { return (ch & 0xc0) != 0x80; });
                if (characters == 1 && prefix.matches(word, 0, prefix.sounds.size()) && visible.insert(text).second)
                    matches.push_back({std::string(text), count, static_cast<double>(word.score(hans))});
            }
            if (!matches.empty()) break;
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Fallback &a, const Fallback &b) { return a.score > b.score; });
    std::unordered_set<std::string> seen;
    std::vector<Fallback> result;
    for (auto &match : matches) {
        if (seen.insert(match.text).second) result.push_back(std::move(match));
        if (result.size() == 28) break;
    }
    return result;
}

// CN-03: fuzzy pinyin. A typed complete syllable may stand for another one
// whose initial or final differs by an enabled pair (zong for zhong, lan for
// nan, xiang for xian). For someone who enables a pair both spellings sound
// the same, so a fuzzy word competes mostly on frequency: each changed
// initial or final costs fuzzy_penalty_near nats (a factor of about 150)
// where an exact word covers the same keys, and exact readings win ties and
// near-ties. Where no exact word does, the exact alternative is a
// composition that a missing dictionary word can make look weak (点赞 typed
// exactly is 点+赞), so only a common fuzzy word gets the near penalty and a
// rarer one pays fuzzy_penalty (电站 stays behind 点赞). Fuzzy words over the
// whole input are merged into the exact whole-word list by weight less 700
// per nat of the near penalty; with no exact whole word they lead only when
// they outscore every exact sentence. Only complete syllables are
// substituted; an initial on its own is an abbreviation and stays as typed,
// and only fully spelled dictionary words are reached. The values were
// chosen on the tuned set, the word development set and the fuzzy
// development sample (Windows/tests/data/chinese-fuzzy-dev.tsv): a near
// penalty of 4 turned the tuned 今天先到这里 into 想到 and 点赞 into 点展.
constexpr double fuzzy_penalty_near = 5.0; // nats per change where an exact word covers the same keys
constexpr double fuzzy_penalty = 8.0;      // nats per change otherwise, unless the fuzzy word is common
constexpr int fuzzy_common = 4500;         // lexicon weight of a common word (about the top 1.5% of entries)
constexpr unsigned fuzzy_max_cost = 3; // changes per dictionary word
constexpr size_t fuzzy_start_nodes = 1024, fuzzy_start_options = 64;

struct FuzzyOption {
    size_t end;                   // input sound position after the last syllable
    std::string key;              // the dictionary reading the variant spells
    std::vector<uint8_t> pieces;  // syllable lengths within key
    std::vector<size_t> ends;     // input sound position closing each syllable
    unsigned cost;                // changed initials and finals
};

// Spellings of one complete syllable under the enabled pairs, with the number
// of changes, that are recorded single-character readings. Includes the
// syllable itself (no change) when it is one.
void fuzzy_syllables(const Lexicon &lexicon, std::string_view piece, uint32_t mask,
                     std::vector<std::pair<std::string, unsigned>> &out) {
    static constexpr std::string_view initials[] = {"zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l", "g",
                                                    "k", "h", "j", "q", "x", "r", "z", "c", "s", "y", "w"};
    std::string_view initial;
    for (const auto candidate : initials)
        if (piece.substr(0, candidate.size()) == candidate) { initial = candidate; break; }
    const auto rest = piece.substr(initial.size());
    if (rest.empty()) return;
    std::array<std::pair<std::string_view, unsigned>, 3> heads{};
    size_t headCount = 0;
    heads[headCount++] = {initial, 0};
    const auto head = [&](uint32_t bit, std::string_view a, std::string_view b) {
        if (!(mask & bit) || headCount == heads.size()) return;
        if (initial == a) heads[headCount++] = {b, 1};
        else if (initial == b) heads[headCount++] = {a, 1};
    };
    head(MPN_FUZZY_Z_ZH, "z", "zh"); head(MPN_FUZZY_C_CH, "c", "ch"); head(MPN_FUZZY_S_SH, "s", "sh");
    head(MPN_FUZZY_N_L, "n", "l"); head(MPN_FUZZY_F_H, "f", "h"); head(MPN_FUZZY_R_L, "r", "l");
    std::array<std::pair<std::string_view, unsigned>, 2> tails{};
    size_t tailCount = 0;
    tails[tailCount++] = {rest, 0};
    const auto tail = [&](uint32_t bit, std::string_view a, std::string_view b) {
        if (!(mask & bit) || tailCount == tails.size()) return;
        if (rest == a) tails[tailCount++] = {b, 1};
        else if (rest == b) tails[tailCount++] = {a, 1};
    };
    // yan/yang and wan/wang spell the finals ian/iang and uan/uang.
    if (initial == "y") tail(MPN_FUZZY_IAN_IANG, "an", "ang");
    else if (initial == "w") tail(MPN_FUZZY_UAN_UANG, "an", "ang");
    else tail(MPN_FUZZY_AN_ANG, "an", "ang");
    tail(MPN_FUZZY_EN_ENG, "en", "eng"); tail(MPN_FUZZY_IN_ING, "in", "ing");
    tail(MPN_FUZZY_IAN_IANG, "ian", "iang"); tail(MPN_FUZZY_UAN_UANG, "uan", "uang");
    for (size_t h = 0; h < headCount; ++h) {
        for (size_t t = 0; t < tailCount; ++t) {
            std::string spelling(heads[h].first);
            spelling += tails[t].first;
            if (std::binary_search(lexicon.singleReadings.begin(), lexicon.singleReadings.end(), spelling))
                out.emplace_back(std::move(spelling), heads[h].second + tails[t].second);
        }
    }
}

// For every input start, the dictionary readings reachable from it with at
// least one substitution: a bounded walk over complete syllables that only
// continues while some dictionary reading begins with the spelling so far.
std::vector<std::vector<FuzzyOption>> fuzzy_options(const Lexicon &lexicon, const Reading &reading, uint32_t mask) {
    const auto &sounds = reading.sounds;
    const size_t n = sounds.size();
    if (!mask || !lexicon.pinyin || !n || lexicon.singleReadings.empty()) return {};
    std::vector<std::array<std::vector<std::pair<std::string, unsigned>>, 6>> variants(n);
    bool any = false;
    for (size_t start = 0; start < n; ++start) {
        for (size_t size = 1; size <= 6 && start + size <= n; ++size) {
            // A tone or separator inside the piece cannot belong to one syllable.
            if (reading.boundary_count[start + size - 1] != reading.boundary_count[start]) break;
            fuzzy_syllables(lexicon, std::string_view(sounds).substr(start, size), mask, variants[start][size - 1]);
            any = any || std::any_of(variants[start][size - 1].begin(), variants[start][size - 1].end(),
                                     [](const auto &item) { return item.second > 0; });
        }
    }
    if (!any) return {};
    std::vector<std::vector<FuzzyOption>> result(n);
    struct Walk {
        const Lexicon &lexicon;
        const std::vector<std::array<std::vector<std::pair<std::string, unsigned>>, 6>> &variants;
        std::vector<FuzzyOption> &out;
        size_t nodes = 0;
        std::string key;
        std::vector<uint8_t> pieces;
        std::vector<size_t> ends;
        void run(size_t position, unsigned cost) {
            for (size_t size = 1; size <= 6 && position + size <= variants.size(); ++size) {
                for (const auto &[spelling, change] : variants[position][size - 1]) {
                    if (cost + change > fuzzy_max_cost || key.size() + spelling.size() > lexicon.longest) continue;
                    if (++nodes > fuzzy_start_nodes || out.size() >= fuzzy_start_options) return;
                    const auto before = key.size();
                    key += spelling;
                    const auto bucket = std::lower_bound(lexicon.buckets.begin(), lexicon.buckets.end(), key,
                        [](const Lexicon::Bucket &item, std::string_view value) { return item.key < value; });
                    if (bucket != lexicon.buckets.end() && bucket->key.substr(0, key.size()) == key) {
                        pieces.push_back(static_cast<uint8_t>(spelling.size()));
                        ends.push_back(position + size);
                        if (cost + change > 0 && bucket->key == key &&
                            std::none_of(out.begin(), out.end(), [&](const FuzzyOption &option) {
                                return option.end == position + size && option.key == key && option.pieces == pieces;
                            }))
                            out.push_back({position + size, key, pieces, ends, cost + change});
                        run(position + size, cost + change);
                        pieces.pop_back();
                        ends.pop_back();
                    }
                    key.resize(before);
                }
            }
        }
    };
    for (size_t start = 0; start < n; ++start) {
        Walk walk{lexicon, variants, result[start]};
        walk.run(start, 0);
    }
    return result;
}

// Nats per changed initial or final for a fuzzy word of this weight. Where an
// exact word covers the same keys, the fuzzy one competes on frequency and
// wins only when clearly more common (near). Where none does, the exact
// alternative is a composition, which a missing dictionary word can make look
// weak (点赞 typed exactly is 点+赞), so only a common fuzzy word competes on
// frequency; a rarer one pays the full penalty (电站 stays behind 点赞). Fuzzy
// whole-input words beside exact whole words are merged by weight instead and
// otherwise stay behind every exact sentence.
double fuzzy_change_penalty(int weight, bool wholeSpan, bool wholeSpelled, bool exactHere) {
    if (wholeSpan) return wholeSpelled || weight < fuzzy_common ? fuzzy_penalty : fuzzy_penalty_near;
    return exactHere || weight >= fuzzy_common ? fuzzy_penalty_near : fuzzy_penalty;
}

// A word reached through a fuzzy option: its syllables are exactly the
// option's, and any typed tone at a syllable end must match the word's.
bool fuzzy_matches(const Reading &reading, const FuzzyOption &option, const Word &word) {
    if (!fully_spelled(word)) return false;
    size_t syllable = 0, letters = 0;
    for (char key : word.reading()) {
        if (!is_tone(key, true)) { ++letters; continue; }
        if (syllable == option.pieces.size() || letters != option.pieces[syllable]) return false;
        const char tone = reading.boundaries[option.ends[syllable]];
        if (tone && tone != key && tone != '\'') return false;
        letters = 0;
        ++syllable;
    }
    return syllable == option.pieces.size() && letters == 0;
}

// CN-04: abbreviated and mixed pinyin. When the keys do not split into
// complete syllables (nh, zgren, nihaom, womenqcf), a piece of the input may
// also be an initial standing for a whole syllable: one letter (z also covers
// zh, c ch, s sh; a, o and e cover the syllables they begin) or zh, ch, sh.
// Dictionary words of two or more syllables are then reached through the
// abbreviation index, mixing typed syllables and initials within one word, and
// compete in sentence composition by frequency less abbrev_penalty for each
// abbreviated syllable, so a reading that spells more syllables in full wins
// near-ties (中国+人 for zgren, not 中国人+嗯). Spans that an exact fully
// spelled word covers take no abbreviation. Inputs that do split into
// complete syllables never reach this code, so their candidates are unchanged;
// the compiler's consonant-only abbreviation rows (weight / 8) stand in for it
// only there. Two initials alone (zg) inside a longer input only reach words
// of at least abbrev_short_min, since short keys match hundreds of rare words.
// Work per input start is bounded by abbrev_start_nodes walked pieces and
// abbrev_start_options readings, and each reading inspects at most
// abbrev_scan index entries.
constexpr double abbrev_penalty = 2.0;     // nats per abbreviated syllable
constexpr int abbrev_short_min = 4000;     // lexicon weight for two initials inside a longer input
constexpr double abbrev_char_penalty = 4.0; // nats for one character typed as its initial
constexpr int abbrev_char_min = 5000;      // lexicon weight of such a character
constexpr size_t abbrev_start_nodes = 256, abbrev_start_options = 32, abbrev_scan = 1024;

struct AbbrevOption {
    size_t end;                 // input sound position after the last piece
    uint64_t key;               // abbreviation index key
    uint8_t count;              // pieces, one per syllable
    uint16_t initials;          // bit k: piece k is an initial rather than a full syllable
    std::array<uint16_t, abbrev_max_syllables + 1> bounds; // sound positions delimiting the pieces
};

// Whether the input's sounds split into complete syllables (recorded
// single-character readings) without crossing a typed tone or separator.
bool spelled_input(const Lexicon &lexicon, const Reading &reading) {
    const auto &sounds = reading.sounds;
    std::vector<char> spelled(sounds.size() + 1, 0);
    spelled[0] = 1;
    for (size_t end = 1; end <= sounds.size(); ++end)
        for (size_t size = 1; size <= std::min(size_t(6), end) && !spelled[end]; ++size)
            spelled[end] = spelled[end - size] && reading.boundary_count[end - 1] == reading.boundary_count[end - size] &&
                std::binary_search(lexicon.singleReadings.begin(), lexicon.singleReadings.end(),
                                   std::string_view(sounds).substr(end - size, size));
    return spelled[sounds.size()] != 0;
}

// CN-05: whether the keys read as complete syllables followed by two to five
// keys that only begin a syllable (budin, jiaobe, shiyon): the last syllable
// is still being typed, so nothing is a slip yet.
bool unfinished_syllable_tail(const Lexicon &lexicon, std::string_view sounds) {
    std::vector<char> spelled(sounds.size() + 1, 0);
    spelled[0] = 1;
    for (size_t end = 1; end <= sounds.size(); ++end)
        for (size_t size = 1; size <= std::min(size_t(6), end) && !spelled[end]; ++size)
            spelled[end] = spelled[end - size] && std::binary_search(lexicon.singleReadings.begin(), lexicon.singleReadings.end(),
                                                                    sounds.substr(end - size, size));
    for (size_t tail = 2; tail <= std::min(size_t(5), sounds.size() - 1); ++tail) {
        if (!spelled[sounds.size() - tail]) continue;
        const auto suffix = sounds.substr(sounds.size() - tail);
        const auto found = std::lower_bound(lexicon.singleReadings.begin(), lexicon.singleReadings.end(), suffix);
        if (found != lexicon.singleReadings.end() && *found != suffix && found->substr(0, tail) == suffix) return true;
    }
    return false;
}

// The compiler's consonant-only abbreviation rows (n3h3 for 你好).
bool abbreviation_row(const Word &word) {
    if (fully_spelled(word)) return false;
    const auto reading = word.reading();
    return std::count_if(reading.begin(), reading.end(), [](char key) { return is_tone(key, true); }) >= 2;
}

// For every input start, the splits into pieces (complete syllables and
// initials, at least one initial, two or more pieces) whose initials key
// leads to dictionary words: a bounded walk pruned by the index.
std::vector<std::vector<AbbrevOption>> abbrev_options(const Lexicon &lexicon, const Reading &reading, const UserWords *user = nullptr) {
    const auto &sounds = reading.sounds;
    const size_t n = sounds.size();
    std::vector<std::vector<AbbrevOption>> result(n);
    struct Walk {
        const Lexicon &lexicon;
        const Reading &reading;
        std::vector<AbbrevOption> &out;
        const UserWords *user; // DICT-02b: its words lead to options too.
        bool any(uint64_t key, size_t letters, bool prefix) const {
            const auto range = lexicon.abbrev_range(key, letters, prefix);
            if (range.first != range.second) return true;
            if (!user) return false;
            const auto own = user->abbrev_range(key, letters, prefix);
            return own.first != own.second;
        }
        size_t nodes = 0;
        AbbrevOption current{};
        bool inside(size_t position, size_t size) const {
            return reading.boundary_count[position + size - 1] != reading.boundary_count[position];
        }
        void piece(size_t position, size_t size, bool initial, size_t depth) {
            if (++nodes > abbrev_start_nodes || out.size() >= abbrev_start_options) return;
            const auto saved = current;
            current.key |= Lexicon::abbrev_letter(reading.sounds[position], depth);
            if (!any(current.key, depth + 1, true)) {
                current = saved;
                return;
            }
            current.bounds[depth + 1] = static_cast<uint16_t>(position + size);
            if (initial) current.initials = static_cast<uint16_t>(current.initials | (1u << depth));
            current.count = static_cast<uint8_t>(depth + 1);
            current.end = position + size;
            if (depth >= 1 && current.initials && any(current.key, depth + 1, false)) out.push_back(current);
            run(position + size, depth + 1);
            current = saved;
        }
        void run(size_t position, size_t depth) {
            const auto &sounds = reading.sounds;
            if (depth == abbrev_max_syllables || position == sounds.size()) return;
            const char letter = sounds[position];
            if (std::string_view("bpmfdtnlgkhjqxrzcsywaoe").find(letter) != std::string_view::npos) {
                piece(position, 1, true, depth);
                if ((letter == 'z' || letter == 'c' || letter == 's') && position + 1 < sounds.size() &&
                    sounds[position + 1] == 'h' && !inside(position, 2))
                    piece(position, 2, true, depth);
            }
            for (size_t size = 2; size <= 6 && position + size <= sounds.size() && !inside(position, size); ++size)
                if (std::binary_search(lexicon.singleReadings.begin(), lexicon.singleReadings.end(),
                                       std::string_view(sounds).substr(position, size)))
                    piece(position, size, false, depth);
        }
    };
    for (size_t start = 0; start < n; ++start) {
        Walk walk{lexicon, reading, result[start], user};
        walk.current.bounds[0] = static_cast<uint16_t>(start);
        walk.run(start, 0);
    }
    return result;
}

// Abbreviated syllables of word read through option; 0 when it does not
// match or spells every piece in full. last: the only abbreviated syllable is
// the option's final piece.
unsigned abbrev_matches(const Reading &reading, const AbbrevOption &option, const Word &word, bool &last) {
    const auto spelling = word.reading();
    size_t piece = 0, start = 0, abbreviated = 0;
    unsigned count = 0;
    for (size_t i = 0; i < spelling.size(); ++i) {
        if (!is_tone(spelling[i], true)) continue;
        if (piece == option.count) return 0;
        const auto syllable = spelling.substr(start, i - start);
        const auto typed = std::string_view(reading.sounds).substr(option.bounds[piece], option.bounds[piece + 1] - option.bounds[piece]);
        if (option.initials >> piece & 1u) {
            if (syllable.substr(0, typed.size()) != typed) return 0;
            if (syllable.size() != typed.size()) { ++count; abbreviated = piece; }
        } else if (syllable != typed) {
            return 0;
        }
        const char tone = reading.boundaries[option.bounds[piece + 1]];
        if (tone && tone != '\'' && tone != spelling[i]) return 0;
        ++piece;
        start = i + 1;
    }
    if (piece != option.count) return 0;
    last = count == 1 && abbreviated + 1 == option.count;
    return count;
}

// CN-05: typo correction. Keys that do not split into complete syllables
// (zhogn, nihoa, zhonguo, womenqucifna) may contain one slip: two adjacent
// letters swapped, one letter missing, one letter too many (often doubled)
// or one letter replaced by a keyboard neighbour. A dictionary word whose
// reading is one such edit away from a piece of the input then competes in
// sentence composition by frequency less typo_penalty, if it is common
// (typo_common) and, inside longer keys, has two or more characters (a
// corrected single character makes junk of romaji such as wosuki); at most
// one corrected word per candidate and never together
// with an abbreviation, never where an exact fully spelled word or (CN-03) a
// fuzzy word covers the same keys. A slip must not look like unfinished
// typing: apart from a swap of the last two keys (zhogn), an edit leaves at
// least the last two keys as typed (zhongg stays an abbreviation of 中国, jiag
// is not jiang); a neighbouring key replaces a vowel by a vowel and a
// consonant by a consonant (bjda stays 比较大); the corrected keys contain a
// vowel (zhd stays an abbreviation); and keys that read as complete
// syllables plus the start of one more (budin, jiaobe) are not corrected at
// all. Keys that split into complete syllables keep their exact
// sentences; there only a common word of two or more characters one swap,
// missing or extra letter from the whole input (zhonggou -> 中国) follows
// the first typo_whole_position candidates, and only when no fully spelled
// word has the input's reading. Tones and separators turn the in-sentence
// correction off. Work per input start is bounded by typo_start_nodes prefix
// lookups and typo_start_options readings; inputs longer than
// typo_max_sounds are not corrected. The penalties were chosen on the typo
// development sample (Windows/tests/data/chinese-typo-dev.tsv): 2–4 nats
// give the same results there and on the abbreviation sample; 1, 5 and 6
// lose one first choice.
constexpr double typo_penalty = 3.0;        // nats per corrected word
constexpr int typo_common = 4500;           // lexicon weight a corrected word needs
constexpr int typo_whole_min = 4500;        // lexicon weight of a whole-input correction of complete syllables
constexpr size_t typo_whole_position = 3, typo_whole_count = 2;
constexpr size_t typo_max_sounds = 40;
constexpr size_t typo_start_nodes = 4096, typo_start_options = 256;

struct TypoOption {
    size_t end;      // input sound position after the corrected piece
    std::string key; // the dictionary reading after the edit
};

// For every input start, the dictionary readings one edit away from a piece
// of the input that begins there: a walk along the exact prefix that tries
// each edit once and then continues exactly while a reading begins with the
// spelling so far.
std::vector<std::vector<TypoOption>> typo_options(const Lexicon &lexicon, const Reading &reading) {
    const auto &sounds = reading.sounds;
    const size_t n = sounds.size();
    std::vector<std::vector<TypoOption>> result(n);
    // vowels[i]: vowels among the first i keys.
    std::vector<size_t> vowels(n + 1, 0);
    const auto is_vowel = [](char key) { return std::string_view("aeiouv").find(key) != std::string_view::npos; };
    for (size_t i = 0; i < n; ++i) vowels[i + 1] = vowels[i] + is_vowel(sounds[i]);
    for (size_t start = 0; start < n; ++start) {
        if (vowels[n] == vowels[start]) break; // nothing but consonants left
        auto &out = result[start];
        size_t nodes = 0;
        std::string key;
        // Whether some reading begins with key; exact: it is one with a
        // fully spelled word (not only consonant abbreviation rows).
        const auto leads = [&](bool &exact) {
            exact = false;
            if (++nodes > typo_start_nodes) return false;
            const auto bucket = std::lower_bound(lexicon.buckets.begin(), lexicon.buckets.end(), key,
                [](const Lexicon::Bucket &item, std::string_view value) { return item.key < value; });
            if (bucket == lexicon.buckets.end() || bucket->key.substr(0, key.size()) != key) return false;
            if (bucket->key == key) {
                const size_t last = bucket + 1 == lexicon.buckets.end() ? lexicon.words.size() : (bucket + 1)->start;
                for (size_t i = bucket->start; i < last && i - bucket->start < 64 && !exact; ++i)
                    exact = fully_spelled(lexicon.words[i]);
            }
            return true;
        };
        // key already holds the edit; continue exactly from input position i.
        const auto finish = [&](size_t i) {
            const auto before = key.size();
            for (;;) {
                bool exact = false;
                if (out.size() >= typo_start_options || key.size() > lexicon.longest || !leads(exact)) break;
                // Keys of consonants only (zhd, bkn) are abbreviations, not slips.
                if (exact && i - start >= 2 && vowels[i] != vowels[start] && std::none_of(out.begin(), out.end(), [&](const TypoOption &option) {
                        return option.end == i && option.key == key; }))
                    out.push_back({i, key});
                if (i == n) break;
                key += sounds[i++];
            }
            key.resize(before);
        };
        for (size_t i = start; i < n && nodes <= typo_start_nodes && out.size() < typo_start_options; ++i) {
            // key == sounds[start, i), a prefix of some reading.
            const auto before = key.size();
            if (i + 1 < n && sounds[i] != sounds[i + 1]) {
                key += sounds[i + 1]; key += sounds[i];
                finish(i + 2);
                key.resize(before);
            }
            // One letter too many, with two keys typed after it: a final
            // doubled or stray letter may begin the next syllable (gangg).
            if (i > start && i + 2 < n) finish(i + 1);
            // One letter missing, with two keys typed after it: a letter
            // before the last key would complete an unfinished syllable (jiag).
            if (i > start && i + 2 <= n && i + 1 < n) {
                for (char letter : std::string_view("abcdefghijklmnopqrstuwxyz")) {
                    key += letter;
                    finish(i);
                    key.resize(before);
                }
            }
            // A neighbouring key of the same kind (vowel for vowel), never the
            // last one typed: an unfinished syllable would otherwise be
            // replaced (jiab, zho), and a vowel for a consonant reads
            // abbreviations as words (bjda).
            for (char letter : i + 1 < n ? keyboard_neighbors(sounds[i]) : std::string_view()) {
                if (is_vowel(letter) != is_vowel(sounds[i])) continue;
                key += letter;
                finish(i + 1);
                key.resize(before);
            }
            key += sounds[i];
            bool exact = false;
            if (!leads(exact)) break;
        }
    }
    return result;
}

// Readings one edit from the whole input, for keys that split into complete
// syllables: swapped neighbours, a missing or an extra letter, never dropping
// the first or last key nor adding before the first or after the last.
std::vector<std::string> typo_whole_keys(std::string_view sounds) {
    std::vector<std::string> keys;
    const size_t n = sounds.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        if (sounds[i] != sounds[i + 1]) { std::string key(sounds); std::swap(key[i], key[i + 1]); keys.push_back(std::move(key)); }
        if (i > 0) { std::string key(sounds); key.erase(i, 1); keys.push_back(std::move(key)); }
    }
    for (size_t i = 1; i < n; ++i)
        for (char letter : std::string_view("abcdefghijklmnopqrstuwxyz")) {
            std::string key(sounds); key.insert(key.begin() + static_cast<std::ptrdiff_t>(i), letter); keys.push_back(std::move(key));
        }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

struct CandidateMetadata {
    uint32_t kind = MPN_CANDIDATE_UNKNOWN;
    uint32_t scoreKind = MPN_SCORE_NONE;
    double score = 0;
    bool fuzzy = false; // CN-03: reached through a fuzzy pinyin substitution
    // CN-04: abbreviated syllables in the candidate; trailing when the only
    // one is the final key (an initial still being typed, as in nihaom).
    unsigned abbreviated = 0;
    bool trailing = false;
    bool typo = false; // CN-05: reached through a typo correction
    bool user = false; // DICT-02b: contains a word the user added
};
struct Candidate {
    std::string text;
    size_t consumed;
    CandidateMetadata metadata;
    // Only Android's complete-reading presentation appends a raw suffix.
    // Selection commits the converted prefix and retains this suffix.
    size_t literalTail = 0;
};

std::vector<std::string> decode(const Lexicon &lexicon, std::string_view input, bool hans, bool completeOnly = false,
                                std::vector<CandidateMetadata> *metadata = nullptr, bool *singleCharacterReading = nullptr, size_t expandedBeam = 0,
                                const Personal *personal = nullptr, uint32_t fuzzy = 0, bool typo = false) {
    const Reading reading(input, lexicon.pinyin);
    const size_t length = reading.sounds.size();
    if (length == 0) return {};
    // CN-03: empty unless fuzzy pairs are enabled and the input has a syllable they change.
    const auto fuzzyOptions = fuzzy_options(lexicon, reading, fuzzy);

    std::vector<std::string> results;
    std::unordered_set<std::string> seen;
    // Vowels alone do not prove that an abbreviation is a pinyin syllable.
    // Require independent single-character dictionary evidence for each
    // syllable. Missing evidence is UNKNOWN, not a guessed correction.
    auto verifiedSpelling = [&](const Word &word) {
        if (!metadata || !lexicon.pinyin) return true;
        const auto spelling = word.reading();
        size_t start = 0;
        for (size_t end = 0; end < spelling.size(); ++end) {
            if (!is_tone(spelling[end], true)) continue;
            const auto syllable = spelling.substr(start, end - start + 1);
            if (!std::binary_search(lexicon.singleSpellings.begin(), lexicon.singleSpellings.end(), syllable)) {
                // Compiler-generated 儿 suffix aliases retain the base syllable's
                // tone (hui4 + er5 -> huir4), with independent base evidence.
                const auto text = word.text(true);
                if (end + 1 != spelling.size() || syllable.size() < 3 || syllable[syllable.size() - 2] != 'r' ||
                    text.size() < 3 || text.substr(text.size() - 3) != "儿") return false;
                auto base = std::string(syllable); base.erase(base.size() - 2, 1);
                if (!std::binary_search(lexicon.singleSpellings.begin(), lexicon.singleSpellings.end(), base)) return false;
            }
            start = end + 1;
        }
        return start == spelling.size();
    };
    auto append = [&](std::string_view text, CandidateMetadata item) {
        if (seen.emplace(text).second) {
            results.emplace_back(text);
            if (metadata) metadata->push_back(item);
        }
    };
    const auto whole = lexicon.range(reading.sounds);
    // DICT-02b: the user's own words, prepared for this lexicon; null when
    // there are none, which leaves every result unchanged.
    const auto userHold = personal && lexicon.pinyin ? personal->user_words(lexicon) : nullptr;
    const UserWords *user = userHold.get();
    // CN-04: abbreviations take part only in user-visible lookups of keys
    // that neither split into complete syllables nor spell a whole word.
    const bool visible = lexicon.pinyin && metadata && !completeOnly;
    const bool spelledKeys = visible && length >= 2 && spelled_input(lexicon, reading);
    const bool abbreviating = visible && length >= 2 && !lexicon.abbrevKeys.empty() && !spelledKeys &&
        std::none_of(lexicon.words.begin() + whole.first, lexicon.words.begin() + whole.second,
                     [&](const Word &word) { return fully_spelled(word) && reading.matches(word, 0, length); });
    const auto abbrevOptions = abbreviating ? abbrev_options(lexicon, reading, user) : std::vector<std::vector<AbbrevOption>>();
    // CN-05: typo corrections inside keys that do not split into complete
    // syllables, without tones or separators; whole-input ones otherwise.
    const bool correcting = typo && visible && length >= 3 && length <= typo_max_sounds && !spelledKeys &&
        reading.boundary_count[length] == 0 && !unfinished_syllable_tail(lexicon, reading.sounds);
    const auto typoOptions = correcting ? typo_options(lexicon, reading) : std::vector<std::vector<TypoOption>>();
    const bool correctingWhole = typo && visible && spelledKeys && length >= 4 && length <= 32 && input == reading.sounds &&
        std::none_of(lexicon.words.begin() + whole.first, lexicon.words.begin() + whole.second,
                     [&](const Word &word) { return fully_spelled(word); });
    const bool singleReading = !completeOnly && metadata && std::any_of(
        lexicon.words.begin() + whole.first, lexicon.words.begin() + whole.second,
        [&](const Word &word) {
            const auto spelling = word.reading();
            return std::count_if(spelling.begin(), spelling.end(),
                       [&](char key) { return is_tone(key, lexicon.pinyin); }) == 1 &&
                   han_characters(word.text(false)).size() == 1 &&
                   reading.matches(word, 0, length);
        });
    if (singleCharacterReading) *singleCharacterReading = singleReading;
    const auto result_limit = expandedBeam ? std::max(candidate_limit, lexicon.words.size()) :
        (singleReading ? single_character_candidate_limit : candidate_limit);
    const auto *wholeLearned = personal ? personal->find(hans, reading.sounds) : nullptr;
    std::vector<double> exactScores;
    // Lexicon weights of the exact whole words, parallel to results; kept only for fuzzy merging.
    std::vector<int> wholeWeights;
    // Parallel to wholeWeights: 1 for an n/ng correction inserted below.
    std::vector<char> wholeNasal;
    bool wholeSpelled = false;
    for (size_t index = whole.first; index < whole.second; ++index) {
        const auto &word = lexicon.word(index, hans);
        if (abbreviating && abbreviation_row(word)) continue;
        if ((!completeOnly || fully_spelled(word)) && reading.matches(word, 0, length)) {
            const auto kind = !reading.unambiguous ? MPN_CANDIDATE_UNKNOWN :
                (lexicon.pinyin && !fully_spelled(word) ? MPN_CANDIDATE_CORRECTION :
                 (verifiedSpelling(word) ? MPN_CANDIDATE_EXACT : MPN_CANDIDATE_UNKNOWN));
            const auto before = results.size();
            append(word.text(hans), {static_cast<uint32_t>(kind), MPN_SCORE_LEXICON, double(word.score(hans))});
            if (wholeLearned && results.size() != before) exactScores.push_back(word.score(hans) / 700.0);
            if (!fuzzyOptions.empty() && results.size() != before) wholeWeights.push_back(word.score(hans));
            wholeSpelled = wholeSpelled || (!fuzzyOptions.empty() && fully_spelled(word));
        }
        if (results.size() == result_limit) break;
    }
    if (wholeLearned && !results.empty()) {
        // CN-06c: learned homophones of the whole reading move forward. The
        // list arrives in descending frequency, so a stable sort leaves every
        // unlearned word where it was.
        std::vector<double> keys(results.size());
        bool learned = false;
        for (size_t i = 0; i < results.size(); ++i) {
            const double bonus = Personal::bonus(wholeLearned, results[i]);
            learned = learned || bonus > 0;
            keys[i] = Personal::lifted(exactScores[i], bonus, exactScores.front());
        }
        if (learned) {
            std::vector<size_t> order(results.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return keys[a] > keys[b]; });
            std::vector<std::string> sorted;
            std::vector<CandidateMetadata> sortedMetadata;
            std::vector<int> sortedWeights;
            for (const auto i : order) {
                sorted.push_back(std::move(results[i]));
                if (metadata) sortedMetadata.push_back((*metadata)[i]);
                if (!wholeWeights.empty()) sortedWeights.push_back(wholeWeights[i]);
            }
            results = std::move(sorted);
            if (metadata) *metadata = std::move(sortedMetadata);
            if (!wholeWeights.empty()) wholeWeights = std::move(sortedWeights);
        }
    }
    if (user) {
        // DICT-02b: words the user added for exactly this reading lead the
        // whole-word list, in the order added. A lexicon word of the same
        // text moves up with them rather than appearing twice.
        const auto span = user->range(reading.sounds);
        size_t position = 0;
        for (size_t i = span.first; i < span.second; ++i) {
            const auto index = user->sorted[i];
            if (!user_matches(reading, user->words[index], user->loose[index], 0, length)) continue;
            const std::string text(user->words[index].text(hans));
            const auto existing = std::find(results.begin(), results.end(), text);
            if (existing != results.end()) {
                const auto at = static_cast<size_t>(existing - results.begin());
                if (at < position) continue;
                results.erase(existing);
                if (metadata) metadata->erase(metadata->begin() + static_cast<std::ptrdiff_t>(at));
                if (!wholeWeights.empty()) wholeWeights.erase(wholeWeights.begin() + static_cast<std::ptrdiff_t>(at));
            } else seen.emplace(text);
            results.insert(results.begin() + static_cast<std::ptrdiff_t>(position), text);
            if (metadata) metadata->insert(metadata->begin() + static_cast<std::ptrdiff_t>(position),
                {static_cast<uint32_t>(reading.unambiguous ? MPN_CANDIDATE_EXACT : MPN_CANDIDATE_UNKNOWN), MPN_SCORE_LEXICON,
                 double(user_weight), false, 0, false, false, true});
            // Fuzzy whole words are placed after it (see following, below).
            if (!fuzzyOptions.empty()) wholeWeights.insert(wholeWeights.begin() + static_cast<std::ptrdiff_t>(position), std::numeric_limits<int>::max());
            ++position;
        }
        if (results.size() > result_limit) {
            results.resize(result_limit);
            if (metadata) metadata->resize(result_limit);
            if (wholeWeights.size() > result_limit) wholeWeights.resize(result_limit);
        }
    }
    if (results.size() == result_limit) return results;

    if (!fuzzyOptions.empty()) wholeNasal.assign(wholeWeights.size(), 0);
    // Keep the first page of exact single-syllable characters intact: a
    // nasal correction like ban should not displace bang's common characters.
    // For longer readings, surface likely corrections after three exact words.
    // CN-05: part of typo correction, off with it.
    if (typo && !completeOnly && lexicon.pinyin && !results.empty() && input == reading.sounds) {
        size_t insertion = std::min(singleReading ? page_size : size_t(3), results.size());
        size_t added = 0;
        for (const auto &variant : nasal_variants(reading.sounds)) {
            const auto range = lexicon.range(variant);
            for (size_t index = range.first; index < range.second && added < 2; ++index) {
                const auto &word = lexicon.word(index, hans);
                if (word.score(hans) < 4000) break;
                const auto text = word.text(hans);
                if (seen.emplace(text).second) {
                    results.insert(results.begin() + insertion, std::string(text));
                    if (metadata) metadata->insert(metadata->begin() + insertion, {MPN_CANDIDATE_CORRECTION, MPN_SCORE_LEXICON, double(word.score(hans)), false, 0, false, true});
                    if (!fuzzyOptions.empty()) {
                        wholeWeights.insert(wholeWeights.begin() + insertion, word.score(hans));
                        wholeNasal.insert(wholeNasal.begin() + insertion, 1);
                    }
                    ++insertion; ++added;
                }
            }
            if (added == 2) break;
        }
        if (results.size() > result_limit) { results.resize(result_limit); if (metadata) metadata->resize(result_limit); }
        if (wholeWeights.size() > results.size()) { wholeWeights.resize(results.size()); wholeNasal.resize(results.size()); }
    }

    // Whole dictionary words stay first; known completions/corrections precede
    // speculative chains such as interpreting a mistyped ending as an abbreviation.
    bool needFallback = results.empty() && lexicon.pinyin && !completeOnly;

    // CN-03: fuzzy, some word was reached through a fuzzy pinyin substitution;
    // whole, the path is one such word over the entire input, and weight its
    // lexicon weight less 700 per nat of its penalty.
    struct Path { std::string text, traditional; double score; bool spelled, verified; uint32_t first; bool fuzzy = false, whole = false; double weight = 0;
                  // CN-04: abbreviated syllables, and whether the only one is the final key.
                  unsigned abbreviated = 0; bool trailing = false;
                  // CN-05: one word was reached through a typo correction.
                  bool typo = false;
                  // DICT-02b: contains a word the user added.
                  bool user = false; };
    // The sentence search beam is a latency bound, not a five-candidate UI cap.
    // Widen user-visible pinyin lookups within 32 letters only. Internal
    // completion probes and longer searches retain their existing bounded work.
    const size_t pathLimit = lexicon.pinyin && metadata && !completeOnly && length <= 32
        ? (expandedBeam ? expandedBeam : (length <= 8 ? page_size * 3 : size_t(8))) : beam;
    std::vector<std::vector<Path>> suffixes(length + 1);
    // CN-03: fuzzy whole-input words with their weight less the near penalty,
    // collected beside the bounded sentence beam when exact whole words exist.
    struct WholeFuzzy { std::string_view text; double weight; };
    std::vector<WholeFuzzy> wholeFuzzy;
    suffixes[length].push_back({"", "", 0, true, true, lexicon.pairEnd});
    // DICT-02b: a user word may be longer than every lexicon reading.
    const size_t longest = std::max(lexicon.longest, user ? user->longest : size_t(0));
    for (size_t start = length; start-- > 0;) {
        std::vector<Path> paths;
        for (size_t end = start + 1; end <= std::min(length, start + longest); ++end) {
            if (suffixes[end].empty()) continue;
            // Exact whole-input words are already listed; fuzzy ones compete here.
            const bool wholeSpan = start == 0 && end == length;
            if (wholeSpan && fuzzyOptions.empty() && !abbreviating && !correcting) continue;
            const auto span = std::string_view(reading.sounds).substr(start, end - start);
            const auto options = lexicon.range(span);
            // CN-06c: learned words of this reading. They also join the beam
            // from beyond its usual width, within a bounded scan.
            const auto *learned = personal ? personal->find(hans, span) : nullptr;
            size_t learnedLeft = learned ? learned->size() : 0;
            // DICT-02b: user words of this reading.
            const auto userSpan = user ? user->range(span) : std::pair<size_t, size_t>{0, 0};
            double top = -std::numeric_limits<double>::infinity();
            std::vector<std::string_view> used_spellings;
            used_spellings.reserve(pathLimit);
            // CN-03: whether a fully spelled exact word covers this span.
            bool exactHere = false;
            // CN-04: whether a fully spelled exact word covers this span.
            bool spelledHere = false;
            // CN-05: whether a fully spelled exact word covers this span.
            bool typoBlocked = false;
            const auto extend = [&](const Word &word, double score, const FuzzyOption *fuzzyWord, double fuzzyWeight = 0,
                                    unsigned abbreviated = 0, bool trailing = false, bool typoWord = false, bool userWord = false) {
                // DICT-02b: a user word's syllables were checked against the lexicon.
                const bool verified = userWord || verifiedSpelling(word);
                for (const auto &suffix : suffixes[end]) {
                    // CN-05: one corrected word per candidate, never beside an abbreviation.
                    if (suffix.typo && (typoWord || abbreviated)) continue;
                    if (typoWord && suffix.abbreviated) continue;
                    const auto traditional = word.text(false);
                    const auto &right = hans ? suffix.traditional : suffix.text;
                    const double bonus = lexicon.join_bonus(traditional, right, hans, false, !lexicon.pairs.empty()) +
                        lexicon.pair_bonus(word.pair, suffix.first) +
                        (start == 0 ? lexicon.pair_bonus(lexicon.pairBegin, word.pair) : 0);
                    paths.push_back({
                        std::string(word.text(hans)) + suffix.text,
                        hans ? std::string(traditional) + right : std::string(),
                        score + suffix.score + bonus,
                        suffix.spelled && !abbreviated && !typoWord && (!lexicon.pinyin || fully_spelled(word)),
                        suffix.verified && verified,
                        word.pair,
                        suffix.fuzzy || fuzzyWord,
                        wholeSpan && fuzzyWord,
                        fuzzyWeight,
                        suffix.abbreviated + abbreviated,
                        abbreviated ? trailing && !suffix.abbreviated : suffix.trailing,
                        suffix.typo || typoWord,
                        suffix.user || userWord,
                    });
                }
            };
            for (size_t index = options.first; index < options.second && !wholeSpan; ++index) {
                if (used_spellings.size() >= pathLimit && (!learnedLeft || index - options.first >= 4096)) break;
                const auto &word = lexicon.word(index, hans);
                if (completeOnly && !fully_spelled(word)) continue;
                if (!reading.matches(word, start, end)) continue;
                if (abbreviating && abbreviation_row(word)) continue;
                exactHere = exactHere || (!fuzzyOptions.empty() && fully_spelled(word));
                spelledHere = spelledHere || (abbreviating && fully_spelled(word));
                typoBlocked = typoBlocked || (correcting && fully_spelled(word));
                // Tone and traditional variants remain available as exact
                // candidates. In a bounded sentence beam, variants with the
                // same simplified form share one slot so they do not crowd
                // out different words and sentence interpretations.
                const auto spelling = word.text(true);
                if (std::find(used_spellings.begin(), used_spellings.end(), spelling) != used_spellings.end()) continue;
                // DICT-02b: a lexicon word the user also added counts as learned to the cap.
                const double bonus = userSpan.first != userSpan.second && user->has(userSpan, word.text(hans), hans)
                    ? personal_cap : Personal::bonus(learned, word.text(hans));
                if (used_spellings.size() >= pathLimit && bonus <= 0) continue;
                used_spellings.push_back(spelling);
                if (bonus > 0 && learnedLeft) --learnedLeft;
                // Compiler weights are already log-frequency priors. Logging them again
                // erases the distinction between common phrases and rare fragments.
                // The correction moves a word towards its frequency in running text.
                const double base = static_cast<double>(word.score(hans)) / 700.0 - 19.0 + word.prior / 100.0;
                top = std::max(top, base);
                const double score = Personal::lifted(base, bonus, top);
                extend(word, score, nullptr);
            }
            for (size_t i = userSpan.first; i < userSpan.second && !wholeSpan; ++i) {
                // DICT-02b: user words the lexicon lacks, at user_weight with
                // the learned-word cap, beside the beam's lexicon words.
                const auto index = user->sorted[i];
                const auto &word = user->words[index];
                if (!user_matches(reading, word, user->loose[index], start, end)) continue;
                exactHere = exactHere || !fuzzyOptions.empty();
                spelledHere = spelledHere || abbreviating;
                typoBlocked = typoBlocked || correcting;
                const auto spelling = word.text(true);
                if (std::find(used_spellings.begin(), used_spellings.end(), spelling) != used_spellings.end()) continue;
                used_spellings.push_back(spelling);
                const double base = static_cast<double>(user_weight) / 700.0 - 19.0;
                extend(word, Personal::lifted(base, personal_cap, top), nullptr, 0, 0, false, false, true);
            }
            if (!fuzzyOptions.empty()) {
                // CN-03: words of fuzzy readings over this span, best first after
                // their penalty, in a beam of their own so that a full beam of
                // exact words cannot hide them. Exact words keep their texts.
                struct Near { const Word *word; double base; const FuzzyOption *option; double weight; };
                std::vector<Near> near;
                for (const auto &option : fuzzyOptions[start]) {
                    if (option.end != end) continue;
                    const auto range = lexicon.range(option.key);
                    size_t taken = 0;
                    for (size_t index = range.first; index < range.second && index - range.first < 512 && taken < pathLimit; ++index) {
                        const auto &word = lexicon.word(index, hans);
                        if (!fuzzy_matches(reading, option, word)) continue;
                        const double penalty = fuzzy_change_penalty(word.score(hans), wholeSpan, wholeSpelled, exactHere);
                        if (wholeSpan && wholeSpelled)
                            wholeFuzzy.push_back({word.text(hans), double(word.score(hans)) - 700.0 * fuzzy_penalty_near * option.cost});
                        near.push_back({&word, static_cast<double>(word.score(hans)) / 700.0 - 19.0 + word.prior / 100.0 -
                            penalty * option.cost, &option, double(word.score(hans)) - 700.0 * penalty * option.cost});
                        ++taken;
                    }
                }
                std::stable_sort(near.begin(), near.end(), [](const Near &a, const Near &b) { return a.base > b.base; });
                // CN-05: a reading the user made equivalent is not a slip.
                typoBlocked = typoBlocked || !near.empty();
                size_t taken = 0;
                double fuzzyTop = -std::numeric_limits<double>::infinity();
                for (const auto &item : near) {
                    if (taken == pathLimit) break;
                    const auto spelling = item.word->text(true);
                    if (std::find(used_spellings.begin(), used_spellings.end(), spelling) != used_spellings.end()) continue;
                    used_spellings.push_back(spelling);
                    ++taken;
                    const auto *learnedFuzzy = personal ? personal->find(hans, item.option->key) : nullptr;
                    fuzzyTop = std::max(fuzzyTop, item.base);
                    extend(*item.word, Personal::lifted(item.base, Personal::bonus(learnedFuzzy, item.word->text(hans)), fuzzyTop), item.option, item.weight);
                }
            }
            if (abbreviating && !spelledHere) {
                // CN-04: words reached through initials over this span, best
                // first after their penalty, in a beam of their own.
                struct Short { const Word *word; double base; unsigned count; bool last; bool user = false; };
                std::vector<Short> found;
                if (user) for (const auto &option : abbrevOptions[start]) {
                    // DICT-02b: user words reached through their initials.
                    if (option.end != end) continue;
                    const auto range = user->abbrev_range(option.key, option.count, false);
                    for (size_t i = range.first; i < range.second; ++i) {
                        const auto &word = user->words[user->abbrevWords[i]];
                        bool last = false;
                        const unsigned count = abbrev_matches(reading, option, word, last);
                        if (!count) continue;
                        found.push_back({&word, static_cast<double>(user_weight) / 700.0 - 19.0 - abbrev_penalty * count,
                                         count, last && end == length, true});
                    }
                }
                for (const auto &option : abbrevOptions[start]) {
                    if (option.end != end) continue;
                    const bool brief = option.count == 2 && option.initials == 3;
                    const auto range = lexicon.abbrev_range(option.key, option.count, false);
                    size_t matched = 0;
                    for (size_t i = range.first; i < range.second && i - range.first < abbrev_scan && matched < 2 * pathLimit; ++i) {
                        const auto &word = lexicon.words[lexicon.abbrevWords[i]];
                        if (brief && !wholeSpan && word.score(hans) < abbrev_short_min) continue;
                        bool last = false;
                        const unsigned count = abbrev_matches(reading, option, word, last);
                        if (!count) continue;
                        ++matched;
                        found.push_back({&word, static_cast<double>(word.score(hans)) / 700.0 - 19.0 + word.prior / 100.0 -
                            abbrev_penalty * count, count, last && end == length});
                    }
                }
                // A lone initial between other keys may stand for one common
                // character (wo-x-h-n: 我+喜欢+你), at a higher penalty.
                const auto piece = std::string_view(reading.sounds).substr(start, end - start);
                if (!wholeSpan && (piece.size() == 1 ? std::string_view("bpmfdtnlgkhjqxrzcsywaoe").find(piece[0]) != std::string_view::npos :
                        piece.size() == 2 && piece[1] == 'h' && (piece[0] == 'z' || piece[0] == 'c' || piece[0] == 's') &&
                        reading.boundary_count[start + 1] == reading.boundary_count[start])) {
                    const char tone = reading.boundaries[end];
                    for (const auto index : hans ? lexicon.hansInitials[static_cast<unsigned char>(piece[0])] : lexicon.initials[static_cast<unsigned char>(piece[0])]) {
                        const auto &word = lexicon.words[index];
                        const auto spelling = word.reading();
                        if (word.score(hans) < abbrev_char_min || !fully_spelled(word) || spelling.substr(0, piece.size()) != piece ||
                            spelling.size() == piece.size() + 1 || (tone && tone != '\'' && tone != spelling.back())) continue;
                        found.push_back({&word, static_cast<double>(word.score(hans)) / 700.0 - 19.0 + word.prior / 100.0 -
                            abbrev_char_penalty, 1, end == length});
                    }
                }
                std::stable_sort(found.begin(), found.end(), [](const Short &a, const Short &b) { return a.base > b.base; });
                const bool learning = personal && !personal->readings[hans ? 1 : 0].empty();
                size_t taken = 0;
                double shortTop = -std::numeric_limits<double>::infinity();
                for (const auto &item : found) {
                    if (taken == pathLimit) break;
                    const auto spelling = item.word->text(true);
                    if (std::find(used_spellings.begin(), used_spellings.end(), spelling) != used_spellings.end()) continue;
                    used_spellings.push_back(spelling);
                    ++taken;
                    // CN-06c: a learned word keeps its bonus when typed by its initials.
                    double bonus = item.user ? personal_cap : 0;
                    if (learning && !item.user) {
                        std::string bare;
                        for (char key : item.word->reading()) if (!is_tone(key, true)) bare += key;
                        bonus = Personal::bonus(personal->find(hans, bare), item.word->text(hans));
                    }
                    shortTop = std::max(shortTop, item.base);
                    extend(*item.word, Personal::lifted(item.base, bonus, shortTop), nullptr, 0, item.count, item.last, false, item.user);
                }
            }
            if (correcting && !typoBlocked) {
                // CN-05: words one edit away from this span, best first after
                // their penalty, in a beam of their own.
                struct Slip { const Word *word; double base; const TypoOption *option; };
                std::vector<Slip> slips;
                for (const auto &option : typoOptions[start]) {
                    if (option.end != end) continue;
                    const auto range = lexicon.range(option.key);
                    size_t taken = 0;
                    for (size_t index = range.first; index < range.second && index - range.first < 512 && taken < pathLimit; ++index) {
                        const auto &word = lexicon.word(index, hans);
                        if (!fully_spelled(word)) continue;
                        const int weight = word.score(hans);
                        if (weight < typo_common || (!wholeSpan && han_characters(word.text(false)).size() == 1)) continue;
                        ++taken;
                        slips.push_back({&word, static_cast<double>(weight) / 700.0 - 19.0 + word.prior / 100.0 - typo_penalty, &option});
                    }
                }
                std::stable_sort(slips.begin(), slips.end(), [](const Slip &a, const Slip &b) { return a.base > b.base; });
                size_t taken = 0;
                double slipTop = -std::numeric_limits<double>::infinity();
                for (const auto &item : slips) {
                    if (taken == pathLimit) break;
                    const auto spelling = item.word->text(true);
                    if (std::find(used_spellings.begin(), used_spellings.end(), spelling) != used_spellings.end()) continue;
                    used_spellings.push_back(spelling);
                    ++taken;
                    const auto *learnedSlip = personal ? personal->find(hans, item.option->key) : nullptr;
                    slipTop = std::max(slipTop, item.base);
                    extend(*item.word, Personal::lifted(item.base, Personal::bonus(learnedSlip, item.word->text(hans)), slipTop),
                           nullptr, 0, 0, false, true);
                }
            }
        }
        std::sort(paths.begin(), paths.end(), [](const Path &a, const Path &b) {
            return a.score == b.score ? a.text < b.text : a.score > b.score;
        });
        std::unordered_set<std::string> unique;
        for (auto &path : paths) {
            if (unique.insert(path.text).second) suffixes[start].push_back(std::move(path));
            if (suffixes[start].size() == pathLimit) break;
        }
    }
    if (!fuzzyOptions.empty()) {
        // CN-03: fuzzy whole-input words that join the whole-word list. With
        // an exact whole word of the input, each fuzzy word whose weight less
        // the near penalty beats the weakest of them is merged by weight; with
        // none, only fuzzy words that outscore every exact sentence lead (the
        // full penalty applies; zongguo -> 中国). Others stay sentence
        // candidates in score order.
        std::vector<std::pair<std::string, double>> promoted;
        if (wholeSpelled && !wholeWeights.empty()) {
            int weakest = std::numeric_limits<int>::max();
            for (size_t i = 0; i < wholeWeights.size(); ++i) if (!wholeNasal[i]) weakest = std::min(weakest, wholeWeights[i]);
            std::stable_sort(wholeFuzzy.begin(), wholeFuzzy.end(), [](const WholeFuzzy &a, const WholeFuzzy &b) { return a.weight > b.weight; });
            for (const auto &item : wholeFuzzy) if (item.weight > weakest) promoted.push_back({std::string(item.text), item.weight});
        } else {
            for (const auto &path : suffixes[0]) { if (!path.whole) break; promoted.push_back({path.text, path.weight}); }
        }
        // A word offered here supersedes the same text as an n/ng correction,
        // which the fuzzy pair places by its frequency instead.
        for (const auto &item : promoted) {
            for (size_t i = 0; i < wholeNasal.size(); ++i) {
                if (!wholeNasal[i] || results[i] != item.first) continue;
                results.erase(results.begin() + i);
                if (metadata) metadata->erase(metadata->begin() + i);
                wholeWeights.erase(wholeWeights.begin() + i);
                wholeNasal.erase(wholeNasal.begin() + i);
                seen.erase(item.first);
                break;
            }
        }
        const size_t exactCount = results.size();
        // Best weight from each exact position on; learning may have reordered them.
        std::vector<int> following(exactCount + 1, std::numeric_limits<int>::min());
        for (size_t i = exactCount; i-- > 0;) following[i] = std::max(following[i + 1], wholeWeights[i]);
        size_t inserted = 0, position = 0;
        for (const auto &[text, weight] : promoted) {
            if (results.size() >= result_limit) break;
            if (!seen.emplace(text).second) continue;
            // The place among exact words only moves down.
            while (position < exactCount && following[position] >= weight) ++position;
            results.insert(results.begin() + position + inserted, text);
            if (metadata) metadata->insert(metadata->begin() + position + inserted, {MPN_CANDIDATE_CORRECTION, MPN_SCORE_LEXICON, weight, true});
            ++inserted;
        }
        if (inserted) needFallback = false;
    }
    const auto appendFallback = [&] {
        if (needFallback) for (const auto &item : fallback(lexicon, input, hans, false, typo)) {
            if (results.size() >= result_limit) break;
            append(item.text, {MPN_CANDIDATE_CORRECTION, MPN_SCORE_FALLBACK, item.score, false, 0, false, item.typo});
        }
    };
    // A rare newly indexed word reached by a typo must not outrank a fully
    // spelled sentence (zhidaole -> 知道了, not keyboard correction 指导科).
    const bool completePath = std::any_of(suffixes[0].begin(), suffixes[0].end(), [](const Path &path) { return path.spelled && path.verified; });
    // CN-04: abbreviations read as typed come before keyboard corrections.
    // CN-05: so do typo corrections, which the sentence search already ranked.
    const bool abbreviationLead = (abbreviating || correcting) && !suffixes[0].empty() &&
        (suffixes[0].front().abbreviated || suffixes[0].front().typo);
    if (!completePath && !abbreviationLead) appendFallback();
    for (const auto &path : suffixes[0]) {
        const auto kind = !reading.unambiguous ? MPN_CANDIDATE_UNKNOWN :
            (!path.spelled || path.fuzzy ? MPN_CANDIDATE_CORRECTION :
             (path.verified ? MPN_CANDIDATE_PATH : MPN_CANDIDATE_UNKNOWN));
        append(path.text, {static_cast<uint32_t>(kind), MPN_SCORE_PATH, path.score, path.fuzzy, path.abbreviated, path.trailing, path.typo, path.user});
        if (results.size() == result_limit) break;
    }
    if (completePath || abbreviationLead) appendFallback();
    if (correctingWhole) {
        // CN-05: keys of complete syllables are read exactly first; a common
        // word one edit from all of them (zhonggou -> 中国, doushao -> 多少)
        // follows the first typo_whole_position candidates.
        struct Whole { std::string_view text; int weight; };
        std::vector<Whole> found;
        for (const auto &key : typo_whole_keys(reading.sounds)) {
            const auto range = lexicon.range(key);
            for (size_t index = range.first; index < range.second; ++index) {
                const auto &word = lexicon.word(index, hans);
                if (word.score(hans) < typo_whole_min) break;
                if (fully_spelled(word) && han_characters(word.text(false)).size() >= 2) found.push_back({word.text(hans), word.score(hans)});
            }
        }
        std::stable_sort(found.begin(), found.end(), [](const Whole &a, const Whole &b) { return a.weight > b.weight; });
        size_t position = std::min(typo_whole_position, results.size()), inserted = 0;
        for (const auto &item : found) {
            if (inserted == typo_whole_count) break;
            const auto existing = std::find(results.begin(), results.end(), item.text);
            if (existing != results.end()) {
                const auto at = static_cast<size_t>(existing - results.begin());
                if (at < position) continue;
                results.erase(existing);
                if (metadata) metadata->erase(metadata->begin() + static_cast<std::ptrdiff_t>(at));
            }
            seen.emplace(item.text);
            results.insert(results.begin() + static_cast<std::ptrdiff_t>(position), std::string(item.text));
            if (metadata) metadata->insert(metadata->begin() + static_cast<std::ptrdiff_t>(position),
                {MPN_CANDIDATE_CORRECTION, MPN_SCORE_LEXICON, double(item.weight), false, 0, false, true});
            ++position; ++inserted;
        }
        if (results.size() > result_limit) { results.resize(result_limit); if (metadata) metadata->resize(result_limit); }
    }
    return results;
}

// An unfinished initial can disambiguate the previous character: ㄒㄧㄏ
// can continue as 喜歡, for instance. Consult bounded dictionary completions
// rather than promoting a single context-free homophone.
std::vector<Fallback> predictive_segments_at(const Lexicon &lexicon, std::string_view input, bool hans, size_t tail,
                                             const Personal *personal = nullptr) {
    if (input.size() < 4 || input.size() > 64 || has_exact_match(lexicon, input)) return {};
    const size_t count = input.size() - tail;
    const auto suffix = input.substr(count);
    if (lexicon.pinyin) {
        if (std::string_view("bpmdtlgkhjqxzcrsfyw").find(suffix.front()) == std::string_view::npos ||
            !std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return {};
    } else if (keys.substr(0, 21).find(suffix.front()) == std::string_view::npos ||
               std::any_of(suffix.begin(), suffix.end(), [](char c) { return is_tone(c) || c == '\''; })) return {};
    if (input[count - 1] == '\'') return {};
    auto phrases = decode(lexicon, input.substr(0, count), hans, lexicon.pinyin, nullptr, nullptr, 0, personal);
    if (phrases.empty()) return {};
    struct Scored { Fallback item; int evidence; size_t order; };
    std::vector<Scored> scored;
    for (size_t i = 0; i < phrases.size() && i < 5; ++i) {
        auto &text = phrases[i];
        if (std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) < 2) continue;
        scored.push_back({{std::move(text), count, 0}, 0, i});
    }
    if (scored.empty()) return {};
    const size_t minStart = count > 6 ? count - 6 : 0;
    for (size_t start = count; start-- > minStart;) {
        if (is_tone(input[start], lexicon.pinyin) || input[start] == '\'') continue;
        const Reading continuation(input.substr(start), lexicon.pinyin);
        const Reading completed(input.substr(start, count - start), lexicon.pinyin);
        if (completed.sounds.empty() || continuation.sounds.size() <= completed.sounds.size()) continue;
        auto bucket = std::lower_bound(lexicon.buckets.begin(), lexicon.buckets.end(), continuation.sounds,
            [](const Lexicon::Bucket &item, std::string_view key) { return item.key < key; });
        for (size_t scanned = 0; bucket != lexicon.buckets.end() && scanned < 96; ++bucket, ++scanned) {
            if (bucket->key.substr(0, continuation.sounds.size()) != continuation.sounds) break;
            if (bucket->key.size() - continuation.sounds.size() > 6) continue;
            const auto range = lexicon.range(bucket->key);
            for (size_t i = range.first; i < range.second && i < range.first + 5; ++i) {
                const auto &word = lexicon.word(i, hans);
                if (word.score(hans) < 4000 || !continuation.matches(word, 0, continuation.sounds.size())) continue;
                const auto reading = word.reading();
                size_t firstTone = 0;
                while (firstTone < reading.size() && !is_tone(reading[firstTone], lexicon.pinyin)) ++firstTone;
                if (firstTone != completed.sounds.size()) continue;
                const auto text = word.text(hans);
                size_t firstEnd = 1;
                while (firstEnd < text.size() && (static_cast<unsigned char>(text[firstEnd]) & 0xc0) == 0x80) ++firstEnd;
                if (firstEnd == text.size()) continue;
                const auto first = text.substr(0, firstEnd);
                for (auto &candidate : scored) {
                    const auto &candidateText = candidate.item.text;
                    if (candidateText.size() >= first.size() && candidateText.compare(candidateText.size() - first.size(), first.size(), first) == 0)
                        candidate.evidence = std::max(candidate.evidence, word.score(hans));
                }
            }
        }
    }
    if (std::none_of(scored.begin(), scored.end(), [](const Scored &item) { return item.evidence > 0; })) return {};
    // Direct, common completed words outrank suffix-only evidence. Otherwise
    // 篮球 can promote 六篮 over 浏览 while the next q is still unfinished.
    const Reading prefix(input.substr(0, count), lexicon.pinyin);
    const auto prefixRange = lexicon.range(prefix.sounds);
    for (auto &candidate : scored) {
        for (size_t i = prefixRange.first; i < prefixRange.second; ++i) {
            const auto &word = lexicon.word(i, hans);
            if (word.score(hans) < 4000) break;
            if (fully_spelled(word) && prefix.matches(word, 0, prefix.sounds.size()) && word.text(hans) == candidate.item.text) {
                candidate.evidence += 10000; break;
            }
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        return a.evidence == b.evidence ? a.order < b.order : a.evidence > b.evidence;
    });
    std::vector<Fallback> result;
    for (auto &item : scored) result.push_back(std::move(item.item));
    return result;
}

std::vector<Fallback> predictive_segments(const Lexicon &lexicon, std::string_view input, bool hans, const Personal *personal = nullptr) {
    if (input.size() < 4 || input.size() > 64) return {};
    for (size_t tail = 1; tail <= std::min(size_t(3), input.size() - 1); ++tail) {
        auto result = predictive_segments_at(lexicon, input, hans, tail, personal);
        if (!result.empty()) return result;
    }
    return {};
}

// A complete pinyin sentence can also be selected in smaller pieces. Only
// offer a split if both sides decode as fully spelled readings.
std::pair<std::vector<Fallback>, size_t> pinyin_segments(const Lexicon &lexicon, std::string_view input, bool hans,
                                                         const Personal *personal = nullptr) {
    if (!lexicon.pinyin || input.size() < 6 || input.size() > 64) return {};
    std::vector<Fallback> result, alternatives;
    size_t inspected = 0;
    const size_t minCut = input.size() > 8 ? input.size() - 8 : 2;
    for (size_t cut = input.size() - 1; cut >= minCut && inspected < 5 && result.size() < 3; --cut) {
        if (is_tone(input[cut], true) || input[cut] == '\'') continue;
        const auto suffix = input.substr(cut);
        if (decode(lexicon, suffix, hans, true).empty()) continue;
        ++inspected;
        auto phrases = decode(lexicon, input.substr(0, cut), hans, true, nullptr, nullptr, 0, personal);
        if (phrases.empty()) continue;
        auto &text = phrases.front();
        if (std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) >= 2) {
            result.push_back({std::move(text), cut, 0});
            for (size_t i = 1; i < std::min(size_t(3), phrases.size()); ++i)
                if (std::count_if(phrases[i].begin(), phrases[i].end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) >= 2)
                    alternatives.push_back({std::move(phrases[i]), cut, 0});
        }
    }
    // Retain the three split points first, then up to two alternative readings
    // at each. Reuse the same decodes; alternatives remain behind whole paths.
    const size_t primaryCount = result.size();
    for (auto &alternative : alternatives) {
        if (result.size() == 9) break;
        result.push_back(std::move(alternative));
    }
    return {std::move(result), primaryCount};
}

// Last-resort ladder for segment-by-segment selection: fully spelled dictionary
// words and single characters whose reading is a leading part of the input,
// longest reading first and dictionary order within one reading. Selecting one
// keeps the remaining keys, so any text in the lexicon stays reachable even
// when every decoded sentence is wrong. Lookups only; no sentence search.
// perReading bounds the entries taken from each leading reading; a bounded
// call also inspects only the head of each bucket, because it runs on every
// key and short readings share their bucket with thousands of abbreviations.
std::vector<Fallback> leading_words(const Lexicon &lexicon, std::string_view input, bool hans, size_t perReading = SIZE_MAX) {
    if (!lexicon.pinyin || input.size() > max_input) return {};
    const Reading reading(input, true);
    const auto &sounds = reading.sounds;
    // Raw bytes consumed by the first n sounds, including the tone or
    // apostrophe that closes them.
    std::vector<size_t> raw(sounds.size() + 1, 0);
    for (size_t i = 0, count = 0; i < input.size(); ++i) {
        if (!is_tone(input[i], true) && input[i] != '\'') ++count;
        raw[count] = i + 1;
    }
    // Only a reading made of recorded syllables can belong to a fully spelled
    // word; this skips abbreviation buckets such as "w" or "zhd" outright.
    const auto syllable = [&](std::string_view value) {
        return std::binary_search(lexicon.singleReadings.begin(), lexicon.singleReadings.end(), value);
    };
    std::vector<char> spelled(sounds.size() + 1, 0);
    spelled[0] = 1;
    for (size_t end = 1; end <= sounds.size(); ++end)
        for (size_t size = 1; size <= std::min(size_t(6), end) && !spelled[end]; ++size)
            spelled[end] = spelled[end - size] && syllable(std::string_view(sounds).substr(end - size, size));
    const size_t inspected = perReading == SIZE_MAX ? SIZE_MAX : 256;
    std::vector<Fallback> result;
    std::unordered_set<std::string_view> visible;
    for (size_t length = std::min(sounds.size(), lexicon.longest); length > 0 && sounds.size() > 1; --length) {
        if (length == sounds.size() || !spelled[length]) continue; // Whole-input matches are already candidates.
        // Cut only where the rest can begin a syllable: a recorded
        // single-character reading starts there, or a short unfinished tail
        // can still grow into one. This rejects ban|gwo while keeping xi|an.
        const auto rest = std::string_view(sounds).substr(length);
        bool boundary = reading.boundaries[length] != 0;
        for (size_t size = std::min(size_t(6), rest.size()); size > 0 && !boundary; --size)
            boundary = syllable(rest.substr(0, size));
        if (!boundary && rest.size() <= 5) {
            const auto found = std::lower_bound(lexicon.singleReadings.begin(), lexicon.singleReadings.end(), rest);
            boundary = found != lexicon.singleReadings.end() && found->substr(0, rest.size()) == rest;
        }
        if (!boundary) continue;
        const auto range = lexicon.range(std::string_view(sounds).substr(0, length));
        visible.clear();
        for (size_t i = range.first; i < range.second && i - range.first < inspected && visible.size() < perReading; ++i) {
            const auto &word = lexicon.word(i, hans);
            if (!fully_spelled(word) || !reading.matches(word, 0, length)) continue;
            const auto text = word.text(hans);
            if (visible.insert(text).second)
                result.push_back({std::string(text), raw[length], double(word.score(hans))});
            if (result.size() == single_character_candidate_limit) return result;
        }
    }
    return result;
}

// A short suffix is unfinished only when a recorded single-character reading
// extends it and no recorded single-character reading ends at the suffix.
// Multi-character abbreviation buckets alone are not syllable evidence.
bool incomplete_pinyin_syllable(const Lexicon &lexicon, std::string_view suffix) {
    if (suffix.empty() || suffix.size() > 5) return false;
    const auto found = std::lower_bound(lexicon.singleReadings.begin(), lexicon.singleReadings.end(), suffix);
    return found != lexicon.singleReadings.end() && found->size() > suffix.size() &&
           found->substr(0, suffix.size()) == suffix;
}

// Preserve a sentence built from several dictionary entries while its next
// syllable is incomplete. Selection commits only the completed prefix.
std::vector<Fallback> unfinished_sentence(const Lexicon &lexicon, std::string_view input, bool hans,
                                          bool onsetOnly = false, bool syllableOnly = false, size_t after = 0,
                                          const Personal *personal = nullptr) {
    if (!lexicon.pinyin || input.size() < 4 || input.size() > max_input) return {};
    for (size_t tail = 1; tail <= std::min(size_t(5), input.size() - 1); ++tail) {
        if (input.size() - tail <= after) break;
        const auto suffix = input.substr(input.size() - tail);
        if (!std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= 'a' && c <= 'z'; })) continue;
        if (syllableOnly && !incomplete_pinyin_syllable(lexicon, suffix)) continue;
        // Only use unmistakably unfinished initials to outrank existing results.
        // Finals such as n/ng may complete an otherwise valid whole sentence.
        if (onsetOnly) {
            constexpr std::string_view initials[] = {"b", "p", "f", "d", "t", "l", "g", "k", "h", "j", "q", "x", "zh", "ch", "sh", "r", "z", "c", "s", "y", "w", "n"};
            if (std::find(std::begin(initials), std::end(initials), suffix) == std::end(initials)) continue;
            if (suffix == "n" && (input.size() < 2 || input[input.size() - 2] != 'n')) continue;
        }
        const auto possible = std::lower_bound(lexicon.buckets.begin(), lexicon.buckets.end(), suffix,
            [](const Lexicon::Bucket &bucket, std::string_view value) { return bucket.key < value; });
        if (possible == lexicon.buckets.end() || possible->key.substr(0, suffix.size()) != suffix) continue;
        const size_t count = input.size() - tail;
        auto phrases = decode(lexicon, input.substr(0, count), hans, true, nullptr, nullptr, 0, personal);
        std::vector<Fallback> result;
        for (const auto &phrase : phrases) {
            const auto characters = std::count_if(phrase.begin(), phrase.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
            if (characters >= (onsetOnly ? 3 : 2)) result.push_back({phrase, count, 0});
            if (result.size() == 3) break;
        }
        if (!result.empty()) return result;
    }
    return {};
}

// Offer completed Zhuyin prefixes while keeping all following raw keys intact.
// Explicit tones are reliable syllable boundaries; when tones are omitted, a
// few additional boundaries before an initial can be decoded from the lexicon.
std::vector<Fallback> zhuyin_segments(const Lexicon &lexicon, std::string_view input, bool hans, const Personal *personal = nullptr) {
    if (lexicon.pinyin || input.size() < 4) return {};
    std::vector<Fallback> result, alternatives;
    auto collect = [&](size_t count) {
        auto phrases = decode(lexicon, input.substr(0, count), hans, false, nullptr, nullptr, 0, personal);
        if (phrases.empty()) return;
        const bool newest = result.empty();
        auto &phrase = phrases.front();
        const auto characters = std::count_if(phrase.begin(), phrase.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
        if (characters >= 2) {
            result.push_back({std::move(phrase), count, 0});
            if (newest)
                for (size_t i = 1; i < std::min(size_t(3), phrases.size()); ++i)
                    if (std::count_if(phrases[i].begin(), phrases[i].end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) >= 2)
                        alternatives.push_back({std::move(phrases[i]), count, 0});
        }
    };
    // Decode at most 64 leading keys even when the composition is longer.
    // Returning no segments past that limit made long sentences fall back to
    // single characters. Selecting a bounded prefix leaves all later keys intact.
    const size_t upper = std::min(input.size() - 1, size_t(64));
    const bool longComposition = input.size() > 64;
    size_t inspected = 0;
    for (size_t count = upper; count > 0 && inspected < (longComposition ? 2 : 6); --count) {
        if (!is_tone(input[count - 1]) || is_tone(input[count]) || input[count] == '\'') continue;
        ++inspected;
        collect(count);
    }
    // Bound the additional search and avoid splitting immediately before a
    // tone, which belongs to the preceding syllable.
    inspected = 0;
    constexpr auto initials = keys.substr(0, 21);
    for (size_t count = upper; count >= 4 && inspected < (longComposition ? 2 : 4) && result.size() < 6; --count) {
        if (is_tone(input[count - 1]) || initials.find(input[count]) == initials.npos) continue;
        ++inspected;
        collect(count);
    }
    for (auto &item : alternatives) result.push_back(std::move(item));
    return result;
}

// CN-06c: the fewest fully spelled dictionary words that spell text over the
// sounds of input, as (untoned reading, text) pairs. Empty when there is no
// such split: corrections, abbreviations and anything not from the lexicon
// are not learned word by word. Runs once per confirmed selection.
std::vector<std::pair<std::string, std::string>> composition(const Lexicon &lexicon, std::string_view input,
                                                             std::string_view text, bool hans) {
    if (!lexicon.pinyin || input.empty() || text.empty() || input.size() > 96 || text.size() > 512) return {};
    const Reading reading(input, true);
    const auto &sounds = reading.sounds;
    if (sounds.empty()) return {};
    struct Step { size_t from, done; size_t words; std::string_view piece; };
    // Keyed by (sounds consumed, text bytes consumed). std::map visits states
    // in order of sounds consumed, so every state is final before expansion.
    std::map<std::pair<size_t, size_t>, Step> reached{{{0, 0}, {0, 0, 0, {}}}};
    size_t scanned = 0;
    for (auto state = reached.begin(); state != reached.end(); ++state) {
        const auto [at, done] = state->first;
        if (at == sounds.size()) continue;
        for (size_t end = at + 1; end <= std::min(sounds.size(), at + lexicon.longest); ++end) {
            const auto range = lexicon.range(std::string_view(sounds).substr(at, end - at));
            for (size_t i = range.first; i < range.second; ++i) {
                if (++scanned > 200000) return {};
                const auto &word = lexicon.words[i];
                const auto piece = word.text(hans);
                if (piece.size() > text.size() - done || text.compare(done, piece.size(), piece) != 0) continue;
                if (!fully_spelled(word) || !reading.matches(word, at, end)) continue;
                const auto key = std::make_pair(end, done + piece.size());
                const auto found = reached.find(key);
                if (found == reached.end() || found->second.words > state->second.words + 1)
                    reached[key] = {at, done, state->second.words + 1, piece};
            }
        }
    }
    auto found = reached.find({sounds.size(), text.size()});
    if (found == reached.end()) return {};
    std::vector<std::pair<std::string, std::string>> result;
    for (auto key = found->first; key.first;) {
        const auto &step = reached.at(key);
        result.emplace_back(sounds.substr(step.from, key.first - step.from), std::string(step.piece));
        key = {step.from, step.done};
    }
    std::reverse(result.begin(), result.end());
    return result;
}

// DICT-02a: diagnostics only, never used for ranking. The best-scoring split
// of text over the sounds of input into lexicon and user words, each scored
// as the sentence decoder scores it (see decode): its base from weight and
// prior, the personal lift under the running best of its span in beam order,
// and the join towards the rest of the text. Limits are generous; the beam
// width and path limits of the decoder are not applied.
std::string explain_composition(const Lexicon &lexicon, const Personal *personal, std::string_view input,
                                std::string_view text, bool hans) {
    if (!lexicon.pinyin || input.empty() || text.empty() || input.size() > 96 || text.size() > 512) return {};
    const Reading reading(input, true);
    const auto &sounds = reading.sounds;
    if (sounds.empty()) return {};
    const auto userHold = personal ? personal->user_words(lexicon) : nullptr;
    const UserWords *user = userHold.get();
    constexpr double none = -std::numeric_limits<double>::infinity();
    struct Piece {
        std::string reading, text, traditional;
        const char *source;
        int weight;
        double prior, base, bonus, lift;
        uint32_t pair;
    };
    std::map<std::pair<size_t, size_t>, std::vector<Piece>> spans;
    size_t scanned = 0;
    const auto options = [&](size_t start, size_t end) -> const std::vector<Piece> & {
        const auto found = spans.find({start, end});
        if (found != spans.end()) return found->second;
        std::vector<Piece> pieces;
        const auto span = std::string_view(sounds).substr(start, end - start);
        const auto range = lexicon.range(span);
        const auto *learned = personal ? personal->find(hans, span) : nullptr;
        const auto userSpan = user ? user->range(span) : std::pair<size_t, size_t>{0, 0};
        double top = none;
        std::vector<std::string_view> used;
        for (size_t index = range.first; index < range.second && scanned < 400000; ++index, ++scanned) {
            const auto &word = lexicon.word(index, hans);
            if (!reading.matches(word, start, end)) continue;
            const auto spelling = word.text(true);
            if (std::find(used.begin(), used.end(), spelling) != used.end()) continue;
            used.push_back(spelling);
            const bool mine = userSpan.first != userSpan.second && user->has(userSpan, word.text(hans), hans);
            const double bonus = mine ? personal_cap : Personal::bonus(learned, word.text(hans));
            const double base = static_cast<double>(word.score(hans)) / 700.0 - 19.0 + word.prior / 100.0;
            top = std::max(top, base);
            const auto piece = word.text(hans);
            if (text.find(piece) == text.npos) continue;
            pieces.push_back({std::string(word.reading()), std::string(piece), std::string(word.text(false)),
                              mine ? "lexicon+user" : "lexicon", word.score(hans), word.prior / 100.0, base, bonus,
                              Personal::lifted(base, bonus, top) - base, word.pair});
        }
        for (size_t i = userSpan.first; i < userSpan.second; ++i) {
            const auto index = user->sorted[i];
            const auto &word = user->words[index];
            if (!user_matches(reading, word, user->loose[index], start, end)) continue;
            const auto spelling = word.text(true);
            if (std::find(used.begin(), used.end(), spelling) != used.end()) continue;
            used.push_back(spelling);
            const double base = static_cast<double>(user_weight) / 700.0 - 19.0;
            const auto piece = word.text(hans);
            if (text.find(piece) == text.npos) continue;
            pieces.push_back({std::string(word.reading()), std::string(piece), std::string(word.text(false)), "user",
                              user_weight, 0, base, personal_cap, Personal::lifted(base, personal_cap, top) - base, 0});
        }
        return spans.emplace(std::make_pair(start, end), std::move(pieces)).first->second;
    };
    const size_t longest = std::max(lexicon.longest, user ? user->longest : size_t(0));
    struct Best { double value = -std::numeric_limits<double>::infinity(); std::vector<std::pair<const Piece *, double>> path; std::string traditional; uint32_t first = 0; };
    std::map<std::pair<size_t, size_t>, Best> memo;
    // Positions only grow, so the recursion is at most one level per sound.
    const auto solve = [&](const auto &self, size_t at, size_t done) -> const Best & {
        const auto found = memo.find({at, done});
        if (found != memo.end()) return found->second;
        Best best;
        if (at == sounds.size()) {
            if (done == text.size()) { best.value = 0; best.first = lexicon.pairEnd; }
        } else if (done < text.size()) {
            for (size_t end = at + 1; end <= std::min(sounds.size(), at + longest); ++end) {
                for (const auto &piece : options(at, end)) {
                    if (piece.text.size() > text.size() - done || text.compare(done, piece.text.size(), piece.text) != 0) continue;
                    const auto &rest = self(self, end, done + piece.text.size());
                    if (rest.value == none) continue;
                    const double join = lexicon.join_bonus(piece.traditional, rest.traditional, hans, false, !lexicon.pairs.empty()) +
                        lexicon.pair_bonus(piece.pair, rest.first) + (at == 0 ? lexicon.pair_bonus(lexicon.pairBegin, piece.pair) : 0);
                    const double value = piece.base + piece.lift + join + rest.value;
                    if (value <= best.value) continue;
                    best.value = value;
                    best.path.assign(1, {&piece, join});
                    best.path.insert(best.path.end(), rest.path.begin(), rest.path.end());
                    best.traditional = piece.traditional + rest.traditional;
                    best.first = piece.pair;
                }
            }
        }
        return memo.emplace(std::make_pair(at, done), std::move(best)).first->second;
    };
    const auto &best = solve(solve, 0, 0);
    if (best.value == none) return {};
    std::string result;
    char number[160];
    for (const auto &[piece, join] : best.path) {
        std::snprintf(number, sizeof(number), "\t%d\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n", piece->weight, piece->prior, piece->base,
                      piece->bonus, piece->lift, join);
        result += piece->reading + '\t' + piece->text + '\t' + piece->source + number;
    }
    return result;
}
} // namespace

struct MPNLexicon { std::shared_ptr<const Lexicon> value; };
struct MPNPersonal { std::shared_ptr<Personal> value = std::make_shared<Personal>(); };
struct MPNSession {
    struct JapaneseWord { std::string reading, text; int weight; };
    std::shared_ptr<const Lexicon> lexicon;
    // CN-06c: optional, shared with the host's MPNPersonal; null or empty
    // leaves every result exactly as without it.
    std::shared_ptr<const Personal> personal;
    std::string candidateWords;
    // DICT-02a: the buffer of mpn_candidate_explain and mpn_explain_text.
    std::string explanation;
    std::string input, preedit, committed, mixedPinyin, segmentContext, exactPinyin, customChinese;
    std::vector<JapaneseWord> japanese;
    std::vector<Candidate> candidates;
    size_t page = 0;
    unsigned expansion = 0;
    bool hans = false;
    bool requireFullCandidate = false;
    // CN-03: enabled fuzzy pinyin pairs (MPN_FUZZY_*); 0 leaves every result unchanged.
    uint32_t fuzzy = 0;
    // CN-05: typo correction, on unless the host turns it off.
    bool typo = true;
    size_t sentenceCount = 0, literalTail = 0;

    const Personal *learned() const { return personal.get(); }

    void reset() {
        input.clear();
        preedit.clear();
        candidates.clear();
        segmentContext.clear();
        sentenceCount = literalTail = 0;
        page = 0;
        expansion = 0;
    }

    // While a recorded Japanese reading is still incomplete, keep the
    // preceding fully decoded Chinese phrase selectable. Never guess or
    // consume the unfinished Japanese keys; selecting a phrase leaves them
    // in the session for subsequent input.
    std::vector<Fallback> unfinishedJapanesePrefix(bool allowOne) const {
        if (!lexicon->pinyin || input.size() < 5 || input.size() > max_input || japanese.empty()) return {};
        const auto maxTail = std::min(size_t(16), input.size() - 2);
        for (size_t tail = maxTail; tail >= (allowOne ? 1u : 2u); --tail) {
            const auto suffix = std::string_view(input).substr(input.size() - tail);
            const bool hasReading = std::any_of(japanese.begin(), japanese.end(), [&](const JapaneseWord &word) {
                return word.weight >= 4000 && word.reading.size() > tail &&
                       std::string_view(word.reading).substr(0, tail) == suffix;
            });
            if (!hasReading) continue;
            const size_t cut = input.size() - tail;
            auto phrases = decode(*lexicon, std::string_view(input).substr(0, cut), hans, true, nullptr, nullptr, 0, learned());
            std::vector<Fallback> result;
            for (auto &phrase : phrases) {
                const auto characters = std::count_if(phrase.begin(), phrase.end(),
                    [](unsigned char c) { return (c & 0xc0) != 0x80; });
                if (characters >= 2) result.push_back({std::move(phrase), cut, 0});
                if (result.size() == 3) break;
            }
            if (!result.empty()) return result;
        }
        return {};
    }

    void refresh() {
        expansion = 0;
        std::string marked;
        if (lexicon->pinyin) marked = input;
        else for (char key : input) marked += symbols[keys.find(key)];
        std::vector<CandidateMetadata> metadata;
        bool singleReading = false;
        auto decoded = decode(*lexicon, input, hans, false, &metadata, &singleReading, 0, learned(), fuzzy, typo);
        // CN-04: the decoder put an abbreviated reading first. CN-05: or a
        // typo correction, which is placed the same way.
        const bool abbreviationLead = !metadata.empty() && (metadata.front().abbreviated || metadata.front().typo);
        const bool abbreviationTrailing = abbreviationLead && metadata.front().trailing;
        const std::string abbreviationText = abbreviationLead ? decoded.front() : std::string();
        std::vector<Candidate> ranked;
        ranked.reserve(decoded.size());
        for (size_t i = 0; i < decoded.size(); ++i)
            ranked.push_back({std::move(decoded[i]), input.size(), metadata[i]});
        const auto ranked_limit = singleReading ? single_character_candidate_limit : candidate_limit;
        const auto sameSelection = [](const Candidate &candidate, const Fallback &item) {
            return candidate.text == item.text && candidate.consumed == item.consumed;
        };
        const auto partialCandidate = [](Fallback &item) {
            return Candidate{std::move(item.text), item.consumed, {MPN_CANDIDATE_PARTIAL, MPN_SCORE_NONE, 0}};
        };
        // Several ranking branches need the same existence check. Decode it
        // once per refresh, lazily, rather than rebuilding identical beams.
        int completeReading = -1;
        const auto hasCompleteReading = [&]() {
            if (completeReading < 0) completeReading = !decode(*lexicon, input, hans, true, nullptr, nullptr, 0, nullptr, fuzzy).empty();
            return completeReading != 0;
        };
        auto partial = unfinished_phrase(*lexicon, input, hans);
        auto prepend = [&](std::vector<Fallback> &items) {
          for (auto it = items.rbegin(); it != items.rend(); ++it) {
            const auto duplicate = std::find_if(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return sameSelection(candidate, *it); });
            if (duplicate != ranked.end()) {
                ranked.erase(duplicate);
            }
            ranked.insert(ranked.begin(), partialCandidate(*it));
            if (ranked.size() > ranked_limit) ranked.pop_back();
          }
        };
        if (!partial.empty() && hasCompleteReading()) {
            // A complete sentence such as zhi-dao-le must not be displaced by
            // a shorter word such as zhi-da merely because 'ole' is unfinished.
            size_t position = std::min(size_t(3), ranked.size());
            for (auto &item : partial) {
                if (std::any_of(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return sameSelection(candidate, item); })) continue;
                ranked.insert(ranked.begin() + position++, partialCandidate(item));
                if (ranked.size() > ranked_limit) ranked.pop_back();
            }
        } else prepend(partial);
        auto predictive = predictive_segments(*lexicon, input, hans, learned());
        if (!predictive.empty()) {
            const bool noComplete = lexicon->pinyin ? !hasCompleteReading() : ranked.empty();
            if (noComplete) prepend(predictive);
            else {
                // An untoned prefix can also form a valid whole sentence.
                // Keep the first three complete interpretations ahead of it.
                size_t position = std::min(size_t(3), ranked.size());
                for (auto &item : predictive) {
                    if (std::any_of(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return sameSelection(candidate, item); })) continue;
                    ranked.insert(ranked.begin() + position, partialCandidate(item));
                    ++position;
                    if (ranked.size() > ranked_limit) ranked.pop_back();
                }
            }
        }
        if (!partial.empty()) {
            // A shorter dictionary word must not prevent a longer decoded
            // sentence prefix from remaining selectable. Search only beyond
            // the prefix already present, and only for an unfinished syllable.
            const size_t consumed = std::max(partial.front().consumed,
                predictive.empty() ? size_t(0) : predictive.front().consumed);
            if (consumed + 1 < input.size() && !hasCompleteReading()) {
                auto longer = unfinished_sentence(*lexicon, input, hans, false, true, consumed, learned());
                prepend(longer);
            }
        }
        if (!ranked.empty() && partial.empty() && predictive.empty() && lexicon->pinyin && input.size() >= 6 && input.size() <= max_input &&
            std::string_view("bpftdlgkhjqxzcrsywn").find(input.back()) != std::string_view::npos &&
            (input.back() != 'n' || input[input.size() - 2] == 'n') &&
            !has_exact_match(*lexicon, input) &&
            // Beyond 64 keys, avoid a second full-sentence decode on every
            // key. Final ng and er can complete a pinyin syllable; other
            // consonant initials here are necessarily incomplete.
            (input.size() <= 64 ? !hasCompleteReading() :
                !((input.back() == 'g' && input[input.size() - 2] == 'n') ||
                  (input.back() == 'r' && input[input.size() - 2] == 'e')))) {
            auto sentence = unfinished_sentence(*lexicon, input, hans, true, false, 0, learned());
            prepend(sentence);
        }
        if (ranked.empty()) {
            for (auto &item : unfinished_sentence(*lexicon, input, hans, false, false, 0, learned())) {
                ranked.push_back(partialCandidate(item));
            }
        }
        if (lexicon->pinyin && !ranked.empty() && partial.empty() && predictive.empty() && input.size() >= 6 &&
            std::none_of(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return candidate.consumed < input.size(); }) &&
            !has_exact_match(*lexicon, input)) {
            // Do not let abbreviation chains such as 狄歐尼 hide a completed
            // Chinese phrase while its next real pinyin syllable is unfinished.
            auto sentence = unfinished_sentence(*lexicon, input, hans, false, true, 0, learned());
            if (!sentence.empty() && !hasCompleteReading()) prepend(sentence);
        }
        if (lexicon->pinyin && !japanese.empty() && input.size() >= 5 && input.size() <= max_input &&
            !has_exact_match(*lexicon, input)) {
            // A one-letter Japanese onset is ambiguous; offer its preceding
            // phrase only when no Chinese result can otherwise be decoded.
            auto segments = unfinishedJapanesePrefix(ranked.empty());
            // Preserve the best existing Chinese interpretations while making
            // a completed phrase available on the first page. An identical
            // existing candidate keeps its original consumed-key semantics.
            size_t position = std::min(size_t(3), ranked.size());
            for (auto &item : segments) {
                const auto existing = std::find_if(ranked.begin(), ranked.end(), [&](const Candidate &candidate) {
                    return sameSelection(candidate, item);
                });
                if (existing != ranked.end()) {
                    const auto index = static_cast<size_t>(existing - ranked.begin());
                    if (index < position) continue;
                    auto candidate = std::move(ranked[index]);
                    ranked.erase(ranked.begin() + index);
                    ranked.insert(ranked.begin() + position, std::move(candidate));
                } else {
                    ranked.insert(ranked.begin() + position, partialCandidate(item));
                }
                ++position;
                if (ranked.size() > ranked_limit) ranked.pop_back();
            }
        }
        if (!lexicon->pinyin) {
            auto segments = zhuyin_segments(*lexicon, input, hans, learned());
            // Preserve the best whole-sentence alternatives. Place up to
            // three segments on the first page and the rest on later pages.
            size_t position = std::min(size_t(3), ranked.size());
            size_t inserted = 0;
            for (auto &segment : segments) {
                // Keep an exact full-input candidate's selection semantics.
                if (std::any_of(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return sameSelection(candidate, segment); })) continue;
                if (inserted == 3) position = std::min(size_t(10), ranked.size());
                ranked.insert(ranked.begin() + position, partialCandidate(segment));
                ++position;
                ++inserted;
                if (ranked.size() > ranked_limit) ranked.pop_back();
            }
        }
        if (lexicon->pinyin && !ranked.empty()) {
            size_t completed = input.size();
            if (!hasCompleteReading()) {
                completed = 0;
                for (const auto &candidate : ranked)
                    if (candidate.consumed < input.size()) completed = std::max(completed, candidate.consumed);
            }
            // An unfinished following word must not remove the earlier split
            // points of a completed phrase. The existing bounded segmenter
            // consumes only keys within that prefix, leaving the entire tail.
            auto [segments, primaryCount] = pinyin_segments(*lexicon, std::string_view(input).substr(0, completed), hans, learned());
            // Keep the existing leading split choices. Append extra homophones
            // after the original candidates so complete sentences keep their ranks.
            const size_t leadingCount = completed < input.size() ? std::min(size_t(3), segments.size()) : primaryCount;
            size_t position = std::min(size_t(3), ranked.size());
            for (size_t i = 0; i < segments.size(); ++i) {
                auto &segment = segments[i];
                if (std::any_of(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return sameSelection(candidate, segment); })) continue;
                if (i >= leadingCount) position = ranked.size();
                ranked.insert(ranked.begin() + position, partialCandidate(segment));
                ++position;
                if (ranked.size() > ranked_limit) ranked.pop_back();
            }
        }
        if (abbreviationLead) {
            // CN-04: the best abbreviated reading leads ahead of the prefixes
            // offered for unfinished keys, except that a single initial
            // after complete syllables (nihaom) may still be the next
            // syllable being typed: the exact prefix (你好) keeps first place
            // and the completion (你好吗) follows it.
            const auto found = std::find_if(ranked.begin(), ranked.end(), [&](const Candidate &candidate) {
                return (candidate.metadata.abbreviated || candidate.metadata.typo) && candidate.consumed == input.size() &&
                    candidate.text == abbreviationText;
            });
            const size_t target = abbreviationTrailing && !ranked.empty() && ranked.front().consumed < input.size() ? 1 : 0;
            if (found != ranked.end() && static_cast<size_t>(found - ranked.begin()) > target)
                std::rotate(ranked.begin() + target, found, found + 1);
        }
        if (ranked.empty()) {
            for (auto &item : fallback(*lexicon, input, hans, true, typo)) {
                ranked.push_back({std::move(item.text), item.consumed,
                    {static_cast<uint32_t>(item.consumed < input.size() ? MPN_CANDIDATE_PARTIAL : MPN_CANDIDATE_CORRECTION),
                     MPN_SCORE_FALLBACK, item.score, false, 0, false, item.typo}});
            }
        }
        if (!segmentContext.empty() && ranked.size() > 1) {
            // Only promote a strong recorded phrase within a bounded candidate
            // window. The original index supplies a modest prior; no text or
            // consumed-key lengths are changed by this local reordering.
            const size_t limit = std::min(size_t(64), ranked.size());
            struct RankedIndex { size_t index; double score; };
            // Context may reorder homophones for the same selection span,
            // but must not move a shorter prefix over an existing whole path.
            std::unordered_map<size_t, std::vector<RankedIndex>> groups;
            const bool exact = has_exact_match(*lexicon, input);
            for (size_t i = 0; i < limit; ++i) {
                const auto traditional = lexicon->traditionalize(ranked[i].text, false);
                const double bonus = traditional.empty() || (exact && ranked[i].consumed != input.size())
                    ? 0 : lexicon->join_bonus(segmentContext, traditional, hans, true);
                groups[ranked[i].consumed].push_back({i, -static_cast<double>(i) * 0.18 + (bonus >= 2.5 ? bonus : 0)});
            }
            std::vector<Candidate> ordered(limit);
            for (const auto &group : groups) {
                auto order = group.second;
                std::stable_sort(order.begin(), order.end(), [](const RankedIndex &a, const RankedIndex &b) { return a.score > b.score; });
                for (size_t i = 0; i < order.size(); ++i)
                    ordered[group.second[i].index] = std::move(ranked[order[i].index]);
            }
            for (size_t i = 0; i < limit; ++i) ranked[i] = std::move(ordered[i]);
        }
        // A list that fits one page is left alone: its next page is the first
        // expansion, which begins with the same ladder.
        if (!requireFullCandidate && lexicon->pinyin && !singleReading && ranked.size() > page_size) {
            // When the first page is wrong, the second page offers building
            // blocks instead of more variants of the same sentence: up to two
            // words for each of the two longest leading multi-syllable
            // readings, then the commonest characters of the leading
            // syllable. The first page keeps its content and order; the
            // complete ladder follows on the first candidate expansion.
            std::vector<Fallback> head;
            std::vector<std::pair<size_t, size_t>> taken; // consumed keys, entries taken
            const auto ladder = leading_words(*lexicon, input, hans, page_size);
            const size_t firstSyllable = [&] {
                // The longest leading reading that has single characters.
                for (const auto &item : ladder)
                    if (han_characters(item.text).size() == 1) return item.consumed;
                return size_t(0);
            }();
            for (int pass = 0; pass < 2; ++pass) {
                for (const auto &item : ladder) {
                    if (head.size() == page_size) break;
                    const bool character = item.consumed == firstSyllable && han_characters(item.text).size() == 1;
                    if (pass == 0 ? item.consumed <= firstSyllable : !character) continue;
                    auto group = std::find_if(taken.begin(), taken.end(), [&](const auto &entry) { return entry.first == item.consumed; });
                    if (pass == 0) {
                        if (group == taken.end()) {
                            if (taken.size() == 2) continue;
                            taken.emplace_back(item.consumed, 0); group = taken.end() - 1;
                        }
                        if (group->second == 2) continue;
                        ++group->second;
                    }
                    if (std::none_of(ranked.begin(), ranked.end(), [&](const Candidate &candidate) { return sameSelection(candidate, item); }))
                        head.push_back(item);
                }
            }
            size_t position = std::min(page_size, ranked.size());
            for (auto &item : head)
                ranked.insert(ranked.begin() + position++, {std::move(item.text), item.consumed, {MPN_CANDIDATE_PARTIAL, MPN_SCORE_LEXICON, item.score}});
        }
        if (requireFullCandidate && lexicon->pinyin && !input.empty()) {
            // The Android presentation needs complete interpretations first,
            // followed by selectable prefixes. Never compare scores from
            // different decoder paths or consume a displayed raw suffix.
            std::vector<Candidate> complete, rest;
            for (auto &candidate : ranked) {
                const bool whole = candidate.consumed == input.size() &&
                    candidate.metadata.kind != MPN_CANDIDATE_CORRECTION &&
                    candidate.metadata.kind != MPN_CANDIDATE_PARTIAL &&
                    candidate.metadata.scoreKind != MPN_SCORE_FALLBACK;
                if (whole && complete.size() < 3) complete.push_back(std::move(candidate));
                else rest.push_back(std::move(candidate));
            }
            literalTail = 0;
            if (complete.empty()) {
                auto prefix = std::find_if(rest.begin(), rest.end(), [&](const Candidate &candidate) {
                    return candidate.consumed > 0 && candidate.consumed < input.size();
                });
                if (prefix != rest.end()) {
                    auto sentence = *prefix;
                    sentence.literalTail = input.size() - sentence.consumed;
                    sentence.text += input.substr(sentence.consumed);
                    literalTail = sentence.literalTail;
                    complete.push_back(std::move(sentence));
                } else {
                    literalTail = input.size();
                    complete.push_back({input, input.size(), {MPN_CANDIDATE_UNKNOWN, MPN_SCORE_NONE, 0}, 0});
                }
            }
            sentenceCount = complete.size();
            complete.insert(complete.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
            if (std::none_of(complete.begin(), complete.end(), [&](const Candidate &candidate) {
                return candidate.text == input && candidate.consumed == input.size();
            })) complete.push_back({input, input.size(), {MPN_CANDIDATE_UNKNOWN, MPN_SCORE_NONE, 0}, 0});
            ranked = std::move(complete);
            if (ranked.size() > ranked_limit) ranked.resize(ranked_limit);
        } else {
            sentenceCount = literalTail = 0;
        }
        preedit.swap(marked);
        candidates.swap(ranked);
        page = 0;
    }
};

MPNLexicon *mpn_lexicon_create(const char *data, size_t size) {
    if (!data || !size) return nullptr;
    try {
        auto lexicon = std::make_shared<Lexicon>();
        if (!lexicon->load({data, size})) return nullptr;
        return new MPNLexicon{std::move(lexicon)};
    } catch (...) { return nullptr; }
}
void mpn_lexicon_destroy(MPNLexicon *lexicon) { delete lexicon; }

MPNSession *mpn_session_create(const MPNLexicon *lexicon) {
    if (!lexicon) return nullptr;
    try {
        auto session = std::make_unique<MPNSession>();
        session->lexicon = lexicon->value;
        return session.release();
    } catch (...) { return nullptr; }
}
void mpn_session_destroy(MPNSession *session) { delete session; }

int mpn_add_japanese_word(MPNSession *session, const char *reading, const char *text, int weight) {
    // 3500 is reserved for pinyin aliases of all-kanji Japanese words. They
    // require another independently recognised Japanese fragment to split a sentence.
    if (!session || !reading || !text || (weight < 4000 && weight != 3500) || session->japanese.size() >= 512) return 0;
    const std::string_view key(reading), value(text);
    // Most two-letter readings collide with common Chinese syllables. "au"
    // (会う) is explicitly supported; it is not a valid standalone pinyin
    // syllable, unlike e.g. "ai" or "su".
    if ((key.size() < 3 && !(key == "au" && weight >= 4000)) || key.size() > 20 || value.empty() || value.size() > 128 ||
        !std::all_of(key.begin(), key.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return 0;
    try { session->japanese.push_back({std::string(key), std::string(value), weight}); return 1; }
    catch (...) { return 0; }
}

const char *mpn_mixed_pinyin(MPNSession *session, const char *reading, int simplified) {
    if (!session) return "";
    session->mixedPinyin.clear();
    if (!reading || !session->lexicon->pinyin) return session->mixedPinyin.c_str();
    const std::string_view input(reading);
    if (input.size() < 5 || input.size() > 80 || !std::all_of(input.begin(), input.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
        return session->mixedPinyin.c_str();
    try {
        struct Match { size_t start, end; const MPNSession::JapaneseWord *word; };
        std::vector<Match> matches;
        auto precedes = [](const Match &a, const Match &b) {
            if (a.start != b.start) return a.start < b.start;
            if (a.end != b.end) return a.end > b.end;
            return a.word->weight > b.word->weight;
        };
        // Keep a bounded shortlist even for adversarial repeated input and
        // duplicate readings: do not allocate/sort every possible occurrence.
        constexpr size_t matchShortlistLimit = 32;
        for (const auto &word : session->japanese) {
            for (size_t pos = input.find(word.reading); pos != input.npos; pos = input.find(word.reading, pos + 1)) {
                if (pos == 0 && word.reading.size() == input.size()) continue;
                const Match match{pos, pos + word.reading.size(), &word};
                if (matches.size() == matchShortlistLimit && !precedes(match, matches.back())) break;
                const auto at = std::lower_bound(matches.begin(), matches.end(), match, precedes);
                matches.insert(at, match);
                if (matches.size() > matchShortlistLimit) matches.pop_back();
            }
        }
        // Prefer a longer recorded form over splitting its own shorter form
        // into Japanese plus accidental Chinese fragments (推し / 推し活).
        std::vector<Match> compact;
        for (const auto &match : matches) {
            const bool covered = std::any_of(compact.begin(), compact.end(), [&](const Match &other) {
                return other.start == match.start && other.end > match.end &&
                    other.word->text.compare(0, match.word->text.size(), match.word->text) == 0;
            });
            if (!covered) compact.push_back(match);
            if (compact.size() == 16) break;
        }
        matches = std::move(compact);
        size_t sentenceDecodes = 0;
        constexpr size_t sentenceDecodeLimit = 12;
        using Chinese = std::pair<std::string, int>;
        std::unordered_map<std::string, Chinese> cache;
        auto chinese = [&](std::string_view part) -> Chinese {
            if (part.empty()) return {"", 0};
            const std::string key(part);
            if (auto it = cache.find(key); it != cache.end()) return it->second;
            Chinese result;
            const Reading parsed(part, true);
            const auto range = session->lexicon->range(parsed.sounds);
            for (size_t i = range.first; i < range.second; ++i) {
                const auto &word = session->lexicon->word(i, simplified != 0);
                if (word.score(simplified != 0) < 2000) break;
                if (parsed.matches(word, 0, parsed.sounds.size())) { result = {std::string(word.text(simplified != 0)), word.score(simplified != 0)}; break; }
            }
            if (result.first.empty() && sentenceDecodes < sentenceDecodeLimit) {
                ++sentenceDecodes;
                auto sentences = decode(*session->lexicon, part, simplified != 0, true);
                if (!sentences.empty()) result = {std::move(sentences.front()), 2000};
            }
            cache.emplace(key, result);
            return result;
        };
        int64_t bestScore = -1;
        auto consider = [&](const Match &first, const Match *second, const Match *third) {
            if (first.word->weight == 3500 && (!second || second->word->weight == 3500) &&
                (!third || third->word->weight == 3500)) return;
            const auto end = third ? third->end : second ? second->end : first.end;
            if (first.start == 0 && end == input.size() &&
                (!second || first.end == second->start) && (!third || second->end == third->start)) return;
            const auto before = chinese(input.substr(0, first.start));
            const auto middle = second ? chinese(input.substr(first.end, second->start - first.end)) : Chinese{"", 0};
            const auto middle2 = third ? chinese(input.substr(second->end, third->start - second->end)) : Chinese{"", 0};
            const auto after = chinese(input.substr(end));
            if ((first.start && before.first.empty()) || (second && first.end != second->start && middle.first.empty()) ||
                (third && second->end != third->start && middle2.first.empty()) ||
                (end < input.size() && after.first.empty())) return;
            // Short readings are much more likely to occur incidentally inside
            // English or Chinese. Require an independent Japanese fragment or
            // an exact, high-weight Chinese word with at least two characters.
            const bool shortReading = first.word->reading.size() == 2 ||
                (second && second->word->reading.size() == 2) || (third && third->word->reading.size() == 2);
            if (shortReading) {
                const auto strongChinese = [](const Chinese &part) {
                    const auto length = std::count_if(part.first.begin(), part.first.end(),
                        [](unsigned char c) { return (c & 0xc0) != 0x80; });
                    return part.second >= 4000 && length >= 2;
                };
                const auto strongJapanese = [](const Match *part) {
                    return part && part->word->reading.size() >= 3 && part->word->weight >= 4000;
                };
                if (!strongChinese(before) && !strongChinese(middle) && !strongChinese(middle2) &&
                    !strongChinese(after) && !strongJapanese(&first) && !strongJapanese(second) && !strongJapanese(third)) return;
            }
            // Prefer a longer recorded Japanese word over splitting its prefix
            // into Japanese plus accidental low-quality Chinese fragments.
            const int japaneseLength = int(first.end - first.start + (second ? second->end - second->start : 0) +
                                           (third ? third->end - third->start : 0));
            // A single-kanji Japanese reading may also be valid pinyin. If
            // a stronger Japanese word follows immediately and the preceding
            // Chinese phrase can absorb this syllable, avoid rewarding an
            // accidental extra Japanese split over the complete Chinese.
            const auto ambiguousHanBeforeJapanese = [&](const Match *part) {
                if (!part || part->start == 0 || han_characters(part->word->text).size() != 1 ||
                    !has_exact_match(*session->lexicon, part->word->reading)) return false;
                const bool strongerFollowing = std::any_of(matches.begin(), matches.end(), [&](const Match &other) {
                    return other.start == part->end && other.word->weight >= 4000 &&
                           other.word->reading.size() >= 4;
                });
                if (!strongerFollowing) return false;
                const auto merged = chinese(input.substr(0, part->end));
                return han_characters(merged.first).size() >= 2;
            };
            // The same ambiguity can occur *after* a known Japanese word:
            // a trailing Chinese syllable may also be a Japanese one-Han
            // reading. Prefer a coherent Chinese phrase following the known
            // Japanese word over inventing another Japanese fragment.
            const auto ambiguousHanAfterJapanese = [&](const Match *part) {
                if (!part || han_characters(part->word->text).size() != 1 ||
                    !has_exact_match(*session->lexicon, part->word->reading)) return false;
                for (const Match *previous : {&first, second, third}) {
                    if (!previous || previous == part || previous->end >= part->start ||
                        previous->word->weight < 4000 || previous->word->reading.size() < 4) continue;
                    const auto merged = chinese(input.substr(previous->end, part->end - previous->end));
                    const auto traditional = session->lexicon->traditionalize(merged.first, false);
                    const auto characters = han_characters(traditional);
                    if (characters.size() < 2) continue;
                    const auto last = characters.back();
                    const auto head = std::string_view(traditional).substr(0, traditional.size() - last.size());
                    if (merged.second >= 4000 || session->lexicon->join_bonus(head, last, simplified != 0) > 0) return true;
                }
                return false;
            };
            const int ambiguityPenalty = 24000 * (int(ambiguousHanBeforeJapanese(&first)) +
                int(ambiguousHanBeforeJapanese(second)) + int(ambiguousHanBeforeJapanese(third)) +
                int(ambiguousHanAfterJapanese(second)) + int(ambiguousHanAfterJapanese(third)));
            // Both dictionaries accept non-negative int weights. Accumulate
            // several fragments in 64 bits so valid large weights cannot wrap.
            const int64_t score = static_cast<int64_t>(before.second) + middle.second + middle2.second + after.second + first.word->weight +
                              (second ? second->word->weight : 0) + (third ? third->word->weight : 0) + japaneseLength * 1000 - ambiguityPenalty;
            if (score > bestScore) {
                bestScore = score;
                session->mixedPinyin = before.first + first.word->text + middle.first + (second ? second->word->text : "") +
                                      middle2.first + (third ? third->word->text : "") + after.first;
            }
        };
        size_t combinations = 0;
        constexpr size_t combinationLimit = 256;
        for (size_t i = 0; i < matches.size() && combinations < combinationLimit; ++i) {
            ++combinations;
            consider(matches[i], nullptr, nullptr);
            for (size_t j = i + 1; j < matches.size() && combinations < combinationLimit; ++j) {
                if (matches[j].start < matches[i].end) continue;
                ++combinations;
                consider(matches[i], &matches[j], nullptr);
                for (size_t k = j + 1; k < matches.size() && combinations < combinationLimit; ++k) {
                    if (matches[k].start < matches[j].end) continue;
                    ++combinations;
                    consider(matches[i], &matches[j], &matches[k]);
                }
            }
        }
    } catch (...) { session->mixedPinyin.clear(); }
    return session->mixedPinyin.c_str();
}

const char *mpn_mixed_pinyin_prefix(MPNSession *session, const char *reading, int simplified, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!session) return "";
    session->mixedPinyin.clear();
    if (!reading || !session->lexicon->pinyin) return session->mixedPinyin.c_str();
    const std::string_view inputView(reading);
    if (inputView.size() < 4 || inputView.size() > max_input || !std::all_of(inputView.begin(), inputView.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return session->mixedPinyin.c_str();
    try {
        // Keep prefix selection available beyond the whole-sentence decoder's
        // bounded window. The reported byte count belongs to the original
        // input, so the entire unconverted suffix remains in the session.
        if (inputView.size() > 80) {
            const std::string window(inputView.substr(0, 80));
            return mpn_mixed_pinyin_prefix(session, window.c_str(), simplified, consumed);
        }
        const std::string input(inputView);
        // Preserve the longest already-decoded mixed phrase when the next
        // Chinese syllable is still an unfinished initial. This avoids
        // reducing 我好き你h to 我好き while keeping the h unconsumed.
        if (input.size() >= 6) {
            constexpr std::string_view initials[] = {"b", "p", "f", "d", "t", "l", "g", "k", "h", "j", "q", "x", "zh", "ch", "sh", "r", "z", "c", "s", "y", "w", "n"};
            for (size_t tail = 1; tail <= std::min(size_t(2), input.size() - 4); ++tail) {
                const auto suffix = std::string_view(input).substr(input.size() - tail);
                if (std::find(std::begin(initials), std::end(initials), suffix) == std::end(initials)) continue;
                if (tail == 1 && ((suffix == "n" && input[input.size() - 2] != 'n') ||
                                  (suffix == "g" && input[input.size() - 2] == 'n') ||
                                  (suffix == "r" && input[input.size() - 2] == 'e'))) continue;
                const bool completeJapanese = std::any_of(session->japanese.begin(), session->japanese.end(), [&](const auto &word) {
                    return word.reading.size() <= input.size() &&
                           std::string_view(input).substr(input.size() - word.reading.size()) == word.reading;
                });
                if (completeJapanese) break;
                const auto candidate = input.substr(0, input.size() - tail);
                const std::string result(mpn_mixed_pinyin(session, candidate.c_str(), simplified));
                if (!result.empty()) {
                    session->mixedPinyin = result;
                    if (consumed) *consumed = candidate.size();
                    return session->mixedPinyin.c_str();
                }
            }
        }
        // Only complete recorded Japanese words can end a prefix. Shortest
        // first gives the user a useful partial choice without eating a suffix.
        size_t attempts = 0;
        for (size_t end = 2; end < input.size(); ++end) {
            for (const auto &word : session->japanese) {
                if (word.reading.size() > end || input.compare(end - word.reading.size(), word.reading.size(), word.reading) != 0) continue;
                const size_t start = end - word.reading.size();
                std::string result;
                if (start == 0) {
                    if (word.weight == 3500) {
                        // An ambiguous all-Han alias may be selected as a
                        // leading segment only when the unselected suffix
                        // independently contains a known Japanese reading.
                        const auto suffix = std::string_view(input).substr(end);
                        const bool hasJapaneseEvidence = std::any_of(session->japanese.begin(), session->japanese.end(),
                            [&](const MPNSession::JapaneseWord &other) {
                                return other.weight >= 4000 && other.reading.size() >= 3 &&
                                    suffix.find(other.reading) != std::string_view::npos;
                            });
                        if (!hasJapaneseEvidence) continue;
                    }
                    if ((word.weight == 3500 || word.reading.size() == 2) &&
                        std::string_view(mpn_mixed_pinyin(session, input.c_str(), simplified)).empty()) continue;
                    result = word.text;
                } else {
                    if (++attempts > 8) return session->mixedPinyin.c_str();
                    const auto prefix = input.substr(0, end);
                    // Do not replace a complete Chinese phrase with an
                    // incidental Japanese homophone inside that phrase.
                    if (!decode(*session->lexicon, prefix, simplified != 0, true).empty()) continue;
                    result = mpn_mixed_pinyin(session, prefix.c_str(), simplified);
                }
                if (!result.empty()) {
                    session->mixedPinyin = std::move(result);
                    if (consumed) *consumed = end;
                    return session->mixedPinyin.c_str();
                }
            }
        }
    } catch (...) {}
    session->mixedPinyin.clear();
    return session->mixedPinyin.c_str();
}

const char *mpn_custom_chinese(MPNSession *session, const char *text) {
    if (!session) return "";
    session->customChinese.clear();
    if (!text) return session->customChinese.c_str();
    const std::string_view input(text);
    if (session->hans) session->customChinese = input;
    else session->customChinese = session->lexicon->to_traditional(input);
    return session->customChinese.c_str();
}

void mpn_set_simplified(MPNSession *session, int simplified) {
    if (session->hans == (simplified != 0)) return;
    session->hans = simplified != 0;
    try { session->refresh(); } catch (...) { session->reset(); }
}
void mpn_set_fuzzy_pinyin(MPNSession *session, uint32_t mask) {
    if (!session) return;
    mask &= MPN_FUZZY_ALL;
    if (session->fuzzy == mask) return;
    session->fuzzy = mask;
    if (session->input.empty()) return;
    try { session->refresh(); } catch (...) { session->reset(); }
}
uint32_t mpn_fuzzy_pinyin(const MPNSession *session) { return session ? session->fuzzy : 0; }
void mpn_set_typo_correction(MPNSession *session, int enabled) {
    if (!session || session->typo == (enabled != 0)) return;
    session->typo = enabled != 0;
    if (session->input.empty()) return;
    try { session->refresh(); } catch (...) { session->reset(); }
}
int mpn_typo_correction(const MPNSession *session) { return session && session->typo ? 1 : 0; }
void mpn_clear(MPNSession *session) {
    session->reset();
    session->committed.clear();
}
void mpn_cancel_composition(MPNSession *session) {
    if (session) session->reset();
}
void mpn_require_full_candidate(MPNSession *session) {
    if (!session) return;
    session->requireFullCandidate = true;
    try { session->refresh(); } catch (...) { session->reset(); }
}
void mpn_set_segment_context(MPNSession *session, const char *text) {
    if (!session) return;
    try {
        const std::string_view input = text ? std::string_view(text) : std::string_view();
        session->segmentContext = input.size() <= 64 ? session->lexicon->traditionalize(input, true) : std::string();
        if (!session->input.empty()) session->refresh();
    } catch (...) { session->segmentContext.clear(); }
}
const char *mpn_segment_context(const MPNSession *session) { return session ? session->segmentContext.c_str() : ""; }
void mpn_commit(MPNSession *session) {
    if (session->input.empty()) return;
    if (!session->candidates.empty()) { mpn_choose(session, session->page * page_size); return; }
    // Preserve what the user sees: Zhuyin's physical keys differ from its
    // displayed symbols, whereas Pinyin preedit is the original reading.
    session->committed += session->preedit;
    session->reset();
}
int mpn_choose(MPNSession *session, size_t index) {
    if (index >= session->candidates.size()) return 0;
    auto text = session->candidates[index].text;
    if (const auto tail = session->candidates[index].literalTail; tail && tail <= text.size())
        text.resize(text.size() - tail);
    auto remaining = session->input.substr(session->candidates[index].consumed);
    const auto traditional = session->lexicon->traditionalize(text, true);
    auto context = traditional.empty() ? std::string() : session->lexicon->traditionalize(session->segmentContext + traditional, true);
    session->reset();
    session->input = std::move(remaining);
    if (!session->input.empty()) session->segmentContext = std::move(context);
    session->refresh();
    session->committed += text;
    return 1;
}

size_t mpn_max_input_bytes(void) { return max_input; }

int mpn_key(MPNSession *session, int32_t key) {
    try {
        auto &input = session->input;
        if (key == 0xff55 || key == 0xff56) {
            if (input.empty()) return 0;
            if (key == 0xff55 && session->page > 0) --session->page;
            if (key == 0xff56 && (session->page + 1) * page_size < session->candidates.size()) ++session->page;
            return 1;
        }
        // Cancelling the active composition must not discard an earlier
        // selection that the host has not taken yet.
        if (key == 0xff1b && !input.empty()) { session->reset(); return 1; }
        if (key == 0xff08 && !input.empty()) {
            input.pop_back();
            if (input.empty()) session->segmentContext.clear();
            session->refresh();
            return 1;
        }
        if (key == 0xff0d && !input.empty()) { mpn_commit(session); return 1; }
        if (key < 32 || key > 126) return 0;
        const auto ch = static_cast<char>(key);
        if (session->lexicon->pinyin) {
            // At the bounded composition limit, preserve the visible reading
            // as entered instead of silently dropping the next key. A space
            // finishes the literal reading and remains an actual space.
            if (ch == ' ' && input.size() == max_input) {
                session->committed += session->preedit;
                session->committed += ' ';
                session->reset();
                return 1;
            }
            if (ch == ' ' && !input.empty()) { mpn_commit(session); return 1; }
            const bool boundary = is_tone(ch, true) || ch == '\'';
            if (!(ch >= 'a' && ch <= 'z') && !boundary) return 0;
            if (boundary && (input.empty() || is_tone(input.back(), true) || input.back() == '\'')) return 0;
            if (input.size() == max_input) {
                session->committed += session->preedit;
                // A tone/separator belongs to the reading before it; do not
                // leave an invalid orphaned boundary in the new composition.
                if (boundary) session->committed += ch;
                session->reset();
                if (boundary) return 1;
            }
            input += ch; session->refresh(); return 1;
        }
        const bool boundary = is_tone(ch) || ch == '\'';
        if (keys.find(ch) == keys.npos || (boundary && input.empty())) return 0;
        if (boundary && (is_tone(input.back()) || input.back() == '\'')) {
            if (ch == ' ') mpn_commit(session);
            return 1;
        }
        if (input.size() == max_input) {
            session->committed += session->preedit;
            // The tone symbol completes the previous syllable, rather than
            // becoming an orphaned new composition after the rollover.
            if (boundary) session->committed += symbols[keys.find(ch)];
            session->reset();
            if (boundary) return 1;
        }
        input += ch;
        session->refresh();
        return 1;
    } catch (...) {
        session->reset();
        return 0;
    }
}

int mpn_set_input(MPNSession *session, const char *input) {
    if (!session || !input) return 0;
    try {
        const std::string_view text(input);
        if (text.size() > max_input) return 0;
        if (text.empty()) {
            session->input.clear();
            session->preedit.clear();
            session->candidates.clear();
            session->sentenceCount = session->literalTail = 0;
            session->page = 0;
            session->expansion = 0;
            return 1;
        }
        const bool pinyin = session->lexicon->pinyin;
        for (size_t i = 0; i < text.size(); ++i) {
            const char ch = text[i];
            if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126 || ch == ' ') return 0;
            if (pinyin) {
                const bool boundary = is_tone(ch, true) || ch == '\'';
                if (!(ch >= 'a' && ch <= 'z') && !boundary) return 0;
                if (boundary && (i == 0 || is_tone(text[i - 1], true) || text[i - 1] == '\'')) return 0;
            } else {
                const bool boundary = is_tone(ch) || ch == '\'';
                if (keys.find(ch) == keys.npos) return 0;
                if (boundary && (i == 0 || is_tone(text[i - 1]) || text[i - 1] == '\'')) return 0;
            }
        }
        session->input.assign(text.data(), text.size());
        session->refresh();
        return 1;
    } catch (...) {
        session->reset();
        return 0;
    }
}

const char *mpn_input(const MPNSession *session) { return session->input.c_str(); }
const char *mpn_preedit(const MPNSession *session) { return session->preedit.c_str(); }
const char *mpn_committed(const MPNSession *session) { return session->committed.c_str(); }
void mpn_clear_commit(MPNSession *session) { session->committed.clear(); }
size_t mpn_candidate_count(const MPNSession *session) { return session->candidates.size(); }
int mpn_candidates_can_expand(const MPNSession *session) {
    return session && session->lexicon->pinyin && !session->input.empty() && session->expansion < 3;
}
size_t mpn_expand_candidates(MPNSession *session) {
    if (!mpn_candidates_can_expand(session)) return 0;
    try {
        const size_t breadth = size_t(32) << session->expansion;
        std::vector<CandidateMetadata> metadata;
        auto decoded = decode(*session->lexicon, session->input, session->hans, false, &metadata, nullptr, breadth, session->learned(), session->fuzzy, session->typo);
        std::set<std::pair<std::string, size_t>> seen;
        for (const auto &candidate : session->candidates) seen.emplace(candidate.text, candidate.consumed);
        std::vector<Candidate> added;
        if (session->expansion == 0) {
            // The first extra page offers leading words and characters before
            // any further whole-sentence guesses.
            for (auto &item : leading_words(*session->lexicon, session->input, session->hans))
                if (seen.emplace(item.text, item.consumed).second)
                    added.push_back({std::move(item.text), item.consumed, {MPN_CANDIDATE_PARTIAL, MPN_SCORE_LEXICON, item.score}});
        }
        for (size_t i = 0; i < decoded.size(); ++i) {
            if (seen.emplace(decoded[i], session->input.size()).second)
                added.push_back({std::move(decoded[i]), session->input.size(), metadata[i]});
        }
        const auto count = added.size();
        session->candidates.insert(session->candidates.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
        ++session->expansion;
        return count;
    } catch (...) { session->expansion = 3; return 0; }
}
const char *mpn_candidate(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() ? session->candidates[index].text.c_str() : "";
}
size_t mpn_candidate_consumed(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() ? session->candidates[index].consumed : 0;
}
size_t mpn_candidate_literal_tail(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() ? session->candidates[index].literalTail : 0;
}
int mpn_candidate_fuzzy(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() && session->candidates[index].metadata.fuzzy ? 1 : 0;
}
int mpn_candidate_typo(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() && session->candidates[index].metadata.typo ? 1 : 0;
}
size_t mpn_candidate_abbreviated(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() ? session->candidates[index].metadata.abbreviated : 0;
}
int mpn_candidate_info(const MPNSession *session, size_t index, MPNCandidateInfo *info, size_t info_size) {
    if (!session || !info || !info_size || index >= session->candidates.size()) return 0;
    const auto &candidate = session->candidates[index];
    const MPNCandidateInfo value{candidate.metadata.kind, candidate.metadata.scoreKind,
        static_cast<uint64_t>(candidate.consumed), static_cast<uint64_t>(candidate.consumed), candidate.metadata.score};
    std::memcpy(info, &value, std::min(info_size, sizeof(value)));
    return 1;
}
size_t mpn_page(const MPNSession *session) { return session->page; }
size_t mpn_sentence_count(const MPNSession *session) { return session ? session->sentenceCount : 0; }
size_t mpn_literal_tail(const MPNSession *session) { return session ? session->literalTail : 0; }
const char *mpn_exact_pinyin(MPNSession *session, const char *reading) {
    if (!session) return "";
    session->exactPinyin.clear();
    if (!session->lexicon->pinyin || !reading || std::string_view(reading).size() > max_input)
        return session->exactPinyin.c_str();
    try {
        auto words = decode(*session->lexicon, reading, session->hans, true);
        if (!words.empty()) session->exactPinyin = std::move(words.front());
    } catch (...) {}
    return session->exactPinyin.c_str();
}

MPNPersonal *mpn_personal_create(void) {
    try { return new MPNPersonal; } catch (...) { return nullptr; }
}
void mpn_personal_destroy(MPNPersonal *personal) { delete personal; }
int mpn_personal_set(MPNPersonal *personal, int simplified, const char *reading, const char *text, double bonus) {
    if (!personal || !reading || !*reading || std::strlen(reading) > max_input) return 0;
    const std::string_view key(reading);
    if (!std::all_of(key.begin(), key.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return 0;
    try {
        auto &table = personal->value->readings[simplified ? 1 : 0];
        auto group = table.find(key);
        if (!text) {
            if (group != table.end()) table.erase(group);
            return 1;
        }
        if (!*text || !(bonus == bonus)) return 0;
        bonus = std::min(personal_cap, bonus);
        if (bonus <= 0) {
            if (group == table.end()) return 1;
            if (const auto found = group->second.find(std::string_view(text)); found != group->second.end()) group->second.erase(found);
            if (group->second.empty()) table.erase(group);
            return 1;
        }
        if (group == table.end()) group = table.emplace(std::string(key), Personal::Texts{}).first;
        group->second[text] = bonus;
        return 1;
    } catch (...) { return 0; }
}
void mpn_personal_clear(MPNPersonal *personal) {
    if (personal) for (auto &table : personal->value->readings) table.clear();
}

namespace {
// DICT-02b: the untoned key of a user word's reading, or empty when the
// reading is not letters with optional tone digits and single apostrophes.
std::string user_reading_key(std::string_view reading) {
    if (reading.empty() || reading.size() > user_max_reading) return {};
    std::string key;
    char previous = '\'';
    for (char c : reading) {
        if (c >= 'a' && c <= 'z') key += c;
        else if (c >= '1' && c <= '5') { if (previous < 'a' || previous > 'z') return {}; }
        else if (c == '\'') { if (previous == '\'') return {}; }
        else return {};
        previous = c;
    }
    return key;
}
// Non-empty, bounded UTF-8 without control characters.
bool user_text(const char *text) {
    if (!text) return false;
    const std::string_view value(text);
    if (value.empty() || value.size() > user_max_text) return false;
    for (size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        if (first < 0x20 || first == 0x7f) return false;
        const size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!length || i + length > value.size()) return false;
        uint32_t scalar = length == 1 ? first : first & (0xff >> (length + 1));
        for (size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(value[i + k]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((length == 3 && scalar < 0x800) || (length == 4 && (scalar < 0x10000 || scalar > 0x10ffff)) ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || (scalar >= 0x80 && scalar < 0xa0)) return false;
        i += length;
    }
    return true;
}
} // namespace

int mpn_personal_add_word(MPNPersonal *personal, const char *reading, const char *simplified, const char *traditional) {
    if (!personal || !reading || !user_text(simplified) || (traditional && *traditional && !user_text(traditional))) return 0;
    auto key = user_reading_key(reading);
    if (key.empty()) return 0;
    try {
        auto &value = *personal->value;
        std::lock_guard<std::mutex> lock(value.userMutex);
        auto found = value.userWords.find({key, simplified});
        if (found == value.userWords.end()) {
            if (value.userWords.size() >= 100000) return 0;
            value.userWords.emplace(std::make_pair(std::move(key), std::string(simplified)),
                Personal::UserEntry{reading, traditional ? traditional : "", ++value.userOrder});
        } else {
            found->second.reading = reading;
            found->second.traditional = traditional ? traditional : "";
        }
        ++value.userRevision;
        return 1;
    } catch (...) { return 0; }
}
int mpn_personal_remove_word(MPNPersonal *personal, const char *reading, const char *simplified) {
    if (!personal || !reading || !simplified) return 0;
    const auto key = user_reading_key(reading);
    if (key.empty()) return 0;
    try {
        auto &value = *personal->value;
        std::lock_guard<std::mutex> lock(value.userMutex);
        const auto found = value.userWords.find({key, simplified});
        if (found == value.userWords.end()) return 0;
        value.userWords.erase(found);
        ++value.userRevision;
        return 1;
    } catch (...) { return 0; }
}
void mpn_personal_clear_words(MPNPersonal *personal) {
    if (!personal) return;
    auto &value = *personal->value;
    std::lock_guard<std::mutex> lock(value.userMutex);
    if (value.userWords.empty()) return;
    value.userWords.clear();
    ++value.userRevision;
}
size_t mpn_personal_word_count(const MPNPersonal *personal) {
    if (!personal) return 0;
    std::lock_guard<std::mutex> lock(personal->value->userMutex);
    return personal->value->userWords.size();
}
int mpn_candidate_personal(const MPNSession *session, size_t index) {
    return session && index < session->candidates.size() && session->candidates[index].metadata.user ? 1 : 0;
}
void mpn_set_personal(MPNSession *session, const MPNPersonal *personal) {
    if (session) session->personal = personal ? personal->value : nullptr;
}
const char *mpn_candidate_words(MPNSession *session, size_t index) {
    if (!session) return "";
    session->candidateWords.clear();
    if (index >= session->candidates.size()) return session->candidateWords.c_str();
    try {
        const auto &candidate = session->candidates[index];
        if (candidate.consumed > session->input.size() || candidate.literalTail > candidate.text.size())
            return session->candidateWords.c_str();
        const auto text = std::string_view(candidate.text).substr(0, candidate.text.size() - candidate.literalTail);
        for (const auto &[reading, piece] : composition(*session->lexicon, std::string_view(session->input).substr(0, candidate.consumed), text, session->hans))
            session->candidateWords += reading + '\t' + piece + '\n';
    } catch (...) { session->candidateWords.clear(); }
    return session->candidateWords.c_str();
}

const char *mpn_decompose_words(MPNSession *session, const char *reading, const char *text) {
    if (!session) return "";
    session->candidateWords.clear();
    if (!reading || !*reading || !text || !*text) return session->candidateWords.c_str();
    try {
        for (const auto &[wordReading, piece] : composition(*session->lexicon, reading, text, session->hans))
            session->candidateWords += wordReading + '\t' + piece + '\n';
    } catch (...) { session->candidateWords.clear(); }
    return session->candidateWords.c_str();
}

const char *mpn_advance_context(MPNSession *session, const char *context, const char *text) {
    if (!session) return "";
    session->candidateWords.clear();
    if (!text || !*text) return session->candidateWords.c_str();
    try {
        const auto traditional = session->lexicon->traditionalize(text, true);
        if (traditional.empty()) return session->candidateWords.c_str();
        const std::string_view previous = context ? std::string_view(context) : std::string_view();
        if (previous.size() > 64) return session->candidateWords.c_str();
        session->candidateWords = session->lexicon->traditionalize(std::string(previous) + traditional, true);
    } catch (...) { session->candidateWords.clear(); }
    return session->candidateWords.c_str();
}

const char *mpn_candidate_explain(MPNSession *session, size_t index) {
    if (!session) return "";
    session->explanation.clear();
    if (index >= session->candidates.size()) return session->explanation.c_str();
    try {
        const auto &candidate = session->candidates[index];
        // Fuzzy, typo and abbreviated readings are not scored word by word over the typed keys.
        if (candidate.consumed > session->input.size() || candidate.literalTail > candidate.text.size() ||
            candidate.metadata.fuzzy || candidate.metadata.typo || candidate.metadata.abbreviated)
            return session->explanation.c_str();
        const auto text = std::string_view(candidate.text).substr(0, candidate.text.size() - candidate.literalTail);
        session->explanation = explain_composition(*session->lexicon, session->learned(),
            std::string_view(session->input).substr(0, candidate.consumed), text, session->hans);
    } catch (...) { session->explanation.clear(); }
    return session->explanation.c_str();
}

const char *mpn_explain_text(MPNSession *session, const char *text) {
    if (!session) return "";
    session->explanation.clear();
    if (!text || !*text || std::strlen(text) > 512) return session->explanation.c_str();
    try {
        const std::string_view wanted(text);
        const auto &lexicon = *session->lexicon;
        size_t lines = 0;
        char number[96];
        for (const auto &word : lexicon.words) {
            if (lines == 512) break;
            if (word.text(true) != wanted && word.text(false) != wanted) continue;
            std::snprintf(number, sizeof(number), "\t%d\t%d\t%.2f\t%d\n", word.weight, word.hansWeight, word.prior / 100.0, word.pair ? 1 : 0);
            session->explanation += "lexicon\t" + std::string(word.reading()) + '\t' + std::string(word.text(false)) + '\t' +
                std::string(word.text(true)) + number;
            ++lines;
        }
        if (const auto user = session->personal ? session->personal->user_words(lexicon) : nullptr) {
            for (const auto &word : user->words) {
                if (lines == 512) break;
                if (word.text(true) != wanted && word.text(false) != wanted) continue;
                session->explanation += "user\t" + std::string(word.reading()) + '\t' + std::string(word.text(false)) + '\t' +
                    std::string(word.text(true)) + '\n';
                ++lines;
            }
        }
        std::vector<std::string> phrases{std::string(wanted)};
        if (const auto traditional = lexicon.to_traditional(wanted); traditional != wanted) phrases.push_back(traditional);
        for (const auto &phrase : phrases) {
            const auto entry = std::lower_bound(lexicon.context.begin(), lexicon.context.end(), std::string_view(phrase),
                [](const Lexicon::ContextScore &item, std::string_view value) { return item.first < value; });
            if (lines == 512 || entry == lexicon.context.end() || entry->first != phrase) continue;
            const bool authored = !lexicon.contextAuthored.empty() && lexicon.contextAuthored[entry - lexicon.context.begin()];
            std::snprintf(number, sizeof(number), "\t%d\t%d\t%d\n", entry->weight, entry->hansWeight, authored ? 1 : 0);
            session->explanation += "context\t" + phrase + number;
            ++lines;
        }
    } catch (...) { session->explanation.clear(); }
    return session->explanation.c_str();
}
