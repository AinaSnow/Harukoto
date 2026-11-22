#pragma once
#include "BaseDictionaryFiles.h"
#include "EngineMessages.h"
#include "SelectionMemory.h"
#include "../Core/MeowpuutCore.h"
#include <deque>
#include <map>
#include <memory>
#include <mutex>

// WIN-03 step 6b: the per-user learning the engine ranks with
// (docs/ENGINE_TSF.md). The TSF services stay the only writers of
// selection-memory.tsv, -words.tsv and -user.tsv; the engine reads the same
// files with the same journal code and never writes them (no compaction).
//
// Catch-up: each request names the exact models its client ranks with (a
// SHA-256 of their internal state). The engine serves only with models of the
// same state: when its own differ it reads what was appended, then (at most
// once a second) re-reads everything; if they still differ it answers
// "behind" and the client lists that composition in-process. Learning just
// flushed by the client is therefore always seen by its next request.
namespace harukoto::engine {
inline std::string memoryDigest(const selection_memory::Model &model) { return base_dictionary::sha256(model.state()); }
inline std::string memoryDigest(const selection_memory::WordModel &model) { return base_dictionary::sha256(model.state()); }
inline std::string memoryDigest(const selection_memory::UserWordModel &model) { return base_dictionary::sha256(model.state()); }

using PersonalTable = std::shared_ptr<const MPNPersonal>;
// The personal table a TSF service attaches (attachPersonal): user words,
// plus learned word bonuses at hour when memory is on. Null when neither.
inline PersonalTable buildPersonal(const selection_memory::WordModel *words, int64_t hour,
                                   const selection_memory::UserWordModel &user) {
    if (!words && user.empty()) return {};
    std::shared_ptr<MPNPersonal> table(mpn_personal_create(), mpn_personal_destroy);
    if (!table) throw std::runtime_error("personal table allocation failed");
    for (const auto &entry : user.entries())
        mpn_personal_add_word(table.get(), entry.reading.c_str(), entry.text.c_str(), entry.traditional.c_str());
    if (words) words->each(hour, [&](bool script, std::string_view reading, const std::string &text, double bonus) {
        mpn_personal_set(table.get(), script, std::string(reading).c_str(), text.c_str(), bonus);
    });
    return table;
}

// One immutable state of the three models, shared by concurrent queries.
class MemoryView {
    mutable std::mutex mutex_;
    mutable std::map<std::pair<bool, int64_t>, PersonalTable> tables_;
public:
    selection_memory::Model phrases;
    selection_memory::WordModel words;
    selection_memory::UserWordModel user;
    std::string phrasesDigest, wordsDigest, userDigest;
    bool matches(const QuerySettings &q) const {
        // Step 6d: an empty phrases digest means the client sent its recall.
        return q.user == userDigest && (!q.memory || ((q.phrases.empty() || q.phrases == phrasesDigest) && q.words == wordsDigest));
    }
    PersonalTable personal(bool memory, int64_t hour) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = std::make_pair(memory, memory ? hour : 0);
        const auto found = tables_.find(key);
        if (found != tables_.end()) return found->second;
        if (tables_.size() >= 8) tables_.clear(); // Hours move on; keep a few.
        auto table = buildPersonal(memory ? &words : nullptr, hour, user);
        tables_.emplace(key, table);
        return table;
    }
};

class MemoryMirror {
    std::filesystem::path path_;
    std::mutex mutex_;
    std::unique_ptr<selection_memory::Journal<selection_memory::Model>> phrases_;
    std::unique_ptr<selection_memory::Journal<selection_memory::WordModel>> words_;
    std::unique_ptr<selection_memory::UserWordTable> user_;
    uint64_t seen_[3] = {};
    std::shared_ptr<const MemoryView> view_;
    // Step 6d: the last few states too. A client lists the rest of a reading
    // after a chosen segment with the state from before that choice, while
    // it has already written what the choice taught.
    std::deque<std::shared_ptr<const MemoryView>> history_;
    uint64_t reloaded_ = 0;
    struct Stats { uint64_t refreshes = 0, reloads = 0, behind = 0; } stats_;
    static std::filesystem::path sibling(const std::filesystem::path &path, const wchar_t *suffix) {
        auto name = path.stem(); name += suffix; name += path.extension();
        return path.parent_path() / name;
    }
    void open() {
        phrases_ = std::make_unique<selection_memory::Journal<selection_memory::Model>>(path_);
        words_ = std::make_unique<selection_memory::Journal<selection_memory::WordModel>>(sibling(path_, L"-words"));
        user_ = std::make_unique<selection_memory::UserWordTable>(sibling(path_, L"-user"));
        std::fill(std::begin(seen_), std::end(seen_), 0);
    }
    // Reads what changed (read-only) and publishes a new view when needed.
    void refresh() {
        ++stats_.refreshes;
        phrases_->refresh(); words_->refresh(); user_->refresh();
        const uint64_t now[3] = {phrases_->revision(), words_->revision(), user_->revision()};
        if (view_ && std::equal(std::begin(now), std::end(now), std::begin(seen_))) return;
        auto view = std::make_shared<MemoryView>();
        view->phrases = phrases_->model(); view->words = words_->model(); view->user = user_->model();
        view->phrasesDigest = memoryDigest(view->phrases);
        view->wordsDigest = memoryDigest(view->words);
        view->userDigest = memoryDigest(view->user);
        if (view_) { history_.push_front(view_); while (history_.size() > 4) history_.pop_back(); }
        view_ = std::move(view);
        std::copy(std::begin(now), std::end(now), std::begin(seen_));
    }
public:
    explicit MemoryMirror(std::filesystem::path path) : path_(std::move(path)) {
        if (path_.empty()) throw std::invalid_argument("missing memory path");
        open();
    }
    // The view whose models equal the client's, or null when the engine
    // cannot reach that state now ("behind").
    std::shared_ptr<const MemoryView> match(const QuerySettings &q, uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (view_ && view_->matches(q)) return view_;
        refresh();
        if (view_->matches(q)) return view_;
        for (const auto &earlier : history_) if (earlier->matches(q)) return earlier;
        // A rewritten file of unchanged length, or an engine ahead of a client
        // that has not looked yet: re-read everything, at most once a second.
        if (!reloaded_ || now - reloaded_ >= 1000) {
            reloaded_ = now; ++stats_.reloads;
            open(); view_.reset(); refresh();
            if (view_->matches(q)) return view_;
        }
        ++stats_.behind;
        return {};
    }
    Stats stats() { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
};
} // namespace harukoto::engine
