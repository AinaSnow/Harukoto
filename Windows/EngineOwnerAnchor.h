#pragma once
#include "EnginePipe.h"

namespace harukoto::engine {
// One-time private bootstrap, published before owner-ready. The source handle
// belongs to the pinned, trusted owner process; it is never a public PID lookup.
class EngineOwnerAnchor {
    static constexpr size_t Size = 32;
    pipe::Handle section_;
    uint8_t *view_ = nullptr;
    bool published_ = false;
    void map() {
        view_ = static_cast<uint8_t *>(MapViewOfFile(section_.get(),FILE_MAP_READ | FILE_MAP_WRITE,0,0,Size));
        pipe::check(view_ != nullptr);
    }
public:
    EngineOwnerAnchor() {
        section_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,Size,nullptr));
        pipe::check(bool(section_)); map();
    }
    explicit EngineOwnerAnchor(pipe::Handle section) : section_(std::move(section)) {
        pipe::check(SetHandleInformation(section_.get(),HANDLE_FLAG_INHERIT,0)); map();
    }
    ~EngineOwnerAnchor() { if (view_) UnmapViewOfFile(view_); }
    EngineOwnerAnchor(const EngineOwnerAnchor &) = delete;
    EngineOwnerAnchor &operator=(const EngineOwnerAnchor &) = delete;
    HANDLE section() const { return section_.get(); }
    void publish(HANDLE engine) {
        if (published_) throw std::logic_error("engine owner anchor already published");
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),engine,GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
        pipe::Handle anchor(raw);
        std::vector<uint8_t> bytes{'H','K','O','A'}; put(bytes,1,4); put(bytes,reinterpret_cast<uintptr_t>(raw),8);
        bytes.resize(Size); std::copy(bytes.begin(),bytes.end(),view_);
        // Deliberate process-lifetime escrow: never explicitly close/reuse this
        // source slot, even when the host exits or the owner begins shutdown.
        // The OS closes this ONE query/wait handle when the owner terminates.
        // A launcher racing shutdown can only duplicate the same kernel object.
        anchor.release(); published_ = true;
    }
    pipe::Handle pin(HANDLE owner) const {
        if (WaitForSingleObject(owner,0) != WAIT_TIMEOUT) throw pipe::Error(pipe::Failure::Authentication,ERROR_PROCESS_ABORTED);
        std::array<uint8_t,Size> bytes{}; std::copy_n(view_,Size,bytes.begin());
        const auto number = get(bytes.data(),8,8);
        if (bytes[0] != 'H' || bytes[1] != 'K' || bytes[2] != 'O' || bytes[3] != 'A' || get(bytes.data(),4,4) != 1 ||
                !number || number >= std::numeric_limits<uintptr_t>::max() ||
                std::any_of(bytes.begin()+16,bytes.end(),[](auto b) { return b != 0; }))
            throw pipe::Error(pipe::Failure::Protocol,ERROR_INVALID_DATA);
        HANDLE raw = nullptr;
        pipe::check(DuplicateHandle(owner,reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number)),GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0));
        pipe::Handle engine(raw);
        if (!GetProcessId(engine.get()) || WaitForSingleObject(owner,0) != WAIT_TIMEOUT || WaitForSingleObject(engine.get(),0) != WAIT_TIMEOUT ||
                !(pipe::LogonIdentity::fromProcess(engine.get()) == pipe::LogonIdentity::fromProcess(GetCurrentProcess())))
            throw pipe::Error(pipe::Failure::Authentication,ERROR_ACCESS_DENIED);
        return engine;
    }
};
} // namespace harukoto::engine
