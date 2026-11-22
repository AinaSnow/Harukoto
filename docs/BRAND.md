# 晴词 · Harukoto 品牌与兼容说明

2026-11-09 起，本独立维护分支使用中文名称“晴词”、英文名称“Harukoto”。品牌展示写作“晴词 · Harukoto”，系统输入法名称为“晴词 Harukoto”。Windows 为开发重点，Android 源码保留；不再维护 macOS、iOS／iPadOS。

候选窗使用简洁矩形、细边框和浅蓝高亮，文字与读音优先。尚未设计独立新图标。旧猫形图标和实验预览属于上游遗留资源，不作为晴词的新品牌标志。

## 显示名称与内部兼容

| 项目 | 处理方式 |
| --- | --- |
| 安装器与 ZIP | Harukoto 前缀，版本 0.4.4-alpha.30 |
| 输入法列表、设置、安装向导、应用列表 | 晴词／Harukoto |
| COM／TSF GUID | 保持原值，覆盖升级已有输入法 |
| Windows 语言分类 | 由 0404（繁体中文）迁移至 0804（简体中文）；安装／卸载兼容旧条目 |
| Program Files/Meowpuut、LOCALAPPDATA/Meowpuut | 保留，继续读取用户词表、设置和日志 |
| HKCU/Software/Meowpuut 和卸载注册键 | 保留；显示名称更新，避免重复条目 |
| MeowpuutTip.dll、MeowpuutGlassPreview.exe、C++ API 和数据格式头 | 保留内部名称，避免破坏加载和词库兼容 |
| Android applicationId、Java 包名、资源标识 | 保留；显示名称更新，尚未构建新版 APK |

开始菜单设置入口更新为“晴词候选窗设置”，升级时清理旧设置快捷方式。登录启动快捷方式保持旧内部文件名以覆盖已有项。

## 来源和授权

基于 [Krysta1ll/Meowpuut](https://github.com/Krysta1ll/Meowpuut) 独立开发。保留原 MIT 版权声明与第三方授权、原 Git 历史。旧提交中的产品名和发布文件名属于历史记录，不批量改写。新功能进度以 [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md) 为准。
