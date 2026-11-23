#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RendererRelease.h"
#include "EngineDeployment.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// RH-3 / RH-4: candidate-window renderer handoff state machine and Win32 host
// (docs/RENDERER_HANDOFF.md).
//  - Machine: pure logic with injectable clock/window/process operations so all
//    cutover, cooldown, rollback, incompatibility and fallback transitions are
//    unit-tested without real processes or registry keys.
//  - LiveHost: Win32 implementation using a detached background checker (holds
//    a DLL module ref, caches SHA-256 verification per (sequence, file ID) and
//    releases EngineDeployment pins immediately after check or launch).
namespace harukoto::renderer::handoff {

constexpr uint64_t CheckIntervalMs = 2000;
constexpr uint64_t StandbyTimeoutMs = 5000;
constexpr uint64_t CooldownMs = 60000;
constexpr unsigned MaxConsecutiveRejects = 3;

// HKCU\Software\Meowpuut\Input DWORD rendererHandoff: 0 disables handoff
// (troubleshoot switch only; cannot specify paths). Absent / other: default on.
// Test harnesses that drive TextService directly may set the default off so
// they never probe the machine's HKLM approvals unless testing handoff.
inline bool rendererHandoffDefault = true;
inline bool readRendererHandoff(HKEY root = HKEY_CURRENT_USER) {
    DWORD value = 0, bytes = sizeof(value);
    const auto result = RegGetValueW(root, L"Software\\Meowpuut\\Input", L"rendererHandoff",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result == ERROR_SUCCESS ? value != 0 : rendererHandoffDefault;
}

using WindowId = uintptr_t;
using ProcessId = uintptr_t;

enum class SourceKind { Own, Desktop, Approved };

inline const char *sourceName(SourceKind kind) {
    switch (kind) {
    case SourceKind::Desktop: return "desktop";
    case SourceKind::Approved: return "approved";
    default: return "own";
    }
}

struct Approved {
    uint64_t sequence = 0;
    std::wstring id;
    std::filesystem::path exePath;
    std::shared_ptr<const void> token; // Verified image identity in Win32 host.
};

struct DesktopMatch {
    WindowId window = 0;
    uint64_t generation = 0;
    bool isOwn = false;
    uint64_t sequence = 0; // >0 when verified against approved; 0 when own.
};

struct Stats {
    SourceKind source = SourceKind::Own;
    uint64_t sequence = 0;
    uint16_t payloadVersion = release::PayloadVersion;
    uint64_t handoffs = 0;
    uint64_t failures = 0;
};

struct Ops {
    std::function<void(uint64_t now)> requestCheck;
    std::function<std::shared_ptr<const Approved>()> latestApproved;
    std::function<DesktopMatch(const Approved *approved, bool allowApproved)> probeDesktop;
    std::function<bool(WindowId window, uint64_t generation)> desktopAlive;
    std::function<ProcessId(const std::wstring &windowSuffix, const Approved *approved)> launch;
    std::function<bool(ProcessId process)> processAlive;
    std::function<WindowId(const std::wstring &windowName, ProcessId process)> findProcessWindow;
    std::function<void(WindowId window)> closeWindow;
    std::function<void(ProcessId process)> terminateProcess;
    std::function<void(ProcessId process)> releaseProcess;
    std::function<bool(WindowId window, const std::wstring &payload, uint16_t payloadVersion)> send;
};

class Machine {
    struct Active {
        enum class Kind { None, Own, Desktop, Approved } kind = Kind::None;
        WindowId window = 0;
        ProcessId process = 0;
        uint64_t generation = 0;
        uint64_t sequence = 0;
        std::wstring windowName;
        uint64_t startedAt = 0;
        bool everReady = false;
    };
    struct Standby {
        uint64_t sequence = 0;
        std::wstring windowName;
        ProcessId process = 0;
        uint64_t startedAt = 0;
    };

    Ops ops_;
    std::wstring baseId_;
    uint16_t payloadVersion_ = release::PayloadVersion;
    bool enabled_ = true;
    bool lastSentEmpty_ = true;
    bool fallbackToOwnAtBoundary_ = false;
    bool checkRequested_ = false;
    uint64_t lastCheckRequested_ = 0;
    unsigned consecutiveRejects_ = 0;
    Active current_;
    Standby standby_;
    std::unordered_map<uint64_t, uint64_t> cooldown_;
    Stats stats_;

    bool inCooldown(uint64_t sequence, uint64_t now) {
        if (!sequence) return false;
        const auto it = cooldown_.find(sequence);
        if (it == cooldown_.end()) return false;
        if (now < it->second) return true;
        cooldown_.erase(it);
        return false;
    }
    void coolDown(uint64_t sequence, uint64_t now) {
        if (sequence) cooldown_[sequence] = now + CooldownMs;
    }
    std::shared_ptr<const Approved> rawApproved() const {
        if (!enabled_ || !ops_.latestApproved) return nullptr;
        auto app = ops_.latestApproved();
        return (app && app->sequence) ? app : nullptr;
    }
    std::shared_ptr<const Approved> eligibleApproved(uint64_t now) {
        auto app = rawApproved();
        if (!app || inCooldown(app->sequence, now)) return nullptr;
        return app;
    }
    bool currentProcessAlive() const {
        return current_.process && (!ops_.processAlive || ops_.processAlive(current_.process));
    }
    bool currentDesktopAlive() const {
        return current_.window && (!ops_.desktopAlive || ops_.desktopAlive(current_.window, current_.generation));
    }
    void syncStats() {
        stats_.payloadVersion = payloadVersion_;
        if (current_.kind == Active::Kind::Desktop) {
            stats_.source = SourceKind::Desktop;
            stats_.sequence = current_.sequence;
        } else if (current_.kind == Active::Kind::Approved) {
            stats_.source = SourceKind::Approved;
            stats_.sequence = current_.sequence;
        } else {
            stats_.source = SourceKind::Own;
            stats_.sequence = 0;
        }
    }
    void closeCurrentPerProcess(bool sendHideFirst) {
        if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved) {
            if (current_.process) {
                if (currentProcessAlive()) {
                    WindowId w = current_.window;
                    if (!w && ops_.findProcessWindow) w = ops_.findProcessWindow(current_.windowName, current_.process);
                    if (w) {
                        if (sendHideFirst && ops_.send) ops_.send(w, L"", payloadVersion_);
                        if (ops_.closeWindow) ops_.closeWindow(w);
                    }
                }
                if (ops_.releaseProcess) ops_.releaseProcess(current_.process);
            }
        }
        current_ = {};
    }
    void cancelStandby(bool terminate) {
        if (!standby_.process) { standby_ = {}; return; }
        if (!ops_.processAlive || ops_.processAlive(standby_.process)) {
            if (terminate) {
                if (ops_.terminateProcess) ops_.terminateProcess(standby_.process);
            } else {
                WindowId w = ops_.findProcessWindow ? ops_.findProcessWindow(standby_.windowName, standby_.process) : 0;
                if (w && ops_.closeWindow) ops_.closeWindow(w);
                else if (ops_.terminateProcess) ops_.terminateProcess(standby_.process);
            }
        }
        if (ops_.releaseProcess) ops_.releaseProcess(standby_.process);
        standby_ = {};
    }
    bool launchPerProcess(const Approved *approved, uint64_t now, Active &out) {
        if (!ops_.launch) return false;
        const std::wstring suffix = approved
            ? (baseId_ + L"-s" + std::to_wstring(approved->sequence))
            : baseId_;
        const ProcessId proc = ops_.launch(suffix, approved);
        if (!proc) return false;
        out = {};
        out.kind = approved ? Active::Kind::Approved : Active::Kind::Own;
        out.process = proc;
        out.sequence = approved ? approved->sequence : 0;
        out.windowName = L"MeowpuutCandidates-" + suffix;
        out.startedAt = now;
        out.everReady = false;
        return true;
    }
    void promoteStandby(WindowId window, bool sendHideToOld, bool countHandoff) {
        if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved)
            closeCurrentPerProcess(sendHideToOld);
        else
            current_ = {};
        current_.kind = Active::Kind::Approved;
        current_.window = window;
        current_.process = standby_.process;
        current_.generation = 0;
        current_.sequence = standby_.sequence;
        current_.windowName = standby_.windowName;
        current_.startedAt = standby_.startedAt;
        current_.everReady = true;
        standby_ = {};
        consecutiveRejects_ = 0;
        if (countHandoff) ++stats_.handoffs;
        syncStats();
    }

public:
    Machine() = default;
    Machine(Ops ops, std::wstring baseId, uint16_t payloadVersion = release::PayloadVersion)
        : ops_(std::move(ops)), baseId_(std::move(baseId)), payloadVersion_(payloadVersion) {
        syncStats();
    }
    ~Machine() { shutdown(); }
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    void configure(Ops ops, std::wstring baseId, uint16_t payloadVersion = release::PayloadVersion) {
        shutdown();
        ops_ = std::move(ops);
        baseId_ = std::move(baseId);
        payloadVersion_ = payloadVersion;
        enabled_ = true;
        lastSentEmpty_ = true;
        fallbackToOwnAtBoundary_ = false;
        checkRequested_ = false;
        lastCheckRequested_ = 0;
        consecutiveRejects_ = 0;
        cooldown_.clear();
        stats_ = {};
        syncStats();
    }

    const Stats &stats() const { return stats_; }
    bool lastSentEmpty() const { return lastSentEmpty_; }
    bool hasStandby() const { return standby_.process != 0; }
    bool hasCurrent() const { return current_.kind != Active::Kind::None; }
    uint64_t currentSequence() const { return current_.sequence; }
    ProcessId currentProcess() const { return current_.process; }
    ProcessId standbyProcess() const { return standby_.process; }
    bool isCooledDown(uint64_t sequence, uint64_t now) { return inCooldown(sequence, now); }

    std::string diagnosticFields() const {
        return ",\"renderer_source\":\"" + std::string(sourceName(stats_.source)) +
            "\",\"renderer_sequence\":" + std::to_string(stats_.sequence) +
            ",\"renderer_payload\":" + std::to_string(stats_.payloadVersion) +
            ",\"renderer_handoffs\":" + std::to_string(stats_.handoffs) +
            ",\"renderer_failures\":" + std::to_string(stats_.failures);
    }

    // Explicitly start the own-directory per-process renderer (used by the live
    // integration test so it can exercise per-process cutover using empty hides
    // only, without flashing a visible popup window on the desktop).
    bool startOwn(uint64_t now) {
        if (current_.kind != Active::Kind::None) return true;
        Active launched;
        if (!launchPerProcess(nullptr, now, launched)) return false;
        current_ = std::move(launched);
        syncStats();
        return true;
    }

    // Called at empty boundaries (Activate, post-commit/cancel, pre-key while
    // idle). Never switches targets when !atBoundary or !lastSentEmpty_.
    void onBoundary(uint64_t now, bool atBoundary, bool enabled = true) {
        enabled_ = enabled;
        if (!atBoundary || !lastSentEmpty_) return;

        if (enabled_ && ops_.requestCheck && (!checkRequested_ || now >= lastCheckRequested_ + CheckIntervalMs)) {
            checkRequested_ = true;
            lastCheckRequested_ = now;
            ops_.requestCheck(now);
        }

        auto raw = rawApproved();
        auto app = (raw && !inCooldown(raw->sequence, now)) ? raw : nullptr;

        // Complete any pending fallback to own directory from a runtime failure.
        if (fallbackToOwnAtBoundary_) {
            fallbackToOwnAtBoundary_ = false;
            consecutiveRejects_ = 0;
            if (current_.kind == Active::Kind::Approved) closeCurrentPerProcess(true);
            else if (current_.kind == Active::Kind::Desktop && current_.sequence > 0) current_ = {};
            cancelStandby(false);
        }

        // If handoff was disabled, or approvals disappeared / became incompatible
        // / tied, or current approved sequence entered cooldown, return to own.
        if (!enabled_ || !raw || (current_.sequence > 0 && inCooldown(current_.sequence, now))) {
            if (current_.kind == Active::Kind::Approved) {
                closeCurrentPerProcess(true);
                consecutiveRejects_ = 0;
            } else if (current_.kind == Active::Kind::Desktop && current_.sequence > 0) {
                current_ = {};
                consecutiveRejects_ = 0;
            }
            if (!app) cancelStandby(false);
        }

        // Main route: desktop renderer.
        if (ops_.probeDesktop) {
            const auto desk = ops_.probeDesktop(app.get(), enabled_ && app != nullptr);
            if (desk.window) {
                if (!desk.isOwn && desk.sequence > 0) {
                    if (current_.kind != Active::Kind::Desktop || current_.window != desk.window
                            || current_.generation != desk.generation || current_.sequence != desk.sequence) {
                        const bool wasActive = current_.kind != Active::Kind::None;
                        const uint64_t prevSeq = current_.sequence;
                        if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved)
                            closeCurrentPerProcess(true);
                        cancelStandby(false);
                        current_ = { Active::Kind::Desktop, desk.window, 0, desk.generation, desk.sequence, {}, now, true };
                        consecutiveRejects_ = 0;
                        if (wasActive && desk.sequence != prevSeq) ++stats_.handoffs;
                    } else {
                        cancelStandby(false);
                    }
                    syncStats();
                    return;
                }
                if (desk.isOwn) {
                    // Own desktop renderer is available: keep using desktop route
                    // unless we already have a newer approved per-process renderer.
                    const bool keepApprovedPerProcess = (current_.kind == Active::Kind::Approved && app && current_.sequence >= app->sequence);
                    if (!keepApprovedPerProcess) {
                        if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved)
                            closeCurrentPerProcess(true);
                        cancelStandby(false);
                        current_ = { Active::Kind::Desktop, desk.window, 0, desk.generation, 0, {}, now, true };
                        consecutiveRejects_ = 0;
                        syncStats();
                        return;
                    }
                }
            } else if (current_.kind == Active::Kind::Desktop && !currentDesktopAlive()) {
                current_ = {};
            }
        }

        // Per-process route: manage standby pre-launch, timeout and cutover.
        if (standby_.process) {
            if (!enabled_ || !app || app->sequence != standby_.sequence) {
                cancelStandby(false);
            } else if (ops_.processAlive && !ops_.processAlive(standby_.process)) {
                const uint64_t failedSeq = standby_.sequence;
                cancelStandby(true);
                coolDown(failedSeq, now);
                ++stats_.failures;
                syncStats();
                return;
            } else {
                const WindowId readyWindow = ops_.findProcessWindow
                    ? ops_.findProcessWindow(standby_.windowName, standby_.process) : 0;
                // Confirm the standby's message loop and payload hook are live
                // via an empty hide probe (shows no window) before retiring current.
                if (readyWindow && (!ops_.send || ops_.send(readyWindow, L"", payloadVersion_))) {
                    promoteStandby(readyWindow, true, true);
                    return;
                }
                if (now >= standby_.startedAt + StandbyTimeoutMs) {
                    const uint64_t failedSeq = standby_.sequence;
                    cancelStandby(true);
                    coolDown(failedSeq, now);
                    ++stats_.failures;
                    syncStats();
                    return;
                }
                syncStats();
                return;
            }
        }

        // Start a standby only when a per-process renderer is already active and
        // a strictly newer approved release is available (desktop unavailable).
        if (enabled_ && app && app->sequence > current_.sequence
                && (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved)) {
            const std::wstring suffix = baseId_ + L"-s" + std::to_wstring(app->sequence);
            const ProcessId proc = ops_.launch ? ops_.launch(suffix, app.get()) : 0;
            if (!proc) {
                coolDown(app->sequence, now);
                ++stats_.failures;
            } else {
                standby_ = { app->sequence, L"MeowpuutCandidates-" + suffix, proc, now };
            }
        }
        syncStats();
    }

    // Send a payload (empty string = hide). Returns true if the renderer
    // accepted the packet; false tells the caller to fall back to the STATIC popup.
    bool send(const std::wstring &payload, uint64_t now, bool atBoundary, bool enabled = true) {
        enabled_ = enabled;
        if (payload.empty()) {
            lastSentEmpty_ = true;
            if (current_.kind == Active::Kind::Desktop) {
                if (currentDesktopAlive()) {
                    return ops_.send ? ops_.send(current_.window, payload, payloadVersion_) : false;
                }
                current_ = {};
                syncStats();
                return false;
            }
            if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved) {
                if (currentProcessAlive()) {
                    WindowId w = ops_.findProcessWindow
                        ? ops_.findProcessWindow(current_.windowName, current_.process) : current_.window;
                    current_.window = w;
                    if (w) {
                        current_.everReady = true;
                        return ops_.send ? ops_.send(w, payload, payloadVersion_) : false;
                    }
                }
                return false;
            }
            return false;
        }

        // Already failed an approved target in this composition: keep using the
        // STATIC fallback until the next empty boundary switches back to own.
        if (fallbackToOwnAtBoundary_ && !atBoundary) return false;
        if (atBoundary && lastSentEmpty_) onBoundary(now, true, enabled_);

        // Check whether current target has already exited (§4 exception: switch
        // immediately because no old window needs to be hidden).
        bool currentExited = false;
        if (current_.kind == Active::Kind::Desktop) currentExited = !currentDesktopAlive();
        else if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved)
            currentExited = !currentProcessAlive();

        if (currentExited) {
            const auto oldKind = current_.kind;
            const uint64_t oldSeq = current_.sequence;
            if (oldKind == Active::Kind::Own || oldKind == Active::Kind::Approved) {
                if (current_.process && ops_.releaseProcess) ops_.releaseProcess(current_.process);
            }
            current_ = {};
            consecutiveRejects_ = 0;

            auto app = eligibleApproved(now);
            bool switched = false;
            // 1. If a ready standby already exists, promote immediately.
            if (standby_.process && (!ops_.processAlive || ops_.processAlive(standby_.process)) && ops_.findProcessWindow) {
                if (const WindowId w = ops_.findProcessWindow(standby_.windowName, standby_.process)) {
                    if (!ops_.send || ops_.send(w, L"", payloadVersion_)) {
                        promoteStandby(w, false, standby_.sequence != oldSeq);
                        switched = true;
                    }
                }
            }
            // 2. Otherwise check desktop renderer (e.g. installer replaced desktop renderer).
            if (!switched && ops_.probeDesktop) {
                const auto desk = ops_.probeDesktop(app.get(), enabled_ && app != nullptr);
                if (desk.window) {
                    const bool replacedByNewer = (!desk.isOwn && desk.sequence > 0 && desk.sequence != oldSeq);
                    const bool wasApproved = (oldKind == Active::Kind::Approved || (oldKind == Active::Kind::Desktop && oldSeq > 0));
                    if (!wasApproved || replacedByNewer) {
                        current_ = { Active::Kind::Desktop, desk.window, 0, desk.generation, desk.sequence, {}, now, true };
                        if (!desk.isOwn && desk.sequence > 0 && desk.sequence != oldSeq) ++stats_.handoffs;
                        switched = true;
                    }
                }
            }
            // 3. Approved target exited without a newer replacement: fail & cooldown (§5).
            if (!switched && (oldKind == Active::Kind::Approved || (oldKind == Active::Kind::Desktop && oldSeq > 0))) {
                coolDown(oldSeq, now);
                ++stats_.failures;
                fallbackToOwnAtBoundary_ = true;
                syncStats();
                return false;
            }
            syncStats();
        }

        // Acquire initial target if none is active yet.
        if (current_.kind == Active::Kind::None) {
            auto app = eligibleApproved(now);
            if (ops_.probeDesktop) {
                const auto desk = ops_.probeDesktop(app.get(), enabled_ && app != nullptr);
                if (desk.window) {
                    current_ = { Active::Kind::Desktop, desk.window, 0, desk.generation, desk.sequence, {}, now, true };
                }
            }
            if (current_.kind == Active::Kind::None) {
                if (enabled_ && app) {
                    Active launched;
                    if (launchPerProcess(app.get(), now, launched)) {
                        current_ = std::move(launched);
                    } else {
                        coolDown(app->sequence, now);
                        ++stats_.failures;
                    }
                }
                if (current_.kind == Active::Kind::None) {
                    Active launched;
                    if (launchPerProcess(nullptr, now, launched)) current_ = std::move(launched);
                }
            }
            syncStats();
        }

        WindowId target = 0;
        if (current_.kind == Active::Kind::Desktop) {
            target = current_.window;
        } else if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved) {
            target = ops_.findProcessWindow
                ? ops_.findProcessWindow(current_.windowName, current_.process) : current_.window;
            current_.window = target;
            if (!target) {
                // Direct-launched approved renderer timed out before becoming ready.
                if (current_.kind == Active::Kind::Approved && !current_.everReady
                        && now >= current_.startedAt + StandbyTimeoutMs) {
                    const uint64_t failedSeq = current_.sequence;
                    if (currentProcessAlive() && ops_.terminateProcess) ops_.terminateProcess(current_.process);
                    if (current_.process && ops_.releaseProcess) ops_.releaseProcess(current_.process);
                    current_ = {};
                    coolDown(failedSeq, now);
                    ++stats_.failures;
                    fallbackToOwnAtBoundary_ = true;
                    syncStats();
                }
                return false;
            }
            current_.everReady = true;
        }

        if (!target) return false;
        const bool accepted = ops_.send ? ops_.send(target, payload, payloadVersion_) : false;
        if (accepted) {
            lastSentEmpty_ = false;
            consecutiveRejects_ = 0;
            syncStats();
            return true;
        }

        // Rejected: tolerate up to MaxConsecutiveRejects - 1 transient misses
        // (e.g. foreground window changed), unless the approved target exited.
        const bool isApproved = (current_.kind == Active::Kind::Approved
            || (current_.kind == Active::Kind::Desktop && current_.sequence > 0));
        if (isApproved) {
            const bool alive = (current_.kind == Active::Kind::Desktop)
                ? currentDesktopAlive() : currentProcessAlive();
            if (!alive) {
                const uint64_t failedSeq = current_.sequence;
                if (current_.kind == Active::Kind::Approved && current_.process && ops_.releaseProcess)
                    ops_.releaseProcess(current_.process);
                current_ = {};
                consecutiveRejects_ = 0;
                coolDown(failedSeq, now);
                ++stats_.failures;
                fallbackToOwnAtBoundary_ = true;
                syncStats();
                return false;
            }
            if (++consecutiveRejects_ >= MaxConsecutiveRejects) {
                consecutiveRejects_ = 0;
                coolDown(current_.sequence, now);
                ++stats_.failures;
                fallbackToOwnAtBoundary_ = true;
                syncStats();
            }
        }
        return false;
    }

    void shutdown() {
        cancelStandby(false);
        if (current_.kind == Active::Kind::Own || current_.kind == Active::Kind::Approved)
            closeCurrentPerProcess(false);
        else
            current_ = {};
        consecutiveRejects_ = 0;
        fallbackToOwnAtBoundary_ = false;
        lastSentEmpty_ = true;
        syncStats();
    }
};

// Production Win32 host for Machine: detached background release checking,
// SHA-256 verification cached per (sequence, file ID), EngineDeployment pin
// held only during verification and suspended-process launch, and cached
// desktop-process verification so normal keystrokes perform no disk hashing.
class LiveHost {
    struct VerifiedImage {
        std::filesystem::path dosPath;
        std::wstring nativePath;
        FILE_ID_INFO fileId{};
        std::string sha256;
    };
    struct CheckerShared {
        std::atomic<bool> inFlight{false};
        std::mutex lock;
        std::shared_ptr<const Approved> published;
        uint64_t cachedSequence = 0;
        FILE_ID_INFO cachedFileId{};
        std::string cachedSha256;
        std::shared_ptr<const Approved> cachedApproved;
    };
    struct CheckTask {
        std::shared_ptr<CheckerShared> shared;
        release::Source source;
        uint16_t payloadVersion = release::PayloadVersion;
        HMODULE module = nullptr;
    };
    struct CachedDesktop {
        HWND hwnd = nullptr;
        DWORD pid = 0;
        HANDLE process = nullptr;
        uint64_t generation = 0;
        bool isOwn = false;
        uint64_t sequence = 0;
        uint64_t checkedApprovedSequence = 0;
        void clear() {
            if (process) CloseHandle(process);
            hwnd = nullptr; pid = 0; process = nullptr; isOwn = false; sequence = 0; checkedApprovedSequence = 0;
        }
        ~CachedDesktop() { clear(); }
    };

    std::shared_ptr<CheckerShared> shared_ = std::make_shared<CheckerShared>();
    std::filesystem::path ownExe_;
    release::Source source_;
    uint16_t payloadVersion_ = release::PayloadVersion;
    bool disableDesktop_ = false;
    HWND lastForeground_ = nullptr;
    std::wstring ownNative_;
    FILE_ID_INFO ownFileId_{};
    bool ownProbed_ = false;
    bool ownValid_ = false;
    uint64_t desktopGen_ = 0;
    CachedDesktop desktop_;

    void ensureOwnIdentity() {
        if (ownProbed_) return;
        ownProbed_ = true;
        ownValid_ = engine::EngineDeployment::probeFileIdentity(ownExe_, ownNative_, ownFileId_);
    }

    static DWORD WINAPI checkerWorker(void *raw) {
        HMODULE module = nullptr;
        {
            std::unique_ptr<CheckTask> task(static_cast<CheckTask *>(raw));
            module = task->module;
            try {
                const auto snapshot = release::read(task->source);
                const auto entry = release::select(snapshot, task->payloadVersion);
                std::shared_ptr<const Approved> result;
                if (entry) {
                    std::wstring native;
                    FILE_ID_INFO id{};
                    if (engine::EngineDeployment::probeFileIdentity(entry->renderer, native, id)) {
                        std::shared_ptr<const Approved> reused;
                        {
                            std::lock_guard<std::mutex> guard(task->shared->lock);
                            if (task->shared->cachedApproved
                                    && task->shared->cachedSequence == entry->sequence
                                    && task->shared->cachedSha256 == entry->rendererSha256
                                    && engine::EngineDeployment::sameFileId(task->shared->cachedFileId, id)) {
                                reused = task->shared->cachedApproved;
                            }
                        }
                        if (reused) {
                            result = std::move(reused);
                        } else {
                            // Pin, hash and verify SHA-256, then release all pin handles immediately.
                            std::filesystem::path dosPath;
                            std::wstring verifiedNative;
                            FILE_ID_INFO verifiedId{};
                            {
                                engine::EngineDeployment deploy(entry->renderer, entry->rendererSha256);
                                dosPath = deploy.path();
                                verifiedNative = deploy.nativePath();
                                verifiedId = deploy.fileId();
                            }
                            auto img = std::make_shared<VerifiedImage>(VerifiedImage{
                                dosPath, std::move(verifiedNative), verifiedId, entry->rendererSha256
                            });
                            result = std::make_shared<Approved>(Approved{
                                entry->sequence, entry->id, std::move(dosPath), std::move(img)
                            });
                            std::lock_guard<std::mutex> guard(task->shared->lock);
                            task->shared->cachedSequence = entry->sequence;
                            task->shared->cachedFileId = verifiedId;
                            task->shared->cachedSha256 = entry->rendererSha256;
                            task->shared->cachedApproved = result;
                        }
                    }
                }
                std::lock_guard<std::mutex> guard(task->shared->lock);
                task->shared->published = std::move(result);
            } catch (...) {
                std::lock_guard<std::mutex> guard(task->shared->lock);
                task->shared->published.reset();
            }
            task->shared->inFlight.store(false, std::memory_order_release);
        }
        FreeLibraryAndExitThread(module, 0);
    }

public:
    LiveHost() = default;
    LiveHost(std::filesystem::path ownExe, release::Source source = {}, uint16_t payloadVersion = release::PayloadVersion, bool disableDesktop = false)
        : ownExe_(std::move(ownExe)), source_(std::move(source)), payloadVersion_(payloadVersion), disableDesktop_(disableDesktop) {}
    LiveHost(const LiveHost &) = delete;
    LiveHost &operator=(const LiveHost &) = delete;

    void reset(std::filesystem::path ownExe, release::Source source = {}, uint16_t payloadVersion = release::PayloadVersion, bool disableDesktop = false) {
        desktop_.clear();
        shared_ = std::make_shared<CheckerShared>();
        ownExe_ = std::move(ownExe);
        source_ = std::move(source);
        payloadVersion_ = payloadVersion;
        disableDesktop_ = disableDesktop;
        lastForeground_ = nullptr;
        ownProbed_ = false;
        ownValid_ = false;
    }

    HWND lastForeground() const { return lastForeground_; }
    void setLastForeground(HWND hwnd) { lastForeground_ = hwnd; }

    bool waitForCheck(uint64_t timeoutMs = 5000) const {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (shared_->inFlight.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
        return !shared_->inFlight.load(std::memory_order_acquire);
    }

    void requestCheck() {
        bool expected = false;
        if (!shared_->inFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&checkerWorker), &mod)) {
            shared_->inFlight.store(false, std::memory_order_release);
            return;
        }
        try {
            auto task = std::make_unique<CheckTask>(CheckTask{shared_, source_, payloadVersion_, mod});
            HANDLE thread = CreateThread(nullptr, 0, &checkerWorker, task.get(), 0, nullptr);
            if (thread) {
                task.release();
                CloseHandle(thread);
                return;
            }
        } catch (...) {}
        FreeLibrary(mod);
        shared_->inFlight.store(false, std::memory_order_release);
    }

    std::shared_ptr<const Approved> latestApproved() const {
        std::lock_guard<std::mutex> guard(shared_->lock);
        return shared_->published;
    }

    DesktopMatch probeDesktop(const Approved *approved, bool allowApproved) {
        if (disableDesktop_) return {};
        HWND hwnd = FindWindowW(nullptr, L"MeowpuutCandidates-desktop");
        if (!hwnd) { desktop_.clear(); return {}; }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) { desktop_.clear(); return {}; }

        // Fast path: cached verified desktop process is still alive on the same HWND.
        const uint64_t wantedApprovedSeq = (allowApproved && approved && approved->token) ? approved->sequence : 0;
        if (desktop_.process && desktop_.hwnd == hwnd && desktop_.pid == pid
                && WaitForSingleObject(desktop_.process, 0) == WAIT_TIMEOUT) {
            if (!desktop_.isOwn) {
                if (wantedApprovedSeq && wantedApprovedSeq == desktop_.sequence)
                    return { reinterpret_cast<WindowId>(hwnd), desktop_.generation, false, desktop_.sequence };
            } else if (desktop_.checkedApprovedSequence == wantedApprovedSeq) {
                return { reinterpret_cast<WindowId>(hwnd), desktop_.generation, true, 0 };
            }
        }

        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!proc) { desktop_.clear(); return {}; }

        if (wantedApprovedSeq) {
            const auto *img = static_cast<const VerifiedImage *>(approved->token.get());
            if (engine::EngineDeployment::verifyImage(proc, img->nativePath, img->fileId)) {
                desktop_.clear();
                desktop_.hwnd = hwnd; desktop_.pid = pid; desktop_.process = proc;
                desktop_.generation = ++desktopGen_; desktop_.isOwn = false;
                desktop_.sequence = approved->sequence; desktop_.checkedApprovedSequence = approved->sequence;
                return { reinterpret_cast<WindowId>(hwnd), desktop_.generation, false, approved->sequence };
            }
        }

        ensureOwnIdentity();
        if (ownValid_ && engine::EngineDeployment::verifyImage(proc, ownNative_, ownFileId_)) {
            desktop_.clear();
            desktop_.hwnd = hwnd; desktop_.pid = pid; desktop_.process = proc;
            desktop_.generation = ++desktopGen_; desktop_.isOwn = true;
            desktop_.sequence = 0; desktop_.checkedApprovedSequence = wantedApprovedSeq;
            return { reinterpret_cast<WindowId>(hwnd), desktop_.generation, true, 0 };
        }

        CloseHandle(proc);
        desktop_.clear();
        return {};
    }

    bool desktopAlive(WindowId window, uint64_t generation) const {
        if (!window || !desktop_.process || desktop_.generation != generation
                || reinterpret_cast<HWND>(window) != desktop_.hwnd)
            return false;
        if (WaitForSingleObject(desktop_.process, 0) != WAIT_TIMEOUT) return false;
        if (!IsWindow(desktop_.hwnd)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(desktop_.hwnd, &pid);
        return pid != 0 && pid == desktop_.pid;
    }

    ProcessId launch(const std::wstring &windowSuffix, const Approved *approved) {
        if (approved) {
            if (!approved->token) return 0;
            const auto *img = static_cast<const VerifiedImage *>(approved->token.get());
            try {
                // Pin, verify SHA-256, launch suspended, verify live image, resume, release pin.
                engine::EngineDeployment deploy(img->dosPath, img->sha256);
                std::wstring cmd = L"\"" + deploy.path().wstring() + L"\" --ime " + windowSuffix;
                const std::wstring cwd = deploy.path().parent_path().wstring();
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                if (!CreateProcessW(deploy.path().c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi))
                    return 0;
                try {
                    deploy.verify(pi.hProcess);
                } catch (...) {
                    TerminateProcess(pi.hProcess, 1);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return 0;
                }
                if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
                    TerminateProcess(pi.hProcess, 1);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return 0;
                }
                CloseHandle(pi.hThread);
                return reinterpret_cast<ProcessId>(pi.hProcess);
            } catch (...) {
                return 0;
            }
        }
        if (ownExe_.empty()) return 0;
        std::wstring cmd = L"\"" + ownExe_.wstring() + L"\" --ime " + windowSuffix;
        const std::wstring cwd = ownExe_.parent_path().wstring();
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(ownExe_.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                0, nullptr, cwd.c_str(), &si, &pi))
            return 0;
        CloseHandle(pi.hThread);
        return reinterpret_cast<ProcessId>(pi.hProcess);
    }

    static bool processAlive(ProcessId process) {
        return process && WaitForSingleObject(reinterpret_cast<HANDLE>(process), 0) == WAIT_TIMEOUT;
    }

    static WindowId findProcessWindow(const std::wstring &windowName, ProcessId process) {
        if (!process || !processAlive(process)) return 0;
        HWND hwnd = FindWindowW(nullptr, windowName.c_str());
        if (!hwnd) return 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return (pid && pid == GetProcessId(reinterpret_cast<HANDLE>(process)))
            ? reinterpret_cast<WindowId>(hwnd) : 0;
    }

    static void closeWindow(WindowId window) {
        if (window) PostMessageW(reinterpret_cast<HWND>(window), WM_CLOSE, 0, 0);
    }

    static void terminateProcess(ProcessId process) {
        if (process) TerminateProcess(reinterpret_cast<HANDLE>(process), 1);
    }

    static void releaseProcess(ProcessId process) {
        if (process) CloseHandle(reinterpret_cast<HANDLE>(process));
    }

    bool send(WindowId window, const std::wstring &payload, uint16_t version) {
        if (!window || payload.size() >= 4096) return false;
        if (!payload.empty()) lastForeground_ = GetForegroundWindow();
        COPYDATASTRUCT packet{
            release::payloadTag(version),
            static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t)),
            const_cast<wchar_t *>(payload.c_str())
        };
        DWORD_PTR accepted = 0;
        return SendMessageTimeoutW(reinterpret_cast<HWND>(window), WM_COPYDATA,
            reinterpret_cast<WPARAM>(lastForeground_), reinterpret_cast<LPARAM>(&packet),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 35, &accepted) && accepted;
    }

    Ops ops() {
        Ops out;
        out.requestCheck = [this](uint64_t) { requestCheck(); };
        out.latestApproved = [this] { return latestApproved(); };
        out.probeDesktop = [this](const Approved *approved, bool allowApproved) { return probeDesktop(approved, allowApproved); };
        out.desktopAlive = [this](WindowId w, uint64_t gen) { return desktopAlive(w, gen); };
        out.launch = [this](const std::wstring &suffix, const Approved *approved) { return launch(suffix, approved); };
        out.processAlive = &LiveHost::processAlive;
        out.findProcessWindow = &LiveHost::findProcessWindow;
        out.closeWindow = &LiveHost::closeWindow;
        out.terminateProcess = &LiveHost::terminateProcess;
        out.releaseProcess = &LiveHost::releaseProcess;
        out.send = [this](WindowId w, const std::wstring &payload, uint16_t version) { return send(w, payload, version); };
        return out;
    }
};

} // namespace harukoto::renderer::handoff
