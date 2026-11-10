#include "EngineHost.h"
#include "EngineWindows.h"
#include <iostream>
#include <sstream>

using namespace harukoto::engine;
// This is an experimental developer executable, not installed or auto-started.
int wmain(int argc, wchar_t **argv) {
    try {
        std::filesystem::path base, themes, personal, tracePath;
        Host::Options options; bool threadsSet = false;
        std::vector<std::wstring> names;
        pipe::Handle ready, stop;
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
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (++i >= argc) throw std::invalid_argument("missing host option");
            const std::wstring value = argv[i];
            if (option == L"--base" && base.empty()) base = value;
            else if (option == L"--themes" && themes.empty()) themes = value;
            else if (option == L"--personal" && personal.empty()) personal = value;
            else if (option == L"--trace" && tracePath.empty()) tracePath = value;
            else if (option == L"--threads" && !threadsSet) {
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::invalid_argument("invalid host thread count");
                const auto number = std::stoull(value);
                if (!number || number > 16) throw std::invalid_argument("invalid host thread count");
                options.threads = static_cast<size_t>(number); threadsSet = true;
            }
            else if (option == L"--pipe" && names.size() < Host::MaxConnections) names.push_back(value);
            else if (option == L"--ready" && !ready) {
                const auto handle = control(value); if (handle == stop.get()) throw std::invalid_argument("duplicate host control"); ready.reset(handle);
            } else if (option == L"--stop" && !stop) {
                const auto handle = control(value); if (handle == ready.get()) throw std::invalid_argument("duplicate host control"); stop.reset(handle);
            }
            else throw std::invalid_argument("invalid host option");
        }
        if (base.empty() || !base.is_absolute() || (!themes.empty() && !themes.is_absolute()) ||
                (!personal.empty() && !personal.is_absolute()) || !ready || !stop || ready.get() == stop.get())
            throw std::invalid_argument("invalid host startup");
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
        uint64_t generation = 0;
        do {
            if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&generation),sizeof(generation),BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("engine generation failure");
        } while (!generation);
        auto dictionary = WindowsDictionary::load(base,themes,personal);
        Host::Observer observer;
        if (traces) observer = [&](const EngineTrace &row) { traces->append(row); };
        Host host(names,dictionary,generation,[](const ServerConnection::Ticket &work) {
            if (!work->dictionary) throw std::invalid_argument("missing engine snapshot");
            return work->dictionary->query(work);
        },options,std::move(observer));
        host.run(stop.get(),ready.get());
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
