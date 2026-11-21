# 开发状态与接手入口

## 2026-11-22 · WIN-03 第 6b 步：模糊音、纠错与学习记录进入引擎

- 前置：6a 已提交（6ccb156）。本批未提交、未打包。设计见[接入 TSF](ENGINE_TSF.md)第 3、10 节。
- 协议：`Payload::Query`（按键＋`QuerySettings`：模糊音、纠错、学习开关、小时、三份模型摘要）；候选页标记字节第 1 位为 `behind`。旧的 3 字节按键仍可用。
- 引擎：新增 `Windows/EngineMemory.h`（摘要、`buildPersonal`、`MemoryView`、只读 `MemoryMirror`：不同则增量读→每秒至多一次全读→`behind`）；`WindowsDictionary::load` 增加学习文件路径，查询时设模糊音、纠错、个人表并在扩展前应用选词记忆；宿主、`EngineProcess::Config::memory`、所有者均接受 `--memory`。
- 选词记忆：`Model`／`WordModel`／`UserWordModel` 增加 `state()`（精确内部状态，仅用于摘要）；`Journal` 增加修订号，`Store` 暴露 `phrasesRevision()`／`wordsRevision()`。文件格式、写入与压缩行为不变。
- 接入层：删除 `settings`／`personal` 门槛；`engineSettings()` 按修订号缓存摘要；`behind` 时该段进程内、同一状态 2 秒内不再询问；诊断加 `engine_memory_behind`，`DiagnosisSupport.ps1` 可选解析。
- 测试：`engine_tsf_full` 扩展为 8 个对照场景（结果见 ENGINE_TSF 第 10 节，不一致全为 0，进程内只因长列表）与脚本驱动的逐项检查（学习、忘记、开关、`-Clear`／`-AddWord`／`-RemoveWord`／`-ClearWords`，用真实管理脚本，只写临时目录）；`engine_messages` 补 Query 与 behind 编解码。评测 tuned／holdout（只看总数）／words-dev／abbrev-dev 与 cn07sym、typo-dev（开与关）／fuzzy-dev（--fuzzy 2047）与 cn05 除时间与内存外相同（`chinese-*-win036b*.json`）。CTest 共 67 项，最后一次代码修改后完整构建、复制 TIP 到 `.build/windows/ime`，在 PowerShell 中一次运行全部通过（643.03 秒，-j 4）；之后无残留进程、HKCU 测试键或本批临时目录。
- 未验证：多个真实应用同时学习时的追上延迟；大学习文件（接近 4 MiB）下每次学习后的摘要耗时（接入层与引擎各算一次，未测）；安装布局中所有者传 `--memory`（6c）。
- 下一步：6c（部分选词与分段上下文、深页、F6、以词定字、正式连接与安装布局数据路径）。

## 2026-11-21 · WIN-03 第 6a 步：接入 TSF（开关、异步与回退，基础拼音候选）

- 前置：第 5 步已提交（49b7e06）。本批未提交、未打包。设计与子批次见[接入 TSF](ENGINE_TSF.md)。
- 协议：候选加核心下标＋1 与标记（日语／强证据／直出／扩展轮次）；`Open` 回复可带数据身份（`EngineIdentity.h`，基础快照代次＋主题代次＋个人表哈希）；`ClientGate` 接受 68 字节的 Open 回复；宿主把 `WindowsDictionary::identity()` 交给每个连接。`EngineMessagesTests` 偏移随编码调整并补新字段编解码。
- 接入层：新增 `Windows/EngineTsf.h`（开关读取、Driver、工作线程 Link、PipeTransport、rebuild、Client）与 `EngineTsfResident.h`（工作线程上 `resident::ensure()`＋交接＋申请端点）；`TextService.cpp` 只在编辑读音的刷新处允许走引擎，回复经隐藏消息窗口回到 TSF 线程，读取候选的键之前补算，`resetCandidates` 为组字边界；`ThemeReload::personal()`；运行时状态与 `diagnose-ime.ps1` 加引擎路径字段（`DiagnosisSupport.ps1` 可选解析，旧记录显示“未报告”）。TIP 与两个服务测试加链接 shell32。
- 能力门槛：学习开启或有用户词、模糊音、关纠错、部分选词／分段上下文、F6、数字模式、数据身份不同（含安装目录）、第 0 轮超过 128 项时走进程内并记原因。
- 测试：新增 CTest `engine_tsf_full`（关闭路径、异步／过期／在途补算／超时／粘滞／身份／门槛、真实宿主进程管道、4,304 个前缀对照不一致 0）。评测 tuned／holdout（只看总数）／words-dev 与 cn07sym、ff1404 除时间与内存外相同（`chinese-*-win036a.json`）。CTest 共 67 项，最后一次代码修改后完整构建、复制 TIP 到 `.build/windows/ime`，在 PowerShell 中一次运行全部通过（168.60 秒，-j 4）；之后无残留进程、HKCU 测试键或本批临时目录。
- 未验证：真实应用中开启开关（生产没有 HKLM 发行元数据、安装目录不是快照，开启后会稳定走进程内）；非按键时刷新候选窗在各类宿主（UI-less、游戏）中的表现；多应用同时开启。
- 下一步：6b（模糊音、纠错、个人词频、用户词、选词记忆进入引擎，身份纳入修订号）。

## 2026-11-21 · WIN-03 第 5 步：成本量化

- 前置：第 4 步已提交（99c37e2）。本批未提交、未打包。TSF 与实际输入路径未改动。
- 新增测量工具 `harukoto_engine_cost_benchmark`（`Windows/tests/EngineCostBenchmark.cpp`）与 `scripts/benchmark-engine-cost.py`：真实 Explorer 按需启动测试所有者（HKCU 测试键批准），真实客户端子进程经交接取槽；冷启动时间线、握手分段、固定散列、状态键、空闲 CPU、修剪后首键、0→8 会话开销、1／2／4／8 客户端逐键（tuned／holdout／words-dev＋FF14，3,464 键）进程内对引擎 2／4／8 线程。`benchmark-engine-ipc.py` 允许 8 客户端。报告：`docs/benchmarks/engine-cost-win03-5.json`、`engine-ipc-8clients-summary.json`（生成于改默认值之前，source_dirty=true）。
- 结果：按需启动到 Admitted 中位 1.75 秒（宿主加载 1.69 秒，Explorer＋所有者约 45 ms）；热 `admit()` 1.1 ms；线程≥客户端时经引擎每键中位多 0.06～0.10 ms，2 线程 8 客户端中位 12.5 ms（排队）；8 客户端总私有量 1,485→211 MiB，1 客户端多 3 MiB；空闲 CPU 0，状态键写约 0.06 ms。
- 决定与改动：`resident::HostThreads`=8（常驻所有者默认，等于接入槽上限）；`resident::KeyDeadlineMs`=1000（第 6 步用）；`EnsureOptions::startWaitMs` 15→10 秒（与预算一致）；空闲、断路器、发现／交接超时不变。`EngineDeployment::shared()`：本进程仍持有固定对象时复用、不再散列（发现 2.3→0.8 ms，冷路径散列 7→2 次），`discover()`／`resident::start()`／`ensure()` 使用。测试构建所有者另接受 `--threads`。
- 测试：新增 CTest `engine_cost_benchmark`；`engine_deployment`（shared 复用与释放后重新散列）、`engine_handoff`（并发会话共用固定对象）、`engine_resident`（默认值、常驻宿主线程数）补断言。CTest 共 66 项，最后一次代码修改后完整构建、在 PowerShell 中一次运行全部通过（423.32 秒）；之后无残留进程、HKCU 测试键或本批临时目录（%TEMP% 下 2026-11-20 的 harukoto-base-production-* 为此前遗留，未动）。
- 未验证：冷磁盘缓存、低配机器、真实应用并发度与 TSF 调用方式。详见[常驻引擎成本](ENGINE_COST.md)。
- 下一步：第 6 步接入 TSF（服务线程 `ensure()`、每键期限、组字边界回退、正式数据路径）。

## 2026-11-21 · WIN-03 第 4 步：正式常驻生命周期

- 前置：第 3 步已提交（0c20ba0）。本批未提交、未打包。TSF 与实际输入路径未改动，TIP 未改、未复制 DLL。
- 决定：不在登录时启动。接入方 `resident::ensure()`（`Windows/EngineResident.h`）在启动锁内再发现、查断路器，然后经 Explorer 桌面自动化（ShellWindows → `IShellDispatch2::ShellExecute`）启动批准的所有者 `--resident demand`：所有者是 Explorer 的子进程，不在应用的 Job 里，宿主应用不创建进程，无需注册。比较过应用内 breakaway（不允许 breakaway 的 Job 中失败，本机受限 shell 实测被拒）、HKLM Run（每用户常驻）、计划任务、COM LocalServer32（需 HKLM CLSID；HKCU 每用户注册在受限环境不生效）。
- 所有者常驻模式（`EngineOwner.cpp`）：读 HKLM 发行元数据并核对自身映像（不符码 3）→ 实例占位（scope `harukoto`，最多等 5 秒，否则 183）→ 易失状态键 → 隐藏窗口（WM_QUERYENDSESSION 同意、WM_ENDSESSION／WM_CLOSE 停止、唤醒重置空闲）→ 宿主循环。宿主崩溃重启 10 分钟 3 次（1／2／4 秒），第 4 次码 4 并打开断路器。空闲默认不退出，`HKCU\Software\Meowpuut\Engine\IdleMinutes` 0 或 5–1440。所有者改为 Windows 子系统（`/ENTRY:wmainCRTStartup`），新增不打包的测试构建 `harukoto_engine_owner_test`。`EngineOwner.h` 增加可选启动方口（常驻模式为空）、空闲计时、退出原因与观察回调。
- 断路器与诊断：易失键 `HKCU\Software\Meowpuut\EngineState.v1\<会话+LUID>.<scope>`。按需启动自上次干净退出起计，10 分钟内间隔 0／2／10 秒，第 4 次断路至注销或 `resident::reset()`；干净退出清零。所有者自报 PID、原因、状态、接入方、宿主、重启次数、上次退出。`harukoto_engine_diagnose --resident [--json]`。
- 安装器：`Stop-HarukotoEngineOwners -Root`（本会话 WM_CLOSE，超时后强制，遗留宿主同样处理），`uninstall-ime.ps1 -Unregister` 在删除元数据后调用；升级不停止旧所有者（第 7 步）。没有 Run 键、COM 或计划任务注册。
- 测试：新增 CTest `engine_resident`（约 50 秒，真实 Explorer 启动，HKCU 测试键批准）：不允许 breakaway 的 kill-on-close Job 中的启动方启动所有者，Job 关闭后所有者仍在并继续服务；所有者崩溃后 4 个并发启动方只启动 1 次；断路器、宿主崩溃循环、空闲与唤醒、会话结束消息、诊断、安装器优雅停止、单实例。`engine_release_install` 增加卸载停止所有者（根目录内强制、根目录外不动）。
- CTest 共 65 项，最后一次代码修改后完整构建、在 PowerShell 中一次运行全部通过（399.17 秒）；之后无残留进程、HKCU 测试键或临时目录。
- 环境注意：本机代理工具的 Bash 受限 shell 虚拟化 HKCU，Explorer 启动的所有者看不到测试键，`engine_resident` 会失败；在普通终端（或 PowerShell）运行 CTest。
- 未验证：真实注销／关机／睡眠、安装路径与真实卸载、多会话、没有 Explorer、提权。安装包版本目录没有 base-dictionary.json，第 6 步前所有者的数据路径未定。
- 下一步：第 5 步成本量化（含按需启动延迟、常驻内存与空闲工作集，复核空闲默认值与断路器间隔）。

## 2026-11-21 · WIN-03 第 3 步：竞争失败后的交接

- 前置：第 2 步已提交（6a94f4f）。本批未提交、未打包。TSF 与实际输入路径未改动。
- 协议：发现握手回 Ok 后同一连接继续 Admit(3)→Grant(4)→Commit(5)→Committed(6)，128 字节 HKDV。每条带握手随机数、随机请求 id、所有者代次（所有者进程创建时间）；所有者在本代次内对同一 id 只受理一次（Duplicate），代次不符 Stale。所有者按管道客户端 PID 打开客户端进程（存活、创建时间符合、令牌同组合），新建映射＋三个事件以最小权限复制进去；Commit 前槽只预留不调度，失败即丢弃；Committed 写出后激活。槽绑定客户端进程退出；释放事件即撤销事件，客户端可置位归还。编号最高位置 1，与启动方编号分开。
- 接入方 `Windows/EngineHandoff.h`：`handoff::admit()` 返回 `Session`（核对过的 Engine＋带双部署策略的 EngineRegistration）；13 种结果，非 Admitted 留在进程内路径且不持有槽；收到 Grant 后的任何失败先置位释放事件。`attach()` 为开发用启动器：启动锁 `Local\Harukoto.Starter.v1.<身份>.<scope>` 排队，持锁再发现，仍无引擎才调用 `start`，暂时性结果指数退避重试。
- 所有者 `EngineOwner.h`：预留／激活／释放三态，等待集合加入客户端进程句柄；发现服务析构（停止）丢弃半途的槽；客户端在复制中途消失只让本次交接失败，不关闭发现管道。
- 测试：新增 CTest `engine_handoff`（真实所有者／宿主、内存发行快照、真实客户端子进程）：会话完成 Inspect／Allocate／查询／部分选词与重复选词被拒；8 槽满 Full、抢最后一槽一胜一 Full、8 线程并发全部 Admitted、重复请求 id Duplicate、原始协议的 Stale／坏随机数／不 Commit／坏 Commit／二次 Admit；慢客户端 Unavailable；客户端在三个阶段死亡；所有者在 Grant 后停止、核对后停止、核对后崩溃、Commit 后崩溃均 Unavailable。每种之后容量恢复 8；24 轮前后句柄数不变。8 个并发启动进程：全部 Admitted、同一所有者、只启动 1 次。
- 顺带修复：`ServiceFocusTests.cpp` 8 个 HKCU 测试键改用 `TestKey`（成功时删除、异常时析构删除、断言失败时 SIGABRT 处理函数删除）。现存的 `HKCU\Software\HarukotoQuickInputService-18496` 未删除，待用户决定。
- CTest 共 64 项，完整构建后在一次运行中全部通过（364.46 秒）；之后对交接失败处理加了一处保护，重新构建后 engine_handoff／engine_discovery／engine_owner_process／engine_owner_full／engine_admission／engine_diagnostics 再次通过。HKCU 下除既有 -18496 外无测试键残留。
- 下一步：第 4 步正式常驻生命周期（所有者不随启动方退出、由谁以发现模式启动、scope 正式取值、空闲／崩溃／注销规则）。

## 2026-11-21 · WIN-03 第 2 步：发现握手

- 前置：第 1 步已提交（9320496）。本批未提交、未打包。TSF 与实际输入路径未改动。
- 新增 `Windows/EngineDiscovery.h`：所有者（`--discovery 1`，需 `--instance`；`EngineProcess::Config.discovery`）在宿主 Ready 后发布 `\\.\pipe\Harukoto.Discovery.v1.<会话+登录LUID>.<登录SID>.<scope>`，单实例、首实例标志、拒绝远程、DACL 只给登录 SID 读写（无 create-instance、无 SYSTEM、无 AppContainer），默认中完整性标签；创建失败（被抢注、令牌不是中完整性）只打印原因，引擎照常运行但不可发现。
- 接入方 `discovery::discover()`：自身令牌检查 → `release::select` → EngineDeployment 固定 → 打开管道 → 本机／会话检查 → 持有服务端进程对象 → 令牌（同用户／登录 SID／LUID／会话、中完整性、非 AppContainer、无限制 SID）→ 映像（旧批准条目判 Stale）→ 128 字节 HKDV 随机数握手 → 宿主按 PID＋创建时间绑定后同样核对。17 种结果，非 Verified 一律留在进程内路径；只证明身份，不交出注册槽或端点（第 3 步）。
- 决定：提权、低完整性、AppContainer 宿主不接入（完整性方向与沙箱攻击面），留在进程内路径；所有者不是中完整性时不发布。
- 测试：新增 CTest `engine_discovery`（夹具副本＋内存发行快照＋真实所有者／宿主）：普通权限 Verified（含并发）；低完整性与 AppContainer 客户端实测管道 ACCESS_DENIED，AppContainer 打不开引擎进程；低完整性运行批准映像的抢注者判 Integrity；未批准抢注判 Image；远程 `\\localhost` 打开失败；Stale／Image／Protocol／Timeout／Rejected／Unavailable／Identity 各错误回复；无元数据不碰管道。提权、另一账户／会话、跨机器未实测。CTest 共 63 项，完整构建后在一次运行中全部通过（347.48 秒）。
- 已知：`tsf_focus_cancel`（ServiceFocusTests.cpp 的 `quickInput()`）在断言失败时会留下 `HKCU\Software\HarukotoQuickInputService-<pid>`（无 RAII 删除）；现存的 -18496 即此来源，未处理。
- 下一步：第 3 步竞争失败后的交接（在 Verified 的连接上交出绑定客户端进程的注册槽）。

## 2026-11-21 · WIN-03 第 1 步：可信发行元数据来源

- 前置：alpha.29 已打包（f5df2c6，文档 77ce616）。本批未提交、未打包。TSF 与实际输入路径未改动。
- 决定：信任根为 `HKLM\SOFTWARE\Harukoto\EngineReleases`（64 位视图），每个版本目录一个 REG_MULTI_SZ 值（固定 8 行：format／id／sequence／protocol／host／host-sha256／owner／owner-sha256）。不用版本目录内清单：旧 TIP 找不到新版本，且属于“候选同目录清单”。安装器显式设置键及父键描述符（O:BA，SY／BA 完全控制，BU／AC／LPAC registryRead 只读，不继承）。
- 读取方 `Windows/EngineRelease.h`：严格解析与上限（≤8 值、单值 ≤8 KiB、路径 ≤1024、固定程序名、目录名等于 id）；键及上级键、版本目录及上级目录须只由 SYSTEM／Administrators／TrustedInstaller 可写（CREATOR OWNER 占位 ACE 忽略，条件 ACE 含写入即拒绝）；单个坏值只排除自己，未知格式跳过；`select` 取协议号相同且 sequence 最大者，并列不批准。Missing／Unprotected／Invalid 一律不批准。
- 写入方 `InstallSupport.ps1`：`Publish-HarukotoEngineRelease` 在注册成功后从 Program Files 内已复制文件计算哈希，sequence＝最大值＋1，写后读回核对，清理损坏与已删除版本、最多保留 4 个；失败只警告。`Unpublish-HarukotoEngineRelease` 由卸载在注销注册后调用。`package-ime.ps1 -MSVC` 打包 harukoto_engine_host.exe／harukoto_engine_owner.exe；alpha.29 等无引擎程序的包升级时不建键。
- 测试：新增 CTest `engine_release`（解析、解码边界、SDDL 判定、System32／Program Files／HKLM\SOFTWARE 为受保护而临时目录／HKCU 不受保护、HKCU 测试键上的多版本／损坏／超限／并列／ACL 拒绝）和 `engine_release_install`（PowerShell 写入经 C++ 读取器核对，10 项，只写 HKCU 测试键）。InstallSupportTests.ps1 14 项仍通过。完整构建后 62 项 CTest 在一次运行中全部通过（402.18 秒）。TIP 未改，未复制 DLL。
- 未做：真实提权安装写 HKLM、低完整性／AppContainer／LPAC 进程读取、管理员宿主——待安装实测。下一步为第 2 步发现握手。

## 2026-11-21 · alpha.29 试用包

- 版本号升至 0.4.4-alpha.29（f5df2c6），内容为 alpha.28 加常用符号（55e8def）。从干净提交重新生成词库、完整 MSVC Release 构建并复制 DLL 到 ime 目录，60 项 CTest 在一次运行中全部通过（372.77 秒）。
- 生成 Harukoto-0.4.4-alpha.29-windows-x64.zip（67.4 MiB，SHA-256 fbcf47ec28edec846432d493616d8d6d7ed61110c89042023a24a1c90a5d546b）和 Harukoto-0.4.4-alpha.29-Setup.exe（172.0 MiB，SHA-256 6f7d1b4da437b8f38b28eb2944ea43662ed1b98d76711d6c3ff1a82fb0836ea3）。BUILD-INFO 记录源提交 f5df2c6、不含未提交修改；包内 DLL 与测试所用 Release 构建哈希一致，DLL 内版本为 alpha.29。
- 维护者本机启用的 FF14 主题快照仍为 0.3.0，安装器不替换已启用的主题包，需在主题词库管理中导入并启用 0.4.0。本机未安装，未发布，安装包未签名；实机抽查待维护者按 VAL-01 进行。此条为打包后记录。

## 2026-11-21 · CN-07 常用符号

- 前置：alpha.28 已打包（d0766e9，文档 3539bf7）。本批未提交、未打包。核心未改动。
- 方案：v＋类别名拼音首字母，整串完全相同时生成，规则同 vrq：vbd 标点、vsx 数学、vjt 箭头、vxh 序号、vdw 单位／货币、vtx 图形、vxl 希腊字母、vkh 括号、vts 特殊；vfh（符号）按类别顺序列出全部，重复的只列一次，读音行写“类别 · 触发码”。不用 v＋单个数字作符号页：v 后的数字键已是数字模式的输入，符号页无法用数字键选，且会改变 v1 等第六批行为。
- 冲突核对（扫描脚本只输出计数）：打包词库 native-pinyin.tsv、japanese.tsv、native-japanese.tsv 中 v 开头读音 0；FF14 0.4.0 包（full）v 开头 1,255、u 开头 572，jobs v 开头 5，第二个字母只有 a／e／i／l／o／p／r／u；FF14-04 验证用的 0.4.0 启用快照同为 v 开头 1,255；本机实际启用快照（FF14 0.3.0）v 开头 1,219、u 开头 558；均不含数字；三者中 vb、vd、vf、vh、vj、vk、vs、vt、vx 开头都是 0，所以十个触发既不等于也不是任何读音的前缀。本机选词记忆两表、六套评测样本输入中 v 开头均为 0。
- 表（`QuickInput.h`，`symbolCategories`／`symbolTrigger`／`symbolCandidates`）：9 类共 248 个（标点 28、数学 41、箭头 18、序号 42、单位 20、图形 31、希腊字母 34、括号 20、特殊 14），vfh 去重后 240 个（《》〈〉（）与 Ω μ 两类都有）；每个候选一个符号，完整列表见[用户指南](USER_GUIDE.md#常用符号下一个试用包起cn-07尚未打包)。
- 合并：`quickCandidates` 在非数字模式下先查日期触发、再查符号触发，生成项 `generated`，经 `mergeQuickCandidates` 排在最前（选词记忆之后），普通候选按原顺序跟在后面。不学习（同第六批），原样上屏，繁体与全角不转换。翻页、方向键越页沿用现有机制。Explain.cmd 自动显示为“CN-07 生成（类别）”。
- 测试：`quick_input` 新增符号表检查（9 类、触发码互不相同且不是日期触发、类内无重复、非空、每类 10–45 个、总数 150–260、各类开头若干项、vfh 去重且覆盖全部、标签、近似输入不触发）；`mixed_candidates` 新增非触发输入（vb、vbdd、vfhx、bd、vx）列表逐项不变、vbd／vxh／vfh 生成项在前且原列表顺序不变、v＋数字时不出符号；`tsf_focus_cancel` 的 `quickInput()` 新增 vbd 数字键选、vxh Enter、= 与 Page Down／- 翻页后数字键选、方向键越页、vfh 读音行、空格原样、vbdd 不触发、v1 仍为数字模式、繁体会话、全角开启时仍原样、全部不学习。CTest 仍为 60 项，完整构建并复制 DLL 后全部通过（364.46 秒）。
- 评测：报告 docs/benchmarks/chinese-{tuned,holdout,words-dev,abbrev-dev}-cn07sym.json；调优集、独立集、整词开发样本与 ff1404 报告、简拼开发样本与 cn05 报告比较，去掉时间、内存、修订号和哈希后逐字段相同。独立集只比较是否相同和总数，未查看条目。
- 未做：实机、打包。已知局限：只有精确触发，没有符号面板或按名称搜索（如 duihao→✓）；成对括号逐个上屏；部分符号（☑ ♀ 等）在某些字体中显示为彩色表情。

## 2026-11-21 · alpha.28 试用包

- 版本号升至 0.4.4-alpha.28（d0766e9），内容为 alpha.27 加自动翻页（8174c77）、DICT-02a（0d64cd8）、FF14-03（3c47baf）与 FF14-04（8c6a184）。从干净提交重新生成词库、完整 MSVC Release 构建并复制 DLL 到 ime 目录，60 项 CTest 在一次运行中全部通过（345.74 秒）。
- 生成 Harukoto-0.4.4-alpha.28-windows-x64.zip（67.4 MiB，SHA-256 989728d4b76ae4b2ec728e0eb8ffdd9a5a97f8b5f59832e38e749f2e5dfa5cf0）和 Harukoto-0.4.4-alpha.28-Setup.exe（172.0 MiB，SHA-256 6d084ec4594597eec334a5b14f91c8f5b19a8d183cf8e812a688ce46d0c7855e）。BUILD-INFO 记录源提交 d0766e9、不含未提交修改；包内 DLL 与测试所用 Release 构建哈希一致，DLL 内版本为 alpha.28；包内 FF14 词库清单为 0.4.0，包根目录含 Explain.cmd 与 harukoto_candidate_explain.exe。
- 本机未安装，未发布，安装包未签名；实机抽查待维护者按 VAL-01 进行，重点是自动翻页、FF14 新增技能／副本／简称是否干扰日常输入，以及 Explain.cmd 在安装环境下能否读到实际数据。此条为打包后记录。

## 2026-11-21 · FF14-04：常用简称与新增类别

- 前置：FF14-03 已提交为 3c47baf。本批未提交、未打包。详见 [FF14 全量词库](FF14_FULL_DICTIONARY.md)“0.4.0 技能、副本与玩家简称”一节。
- 来源：同一固定提交（国服 3ba064a6、日英 a67c23b0＝7.56）新增 cn／ja／en 的 Action.csv、ContentFinderCondition.csv，SHA-256 写入 build-ff14-full.py ADDED_PINS 与 full/source-checks.json；coverage.json 按表记录 revision，类别为记录 ID 前缀。
- Action：国服列名错位，按同 ID 日服行筛选（非 PvP、ActionCategory 2／3／4、IsPlayerAction 或 ClassJobCategory≠0）。范围内 1,923，收录 1,905，排除 18（缺日文 8、占位 1、触发全冲突 9）；范围外 43,567 只计数。ContentFinderCondition 863 全收。
- 常用词冲突：audit-ff14-readings.py --write-overrides 生成 DataSources/FF14/common-reading-collisions.tsv（新表触发与基础词库权重≥3000 的词同读音，230 个），构建时丢弃，313 条记录写 dropped_triggers。重跑审计稳定。读音审计扩展到新表，新增 10 行 evidence 修订（勒尔 le、都灵 du、重置 chong 等），旧 57,672 条逐条不变。
- 简称：DataSources/FF14/player-abbreviations.tsv，51 条 curated、24 条 excluded（纯字母、单音节、与日常词同字／同读音、日服无对应、探查中降位的大区）。
- 0.4.0：60,491 条记录、117,507 组触发；重建逐字节一致；0.3.1→0.4.0 更新保持启用、回退回到 0.3.1。words 8.48 MB（+4.0%），评测加载约 0.80 s 不变，私有内存 +1.3 MB。
- 测试：test_japanese_ff14_full.py 新增三项（新表对齐与冲突过滤、简称表校验、0.3.1→0.4.0 更新回退），计数更新（含 test_japanese_manager_gui.py）。完整 CTest 60 项通过（345.59 秒）。
- 打字验证：Explain（--bundled、关模糊音／纠错／记忆，0.3.1 与 0.4.0 主题快照）：bushi、zhuyao、baohu 前 5 不变；lingshi 前 3 不变；jueou、juelongshi 首选绝欧、绝龙诗。三套中文评测 12 组名次变化 0。
- 未做：实机抽查与打包；简称日文说法的人工复核；旧三表的常用词冲突过滤；NPC／敌人技能与技能说明。

## 2026-11-20 · FF14-03：缺口与读音质量（部分完成）

- 前置：DICT-02a 已提交为 0d64cd8。本批未提交、未打包。详见 [FF14 全量词库](FF14_FULL_DICTIONARY.md)“0.3.1 读音质量与缺口”一节。
- 缺口：scripts/audit-ff14-readings.py 读固定来源缓存核对 362 条物品，日英同 ID 行整行为空、同名无已对齐 ID，全部判为国服独有（复制品 330、节日／活动／赛事 17、冒险录 11、兑换券／教材 4），写入 DataSources/FF14/item-gaps.tsv，不收录、不猜译名。
- 读音：新增 scripts/ff14_readings.py（逐字自动拼音，无修订时与 0.3.0 逐条相同）。审计用构建产物 .build/windows/ime/native-pinyin.tsv 中覆盖多音字的最长词读音作证据：100,032 个多音字位置中一致 66,381、按证据修订 790、需人工 2,997、低风险未核 29,864。拒收跨词边界误配和有争议读音（长的、卡的、要的、之长、劲舞、熟成、利爪、伽玛、大巴扎），理由写在脚本 REJECT。
- 修订表 DataSources/FF14/reading-overrides.tsv（786 行，id／text／reading／status／note；evidence＝词证据、程序判定、代理核对，reviewed＝人工复核，当前 0 行）。build-ff14-full.py 读修订表替换自动拼音，未匹配行报错；reviewed-source.json 每条加 pinyin_review，coverage.json 加计数（auto 55,425／evidence 796／reviewed 0／completion-search-key 1,247／curated-0.2.0 204）。
- 版本 0.3.0→0.3.1：来源、记录数（57,672）与触发组数（113,463）不变，只改拼音；同版本不同内容会被导入拒绝，故需升版本号。重建两次逐字节一致；审计重跑结果不变。
- 测试：test_japanese_ff14_full.py 新增修订与来源检查（每条修订生效、计数、跨边界拒收、缺口不收录）。完整 CTest 60 项在一次运行中全部通过（324.12 秒，含 ff14_full_production 真实 C++ 加载 0.3.1 全部触发）。
- 打字验证：harukoto_candidate_explain 用新包的主题快照（.build/ff14-03-local，--bundled、关模糊音／纠错／记忆）：changjingtuoge 首选长颈驼革、第 4 项ダルメルレザー；tuisejingcu 首选褪色晶簇；zhangjingtuoge、tuishaijingcu 不再出主题词。japanese_packages 实测 0.3.0→0.3.1 更新保持启用，回退回到 0.3.0。
- 人工复核：[读音人工复核清单](FF14_READING_REVIEW.md)（前 50 组，按记录数 × 地名 5 × 任务多音字 4／其他高风险 2 排序；如长弓、长杖、长外衣、长须，水晶都的“都”，卡扎纳尔的“扎”，角尊的“角”），全表 DataSources/FF14/reading-audit.tsv。
- 未做：人工复核（reviewed 行为 0）；中文评测在 FF14 开启下的前后对比；打包与游戏内抽查；Completion 和精选词条的拼音不在审计范围（前者用原生检索词）。

## 2026-11-20 · DICT-02a：候选来源与冲突诊断

- 前置：WIN-08 反馈已提交为 80d5110。本批未提交、未打包。
- 新增只读工具 `harukoto_candidate_explain.exe`（`Windows/CandidateExplain.cpp`，逻辑在 `CandidateExplain.h`）与 `Explain.cmd`。读输入法实际所用的数据：注册的 DLL 目录（或已安装的基础词库快照）、主题启用快照及同快照 state.json、japanese-custom.tsv、selection-memory 三张表、HKCU 设置；按 windowsCandidates＋选词记忆＋CN-07 同一代码重建候选，另用不挂个人数据的会话对照。输出来源、核心按词得分、个人数据影响、冲突；`--text` 反查、`--audit` 全表检查。说明见[候选诊断](CANDIDATE_EXPLAIN.md)。
- 只读保证：日志只经 `Journal::refresh`（不调用 poll／flush／compact），个人词表 `UserWordTable::refresh`，SQLite 只读打开，注册表只读，无网络。
- 核心新增 `mpn_candidate_explain`（最高分切分的逐词权重、先验、基础分、个人加分与实得、衔接，模糊音／纠错／简拼候选返回空）与 `mpn_explain_text`（词库、个人词、衔接短语表反查），只读、现有签名不变。
- 打包：MSVC 包根目录加入 exe 与 Explain.cmd，docs 加 CANDIDATE_EXPLAIN.md；`Install.ps1 -VerifyOnly` 在有 Explain.cmd 时检查 exe。不进 Program Files、不加快捷方式。
- 测试：新增 CTest `candidate_explain`（合成词库与合成主题／记忆／个人词／自定义表：来源、得分合计等于生成分、记忆提前与插入、个人词重复、主题排在基础整词前、多处定义、未启用主题、反查、检查、记忆开关关闭；核心候选在调用前后不变；进程内和 exe 运行后所有文件字节与修改时间不变）。完整 MSVC Release 构建、DLL 已复制到 ime，60 项 CTest 全部通过（321.94 秒）。
- 评测：tuned／holdout／words-dev 写入 `docs/benchmarks/chinese-*-dict02a.json`，与 dict02b 报告去掉耗时、哈希、时间、修订等字段后完全相同（只比较整体，未逐条查看 holdout）。
- 本机真实数据：gongsi、yitaizhiguang、aetheryte 运行约 1.2 秒；组句候选合计与生成分一致。
- 未验证：安装包内运行（未打包）、他人机器上的数据组合、繁体与模糊音下的输出。局限见说明文档。

## 2026-11-20 · WIN-08 实机反馈

- 维护者安装 alpha.27 后在 FF14（无边框窗口）试用，报告游戏内输入没有问题；问到如何翻页后，改为方向键／Tab 越过本页末项时自动翻页（8174c77，未打包）。维护者确认显示的是游戏自己的候选框，说明 FF14 接手了候选列表（UI-less 路径生效）；Diagnose 字段没有留证。

## 2026-11-20 · alpha.27 试用包

- 版本号升至 0.4.4-alpha.27（8098ef5），内容为 alpha.26 加 CN-03（3ffe2c6）、CN-04（8af8155）、CN-05（c59279b）、DICT-02b（65d6811）、CN-07 七批（9ef4623 至 1b5e91d）与 WIN-08（4fb1d87）。从干净提交重新生成词库、完整 MSVC Release 构建并复制 DLL 到 ime 目录，59 项 CTest 在一次运行中全部通过（326.75 秒）。
- 生成 Harukoto-0.4.4-alpha.27-windows-x64.zip（66.4 MiB，SHA-256 8d7022f68d4b0d1370962a4e84beadee7f244394f05c236dc032d7d89788b03f）和 Harukoto-0.4.4-alpha.27-Setup.exe（171.0 MiB，SHA-256 8d699d97d8e5d02647a0c3f13d40beff7b524ff633b5502dfd14bc26af114eef）。BUILD-INFO 记录源提交 8098ef5、不含未提交修改；包内 DLL 与测试所用 Release 构建哈希一致，DLL 内版本为 alpha.27。
- 新增 UIELEMENTENABLED 注册类别，须用安装器覆盖安装（重新注册）后生效，并重开各输入应用。本机未安装，未发布，安装包未签名；实机试用待维护者进行，重点是 FF14 游戏内候选与输入法掉线（按用户指南“游戏中输入”清单），以及 CN-03 至 CN-07 的手感。此条为打包后记录。

## 2026-11-20 · WIN-08：游戏内输入兼容（FF14）

- 前置：CN-07 第七批已提交为 1b5e91d。本批未提交、未打包；改动注册类别，需重新安装。核心未改动。
- 用户报告（FF14，无边框窗口，约 alpha.26）：游戏里没有单独的晴词候选、与游戏外不同；切到游戏后输入法掉了。没有游戏可测，以下原因由代码推断：
  - 服务没有任何 TSF UI 元素（无 ITfUIElement／BeginUIElement），也没有组字（只用 InsertTextAtSelection 上屏，拼音只画在自己的候选窗）。自绘输入法 UI 的游戏通过 UI 元素（IMM32 游戏经 TSF 兼容层的 ImmGetCandidateList）和组字串取候选与拼音，所以游戏里拿不到；而我们的候选窗位置依赖 GetTextExt／光标，游戏常给不出，可能不在可见位置。
  - CN-07 第二批起中英文与 GUID_COMPARTMENT_KEYBOARD_OPENCLOSE 双向同步：游戏在聊天框无焦点时关闭输入法（ImmSetOpenStatus 等），晴词随之切成英文；游戏若不再打开，就一直停在英文，表现为“掉了”。还会在游戏里弹“英”提示。
  - 其他可能（未能区分）：每个应用使用不同输入法、Activate 失败（游戏进程未加载 DLL）。Diagnose 现在可分辨。
- 候选 UI 元素（TextService.cpp `CandidateElement`）：实现 ITfCandidateListUIElementBehavior。有输入时 `presentCandidates` 调 BeginUIElement（调用前状态已就绪），之后按文字／高亮／页变化设 TF_CLUIE_* 并 UpdateUIElement，输入清空、焦点变化、上下文弹出、停用时 EndUIElement。GetString 只给候选文字（不含读音、[日]、纠错标记）；索引为整个列表、每页 7 个，与数字键一致；SetPageIndex 返回 E_NOTIMPL；SetSelection／Finalize／Abort 分别等于移高亮、Enter、Esc。pbShow 为 FALSE（或之后 Show(FALSE)）时隐藏自己的候选窗；没有 UI 元素管理器或 Begin 失败时照旧用自己的候选窗。中英／全角提示不受影响（只在空闲时出现）。
- 组字（`applyComposition`，在编辑会话里按会话运行时的状态写）：`InlineComposition`（HKCU\Software\Meowpuut\Input，`GameInput.h`，每键读）0 自动（默认，只在应用自绘候选时）、1 总是、2 从不。组字范围从选区 StartComposition，文字为候选窗里的拼音（`preeditText`），光标置于末尾；上屏前先清空并结束组字，再走原有投递队列，剩余拼音另起组字；Esc、焦点变化、切英文时结束。应用或 TSF 结束组字（OnCompositionTerminated）时丢弃当前拼音。没有显示属性提供者。默认 0 时普通应用行为与改动前完全一致。
- OPENCLOSE（`OpenCloseRule`）：只有用户切换（Shift、任务栏图标）改偏好并写隔间；与已知值相同的通知视为回声（含自己的写入）；外部关闭→暂时英文、偏好不变、不弹提示（`host_closed`）；外部打开→若之前是外部关闭则恢复偏好，否则（用户选了英文后 Ctrl+空格之类）为中文并成为偏好，与改动前一致；Shift 任何时候都可切回。没有做“焦点回来自动重开”：同一事件在游戏里也可能发生在未聚焦聊天框时，会吃掉 WASD。
- 注册：`registerProfile` 新增 GUID_TFCAT_TIPCAP_UIELEMENTENABLED（值经测试与 SDK 对照），卸载时移除。需要重新安装。注意：IMMERSIVESUPPORT 并未注册（之前只注册了 SYSTRAYSUPPORT）。
- 诊断：运行时记录新增 `input_mode`、`open_close_changes`、`host_closes`、`candidate_ui`（none／own／host／own_fallback）、`ui_elements`、`ui_elements_host_drawn`、`ui_element_failures`、`compositions`、`composition_failures`、`compositions_terminated`（只有计数和状态，无文字）；每键与开关变化时发布。DiagnosisSupport.ps1 可选解析（旧版显示“未报告”），diagnose-ime.ps1 显示这些字段，并新增“输入设置”：语言列表是否含晴词、默认输入法覆盖、InlineComposition 与 ShiftToggle。
- 测试：`tsf_focus_cancel` 新增 `gameInput()`，带假线程管理器（开关隔间、UI 元素管理器）和可组字的假上下文：应用自绘时候选窗不显示、元素的数量／文字／选中／页／更新标志、方向键只报 SELECTION、数字键先结束组字再上屏、SetSelection＋Finalize、Abort、应用结束组字、Show(TRUE) 交回；应用不接手时无组字；InlineComposition 1／2；无 UI 元素管理器；应用关闭／打开、组字中关闭上屏字母、Shift 重开并写隔间、显式打开、回声不计数；GUID 与 SDK 对照；`OpenCloseRule` 单独测试。`installation_diagnosis` 新增运行时记录新旧字段解析与非法值拒绝。CTest 仍为 59 项。
- 未验证：FF14 是否回答 pbShow=FALSE、是否经 TSF 兼容层显示我们的候选和组字串、何时调用开关；“掉了”的真实原因。清单见操作指南“游戏中输入”。
- 已知局限：没有 ITfReadingInformationUIElement；候选文字不带读音和标记；组字没有下划线（无显示属性）；游戏里只能靠 Shift 恢复中文；中英提示在游戏中仍会出现（用户自己切换时）。

## 2026-11-20 · CN-07 第七批：全角半角（CN-07 完成）

- 前置：第六批已提交为 389901c。本批未提交、未打包。核心未改动。CN-07 七批（标点、中英文切换与状态提示、-= 翻页、以词定字、候选中忘记、数字与日期、全角半角）全部完成，常用符号推迟另立。
- 开关：DWORD `FullWidthHotkey`（HKCU\Software\Meowpuut\Input，只有 1 为开启，默认关闭；`PunctuationMode.h` 的 `readFullWidthHotkey`，每次按键读取）。开启时 Shift+空格（`pinyinFullWidthShortcut`：只有 Shift）切换，按住重复只切一次，空闲、组字、中英文模式都可用，组字保持。关闭时 Shift+空格与改动前一样交给应用（游戏跳跃／冲刺），并且全角输出立即失效（`fullWidthOn()` = 状态 && 选项）。设置窗口新增复选框，LayoutOptions.cs 新增 `FullWidthOptions`（只在改变时写入）。
- 状态归属：按服务实例（同中英文模式），启动为半角，不保存。理由：与中英文模式一致，重开应用即恢复常态，游戏外切换不影响游戏。
- 映射（`pinyinFullWidth`）：U+0021–007E → U+FF01–FF5E，空格 → U+3000，其余不变。只用于输入法原样输出的部分：中文模式空闲时原本交给应用的可打印键（数字、空格、Shift+字母、未映射的 Shift 符号、/ - = ` 等，英文标点模式下的标点）改为接收并输出全角；组字中空格的原始字母与空格、字面标点（/、英文标点、未映射 Shift 符号）、不选词的数字、Shift+字母的原始字母与大写字母；组字中切到英文时上屏的字母；英文模式下全部可打印键（含 Shift 与空格）。不变：拼音组字、所有候选（含数字与日期生成项）、中文标点、选词数字键、Enter／方向键等。Caps Lock 打开时仍全部交给应用（未改）。
- 状态提示：切换后空闲时光标提示显示“中文 · 中文标点 · 全角”；任务栏图标提示写明全角／半角（图标仍为中／英）；组字时候选窗状态栏（含旧式弹出框）追加“ · 全角”。
- 测试：`pinyin_punctuation` 新增全角映射（全部 94 个可打印字符、空格、非 ASCII 与控制字符不变）与快捷键判定；`punctuation_preference` 新增 `FullWidthHotkey`（默认、1／0／2、类型不对）；`tsf_focus_cancel` 新增 `fullWidth()`：选项关闭时 Shift+空格与数字空格放行、开启后切换；中文空闲数字／空格／/ - = `／Shift+2／Shift+A 全角而 ，‘！不变；组字 ni 空格得“ｎｉ　”、/ 得“你／”、9 得“你９”、1 仍选词、Shift+W 得“ｎｉＷ”；英文标点下 . [ 全角；组字中切英文上屏“ｎｉ”；英文模式字母大小写、数字、空格、Shift+1 全角，Enter／方向键放行，Shift+空格在英文模式也切换；关闭选项后立即半角且 Shift+空格放行。LayoutOptionsTests 新增持久化，test-preview.ps1 全部通过（设置窗口同时编译）。CTest 仍为 59 项，完整构建并复制 DLL 后全部通过（328.13 秒）。
- 未验证：真实应用与游戏中的效果；没有打包。
- 已知局限：Caps Lock 打开时不转全角；按美式布局；全角状态不跨应用。

## 2026-11-20 · CN-07 第六批：数字、日期与时间

- 前置：第五批已提交为 ccef9b6。本批未提交、未打包。核心未改动。
- 前缀规则与冲突核对：用 v。打包词库 native-pinyin.tsv、japanese.tsv、native-japanese.tsv 与仓库 FF14 包（full、jobs、example）中以 v、u 开头的读音都是 0；本机已启用的 FF14 英文触发快照有 1,221 个 v 开头、562 个 u 开头，但没有一个含数字，也没有一个等于或以 vrq、vsj、vxq 开头（vr 开头 23 个，vs、vx 为 0）。评测样本只含 a–z，且三个触发词不在其中。因此：v＋数字是数字模式；vrq／vsj／vxq 只在整串完全相等时生成。不用 rq／sj：它们是 CN-04 的简拼（日期、时间等词），放前面会改变简拼名次，放后面又找不到。
- 数字模式（`TextService`）：核心不接受 0、6–9 和小数点，所以在核心输入恰为“v”时，数字和一个小数点存在 `numberInput`，显示为 v123，候选由 `quickCandidates` 生成（中文数字、大写、金额、逐位、原样、千分位，最后是原样 v123）。数字键在此模式下是输入而不是选词；方向键／Tab 移动，空格或 Enter 上屏高亮项，Backspace 删一位（删空回到普通“v”），Esc 取消；标点上屏后照常输出；字母、' [ ] 上屏后从空闲重新处理；超过 24 个字符或第二个小数点被接收并忽略。焦点变化等走 `resetCandidates` 一并清空。
- 格式（新增 `QuickInput.h`，纯函数）：中文数字按四位分组（万、亿、万亿），组内和组间的零只在后面有非零数字时写一个“零”，开头的一十读作十，大写保留壹拾；最多 16 位整数，更长只给逐位、原样、千分位。金额：元整／角整，角为零而有分时写零，不足一元时省略元，超过两位小数不给金额。日期：年月日、ISO、斜线、月日、汉字（二〇二六年十一月二十日）、带星期；时间：时:分、时:分:秒、凌晨／上午／中午／下午／晚上 X 点 Y 分、X 时 Y 分、日期时间；星期：星期五、周五、礼拜五（星期按公历计算，不依赖系统字段）。用本机时间（GetLocalTime），`Service::clock` 可在测试中替换。
- 合并（`MixedComposition.h`）：`WindowsCandidate` 新增 `generated`；`quickCandidates`／`mergeQuickCandidates` 在 `refreshCandidates` 中调用——数字模式下就是整个列表；vrq／vsj／vxq 把生成项放在最前（在选词记忆之后合并，保证在首位），普通候选按原顺序跟在后面，已有同文字的非核心项去重。`windowsCandidates` 本身和探针未改，所以评测路径不变。理由：触发词是显式的，用户打它就是要日期。生成项不学习（`chooseCandidate` 视同 F6 词形），Ctrl+Delete 对它显示“没有学到的记录”。
- 常用符号：推迟。需要再定一组触发码并逐个核对 FF14 英文触发，本批不做。
- 评测：调优集、独立集、整词开发样本与 dict02b 报告、简拼开发样本与 cn05 报告比较，去掉时间、内存、修订号和哈希后逐字段相同（名次、前 7 项、缺失、compose）；独立集只比较了是否相同和总数（四种模式均为 72 条：首选 54、前 7 项 64、缺失 5），没有查看条目。报告在临时目录，未归档。
- 测试：新增 CTest `quick_input`（0、10、12、101、110、1001、1010、10000、10010、100010、10000001、亿、万亿、16 位上限与 17 位、前导零；大写；金额 13 例与超出；逐位与〇；千分位；输入限制；候选顺序与去重、末尾小数点、超长数字；星期；日期／时间／星期全部格式与各时段）；`mixed_candidates` 新增 `verifyQuickCandidates`（非触发输入列表逐项不变、vrq 生成项在前且原列表顺序不变、去重、数字模式列表）；`tsf_focus_cancel` 新增 `quickInput()`（v123 显示与候选、空格上屏、小数与第二个点、方向键选大写、Backspace 两步、Esc、逗号、字母、[、Shift+1 结束、24 位上限、vrq／vsj（数字键选第 3 项）／vxq、vxq 空格仍为原样、不学习、英文模式放行）。CTest 共 59 项（原 58 项加 quick_input），完整构建并复制 DLL 后全部通过（325.70 秒）。
- 未验证：真实应用中的效果；没有打包。
- 已知局限：输入恰为“v”时数字键不再选词；只按美式键盘的数字键和句点（小键盘数字交给应用）；金额只到分；不支持负数和运算；没有常用符号。

## 2026-11-20 · CN-07 第五批：候选中忘记学到的词

- 前置：第四批已提交为 a20a599。本批未提交、未打包。核心未改动（用已有的 `mpn_candidate_consumed`／`mpn_candidate_words`／`mpn_candidate_personal`）。
- 快捷键：组字时 Ctrl+Delete 或 Shift+Delete（`pinyinForgetShortcut`：只按其中一个，无 Alt／Win；与 Rime 相同）。`accepts` 只为这个组合放行 Ctrl／Shift，且只在中文模式组字时接收；空闲、英文模式、Ctrl+Shift+Delete、单独 Delete 都照旧交给应用。按住重复只执行一次。没有做 Ctrl+Shift+数字。
- 作用（`TextService::forgetCandidate`）：(1) CN-06a／b：删除“候选消耗的那段实际输入 → 候选文字”（当前简繁），包括选词记忆插入的整句候选——模糊音、纠错、简拼候选按用户实际打的拼音忘记；(2) CN-06c：删除 `mpn_candidate_words` 列出的每个组成词的权重（按词、当前简繁），简拼和纠错候选没有组成词，只忘记 (1)。随后 flush、`attachPersonal` 重建加分表、重设同一上下文让核心重新解码、重建候选，组字保持；状态栏（`notice`，到下一次按键）显示“已忘记”或“没有学到的记录”。基础词库从不改动。
- DICT-02b 个人词：不删除，只忘记为它学到的记录，提示用 `manage-selection-memory.ps1 -RemoveWord`。理由：个人词表只由脚本整表原子写入并可 -UndoWords 回退，输入法写入会多一个写入者；按两次删除在游戏中误触不可恢复。
- 存储（`SelectionMemory.h`）：`Model::forget`／`forgetRow`（`-<TAB>s|t<TAB>读音<TAB>文字`），`WordModel::forget`（`-<TAB>s|t<TAB>读音<TAB>词<TAB>小时`，改 revision），`Store::forget`／`forgetWord` 只有真删掉时才追加；重放按顺序执行，之后再选从头记起；合并（compact）不写被忘记的条目和“-”行。其他进程靠原有 2 秒轮询读到。旧版本：输入法与脚本把“-”行当坏行跳过（选词要求数字次数，个人词频只认 +／=），不报错但旧进程不忘记；旧进程合并时会丢掉“-”行使记录恢复——已写入 SELECTION_MEMORY.md。管理脚本 Read-Memory／Read-Words 处理“-”行，查看与导出一致。
- 其他：`selection_memory::enabled` 改为读 `preferenceRoot`（生产仍为 HKCU，测试可隔离）。
- 撤销：没有；重新选择即重新记住。
- 测试：`selection_memory` 新增 `forgetting()`（两种模型的 forget、另一简繁不受影响、行格式、重放顺序与重新学习、坏行、两个进程共享与重新学习、无可忘记时不写、合并去掉被忘记条目和“-”行）；`selection_memory_tool`（ps1）新增“-”行的查看与导出；`tsf_focus_cancel` 新增 `forgetting()`：Enter 学会“号”后 Ctrl+Delete 忘记（选词与词权重、两个日志的“-”行、另一个 Store 读到、组字保留、首位恢复“好”）、基础候选无记录不写文件、提示到下一键清除、Shift+Delete、Ctrl+Shift+Delete 与单独 Delete 放行、空闲与英文放行、个人词“张珊”不删除且提示脚本、学到的记录被忘记而个人词仍在首位。CTest 仍为 58 项，完整构建并复制 DLL 后全部通过（314.51 秒）。
- 未验证：真实应用中的效果与提示显示；多个应用同时使用时的实际同步；没有打包。
- 已知局限：简拼／纠错候选的词权重无法从候选忘记；整句候选会忘记句中所有学到的词；只忘记当前简繁；旧版本进程合并会恢复被忘记的记录。

## 2026-11-20 · CN-07 第四批：以词定字 [ ]

- 前置：第三批已提交为 98c5163。本批未提交、未打包。核心未改动（只用已有的 `mpn_choose`、`mpn_segment_context`／`mpn_set_segment_context`）。
- 行为（`PinyinPunctuation.h` 的 `pinyinWordCharacterKey`、`pinyinWordCharacter`；`TextService::pickCharacter`）：中文模式组字时不带 Shift 的 [ 上屏高亮候选的第一个字，] 上屏最后一个字。空闲时仍为「」，英文模式交给应用，Shift+[ ] 仍为『』（组字时先上屏候选）。
- 消耗：按该候选本来消耗的按键（`mpn_choose` 后丢弃核心的上屏文字，只取其中第一个／最后一个码点），其余读音继续组字，下一段按“之前的上下文＋这个字”排序（不是整个词）。理由：与主流一致，最简单；核心没有把单字映射回其音节的接口，按字消耗需要改核心。取字按 UTF-8 码点，BMP 以外的字（UTF-16 代理对）保持完整；简繁取实际上屏的文字。
- 学习：不记入选词记忆（CN-06a）和个人词频（CN-06c），与 F6 词形一致——读音与上屏文字不再对应；也清空本次组字的分段记录。
- 非中文候选：高亮为日语、英语等非核心候选，或活用面板（F6）打开，或没有候选时，[ ] 被接收但不做任何事（不上屏、不漏到应用、不打出「」）。理由：这些候选不是逐字对应的中文词，按普通上屏又会让人意外。
- 测试：`pinyin_punctuation` 新增按键判定与取字（首末字、单字、空串、ASCII、4 字节 UTF-8 在两端）；`tsf_focus_cancel` 新增 `wordCharacters()`：空闲「」；zhongguo 高亮“中国”[→中、]→国；高亮靠后的“中”时 ]→中、剩 guo 继续组字并按 1 得国；zhongguoren 高亮“中国”]→国、剩 ren、下一段上下文为“國”而非“中國”；𪚥龘 [ 得代理对 U+2A6A5、] 得龘；开启选词记忆时没有任何记录（选词记忆开关读本机设置，默认开）；高亮日语候选时 [ ] 吃键不变；组字 Shift+[ 得“中国『”；英文模式放行。CTest 仍为 58 项，完整构建并复制 DLL 后全部通过（317.65 秒）。
- 未验证：真实应用中的效果；没有打包。
- 已知局限：消耗整个候选的按键，而不是只消耗所取字的音节（例如 zhongguo 选“国”后不会留下 zhong）；BMP 以外的组合字符序列（如带变体选择符）按单个码点处理。

## 2026-11-20 · CN-07 第三批：- = 翻页

- 前置：第二批已提交为 6d36c54。本批未提交、未打包。核心未改动。
- 行为（`PinyinPunctuation.h` 的 `pinyinPagingKey`）：组字时不带 Shift 的 - 上一页、= 下一页，与 Page Up／Page Down 共用同一分支（翻页后高亮回到第一项；= 到末页时先扩展候选，没有更多时停在末页；- 在第一页停住），两种标点模式相同。改动前组字时这两个键会漏到应用（先于拼音出现）。空闲时、英文模式下照旧交给应用；Shift+- 仍为 ——（中文标点）、Shift+= 组字时上屏候选后输出 +（第二批规则）。
- 取舍：不加设置开关（只在组字时生效，原来是漏键，没有可保留的旧行为）；不提供 , . 翻页（它们是会自动上屏的标点，主流也只作可选项）。
- 测试：`pinyin_punctuation` 新增 `pinyinPagingKey`；`tsf_focus_cancel` 新增 `paging()`（16 个 yi 候选：空闲放行；= 逐页前进、末页停住、高亮复位；- 回到首页并停住、读音不变；与 Page Up／Page Down 交替一致；翻页后按 1 上屏该页候选；Shift+- 得“一——”、Shift+= 得“一+”、空闲 Shift+= 放行；英文模式放行），偏好写在临时注册表键。组字中翻页会显示旧式弹出框（测试目录没有渲染器），Deactivate 时销毁。CTest 仍为 58 项，完整构建并复制 DLL 后全部通过（318.22 秒）。
- 未验证：真实应用中的效果；没有打包。

## 2026-11-20 · CN-07 第二批：中英文切换与状态提示

- 前置：第一批（标点）已提交为 9ef4623。本批未提交、未打包。核心未改动。
- Shift 单按（新增 `ShiftToggle.h`）：`ShiftTap` 纯状态机，Shift 按下时没有 Ctrl／Alt／Win、500 ms（`shiftTapLimit`）内松开、中间没有其他按键、两个 Shift 没有同时按、松开时没有修饰键或鼠标按住，且鼠标按键状态（含每次点击翻转的切换位）与按下时相同，才算一次。按下／松开在 OnTestKeyDown／OnKeyDown 与 OnTestKeyUp／OnKeyUp 都会喂入（TSF 只在吃键时才调用后者），重复通知与自动重复都忽略，所以只切换一次。Shift 从不吃键，应用照常看到修饰键。左右由扫描码（0x36 为右）区分。DWORD `ShiftToggle`：0 关闭、1 左右（默认）、2 仅左，其他值或类型为默认；每次松开 Shift 时读取。默认开启的理由：只有“单独、短按、无点击”才切换，FF14 中把 Shift 当修饰键（Shift+数字、Shift+点击）不会误切；仍提供仅左／关闭。
- 模式归属：按服务实例（每个应用的输入线程），激活时为中文，不保存。理由：在 FF14 里切到英文不影响其他应用，重开应用或重启都回到中文，行为可预期；与 TSF 线程级的键盘开关隔间一致。模式与 `GUID_COMPARTMENT_KEYBOARD_OPENCLOSE` 同步：激活时设为开，切换时写入，`ITfCompartmentEventSink` 跟随 Windows（如 Ctrl+空格）或应用对它的修改。
- 英文模式：所有按键交给应用（`accepts` 返回 false），只保留繁简快捷键；Ctrl+. 也交给应用（如 VS Code）。唯一例外：本上下文还有待 TSF 插入的文字时，可打印键（字母、数字、空格、符号，按美式布局与 Shift，`pinyinLiteral`）由输入法按原样输出，保持顺序。
- 组字中切到英文：上屏已选部分加原始字母（同空格，但不加空格），清空候选。
- 漏键修复：组字中 Shift+字母上屏原始字母加该大写字母（同空格的“原样”约定；i Shift+P hone 空格 → “iPhone ”）；其他 Shift 符号（中文标点下未映射的 @ # % & * + ~ | 等，英文标点下全部）上屏高亮候选后输出该符号（同英文标点组字时的规则，`pinyinMarkKind` 返回 literal）。空闲时 Shift+字母与未映射 Shift 键照旧交给应用。Caps Lock 打开时组字中按键仍交给应用（未改，见局限）。
- 状态提示：(1) 任务栏输入模式图标：`ModeButton`（ITfLangBarItemButton＋ITfSource，GUID_LBI_INPUTMODE，TF_LBI_STYLE_SHOWNINTRAY），运行时绘制“中／英”图标（按 SystemUsesLightTheme 选黑白），提示文字含标点模式，左键单击切换；DllRegisterServer 增加 GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT 类别（需重新安装注册）。(2) 切换后（含 Ctrl+.）在没有组字时于光标附近显示约 1 秒“中／英”与标点模式：沿用候选渲染器（无候选的负载，状态栏为说明），失败时用旧式弹出框；都不抢焦点；线程计时器隐藏，若宿主不分发线程消息则保留到下一次按键。测试中没有键盘管理器时不显示。
- 其他：旧式弹出框的 Shift 点击保护（第一批）保留。
- 测试：新增 CTest `shift_toggle`（单按、时长边界、设置、其他键、修饰键、鼠标点击、双 Shift、重复通知与自动重复、扫描码、注册表）；`pinyin_punctuation` 改为组字中未映射 Shift 符号为 literal，新增 `pinyinShiftedAscii`／`pinyinLiteral`；`tsf_focus_cancel` 新增 `inputMode()`：真实回调里左右单按切换、英文全部放行（含 Ctrl+.、Shift 键）、组字中切换上屏“ni”、英文下待上屏队列后的字母顺序、Shift+W 得“niW”、组字 Shift+2 得“你@”而空闲放行、Shift 作修饰键／Ctrl+Shift／Shift+点击不切换、三种设置，并核对自定义 GUID 与 SDK 的 GUID_LBI_INPUTMODE／GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT 相同、图标能生成。LayoutOptionsTests 新增 Shift 设置持久化；test-preview.ps1 全部通过。CTest 共 58 项（原 57 项加 shift_toggle），完整构建并复制 DLL 后全部通过（321.54 秒）。
- 未验证：任务栏图标是否实际出现（需安装注册，未安装）、深浅色效果、点击切换；光标提示在真实应用和 FF14 全屏下的效果与位置；Windows Ctrl+空格同步；TSF 是否对每个 Shift 松开调用 OnTestKeyUp（按文档推断，实机未验证）；没有打包。
- 已知局限：鼠标点击只能察觉本线程处理过的（翻转位）；按美式布局；Caps Lock 打开时组字中的按键仍交给应用；英文模式下仅待上屏队列期间由输入法输出字符；提示计时器依赖宿主分发线程消息。
- 下一步：维护者验收；随下一个试用包实机抽查（任务栏图标、FF14 中 Shift 单按与 Shift 修饰、组字中切换）。

## 2026-11-20 · CN-07 第一批：标点

- 前置：DICT-02b 已提交为 65d6811。本批未提交、未打包。核心未改动。
- 行为（`PinyinPunctuation.h` 的 `pinyinMarkKind`／`pinyinMarkText`）：中文标点下 , . ; [ ] \ 照旧为 ，。；「」、；`/` 不再是 ？：空闲时交给应用（按键原样到达，与切换前的英文输入一致，FF14 行首 /say 等指令不受影响），组字时上屏高亮候选后输出 `/`。理由：主流输入法 `/` 输出 / 或 、，、已由 \ 提供；组字时也输出 / 使同一个键在两种状态下结果一致，网址、路径、日期不必先切英文；中文问号改由 Shift+/。空闲时的 ' 输出 ‘’ 交替，组字时仍为音节分隔符。Shift 层（只在中文标点下）：Shift+1 ！、4 ￥、6 ……、9 （、0 ）、- ——、; ：、' “”交替、, 《、. 》、/ ？、[ 『、] 』；其余 Shift 键与 Shift+字母照旧交给应用（组字时 Shift+字母泄漏留给下一批）。组字时按任一标点与改动前相同：先 `finishCandidateText` 上屏高亮候选（其余读音自动补全），再输出标点。
- 英文标点：上述键全部按原样；空闲时交给应用，组字时上屏候选后输出 ASCII 字符；Shift 层交给应用；' 组字时仍是分隔符。
- 待上屏队列：空闲时本应交给应用的标点（`/`、英文标点）在本上下文 TSF 插入尚未完成时由输入法按原样输出，不越过前面的文字（与原有数字、空格的规则相同）。
- Ctrl+.（`pinyinPunctuationShortcut`：只有 Ctrl，无 Shift／Alt／Win）切换，按住重复只切一次，写 HKCU\Software\Meowpuut\Input 的 DWORD `EnglishPunctuation`（1 为英文，其他／缺失／类型不对为中文），所有上下文与之后的会话共用；与繁简快捷键一样不受 Caps Lock 影响。DWORD `PunctuationHotkey` 为 0 时 Ctrl+. 交给应用。两个值均在每次按键时读取（新增 `PunctuationMode.h`）。设置窗口新增“标点”：“使用英文标点”“Ctrl+. 切换中英文标点”（默认开）；保存时英文标点只在复选框改变时写入，避免覆盖设置窗口打开期间 Ctrl+. 的切换；LayoutOptions.cs 新增 `PunctuationOptions`。
- 状态提示：没有现成的独立状态指示机制。组字时候选窗状态栏（及旧式弹出框）在“简／繁”后显示“ · 英文标点”；空闲时切换没有反馈，留给第二批（中英文切换键与状态提示）。
- [ ] 仍为「」（第四批以词定字只在组字时占用 [ ]）。
- 旧式弹出框（无渲染器时）按住 Shift 点击候选不再调用 OnKeyDown，避免 Shift+数字被当成中文标点；改动前此时也不选词。
- 测试：`pinyin_punctuation` 扩充（两种模式×空闲／组字的全部映射、Shift 层、引号两类各自交替、未映射 Shift 键、Ctrl+. 判定）；新增 CTest `punctuation_preference`（注册表读写、默认值、只有 DWORD 1 为英文、类型不对保持默认）；`tsf_focus_cancel` 新增正式按键路径：带插入记录的假 ITfContext，用 SetKeyboardState 模拟 Shift／Ctrl／Caps Lock，逐键核对 OnTestKeyDown 与 OnKeyDown 是否吃键一致及上屏文字（空闲／组字、Shift 层、未映射键放行、Caps Lock 放行、Ctrl+. 切换与重复、持久化、关闭快捷键后放行、英文模式、待上屏队列后的标点顺序），偏好写在临时注册表键（`Service::preferenceRoot`）。LayoutOptionsTests 新增标点持久化（不在 CTest 中）；test-preview.ps1 全部通过（设置窗口同时编译）。CTest 共 57 项（原 56 项加 punctuation_preference），完整构建并复制 DLL 后全部通过（316.77 秒；加 Shift 点击保护后重新构建、复制 DLL 并重跑，仍全部通过，310.82 秒）。
- 未验证：真实应用（记事本、浏览器、FF14 聊天框）中的效果；非美式键盘布局；没有打包、安装；设置窗口新控件未实际打开查看；Android 未涉及。
- 已知局限：按美式布局的虚拟键码；引号开合按服务实例（每个应用线程）交替，不看光标前文字；空闲时切换无提示；Shift+` ～、` ·、Shift+\ 等未映射。
- 下一步：维护者验收；随下一个试用包实机抽查（FF14 /指令、Shift 标点、Ctrl+. 在 VS Code 等应用里的冲突）。

## 2026-11-20 · DICT-02b 个人中文词的手动添加与修订

- 前置：CN-05 已提交为 c59279b。本批未提交、未打包。
- 核心接口（只新增，现有签名不变）：`mpn_personal_add_word(personal, reading, simplified, traditional)`、`mpn_personal_remove_word`、`mpn_personal_clear_words`、`mpn_personal_word_count`、`mpn_candidate_personal`。个人词放在已有的 `MPNPersonal` 里（CN-06c 加分同一对象；`mpn_personal_clear` 仍只清加分）。读音为字母、紧跟音节的声调数字和不连续撇号；身份为去声调读音＋简体文字。内部：`Personal` 新增个人词表、版本号与按词库缓存（加互斥锁）；`UserWords` 为某词库准备好的行（每音节一个声调数字，未写声调记为 5 并在位掩码里标“任何声调”）、读音索引和简拼索引；`Lexicon` 新增序号 `serial` 与 `to_traditional`（从 `mpn_custom_chinese` 抽出，行为不变）；候选元数据与路径各多一个 user 标记；`abbrev_options` 可同时查个人词索引；decode 的组句跨度取词库与个人词最长读音的较大者。
- 排序：整串恰为个人词读音时个人词排整词列表最前（词库同文字词提前不重复；开模糊音时它在模糊整词之前）。组句与简拼里个人词按权重 3000（`user_weight`，约两字词前 10%）计，并按 CN-06c 的上限加分 `Personal::lifted(base, 3, top)`：词库没有该读音时为 3000 分＋0.6 奈特，同读音最好的词库词较弱时至多领先它 0.6 奈特，较强（≥约 5100 分）时低于它。词库也有的同文字词在组句里按自身分数加上限加分。理由：与 CN-06c“越过同读音最好的词只多 0.2 倍”的边界一致，不改变句子切分；单音节个人词因此不会在句中替换常用字。模糊音、句中纠错、逐段兜底、第二页阶梯和 `mpn_candidate_words` 不涉及个人词；注音词库不使用。
- Windows：`SelectionMemory.h` 新增 `UserWordModel`（逐行解析、同词更新、最多 10,000 个）与 `UserWordTable`（按大小、修改时间、卷序号和文件索引判断变化，变了整份重读；打不开或超过 2 MiB 保留上次结果；不存在即空）；`Store` 增加 `pollUserWords`（每 2 秒至多一次，与开关无关）。`TextService::OnKeyDown` 不论开关都检查个人词；`attachPersonal` 改为：没有个人词且记忆关闭时摘下个人表（与改动前相同），否则挂上，个人词版本变化时整表重加，记忆关闭时清空加分并在重新开启时重建。`MixedComposition.h` 中个人词整串候选视为“有依据”，查询数据库对别的词的支持不会把它挤下。
- 管理脚本：`manage-selection-memory.ps1` 新增 `-ListWords`、`-AddWord／-ReviseWord／-RemoveWord`（`-Text`、`-Traditional`、`-NewReading`、`-NewText`）、`-ExportWords`、`-ImportWords [-Replace]`、`-UndoWords`、`-ClearWords`，默认查看列出个人词，`-Clear` 的提示说明不动个人词。音节表 412 个（词库里有单字的 419 个音节去掉 m、eh、biang、fiao、lvan、yai、sei），连写取最少音节、首音节最长；写入时先存 `selection-memory-user.previous.tsv` 再临时文件原子替换，替换被输入法短暂占用时每 100 ms 重试至多 50 次。回退为交换两个版本。没有新增安装文件（脚本已在包内），package-ime.ps1 不变。设置窗口没有词条管理的位置，没有加界面。
- 存储取舍：与 CN-06 共用目录、命名、`Store`、轮询节奏、脚本和 `MPNPersonal`；不共用追加日志格式（整表替换会让日志偏移量读错；导出／回退就是复制文件；旧版本合并日志时会丢掉不认识的行），不共用开关（显式词条不是学习）。
- 测试：`mixed_candidates` 新增个人词一节（合成词库）与真实词库检查；`selection_memory` 新增个人词表解析与重读；`tsf_focus_cancel` 新增正式路径；`selection_memory_tool` 新增脚本全流程。CTest 仍为 56 项，全部通过（316.61 秒；补真实词库断言、重新完整构建并复制 DLL 后再跑一次，仍全部通过，331.72 秒）。
- 评测：[调优集](benchmarks/chinese-tuned-dict02b.json)、[独立集](benchmarks/chinese-holdout-dict02b.json)、[整词](benchmarks/chinese-words-dev-dict02b.json) 与 cn05 报告相比，除修订号、源码哈希、耗时和采样内存外逐字段相同（名次、前 7 项、缺失、compose）。独立集只做了整体比较，没有逐条查看。
- 耗时：见[选词记忆](SELECTION_MEMORY.md#测量dict-02b)。298 条输入逐键中位／p95 0.661／2.239 ms，1,000 个个人词 0.664／2.244 ms，10,000 个 0.669／2.303 ms；表变化后一次性准备 1,000 个约 2.6 ms、10,000 个约 19 ms，读 10,000 个词的文件约 7.8 ms，均在输入线程上。
- 未验证：真实应用里的效果与生效时机；没有打包、安装；Android 未构建（只新增核心函数）；Core/tests 的 Python 检查未运行；实验独立引擎未接入。
- 已知局限：只打两个首字母（zs）时个人词按权重 3000 竞争，通常不在第一页；模糊音、句中纠错不作用于个人词；个人词不按词学习（CN-06c），选中后只由选词记录按整串记住；没写声调的音节匹配任何声调，写了声调的只匹配同声调；只保留一个上一版本；两个脚本同时修改不加锁；连写 xian 只按一个音节理解。
- 下一步：维护者验收；随下一个试用包实机抽查（添加人名、句中组出、修订后约 2 秒生效、回退、繁体）。

## 2026-11-19 · CN-05 纠错增强

- 前置：CN-04 已提交为 8af8155。本批未提交、未打包。
- 核心接口：新增 `mpn_set_typo_correction(session, enabled)`／`mpn_typo_correction`、`mpn_candidate_typo(session, index)`，按会话，默认开启；改开关时有组字即重建候选。现有签名不变，注音词库不受影响。decode 新增一个默认参数（主候选、候选扩展两处传入）；`fallback` 新增 `correct` 参数；内部候选元数据与 Fallback 各多一个 typo 标记；原 `fallback` 里的键盘相邻表抽成 `keyboard_neighbors`。
- 句中纠错（新）：只在输入切不成完整音节（与 CN-04 同一判定）、没有声调或隔音符、3–40 个字母、且不是“完整音节＋一个音节的开头”（budin、jiaobe、shiyon）时启用。从每个起点沿精确前缀走，每个位置各试一次：相邻颠倒、删一个字母（其后至少还有两个按键）、补一个字母（不在段首，其后至少还有两个按键）、同类相邻键（元音换元音、辅音换辅音，不是最后一个按键）；改动后逐字匹配，只在词库有以此开头的读音时继续；记下的读音须含完整拼写词，改动段须含元音、至少两个按键。每起点最多 4,096 次查找、256 个读音。
- 组句：纠错词另占一份与精确词等宽的搜索宽度；只取权重≥4500 的完整拼写词，整串以外须至少两字；分数＝词库分数－3 奈特（`typo_penalty`）；同一段有完整拼写的精确词或（开启时）模糊词就不出；每个候选最多一个纠错词，不与简拼词同在一个候选；纠错路径不算“完整拼写”，来源为 CORRECTION，`mpn_candidate_typo` 为 1，`mpn_candidate_words` 为空。解码器第一项是纠错时，与简拼一样排在整串键盘纠错兜底前，并在 refresh 里移到逐段前缀候选之前。
- 与简拼、模糊音的先后：三者都按分数竞争，规则是“精确＞模糊（用户开启）＞纠错与简拼按分数”。纠错与简拼在同一类输入（切不成完整音节）上启用：zhogn、zhonguo、nihoa、xiexei、womne 的纠错是一个常用词扣 3 奈特，简拼读法是多个词（中＋喔＋功能）且每个简拼音节扣 2 奈特，所以纠错胜出；只有声母的串（zhd、bkn）、末尾单个声母（zhongg、nihaom）、半截音节（budin）不纠错，简拼和前缀结果与改动前相同。开启模糊音后 lenshi 仍是 认识（模糊）而不是 了事（纠错）。
- 整串纠错（能切成完整音节时）：整串没有完整拼写的词时，与整串差一处颠倒、漏字母、多字母（不动首尾）的、权重≥4500 的两字以上词最多 2 个，放在第 4 位起（zhonggou→中国 第 4，doushao→多少 第 5）。原有的整串局部纠错（相邻键、颠倒、重复字母、n／ng，`fallback`）和整词列表中的 n／ng 纠错照旧，并标为 typo。
- 开关：默认开启，理由是纠错只在拼音读不通时进句中，读得通时只在第 4 位后补一两个常用词，对精确输入的前三项没有影响，而关闭时常见手误几乎打不出来（样本首选 13）。“关闭”解释为不出任何基于手误猜测的候选，原有的整串纠错和 n／ng 纠错也一起关闭，因此关闭时与改动前不同（少了原有纠错；前缀补全不变）。Windows：新增 Windows/TypoCorrection.h，读写 HKCU\Software\Meowpuut\Input 的 DWORD `TypoCorrection`（缺失或类型不对即开启，0 为关闭）；文本服务在 Activate 和每次按键前读取并交给当前会话，变化且有组字时重建候选。设置窗口模糊音下方新增“纠错 · 纠正常见手误”复选框，保存时只在值改变时写入（保存默认值不会新建）；LayoutOptions.cs 新增 `TypoCorrectionOptions`。实验独立引擎未接入。
- 测试：`mixed_candidates` 新增纠错一节（合成词库：zhogn／zhonguo／zhonggguo／nihoa／nihhao／womne／wozaizhognguo 首选为纠错且消耗全部按键、不按词学习、可选择上屏；两处手误不同时纠正；zhongg 仍是简拼、zgren／nh 不变；带声调不纠错；zhongguo 开关前后逐项相同；zhonggou 前三项与关闭时相同、中国 在第 4；关闭即时重建且没有纠错候选；繁体；长串有界）；CN-03 模糊音一节的“每对单开能出、其余不出”在纠错关闭下进行（zongguo 与 zhongguo 只差一个 h）。新增 CTest `typo_correction_preference`（注册表读写、默认开启、错误类型、只在变化时重建）；`tsf_focus_cancel` 新增正式路径（默认纠正并上屏、组字中关闭即重建、新会话按键时补上开关、再开还原）；test-preview.ps1 的 LayoutOptionsTests 新增纠错持久化（不在 CTest 中，单独运行通过，设置窗口同时编译、实时滑块等检查通过）。CTest 共 56 项。
- 评测（[中文评测](CHINESE_EVALUATION.md#纠错增强cn-052026-11-19)）：[调优集](benchmarks/chinese-tuned-cn05.json)、[独立集](benchmarks/chinese-holdout-v1-cn05.json)、[整词](benchmarks/chinese-words-dev-cn05.json)、[简拼](benchmarks/chinese-abbrev-dev-cn05.json) 与 cn04 报告逐条相同（名次、前 7 项列表、缺失、compose），[模糊音样本](benchmarks/chinese-fuzzy-dev-cn05.json)（全开）名次与 cn03 相同。探针逐项比较五套语料 3,204 个前缀：可切成完整音节的前三项全部不变，第 4 位起有整串纠错插入；切不成的前缀首选只有 6 个变化（独立集 3 个只统计未查看）。[纠错开发样本](benchmarks/chinese-typo-dev-cn05.json)（83 条）首选 19→60、前 7 项 21→70、缺失 25→8（[改动前](benchmarks/chinese-typo-dev-before.json)；[关闭时](benchmarks/chinese-typo-dev-cn05-off.json) 13／15）。
- 耗时：逐键 P50／P95（FF14 关）调优集 0.95／2.37→1.03／2.77、独立集 1.28／3.86→1.48／4.64、整词 0.53／1.81→0.57／1.90 ms。长串压力（16 种串各 5 轮，跑两次）：43–47 个字母、含多处手误的整句单键最大 3.6–8.7→4.6–9.6 ms，40 个随机字母 5.1–6.1→6.1–9.7 ms，纯声母长串（30／66 字母 zhshch…、64 个 y）与改动前在噪声范围内（不出纠错，11–16 ms 对 11–14 ms）。加载与内存不变。
- 构建与测试：完整 MSVC Release 构建通过（MeowpuutTip.dll 已复制到 .build/windows/ime，词库未变未重新生成），56 项 CTest（原 55 项加 typo_correction_preference）在一次运行中全部通过（311.73 秒；加候选窗标记后重新构建、复制 DLL 并重跑，仍全部通过，312.59 秒）。第一次完整运行时 `session` 失败（wosuki 被纠成“我肃立”排第一，挤掉日语混合项），据此改为只纠正成常用词、整串以外至少两字，之后重跑。
- 候选窗标记（维护者要求补做）：`mpn_candidate_typo` 为真的核心候选，文字下方的小字行（原本显示部分选择的读音、日语假名、主题词说明的那一行，字号与透明度沿用）显示“纠错”；已有读音时为“读音 · 纠错”。复用渲染器现有的读音行，横排、竖排、深浅色与高对比度都按原规则，不改渲染器、布局与按键选择；没有获得焦点时的旧式 STATIC 弹出框在行尾加“[纠错]”。标记只在 TextService 组 payload 时加入（MixedComposition.h 的 `typoCandidate`、`typoMarker`），候选文字与上屏文字不含它。原有整串纠错（相邻键、颠倒、重复字母、n／ng）同属开关、同样标出；模糊音与简拼不标：模糊音是用户在设置里声明的等价读音，简拼是有意的打法，都不是猜测手误。测试：`tsf_focus_cancel` 断言 zhonguo 的“中国”读音行为“纠错”、上屏为“中国”，zhongguo 的所有候选与关闭时都没有标记；`mixed_candidates` 断言 zhonguo 有标记、zg（简拼）与 zhongguo 没有。用临时程序以 CandidateView 渲染横排、竖排截图看过（标记在文字下方小字，与部分选择的读音同样式）。归档评测报告记录的 MixedComposition.h 哈希早于这次只影响显示的改动，候选文字不受影响，未重跑评测。
- 未验证：真实应用里的手感、标记的实际观感（只看了离屏渲染）与设置页保存后实际生效；未提交、未打包、未安装；Android 未构建（核心新增函数，未改现有接口；Android 不调用开关，默认开启）；Core/tests 的 Python 检查未运行。
- 已知局限：拼音碰巧读得通的手误不在句中纠正（jitian→几天、nihai→你还），整串只在没有完整拼写整词时补；最后两个按键里的漏打、多打、相邻键不纠正（shoujhi、zhongwn、gongzup）；只纠正成常用词，冷僻词打错不纠；罚分与门槛为固定常数。
- 下一步：维护者验收；随下一个试用包实机抽查（nihoa、zhonguo、wozaizhognguo、zhongg 简拼不变、设置页开关）。

## 2026-11-19 · CN-04 简拼与混拼

- 核心接口：新增 `mpn_candidate_abbreviated(session, index)`（简拼音节数），始终开启、没有设置项；现有签名不变，注音词库不受影响。内部候选元数据多两个字段（简拼音节数、是否只有末尾一个声母）。
- 启用条件：拼音词库、主候选或候选扩展（decode 传入元数据且非 completeOnly）、输入至少 2 个字母、不能切成完整音节（`singleReadings`，不跨声调或隔音符）、整串也不是某个完整拼写词的读音。其余输入（包括所有完整拼音）不进入新代码，原来的纯声母缩写行照旧参与。等一会儿 这类儿化（huir 不是单字读音）会启用，但整词照旧在最前。
- 索引：载入时把 2–8 音节、每个音节都有韵母的词按“各音节首字母”压成 64 位键（每字母 5 位，高位在前，同前缀连续），按键、再按两种字形权重中较大者排序；约 38 万项、4.4 MiB，加载约 355→410 ms。
- 展开：从每个起点把输入切成“完整音节”（单字读音，2–6 个字母）或“声母”（一个字母 bpmfdtnlgkhjqxrzcsyw 及 a／o／e，或 zh／ch／sh），只在索引里存在该前缀时继续；每起点最多 256 步、32 种切法（至少两段、至少一个声母、键在索引中存在）。匹配要求音节数相同，完整音节逐字相同，声母是音节开头（z 匹配 z／zh），输入声调须一致；全部拼全的匹配（如 a 正好是音节 a）不算简拼。
- 组句：每段的简拼词另占一份与精确词等宽的搜索宽度，每种切法最多看 1,024 项、取两倍宽度的匹配。分数＝词库分数－2 奈特×简拼音节数（`abbrev_penalty`）；整串以外只有两个声母的段只取权重≥4000 的词（`abbrev_short_min`）；整串以外单独一个声母可以是权重≥5000 的单字，扣 4 奈特（`abbrev_char_penalty`、`abbrev_char_min`），否则 woxhn 的“你”、nzmle 的“你”、wozdl 的“了”出不来。同段有完整拼写的精确词时不出简拼词。启用时跳过编译器生成的 1/8 权重缩写行（多音节、非完整拼写），整串的简拼词在组句里竞争。简拼路径不算“完整拼写”，来源为 CORRECTION。
- 排序位置：解码器第一项是简拼时，简拼路径排在键盘纠错兜底之前；在 refresh 里把它移到逐段前缀候选之前。只有末尾一个声母是简拼时（nihaom、wozhidaol），若第一项是未消耗全部按键的前缀，就把补全放到第 2 项：这是维持“简拼不压过完整拼音精确结果”的解释，也与调优集 partial 类（wozhidaol→我知道 首选）一致。
- 其他功能：CN-06a 选词记录按原始字母串工作，简拼照常可记；CN-06c 个人词频对简拼词按其完整读音查加分（合成测试：学过 男孩 后 nh 首选 男孩）；`mpn_candidate_words` 对简拼返回空，不按词学习。CN-03 模糊音只替换完整音节，简拼词也不做模糊替换，同一个词里不会同时用模糊音和简拼。
- 罚分取值：只用简拼开发样本判断。每音节 1／2／3 奈特、单字 2.5／4 奈特首选与前 7 项都相同；单字 6 奈特时首选 62→60。取 2 与 4。
- 测试：`mixed_candidates` 新增简拼一节（合成词库：nh→你好 且为 CORRECTION、消耗全部按键、不按词学习、可选择上屏；zg／zhg／zhgr；zgren→中国人 在 主管人 前；wobzd→我不知道；nihaozg→你好中国；nihaom 第一项 你好（消耗 5）、第二项 你好吗；z1g2、z'g 出 中国，z3g2 不出；完整拼音 nihao、xian、xi'an、nihaoma、zhongguo、zhongguoren、zhidao、wo 没有任何简拼候选、首选不变；繁体；个人词频；模糊音全开；256 字母长串有界）。CTest 仍为 55 项。
- 评测（[中文评测](CHINESE_EVALUATION.md#简拼与混拼cn-042026-11-19)）：改动前后探针逐项比较四套语料的输入前缀，可切成完整音节的 1,932 个简繁、模糊音全开下全部一致。[调优集](benchmarks/chinese-tuned-cn04.json)、[独立集](benchmarks/chinese-holdout-v1-cn04.json)、[整词开发样本](benchmarks/chinese-words-dev-cn04.json) 名次全部不变、compose 相同；调优集 partial 类 3 条的第 2 项变成补全（我知道了、浏览器、文件夹）。独立集只做程序比较，未查看样本。[简拼开发样本](benchmarks/chinese-abbrev-dev-cn04.json)：首选 42→62（FF14 开 41→61），前 7 项 61→76，缺失 16→2，compose 可达 66→80。
- 耗时：逐键 P50／P95（FF14 关）调优集 0.74／1.84→0.95／2.37、独立集 0.99／3.23→1.28／3.86、整词 0.44／1.42→0.53／1.81 ms；采样最大私有内存多约 4.7 MiB。长串压力每串 5 轮：20–40 个随机声母单键最大 5.3–6.7 ms，30 字母 zhshch… 10.8 ms（改动前 4.5），66 字母 zhshch… 13.2 ms（4.6），64 个 y 10.2 ms（6.3）。
- 构建与测试：完整 MSVC Release 构建通过（MeowpuutTip.dll 已复制到 .build/windows/ime，词库未变未重新生成），55 项 CTest 在一次运行中全部通过（304.02 秒）。
- 未验证：真实应用里的手感；未提交、未打包、未安装；Android 未构建（核心新增函数，未改现有接口）；Core/tests 的 Python 检查未运行。
- 已知局限：同一组首字母按词库权重排，zd／zhd 的 知道 在第 5、yx 的 游戏 在第 10；单个声母补的单字只看字频，不偏好语气词（fanganm 第 2 项是“方案目”，“方案吗”更后）；词库没有的整词（明天见）只能靠单字拼出常用字（明天就）；长纯声母串单键最大约 13 ms，比改动前多一倍多；加载多约 50 ms。
- 下一步：维护者验收；随下一个试用包实机抽查（nh、zgren、nihaom、wobzd、长串耗时）。

## 2026-11-19 · CN-03 模糊音

- 核心接口：新增 `mpn_set_fuzzy_pinyin(session, mask)`／`mpn_fuzzy_pinyin`、`mpn_candidate_fuzzy(session, index)` 与 `MPN_FUZZY_*`（11 位，z／zh、c／ch、s／sh、n／l、f／h、r／l、an／ang、en／eng、in／ing、ian／iang、uan／uang），按会话，默认 0；未知位丢弃；输入不为空时改掩码立即重建候选。现有签名不变，注音词库忽略。decode 新增一个默认参数，只有主候选、`hasCompleteReading` 和候选扩展三处传入掩码。内部候选元数据多一个 fuzzy 标记。
- 展开方式：把输入切成完整音节（有韵母，且替换后的拼写是词库里有单字的读音；输入本身可以不是合法音节，如 cuang），每个音节按已开启的对换声母、韵母或两者；y＋an／ang 归 ian／iang，w＋an／ang 归 uan／uang，an／ang 不含 ian、uan。再从每个起点向后走，只在“词库里有以此开头的读音”时继续，得到每段输入可代表的读音和替换次数（每词最多 3 处；每起点最多 1024 步、64 个读音）。找到的词须是完整拼写的词，音节与替换后的音节一一对应，输入声调须相符。
- 排序（维护者 2026-11-19 要求加强后的第二版）：“精确读音优先”解释为精确读音赢平局和近似平局，开了某对的人听不出区别，模糊词主要按频率竞争。
  - 同一段按键上有完整拼写的精确词时，每处替换扣 5 奈特（`fuzzy_penalty_near`，频率约差 150 倍）。
  - 没有精确词时，精确的组合可能只是词库缺词（点赞＝点＋赞），所以只有常用模糊词（权重≥4500，`fuzzy_common`，约前 1.5% 词条）扣 5 奈特，其余扣 8 奈特（`fuzzy_penalty`）。
  - 整段输入：有精确整词时，模糊整词按“权重－每处 3500”并入整词列表（高于最弱的精确整词才并入；与 n／ng 纠错重复的按模糊音位置重排），否则按 8 奈特留在整句里；没有精确整词时，得分压过全部精确整句的才进入整词列表。
  - Windows 合并把有查询数据库证据的词排前，但核心已排在这类词之前的模糊候选（`mpn_candidate_fuzzy`）保持原位；没有模糊候选时与以前相同。
  - 用到替换的候选来源标为 CORRECTION，`mpn_candidate_words` 返回空，因而不按词学习（CN-06a 的整串记忆照常）。个人词频按替换后的读音查。
- 罚分取值（只用调优集、整词开发样本和新建的模糊音开发样本判断）：第一版一律 8 奈特时 sijian 下的 时间 在第 15、wosizhongguoren 的“我是中国人”在第 18；近罚分 4 奈特会让调优集“今天先到这里”变“想到”、整词样本“点赞”被“点展”压过；5 奈特两者都不变。没有精确词时也一律用近罚分会让 电站（3952）压过 点赞，所以加了常用词门槛 4500（生日 4855、风景 5600 不受影响）。
- 简拼：只有声母的音节（zg、zhd 的每一位）不替换；整段都是简拼时结果与关闭时相同。逐段兜底候选（句首词、首音节单字、分段整句、未完成音节前的整句、日语前缀判断）仍只用精确读音。
- Windows：新增 Windows/FuzzyPinyin.h，读写 HKCU\Software\Meowpuut\Input 的 DWORD `FuzzyPinyin`（缺失或类型不对即全关）；文本服务在 Activate 和每次按键处理前（切换上下文、词库热更新换会话之后）读取并交给当前会话，变化且有组字时重建候选。设置窗口左侧“模糊音”11 个复选框（两列），保存时只在值改变时写入；LayoutOptions.cs 里的 `FuzzyPinyinOptions` 供设置页和测试用。实验独立引擎未接入。
- 测试：`mixed_candidates` 新增模糊音一节（合成词库：关闭及开后再关与无此功能逐项相同含分数；11 对各自单开能出、其余全开不出；sijian→时间、cifan→吃饭 首选，lihao→利好、si→四 仍首选，dianzan 的 点赞 在 电站 前；整句；声调；简拼；繁体；选择；开关即时重建；Windows 合并中 时间 保持在有证据的 四件 前，关闭时合并结果不变）；新增 CTest `fuzzy_pinyin_preference`（注册表读写、未知位、错误类型、只在变化时重建）；`tsf_focus_cancel` 新增正式路径（打开时重建、选择上屏、新会话按键时补上掩码、关闭后逐项还原）；test-preview.ps1 的 LayoutOptionsTests 新增模糊音持久化（不在 CTest 中，第一版时单独运行通过，第二版未改 C#）。CTest 共 55 项。
- 评测（[中文评测](CHINESE_EVALUATION.md#模糊音cn-032026-11-19)）：关闭时调优集与独立集报告（[调优集](benchmarks/chinese-tuned-cn03.json)、[独立集](benchmarks/chinese-holdout-v1-cn03.json)）逐条、分类、compose 与 cn06c 完全相同；改动前后探针对四套语料 2,655 个输入前缀的全部候选逐项比较，简繁一致。全部开启（[调优集](benchmarks/chinese-tuned-cn03-fuzzy.json)、[独立集](benchmarks/chinese-holdout-v1-cn03-fuzzy.json)、[整词样本](benchmarks/chinese-words-dev-cn03-fuzzy.json)）：调优集名次全部不变；整词样本首选、前 7 项不变（siliao 的 私聊 第 3→4）；独立集四种模式首选、前 7 项、缺失不变，每种模式各 1 条名次变差，均在“游戏”类，该类“非预期纠错首选”0→1（原本就不是首选；未查看是哪一条，未据此调参）。模糊音开发样本（[报告](benchmarks/chinese-fuzzy-dev-cn03.json)，60 条，全开）：关闭 10／19，第一版 23／49，现行 42／60（FF14 开前 7 项 59）。
- 耗时：同机各 3 轮，全开比关闭的逐键 P50／P95，FF14 关调优集 0.74／1.84→0.87／2.33、独立集 0.99／3.23→1.18／3.90；FF14 开多约 0–0.15／0.1–0.6 ms。长串压力每串 5 轮：40 字母左右几乎每音节可替换的输入单键最大约 5.6–7.1→9.3–10.2 ms，52 字母长句 8.1→9.2 ms。加载时间不变，采样最大私有内存多约 0.5 MiB。
- 构建与测试：完整 MSVC Release 构建通过（MeowpuutTip.dll 已复制到 .build/windows/ime，词库未变未重新生成），55 项 CTest 在一次运行中全部通过（288.08 秒）。
- 未验证：真实应用里的手感与设置页保存后实际生效（只在测试里走过 TSF 回调，设置窗口做过 UI 自动化检查和截图）；未提交、未打包、未安装；Android 未构建（核心新增函数与枚举，未改现有接口）；Core/tests 的 Python 检查未运行。
- 已知局限：罚分与门槛是固定常数，不能按对调整；频率相近时精确优先，所以 fanjian（反剪 3548 对 房间 5474）、banzhu、guanchang 等模糊词在第 2–5 项；单音节输入的模糊字排在该读音常用字之后（zi 的“之”第 10 项）；以高频单字开头的拼凑整句有时压过模糊词（leqing 的“热情”第 6）；模糊候选不按词学习；分段候选不含模糊音。
- 下一步：维护者验收；随下一个试用包实机抽查（设置页勾选、sijian、wosizhongguoren、lihao、长句耗时）。

## 2026-11-19 · CN-06c 个人词频参与组句

- 数据来源：只用确认过的核心候选（分段的每一段、单独输入的整词、整句候选）。核心新增只读接口 `mpn_candidate_words`，在确认时把候选拆成最少个完整拼写的词库词，返回无声调读音和文字；拆不开的（纠错、简拼、字面尾巴）不按词记。日语、英语直通、F6 词形、查询数据库补充和选词记录插入的整句不计。没有做相邻两段的个人搭配。
- 核心接口：新增 `MPNPersonal`（`mpn_personal_create／destroy／set／clear`、`mpn_set_personal`），C 接口风格，现有签名不变。会话共享接入层的表；decode 的词得分和整词匹配列表在有表、且该读音有记录时才改变。有效分数 min(原分数＋加分, 该读音最好的词＋0.2×加分)：同读音内按完整加分上升，越过最好的词后最多再 0.6 奈特，所以不改别的读音，也拆不开更长的词（合成词库中“是”“见”封顶仍是“时间很长”）。学过的词在组句搜索宽度之外也会在有限扫描（4096 项）内加入。
- 刻度：加分 ln(1+权重) 奈特，核心截到 3（相当于词库分数 2100）；权重每次确认 +1、封顶 20，同读音其他词每次减半，每 14 天减半，低于 0.05 遗忘。选一次 0.69 奈特；一次误选被一次纠正抵消；封顶习惯 3 次改回；只用一次的词约 61 天消失。
- 存储：%LOCALAPPDATA%\Meowpuut\selection-memory-words.tsv，与 selection-memory.tsv 同目录、同开关、同一次轮询和合并机制（`Journal<M>` 模板，原 Store 逻辑不变）。不放进原文件的原因：旧版本逐行跳过不认识的行、不会崩溃，但启动时合并超过 1 MiB 的文件会丢掉新行，而升级后已打开的应用可能仍是旧 DLL。管理脚本查看、导出（新增 Kind 列）、清除同时覆盖两个文件；旧版本的脚本不清除新文件。
- 接入层：每次按键读开关后挂上或摘下个人表；关闭时核心结果与改动前逐项相同。别的应用写入、清除、合并或整点衰减时整表重建，自己确认的词只更新所在读音。实验独立引擎未接入，个人数据目前由接入层持有，归属留到阶段二第 6 步之前决定。
- 测试：`mixed_candidates`（核心，合成词库：空表／无关表逐项相同、另一句话与单独输入靠前、封顶、拆不开长词、繁体分开、分词接口）、`selection_memory`（权重、衰减、纠正、上限、行格式与新旧互不读取、两进程共享、清除、合并、读写耗时）、`tsf_focus_cancel`（正式选词路径：分段选 我／在／公司 后 gongsihenda 与 gongsi 首选变化且由核心给出、关闭后三种输入逐项还原、误选与纠正、清除）、`selection_memory_tool`（个人词的查看／导出／清除）。都扩展在现有测试里，CTest 仍为 54 项。
- 完整 MSVC Release 构建通过，54 项 CTest 在一次运行中全部通过（358.95 秒）。评测（探针不加载个人数据）：调优集简体 44／44／0、繁体 42／44／0（[报告](benchmarks/chinese-tuned-cn06c.json)），独立集共同 72 条四种模式均为 54／64／5（[报告](benchmarks/chinese-holdout-v1-cn06c.json)），与 cn01r 报告逐条比对（程序比较名次和前 7 项，未查看样本）无差异，compose 结果相同。
- 测量（见[选词记忆](SELECTION_MEMORY.md#测量cn-06c)）：与 HEAD 构建的探针交替运行，不挂表时逐键中位 1.077 ms 对 1.055 ms，平均 1.391 对 1.383，候选逐项相同；挂 5,000 个词时中位 1.172 ms、p95 4.06 ms（约多 8%／6%）。5,000 个词的文件首次读入 1.8 ms，结束一次输入追加 4 个词平均 1.15 ms，另一进程增量读取平均 0.13 ms，核心整表重建约 2.0 ms。
- 未验证：真实应用效果、多个真实应用同时输入、慢速磁盘；未提交、未打包、未安装；Android 未构建（核心新增函数，未改现有接口）；Core/tests 的 Python 检查需要 c++，未运行。
- 下一步：维护者验收；随下一个试用包实机抽查 CN-01b／c／e 与 CN-06c。

## 2026-11-19 · alpha.26 试用包（CN-06c）

- 版本号升至 0.4.4-alpha.26（bad72cc），内容为 alpha.25 加个人词频参与组句（24d4b77）。从干净提交重新生成词库、完整 MSVC Release 构建，54 项 CTest 在一次运行中全部通过（289.63 秒）。
- 生成 Harukoto-0.4.4-alpha.26-windows-x64.zip（66.2 MiB，SHA-256 b254c59c291b443e57cddc8ba53bc63fffd76ac95ea81d072811ab588461210c）和 Harukoto-0.4.4-alpha.26-Setup.exe（170.8 MiB，SHA-256 b2d9f0e1709528e31da2f964dc871e8758efd20ea0c35ed1eb553ff5283af913）。BUILD-INFO 记录源提交 bad72cc、不含未提交修改；包内 DLL 与测试所用 Release 构建哈希一致，DLL 内版本为 alpha.26。
- 打包脚本只从 `.build/windows/ime/` 取 DLL，不会从 CMake 输出目录复制；第一次打包时其中仍是 alpha.25 的 DLL，已手动复制后重新打包，上面是重打后的哈希。只跑 CMake 构建时须先把 `msvc/Release/MeowpuutTip.dll` 复制到 `ime/`（或用 build-ime-msvc.ps1）。
- 词库格式与 alpha.25 相同。本机未安装，未发布，安装包未签名；实机试用待维护者按 VAL-01 进行，重点是个人词频：选过的词换句后是否靠前、误选后能否纠正、关闭选词记忆后是否恢复。此条为打包后记录。

## 2026-11-19 · alpha.25 试用包（CN-01b／c／e）

- 版本号升至 0.4.4-alpha.25（1454d70），内容为 alpha.24 加组句语料统计（335b7ce）、同音偏好第一批（e96c066）、繁体独有词简体降权与通用用语补词（62fc70c）。从干净提交重新生成词库、完整 MSVC Release 构建，54 项 CTest 在一次运行中全部通过（362.50 秒），日语桥接检查通过，渲染器构建完成。
- 生成 Harukoto-0.4.4-alpha.25-windows-x64.zip（66.2 MiB，SHA-256 a7b56e0db54974695865f156704d79e2fa7f2fc204113be976aef1c61fdf3075）和 Harukoto-0.4.4-alpha.25-Setup.exe（170.8 MiB，SHA-256 1921ca93ae9281022e04fc427c0a8b917b5b5bc42f22777e8c4a6ca592337192）。BUILD-INFO 记录源提交 1454d70、不含未提交修改；包内 DLL 与拼音词库和构建产物哈希一致，DLL 内版本为 alpha.25，词库含统计段，授权目录新增 SentenceCorpora-SOURCE.json。内置 PowerShell 签名与安装预检通过，预检不执行实际安装。
- 词库格式与 alpha.24 不兼容，覆盖安装后需要重开各输入应用。本机未安装，未发布，安装包未签名；实机试用待维护者按 VAL-01 进行，重点是长句、口语和游戏用语的首选，以及登录／公式／售后／预约等同音词。此条为打包后记录。

## 2026-11-15 · CN-01c／CN-01e 剩余部分（整词样本、繁体独有词、补词、重新登录）

- 新建整词开发样本 Windows/tests/data/chinese-words-dev.tsv（250 条，日常／工作／电脑／游戏，先写后查，可调优；输入与独立集无重复，脚本比对，未阅读独立集）。基线简繁 FF14 关均为首选 218／前 7 项 250，FF14 开 209／250（[报告](benchmarks/chinese-words-dev-before.json)）。
- 繁体独有词在简体中的刻度（scripts/compile-native-lexicon.py）：同一无声调读音下分数最高的有来源词在语料中出现至少 20 次、繁体独有词不足 20 次且语料切分会把它当整词计数时，后者的 hans_weight 降到对手之下 1 分，最多 1400 分；繁体权重和人工词条不动。逐条看过后，把存疑或变差的日常词（像话、北京话、试一次、万寿、花田、慕斯等）共 59 条写进人工豁免表 Resources/Lexicon/simplified-traditional-only-keep.tsv（编辑判断，2026-11-15），编译时不降权；对手是切分噪声（北京华、十一次）的情况没有简单有界的规则可排除，只靠豁免表。最终 156 条读音、151 组受影响，120 组简体首位改变（布希→不惜、峇里岛→巴厘岛、爱滋病毒→艾滋病毒等），清单在编译输出 simplified-traditional-only-audit.json。单独测量：整词样本简体 218→219，调优集、119 条开发题、独立集逐条不变。
- 用语料词频普遍修正整词顺序：只做分析，不实现。在整词样本上与组句相同的修正改对 9、改错 3，但全词库改变 2,127 组首位，抽样改对改错相当，错的集中在切分噪声和新闻财经腔；找不到能挡住这两点的有界规则。
- chinese.tsv 新增 93 条通用游戏组队、网络和口语用语（开团、掉线、补刀、氪金、摆烂、咋了、好哒等），凭判断列出、先列后查，未参考独立集；权重为编辑优先级，规则写在词表注释和[社群词条](LEXICON_COMMUNITY.md)。93 条简繁首选 77、其余在前 7 项。
- “重新登录”：simplified-priorities.tsv 中有简体编辑权重的词不再写负的语料修正（登录、公式、共识、售后、惊艳），调优集 pc-04 改为首选。把全部人工词条都豁免的方案试过，独立集游戏类掉 1 条首选，未采用。shouyin 的“手淫”：现有机制没有不雅词降权或过滤，只报告。
- 评测（源码 e96c066 加本批未提交改动，含 B 项豁免表，每组 3 轮；评测时本机有 FF14 客户端在运行；加入豁免表前后的数字相同）。整词样本（[报告](benchmarks/chinese-words-dev-cn01r.json)）四种模式 218／218／209／209→225／225／216／216，前 7 项均 250，游戏类 42→49。调优集（[报告](benchmarks/chinese-tuned-cn01r.json)）简体 43／44／0→44／44／0，繁体 42／44／0 不变。119 条开发题简体 96→97（FF14 开 97→98），繁体不变（每组 1 轮，未归档）。独立集（[报告](benchmarks/chinese-holdout-v1-cn01r.json)）共同 72 条四种模式 53／64／5→54／64／5；按类别（简体，FF14 关）聊天 20／23／1、工作 20／22／2 不变，游戏 13／19／2→14／19／2；只统计了进出条数（每种模式进首选 1、掉出 0，前 7 项无进出），没有查看具体样本。
- 重新生成词库，完整 MSVC Release 构建通过，54 项 CTest 在一次运行中全部通过（加入豁免表后 297.34 秒，FF14 客户端在运行；此前两版词库也各跑过一次，均 54／54），没有测试需要修改。scripts/check-community-lexicon.py 和 Core/tests 的 Python 检查需要 c++ 编译器，本机未运行。
- 未验证：真实应用效果；未打包、未安装、未提交；繁体顺序未改；独立集仍是 72 条小样本。补词和权重是编辑判断，没有外部语料依据。
- 下一步：维护者验收本批；随下一个试用包按 VAL-01 实机抽查；之后为 CN-06c。Windows 0.4.4-alpha.24（试用包，不含 CN-01b 起的改动）、FF14 0.3.0 保持。

## 2026-11-15 · CN-01c 同音偏好（第一批）

- 查清已知的整词同音排错原因：单独输入一个词时按词库权重排序，简体用 hans_weight。登录／登陆、守候／售后、愉悦／预约、连接／链接、手印／收银都是 Pinyin Pro 来源分数本身如此；gongshi 的“公视”是繁体独有词（Pinyin Pro 无此词，McBopomofo 公視 出现 1573 次），按繁体刻度换算（偏移 969、减 350）得 5772，高过所有简体来源词。
- 只改 Resources/Lexicon/simplified-priorities.tsv（只影响简体），新增 5 条编辑权重，各自刚好越过需要越过的那个词，其余顺序保持来源顺序：登录 5180（登陆 5126）、公式 5850 与共识（gong4 shi2）5800（公视 5772）、售后 3700（守候 3650）、预约 3900（愉悦 3865）。逐条依据见[中文评测](CHINESE_EVALUATION.md#同音偏好cn-01c2026-11-15)：售后、预约、公式／共识以语料词频为据；登录是编辑决定，依据为规范用法和语料中“登陆”约四分之一到四成是登录义的抽样估计。
- 查过但不改：连接／链接（来源与语料都是连接多，链接不是唯一合理首选）、手印／收银（两份来源都是手印多，只有主观判断）。繁体模式不改：simplified-priorities 只作用于简体，繁体的公視在台湾用语中确实常见，改繁体需在 chinese.tsv 加双字形人工词条，留待决定。
- 改前→改后（简体单独输入）：denglu 登陆、登录 → 登录、登陆；gongshi 公视、共识、公式 → 公式、共识、公视；shouhou 守候、售后 → 售后、守候；yuyue 愉悦、逾越、预约 → 预约、愉悦、逾越。119 条开发集（调优集加 Core/tests/data 开发／验证题）简体首选 92→96（FF14 开 93→97），只有这 4 条名次变化，繁体不变。
- 评测（源码 335b7ce 加本批改动，每组 3 轮；评测时本机有 FF14 客户端在运行）。调优集（[报告](benchmarks/chinese-tuned-cn01c.json)）简体两种模式 42／44／0→43／44／0（gongshi 改为首选），繁体两种模式仍为 42／44／0。“重新登录”（pc-04）仍是第 2：组句时语料词频修正对登录的扣分比登陆多，抵消了编辑权重，约差 0.3 奈特；单独输入 denglu 已是登录在前。独立集（[报告](benchmarks/chinese-holdout-v1-cn01c.json)）共同 72 条四种模式均为 53／64／5，按类别（简体，FF14 关）聊天 20／23／1、工作 20／22／2、游戏 13／19／2，与上一批相同，且逐条名次与前 7 项均无变化（只比较了总数，没有查看具体样本）。
- 重新生成词库（manifest 记录编辑权重 7 对），完整 MSVC Release 构建通过，54 项 CTest 在一次运行中全部通过（328.55 秒，运行时 FF14 客户端仍在运行），没有测试需要修改。Core/tests/check_simplified_editorial.py 中记录的编辑权重条数由 2 改为 7，该检查依赖 c++ 命令，本机未运行。
- 未验证：真实应用效果未实机验证；未打包、未安装（已提交为 e96c066）；Android 未构建。yuyue→预约 来自 Core/tests/data/pinyin-validation.tsv，本批据此调整后该题不再是未见过的验证题；eval-pinyin-holdout.py 会因人工词表重叠自动排除登录、售后、预约三题。
- 发现的系统性问题未实现，见[中文评测](CHINESE_EVALUATION.md#同音偏好cn-01c2026-11-15)的“未实现的系统性办法”：繁体独有词在简体中排在有来源的同音词之前（粗查 265 组），以及按语料词频整体修正整词顺序（粗查 1,040 组前两名与语料相反，含不少切分噪声和新词）。
- 下一步：维护者决定是否处理繁体模式与上述系统性办法；随下一个试用包按 VAL-01 实机抽查；之后为 CN-03。Windows 0.4.4-alpha.24（试用包，不含 CN-01b／CN-01e 与本批）、FF14 0.3.0 保持。

## 2026-11-12 · CN-01b／CN-01e 组句引入语料统计

- 新增 scripts/build-collocations.py：从 Leipzig 句子语料（zho-cn_web_2015_1M、zho_news_2007-2009_1M，CC BY）和 Tatoeba 中文句子（CC BY 2.0 FR）统计，输出 Resources/Lexicon/collocations.tsv.gz（100 万对相邻词搭配，其中短语开头 ^／结尾 $ 标记 37,533 对）和 word-priors.tsv.gz（64,704 个词的连续文本词频）。仓库只存统计，不存句子；来源、哈希与计数见 DataSources/SentenceCorpora 下的 README.md、SOURCE.json、collocations-report.json。
- scripts/compile-native-lexicon.py 在拼音索引末尾写可选的 `# statistics` 段（authored／priors／pairwords／pairs），格式见 [ENGINE](ENGINE.md)。词库格式因此变了：旧 DLL 读不了新词库，程序与数据须一起更新。
- Core/MeowpuutCore.cpp：组句时词得分加语料词频修正（负向只计一半，上下限 6 奈特，出现不足 20 次不修正）；相邻词搭配加分按满刻度计、单对封顶 6 奈特；有搭配数据时，“跨界恰好拼成词库词”的旧加分减半，context.tsv 人工短语加分为 1.5 倍。单独输入一个词时的同音词顺序不变；没有统计段时行为与之前相同。统计段从调用方缓冲区解析，不留在内存。Windows/TextService.cpp 与 tests/CandidateProbe.cpp 改为按文件大小一次读入词库。
- 调参只用从语料按哈希留出的开发短语（不入库，未参与正式统计）：口语 1500 条首选 649→约 1000、前 7 项 998→约 1270；书面 1500 条首选 635→约 939、前 7 项 950→约 1176。项目内 119 条开发集（调优集加 Core/tests/data 的拼音开发／验证题）未命中 35→27。独立集没有用于调参。工作区里最初的一版（30 万对、权重 0.4）在独立集上首选仍为 43／72，因此重做。
- 评测，源码 f6e9621 加本批未提交改动，每组 3 轮。独立集（[报告](benchmarks/chinese-holdout-v1-cn01be.json)）共同 72 条在四种模式下均为首选 53、前 7 项 64、缺失 5（CN-08 时为 43／56／11）。简体、FF14 关闭按类别：聊天 14／19／4→20／23／1，工作 17／20／4→20／22／2，游戏 12／17／3→13／19／2。三类都没有总数回退，但游戏类内部有 2 条由首选变为非首选、1 条掉出前 7 项，净增只有 1 条首选；聊天与工作没有掉出的样本。主题 4 条简体全部首选、繁体 3 条，与之前相同。compose：72 条都能只靠选词打出，一次选择完成 57→64，不用翻页 58→65，最多翻页仍为 5。调优集（[报告](benchmarks/chinese-tuned-cn01be.json)）四种模式均为 42／44／0，不变。
- 耗时与内存：独立集逐键 P50／P95 在简体、FF14 关闭下为 1.14／3.83 ms（CN-08 归档 1.11／3.56，基线 0.98／3.27），本次评测时本机有一个游戏客户端在运行（CPU 占用约 45%），不能据此判断增减。探针新进程加载 462 ms（CN-08 归档 487 ms），加载后私有内存 122.3 MiB（CN-08 归档 116.5 MiB），即评测探针里多了约 6 MiB。另行用候选探针空闲测量为加载约 420 ms、私有内存约 129 MB；改动前约 505 ms／135 MB 的对照测量时有测试并行，偏高，不宜据此说内存下降。
- 完整 MSVC Release 构建通过，54 项 CTest 在一次运行中全部通过（310.58 秒）。MixedCompositionTests.cpp 新增 verifyCompositionStatistics（合成词库：句末与句首标记、相邻搭配、封顶、词频修正不影响单独输入、跨界加分减半与人工短语加权、19 种坏格式拒绝）。
- 放宽了三处断言：JapaneseCandidateIntegrationTests.cpp 中 wenjianj 第二页以后的备选不再写死“问见”，改为任一消耗 wenjian 的候选，因为同音备选的顺序随搭配统计变化；ThemeReloadTests.cpp 中“特级恢复药”开主题后仍为首选，但核心自己的初始候选里不再有同名整句，主题候选因此不带原生选词索引，测试改为有索引走索引、没有则只验证它是非日语的首选（文本服务对无索引候选按整串输入提交）。
- 删除了上次参数扫描遗留的临时文件 Windows/tests/data/chinese-holdout.tsv.x-x.wrong。
- 未验证：真实应用中的效果未实机验证；已提交为 335b7ce，未打包、未安装；Android 未构建；Core/tests 下依赖 c++ 命令的 Python 检查未运行。调参用的留出短语与统计来自同一批语料的分布，开发集上的提高偏乐观；独立集仍是单人手写的 72 条小样本。
- 已知问题：不开主题时 tejihuifuyao 首选是“特机会服药”（之前是“特级会服药”，同样不对）；书面语料是 2007–2015 年的新闻和网页，新词与游戏用语偏少；口语语料只有约 51 万词。
- 下一步：（已提交为 335b7ce）出试用包，随 VAL-01 实机抽查长句与口语；之后为 CN-01c 同音偏好。Windows 0.4.4-alpha.24（试用包，不含本批）、FF14 0.3.0 保持。

## 2026-11-12 · alpha.24 试用包（CN-08＋CN-06）

- 版本号升至 0.4.4-alpha.24，内容为 alpha.22 加逐段兜底候选和选词记忆。运行 Windows/build-ime-msvc.ps1：重新生成词库、完整 MSVC Release 构建、54 项 CTest 在一次运行中全部通过（273.51 秒），渲染器构建完成。
- 生成 Harukoto-0.4.4-alpha.24-windows-x64.zip（60.4 MiB，SHA-256 be83c3708502932290220c20850100f6ae3fb6fb03a7ad43f5dc0deb29fe51de）和 Harukoto-0.4.4-alpha.24-Setup.exe（165.0 MiB，SHA-256 ee8e7fc12987451bd9c08ae593f30d8c25cc838e9854346299ba0a878f0b8b41）。包内 DLL 与构建产物一致，含管理脚本和选词记忆说明，校验和一致；安装器预检通过，机器注册未改变。
- 试用包不是从干净提交构建：源提交 ed3141b 加未提交的 CN-06 与版本号改动。此前的 alpha.23 试用包只含逐段兜底候选，同样未从干净提交构建。本机未安装，未发布，安装包未签名；实机试用待维护者进行。

## 2026-11-12 · CN-06 选词记忆（调频与自造词）

- 新增 Windows/SelectionMemory.h：内存模型按“拼音＋简繁 → 文字”记录确认过的选择，次数多者在前、相同则最近者在前，单条次数封顶 8，每串拼音最多 8 种文字，总量 20,000 条。存储为当前用户 %LOCALAPPDATA%/Meowpuut/selection-memory.tsv 的追加日志，多个进程共享：各自追加整行，按已读偏移增量读取他人的新增，不重复计入自己写的行；文件变短视为清除或合并并重新读取；启动时超过约 1 MiB 则独占合并为每条一行，上限 4 MiB。
- 接入 TextService：chooseCandidate 记录确认的候选及其消耗的拼音；分段选完整串拼音时另记整句；refreshCandidates 把记过的整串拼音选择移到最前（最多三项），词库拼不出的整句作为整串候选插入。取消、失焦、空格上屏原文不记整句；标点触发的自动补完只记高亮的第一段；F6 活用面板的选择不记。开关为 HKCU/Software/Meowpuut/Input 的 SelectionMemory，缺省开启，每次按键读取。实现放在 Windows 合并层而不是核心，因为最终候选顺序在这一层决定；核心、候选合并函数和实验引擎未改。
- 新增 Windows/manage-selection-memory.ps1：查看、导出 CSV、清除（原地截断，各进程自动同步）、关闭与开启；已加入安装包。设置窗口暂无对应开关。
- 完整 MSVC Release 构建通过，54 项 CTest 全部通过。全量运行时有 4 项 PowerShell 测试显示未运行，原因是构建目录缓存的 pwsh 路径指向应用商店包目录、从当前终端无法直接执行，与代码无关；把缓存改为可执行的别名后这 4 项通过，其余 50 项在全量运行中通过（276.25 秒）。新增 selection_memory（模型、双存储对象共享、清除、占用、合并、超限、开关）、selection_memory_tool（管理脚本）两项，tsf_focus_cancel 增加经正式选词路径的记忆回归。
- 评测探针不加载选词记忆，两套评测的数字不变，也不反映本功能的效果；“纠正一次后下次首选正确”由服务级回归验证，没有在独立集上量化。
- 未验证：真实应用中的效果、多个真实应用同时输入、输入线程上追加与增量读取的耗时。已知边界见[选词记忆](SELECTION_MEMORY.md)：合并或清除与并发追加的极端时序可能丢个别记录或延迟同步；记录是明文，卸载不删除。下一步为 CN-01b／CN-01e。FF14 0.3.0 保持。

## 2026-11-11 · alpha.23 试用包（CN-08）

- 版本号升至 0.4.4-alpha.23：安装器、打包脚本、运行时诊断的 service_version 及相关文档。除版本号外没有改动运行时代码；相对 alpha.22 的输入行为变化只有 CN-08 逐段兜底候选。
- 运行 Windows/build-ime-msvc.ps1：重新生成词库、完整 MSVC Release 构建、52 项 CTest 全部通过（427.56 秒）、日语桥接检查通过，渲染器构建完成。
- 生成 Harukoto-0.4.4-alpha.23-windows-x64.zip（60.4 MiB，SHA-256 8b1f4233e43ce7c7cd44d7d235788eab8a917a1433fc327eb563323881a85069）和 Harukoto-0.4.4-alpha.23-Setup.exe（165.0 MiB，SHA-256 f193e6951bb0cc99895880707fb2a8173aebea3e6437e33e366031ed41cb550c）。包内 DLL、原生拼音词库、渲染器与构建产物哈希一致，两份校验和文件与实际文件一致，DLL 内含新版本号。
- 安装器预检通过：内置 PS7 解包、安装／卸载脚本预检、机器注册未改变、安装窗口渲染。预检不执行实际安装。
- 这是试用包，不是从干净提交构建：源提交 ed3141b 加未提交的版本号改动，BUILD-INFO 记录“包含未提交修改：True”。提交版本号改动后如需留档应重新打包。
- 本机未安装，未发布 Gitea Release，安装包未签名。待维护者安装后按 VAL-01 在记事本、QQ、FF14 实机确认第二页积木、继续翻页和分段选词上屏；升级后需重开各输入应用加载新 DLL。FF14 词库 0.3.0 保持。

## 2026-11-11 · CN-08 逐段兜底候选

- Core/MeowpuutCore.cpp 新增 leading_words：列出读音为输入开头部分的完整拼写词与单字，读音由长到短，同读音按词库顺序。只在由已知单字读音拼成、且后面还能接一个音节的位置切分；显式分隔符与声调归属它所结束的音节。只做词库查找，不做整句搜索。
- 两处接入。初始候选超过一页时，在第 8 项起插入最多 7 项：最长的两个多音节读音各取两个词，再用首音节常用单字补满；不足一页时不插入，Android 的整句优先模式不插入。首次 mpn_expand_candidates 先追加完整列表，再做原有的整句扩展；扩展轮数仍为 3，已有索引不变。Windows 合并层、文本服务和实验引擎未改，沿用现有的部分选词与扩展合并。
- 候选探针新增“拼音<TAB>文字”行：在完全展开的候选上求只靠选词打出该文字的最小代价（每次选择和每次翻页各计 1），与文本服务翻页扩展的做法一致，但每个状态从剩余拼音重建，名次是近似值。evaluate-chinese.py 把它写入报告的 compose 字段。
- 独立集 72 条：能打出的从 69 增至 72，最多翻页从 11 降到 5，不用翻页的仍为 58。改动前的数字来自临时换回旧核心的同一探针。两套样本四种模式下首页 7 项逐条相同，首选／前 7 项／缺失不变（独立集 43／56／11，调优集 42／44／0）；第 8 项以后的整句变体顺延 7 位。
- 完整 MSVC Release 构建通过，52 项 CTest 全部通过（372.11 秒）。中途 session 一项失败，原因是首版把积木补进了不足一页的列表，改变了首页内容，已改为只在超过一页时插入。新增 verifyLeadingWords 合成词库回归（首页不变、第二页内容、扩展后索引不变、非音节边界拒绝、分段选词保留余串、分隔符归属、单音节不重复），以及独立集 72 条逐条可达的探针测试；评测单元测试共 7 项通过（41.5 秒）。
- 耗时：归档运行逐键 P50／P95 为 1.11／3.56 ms（基线 0.98／3.27）；同负载交替测量的差异在波动范围内。首版每键扫描缩写大桶导致明显变慢，已限制为每个读音只检查前 256 行。
- 未做：Core/tests 下依赖 c++ 命令的 Python 检查（本机和构建机都没有该编译器）未运行；Android 未构建；真实应用中的翻页和上屏未实机验证；未打包安装。第二页单字只有最常用的 7 个，首音节的词不一定排在单字前，留待打磨。下一步为 CN-06a 选词调频。Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 2026-11-11 · CN-01d 与微软拼音的对照及计划调整

- 新增 scripts/compare-chinese-holdout.py 和记录表 docs/benchmarks/chinese-holdout-v1-comparison.csv：76 行，预填独立集基线中晴词的简体首选与名次。只记录对方的首选文字，不复制其词库或词频。汇总输出双方命中、仅一方命中、都未命中的数量与分类；读取兼容 UTF-8 与 GB18030，输入或期望被改动、编号增减、名次自相矛盾都会拒绝。
- 维护者已在微软拼音上填完全部 76 行（云候选和个人学习开启，版本未记录，未记录期望词名次）。前九行误填的名次中有两处与首选文字矛盾，已清空，其余未改。共同 72 条首选：晴词 43、微软拼音 61，仅微软命中 21 条，都未命中 8 条；主题 4 条晴词 4、微软 2。三条对方上屏内容与表中拼音不符，疑为输入出入，按未命中计；全算命中则对方为 64。条件偏向对方，不是干净对比。
- 仅微软命中的 21 条中，句中单字同音选错 14 条、分词走偏 5 条、游戏用语 2 条。都未命中的 8 条里有三条是的／得，对方同样输出“的”，v2 样本应放宽。
- 维护者反馈的最大差距是容错与记忆：对方首选不对时可以一直往后选，选过一次下次就对。核对代码确认：初始候选仅 14 到 24 项且几乎都是整句变体，没有句首的词和首音节单字；mpn_expand_candidates 三轮扩展只追加消耗整串拼音的句子；核心不记录选词。29 条未命中样本中 9 条在初始候选里没有任何可用的正确前缀。
- 开发计划据此调整：新增 CN-08 逐段兜底候选并排到最前，CN-06 选词调频与自造词紧随其后，CN-01b／CN-01e 其次，模糊音、简拼、纠错增强顺延。
- test_chinese_evaluation.py 新增往返与校验测试，共 6 项单元测试通过（2 项需真实探针的在直接运行中跳过，上一批已随 CTest 通过）；已填写的记录表通过同一校验。本批未改核心、词库、排序或评测样本，未重新构建。下一步为 CN-08。未打包、安装或修改用户词库，Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 2026-11-11 · CN-01a 独立评测集与基线

- 开发计划已重新排序：第一主线为日常中文输入体验，独立引擎按原设计作为第二主线，见[开发计划](DEVELOPMENT_PLAN.md)。本批是第一主线的第一项。
- 新增 Windows/tests/data/chinese-holdout.tsv（holdout v1）：日常聊天、工作用语、游戏组队各 24 条，FF14 主题 4 条，共 76 条，含短词、口语和最长 50 个字母的句子。先写样本后运行引擎；运行前按预定规则替换了 7 条与人工词表整条相同的样本（其中 1 条的首个替换仍重复，再换一次），运行后未修改任何样本或可接受集合。
- evaluate-chinese.py 新增 `--set tuned|holdout`（默认 tuned，原行为不变）、`--compact` 归档选项和 corpus_set 字段；选择 holdout 时先检查与调优集输入／目标及四份人工词表的重复，有重复则拒绝运行。原第四条说明文字改为按样本集区分。
- 基线从 a449936 加本批工具改动运行（dirty=true），每组 3 轮，两次独立运行的逐条结果一致：共同 72 条在简繁、FF14 开关四组均为首选 43、前 7 项 56、缺失 11；聊天 14／19／4，工作 17／20／4，游戏 12／17／3。主题 4 条简体全部首选，繁体 3 条。同版本调优集为 42／44，独立集首选约 60%。
- 未命中的 29 条主要是句中单字同音选错、分词错误、的／得／地、游戏常用语靠后，以及一条 FF14 繁体名称转换错误（格里達尼亞被输出为格裡達尼亞，属词库数据问题，样本保留为未命中）。分类与例子见[中文评测](CHINESE_EVALUATION.md)。
- test_chinese_evaluation.py 新增冻结与防泄漏测试：类别数量、主题范围、与调优材料零重复、LF 文本 SHA-256 固定，并验证检查函数能识别复制输入和人工词条。5 项单元测试含真实探针全部通过，CTest chinese_evaluation_probe 通过（4.04 秒）。本批未改核心、词库或排序，未重新构建，其余 51 项 CTest 未重复。
- 独立集仍是单人手写的小样本，不是外部盲测或准确率认证；逐键耗时因句子更长不能与调优集直接比较，也不含 TSF 与渲染。下一步为 CN-01d（与商业输入法的结果对照），需要人工在对方输入法上逐条记录。未打包、安装或修改用户词库，Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 2026-11-11 · WIN-03b 接收方独立核对部署身份

- 新增 EngineVerifiedPeer：接收方用自己信任的 EngineDeployment 核对私有交接的真实进程对象，只保留不可继承的 query／wait 副本；拒绝空策略、线程模拟令牌、错误对象、已退出进程、不同登录身份和映像不符。connect 在打开管道前后各核对一次，失败不发送 Hello、输入或控制请求，不等待、重试或寻找其他实例。
- EngineRegistration 增加可选 DeploymentPolicy（owner／engine 必须成对且为不同文件），在第一条请求前核对双程序；deploymentVerified() 只表示初次接收结果，不是心跳。Endpoint 携带宿主策略并提供 connect()，EngineProcess 的预分配／动态注册、静态／动态端点和 claim() 自动传播已启用的策略，接收失败撤销本次已消费的槽。未配置时行为保持。
- 部署对象改为只读共享引用，Channel 首个成员持有它：注册、端点和启动方释放后，存续连接继续固定 EXE 与父目录，最后一个引用释放才解除。共享引用只延长文件保护，不延长进程寿命，原 Job 回收规则不变。
- 增量 MSVC Release 构建通过；全部 22 项 engine_ 前缀 CTest 通过，共 177.27 秒：控制 0.01、接入 0.01、进程 0.38、实例 0.07、宿主 0.11、管道 0.08、核心 0.04、服务端 0.04、消息 0.07、协议 0.02、部署 0.25、接收方核对 0.20、诊断 0.42、直接宿主进程 12.55、独立所有者 21.53、逐键基准 7.23、进程间基准 36.11、全量核心 6.61、全量合并 21.47、全量宿主 16.68、全量所有者 53.34 秒；另一项为执行器，单项耗时未摘录。新增 engine_verified_peer 后共 52 项，其余 30 项未重复。
- 新回归用临时复制的真实进程覆盖同内容不同文件、错误对象、空／单边／同文件策略、owner／engine 各自不符、线程模拟、受限句柄权限、批准进程不能认证他人管道及远程管道名拒绝；错误导入不发布控制请求。真实管道完成 Hello 和有载荷往返，宿主退出后不能重连，两条连接逐个销毁后目录才可改名。微型／全量所有者用例改为三个独立客户端各自从命令行取得可信路径与哈希、核对双程序后申请端点，简繁、假名、FF14、部分与重复选择保持；父方另查预分配与动态策略传播。
- 新增[接收方校验说明](ENGINE_VERIFIED_PEER.md)，同步计划、部署／所有者／进程／接入文档与基准源码指纹。校验只证明批准的 EXE 和保留对象，不证明公开控制映射的来源；可信发行元数据来源、公开发现握手、竞争失败交接、正式常驻和 TSF／换代仍待实现。未测新增接收与连接校验成本，未验证跨账户／会话矩阵，未打包、安装或修改用户词库，Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 2026-11-11 · WIN-03b 部署文件固定与实际映像核对

- 新增 EngineDeployment 及 Config.hostSha256／ownerSha256（默认空）：调用方提供已信任的 64 位哈希，独立所有者模式须配对；固定 EXE 与卷 GUID 解析后的全部父目录，拒绝写入／删除／目录改名，允许同级正常文件创建。按同一保留文件对象分块计算哈希，核对卷和 128 位文件 ID，失败拒绝，不从公开 PID 或自报哈希恢复信任。
- 直接宿主／所有者都在挂起创建、加入 Job 后核对实际原生映像与文件 ID，成功才恢复线程。所有者接收预期宿主哈希并对内层执行同样检查；外层接受私有宿主对象后、Ready 前再次核对，错误宿主记 DeploymentIdentity，不保留 engineHandle 或交出接入能力。两个路径实际为同一文件时拒绝。默认未配置行为保持，文件／目录固定持续到 EngineProcess 销毁。
- 状态和只读诊断增加 deploymentRequired／deploymentVerified，文本为简体中文、JSON 为 deployment_required／deployment_verified；表示初始映像检查结果，停止后保留历史，不是持续内存校验或健康心跳。开发 v1 诊断 flags 增加两个已知位、失败类别追加 deployment_identity，未知值仍拒绝，应配套构建。
- 完整 MSVC Release 构建及 13 项相关 CTest 全部通过，共 130.55 秒：控制 0.02、接入 0.02、进程 0.48、实例 0.09、宿主 0.10、管道 0.09、部署 0.27、诊断 0.56、直接宿主进程 7.96、独立所有者 20.09、进程间基准 35.50、全量宿主 15.89、全量所有者 49.44 秒。新增 engine_deployment 后共 51 项，其余未重复；仅保留既有 /UNDEBUG 编译提示。
- 新回归覆盖坏哈希／格式／配对、空文件／目录、同内容不同文件／硬链接、保留期间文件与父目录修改拒绝及释放后可操作；真实直接子进程、已退出／错误对象、私有锚点有效但宿主映像错误的拒绝；微型／全量真实所有者开启双哈希后，三个独立动态客户端完成简繁、假名、FF14、部分与重复选择回归，并覆盖状态／诊断、实例竞争、撤销和退出。
- 新增[配置、路径固定与信任边界](ENGINE_DEPLOYMENT.md)，更新计划／诊断／接手入口，基准指纹增加部署头文件与复用的 BaseDictionaryFiles.h。可信元数据来源、公开发现握手与竞争失败交接、正式常驻和 TSF／换代仍待实现；未测新增校验成本，未打包、安装或修改用户词库，Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 2026-11-11 · WIN-03b 实验引擎只读诊断入口

- 新增 EngineDiagnostics 及原生 `harukoto_engine_diagnose.exe --process <根PID> [--json]`。Config.diagnostics 默认关闭，由持有 EngineProcess 的可信启动方发布；支持直接宿主和独立所有者，文本为简体中文，JSON 固定格式。只含进程／创建时间、状态／故障、控制／接入状态及计数，不含管道名、原始句柄、输入、候选或个人路径。
- 每个根进程使用 264 字节观测映射，按 PID 和创建时间命名；受保护 DACL 只给当前登录 SID 读取权限，拒绝已有对象，读取最多三次一致性采样。诊断创建／发布失败仅记 diagnosticsWin32，不改变原控制／查询和进程寿命。映射与公开 PID 都不能作为可信发现、认证或能力交接来源。
- 记录分开显示上次发布、实际 poll 和最后自行领取 Inspect 的时间／年龄。读取不消费 Pending／Ready 回复，不申请端点或刷新心跳；退出后的旧 ready、缺失／写入中／无效记录、发布进程不可用均有明确边界。真实进程测试保留旧映射，验证不能将其视为新实例。
- 完整 MSVC Release 构建与 12 项相关 CTest 通过，共 116.58 秒：控制 0.02、接入 0.02、进程 0.44、实例 0.08、宿主 0.11、管道 0.09、诊断 0.51、直接宿主进程 7.15、独立所有者 17.39、进程间基准 32.73、全量宿主 14.81、全量所有者 43.21 秒。新增 engine_diagnostics 后共 50 项，其余未重复；编译只保留既有 /UNDEBUG 提示。
- 新回归覆盖包校验／保留字节、对象冲突和写权限拒绝、冻结发布后的年龄增长、跨进程并发写读与文本／JSON 命令行；真实所有者覆盖启动、绑定、动态补充、撤销、中文选择、未领取 Inspect 保留和领取后缓存、停止／崩溃以及默认关闭。原全量简繁、假名、FF14、部分选词和动态接入回归保持。
- 新增[使用、格式与观察边界](ENGINE_DIAGNOSTICS.md)，同步计划和接手入口，基准指纹加入诊断头文件。静态基准默认不开诊断，没有测发布或新增所有者／调度成本；可信发现／交接、正式常驻及 TSF／换代继续推进。本批未打包、安装或修改用户词库，Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 2026-11-11 · WIN-03b 运行中创建与补充接入槽

- 新增 EngineAdmission 私有通道及 Config.dynamicRegistrations（默认关闭，仅独立所有者模式）：可信启动方在 Ready 后异步 requestRegistration／takeRegistrationResult，预分配和动态能力共享同时 8 个槽的上限，满时明确返回 Full。退出／主动撤销后可补充新的映射、事件和单调编号，不复用旧能力，也不自动重试。
- 启动方使用实际保留的所有者对象复制四个新句柄，私有请求成功发布后只由所有者接管关闭；超时不能按旧数字回收远端槽。复制中途／发布前失败由守卫清理，发布后失败至多留一份四句柄在途申请，随后消费或在进程退出时回收。普通客户端不接收接入通道、stop、Job 或复制权限；查询／Allocate／Inspect 格式保持。
- 所有者在宿主请求在途时也可接纳新槽；旧请求同时固定原位置和注册编号，位置替换后丢弃旧回复。收到申请后再次观察撤销，防止按上一轮容量状态误报 Full。Pending／未领取 Ready 占用动态通道，精确超时或坏回复永久停用该通道并撤销未交出的能力，原控制／查询继续可用；停止／实际退出不交出失效结果。状态增加累计创建、当前有效及接入状态／错误，原计数口径已补齐。
- 完整 MSVC Release 构建及 11 项相关 CTest 全部通过，首轮共 108.30 秒：控制 0.02、接入 0.01、进程 0.43、实例 0.08、宿主 0.11、管道 0.09、直接宿主进程 7.10、独立所有者 15.57、进程间基准 32.37、全量宿主 14.79、全量所有者 37.70 秒。最后容量竞争防护修改后再次完整构建，受影响的独立所有者／全量所有者两项复测通过，分别 15.44／37.96 秒，共 53.42 秒。新增 engine_admission 后共 49 项，其余未重复；仅保留既有 /UNDEBUG 编译提示。
- 微型／全量基础＋FF14 回归覆盖预分配／动态混合、8 槽满、连续 24 轮撤销补充及所有者句柄数不增长；真实独立客户端通过动态能力自行申请并查询，保留简繁、假名、FF14、部分选词／重复拒绝。事件屏障在旧请求未返回时撤销并替换位置，验证新槽只收到新回复；另覆盖旧句柄隔离、动态绑定退出、接入 Pending／Ready 停止／崩溃、精确超时与原通道存活，以及发布前部分复制失败回收。
- 新增[动态接入与所有权说明](ENGINE_ADMISSION.md)，同步计划、接手文档和基准源码指纹；静态基准不代表补槽／调度性能。下一步落实可信发现／交接、正式常驻及只读诊断，再接 TSF／换代。本批未打包、安装或修改用户词库，Windows 仍为 0.4.4-alpha.22、FF14 0.3.0。

## 2026-11-10 · WIN-03b 客户端退出监测与接入撤销

- EngineProcess 新增 bindRegistration／revokeRegistration：可信启动方可绑定保留的真实客户端进程对象，Ready 等待集合观察正常／强制退出后撤销对应槽，也可主动撤销。只复制 query／wait 权限，不通过公开 PID 查找进程；重复绑定、已退役或未发出的槽拒绝。状态增加配置／已发出／当前绑定／退役计数，随 poll 刷新。
- 每槽独立撤销事件经受限交接，客户端只有等待权限；撤销使 Idle／Pending／未领取 Ready 进入 Closed。所有者回收对应映射和请求／回复句柄，移除等待事件；排队请求不转交，已转交请求消耗后丢弃回复。坏请求也单独撤销，不影响其他客户端。已有查询连接继续使用；固定槽不复用或动态补充，仍不是普通应用自助注册。
- 完整 MSVC Release 构建和 10 项相关 CTest 全部通过，共 93.69 秒：控制 0.02、进程 0.42、实例 0.08、宿主 0.10、管道 0.08、直接宿主进程 7.35、独立所有者 11.50、进程间基准 33.02、全量宿主 14.90、全量所有者 26.19 秒。当前仍 48 项，其余未重复；仅保留既有 /UNDEBUG 编译提示。
- 微型／全量夹具新增事件屏障，固定在途请求再撤销其槽与排队槽，验证迟到回复丢弃、排队 Allocate 未到宿主、旧句柄无法重新激活；真实客户端正常／强制退出、已退出对象绑定、错误对象、只读撤销权限、未领取 Ready 撤销和原查询选词继续可用均通过。基准仍为原静态路径，不能当成撤销调度的性能测量。
- [接入交接及寿命说明](ENGINE_REGISTRATION.md)同步四句柄启动格式、绑定方法、计数与边界。下一步落实可信发现／交接、动态创建与补充注册槽、正式常驻和诊断入口，再接 TSF／换代。本批未打包、安装或修改用户词库，Windows 仍为 0.4.4-alpha.22、FF14 0.3.0。

## 2026-11-10 · WIN-03b 受控客户端独立申请端点

- 新增 C++ EngineRegistration 和 ownerRegistrations（默认 0，最多 8）：可信启动方预分配私有接入能力，Ready 后按槽一次交接；独立客户端自行 Allocate／Inspect，取得真实宿主的受限对象后直接查询。原启动方控制、stop、Job 和 PROCESS_DUP_HANDLE 不交给客户端，查询协议不变。
- 所有者将启动方与客户端端口轮转调度，内层仍只有一个在途请求；回复按原端口／序号关联。坏客户端请求只停用对应端口，宿主控制失败仍按原停止路径处理；所有者或宿主退出撤销 Idle／Pending／未领取 Ready。注册槽固定且不复用，端点仍按既有容量／过期规则回收。
- 完整 MSVC Release 构建及 10 项相关 CTest 通过，共 89.94 秒：控制 0.02、进程 0.43、实例 0.08、宿主 0.10、管道 0.09、直接宿主进程 7.21、独立所有者 10.98、进程间基准 32.20、全量宿主 14.70、全量所有者 24.11 秒。新增微型／全量夹具的三个独立客户端自行接入、坏端口隔离、八客户端与上游同时请求、序号相同不串回复、2 成功／6 Full、正常停止／崩溃／故障宿主时撤销回复。当前仍 48 项，其余未重复。
- [受控接入、交接步骤与资源边界](ENGINE_REGISTRATION.md)已记录，基准指纹增加 EngineRegistration.h。普通应用仍不能自行发现或取得注册能力；下一步是可信发现／交接、动态注册与退出回收、正式常驻及诊断入口，再接 TSF／换代。本批没有测新调度性能，也未打包、安装或修改用户词库；Windows 仍为 alpha.22，FF14 0.3.0。

## 2026-11-10 · WIN-03b 可选单实例竞争与宿主占位寿命

- 新增 C++ EngineInstance；由可信部署配置 ownerInstance 显式启用，按当前登录身份与部署标识原子占位。四个独立所有者竞争时只有一个 Ready，三个退出码 183／InstanceBusy；输家不启动宿主。空标识及原直接模式保持，不自动附着已有实例。
- 受保护 DACL 只授权当前登录 SID，保留不可继承的受限对象；所有者再显式复制一份给动态宿主。宿主固定保留到操作系统实际结束进程，覆盖所有者崩溃后的 Job 异步清理空档。已有对象、类型冲突或权限不足均拒绝，名称不提供可信发现或身份认证。
- 完整 MSVC Release 构建及 10 项相关 CTest 通过，共 83.22 秒：控制 0.02、进程 0.44、实例 0.08、宿主 0.10、管道 0.08、直接宿主进程 7.17、独立所有者 9.06、进程间基准 32.54、全量宿主 14.55、全量所有者 19.15 秒。新增权限／标识／对象冲突、继承子进程正常／强制退出后的占位回收、四进程竞争与不同标识并行、正常／崩溃／坏词库后重启；原实际查询和多客户端回归保留。当前共 48 项，其余未重复。
- [占位接口、固定句柄寿命与边界](ENGINE_INSTANCE.md)已记录，基准指纹新增 EngineInstance.h。正式可信发现、竞争失败后的接手、常驻生命周期、普通应用注册／诊断入口仍待推进；没有测所有者／占位性能，也未接 TSF、打包、安装或修改用户词库。发行与本机安装保持 alpha.22，FF14 0.3.0。

## 2026-11-10 · WIN-03b 独立所有者与跨进程可信交接

- 新增 C++ 开发用独立所有者进程，监督自己的动态查询宿主，转交 Allocate／Inspect；查询客户端直接连接宿主。首个客户端退出后共享引擎继续工作，可在容量回收后给另一独立客户端分配新端点；普通应用发现／注册尚未接通。
- EngineProcess 可选 ownerExecutable；启动方通过私有映射和保留的所有者对象固定真实宿主对象，分配结果仅交接非继承 query／wait 副本。processHandle 与 engineHandle 分开，源句柄槽在所有者进程寿命内只保留一个，避免关闭／槽复用竞争。没有公开 PID 自认证或把所有者控制权限交给客户端。
- 完整 MSVC Release 构建及 9 项相关 CTest 通过，共 73.88 秒：控制 0.02、进程 0.45、宿主 0.11、管道 0.09、直接宿主进程 7.18、独立所有者 6.72、进程间基准 32.11、全量宿主 14.63、全量所有者 12.55 秒。新增微型／全量夹具上的三个独立客户端、首个退出／第三个接入、简繁／日语／FF14、部分与重复选择、未领取控制请求停止、所有者崩溃、故障宿主退出、坏启动和 Job 回收。当前共 47 项，其余未重复。
- [接口、信任链与生命周期](ENGINE_OWNER.md)已记录；基准源码指纹加入交接头文件，旧基准仍使用直接宿主，没有测新增所有者的性能／内存成本。可信启动方退出仍回收整个进程树，不是完整常驻服务。下一步做正式生命周期、单实例发现、应用注册和诊断入口，再接 TSF／换代；未打包、安装或修改用户词库，发行保持 alpha.22。

## 2026-11-10 · WIN-03b 私有控制、动态端点与容量诊断

- 新增 C++ 私有单槽控制通道，以未命名映射／事件通过显式继承列表交接，不增加公开发现入口或改变查询协议。可选动态模式支持 Ready 后申请新 Listener、全部客户端断开后继续服务；默认静态模式保持原行为，宿主仍由创建它的 Job 所有者控制。
- 新端点在旧管道 I/O 和原 Ticket 全部回收后才释放容量；取消中的运行任务仍占槽，名称和追踪编号不复用。Inspect 只返回代次、容量、监听／连接／待回收与任务计数，不改变输入或记录候选。控制序号／操作／代次严格匹配，超时永久退役控制槽，停止／退出撤销未领取回复，无自动重放。
- 完整 MSVC Release 构建与 7 项相关 CTest 通过（engine_control 0.02、engine_process 0.41、engine_host 0.10、engine_pipe 0.08、engine_host_process 7.50、engine_ipc_benchmark 38.27、engine_host_full 16.32 秒，共 62.72 秒）。新增手动复位事件保留外层等待通知、延迟／损坏回复、关闭任务回收、孤立 Listener 到期、控制超时／崩溃及真实容量 2 下连续 12 轮连接检查。当前注册 45 项，其余未重复。
- [接口、信任与测试边界](ENGINE_CONTROL.md)已整理，进程间基准指纹加入新头文件。动态客户端仍来自同一个测试进程，基准矩阵仍走静态端点，未宣称动态性能、正式多应用发现或 TSF 通过。下一步落实独立可信所有者、单实例竞争／服务发现与诊断入口，再做 TSF 和组字边界换代；未打包、安装或改用户词库，发行保持 alpha.22。

## 2026-11-10 · CN-02／WIN-03b 按页扩展全量结果

- 已归档修改前 d3304c1／修改后 3ce8e68 的第一页逐键报告，各 384 键、66 次选择，均 source_dirty=false；基础／FF14、样本身份和其余记录源码相同，只有 EngineWindows.h 改变。FF14 单工作线程查询合计约 978.67→190.07 ms，扩展合计约 808.93→13.42 ms。最大请求 59.14→13.49 ms，修改后最大值含一次约 8.98 ms 的 SQLite 打开波动。
- 修改后的四客户端全量计时矩阵完成 8 组／2,460 键／438 次选择／2,460 条服务记录，零丢失，与之前相同参数报告的 18 个源码仅 EngineWindows.h 改变。FF14 4 线程 uniform 请求中位／最大约 2.41／66.22→1.23／7.60 ms，总私有量观测峰值约 188.00→179.80 MiB。
- [逐键原始报告与阶段](ENGINE_PERFORMANCE.md#按页扩展的同条件对照)、[共享矩阵及限制](ENGINE_IPC_PERFORMANCE.md#按页扩展后的共享查询)已入库并重建验证汇总。收益来自延后非当前页计算，不是缩减完整候选，深页／冷缓存／实际 TSF 尚未测量。下一步推进共享所有者、可信发现／动态端点和只读诊断；默认线程、超时与 alpha.22 安装不改。

## 2026-11-10 · CN-02／WIN-03b 按页扩展

- 实验 WindowsDictionary 不再每键固定跑满三轮：当前页已有一个有效后继项即可确认 more=true，停止更晚页计算。后续页仍从相同初始列表按原预算追加；转换按原索引编号，正文过滤不改变 ID，未减少可翻页候选或改核心排序。
- 完整 MSVC Release 构建及 5 项相关 CTest 通过（engine_host_process 6.42、engine_benchmark 5.72、engine_ipc_benchmark 31.88、engine_windows_full 18.20、engine_host_full 12.72 秒），当前仍 44 项，其余未重复。新增 45 同音词、简繁全页参考对照、末页后／4095 空页及末页选择重放；原主题、混打、部分消费与锁定检查继续通过。
- [查询规则](ENGINE_WINDOWS.md#查询与提交)及[计时口径](ENGINE_PERFORMANCE.md#记录与解释)已更新。已从干净 d3304c1 留存修改前测量，下一步从干净实现提交运行修改后同条件测量及四客户端对照；安装和发行仍为 alpha.22。

## 2026-11-10 · WIN-03b 完整词库线程与排队结果

- 从干净 9d9f191 先后完成开启／关闭计时两矩阵，各 8 组、2,460 键／438 次选择／32 个客户端；开启计时另有 2,460 条服务记录且零丢失。两份 source_dirty=false、程序／18 个源码／词库身份一致，未与构建或 CTest 并行。
- 四客户端共享 FF14 uniform，2→4 线程的排队中位 3.266→0.008 ms，短词往返中位 6.29→2.17 ms，整体最大 125.35→66.22 ms；观测总私有量采样峰值 184.34→188.00 MiB。mixed 短词中位 6.16→2.10 ms。无计时对照方向一致，不能用单次前后差估计探针成本。
- [两份原始报告、分段表与限制](ENGINE_IPC_PERFORMANCE.md#完整词库线程对照结果)已归档；后续四客户端实验显式用 4 线程，默认 2 和实验超时保持原值。4 线程时执行仍占请求合计约 99.1%，长前缀约 66 ms，继续定位完整扩展成本；共享所有者、发现／动态端点、诊断、换代和 TSF 待做，安装／发行仍为 alpha.22。

## 2026-11-10 · WIN-03b 服务分段与线程对照工具

- 执行器新增默认关闭的五时间点计时，宿主再记录回复入队时间；只记录固定身份元数据，以预分配缓冲保存，收尾写新文件，已有文件拒绝覆盖。未调整默认 2 工作线程或正式运行路径。
- 多进程报告升为格式 2，支持线程列表与 uniform／mixed 长短负载，按宿主／端点／完整请求身份逐键匹配，另报长短分组与排队、执行、发布、邮箱、所属线程及剩余往返时间。剩余部分不是纯 IPC。
- 完整 MSVC Release 构建及 7 项相关 CTest 通过，当前仍注册 44 项，其余未重复。新增矩阵 20 组／2,050 键（1,640 条服务计时）及坏记录拒绝，保留默认 18 组／1,344 键。首次测量回归检测到运行期间编辑源码而拒绝，固定代码重跑后通过；最终相关四项为 0.08／6.47／31.32／14.47 秒。
- [方法、复现命令及限制](ENGINE_IPC_PERFORMANCE.md#可选服务计时与混合负载)已记录。下一步从干净工具提交运行完整词库 2／4 线程对照后归档结果；安装和发行仍为 alpha.22，尚未接 TSF 或修改用户词库。

## 2026-11-10 · WIN-03b 完整词库多进程基线

- 从干净 61a25e3 完成基础／完整 FF14、三模式、1／2／4 独立客户端的三轮固定测量，18 组、4,032 键、693 次选择全部通过。source_dirty=false，两个程序、相关源码与词库前后身份一致，未与构建或 CTest 并行。
- 四客户端 FF14 的共享总私有量采样峰值 183.43 MiB，本地每客户端加载为 693.73 MiB（约降 73.6%）；共享中位／最大请求 7.73／124.69 ms，独占宿主为 2.32／69.91 ms。收益和竞争代价均已记录，不能当作实际 TSF、游戏或旧服务切换数据。
- [原始报告、完整表格与限制](ENGINE_IPC_PERFORMANCE.md)已归档。下一批补排队／执行分段，比较工作线程与长短请求竞争，再决定正式预算；共享所有者、服务发现、诊断、组字换代和 TSF 尚未完成，安装及发行仍为 alpha.22。

## 2026-11-10 · WIN-03b 多进程竞争测量工具

- 新增三模式、1／2／4 个独立客户端的真实逐键测量：本地冻结查询、各自独占宿主、共用一个宿主。记录完整候选回复时延、QPC 工作区间、各轮角色分项与总私有提交量，不相加不同进程的历史峰值。
- EngineProcess 增加一次性 takeEndpoint，交接非继承、仅 query／wait 权限的可信进程对象；测量启动方只显式继承控制句柄与该对象。没有公开 PID 自认证或正式服务发现。原 claim 复用该接口，既有真实查询回归通过。
- 完整 Release 构建和 5 项相关测试通过，当前 44 项，其余未重复。微型基础＋完整 FF14 测量矩阵完成 18 组、1,344 键和 231 次选择，含坏报告拒绝、源数据不变及端点句柄权限检查。
- [测量方法与限制](ENGINE_IPC_PERFORMANCE.md)已记录，完整词库将从干净工具提交运行后归档。该基线不代表已安装 DLL、游戏或 TSF；发行及用户安装仍为 alpha.22。

## 2026-11-10 · WIN-03b 受控启动与退出监督

- 新增可复用 EngineProcess：可信绝对路径、完整 argv 引用、仅继承控制事件、挂起启动加入自有 Job；保留进程对象认证，端点单次尝试，无自动重启或按键重放。原真实宿主测试已使用该模块。
- 启动／停止使用绝对期限及非阻塞 poll，退出优先于 ready，超时终止自有子进程树后等待实际退出；析构不阻塞，以 Job 关闭保护资源。状态只含固定故障分类、Win32／退出码与连接数量，不含输入。
- 完整 MSVC Release 构建和 4 项相关 CTest 通过，当前共注册 43 项，其余既有测试未重复。新故障子进程验证参数／继承隔离、ready 与退出竞争、启动和停止期限、崩溃及 Job 回收；微型／全量基础＋FF14 查询继续通过，并检查真实崩溃状态与端点用尽。
- [接口、等待方式及接手限制](ENGINE_PROCESS.md)已入库。完成启动所有权与基础退出监督，服务发现／动态端点、正式诊断、竞争延迟／总内存、版本与快照换代、TSF 仍待推进；发行及用户安装继续为 alpha.22。

## 2026-11-10 · WIN-03b 真实独立查询宿主

- 新增实验 Host 与 harukoto_engine_host.exe，单进程准备一份固定只读 Windows 词库，1～8 个认证连接共用有界执行器；按原 Ticket 对象路由，取消／断连后任务保留容量至回收。完成事件、管道与最近截止时间合并等待，回复队列含在写帧也有上限。
- Overloaded／Backend 发固定终止错误并保留短暂发送宽限；坏业务／查询超时关闭原连接，其他连接继续工作。不重放按键或选择。新测试发现关闭后遗漏取消回收会延迟客户端断连，已在关闭端点继续回收并纳入等待事件。
- MSVC Release 完整构建通过，10 项相关 CTest 分组通过，最终修改后新增三项约 0.08／6.37／14.42 秒。当前注册 42 项，其余既有测试未重复。真实宿主完成全量基础＋FF14／个人／SQLite 逐键、分页直调对照、部分选择、重复选择、stop／崩溃／坏启动及文件保护释放；测试夹具修正原生排序／context 和微型简繁映射，未改生产校验。
- 详见[启动、容量、验证及接手顺序](ENGINE_HOST.md)。进程间真实查询已接通，正式启动发现／诊断、竞争延迟和总内存对照、快照更新及 TSF 仍待做；未生成安装包或修改用户安装，发行继续为 alpha.22。两个测试连接来自同一客户端测试进程，不替代实际应用验收。

## 2026-11-10 · WIN-03b 认证本地管道

- 新增 EnginePipe.h：随机单连接端点、当前登录 SID 的受保护 DACL、拒绝远程与同名实例；客户端固定可信启动保留的服务进程对象，服务端在读取空 Hello 后核验实际连接令牌。用户／登录 SID、AuthenticationId、session 四项均须相同，不把消息身份数字当认证。
- OVERLAPPED 读写有界分帧，先校验 56 字节头再读取最多 64 KiB 正文；一个读取、待领取帧和写入容量，部分数据不续期。关闭撤销旧帧，CancelIoEx 后保留缓冲／事件／OVERLAPPED 至完成回收，析构仅在 I/O 所属线程等待。
- MSVC Release 新目标构建和 5 项相关 CTest 通过；真实管道覆盖最大帧／碎片／粘连、坏头、超时、双向挂起取消、DACL、匿名拒绝及模拟恢复。独立测试子进程用可信 hProcess 完成握手与最大帧往返，错误及已退出进程对象拒绝。其余既有测试未重复，当前共 39 项；既有 /UNDEBUG 覆盖提示仍在。
- 详见[接口、验证范围与接手顺序](ENGINE_PIPE.md)。仅完成传输层，子进程是消息往返测试，尚未连接真实词库／ServerConnection／Executor；下一步做实验查询宿主与有界事件循环，再测进程间性能并接 TSF。跨用户／登录与实际远程攻击未实测；alpha.22 发行包、用户安装和词库未改变。

## 2026-11-10 · WIN-03b 读音索引逐键复测

- 干净 42458d7 完成同样的 384 个逐键样本与 66 次选择验证；与干净 cadd221 基线核对基础四文件、FF14、驱动／脚本和样本身份一致，记录源码仅 EngineWindows.h 改变，程序哈希相应变化。两份报告均已归档，未与构建／测试并行。
- 本机固定样本中 FF14 后台请求中位 7.93→2.36 ms、最大 66.82→56.89 ms，查询合计下降约 41.5%；首轮合并 182.29→18.62 ms，扩展 1289.54→781.19 ms。只是三轮热缓存附近的实验查询，不能当作实际游戏／TSF 延迟，长前缀仍有开销。详见[前后原始报告与限制](ENGINE_PERFORMANCE.md)。
- 性能测量和第一项索引优化完成，接下来做当前用户认证管道及独立引擎进程；SQLite 连接和核心会话仍每次独立创建，索引内存成本／竞争延迟／真实应用仍待测。最新相关三项测试通过，其余既有测试本批未重复运行；alpha.22 安装和发行包不变。

## 2026-11-10 · WIN-03b 冻结读音索引

- 实验 WindowsDictionary 按原词表位置建立不可变读音索引，每请求只取完整触发和合法混打子串对应词条，扩展复用，避免多轮扫描整张主题表。保留同权重／首匹配顺序，不改正式合并、搜索预算、SQLite 连接或核心重建。
- 完整 MSVC Release 构建通过；相关执行器（0.02 秒）、测量工具（5.61 秒）和扩大的全量候选对照（16.36 秒）通过。新增受控 romaji 验证中文前／后／中间混打及同权重首匹配，逐页对照原完整词表的 ID／正文／消费／注释；含英语子串排除、声调／分隔符、简繁和中文锁定。测试词先放错容器，被既有校验拒绝；改用三字段基础 romaji 行后通过，未放宽加载规则。
- 接着从干净提交复测完整逐键基准，暂不宣称具体提速。索引增加准备内存，未测总内存收益；正式 TSF、发行程序和本机安装仍为 alpha.22，认证管道及引擎进程继续待做。详见[合并与索引说明](ENGINE_WINDOWS.md)、[性能测量](ENGINE_PERFORMANCE.md)。

## 2026-11-10 · WIN-03b 全量逐键基线

- 从干净 cadd221 完成基础／FF14、direct／单工作线程共 384 个逐键样本和 66 次实际选择检查；哈希复核通过，原始报告纳入版本管理。固定测试输入，无用户词库或安装变更。
- FF14 后台请求中位／最大 7.93／66.82 ms，查询合计 1621.67 ms，其中扩展约 80%、SQLite 打开约 3.5%；这是服务端查询往返，不是 TSF 到屏幕延迟。基础后台请求中位／最大 2.03／13.55 ms。仅三轮，热缓存与其他系统负载未受控。
- 详见[原始数据与解释](ENGINE_PERFORMANCE.md)。下一步按读音建立不可变索引，减少各扩展轮扫描整张主题词表，保留正式合并顺序与消费；不先引入 SQLite 连接池。认证管道／进程／TSF 继续待做。

## 2026-11-10 · WIN-03b 逐键分阶段测量工具

- 新增真实逐键测量程序和临时快照驱动脚本，区分核心重建、SQLite 打开、首轮合并、候选扩展、转换／清理与服务端请求往返。direct 与单工作线程模式均对每个前缀运行真实查询，末尾验证实际选词、部分消耗和重复选择；不将阶段耗时当屏幕延迟。
- MSVC Release 完整构建通过；相关 engine_executor／engine_windows_full 通过（0.02／14.16 秒），新增 engine_benchmark 修复微型测试容器 LF 写入后通过（6.57 秒），四模式合计 128 次逐键查询、22 次选词验证。未重复运行其余既有测试，当前 CTest 共 38 项。
- 详见[测量方法](ENGINE_PERFORMANCE.md)。工具准备完成，完整词库正式报告接着从干净提交生成；尚未优化每次重建或引入 SQLite 连接复用。alpha.22 发行和用户安装／词库保持原状。

## 2026-11-10 · WIN-03b 有界后台执行与错误收尾

- 新增 Executor：固定 1～16 个工作线程、1～256 个任务槽，排队／运行／未领取结果共用容量；工作线程只读任务和快照，所属线程领取后再修改原连接。支持重复提交检查、排队取消、运行结果撤销、计算前后超时、停止和线程回收。
- 新增固定 4 字节 Error 载荷。精确当前任务的查询异常／坏候选通过 fail 终止连接并清掉 pending；队列满由所属线程报 Overloaded。旧任务／其他连接／复制品失败拒绝，客户端先验证完整错误再断开；不自动重放选择，也不改已确认的本地投递队列。
- MSVC Release 完整构建通过，37 项 CTest 分两组全部通过：相关 5 项 12.83 秒（其中全量合并 12.69 秒），其余 32 项 113.93 秒。新增 engine_executor 确定性并发回归，并在真实全量 Windows 测试中加入 SQLite 独有补词、日语双候选、FF14 繁体名称后台查询、缺失快照错误，以及真实查询停止后的文件保护释放。原有测试 /UNDEBUG 覆盖警告仍在，未新增构建错误。
- 详见[执行器与错误边界](ENGINE_EXECUTOR.md)。尚未量化性能／内存收益；每次查询仍独立重建核心及打开 SQLite，后台准备快照、认证管道、独立引擎进程、TSF 和自动换代继续待做。取消不能强杀正在运行的核心调用，join 须等后端返回；发行基线 alpha.22，本批不生成安装包或修改用户安装／词库。

## 2026-11-10 · WIN-03b Windows 正式候选合并与快照归属

- 新增 WindowsDictionary，将正式 windowsCandidates／appendCandidateExpansion 接到实验服务任务，读取经过完整校验和文件保护的基础快照，准备基础日语／活用词条、个人表和 FF14 主题合并；四文件哈希及实际合并词条参与完整代次标识。
- 连接与 Work 固定持有同一个只读词库对象，跨对象查询拒绝；每次查询独立核心会话，并在工作线程创建／使用／关闭自己的 SQLite 连接，避免共享可变查询语句。部分选择保留中文锁定；最后任务引用释放后旧快照文件保护才解除。
- MSVC Release 完整构建及 36 项 CTest 通过（123.40 秒）。新增全量合并回归（12.55 秒）覆盖 SQLite 独有补词、日语表记／假名分选和注释、英语触发、FF14 简繁名称提交与重复选择、全页排序／消费对照、四线程 24 次完整查询、个人表两代并存、坏准备沿用旧对象、迟到结果拒绝及文件保护释放。
- 测试使用临时快照／FF14 仓库，初次准备因测试 INSERT 未填实际 SQLite 的 phonetic 必填列而失败，补齐测试字段后通过。多字输入中间步骤使用空候选完成，仅最终输入调用完整合并，不据此宣称逐键速度或真实应用投递通过。已有 caret 隐藏成员警告仍在。
- 详见[合并适配器](ENGINE_WINDOWS.md)。下一步实现有界后台执行器与错误结果处理，再测重建／SQLite 连接开销；认证管道、独立进程、自动快照切换、完整 F6 交互与 TSF 接入仍待做。源代码已接真实查询，发行仍为 alpha.22，不重新打包或安装。

## 2026-11-10 · WIN-03b 真实中文核心查询

- 新增 CoreDictionary 适配器，共享一份原生只读拼音词库，每次查询独立创建核心会话，真实服务任务支持简繁、分页和部分选择后的上下文恢复。当前每次逐字重建且运行三轮有界扩展，是正确性原型，不代表低延迟或完整 Windows 候选路径。
- 核心新增只读 mpn_candidate_literal_tail API，适配器去掉候选显示用的未转换尾部；测试逐项对照真实 mpn_choose 的提交与剩余拼音，避免把显示尾部错误提交。现有选词算法未改。
- 小词库与全量基础词库检查通过：双连接“中国／中國”隔离、nihaoz 部分选择保留 z、取消后真实结果拒绝、声调／非法输入、45 同音词分页与稳定 ID。四个真实线程共享同一词库各执行 20 轮双查询（共 160 次），未观察到结果串扰；没有测内存收益或端到端延迟。
- 完整 MSVC Release 构建和 35 项 CTest 全通过（105.88 秒），包含既有基础／日语／FF14／热更新／失焦回归。首次测试目标编译缺少 sstream 且函数名与 MSVC concurrency 宏冲突，补头文件并重命名后构建通过；既有 caret 隐藏成员警告仍在。
- 详见[核心适配器](ENGINE_CORE.md)。下一步接 SQLite／日语／FF14 正式候选合并与快照归属，随后优化重复解码；独立进程、认证管道、全局调度及 TSF 接入未实现。源代码增加只读接口但发行保持 alpha.22，未重新打包或安装；旧安装包没有重新生成。

## 2026-11-10 · WIN-03b 服务端会话调度原型

- 新增 ServerConnection，按连接对象隔离同编号会话，保存独立输入／组字／候选并分发不可变任务。后台完成必须匹配当前任务对象，取消、关闭、超时或断连后的旧结果不能发布。没有真实核心、线程池、认证传输或独立服务进程。
- 在途重复请求不重复派发；相同已完成请求返回缓存原回复，不再消费拼音。缓存最多 64 项，淘汰／取消后旧请求拒绝；每连接最多 64 会话与 64 个仍有引用的任务，取消后被执行器保留的任务仍计数。部分选择保留精确后缀，Refresh 不重放选择。
- MSVC Release 新目标构建通过，engine_server／engine_messages／engine_protocol／tsf_focus_cancel／pending_delivery 五项 CTest 全通过（最终 0.16 秒）。覆盖两个连接相同身份数字、多会话乱序、100 轮取消后旧结果晚到、去重／缓存淘汰、错误结果／容量边界、超时／重建和客户端到服务端及原投递队列组合。测试用固定候选，不等于真实解码或跨应用验收；未重跑完整套件。
- 详细边界见[服务端原型](ENGINE_SERVER.md)。下一步接真实核心适配器，验证只读词库共享、可变状态隔离和实际候选；完整按键／上下文／错误载荷、全局调度与认证管道、进程生命周期和 TSF 接入仍待实现。跨断连的未确认选词恢复尚无保证。
- 运行时代码与发行基线仍为 alpha.22，不重新打包或安装；本批只增加测试程序，未重新生成旧安装包。

## 2026-11-10 · WIN-03b 候选消息与构建清理

- 新增 EngineMessages.h：实验按键／候选／选择／提交载荷，严格长度、UTF-8、候选 ID 和消费范围校验；候选使用回复请求号作为版本，旧列表选择拒绝。表记／假名分别可选，部分提交精确保留剩余拼音，详见[业务消息](ENGINE_MESSAGES.md)。
- CandidateSnapshot 与 ClientGate 分层组合，先校验业务再消费请求、确认并交给原上下文；坏回复不修改候选，重复提交拒绝。仍未接真实引擎、服务端、认证管道或 TSF，下一步做服务端会话调度、取消和执行去重。
- MSVC Release 新目标构建通过，消息、协议、失焦、队列、清理脚本五项相关 CTest 通过。未重跑完整套件；alpha.22 发行程序未改，不重新打包或安装。
- 清理 54 个旧文件／目录目标，共 5,227,347,151 字节（约 4.87 GiB）；alpha.21／22、FF14 0.3.0 及校验和文件前后哈希一致。保留当前 MSVC／ime／tools、词库源数据、评测和本机安装。新增默认预览的 clean-build.ps1，复跑无待清理项；回归覆盖保留两版／未知文件、发现受版本管理文件时先拒绝及重复执行。

## 2026-11-10 · WIN-03b 实验协议与客户端会话隔离

- 新增 EngineProtocol.h：56 字节固定小端头、64 KiB 载荷上限、精确版本／消息类型／身份校验和分帧解码。ClientGate 按连接、引擎、会话、组字、请求五项匹配响应，单会话一个普通在途请求；重复／过期回复拒绝。
- 支持握手、建立中取消／关闭、反复失焦取消、请求超时与重连；会话／请求编号不复用。超时和匹配的 Error 清除整条连接状态，不自动重放可能已经执行的选词。已确认的本地待上屏文字仍归原 PendingDelivery 队列。
- MSVC Release 新测试目标构建通过；engine_protocol、tsf_focus_cancel、pending_delivery 三项 CTest 通过（最终 0.31 秒）。包含 2,000 组确定性随机编码往返、截断／粘连／非法头／超限、双会话乱序、重复回复、旧连接回复、握手及请求超时、会话上限与本地队列归属。未重跑完整套件，现有运行时代码未改。
- 详细格式、状态与接手顺序见[引擎协议](ENGINE_PROTOCOL.md)。仅客户端协议准备，尚无类型化候选／提交载荷、服务端会话执行、认证管道、引擎进程或 TSF 接入；不能据此宣称跨应用已隔离、支持引擎热更新或减少内存。下一批先完成类型化载荷和过期候选选择检查，再接服务端与真实传输。
- 发行基线与安装包仍为 alpha.22，本批不重新打包或安装，未改用户词库。

## 2026-11-10 · CN-02／WIN-03 并发词库更新负载

- benchmark-reload.py 新增 1～8 个独立服务测试进程共享临时仓库，同时触发更新；记录各进程耗时、整组完成时间和逐轮合计私有内存采样。发送时间差、采样轮数和遍历耗时均保留，不将独立峰值相加，不代替真实应用验收。
- MSVC 构建通过，新增并发基准及两项现有服务重载 CTest 共三项通过（7.69 秒）；三进程回归验证不同词条更新／回退、逐进程选词、采样失败报错与句柄释放。既有完整 28 项结果见 alpha.22，本批没有重跑完整套件。
- 从干净提交 80848c2 顺序测 1／2／4 进程，纯基础／FF14 各三轮，共 168 个操作样本、72 组；版本、变化状态、逐进程“你好”提交和原始报告不变量检查通过。测试程序／脚本／词库哈希一致，详见[性能测量](RELOAD_PERFORMANCE.md)。
- 四进程 FF14 更新每进程前台发布中位／最大 0.100／0.143 ms，整组完成中位 2333.0 ms（含后台与清理等待）；合计私有内存采样峰值 1398.3 MiB、清理后三轮中位 626.1 MiB。不是端到端输入延迟，也不是物理内存或严格瞬时峰值。
- WIN-03b 下一步优先验证单用户引擎共享只读词库与会话隔离，先落实协议版本、请求／组字代次、旧响应丢弃及断连行为。冷缓存、大个人表、更多上下文和实际应用投递仍待测。运行时代码与版本未改，沿用 alpha.22；不重新打包或安装，不修改用户词库。

## 2026-11-10 · alpha.22 / WIN-03、CN-02 热更新前台停顿优化

- 已从干净提交 14d801e 构建 alpha.22 EXE／ZIP；包内 DLL／渲染器、四份基础词库、诊断工具、原生检查器及性能文档／原始报告共 17 份文件与构建或源码一致，ZIP／EXE SHA256 通过。内置 PS7 解包、安装／卸载预检和安装器渲染通过，机器注册未改变。本机未安装，未发布 Gitea Release；此条为打包后记录。
- 新增 benchmark-reload.py，真实服务测量初次采用／更新／回退／无变化的后台派发、准备等待、前台发布和私有内存；纯基础与 FF14 全量各三轮。报告记录数据／测试程序哈希及源码工作区状态，详见[测量说明](RELOAD_PERFORMANCE.md)。不修改用户安装或词库。
- 定位到前台释放旧基础快照、安装原生词库及 FF14 合并表的成本，改为独立后台清理。任务持有 DLL 引用，不访问 Service／COM；启动前转移最后一个前台引用，避免快线程竞态。创建失败保留旧对象并后续重试；没有变化时不增加清理任务。
- MSVC 构建及完整 28 项 CTest 通过（103.46 秒）；后续补最后引用交接与 64 轮清理线程归属检查，五项相关回归通过（14.65 秒）。已覆盖旧代次文件锁释放、新代次仍受保护、上屏队列身份、坏更新恢复、失焦及回退。
- 最终三轮对照：基础更新前台发布中位 27.799→0.072 ms，FF14 49.012→0.074 ms；这不是端到端延迟。补齐旧 LLVM-MinGW 构建脚本的 bcrypt 链接依赖，仅验证脚本语法，发行构建仍使用 MSVC。
- 前后台耗时与采样内存保留原始 JSON；测试为本机缓存通常已热、两个上下文，不代表真实 TSF 上屏／渲染延迟或多应用负载。后台加载时间和新旧数据共存内存未被此次优化消除。词库内容与中文排序未改，FF14 保持 0.3.0。
- 下一步补冷缓存／多宿主负载及跨应用投递验收，独立引擎仍待实现。安装包在源码提交后构建，不自动安装。

## 2026-11-10 · alpha.21 / WIN-01、WIN-03 运行时版本诊断

- 已从干净提交 d8e353e 构建 alpha.21 EXE／ZIP；包内 DLL／渲染器、诊断入口／脚本／读取器和相关文档一致性、ZIP／EXE SHA256 通过。内置 PS7 解包、安装／卸载预检和安装器渲染通过，机器注册未改变。本机未安装，未发布 Gitea Release。

- 输入服务通过本地共享内存报告每个实例的服务版本、实际基础版本／标识、主题标识、待采用基础标识和加载阶段。按进程 ID＋创建时间命名，每进程 64 个槽位，线程／槽位区分实例；关闭释放，双读序号拒绝并发写入中的记录。诊断不记录输入文本、不逐键写磁盘，失败不阻止输入。
- 只读脚本区分磁盘发布与实例实际采用，支持等待组字边界、失败沿用旧版和未知状态。JSON 升为格式 2；没有旧版报告、访问受限、容量超限或损坏时不确认已更新。观察时间是最近状态变更，不是心跳，空闲时加载阶段可能滞后。安装目录基础哈希、个人指纹和主题磁盘自动对比尚未补齐。
- MSVC 构建与 28 项 CTest 通过（102.42 秒），新增原生／PS7 多实例报告、关闭／复用、并发读写与坏记录检查；生产服务测试确认待切换标识与实际采用代次不同、失败状态保留。补容量上限及发布指针类型检查后，两项诊断复测通过（1.60 秒）。
- 本机只读报告成功生成格式 2，检查 402 个进程、发现 7 个加载晴词 DLL 的进程；本机详细信息仅留在 .build，不提交仓库，不据此宣称这些应用已加载 alpha.21。
- 发行包加入 Diagnose.cmd、诊断脚本和 C# 读取器；暂无开始菜单入口。首次升级新报告功能仍需重开应用。跨应用实际诊断、更新切换延迟／内存和独立引擎仍待推进。安装包在源码提交后构建，不自动安装。

## 2026-11-10 · alpha.20 / WIN-03a 通用基础词库热更新

- 已从干净提交 b2e4c89 构建 alpha.20 EXE／ZIP；包内 DLL、渲染器、四份词库、发布工具、原生检查器和指南与构建一致，ZIP／EXE SHA256 通过。内置 PS7 解包及安装／卸载预检、安装器渲染通过，机器注册未改变。本机未安装，未发布 Gitea Release。

- 基础四文件快照接入通用 TSF 后台任务，与主题／个人表合并后统一发布；用户仓库为 %LOCALAPPDATA%/Meowpuut/BaseDictionaries，各应用实例独立按需检查和切换，没有 QQ 专用分支。安装启动先使用自带基础词库，后续加载用户版本。
- 组字、活用及活动／挂起会话的引擎未取走提交会推迟切换。全部替代会话准备成功后交换，保留繁简与片段上下文；字段上屏队列和已选文字保持原归属。释放最初安装词库与旧查询连接。坏基础／主题／个人表保留整套旧结果并重试，回退通过版本指针生效。
- 完整 MSVC 构建及 27 项 CTest 通过（95.51 秒），包括两个独立进程运行生产服务的组字隔离、已选文字、挂起字段、队列身份、失焦、合并、失败恢复、回退和加载中销毁；全量基础词库服务切换／回退测试通过（5.66 秒，为整项运行时间，不是单键延迟）。新增释放方法最初与 SQLite 函数指针同名导致构建失败，重命名后构建通过。
- 管理工具输出改为 runtime_activation_confirmed=false，准确区分发布和各应用实际采用；MSVC 包附带发布脚本、原生检查器及指南。基础更新暂为命令行，安装器不自动写入用户基础仓库。已发布用户版本会继续覆盖安装包基础词库，见指南。
- 首次升级 alpha.20 仍须重开各输入应用；之后兼容词库更新可在新组字生效。记事本／浏览器／VS Code／QQ／FF14／远程实机投递、切换耗时／内存峰值、生效版本诊断尚待完成，独立引擎代码更新仍待实现。安装包在源码提交后构建，本机不自动安装。

## 2026-11-10 · WIN-03a 基础快照原生加载器

- 新增 Snapshot 对象统一持有原生拼音词库、SQLite 连接、日语表、活用表及只读文件保护句柄。校验清单、SHA256 和内容标识后调用原生解析器，SQLite 需通过 schema／integrity_check；拒绝 WAL 与边车文件，不恢复日志。失败不交出半成品对象。
- 新增独立 harukoto_base_dictionary_check.exe，以及 Python verify 的 --native-loader 入口。检查 current 指针对应代次并在加载结束复查，输出 native_loaded=true／runtime_activated=false。不注册输入法、不修改当前指针、不启动后台监控。Python 与 C++ 同步限制版本长度、拒绝重复 JSON 字段。
- MSVC 构建及 25 项 CTest 全通过（92.51 秒）。补充占用／缺失／同大小损坏测试和活用表大小限制后重新构建，三项快照 CTest 通过（9.86 秒）；包括 10 项存储测试、10 项原生交叉测试和全量生产词库测试。覆盖哈希正确但内容损坏、两代词库并存、旧组字及已选文字保持、文件保护与释放、简繁候选及活用查询。
- 本批完成原生加载准备，尚未接入 TSF 后台重载与会话切换，也未合并主题／个人表到 Snapshot。下一批需同时处理活动与挂起会话、待投递文字和合并代次，再做 QQ 实测。发行基线保持 alpha.19；无新安装包，本机安装未改变。

## 2026-11-10 · WIN-03a 基础词库版本快照工具

- 新增开发命令 create／verify／stage／rollback／status，将原生拼音、SQLite、基础日语及活用表四文件打成完整快照。清单记录格式／兼容协议／语义版本及大小、SHA256；复制后复验、独占写锁、原子状态指针，保留上一版本。重复暂存不破坏回退位置。
- 存储层只校验容器与完整性；新增生产集成测试另行验证 SQLite integrity_check、现有原生加载与选词、简繁候选，以及真实快照暂存／回退／拒绝坏更新。操作和故障恢复见[快照文档](BASE_DICTIONARY_SNAPSHOTS.md)。
- MSVC 构建通过；全套 24 项 CTest 初跑 23 项通过，新生产项因版本校验误拒连字符失败。修正语义版本规则并补测试；复跑发现测试数据库连接未关闭导致清理失败，改为显式关闭。最终两项新 CTest 均通过（5.89 秒），存储项含 10 个测试；现有 22 项均在完整运行通过（总计 83.96 秒）。
- 当前只完成开发存储工具，安装器和 TSF 不读取其状态指针，没有基础中文运行时热更新；不改中文排序、FF14 内容或本机安装，不重新打包，发行基线仍为 alpha.19。下一步为基础原生加载器和会话／SQLite／合并词表统一切换。

## 2026-11-10 · alpha.19 / WIN-03a 个人触发词表热更新

- `japanese-custom.tsv` 接入主题重载的后台工作，按实际字节变化识别更新；同大小同时间戳也能识别，相同内容复用已解析快照。只在空组字的实际按键前发布，正在输入和翻页继续用原候选。个人表用于拼音／英语触发日语，不是中文自动学习。
- 稳定文件句柄读取、32 MiB 上限、UTF-8／假名／六列完整校验；拒绝坏行、文件占用或意外丢失，暂缓合并结果并保留当前进程旧词库。启动时没有个人文件属正常；内存旧版不等于持久备份。
- 导入工具继续原子整表替换，新增显式 `--clear` 发布空个人表，清空不影响基础或主题词；`--check` 不写文件。恢复旧词条须重新导入保留的源词表。
- 完整 22 项 CTest 全通过（86.56 秒），补 CRLF／初始快照检查后重新构建并复测失焦、重载和 FF14 全量三项通过（7.20 秒）；Python 7 项导入测试通过。覆盖同大小同时间变更、占用、超限、坏行恢复、空表、主题共存／去重，以及真实 Python 导出个人表在 C++ 简繁候选中可见。
- 首次升级到 alpha.19 仍需重开宿主；个人与主题更新之后可在新组字生效。QQ／游戏实机热更新、多应用同时加载大个人表的负载未验收。基础中文／活用表版本化及独立引擎继续待实现。本批不改中文排序或 FF14 内容。
- 安装包在源码提交后构建，本机不自动安装。

- 已从干净提交 abcb561 构建 alpha.19 EXE／ZIP；包内 DLL、渲染器、词库、个人导入工具及文档一致性、SHA256、内置 PS7 解包与安装／卸载预检通过，机器注册未改变。本机未安装，未发布 Gitea Release。

## 2026-11-10 · alpha.18 / WIN-03a 主题快照热更新首版

- 新增 ThemeReload：实际按键时至多每秒触发后台检查，仅 CURRENT 变化时加载／校验／合并 enabled.tsv；后台完成后在空组字的下一实际按键前交换词表。当前组字、翻页、部分选词和活用沿用旧候选。查询按键是否接收不触发重载。
- 区分有效空快照与错误，关闭全部主题会移除主题词，损坏／缺失快照保留旧表并提示；恢复有效代次后重试。后台线程只持有独立数据及模块引用，不使用服务指针或 COM，服务退出不等待磁盘读取。
- 完整 MSVC 构建与 22 项 CTest 全通过（85.91 秒）。新增启停、坏快照、回退、去重、组字隔离和加载中销毁测试；真实管理器生成 113,463 行 FF14 快照，在同一进程／会话启用、关闭、回退及“特级恢复药”选词通过。三次切换与候选检查共 797.51 ms 单次观测，含后台读取等待，不是按键延迟。
- 管理器和操作指南已说明生效时机：若变更后第一段尚未更新，完成或取消再输入。首次安装 alpha.18 需要重开宿主加载新 DLL，之后主题变更支持新组字生效；不是引擎 DLL 热替换。
- WIN-03a 的基础词库／旧个人 TSV／活用表版本化、WIN-03b 独立引擎和 WIN-03c 更新协调仍待实现；QQ／游戏实机验收未完成。本批不改中文排序或 FF14 0.3.0 内容，不重新跑已覆盖的质量基线以充当新准确率。
- 安装包在源码提交后构建，本机不自动安装。

- 已从干净提交 64c6ada 构建 alpha.18 EXE／ZIP；包内 DLL、渲染器、词库、管理器及热更新文档一致性、SHA256、内置 PS7 解包与安装／卸载预检通过，机器注册未改变。本机未安装，未发布 Gitea Release。

## 2026-11-10 · alpha.17 / QQ 候选、分页与失焦

- QQ／QQKit 模块路径文件哈希与 alpha.16 注册 DLL 一致，同版探针复现截图。主题名称已有同名组词候选时不再跳过提权；特级恢复药从第 13 项变首选，简繁可正确提交，普通完整词典词仍优先于主题扩写。
- 末页 PageDown 按需追加候选，页码 + 表示仍可继续搜索；原候选顺序／消耗范围／索引保持。完整词典匹配扩展到全部，组词按 32／64／128 路径分三轮，超过 32 字母组合保留原预算，不宣称无限。特级恢复药 19→139 项，三轮合并约 55–57 ms 单次观测。
- TSF 增加线程焦点通知，输入法失前台、线程失焦、文档切换和上下文切换取消未选拼音、候选、活用及片段上下文。已选待投递文字保留于原字段队列，不迁移到其他输入框。新增测试直接调用生产 TSF 回调验证清空、返回重新输入与已选文字保留；不替代 QQ 的真实焦点通知验收。
- 固定评测 44 条基础保持 42／44，FF14 开启全部 48 条变为 46／48；仅恢复药从第 8 到首选，其他排名不变。词库校验通过。首次焦点测试因测试词表未按读音排序而失败，修正测试数据后复测通过。
- 包含开始菜单“晴词 Harukoto”迁移；旧 Actions 工作流已移除并推送 9273d1b。热更新拆分为 WIN-03a/b/c，见 HOT_RELOAD.md，目前是计划，核心仍在宿主 DLL 中。
- 安装包在验证与源码提交后构建；QQ／游戏实机失焦与翻页、升级仍待验收，不自动安装。
- 完整 MSVC 构建及 20 项 CTest 全通过（71.96 秒），包含生产 TSF 回调、400 条同读音候选展开／稳定索引／末项提交、真实 FF14 名称提权和扩展、已选文字队列回归；词库校验与差异空白检查通过。

- 已从干净提交 bba98a8 构建 alpha.17 EXE／ZIP；包内 DLL、渲染器、词库、安装脚本及评测报告一致性和 SHA256 通过；内置 PS7 解包、安装／卸载预检、安装器渲染通过，机器注册未改变。未自动安装，未发布 Gitea Release。

## 2026-11-10 · WIN-01 开始菜单品牌清理（待下一安装包）

- 安装时开始菜单文件夹改为“晴词 Harukoto”，内含“晴词候选窗设置”和“晴词主题词库”。先创建新入口，再移除旧 Meowpuut 文件夹中三个已知快捷方式；仅在旧目录为空时删除目录，保留用户其他文件。卸载同时清理新旧入口及空目录。
- 安装／卸载脚本 PowerShell 语法检查与差异空白检查通过。本批未重新打包，已有 alpha.16 安装包仍含旧目录逻辑，下个安装包应包含本修复。
- 当前机器快捷方式迁移被自动审批策略阻止，未执行；不能将源码修复当作本机菜单已改名。

## 2026-11-10 · alpha.16 / CN-01 儿化与未完成拼音

- erhua.tsv 收录 8 对儿化白名单，生成末尾 r 别名，完整 er 读音继续保留；同别名权重合并取最大原权重。不为女儿等独立 er 音节生成缩写。wohuir 选择“我会”后保留 r，不自动造“我会儿”。基础中文读音记录仍为 410,546，新增的是原生拼音别名。
- 32 字母内用户可见组词扩大路径覆盖；未完成尾音时常用完整词典前缀优先，liulanq／wenjianj 首选浏览／文件。内部探测及更长句保持原路径边界。
- 固定 44 条回归四组均首选 42、前 7 项 44、缺失 0，较 alpha.15 的 38／42／2 改善；四条排名改善，其余不变。含主题 48 条为 45／47，语料已参与调优，不是独立语言准确率。报告保留哈希与逐条对照。
- 本轮 19 项 CTest 全通过（81.41 秒），词库合并／变体／多音字／原生索引校验通过。新增 11 条简繁首选／提交案例、短语非首选提交、后续页部分选词与白名单／完整读音／权重校验。
- 共同样本 P95：简繁关闭约 1.82 ms，开启约 4.41 ms；比 alpha.15 观测值上升，评测与测试并行，未隔离负载，不归因于单一因素。下一步同条件比较搜索开销。
- 未解决：队友到齐仍第 3；队友到齐了／等队友到齐了首选仍不自然；登录／登陆、公司／公视偏好保留。下一轮查上下文与带后缀搜索，不以整句测试通过宣称组词全面修复。
- 本批准备 alpha.16 安装包，FF14 保持 0.3.0；实机升级、跨应用和游戏上屏仍待验收。打包结果在构建后另记。

- 已从干净提交 7578f77 构建 alpha.16 EXE／ZIP；包内 DLL、渲染器、原生词库及报告一致性、SHA256、内置 PS7 解包和安装／卸载预检通过，机器注册未改变。本机未安装，未发布 Gitea Release。此项为打包后记录。

## 2026-11-10 · alpha.15 / CN-01 中文短搭配与主题排序

- context.tsv 新增 12 个自编短搭配，只给相邻片段提供排序依据，不新增评测整句词条、不提高全部字词权重，基础中文仍为 410,546 条读音／410,379 条原生记录。
- “我们明天见”“请帮我看一下”“组队时间”“我断线了”“我们开始吧”“这个怎么打”恢复首选；另测不同主语／时段的句式，简繁首选及选择提交通过。
- 主题中文名不再抢在完整词典读音前；当主题扩写包含原完整中文路径时保留原表达在前。“辛苦了”“早上好”在 FF14 开启后仍首选，日文／假名及主题扩写仍可选；乌尔达哈继续优于弱拼接。
- 固定 v1 评测：44 条基础样本简繁、FF14 开关均首选 38／44、前 7 项 42／44；此前关闭时 32／40，开启时 30／40。全量开启 48 条为首选 41、前 7 项 45。语料／预期未变，逐条比较未发现排名下降；该集合现已参与调优，不再称独立盲测，见中文评测说明。
- 本机共同语料逐键 P95：简／繁关闭 1.71／1.68 ms，开启 3.76／3.61 ms；仅核心＋候选合并，历史运行与本轮负载不同，不归因为性能改善或退化。
- 完整 MSVC 构建与 19 项 CTest 通过（92.62 秒），新增测试后重跑候选及全量 FF14 两项通过；词库合并／多音字／变体／索引校验通过。覆盖 11 条简繁组合句首选与提交、主题开关首选／原生索引及全量日语候选回归。
- 已知未解决：儿化 xianxiuxiyihuir、等队友到齐的路径缺失、部分拼音 liulanq／wenjianj、公司与公视等偏好；中文候选没有达到完整语义理解。本机未自动安装，安装包在提交后构建。
- 已从干净提交 c9bb57e 构建 alpha.15 EXE（约 164.5 MiB）／ZIP；渲染器、DLL、原生词库和评测报告的包内一致性、SHA256、内置 PS7 解包与安装／卸载预检通过，机器注册未改。未发布 Gitea Release。

## 2026-11-10 · alpha.14 / UI-03 边界与键盘检查首轮

- 候选适配同时限制宽高，从原始逻辑宽度重新计算，修复极窄工作区最小候选宽度越界及连续适配再次缩小；离开小工作区后恢复自然尺寸，不修改配置。
- 首次显示前改用 HWND 查找渲染源，避免未连接视觉树时丢失 DPI 变换；定位计算抽取共享方法，保留负坐标屏幕与任务栏工作区。未更改进程 DPI 感知策略，不能据此宣称混合 DPI 已支持完善。
- 预览可 Tab 聚焦，方向键／Home／End／数字／小键盘选择可见示例；隐藏候选不会被选中，Enter／Space 不向外触发保存。真实候选仍不抢焦点、不改变按键或上屏协议。高对比模式下宽度拖动条使用系统色，页码说明支持换行。
- test-preview.ps1 全部通过：既有 9＋108＋96 组检查；新增 192 组模拟 100%／125%／150%／200% 变换与工作区／方向／字号／示例组合，每组 6 个定位锚点，检查重复适配、大工作区恢复、全部候选边界和序号。四档离屏图已生成，未当作视觉验收。
- 本机隐藏 HWND 检查选择了两个实际桌面工作区，渲染变换 1／1；桌面枚举为两块 1920×1080，工作区 1920×1032。没有改系统缩放、显示布局、注册或输入设置，也未实际显示候选测试窗口。键盘路由和候选过滤检查通过。
- UI-03 仍部分完成：混合 DPI 真正跨屏、切换缩放后重启、远程双端可见性、系统高对比可读性、真实输入上屏待执行，见 Windows 验收表。本批未改词库／排序；安装包于源码提交后构建，本机未自动安装。
- 已从干净提交 fdc8077 构建 alpha.14 EXE（约 164.5 MiB）／ZIP；渲染器、DLL 包内一致性与 SHA256 通过，内置 PS7 解包、安装／卸载预检及安装器渲染通过，机器注册未改。未发布 Gitea Release。

## 2026-11-10 · alpha.13 / UI-02 布局编辑与保存

- 新增横向换行／纵向列表、手动或自动读音字号、候选横纵间距、候选左右／上下留白、窗口内边距；右侧拖动条与宽度滑块联动，拖动条也支持左右方向键。
- 紧凑／标准／宽松预设仅改布局草稿，应用时恢复标准字号、宽度和横向排列；默认外观仍为 18／672，简繁与词库保持。
- LayoutOptions 统一校验、旧 FontSize／Width 迁移、新布局序列化。布局通过单个 LayoutV1 注册表字符串整体发布；失败保留之前布局。简繁仍使用既有独立值，布局写入失败时尝试恢复此次简繁变更，恢复失败明确报错，不宣称跨键事务。
- 纵向高列表超过工作区高度时临时整体缩放，不修改用户布局参数，数字序号保持。极端参数下文字可能变小，真实显示可读性仍待 UI-03。
- 自动检查通过：既有 9＋108 组布局、新增 96 组预设／方向／极值布局对照，短工作区高度限制、拖动同步和边界、预设／默认／取消隔离。独立临时注册表覆盖保存重读、旧配置、损坏类型／未来版本回退、拒绝写入保留旧布局和简繁补偿；PowerShell 7 外观桥接通过。
- 命令行外观工具沿用同一配置实现，改字号／宽度保留其他布局字段；不再写被新布局覆盖的旧值。本批未修改词库和解码。
- 已从干净提交 b8a7bae 构建 alpha.13 EXE（约 164.5 MiB）／ZIP，包内渲染器、DLL 和外观命令行源码与构建一致，SHA256 通过。内置 PS7 解包、安装／卸载预检和安装器渲染通过，机器注册未改变；本机未自动安装，未发布 Gitea Release。

## 2026-11-10 · alpha.12 / UI-01 真实候选布局预览

- 设置页新增左右参数／预览分区、六类本地示例：短中文、FF14 长名称、日文／假名、同形异读、第二页、F6 活用。字号和宽度即时更新，点击示例只改变高亮，日语可展开／收起；默认 18／672 保持。
- CandidateView 共用正式候选与预览的测量／渲染，CandidateWindow.xaml 内容复用。预览不修改正式渲染字号、不接收输入法 IPC、不写注册表；旧的三个设置字段兼容。
- 恢复默认只重置布局草稿，保留简繁和词库；取消／关闭放弃未保存改动。宽预览使用横向滚动，长内容可纵向滚动。现有保存仍逐项写入，事务式失败恢复和新布局字段留待 UI-02。
- test-preview.ps1 通过：既有 9 组布局、表记／假名选择，新增 108 组预览与生产尺寸／位置／序号对照，以及滑块联动、重置、取消不写配置、小窗滚动和局部高亮。已生成离屏渲染图，不把自动布局检查当成实机多屏／DPI／远程验收。
- 本批未改中文解码与词库，CN-01 失败清单继续保留；UI-02 为下一批。已生成 Harukoto-0.4.4-alpha.12-Setup.exe（约 164.5 MiB）及 ZIP，构建源提交 f71bd97，工作区干净；包内渲染器／DLL 与构建产物一致，SHA256 匹配。解包、内置 PS7 安装／卸载预检、安装器离屏渲染通过，机器注册未改变；尚未执行实际安装或发布 Gitea Release。

## 2026-11-10 · CN-01／CN-02／WIN-01 首轮工具与基线

- 新增独立候选探针及 48 条手写场景语料，简繁×FF14 开关各重复 3 次，每个按键执行核心输入和 Windows 候选合并；保留版本／哈希、候选来源、消耗长度、分组命中及逐键性能。见[评测说明](CHINESE_EVALUATION.md)及[基线摘要](benchmarks/chinese-alpha11-baseline.json)。
- FF14 关闭时 44 个基础样本：简繁均 32 个首选、40 个前 7 项命中；3 个整个候选列表未命中。包含 4 个已知回归样本，不代表总体语言准确率；具体错误列入下一批排序与分词定位。
- WIN-01 新增 PowerShell 7 只读诊断：64 位用户／机器注册、DLL 哈希对比、安装器记录版本、主题状态及无法读取计数；有读不到的进程不报“全部更新”。不终止应用、不提权、不读取输入文本。见[诊断说明](WINDOWS_DIAGNOSIS.md)。
- 19 项 CTest 通过（76.21 秒），包括新增探针指标／实际候选和诊断损坏快照／跨包去重测试。本机诊断识别到部分不同版本模块；日志仅存本地，不提交进程信息。
- 本批只新增开发／诊断工具，输入引擎和用户布局未改变，发行基线仍为 alpha.11；未重新打包或安装。冷磁盘启动、真实 TSF／渲染延迟、升级失败矩阵仍待验收。下一批优先 UI-01 真实候选布局预览，默认 18／672 保持。

## 2026-11-10 · alpha.11 基础中文覆盖与短拼音候选

- 已校验的多字词全部进入原生组词索引，保留原始低频权重，不再仅存在于 SQLite 单词查询中。最终中文读音记录 410,546 条，原生记录从 219,230 增加到 410,379（增加 191,149）。数量是读音／繁简变体记录，不等于唯一词数。
- 新增 daily-chinese.tsv：272 条读音、271 个日常词句，覆盖聊天、电脑操作和游戏交流；明确编辑权重，保留带声调拼音和繁简对应。基础表加入野队，不依赖 FF14 启用。
- 短拼音（最多 8 字母）对外候选搜索从 5 条路径扩为最多 21 条；内部探测及长句仍保留原边界。yedui 第一页能选野队，第二页可正确选词。
- 修复扩充后低频纠错抢在完整拼音前的问题：zhidaole 的知道了优先于猜测指导科。
- 新增全部 272 条日常词句的简繁首选与上屏自动测试；全部词库合并、去重和多音字索引检查通过。词库范围扩大仍不等于具备完整语义理解，其他歧义排序继续收集。
- 包含 alpha.10 修改，FF14 词库保持 0.3.0，无需重复导入。alpha.11 实机安装与持续输入验收待执行。


## 2026-11-10 · alpha.10 中文排序修复

- 修复 zhidaole 被未完成的 zhi-da + ole 分段抢先，知道了恢复首选；保留未完成拼音的部分候选。新增十组普通中文真实词库回归及诊断入口。
- 已启用主题加入独立中文名称，优先于弱拼接；wuerdaha 首选乌尔达哈／烏爾達哈，日文与假名保持。主题中文繁体按词组优先转换，保留标点和长名称。
- 实机只读确认用户已安装 alpha.9、FF14 0.3.0 启用且含 xinkule，但 QQ 等进程仍加载旧 DLL；必须从托盘彻底退出并重开。安装完成提示已补充。
- 16 项 CTest 已通过；alpha.10 尚未安装。普通中文词频与生硬组合仍待专项评估，不宣称已解决全部语义问题。词库仍为 0.3.0，无需重新导入。


历史批次（2026-11-10）：alpha.9 随附 [FF14 全量包 0.3.0](FF14_FULL_DICTIONARY.md)，57,672 条来源记录、113,463 组触发。支持显式无假名词条与大容量主题包，完整处理 Item／PlaceName／Completion；362 条缺日文物品、4 条占位地名和 50 条动态索引排除。自动拼音、程序对齐不等同逐项人工审核。41 项 Python 测试与 16 项 CTest 验证；用户安装尚未更新。下方为此前批次记录。


项目现名为“晴词 · Harukoto”，独立维护自 Meowpuut。仅保留 Windows 与 Android；macOS、iOS／iPadOS 代码及专用脚本已移除。当前开发构建为 0.4.4-alpha.15，升级兼容标识见[品牌说明](BRAND.md)。

更新日期：2026-11-09。维护仓库：[NemophilaHare/input](https://git.hare.fun/NemophilaHare/input)，当前集成分支为 `main`。本文记录源码状态；“代码完成”“本机验证”“已发布／已安装”分别记录，不能互相替代。

## 当前进度

历史 alpha.8 随附 FF14 0.2.0：204 条、431 组触发；所选制作采集与深层迷宫组已覆盖，金碟币因读音不足排除，其他全量 Item 尚未收录。修复检索后缀被误当完整读音的处理，并保留名称冒号。覆盖清单见 Resources/DictionaryPackages/ff14/coverage.json。

历史构建 alpha.7 随附 FF14 词库 0.1.1：23 个职业和 10 个物品相关定型文术语，共 89 组触发。名称及读音已在线交叉核对；Completion 使用固定提交并按行 ID、Group、Key 对齐。管理窗口增加“预览 FF14 词库”，默认关闭导入。新增真实词库的全部触发首屏、简繁双模式、关闭与中文隔离测试；实机上屏和第二位人工复核待做。

历史构建 0.4.4-alpha.6：新增主题词库图形窗口、来源／许可与冲突预览、开始菜单和设置入口。窗口需要 Python 3.10+ 及 tkinter；预览后词库改变时拒绝导入。FF14 审核格式、校验和仅 verified 导出工具已实现，当时为空目录模板。32 项日语 Python 测试包含 GUI 按钮流程和 FF14 导出检查，其中 1 项 C++ 联动由 CTest 单独执行。本轮未更改用户安装或个人词库。

0.4.4-alpha.5 已实现主题包命令行管理与 TSF 加载、独立启停、更新和回退；参见[管理说明](DICTIONARY_PACKAGES.md)。14 项 CTest 覆盖 Python→C++ 快照读取与候选联动。

0.4.4-alpha.4 修复 chi 被罗马字“血”挤出第一页的问题，明确触发词的表记／假名可分别选择且收起时都显示；保留中文首选。真实词库与系统 SQLite 的繁简回归、9 组布局及假名选词离屏渲染通过。日常用法见[操作指南](USER_GUIDE.md)。真实 TSF 投递与跨应用验收仍待执行。

0.4.4-alpha.3 已加入 F6 日语活用面板首版，支持整段显式触发词；部分组字活用尚未完成。

历史开发构建 0.4.4-alpha.2：Windows 注册分类改为简体中文；新增按用户持久保存的“输出繁体中文”开关，默认关闭。旧分类迁移逻辑及设置读写已增加自动测试；实际升级与跨应用切换仍待实机验收。Android 保持已有繁简设置，本轮未修改。

可认领的任务、依赖和验收标准见[后续开发计划](DEVELOPMENT_PLAN.md)，其中包含后续 FF14 主题词库。

| 工作项 | 状态 | 证据与边界 |
| --- | --- | --- |
| 品牌独立与平台收敛 | 源码已完成 | 输入法、安装向导、设置及文档统一晴词／Harukoto；Apple 实现与脚本移除，Android 保留且未构建新 APK；当前机器仍为旧安装 |
| 文档简体中文化 | 已完成首轮 | 根目录、Windows、平台说明及主要 docs 已转换；日语词例、繁简对照和第三方原始授权保留 |
| 远程画面候选窗不可见 | 已修复并安装验证 | 输入法候选窗不再排除屏幕捕获；用户通过远程／串流确认恢复显示；独立折射预览保留原行为 |
| 日语辅助输入第一阶段 | 已实现并安装 | 41 组拼音／英语触发词、读音标注、同形异读候选、本地词表导入；真实词库测试覆盖首屏候选 |
| 候选拥挤 | 已修复并安装 | 自然词宽、自动换行、读音分层；默认字号保持 18，默认窗口宽度 672；9 组字号／宽度布局测试 |
| UI 布局可视化调整 | UI-01／UI-02 已实现 | 共享预览、横纵排列、读音与间距、内边距、拖动宽度、预设和布局快照保存；UI-03 多屏／混合 DPI／远程实机验收待做；见 [设计范围](UI_LAYOUT_EDITOR.md) |
| 候选外观设置界面 | 源码已完成 | 字号／宽度滑块、保存／恢复默认、开始菜单和软件包入口；构建与离屏渲染检查通过，尚未安装到用户当前版本 |
| PowerShell 7 安装与打包 | 源码已完成 | 安装、提权沿用 PS7；MSVC 打包保留许可证及导入工具；尚需新包的交互安装验收 |
| 安装日志与注册失败恢复 | 核心实现及自动检查完成 | 安装／注册分阶段日志、注册失败恢复旧 DLL、文件校验与锁定文件保留；14 项检查通过，系统级提权与失败恢复尚待实机验收 |
| 动词与形容词变形 | 规则与数据加载已完成 | 十条自有词表示例、例外规则、校验与原子输出；日语相关 Python 测试合计 16 项通过，已接入 TSF 辅助数据及整段触发 F6 面板，实机验收待执行 |
| FF14 主题词库 | 全量表处理已实现并自动验证 | 0.3.0：57,672 条来源记录、113,463 组触发，含物品、地名与定型文；覆盖缺口见全量说明，真实应用上屏及逐项复核待做 |
| DICT-01 多主题词库 | 图形与命令行首版已实现并自动验证 | 默认关闭、独立启停、跨包诊断、原子快照、单级回退、来源预览及 TSF 加载；保留旧导入；基础词库冲突诊断、候选来源标记与实机验收待做 |
| FF14-01 格式与审核 | 校验及导出已实现 | 稳定 ID、分类、正式名／别名分离、版本、来源及审核字段；只导出 verified；首批来源已在线交叉核对，未经过第二位人工复核 |
| JP-02 活用数据加载 | 已实现并自动验证 | 二进制编译、C++ 严格校验、启动加载、按词／触发查找及打包安装接入；整段触发词已关联 F6 面板，部分组字活用未支持 |
| 单文件 EXE 安装向导 | 已构建、预检通过 | 安装／更新向导、内置官方 PS7、应用列表卸载入口；真实 UAC 安装／卸载待验收；保留个人数据、版本文件和缓存 |
| 简洁候选外观 | 已实现、布局检查通过 | 去掉猫耳／猫爪／玻璃装饰，细边矩形、浅蓝高亮，保留读音及字号；尚未替换当前安装 |
| 敬语、完整日语整句、生词本 | 待开发 | 见[分阶段路线图](JAPANESE_INPUT_ROADMAP.md)，不可标为已支持 |

当前实现参考 Madamada 的公开产品文档与自定义词表格式，并独立编写。没有接入其私有引擎、完整词库或未公开接口。

## 开发者从这里开始

1. 阅读 [Windows 构建、安装与快捷键](../Windows/README.md) 和[日语路线图](JAPANESE_INPUT_ROADMAP.md)。
2. 准备 Windows x64、PowerShell 7、Python 3、Visual Studio C++ 工具、Windows SDK 和 CMake。使用 `Windows/build-ime-msvc.ps1` 构建；CMake 不在 PATH 时用 `-CMake` 指定绝对路径。
3. 执行 `python -m unittest discover -s Windows/tests -p test_japanese_dictionary.py` 和 `Windows/test-preview.ps1`。构建脚本已运行 CTest。
   安装流程变更还须运行 `Windows/tests/InstallSupportTests.ps1`；实机测试按 [Windows 验收记录](WINDOWS_VALIDATION.md)填写。
4. 安装前运行 `Windows/install-ime.ps1 -VerifyOnly`；需要真实测试输入法时才执行正常安装。DLL 更新后重新打开测试应用。
5. 使用 `Windows/package-ime.ps1 -MSVC -Version <版本>` 打包。`.build/` 为生成目录，不提交 DLL、词库构建结果或本机日志。

本机验证环境为 Windows x64、PS7 7.6.9、Visual Studio 18 Community / MSVC 19.51。构建脚本通过参数和 PATH 定位工具，不依赖某个开发者的用户名或缓存路径。WPF 渲染器使用系统 .NET Framework 编译器不代表脚本运行在 PS5.1。

## 代码导航

| 位置 | 职责 |
| --- | --- |
| `Windows/TextService.cpp` | TSF 注册、按键、候选传送、用户词表加载 |
| `Windows/MixedComposition.h` | 中文／日语候选合并、显式触发和读音标注 |
| `Windows/GlassPreview.cs`、`CandidateLayout.cs` | WPF 候选显示、自动换行、屏幕坐标与设置读取 |
| `Windows/CandidateSettings.cs` | 当前用户候选外观设置界面 |
| `Resources/Lexicon/japanese-bridge.txt` | 自编的 41 组基础触发词 |
| `scripts/japanese_dictionary.py` | 公开词表格式子集解析与序列化 |
| `Windows/import-japanese-dictionary.py` | 用户词表校验、原子替换导入 |
| `Windows/manage-japanese-packages.py`、`scripts/japanese_packages.py` | 主题包预览、启停、冲突、快照及回退 |
| `Windows/JapanesePackages.h` | 启动读取启用快照、边界检查、失败时忽略可选主题 |
| `Windows/compile-lexicon.py` | Windows 增强日语表生成；跨平台三列表保持原格式 |
| `Windows/tests` | 中文回归、日语候选、词表导入与布局测试 |
| `Windows/install-ime.ps1`、`package-ime.ps1` | 版本目录安装、注册、设置入口与分发打包 |
| `Windows/InstallSupport.ps1` | 可测试的注册恢复逻辑、版本文件 SHA-256 校验、引擎发行元数据写入／删除 |
| `Windows/EngineRelease.h` | 读取 HKLM 中安装器批准的引擎路径与哈希（WIN-03 第 1 步），严格校验与 ACL 核对 |
| `Windows/Setup.cs`、`build-setup.ps1` | 单文件 EXE 安装向导、内置官方 PS7、应用列表卸载入口 |
| `Windows/uninstall-ime.ps1` | 移除机器注册和当前用户配置，保留词表、版本文件及缓存 |
| `Windows/CandidateWindow.xaml` | 当前简洁候选外观；旧 GlassPreview.xaml 仅用于独立实验预览 |

## 已验证与仍需验收

- C++ Release：14 项 CTest，含主题包工作流、损坏快照、真实中文词库、日语触发和会话测试；MSVC Release 测试保留断言。
- Python：38 项日语相关测试（包含需要 C++ 读取器、由 CTest 执行的 2 项），覆盖词表、活用规则／编译、主题包事务、GUI 按钮及 FF14 审核导出。
- WPF：9 组字号／宽度组合、7 个候选无重叠；设置页创建并离屏渲染。
- 打包：MSVC ZIP 生成成功，源码及软件包的 `-VerifyOnly` 通过；包内包含设置入口、PS7 解析脚本、词表导入依赖及许可证。
- 安装可靠性：14 项检查通过，含模拟注册／恢复失败、真实文件共享锁及真实安装入口的预检失败日志。注册调用在测试中被替换，不能据此认定系统 TSF 恢复已经实机通过。
- 本机手动：旧版候选不可见问题复现，修复后远程画面恢复；候选布局已更新到当前安装。
- 尚未完整验证：多显示器混合 DPI、各类编辑器／浏览器／游戏的 TSF 生命周期、焦点切换、设置页保存交互、新安装脚本的完整提权流程。文件完整性检查不代表这些行为已通过。
- 当前机器曾有旧 DLL 被应用占用并安排重启删除；这是本机维护记录，不是安装器已具有通用卸载／回滚能力。
- 新安装器只在机器注册阶段失败时尝试恢复旧 DLL；后续用户配置失败不自动回滚，保留日志后重试。旧版本目录保留，不自动删除。

2026-11-10 按维护者要求移除了旧 GitHub core／Windows Actions 工作流；Gitea 尚未配置 runner，当前使用本地构建与验证。将来启用 CI 时重新配置可用 runner 及标签。

## 接下来按这个顺序推进

| 顺序 | 任务 | 完成标准 |
| --- | --- | --- |
| P0 | 安装／更新实机验收 | 日志、注册失败恢复与锁定文件处理已实现；按验收表完成全新安装、覆盖升级及隔离环境下的失败恢复，记录操作与日志 |
| P0 | Windows 兼容性验收 | 记事本、浏览器、VS Code，以及远程串流；覆盖选词、翻页、切焦点、中文长句和日语读音；记录 OS、应用版本、结果 |
| P1 | 第二阶段词性与活用核心 | 一段、五段、する、来る及形容词；逐类例外测试；表记和读音同步；先完成纯逻辑再接面板 |
| P1 | 键盘变形面板 | 按键进入／退出、选择活用形、保留剩余组字；明确与现有 F4 展开行为的关系，避免抢占快捷键 |
| P2 | 敬语与表达表 | 独立维护特殊敬语、礼貌等级与用法提示；不以字符串替换代替语境处理 |
| P2 | 整句与词库管理 | 分段和读音消耗模型、中文回归、词库启停和诊断、生词记录；详见路线图 |

## 多人协作与进度维护

- 每个任务开 `codex/<主题>` 或团队约定的功能分支，从 Gitea 的 `main` 拉取最新代码。单次提交围绕一个可说明的变化；保留上游历史和授权文件。
- 提交说明写清修改目的与行为；PR 记录验证命令、结果和未覆盖范围。功能提交同步修改本文、相应使用说明和日语路线图，避免只在聊天里报告进度。
- 认领任务时在 Gitea issue／PR 写明范围和负责人；完成后在相关记录链接提交。不要仅凭计划标记“已完成”。
- 本地 `origin` 保留 GitHub 上游；维护远程使用 `gitea`。推送显式指定 `git push gitea <分支>`，避免误推上游。新克隆的远程名可能不同，先用 `git remote -v` 检查。
- 历史查询：`git log --oneline --decorate -20`；按功能查询如 `git log -- Windows/CandidateLayout.cs`；按文档查询 `git log -- docs/DEVELOPMENT_STATUS.md`。

提交索引见[开发变更记录](DEVELOPMENT_CHANGELOG.md)。本文未声称未发布的代码已经安装或通过全部实机验收。
