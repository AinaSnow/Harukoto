# 发现握手

WIN-03 阶段二第 2 步，2026-11-21。普通应用里的接入方找到本登录会话的引擎所有者，并证明为它服务的确实是批准的程序。实现在 `Windows/EngineDiscovery.h`（命名空间 `harukoto::engine::discovery`），所有者用 `--discovery 1` 发布。TSF 未调用，实际输入路径不变，安装包里的所有者也没有以发现模式启动。

## 用法

```cpp
// 服务／I/O 线程；所有调用都可能阻塞，不能放进 TSF 前台回调。
const auto outcome = discovery::discover();          // 默认：release::read()（HKLM，要求保护）
if (outcome.result != discovery::Result::Verified) { /* 留在进程内路径 */ }
const auto &engine = *outcome.engine;                 // 批准条目、两份 EngineDeployment、
                                                      // 所有者与宿主的受限进程对象
```

- `discover(snapshot, options)` 供测试注入内存中的发行快照；正式调用只能用无参版本，不能把引擎回复、HKCU、诊断映射的值放进快照。
- `Options{scope, protocol, timeoutMs}`：scope 默认 `harukoto`，与所有者 `--instance` 相同；超时默认 2 秒，上限 60 秒。
- `EngineProcess::Config.discovery = true` 让所有者发布管道，要求同时设置 `ownerInstance`。
- `discover()` 只证明身份，不交出任何能力，返回前关闭连接。要取得注册槽，用第 3 步的 `handoff::admit()`，它在同一条连接上继续交接，见[竞争失败后的交接](ENGINE_HANDOFF.md)。返回的进程对象只有查询／等待权限、不可继承，两份部署对象继续固定批准的文件和目录。

## 名称与权限

| 项 | 取值 |
| --- | --- |
| 管道名 | `\\.\pipe\Harukoto.Discovery.v1.<会话8位><登录LUID16位>.<登录SID十六进制>.<scope>`。管道名在整机共享，所以名称里带 Windows 会话、登录会话 LUID 与登录 SID |
| 创建方 | 所有者，在宿主 Ready 之后、发出 ready 之前；停止时最先关闭 |
| 实例 | 只有 1 个，用 `FILE_FLAG_FIRST_PIPE_INSTANCE` 创建，每次连接结束后 Disconnect／Connect 复用；同一时间服务一个客户端，每个连接最多 2 秒，忙时客户端用 WaitNamedPipe 等到自己的期限 |
| DACL | 受保护，只有一条：当前登录 SID，`FILE_READ_DATA｜FILE_WRITE_DATA｜FILE_READ_ATTRIBUTES｜SYNCHRONIZE`。不给 SYSTEM、不给 ALL APPLICATION PACKAGES、不给 `FILE_CREATE_PIPE_INSTANCE`（谁都不能加实例，包括所有者自己，所以只用单实例） |
| 完整性标签 | 不显式设置，取默认中完整性、禁止向上写：低完整性进程打不开 |
| 远程 | `PIPE_REJECT_REMOTE_CLIENTS`；服务端另查 `GetNamedPipeClientComputerName` 必须是本机 |

不给 SYSTEM：没有以 SYSTEM 身份接入的需求，多一条 ACE 只是多一个能连接的主体。

## 支持的组合

两端都必须：同一用户 SID、同一登录 SID、同一登录会话 LUID、同一 Windows 会话；中完整性；不是 AppContainer；令牌没有限制 SID。不满足的一方直接放弃，接入方留在进程内路径。

- 提权（高完整性）应用：不接入非提权引擎。中完整性的引擎可能被同桌面的普通程序影响，让它向提权应用的输入框送文字，等于让低完整性一方驱动高完整性一方。UAC 拆分令牌的提权一半本来就是另一个登录会话 LUID，身份比较也不会通过。
- 提权的引擎：所有者自查令牌不是中完整性时不发布管道；普通应用本来也打不开提权进程的令牌。
- 低完整性应用：管道标签拒绝写入；客户端自查先放弃。它能以只查询权限打开引擎进程和令牌（实测），即能核对身份，但接入意味着把沙箱内的输入交给中完整性进程处理，扩大沙箱逃逸面，因此不接入。
- AppContainer 应用（如 UWP、开始菜单搜索框）：DACL 里没有包 SID，打不开管道；也打不开引擎进程（实测拒绝访问），无法核对身份。若将来要支持，至少需要所有者给自身进程对象和管道加 AppContainer 的受限 ACE，并重新评估攻击面；本步不做，这类宿主更新后仍需重开。

## 接入方校验顺序

1. 线程不在模拟；本进程令牌符合上面的组合，否则 `UnsupportedClient`。
2. `release::select` 取批准条目，没有则 `NoRelease`（不碰管道）；用 EngineDeployment 固定 owner／host 文件并核对哈希，失败 `DeploymentInvalid`。
3. 按名称以 `SECURITY_IDENTIFICATION` 打开管道：不存在 `NotRunning`、拒绝 `Denied`、忙到期限 `Busy`。
4. 句柄必须是本机命名管道的客户端端（查询远程协议信息必须失败），服务端会话号等于自己，否则 `Remote`／`Identity`。
5. `GetNamedPipeServerProcessId` 取得 PID，立即以只查询／等待权限打开并一直持有——持有期间 PID 不会被复用。打不开或已退出为 `Identity`。
6. 服务端令牌：身份四项相同，否则 `Identity`；完整性不同或是沙箱，`Integrity`。
7. 映像：EngineDeployment::verify（原生路径＋文件 ID）。不符时若与同一快照里同协议的其他条目相符，记 `Stale`（升级窗口里的旧引擎，第 7 步处理），否则 `Image`。
8. 握手：发 128 字节请求（含协议号、本进程 PID、32 字节随机数），读 128 字节回复。超时 `Timeout`；格式错、随机数不符、协议不符、所有者 PID 与管道服务端不符为 `Protocol`；`Rejected`／`Unavailable` 按回复。
9. 再核对所有者仍存活且映像不变。
10. 宿主：回复里的宿主 PID 只是定位，不能等于所有者或自己；打开后比较进程创建时间（绑定到所有者持有的那个实例），再做第 6、7 步的令牌和映像核对。全部通过才是 `Verified`。

不在握手后再查询管道服务端 PID：服务端可能已经断开，而进程对象从第 5 步起一直持有，足以排除 PID 复用。

## 服务端检查

读到完整请求后（回 Ok 之后连接可继续第 3 步的交接消息，见 [ENGINE_HANDOFF.md](ENGINE_HANDOFF.md)；每个连接含交接总共最多 2 秒）：请求里的 PID 等于 `GetNamedPipeClientProcessId`，客户端会话号相同，客户端是本机；模拟客户端（标识级）取令牌，按同一组合比较；不通过回 `Rejected`（不带任何定位）。通过后才比较协议号（不同回 `ProtocolMismatch`），宿主已退出回 `Unavailable`，否则回 `Ok`、所有者 PID、宿主 PID 与宿主创建时间。写完后等客户端关闭（读到 EOF）、Admit（仅限 Ok 之后）或期限到，然后断开、重新监听。坏请求、超时只断开当前连接。

## 线格式

小端，固定 128 字节，未用字节必须为零，魔数 `HKDV`，版本 1。

| 偏移 | 请求（类型 1） | 回复（类型 2） |
| --- | --- | --- |
| 0／4／6 | 魔数／版本 uint16／类型 uint16 | 同左 |
| 8 | 协议号 uint16（非零） | 协议号 uint16 |
| 10 | 保留 | 结果 uint16：1 Ok、2 Rejected、3 ProtocolMismatch、4 Unavailable |
| 12 | 客户端 PID uint32（非零） | 所有者 PID uint32 |
| 16 | 随机数 32 字节 | 原样回显 |
| 48 | 保留 | 宿主 PID uint32 |
| 56 | 保留 | 宿主创建时间 FILETIME uint64 |

只有 Ok 带三个定位值且必须都非零，其他结果必须全为零。

## 威胁模型

- **名称、PID、回复都不是身份。** 身份来自内核：管道服务端进程对象、它的令牌和映像。随机数只用来对应请求与回复，不承担认证。
- **公开对象不能只凭映像接入。** 批准的程序谁都能启动，包括沙箱里的进程。所以除了映像，还要求服务端令牌与接入方同身份、同为中完整性且不是沙箱；测试中以低完整性运行批准映像的抢注者被判为 `Integrity`。同理，管道必须由内核绑定到被核对的进程（服务端 PID＋持有进程对象），不能用“找到一个批准进程”加“某个同名对象”拼出信任。
- **抢注。** 名称可以被预先占用。真实所有者创建时因首实例标志失败，不加入对方的管道，打印原因后继续运行但不可发现；接入方连到抢注者时按服务端进程判定（未批准映像为 `Image`，其他用户为 `Identity`，低完整性或沙箱为 `Integrity`）。结果只是拒绝服务，回退进程内路径。低完整性进程能否抢注：实测能，所以低完整性程序可以让发现失效（不能冒充）。用私有命名空间加完整性边界可以挡住这一种，留作后续加固。
- **其他用户、其他会话。** 名称不同；即使抢到同名，DACL 让本会话的登录 SID 之外的主体打不开，接入方也会因身份不符拒绝。
- **同一用户、同一完整性的恶意程序** 不在防护范围：它能以批准映像和任意参数启动所有者（测试中的“ok”抢注者因此被接受），也能直接改个人词表、注入进程。这与第 1 步及之前各步的边界一致。
- **管理员、内核、离线改盘** 不在范围内。

## 测试矩阵

CTest `engine_discovery`（`tests/test_engine_discovery.py` 生成临时词库夹具后运行 `harukoto_engine_discovery_test`）。夹具是本测试程序复制到临时目录的几份副本（追加尾部字节使哈希不同），以内存发行快照批准；不读 HKLM、不安装、不写注册表。临时目录、AppContainer 配置文件在结束时删除，所有子进程放在关闭即终止的 Job 中。

| 情况 | 实际运行 | 结果 |
| --- | --- | --- |
| 普通权限客户端 ↔ 普通权限夹具服务端 | 是，连续 5 次＋4 线程并发 | Verified，所有者／宿主 PID 正确，进程对象不可继承 |
| 普通权限客户端 ↔ 真实所有者＋宿主（`EngineProcess` 发现模式、双哈希校验） | 是，3 次＋4 线程并发 | Verified，PID 与 EngineProcess 的所有者／宿主一致；元数据中 owner／host 对调为 Image；停止后 NotRunning；缺 ownerInstance 时配置被拒绝 |
| 低完整性客户端 | 是（复制令牌降为低完整性后 CreateProcessAsUser） | 自查 UnsupportedClient；直接打开管道 ERROR_ACCESS_DENIED；能以只查询权限打开引擎进程和令牌 |
| AppContainer 客户端 | 是（临时 AppContainer 配置文件＋SECURITY_CAPABILITIES，测试后删除） | 自查 UnsupportedClient；打开管道 ERROR_ACCESS_DENIED；打开引擎进程 ERROR_ACCESS_DENIED |
| 以低完整性运行批准映像的抢注者 | 是 | Integrity；随后真实服务端创建失败（被抢注，可检测） |
| 本测试进程（未批准映像）抢注 | 是 | Image；夹具服务端以“名称已存在”退出 |
| 第二个服务端 | 是 | 首实例失败退出，第一个不受影响 |
| 远程：经 `\\localhost\pipe\...`（本机 SMB 回环） | 是 | 打开失败 ERROR_ACCESS_DENIED，服务端随后仍可正常接入 |
| 未批准映像／旧批准版本 | 是 | Image／Stale |
| 协议号不同 | 是 | Protocol（服务端回 ProtocolMismatch） |
| 无发行元数据、键缺失、无对应协议、并列、批准哈希与文件不符 | 是 | NoRelease×4、DeploymentInvalid，均不打开管道 |
| 没有服务端、服务端退出后 | 是 | NotRunning |
| 批准映像但回复错误：随机数错、宿主 PID 等于所有者、宿主创建时间不符、拒绝、宿主不可用、不回复 | 是 | Protocol、Identity、Identity、Rejected、Unavailable、Timeout |
| 令牌策略：低完整性、限制 SID、另一登录会话、另一 Windows 会话、高完整性、AppContainer | 字段级（真实降级令牌与受限令牌，其余改字段） | 都不是对等方 |
| 提权（管理员）客户端或提权引擎 | **未运行**：测试账户为过滤后的管理员令牌，非交互测试无法取得提权令牌 | 按设计：客户端自查 UnsupportedClient；所有者不发布 |
| 另一用户、另一交互会话 | **未运行**：需要另一个账户或会话 | 按设计：名称不同，DACL 与身份比较拒绝 |
| 真实跨机器远程 | **未运行**：只做了本机 SMB 回环 | 按设计：服务端拒绝远程，客户端拒绝非本机句柄 |
| 安装后在真实提权应用、UWP、开始菜单里的行为 | 未运行，待第 6 步接入 TSF 后实机 | — |

测试中观察到：低完整性进程启动子进程（夹具的假宿主）被拒绝（ERROR_ACCESS_DENIED），所以低完整性抢注者回报自己的 PID 作宿主；它在握手之前就被拒绝，不影响结论。

## 留给后续

- 第 3 步（已完成）：在同一连接上交出绑定客户端进程的注册槽，见[竞争失败后的交接](ENGINE_HANDOFF.md)。
- 第 4 步（已完成）：常驻所有者总是以 scope `harukoto` 发布，由 Explorer 按需启动，见[常驻生命周期](ENGINE_LIFECYCLE.md)。发现管道被抢注时所有者照常运行但不可发现（不退出），客户端留在进程内路径。
- 可选加固：私有命名空间＋中完整性边界描述符，防止低完整性进程抢注；AppContainer 支持的取舍。
- 成本（第 5 步）：发现握手包括两次文件固定与哈希（批准文件首次固定时）、两次进程映像核对，尚未测量。
