# 竞争失败后的交接

WIN-03 阶段二第 3 步，2026-11-21。普通应用里的接入方找到正在运行的引擎（包括刚在单实例竞争中落败的一方）后，在**同一条已核对的发现连接**上取得一个注册槽，之后自行 Allocate／Inspect 并连接查询端点。不另开公开对象，不启动第二个引擎，交接完成之前不发任何查询。实现：`Windows/EngineHandoff.h`（接入方 `handoff::admit`、`Session`、开发用启动器 `attach`）、`Windows/EngineDiscovery.h`（线格式与服务端交接阶段）、`Windows/EngineOwner.h`（槽的预留、激活与回收）。TSF 未调用，安装包不启动所有者。

## 用法

```cpp
// 服务／I/O 线程；所有调用都可能阻塞。
auto outcome = handoff::admit();                      // 默认 release::read()（HKLM）
if (outcome.result != handoff::Result::Admitted) { /* 留在进程内路径 */ }
auto &registration = outcome.session->registration(); // 与受控接入相同的 EngineRegistration
registration.request(control::Operation::Allocate, pipe::now());
// ... take() 得到 endpoint，endpoint->connect() 后走原 Hello／ClientGate
outcome.session.reset();                              // 或 release()：立即把槽还给所有者
```

- `Options.discovery.timeoutMs` 覆盖整个交换（核对＋交接），默认 3 秒；所有者每个连接最多 2 秒。
- `Options.testHook`／`testRequestId` 只供测试注入故障，正式调用不设置。
- `Session` 持有核对过的 `discovery::Engine`（批准条目、两份部署对象、所有者／宿主受限进程对象）和注册通道；注册通道在第一条请求前用同一份批准条目独立核对所有者与宿主映像。

## 协议

发现握手（ENGINE_DISCOVERY.md）回 Ok 之后，连接不关闭，继续四条 128 字节消息（魔数 `HKDV`，版本 1，未用字节为零）：

| 类型 | 方向 | 内容（偏移） |
| --- | --- | --- |
| 3 Admit | 客户端→所有者 | 8 协议号、12 客户端 PID、16 握手随机数、48 请求 id（随机非零）、56 所有者代次、64 客户端创建时间 |
| 4 Grant | 所有者→客户端 | 10 结果（1 Ok、2 Full、3 Closing、4 Rejected、5 Stale、6 Duplicate）、12 所有者 PID、16／48／56 回显、64 注册编号、72 四个句柄值（映射、请求、回复、释放事件，均在客户端进程内） |
| 5 Commit | 客户端→所有者 | 同 Admit，64 改为注册编号 |
| 6 Committed | 所有者→客户端 | 10 结果（Ok／Closing）、回显、64 注册编号 |

- **所有者代次**＝所有者进程创建时间（FILETIME）。客户端从第 1 步起持有的所有者进程对象取得，所有者用自己的值比较；不同即 Stale。每条消息都带握手随机数、请求 id 与代次，别的连接、别的尝试、上一个所有者实例的消息都对不上。
- 只有 Ok 的 Grant 带编号和句柄，且四个值非零、互不相同；其他结果这些字段必须为零。
- 所有者处理 Admit 时：消息必须与本连接的握手（随机数、PID、协议号）一致，否则断开；请求 id 在本代次内只受理一次（最近 256 个，无论结果）；代次不符回 Stale；按管道客户端 PID 以 `PROCESS_DUP_HANDLE｜SYNCHRONIZE｜QUERY_LIMITED` 打开客户端进程，要求存活、创建时间等于 Admit 所述、令牌与所有者同身份同完整性（同发现握手的组合），否则 Rejected。
- 槽的对象由所有者新建（未命名映射＋三个手动复位事件），以最小权限复制进客户端进程：映射读写，请求／回复事件 `EVENT_MODIFY_STATE｜SYNCHRONIZE`，释放事件同。编号在所有者内单调递增，最高位置 1，与启动方的编号空间分开，不复用、不回绕。
- 一个连接只受理一次 Admit；Committed 之后所有者等 EOF，再服务下一个客户端。

## 状态

所有者侧每个槽：空闲 → **预留**（Grant 已发，占容量，不参与调度，不接受请求）→ **激活**（收到 Commit 且槽仍有效，写 Committed）→ 撤销。预留期间任何失败都丢弃该槽：连接超时（慢客户端）、EOF、坏 Commit、所有者停止（发现服务对象析构时丢弃）、客户端进程退出、释放事件被置位。Committed 写失败时撤销已激活的槽。

客户端侧：核对 → Admit → 收到 Ok 的 Grant 后**立即**接管四个句柄 → Commit → 收到 Ok 的 Committed → 再核对所有者／宿主存活与映像 → 建立 EngineRegistration → 关闭发现连接。收到 Grant 之后的任何失败都会先置位释放事件再关闭句柄，所以无论所有者是否已激活，槽都会被收回。

激活后的槽沿用[受控接入](ENGINE_REGISTRATION.md)与[动态补充](ENGINE_ADMISSION.md)的全部规则（与启动方的槽共用最多 8 个、单请求在途、轮转调度、撤销后不复用）。另外：

- 槽绑定客户端进程：所有者保留其 `SYNCHRONIZE` 句柄，进程退出即撤销。
- 释放事件就是撤销事件：所有者置位表示撤销，客户端置位表示归还（`Session` 析构或 `release()`）。客户端也能 ResetEvent，但只影响自己的槽，所有者一旦观察到置位即关闭端口，之后重置无效。启动方的槽不变，仍只给客户端等待权限。

## 句柄所有权

- Grant 写入失败（客户端已断开）：客户端没读到这些值，所有者用 `DUPLICATE_CLOSE_SOURCE` 关闭客户端进程里的副本。复制中途失败同样回收已复制的部分。
- Grant 已写出：所有者不再远程关闭（客户端可能已关闭并复用了那些值），只丢弃槽；客户端读到 Grant 就自己关闭。
- 剩余的唯一残留：Grant 已写入管道、客户端却在读到之前超时，四个指向已撤销对象的句柄留在客户端进程里，随进程退出回收。槽本身不残留。

## 恰好一次

这一层的“不丢失、不重放”指：

1. **交接完成之前不发查询。** 只有收到 Ok 的 Committed 才构造注册通道；之前失败时没有任何查询或 Allocate 发出，调用方直接走进程内路径。
2. **交接本身不重放。** 每次尝试用新的随机请求 id；所有者对同一 id 只受理一次（测试用固定 id 重发得到 Duplicate，不会占第二个槽）。失败的尝试不保留半个槽：未 Commit 的槽一律丢弃，已 Commit 但客户端没收到确认的槽由客户端置位释放事件收回。重试由调用方决定，总是新的请求 id。
3. **旧回复不被接受。** Grant／Committed 必须回显本连接随机数、本次请求 id、客户端核对过的所有者代次和注册编号；对不上的判 Protocol 或 Stale。私有控制层另有请求序号与引擎代次（[私有控制](ENGINE_CONTROL.md)），所有者或宿主退出时注册通道进入 Closed，不会转向别的引擎。
4. **请求是否可重试：**

| 请求 | 层 | 幂等 | 规则 |
| --- | --- | --- | --- |
| Admit／Commit | 交接 | 否（占槽） | 不重发；失败后新尝试用新 id |
| Inspect | 私有控制 | 是 | 只读；仍不自动重试，超时永久停用该注册通道 |
| Allocate | 私有控制 | 否（创建监听端点） | 不自动重试；未连接的端点 30 秒过期，重复只浪费容量 |
| Key／刷新／取消 | 查询 | 按请求 id 去重 | 原有 ClientGate 规则，不由交接层重放 |
| Select（选词上屏） | 查询 | 否 | 绝不重放；宿主对同一请求 id 返回缓存的同一回复，ClientGate 拒绝再次应用（测试中复核） |

## 失败与结果

除 Admitted 外一律留在进程内路径，不持有槽。

| 情况 | 结果 | 实测 |
| --- | --- | --- |
| 没有引擎 | NotRunning | 是 |
| 8 个槽全占 | Full | 是 |
| 两个客户端抢最后一个槽 | 一个 Admitted，一个 Full | 是（线程并发） |
| 同一请求 id 再来一次 | Duplicate | 是 |
| Admit 声称另一个所有者代次 | Stale | 是（原始协议） |
| Admit 随机数与握手不符／同一连接第二次 Admit／Commit 编号不符 | 所有者断开，无 Grant 或槽被丢弃 | 是（原始协议） |
| 拿到 Grant 后不 Commit 就关闭 | 槽被丢弃 | 是（原始协议） |
| 慢客户端（Grant 后停 2.7 秒） | Unavailable；本进程句柄数复原 | 是 |
| 客户端进程在核对后／Grant 后／Commit 后被终止 | 未 Commit 的槽被丢弃，已激活的按进程退出撤销 | 是（真实子进程） |
| 所有者在 Grant 与 Commit 之间正常停止 | Unavailable（或 Closing） | 是；停止前已持有的会话进入 Closed |
| 所有者在核对后、Admit 前正常停止 | Unavailable | 是 |
| 所有者在核对后、Admit 前崩溃 | Unavailable | 是 |
| 所有者在 Commit 后、确认被读到前崩溃 | Unavailable | 是 |
| 发现管道忙到期限／启动锁等到期限 | Busy | 未单独构造 |
| 核对失败（发行元数据、部署、身份、完整性、映像） | Untrusted（`verification` 给出发现层原因） | 由 engine_discovery 覆盖 |

每种失败之后都探测容量：连续 Admit 直到 Full，必须恰好 8 个，证明槽回到基线。24 轮“交接＋Inspect＋释放”前后所有者与客户端进程句柄数不变（实测 101→101、95→95）。

## 启动器与并发

`handoff::attach(snapshot, AttachOptions)` 是开发与测试用的显式启动器（正式常驻生命周期是第 4 步）：

1. `admit`；Admitted 即返回。
2. NotRunning 且提供了 `start`：取启动锁（`Local\Harukoto.Starter.v1.<会话+LUID>.<登录SID>.<scope>`，受保护 DACL 只给登录 SID `SYNCHRONIZE｜MUTEX_MODIFY_STATE`），持锁后再 `admit` 一次；仍 NotRunning 才调用 `start`（以 `--discovery` 启动所有者并等到 Ready），然后再 `admit`，释放锁。每次调用最多启动一次。
3. Busy／Closing／Unavailable／Timeout 在预算内（默认 10 秒、最多 32 次）指数退避重试（10 ms 起、上限 200 ms、带抖动）；Full、Untrusted、Rejected 等立即返回。

启动锁只用来排队启动方，不是认证对象：被同用户程序占住只会让启动方等到期限（Busy），回退进程内路径；持锁者崩溃（abandoned）也安全，下一位持锁后会先重新发现。真正保证只有一个引擎的仍是所有者的[实例占位](ENGINE_INSTANCE.md)。

实测：8 个独立启动进程同时开始（共享事件放行），结果 8 个都 Admitted、都接到同一个所有者 PID，只有 1 个调用了 `start`、只启动了 1 个所有者进程，总尝试 17 次（每个 2～3 次），每个都能通过自己的槽 Inspect。第 9 个启动方在 8 槽全占时得到 Full 且不启动任何东西。`start` 失败时 2 次尝试后返回 NotRunning；不给 `start` 时 1 次即返回。

本步启动的所有者仍在启动它的进程的 kill-on-close Job 里：该启动方退出，引擎随之退出（测试中复核其后为 NotRunning），其余客户端的注册通道进入 Closed，回退进程内路径。这正是第 4 步要解决的。

## 测试

CTest `engine_handoff`（`tests/test_engine_handoff.py` 生成临时词库夹具后运行 `harukoto_engine_handoff_test`）。使用构建出的真实所有者与宿主，以内存发行快照批准；不读 HKLM、不写注册表；子进程都在关闭即终止的 Job 里，临时目录结束时删除。单元部分覆盖线格式（类型混淆、保留字节、零值、只有 Ok 带句柄、句柄重复）和客户端对 Grant／Committed 的回显、代次检查。真实部分见上表；Admitted 的会话完成 Inspect、Allocate、连接查询端点、组字“nihaoz”、部分选词“你好”与重复选词被拒。

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_discovery|engine_handoff|engine_owner_process)$' --output-on-failure
```

## 留给后续

- 第 4 步：所有者常驻（不随启动方退出）、由谁以发现模式启动、scope 的正式取值、空闲／崩溃／注销规则；`attach` 的 `start` 届时换成正式启动路径。
- 第 6 步：TSF 接入时，交接放在服务线程，失败或超时回退进程内路径。
- 提权、另一账户／会话下的交接未实测（与第 2 步相同的限制）；交接延迟未测量（第 5 步）。
