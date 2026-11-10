#include "../EngineWindows.h"
#include "../EngineExecutor.h"
#include <iomanip>
#include <iostream>
using namespace harukoto::engine;
using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
static void require(bool condition) { if (!condition) throw std::runtime_error("benchmark invariant failed"); }
struct Case { const char *input, *expected, *remaining; };
static void print(const WindowsDictionary::QueryTimings &t) {
    std::cout << "{\"setup_ms\":" << t.setup_ms << ",\"replay_ms\":" << t.replay_ms
        << ",\"sqlite_open_ms\":" << t.sqlite_open_ms << ",\"merge_ms\":" << t.merge_ms
        << ",\"expansion_ms\":" << t.expansion_ms << ",\"conversion_ms\":" << t.conversion_ms
        << ",\"cleanup_ms\":" << t.cleanup_ms << ",\"total_ms\":" << t.total_ms << '}';
}
int main(int argc,char **argv) {
    try {
        require(argc==5);
        const auto rounds=std::stoi(argv[3]); const std::string mode=argv[4];
        require(rounds>=1 && rounds<=20 && (mode=="direct" || mode=="executor"));
        const auto start=Clock::now();
        const bool themes=std::string(argv[2])!="-";
        auto dictionary=WindowsDictionary::load(std::filesystem::u8path(argv[1]),
            themes ? std::filesystem::u8path(argv[2]) : std::filesystem::path{});
        std::cout << std::setprecision(10) << "{\"type\":\"ready\",\"generation\":\"" << dictionary->generation()
            << "\",\"load_ms\":" << elapsed(start) << "}\n";
        WindowsDictionary::QueryTimings timings;
        // Exactly one outstanding key, so publication through Executor's result
        // mutex also synchronizes this single worker's timings with the owner.
        std::unique_ptr<Executor> executor;
        if(mode=="executor") executor=std::make_unique<Executor>(1,1,[&](const auto &work) {
            return work->dictionary->query(work,&timings);
        });
        std::vector<Case> cases={{"nihao","你好",""},{"xinkule","辛苦了",""},{"riben","日本",""},
            {"eat","食べる",""},{"nihaoz","你好","z"}};
        if(themes) cases.push_back({"tejihuifuyao","特级恢复药",""});
        for(int round=0;round<rounds;++round) for(const auto &sample:cases) {
            ServerConnection server(42,dictionary); uint64_t sequence=0;
            auto frame=[&](Kind kind) { return Frame{kind,{1,42,1,1,++sequence},{}}; };
            auto hello=frame(Kind::Hello); hello.id.engine=hello.id.session=hello.id.composition=0;
            require(server.receive(hello,Executor::now(),Executor::now()+60000).response.has_value());
            require(server.receive(frame(Kind::Open),Executor::now(),Executor::now()+60000).response.has_value());
            CandidatePage final; ServerConnection::Ticket last;
            const std::string input=sample.input;
            for(size_t key=0;key<input.size();++key) {
                auto request=frame(Kind::Key); request.payload=encodePayload(KeyMessage{KeyAction::Text,static_cast<uint8_t>(input[key]),true});
                const auto began=Clock::now(); const auto now=Executor::now();
                auto scheduled=server.receive(request,now,now+60000); require(bool(scheduled.work));
                last=scheduled.work; ServerConnection::Result response;
                if(executor) {
                    require(executor->submit(last)==Executor::Admission::Accepted);
                    auto result=executor->takeUntil(Clock::now()+std::chrono::seconds(60));
                    require(result && result->work==last && result->page);
                    final=std::move(*result->page); response=server.complete(last,final,Executor::now());
                } else { final=dictionary->query(last,&timings); response=server.complete(last,final,Executor::now()); }
                const auto requestMs=elapsed(began);
                require(response.status==ServerConnection::Status::Reply && response.response.has_value());
                CandidatePage decoded; require(decodePayload(response.response->payload,decoded));
                require(encodePayload(decoded)==encodePayload(final) && final.input==input.substr(0,key+1));
                std::cout << "{\"type\":\"sample\",\"case\":\"" << sample.input << "\",\"run\":" << round
                    << ",\"key\":" << key+1 << ",\"request_ms\":" << requestMs << ",\"query\":";
                print(timings); std::cout << "}\n";
            }
            auto found=std::find_if(final.words.begin(),final.words.end(),[&](const auto &word) { return word.text==sample.expected; });
            require(found!=final.words.end());
            auto select=frame(Kind::Select); select.payload=encodePayload(Selection{last->request.id.request,found->id});
            const auto now=Executor::now(); auto result=server.receive(select,now,now+60000); Commit commit;
            require(result.response && decodePayload(result.response->payload,commit));
            require(commit.text==sample.expected && commit.remaining==sample.remaining);
            auto replay=server.receive(select,now,now+60000);
            require(replay.status==ServerConnection::Status::Replay && encode(*replay.response)==encode(*result.response));
            std::cout << "{\"type\":\"commit\",\"case\":\"" << sample.input << "\",\"run\":" << round << ",\"verified\":true}\n";
        }
        if(executor) { executor->stop(); executor->join(); }
    } catch(const std::exception &) { std::cerr << "engine benchmark failed\n"; return 1; }
}
