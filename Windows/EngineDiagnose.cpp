#include "EngineDiagnostics.h"
#include "EngineResident.h"
#include <iostream>

using namespace harukoto::engine;
namespace dg = harukoto::engine::diagnostics;
namespace rs = harukoto::engine::resident;
static const char *availability(dg::Availability value) {
    switch (value) {
    case dg::Availability::Observed: return "observed";
    case dg::Availability::Unavailable: return "unavailable";
    case dg::Availability::Inconsistent: return "inconsistent";
    case dg::Availability::Invalid: return "invalid";
    case dg::Availability::ProcessExited: return "process_exited";
    case dg::Availability::PublisherUnavailable: return "publisher_unavailable";
    }
    return "unavailable";
}
static void json(DWORD pid, const dg::Observation &o) {
    std::cout << "{\"format\":1,\"scope\":\"experimental_engine\",\"trusted_discovery\":false,\"process_id\":" << pid
        << ",\"availability\":\"" << availability(o.availability) << "\",\"win32\":" << o.win32 << ",\"snapshot\":";
    if (!o.record) { std::cout << "null}\n"; return; }
    const auto &r=*o.record;
    constexpr const char *states[]{"starting","ready","stopping","terminating","exited"};
    constexpr const char *failures[]{"none","startup_exit","unexpected_exit","startup_timeout","stop_timeout","control_io","instance_busy","deployment_identity"};
    constexpr const char *ports[]{"disabled","idle","pending","ready","failed"};
    constexpr const char *errors[]{"none","closed","io","protocol","authentication","deadline"};
    const auto now=GetTickCount64();
    auto process=[](const char *key, dg::ProcessIdentity id) {
        std::cout << '"' << key << "\":{\"pid\":" << id.pid << ",\"creation_filetime\":" << id.created << '}';
    };
    std::cout << '{'; process("root",r.root); std::cout << ','; process("publisher",r.publisher);
    std::cout << ','; process("host",r.host);
    std::cout << ",\"owner_mode\":" << (r.owner ? "true" : "false")
        << ",\"deployment_required\":" << (r.deploymentRequired ? "true" : "false")
        << ",\"deployment_verified\":" << (r.deploymentVerified ? "true" : "false")
        << ",\"published_tick_ms\":" << r.published << ",\"publication_age_ms\":" << now-r.published
        << ",\"polled_tick_ms\":" << r.polled << ",\"poll_age_ms\":";
    if (r.polled) std::cout << now-r.polled; else std::cout << "null";
    std::cout << ",\"state\":\"" << states[r.state] << "\",\"failure\":\"" << failures[r.failure]
        << "\",\"win32\":" << r.win32 << ",\"exit_code\":";
    if (r.exitCode) std::cout << *r.exitCode; else std::cout << "null";
    std::cout << ",\"configured_endpoints\":" << r.configured << ",\"supervisor_claimed_total\":" << r.claimed;
    auto port=[&](const char *key, uint32_t state, uint32_t failure, uint32_t win32) {
        std::cout << ",\"" << key << "\":{\"state\":\"" << ports[state] << "\",\"failure\":\"" << errors[failure] << "\",\"win32\":" << win32 << '}';
    };
    port("control",r.controlState,r.controlFailure,r.controlWin32);
    port("admission",r.admissionState,r.admissionFailure,r.admissionWin32);
    std::cout << ",\"registrations\":{\"configured\":" << r.registrationConfigured << ",\"issued_total\":" << r.registrationIssued
        << ",\"bound\":" << r.registrationBound << ",\"revoked_total\":" << r.registrationRevoked
        << ",\"created_total\":" << r.registrationCreated << ",\"active\":" << r.registrationActive << '}'
        << ",\"last_inspect\":";
    const auto &c=r.capacity;
    if (!c.tick) std::cout << "null";
    else std::cout << "{\"observed_tick_ms\":" << c.tick << ",\"age_ms\":" << now-c.tick << ",\"generation\":" << c.generation
        << ",\"limit\":" << c.limit << ",\"listening\":" << c.listening << ",\"connected\":" << c.connected
        << ",\"retiring\":" << c.retiring << ",\"jobs\":" << c.jobs << '}';
    std::cout << "}}\n";
}
// --resident: the self-reported resident state of this logon session's owner
// (volatile HKCU ledger, ENGINE_LIFECYCLE.md). Read only; no input content.
static int residentStatus(const rs::Store &store, const std::wstring &scope, bool asJson) {
    rs::Status s;
    try { s=rs::inspect(store,scope); } catch (...) { std::cerr << "invalid scope\n"; return 1; }
    const auto now=GetTickCount64();
    constexpr const char *reasons[]{"unknown","logon","demand"};
    constexpr const char *states[]{"none","starting","ready","restarting","stopping","exited"};
    constexpr const char *exits[]{"none","stopped","idle","logoff","shutdown","crash_loop","failed","superseded"};
    const bool running=s.alive && s.startTick && now>=s.startTick;
    if (asJson) {
        std::cout << "{\"format\":1,\"scope\":\"resident_engine\",\"trusted_discovery\":false,\"present\":" << (s.present ? "true" : "false");
        if (s.present) {
            std::cout << ",\"owner_pid\":" << s.ownerPid << ",\"alive\":" << (s.alive ? "true" : "false")
                << ",\"start_reason\":\"" << reasons[static_cast<size_t>(s.reason)] << "\",\"state\":\"" << states[static_cast<size_t>(s.state)]
                << "\",\"uptime_ms\":";
            if (running) std::cout << now-s.startTick; else std::cout << "null";
            std::cout << ",\"clients\":" << s.clients << ",\"host_pid\":" << s.hostPid << ",\"host_restarts\":" << s.hostRestarts
                << ",\"last_exit\":\"" << exits[static_cast<size_t>(s.lastExit)] << "\",\"last_exit_pid\":" << s.lastExitPid
                << ",\"last_exit_age_ms\":";
            if (s.lastExitTick && now>=s.lastExitTick) std::cout << now-s.lastExitTick; else std::cout << "null";
            std::cout << ",\"recent_on_demand_starts\":" << s.starts.size() << ",\"breaker_tripped\":" << (s.tripped ? "true" : "false");
        }
        std::cout << "}\n";
    } else {
        SetConsoleOutputCP(CP_UTF8);
        std::cout << "晴词常驻引擎状态（本登录会话，自报，仅供查看）\n";
        if (!s.present) std::cout << "没有记录：本次登录后尚未启动过常驻引擎。\n";
        else {
            constexpr const char *reasonText[]{"未知","登录启动","按需启动"};
            constexpr const char *exitText[]{"无","被要求停止","空闲退出","注销","关机","宿主反复崩溃","失败","已换代退出"};
            std::cout << "所有者 " << s.ownerPid << (s.alive ? "（运行中）" : "（已不在运行）") << "，" << reasonText[static_cast<size_t>(s.reason)];
            if (running) std::cout << "，已运行 " << (now-s.startTick)/1000 << " 秒";
            std::cout << "\n接入方 " << s.clients << "，宿主 " << s.hostPid << "，宿主重启 " << s.hostRestarts
                << " 次\n上次退出：" << exitText[static_cast<size_t>(s.lastExit)] << "\n近期按需启动 " << s.starts.size()
                << " 次，断路器" << (s.tripped ? "已打开（注销后或手动重置后恢复）" : "未打开") << '\n';
        }
    }
    return s.present ? 0 : 2;
}
int wmain(int argc, wchar_t **argv) {
    if (argc>=2 && std::wstring(argv[1])==L"--resident") {
        rs::Store store; std::wstring scope=rs::DefaultScope; bool scopeSet=false, asJson=false;
        for (int i=2;i<argc;++i) {
            const std::wstring key=argv[i];
            if (key==L"--json" && !asJson) asJson=true;
            else if (key==L"--scope" && i+1<argc) { scope=argv[++i]; scopeSet=true; }
            else if (key==L"--store-subkey" && i+1<argc) store=rs::Store::under(argv[++i]); // Tests; read only.
            else { std::cerr << "usage: harukoto_engine_diagnose --resident [--json]\n"; return 1; }
        }
        // Step 7a: production resident owners use per-release scope harukoto-s<seq>.
        // Prefer the newest present versioned scope (approved first, then any
        // draining older release), falling back to legacy DefaultScope.
        if (!scopeSet) {
            try {
                const auto snap=release::read();
                std::wstring chosen;
                if (const auto approved=release::select(snap,ProtocolVersion)) {
                    const auto versioned=rs::releaseScope(approved->sequence,scope);
                    if (rs::inspect(store,versioned).present) chosen=versioned;
                }
                if (chosen.empty() && snap.status==release::Status::Ready) {
                    uint64_t bestSeq=0;
                    for (const auto &entry : snap.entries) {
                        if (entry.sequence<=bestSeq) continue;
                        const auto versioned=rs::releaseScope(entry.sequence,scope);
                        if (rs::inspect(store,versioned).present) { bestSeq=entry.sequence; chosen=versioned; }
                    }
                }
                if (!chosen.empty()) scope=chosen;
            } catch (...) {}
        }
        return residentStatus(store,scope,asJson);
    }
    DWORD pid=0; bool asJson=false;
    for (int i=1;i<argc;++i) {
        const std::wstring key=argv[i];
        if (key==L"--json" && !asJson) asJson=true;
        else if (key==L"--process" && !pid && i+1<argc) {
            const std::wstring value=argv[++i];
            if (value.empty() || value.size()>10 || !std::all_of(value.begin(),value.end(),[](wchar_t c) { return c>=L'0' && c<=L'9'; })) return 1;
            const auto number=std::stoull(value); if (!number || number>MAXDWORD) return 1;
            pid=static_cast<DWORD>(number);
        } else { std::cerr << "usage: harukoto_engine_diagnose --process PID [--json]\n"; return 1; }
    }
    if (!pid) { std::cerr << "usage: harukoto_engine_diagnose --process PID [--json]\n"; return 1; }
    dg::Observation result;
    try {
        pipe::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,pid));
        if (!process) result.win32=GetLastError();
        else result=dg::read(process.get());
    } catch (const pipe::Error &error) { result.win32=error.code; }
    if (asJson) json(pid,result);
    else {
        SetConsoleOutputCP(CP_UTF8);
        constexpr const char *labels[]{"已读取快照","无可读快照","写入中或尚未发布","记录无效","根进程已退出","发布进程不可用"};
        std::cout << "晴词实验引擎只读诊断\n观察结果：" << labels[static_cast<size_t>(result.availability)] << "，Win32=" << result.win32 << '\n';
        if (result.record) {
            const auto &r=*result.record;
            constexpr const char *states[]{"启动中","已就绪","停止中","终止中","已退出"};
            constexpr const char *failures[]{"无","启动时退出","意外退出","启动超时","停止超时","控制错误","实例已存在","部署身份不符"};
            std::cout << "根进程 " << r.root.pid << "，发布进程 " << r.publisher.pid << "，查询宿主 " << r.host.pid
                << "\n上次状态：" << states[r.state] << "，故障：" << failures[r.failure] << "，Win32=" << r.win32
                << "\n部署校验：" << (!r.deploymentRequired ? "未开启" : r.deploymentVerified ? "已通过初始映像核对" : "尚未通过")
                << "\n接入槽：有效 " << r.registrationActive << "，已绑定 " << r.registrationBound
                << "，累计创建 " << r.registrationCreated << "，累计撤销 " << r.registrationRevoked
                << "\n距上次发布 " << GetTickCount64()-r.published << " ms；";
            if (r.polled) std::cout << "距上次轮询 " << GetTickCount64()-r.polled << " ms\n";
            else std::cout << "尚未轮询\n";
        }
        std::cout << "仅显示上次发布的元数据，不表示健康心跳或可信发现。完整字段请加 --json。\n";
    }
    return result.availability==dg::Availability::Observed ? 0 : 2;
}
