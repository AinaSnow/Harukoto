# 引擎私有控制与动态端点

WIN-03b，2026-11-10。实现位于 `Windows/EngineControl.h`、`EngineHost.h`、`EngineProcess.h`。本批让可信启动所有者在同一个宿主上持续申请新连接，并读取固定容量状态；查询仍使用原[认证管道](ENGINE_PIPE.md)与业务协议，不传输用户输入到控制通道。Windows 发行及本机安装仍为 0.4.4-alpha.22。

后续进度：[受控独立所有者](ENGINE_OWNER.md)已在另一进程转交这两个操作；[受控接入](ENGINE_REGISTRATION.md)支持每客户端私有通道、自行申请端点和坏请求隔离，并增补独立撤销事件、可信进程绑定与退出后端口回收。撤销丢弃排队请求、消耗后丢弃迟到回复，客户端进入 Closed；既有查询连接保持。三个独立客户端验证首个退出和重新接入；旧对象／编号不复用，正式可信发现与 TSF 仍待做。以下原批次验证边界与控制格式保持有效，256 字节控制包未变。

2026-11-11 新增独立的[私有动态接入通道](ENGINE_ADMISSION.md)：可信启动方可在运行中创建和补充客户端能力，最多同时 8 个，旧对象和编号不复用；已替换位置不会接收旧请求的回复。它与本 Allocate／Inspect 控制通道并行，使用独立 HKRG 格式，不增加原控制操作或改变查询协议。普通应用自助注册、可信发现和 TSF 仍待做。

这是开发接口，没有面向用户的诊断菜单或命令行入口，也没有跨应用服务发现、常驻代理、TSF 接入、自动重启、版本选择或词库换代。现有进程间性能报告使用默认静态端点，不能据此声称动态分配的性能已测量。

## 所有者与信任边界

`EngineProcess::Config.dynamicEndpoints` 默认关闭。开启后，`connections` 表示同时保留的端点上限，范围 1～8；启动时没有查询端点。宿主完成词库准备后发 ready，由启动所有者通过私有控制接口申请 Listener。最后一个查询客户端退出后宿主继续存活，直到所有者发送 stop、放弃自己的 Job 或所有者进程退出。静态模式继续预建一次性端点，并在全部断开／过期后自行退出。

控制通道是一块未命名的 256 字节映射和两个未命名的手动复位事件，没有公开管道名、注册表记录或 PID 发现。只有可信启动方创建的三个句柄副本进入显式继承列表，与 ready／stop 一起交给自己挂起创建并加入 Job 的宿主；父方临时副本在恢复线程前关闭，子方接管时清除继承标记。其他原有可继承句柄不进入列表。宿主 CLI 使用 `--control-section`、`--control-request`、`--control-response` 与 `--connections`，要求完整提供，且不能与 `--pipe` 混用。

这条继承链不能直接供任意应用自助发现服务。[独立所有者](ENGINE_OWNER.md)现可持有 EngineProcess，由可信启动方交接宿主对象，不把寿命挂在首个查询客户端上，并支持[可选实例竞争](ENGINE_INSTANCE.md)及预分配客户端通道；正式常驻生命周期、可信发现与动态应用注册／认证仍待实现。绝对路径、未命名对象或随机名称都不能替代部署目录权限、程序身份和版本校验，也不声称可以隔离拥有同用户进程访问权限的恶意程序。

分配成功只返回新随机名称。EngineProcess 随领取结果附上一份保留宿主进程对象的非继承副本，权限仅 query／wait；客户端仍用该对象调用 Channel::connect，随后完成 Hello 和业务握手。不根据公开数字 PID 重新打开进程，也不把控制回复中的运行代次当成认证。

## 调用和等待

所有者仍在自己的单一服务／I/O 线程调用，不能放到 TSF 前台回调。下面是接口顺序，实际事件循环须同时处理其他连接和最近截止时间：

```cpp
EngineProcess::Config config{executable, base, themes, personal};
config.dynamicEndpoints = true;
config.connections = 8;
EngineProcess process(config);
// 合并 process.waitHandles() 和 process.deadline()，poll 至 Ready。
process.requestControl(control::Operation::Allocate, pipe::now());
// 继续合并等待；每次唤醒或到期 poll，控制状态不再 Pending 后领取。
auto result = process.takeControl(pipe::now());
if (result && result->endpoint) {
    auto channel = pipe::Channel::connect(result->endpoint->name,
                                         result->endpoint->process.get());
    // 在此连接上完成 Hello／ClientGate，而后才允许业务请求。
}
```

`requestControl(operation,time,timeoutMs)` 仅在 Ready 且控制槽 Idle 时接受；默认 5 秒，范围 1～300,000 毫秒。一次只有一个在途请求，未领取的 Ready 回复也占槽。`waitHandles()` 在 Pending 时包含回复事件，`deadline()` 合并控制绝对期限；`poll()` 用零等待推进，不额外创建定时线程。`takeControl()` 至多领取一次，返回元数据及可选 Endpoint。Full／Backend 也是可领取结果，调用方必须检查 `metadata.code`，不能把返回空 Endpoint 当作没有回复。

动态模式的 `takeEndpoint()`／`claim()` 始终返回空，避免和静态序号混用。`status().claimed` 累计成功交接的端点数量，`configured` 在动态模式表示并发上限；控制状态、固定失败类别与 Win32 数字另列，不含输入或候选。

请求和响应固定为 256 字节，以小端整数编码；包含版本、单调请求序号和操作，响应另带宿主运行代次。解码拒绝未知版本／操作、非零保留字节、非法计数或管道名称，失败不修改输出。客户端将响应关联到精确序号和操作，首次回复固定运行代次，后续变代拒绝；不接受延迟的旧回复。控制格式独立于查询协议，不增加查询 Kind。

事件同步交接共享块的独占读写权：所有者写请求后通知，宿主领取并复位请求事件，写完回复再通知；所有者领取后才能发下一条。请求事件必须保持手动复位，避免外层 WaitForMultipleObjects 提前消耗通知；两侧对象都不能被并发调用或自行修改映射内容。

## 分配、诊断与回收

| 操作／结果 | 行为 |
| --- | --- |
| Allocate／Ok | 在 I/O 线程先创建新随机 Listener，成功后才回复名称；每个名称仍只接入一次 |
| Allocate／Full | 含监听、已连接、等待回收的端点已达上限，返回固定结果，不取消其他连接或排队重试 |
| Allocate／Backend | 随机数／Listener 等分配失败，返回固定结果，不泄露异常文本 |
| Inspect／Ok | 只读取得运行代次、并发上限、监听数、已连接数、待回收数和当前任务数；不创建端点、取消任务、重置组字或读期限 |

计数是宿主所属线程处理该请求时的快照，不是原子覆盖所有后端线程的实时系统诊断；任务数包含排队、运行与尚未领取的完成对象。诊断只在内存传输，不写日志或用户文件。

一个关闭的端点必须同时满足管道取消 I/O 已回收、原 Ticket 全部返回并领取，才从宿主容器移除并释放连接槽。运行中的取消任务继续占连接和全局任务容量；后台结果只能找到原对象，不允许在原对象仍被引用时复用槽。用于追踪的端点编号在宿主寿命内只递增，新连接不复用旧编号。没有无限端点列表或请求队列。

未连接的 Listener 沿用分配时起算的 30 秒绝对期限，到期释放；领取控制回复不延长期限。未交接、领取失败或控制超时造成的孤立 Listener 也按此回收。客户端不得假定名称在领取后还能保留完整 30 秒，连接失败不能自动重放未确认 Key／Select。

控制超时在等于期限时即成立，即使回复事件已经触发；通道永久进入 Failed，不允许发新请求，晚到回复丢弃。已有查询连接仍由原通道管理，不因控制超时自动断开或重建。所有者仍能停止宿主；stop／实际退出撤销未领取结果，真实进程退出优先于控制响应。控制首次失败原因保留，停止不会覆盖原来的 Deadline／Protocol。宿主发现控制协议损坏则结束自身事件循环并按正常异常路径回收，不尝试恢复共享块。

## 验证与下一步

- `engine_control`：固定编码、保留字节和计数拒绝、单槽容量、外层等待后仍可领取、序号／操作／运行代次归属、准确到期、延迟回复、通道放弃。
- `engine_process`：动态启动句柄继承、无关句柄隔离、控制期限合并及超时不重放、请求中崩溃／停止、Job 放弃保护；使用真实故障子进程与受控到期时间，不以此断言墙钟定时精度。
- `engine_host`：空闲后继续服务、容量满、断开再分配、取消中的运行任务保留槽、迟到任务回收、未连接端点到期及显式停止。Listener 到期用 25 毫秒测试配置，其他期限维持原值。
- `engine_host_process`／`engine_host_full`：真实宿主在上限 2 下连续连接／断开 12 轮，逐键查询与直调完整候选一致，检查同代次、新名称、只读进程对象权限、诊断不改变输入、容量满与未领取回复被停止撤销；原简繁／日语／FF14／分页／部分选词／重放回归保留。
- `engine_pipe` 与 `engine_ipc_benchmark`：原认证边界、静态多进程三模式及线程／混合负载回归；基准源码指纹加入新控制头文件。动态测试客户端仍在同一个测试进程，未证明实际多应用发现与 TSF 行为。

复现：

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_control|engine_process|engine_host|engine_host_process|engine_host_full|engine_pipe|engine_ipc_benchmark)$' --output-on-failure
```

受控独立所有者的启动、可信宿主对象交接与退出监督已在[后续批次](ENGINE_OWNER.md)完成，单实例竞争和动态接入亦已增加。另有默认关闭的[只读诊断入口](ENGINE_DIAGNOSTICS.md)，缓存启动方最后自行领取的 Inspect；读取不会消费本控制槽，也不假装获得最新容量。接下来落实正式常驻生命周期和可信服务发现／交接，再接 TSF；版本及快照切换继续要求组字边界和失败回退。私有控制句柄不暴露给任意应用，也没有自动重连或按键重放。
