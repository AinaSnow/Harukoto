# 数据来源与授权

晴词 · Harukoto 基于 Meowpuut 独立维护。原版权声明保留在 LICENSE，原始数据和第三方授权不因改名而变更。

## McBopomofo 词库数据（MIT）

- 上游：https://github.com/openvanilla/McBopomofo
- 固定修订：`745e8641a95d958532f0820c3ee679c2a585d8e3`
- 只导入 `BPMFBase.txt`、`BPMFMappings.txt`、`phrase.occ` 及三份 `heterophony` 表；目前编译器使用第一优先读音表。
- 原始文件、MIT 授权及逐文件 SHA-256 位于 `DataSources/McBopomofo/`。Windows 软件包附带 `Licenses/McBopomofo-MIT.txt` 与来源版本信息。
- `Resources/Lexicon/mcbopomofo-retained.tsv` 保留 23 条在上述修订中移除的常用词，并记录原始修订、读音与词频；同样遵循上游 MIT 授权。
- Meowpuut 自行解析数据、建立索引、解码、排序与输出；未导入 McBopomofo 引擎或界面。修改放在 `Resources/Lexicon`，不修改上游原始文件。

## Pinyin Pro 完整简体词表（MIT）

- 上游：https://github.com/zh-lx/pinyin-pro/tree/main/packages/data
- 只采 `packages/data/json/complete.json`，其 README 标示来源为 jieba 词表；不采 modern.json 或任何第三方执行引擎。
- 固定版本、数据提交日期、SHA-256 与 MIT 原文位于 `DataSources/PinyinPro/`。
- `SOURCE.json` 区分仓库版本与数据档提交；不将仓库更新日期宣称为每个词条的新鲜度。
- App 附带 `Licenses/PinyinPro-MIT.txt` 与来源信息。繁体新词使用固定版本的繁简转换快照，已有对应词时保留原词库字形；有歧义的字形仍需后续人工校订。

## 固定繁简转换数据（Unicode License V3）

- 编译词库时使用 ICU 78.2 的 `Traditional-Simplified` 转换及其反向结果，随项目提供已产生的固定查表；所有平台使用同一份数据，不依赖系统的 ICU 或字形转换版本。
- ICU 的 `Hans_Hant.txt` 由 CLDR 产生，原始文件保留 Apple Inc.、Unicode Inc. 及其他贡献者的版权声明。原规则、Unicode License V3 完整原文、版本与 SHA-256 存于 `DataSources/MimoNormalization/`，并随各平台软件包提供。
- `reviewed-overrides.tsv` 是喵扑人工审核的 11 笔本地修订，用以保持既有字形。来源与更新方法见 [固定转换数据说明](DataSources/MimoNormalization/README.md)。

## 其他

不打包或链接 Rime，也不使用旧 Terra Pinyin、essay、Mozc、JMdict 或 OpenCC 词表。旧数据及其分发构建产物已移除。

## UniDic 日语常用词精选（BSD-3-Clause）

- 以国立国语研究所 UniDic-CWJ 2025.12 为上游，从 UniDic-to-Mozc v1.1.0 衍生表人工挑选 50 个常用词；版本、来源、哈希与授权保留于 `DataSources/UniDic/`。
- 已选择 UniDic 多重授权中的修订 BSD 条款；衍生表及原始 UniDic 2025.12 的 BSD 声明均随 macOS/iOS 安装包附上。人工补充的汉字拼音、罗马字和排序分数不代表 UniDic 词频。Windows 词库分发须一并附上这些声明，见 `Windows/README.md`。
- 其他日语词条与罗马字组字器由本项目编写。Apple 平台执行时使用系统框架、系统 SQLite 和 C++ 标准库；构建时使用上述固定繁简转换快照，修订档可明确覆盖词面。图示使用自有双猫素材及系统 SF Symbols。

本文件适用于本次发布的源代码与安装包。项目原创代码与随附素材见根目录 LICENSE；上游词库与转换数据分别依随附的 MIT、BSD 或 Unicode 授权声明分发。

## FF14 名称与简短定型表达

全量主题包从固定版本 thewakingsands/ffxiv-datamining-cn 和 xivapi/ffxiv-datamining 对齐 Item、PlaceName、Completion 的名称／简短表达，不含物品说明、任务对白、图标或游戏程序。来源链接、SHA-256 和覆盖账本随包；游戏数据权利归 SQUARE ENIX，无官方背书，不将项目 MIT 许可套用于游戏数据。构建时使用 pypinyin 0.55.0（MIT）生成拼音，运行时不打包该库；拼音自动生成状态见全量说明。
