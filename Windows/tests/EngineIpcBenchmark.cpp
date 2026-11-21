#include "../EngineWindows.h"
#include "../EngineProcess.h"
#include "EngineHostTestClient.h"
#include <psapi.h>
#include <fstream>
#include <iomanip>

using namespace host_test;
using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
static void require(bool value) { if (!value) throw std::runtime_error("IPC benchmark invariant failed"); }
static uint64_t counter() { LARGE_INTEGER value{}; ep::check(QueryPerformanceCounter(&value)); return value.QuadPart; }
static pipe::Handle event() {
    pipe::Handle value(CreateEventW(nullptr,TRUE,FALSE,nullptr)); ep::check(bool(value)); return value;
}
static std::wstring quote(const std::wstring &value) {
    std::wstring output = L"\""; size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(c == L'\"' ? slashes*2+1 : slashes,L'\\'); slashes = 0; output += c;
    }
    output.append(slashes*2,L'\\'); return output + L"\"";
}
static std::filesystem::path self() {
    std::wstring buffer(32768,L'\0');
    const auto size = GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    require(size && size < buffer.size()); buffer.resize(size); return buffer;
}
static pipe::Handle inherited(const wchar_t *text) {
    const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(text)));
    DWORD flags = 0; ep::check(GetHandleInformation(handle,&flags)); require(flags & HANDLE_FLAG_INHERIT);
    ep::check(SetHandleInformation(handle,HANDLE_FLAG_INHERIT,0)); return pipe::Handle(handle);
}
struct Case { const char *input, *expected, *remaining; };
struct Sample { const char *input; int round; size_t key; double ms; Identity id; };
static int worker(int argc, wchar_t **argv) {
    require(argc == 14);
    const std::filesystem::path base = argv[2], themes = std::wstring(argv[3]) == L"-" ? L"" : argv[3];
    const bool remote = std::wstring(argv[4]) != L"-";
    auto engine = remote ? inherited(argv[5]) : pipe::Handle{};
    auto start = inherited(argv[6]), ready = inherited(argv[7]), done = inherited(argv[8]), release = inherited(argv[9]);
    const auto runs = std::stoi(argv[10]), index = std::stoi(argv[11]);
    const std::wstring workload = argv[13]; require(workload == L"uniform" || workload == L"mixed");
    require(runs >= 1 && runs <= 20 && index >= 0 && index < 8);
    std::shared_ptr<const WindowsDictionary> dictionary;
    std::unique_ptr<ServerConnection> server;
    std::unique_ptr<Client> client;
    if (remote) client = std::make_unique<Client>(argv[4],engine.get());
    else {
        dictionary = WindowsDictionary::load(base,themes);
        server = std::make_unique<ServerConnection>(42,dictionary);
        client = std::make_unique<Client>(std::function<Frame(const Frame &)>([&](const Frame &frame) {
            auto result = server->receive(frame,ep::now(),ep::now()+Timeout);
            if (result.work) {
                const auto page = dictionary->query(result.work);
                result = server->complete(result.work,page,ep::now());
            }
            require(result.response.has_value()); return *result.response;
        }));
    }
    ep::check(SetEvent(ready.get())); require(WaitForSingleObject(start.get(),90000) == WAIT_OBJECT_0);
    const auto startedCounter = counter();
    std::vector<Case> cases{{"nihao","你好",""},{"xinkule","辛苦了",""},{"riben","日本",""},
        {"eat","食べる",""},{"nihaoz","你好","z"}};
    if (!themes.empty()) cases.push_back({"tejihuifuyao","特级恢复药",""});
    if (workload == L"mixed") {
        const std::string longCase = themes.empty() ? "xinkule" : "tejihuifuyao";
        cases.erase(std::remove_if(cases.begin(),cases.end(),[&](const Case &sample) {
            return index == 0 ? sample.input != longCase : sample.input == longCase;
        }),cases.end());
    }
    std::vector<Sample> samples; samples.reserve(38*runs);
    uint64_t generation = 0; std::vector<std::pair<const char *,int>> commits;
    for (int round=0;round<runs;++round) for (const auto &sample : cases) {
        client->cancel(); const std::string input = sample.input;
        for (size_t key=0;key<input.size();++key) {
            const auto began = Clock::now(); client->key(KeyAction::Text,static_cast<uint8_t>(input[key]));
            const auto ms = elapsed(began);
            require(client->current.input == input.substr(0,key+1));
            if (!generation) generation = client->last.id.engine;
            require(generation == client->last.id.engine);
            samples.push_back({sample.input,round,key+1,ms,client->last.id});
        }
        const auto commit = client->choose(find(client->current,sample.expected));
        require(commit.text == sample.expected && commit.remaining == sample.remaining &&
            commit.consumed == input.size()-std::string(sample.remaining).size()); commits.push_back({sample.input,round});
    }
    const auto finishedCounter = counter();
    std::ofstream output{std::filesystem::path(argv[12])}; require(bool(output)); output << std::setprecision(10);
    for (const auto &sample : samples) {
        const auto &id = sample.id;
        output << "{\"type\":\"sample\",\"client\":" << index
            << ",\"case\":\"" << sample.input << "\",\"run\":" << sample.round << ",\"key\":" << sample.key
            << ",\"request_ms\":" << sample.ms << ",\"engine\":" << id.engine << ",\"connection\":" << id.connection
            << ",\"session\":" << id.session << ",\"composition\":" << id.composition << ",\"request\":" << id.request << "}\n";
    }
    for (const auto &commit : commits) output << "{\"type\":\"commit\",\"client\":" << index
        << ",\"case\":\"" << commit.first << "\",\"run\":" << commit.second << ",\"verified\":true}\n";
    output << "{\"type\":\"client\",\"client\":" << index << ",\"engine\":" << generation
        << ",\"verified_commits\":" << commits.size() << ",\"started_counter\":" << startedCounter
        << ",\"finished_counter\":" << finishedCounter << "}\n";
    output.close(); require(bool(output)); ep::check(SetEvent(done.get()));
    // Keep dictionaries/channels alive for the held memory sweep, without
    // allocating candidate/output buffers after the coordinator's release.
    require(WaitForSingleObject(release.get(),90000) == WAIT_OBJECT_0); return 0;
}
struct Worker {
    pipe::Handle ready = event(), done = event(), process, job;
    std::filesystem::path output;
    Worker(const std::filesystem::path &base, const std::filesystem::path &themes,
            std::optional<EngineProcess::Endpoint> endpoint, HANDLE start, HANDLE release,
            int runs, int index, const std::filesystem::path &folder, const std::wstring &workload) : output(folder/(std::to_wstring(index)+L".jsonl")) {
        job.reset(CreateJobObjectW(nullptr,nullptr)); ep::check(bool(job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ep::check(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
        std::vector<pipe::Handle> copies;
        for (const auto handle : {start,ready.get(),done.get(),release}) {
            HANDLE raw = nullptr;
            ep::check(DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
            pipe::Handle copy(raw); copies.push_back(std::move(copy));
        }
        if (endpoint) {
            HANDLE raw = nullptr;
            ep::check(DuplicateHandle(GetCurrentProcess(),endpoint->process.get(),GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
            pipe::Handle copy(raw); copies.push_back(std::move(copy));
        }
        std::vector<HANDLE> handles; for (const auto &copy : copies) handles.push_back(copy.get());
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> bytes(size); auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        ep::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(HANDLE)*handles.size(),nullptr,nullptr));
        const auto executable = self();
        std::vector<std::wstring> args{executable.wstring(),L"--worker",base.wstring(),themes.empty() ? L"-" : themes.wstring(),
            endpoint ? endpoint->name : L"-",endpoint ? std::to_wstring(reinterpret_cast<uintptr_t>(handles[4])) : L"0"};
        for (size_t i=0;i<4;++i) args.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(handles[i])));
        args.insert(args.end(),{std::to_wstring(runs),std::to_wstring(index),output.wstring(),workload});
        std::wstring command; for (const auto &arg : args) { if (!command.empty()) command += L' '; command += quote(arg); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        ep::check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,executable.parent_path().c_str(),&startup.StartupInfo,&info));
        process.reset(info.hProcess); pipe::Handle thread(info.hThread);
        try {
            ep::check(AssignProcessToJobObject(job.get(),process.get())); copies.clear();
            if (ResumeThread(thread.get()) == DWORD(-1)) throw ep::Error(ep::Failure::Io,GetLastError());
        } catch (...) { TerminateProcess(process.get(),99); throw; }
    }
    ~Worker() { job.reset(); }
};
struct Memory {
    uint64_t controller = 0, clients = 0, engines = 0;
    uint64_t total() const { return controller+clients+engines; }
};
static uint64_t privateBytes(HANDLE process) {
    PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
    ep::check(GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),sizeof(counters)));
    return counters.PrivateUsage;
}
static void print(const Memory &m) {
    std::cout << "{\"controller\":" << m.controller << ",\"clients\":" << m.clients << ",\"engines\":" << m.engines
        << ",\"total\":" << m.total() << '}';
}
static void waitEngine(EngineProcess &engine) {
    while (engine.poll(ep::now()) == EngineProcess::State::Starting) {
        const auto events = engine.waitHandles(); const auto time = ep::now();
        const auto deadline = engine.deadline();
        require(WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,
            static_cast<DWORD>(deadline > time ? deadline-time : 0)) != WAIT_FAILED);
    }
    require(engine.status().state == EngineProcess::State::Ready);
}
static int coordinator(int argc, wchar_t **argv) {
    require(argc == 11);
    const std::filesystem::path host = argv[1], base = argv[2], themes = std::wstring(argv[3]) == L"-" ? L"" : argv[3], folder = argv[7];
    const auto count = std::stoi(argv[4]), runs = std::stoi(argv[5]); const std::wstring mode = argv[6];
    const auto threads = std::stoi(argv[8]); const bool profile = std::wstring(argv[9]) == L"1";
    const std::wstring workload = argv[10]; require(workload == L"uniform" || workload == L"mixed");
    require(host.is_absolute() && base.is_absolute() && folder.is_absolute() && (themes.empty() || themes.is_absolute()));
    require((count == 1 || count == 2 || count == 4 || count == 8) && runs >= 1 && runs <= 20);
    require(mode == L"local" || mode == L"isolated" || mode == L"shared");
    require(mode == L"local" ? threads == 0 && !profile : threads >= 1 && threads <= 16);
    require(std::wstring(argv[9]) == L"0" || std::wstring(argv[9]) == L"1");
    const auto began = Clock::now();
    auto start = event(), release = event();
    std::vector<std::unique_ptr<EngineProcess>> engines;
    const int engineCount = mode == L"local" ? 0 : mode == L"shared" ? 1 : count;
    for (int i=0;i<engineCount;++i) {
        EngineProcess::Config config{host,base,themes,{}};
        config.connections = mode == L"shared" ? count : 1;
        config.threads = static_cast<size_t>(threads);
        if (profile) config.trace = folder/(L"engine-"+std::to_wstring(i)+L".jsonl");
        engines.push_back(std::make_unique<EngineProcess>(config)); waitEngine(*engines.back());
    }
    std::vector<std::unique_ptr<Worker>> workers;
    for (int i=0;i<count;++i) {
        std::optional<EngineProcess::Endpoint> endpoint;
        if (engineCount) { endpoint = engines[mode == L"shared" ? 0 : i]->takeEndpoint(ep::now()); require(endpoint.has_value()); }
        workers.push_back(std::make_unique<Worker>(base,themes,std::move(endpoint),start.get(),release.get(),runs,i,folder,workload));
    }
    auto memory = [&] {
        Memory result; result.controller = privateBytes(GetCurrentProcess());
        for (const auto &w : workers) result.clients += privateBytes(w->process.get());
        for (const auto &e : engines) result.engines += privateBytes(e->processHandle());
        return result;
    };
    Memory peak; size_t samples = 0; double maxSweep = 0;
    auto sweep = [&] {
        const auto time = Clock::now(); const auto current = memory();
        maxSweep = std::max(maxSweep,elapsed(time)); ++samples;
        if (current.total() > peak.total()) peak = current;
        return current;
    };
    auto waitWorkers = [&](bool done, bool measuring) {
        const auto deadline = ep::now()+90000;
        for (;;) {
            std::vector<HANDLE> events; bool all = true;
            for (const auto &w : workers) {
                require(WaitForSingleObject(w->process.get(),0) == WAIT_TIMEOUT); events.push_back(w->process.get());
                const auto control = done ? w->done.get() : w->ready.get();
                const auto status = WaitForSingleObject(control,0); require(status != WAIT_FAILED);
                if (status != WAIT_OBJECT_0) { all = false; events.push_back(control); }
            }
            for (const auto &e : engines) { require(e->poll(ep::now()) == EngineProcess::State::Ready); events.push_back(e->processHandle()); }
            if (measuring) sweep();
            if (all) return;
            const auto time = ep::now(); require(time < deadline);
            require(WaitForMultipleObjects(static_cast<DWORD>(events.size()),events.data(),FALSE,
                static_cast<DWORD>(std::min<uint64_t>(deadline-time,measuring ? 10 : 1000))) != WAIT_FAILED);
        }
    };
    waitWorkers(false,false); const auto prepareMs = elapsed(began); const auto ready = sweep();
    LARGE_INTEGER frequency{}; ep::check(QueryPerformanceFrequency(&frequency));
    const auto workloadStart = Clock::now(); const auto releaseCounter = counter();
    ep::check(SetEvent(start.get())); waitWorkers(true,true);
    const auto wallMs = elapsed(workloadStart); const auto held = sweep();
    ep::check(SetEvent(release.get()));
    for (const auto &w : workers) {
        require(WaitForSingleObject(w->process.get(),10000) == WAIT_OBJECT_0);
        DWORD code = 0; ep::check(GetExitCodeProcess(w->process.get(),&code)); require(code == 0);
    }
    for (const auto &e : engines) e->requestStop(ep::now());
    for (const auto &e : engines) {
        const auto deadline = ep::now()+10000;
        while (e->poll(ep::now()) != EngineProcess::State::Exited) {
            require(ep::now() < deadline);
            require(WaitForSingleObject(e->processHandle(),10) != WAIT_FAILED);
        }
        require(e->status().failure == EngineProcess::Failure::None && e->status().exitCode == DWORD(0));
    }
    const char *modeName = mode == L"local" ? "local" : mode == L"isolated" ? "isolated" : "shared";
    const char *workloadName = workload == L"uniform" ? "uniform" : "mixed";
    std::cout << std::setprecision(10) << "{\"type\":\"group\",\"mode\":\"" << modeName
        << "\",\"clients\":" << count << ",\"runs\":" << runs << ",\"engine_processes\":" << engineCount
        << ",\"threads\":" << threads << ",\"profile\":" << (profile ? "true" : "false") << ",\"workload\":\"" << workloadName << '"'
        << ",\"prepare_ms\":" << prepareMs << ",\"wall_ms\":" << wallMs << ",\"release_counter\":" << releaseCounter
        << ",\"counter_frequency\":" << frequency.QuadPart << ",\"memory\":{\"ready\":";
    print(ready); std::cout << ",\"sampled_peak\":"; print(peak); std::cout << ",\"held\":"; print(held);
    std::cout << ",\"samples\":" << samples << ",\"interval_ms\":10,\"max_sweep_ms\":" << maxSweep << "}}\n";
    for (const auto &w : workers) {
        std::ifstream input(w->output); require(bool(input)); std::cout << input.rdbuf(); require(!input.bad());
    }
    if (profile) for (int i=0;i<engineCount;++i) {
        std::ifstream input(folder/(L"engine-"+std::to_wstring(i)+L".jsonl")); require(bool(input));
        std::string line;
        while (std::getline(input,line)) {
            require(!line.empty() && line.front() == '{');
            std::cout << "{\"engine_index\":" << i << ',' << line.substr(1) << '\n';
        }
        require(!input.bad());
    }
    require(bool(std::cout)); return 0;
}
int wmain(int argc, wchar_t **argv) {
    try { return argc > 1 && std::wstring(argv[1]) == L"--worker" ? worker(argc,argv) : coordinator(argc,argv); }
    catch (const ep::Error &error) { std::cerr << "engine IPC benchmark failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1; }
    catch (...) { std::cerr << "engine IPC benchmark failed\n"; return 1; }
}
