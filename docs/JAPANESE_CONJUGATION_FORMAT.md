# 活用词表 v1（开发接口）

本格式为晴词自有开发接口，不是 Madamada 词表格式。JSON 为源数据／审核格式，Windows TSF 加载由它编译的独立二进制文件，尚未提供活用候选交互。样例见 Resources/Lexicon/japanese-conjugation.json。

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

## Windows 运行时格式

    python scripts/compile-japanese-conjugations.py Resources/Lexicon/japanese-conjugation.json --format binary --output .build/windows/ime/japanese-conjugations.bin

Windows/compile-lexicon.py 在正常构建时生成此文件。打包包含它，安装器将它纳入版本目录与内容指纹；输入服务启动时加载一次，不在每次按键时解析文件或调用 Python。旧包没有该文件仍可安装，运行时缺失或校验失败仅停用活用辅助数据。

文件以 ASCII 标记 HARUKOTO-CONJ-1 加换行开头。数字使用 32 位小端无符号整数；字符串为字节长度加 UTF-8 内容。根部记录词条数；每条依次记录 id、表记、读音、触发词数及各组来源／触发词、形式数及各组名称／表记／读音。

限制：文件不超过 4 MiB、1–10000 条词条，每条 1–32 个触发项及 1–32 个形式，每个字符串 1–256 字节。拒绝重复 ID、重复触发项、重复形式标签、非法 UTF-8、非假名读音、辞书形不一致及尾部多余数据。损坏时不保留部分解析结果。

C++ 提供按“表记＋读音”和“来源＋触发词”查找的接口；相同表记不同读音不会混用。同一触发词可以关联多个词条。接口返回的词条指针只在下一次加载前有效。

下一步把查找结果关联候选并增加活用面板。FF14 名词主题词库仍依赖独立主题管理，不需要为每个名词填写活用规则。
