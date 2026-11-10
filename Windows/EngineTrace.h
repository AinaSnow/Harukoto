#pragma once
#include "EngineExecutor.h"

namespace harukoto::engine {
// Fixed metadata only. No input/candidate text, paths or exception details.
struct EngineTrace {
    size_t endpoint = 0;
    Identity id;
    Executor::Timings timings;
    uint64_t enqueued = 0;
};
class EngineTraceBuffer {
    std::vector<EngineTrace> rows_;
    size_t limit_, dropped_ = 0;
public:
    explicit EngineTraceBuffer(size_t limit = 8192) : limit_(limit) {
        if (!limit || limit > 65536) throw std::invalid_argument("invalid engine trace capacity");
        rows_.reserve(limit);
    }
    void append(const EngineTrace &row) noexcept {
        if (rows_.size() == limit_) { ++dropped_; return; }
        rows_.push_back(row); // Preallocated storage, trivial record, no I/O.
    }
    const std::vector<EngineTrace> &rows() const { return rows_; }
    size_t dropped() const { return dropped_; }
};
} // namespace harukoto::engine
