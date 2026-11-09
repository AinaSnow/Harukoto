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
};
}
int main() { ServiceFocusTest::run(); std::cout << "PASS: TSF focus cancellation, fresh input and selected-text preservation\n"; }
