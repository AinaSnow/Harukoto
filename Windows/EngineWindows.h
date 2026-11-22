#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EngineServer.h"
#include "BaseDictionary.h"
#include "PersonalDictionary.h"
#include "EngineIdentity.h"
#include "EngineMemory.h"
#include <chrono>

namespace harukoto::engine {
// Preparation validates one complete immutable base. Queries share its lexicon
// and copied merged words, but never its mutable SQLite connection/statements.
class WindowsDictionary {
    std::unique_ptr<base_dictionary::Snapshot> base_;
    std::vector<JapaneseWord> words_;
    std::unordered_map<std::string, std::vector<size_t>> readings_;
    std::string generation_, identity_;
    std::shared_ptr<MemoryMirror> memory_; // Step 6b: read-only mirror of the user's learning.
    WindowsDictionary() = default;
public:
    struct QueryTimings {
        double setup_ms = 0, replay_ms = 0, sqlite_open_ms = 0, merge_ms = 0;
        double expansion_ms = 0, conversion_ms = 0, cleanup_ms = 0, total_ms = 0;
    };
    static constexpr size_t PageSize = 16;
    const std::string &generation() const { return generation_; }
    // WIN-03 step 6a: what a TSF client compares with its own data before
    // taking these candidates (EngineIdentity.h).
    const std::string &identity() const { return identity_; }
    const base_dictionary::Snapshot &base() const { return *base_; }
    const std::vector<JapaneseWord> &words() const { return words_; }
    // memory: the user's selection-memory.tsv (its -words and -user siblings
    // beside it), read but never written; empty: requests that need learned
    // state are answered "behind".
    static std::shared_ptr<const WindowsDictionary> load(const std::filesystem::path &folder,
            const std::filesystem::path &themes = {}, const std::filesystem::path &personal = {},
            const std::filesystem::path &memory = {}) {
        auto result = std::shared_ptr<WindowsDictionary>(new WindowsDictionary);
        if (!memory.empty()) result->memory_ = std::make_shared<MemoryMirror>(memory);
        result->base_ = base_dictionary::Snapshot::load(folder);
        result->words_ = result->base_->words();
        std::string themeGeneration, personalHash;
        if (!personal.empty()) {
            auto source = readPersonalDictionary(personal, true, {});
            if (!source) throw std::runtime_error("invalid personal dictionary");
            personalHash = personalIdentity(source->present, source->bytes);
            result->words_.insert(result->words_.end(), source->words.begin(), source->words.end());
        }
        if (!themes.empty()) {
            auto source = readJapanesePackageSnapshot(themes);
            if (!source) throw std::runtime_error("invalid theme snapshot");
            themeGeneration = source->generation;
            result->words_.insert(result->words_.end(), source->words.begin(), source->words.end());
        }
        std::stable_sort(result->words_.begin(), result->words_.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        for (size_t i = 0; i < result->words_.size(); ++i)
            result->readings_[result->words_[i].reading].push_back(i);
        base_dictionary::Sha256 digest;
        auto field = [&](const std::string &value) {
            const auto length = std::to_string(value.size()) + ":";
            digest.add(length.data(), static_cast<ULONG>(length.size()));
            digest.add(value.data(), static_cast<ULONG>(value.size()));
        };
        field(result->base_->generation()); field(themeGeneration); field(personalHash);
        // Include actual frozen rows, even if a producer incorrectly reuses a
        // theme pointer name after changing its contents.
        for (const auto &word : result->words_) {
            field(word.reading); field(word.text); field(std::to_string(word.weight));
            field(word.kana); field(word.source); field(word.chinese); field(word.theme ? "1" : "0");
        }
        result->generation_ = digest.finish();
        result->identity_ = dataIdentity(result->base_->generation(), themeGeneration, personalHash);
        return result;
    }
    // Profiling is opt-in; normal queries do not read a clock for these phases.
    CandidatePage query(const ServerConnection::Ticket &ticket, QueryTimings *timings = nullptr) const {
        if (!timings) return queryImpl(ticket, nullptr);
        *timings = {};
        const auto started = std::chrono::steady_clock::now();
        auto result = queryImpl(ticket, timings);
        timings->total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        timings->cleanup_ms = std::max(0.0, timings->total_ms - timings->setup_ms - timings->replay_ms -
            timings->sqlite_open_ms - timings->merge_ms - timings->expansion_ms - timings->conversion_ms);
        return result;
    }
private:
    // windowsCandidates uses whole-input matches; mixedReading additionally
    // uses source-empty substrings of length 3..10 for all-letter inputs 5..24.
    // Original row indices preserve every tie/order/first-match decision.
    std::vector<JapaneseWord> queryWords(const ServerConnection::Work &work, bool locked) const {
        if (locked) return {};
        std::vector<size_t> indices;
        auto include = [&](const std::string &reading, bool mixedOnly) {
            const auto found = readings_.find(reading);
            if (found == readings_.end()) return;
            for (auto index : found->second)
                if (!mixedOnly || words_[index].source.empty()) indices.push_back(index);
        };
        include(work.input, false);
        if (work.input.size() >= 5 && work.input.size() <= 24 &&
            std::all_of(work.input.begin(), work.input.end(), [](char c) { return c >= 'a' && c <= 'z'; })) {
            for (size_t start = 0; start + 3 <= work.input.size(); ++start)
                for (size_t length = 3; length <= 10 && start + length <= work.input.size(); ++length)
                    include(work.input.substr(start, length), true);
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        std::vector<JapaneseWord> result;
        result.reserve(indices.size());
        for (auto index : indices) result.push_back(words_[index]);
        return result;
    }
    CandidatePage queryImpl(const ServerConnection::Ticket &ticket, QueryTimings *timings) const {
        auto previous = timings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto mark = [&](double QueryTimings::*field) {
            if (!timings) return;
            const auto current = std::chrono::steady_clock::now();
            timings->*field += std::chrono::duration<double, std::milli>(current - previous).count();
            previous = current;
        };
        if (!ticket || ticket->dictionary.get() != this) throw std::invalid_argument("query snapshot mismatch");
        const auto &work = *ticket;
        if (!inputValid(work.input) || work.input.size() > mpn_max_input_bytes() || work.page > 4095 ||
            work.context.size() > 64 || !textValid(work.context) || !valid(work.key))
            throw std::invalid_argument("unsupported Windows query");
        auto session = base_->session(work.key.simplified);
        // Step 6c: a Query carries the client's own segment context and
        // partial-selection state; a plain key uses the server session's.
        const bool locked = work.key.extended ? work.key.settings.locked : work.chineseLocked;
        mpn_set_segment_context(session.get(), (work.key.extended ? work.key.settings.context : work.context).c_str());
        const size_t pageSize = work.key.extended ? QueryPageSize : PageSize;
        // Step 6b: the client's fuzzy pinyin, typo switch and learned state,
        // set before any key exactly as its own session has them.
        std::shared_ptr<const MemoryView> view;
        PersonalTable personal;
        const auto &settings = work.key.settings;
        if (work.key.extended) {
            mpn_set_fuzzy_pinyin(session.get(), settings.fuzzy);
            mpn_set_typo_correction(session.get(), settings.typo ? 1 : 0);
            static const std::string noUserWords = memoryDigest(selection_memory::UserWordModel{});
            if (memory_) view = memory_->match(settings, GetTickCount64());
            if (!view && (settings.memory || settings.user != noUserWords || memory_)) {
                CandidatePage behind{work.input, false, {}, true};
                encodePayload(behind);
                return behind;
            }
            if (view) personal = view->personal(settings.memory, settings.hour);
            if (personal) mpn_set_personal(session.get(), personal.get());
        }
        mark(&QueryTimings::setup_ms);
        for (unsigned char c : work.input)
            if (!mpn_key(session.get(), c) || *mpn_committed(session.get()))
                throw std::invalid_argument("core rejected input or produced implicit commit");
        if (work.input != mpn_input(session.get())) throw std::runtime_error("core input mismatch");
        mark(&QueryTimings::replay_ms);
        WordEvidence evidence; // Construct, use, close entirely on the query thread.
        evidence.open(base_->databasePath());
        if (!evidence.ready()) throw std::runtime_error("snapshot SQLite unavailable");
        mark(&QueryTimings::sqlite_open_ms);
        auto lookup = [&](const std::string &reading) { return evidence.dictionary(reading, work.key.simplified); };
        const auto relevant = queryWords(work, locked);
        auto choices = windowsCandidates(session.get(), relevant, lookup, locked);
        if (view && settings.memory) {
            // What the user chose for this exact reading comes first (CN-06a/b),
            // as the TSF service does before any expansion.
            selection_memory::apply(choices, work.input, view->phrases.recall(work.input, work.key.simplified), [&](const WindowsCandidate &c) {
                return c.coreIndex ? mpn_candidate_consumed(session.get(), *c.coreIndex) : work.input.size();
            });
        }
        std::vector<uint8_t> rounds(choices.size(), 0); // Expansion pass of each choice (step 6a).
        mark(&QueryTimings::merge_ms);
        std::vector<Candidate> converted;
        size_t convertedChoices = 0;
        auto convert = [&] {
            for (; convertedChoices<choices.size(); ++convertedChoices) {
                const auto &choice = choices[convertedChoices];
                auto text = choice.text;
                size_t consumed = work.input.size();
                if (choice.coreIndex) {
                    const auto tail = mpn_candidate_literal_tail(session.get(), *choice.coreIndex);
                    if (tail > text.size()) throw std::runtime_error("invalid candidate tail");
                    text.resize(text.size() - tail);
                    consumed = mpn_candidate_consumed(session.get(), *choice.coreIndex);
                }
                if (text.empty() || !consumed) continue;
                const uint8_t flags = static_cast<uint8_t>((choice.japanese ? candidate_flag::Japanese : 0) |
                    (choice.strong ? candidate_flag::Strong : 0) | (choice.directJapanese ? candidate_flag::Direct : 0) |
                    (rounds[convertedChoices] << candidate_flag::RoundShift));
                converted.push_back({convertedChoices + 1, static_cast<uint32_t>(consumed), std::move(text), choice.annotation,
                    choice.coreIndex ? static_cast<uint32_t>(*choice.coreIndex + 1) : 0u, flags});
            }
        };
        const auto start = size_t(work.page) * pageSize;
        // Expansion only appends, and native indices remain stable. Once we
        // have this page plus one valid choice, neither its contents/IDs nor
        // more=true can change. Later pages rebuild the same prefix and expand
        // further as needed, retaining the original three-round search budget.
        const auto required = start + pageSize + (work.page < 4095 ? 1 : 0);
        convert(); mark(&QueryTimings::conversion_ms);
        for (int i=0; converted.size()<required && i<3 && mpn_candidates_can_expand(session.get()); ++i) {
            mpn_expand_candidates(session.get());
            appendCandidateExpansion(session.get(), choices, windowsCandidates(session.get(), relevant, lookup, locked));
            rounds.resize(choices.size(), static_cast<uint8_t>(i + 1));
            mark(&QueryTimings::expansion_ms);
            convert(); mark(&QueryTimings::conversion_ms);
        }
        CandidatePage page{work.input, false, {}};
        if (start < converted.size()) {
            const auto end = std::min(converted.size(), start + pageSize);
            page.words.assign(converted.begin() + start, converted.begin() + end);
            page.more = end < converted.size() && work.page < 4095;
        }
        encodePayload(page); // Reject bad text/ranges and budget violations atomically.
        mark(&QueryTimings::conversion_ms);
        return page;
    }
};
} // namespace harukoto::engine
