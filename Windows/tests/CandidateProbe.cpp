#define NOMINMAX
#include "../ConjugationPanel.h"
#include "../WordEvidence.h"
#include "../JapanesePackages.h"
#include <psapi.h>
#include <chrono>
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
