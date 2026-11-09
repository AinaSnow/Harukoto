#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>

struct JapaneseForm { std::string name, text, reading; };
struct JapaneseConjugationEntry {
    std::string id, text, reading;
    std::vector<std::pair<std::string, std::string>> triggers;
    std::vector<JapaneseForm> forms;
};

class JapaneseConjugations {
public:
    static constexpr size_t maxBytes = 4 * 1024 * 1024;
    bool loadFile(const std::filesystem::path &path) {
        clear();
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > maxBytes) return fail("missing or oversized file");
        std::ifstream file(path, std::ios::binary);
        std::string bytes(static_cast<size_t>(size), '\0');
        if (!file.read(bytes.data(), static_cast<std::streamsize>(size)) || file.peek() != EOF)
            return fail("file changed or could not be read");
        return load(bytes);
    }
    bool load(const std::string &bytes) {
        clear();
        if (bytes.size() > maxBytes) return fail("oversized file");
        Reader reader{bytes};
        const std::string magic = "HARUKOTO-CONJ-1\n";
        if (bytes.compare(0, magic.size(), magic) != 0) return fail("unsupported header");
        reader.position = magic.size();
        const auto count = reader.number();
        if (!reader.ok || count == 0 || count > 10000) return fail("invalid entry count");
        std::vector<JapaneseConjugationEntry> parsed;
        std::set<std::string> ids;
        for (uint32_t i = 0; i < count; ++i) {
            JapaneseConjugationEntry entry;
            entry.id = reader.string(); entry.text = reader.string(); entry.reading = reader.string();
            if (!reader.ok || !identifier(entry.id) || !ids.insert(entry.id).second || !kana(entry.reading)) return fail("invalid entry");
            const auto triggers = reader.number();
            if (!reader.ok || triggers == 0 || triggers > 32) return fail("invalid trigger count");
            std::set<std::pair<std::string,std::string>> seenTriggers;
            for (uint32_t j = 0; j < triggers; ++j) {
                auto kind = reader.string(); auto trigger = reader.string();
                if (!reader.ok || (kind != "pinyin" && kind != "english") || trigger.size() < 2 || trigger.size() > 40) return fail("invalid trigger");
                for (char c : trigger) if (c < 'a' || c > 'z') return fail("invalid trigger letters");
                if (!seenTriggers.emplace(kind, trigger).second) return fail("duplicate trigger");
                entry.triggers.emplace_back(std::move(kind), std::move(trigger));
            }
            const auto forms = reader.number();
            if (!reader.ok || forms == 0 || forms > 32) return fail("invalid form count");
            std::set<std::string> names;
            bool base = false;
            for (uint32_t j = 0; j < forms; ++j) {
                JapaneseForm form;
                form.name = reader.string(); form.text = reader.string(); form.reading = reader.string();
                if (!reader.ok || !names.insert(form.name).second || !kana(form.reading)) return fail("invalid form");
                if (form.name == "辞书形") {
                    if (form.text != entry.text || form.reading != entry.reading) return fail("base form mismatch");
                    base = true;
                }
                entry.forms.push_back(std::move(form));
            }
            if (!base) return fail("base form missing");
            parsed.push_back(std::move(entry));
        }
        if (!reader.ok || reader.position != bytes.size()) return fail("trailing or truncated data");
        entries_ = std::move(parsed);
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto &entry = entries_[i];
            words_[{entry.text, entry.reading}].push_back(i);
            for (const auto &trigger : entry.triggers) triggers_[trigger].push_back(i);
        }
        return true;
    }
    const std::vector<JapaneseConjugationEntry> &entries() const { return entries_; }
    std::vector<const JapaneseConjugationEntry *> findWord(const std::string &text, const std::string &reading) const { return find(words_, {text, reading}); }
    std::vector<const JapaneseConjugationEntry *> findTrigger(const std::string &kind, const std::string &value) const { return find(triggers_, {kind, value}); }
    const std::string &error() const { return error_; }
private:
    using Index = std::map<std::pair<std::string,std::string>, std::vector<size_t>>;
    std::vector<JapaneseConjugationEntry> entries_;
    Index words_, triggers_;
    std::string error_;
    void clear() { entries_.clear(); words_.clear(); triggers_.clear(); error_.clear(); }
    bool fail(const char *message) { clear(); error_ = message; return false; }
    std::vector<const JapaneseConjugationEntry *> find(const Index &index, const std::pair<std::string,std::string> &key) const {
        std::vector<const JapaneseConjugationEntry *> result;
        const auto found = index.find(key);
        if (found != index.end()) for (auto i : found->second) result.push_back(&entries_[i]);
        return result;
    }
    static bool identifier(const std::string &s) {
        if (s.empty() || s.size() > 64) return false;
        for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        return true;
    }
    static bool unicode(const std::string &s, bool onlyKana = false) {
        for (size_t i = 0; i < s.size();) {
            uint32_t value = static_cast<unsigned char>(s[i++]);
            unsigned continuation = 0; uint32_t minimum = 0;
            if (value >= 0xc2 && value <= 0xdf) { value &= 31; continuation = 1; minimum = 0x80; }
            else if (value >= 0xe0 && value <= 0xef) { value &= 15; continuation = 2; minimum = 0x800; }
            else if (value >= 0xf0 && value <= 0xf4) { value &= 7; continuation = 3; minimum = 0x10000; }
            else if (value >= 0x80) return false;
            for (unsigned j = 0; j < continuation; ++j) {
                if (i == s.size()) return false;
                const auto next = static_cast<unsigned char>(s[i++]);
                if ((next & 0xc0) != 0x80) return false;
                value = (value << 6) | (next & 63);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) || value < 32 || value == 127) return false;
            if (onlyKana && !((value >= 0x3041 && value <= 0x3096) || value == 0x30fc)) return false;
        }
        return !s.empty();
    }
    static bool kana(const std::string &s) { return unicode(s, true); }
    struct Reader {
        const std::string &bytes; size_t position = 0; bool ok = true;
        uint32_t number() {
            if (!ok || bytes.size() - position < 4) { ok = false; return 0; }
            uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i) value |= uint32_t(static_cast<unsigned char>(bytes[position++])) << (i * 8);
            return value;
        }
        std::string string() {
            const auto size = number();
            if (!ok || size == 0 || size > 256 || size > bytes.size() - position) { ok = false; return {}; }
            auto value = bytes.substr(position, size); position += size;
            if (!unicode(value)) { ok = false; return {}; }
            return value;
        }
    };
};
