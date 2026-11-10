#pragma once
#include "BaseDictionaryFiles.h"
#include "BaseDictionaryMetadata.h"
#include "ConjugationPanel.h"
#include "JapanesePackages.h"
#include "WordEvidence.h"
#include <memory>
#include <regex>

namespace base_dictionary {
inline constexpr const char *files[] = {
    "native-pinyin.tsv", "language-evidence.sqlite", "japanese.tsv", "japanese-conjugations.bin"
};
using Lexicon = std::unique_ptr<MPNLexicon, decltype(&mpn_lexicon_destroy)>;
using Session = std::unique_ptr<MPNSession, decltype(&mpn_session_destroy)>;

inline std::vector<JapaneseWord> japanese(const std::string &bytes) {
    std::vector<JapaneseWord> result;
    std::istringstream lines(bytes);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const auto separators = std::count(line.begin(), line.end(), '|');
        if (line.size() > 4096 || (separators != 2 && separators != 5))
            throw std::runtime_error("invalid base Japanese row");
        const auto end = line.find('|');
        const auto aliases = 1 + std::count(line.begin(), line.begin() + end, ',');
        std::istringstream row(line);
        auto words = readJapaneseWords(row);
        if (words.size() != static_cast<size_t>(aliases)) throw std::runtime_error("invalid base Japanese alias");
        for (const auto &word : words) {
            if (!validPackageUnicode(word.text) || !validPackageUnicode(word.kana, true, true) ||
                !validPackageUnicode(word.chinese, false, true) || (separators == 5 && word.source.empty()))
                throw std::runtime_error("invalid base Japanese text");
        }
        result.insert(result.end(), std::make_move_iterator(words.begin()), std::make_move_iterator(words.end()));
        if (result.size() > 500000) throw std::runtime_error("too many base Japanese aliases");
    }
    if (result.empty()) throw std::runtime_error("empty base Japanese dictionary");
    return result;
}

// One prepared generation. Not shared for concurrent queries: SQLite statements
// and sessions belong to one consumer thread after a synchronized handoff.
// Declaration order keeps all file guards alive until SQLite has closed.
class Snapshot {
    std::vector<std::unique_ptr<StableFile>> guards;
    Lexicon lexicon{nullptr, &mpn_lexicon_destroy};
    WordEvidence evidence_;
    JapaneseConjugations conjugations_;
    std::vector<JapaneseWord> words_;
    std::string generation_, version_;
    Snapshot() = default;
public:
    Snapshot(const Snapshot &) = delete;
    Snapshot &operator=(const Snapshot &) = delete;
    const std::string &generation() const { return generation_; }
    const std::string &version() const { return version_; }
    const std::vector<JapaneseWord> &words() const { return words_; }
    const JapaneseConjugations &conjugations() const { return conjugations_; }
    const WordEvidence &evidence() const { return evidence_; }
    Session session(bool simplified) const {
        Session result(mpn_session_create(lexicon.get()), &mpn_session_destroy);
        if (!result) throw std::runtime_error("base session allocation failed");
        mpn_set_simplified(result.get(), simplified);
        return result;
    }
    static std::unique_ptr<Snapshot> load(const std::filesystem::path &folder, const std::string &expected = {}) {
        auto result = std::unique_ptr<Snapshot>(new Snapshot);
        result->guards.push_back(std::make_unique<StableFile>(folder / "base-dictionary.json", 65536));
        const auto manifest = MetadataParser(result->guards.back()->read()).parse();
        manifest.keys({"format", "engine_abi", "version", "files"});
        if (manifest.at("format").count() != 1 || manifest.at("engine_abi").count() != 1)
            throw std::runtime_error("unsupported base dictionary format or ABI");
        result->version_ = manifest.at("version").text();
        static const std::regex version(
            R"((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-(0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?)");
        if (result->version_.size() > 128 || !std::regex_match(result->version_, version))
            throw std::runtime_error("invalid base dictionary version");
        result->generation_ = sha256(manifest.canonical());
        if (!expected.empty() && result->generation_ != expected) throw std::runtime_error("base generation mismatch");
        const auto &members = manifest.at("files");
        members.keys({files[0], files[1], files[2], files[3]});
        // Lock all members before validating any of them, and keep those handles.
        for (const auto *name : files) {
            const auto &record = members.at(name);
            record.keys({"size", "sha256"});
            if (!base_dictionary::generation(record.at("sha256").text())) throw std::runtime_error("invalid file digest");
            auto file = std::make_unique<StableFile>(folder / name,
                name == files[3] ? JapaneseConjugations::maxBytes : 512ull * 1024 * 1024);
            if (file->size() != record.at("size").count()) throw std::runtime_error("base file size mismatch");
            result->guards.push_back(std::move(file));
        }
        for (size_t i = 0; i < 4; ++i) {
            auto &file = *result->guards[i + 1];
            if (i == 1) {
                const auto header = file.prefix(20);
                if (header.compare(0, 16, std::string("SQLite format 3\0", 16)) != 0 || header[18] != 1 || header[19] != 1)
                    throw std::runtime_error("SQLite must use standalone rollback-journal format, not WAL");
            }
            // Hash SQLite as a stream; the connection reads the guarded file.
            auto bytes = file.read(i == 1);
            if ((i == 1 ? bytes : sha256(bytes)) != members.at(files[i]).at("sha256").text())
                throw std::runtime_error(std::string("base file hash mismatch: ") + files[i]);
            if (i == 0) {
                const std::string magic = "MEOWPUUT-PINYIN-2\n";
                if (bytes.compare(0, magic.size(), magic) != 0) throw std::runtime_error("unsupported pinyin header");
                result->lexicon.reset(mpn_lexicon_create(bytes.data(), bytes.size()));
                if (!result->lexicon) throw std::runtime_error("invalid native pinyin dictionary");
            } else if (i == 2) result->words_ = japanese(bytes);
            else if (i == 3 && !result->conjugations_.load(bytes)) throw std::runtime_error("invalid conjugation dictionary");
        }
        // Frozen snapshots must be checkpointed standalone SQLite databases.
        // Sidecars can otherwise change what SQLite sees without changing the
        // main file's manifest hash. Never perform journal recovery here.
        const auto database = folder / files[1];
        for (const auto *suffix : {"-wal", "-shm", "-journal"})
            if (std::filesystem::exists(std::filesystem::path(database.native() + std::filesystem::path(suffix).native())))
                throw std::runtime_error("SQLite sidecar in base snapshot");
        result->evidence_.open(database);
        if (!result->evidence_.integrityCheck()) throw std::runtime_error("base SQLite schema or integrity check failed");
        appendConjugationWords(result->conjugations_, result->words_);
        std::stable_sort(result->words_.begin(), result->words_.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
        return result;
    }
};

inline std::string currentGeneration(const std::filesystem::path &root) {
    StableFile pointer(root / "STATE.json", 65536);
    const auto state = MetadataParser(pointer.read()).parse();
    state.keys({"format", "current", "previous"});
    if (state.at("format").count() != 1) throw std::runtime_error("invalid base state format");
    const auto current = state.at("current").text();
    if (!generation(current) || (state.at("previous").kind != Metadata::null && !generation(state.at("previous").text())))
        throw std::runtime_error("invalid base state generation");
    return current;
}
inline std::unique_ptr<Snapshot> loadCurrent(const std::filesystem::path &root) {
    const auto current = currentGeneration(root);
    auto result = Snapshot::load(root / current, current);
    if (currentGeneration(root) != current) throw std::runtime_error("base state changed during preparation; retry");
    return result;
}
} // namespace base_dictionary
