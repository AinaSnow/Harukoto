#include "../JapaneseConjugation.h"
#include "../../Core/MeowpuutCore.h"
#include <cassert>
#include <iostream>
#include <iterator>

static void number(std::string &out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<char>(value >> (i * 8)));
}
static void field(std::string &out, const std::string &s) { number(out, static_cast<uint32_t>(s.size())); out += s; }
static std::string fixture(const std::string &reading = "たべる") {
    std::string out = "HARUKOTO-CONJ-1\n"; number(out, 1);
    field(out, "eat"); field(out, "食べる"); field(out, reading);
    number(out, 1); field(out, "english"); field(out, "eat");
    number(out, 2);
    field(out, "辞书形"); field(out, "食べる"); field(out, reading);
    field(out, "て形"); field(out, "食べて"); field(out, "たべて");
    return out;
}
int main(int argc, char **argv) {
    JapaneseConjugations table;
    const auto good = fixture();
    assert(table.load(good));
    assert(table.entries().size() == 1);
    assert(table.findWord("食べる", "たべる")[0]->forms[1].text == "食べて");
    assert(table.findWord("食べる", "wrong").empty());
    assert(table.findTrigger("english", "eat").size() == 1);
    assert(table.findTrigger("pinyin", "eat").empty());
    for (size_t i = 0; i < good.size(); ++i) {
        assert(!table.load(good.substr(0, i)));
        assert(table.entries().empty());
        assert(!table.error().empty());
    }
    assert(!table.load(good + "trailing"));
    assert(!table.load(fixture("taberu")));
    assert(!table.load(fixture(std::string("\xc0\xaf", 2))));
    assert(!table.load(fixture(std::string("\xed\xa0\x80", 3))));
    assert(!table.load(fixture(std::string("\0", 1))));
    auto huge = good; huge.replace(16, 4, 4, static_cast<char>(255));
    assert(!table.load(huge));
    assert(!table.load(std::string(JapaneseConjugations::maxBytes + 1, 'x')));
    auto duplicate = good; duplicate[16] = 2; duplicate += good.substr(20);
    assert(!table.load(duplicate));
    assert(table.load(good));
    assert(!table.loadFile(std::filesystem::path("missing-conjugation-fixture.bin")));
    assert(table.findTrigger("english", "eat").empty());
    // Auxiliary failure leaves the core available and able to commit Chinese.
    std::string lexiconData = "MEOWPUUT-PINYIN-2\nhan\than4\t漢\t汉\t9000\t9000\n";
    auto *lexicon = mpn_lexicon_create(lexiconData.data(), lexiconData.size());
    assert(lexicon);
    auto *session = mpn_session_create(lexicon); assert(session);
    mpn_set_simplified(session, 1);
    for (char c : std::string("han")) assert(mpn_key(session, c));
    assert(std::string(mpn_candidate(session, 0)) == "汉");
    assert(mpn_choose(session, 0));
    assert(std::string(mpn_committed(session)) == "汉");
    mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
    if (argc == 2) {
        assert(table.loadFile(std::filesystem::path(argv[1])));
        assert(table.entries().size() == 10);
        const auto entries = table.findTrigger("english", "come");
        assert(entries.size() == 1);
        bool found = false;
        for (const auto &form : entries[0]->forms)
            if (form.name == "ない形") { assert(form.text == "来ない" && form.reading == "こない"); found = true; }
        assert(found);
    }
    std::cout << "PASS: bounded conjugation loader, lookups, truncation/UTF-8 failures, Chinese fallback and compiled data\n";
}
