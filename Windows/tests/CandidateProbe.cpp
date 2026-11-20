#define NOMINMAX
#include "../ConjugationPanel.h"
#include "../WordEvidence.h"
#include "../JapanesePackages.h"
#include <psapi.h>
#include <chrono>
#include <cmath>
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
        // Optional --personal=N attaches N learned words (CN-06c) for timing
        // only; evaluation runs never pass it.
        size_t personalWords = 0;
        // Optional --user-words=N adds N hand-added words (DICT-02b) for
        // timing only; evaluation runs never pass it either.
        size_t userWords = 0;
        // Optional --fuzzy=MASK enables CN-03 fuzzy pinyin pairs (MPN_FUZZY_*).
        uint32_t fuzzy = 0;
        // Optional --typo=0 turns CN-05 typo correction off (on by default);
        // --scores adds each core candidate's generation score and typo flag.
        bool typo = true, scores = false;
        std::vector<char *> positional{argv[0]};
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument.substr(0, 11) == "--personal=") personalWords = std::stoul(std::string(argument.substr(11)));
            else if (argument.substr(0, 13) == "--user-words=") userWords = std::stoul(std::string(argument.substr(13)));
            else if (argument.substr(0, 8) == "--fuzzy=") fuzzy = static_cast<uint32_t>(std::stoul(std::string(argument.substr(8)), nullptr, 0));
            else if (argument.substr(0, 7) == "--typo=") typo = argument.substr(7) != "0";
            else if (argument == "--scores") scores = true;
            else positional.push_back(argv[i]);
        }
        argc = static_cast<int>(positional.size()); argv = positional.data();
        if (argc != 3 && argc != 4) throw std::runtime_error("Usage: candidate_probe LEXICON hans|hant [SNAPSHOT_ROOT] [--personal=N] [--user-words=N] [--fuzzy=MASK] [--typo=0|1] [--scores]");
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
        MPNPersonal *personal = nullptr;
        double personalMs = 0, userMs = 0, userPrepareMs = 0;
        {
            std::ifstream file(dir / "native-pinyin.tsv", std::ios::binary);
            if (!file) throw std::runtime_error("Missing native dictionary");
            // Sized read, as the text service does; a per-character copy dominates loading.
            std::string bytes;
            file.seekg(0, std::ios::end);
            if (const auto size = file.tellg(); size > 0) {
                bytes.resize(static_cast<size_t>(size));
                file.seekg(0);
                if (!file.read(bytes.data(), size)) bytes.clear();
            }
            lexicon = mpn_lexicon_create(bytes.data(), bytes.size());
            if (userWords) {
                // Words the lexicon lacks, on the readings of multi-syllable
                // lexicon words spread over the whole table (the lexicon's
                // text with one character added), so they compete with real
                // input; toned readings as the management script may write.
                struct Row { std::string reading, text; };
                std::vector<Row> rows;
                std::istringstream lines(bytes);
                std::string line;
                std::getline(lines, line);
                while (std::getline(lines, line) && line != "# context") {
                    std::vector<std::string> fields;
                    std::istringstream row(line);
                    for (std::string field; std::getline(row, field, '\t');) fields.push_back(field);
                    // Fully spelled readings only: every syllable has a vowel (no n3h3 rows).
                    bool vowel = false, spelled = true;
                    for (char c : fields.size() == 6 ? fields[1] : std::string()) {
                        if (c >= '1' && c <= '5') { spelled = spelled && vowel; vowel = false; }
                        else vowel = vowel || std::string_view("aeiouv").find(c) != std::string_view::npos;
                    }
                    if (fields.size() == 6 && spelled && fields[3].size() > 3 && std::count_if(fields[1].begin(), fields[1].end(), [](char c) { return c >= '1' && c <= '5'; }) >= 2)
                        rows.push_back({fields[1], fields[3] + "\xe7\x94\xb2"}); // + 甲
                }
                if (!personal) personal = mpn_personal_create();
                const auto built = Clock::now();
                const size_t step = (std::max)(size_t(1), rows.size() / userWords);
                for (size_t i = 0, added = 0; i < rows.size() && added < userWords; i += step, ++added)
                    if (!mpn_personal_add_word(personal, rows[i].reading.c_str(), rows[i].text.c_str(), nullptr)) throw std::runtime_error("User word rejected");
                userMs = ms(built);
            }
            if (personalWords) {
                // Spread over common multi-character words, with one to five
                // selections' worth of bonus each, as the host hands them over.
                struct Row { std::string reading, text; };
                std::vector<Row> rows;
                std::istringstream lines(bytes);
                std::string line;
                std::getline(lines, line);
                while (std::getline(lines, line) && line != "# context") {
                    std::vector<std::string> fields;
                    std::istringstream row(line);
                    for (std::string field; std::getline(row, field, '\t');) fields.push_back(field);
                    if (fields.size() != 6 || std::stoi(fields[simplified ? 5 : 4]) < 3000) continue;
                    const auto &text = fields[simplified ? 3 : 2];
                    if (text.size() > 3) rows.push_back({fields[0], text});
                }
                if (!personal) personal = mpn_personal_create();
                const auto built = Clock::now();
                const size_t step = (std::max)(size_t(1), rows.size() / personalWords);
                for (size_t i = 0, added = 0; i < rows.size() && added < personalWords; i += step, ++added)
                    mpn_personal_set(personal, simplified, rows[i].reading.c_str(), rows[i].text.c_str(), std::log1p(double(added % 5 + 1)));
                personalMs = ms(built);
            }
        }
        if (!lexicon) throw std::runtime_error("Invalid native dictionary");
        auto *session = mpn_session_create(lexicon);
        if (!session) throw std::runtime_error("Cannot create session");
        mpn_set_simplified(session, simplified);
        if (personal) mpn_set_personal(session, personal);
        if (userWords) {
            // The table is prepared for the lexicon at the first lookup.
            const auto prepared = Clock::now();
            mpn_key(session, 'a');
            userPrepareMs = ms(prepared);
            mpn_clear(session);
        }
        mpn_set_fuzzy_pinyin(session, fuzzy);
        mpn_set_typo_correction(session, typo);
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "{\"type\":\"startup\",\"load_ms\":" << ms(started) << ",\"theme_rows\":" << themeRows << ',';
        if (personalWords) std::cout << "\"personal_words\":" << personalWords << ",\"personal_build_ms\":" << personalMs << ',';
        if (userWords) std::cout << "\"user_words\":" << mpn_personal_word_count(personal) << ",\"user_add_ms\":" << userMs <<
            ",\"user_first_key_ms\":" << userPrepareMs << ',';
        if (fuzzy) std::cout << "\"fuzzy\":" << mpn_fuzzy_pinyin(session) << ',';
        if (!typo) std::cout << "\"typo\":0,";
        std::cout << memory() << "}\n";
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
                    ",\"kind\":" << info.kind << ",\"consumed\":" << (c.coreIndex ? info.consumed_bytes : input.size());
                if (scores && c.coreIndex) std::cout << ",\"score\":" << info.score << ",\"typo\":" << mpn_candidate_typo(session, *c.coreIndex);
                std::cout << '}';
            }
            std::cout << "]," << memory() << "}\n";
        }
        mpn_session_destroy(session); mpn_personal_destroy(personal); mpn_lexicon_destroy(lexicon);
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
