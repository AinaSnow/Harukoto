// Exercise the actual TSF callbacks without registering or installing an IME.
#include "../TextService.cpp"
#include <cassert>
#include <iostream>

namespace {
struct ServiceFocusTest {
    static void run() {
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\nhao\thao3\t好\t好\t9000\nni\tni3\t你\t你\t9000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        auto *saved = mpn_session_create(service.lexicon);
        service.suspended.emplace(nullptr, Service::SavedContext{saved});
        const auto prepare = [&] {
            mpn_clear(service.session);
            for (char c : std::string("ni")) assert(mpn_key(service.session, c));
            assert(mpn_choose(service.session, 0)); // Accepted text must survive cancellation.
            for (char c : std::string("hao")) assert(mpn_key(service.session, c));
            mpn_set_segment_context(service.session, "你");
            service.choices = {{"好", 0}};
            service.page = service.highlighted = 2;
            service.expanded = service.chineseLocked = true;
            mpn_clear(saved);
            for (char c : std::string("ni")) assert(mpn_key(saved, c));
            auto &state = service.suspended.at(nullptr);
            state.choices = {{"你", 0}}; state.page = 1; state.expanded = true;
        };
        const auto check = [&] {
            assert(!*mpn_input(service.session) && !*mpn_preedit(service.session));
            assert(!*mpn_segment_context(service.session) && !mpn_candidate_count(service.session));
            assert(std::string(mpn_committed(service.session)) == "你");
            assert(service.choices.empty() && !service.page && !service.highlighted);
            assert(!service.expanded && !service.chineseLocked);
            assert(!*mpn_input(saved) && service.suspended.at(nullptr).choices.empty());
            service.OnSetThreadFocus(); service.OnSetFocus(TRUE);
            assert(!*mpn_input(service.session));
            assert(mpn_key(service.session, 'n') && std::string(mpn_input(service.session)) == "n");
        };
        prepare(); service.OnSetFocus(TRUE); assert(std::string(mpn_input(service.session)) == "hao");
        service.OnSetFocus(FALSE); check();
        prepare(); service.OnKillThreadFocus(); check();
        prepare(); service.OnSetFocus(reinterpret_cast<ITfDocumentMgr *>(1), nullptr); check();
        prepare(); service.OnPushContext(nullptr); check();
        service.suspended.erase(nullptr); mpn_session_destroy(saved);
    }

    // CN-06 through the production selection path: what was chosen once is
    // first the next time, and a reading completed in segments is remembered
    // as a whole. No TSF objects are needed for candidate handling.
    static void memory(const std::filesystem::path &folder) {
        const auto path = folder / (L"service-memory-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        std::filesystem::remove(path);
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "hao\thao3\t好\t好\t9000\nhao\thao4\t号\t号\t8000\n"
            "ni\tni3\t你\t你\t9000\nni\tni2\t泥\t泥\t5000\n"
            "nihao\tni3hao3\t你好\t你好\t9000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        service.memory = std::make_unique<selection_memory::Store>(path);
        service.memory->poll();
        service.memoryEnabled = true;
        const auto type = [&](const std::string &keys) {
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
        };
        const auto find = [&](const std::string &text) {
            // Leading words and characters may only appear on a later page.
            for (int pass = 0; pass < 2; ++pass) {
                for (size_t i = 0; i < service.choices.size(); ++i) if (service.choices[i].text == text) return i;
                if (!pass) service.expandCandidates();
            }
            return service.choices.size();
        };
        const auto committed = [&](std::wstring text) {
            text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
            return text;
        };
        std::wstring text;
        // A different homophone, chosen once, leads next time.
        type("hao");
        assert(service.choices.front().text == "好" && find("号") > 0 && find("号") < service.choices.size());
        assert(service.chooseCandidate(find("号"), text) && committed(text) == L"号");
        type("hao");
        assert(service.choices.front().text == "号" && service.choices.front().coreIndex && find("好") == 1);
        // Choosing the original again restores it: equal counts, latest first.
        text.clear();
        assert(service.chooseCandidate(find("好"), text));
        type("hao");
        assert(service.choices.front().text == "好");
        // Segment by segment: 泥 then 号. The phrase is then offered whole.
        type("nihao");
        assert(service.choices.front().text == "你好" && find("泥号") > 0);
        text.clear();
        if (find("泥") == service.choices.size()) service.expandCandidates(); // Leading characters follow on the next page.
        const auto mud = find("泥");
        assert(mud < service.choices.size() && service.chooseCandidate(mud, text));
        assert(std::string(mpn_input(service.session)) == "hao" && service.composedSegments == 1);
        service.refreshCandidates();
        assert(service.chooseCandidate(find("号"), text) && committed(text) == L"泥号" && !service.composedSegments);
        type("nihao");
        assert(service.choices.front().text == "泥号" && find("你好") == 1);
        text.clear();
        assert(service.chooseCandidate(0, text) && committed(text) == L"泥号" && !*mpn_input(service.session));
        // A cancelled composition leaves no phrase behind.
        type("nihao");
        text.clear();
        assert(service.chooseCandidate(find("你"), text) && service.composedSegments == 1);
        service.cancelForFocusChange();
        assert(!service.composedSegments && service.composedReading.empty());
        mpn_clear_commit(service.session);
        // Automatic completion after punctuation learns only the confirmed first choice.
        type("nihao");
        text.clear();
        service.finishCandidateText(text, find("你"));
        assert(text == L"你号" && !service.composedSegments); // 号 leads hao by now; that completion is not learned.
        type("nihao");
        assert(service.choices.front().text == "泥号" && find("你好") == 1);
        // Switched off: nothing is applied and nothing is recorded.
        const auto before = service.memory->recall("hao", true);
        assert(before.size() == 2 && before.front() == "号");
        service.memoryEnabled = false;
        type("hao");
        assert(service.choices.front().text == "好");
        text.clear();
        assert(service.chooseCandidate(0, text));
        service.memoryEnabled = true;
        assert(service.memory->recall("hao", true) == before);
        type("hao");
        assert(service.choices.front().text == "号");
        // The journal survives the service and is shared with the next one.
        service.memory->flush();
        selection_memory::Store other(path); other.poll();
        assert(other.recall("nihao", true).front() == "泥号" && other.recall("hao", true) == before);
        mpn_clear(service.session); service.resetCandidates();
        const auto words = service.memory->wordsFile();
        service.memory.reset(); std::filesystem::remove(path); std::filesystem::remove(words);
    }

    // CN-06c through the production selection path: a word chosen as one
    // segment of a sentence leads in another sentence and on its own, and
    // switching memory off restores the original lists exactly.
    static void personalWords(const std::filesystem::path &folder) {
        const auto path = folder / (L"service-words-" + std::to_wstring(GetCurrentProcessId()) + L".tsv");
        Service service;
        const std::string data = "MEOWPUUT-PINYIN-1\n"
            "da\tda4\t大\t大\t8000\n"
            "gong\tgong1\t工\t工\t3000\n"
            "gongsi\tgong1si1\t攻絲\t攻丝\t6000\ngongsi\tgong1si1\t公司\t公司\t5800\n"
            "hen\then3\t很\t很\t8000\n"
            "si\tsi1\t絲\t丝\t3000\n"
            "wo\two3\t我\t我\t9000\n"
            "zai\tzai4\t在\t在\t9000\n";
        service.lexicon = mpn_lexicon_create(data.data(), data.size());
        service.session = mpn_session_create(service.lexicon);
        assert(service.lexicon && service.session);
        mpn_set_simplified(service.session, service.simplified);
        service.memory = std::make_unique<selection_memory::Store>(path);
        std::filesystem::remove(service.memory->wordsFile());
        service.memory->poll();
        // As OnKeyDown does: read the switch, then attach or detach the table.
        const auto type = [&](const std::string &keys, bool enabled = true) {
            service.memoryEnabled = enabled;
            service.attachPersonal();
            mpn_clear(service.session); mpn_clear_commit(service.session); service.resetCandidates();
            for (char c : keys) assert(mpn_key(service.session, c));
            service.refreshCandidates();
            std::vector<std::string> texts;
            for (const auto &choice : service.choices) texts.push_back(choice.text);
            return texts;
        };
        const auto find = [&](const std::string &text) {
            // Leading words and characters may only appear on later pages.
            for (int pass = 0; pass < 4; ++pass) {
                for (size_t i = 0; i < service.choices.size(); ++i) if (service.choices[i].text == text) return i;
                service.expandCandidates();
            }
            return service.choices.size();
        };
        // Nothing learned yet: the lists are those of a service without memory.
        const auto single = type("gongsi", false), sentence = type("gongsihenda", false), start = type("wozaigongsi", false);
        assert(single.front() == "攻丝" && sentence.front() == "攻丝很大" && start.front() == "我在攻丝");
        assert(type("gongsi") == single && type("gongsihenda") == sentence && type("wozaigongsi") == start);
        // 我, 在, then 公司, one segment at a time.
        type("wozaigongsi");
        std::wstring text;
        for (const char *segment : {"我", "在", "公司"}) {
            const auto index = find(segment);
            assert(index < service.choices.size() && service.chooseCandidate(index, text));
            service.refreshCandidates();
        }
        text += wide(mpn_committed(service.session)); mpn_clear_commit(service.session);
        assert(text == L"我在公司" && !*mpn_input(service.session));
        assert(service.memory->words().bonus("gongsi", "公司", true, selection_memory::currentHour()) > 0);
        // Another sentence and the word alone: the core itself now ranks 公司 first.
        assert(type("gongsihenda").front() == "公司很大" && std::string(mpn_candidate(service.session, 0)) == "公司很大");
        assert(type("gongsi").front() == "公司" && std::string(mpn_candidate(service.session, 0)) == "公司");
        // Switched off: exactly the original lists; on again: learned again.
        assert(type("gongsi", false) == single && type("gongsihenda", false) == sentence && type("wozaigongsi", false) == start);
        assert(type("gongsihenda").front() == "公司很大");
        // Whole-sentence selections are learned word by word too; one
        // correction undoes one slip.
        type("gongsihenda");
        text.clear();
        assert(service.chooseCandidate(find("攻丝很大"), text));
        assert(type("gongsihenda").front() == "攻丝很大" && std::string(mpn_candidate(service.session, 0)) == "攻丝很大");
        type("gongsihenda"); text.clear();
        assert(service.chooseCandidate(find("公司很大"), text));
        assert(type("gongsihenda").front() == "公司很大" && std::string(mpn_candidate(service.session, 0)) == "公司很大");
        assert(type("gongsi").front() == "公司" && std::string(mpn_candidate(service.session, 0)) == "公司");
        // The words reach the journal and the next service.
        service.memory->flush();
        selection_memory::Store other(path); other.poll();
        assert(other.words().bonus("gongsi", "公司", true, selection_memory::currentHour()) >
               other.words().bonus("gongsi", "攻丝", true, selection_memory::currentHour()));
        // Clearing the journal clears the bonuses at the next look.
        const auto words = service.memory->wordsFile();
        std::filesystem::resize_file(words, 0); std::filesystem::resize_file(path, 0);
        service.memory->poll(GetTickCount64() + 10000);
        assert(type("gongsi") == single && type("gongsihenda") == sentence);
        mpn_clear(service.session); service.resetCandidates();
        service.memory.reset(); std::filesystem::remove(path); std::filesystem::remove(words);
    }
};
}
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    ServiceFocusTest::run(); ServiceFocusTest::memory(argv[1]); ServiceFocusTest::personalWords(argv[1]);
    std::cout << "PASS: TSF focus cancellation, fresh input, selected-text preservation, selection memory and personal words\n";
}
