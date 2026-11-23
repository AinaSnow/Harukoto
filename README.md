# 晴词 · Harukoto

Windows 上的离线中文拼音输入法，带日语辅助输入和游戏主题词库（FF14）。不联网、不上传输入内容。

独立维护自 [Meowpuut](https://github.com/Krysta1ll/Meowpuut)，保留上游版权与 MIT 授权；晴词不代表原上游的官方发行。

**[下载试用版](https://github.com/AinaSnow/Harukoto/releases)** · [试用说明](docs/FRIEND_TESTING.md) · [使用指南](docs/USER_GUIDE.md) · [发布说明](docs/RELEASE_NOTES.md)

## 功能

**中文输入**
- 拼音整句输入，简体／繁体输出切换；空格、数字选词，`-` `=` 或方向键翻页，选到页尾自动翻页；可只选句首一部分，剩下的拼音继续组句。
- 简拼与混拼（`nh` → 你好，`zgren` → 中国人）、可选模糊音（z=zh、n=l 等按对开启）、常见手误纠正（候选下标“纠错”）。
- 选词记忆与个人词频：选过的词以后优先，误选后改选即可纠正；可添加个人词，`Ctrl+Delete` 忘记学到的词，`[` `]` 以词定字。
- 常用符号与快捷输入：`v` + 数字、日期时间（`vrq`／`vsj`）、`vfh` 列出全部符号分类；中英文标点切换（`Ctrl+.`）、单按 Shift 切换中英文、可选 Shift+空格 全角。

**日语辅助**
- 拼音或英语触发日语词，表记与假名分开选；F6 活用面板（首版）。

**主题词库**
- 可导入、启停、更新和回退的主题包。随附 FF14 词库 0.5.0：物品、地名、技能、副本、NPC、怪物、任务、坐骑、状态等 9 万余条，以及团辅、团本、幻卡等玩家用语。与常用词同音的条目已过滤，不抢日常输入首选。

**Windows 体验**
- 独立引擎（默认开启）：所有应用共用一个后台引擎，词库只加载一份，多开应用时省内存、打字更快；引擎不可用时自动改用应用内输入。
- 游戏内输入：支持游戏自带候选框的系统接口（如 FF14）。
- 候选窗字号、宽度、布局可视化调整，支持高 DPI 与远程串流显示。
- 词库与输入法更新在两段输入之间生效，多数情况下不必重开应用。
- 诊断工具 `Diagnose.cmd`（不记录输入内容）与候选来源说明 `Explain.cmd`。

## 安装

1. 从 [Releases](https://github.com/AinaSnow/Harukoto/releases) 下载 `Harukoto-<版本>-Setup.exe`，核对页面上的 SHA-256。
2. 双击安装。安装包**未签名**，Windows 提示“已保护你的电脑”时点“更多信息 → 仍要运行”，并同意管理员授权。
3. 重新打开要打字的应用，按 `Win + 空格` 切换到“晴词 Harukoto”。

系统要求：Windows 10／11 x64。启用 FF14 等主题词库目前需要另装 Python 3.10+（计划去掉这一依赖）。卸载在“设置 → 应用”中进行，个人词表和设置会保留。详见[安装说明](docs/INSTALL.md)。

## 当前状态

开发测试阶段（0.4.4-alpha），没有正式版。已完成日常中文输入（阶段一）和独立引擎（阶段二），下一步是实机验收与对外发布（阶段三），之后扩展日语能力。见[开发计划](docs/DEVELOPMENT_PLAN.md)与[开发状态](docs/DEVELOPMENT_STATUS.md)。

| 平台 | 状态 |
| --- | --- |
| Windows x64 | 开发重点，提供安装包 |
| Android | 保留源码，暂不发布 |
| macOS、iOS | 已移除 |

## 参与开发

- 构建与测试：[Windows/README.md](Windows/README.md)（Visual Studio C++ 工具、Windows SDK、CMake、Python 3、PowerShell 7）
- 开发边界与约定（给参与开发的人和 AI 工具）：[docs/AI_DEVELOPMENT_GUIDE.md](docs/AI_DEVELOPMENT_GUIDE.md)
- 贡献方式：[CONTRIBUTING.md](CONTRIBUTING.md)
- 核心与词库：[docs/ENGINE.md](docs/ENGINE.md)；中文评测：[docs/CHINESE_EVALUATION.md](docs/CHINESE_EVALUATION.md)
- 为兼容此前的 Meowpuut 开发版，保留了旧的 TSF 标识、注册表键和数据目录，见[品牌与兼容说明](docs/BRAND.md)。

## 授权

[MIT 许可证](LICENSE)。词库与第三方数据的来源和授权见 [THIRD_PARTY.md](THIRD_PARTY.md)。
