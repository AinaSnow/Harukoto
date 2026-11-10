#define NOMINMAX
#include "../BaseDictionary.h"
#include <cassert>
#include <iostream>
using namespace base_dictionary;

static void check(const Snapshot &snapshot, MPNSession *session, const std::string &expected) {
    assert(std::string(mpn_candidate(session, 0)) == expected);
    assert(snapshot.evidence().dictionary("nihao", true).count(expected));
    assert(!snapshot.words().empty());
    assert(snapshot.conjugations().findTrigger("english", "eat").size() == 1);
}

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--hold") {
            auto snapshot = Snapshot::load(argv[2]);
            std::cout << "ready" << std::endl;
            std::string line; std::getline(std::cin, line);
            return 0;
        }
        if (argc == 5 && std::wstring(argv[1]) == L"--pair") {
            auto first = Snapshot::load(argv[2]);
            auto session = first->session(true);
            for (char c : std::string("niha")) assert(mpn_key(session.get(), c));
            const std::string preedit = mpn_preedit(session.get());
            auto second = Snapshot::load(argv[3]);
            assert(first->generation() != second->generation());
            assert(std::string(mpn_preedit(session.get())) == preedit);
            assert(mpn_key(session.get(), 'o'));
            check(*first, session.get(), "测试甲");
            assert(mpn_choose(session.get(), 0));
            assert(std::string(mpn_committed(session.get())) == "测试甲");
            auto next = second->session(true);
            for (char c : std::string("nihao")) assert(mpn_key(next.get(), c));
            check(*second, next.get(), "测试乙");
            bool rejected = false;
            try { Snapshot::load(argv[4]); } catch (const std::exception &) { rejected = true; }
            assert(rejected);
            check(*second, next.get(), "测试乙");
            // The C core retains its lexicon, but other consumers must keep the
            // Snapshot owner alive while accessing evidence/words/conjugations.
            second.reset();
            assert(mpn_choose(next.get(), 0));
            assert(std::string(mpn_committed(next.get())) == "测试乙");
            std::cout << "PASS: independent generations, unchanged preedit, rejected replacement and lexicon lifetime\n";
            return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--production") {
            auto snapshot = Snapshot::load(argv[2]);
            for (bool hans : {true, false}) {
                auto session = snapshot->session(hans);
                for (const auto &example : {std::make_pair("nihao", "你好"), std::make_pair("zhidaole", "知道了"),
                                           std::make_pair("chi", "吃"), std::make_pair("riben", "日本")}) {
                    mpn_clear(session.get());
                    for (char c : std::string(example.first)) assert(mpn_key(session.get(), c));
                    const auto choices = windowsCandidates(session.get(), snapshot->words(), [&](const std::string &reading) {
                        return snapshot->evidence().dictionary(reading, hans);
                    }, false);
                    assert(std::any_of(choices.begin(), choices.end(), [&](const auto &c) { return c.text == example.second; }));
                }
                assert(!snapshot->conjugations().findTrigger("english", "eat").empty());
            }
            std::cout << "PASS: production snapshot owns Chinese, SQLite, Japanese and conjugation data in both scripts\n";
            return 0;
        }
        throw std::runtime_error("invalid test arguments");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
