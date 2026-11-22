#include "../EngineWindows.h"
#include "../EngineExecutor.h"
#include <cassert>
#include <future>
#include <iostream>
using namespace harukoto::engine;
static bool writable(const std::filesystem::path &path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle); return true;
}
struct Peer {
    ServerConnection server;
    uint64_t seq = 0, composition = 1;
    explicit Peer(std::shared_ptr<const WindowsDictionary> dictionary) : server(42, std::move(dictionary)) {
        Frame hello{Kind::Hello,{1,0,0,0,++seq},{}};
        assert(server.receive(hello,0,100).response);
        assert(server.receive(frame(Kind::Open),0,100).response);
    }
    Frame frame(Kind kind) { return {kind,{1,42,1,composition,++seq},{}}; }
    ServerConnection::Ticket query(const std::string &input, bool simplified = true) {
        ServerConnection::Ticket result;
        for (size_t i=0; i<input.size(); ++i) {
            auto request = frame(Kind::Key);
            request.payload = encodePayload(KeyMessage{KeyAction::Text,static_cast<uint8_t>(input[i]),simplified});
            auto accepted = server.receive(request,0,100); assert(accepted.work); result = accepted.work;
            // Advance intermediate keys without expensive candidate generation.
            if (i+1<input.size()) assert(server.complete(result,{result->input,false,{}},1).response);
        }
        return result;
    }
};
static bool contains(const CandidatePage &page, const std::string &text) {
    return std::any_of(page.words.begin(),page.words.end(),[&](const auto &c) { return c.text==text; });
}
static CandidatePage query(std::shared_ptr<const WindowsDictionary> dictionary, const std::string &input, bool hans=true) {
    Peer peer(dictionary); auto ticket=peer.query(input,hans);
    auto result=dictionary->query(ticket);
    assert(peer.server.complete(ticket,result,1).response);
    return result;
}
static void choose(std::shared_ptr<const WindowsDictionary> dictionary, const std::string &input, const std::string &text, bool hans=true) {
    Peer peer(dictionary); auto ticket=peer.query(input,hans); auto page=dictionary->query(ticket);
    auto response=peer.server.complete(ticket,page,1); assert(response.response);
    const auto found=std::find_if(page.words.begin(),page.words.end(),[&](const auto &word) { return word.text==text; });
    assert(found!=page.words.end());
    auto request=peer.frame(Kind::Select); request.payload=encodePayload(Selection{ticket->request.id.request,found->id});
    auto selected=peer.server.receive(request,2,100); Commit commit;
    assert(selected.response && decodePayload(selected.response->payload,commit));
    assert(commit.text==text && commit.remaining.empty() && commit.consumed==input.size());
    assert(peer.server.receive(request,3,100).status==ServerConnection::Status::Replay);
}
static void directComparison(std::shared_ptr<const WindowsDictionary> dictionary, const std::string &input, bool hans, bool locked=false) {
    Peer peer(dictionary); const auto ticket=peer.query(input,hans);
    auto ticketCopy=std::make_shared<ServerConnection::Work>(*ticket); ticketCopy->chineseLocked=locked;
    auto session=dictionary->base().session(hans);
    for(char c:input) assert(mpn_key(session.get(),c));
    WordEvidence evidence; evidence.open(dictionary->base().databasePath()); assert(evidence.ready());
    auto lookup=[&](const std::string &reading) { return evidence.dictionary(reading,hans); };
    auto formal=windowsCandidates(session.get(),dictionary->words(),lookup,locked);
    for(int i=0;i<3 && mpn_candidates_can_expand(session.get());++i) {
        mpn_expand_candidates(session.get());
        appendCandidateExpansion(session.get(),formal,windowsCandidates(session.get(),dictionary->words(),lookup,locked));
    }
    std::vector<Candidate> expected;
    for(size_t i=0;i<formal.size();++i) {
        auto text=formal[i].text; size_t consumed=input.size();
        if(formal[i].coreIndex) {
            const auto index=*formal[i].coreIndex;
            text.resize(text.size()-mpn_candidate_literal_tail(session.get(),index));
            consumed=mpn_candidate_consumed(session.get(),index);
        }
        if(!text.empty() && consumed) expected.push_back({i+1,static_cast<uint32_t>(consumed),text,formal[i].annotation});
    }
    for(size_t p=0;p*16<expected.size();++p) {
        auto work=std::make_shared<ServerConnection::Work>(*ticketCopy); work->page=static_cast<uint32_t>(p);
        const auto actual=dictionary->query(work);
        assert(actual.words.size()==std::min(size_t(16),expected.size()-p*16));
        for(size_t i=0;i<actual.words.size();++i) {
            const auto &a=actual.words[i], &b=expected[p*16+i];
            assert(a.id==b.id && a.text==b.text && a.consumed==b.consumed && a.reading==b.reading);
        }
        assert(actual.more==(p*16+actual.words.size()<expected.size()));
    }
    // Exactly past the final page and the protocol's last legal page must not
    // invent more results when earlier pages stopped after a lookahead choice.
    for (const auto p : {static_cast<uint32_t>((expected.size()+15)/16), uint32_t(4095)}) {
        auto work=std::make_shared<ServerConnection::Work>(*ticketCopy); work->page=p;
        const auto actual=dictionary->query(work); assert(actual.words.empty() && !actual.more);
    }
}
static void latePageSelection(std::shared_ptr<const WindowsDictionary> dictionary) {
    Peer peer(dictionary); auto work=peer.query("fanyeceshi");
    auto page=dictionary->query(work); assert(page.more);
    assert(peer.server.complete(work,page,1).response);
    size_t pages=1; std::set<uint64_t> ids;
    for (;;) {
        for (const auto &word : page.words) assert(ids.insert(word.id).second);
        if (!page.more) break;
        auto next=peer.frame(Kind::Key); next.payload=encodePayload(KeyMessage{KeyAction::PageNext,0,true});
        const auto scheduled=peer.server.receive(next,2,100); assert(scheduled.work);
        work=scheduled.work; page=dictionary->query(work);
        assert(peer.server.complete(work,page,3).response); assert(++pages<100);
    }
    assert(pages>=3 && !page.words.empty());
    const auto word=page.words.back();
    auto select=peer.frame(Kind::Select); select.payload=encodePayload(Selection{work->request.id.request,word.id});
    const auto selected=peer.server.receive(select,4,100); Commit commit;
    assert(selected.response && decodePayload(selected.response->payload,commit));
    assert(commit.text==word.text && commit.consumed==word.consumed && commit.remaining==work->input.substr(word.consumed));
    assert(peer.server.receive(select,5,100).status==ServerConnection::Status::Replay);
}
static void backgroundQueries(std::shared_ptr<const WindowsDictionary> dictionary) {
    Executor executor(2,3,[](const auto &work) {
        if (!work->dictionary) throw std::invalid_argument("missing query snapshot");
        return work->dictionary->query(work);
    },[] { return 0; });
    Peer japanese(dictionary), chinese(dictionary), theme(dictionary);
    const auto japan=japanese.query("riben"), supplement=chinese.query("ceshibuci"), ff14=theme.query("tejihuifuyao",false);
    for(const auto &work:{japan,supplement,ff14}) assert(executor.submit(work)==Executor::Admission::Accepted);
    for(int i=0;i<3;++i) {
        auto result=executor.takeUntil(std::chrono::steady_clock::now()+std::chrono::seconds(10));
        assert(result && result->page);
        if(result->work==japan) {
            assert(contains(*result->page,"日本") && contains(*result->page,"にほん"));
            assert(result->deliver(japanese.server,1).response);
        } else if(result->work==supplement) {
            assert(contains(*result->page,"旧补词")); assert(result->deliver(chinese.server,1).response);
        } else {
            assert(result->work==ff14 && contains(*result->page,"特級恢復藥")); assert(result->deliver(theme.server,1).response);
        }
    }
    // Actual adapter precondition failure is returned as a typed terminal error.
    Peer missing({}); const auto invalid=missing.query("a");
    assert(executor.submit(invalid)==Executor::Admission::Accepted);
    auto failed=executor.takeUntil(std::chrono::steady_clock::now()+std::chrono::seconds(10));
    assert(failed && !failed->page && failed->error==ErrorCode::Backend);
    assert(failed->deliver(missing.server,1).status==ServerConnection::Status::Failed && missing.server.closed());
    executor.stop(); executor.join();
}
int main(int argc,char **argv) {
    try {
        assert(argc==5);
        const auto first=std::filesystem::u8path(argv[1]), second=std::filesystem::u8path(argv[2]);
        const auto theme=std::filesystem::u8path(argv[3]), personal=std::filesystem::u8path(argv[4]);
        std::weak_ptr<const WindowsDictionary> weak;
        ServerConnection::Ticket retained;
        {
            auto old=WindowsDictionary::load(first,theme,personal); weak=old;
            assert(!writable(first/"language-evidence.sqlite"));
            assert(contains(query(old,"ceshibuci"),"旧补词"));
            auto japan=query(old,"riben");
            assert(contains(japan,"日本") && contains(japan,"にほん"));
            assert(std::any_of(japan.words.begin(),japan.words.end(),[](const auto &c) { return c.text=="日本" && !c.reading.empty(); }));
            assert(contains(query(old,"eat"),"食べる"));
            assert(contains(query(old,"tejihuifuyao"),"特级恢复药"));
            assert(contains(query(old,"tejihuifuyao",false),"特級恢復藥"));
            assert(contains(query(old,"testcoffee"),"珈琲"));
            backgroundQueries(old);
            choose(old,"riben","日本"); choose(old,"riben","にほん");
            choose(old,"ceshibuci","旧补词"); choose(old,"eat","食べる");
            choose(old,"tejihuifuyao","特级恢复药"); choose(old,"tejihuifuyao","特級恢復藥",false);
            for(const auto &input:{"riben","ceshibuci","tejihuifuyao","nihaoz","nihao","fanyeceshi"})
                directComparison(old,input,true);
            directComparison(old,"fanyeceshi",false);
            latePageSelection(old);
            // Controlled source-empty aliases exercise mixed-prefix/suffix/
            // middle indexing, equal-weight first-match order and excluded EN aliases.
            assert(contains(query(old,"ceshibuciabc"),"旧补词世界"));
            assert(contains(query(old,"abcceshibuci"),"世界旧补词"));
            assert(contains(query(old,"ceshibuciabcceshibuci"),"旧补词世界旧补词"));
            for(const auto &input:{"ceshibuciabc","abcceshibuci","ceshibuciabcceshibuci",
                                  "ceshibuciabcd","ceshibuciab","ni'hao","ni3hao3"})
                directComparison(old,input,true);
            directComparison(old,"tejihuifuyao",false);
            directComparison(old,"riben",true,true);
            Peer peer(old); auto pending=peer.query("testcoffee"); retained=pending;
            { std::ofstream file(personal,std::ios::binary); file<<"testcoffee|コーヒー|5000|こーひー|english|咖啡\n"; }
            auto newer=WindowsDictionary::load(second,theme,personal);
            assert(newer->generation()!=old->generation());
            assert(contains(query(newer,"ceshibuci"),"新补词"));
            assert(contains(query(newer,"testcoffee"),"コーヒー"));
            // Pre-step-7: theme/personal reload reuses the pinned base snapshot
            // without reloading the base lexicon or SQLite database.
            auto reused=WindowsDictionary::load(first,theme,personal,{},old->baseSnapshot(),old->memoryMirror());
            assert(reused->baseSnapshot().get()==old->baseSnapshot().get());
            assert(reused->identity()!=old->identity());
            assert(contains(query(reused,"ceshibuci"),"旧补词"));
            assert(contains(query(reused,"testcoffee"),"コーヒー"));
            assert(contains(old->query(pending),"珈琲"));
            bool mismatch=false;
            try { newer->query(pending); } catch(const std::invalid_argument &) { mismatch=true; }
            assert(mismatch);
            auto cancel=peer.frame(Kind::Cancel); cancel.id.composition=++peer.composition;
            assert(peer.server.receive(cancel,2,100).response);
            assert(peer.server.complete(pending,old->query(pending),3).reason==ServerConnection::Reason::Stale);
            // Actual concurrent SQLite connections on separate worker threads.
            std::vector<std::future<void>> workers;
            for(int i=0;i<4;++i) workers.push_back(std::async(std::launch::async,[old,i] {
                for(int round=0;round<3;++round) {
                    auto p=query(old,"zhongguo",i%2==0);
                    assert(p.words.front().text==(i%2==0 ? "中国" : "中國"));
                    assert(contains(query(old,"riben",i%2==0),"にほん"));
                }
            }));
            for(auto &worker:workers) worker.get();
            // Partial selection keeps context and suppresses Japanese bridge choices.
            Peer partial(old); auto job=partial.query("nihaoz");
            auto page=old->query(job); auto completed=partial.server.complete(job,page,1); assert(completed.response);
            auto word=std::find_if(page.words.begin(),page.words.end(),[](const auto &c) { return c.text=="你好" && c.consumed==5; });
            assert(word!=page.words.end());
            auto select=partial.frame(Kind::Select); select.payload=encodePayload(Selection{job->request.id.request,word->id});
            assert(partial.server.receive(select,2,100).response);
            auto key=partial.frame(Kind::Key); key.payload=encodePayload(KeyMessage{KeyAction::Refresh,0,true});
            auto tail=partial.server.receive(key,3,100);
            assert(tail.work && tail.work->chineseLocked && tail.work->context=="你好" && tail.work->input=="z");
            assert(partial.server.complete(tail.work,old->query(tail.work),4).response);
            // Invalid optional input fails preparation without damaging the old snapshot.
            { std::ofstream file(personal); file<<"broken"; }
            bool failed=false; try { WindowsDictionary::load(second,theme,personal); } catch(const std::runtime_error &) { failed=true; }
            assert(failed && contains(query(old,"testcoffee"),"珈琲"));
        }
        assert(!weak.expired() && !writable(first/"language-evidence.sqlite"));
        assert(contains(retained->dictionary->query(retained),"珈琲"));
        {
            std::promise<void> entered,release; auto go=release.get_future().share();
            Executor executor(1,1,[&](const auto &work) {
                entered.set_value(); go.wait();
                auto result=work->dictionary->query(work); assert(contains(result,"珈琲")); return result;
            },[] { return 0; });
            assert(executor.submit(retained)==Executor::Admission::Accepted);
            assert(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready);
            retained.reset(); executor.stop();
            assert(!weak.expired() && !writable(first/"language-evidence.sqlite"));
            release.set_value(); executor.join();
            {
                auto result=executor.tryTake(); assert(result && !result->page && result->error==ErrorCode::Stopped);
                assert(!weak.expired() && !writable(first/"language-evidence.sqlite"));
            }
            assert(weak.expired() && writable(first/"language-evidence.sqlite"));
        }
        std::cout<<"PASS: Windows merge, SQLite supplements, Japanese pair, FF14, pinned generations and query-thread SQLite\n";
    } catch(const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
