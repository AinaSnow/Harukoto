# 晴词 · Harukoto

面向 Windows 的中文拼音与日语辅助输入法，离线运行。独立维护自 [Meowpuut](https://github.com/Krysta1ll/Meowpuut)，保留上游版权与 MIT 授权。

[代码仓库](https://git.hare.fun/NemophilaHare/input) · [开发进度](docs/DEVELOPMENT_STATUS.md) · [变更记录](docs/DEVELOPMENT_CHANGELOG.md) · [参与开发](CONTRIBUTING.md)

## 平台范围

| 平台 | 状态 |
| --- | --- |
| Windows x64 | 当前开发重点，提供 EXE 安装向导 |
| Android | 保留源码，当前未发布晴词 APK，待独立验证 |

本仓库已移除 macOS、iOS／iPadOS 实现和专用构建工具；历史实现可在上游及 Git 历史查询。

## 已有功能

- 中文拼音、简繁切换、候选翻页和分段选词。
- 日语罗马字词库、41 组拼音／英语日语触发词、读音标注及本地词表导入。
- 简洁矩形候选窗，自动换行，字号与宽度可调，支持远程串流显示。
- 单文件安装／更新向导，内置 PS7，安装日志及注册失败恢复。

日语活用已接入 F6 候选面板首版，仅支持整段精确匹配的基础触发词，尚待实机验收；敬语、完整日语整句和生词本仍在规划。详见[日语路线图](docs/JAPANESE_INPUT_ROADMAP.md)。

## 安装与构建

晴词开发版安装器为 Harukoto-0.4.4-alpha.33-Setup.exe，构建产物位于 .build/windows/dist。当前尚未发布 Gitea Release；请参阅 [Windows 说明](Windows/README.md)自行构建。安装后按 Win + 空格切换到“晴词 Harukoto”。

日常使用见[操作指南](docs/USER_GUIDE.md)：选词、日语表记／假名选择、F6 活用、繁简开关及常见问题。

需要 Windows x64、PS7、Python 3、Visual Studio C++ 工具、Windows SDK 和 CMake。实际安装用户无需另装 PS7。

为兼容此前 Meowpuut 开发版，保留旧 TSF GUID、内部二进制名、注册表键和数据目录，升级不创建第二个输入法。[品牌与兼容说明](docs/BRAND.md)列出了保留项。

## 开发与授权

- [开发状态及代码导航](docs/DEVELOPMENT_STATUS.md)
- [后续任务与 FF14 主题词库计划](docs/DEVELOPMENT_PLAN.md)
- [Windows 实机验收](docs/WINDOWS_VALIDATION.md)
- [Android 构建说明](Android/README.md)
- [核心与词库](docs/ENGINE.md)
- [MIT 许可证](LICENSE)与[第三方来源](THIRD_PARTY.md)

晴词是独立分支，不代表原上游的官方发行。
