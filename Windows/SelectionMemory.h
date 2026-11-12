#pragma once
#include <windows.h>
#include <algorithm>
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

// The journal on disk. Rows written by this object are applied to the model
// when they are recorded, so reading them back must not count them twice:
// the store tracks how far it has read and how many of its own bytes lie
// beyond that point.
class Store {
    std::filesystem::path path_;
    Model model_;
    std::string pending_;          // Rows not yet appended.
    uint64_t offset_ = 0;          // File bytes already reflected in the model.
    ULONGLONG checked_ = 0;
    bool loaded_ = false;
    static HANDLE open(const std::filesystem::path &path, DWORD access, DWORD share, DWORD creation) {
        return CreateFileW(path.c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    static bool readAt(HANDLE file, uint64_t from, uint64_t size, std::string &out) {
        LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(from);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
        out.resize(static_cast<size_t>(size));
        DWORD done = 0;
        if (!out.empty() && (!ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &done, nullptr) || done != out.size())) return false;
        return true;
    }
    // Reads whatever other writers appended since the last look. A shorter
    // file means it was cleared or compacted: start again from its beginning.
    void refresh() {
        const HANDLE file = open(path_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
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
            if (length > offset_ && readAt(file, offset_, length - offset_, data)) offset_ += model_.replay(data);
        }
        CloseHandle(file);
    }
public:
    explicit Store(std::filesystem::path path) : path_(std::move(path)) {}
    const Model &model() const { return model_; }
    // Cheap after the first call; looks at the file at most once per interval.
    void poll(ULONGLONG now = GetTickCount64(), ULONGLONG interval = 2000) {
        if (loaded_ && now - checked_ < interval) return;
        checked_ = now;
        if (!pending_.empty()) flush();
        refresh();
        if (!loaded_) { loaded_ = true; compact(); }
    }
    std::vector<std::string> recall(std::string_view reading, bool simplified) const { return model_.recall(reading, simplified); }
    void record(std::string_view reading, std::string_view text, bool simplified) {
        if (model_.record(reading, text, simplified)) pending_ += Model::row(reading, text, simplified);
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
        const HANDLE file = open(path_, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_ALWAYS);
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
    // Rewrites a long journal as one row per remembered selection. Exclusive:
    // if another process has the file open, try again on a later start.
    bool compact() {
        if (offset_ < CompactAt) return false;
        const HANDLE file = open(path_, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        std::string data;
        bool ok = GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) <= MaxFile && readAt(file, 0, static_cast<uint64_t>(size.QuadPart), data);
        if (ok) {
            Model rebuilt; rebuilt.replay(data);
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
