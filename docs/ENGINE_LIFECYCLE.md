# 常驻生命周期

WIN-03 阶段二第 4 步，2026-11-21。引擎所有者不再属于某个启动方的 Job：由会话的 Explorer 按需启动，常驻到注销、被要求停止、（可选）空闲超时或宿主反复崩溃为止。实现：`Windows/EngineResident.h`（接入方 `resident::ensure`、断路器、状态记录、所有者侧会话窗口）、`Windows/EngineOwner.cpp`（`--resident` 模式）、`Windows/EngineOwner.h`（空闲计时与退出原因）、`Windows/InstallSupport.ps1`（卸载时停止所有者）。TSF 仍未调用（第 6 步），安装包不启动引擎。

## 由谁启动

| 方案 | 结论 |
| --- | --- |
| 应用内直接创建（`CREATE_BREAKAWAY_FROM_JOB`） | 不采用。游戏、浏览器常把子进程放进不允许 breakaway 的 kill-on-close Job，此时创建失败；嵌套 Job 要求整条链都允许。即使能脱离，也是从任意宿主应用（包括游戏）里创建进程，容易触发反作弊。本机某代理工具的受限 shell 里，进程就处在不允许 breakaway 的 Job 中，实测被拒 |
| 登录启动（HKLM Run） | 不采用。每个登录用户都常驻约 170 MiB，不用晴词的用户也一样；启动项还可能被用户禁用，仍需按需启动兜底 |
| 计划任务 | 不采用。每用户或组主体的任务注册与清理复杂，任务也可能被禁用 |
| COM 进程外类（LocalServer32，由 DCOM 启动） | 暂不采用，列为后续备选。要在 HKLM 注册 CLSID，本步不写 HKLM；HKCU 每用户注册在受限测试环境里不生效（实测 REGDB_E_CLASSNOTREG），无法实测 |
| **请会话的 Explorer 代为启动** | **采用** |

做法：`ensure()` 在服务线程上通过 Explorer 的桌面自动化对象（ShellWindows → 桌面视图 → `IShellDispatch2::ShellExecute`）请它启动所有者。所有者成为 Explorer 的子进程：在用户自己的会话里，拿用户默认环境，不在调用方的进程树和 Job 里。宿主应用自己不创建任何进程，游戏关闭（关掉它的 kill-on-close Job）也带不走引擎。不需要任何注册。

- 只启动批准的程序：路径与哈希来自第 1 步的 HKLM 发行条目，先用 EngineDeployment 固定文件并核对哈希，固定期间请求 Explorer，传给 Explorer 的是固定文件的盘符路径；所有者启动后再按同一份元数据核对自身映像（不符退出码 3），所以从用户可写位置启动的同名程序不会服务。
- 没有 Explorer（自定义外壳、Explorer 正在重启）或 Explorer 拒绝：`Failed`，不启动任何东西，留在进程内路径，之后再试。
- Explorer 可能把它启动的进程放进它自己的 Job（实测所有者 `IsProcessInJob` 为真）。这个 Job 跟随 Explorer，不跟随任何应用；测试只断言“启动方的 Job 关闭后所有者仍在”。
- 提权、低完整性、AppContainer 客户端在发现阶段就是 `Untrusted`，不会走到启动（第 2 步的结论）。

## 名称与单实例

- scope 正式取值 `harukoto`（`resident::DefaultScope`，与发现、实例占位、启动锁相同）。
- 每个登录会话（Windows 会话＋登录 LUID＋登录 SID）至多一个所有者：实例占位 `Local\Harukoto.OwnerLease.v1.<…>.harukoto`。常驻所有者拿不到占位时最多等 5 秒（上一个所有者正在停止、宿主还持有占位），仍拿不到就退出码 183。
- 启动方排队用第 3 步的启动锁；锁内再发现一次，仍没有引擎才考虑启动。

## 所有者常驻模式

```
harukoto_engine_owner.exe --resident demand [--base 目录] [--themes 目录] [--personal 文件]
```

不接受继承句柄，没有启动方控制口；Windows 子系统（不开控制台窗口）。启动顺序：读 HKLM 发行元数据并选中条目 → 核对自身映像 → 实例占位 → 状态记录 → 隐藏窗口 → 启动宿主（宿主路径与哈希来自同一条目，放在所有者自己的 kill-on-close Job 里）→ 宿主 Ready 后发布发现管道。数据路径默认 `--base` 为版本目录本身，主题与个人表不加载；正式的数据布局由第 6 步决定。`harukoto_engine_owner_test.exe` 是同一源码的测试构建，另外接受 `--scope`、`--release-subkey`、`--store-subkey`、`--idle-ms`，不打包。

### 状态机

| 状态 | 进入 | 离开 |
| --- | --- | --- |
| Starting | 所有者启动 | 宿主 Ready → Ready；宿主启动失败 → Restarting 或 Exited(CrashLoop) |
| Ready | 宿主 Ready，发现管道已发布 | 宿主退出 → Restarting；停止请求／空闲 → Stopping |
| Restarting | 宿主崩溃、启动失败或控制口失败 | 退避后 → Starting 新宿主；退避期间收到停止请求 → Exited |
| Stopping | WM_ENDSESSION、WM_CLOSE、空闲超时 | 先关发现管道、撤销全部接入槽，再停宿主（宽限 3 秒）→ Exited |
| Exited | 记录退出原因 | — |

退出码：0 干净退出（停止、空闲、注销、关机），3 不是批准的程序，4 宿主反复崩溃，183 本会话已有所有者，1 其他失败。

## 规则

**空闲。** 默认不因空闲退出。理由：重启要经过 Explorer 再加载词库，期间回退进程内路径（那个应用也要加载自己的词库）；空闲进程的工作集会被系统修剪，常驻的实际代价小于约 170 MiB 的提交量。可用 `HKCU\Software\Meowpuut\Engine` 的 DWORD `IdleMinutes` 开启：0 或不存在表示不退出，1–4 按 5，超过 1440 按 1440，类型不对按 0。所有者启动时读取一次。空闲指没有已激活或预留的接入槽、没有在途请求；最后一个客户端离开时重新计时。

**注销与关机。** 隐藏的顶层窗口（类名 `Harukoto.ResidentOwner.v1`，不显示）收到 WM_QUERYENDSESSION 一律同意，不阻止注销；收到 WM_ENDSESSION 后发出停止，等宿主停完（最多 4 秒）再返回，记录 Logoff／Shutdown。所有者被系统强制结束时，宿主随所有者的 Job 一起结束。

**睡眠与唤醒。** PBT_APMRESUMEAUTOMATIC／PBT_APMRESUMESUSPEND 重新开始空闲计时，避免唤醒后立刻因“睡前空闲”退出。

**宿主崩溃（所有者还在）。** 所有者重启宿主：10 分钟内最多 3 次，分别在 1、2、4 秒后；第 4 次崩溃时所有者以 CrashLoop 退出（码 4）并打开断路器。旧宿主上的会话进入 Closed，客户端重新交接到新宿主。

**所有者崩溃。** 客户端的注册通道与查询连接进入 Closed，回退进程内路径；下一次 `ensure()` 发现 NotRunning，在断路器允许时请 Explorer 重新启动。断路器记录自上次干净退出以来的按需启动（易失注册表，见下）：10 分钟内第 2 次至少距上次 2 秒，第 3 次至少 10 秒，第 4 次打开断路器，直到注销或显式 `resident::reset()`。干净退出（空闲、注销、被要求停止）清零。所有者是否在等待时间内发布管道都计入（Pending），Explorer 拒绝、没有批准条目则不计。

**丢失什么、不重放什么。** 引擎侧的组字状态（未确认的拼音、候选页、未完成的部分选词）随宿主丢失；已经上屏的文字在应用里，不受影响。客户端从空组字重新开始（第 6 步决定是否把当前按键序列交给进程内路径重建）。选词等非幂等请求从不重放：旧连接已关闭，宿主不会收到重复的 Select；交接与控制请求的规则见[竞争失败后的交接](ENGINE_HANDOFF.md#恰好一次)。

## 接入方

```cpp
// 服务／I/O 线程；可能阻塞数秒。
auto out = resident::ensure();                   // 默认 release::read()（HKLM）、scope harukoto
if (out.attach.outcome.result != handoff::Result::Admitted) { /* 留在进程内路径 */ }
auto &session = *out.attach.outcome.session;     // 与第 3 步相同的 Session
```

`EnsureOptions`：`budgetMs`（默认 10 秒，整个过程）、`startWaitMs`（启动后等管道，默认 15 秒，不超过预算）、`policy`、`store`、`ownerArguments`（数据路径；测试构建的开关）。`launch` 给出启动环节的结果：NotNeeded（已有引擎）、Started、Pending（Explorer 已受理但到时没有管道）、Failed、Backoff、Tripped、NoRelease。

## 升级与卸载

- 升级：安装器不停止正在运行的所有者，旧所有者继续服务（它在启动时核对过自己，之后不再核对）；旧所有者退出后，下一次启动的是新批准的版本。已打开的应用在组字边界切换到新引擎是第 7 步。
- 卸载：`uninstall-ime.ps1 -Unregister` 在删除发行元数据（之后谁也启动不了新的所有者）后调用 `Stop-HarukotoEngineOwners -Root <Program Files\Meowpuut\IME>`：对路径在该目录下的所有者，先给本会话桌面上的隐藏窗口发 WM_CLOSE（优雅停止），5 秒后仍在运行的（包括其他会话的）强制结束，遗留的宿主同样处理。版本文件照旧保留。安装时不调用。

## 诊断

所有者把自报状态写在易失键 `HKCU\Software\Meowpuut\EngineState.v1\<会话8位><登录LUID16位>.<scope>`（注销即消失；名称带 LUID，同一用户再次登录看不到上次的值）：所有者 PID 与创建时间、启动原因、启动时刻、状态、接入方数量、宿主 PID、宿主重启次数、上次退出原因／时刻／PID、近期按需启动、断路器。不含任何输入内容。同用户程序可以读改这个键，它只影响断路器与诊断，从不决定运行哪个程序或信任谁。

```powershell
& ./.build/windows/msvc/Release/harukoto_engine_diagnose.exe --resident          # 中文摘要
& ./.build/windows/msvc/Release/harukoto_engine_diagnose.exe --resident --json   # 字段：owner_pid、alive、start_reason、state、uptime_ms、clients、host_pid、host_restarts、last_exit、recent_on_demand_starts、breaker_tripped 等
```

`alive` 由 PID 加创建时间判断；记录了 Ready 而 `alive=false`、又没有退出原因，说明所有者崩溃了。

## 测试

CTest `engine_resident`（`tests/test_engine_resident.py` 生成临时词库后运行 `harukoto_engine_resident_test`，约 50 秒）。把测试构建的所有者和宿主复制到临时版本目录，用 HKCU 测试键批准（读取路径与正式相同，只是不要求 ACL 保护）；真实 Explorer 启动所有者；不写 HKLM、Run 键、计划任务或 COM 注册。断言失败时 SIGABRT 处理函数结束本测试启动的所有者并删除测试键。

| 情况 | 结果 |
| --- | --- |
| 单元：断路器间隔／断路／窗口过期／异常时刻，宿主重启退避，IdleMinutes 边界与类型，状态键确为易失（其下不能建持久子键）、设置键仍持久，篡改的批准哈希不触达 Explorer | 通过 |
| 所有者读到的元数据不批准自己 | 直接启动退出码 3、不写状态；经 Explorer 启动为 Pending、NotRunning |
| 启动方在**不允许 breakaway 的 kill-on-close Job** 里 | Explorer 启动所有者，启动方 Admitted；关闭该 Job（启动方被结束）后所有者与宿主仍在，状态 Demand、接入方回到 0，再次交接成功 |
| 另一个客户端在同类 Job 里接入后随 Job 被结束 | 所有者继续服务，按需启动计数不变 |
| 所有者被强制结束 | 持有的注册通道 Failed、查询连接关闭（组字丢失、未重放）；状态无退出记录；4 个并发启动进程（同类 Job）恰好 1 次启动、全部接到同一个新所有者；新宿主从空组字开始，“nihao”选“你好”正常 |
| 断路器（测试策略：2 次、间隔 8 秒） | 紧接着的第 2 次 Backoff，8 秒后 Started，第 3 次 Tripped 且保持，reset 后 Started |
| 宿主连续被结束 | 前 3 次所有者重启宿主（PID 不变、重启计数 1／2／3、每次都能交接），第 4 次所有者退出码 4、状态 CrashLoop、断路器打开，之后 ensure 为 Tripped |
| 空闲（测试 3 秒） | 持有接入槽 4 秒不退出；释放后 1.5 秒发唤醒消息，3.8 秒时仍在，随后退出码 0、原因 Idle、按需启动计数清零 |
| WM_QUERYENDSESSION／WM_ENDSESSION（发到隐藏窗口） | 前者回 TRUE 且不退出；后者返回时宿主已结束，退出码 0、原因 Logoff，客户端通道 Failed |
| 诊断 `--resident --json` | owner_pid、alive、demand、clients=1、host_pid 与实际一致 |
| `Stop-HarukotoEngineOwners`（经 pwsh 调用安装器函数） | 1 个优雅、0 个强制，退出码 0、原因 Stopped，之后 NotRunning |
| 本会话已有所有者时再启动一个 | 等约 5 秒后退出码 183，原所有者不受影响 |

`engine_release_install` 增加：卸载在删除元数据之后停止所有者、安装脚本不停止所有者；根目录下不响应 WM_CLOSE 的同名进程被强制结束，根目录外的同名进程不受影响。

**运行环境注意。** 在路径或注册表被虚拟化的沙箱里（例如某些代理工具的受限 shell），测试进程写的 HKCU 测试键对 Explorer 启动的进程不可见，所有者会以码 3 退出、本测试失败；请在普通终端运行 CTest。

## 未验证

- 真实注销、关机、睡眠唤醒（只向隐藏窗口发送了对应消息）。
- 已安装路径下的实际运行：HKLM 元数据、Program Files 版本目录、真实卸载停止其他会话的所有者。
- 没有 Explorer、Explorer 重启中、另一会话或另一用户、提权客户端。
- 安装包的数据布局（版本目录内没有 base-dictionary.json，正式启动会失败并很快打开断路器）——第 6 步接入前不会被调用。

## 留给后续

- 第 5 步：测量按需启动延迟（Explorer 受理＋所有者核对＋宿主加载词库）、占位等待、常驻内存与空闲工作集，据此复核空闲默认值、`startWaitMs` 与断路器间隔。
- 第 6 步：确定所有者的数据路径；TSF 在服务线程调用 `ensure()`，失败、Pending、超时都回退进程内路径。
- 第 7 步：新版本出现时的换代与旧所有者退出。
- 备选加固：HKLM 注册的 COM LocalServer32 作为不依赖 Explorer 的启动方式。
