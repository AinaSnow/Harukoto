#include "../EngineWindows.h"
#include "EngineHostTestClient.h"
#include <set>
#include <iostream>

using namespace host_test;
// Quote one Windows argv element; no shell is involved.
static std::wstring quote(const std::wstring &value) {
    std::wstring output = L"\""; size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(c == L'\"' ? slashes*2+1 : slashes,L'\\'); slashes = 0; output += c;
    }
    output.append(slashes*2,L'\\'); return output + L"\"";
}
struct Child {
    ep::Handle ready, stop, process, job;
    std::vector<std::wstring> names{ep::newName(),ep::newName()};
    Child(const std::filesystem::path &executable, const std::filesystem::path &base,
            const std::filesystem::path &themes, const std::filesystem::path &personal, bool valid = true) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        ready.reset(CreateEventW(&security,TRUE,FALSE,nullptr)); stop.reset(CreateEventW(&security,TRUE,FALSE,nullptr));
        ep::check(bool(ready)); ep::check(bool(stop));
        job.reset(CreateJobObjectW(nullptr,nullptr)); ep::check(bool(job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ep::check(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size); auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        ep::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        HANDLE inherited[]{ready.get(),stop.get()};
        ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr));
        const std::vector<std::wstring> arguments{executable.wstring(),L"--base",base.wstring(),L"--themes",themes.wstring(),
            L"--personal",personal.wstring(),L"--ready",std::to_wstring(reinterpret_cast<uintptr_t>(ready.get())),
            L"--stop",std::to_wstring(reinterpret_cast<uintptr_t>(stop.get())),L"--pipe",names[0],L"--pipe",names[1]};
        std::wstring command;
        for (const auto &argument : arguments) { if (!command.empty()) command += L' '; command += quote(argument); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        ep::check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,nullptr,&startup.StartupInfo,&info));
        process.reset(info.hProcess); ep::Handle thread(info.hThread);
        try {
            ep::check(AssignProcessToJobObject(job.get(),process.get()));
            if (ResumeThread(thread.get()) == DWORD(-1)) throw ep::Error(ep::Failure::Io,GetLastError());
        } catch (...) { TerminateProcess(process.get(),99); WaitForSingleObject(process.get(),Timeout); throw; }
        HANDLE events[]{ready.get(),process.get()};
        assert(WaitForMultipleObjects(2,events,FALSE,30000) == (valid ? WAIT_OBJECT_0 : WAIT_OBJECT_0+1));
        if (!valid) {
            assert(WaitForSingleObject(ready.get(),0) == WAIT_TIMEOUT);
            DWORD code = 0; ep::check(GetExitCodeProcess(process.get(),&code)); assert(code != 0);
        }
    }
    ~Child() {
        SetEvent(stop.get());
        if (WaitForSingleObject(process.get(),Timeout) != WAIT_OBJECT_0) { TerminateProcess(process.get(),99); WaitForSingleObject(process.get(),Timeout); }
    }
    void finish(bool signal = true) {
        if (signal) ep::check(SetEvent(stop.get()));
        assert(WaitForSingleObject(process.get(),Timeout) == WAIT_OBJECT_0);
        DWORD code = 0; ep::check(GetExitCodeProcess(process.get(),&code)); assert(code == 0);
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
            Client simplified(host.names[0],host.process.get()), traditional(host.names[1],host.process.get());
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
            Client client(host.names[0],host.process.get()); client.type("riben"); assert(client.last.id.engine != generation);
            host.finish(); client.closed(); // Explicit stop while connected, no replay/restart.
        }
        {
            Child host(executable,base,themes,personal); Client client(host.names[0],host.process.get());
            ep::check(TerminateProcess(host.process.get(),77)); client.closed();
            assert(WaitForSingleObject(host.process.get(),Timeout) == WAIT_OBJECT_0);
        }
        { Child invalid(executable,base/L"missing",themes,personal,false); }
        assert(writable(base/L"language-evidence.sqlite"));
        std::cout << "engine_host_process: real per-key Chinese/Japanese/FF14, pagination, partial/replayed selection, stop/crash and startup failure passed\n";
        return 0;
    } catch (const ep::Error &error) { std::cerr << "host process failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
    catch (const std::exception &error) { std::cerr << "host process fixture failed: " << error.what() << '\n'; return 1; }
}
