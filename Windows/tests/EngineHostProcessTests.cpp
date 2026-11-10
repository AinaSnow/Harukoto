#include "../EngineWindows.h"
#include "../EngineProcess.h"
#include "EngineHostTestClient.h"
#include <set>
#include <iostream>

using namespace host_test;
// The test only supplies a blocking harness around the reusable I/O-thread owner.
struct Child {
    EngineProcess manager;
    Child(const std::filesystem::path &executable, const std::filesystem::path &base,
            const std::filesystem::path &themes, const std::filesystem::path &personal, bool valid = true)
        : manager(EngineProcess::Config{executable,base,themes,personal}) {
        while (manager.poll(ep::now()) == EngineProcess::State::Starting)
            wait(manager.waitHandles(),manager.deadline());
        const auto status = manager.status();
        assert(status.state == (valid ? EngineProcess::State::Ready : EngineProcess::State::Exited));
        if (!valid) assert(status.failure == EngineProcess::Failure::StartupExit && status.exitCode && *status.exitCode != 0);
    }
    void finish(bool signal = true) {
        if (signal) manager.requestStop(ep::now());
        const auto deadline = ep::now()+Timeout;
        while (manager.poll(ep::now()) != EngineProcess::State::Exited) wait(manager.waitHandles(),deadline);
        const auto status = manager.status();
        assert(status.failure == EngineProcess::Failure::None && status.exitCode == DWORD(0));
    }
};
static void compare(const WindowsDictionary &dictionary, const Client &client, bool hans = true,
        uint32_t index = 0, std::string context = {}, bool locked = false) {
    auto work = std::make_shared<ServerConnection::Work>();
    work->input = client.current.input; work->key = {KeyAction::Refresh,0,hans}; work->page = index;
    work->context = std::move(context); work->chineseLocked = locked;
    work->dictionary = std::shared_ptr<const WindowsDictionary>(&dictionary,[](auto *) {});
    assert(encodePayload(dictionary.query(work)) == encodePayload(client.current));
}
static void choose(Client &client, const std::string &input, const std::string &text, bool hans = true) {
    client.cancel(); client.type(input,hans);
    const auto commit = client.choose(find(client.current,text));
    assert(commit.text == text && commit.remaining.empty() && commit.consumed == input.size());
}
static bool writable(const std::filesystem::path &file) {
    ep::Handle handle(CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)); return bool(handle);
}
int wmain(int argc, wchar_t **argv) {
    try {
        assert(argc == 5);
        const auto executable = std::filesystem::path(argv[1]), base = std::filesystem::path(argv[2]);
        const auto themes = std::filesystem::path(argv[3]), personal = std::filesystem::path(argv[4]);
        auto dictionary = WindowsDictionary::load(base,themes,personal);
        uint64_t generation = 0;
        {
            Child host(executable,base,themes,personal);
            assert(!writable(base/L"language-evidence.sqlite"));
            Client simplified(host.manager.claim(ep::now())), traditional(host.manager.claim(ep::now()));
            assert(!host.manager.claim(ep::now()) && host.manager.status().claimed == 2);
            simplified.type("zhongguo"); traditional.type("zhongguo",false); generation = simplified.last.id.engine;
            assert(simplified.session == traditional.session && generation == traditional.last.id.engine);
            compare(*dictionary,simplified); compare(*dictionary,traditional,false);
            assert(simplified.choose(find(simplified.current,"中国")).text == "中国");
            assert(traditional.choose(find(traditional.current,"中國")).text == "中國");
            for (const auto &item : std::vector<std::pair<std::string,std::string>>{
                    {"riben","日本"},{"riben","にほん"},{"eat","食べる"},{"ceshibuci","旧补词"},
                    {"testcoffee","珈琲"},{"tejihuifuyao","特级恢复药"}}) {
                simplified.cancel(); simplified.type(item.first); compare(*dictionary,simplified);
                assert(simplified.choose(find(simplified.current,item.second)).text == item.second);
            }
            choose(traditional,"tejihuifuyao","特級恢復藥",false);
            simplified.cancel(); simplified.type("nihaoz"); compare(*dictionary,simplified);
            const auto word = find(simplified.current,"你好"); assert(word.consumed == 5);
            const auto request = simplified.selection(word), response = simplified.exchange(request); Commit commit;
            assert(simplified.candidates.confirm(request,response,commit) && simplified.gate.accept(response,ep::now()));
            assert(commit.text == "你好" && commit.remaining == "z");
            const auto duplicate = simplified.exchange(request);
            assert(duplicate.id == response.id && duplicate.payload == response.payload && !simplified.gate.accept(duplicate,ep::now()));
            assert(simplified.key(KeyAction::Refresh).input == "z"); compare(*dictionary,simplified,true,0,"你好",true);
            assert(simplified.key(KeyAction::Backspace).input.empty());
            simplified.cancel(); simplified.type("ceshi"); std::set<uint64_t> ids; std::set<std::string> words;
            uint32_t page = 0;
            for (;;) {
                compare(*dictionary,simplified,true,page);
                for (const auto &candidate : simplified.current.words) { assert(ids.insert(candidate.id).second); words.insert(candidate.text); }
                if (!simplified.current.more) break;
                assert(++page < 32); simplified.key(KeyAction::PageNext);
            }
            for (int i=0;i<45;++i) assert(words.count("测试"+std::to_string(i)));
            while (page) { --page; simplified.key(KeyAction::PagePrevious); compare(*dictionary,simplified,true,page); }
            simplified.cancel(); assert(simplified.key(KeyAction::Refresh).input.empty());
            traditional.channel.reset(); assert(simplified.type("riben").input == "riben");
            simplified.channel.reset(); host.finish(false); // All endpoints disconnected: automatic clean exit.
        }
        dictionary.reset(); assert(writable(base/L"language-evidence.sqlite"));
        {
            Child host(executable,base,themes,personal);
            Client client(host.manager.claim(ep::now())); client.type("riben"); assert(client.last.id.engine != generation);
            host.finish(); client.closed(); // Explicit stop while connected, no replay/restart.
        }
        {
            Child host(executable,base,themes,personal); Client client(host.manager.claim(ep::now()));
            ep::check(TerminateProcess(host.manager.processHandle(),77)); client.closed();
            assert(WaitForSingleObject(host.manager.processHandle(),Timeout) == WAIT_OBJECT_0);
            assert(host.manager.poll(ep::now()) == EngineProcess::State::Exited);
            assert(host.manager.status().failure == EngineProcess::Failure::UnexpectedExit &&
                host.manager.status().exitCode == DWORD(77) && !host.manager.claim(ep::now()));
        }
        { Child invalid(executable,base/L"missing",themes,personal,false); }
        assert(writable(base/L"language-evidence.sqlite"));
        std::cout << "engine_host_process: real per-key Chinese/Japanese/FF14, pagination, partial/replayed selection, stop/crash and startup failure passed\n";
        return 0;
    } catch (const ep::Error &error) { std::cerr << "host process failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
    catch (const std::exception &error) { std::cerr << "host process fixture failed: " << error.what() << '\n'; return 1; }
}
