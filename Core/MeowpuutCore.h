#ifndef MEOWPUUT_CORE_H
#define MEOWPUUT_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MPNLexicon MPNLexicon;
typedef struct MPNSession MPNSession;

// The loader copies the UTF-8 dictionary. Sessions retain it independently.
MPNLexicon *mpn_lexicon_create(const char *data, size_t size);
void mpn_lexicon_destroy(MPNLexicon *lexicon);
MPNSession *mpn_session_create(const MPNLexicon *lexicon);
void mpn_session_destroy(MPNSession *session);

// Printable ASCII keys and the same X11 editing keys as the Rime adapter.
// A session is used on one thread. Copy returned strings before the next mutation.
// At the limit, the next valid key preserves the old preedit as a commit and
// begins a new composition; boundary tone marks complete the old reading.
size_t mpn_max_input_bytes(void);
int mpn_key(MPNSession *session, int32_t key);
// When enabled, the first candidate always covers the complete composition.
void mpn_require_full_candidate(MPNSession *session);
void mpn_set_simplified(MPNSession *session, int simplified);
// Render a simplified custom name in the session's output script without changing composition.
// The returned buffer remains valid until the next call to this function on the session.
const char *mpn_custom_chinese(MPNSession *session, const char *text);
void mpn_clear(MPNSession *session);
// Cancel only unselected composition; keep text already chosen by the user
// until the host acknowledges its delivery.
void mpn_cancel_composition(MPNSession *session);
// Set adjacent Chinese ranking context before a new composition, or restore
// its selected prefix while rebuilding the same partial composition. Hosts
// must replace it at a document boundary. NULL/empty clears it; the getter
// points into the session until its next mutation.
void mpn_set_segment_context(MPNSession *session, const char *text);
const char *mpn_segment_context(const MPNSession *session);
void mpn_commit(MPNSession *session);
int mpn_choose(MPNSession *session, size_t index);
const char *mpn_input(const MPNSession *session);
const char *mpn_preedit(const MPNSession *session);
// Selections accumulate until the host consumes them with mpn_clear_commit.
const char *mpn_committed(const MPNSession *session);
void mpn_clear_commit(MPNSession *session);
size_t mpn_candidate_count(const MPNSession *session);
// Lazy expansion appends candidates without invalidating existing indices.
// The first expansion begins with every fully spelled word and character
// whose reading leads the input (longest reading first), so a text stays
// reachable segment by segment; a list longer than one page already shows
// the commonest of them on its second page. Exact dictionary matches are
// exhausted; sentence search remains bounded.
int mpn_candidates_can_expand(const MPNSession *session);
size_t mpn_expand_candidates(MPNSession *session);
const char *mpn_candidate(const MPNSession *session, size_t index);
// Raw input bytes consumed when selecting this candidate; 0 for invalid input.
size_t mpn_candidate_consumed(const MPNSession *session, size_t index);
// Display-only literal suffix bytes, excluded by mpn_choose from committed text.
size_t mpn_candidate_literal_tail(const MPNSession *session, size_t index);
// Provenance describes how a candidate was generated, not its confidence.
// A fully spelled path is an exact reading, but may not be a plausible phrase.
typedef enum {
    MPN_CANDIDATE_UNKNOWN = 0,
    MPN_CANDIDATE_EXACT = 1,
    MPN_CANDIDATE_PATH = 2,
    MPN_CANDIDATE_PARTIAL = 3,
    MPN_CANDIDATE_CORRECTION = 4
} MPNCandidateKind;
typedef enum {
    MPN_SCORE_NONE = 0,
    MPN_SCORE_LEXICON = 1,
    MPN_SCORE_PATH = 2,
    MPN_SCORE_FALLBACK = 3
} MPNCandidateScoreKind;
typedef struct {
    uint32_t kind;
    uint32_t score_kind;
    uint64_t consumed_bytes;
    // First raw ASCII input byte retained after selection. This may be a
    // valid next word as well as an unfinished or unrecognised suffix.
    uint64_t remaining_offset_bytes;
    // Generation score before session-context reordering. Interpret only
    // within score_kind; it is not a probability or a calibrated confidence.
    double score;
} MPNCandidateInfo;
// Writes the fields fitting in info_size; returns 0 for invalid arguments.
// The info and string describe the same candidate until the next mutation.
int mpn_candidate_info(const MPNSession *session, size_t index, MPNCandidateInfo *info, size_t info_size);
size_t mpn_page(const MPNSession *session);
size_t mpn_sentence_count(const MPNSession *session);
size_t mpn_literal_tail(const MPNSession *session);
// Convert a separate Pinyin segment without changing the active composition.
const char *mpn_exact_pinyin(MPNSession *session, const char *reading);

// Optional personal word preferences (CN-06c), learned by the host from
// confirmed selections. A bonus is in nats on the scale of word scores and is
// clamped to at most 3; reading is the word's untoned spelling (a-z only) and
// text is in the given output script. A learned word moves ahead of its
// homophones in whole-word lists and sentence composition, but past the best
// homophone it gains only a fifth of its bonus, so it reorders words of one
// reading without splitting longer words apart. Bonus <= 0 removes a word,
// NULL text removes a whole reading. Sessions share the table: changes apply
// at their next refresh, on the thread that uses them. Without a table, or
// with an empty one, every result is unchanged.
typedef struct MPNPersonal MPNPersonal;
MPNPersonal *mpn_personal_create(void);
void mpn_personal_destroy(MPNPersonal *personal);
int mpn_personal_set(MPNPersonal *personal, int simplified, const char *reading, const char *text, double bonus);
void mpn_personal_clear(MPNPersonal *personal);
// NULL detaches. A session keeps an attached table alive after its handle is destroyed.
void mpn_set_personal(MPNSession *session, const MPNPersonal *personal);
// The dictionary words that spell a candidate, for learning: one
// "reading<TAB>text<LF>" line per word, readings untoned, in the session's
// output script, fewest words first. Empty when the candidate is not exactly
// a sequence of fully spelled dictionary words over the keys it consumes
// (corrections, abbreviations, literal text). Valid until the next call.
const char *mpn_candidate_words(MPNSession *session, size_t index);

// Optional fuzzy pinyin (CN-03), per session; 0 (the default) changes nothing.
// Each bit lets a typed complete syllable also stand for the syllable whose
// initial or final differs by that pair, in whole-word lists and sentence
// composition, competing mostly on frequency: every changed initial or final
// costs 5 nats where an exact word covers the same keys, so an exact reading
// wins ties and near-ties, and 8 nats where none does unless the fuzzy word is
// common. Candidates that use a substitution report MPN_CANDIDATE_CORRECTION
// and mpn_candidate_fuzzy. Lone
// initials (abbreviations) are never substituted, and segment-by-segment
// candidates stay exact. Pinyin lexicons only; unknown bits are ignored.
// Changing the mask rebuilds the candidates of an active composition.
enum {
    MPN_FUZZY_Z_ZH = 1u << 0,
    MPN_FUZZY_C_CH = 1u << 1,
    MPN_FUZZY_S_SH = 1u << 2,
    MPN_FUZZY_N_L = 1u << 3,
    MPN_FUZZY_F_H = 1u << 4,
    MPN_FUZZY_R_L = 1u << 5,
    MPN_FUZZY_AN_ANG = 1u << 6,
    MPN_FUZZY_EN_ENG = 1u << 7,
    MPN_FUZZY_IN_ING = 1u << 8,
    MPN_FUZZY_IAN_IANG = 1u << 9,
    MPN_FUZZY_UAN_UANG = 1u << 10,
    MPN_FUZZY_ALL = (1u << 11) - 1
};
void mpn_set_fuzzy_pinyin(MPNSession *session, uint32_t mask);
uint32_t mpn_fuzzy_pinyin(const MPNSession *session);
// 1 when the candidate uses at least one fuzzy substitution, else 0.
int mpn_candidate_fuzzy(const MPNSession *session, size_t index);

// Optional Japanese words for mixed pinyin/romaji. Registration and decoding
// are local to the session; a returned string remains valid until the next call.
// Weight 3500 denotes an ambiguous all-kanji pinyin alias: it participates in
// a mixed sentence only when another, higher-confidence Japanese word is present.
// Two-letter readings are rejected except the high-confidence reading "au";
// mixed decoding of that short reading also requires independent evidence.
int mpn_add_japanese_word(MPNSession *session, const char *reading, const char *text, int weight);
const char *mpn_mixed_pinyin(MPNSession *session, const char *reading, int simplified);
// A complete leading segment and its ASCII reading length, leaving the suffix untouched.
const char *mpn_mixed_pinyin_prefix(MPNSession *session, const char *reading, int simplified, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif
