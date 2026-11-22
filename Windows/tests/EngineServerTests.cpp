#include "../EngineServer.h"
#include "../PendingDelivery.h"
#include <cassert>
#include <iostream>
using namespace harukoto::engine;
using Status = ServerConnection::Status;
using Reason = ServerConnection::Reason;

struct Peer {
    ServerConnection server{42};
    uint64_t sequence = 0;
    Peer() {
        auto hello = frame(Kind::Hello,0,0); hello.id.engine = 0;
        auto response = server.receive(hello,0,100);
        assert(response.status == Status::Reply && response.response->kind == Kind::HelloAck);
        assert(server.receive(hello,0,100).status == Status::Replay);
    }
    Frame frame(Kind kind, uint64_t session = 1, uint64_t composition = 1) {
        return {kind,{1,42,session,composition,++sequence},{}};
    }
    Frame open(uint64_t session = 1) {
        auto request = frame(Kind::Open,session);
        assert(server.receive(request,0,100).status == Status::Reply); return request;
    }
    Frame key(char text, uint64_t session = 1, uint64_t composition = 1) {
        auto request = frame(Kind::Key,session,composition);
        request.payload = encodePayload(KeyMessage{KeyAction::Text,static_cast<uint8_t>(text),true}); return request;
    }
    Frame action(KeyAction action, uint64_t session = 1, uint64_t composition = 1) {
        auto request = frame(Kind::Key,session,composition);
        request.payload = encodePayload(KeyMessage{action,0,true}); return request;
    }
};
// A deterministic test backend. No real core decoding or host insertion here.
static CandidatePage page(const ServerConnection::Ticket &work, std::string text = "你好", uint32_t consumed = 0) {
    CandidatePage result{work->input,false,{}};
    if (!result.input.empty()) result.words.push_back({1,consumed ? consumed : static_cast<uint32_t>(result.input.size()),std::move(text),""});
    return result;
}
static Frame finish(Peer &peer, const Frame &request, std::string text = "你好", uint32_t consumed = 0) {
    auto job = peer.server.receive(request,0,100);
    assert(job.status == Status::Scheduled && job.work && !job.response);
    auto result = peer.server.complete(job.work,page(job.work,std::move(text),consumed),1);
    assert(result.status == Status::Reply && !result.work); return *result.response;
}
static void independentConnectionsAndSessions() {
    Peer a,b; a.open(); b.open(); a.open(2); b.open(2);
    auto ka = a.key('a'), kb = b.key('b'); assert(ka.id == kb.id);
    auto first = a.server.receive(ka,0,100), second = b.server.receive(kb,0,100);
    assert(first.work && second.work && first.work->input == "a" && second.work->input == "b");
    assert(a.server.complete(second.work,page(second.work),1).reason == Reason::Stale);
    auto cloned = std::make_shared<const ServerConnection::Work>(*first.work);
    assert(a.server.complete(cloned,page(cloned),1).reason == Reason::Stale);
    auto other = a.key('c',2); auto third = a.server.receive(other,0,100); assert(third.work);
    assert(a.server.complete(third.work,page(third.work),1).status == Status::Reply);
    assert(a.server.complete(first.work,page(first.work),1).status == Status::Reply);
    assert(b.server.complete(second.work,page(second.work),1).status == Status::Reply);
    auto ar = a.server.receive(a.action(KeyAction::Refresh),2,100);
    auto br = b.server.receive(b.action(KeyAction::Refresh),2,100);
    auto cr = a.server.receive(a.action(KeyAction::Refresh,2),2,100);
    assert(ar.work->input == "a" && br.work->input == "b" && cr.work->input == "c");
    a.server.disconnect(); assert(a.server.complete(ar.work,page(ar.work),3).status == Status::Closed);
    assert(b.server.complete(br.work,page(br.work),3).status == Status::Reply);
}
static void cancelAndClose() {
    Peer p; p.open();
    auto request = p.key('n'); auto pending = p.server.receive(request,0,100); assert(pending.work);
    assert(p.server.receive(request,1,100).status == Status::Pending);
    assert(!p.server.receive(request,1,100).work);
    auto conflict = request; conflict.payload = encodePayload(KeyMessage{KeyAction::Text,'x',true});
    assert(p.server.receive(conflict,1,100).reason == Reason::Conflict);
    assert(p.server.receive(p.key('x'),1,100).reason == Reason::Busy);
    auto cancel = p.frame(Kind::Cancel,1,2); assert(p.server.receive(cancel,1,100).status == Status::Reply);
    assert(p.server.receive(cancel,1,100).status == Status::Replay);
    assert(p.server.complete(pending.work,page(pending.work),2).reason == Reason::Stale);
    assert(p.server.receive(request,2,100).reason == Reason::Stale);
    auto refresh = p.action(KeyAction::Refresh,1,2); auto empty = p.server.receive(refresh,2,100);
    assert(empty.work && empty.work->input.empty());
    auto again = p.frame(Kind::Cancel,1,3); assert(p.server.receive(again,3,100).status == Status::Reply);
    assert(p.server.complete(empty.work,page(empty.work),3).reason == Reason::Stale);
    auto close = p.frame(Kind::Close,1,4); assert(p.server.receive(close,3,100).status == Status::Reply);
    assert(p.server.receive(close,3,100).status == Status::Replay && p.server.sessionCount() == 0);
    assert(p.server.receive(p.frame(Kind::Open,1),3,100).reason == Reason::Stale);
    p.open(2);
    auto next = p.server.receive(p.key('z',2),4,100); assert(next.work->input == "z");
    assert(p.server.complete(pending.work,page(pending.work),4).reason == Reason::Stale);
}
static void selectionAndReplay() {
    Peer p; p.open();
    for (char c : std::string("ribenniha")) finish(p,p.key(c));
    const auto candidates = finish(p,p.key('o'),"日本",5);
    auto select = p.frame(Kind::Select); select.payload = encodePayload(Selection{candidates.id.request,1});
    auto bad = select; bad.payload = encodePayload(Selection{candidates.id.request-1,1});
    assert(p.server.receive(bad,2,100).reason == Reason::Candidate);
    auto accepted = p.server.receive(select,2,100); assert(accepted.status == Status::Reply);
    Commit commit; assert(decodePayload(accepted.response->payload,commit));
    assert(commit.text == "日本" && commit.remaining == "nihao" && commit.consumed == 5);
    for (int i=0;i<10;++i) {
        auto duplicate = p.server.receive(select,3,100);
        assert(duplicate.status == Status::Replay && encode(*duplicate.response) == encode(*accepted.response));
    }
    bad = select; bad.payload = encodePayload(Selection{candidates.id.request,2});
    assert(p.server.receive(bad,3,100).reason == Reason::Conflict);
    auto stale = p.frame(Kind::Select); stale.payload = select.payload;
    assert(p.server.receive(stale,3,100).reason == Reason::Candidate);
    auto refresh = p.action(KeyAction::Refresh);
    auto job = p.server.receive(refresh,3,100); assert(job.work->input == "nihao");
    assert(p.server.complete(job.work,page(job.work),4).status == Status::Reply);
    assert(p.server.complete(job.work,page(job.work),4).reason == Reason::Stale);
    auto backspace = p.action(KeyAction::Backspace); auto trimmed = p.server.receive(backspace,4,100);
    assert(trimmed.work->input == "niha"); assert(p.server.complete(trimmed.work,page(trimmed.work),4).response);
    auto nextPage = p.server.receive(p.action(KeyAction::PageNext),4,100); assert(nextPage.work->input == "niha");
    assert(nextPage.work->key.action == KeyAction::PageNext);
    assert(p.server.complete(nextPage.work,page(nextPage.work),4).response);
    // Evicted completed requests stay stale, never executed again.
    for (size_t i=0;i<ServerConnection::ReplayLimit+2;++i) finish(p,p.action(KeyAction::Refresh));
    assert(p.server.cachedReplies() == ServerConnection::ReplayLimit);
    assert(p.server.receive(select,5,100).reason == Reason::Stale);
    auto after = p.server.receive(p.action(KeyAction::Refresh),5,100); assert(after.work->input == "niha");
    assert(p.server.complete(after.work,page(after.work),5).response);
    // Pre-step-7: KeyAction::Set replaces the reading and resets page in one request.
    auto nextForSet = p.server.receive(p.action(KeyAction::PageNext),5,100);
    assert(nextForSet.work && nextForSet.work->page == 2);
    assert(p.server.complete(nextForSet.work,page(nextForSet.work),5).response);
    auto setFrame = p.frame(Kind::Key);
    setFrame.payload = encodePayload(KeyMessage{KeyAction::Set,0,true,false,{},"zhongguorenmin"});
    auto setJob = p.server.receive(setFrame,5,100);
    assert(setJob.work && setJob.work->input == "zhongguorenmin" && setJob.work->page == 0);
    assert(p.server.complete(setJob.work,page(setJob.work,"中国人民",14),5).response);
    auto setPageNext = p.server.receive(p.action(KeyAction::PageNext),5,100);
    assert(setPageNext.work && setPageNext.work->input == "zhongguorenmin" && setPageNext.work->page == 1);
    assert(p.server.complete(setPageNext.work,page(setPageNext.work,"中国人民",14),5).response);
    // Step 7c: KeyAction::Inspect preserves session input, page and candidate snapshot.
    auto inspectFrame = p.frame(Kind::Key);
    inspectFrame.payload = encodePayload(KeyMessage{KeyAction::Inspect,0,true,false,{},"zhongguo","中国","你好"});
    auto inspectJob = p.server.receive(inspectFrame,5,100);
    assert(inspectJob.work && inspectJob.work->input == "zhongguo" && inspectJob.work->key.action == KeyAction::Inspect);
    CandidatePage inspectReply{"zhongguo",false,{},false,{},true,"zhong1guo2\t中国\n","你好中國"};
    auto inspectDone = p.server.complete(inspectJob.work,inspectReply,5);
    assert(inspectDone.status == Status::Reply && inspectDone.response);
    CandidatePage inspectDecoded; assert(decodePayload(inspectDone.response->payload,inspectDecoded) && inspectDecoded.inspect && inspectDecoded.nextContext == "你好中國");
    // Session's previous page snapshot is still selectable after Inspect!
    auto setSelect = p.frame(Kind::Select); setSelect.payload = encodePayload(Selection{setPageNext.work->request.id.request,1});
    auto setSelected = p.server.receive(setSelect,5,100); Commit setCommit;
    assert(setSelected.response && decodePayload(setSelected.response->payload,setCommit) && setCommit.text == "中国人民" && setCommit.remaining.empty());
    auto cancel = p.frame(Kind::Cancel,1,2); assert(p.server.receive(cancel,6,100).response);
    assert(p.server.receive(refresh,6,100).reason == Reason::Stale);
}
static void faultsAndBounds() {
    Peer p; auto open = p.open();
    auto malformed = p.key('n'); malformed.payload.push_back(0);
    assert(p.server.receive(malformed,0,100).reason == Reason::Malformed);
    auto key = p.key('n'); auto mismatch = key; ++mismatch.id.engine;
    assert(p.server.receive(mismatch,0,100).reason == Reason::Identity);
    mismatch = key; ++mismatch.id.connection; assert(p.server.receive(mismatch,0,100).reason == Reason::Identity);
    mismatch = key; ++mismatch.id.composition; assert(p.server.receive(mismatch,0,100).reason == Reason::Composition);
    auto pending = p.server.receive(key,0,10); assert(pending.work->input == "n");
    auto badPage = page(pending.work); badPage.input = "x";
    auto failed = p.server.complete(pending.work,badPage,1); ErrorMessage error;
    assert(failed.status == Status::Failed && failed.response->kind == Kind::Error);
    assert(decodePayload(failed.response->payload,error) && error.code == ErrorCode::Backend);
    assert(p.server.closed() && p.server.sessionCount() == 0 && p.server.cachedReplies() == 0);
    assert(p.server.receive(open,11,100).status == Status::Closed);
    Peer malformedResult; malformedResult.open();
    auto badWork = malformedResult.server.receive(malformedResult.key('n'),0,10).work;
    badPage = page(badWork); badPage.words[0].text = std::string("\xff",1);
    assert(malformedResult.server.complete(badWork,badPage,1).status == Status::Failed);
    Peer timeout; timeout.open(); auto expiring = timeout.key('n');
    auto timed = timeout.server.receive(expiring,0,10); assert(timed.work);
    assert(timeout.server.receive(expiring,1,100).status == Status::Pending);
    assert(!timeout.server.expire(9));
    assert(timeout.server.complete(timed.work,page(timed.work),10).status == Status::Closed);
    Peer reopened; reopened.open(); auto fresh = reopened.server.receive(reopened.key('z'),0,100);
    assert(reopened.server.complete(pending.work,page(pending.work),1).reason == Reason::Stale);
    assert(reopened.server.complete(fresh.work,page(fresh.work),1).response);
    Peer capacity;
    for (uint64_t i=1;i<=MaxSessions;++i) capacity.open(i);
    assert(capacity.server.receive(capacity.frame(Kind::Open,MaxSessions+1),0,100).reason == Reason::Capacity);
    Peer input; input.open();
    for (size_t i=0;i<MaxInput;++i) finish(input,input.key('a'));
    assert(input.server.receive(input.key('a'),2,100).reason == Reason::InputLimit);
    Peer jobs; jobs.open(); std::vector<ServerConnection::Ticket> held;
    uint64_t composition = 1;
    for (size_t i=0;i<ServerConnection::WorkLimit;++i) {
        auto scheduled = jobs.server.receive(jobs.key('a',1,composition),0,100); assert(scheduled.work);
        held.push_back(scheduled.work);
        assert(jobs.server.receive(jobs.frame(Kind::Cancel,1,++composition),0,100).response);
    }
    auto retry = jobs.key('b',1,composition);
    assert(jobs.server.receive(retry,0,100).reason == Reason::Capacity);
    held.clear(); assert(jobs.server.receive(retry,0,100).work);
}
static void admissionAndLateResults() {
    ServerConnection blank(42);
    Frame beforeHello{Kind::Open,{1,42,1,1,1},{}};
    assert(blank.receive(beforeHello,0,100).reason == Reason::Malformed);
    Frame badHello{Kind::Hello,{1,0,0,0,1},{1}};
    assert(blank.receive(badHello,0,100).reason == Reason::Malformed);
    Peer p; p.open();
    auto original = p.server.receive(p.key('a'),0,100); assert(original.work);
    auto malformedCancel = p.frame(Kind::Cancel,1,2); malformedCancel.payload = {1};
    assert(p.server.receive(malformedCancel,0,100).reason == Reason::Malformed);
    auto noAdvance = p.frame(Kind::Cancel);
    assert(p.server.receive(noAdvance,0,100).reason == Reason::Composition);
    assert(p.server.complete(original.work,page(original.work),1).response);
    uint64_t composition = 1;
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto oldKey = p.key('b',1,composition);
        auto old = p.server.receive(oldKey,2,100); assert(old.work);
        auto cancel = p.frame(Kind::Cancel,1,++composition);
        assert(p.server.receive(cancel,3,100).response);
        auto newKey = p.key('c',1,composition);
        newKey.payload = encodePayload(KeyMessage{KeyAction::Text,'c',false});
        auto fresh = p.server.receive(newKey,4,100);
        assert(fresh.work && fresh.work->input == "c" && !fresh.work->key.simplified);
        assert(p.server.complete(fresh.work,page(fresh.work,"新"),5).response);
        assert(p.server.complete(old.work,page(old.work,"旧"),6).reason == Reason::Stale);
        auto select = p.frame(Kind::Select,1,composition);
        select.payload = encodePayload(Selection{newKey.id.request,1});
        auto selected = p.server.receive(select,7,100); Commit actual;
        assert(selected.response && decodePayload(selected.response->payload,actual));
        assert(actual.text == "新" && actual.remaining.empty());
    }
    auto pending = p.server.receive(p.key('x',1,composition),8,100); assert(pending.work);
    auto close = p.frame(Kind::Close,1,++composition);
    assert(p.server.receive(close,9,100).response);
    assert(p.server.complete(pending.work,page(pending.work),10).reason == Reason::Stale);
    p.open(2);
    auto untouched = p.server.receive(p.action(KeyAction::Refresh,2),11,100);
    assert(untouched.work && untouched.work->input.empty());
    assert(p.server.complete(untouched.work,page(untouched.work),12).response);
}
static void clientRoundTrip() {
    ClientGate client; ServerConnection server(42);
    auto hello = client.connect(0,100); auto ack = server.receive(hello,0,100);
    assert(client.acknowledge(*ack.response,0));
    auto opened = client.open(0,100); auto answer = server.receive(*opened,0,100);
    assert(client.accept(*answer.response,0));
    CandidateSnapshot snapshot; uint64_t source = 0;
    for (char c : std::string("nihao")) {
        auto key = client.request(opened->id.session,Kind::Key,0,100); assert(key);
        key->payload = encodePayload(KeyMessage{KeyAction::Text,static_cast<uint8_t>(c),true});
        snapshot.invalidate(); auto job = server.receive(*key,0,100); assert(job.work);
        auto result = server.complete(job.work,page(job.work),1);
        auto staged = snapshot; assert(staged.publish(*result.response));
        assert(client.accept(*result.response,1)); snapshot = std::move(staged); source = key->id.request;
    }
    auto select = client.request(opened->id.session,Kind::Select,2,100); assert(select);
    select->payload = encodePayload(Selection{source,1}); auto result = server.receive(*select,2,100);
    Commit commit; assert(snapshot.validateConfirmation(*select,*result.response,commit));
    assert(client.accept(*result.response,2) && snapshot.confirm(*select,*result.response,commit));
    assert(commit.text == "你好" && commit.remaining.empty());
    PendingDelivery original, other; original.append(L"你好");
    auto repeated = server.receive(*select,3,100); assert(repeated.status == Status::Replay);
    assert(!client.accept(*repeated.response,3));
    original.suspend(); snapshot.invalidate(); auto cancel = client.cancel(opened->id.session,4,100);
    auto canceled = server.receive(*cancel,4,100); assert(client.accept(*canceled.response,4));
    assert(server.receive(*select,4,100).reason == Reason::Stale);
    assert(!original.empty() && original.begin() == nullptr && other.empty());
    original.resume(); assert(*original.begin() == L"你好"); original.finish(true); assert(original.empty());
}
int main() {
    independentConnectionsAndSessions(); cancelAndClose(); selectionAndReplay(); faultsAndBounds();
    admissionAndLateResults(); clientRoundTrip();
    std::cout << "PASS: server connection isolation, cancellation, replay bounds, stale work and client round trip\n";
}
