#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EngineServer.h"
#include "BaseDictionary.h"
#include "PersonalDictionary.h"
#include <chrono>

namespace harukoto::engine {
// Preparation validates one complete immutable base. Queries share its lexicon
// and copied merged words, but never its mutable SQLite connection/statements.
class WindowsDictionary {
    std::unique_ptr<base_dictionary::Snapshot> base_;
    std::vector<JapaneseWord> words_;
    std::string generation_;
    WindowsDictionary() = default;
public:
    struct QueryTimings {
        double setup_ms = 0, replay_ms = 0, sqlite_open_ms = 0, merge_ms = 0;
        double expansion_ms = 0, conversion_ms = 0, cleanup_ms = 0, total_ms = 0;
    };
    static constexpr size_t PageSize = 16;
    const std::string &generation() const { return generation_; }
    const base_dictionary::Snapshot &base() const { return *base_; }
    const std::vector<JapaneseWord> &words() const { return words_; }
    static std::shared_ptr<const WindowsDictionary> load(const std::filesystem::path &folder,
            const std::filesystem::path &themes = {}, const std::filesystem::path &personal = {}) {
        auto result = std::shared_ptr<WindowsDictionary>(new WindowsDictionary);
        result->base_ = base_dictionary::Snapshot::load(folder);
        result->words_ = result->base_->words();
        std::string themeGeneration, personalHash;
        if (!personal.empty()) {
            auto source = readPersonalDictionary(personal, true, {});
            if (!source) throw std::runtime_error("invalid personal dictionary");
            personalHash = source->present ? base_dictionary::sha256(source->bytes) : "absent";
            result->words_.insert(result->words_.end(), source->words.begin(), source->words.end());
        }
        if (!themes.empty()) {
            auto source = readJapanesePackageSnapshot(themes);
            if (!source) throw std::runtime_error("invalid theme snapshot");
            themeGeneration = source->generation;
            result->words_.insert(result->words_.end(), source->words.begin(), source->words.end());
        }
        std::stable_sort(result->words_.begin(), result->words_.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
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
    CandidatePage queryImpl(const ServerConnection::Ticket &ticket, QueryTimings *timings) const {
        auto previous = timings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto mark = [&](double QueryTimings::*field) {
            if (!timings) return;
            const auto current = std::chrono::steady_clock::now();
            timings->*field = std::chrono::duration<double, std::milli>(current - previous).count();
            previous = current;
        };
        if (!ticket || ticket->dictionary.get() != this) throw std::invalid_argument("query snapshot mismatch");
        const auto &work = *ticket;
        if (!inputValid(work.input) || work.input.size() > mpn_max_input_bytes() || work.page > 4095 ||
            work.context.size() > 64 || !textValid(work.context) || !valid(work.key))
            throw std::invalid_argument("unsupported Windows query");
        auto session = base_->session(work.key.simplified);
        mpn_set_segment_context(session.get(), work.context.c_str());
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
        auto choices = windowsCandidates(session.get(), words_, lookup, work.chineseLocked);
        mark(&QueryTimings::merge_ms);
        // Reproduce formal append semantics: existing choices keep their order.
        for (int i=0; i<3 && mpn_candidates_can_expand(session.get()); ++i) {
            mpn_expand_candidates(session.get());
            appendCandidateExpansion(session.get(), choices, windowsCandidates(session.get(), words_, lookup, work.chineseLocked));
        }
        mark(&QueryTimings::expansion_ms);
        std::vector<Candidate> converted;
        for (size_t index=0; index<choices.size(); ++index) {
            const auto &choice = choices[index];
            auto text = choice.text;
            size_t consumed = work.input.size();
            if (choice.coreIndex) {
                const auto tail = mpn_candidate_literal_tail(session.get(), *choice.coreIndex);
                if (tail > text.size()) throw std::runtime_error("invalid candidate tail");
                text.resize(text.size() - tail);
                consumed = mpn_candidate_consumed(session.get(), *choice.coreIndex);
            }
            if (text.empty() || !consumed) continue;
            converted.push_back({index + 1, static_cast<uint32_t>(consumed), std::move(text), choice.annotation});
        }
        CandidatePage page{work.input, false, {}};
        const auto start = size_t(work.page) * PageSize;
        if (start < converted.size()) {
            const auto end = std::min(converted.size(), start + PageSize);
            page.words.assign(converted.begin() + start, converted.begin() + end);
            page.more = end < converted.size() && work.page < 4095;
        }
        encodePayload(page); // Reject bad text/ranges and budget violations atomically.
        mark(&QueryTimings::conversion_ms);
        return page;
    }
};
} // namespace harukoto::engine
