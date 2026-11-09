# 晴词发布说明

## 0.4.4-alpha.5（开发构建，尚未发布 Release）

- 新增本地主题包清单及命令行管理：导入预览、跨包冲突报告、独立启停、更新和上一版回退。新主题默认关闭。
- 接入 Windows TSF 启动加载；不可变快照与原子指针切换，坏文件不阻止基础输入。旧单文件导入及用户别名保持独立。
- 随包附带管理工具、示例主题和[使用说明](DICTIONARY_PACKAGES.md)。图形管理页及 FF14 数据尚未提供。
- 14 项 CTest 覆盖完整主题包工作流与原有输入回归；安装及实际 TSF 应用交互仍待验收。

## 0.4.4-alpha.4（开发构建，尚未发布 Release）

- 修复 chi 对应食べる被挤到第二页的问题；中文“吃”仍保留首选。
- 明确拼音／英语触发词增加独立假名候选，riben 可以分别输出日本或にほん，收起状态也可选择。
- 新增[Windows 操作指南](USER_GUIDE.md)，说明选词、F6 活用与繁简设置。
- 12 项 CTest 及候选布局检查通过；新包尚需实机安装和 TSF 投递验收。

## 0.4.4-alpha.3（开发构建，尚未发布 Release）

- 独立品牌：晴词 · Harukoto。
- Windows EXE 安装／更新向导、简洁候选窗、日语触发词与读音、外观设置。
- 保留旧标识和用户数据路径以兼容升级。
- 移除 Apple 平台代码与专用构建工具；Android 保留。

验证范围和安装状态见[开发状态](DEVELOPMENT_STATUS.md)，逐批改动见[开发变更记录](DEVELOPMENT_CHANGELOG.md)。

改名前的 Meowpuut 发行记录请查询[原上游](https://github.com/Krysta1ll/Meowpuut/releases)或 Git 历史，它们不是晴词的发行包。
