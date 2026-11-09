# 活用词表 v1（开发接口）

本格式为晴词自有开发接口，不是 Madamada 词表格式，也不是当前 TSF 已支持的输入格式。样例见 Resources/Lexicon/japanese-conjugation.json。

根对象只包含 schema（整数 1）和 entries 数组。词条包含 id、text、reading、pos、triggers，可选 irregular。未知字段报错，便于发现拼写错误。

| 字段 | 约束 |
| --- | --- |
| id | 1–64 位 ASCII 字母、数字、连字符、下划线，文件内唯一 |
| text | 词的表记；な形容词传词干，例如静か；其余传辞书形 |
| reading | 最多 32 字的平假名读音 |
| pos | ichidan、godan、suru、kuru、i-adjective、na-adjective |
| irregular | 可选 iku、aru、ii；与词性及适用词尾校验 |
| triggers | pinyin／english 数组，规范化规则与第一阶段触发词一致 |

运行：

    python scripts/compile-japanese-conjugations.py Resources/Lexicon/japanese-conjugation.json
    python scripts/compile-japanese-conjugations.py Resources/Lexicon/japanese-conjugation.json --output .build/windows/japanese-conjugations.json

不指定输出时只校验。指定输出时先完成全部校验再原子替换文件；错误包含词条序号。输出按 id 排序，每条附 forms 数组，其中 name 为形式标签，text 为表记，reading 为对应读音。

存在义ある不生成可能／被动／使役；可愛い按普通い形容词变化，いい及相关已核对词显式指定 ii。复合词及更多不规则词仍需审核，词性由词表显式指定，本模块不自动判断一段和五段。日文语境和语义适用性不等同于词形可生成。

下一步由 C++ 加载编译结果并关联候选；在接入前，本文件只用于开发和测试，不随安装加载。FF14 名词主题词库使用独立主题管理规划，不需要为每个名词填写活用规则。
