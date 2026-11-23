#include "../RendererHandoff.h"
#include <cassert>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

// RH-5: Live per-process renderer handoff integration test (docs/RENDERER_HANDOFF.md §7).
// Copies the built MeowpuutGlassPreview.exe into two temporary version folders
// A and B, uses an HKCU test key (never HKLM / Program Files), starts A per-process
// and sends empty payloads only (no visible window on the desktop), publishes B,
// drives empty boundaries to confirm A exits cleanly and B accepts packets, then
// tests rollback to A and verifies all pin handles, processes, keys and files clean up.
namespace release = harukoto::renderer::release;
namespace handoff = harukoto::renderer::handoff;

namespace {
struct CleanupState {
    std::wstring subkey;
    std::filesystem::path tempRoot;
    std::vector<HANDLE> trackedProcesses;
    void track(HANDLE proc) {
        if (!proc) return;
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), proc, GetCurrentProcess(), &dup,
                SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
            trackedProcesses.push_back(dup);
        }
    }
    void purge(bool terminateOnly = false) {
        for (HANDLE h : trackedProcesses) {
            if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
                TerminateProcess(h, 1);
                WaitForSingleObject(h, 3000);
            }
            CloseHandle(h);
        }
        trackedProcesses.clear();
        if (!subkey.empty()) {
            RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
            RegDeleteKeyW(HKEY_CURRENT_USER, subkey.c_str());
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\HarukotoTest");
            subkey.clear();
        }
        if (!terminateOnly && !tempRoot.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(tempRoot, ec);
            tempRoot.clear();
        }
    }
};

CleanupState *gCleanup = nullptr;
void abortHandler(int) {
    if (gCleanup) gCleanup->purge(false);
}

std::string sha256File(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    base_dictionary::Sha256 hash;
    char buf[65536];
    while (in) {
        in.read(buf, sizeof(buf));
        const auto n = in.gcount();
        if (n > 0) hash.add(buf, static_cast<ULONG>(n));
    }
    return hash.finish();
}

std::wstring widenAscii(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}

void publishEntry(HKEY key, const std::wstring &id, uint64_t sequence,
        const std::filesystem::path &exe, const std::string &sha256,
        const std::wstring &payload = L"1-1") {
    const std::vector<std::wstring> lines = {
        release::FormatLine,
        L"id=" + id,
        L"sequence=" + std::to_wstring(sequence),
        L"payload=" + payload,
        L"renderer=" + exe.wstring(),
        L"renderer-sha256=" + widenAscii(sha256)
    };
    std::wstring multi;
    for (const auto &line : lines) { multi += line; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    const LONG rc = RegSetValueExW(key, id.c_str(), 0, REG_MULTI_SZ,
        reinterpret_cast<const BYTE *>(multi.data()), static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
    assert(rc == ERROR_SUCCESS);
}

HANDLE openTracked(handoff::ProcessId procId) {
    assert(procId != 0);
    const DWORD pid = GetProcessId(reinterpret_cast<HANDLE>(procId));
    assert(pid != 0);
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    assert(h != nullptr);
    return h;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    const std::filesystem::path sourceExe(argv[1]);
    assert(std::filesystem::is_regular_file(sourceExe));

    CleanupState cleanup;
    gCleanup = &cleanup;
    std::signal(SIGABRT, &abortHandler);
    struct Guard {
        CleanupState &c;
        ~Guard() { c.purge(false); gCleanup = nullptr; }
    } guard{cleanup};

    const std::wstring tag = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    cleanup.subkey = L"Software\\HarukotoTest\\RendererLive-" + tag;
    wchar_t tempBuf[MAX_PATH + 2]{};
    const DWORD tempLen = GetTempPathW(MAX_PATH + 1, tempBuf);
    assert(tempLen > 0 && tempLen <= MAX_PATH);
    cleanup.tempRoot = std::filesystem::path(tempBuf) / (L"harukoto-rh-live-" + tag);

    const std::wstring idA = L"VERA0001-1111";
    const std::wstring idB = L"VERB0002-2222";
    const auto dirA = cleanup.tempRoot / idA;
    const auto dirB = cleanup.tempRoot / idB;
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    const auto exeA = dirA / release::RendererName;
    const auto exeB = dirB / release::RendererName;
    std::filesystem::copy_file(sourceExe, exeA);
    std::filesystem::copy_file(sourceExe, exeB);
    const std::string shaA = sha256File(exeA);
    const std::string shaB = sha256File(exeB);

    HKEY key = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, cleanup.subkey.c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS);
    struct KeyGuard { HKEY k; ~KeyGuard() { if (k) RegCloseKey(k); } } keyGuard{key};

    const release::Source testSource{HKEY_CURRENT_USER, cleanup.subkey, false};
    // Keep desktop probing enabled: any real desktop renderer running on the host
    // machine is in a different directory and must be rejected by verifyImage().
    handoff::LiveHost host(exeA, testSource, release::PayloadVersion, false);
    handoff::Machine machine(host.ops(), std::to_wstring(GetCurrentProcessId()) + L"-live", release::PayloadVersion);

    // 1. Start A per-process and confirm it accepts empty payloads without showing a window.
    uint64_t clockMs = 1000;
    assert(machine.startOwn(clockMs));
    HANDLE procA = openTracked(machine.currentProcess());
    cleanup.track(procA);
    {
        const auto deadline = GetTickCount64() + 15000;
        bool accepted = false;
        while (GetTickCount64() < deadline) {
            if (machine.send(L"", clockMs, false, true)) { accepted = true; break; }
            Sleep(25);
        }
        assert(accepted);
        assert(machine.stats().source == handoff::SourceKind::Own);
        assert(machine.stats().sequence == 0 && machine.stats().handoffs == 0);
        // Verify A's live image via EngineDeployment (pin released immediately on scope exit).
        harukoto::engine::EngineDeployment deployA(exeA, shaA);
        deployA.verify(procA);
    }
    std::wcout << L"PASS: live per-process renderer A started and accepted empty payload\n";

    // 2. Publish B (sequence 2), drive empty boundary -> standby launch -> cutover A -> B.
    publishEntry(key, idB, 2, exeB, shaB);
    clockMs += 3000;
    machine.onBoundary(clockMs, true, true);
    assert(host.waitForCheck(5000));
    const auto approvedB = host.latestApproved();
    assert(approvedB && approvedB->sequence == 2 && approvedB->id == idB);

    // Next empty boundary launches standby B.
    machine.onBoundary(clockMs, true, true);
    assert(machine.hasStandby());
    HANDLE procB = openTracked(machine.standbyProcess());
    cleanup.track(procB);
    {
        harukoto::engine::EngineDeployment deployB(exeB, shaB);
        deployB.verify(procB);
    }

    // Wait for standby B's hidden window to appear and cut over at empty boundary.
    {
        const auto startWall = GetTickCount64();
        const auto deadline = startWall + 15000;
        while (GetTickCount64() < deadline) {
            // Keep virtual clock below the 5s standby timeout while WPF initializes on slower CI hosts,
            // yet still advance it monotonically.
            const uint64_t stepClock = clockMs + (std::min<uint64_t>)(GetTickCount64() - startWall, 4000);
            machine.onBoundary(stepClock, true, true);
            if (machine.stats().source == handoff::SourceKind::Approved && machine.stats().sequence == 2) break;
            Sleep(25);
        }
        assert(!machine.hasStandby());
        assert(machine.stats().source == handoff::SourceKind::Approved);
        assert(machine.stats().sequence == 2 && machine.stats().handoffs == 1 && machine.stats().failures == 0);
        // Confirm A exited cleanly after receiving hide + WM_CLOSE.
        assert(WaitForSingleObject(procA, 5000) == WAIT_OBJECT_0);
        // Confirm B is alive and accepts empty payload (still no window shown).
        assert(WaitForSingleObject(procB, 0) == WAIT_TIMEOUT);
        assert(machine.send(L"", clockMs + 4500, false, true));
    }
    CloseHandle(procA);
    std::wcout << L"PASS: live handoff from A to B (A exited cleanly, B accepted empty payload)\n";

    // 3. Rollback: re-publish A with sequence 3 -> cutover B -> A.
    publishEntry(key, idA, 3, exeA, shaA);
    clockMs += 8000;
    machine.onBoundary(clockMs, true, true);
    assert(host.waitForCheck(5000));
    const auto approvedA = host.latestApproved();
    assert(approvedA && approvedA->sequence == 3 && approvedA->id == idA);

    machine.onBoundary(clockMs, true, true);
    assert(machine.hasStandby());
    HANDLE procA2 = openTracked(machine.standbyProcess());
    cleanup.track(procA2);
    {
        const auto startWall = GetTickCount64();
        const auto deadline = startWall + 15000;
        while (GetTickCount64() < deadline) {
            const uint64_t stepClock = clockMs + (std::min<uint64_t>)(GetTickCount64() - startWall, 4000);
            machine.onBoundary(stepClock, true, true);
            if (machine.stats().source == handoff::SourceKind::Approved && machine.stats().sequence == 3) break;
            Sleep(25);
        }
        assert(machine.stats().source == handoff::SourceKind::Approved);
        assert(machine.stats().sequence == 3 && machine.stats().handoffs == 2 && machine.stats().failures == 0);
        assert(WaitForSingleObject(procB, 5000) == WAIT_OBJECT_0);
        assert(WaitForSingleObject(procA2, 0) == WAIT_TIMEOUT);
        assert(machine.send(L"", clockMs + 4500, false, true));
    }
    CloseHandle(procB);
    std::wcout << L"PASS: live rollback handoff from B back to A (sequence 3)\n";

    // 4. Clean shutdown: A2 exits cleanly and all files/folders delete without pin locks.
    machine.shutdown();
    assert(WaitForSingleObject(procA2, 5000) == WAIT_OBJECT_0);
    CloseHandle(procA2);
    host.reset({}, {}, release::PayloadVersion, false);

    // Explicit deletion verifies no EngineDeployment pin handle was leaked.
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(cleanup.tempRoot, ec);
    assert(!ec && removed > 0 && !std::filesystem::exists(cleanup.tempRoot));
    cleanup.tempRoot.clear();
    std::wcout << L"PASS: clean shutdown, zero leaked processes or pin handles\n";
    return 0;
}
