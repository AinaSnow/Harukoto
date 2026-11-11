#define NOMINMAX
#include "../ConjugationPanel.h"
#include "../WordEvidence.h"
#include "../JapanesePackages.h"
#include <psapi.h>
#include <chrono>
#include <functional>
#include <map>
#include <iomanip>
#include <iostream>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static std::string json(const std::string &value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << int(c);
        else out << c;
    }
    out << '"'; return out.str();
}
static std::string memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        throw std::runtime_error("Cannot query process memory");
    return "\"private_bytes\":" + std::to_string(counters.PrivateUsage) +
        ",\"working_set_bytes\":" + std::to_string(counters.WorkingSetSize);
}

// Standalone measurement process: no regression assertions or user dictionaries
// are loaded implicitly. Each key uses the production candidate merge path.
int main(int argc, char **argv) {
    try {
        if (argc != 3 && argc != 4) throw std::runtime_error("Usage: candidate_probe LEXICON hans|hant [SNAPSHOT_ROOT]");
        const bool simplified = std::string(argv[2]) == "hans";
        if (!simplified && std::string(argv[2]) != "hant") throw std::runtime_error("Invalid script");
        const auto started = Clock::now();
        const auto dir = std::filesystem::u8path(argv[1]);
        JapaneseConjugations table;
        if (!table.loadFile(dir / "japanese-conjugations.bin")) throw std::runtime_error("Missing conjugations");
        std::ifstream dictionary(dir / "japanese.tsv");
        if (!dictionary) throw std::runtime_error("Missing Japanese dictionary");
        auto words = readJapaneseWords(dictionary); appendConjugationWords(table, words);
        size_t themeRows = 0;
        if (argc == 4) {
            auto themes = readEnabledJapanesePackages(std::filesystem::u8path(argv[3]));
            themeRows = themes.size();
            if (!themeRows) throw std::runtime_error("Requested theme snapshot is empty or invalid");
            words.insert(words.end(), themes.begin(), themes.end());
        }
        std::stable_sort(words.begin(), words.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        WordEvidence evidence; evidence.open(dir / "language-evidence.sqlite");
        if (evidence.dictionary("chi", simplified).empty()) throw std::runtime_error("Chinese evidence unavailable");
        MPNLexicon *lexicon;
        {
            std::ifstream file(dir / "native-pinyin.tsv", std::ios::binary);
            if (!file) throw std::runtime_error("Missing native dictionary");
            const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            lexicon = mpn_lexicon_create(bytes.data(), bytes.size());
        }
        if (!lexicon) throw std::runtime_error("Invalid native dictionary");
        auto *session = mpn_session_create(lexicon);
        if (!session) throw std::runtime_error("Cannot create session");
        mpn_set_simplified(session, simplified);
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "{\"type\":\"startup\",\"load_ms\":" << ms(started) << ",\"theme_rows\":" << themeRows << ',' << memory() << "}\n";
        for (std::string input; std::getline(std::cin, input);) {
            if (!input.empty() && input.back() == '\r') input.pop_back();
            if (const auto tab = input.find('\t'); tab != std::string::npos) {
                // "reading<TAB>expected": can the text be produced by selection alone?
                const std::string reading = input.substr(0, tab), expected = input.substr(tab + 1);
                if (reading.empty() || expected.empty() || reading.size() > mpn_max_input_bytes() ||
                    !std::all_of(reading.begin(), reading.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
                    throw std::runtime_error("Compose line must be a-z<TAB>text");
                // Cheapest way to produce the text by selection alone. One unit
                // per selection and per PageDown needed to reach the choice.
                struct Best { size_t cost = SIZE_MAX; std::string text; size_t rank = 0, count = 0, next = 0, nextDone = 0; };
                std::map<std::pair<size_t, size_t>, Best> memo; size_t states = 0;
                const auto lookup = [&](const std::string &value) { return evidence.dictionary(value, simplified); };
                // Each state is rebuilt from the remaining keys, with the text
                // chosen so far as ranking context, then expanded to its end
                // exactly as repeated PageDown does in the text service.
                std::function<size_t(size_t, size_t)> compose = [&](size_t offset, size_t done) -> size_t {
                    if (offset == reading.size()) return done == expected.size() ? 0 : SIZE_MAX;
                    if (done == expected.size()) return SIZE_MAX;
                    if (const auto found = memo.find({offset, done}); found != memo.end()) return found->second.cost;
                    if (++states > 512) throw std::runtime_error("Compose search exceeded its bound");
                    mpn_clear(session); mpn_clear_commit(session);
                    mpn_set_segment_context(session, expected.substr(0, done).c_str());
                    for (size_t i = offset; i < reading.size(); ++i)
                        if (!mpn_key(session, reading[i])) throw std::runtime_error("Key rejected");
                    auto choices = windowsCandidates(session, words, lookup, false);
                    while (mpn_candidates_can_expand(session)) {
                        mpn_expand_candidates(session);
                        appendCandidateExpansion(session, choices, windowsCandidates(session, words, lookup, false));
                    }
                    struct Option { std::string text; size_t rank, consumed; };
                    std::vector<Option> options;
                    std::set<std::pair<size_t, size_t>> targets; // Only the earliest rank of each outcome matters.
                    for (size_t i = 0; i < choices.size(); ++i) {
                        const auto &c = choices[i];
                        const size_t consumed = c.coreIndex ? mpn_candidate_consumed(session, *c.coreIndex) : reading.size() - offset;
                        if (!c.japanese && consumed && expected.compare(done, c.text.size(), c.text) == 0 &&
                            targets.emplace(consumed, c.text.size()).second)
                            options.push_back({c.text, i + 1, consumed});
                    }
                    const size_t count = choices.size();
                    Best best;
                    for (const auto &option : options) {
                        const auto rest = compose(offset + option.consumed, done + option.text.size());
                        if (rest == SIZE_MAX) continue;
                        const size_t cost = rest + 1 + (option.rank - 1) / 7;
                        if (cost < best.cost) best = {cost, option.text, option.rank, count, offset + option.consumed, done + option.text.size()};
                    }
                    memo[{offset, done}] = best;
                    return best.cost;
                };
                const bool ok = compose(0, 0) != SIZE_MAX;
                std::cout << "{\"type\":\"compose\",\"input\":" << json(reading) << ",\"expected\":" << json(expected) <<
                    ",\"ok\":" << (ok ? "true" : "false") << ",\"cost\":" << (ok ? memo[{0, 0}].cost : 0) << ",\"steps\":[";
                for (std::pair<size_t, size_t> state{0, 0}; ok && state.first < reading.size();) {
                    const auto &step = memo[state];
                    std::cout << (state.first ? "," : "") << "{\"text\":" << json(step.text) << ",\"rank\":" << step.rank <<
                        ",\"candidates\":" << step.count << '}';
                    state = {step.next, step.nextDone};
                }
                std::cout << "]}\n";
                continue;
            }
            if (input.empty() || input.size() > mpn_max_input_bytes() ||
                !std::all_of(input.begin(), input.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
                throw std::runtime_error("Input must be a-z within composition limit");
            mpn_clear(session); mpn_clear_commit(session); mpn_set_segment_context(session, "");
            std::vector<WindowsCandidate> choices; std::vector<double> times;
            for (char c : input) {
                const auto before = Clock::now();
                if (!mpn_key(session, c)) throw std::runtime_error("Key rejected");
                choices = windowsCandidates(session, words, [&](const std::string &reading) { return evidence.dictionary(reading, simplified); }, false);
                times.push_back(ms(before));
            }
            std::cout << "{\"type\":\"sample\",\"input\":" << json(input) << ",\"key_ms\":[";
            for (size_t i = 0; i < times.size(); ++i) std::cout << (i ? "," : "") << times[i];
            std::cout << "],\"candidates\":[";
            for (size_t i = 0; i < choices.size(); ++i) {
                const auto &c = choices[i]; MPNCandidateInfo info{};
                if (c.coreIndex) mpn_candidate_info(session, *c.coreIndex, &info, sizeof(info));
                std::cout << (i ? "," : "") << "{\"text\":" << json(c.text) <<
                    ",\"kind\":" << info.kind << ",\"consumed\":" << (c.coreIndex ? info.consumed_bytes : input.size()) << '}';
            }
            std::cout << "]," << memory() << "}\n";
        }
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
