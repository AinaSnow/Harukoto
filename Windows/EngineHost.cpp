#include "EngineHost.h"
#include "EngineWindows.h"
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>

// WIN-03 step 6d: what the host should serve now. base is the newest
// published snapshot of --base-store, else --base (the version folder);
// themes only while a package snapshot is current; the personal table by its
// file stamp. key changes whenever any of them does.
struct Sources { std::filesystem::path base, themes; std::wstring key; };
static std::string themeGeneration(const std::filesystem::path &root) {
    std::ifstream pointer(root / "CURRENT", std::ios::binary);
    char text[33]{}; pointer.read(text, 32);
    return pointer.gcount() == 32 ? std::string(text, 32) : std::string();
}
static Sources currentSources(const std::filesystem::path &base, const std::filesystem::path &store,
                              const std::filesystem::path &themes, const std::filesystem::path &personal) {
    Sources s; s.base = base;
    if (!store.empty()) try { s.base = store / base_dictionary::currentGeneration(store); } catch (...) {}
    std::string generation;
    if (!themes.empty() && !(generation = themeGeneration(themes)).empty()) s.themes = themes;
    std::string stamp = "absent";
    std::error_code error;
    if (!personal.empty() && std::filesystem::exists(personal, error)) {
        const auto size = std::filesystem::file_size(personal, error);
        const auto time = std::filesystem::last_write_time(personal, error).time_since_epoch().count();
        stamp = std::to_string(size) + ":" + std::to_string(time);
    }
    s.key = s.base.wstring() + L"|" + std::wstring(generation.begin(), generation.end()) + L"|" + std::wstring(stamp.begin(), stamp.end());
    return s;
}

using namespace harukoto::engine;
// This is an experimental developer executable, not installed or auto-started.
int wmain(int argc, wchar_t **argv) {
    try {
        std::filesystem::path base, baseStore, themes, personal, memory, tracePath;
        Host::Options options; bool threadsSet = false, connectionsSet = false;
        std::vector<std::wstring> names;
        pipe::Handle ready, stop, ownerLease;
        std::array<pipe::Handle,3> privateControl;
        auto control = [](const std::wstring &value) {
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::invalid_argument("invalid inherited control handle");
            const auto number = std::stoull(value);
            if (!number || number >= std::numeric_limits<uintptr_t>::max()) throw std::invalid_argument("invalid inherited control handle");
            const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number));
            DWORD flags = 0; pipe::check(GetHandleInformation(handle,&flags));
            if (!(flags & HANDLE_FLAG_INHERIT)) throw std::invalid_argument("control handle is not inherited");
            return handle;
        };
        auto ownControl = [&](pipe::Handle &slot, const std::wstring &value) {
            const auto handle = control(value);
            if (slot || handle == ready.get() || handle == stop.get() || handle == ownerLease.get() ||
                    std::any_of(privateControl.begin(),privateControl.end(),[&](const auto &entry) { return entry.get() == handle; }))
                throw std::invalid_argument("duplicate host control");
            slot.reset(handle);
        };
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (++i >= argc) throw std::invalid_argument("missing host option");
            const std::wstring value = argv[i];
            if (option == L"--base" && base.empty()) base = value;
            else if (option == L"--themes" && themes.empty()) themes = value;
            else if (option == L"--personal" && personal.empty()) personal = value;
            else if (option == L"--memory" && memory.empty()) memory = value;
            else if (option == L"--base-store" && baseStore.empty()) baseStore = value;
            else if (option == L"--trace" && tracePath.empty()) tracePath = value;
            else if (option == L"--threads" && !threadsSet) {
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::invalid_argument("invalid host thread count");
                const auto number = std::stoull(value);
                if (!number || number > 16) throw std::invalid_argument("invalid host thread count");
                options.threads = static_cast<size_t>(number); threadsSet = true;
            }
            else if (option == L"--pipe" && names.size() < Host::MaxConnections) names.push_back(value);
            else if (option == L"--connections" && !connectionsSet) {
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) throw std::invalid_argument("invalid connection limit");
                const auto number = std::stoull(value);
                if (!number || number > Host::MaxConnections) throw std::invalid_argument("invalid connection limit");
                options.connections = static_cast<size_t>(number); connectionsSet = true;
            }
            else if (option == L"--ready") ownControl(ready,value);
            else if (option == L"--stop") ownControl(stop,value);
            else if (option == L"--owner-lease") ownControl(ownerLease,value);
            else if (option == L"--control-section") ownControl(privateControl[0],value);
            else if (option == L"--control-request") ownControl(privateControl[1],value);
            else if (option == L"--control-response") ownControl(privateControl[2],value);
            else throw std::invalid_argument("invalid host option");
        }
        if (base.empty() || !base.is_absolute() || (!themes.empty() && !themes.is_absolute()) ||
                (!personal.empty() && !personal.is_absolute()) ||
                (!memory.empty() && !memory.is_absolute()) || (!baseStore.empty() && !baseStore.is_absolute()) || !ready || !stop || ready.get() == stop.get())
            throw std::invalid_argument("invalid host startup");
        const auto controlCount = std::count_if(privateControl.begin(),privateControl.end(),[](const auto &handle) { return bool(handle); });
        if ((controlCount != 0 && controlCount != 3) || connectionsSet != (controlCount == 3) || (controlCount && !names.empty()) ||
                (ownerLease && controlCount != 3))
            throw std::invalid_argument("invalid private host control");
        std::unique_ptr<control::Server> endpointControl;
        if (controlCount) endpointControl = std::make_unique<control::Server>(std::move(privateControl));
        pipe::Handle traceFile;
        std::unique_ptr<EngineTraceBuffer> traces;
        if (!tracePath.empty()) {
            if (!tracePath.is_absolute()) throw std::invalid_argument("invalid trace path");
            // Refuse to overwrite any existing file, including dictionary files.
            traceFile.reset(CreateFileW(tracePath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            pipe::check(bool(traceFile)); traces = std::make_unique<EngineTraceBuffer>();
        }
        // Prevent accidental leakage if future code launches a child. Never use
        // these command-line handle values as a substitute for caller trust.
        pipe::check(SetHandleInformation(ready.get(),HANDLE_FLAG_INHERIT,0));
        pipe::check(SetHandleInformation(stop.get(),HANDLE_FLAG_INHERIT,0));
        if (ownerLease) {
            pipe::check(SetHandleInformation(ownerLease.get(),HANDLE_FLAG_INHERIT,0));
            // Exactly one inherited existence lease is escrowed until actual
            // process termination, even on exceptions or owner Job teardown.
            // Closing at stack unwind would permit a new host before we exit.
            (void)ownerLease.release();
        }
        uint64_t generation = 0;
        do {
            if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&generation),sizeof(generation),BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("engine generation failure");
        } while (!generation);
        // Step 6d: the same rule as the TSF service: a published snapshot if
        // there is one, otherwise the version folder; theme packages while on.
        auto sources = currentSources(base,baseStore,themes,personal);
        std::shared_ptr<const WindowsDictionary> dictionary;
        try { dictionary = WindowsDictionary::load(sources.base,sources.themes,personal,memory); }
        catch (...) {
            if (sources.base == base) throw;
            sources.base = base; // A damaged publication: the version folder, as the service keeps.
            dictionary = WindowsDictionary::load(sources.base,sources.themes,personal,memory);
        }
        options.identity = dictionary->identity();
        Host::Observer observer;
        if (traces) observer = [&](const EngineTrace &row) { traces->append(row); };
        Host host(names,dictionary,generation,[](const ServerConnection::Ticket &work) {
            if (!work->dictionary) throw std::invalid_argument("missing engine snapshot");
            return work->dictionary->query(work);
        },options,std::move(observer),std::move(endpointControl));
        // Step 6d: adopt newly published data. Load and verify everything on
        // this thread, then hand it to the host, which switches between
        // requests; a failed load keeps the old data (retried when a source
        // changes again).
        std::atomic<bool> quit{false};
        std::thread reload;
        if (!baseStore.empty() || !themes.empty() || !personal.empty()) reload = std::thread([&] {
            std::wstring loaded = sources.key;
            while (!quit.load()) {
                for (int i = 0; i < 10 && !quit.load(); ++i) Sleep(100);
                if (quit.load()) break;
                const auto next = currentSources(base,baseStore,themes,personal);
                if (next.key == loaded) continue;
                loaded = next.key;
                try {
                    auto fresh = WindowsDictionary::load(next.base,next.themes,personal,memory);
                    host.offer(fresh,fresh->identity());
                } catch (...) {}
            }
        });
        try { host.run(stop.get(),ready.get()); } catch (...) { quit = true; if (reload.joinable()) reload.join(); throw; }
        quit = true;
        if (reload.joinable()) reload.join();
        if (traces) {
            // No file writes in the I/O/worker query path. A dropped trace makes
            // the benchmark reject this run, without changing service replies.
            std::ostringstream data;
            data << "{\"type\":\"trace_status\",\"engine\":" << generation << ",\"threads\":" << options.threads
                << ",\"records\":" << traces->rows().size() << ",\"dropped\":" << traces->dropped() << "}\n";
            for (const auto &row : traces->rows()) {
                const auto &id = row.id; const auto &t = row.timings;
                data << "{\"type\":\"trace\",\"endpoint\":" << row.endpoint << ",\"engine\":" << id.engine
                    << ",\"connection\":" << id.connection << ",\"session\":" << id.session << ",\"composition\":" << id.composition
                    << ",\"request\":" << id.request << ",\"submitted\":" << t.submitted << ",\"started\":" << t.started
                    << ",\"finished\":" << t.finished << ",\"published\":" << t.published << ",\"taken\":" << t.taken
                    << ",\"enqueued\":" << row.enqueued << "}\n";
            }
            const auto text = data.str(); size_t position = 0;
            while (position < text.size()) {
                DWORD written = 0; const auto bytes = static_cast<DWORD>(std::min<size_t>(text.size()-position,65536));
                pipe::check(WriteFile(traceFile.get(),text.data()+position,bytes,&written,nullptr));
                if (!written) throw std::runtime_error("trace write failed"); position += written;
            }
        }
        return 0;
    } catch (const pipe::Error &error) {
        std::cerr << "engine_host failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n';
    } catch (...) { std::cerr << "engine_host: startup or execution failed\n"; }
    return 1;
}
