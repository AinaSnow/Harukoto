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

## 5. 7c 设计细节：引擎接管扩展、选词与学习元数据

### 5.1 协议升至 v2 与只读元数据原语（`Core/MeowpuutCore.h`、`EngineProtocol.h`、`EngineMessages.h`）

- **协议版本号升至 `ProtocolVersion = 2`**（同步更新 `Windows/InstallSupport.ps1` 的 `$script:HarukotoEngineProtocol = 2`）：因候选标志新增位宽且新增无状态元数据动作，升版触发 7a 的 `ProtocolMismatch` 保护，防止机器上未关闭的 v1 旧接入层与 v2 引擎静默混用。
- **核心新增两个只读 C API（不改动任何既有签名）**：
  - `mpn_decompose_words(session, reading, text)`：对已确认的中文片断按核心词典做按词拆解，输出与 `mpn_candidate_words` 相同格式的 `reading\tword\n` 文本，供选词词频学习（CN-06c）与忘记（CN-07）使用；
  - `mpn_advance_context(session, context, text)`：把新选文字按单字繁化并截断保留最新语境（与 `mpn_choose` 内部语境推进一致），供分段选词与以词定字后延续上下文。
- **候选标志与无状态检查消息**：
  - `candidate_flag` 新增 `Typo = 8`、`Personal = 64`、`LiteralTail = 128`（`Known = 0xFF`），覆盖完整 `WindowsCandidate` 元数据，无需再反查本地 `MPNSession`；
  - 新增 `KeyAction::Inspect = 7`（携带 `reading`、`inspectText`、`inspectContext`、`simplified`）及 `CandidatePage::inspect` 标志（仅在该标志置位时编码 `decomposed` 与 `nextContext`，普通候选页零额外字节）。相比依赖服务端有状态 `Select` 快照，`Inspect` 无状态、不改动会话内容，跨深页翻页、以词定字与忘记均可毫秒内直接完成。

### 5.2 宿主短路执行与接入层自足候选化（`EngineServer.h`、`EngineWindows.h`、`EngineTsf.h`、`TextService.cpp`）

- `EngineServer` 收到 `KeyAction::Inspect` 时保留 `session.input/page/context/chineseLocked/candidates` 原样，直接交由 `WindowsDictionary::queryImpl` 顶部短路执行（仅调用 `mpn_decompose_words` 与 `mpn_advance_context`，不做拼音解码也不查 SQLite，耗时数微秒）。
- `WindowsCandidate` 补充 `consumed`、`literalTail`、`typo`、`personal`、`engineId` 字段；`tsf_engine::materialize()` 仅凭引擎返回的 `Candidate` 与当前读音即可还原完整候选（含 `LiteralTail` 尾缀拼接），不再读取本地 `MPNSession`。
- `tsf_engine::Driver` 与 `Link::callSync` 新增 `fetchMore()`（`PageNext` 按需拉取后续翻页）与 `inspect()`（`KeyAction::Inspect`）；`Driver::fetch()` 保留首页随带的第 1～3 轮扩展候选（`extraRounds[3]`），按 `PageDown`／`=` 扩展时先消费本地缓存轮次，不足再按需向引擎拉取下一页。
- `TextService` 的 `chooseCandidate()`、`forgetCandidate()`、`pickCharacter()`、`candidateReading()`、`canExpandCandidates()`、`expandCandidates()` 与 `typoCandidate()` 对引擎候选全部走引擎元数据与 `Inspect`，完全解除对本地 `MPNSession` 的依赖。

## 6. 7d 设计细节：接入层去启动期基础词库与严格按需兜底

### 6.1 瘦客户端启动条件与 35 字节占位词库（`TextService.cpp`）

- `Activate()` 先加载轻量的 `japanese-conjugations.bin`（约 4 KiB，供 F6 活用面板使用）与个人学习记录存储，随后检查 `canStartThinClient()`：
  1. `StandaloneEngine` 开关开启；
  2. 当前进程令牌符合常驻引擎接入条件（`discovery::inspectProcess(GetCurrentProcess()).supported()`，即中完整性、非沙箱、非受限、非提权）；
  3. HKLM 存在同协议批准发行（`release::evaluate(...) == Compatibility::Match`）。
- 满足上述三项时以 **瘦客户端模式**（`localLexiconLoaded = false`）启动：不打开 `native-pinyin.tsv`、`japanese.tsv`、主题包与 `language-evidence.sqlite`，仅用 35 字节单行占位词库 `createStubLexicon()` 创建 `< 2 KiB` 的最小 `MPNSession` 负责纯编辑态（`mpn_input`／`mpn_preedit`／`mpn_segment_context`／`mpn_clear`），实测单实例私有内存增量 `< 1 MiB`（验收门槛 `< 10 MiB`，较进程内模式 ~185 MiB 节省 99%+）。
- 其余场景（开关关闭、提权/低完整性/AppContainer 进程、未安装或协议不兼容发行）在 `Activate()` 保持原有本地词库加载路径不变。

### 6.2 建连推迟、选词短同步等待与按需懒加载兜底（`EngineTsf.h`、`TextService.cpp`）

- **首次建连不误触发本地加载**：瘦客户端首次按键且后台工作线程正处于 `Connecting`（热握手约 1 ms、冷启动约 1.75 s）时，标记 `engineDeferred = true` 暂存请求而不回退；工作线程就绪唤醒 `onEngineReply()` 后立即发出查询并刷新候选（候选窗在 `pending || engineDeferred` 期间保持不闪空帧）。
- **选词键有界同步等待**：`Link` 新增 `stateEvent`/`replyEvent`/`waitReady()`/`waitPending()`；当用户在建连或回复在途时按下空格／回车／数字选词／翻页／标点／F4／F6，`settleEngine()` 在瘦客户端模式下先同步等待最多 1.5 s 建连与 1 s 查询回复，正常情况下直接由引擎完成，绝不因手速快而拉起 185 MiB 本地词库。
- **严格懒加载兜底 `ensureLocalLexicon()`**：仅当真正发生不可恢复回退（用户关闭开关、引擎连接/超时失败、断路器断开、超出协议能力门槛或学习记录未追上）时才触发 `ensureLocalLexicon(true)`，一次性加载已发布基础快照或安装目录词库、主题包与 SQLite 证据，并强异常安全地把当前与挂起上下文中的输入、语境、简繁、模糊音与纠错设置完整迁移到新会话。
- **诊断可见**：`engineClient.diagnosticFields()` 新增 `engine_local_lexicon` 布尔字段，`DiagnosisSupport.ps1` 在 `Engine` 摘要行标注 `，瘦客户端`。
