#include "../EngineMessages.h"
#include "../PendingDelivery.h"
#include <cassert>
#include <iostream>
using namespace harukoto::engine;

template<class T> static void codec(const T &message) {
    const auto wire = encodePayload(message); T output = message;
    assert(decodePayload(wire, output) && encodePayload(output) == wire);
    for (size_t size = 0; size < wire.size(); ++size) {
        std::vector<uint8_t> cut(wire.begin(), wire.begin() + size);
        assert(!decodePayload(cut, output) && encodePayload(output) == wire);
    }
    auto trailing = wire; trailing.push_back(0);
    assert(!decodePayload(trailing, output) && encodePayload(output) == wire);
    auto unknown = wire; unknown[1] = 0xff; assert(!decodePayload(unknown, output));
}
static Frame reply(const Frame &request, const std::vector<uint8_t> &payload = {}) {
    return {Kind::Reply, request.id, payload};
}
static CandidatePage page() { return {"ribennihao", true, {{11,5,"日本","にほん"}, {12,5,"にほん",""}}}; }
static void limits() {
    codec(KeyMessage{KeyAction::Text, 'a', true});
    codec(KeyMessage{KeyAction::PageNext, 0, false});
    codec(KeyMessage{KeyAction::Refresh, 0, true});
    codec(page()); codec(Selection{8,11}); codec(Commit{{8,11},5,"日本","nihao"});
    codec(CandidatePage{});
    KeyMessage key; auto b = encodePayload(KeyMessage{KeyAction::Text, 'a', true});
    b[4] = 2; assert(!decodePayload(b, key)); b[4] = 1; b[3] = 0; assert(!decodePayload(b, key));
    b = encodePayload(KeyMessage{KeyAction::Backspace, 0, true}); b[3] = 'a'; assert(!decodePayload(b, key));
    for (const auto &bad : {std::string("\xc0\xaf",2), std::string("\xed\xa0\x80",3),
                            std::string("\xf4\x90\x80\x80",4), std::string("\xe3\x81",2),
                            std::string("\xc2\x85",2), std::string("\0",1), std::string("a\nb")}) {
        assert(!textValid(bad));
        auto p = page(); p.words[0].text = bad; assert(!valid(p));
    }
    assert(textValid("日本にほん𠮷"));
    assert(!textValid(std::string(MaxText + 1, 'a')));
    auto p = page(); p.words[1].id = 11; assert(!valid(p));
    p = page(); p.words[0].consumed = 0; assert(!valid(p));
    p = page(); p.words[0].consumed = 11; assert(!valid(p));
    p = page(); p.words[0].id = 0; assert(!valid(p));
    p = page(); p.input = "日本"; assert(!valid(p));
    p = page(); p.words.clear(); assert(!valid(p)); // more without any candidates
    p = {std::string(MaxInput, 'a'), false, {}};
    for (uint64_t id = 1; id <= MaxCandidates; ++id) p.words.push_back({id,256,"字",""});
    codec(p); p.words.push_back({129,1,"词",""}); assert(!valid(p));
    p.words.pop_back(); p.input.push_back('a'); assert(!valid(p)); p.input.pop_back();
    for (auto &word : p.words) word.text = std::string(MaxText, 'a');
    bool rejected = false;
    try { encodePayload(p); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    CandidatePage output = page();
    assert(!decodePayload(std::vector<uint8_t>(MaxPayload + 1), output));
    assert(output.input == "ribennihao");
    // Corrupt a valid wire: duplicate IDs and over-range consumed are rejected too.
    b = encodePayload(page());
    const size_t first = 2 + 2 + page().input.size() + 1 + 2;
    b[first + 8] = 0; assert(!decodePayload(b, output));
    b = encodePayload(page()); b[first + 8] = 255; assert(!decodePayload(b, output));
    const size_t second = first + 8 + 4 + 2 + page().words[0].text.size() + 2 + page().words[0].reading.size() + 4 + 1;
    b = encodePayload(page()); b[second] = 11; assert(!decodePayload(b, output));
    b = encodePayload(page()); b[first + 14] = 0xff; assert(!decodePayload(b, output));
    b = encodePayload(page()); b[2] = 255; b[3] = 255; assert(!decodePayload(b, output));
    // WIN-03 step 6a: core index and flags travel with each candidate.
    p = page(); p.words[0].core = 7; p.words[0].flags = candidate_flag::Japanese | candidate_flag::Direct | (2 << candidate_flag::RoundShift);
    codec(p); assert(candidateRound(p.words[0]) == 2);
    b = encodePayload(p); assert(decodePayload(b, output) && output.words[0].core == 7 && output.words[0].flags == p.words[0].flags);
    p.words[0].flags = 8; assert(!valid(p));
    p = page(); p.words[0].core = MaxCoreIndex + 1; assert(!valid(p));
    // WIN-03 step 6b: a key with the client's ranking state, and "behind".
    KeyMessage query{KeyAction::Text, 'n', false, true, {0x7ff, false, true, 493000, std::string(64, 'b'), std::string(64, 'c'), std::string(64, 'd')}};
    codec(query);
    KeyMessage queryOut;
    assert(decodePayload(encodePayload(query), queryOut) && queryOut.extended && queryOut.settings.fuzzy == 0x7ff &&
           !queryOut.settings.typo && queryOut.settings.memory && queryOut.settings.hour == 493000 && queryOut.settings.words == std::string(64, 'c'));
    for (auto bad : {query, query, query, query}) {
        static int which = 0;
        switch (which++) {
        case 0: bad.settings.user.clear(); break;                      // the user digest is always given
        case 1: bad.settings.memory = false; break;                    // phrase/word digests only with memory
        case 2: bad.settings.phrases = std::string(64, 'G'); break;
        default: bad.settings.hour = -1; break;
        }
        assert(!valid(bad));
    }
    p = {"ni", false, {}, true}; codec(p);
    // Step 6d: each page names the data identity it was listed from; remembered texts travel with a query.
    p = {"ni", false, {{1, 2, "你", ""}}, false, std::string(64, 'e')}; codec(p);
    p.identity = std::string(63, 'e'); assert(!valid(p));
    query.settings.remembered = {"你好", "拟好"}; codec(query);
    query.settings.remembered.assign(MaxRemembered + 1, "字"); assert(!valid(query));
    p.words.push_back({1, 2, "你", ""}); assert(!valid(p));
    p = {"ni", true, {{1, 2, "你", ""}}, true}; assert(!valid(p));
    const Opened opened{std::string(IdentitySize, 'a')}; codec(opened);
    Opened openedOut;
    assert(!decodePayload(encodePayload(KeyMessage{KeyAction::Text, 'a', true}), openedOut));
    for (const auto &bad : {std::string(IdentitySize - 1, 'a'), std::string(IdentitySize, 'A'), std::string(IdentitySize, 'g')}) {
        bool refused = false;
        try { encodePayload(Opened{bad}); } catch (const std::invalid_argument &) { refused = true; }
        assert(refused);
    }
}
static void selection() {
    CandidateSnapshot snapshot;
    Frame candidates{Kind::Reply,{1,2,3,1,10},encodePayload(page())};
    assert(snapshot.publish(candidates)); assert(!snapshot.publish(candidates));
    Frame choose{Kind::Select,{1,2,3,1,11},encodePayload(Selection{10,11})};
    assert(snapshot.resolve(choose)->text == "日本");
    auto kana = choose; kana.payload = encodePayload(Selection{10,12});
    assert(snapshot.resolve(kana)->text == "にほん");
    for (auto member : {&Identity::connection, &Identity::engine, &Identity::session, &Identity::composition}) {
        auto wrong = choose; ++(wrong.id.*member); assert(!snapshot.resolve(wrong));
    }
    auto wrong = choose; wrong.payload = encodePayload(Selection{9,11}); assert(!snapshot.resolve(wrong));
    wrong.payload = encodePayload(Selection{10,999}); assert(!snapshot.resolve(wrong));
    auto expected = snapshot.preview(choose); assert(expected && expected->remaining == "nihao");
    auto invalidPage = candidates; invalidPage.id.request = 11; invalidPage.payload.push_back(0);
    assert(!snapshot.publish(invalidPage) && snapshot.resolve(choose));
    Commit output{{99,99},1,"保留",""};
    for (int n = 0; n < 5; ++n) {
        auto bad = *expected;
        if (n == 0) bad.text = "错误";
        if (n == 1) bad.remaining = "hao";
        if (n == 2) bad.consumed = 4;
        if (n == 3) bad.selection.candidate = 12;
        if (n == 4) bad.selection.sourceRequest = 9;
        assert(!snapshot.confirm(choose,reply(choose,encodePayload(bad)),output));
        assert(output.text == "保留" && snapshot.resolve(choose));
    }
    auto response = reply(choose,encodePayload(*expected));
    auto forged = response; ++forged.id.request; assert(!snapshot.confirm(choose,forged,output));
    assert(snapshot.confirm(choose,response,output) && output.remaining == "nihao");
    assert(!snapshot.confirm(choose,response,output));
    assert(!snapshot.publish(candidates));
    candidates.id.request = 12; assert(snapshot.publish(candidates));
    assert(!snapshot.resolve(choose)); // same ID but previous candidate generation
    choose.id.request = 13; choose.payload = encodePayload(Selection{12,11});
    assert(snapshot.resolve(choose)); snapshot.invalidate(); assert(!snapshot.resolve(choose));
    assert(!snapshot.publish(candidates)); // invalidation keeps version high-water mark
    candidates.id.request = 14; candidates.id.composition = 2; assert(snapshot.publish(candidates));
    auto old = candidates; old.id.request = 15; old.id.composition = 1; assert(!snapshot.publish(old));
    old = candidates; old.id.request = 15; old.id.session = 4; assert(!snapshot.publish(old));
    choose.id.request = 15; choose.id.composition = 2; choose.payload = encodePayload(Selection{14,12});
    assert(snapshot.preview(choose)->text == "にほん");
}
static void gateAndQueue() {
    ClientGate gate; auto hello = gate.connect(0,100);
    auto ack = hello; ack.kind = Kind::HelloAck; ack.id.engine = 8; assert(gate.acknowledge(ack,1));
    auto opened = gate.open(2,100); assert(opened && gate.accept(reply(*opened),3));
    const auto session = opened->id.session;
    auto key = gate.request(session,Kind::Key,4,100); assert(key);
    key->payload = encodePayload(KeyMessage{KeyAction::Text,'n',true});
    auto candidates = reply(*key,encodePayload(page()));
    CandidateSnapshot snapshot;
    // Stage payload validation before consuming the gate's matching response.
    auto staged = snapshot; assert(staged.publish(candidates));
    assert(gate.accept(candidates,5)); snapshot = std::move(staged);
    auto select = gate.request(session,Kind::Select,6,100); assert(select);
    select->payload = encodePayload(Selection{key->id.request,11});
    auto commit = snapshot.preview(*select); assert(commit);
    auto response = reply(*select,encodePayload(*commit)); Commit output;
    auto corrupt = *commit; corrupt.text = "坏结果";
    assert(!snapshot.validateConfirmation(*select,reply(*select,encodePayload(corrupt)),output));
    assert(snapshot.validateConfirmation(*select,response,output));
    assert(gate.accept(response,7) && snapshot.confirm(*select,response,output));
    PendingDelivery original, other;
    original.append(L"日本"); // production adapter will convert validated UTF-8
    assert(!gate.accept(response,7) && !snapshot.confirm(*select,response,output));
    original.suspend(); snapshot.invalidate(); auto cancel = gate.cancel(session,8,100); assert(cancel);
    assert(!original.empty() && other.empty() && original.begin() == nullptr);
    gate.disconnect(); assert(!gate.accept(response,9));
    original.resume(); assert(*original.begin() == L"日本"); original.finish(true); assert(original.empty());
}
int main() {
    limits(); selection(); gateAndQueue();
    std::cout << "PASS: typed payload bounds, UTF-8, stale candidates, partial selection, confirmation and delivery ownership\n";
}
