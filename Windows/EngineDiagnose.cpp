#include "EngineDiagnostics.h"
#include <iostream>

using namespace harukoto::engine;
namespace dg = harukoto::engine::diagnostics;
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
    constexpr const char *failures[]{"none","startup_exit","unexpected_exit","startup_timeout","stop_timeout","control_io","instance_busy"};
    constexpr const char *ports[]{"disabled","idle","pending","ready","failed"};
    constexpr const char *errors[]{"none","closed","io","protocol","authentication","deadline"};
    const auto now=GetTickCount64();
    auto process=[](const char *key, dg::ProcessIdentity id) {
        std::cout << '"' << key << "\":{\"pid\":" << id.pid << ",\"creation_filetime\":" << id.created << '}';
    };
    std::cout << '{'; process("root",r.root); std::cout << ','; process("publisher",r.publisher);
    std::cout << ','; process("host",r.host);
    std::cout << ",\"owner_mode\":" << (r.owner ? "true" : "false")
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
int wmain(int argc, wchar_t **argv) {
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
            constexpr const char *failures[]{"无","启动时退出","意外退出","启动超时","停止超时","控制错误","实例已存在"};
            std::cout << "根进程 " << r.root.pid << "，发布进程 " << r.publisher.pid << "，查询宿主 " << r.host.pid
                << "\n上次状态：" << states[r.state] << "，故障：" << failures[r.failure] << "，Win32=" << r.win32
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
