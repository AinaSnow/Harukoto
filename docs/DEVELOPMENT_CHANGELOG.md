# 开发变更记录

本文件记录本维护分支的开发批次；上游历史发布说明见 [RELEASE_NOTES.md](RELEASE_NOTES.md)。安装包发布和源码提交分别记录。

## 2026-11-09 · Windows 日语辅助输入第一阶段

| 提交 | 范围 |
| --- | --- |
| [`28c6f82`](https://git.hare.fun/NemophilaHare/input/commit/28c6f82) | 主要文档简体中文化 |
| [`9651ec7`](https://git.hare.fun/NemophilaHare/input/commit/9651ec7) | 日语触发词、读音、自定义词表与 MSVC 构建 |
| [`f2a0543`](https://git.hare.fun/NemophilaHare/input/commit/f2a0543) | 远程候选显示、布局、外观设置和布局验证 |
| [`3078e12`](https://git.hare.fun/NemophilaHare/input/commit/3078e12) | PS7 安装、设置入口与 MSVC 分发打包 |

- 文档：主要项目说明统一简体中文，保留日语例词、繁简对照与第三方原始授权。提交 `28c6f82`。
- Windows：修复候选窗被远程屏幕捕获排除的问题；候选改为自然宽度与自动换行，默认字号保持原值。
- 日语：新增 41 组拼音／英语触发词、候选读音和来源标记、同形异读保留及自定义词表导入。
- 开发环境：加入 MSVC 构建入口与 COM 导出定义，Release 测试保留断言；跨平台核心仅做必要兼容修改。
- 设置与安装：加入候选外观 GUI 和设置入口；安装提权沿用 PS7；打包支持 MSVC，带许可证、导入工具和构建来源信息。
- 验证：7 项 CTest、4 项 Python 测试、9 组 WPF 布局组合通过；设置页离屏渲染通过。完整跨应用／安装回滚验收仍未完成。
- 打包：本机已生成 `Meowpuut-0.4.3-japanese.1-windows-x64.zip`，预构建文件完整性校验通过；尚未发布为 Release，也未替换用户当前安装。

上述新功能并未上传为 Gitea Release；当前先提供源码和可重复构建流程。当前安装与最新源码的区别见[开发状态](DEVELOPMENT_STATUS.md)。
