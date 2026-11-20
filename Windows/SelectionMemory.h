#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// CN-06: remember what the user actually selected, per reading and output
// script, and offer it first the next time the same reading is typed.
//
// Only explicit selections are recorded; nothing is inferred from text the
// user merely passed over. The file lives under the current user's local
// application data and never leaves the machine. All methods belong to the
// input thread that owns the service; the file is shared by every process
// that loads the input service, so it is an append-only journal.
namespace selection_memory {

constexpr size_t MaxReading = 128, MaxText = 192;
constexpr uint32_t MaxCount = 8;            // A habit can still be overturned.
constexpr size_t MaxEntries = 20000, MaxPerReading = 8;
constexpr size_t MaxFile = 4 * 1024 * 1024, CompactAt = 1024 * 1024;

inline bool enabled(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 1, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"SelectionMemory",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result != ERROR_SUCCESS || value != 0;
}

// In-memory model. Deterministic and file-free so it can be tested directly.
class Model {
public:
    struct Entry { uint32_t count = 0; uint64_t order = 0; };
private:
    // reading + '\t' + script -> text -> entry
    std::map<std::string, std::map<std::string, Entry>> readings_;
    uint64_t clock_ = 0;
    size_t entries_ = 0;
    static std::string key(std::string_view reading, bool simplified) {
        std::string value(reading); value += simplified ? "\ts" : "\tt"; return value;
    }
public:
    static bool acceptable(std::string_view reading, std::string_view text) {
        if (reading.empty() || text.empty() || reading.size() > MaxReading || text.size() > MaxText) return false;
        if (!std::all_of(reading.begin(), reading.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '\''; })) return false;
        // Text is UTF-8 from the candidate list; control bytes would break the journal.
        return std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
    }
    size_t size() const { return entries_; }
    void clear() { readings_.clear(); entries_ = 0; clock_ = 0; }
    // Returns false when the pair cannot be stored. amount > 1 replays a
    // compacted journal row.
    bool record(std::string_view reading, std::string_view text, bool simplified, uint32_t amount = 1) {
        if (!amount || !acceptable(reading, text)) return false;
        auto &texts = readings_[key(reading, simplified)];
        auto found = texts.find(std::string(text));
        if (found == texts.end()) {
            if (texts.size() == MaxPerReading) {
                // Drop the least useful alternative of this reading.
                auto weakest = std::min_element(texts.begin(), texts.end(), [](const auto &a, const auto &b) {
                    return a.second.count != b.second.count ? a.second.count < b.second.count : a.second.order < b.second.order;
                });
                texts.erase(weakest); --entries_;
            }
            found = texts.emplace(std::string(text), Entry{}).first; ++entries_;
        }
        found->second.count = (std::min)(MaxCount, found->second.count + amount);
        found->second.order = ++clock_;
        if (entries_ > MaxEntries) evict();
        return true;
    }
    // Most selected first; the most recent selection breaks a tie, so one
    // correction is enough to replace one mistake.
    std::vector<std::string> recall(std::string_view reading, bool simplified) const {
        const auto found = readings_.find(key(reading, simplified));
        if (found == readings_.end()) return {};
        std::vector<std::pair<std::string, Entry>> ranked(found->second.begin(), found->second.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            return a.second.count != b.second.count ? a.second.count > b.second.count : a.second.order > b.second.order;
        });
        std::vector<std::string> result;
        for (auto &item : ranked) result.push_back(std::move(item.first));
        return result;
    }
    // Journal row: count<TAB>script<TAB>reading<TAB>text<LF>.
    static std::string row(std::string_view reading, std::string_view text, bool simplified, uint32_t count = 1) {
        return std::to_string(count) + (simplified ? "\ts\t" : "\tt\t") + std::string(reading) + '\t' + std::string(text) + '\n';
    }
    // Replays complete rows; malformed rows are skipped one by one. Returns
    // the number of bytes consumed, never splitting a row.
    size_t replay(std::string_view data) {
        size_t consumed = 0;
        for (;;) {
            const auto end = data.find('\n', consumed);
            if (end == data.npos) break;
            auto line = data.substr(consumed, end - consumed);
            consumed = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const auto first = line.find('\t');
            const auto second = first == line.npos ? line.npos : line.find('\t', first + 1);
            const auto third = second == line.npos ? line.npos : line.find('\t', second + 1);
            if (third == line.npos || first == 0 || first > 2) continue;
            uint32_t count = 0;
            bool digits = true;
            for (char c : line.substr(0, first)) { digits = digits && c >= '0' && c <= '9'; count = count * 10 + static_cast<uint32_t>(c - '0'); }
            const auto script = line.substr(first + 1, second - first - 1);
            if (!digits || !count || (script != "s" && script != "t")) continue;
            record(line.substr(second + 1, third - second - 1), line.substr(third + 1), script == "s", count);
        }
        return consumed;
    }
    // Aggregated rows in order of use, for compaction and export.
    std::string snapshot() const {
        struct Item { uint64_t order; std::string line; };
        std::vector<Item> items;
        for (const auto &reading : readings_) {
            const auto tab = reading.first.rfind('\t');
            const bool simplified = reading.first.back() == 's';
            for (const auto &text : reading.second)
                items.push_back({text.second.order, row(std::string_view(reading.first).substr(0, tab), text.first, simplified, text.second.count)});
        }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.order < b.order; });
        std::string result;
        for (auto &item : items) result += item.line;
        return result;
    }
private:
    void evict() {
        // Rare: remove the oldest tenth rather than one entry per selection.
        std::vector<uint64_t> orders;
        for (const auto &reading : readings_) for (const auto &text : reading.second) orders.push_back(text.second.order);
        std::nth_element(orders.begin(), orders.begin() + orders.size() / 10, orders.end());
        const auto threshold = orders[orders.size() / 10];
        for (auto reading = readings_.begin(); reading != readings_.end();) {
            for (auto text = reading->second.begin(); text != reading->second.end();) {
                if (text->second.order <= threshold) { text = reading->second.erase(text); --entries_; }
                else ++text;
            }
            reading = reading->second.empty() ? readings_.erase(reading) : std::next(reading);
        }
    }
};

// CN-06c: how often the user confirmed each dictionary word, per untoned
// reading and script, whichever sentence it was part of. The core turns this
// into a bounded bonus for that word wherever its reading is typed.
//
// Weights grow by one per confirmed selection and are capped, halve every
// HalfLifeHours, and halve for every other word confirmed for the same
// reading: one slip is undone by one correction, and an old habit fades.
constexpr double HalfLifeHours = 14 * 24;
constexpr double MaxWeight = 20;        // log1p(19) reaches BonusCap.
constexpr double MinWeight = 0.05;      // Below this an entry is forgotten.
constexpr double BonusCap = 3.0;        // Nats; the core clamps to the same.

// Whole hours since 1970 (UTC); the journal's clock.
inline int64_t currentHour() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t ticks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return ticks > 116444736000000000ULL ? int64_t((ticks - 116444736000000000ULL) / 36000000000ULL) : 0;
}

class WordModel {
public:
    struct Entry { double weight = 0; int64_t hour = 0; uint64_t order = 0; };
private:
    // reading + '\t' + script -> text -> entry
    std::map<std::string, std::map<std::string, Entry>> readings_;
    uint64_t clock_ = 0, revision_ = stamp();
    int64_t latest_ = 0;
    size_t entries_ = 0;
    static std::string key(std::string_view reading, bool simplified) {
        std::string value(reading); value += simplified ? "\ts" : "\tt"; return value;
    }
    static double decayed(const Entry &entry, int64_t hour) {
        return hour > entry.hour ? entry.weight * std::exp2(-double(hour - entry.hour) / HalfLifeHours) : entry.weight;
    }
    static void age(Entry &entry, int64_t hour) {
        if (hour > entry.hour) { entry.weight = decayed(entry, hour); entry.hour = hour; }
    }
    Entry *slot(std::map<std::string, Entry> &texts, std::string_view text, int64_t hour) {
        auto found = texts.find(std::string(text));
        if (found != texts.end()) return &found->second;
        if (texts.size() == MaxPerReading) {
            auto weakest = std::min_element(texts.begin(), texts.end(), [&](const auto &a, const auto &b) {
                return decayed(a.second, hour) < decayed(b.second, hour);
            });
            texts.erase(weakest); --entries_;
        }
        ++entries_;
        return &texts.emplace(std::string(text), Entry{}).first->second;
    }
    void settle(std::map<std::string, Entry> &texts, const std::string &group) {
        for (auto it = texts.begin(); it != texts.end();) {
            if (it->second.weight < MinWeight) { it = texts.erase(it); --entries_; }
            else ++it;
        }
        if (texts.empty()) readings_.erase(group);
        if (entries_ > MaxEntries) evict();
        revision_ = stamp();
    }
    // Unique across every model in the process, so a replaced model never
    // repeats a revision the host has already seen.
    static uint64_t stamp() { static std::atomic<uint64_t> next{0}; return ++next; }
    void evict() {
        std::vector<uint64_t> orders;
        for (const auto &reading : readings_) for (const auto &text : reading.second) orders.push_back(text.second.order);
        std::nth_element(orders.begin(), orders.begin() + orders.size() / 10, orders.end());
        const auto threshold = orders[orders.size() / 10];
        for (auto reading = readings_.begin(); reading != readings_.end();) {
            for (auto text = reading->second.begin(); text != reading->second.end();) {
                if (text->second.order <= threshold) { text = reading->second.erase(text); --entries_; }
                else ++text;
            }
            reading = reading->second.empty() ? readings_.erase(reading) : std::next(reading);
        }
    }
public:
    static bool acceptable(std::string_view reading, std::string_view text) {
        return Model::acceptable(reading, text) && reading.find('\'') == reading.npos;
    }
    size_t size() const { return entries_; }
    // Changes whenever any bonus may have changed, for the host's sync.
    uint64_t revision() const { return revision_; }
    void clear() { readings_.clear(); entries_ = 0; clock_ = 0; latest_ = 0; revision_ = stamp(); }
    // One confirmed selection of text for reading at hour.
    bool select(std::string_view reading, std::string_view text, bool simplified, int64_t hour) {
        if (!acceptable(reading, text) || hour <= 0) return false;
        latest_ = (std::max)(latest_, hour);
        const auto group = key(reading, simplified);
        auto &texts = readings_[group];
        for (auto &other : texts) {
            if (other.first == text) continue;
            age(other.second, hour);
            other.second.weight /= 2;
        }
        auto *entry = slot(texts, text, hour);
        age(*entry, hour);
        entry->weight = (std::min)(MaxWeight, entry->weight + 1);
        entry->hour = (std::max)(entry->hour, hour);
        entry->order = ++clock_;
        settle(texts, group);
        return true;
    }
    // A compacted row: the weight as it stood at hour.
    bool restore(std::string_view reading, std::string_view text, bool simplified, double weight, int64_t hour) {
        if (!acceptable(reading, text) || hour <= 0 || !(weight > 0)) return false;
        latest_ = (std::max)(latest_, hour);
        const auto group = key(reading, simplified);
        auto &texts = readings_[group];
        auto *entry = slot(texts, text, hour);
        age(*entry, hour);
        entry->weight = (std::min)(MaxWeight, entry->weight + weight);
        entry->hour = (std::max)(entry->hour, hour);
        entry->order = ++clock_;
        settle(texts, group);
        return true;
    }
    static double bonus(double weight) {
        return weight < MinWeight ? 0 : (std::min)(BonusCap, std::log1p(weight));
    }
    double weight(std::string_view reading, std::string_view text, bool simplified, int64_t hour) const {
        const auto group = readings_.find(key(reading, simplified));
        if (group == readings_.end()) return 0;
        const auto found = group->second.find(std::string(text));
        return found == group->second.end() ? 0 : decayed(found->second, hour);
    }
    double bonus(std::string_view reading, std::string_view text, bool simplified, int64_t hour) const {
        return bonus(weight(reading, text, simplified, hour));
    }
    // Every learned word with a non-zero bonus at hour: f(simplified, reading, text, bonus).
    template<class F> void each(int64_t hour, F f) const {
        for (const auto &group : readings_) {
            const auto tab = group.first.rfind('\t');
            const auto reading = std::string_view(group.first).substr(0, tab);
            const bool simplified = group.first.back() == 's';
            for (const auto &text : group.second)
                if (const double value = bonus(decayed(text.second, hour)); value > 0) f(simplified, reading, text.first, value);
        }
    }
    // The same for one reading.
    template<class F> void each(std::string_view reading, bool simplified, int64_t hour, F f) const {
        const auto group = readings_.find(key(reading, simplified));
        if (group == readings_.end()) return;
        for (const auto &text : group->second)
            if (const double value = bonus(decayed(text.second, hour)); value > 0) f(text.first, value);
    }
    // Journal rows. A selection: +<TAB>s|t<TAB>reading<TAB>text<TAB>hour.
    // A compacted weight in hundredths: =NNN<TAB>s|t<TAB>reading<TAB>text<TAB>hour.
    static std::string event(std::string_view reading, std::string_view text, bool simplified, int64_t hour) {
        return std::string("+") + (simplified ? "\ts\t" : "\tt\t") + std::string(reading) + '\t' + std::string(text) + '\t' + std::to_string(hour) + '\n';
    }
    size_t replay(std::string_view data) {
        size_t consumed = 0;
        for (;;) {
            const auto end = data.find('\n', consumed);
            if (end == data.npos) break;
            auto line = data.substr(consumed, end - consumed);
            consumed = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            std::string_view fields[6];
            size_t count = 0;
            for (size_t start = 0; count < 6;) {
                const auto tab = line.find('\t', start);
                fields[count++] = line.substr(start, tab == line.npos ? line.npos : tab - start);
                if (tab == line.npos) break;
                start = tab + 1;
            }
            if (count != 5 || (fields[1] != "s" && fields[1] != "t")) continue;
            const auto number = [](std::string_view value, int64_t limit, int64_t &out) {
                if (value.empty() || value.size() > 9) return false;
                out = 0;
                for (char c : value) { if (c < '0' || c > '9') return false; out = out * 10 + (c - '0'); }
                return out > 0 && out <= limit;
            };
            int64_t hour = 0, hundredths = 0;
            if (!number(fields[4], 100000000, hour)) continue;
            if (fields[0] == "+") select(fields[2], fields[3], fields[1] == "s", hour);
            else if (fields[0].size() > 1 && fields[0][0] == '=' && number(fields[0].substr(1), int64_t(MaxWeight * 100), hundredths))
                restore(fields[2], fields[3], fields[1] == "s", hundredths / 100.0, hour);
        }
        return consumed;
    }
    // Current weights in order of last use, forgetting faded words.
    std::string snapshot() const {
        struct Item { uint64_t order; std::string line; };
        std::vector<Item> items;
        for (const auto &group : readings_) {
            const auto tab = group.first.rfind('\t');
            const bool simplified = group.first.back() == 's';
            for (const auto &text : group.second) {
                const auto hundredths = static_cast<int64_t>(decayed(text.second, latest_) * 100 + 0.5);
                if (hundredths < MinWeight * 100) continue;
                items.push_back({text.second.order, "=" + std::to_string(hundredths) + (simplified ? "\ts\t" : "\tt\t") +
                    group.first.substr(0, tab) + '\t' + text.first + '\t' + std::to_string(latest_) + '\n'});
            }
        }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.order < b.order; });
        std::string result;
        for (auto &item : items) result += item.line;
        return result;
    }
};

namespace detail {
inline HANDLE open(const std::filesystem::path &path, DWORD access, DWORD share, DWORD creation) {
    return CreateFileW(path.c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
}
inline bool readAt(HANDLE file, uint64_t from, uint64_t size, std::string &out) {
    LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(from);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    out.resize(static_cast<size_t>(size));
    DWORD done = 0;
    if (!out.empty() && (!ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &done, nullptr) || done != out.size())) return false;
    return true;
}
} // namespace detail

// One append-only journal file and the model it replays. Rows written by this
// object are applied to the model when they are recorded, so reading them
// back must not count them twice: the journal tracks how far it has read and
// how many of its own bytes lie beyond that point.
template<class M>
class Journal {
    std::filesystem::path path_;
    M model_;
    std::string pending_;          // Rows not yet appended.
    uint64_t offset_ = 0;          // File bytes already reflected in the model.
public:
    explicit Journal(std::filesystem::path path) : path_(std::move(path)) {}
    const std::filesystem::path &path() const { return path_; }
    M &model() { return model_; }
    const M &model() const { return model_; }
    bool pending() const { return !pending_.empty(); }
    void append(const std::string &row) { pending_ += row; }
    // Reads whatever other writers appended since the last look. A shorter
    // file means it was cleared or compacted: start again from its beginning.
    void refresh() {
        const HANDLE file = detail::open(path_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
                if (offset_) { model_.clear(); offset_ = 0; }
            }
            return;
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) <= MaxFile) {
            const auto length = static_cast<uint64_t>(size.QuadPart);
            if (length < offset_) { model_.clear(); offset_ = 0; }
            std::string data;
            if (length > offset_ && detail::readAt(file, offset_, length - offset_, data)) offset_ += model_.replay(data);
        }
        CloseHandle(file);
    }
    // One small append per finished composition. Appends by several
    // processes interleave as whole rows. On failure the rows stay pending.
    bool flush() {
        if (pending_.empty()) return true;
        if (pending_.size() > 64 * 1024) { pending_.clear(); return false; } // Persistently unwritable.
        std::error_code ignored;
        std::filesystem::create_directories(path_.parent_path(), ignored);
        // Bring the model up to date first, so the bytes after offset_ are
        // exactly the rows written here.
        refresh();
        const HANDLE file = detail::open(path_, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_ALWAYS);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        DWORD done = 0;
        const bool sized = GetFileSizeEx(file, &size) != 0;
        const bool ok = sized && static_cast<uint64_t>(size.QuadPart) + pending_.size() <= MaxFile &&
            WriteFile(file, pending_.data(), static_cast<DWORD>(pending_.size()), &done, nullptr) && done == pending_.size();
        CloseHandle(file);
        if (ok) {
            // Rows appended by others between refresh and this write are
            // picked up by the next refresh only if the file did not grow
            // in between; otherwise re-read everything once.
            if (static_cast<uint64_t>(size.QuadPart) == offset_) offset_ += pending_.size();
            else { model_.clear(); offset_ = 0; pending_.clear(); refresh(); return true; }
            pending_.clear();
        } else if (sized && static_cast<uint64_t>(size.QuadPart) + pending_.size() > MaxFile) pending_.clear();
        return ok;
    }
    // Rewrites a long journal as one row per remembered entry. Exclusive:
    // if another process has the file open, try again on a later start.
    bool compact() {
        if (offset_ < CompactAt) return false;
        const HANDLE file = detail::open(path_, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        std::string data;
        bool ok = GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) <= MaxFile && detail::readAt(file, 0, static_cast<uint64_t>(size.QuadPart), data);
        if (ok) {
            M rebuilt; rebuilt.replay(data);
            const auto rows = rebuilt.snapshot();
            LARGE_INTEGER zero{}; DWORD done = 0;
            ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) &&
                WriteFile(file, rows.data(), static_cast<DWORD>(rows.size()), &done, nullptr) && done == rows.size() && SetEndOfFile(file);
            if (ok) { model_ = std::move(rebuilt); offset_ = rows.size(); }
        }
        CloseHandle(file);
        return ok;
    }
};

// DICT-02b: words the user added by hand ("<name>-user.tsv" beside the
// journals). Unlike the journals this is a plain table that only the
// management script writes, always as a whole and by atomic replacement, so
// that export, import and rollback are copies of one file. Each row:
//   reading<TAB>simplified[<TAB>traditional]
// reading is pinyin letters with optional tone digits and apostrophes between
// syllables; '#' starts a comment. Malformed rows are skipped one by one; a
// later row for the same untoned reading and text replaces an earlier one.
class UserWordModel {
public:
    struct Entry { std::string reading, text, traditional; };
    static constexpr size_t MaxWords = 10000, MaxReadingBytes = 128, MaxTextBytes = 192;
    static constexpr size_t MaxFile = 2 * 1024 * 1024;
private:
    std::vector<Entry> entries_;
    std::map<std::pair<std::string, std::string>, size_t> index_;
public:
    // The untoned key, or empty for a malformed reading.
    static std::string key(std::string_view reading) {
        if (reading.empty() || reading.size() > MaxReadingBytes) return {};
        std::string result;
        char previous = '\'';
        for (char c : reading) {
            if (c >= 'a' && c <= 'z') result += c;
            else if (c >= '1' && c <= '5') { if (previous < 'a' || previous > 'z') return {}; }
            else if (c == '\'') { if (previous == '\'') return {}; }
            else return {};
            previous = c;
        }
        return result;
    }
    static bool text(std::string_view value, bool optional = false) {
        if (value.empty()) return optional;
        if (value.size() > MaxTextBytes) return false;
        // UTF-8 checks are left to the core, which rejects malformed text;
        // control bytes would break the table.
        return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
    }
    const std::vector<Entry> &entries() const { return entries_; }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void clear() { entries_.clear(); index_.clear(); }
    bool add(std::string_view reading, std::string_view simplified, std::string_view traditional = {}) {
        const auto bare = key(reading);
        if (bare.empty() || !text(simplified) || !text(traditional, true)) return false;
        const auto found = index_.find({bare, std::string(simplified)});
        if (found != index_.end()) {
            entries_[found->second].reading = reading;
            entries_[found->second].traditional = traditional;
            return true;
        }
        if (entries_.size() >= MaxWords) return false;
        index_.emplace(std::make_pair(bare, std::string(simplified)), entries_.size());
        entries_.push_back({std::string(reading), std::string(simplified), std::string(traditional)});
        return true;
    }
    // Replaces the model by the rows of data.
    void parse(std::string_view data) {
        clear();
        if (data.size() >= 3 && data.substr(0, 3) == "\xEF\xBB\xBF") data.remove_prefix(3);
        for (size_t start = 0; start < data.size();) {
            auto end = data.find('\n', start);
            if (end == data.npos) end = data.size();
            auto line = data.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line.front() == '#') continue;
            const auto first = line.find('\t');
            if (first == line.npos) continue;
            const auto second = line.find('\t', first + 1);
            const auto rest = second == line.npos ? std::string_view() : line.substr(second + 1);
            if (rest.find('\t') != rest.npos) continue;
            add(line.substr(0, first), line.substr(first + 1, (second == line.npos ? line.size() : second) - first - 1), rest);
        }
    }
};

// The table file as last read. poll() looks at its size, time and identity
// and re-reads the whole file only when one changed; a file that cannot be
// read (locked, too large) keeps the last good table and is retried later.
// A missing file is an empty table.
class UserWordTable {
    std::filesystem::path path_;
    UserWordModel model_;
    uint64_t revision_ = 0, stamp_[4] = {};
    bool seen_ = false;
    // Unique across every table in the process; 0 stands for "empty, never read".
    static uint64_t next() { static std::atomic<uint64_t> counter{0}; return ++counter; }
public:
    explicit UserWordTable(std::filesystem::path path) : path_(std::move(path)) {}
    const std::filesystem::path &path() const { return path_; }
    const UserWordModel &model() const { return model_; }
    // Changes whenever the table may have changed.
    uint64_t revision() const { return revision_; }
    // Returns true when the table changed.
    bool refresh() {
        const HANDLE file = detail::open(path_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        if (file == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
            std::fill(std::begin(stamp_), std::end(stamp_), 0);
            seen_ = true;
            if (model_.empty()) return false;
            model_.clear(); revision_ = next();
            return true;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        bool changed = false;
        if (GetFileInformationByHandle(file, &info)) {
            const uint64_t stamp[4] = {
                (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
                (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
                (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
                info.dwVolumeSerialNumber | 0x100000000ULL};
            const uint64_t size = stamp[0];
            std::string data;
            if ((!seen_ || !std::equal(std::begin(stamp), std::end(stamp), std::begin(stamp_))) &&
                size <= UserWordModel::MaxFile && detail::readAt(file, 0, size, data)) {
                UserWordModel parsed;
                parsed.parse(data);
                model_ = std::move(parsed);
                std::copy(std::begin(stamp), std::end(stamp), std::begin(stamp_));
                seen_ = true; changed = true; revision_ = next();
            }
        }
        CloseHandle(file);
        return changed;
    }
};

// Both journals of one user. CN-06a/b phrases keep their original file, which
// older versions also read and compact; CN-06c words live beside it in
// "<name>-words.tsv", so an older version can neither misread nor drop them.
// One switch, one poll and one clear cover both.
//
// DICT-02b: the user's own words live in the same folder as "<name>-user.tsv"
// and are polled by the same store, but independently of the switch: they are
// explicit dictionary entries, not learning, so pollUserWords runs whether or
// not selection memory is on, and clearing the journals leaves them alone.
class Store {
    Journal<Model> phrases_;
    Journal<WordModel> words_;
    UserWordTable user_;
    ULONGLONG checked_ = 0, userChecked_ = 0;
    bool loaded_ = false, userLoaded_ = false;
    static std::filesystem::path sibling(const std::filesystem::path &path, const wchar_t *suffix) {
        auto name = path.stem();
        name += suffix;
        name += path.extension();
        return path.parent_path() / name;
    }
public:
    explicit Store(std::filesystem::path path) : phrases_(path), words_(sibling(path, L"-words")), user_(sibling(path, L"-user")) {}
    const Model &model() const { return phrases_.model(); }
    const WordModel &words() const { return words_.model(); }
    const std::filesystem::path &wordsFile() const { return words_.path(); }
    const UserWordModel &userWords() const { return user_.model(); }
    uint64_t userWordsRevision() const { return user_.revision(); }
    const std::filesystem::path &userWordsFile() const { return user_.path(); }
    // Cheap after the first call; looks at the table at most once per interval.
    // Returns true when the user words changed.
    bool pollUserWords(ULONGLONG now = GetTickCount64(), ULONGLONG interval = 2000) {
        if (userLoaded_ && now - userChecked_ < interval) return false;
        userChecked_ = now; userLoaded_ = true;
        return user_.refresh();
    }
    // Cheap after the first call; looks at the files at most once per interval.
    void poll(ULONGLONG now = GetTickCount64(), ULONGLONG interval = 2000) {
        if (loaded_ && now - checked_ < interval) return;
        checked_ = now;
        if (phrases_.pending()) phrases_.flush();
        if (words_.pending()) words_.flush();
        phrases_.refresh();
        words_.refresh();
        if (!loaded_) { loaded_ = true; phrases_.compact(); words_.compact(); }
    }
    std::vector<std::string> recall(std::string_view reading, bool simplified) const { return phrases_.model().recall(reading, simplified); }
    void record(std::string_view reading, std::string_view text, bool simplified) {
        if (phrases_.model().record(reading, text, simplified)) phrases_.append(Model::row(reading, text, simplified));
    }
    // CN-06c: one confirmed dictionary word.
    void recordWord(std::string_view reading, std::string_view text, bool simplified, int64_t hour = currentHour()) {
        if (words_.model().select(reading, text, simplified, hour)) words_.append(WordModel::event(reading, text, simplified, hour));
    }
    bool flush() {
        const bool phrases = phrases_.flush();
        const bool words = words_.flush();
        return phrases && words;
    }
    bool compact() {
        const bool phrases = phrases_.compact();
        const bool words = words_.compact();
        return phrases || words;
    }
};

// Moves remembered selections for the whole reading to the front. A
// remembered text that is no longer among the candidates (a phrase the user
// composed segment by segment) is offered as a whole-input candidate.
template<class Candidate, class Consumed>
void apply(std::vector<Candidate> &choices, const std::string &input, const std::vector<std::string> &remembered, Consumed consumed) {
    size_t front = 0;
    for (const auto &text : remembered) {
        if (front == 3) break; // Keep the rest of the first page for the dictionary.
        auto found = std::find_if(choices.begin() + front, choices.end(), [&](const Candidate &c) {
            return c.text == text && consumed(c) == input.size();
        });
        if (found == choices.end()) {
            if (std::any_of(choices.begin(), choices.begin() + front, [&](const Candidate &c) { return c.text == text; })) continue;
            Candidate added{}; added.text = text; added.strong = true;
            choices.insert(choices.begin() + front, std::move(added));
        } else std::rotate(choices.begin() + front, found, found + 1);
        ++front;
    }
}
} // namespace selection_memory
