#pragma once
// DICT-02a: candidate sources and conflicts, read-only.
//
// Rebuilds the candidate list for one reading exactly as the text service
// does (windowsCandidates, selection memory, CN-07 generated candidates) from
// the same files, then says for every candidate where it came from, what its
// score is made of, whether the user's own data moved it, and which sources
// disagree. A reverse lookup lists every place a text exists.
//
// Nothing here writes: every file is opened for reading (the selection
// memory journals through Journal::refresh, never poll/flush/compact), the
// registry is only read, SQLite is opened read-only, and the core functions
// used do not change ranking. Output stays in the process.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "BaseDictionary.h"
#include "FuzzyPinyin.h"
#include "PersonalDictionary.h"
#include "ScriptPreference.h"
#include "SelectionMemory.h"
#include "TypoCorrection.h"
#include <cstdio>
#include <functional>

namespace candidate_explain {

struct Options {
    std::filesystem::path program; // Folder of native-pinyin.tsv, japanese.tsv, ...
    std::filesystem::path local;   // %LOCALAPPDATA%\Meowpuut
    bool simplified = true;
    uint32_t fuzzy = 0;
    bool typo = true;
    bool memory = true;            // Selection memory switch (CN-06).
    bool baseSnapshot = true;      // Use an installed base dictionary snapshot like the IME.
    std::string context;           // Ranking context before the composition.
    size_t limit = 20;             // Candidates shown; 0 = all.
    bool expand = false;           // Expand the list to its end, as repeated PageDown does.
};

// Reads the user's settings as the text service does.
inline Options settingsFromRegistry(Options options) {
    options.simplified = readSimplifiedPreference();
    options.fuzzy = readFuzzyPinyin();
    options.typo = readTypoCorrection();
    options.memory = selection_memory::enabled();
    return options;
}

namespace detail {
inline std::string number(double value, bool sign = true) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), sign ? "%+.2f" : "%.2f", value);
    return buffer;
}
inline std::vector<std::string> split(std::string_view line, char separator) {
    std::vector<std::string> fields;
    for (size_t start = 0;;) {
        const auto end = line.find(separator, start);
        fields.emplace_back(line.substr(start, end == line.npos ? line.npos : end - start));
        if (end == line.npos) break;
        start = end + 1;
    }
    return fields;
}
inline std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> result;
    for (size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == text.npos) end = text.size();
        if (end > start) result.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return result;
}
inline std::string untoned(std::string_view reading) {
    std::string result;
    for (char c : reading) if (c >= 'a' && c <= 'z') result += c;
    return result;
}
inline std::string readAll(const std::filesystem::path &path, uint64_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit) return {};
    std::ifstream file(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!file || !file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
}
inline std::string display(const std::filesystem::path &path) { return path.u8string(); }

// Just enough JSON for the theme package state written by
// scripts/japanese_packages.py: objects, arrays, strings, numbers, booleans
// and null; values that are not needed are skipped without being stored.
class Json {
    std::string_view in;
    size_t at = 0;
    [[noreturn]] void bad() const { throw std::runtime_error("invalid theme state JSON"); }
    void space() { while (at < in.size() && (in[at] == ' ' || in[at] == '\n' || in[at] == '\r' || in[at] == '\t')) ++at; }
    static void utf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
        else { out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
    }
    uint32_t hex4() {
        if (at + 4 > in.size()) bad();
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = in[at++];
            value = value * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (bad(), 0));
        }
        return value;
    }
public:
    explicit Json(std::string_view bytes) : in(bytes) {}
    bool done() { space(); return at == in.size(); }
    bool take(char c) { space(); if (at < in.size() && in[at] == c) { ++at; return true; } return false; }
    void expect(char c) { if (!take(c)) bad(); }
    char peek() { space(); if (at == in.size()) bad(); return in[at]; }
    std::string text() {
        expect('"');
        std::string out;
        while (at < in.size()) {
            const char c = in[at++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (at == in.size()) bad();
            const char e = in[at++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff && at + 6 <= in.size() && in[at] == '\\' && in[at + 1] == 'u') {
                    at += 2;
                    const uint32_t low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) bad();
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, cp); break;
            }
            default: bad();
            }
        }
        bad();
    }
    double numberValue() {
        space();
        const size_t start = at;
        while (at < in.size() && std::string_view("+-0123456789.eE").find(in[at]) != std::string_view::npos) ++at;
        if (start == at) bad();
        return std::strtod(std::string(in.substr(start, at - start)).c_str(), nullptr);
    }
    bool boolean() {
        space();
        if (in.substr(at, 4) == "true") { at += 4; return true; }
        if (in.substr(at, 5) == "false") { at += 5; return false; }
        bad();
    }
    void skip(unsigned depth = 0) {
        if (depth > 64) bad();
        const char c = peek();
        if (c == '{') {
            ++at;
            if (take('}')) return;
            do { text(); expect(':'); skip(depth + 1); } while (take(','));
            expect('}');
        } else if (c == '[') {
            ++at;
            if (take(']')) return;
            do skip(depth + 1); while (take(','));
            expect(']');
        } else if (c == '"') text();
        else if (c == 't' || c == 'f') boolean();
        else if (in.substr(at, 4) == "null") at += 4;
        else numberValue();
    }
    // Calls field(key) for each member; the callback consumes the value.
    template<class F> void object(F field) {
        expect('{');
        if (take('}')) return;
        do { const auto key = text(); expect(':'); field(key); } while (take(','));
        expect('}');
    }
    template<class F> void array(F item) {
        expect('[');
        if (take(']')) return;
        do item(); while (take(','));
        expect(']');
    }
};
} // namespace detail

struct ThemePackage {
    std::string id, name, version;
    bool enabled = false;
    std::vector<JapaneseWord> entries;
};

// The packages of a theme state.json, current versions only.
inline std::vector<ThemePackage> readThemeState(std::string_view bytes) {
    std::vector<ThemePackage> packages;
    detail::Json json(bytes);
    json.object([&](const std::string &key) {
        if (key != "packages") { json.skip(); return; }
        json.object([&](const std::string &id) {
            ThemePackage package;
            package.id = id;
            json.object([&](const std::string &field) {
                if (field == "enabled") { package.enabled = json.peek() == 't' || json.peek() == 'f' ? json.boolean() : (json.skip(), false); return; }
                if (field != "current") { json.skip(); return; }
                json.object([&](const std::string &part) {
                    if (part == "manifest") {
                        json.object([&](const std::string &name) {
                            if ((name == "name" || name == "version") && json.peek() == '"') (name == "name" ? package.name : package.version) = json.text();
                            else json.skip();
                        });
                    } else if (part == "entries") {
                        json.array([&] {
                            std::vector<std::string> fields;
                            int weight = 0;
                            json.array([&] {
                                if (json.peek() == '"') fields.push_back(json.text());
                                else { weight = static_cast<int>(json.numberValue()); fields.emplace_back(); }
                            });
                            if (fields.size() >= 3) {
                                fields.resize(6);
                                package.entries.push_back({fields[0], fields[1], weight, fields[3], fields[4], fields[5], true});
                            }
                        });
                    } else json.skip();
                });
            });
            packages.push_back(std::move(package));
        });
    });
    if (!json.done()) throw std::runtime_error("invalid theme state JSON");
    return packages;
}

class Explainer {
public:
    using LexiconPtr = std::unique_ptr<MPNLexicon, decltype(&mpn_lexicon_destroy)>;
    using SessionPtr = std::unique_ptr<MPNSession, decltype(&mpn_session_destroy)>;
    using PersonalPtr = std::unique_ptr<MPNPersonal, decltype(&mpn_personal_destroy)>;

    explicit Explainer(Options options)
        : options_(std::move(options)),
          phrases_(options_.local / "selection-memory.tsv"),
          learned_(options_.local / "selection-memory-words.tsv"),
          user_(options_.local / "selection-memory-user.tsv") { load(); }

    // Where the data came from, for the report header.
    std::string sources() const {
        std::string out = "数据来源（只读）：\n";
        out += "  基础词库：" + baseNote_ + "\n";
        out += "  语言证据：" + std::string(evidenceReady() ? "已加载" : "不可用") + "；日语词表 " + std::to_string(baseJapanese_) +
               " 条（含 F6 词形触发 " + std::to_string(conjugationTriggers_) + " 条）\n";
        out += "  日语自定义词表 japanese-custom.tsv：" + customNote_ + "\n";
        out += "  主题：" + themeNote_ + "\n";
        for (const auto &package : packages_)
            out += "    " + package.id + "「" + package.name + "」" + package.version + "：" + (package.enabled ? "启用" : "未启用") +
                   "，" + std::to_string(package.entries.size()) + " 条\n";
        out += "  选词记忆（CN-06a/b）：" + std::to_string(phrases_.model().size()) + " 条；个人词频（CN-06c）：" +
               std::to_string(learned_.model().size()) + " 条；开关" + (options_.memory ? "开" : "关（不参与排序）") + "\n";
        out += "  个人词（DICT-02b）：" + std::to_string(user_.model().size()) + " 个（不受选词记忆开关影响）\n";
        out += std::string("  设置：") + (options_.simplified ? "简体" : "繁体") + "；模糊音 " +
               (options_.fuzzy ? "0x" + hex(options_.fuzzy) : std::string("关")) + "；纠错 " + (options_.typo ? "开" : "关") + "\n";
        return out;
    }

    // The candidate list for input, each with its sources, score parts and
    // the effect of personal data, followed by conflicts.
    std::string explain(const std::string &typed) {
        std::string input = typed, number;
        // CN-07 number mode: v then digits, as the host keeps them.
        if (input.size() > 1 && input[0] == 'v' && std::all_of(input.begin() + 1, input.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) {
            number = input.substr(1); input = "v";
        }
        if (input.empty() || input.size() > mpn_max_input_bytes() || input.front() == '\'' ||
            !std::all_of(input.begin(), input.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '\''; }))
            return "无法诊断：拼音只能是 a-z 与 '（数字模式写成 v123）。\n";
        const auto now = moment();
        std::string out = "【候选】" + typed + "（" + (options_.simplified ? "简体" : "繁体") + "）\n";
        if (!number.empty()) {
            const auto quick = quickCandidates("v", number, now);
            for (size_t i = 0; i < quick.size(); ++i)
                out += " " + std::to_string(i + 1) + ". " + quick[i].text + "\n    来源：CN-07 数字模式生成（" + quick[i].annotation + "），不学习、不查词库\n";
            return out;
        }
        if (!compose(session_.get(), input) || !compose(baseline_.get(), input)) return out + "无法诊断：核心不接受这串按键。\n";
        const auto lookup = [&](const std::string &reading) { return evidence().dictionary(reading, options_.simplified); };
        auto merged = windowsCandidates(session_.get(), words_, lookup, false);
        auto plain = windowsCandidates(baseline_.get(), words_, lookup, false);
        if (options_.expand) { expand(session_.get(), merged, lookup); expand(baseline_.get(), plain, lookup); }
        const auto beforeMemory = merged;
        const auto remembered = options_.memory ? phrases_.model().recall(input, options_.simplified) : std::vector<std::string>{};
        if (options_.memory) {
            selection_memory::apply(merged, input, remembered, [&](const WindowsCandidate &c) {
                return c.coreIndex ? mpn_candidate_consumed(session_.get(), *c.coreIndex) : input.size();
            });
        }
        mergeQuickCandidates(merged, quickCandidates(input, {}, now));
        mergeQuickCandidates(plain, quickCandidates(input, {}, now));
        const auto evidenceHere = lookup(input);
        const auto counts = phraseCounts();
        std::vector<std::vector<std::string>> sourceList(merged.size());
        std::vector<std::string> conflicts, notes;
        const size_t shown = options_.limit ? std::min(options_.limit, merged.size()) : merged.size();
        for (size_t rank = 0; rank < merged.size(); ++rank) {
            const auto &c = merged[rank];
            const auto core = coreMatch(session_.get(), c, input);
            const size_t consumed = c.coreIndex ? mpn_candidate_consumed(session_.get(), *c.coreIndex) : input.size();
            auto &sources = sourceList[rank];
            std::string detailText;
            if (c.generated) sources.push_back("CN-07 生成（" + c.annotation + "）");
            if (core) {
                MPNCandidateInfo info{};
                mpn_candidate_info(session_.get(), *core, &info, sizeof(info));
                sources.push_back("基础词库·" + kindName(info.kind) + flags(session_.get(), *core));
                detailText += "    核心：生成分 " + (info.score_kind == MPN_SCORE_LEXICON ? std::to_string(static_cast<long long>(info.score))
                              : detail::number(info.score, false)) + "（" + scoreName(info.score_kind) + "）；消耗 " +
                              input.substr(0, static_cast<size_t>(info.consumed_bytes)) +
                              (info.consumed_bytes < input.size() ? "，其余 " + input.substr(static_cast<size_t>(info.consumed_bytes)) + " 继续组字" : std::string()) + "\n";
                detailText += composition(session_.get(), *core, info);
            }
            if (!c.japanese && consumed == input.size()) {
                if (const auto found = evidenceHere.find(c.text); found != evidenceHere.end())
                    sources.push_back("语言证据 " + std::to_string(found->second) + (found->second >= 2000 ? "（强）" : ""));
            }
            for (const auto &theme : themeNames(input, c.text))
                sources.push_back("主题中文名：" + theme);
            if (c.japanese) {
                const auto origins = japaneseOrigins(input, c);
                if (origins.empty()) sources.push_back("中日混合组句（日语词与两侧中文证据）");
                for (const auto &origin : origins) sources.push_back(origin);
                if (origins.size() > 1) conflicts.push_back("「" + c.text + "」（第 " + std::to_string(rank + 1) + " 项）有 " +
                    std::to_string(origins.size()) + " 处定义：" + join(origins, "；") + "；按权重取第一处的注释");
                if (conjugationFor(conjugations(), c, input)) sources.push_back("可按 F6 变形（词形表）");
            }
            if (!core && !c.japanese && !c.generated && c.text == input) sources.push_back("原样字母");
            if (const auto found = counts.find(c.text); found != counts.end() && consumed == input.size())
                sources.push_back("选词记忆：选过 " + found->second + " 次");
            if (core && mpn_candidate_personal(session_.get(), *core)) sources.push_back("个人词（DICT-02b）");
            if (sources.empty()) sources.push_back("未识别");
            // Personal data: where the list without it had this candidate.
            const auto before = position(plain, c);
            std::string effect;
            if (!before) effect = "无个人数据时不在列表中";
            else if (*before != rank) effect = "无个人数据时第 " + std::to_string(*before + 1) + " 项";
            else effect = "无个人数据时同为第 " + std::to_string(rank + 1) + " 项";
            const auto unmoved = position(beforeMemory, c);
            if (options_.memory && std::find(remembered.begin(), remembered.end(), c.text) != remembered.end()) {
                if (!unmoved) effect += "；选词记忆把它作为整串候选插入（候选中原本没有）";
                else if (*unmoved != rank) effect += "；选词记忆把它从第 " + std::to_string(*unmoved + 1) + " 项提到第 " + std::to_string(rank + 1) + " 项";
            }
            if (rank >= shown) continue;
            out += " " + std::to_string(rank + 1) + ". " + c.text + (c.annotation.empty() ? std::string() : "  〔" + c.annotation + "〕") + "\n";
            out += "    来源：" + join(sources, "；") + "\n" + detailText + "    个人数据：" + effect + "\n";
        }
        if (shown < merged.size()) out += " ……另有 " + std::to_string(merged.size() - shown) + " 项（--all 显示全部）\n";
        // Conflicts across the whole list, not only the shown part.
        std::map<std::string, std::vector<size_t>> byText;
        for (size_t i = 0; i < merged.size(); ++i) byText[merged[i].text].push_back(i);
        for (const auto &[text, ranks] : byText) {
            if (ranks.size() < 2) continue;
            std::string where;
            for (auto r : ranks) where += (where.empty() ? "" : "、") + std::string("第 ") + std::to_string(r + 1) + " 项（" + sourceList[r].front() + "）";
            conflicts.push_back("同一文字「" + text + "」出现 " + std::to_string(ranks.size()) + " 次：" + where);
        }
        // A theme or custom trigger ranked ahead of a whole base word of the same reading.
        std::vector<size_t> exact;
        for (size_t i = 0; i < merged.size(); ++i) {
            const auto core = coreMatch(session_.get(), merged[i], input);
            MPNCandidateInfo info{};
            if (core && mpn_candidate_info(session_.get(), *core, &info, sizeof(info)) &&
                info.kind == MPN_CANDIDATE_EXACT && info.consumed_bytes == input.size()) exact.push_back(i);
        }
        bool shadowed = false;
        for (size_t i = 0; i < merged.size(); ++i) {
            const auto &sources = sourceList[i];
            const bool theme = std::any_of(sources.begin(), sources.end(), [](const std::string &s) {
                return s.find("主题") != std::string::npos || s.rfind("日语自定义", 0) == 0;
            });
            const auto later = std::find_if(exact.begin(), exact.end(), [&](size_t j) { return j > i; });
            if (!theme || later == exact.end() || std::find(exact.begin(), exact.end(), i) != exact.end()) continue;
            shadowed = true;
            conflicts.push_back("主题／自定义触发「" + merged[i].text + "」（第 " + std::to_string(i + 1) + " 项）排在基础词库整词「" +
                                merged[*later].text + "」（第 " + std::to_string(*later + 1) + " 项）之前");
        }
        for (size_t i = 0; i < merged.size(); ++i) {
            const auto core = coreMatch(session_.get(), merged[i], input);
            if (!core || !mpn_candidate_personal(session_.get(), *core)) continue;
            for (const auto &line : detail::lines(mpn_candidate_explain(session_.get(), *core))) {
                const auto fields = detail::split(line, '\t');
                if (fields.size() >= 4 && fields[2] == "lexicon+user")
                    conflicts.push_back("个人词「" + fields[1] + "」（" + fields[0] + "）与基础词库同读音同文字重复（词库权重 " + fields[3] +
                                        "）：个人词只把它提前，不会出现两次");
            }
        }
        for (const auto &package : packages_) {
            if (package.enabled) continue;
            std::vector<std::string> hits;
            for (const auto &word : package.entries)
                if (word.reading == input) hits.push_back(word.text + (word.chinese.empty() ? std::string() : "（中文 " + word.chinese + "）"));
            if (!hits.empty()) conflicts.push_back("未启用的主题 " + package.id + "「" + package.name + "」也有触发词 " + input + "：" +
                                                   join(hits, "、") + "（启用后会出现）");
        }
        for (const auto &text : remembered) {
            if (std::none_of(beforeMemory.begin(), beforeMemory.end(), [&](const WindowsCandidate &c) { return c.text == text; }))
                notes.push_back("选词记忆记录「" + text + "」不在词库候选中，按整串插入（多段组成的词组）");
        }
        if (!options_.memory && (phrases_.model().size() || learned_.model().size()))
            notes.push_back("选词记忆开关已关闭：记录和个人词频都不参与排序（个人词仍生效）");
        if (std::any_of(words_.begin(), words_.end(), [&](const JapaneseWord &w) { return w.theme && w.reading == input; }) && !exact.empty() && !shadowed)
            notes.push_back("读音 " + input + " 也是主题触发词，基础词库整词仍在前");
        out += conflicts.empty() ? "冲突：无\n" : "冲突：\n";
        for (const auto &line : conflicts) out += "  - " + line + "\n";
        for (const auto &line : notes) out += "  提示：" + line + "\n";
        return out;
    }

    // Every place text exists.
    std::string lookup(const std::string &text) {
        std::string out = "【反查】" + text + "\n";
        bool any = false;
        mpn_clear(session_.get());
        std::vector<std::string> readings;
        for (const auto &line : detail::lines(mpn_explain_text(session_.get(), text.c_str()))) {
            const auto f = detail::split(line, '\t');
            if (f[0] == "lexicon" && f.size() == 8) {
                out += "  基础词库：" + f[1] + "  " + f[2] + "／" + f[3] + "  权重 繁 " + f[4] + "／简 " + f[5] + "  先验 " + f[6] +
                       (f[7] == "1" ? "  有搭配统计" : "") + "\n";
                readings.push_back(detail::untoned(f[1]));
                any = true;
            } else if (f[0] == "user" && f.size() == 4) {
                out += "  个人词（已按本词库拆成音节）：" + f[1] + "  " + f[2] + "／" + f[3] + "\n";
                any = true;
            } else if (f[0] == "context" && f.size() == 5) {
                out += "  衔接短语表：" + f[1] + "  权重 繁 " + f[2] + "／简 " + f[3] + (f[4] == "1" ? "  编辑短语" : "") + "\n";
                any = true;
            }
        }
        std::sort(readings.begin(), readings.end());
        readings.erase(std::unique(readings.begin(), readings.end()), readings.end());
        for (const auto &reading : readings) {
            const auto found = evidence().dictionary(reading, options_.simplified);
            if (const auto hit = found.find(text); hit != found.end())
                out += "  语言证据：" + reading + " → " + std::to_string(hit->second) + "\n";
        }
        for (const auto &entry : user_.model().entries()) {
            if (entry.text != text && entry.traditional != text) continue;
            out += "  个人词表 selection-memory-user.tsv：" + entry.reading + "  " + entry.text + (entry.traditional.empty() ? std::string() : "／" + entry.traditional) + "\n";
            any = true;
        }
        for (const auto &line : detail::lines(phrases_.model().snapshot())) {
            const auto f = detail::split(line, '\t');
            if (f.size() == 4 && f[3] == text) {
                out += "  选词记忆：" + f[2] + "（" + (f[1] == "s" ? "简体" : "繁体") + "）选过 " + f[0] + " 次\n";
                any = true;
            }
        }
        learned_.model().each(selection_memory::currentHour(), [&](bool simplified, std::string_view reading, const std::string &word, double bonus) {
            if (word != text) return;
            out += "  个人词频：" + std::string(reading) + "（" + (simplified ? "简体" : "繁体") + "）加分 " + detail::number(bonus) + "\n";
            any = true;
        });
        for (const auto &tagged : tagged_) {
            const auto &w = tagged.word;
            if (w.text != text && w.kana != text && w.chinese != text) continue;
            out += "  " + tagged.origin + "：触发 " + w.reading + "（" + sourceName(w.source) + "）→ " + w.text +
                   (w.kana.empty() ? std::string() : "  假名 " + w.kana) + (w.chinese.empty() ? std::string() : "  中文 " + w.chinese) +
                   "  权重 " + std::to_string(w.weight) + "\n";
            any = true;
        }
        for (const auto &package : packages_) {
            for (const auto &w : package.entries) {
                if (w.text != text && w.kana != text && w.chinese != text) continue;
                out += "  主题 " + package.id + "（" + (package.enabled ? "启用" : "未启用") + "）：触发 " + w.reading + "（" + sourceName(w.source) +
                       "）→ " + w.text + (w.chinese.empty() ? std::string() : "  中文 " + w.chinese) + "  权重 " + std::to_string(w.weight) + "\n";
                any = true;
            }
        }
        if (!any) out += "  没有找到（基础词库、个人词、选词记忆、日语词表和主题都没有这个文字）\n";
        return out;
    }

    // Whole-table checks that do not depend on one input.
    std::string audit() {
        std::string out = "【检查】\n";
        size_t found = 0;
        mpn_clear(session_.get());
        for (const auto &entry : user_.model().entries()) {
            const auto key = selection_memory::UserWordModel::key(entry.reading);
            bool prepared = false, duplicate = false;
            std::string weight;
            for (const auto &line : detail::lines(mpn_explain_text(session_.get(), entry.text.c_str()))) {
                const auto f = detail::split(line, '\t');
                if (f[0] == "user" && f.size() == 4 && detail::untoned(f[1]) == key && f[3] == entry.text) prepared = true;
                if (f[0] == "lexicon" && f.size() == 8 && detail::untoned(f[1]) == key && f[3] == entry.text) { duplicate = true; weight = f[5]; }
            }
            if (!personal_) prepared = false;
            if (!prepared) { out += "  - 个人词「" + entry.text + "」（" + entry.reading + "）的读音拆不成本词库的音节，被忽略\n"; ++found; }
            if (duplicate) { out += "  - 个人词「" + entry.text + "」（" + entry.reading + "）与基础词库重复（简体权重 " + weight + "）：只是提前\n"; ++found; }
        }
        std::map<std::string, std::set<std::string>> custom, themes;
        for (const auto &tagged : tagged_) if (tagged.custom) custom[tagged.word.reading].insert(tagged.word.text);
        for (const auto &word : words_) if (word.theme && custom.count(word.reading)) themes[word.reading].insert(word.text);
        for (const auto &[reading, texts] : custom) {
            const auto other = themes.find(reading);
            if (other == themes.end()) continue;
            for (const auto &text : texts) {
                if (other->second.size() == 1 && other->second.count(text)) continue;
                out += "  - 触发词 " + reading + "：日语自定义词表「" + text + "」与主题「" + join(std::vector<std::string>(other->second.begin(), other->second.end()), "、") + "」同时匹配\n";
                ++found;
            }
        }
        if (!found) out += "  没有发现问题（个人词读音、个人词与词库重复、自定义词与主题同触发）\n";
        return out;
    }

private:
    struct Tagged { JapaneseWord word; std::string origin; bool custom = false; };
    Options options_;
    LexiconPtr lexicon_{nullptr, &mpn_lexicon_destroy};
    std::unique_ptr<base_dictionary::Snapshot> snapshot_;
    WordEvidence evidence_;
    JapaneseConjugations conjugations_;
    SessionPtr session_{nullptr, &mpn_session_destroy}, baseline_{nullptr, &mpn_session_destroy};
    PersonalPtr personal_{nullptr, &mpn_personal_destroy};
    std::vector<JapaneseWord> words_;
    std::vector<Tagged> tagged_;
    std::vector<ThemePackage> packages_;
    selection_memory::Journal<selection_memory::Model> phrases_;
    selection_memory::Journal<selection_memory::WordModel> learned_;
    selection_memory::UserWordTable user_;
    std::string baseNote_, customNote_ = "无", themeNote_ = "无";
    size_t baseJapanese_ = 0, conjugationTriggers_ = 0;

    static std::string hex(uint32_t value) { char buffer[16]; std::snprintf(buffer, sizeof(buffer), "%x", value); return buffer; }
    static std::string join(const std::vector<std::string> &items, const char *separator) {
        std::string out;
        for (const auto &item : items) out += (out.empty() ? "" : separator) + item;
        return out;
    }
    static quick_input::Moment moment() {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        return {now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond};
    }
    const WordEvidence &evidence() const { return snapshot_ ? snapshot_->evidence() : evidence_; }
    bool evidenceReady() const { return evidence().ready(); }
    const JapaneseConjugations &conjugations() const { return snapshot_ ? snapshot_->conjugations() : conjugations_; }

    void load() {
        std::vector<JapaneseWord> base;
        const auto baseRoot = options_.local / "BaseDictionaries";
        if (options_.baseSnapshot && std::filesystem::exists(baseRoot / "STATE.json")) {
            try {
                snapshot_ = base_dictionary::loadCurrent(baseRoot);
                baseNote_ = "已安装的基础词库快照 " + snapshot_->version() + "（" + detail::display(baseRoot) + "）";
                base = snapshot_->words();
            } catch (const std::exception &error) {
                snapshot_.reset();
                baseNote_ = std::string("基础词库快照无效（") + error.what() + "），改用程序目录；";
            }
        }
        if (!snapshot_) {
            const auto bytes = detail::readAll(options_.program / "native-pinyin.tsv", 512ull * 1024 * 1024);
            lexicon_.reset(mpn_lexicon_create(bytes.data(), bytes.size()));
            if (!lexicon_) throw std::runtime_error("无法读取基础词库：" + detail::display(options_.program / "native-pinyin.tsv"));
            baseNote_ += "程序目录 " + detail::display(options_.program);
            conjugations_.loadFile(options_.program / "japanese-conjugations.bin");
            std::ifstream japanese(options_.program / "japanese.tsv", std::ios::binary);
            base = readJapaneseWords(japanese);
            appendConjugationWords(conjugations_, base);
            evidence_.open(options_.program / "language-evidence.sqlite");
        }
        std::set<std::pair<std::string, std::string>> forms;
        for (const auto &entry : conjugations().entries())
            for (const auto &trigger : entry.triggers) forms.emplace(trigger.second, entry.text);
        for (const auto &word : base) {
            const bool form = forms.count({word.reading, word.text}) && word.weight == 5000 && word.chinese.empty();
            conjugationTriggers_ += form;
            tagged_.push_back({word, form ? "F6 词形表" : "基础日语词表"});
        }
        baseJapanese_ = base.size();
        words_ = base;
        if (auto custom = readPersonalDictionary(options_.local / "japanese-custom.tsv")) {
            if (custom->present) customNote_ = std::to_string(custom->words.size()) + " 条";
            for (const auto &word : custom->words) tagged_.push_back({word, "日语自定义词表", true});
            words_.insert(words_.end(), custom->words.begin(), custom->words.end());
        } else customNote_ = "格式无效，输入法也会忽略";
        const auto themeRoot = options_.local / "Dictionaries";
        if (auto snapshot = readJapanesePackageSnapshot(themeRoot)) {
            themeNote_ = "快照 " + snapshot->generation.substr(0, 8) + "，" + std::to_string(snapshot->words.size()) + " 条启用";
            words_.insert(words_.end(), snapshot->words.begin(), snapshot->words.end());
            const auto state = detail::readAll(themeRoot / snapshot->generation / "state.json", 256ull * 1024 * 1024);
            try { if (!state.empty()) packages_ = readThemeState(state); }
            catch (const std::exception &) { themeNote_ += "（state.json 无法解析，无法区分主题包）"; }
        } else if (std::filesystem::exists(themeRoot / "CURRENT")) themeNote_ = "快照无效，输入法也会忽略";
        std::stable_sort(words_.begin(), words_.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });

        phrases_.refresh();
        learned_.refresh();
        user_.refresh();
        // As TextService::attachPersonal: user words always, learned bonuses
        // only while selection memory is on; nothing attached otherwise.
        if (options_.memory || !user_.model().empty()) {
            personal_.reset(mpn_personal_create());
            if (!personal_) throw std::runtime_error("内存不足");
            for (const auto &entry : user_.model().entries())
                mpn_personal_add_word(personal_.get(), entry.reading.c_str(), entry.text.c_str(), entry.traditional.c_str());
            if (options_.memory)
                learned_.model().each(selection_memory::currentHour(), [&](bool script, std::string_view reading, const std::string &text, double bonus) {
                    mpn_personal_set(personal_.get(), script, std::string(reading).c_str(), text.c_str(), bonus);
                });
        }
        session_ = newSession();
        baseline_ = newSession();
        if (personal_) mpn_set_personal(session_.get(), personal_.get());
    }

    SessionPtr newSession() const {
        SessionPtr session = snapshot_ ? SessionPtr(snapshot_->session(options_.simplified).release(), &mpn_session_destroy)
                                       : SessionPtr(mpn_session_create(lexicon_.get()), &mpn_session_destroy);
        if (!session) throw std::runtime_error("无法创建会话");
        mpn_set_simplified(session.get(), options_.simplified);
        mpn_set_fuzzy_pinyin(session.get(), options_.fuzzy & MPN_FUZZY_ALL);
        mpn_set_typo_correction(session.get(), options_.typo);
        return session;
    }

    bool compose(MPNSession *session, const std::string &input) const {
        mpn_clear(session); mpn_clear_commit(session);
        mpn_set_segment_context(session, options_.context.c_str());
        for (char c : input) if (!mpn_key(session, c)) return false;
        return mpn_input(session) == input;
    }

    template<class Lookup> void expand(MPNSession *session, std::vector<WindowsCandidate> &choices, Lookup lookup) const {
        while (mpn_candidates_can_expand(session)) {
            mpn_expand_candidates(session);
            appendCandidateExpansion(session, choices, windowsCandidates(session, words_, lookup, false));
        }
    }

    // The core candidate behind c: its own index, or a core candidate of the
    // same text over the whole input that the merge layer folded into it.
    static std::optional<size_t> coreMatch(MPNSession *session, const WindowsCandidate &c, const std::string &input) {
        if (c.coreIndex) return c.coreIndex;
        if (c.japanese || c.generated) return {};
        for (size_t i = 0; i < mpn_candidate_count(session); ++i)
            if (c.text == mpn_candidate(session, i) && mpn_candidate_consumed(session, i) == input.size()) return i;
        return {};
    }
    static std::optional<size_t> position(const std::vector<WindowsCandidate> &list, const WindowsCandidate &c) {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].text == c.text && list[i].annotation == c.annotation && list[i].japanese == c.japanese) return i;
        return {};
    }

    static std::string kindName(uint32_t kind) {
        switch (kind) {
        case MPN_CANDIDATE_EXACT: return "整词（精确读音）";
        case MPN_CANDIDATE_PATH: return "组句";
        case MPN_CANDIDATE_PARTIAL: return "前段";
        case MPN_CANDIDATE_CORRECTION: return "非精确读音";
        default: return "未分类读音";
        }
    }
    static std::string scoreName(uint32_t kind) {
        switch (kind) {
        case MPN_SCORE_LEXICON: return "词库权重";
        case MPN_SCORE_PATH: return "组句得分，奈特";
        case MPN_SCORE_FALLBACK: return "兜底得分";
        default: return "无";
        }
    }
    static std::string sourceName(const std::string &source) {
        return source == "english" ? "英文触发" : source == "pinyin" ? "拼音触发" : source.empty() ? "罗马字" : source;
    }
    static std::string flags(MPNSession *session, size_t index) {
        std::string out;
        if (mpn_candidate_fuzzy(session, index)) out += "·模糊音（CN-03）";
        if (const auto n = mpn_candidate_abbreviated(session, index)) out += "·简拼 " + std::to_string(n) + " 个音节（CN-04）";
        if (mpn_candidate_typo(session, index)) out += "·纠错（CN-05）";
        return out;
    }

    // The core's word-by-word score of one candidate.
    std::string composition(MPNSession *session, size_t index, const MPNCandidateInfo &info) const {
        const auto rows = detail::lines(mpn_candidate_explain(session, index));
        if (rows.empty()) return "    组成：无法逐词拆分（模糊音、纠错、简拼或兜底读音不按词计分）\n";
        std::string out;
        double total = 0;
        for (const auto &row : rows) {
            const auto f = detail::split(row, '\t');
            if (f.size() != 9) continue;
            const double base = std::atof(f[5].c_str()), bonus = std::atof(f[6].c_str()), lift = std::atof(f[7].c_str()), join = std::atof(f[8].c_str());
            total += base + lift + join;
            const std::string source = f[2] == "user" ? "个人词" : f[2] == "lexicon+user" ? "词库+个人词" : "词库";
            out += "    组成：" + f[0] + " " + f[1] + "（" + source + "）权重 " + f[3] + " · 先验 " + detail::number(std::atof(f[4].c_str())) +
                   " · 基础 " + detail::number(base) +
                   (bonus > 0 ? std::string(" · ") + (f[2] == "lexicon" ? "个人词频" : "个人词") + "加分 " + detail::number(bonus) + "（实得 " + detail::number(lift) + "）" : std::string()) +
                   " · 衔接 " + detail::number(join) + "\n";
        }
        if (rows.size() > 1 || info.score_kind == MPN_SCORE_PATH)
            out += "    合计 " + detail::number(total) + (info.score_kind == MPN_SCORE_PATH ? "（生成分 " + detail::number(info.score) + "）" : std::string()) + "\n";
        return out;
    }

    // Selection counts of the remembered texts of the current reading.
    std::map<std::string, std::string> phraseCounts() const {
        std::map<std::string, std::string> result;
        if (!options_.memory) return result;
        const std::string input = mpn_input(session_.get());
        const std::string script = options_.simplified ? "s" : "t";
        for (const auto &line : detail::lines(phrases_.model().snapshot())) {
            const auto f = detail::split(line, '\t');
            if (f.size() == 4 && f[1] == script && f[2] == input) result[f[3]] = f[0];
        }
        return result;
    }

    // Theme Chinese names for this reading equal to text, with their package.
    std::vector<std::string> themeNames(const std::string &input, const std::string &text) {
        std::vector<std::string> result;
        for (const auto &word : words_) {
            if (!word.theme || word.reading != input || word.chinese.empty()) continue;
            const std::string name = mpn_custom_chinese(session_.get(), word.chinese.c_str());
            if (name != text) continue;
            result.push_back(packageOf(word) + "（" + word.text + "）");
        }
        return result;
    }
    std::string packageOf(const JapaneseWord &word) const {
        std::string ids;
        for (const auto &package : packages_) {
            if (!package.enabled) continue;
            if (std::any_of(package.entries.begin(), package.entries.end(), [&](const JapaneseWord &e) {
                return e.reading == word.reading && e.text == word.text && e.chinese == word.chinese;
            })) ids += (ids.empty() ? "" : "、") + package.id;
        }
        return "主题包 " + (ids.empty() ? std::string("（快照）") : ids);
    }
    // Every Japanese definition that produces this candidate for input.
    std::vector<std::string> japaneseOrigins(const std::string &input, const WindowsCandidate &c) const {
        std::vector<std::string> result;
        const std::string marker = " \xc2\xb7 \xe5\x81\x87\xe5\x90\x8d"; // " · 假名", as appendWord writes it
        const bool kana = c.annotation.size() > marker.size() && c.annotation.compare(c.annotation.size() - marker.size(), marker.size(), marker) == 0;
        const auto describe = [&](const JapaneseWord &w, const std::string &origin) {
            return origin + "：" + sourceName(w.source) + " " + w.reading + " → " + w.text + "，权重 " + std::to_string(w.weight);
        };
        for (const auto &tagged : tagged_) {
            const auto &w = tagged.word;
            if (w.reading == input && (w.text == c.text || (kana && w.kana == c.text))) result.push_back(describe(w, tagged.origin));
        }
        for (const auto &w : words_) {
            if (!w.theme || w.reading != input || !(w.text == c.text || (kana && w.kana == c.text))) continue;
            result.push_back(describe(w, packageOf(w)));
        }
        return result;
    }
};

} // namespace candidate_explain
