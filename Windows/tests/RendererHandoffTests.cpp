#include "../RendererHandoff.h"
#include <cassert>
#include <iostream>
#include <map>
#include <vector>

// RH-3: Pure-logic state machine tests with fake clock, fake desktop, fake
// launcher, fake windows and fake sends (docs/RENDERER_HANDOFF.md §7).
namespace handoff = harukoto::renderer::handoff;
using handoff::Approved;
using handoff::DesktopMatch;
using handoff::Machine;
using handoff::Ops;
using handoff::ProcessId;
using handoff::SourceKind;
using handoff::WindowId;

struct FakeProc {
    ProcessId id = 0;
    WindowId window = 0;
    std::wstring expectedName;
    uint64_t sequence = 0; // 0 = own
    bool alive = true;
    bool ready = false;
    bool closed = false;
    bool terminated = false;
    bool released = false;
    bool acceptSends = true;
};

struct FakeEnv {
    uint64_t checkCount = 0;
    std::shared_ptr<Approved> approved;

    // Desktop renderer state.
    WindowId desktopWindow = 0;
    uint64_t desktopGen = 0;
    bool desktopIsOwn = false;
    uint64_t desktopSeq = 0;
    bool desktopAlive = false;
    bool desktopAcceptSends = true;
    unsigned desktopCloseCount = 0;

    // Per-process renderer state.
    ProcessId nextProc = 100;
    WindowId nextWindow = 1000;
    bool failNextLaunch = false;
    bool autoReadyOnLaunch = false;
    std::map<ProcessId, FakeProc> procs;
    std::vector<std::pair<WindowId, std::wstring>> sends;

    void setDesktop(WindowId w, bool isOwn, uint64_t seq, bool alive = true) {
        desktopWindow = w;
        desktopGen++;
        desktopIsOwn = isOwn;
        desktopSeq = seq;
        desktopAlive = alive;
        desktopAcceptSends = true;
    }

    void setApproved(uint64_t seq, const std::wstring &id) {
        if (!seq) { approved.reset(); return; }
        approved = std::make_shared<Approved>(Approved{seq, id, L"C:\\Program Files\\Meowpuut\\IME\\" + id + L"\\MeowpuutGlassPreview.exe", {}});
    }

    Ops ops() {
        Ops o;
        o.requestCheck = [this](uint64_t) { ++checkCount; };
        o.latestApproved = [this]() -> std::shared_ptr<const Approved> { return approved; };
        o.probeDesktop = [this](const Approved *app, bool allowApproved) -> DesktopMatch {
            if (!desktopAlive || !desktopWindow) return {};
            if (desktopIsOwn) {
                if (allowApproved && app && desktopSeq == app->sequence && desktopSeq > 0)
                    return {desktopWindow, desktopGen, false, desktopSeq};
                return {desktopWindow, desktopGen, true, 0};
            }
            if (allowApproved && app && desktopSeq == app->sequence)
                return {desktopWindow, desktopGen, false, desktopSeq};
            return {};
        };
        o.desktopAlive = [this](WindowId w, uint64_t gen) {
            return desktopAlive && desktopWindow == w && desktopGen == gen;
        };
        o.launch = [this](const std::wstring &suffix, const Approved *app) -> ProcessId {
            if (failNextLaunch) { failNextLaunch = false; return 0; }
            FakeProc p;
            p.id = ++nextProc;
            p.window = ++nextWindow;
            p.expectedName = L"MeowpuutCandidates-" + suffix;
            p.sequence = app ? app->sequence : 0;
            p.alive = true;
            p.ready = autoReadyOnLaunch;
            procs[p.id] = p;
            return p.id;
        };
        o.processAlive = [this](ProcessId id) {
            const auto it = procs.find(id);
            return it != procs.end() && it->second.alive;
        };
        o.findProcessWindow = [this](const std::wstring &name, ProcessId id) -> WindowId {
            const auto it = procs.find(id);
            if (it == procs.end() || !it->second.alive || !it->second.ready) return 0;
            return it->second.expectedName == name ? it->second.window : 0;
        };
        o.closeWindow = [this](WindowId w) {
            if (w == desktopWindow) { ++desktopCloseCount; return; }
            for (auto &kv : procs) {
                if (kv.second.window == w) { kv.second.closed = true; kv.second.alive = false; }
            }
        };
        o.terminateProcess = [this](ProcessId id) {
            const auto it = procs.find(id);
            if (it != procs.end()) { it->second.terminated = true; it->second.alive = false; }
        };
        o.releaseProcess = [this](ProcessId id) {
            const auto it = procs.find(id);
            if (it != procs.end()) it->second.released = true;
        };
        o.send = [this](WindowId w, const std::wstring &payload, uint16_t version) -> bool {
            assert(version == harukoto::renderer::release::PayloadVersion);
            sends.emplace_back(w, payload);
            if (w == desktopWindow) return desktopAlive && desktopAcceptSends;
            for (auto &kv : procs) {
                if (kv.second.window == w) return kv.second.alive && kv.second.acceptSends;
            }
            return false;
        };
        return o;
    }
};

// 1. Mid-composition does not switch; empty boundary switches desktop renderer cleanly.
static void desktopCutoverAndNoSwitchMidComposition() {
    FakeEnv env;
    env.setDesktop(10, true, 0);
    Machine m(env.ops(), L"123-1");

    // Initial check throttled at 2000ms.
    m.onBoundary(1000, true, true);
    assert(env.checkCount == 1);
    m.onBoundary(2500, true, true);
    assert(env.checkCount == 1);
    m.onBoundary(3000, true, true);
    assert(env.checkCount == 2);

    // Start composing on own desktop (window 10).
    assert(m.send(L"0\n0\nnihao\t0\t简\t0\n你好", 3100, false, true));
    assert(m.stats().source == SourceKind::Desktop && m.stats().sequence == 0 && m.stats().handoffs == 0);
    assert(!m.lastSentEmpty());

    // Upgrade arrives while composing, desktop v2 appears while v1 stays alive.
    env.setApproved(2, L"V2");
    m.onBoundary(6000, false, true); // Mid-composition: must NOT check or switch.
    assert(env.checkCount == 2);
    assert(m.send(L"0\n0\nnihaom\t0\t简\t0\n你好吗", 6100, false, true));
    assert(env.sends.back().first == 10); // Still old desktop window 10!
    assert(m.stats().sequence == 0 && m.stats().handoffs == 0);

    // User commits -> hide -> empty boundary -> now switch to approved desktop v2.
    assert(m.send(L"", 6200, false, true));
    assert(m.lastSentEmpty());
    env.setDesktop(20, false, 2);
    m.onBoundary(6200, true, true);
    assert(m.stats().source == SourceKind::Desktop && m.stats().sequence == 2 && m.stats().handoffs == 1);
    assert(env.desktopCloseCount == 0); // DLL never closes desktop renderer.
    assert(m.send(L"0\n0\nshijie\t0\t简\t0\n世界", 6300, false, true));
    assert(env.sends.back().first == 20);
    std::cout << "PASS: desktop cutover at empty boundary only, check throttle, never closes desktop\n";
}

// 2. Exception (§4): current target already exited mid-composition -> switch immediately.
static void immediateSwitchWhenCurrentExited() {
    FakeEnv env;
    env.setDesktop(10, true, 0);
    Machine m(env.ops(), L"123-2");
    assert(m.send(L"0\n0\na\t0\t简\t0\n啊", 1000, false, true));
    assert(env.sends.back().first == 10);

    // Installer kills old desktop (10) and starts approved desktop v2 (20) mid-composition!
    env.setApproved(2, L"V2");
    env.setDesktop(20, false, 2);
    assert(m.send(L"0\n0\nab\t0\t简\t0\n阿", 1100, false, true));
    assert(env.sends.back().first == 20);
    assert(m.stats().source == SourceKind::Desktop && m.stats().sequence == 2 && m.stats().handoffs == 1);
    assert(m.stats().failures == 0);
    std::cout << "PASS: immediate switch when current desktop target exited mid-composition\n";
}

// 3. Per-process standby pre-launch, hide -> WM_CLOSE -> switch, and rollback reinstall.
static void perProcessStandbyAndRollback() {
    FakeEnv env;
    Machine m(env.ops(), L"456-9");
    assert(m.startOwn(1000));
    const ProcessId ownProc = m.currentProcess();
    assert(ownProc && env.procs[ownProc].expectedName == L"MeowpuutCandidates-456-9");
    env.procs[ownProc].ready = true;
    assert(m.send(L"0\n0\na\t0\t简\t0\n啊", 1100, false, true));
    assert(m.send(L"", 1200, false, true));

    // Publish seq 2 -> boundary launches standby with -s2 suffix; current stays own until ready.
    env.setApproved(2, L"V2");
    m.onBoundary(1300, true, true);
    assert(m.hasStandby());
    const ProcessId p2 = m.standbyProcess();
    assert(env.procs[p2].expectedName == L"MeowpuutCandidates-456-9-s2");
    assert(m.stats().source == SourceKind::Own && m.stats().sequence == 0);

    // Mid-composition while standby becomes ready: must NOT cut over yet.
    assert(m.send(L"0\n0\nb\t0\t简\t0\n吧", 1400, false, true));
    env.procs[p2].ready = true;
    m.onBoundary(1500, false, true);
    assert(m.stats().source == SourceKind::Own && env.procs[ownProc].alive);

    // Hide + empty boundary -> cut over: probe standby -> hide old -> WM_CLOSE old -> release -> active = seq 2.
    assert(m.send(L"", 1600, false, true));
    const size_t sendsBeforeCutover = env.sends.size();
    m.onBoundary(1600, true, true);
    assert(!m.hasStandby() && m.stats().source == SourceKind::Approved && m.stats().sequence == 2 && m.stats().handoffs == 1);
    assert(env.procs[ownProc].closed && !env.procs[ownProc].alive && env.procs[ownProc].released);
    // Verify readiness probe was sent to standby and hide (empty payload) was sent to old window before WM_CLOSE.
    assert(env.sends.size() == sendsBeforeCutover + 2
        && env.sends[sendsBeforeCutover].first == env.procs[p2].window && env.sends[sendsBeforeCutover].second.empty()
        && env.sends[sendsBeforeCutover + 1].first == env.procs[ownProc].window && env.sends[sendsBeforeCutover + 1].second.empty());
    assert(m.send(L"0\n0\nc\t0\t简\t0\n才", 1700, false, true));
    assert(env.sends.back().first == env.procs[p2].window);
    assert(m.send(L"", 1800, false, true));

    // Rollback install: older package gets higher sequence (seq 3 > 2).
    env.setApproved(3, L"V1");
    m.onBoundary(2000, true, true);
    assert(m.hasStandby());
    const ProcessId p3 = m.standbyProcess();
    assert(env.procs[p3].expectedName == L"MeowpuutCandidates-456-9-s3");
    env.procs[p3].ready = true;
    m.onBoundary(2200, true, true);
    assert(m.stats().source == SourceKind::Approved && m.stats().sequence == 3 && m.stats().handoffs == 2);
    assert(env.procs[p2].closed && env.procs[p2].released);
    std::cout << "PASS: per-process standby pre-launch, hide+WM_CLOSE cutover and rollback install\n";
}

// 4. Standby timeout -> terminate, 60s cooldown, failure count; newer sequence bypasses cooldown.
static void standbyTimeoutAndCooldown() {
    FakeEnv env;
    Machine m(env.ops(), L"789-1");
    assert(m.startOwn(1000));
    env.procs[m.currentProcess()].ready = true;

    env.setApproved(2, L"V2");
    m.onBoundary(1000, true, true);
    assert(m.hasStandby());
    const ProcessId p2 = m.standbyProcess();

    // Not ready before 5000ms -> still waiting.
    m.onBoundary(5999, true, true);
    assert(m.hasStandby() && env.procs[p2].alive && m.stats().failures == 0);

    // At 6000ms (5000ms elapsed) -> terminated, cooled down for 60s, failures++.
    m.onBoundary(6000, true, true);
    assert(!m.hasStandby() && env.procs[p2].terminated && env.procs[p2].released);
    assert(m.stats().failures == 1 && m.stats().source == SourceKind::Own);
    assert(m.isCooledDown(2, 65999));

    // During cooldown: seq 2 is NOT retried.
    m.onBoundary(30000, true, true);
    assert(!m.hasStandby());

    // After cooldown expires (66000ms): seq 2 is retried.
    m.onBoundary(66000, true, true);
    assert(m.hasStandby());
    const ProcessId p2Retry = m.standbyProcess();
    // Simulate early exit of standby -> fails immediately and re-cools.
    env.procs[p2Retry].alive = false;
    m.onBoundary(66100, true, true);
    assert(!m.hasStandby() && env.procs[p2Retry].released && m.stats().failures == 2);
    assert(m.isCooledDown(2, 66200));

    // Newer sequence (seq 3) is NOT blocked by seq 2's cooldown.
    env.setApproved(3, L"V3");
    m.onBoundary(66300, true, true);
    assert(m.hasStandby() && env.procs[m.standbyProcess()].sequence == 3);
    std::cout << "PASS: standby timeout, early exit, 60s cooldown and newer-sequence bypass\n";
}

// 5. Three consecutive rejects on approved target -> STATIC fallback, cooldown, return to own at boundary.
static void consecutiveRejectsAndCrashFallback() {
    FakeEnv env;
    env.setApproved(2, L"V2");
    env.setDesktop(20, false, 2);
    Machine m(env.ops(), L"111-1");
    assert(m.send(L"0\n0\na\t0\t简\t0\n啊", 1000, false, true));
    assert(m.stats().source == SourceKind::Desktop && m.stats().sequence == 2);

    // 2 transient rejects followed by success: must NOT trigger cooldown/failure.
    env.desktopAcceptSends = false;
    assert(!m.send(L"0\n0\nab\t0\t简\t0\n阿", 1100, false, true));
    assert(!m.send(L"0\n0\nabc\t0\t简\t0\n啊", 1200, false, true));
    assert(m.stats().failures == 0 && !m.isCooledDown(2, 1200));
    env.desktopAcceptSends = true;
    assert(m.send(L"0\n0\nabcd\t0\t简\t0\n啊", 1300, false, true));
    assert(m.stats().failures == 0);

    // 3 consecutive rejects -> fails, cools down seq 2, returns to own at next empty boundary.
    env.desktopAcceptSends = false;
    assert(!m.send(L"0\n0\nx\t0\t简\t0\n下", 1400, false, true));
    assert(!m.send(L"0\n0\nxy\t0\t简\t0\n下", 1500, false, true));
    assert(m.stats().failures == 0);
    assert(!m.send(L"0\n0\nxyz\t0\t简\t0\n下", 1600, false, true));
    assert(m.stats().failures == 1 && m.isCooledDown(2, 1600));
    // Still mid-composition: returns false (STATIC fallback) without switching yet.
    assert(!m.send(L"0\n0\nxyzw\t0\t简\t0\n下", 1700, false, true));

    // Hide + empty boundary -> back to own directory (per-process, since desktop is cooled down v2).
    m.send(L"", 1800, false, true);
    m.onBoundary(1800, true, true);
    assert(m.stats().source == SourceKind::Own && m.stats().sequence == 0);
    env.autoReadyOnLaunch = true;
    assert(m.send(L"0\n0\nk\t0\t简\t0\n看", 1900, false, true));
    assert(m.stats().source == SourceKind::Own && env.procs[m.currentProcess()].sequence == 0);
    std::cout << "PASS: 3 consecutive rejects trigger STATIC fallback, cooldown and return to own\n";
}

// 6. Incompatible / tie / missing approval returns to own; handoff switch off disables all handoff.
static void incompatibleTieAndSwitchOff() {
    FakeEnv env;
    env.autoReadyOnLaunch = true;
    env.setApproved(2, L"V2");
    Machine m(env.ops(), L"222-1");
    assert(m.send(L"0\n0\na\t0\t简\t0\n啊", 1000, false, true));
    assert(m.stats().source == SourceKind::Approved && m.stats().sequence == 2);
    const ProcessId p2 = m.currentProcess();
    assert(m.send(L"", 1100, false, true));

    // Incompatible version or tie -> release::select returns nullopt -> latestApproved is null.
    env.setApproved(0, L"");
    m.onBoundary(1200, true, true);
    assert(m.stats().source == SourceKind::Own && m.stats().sequence == 0);
    assert(env.procs[p2].closed && env.procs[p2].released);

    // Switch off (rendererHandoff=0): approved desktop and per-process are both ignored.
    env.setApproved(5, L"V5");
    env.setDesktop(50, false, 5);
    const uint64_t checksBefore = env.checkCount;
    m.onBoundary(5000, true, false);
    assert(env.checkCount == checksBefore); // No check when disabled.
    assert(m.send(L"0\n0\nb\t0\t简\t0\n吧", 5100, false, false));
    assert(m.stats().source == SourceKind::Own && m.stats().sequence == 0);
    assert(env.sends.back().first != 50); // Approved desktop ignored when switch is off.

    // Own desktop IS still usable when switch is off (existing behavior).
    m.send(L"", 5200, false, false);
    env.setDesktop(51, true, 0);
    m.onBoundary(5300, true, false);
    assert(m.stats().source == SourceKind::Desktop && m.stats().sequence == 0);
    assert(m.send(L"0\n0\nc\t0\t简\t0\n才", 5400, false, false));
    assert(env.sends.back().first == 51);
    std::cout << "PASS: incompatible/tie return to own and handoff switch off preserves legacy behavior\n";
}

// 7. Diagnostic fields format check & clean shutdown.
static void diagnosticsAndShutdown() {
    FakeEnv env;
    env.autoReadyOnLaunch = true;
    env.setApproved(4, L"V4");
    Machine m(env.ops(), L"333-1");
    assert(m.send(L"0\n0\na\t0\t简\t0\n啊", 1000, false, true));
    const ProcessId p4 = m.currentProcess();
    assert(m.diagnosticFields() == ",\"renderer_source\":\"approved\",\"renderer_sequence\":4,\"renderer_payload\":1,\"renderer_handoffs\":0,\"renderer_failures\":0");
    m.shutdown();
    assert(env.procs[p4].closed && env.procs[p4].released);
    assert(m.stats().source == SourceKind::Own && m.stats().sequence == 0);
    std::cout << "PASS: diagnostic JSON fields and clean shutdown\n";
}

static void registrySwitchPreference() {
    const std::wstring base = L"Software\\HarukotoTest\\RendererSwitch-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr, input = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
    struct Guard {
        HKEY &root, &input; const std::wstring &base;
        ~Guard() {
            if (input) RegCloseKey(input);
            if (root) RegCloseKey(root);
            RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\HarukotoTest");
        }
    } guard{root, input, base};
    const bool savedDefault = handoff::rendererHandoffDefault;
    handoff::rendererHandoffDefault = true;
    assert(handoff::readRendererHandoff(root)); // Absent -> default on.
    handoff::rendererHandoffDefault = false;
    assert(!handoff::readRendererHandoff(root)); // Absent -> default off when overridden.
    handoff::rendererHandoffDefault = true;
    assert(RegCreateKeyExW(root, L"Software\\Meowpuut\\Input", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &input, nullptr) == ERROR_SUCCESS);
    DWORD zero = 0, one = 1;
    assert(RegSetValueExW(input, L"rendererHandoff", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof(zero)) == ERROR_SUCCESS);
    assert(!handoff::readRendererHandoff(root));
    assert(RegSetValueExW(input, L"rendererHandoff", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one), sizeof(one)) == ERROR_SUCCESS);
    assert(handoff::readRendererHandoff(root));
    // Non-DWORD value (e.g. a path string) is ignored and falls back to default.
    const wchar_t evil[] = L"C:\\temp\\evil.exe";
    assert(RegSetValueExW(input, L"rendererHandoff", 0, REG_SZ, reinterpret_cast<const BYTE *>(evil), sizeof(evil)) == ERROR_SUCCESS);
    assert(handoff::readRendererHandoff(root));
    handoff::rendererHandoffDefault = savedDefault;
    std::cout << "PASS: rendererHandoff HKCU switch semantics\n";
}

int main() {
    desktopCutoverAndNoSwitchMidComposition();
    immediateSwitchWhenCurrentExited();
    perProcessStandbyAndRollback();
    standbyTimeoutAndCooldown();
    consecutiveRejectsAndCrashFallback();
    incompatibleTieAndSwitchOff();
    diagnosticsAndShutdown();
    registrySwitchPreference();
    std::cout << "PASS: renderer handoff state machine\n";
    return 0;
}
