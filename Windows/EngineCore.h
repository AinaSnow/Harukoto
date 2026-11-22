#pragma once
#include "EngineServer.h"
#include "../Core/MeowpuutCore.h"
#include <fstream>
#include <filesystem>

namespace harukoto::engine {
// Shared immutable lexicon; each query owns a fresh mutable session. This is
// a correctness prototype, not the full Windows candidate merge pipeline.
class CoreDictionary {
    std::shared_ptr<MPNLexicon> lexicon_;
public:
    static constexpr size_t PageSize = 16;
    explicit CoreDictionary(const std::string &bytes) {
        const std::string magic = "MEOWPUUT-PINYIN-2";
        if (bytes.compare(0,magic.size(),magic) != 0 || bytes.size() <= magic.size() ||
            (bytes[magic.size()] != char(10) && bytes[magic.size()] != char(13)))
            throw std::invalid_argument("expected native pinyin dictionary");
        lexicon_ = {mpn_lexicon_create(bytes.data(),bytes.size()), mpn_lexicon_destroy};
        if (!lexicon_) throw std::runtime_error("cannot load native pinyin dictionary");
    }
    static CoreDictionary load(const std::filesystem::path &path) {
        std::ifstream file(path,std::ios::binary);
        if (!file) throw std::runtime_error("cannot open native pinyin dictionary");
        const std::string bytes{std::istreambuf_iterator<char>(file),{}};
        if (file.bad()) throw std::runtime_error("cannot read native pinyin dictionary");
        return CoreDictionary(bytes);
    }
    CandidatePage query(const ServerConnection::Work &work) const {
        if (!inputValid(work.input) || work.input.size() > mpn_max_input_bytes() || work.page > 4095 ||
            work.context.size() > 64 || !textValid(work.context) || !valid(work.key))
            throw std::invalid_argument("unsupported core query");
        std::unique_ptr<MPNSession,decltype(&mpn_session_destroy)> session(mpn_session_create(lexicon_.get()),mpn_session_destroy);
        if (!session) throw std::runtime_error("cannot create core session");
        mpn_set_simplified(session.get(),work.key.simplified);
        mpn_set_segment_context(session.get(),work.context.c_str());
        if (!mpn_set_input(session.get(),work.input.c_str()) || *mpn_committed(session.get()))
            throw std::invalid_argument("core rejected input or produced implicit commit");
        if (work.input != mpn_input(session.get())) throw std::runtime_error("core input mismatch");
        // Exhaust the existing three bounded expansion passes before slicing,
        // preserving the reconstructed index order across pages.
        for (int i=0;i<3 && mpn_candidates_can_expand(session.get());++i) mpn_expand_candidates(session.get());
        CandidatePage result{work.input,false,{}};
        std::vector<Candidate> words;
        for (size_t index=0;index<mpn_candidate_count(session.get());++index) {
            std::string text = mpn_candidate(session.get(),index);
            const auto tail = mpn_candidate_literal_tail(session.get(),index);
            if (tail > text.size()) throw std::runtime_error("invalid core literal tail");
            text.resize(text.size()-tail);
            const auto consumed = mpn_candidate_consumed(session.get(),index);
            if (text.empty() || !consumed) continue;
            if (consumed > work.input.size() || !textValid(text)) throw std::runtime_error("invalid core candidate");
            words.push_back({index+1,static_cast<uint32_t>(consumed),std::move(text),"",static_cast<uint32_t>(index+1),0});
        }
        const size_t start = size_t(work.page)*PageSize;
        if (start < words.size()) {
            const auto end = std::min(words.size(),start+PageSize);
            result.words.assign(words.begin()+start,words.begin()+end);
            result.more = end < words.size() && work.page < 4095;
        }
        encodePayload(result);
        return result;
    }
};
} // namespace harukoto::engine
