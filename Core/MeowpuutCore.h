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
// Set a complete reading directly and decode it once, instead of refreshing on
// every prefix as repeated mpn_key calls would. Accepts empty input (clearing
// the reading while preserving segment context and pending commits) or any
// sequence up to mpn_max_input_bytes() that repeated mpn_key calls would accept
// verbatim without implicit commits or ignored keys; otherwise leaves the
// session unchanged and returns 0.
int mpn_set_input(MPNSession *session, const char *input);
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

// User words (DICT-02b): words the user added by hand to the same personal
// table, such as names or new terms the lexicon lacks. reading is pinyin
// letters a-z, each syllable optionally ending in a tone digit 1-5, with
// single apostrophes allowed between syllables (zhang'san, zhang1san1,
// xi'an); letters between separators are split into syllables of the
// session's lexicon (fewest syllables, then the longest first), and a word
// whose reading does not split is ignored for that lexicon. A syllable
// without a tone matches any typed tone. simplified is the text shown in
// simplified output and the word's identity together with the untoned
// reading: adding the same pair again updates it. traditional may be NULL or
// empty, in which case the lexicon's simplified-to-traditional mapping is
// used. Texts are non-empty UTF-8 of at most 192 bytes without control
// characters. Returns 1 on success, 0 for invalid arguments.
//
// For its exact reading a user word leads the whole-word list (several in the
// order added; a lexicon word of the same text moves up with it). In sentence
// composition and through its initials (CN-04) it counts as a fairly common
// word (weight 3000) at the learned-word cap, so it can replace rarer
// homophones and individually spelled characters but not a common word of
// the same reading; its candidates report mpn_candidate_personal. Fuzzy
// pinyin and typo correction do not reach user words. Pinyin lexicons only.
// mpn_personal_clear removes learned bonuses only, not user words.
int mpn_personal_add_word(MPNPersonal *personal, const char *reading, const char *simplified, const char *traditional);
// 1 when the word (untoned reading, simplified text) was present and removed.
int mpn_personal_remove_word(MPNPersonal *personal, const char *reading, const char *simplified);
void mpn_personal_clear_words(MPNPersonal *personal);
size_t mpn_personal_word_count(const MPNPersonal *personal);
// 1 when the candidate contains a user word, else 0.
int mpn_candidate_personal(const MPNSession *session, size_t index);

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

// Abbreviated and mixed pinyin (CN-04), always on for Pinyin lexicons. When
// the keys do not split into complete syllables (nh, zgren, nihaom), an
// initial may stand for a whole syllable of a dictionary word of two or more
// syllables: one letter (z/c/s also cover zh/ch/sh; a/o/e cover the syllables
// they begin) or zh/ch/sh. Such words compete on frequency less a penalty per
// abbreviated syllable, never where an exact fully spelled word covers the
// same keys, and report MPN_CANDIDATE_CORRECTION. Keys that do split into
// complete syllables are decoded exactly as without this feature. A single
// initial after complete syllables (nihaom) leaves the exact prefix (你好)
// first and puts the completion (你好吗) second. Lone initials are never
// fuzzy-substituted; mpn_candidate_words returns nothing for abbreviations.
// The number of abbreviated syllables in the candidate; 0 for none.
size_t mpn_candidate_abbreviated(const MPNSession *session, size_t index);

// Typo correction (CN-05), per session, on by default. Pinyin keys that do
// not split into complete syllables may contain one slip, read as one of:
// two adjacent letters swapped (zhogn, nihoa), one letter missing (zhonguo),
// one letter too many (nihhao, zhonggguo) or one letter replaced by a keyboard
// neighbour. A dictionary word one such edit from a piece of the keys
// competes in sentence composition on frequency less 3 nats (common words) or
// 6 nats, at most one corrected word per candidate, never beside an
// abbreviation and never where an exact fully spelled word covers the same
// keys; the last key is never dropped and nothing is assumed after it, so an
// unfinished syllable stays an abbreviation or prefix. Keys that do split into
// complete syllables keep their exact candidates first; a common word one
// edit from all of them (zhonggou -> 中国) follows the first three. Such
// candidates report MPN_CANDIDATE_CORRECTION and mpn_candidate_typo; they are
// not learned word by word. Turning it off also removes the earlier local
// corrections (keyboard neighbours, swaps and n/ng of a whole reading), so no
// candidate then rests on a guessed slip; prefix completion stays. Changing
// it rebuilds the candidates of an active composition.
void mpn_set_typo_correction(MPNSession *session, int enabled);
int mpn_typo_correction(const MPNSession *session);
// 1 when the candidate rests on a CN-05 typo correction, else 0.
int mpn_candidate_typo(const MPNSession *session, size_t index);

// Candidate diagnostics (DICT-02a). Read-only: neither function changes the
// session's candidates, ranking or composition (they may prepare the user
// words for the lexicon, as any lookup does). The returned buffer is valid until the next call of either function
// on the session.
//
// mpn_candidate_explain: the highest-scoring split of the candidate's text
// over the keys it consumes into lexicon and user words, as the sentence
// decoder scores it, one line per word:
//   reading<TAB>text<TAB>source<TAB>weight<TAB>prior<TAB>base<TAB>bonus<TAB>lift<TAB>join<LF>
// reading is the word's toned spelling; source is "lexicon", "user" or
// "lexicon+user"; weight is the lexicon weight in the session's script (3000
// for a user word); prior is the corpus correction in nats; base is
// weight / 700 - 19 + prior; bonus is the personal bonus offered (CN-06c
// learned word, or the cap for a DICT-02b user word); lift is what the bonus
// adds after the decoder's bounded rule; join is the collocation and context
// bonus towards the rest of the text (and the phrase start for the first
// word, the phrase end for the last). Values have four decimals. For a
// sentence candidate (MPN_SCORE_PATH) the sum of base + lift + join over the
// lines equals its score when the decoder took the same split. Empty for a
// candidate that reports mpn_candidate_fuzzy, mpn_candidate_typo or
// mpn_candidate_abbreviated, and when the text does not split into words of
// those keys (Japanese text, literal suffixes).
const char *mpn_candidate_explain(MPNSession *session, size_t index);
// mpn_explain_text: where a text exists, for reverse lookup. One line per entry:
//   lexicon<TAB>reading<TAB>traditional<TAB>simplified<TAB>weight<TAB>simplified weight<TAB>prior<TAB>pair<LF>
//   user<TAB>reading<TAB>traditional<TAB>simplified<LF>
//   context<TAB>phrase<TAB>weight<TAB>simplified weight<TAB>authored<LF>
// lexicon rows whose traditional or simplified text equals text (pair is 1
// when the word has collocation statistics); user words of the attached
// personal table prepared for this lexicon; the context phrase equal to the
// text or its traditional form (authored is 1 for an editorial phrase). At
// most 512 lines.
const char *mpn_explain_text(MPNSession *session, const char *text);

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

// Pure read-only helpers for standalone engine metadata (WIN-03 step 7c):
// decompose (reading, text) into "reading\tword\n" pairs for CN-06c learning
// and forgetting, and compute the traditionalized segment context after text.
const char *mpn_decompose_words(MPNSession *session, const char *reading, const char *text);
const char *mpn_advance_context(MPNSession *session, const char *context, const char *text);

#ifdef __cplusplus
}
#endif

#endif
