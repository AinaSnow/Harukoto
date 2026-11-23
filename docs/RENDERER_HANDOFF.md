# 候选窗渲染器换代（设计稿）

2026-11-23，维护者已确认 §10，RH-1～RH-5 已实现、通过自动测试并验收提交（待实机），RH-6 待打包两个界面有差异的连续版本并由维护者走实机清单。目标：更新晴词后，已经打开的应用不用重开就能用上新版候选窗界面。做法参照引擎换代（[ENGINE_RELEASE](ENGINE_RELEASE.md)、[ENGINE_CUTOVER](ENGINE_CUTOVER.md) 7a）。

## 1. 现状（已核实）

- 渲染器有两种，都是 `MeowpuutGlassPreview.exe --ime <名>`（`GlassPreview.cs` 的 `RunCandidateHost`）：
  - **桌面渲染器**：`--ime desktop`，窗口 `MeowpuutCandidates-desktop`，由安装器和开机启动快捷方式拉起，普通应用都用它。
  - **每进程渲染器**：`--ime <pid>-<this>`，由 DLL 从 `modulePath().parent_path()` 启动（`TextService.cpp` `sendGlass`，约 2176 行），在桌面渲染器不可用时使用（沙箱应用等）。
- DLL 只在桌面渲染器的映像路径**等于本 DLL 目录**时才用它（`desktopRendererWindow`，约 2161 行）。安装器换版时按旧路径停掉旧桌面渲染器、启动新版的（`install-ime.ps1` 约 225～250 行）。于是旧 DLL 判定路径不符，改从自己目录启动旧版每进程渲染器——这就是试用时看到旧样式的原因。
- 载荷：`WM_COPYDATA`，标签 `0x4D50`，UTF-16 文本，没有版本号（格式见 §3.1）。渲染器只回“接受／不接受”，不回传数据。
- 结论：大部分情况只要让 DLL 认可“HKLM 批准的最新渲染器”，就能直接用上安装器已启动的新版桌面渲染器；每进程渲染器还需要预启动和边界切换。

## 2. 发现：只信任 HKLM 批准记录

新键 `HKLM\SOFTWARE\Harukoto\RendererReleases`（64 位视图），一个版本一个 REG_MULTI_SZ 值，值名为版本目录名。ACL、上限、目录保护检查与 `EngineReleases` 相同。

不并入 `EngineReleases` 的原因：那里每条固定 8 行，alpha.30 起的 DLL 会把多出行的条目当作损坏并排除，等于让已打开的应用失去引擎换代；而且只有带引擎程序的包才写那个键。

格式（固定 6 行、固定顺序）：

```
format=harukoto-renderer-release/1
id=<版本目录名，与值名相同>
sequence=<本键内的安装序号>
payload=<最低>-<最高>          # 支持的载荷版本，如 1-1
renderer=<绝对路径>\MeowpuutGlassPreview.exe
renderer-sha256=<64 位小写十六进制>
```

- 读取方 `Windows/RendererRelease.h`：复用 `EngineRelease.h` 的 REG_MULTI_SZ 解码、路径校验、键与目录 ACL 检查（抽成共用的内部函数，引擎读取器行为不变）。`select(snapshot, E)`：在 `payload` 范围包含本 DLL 发送版本 E 的条目中取 sequence 最大者，并列时一个都不选。首行是其他 `/N` 的条目跳过。
- 只在后台线程读取（注册表、ACL、哈希都可能阻塞），TSF 回调里只读后台发布的结果。
- 文件身份：用 `EngineDeployment(path, sha256)` 固定文件并核对哈希；启动后或发现桌面渲染器时，用 `verify(process)` 核对**运行中进程的映像文件**。窗口名、PID、映像路径字符串只用来定位，不作身份依据。固定句柄只在核对和启动期间保持，之后释放，以免挡住卸载或清理。
- 不使用：HKCU、环境变量、当前目录、PATH、桌面渲染器自报的信息、候选目录里的清单文件。
- 写入方：`InstallSupport.ps1` 新增 `Publish-/Unpublish-HarukotoRendererRelease`，在提权阶段注册成功后写入（sequence＝现有最大值＋1，写后读回）；清理损坏或文件已删除的条目，只保留最新 4 条；卸载时删除整个键。重装旧包会得到最大 sequence，这就是回退方式（与引擎相同）。
- `payload` 范围是构建常量：C#（`CandidatePayload.MinVersion/MaxVersion`）、C++（DLL 的发送版本 E）、安装脚本三处各一份，用一致性测试锁住（与引擎协议号的做法相同）。

## 3. 载荷协议

### 3.1 v1（即现状，固定为规范）

- `COPYDATASTRUCT.dwData = 0x4D50`，UTF-16 文本以 NUL 结尾，≤ 8192 字节（DLL 端 < 4096 字符）。`wParam` 是发送方记下的前台窗口。
- 空文本：隐藏（仅当 `wParam` 与上次显示的前台窗口相同），总是回 1。
- 非空：按 `\n` 分行，共 3～10 行。
  - 第 0、1 行：锚点 x、y（屏幕像素，整数）。
  - 第 2 行：`编码串\t高亮序号\t状态栏文字\t模式`，模式为 0 收起、1 展开、2 变形面板。状态栏文字由 DLL 拼好（简繁、标点、提示、页码、`+`）。
  - 第 3～9 行：候选 `文字\t读音[\tja|\tja-direct]`。
  - 模式提示（切换中英）用同一格式，没有候选行。
- `wParam` 不是当前前台窗口时，渲染器回 0（不接受）。

### 3.2 版本表达

用 `dwData` 标签表示版本：v1 仍是 `0x4D50`；vN（N ≥ 2）用 `0x4D500000 + N`。旧渲染器不处理未知标签、回 0，DLL 会回退，不会显示错乱的内容。

不靠试发来探测版本：v1 渲染器在前台窗口不符时也回 0，“不接受”分不清是版本不兼容还是焦点变了。所以版本范围以 HKLM 记录为准。

### 3.3 兼容规则

1. **新渲染器必须能显示旧 DLL 的数据**：渲染器接受从 1 到自己最高版本的所有版本，v1 解析器永久保留（任何带换代逻辑的 DLL 都可能只会发 v1）。因此记录里的下限一直是 1，除非维护者明确宣布放弃某个版本。
2. **同一版本内只做追加**：新字段只加在 tab 分隔的末尾，渲染器忽略多出的字段，缺失的字段用默认值。行数上限、字段含义、坐标单位一旦改变，就要升版本号。
3. **新数据遇到旧渲染器**：DLL 只发一种版本 E。只有 `下限 ≤ E ≤ 上限` 的渲染器可用；否则（例如重装了旧包之后）不用它，改用自己目录的渲染器。DLL 可以选择保留低版本发送器来向下兼容，但不要求。
4. 自己目录的渲染器与 DLL 是同一构建，视为支持 E，不查记录。
5. 第一步不引入 v2，只把 v1 写成规范并加测试。等渲染器真正需要 v1 带不了的数据时再加 v2（见 §8）。

## 4. 切换时机与旧渲染器退场

**空边界**：不在组字（`!*mpn_input(session)` 且没有在途查询），并且最近一次发给当前渲染器的是隐藏（空载荷）。判定方式与引擎 7a 相同。

后台检查：在空边界和激活时触发，至少间隔 2 秒。读记录、选条目，然后对每个（sequence, 文件 ID）只核对一次哈希，结果做成不可变对象，用原子 `shared_ptr` 发布。

DLL 内的状态：`current`（类型：自己目录／桌面／批准的每进程；窗口、进程句柄、sequence），`standby`，`approved`（后台结果），`cooldown`（失败的 sequence → 截止时间）。

- **桌面渲染器（主路线）**：发送时，如果桌面窗口存在，且其进程能通过自己目录或 `approved` 的核对（`verify(process)`），就用它。目标只在空边界切换；例外是当前目标已经退出（比如被安装器停掉），这时立即切换，因为没有需要隐藏的旧窗口。
- **每进程渲染器**：当 `approved.sequence` 大于 `current` 且桌面渲染器不可用时，在空边界从批准目录启动 `standby`（窗口名加 `-s<sequence>` 后缀，避免与当前窗口同名）。在它准备好之前，继续用 `current`。窗口出现且 PID 对得上之后，在下一个空边界：给旧渲染器发隐藏 → `WM_CLOSE` → 切到新的。5 秒内没准备好或进程已退出，就结束这个子进程，把该 sequence 冷却 60 秒，失败次数加一。
- **退场**：每进程渲染器收到 `WM_CLOSE` 后退出（现有逻辑）；宿主进程退出时也会自行退出（现有的 200 ms 定时器）。桌面渲染器由安装器按路径停止（现有逻辑），DLL 不停止桌面渲染器。
- 启动参数：使用绝对路径，`lpCurrentDirectory` 设为版本目录，不继承应用的当前目录。

## 5. 失败回退

回退链：批准的新版渲染器 → DLL 自己目录的渲染器 → 原生 STATIC 弹窗（现有的最后兜底）。

| 情况 | 处理 |
| --- | --- |
| 键 Missing／Unprotected／Invalid、没有可选条目、哈希不符、文件缺失 | 不换代，继续用自己目录的渲染器（行为与现在一样） |
| 载荷版本范围不含 E | 同上 |
| standby 启动失败、超时或退出 | 保留 current，该 sequence 冷却 60 秒 |
| 已切到的新渲染器连续 3 次不接受（避开前台窗口不符造成的误判）或进程退出 | 本次按键用 STATIC 弹窗；该 sequence 冷却；下一个空边界回到自己目录的渲染器 |
| 用户关闭开关（默认开启；HKCU `rendererHandoff=0` 关闭，只能关、不能指定路径） | 行为与现在完全一样 |

诊断新增字段：`renderer_source`（own／desktop／approved）、`renderer_sequence`、`renderer_payload`、`renderer_handoffs`、`renderer_failures`。这样诊断里能分开看到 DLL 版本和渲染器版本。

## 6. 安全、沙箱与管理员进程

- 信任根与 ENGINE_RELEASE 相同：HKLM 键 ACL、版本目录 ACL、SHA-256。能改这些的只有管理员／SYSTEM，而他们本来就能替换 DLL，不在防护范围内。
- 防冒名：有人伪造 `MeowpuutCandidates-*` 窗口，或在用户可写目录里放一个同名 exe，都通不过 `verify(process)`，候选文字不会发给它。
- **管理员（高完整性）应用**：只从 HKLM 批准目录启动，子进程同样是高完整性（与现状相同）。已有的边界：高完整性应用会把候选发给中等完整性的桌面渲染器，同一用户的中等完整性进程可以注入它、读到管理员窗口里的输入。本方案不扩大这个面；已确认继续使用桌面渲染器（§10）。
- **低完整性／AppContainer**：能否读 HKLM 键以 ENGINE_RELEASE 的结论为准；LPAC 读不到时是 Missing，回到自己目录（与现状相同）。子进程继承 AppContainer，Program Files 版本目录对 ALL APPLICATION PACKAGES 可读、可执行（与现状相同）。由于 UIPI，它们本来就发不到桌面渲染器，会走每进程路线。
- 数据只单向流动：渲染器只回 0／1，DLL 不采信渲染器返回的任何其他内容。
- 测试注入：`Source` 只能通过测试程序的构造参数传入；DLL 不从环境变量或 HKCU 配置读取根路径。

## 7. 测试方案（不写 HKLM、不真实安装）

| 测试 | 内容 |
| --- | --- |
| `renderer_release`（C++） | 解析与选择规则（行数、顺序、范围、并列、未知格式、上限）；在 HKCU 测试键上用 `requireProtected=false` 注入；ACL 判定复用 engine_release 的用例 |
| `renderer_release_install`（pwsh） | Publish／Unpublish 写到 HKCU 测试键后，由 C++ 读取器核对；升级并存、重装旧版成为选中、清理、卸载；C#／C++／ps1 三处载荷常量一致 |
| `candidate_payload`（C#） | 把 `RunCandidateHost` 的解析逻辑提成纯函数；用 v1 样例（候选、模式提示、变形面板、空载荷）锁住现有行为；未知标签不接受；末尾多出的字段被忽略 |
| `renderer_handoff`（C++，纯逻辑） | 换代状态机，时钟、启动器、窗口查询都用假的：组字中不切换、目标已退出立即切换、standby 超时冷却、版本不兼容回到自己目录、回退安装、并列不选、开关关闭 |
| `renderer_handoff_live`（C++） | 把构建出的 exe 复制到两个临时目录 A、B，用 HKCU 测试来源。先以每进程方式启动 A，只发空载荷（不显示窗口），再发布 B，驱动空边界，确认 A 退出、B 接受。结束时清理进程、测试键和临时目录。在 pwsh 的 ctest 里跑 |

实机清单（追加到 WINDOWS_VALIDATION）：装 N，打开记事本和 Claude 桌面版；装一个界面有明显变化的 N+1，不重开应用，提交一次后新样式生效；管理员记事本；AppContainer 应用（计算器、设置搜索框）；重装旧包回退；关闭开关。

## 8. 仍然必须重开应用的改动

渲染器换代只覆盖“怎么画”。DLL 里的逻辑仍然随旧 DLL 留在已打开的应用里：

- TSF 接入：按键处理、组字与编辑会话、焦点、光标锚点的计算（偏移 +7、宽度预留 850、上翻 155）。
- 载荷里放什么：哪些候选、状态栏文字（简繁、标点、提示、页码）、各种提示语。新增要显示的数据，需要 DLL 发出来，同时还要配合 v2。
- STATIC 兜底弹窗及其点击选词，`showModeHint` 的内容。
- 进程内兜底路径：Core 与词库加载、个人词库、模糊音和容错开关的读取。
- 引擎客户端（EngineTsf.h），以及引擎协议号变更（已有“请重开应用”提示）。
- 本方案的发现与切换逻辑本身，以及 DLL 的发送版本 E。
- **过渡期**：只有加载了含本功能 DLL 的应用才能热换。alpha.35 及更早版本已经打开的应用，还需要再重开一次。

只改布局、样式、主题、字体、动画，或者对已有字段换一种展示方式：可以热换，不用重开。

**长期减少 DLL 改动的方向**：

1. 视图模型交给引擎生成：引擎返回结构化的显示数据（编码串、候选、注释、页码、状态项），DLL 只补上锚点和前台窗口后原样转发。v2 载荷就按这个结构设计，状态栏改成键值字段，不再由 DLL 拼字符串。这样新增显示内容只需要换引擎和渲染器。
2. 按键策略交给引擎：DLL 送原始按键、修饰键和上下文状态，由引擎决定是否吃掉这个键、执行什么动作（沿着 7c 的接管继续推进）。
3. 设置由引擎读取并下发，DLL 不再各自读模糊音、容错等开关。
4. DLL 最终只保留 TSF 管道、编辑会话、锚点、兜底弹窗、发现与版本协商这些很少变化的部分。

## 9. 实施步骤（每步可单独验收、可续做）

| 步骤 | 内容 | 主要文件 | 碰 TextService.cpp |
| --- | --- | --- | --- |
| RH-1（已提交） | 把 v1 写成规范：解析提成纯函数，加 `candidate_payload` 测试，行为不变 | GlassPreview.cs、CandidateView.cs、新 CandidatePayload.cs、tests | 否 |
| RH-2（已提交） | 渲染器发行记录：读取器加 `renderer_release` 测试；安装器写入与清理，加 `renderer_release_install` 测试；三处常量一致 | 新 RendererRelease.h、EngineRelease.h（抽出共用函数）、InstallSupport.ps1、install／uninstall 脚本、CMakeLists | 否 |
| RH-3（已提交） | 换代状态机（纯逻辑）加 `renderer_handoff` 测试 | 新 RendererHandoff.h、EngineDeployment.h（抽出非抛出校验辅助） | 否 |
| RH-4（已提交，待实机） | 接入 DLL：后台检查、桌面渲染器认可批准目录、每进程 standby 与边界切换、回退、开关、诊断字段 | TextService.cpp、RuntimeStatus.h（v2 2048B 槽位）、RuntimeStatusReader.cs、诊断脚本 | **是**（一次改完，先确认没人在改） |
| RH-5（已提交，待实机） | `renderer_handoff_live` 集成测试；补用户文档（含过渡期需再重开一次）和实机清单 | tests、USER_GUIDE、Windows/README、WINDOWS_VALIDATION | 否 |
| RH-6（待打包与实机） | 打包两个界面有差异的连续版本，维护者走实机清单 | 见 `WINDOWS_VALIDATION.md` 末尾清单 | 否 |

RH-1～RH-3 可以并行，RH-4 依赖前三步。每步完成后更新 `.build/renderer-handoff-progress.md`，并按 AGENTS.md 同步 PLAN／STATUS／CHANGELOG（未在实机观察到的写“待实机”）。

## 10. 已确认的决定（2026-11-23）

1. **新建 `RendererReleases` 键**，不并入 `EngineReleases`（§2）。
2. **默认开启**。HKCU `rendererHandoff=0` 只作排障用的关闭开关，只能关闭，不能指定路径（§5）。
3. **高完整性应用继续使用桌面渲染器**，与现状一致。§6 所述的注入风险保持为已知边界，本任务不处理。
4. **过渡期写入用户文档**：第一个含 RH-4 的版本装好后，已打开的应用还要再重开一次；从那以后，界面更新不用重开。RH-5 把这一点写进 USER_GUIDE 和该版本的 Windows/README 说明。
