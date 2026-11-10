#include "../EngineCore.h"
#include <cassert>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
using namespace harukoto::engine;
static ServerConnection::Work work(std::string input, bool hans = true, uint32_t page = 0) {
    return {{Kind::Key,{1,42,1,1,3},{}},std::move(input),{KeyAction::Refresh,0,hans},100,page,{}};
}
static std::string fixture() {
    std::string bytes = "MEOWPUUT-PINYIN-2\nhao\thao3\t好\t好\t8000\t8000\n"
        "nihao\tni3hao3\t你好\t你好\t9000\t9000\n"
        "zhongguo\tzhong1guo2\t中國\t中国\t9500\t9500\n";
    for (int i=0;i<45;++i) bytes += "ceshi\tce4shi4\t測試" + std::to_string(i) + "\t测试" + std::to_string(i) + "\t5000\t5000\n";
    // Lexicon loader expects reading order.
    const auto header = bytes.substr(0,bytes.find('\n')+1);
    std::istringstream input(bytes.substr(header.size())); std::vector<std::string> lines;
    for (std::string line;std::getline(input,line);) lines.push_back(line);
    std::sort(lines.begin(),lines.end()); bytes = header;
    for (const auto &line : lines) bytes += line + '\n';
    return bytes;
}
// Compare actual core selection, including literal display tails, not just
// candidate metadata reconstructed by the adapter.
static bool compareChoose(const std::string &bytes, const ServerConnection::Work &query, const CandidatePage &page) {
    std::unique_ptr<MPNLexicon,decltype(&mpn_lexicon_destroy)> lex(mpn_lexicon_create(bytes.data(),bytes.size()),mpn_lexicon_destroy);
    assert(lex); bool partial = false;
    for (const auto &candidate : page.words) {
        std::unique_ptr<MPNSession,decltype(&mpn_session_destroy)> s(mpn_session_create(lex.get()),mpn_session_destroy);
        mpn_set_simplified(s.get(),query.key.simplified); mpn_set_segment_context(s.get(),query.context.c_str());
        for (char c : query.input) assert(mpn_key(s.get(),c));
        for (int i=0;i<3 && mpn_candidates_can_expand(s.get());++i) mpn_expand_candidates(s.get());
        assert(candidate.id && candidate.id <= mpn_candidate_count(s.get()));
        const auto index = size_t(candidate.id-1);
        assert(mpn_choose(s.get(),index));
        assert(candidate.text == mpn_committed(s.get()));
        assert(query.input.substr(candidate.consumed) == mpn_input(s.get()));
        partial = partial || candidate.consumed < query.input.size();
    }
    return partial;
}
struct Peer {
    ServerConnection server{42}; uint64_t seq=0; uint64_t generation=1;
    Frame frame(Kind kind) { return {kind,{1,42,1,generation,++seq},{}}; }
    Peer() {
        auto hello = frame(Kind::Hello); hello.id.engine=0; hello.id.session=0; hello.id.composition=0;
        assert(server.receive(hello,0,100).response);
        assert(server.receive(frame(Kind::Open),0,100).response);
    }
    ServerConnection::Result key(KeyAction action, char c=0, bool hans=true) {
        auto request = frame(Kind::Key); request.payload=encodePayload(KeyMessage{action,static_cast<uint8_t>(c),hans});
        return server.receive(request,0,100);
    }
    Frame type(const CoreDictionary &core, const std::string &input, bool hans=true) {
        Frame response;
        for (char c : input) {
            auto job=key(KeyAction::Text,c,hans); assert(job.work);
            auto result=server.complete(job.work,core.query(*job.work),1); assert(result.response); response=*result.response;
        }
        return response;
    }
};
static void integration(const CoreDictionary &core) {
    Peer a,b;
    auto ar=a.type(core,"zhongguo",true), br=b.type(core,"zhongguo",false);
    assert(ar.id == br.id); CandidatePage ap,bp;
    assert(decodePayload(ar.payload,ap) && decodePayload(br.payload,bp));
    assert(ap.words.front().text=="中国" && bp.words.front().text=="中國");
    auto refresh=a.key(KeyAction::Refresh); assert(refresh.work);
    auto cancel=a.frame(Kind::Cancel); cancel.id.composition=++a.generation;
    assert(a.server.receive(cancel,2,100).response);
    assert(a.server.complete(refresh.work,core.query(*refresh.work),3).reason==ServerConnection::Reason::Stale);
    auto empty=a.key(KeyAction::Refresh); assert(empty.work && empty.work->input.empty() && empty.work->context.empty());
    assert(a.server.complete(empty.work,core.query(*empty.work),3).response);
    auto bquery=b.key(KeyAction::Refresh,0,false); assert(bquery.work->input=="zhongguo");
    assert(core.query(*bquery.work).words.front().text=="中國");
    Peer partial;
    auto candidates=partial.type(core,"nihaoz"); CandidatePage p; assert(decodePayload(candidates.payload,p));
    auto found=std::find_if(p.words.begin(),p.words.end(),[](const auto &c) { return c.text=="你好" && c.consumed==5; });
    assert(found!=p.words.end());
    auto select=partial.frame(Kind::Select); select.payload=encodePayload(Selection{candidates.id.request,found->id});
    auto result=partial.server.receive(select,2,100); assert(result.response); Commit commit;
    assert(decodePayload(result.response->payload,commit) && commit.text=="你好" && commit.remaining=="z");
    assert(partial.server.receive(select,2,100).status==ServerConnection::Status::Replay);
    auto tail=partial.key(KeyAction::Refresh); assert(tail.work->input=="z" && tail.work->context=="你好" && tail.work->page==0);
    assert(partial.server.complete(tail.work,core.query(*tail.work),3).response);
    auto erased=partial.key(KeyAction::Backspace); assert(erased.work->input.empty());
    assert(partial.server.complete(erased.work,core.query(*erased.work),3).response);
    auto clean=partial.key(KeyAction::Refresh); assert(clean.work->context.empty());
}
static void paging(const CoreDictionary &core) {
    std::set<uint64_t> ids; size_t count=0;
    for (uint32_t index=0;index<4;++index) {
        auto p=core.query(work("ceshi",true,index));
        for (const auto &c : p.words) { assert(ids.insert(c.id).second); ++count; }
        assert(p.words.size()<=16);
        if (!p.more) break;
    }
    assert(count>=45);
    Peer p; auto response=p.type(core,"ceshi"); CandidatePage first; assert(decodePayload(response.payload,first));
    auto next=p.key(KeyAction::PageNext); assert(next.work->page==1);
    auto second=core.query(*next.work); assert(second.words.front().id!=first.words.front().id);
    assert(p.server.complete(next.work,second,1).response);
    auto previous=p.key(KeyAction::PagePrevious); assert(previous.work->page==0);
    assert(encodePayload(core.query(*previous.work))==encodePayload(first));
    assert(p.server.complete(previous.work,first,1).response);
}
static void sharedQueries(const CoreDictionary &core) {
    std::vector<std::future<void>> workers;
    for (int thread=0;thread<4;++thread) workers.push_back(std::async(std::launch::async,[&,thread] {
        auto shared=core; // retains the same lexicon, no reload or dictionary copy
        for (int n=0;n<20;++n) {
            auto q=work("zhongguo",thread%2==0); auto p=shared.query(q);
            assert(p.words.front().text==(thread%2==0 ? "中国" : "中國"));
            auto other=shared.query(work("nihao")); assert(other.words.front().text=="你好");
        }
    }));
    for (auto &worker : workers) worker.get();
}
int main(int argc,char **argv) {
    try {
        std::string bytes=fixture();
        if (argc==2) { std::ifstream file(std::filesystem::u8path(argv[1]),std::ios::binary); assert(file); bytes.assign(std::istreambuf_iterator<char>(file),{}); }
        CoreDictionary core(bytes);
        for (const bool hans : {true,false}) {
            auto q=work("zhongguo",hans); auto p=core.query(q);
            assert(p.words.front().text==(hans ? "中国" : "中國")); compareChoose(bytes,q,p);
        }
        auto partial=work("nihaoz"); assert(compareChoose(bytes,partial,core.query(partial)));
        auto toned=work("ni3hao3"); assert(core.query(toned).words.front().text=="你好");
        compareChoose(bytes,toned,core.query(toned));
        assert(core.query(work("")).words.empty());
        for (auto bad : {std::string("'nihao"),std::string("ni9"),std::string(257,'a')}) {
            bool rejected=false; try { core.query(work(bad)); } catch(const std::invalid_argument &) { rejected=true; } assert(rejected);
        }
        integration(core); sharedQueries(core); if(argc==1) paging(core);
        assert(mpn_candidate_literal_tail(nullptr,0)==0);
        std::cout << "PASS: real core queries, shared dictionary threads, script isolation, partial selection and paging\n";
    } catch(const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
