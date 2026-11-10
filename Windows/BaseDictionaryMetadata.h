#pragma once
#include <charconv>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace base_dictionary {
// The snapshot protocol needs only objects, printable ASCII strings, unsigned
// integers and null. Reject other JSON values, duplicate keys and deep nesting.
// Canonical output matches Python's sorted compact JSON for this ASCII schema.
struct Metadata {
    enum Kind { object, string, integer, null } kind = null;
    std::map<std::string, Metadata> fields;
    std::string value;
    uint64_t number = 0;
    const Metadata &at(const std::string &key) const {
        const auto found = fields.find(key);
        if (kind != object || found == fields.end()) throw std::runtime_error("missing metadata field: " + key);
        return found->second;
    }
    void keys(std::initializer_list<const char *> expected) const {
        if (kind != object || fields.size() != expected.size()) throw std::runtime_error("unexpected metadata fields");
        for (const auto *key : expected) at(key);
    }
    std::string text() const {
        if (kind != string) throw std::runtime_error("expected metadata string");
        return value;
    }
    uint64_t count() const {
        if (kind != integer) throw std::runtime_error("expected metadata integer");
        return number;
    }
    static std::string quote(const std::string &s) {
        std::string result = "\"";
        for (char c : s) { if (c == '"' || c == '\\') result += '\\'; result += c; }
        return result + '"';
    }
    std::string canonical() const {
        if (kind == null) return "null";
        if (kind == string) return quote(value);
        if (kind == integer) return std::to_string(number);
        std::string result = "{";
        for (const auto &field : fields) {
            if (result.size() > 1) result += ',';
            result += quote(field.first) + ':' + field.second.canonical();
        }
        return result + '}';
    }
};

class MetadataParser {
    std::string_view input;
    size_t position = 0;
    [[noreturn]] void bad() const { throw std::runtime_error("invalid snapshot JSON"); }
    void whitespace() {
        while (position < input.size() && (input[position] == ' ' || input[position] == '\t' ||
               input[position] == '\n' || input[position] == '\r')) ++position;
    }
    bool take(char c) {
        whitespace();
        if (position == input.size() || input[position] != c) return false;
        ++position; return true;
    }
    std::string text() {
        if (!take('"')) bad();
        std::string result;
        while (position < input.size()) {
            unsigned char c = input[position++];
            if (c == '"') return result;
            if (c == '\\') {
                if (position == input.size()) bad();
                c = input[position++];
                if (c == 'u') {
                    unsigned value = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (position == input.size()) bad();
                        const char digit = input[position++];
                        value *= 16;
                        if (digit >= '0' && digit <= '9') value += digit - '0';
                        else if (digit >= 'a' && digit <= 'f') value += digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F') value += digit - 'A' + 10;
                        else bad();
                    }
                    if (value < 32 || value > 126) bad();
                    c = static_cast<unsigned char>(value);
                } else if (c != '"' && c != '\\' && c != '/') bad();
            }
            if (c < 32 || c > 126) bad();
            result += static_cast<char>(c);
        }
        bad();
    }
    Metadata read(unsigned depth) {
        if (depth > 6) bad();
        whitespace();
        Metadata result;
        if (position == input.size()) bad();
        if (take('{')) {
            result.kind = Metadata::object;
            if (take('}')) return result;
            do {
                const auto key = text();
                if (!take(':')) bad();
                if (!result.fields.emplace(key, read(depth + 1)).second) bad();
                if (take('}')) return result;
            } while (take(','));
            bad();
        }
        if (input[position] == '"') {
            result.kind = Metadata::string; result.value = text(); return result;
        }
        if (input.substr(position, 4) == "null") { position += 4; return result; }
        const auto start = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') ++position;
        if (position == start || (position - start > 1 && input[start] == '0')) bad();
        const auto parsed = std::from_chars(input.data() + start, input.data() + position, result.number);
        if (parsed.ec != std::errc{}) bad();
        result.kind = Metadata::integer;
        return result;
    }
public:
    explicit MetadataParser(std::string_view bytes) : input(bytes) {}
    Metadata parse() {
        if (input.size() > 65536) bad();
        auto result = read(0); whitespace();
        if (position != input.size()) bad();
        return result;
    }
};
inline bool generation(const std::string &s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
} // namespace base_dictionary
