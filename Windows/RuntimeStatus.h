#pragma once
#include <windows.h>
#include <cstring>
#include <string>

// Read-only diagnostics for the same Windows session/user. No input text,
// window titles, document paths or personal dictionary contents are recorded.
// Mapping lifetime follows service instances; no files/registry writes per key.
class RuntimeStatus {
    struct Slot { volatile LONG owner; volatile LONG sequence; char json[2040]; };
    static_assert(sizeof(Slot) == 2048, "runtime protocol layout");
    static constexpr unsigned capacity = 64;
    HANDLE mapping = nullptr;
    Slot *slots = nullptr, *slot = nullptr;
    std::string last;
public:
    RuntimeStatus() = default;
    RuntimeStatus(const RuntimeStatus &) = delete;
    RuntimeStatus &operator=(const RuntimeStatus &) = delete;
    ~RuntimeStatus() { close(); }
    void close() noexcept {
        if (slot) InterlockedExchange(&slot->owner, 0);
        slot = nullptr;
        if (slots) UnmapViewOfFile(slots);
        if (mapping) CloseHandle(mapping);
        slots = nullptr; mapping = nullptr; last.clear();
    }
    bool open() noexcept {
        close();
        try {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
            const auto stamp = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            const auto name = L"Local\\Harukoto.Runtime.v2." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(stamp);
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, capacity * sizeof(Slot), name.c_str());
            if (!mapping) return false;
            slots = static_cast<Slot *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, capacity * sizeof(Slot)));
            if (!slots) { close(); return false; }
            for (unsigned i = 0; i < capacity; ++i) {
                // Negative owner is a reservation, not a readable record.
                if (InterlockedCompareExchange(&slots[i].owner, -1, 0) == 0) { slot = &slots[i]; return true; }
            }
        } catch (...) {}
        close(); return false;
    }
    void publish(const std::string &fields) noexcept {
        if (!slot || fields == last) return;
        try {
            const auto json = "{\"format\":1,\"thread_id\":" + std::to_string(GetCurrentThreadId()) +
                ",\"slot_id\":" + std::to_string(slot - slots) + ",\"observed_tick_ms\":" + std::to_string(GetTickCount64()) + "," + fields + "}";
            if (json.size() >= sizeof(slot->json)) return;
            // One writer per slot. Readers sample sequence/owner on both sides.
            // An interrupted write is rejected rather than reported as active.
            InterlockedIncrement(&slot->sequence);
            std::memset(slot->json, 0, sizeof(slot->json));
            std::memcpy(slot->json, json.data(), json.size());
            InterlockedIncrement(&slot->sequence);
            InterlockedExchange(&slot->owner, static_cast<LONG>(GetCurrentThreadId()));
            last = fields;
        } catch (...) {} // Diagnostics must never prevent typing.
    }
};
