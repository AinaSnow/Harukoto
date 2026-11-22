# 独立引擎换代、槽位回收与接入层去词库（WIN-03 第 7 步）

WIN-03 阶段二第 7 步，2026-11-22 设计（基于 alpha.31 与 alpha.30 实机首测反馈）。

## 1. 目标与背景

第 6 步（6a～6d）与前置优化已证明独立引擎在普通应用与 FF14（UI-less）中都能 0 偏差提供候选、热更新词库与崩溃自动恢复。但距离真正兑现“更新后不重开应用即生效”与“多应用大幅省内存”还有四道架构缺口：

1. **固定单实例 scope 阻塞无缝换代**：当前常驻所有者与发现管道绑定在固定 `scope = harukoto`。安装新版本写入更高 `sequence` 后，只要旧所有者仍服务哪怕 1 个未切换的连接，新所有者就因互斥租约冲突无法启动；若先杀旧所有者又会打断正在组字的旧连接，且无法满足“新引擎启动失败继续用旧引擎”。
2. **8 槽硬上限在多应用常驻下易打满**：接入层只在 `Deactivate()` 时释放连接槽，失焦后长期占用。桌面切过 9 个应用即可能让前台应用撞上 `Full` 回退。
3. **接入层仍依赖本地 `MPNSession`**：选词（`mpn_choose`）、按页扩展（`mpn_expand_candidates`）、纠错/个人词标记、按词拆分学习（`mpn_candidate_words`）与以词定字都在本地核心会话完成。
4. **每个应用仍启动加载 ~180 MiB 词库**：alpha.30 实机诊断已观察到单机同时有 5 进程 7 个 TSF 实例加载 DLL，未打字也各持一份基础词库（合计 1.2+ GiB）。

## 2. 子批次拆分（7a～7d）

| 子批次 | 内容 | 验收 |
| --- | --- | --- |
| **7a** | 按版本 `sequence` 分 scope 共存；旧所有者自动关发现并在连接归零后优雅退出（`Superseded`）；接入层组字边界预连新引擎并原子切换（新引擎失败继续用旧引擎）；协议不兼容明确提示重开应用；诊断补充换代与不兼容字段 | `v1`→`v2` 并存且旧组字不断流，空边界切到 `v2` 后 `v1` 自退；坏 `v3` 启动失败继续用 `v2`；高协议 `v4` 报不兼容与重开提示，不静默混用；开关关闭路径不变 |
| **7b** | 失焦宽限后主动礼让释放连接槽 + 所有者空闲槽 LRU 回收（保证活跃前台绝不因后台闲置进程被拒）；重新聚焦打字时异步/热 `admit()`（~1.1 ms）无缝重连 | 12+ 客户端先后打字共用 8 槽全部走独立引擎、无 `Full` 粘滞回退；不中断在途查询与正在组字的客户端 |
| **7c** | 协议与宿主接管按页扩展、选词确认与学习元数据（候选标志增 `Typo`/`Personal`、`Commit` 携 `words` 与新 `segmentContext`、翻页扩展走 `PageNext`、以词定字与忘记不再读本地核心会话） | 8 场景全量对照（含选词、翻页扩展、学习、以词定字、忘记、F6）与进程内路径 0 不一致，接入层引擎路径不再依赖本地 `MPNSession` 计算 |
| **7d** | 接入层去启动期基础词库加载（开关开启且存在批准同协议发行时 `Activate()` 零基础词库加载）；本地词库改为严格按需懒加载兜底（供关闭开关、提权/沙箱、断路器回退）；在途选词改为短同步等待引擎回复 | 开启开关后每应用私有内存从 ~185 MiB 降至 < 10 MiB；关闭开关与沙箱/提权/断路器兜底行为完整；全量 CTest 与 6 套评测通过 |

## 3. 7a 设计细节：按版本分 scope 换代

### 3.1 发行评估与不兼容判定（`EngineRelease.h`）

在保留既有 `release::select(snapshot, protocol)` 不变的基础上，新增 `release::evaluate(snapshot, protocol)`：
- 在 `snapshot.entries` 中找出全局最大 `sequence`（若并列或无有效条目，返回 `Compatibility::None`）；
- 若存在未知新格式（`snapshot.unsupported > 0`），或全局最新条目的 `protocol != protocol`，返回 `Compatibility::ProtocolMismatch`（并记录 `latestProtocol` 与 `latestSequence`），明确告知旧接入层“机器上已安装协议不兼容的新版本”；
- 否则返回 `Compatibility::Match` 及该最新条目 `selected`。

### 3.2 按发行序号隔离 scope（`EngineResident.h`、`EngineOwner.cpp`）

- 新增 `resident::releaseScope(sequence, prefix = DefaultScope)` → `<prefix>-s<sequence>`（如 `harukoto-s1`、`harukoto-s2`）。合法字符集 `[a-z0-9-]`，与既有测试显式传入自定义 `--scope` 完全兼容。
- 生产常驻所有者在未显式指定 `--scope` 时，读取批准条目后自动使用 `releaseScope(approved->sequence)`，因此新旧版本各自持有独立的实例租约、发现管道与易失状态键，升级窗口期可安全并存。
- `resident::ensure()` 在启用版本化 scope（默认对 `DefaultScope` 开启，自定义测试 scope 可选开启）时，自动按 `approved->sequence` 连接对应 scope 的所有者。
- `harukoto_engine_diagnose --resident` 未传 `--scope` 时优先查当前 HKLM 批准条目的 `releaseScope`，无记录时回退查旧 `DefaultScope`。

### 3.3 旧所有者自动优雅退场（`EngineOwner.h`、`EngineOwner.cpp`）

- `EngineOwner::Lifecycle` 新增可选回调 `superseded()` 与退出原因 `Exit::Superseded`（`rs::Exit::Superseded = 7`，属于干净退出，不触发断路器）。
- 常驻所有者每 2 秒（以及客户端数量变化时）检查 `superseded()`：当 HKLM 发行来源消失、被撤销，或出现更大 `sequence` 的条目（无论同协议还是更高协议）时进入 `draining` 状态：
  1. 立即关闭发现管道（不再接收新客户端，也绝不干扰新版本）；
  2. 已有连接继续正常完成查询与组字，不强行切断；
  3. 一旦 `clients == 0 && !pending`，立即优雅停止宿主并退出。

### 3.4 接入层空组字边界换代与保旧（`EngineTsf.h`、`TextService.cpp`）

- `tsf_engine::Client` 维护主连接 `link_`（及其 `linkSequence_`）与后台预连接 `standby_`（及其 `targetSequence_`）：
  - **正在组字时绝不切换**：换代检查与切换只在空组字边界（`!*mpn_input(session) && !pending_`）触发，且检查频率节流（至少间隔 2 秒）。
  - **新引擎预连与原子切换**：发现同协议更高 `sequence` 时，在后台创建 `standby_` 连接新版本引擎；期间主 `link_` 继续照常服务。`standby_` 达到 `Ready` 后，在下一个空组字边界原子替换 `link_` 并释放旧链接（旧连接关闭触发旧引擎连接数归零自退）。
  - **失败保旧**：若 `standby_` 连接/启动失败，丢弃 `standby_`，记录该 `sequence` 冷却 60 秒并递增 `cutoverFailures`，主 `link_` 丝毫不受影响，继续用旧引擎。
  - **协议不兼容**：收到 `ProtocolMismatch` 时标记 `incompatible_ = true`，取消 `standby_`，不再尝试切新引擎；候选窗状态栏提示“输入法已更新，请重开应用”，运行时诊断输出 `engine_incompatible: true` 与 `engine_sequence`／`engine_cutovers`／`engine_cutover_failures`。

## 4. 7b 设计细节：失焦宽限礼让、空闲 LRU 回收与零惩罚重连

### 4.1 宿主空闲读超时与 LRU 端点回收（`EngineHost.h`、`EngineHost.cpp`、`EngineServer.h`）

- 常驻宿主管道读超时 `readMs` 由 30 秒提升至 300 秒（`rs::HostReadMs`，5 分钟），避免聚焦应用短暂阅读/停顿超过 30 秒即被宿主断开数据管道。
- `ServerConnection` 新增 `busy()`（存在在途请求为真）；`Host::Endpoint` 记录每次建连、收帧与任务完成的 `lastActivity`。
- `Host::Options` 新增 `idleEvictMs`（默认 0 关闭，常驻模式由所有者经 `--idle-evict-ms` 传入 `rs::SlotIdleEvictMs = 5000`）：当 `Allocate` 发现 `endpoints_.size() >= connections` 时，严格按 LRU 回收最久安静且无在途请求、无待写回复的已连接端点并立即 `drain(true)` 释放，绝不驱逐正在计算或仍在监听/退场中的端点。

### 4.2 所有者 8 槽空闲 LRU 回收（`EngineOwner.h`、`EngineOwner.cpp`、`EngineHandoff.h`）

- `handoff::Session` 暴露只读等待句柄 `revokeEvent()`（复用既有带 `SYNCHRONIZE` 权限的 `release_`）。
- `EngineOwner::Lifecycle` 新增 `slotIdleEvictMs`（默认 0 关闭以保持既有满槽测试行为，常驻模式为 `5000` ms，测试构建支持 `--slot-idle-ms`）：8 槽均被占用且新接入方到达 `reserve()` 时，选取最久无控制活动且无在途控制请求的活跃槽位 `release(victim)`（置位 `revoked` 事件）并即时让位。

### 4.3 接入层失焦宽限礼让与 `Reclaimed` 零惩罚重连（`EngineTsf.h`、`TextService.cpp`）

- **失焦与静置主动礼让**：`TextService` 在 `engineWindow` 上维护 `EngineIdleTimerId` 定时器——失焦（`OnSetFocus(FALSE)`／`OnKillThreadFocus()`）后宽限 15 秒（`FocusYieldMs`）未重新聚焦则于空组字边界调用 `engineClient.yield()` 主动释放槽位（快切窗口 15 秒内不掉热连接）；持续聚焦但静置 180 秒（`IdleYieldMs`，早于宿主 300 秒读超时）亦在空边界礼让释放；组字进行中或有在途查询绝不礼让。
- **即时退管与零惩罚重连**：`Link` 后台工作线程以 `WaitForMultipleObjects` 同时等待新请求与 `revokeEvent()`；一旦被所有者 LRU 回收，毫秒内关闭宿主管道、置 `State::Reclaimed` 并退出线程。`tsf_engine::Client` 识别 `Reclaimed` 后静默重建连接并递增 `engine_reclaimed`，不计 `failures`、不触发 3 次粘滞回退。诊断字段补充 `engine_yields` 与 `engine_reclaimed`。
