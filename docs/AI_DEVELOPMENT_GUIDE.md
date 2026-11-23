# 开发边界与范围（给 AI 工具和参与开发的人）

本文说明晴词做什么、不做什么、代码在哪、改动时不能碰的边界。任务与进度以[开发计划](DEVELOPMENT_PLAN.md)和[开发状态](DEVELOPMENT_STATUS.md)为准，历史见[变更记录](DEVELOPMENT_CHANGELOG.md)。

## 产品范围

晴词是 Windows x64 上的**离线**中文拼音输入法，附日语辅助输入与游戏主题词库（FF14）。

- **在范围内**：Windows 10／11 x64 的 TSF 输入法；中文拼音整句输入与排序；日语触发与活用；主题词库；独立引擎；安装、诊断与设置工具。
- **暂不交付**：Android（保留源码与核心共用，不需要构建或发布）。
- **不做**：macOS、iOS（已移除）；联网功能、云同步、上传输入内容或遥测；复制没有明确授权的商业词库。

## 组成与目录

| 目录 | 内容 |
| --- | --- |
| `Core/` | C++17 中文核心：拼音解码、整句组句、排序、容错、选词学习。对外是 C API（`MeowpuutCore.h`），Android 共用 |
| `Windows/` | TSF 输入服务（`TextService.cpp`）、独立引擎（`Engine*.h`、宿主与所有者程序）、WPF 候选窗与设置（`*.cs`）、安装与诊断脚本（`*.ps1`）、主题词库管理（Python） |
| `Windows/tests/` | C++ 测试、Python 测试与 CTest 注册（`Windows/CMakeLists.txt`） |
| `Resources/` | 词库源与随包主题包（`DictionaryPackages/ff14` 等） |
| `DataSources/` | 词库构建用的固定来源、复核表、读音修订 |
| `scripts/` | 词库构建、审计、评测与基准脚本 |
| `docs/` | 设计、计划、状态与用户文档；`docs/benchmarks/` 为评测归档 |
| `Android/` | 保留的安卓键盘接入，不在当前开发范围 |

## 已有功能（改动时不能退化）

- 中文：整句拼音、简繁输出、翻页与页尾自动翻页、部分选词、简拼／混拼、模糊音、纠错（候选标“纠错”）、选词记忆与个人词频、个人词、忘记词、以词定字、v 命令与常用符号、中英文标点与 Shift 切换、全角。
- 日语：拼音／英语触发、表记与假名分选、F6 活用面板。
- 主题词库：多包导入、启停、更新、回退；FF14 词库（与常用词同音的触发会被过滤）。
- Windows：游戏内系统候选接口（UI-less）、候选窗布局编辑、高 DPI、词库热更新（组字边界生效）、独立引擎（默认开启，可在设置中关闭，不可用时回退到进程内）、诊断与候选来源说明。

## 硬性边界

- **兼容标识不改**：TSF CLSID／Profile GUID、注册表键 `Software\Meowpuut`、数据目录 `%LOCALAPPDATA%\Meowpuut`、内部二进制名保持不变，见 [BRAND.md](BRAND.md)。
- **核心 C API 只增不改**：`Core/` 只能新增函数，不改已有签名与语义（Android 共用）。
- **独立引擎协议**：改动消息格式必须升 `ProtocolVersion`（`Windows/EngineProtocol.h`），并同步安装脚本中的协议号；开关关闭时的路径必须与进程内结果逐项一致。
- **测试隔离**：测试不得写 HKLM、Program Files，不得真实安装或连接本机已安装的引擎；使用可注入的根路径与 HKCU 测试键，结束时清理进程、测试键和临时目录。
- **隐私**：不记录、不输出用户输入内容；诊断只报告状态。任何地方不得写入密钥或密码。
- **第三方数据**：新数据必须有来源、固定版本（提交或校验值）和兼容授权，记入 [THIRD_PARTY.md](../THIRD_PARTY.md)。

## 评测规则

- 调优集与各开发集（tuned、words-dev、abbrev-dev、fuzzy-dev、typo-dev）可以逐条查看、据此调参。
- **独立集（`Windows/tests/data/chinese-holdout.tsv`）不得逐条查看或据此调参**，只看总数、分类计数与名次变化条数。
- 排序、容错、词库改动必须同时报告调优集与独立集结果；不影响排序的改动要证明报告与上一版逐条相同（只允许耗时、内存、修订号、哈希字段不同）。
- 命令与集合说明见 [中文评测](CHINESE_EVALUATION.md)。

## 构建与测试

- CMake + MSVC Release 构建，CTest 全量测试；命令见 [Windows/README.md](../Windows/README.md)。
- 全量测试在 PowerShell 中运行（部分测试经资源管理器启动引擎，在虚拟化 HKCU 的环境中会失败）。
- 改动 TIP（`MeowpuutTip.dll`）后，打包前要把新 DLL 放进打包用的 `ime` 目录；词库源改动后要重建词库。

## 改动与文档

- 一次改一组相关问题；高冲突文件（`Windows/TextService.cpp`、`Core/MeowpuutCore.cpp`、`docs/DEVELOPMENT_*.md`）改之前先确认没有别人在改。
- 每批改动同时更新 DEVELOPMENT_PLAN（状态）、DEVELOPMENT_STATUS（顶部新条目：验证范围和未验证项）、DEVELOPMENT_CHANGELOG（顶部新条目），以及受影响的用户文档。
- 只在自动测试里验证过、没在真实应用里观察过的结果，写“待实机”，不写“完成”。
- 提交说明写清“什么输入原先失败，现在变成什么”以及验证依据。
