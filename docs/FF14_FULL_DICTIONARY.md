# FF14 全量名称与定型文词库 0.5.0

更新：2026-11-23（0.5.0 新增 NPC、怪物、坐骑、宠物、状态、特性、PvP 技能、任务、情感动作、基础职业与玩家用语，见下文“0.5.0 NPC、任务、状态等十张表与玩家用语”）；0.4.0 为 2026-11-21，0.3.1 读音修订为 2026-11-20，0.3.0 为 2026-11-10。要求晴词 **0.4.4-alpha.9 或更新版本**。本包处理固定来源的 Item、PlaceName、Completion、Action、ContentFinderCondition 以及 ClassJob、Trait、Status、Mount、Companion、Ornament、Emote、Quest、ENpcResident、BNpcName 等表，按同表行 ID 对齐中日英名称；另附人工维护的玩家简称与用语。原精选 0.2.0 保留作审核和回归基准。

## 覆盖情况

| 类别 | 国服非空记录 | 收录记录 | 排除记录 |
| --- | ---: | ---: | ---: |
| Item 物品（装备、材料、料理、药品等） | 51,225 | 50,863 | 362 |
| PlaceName 地名（区域、地点、设施等） | 5,299 | 5,295 | 4 |
| Completion 定型文（问候、组队、交易、常用术语等） | 1,541 | 1,491 | 50 |
| Action 技能（玩家可用 PvE 魔法／战技／能力） | 1,923 | 1,905 | 18 |
| ContentFinderCondition 副本／任务搜索名 | 863 | 863 | 0 |
| 既有职业 | 23 | 23 | 0 |
| ClassJob 基础职业、生产采集职业（0.5.0） | 44 | 20 | 1（另 23 与既有职业同名） |
| PvPAction 玩家 PvP 技能（0.5.0，取自 Action 表） | 369 | 56 | 3（另 310 与 PvE 技能同名） |
| Trait 特性（0.5.0） | 682 | 454 | 0（另 228 同名） |
| Status 状态效果，有图标（0.5.0） | 4,778 | 2,621 | 94（另 2,063 同名） |
| Mount 坐骑（0.5.0） | 370 | 308 | 13（另 49 同名） |
| Companion 宠物（0.5.0） | 590 | 3 | 0（另 587 与同名物品重复） |
| Ornament 时尚配饰（0.5.0） | 56 | 0 | 0（56 条全部与同名物品重复） |
| Emote 情感动作（0.5.0） | 305 | 292 | 1（另 12 同名） |
| Quest 任务名（0.5.0） | 5,377 | 5,164 | 4（另 209 同名） |
| ENpcResident 城镇／任务 NPC（0.5.0） | 30,779 | 9,417 | 244（另 21,118 同名） |
| BNpcName 怪物、Boss（0.5.0） | 14,799 | 11,690 | 222（另 2,887 同名） |
| 玩家简称与用语（人工维护） | — | 71 | — |

合计 **90,536 条记录、176,400 组去重后的触发记录**（0.4.0 为 60,491／117,507）。“同名”指 0.5.0 新表中（中文，日文）完全相同的记录只收第一次出现的一条（含与旧表、精选职业、简称相同的），只计数不列 ID。Action 另有 43,567 条、PvPAction 另有 45,121 条（其中 45,073 条为 PvE 技能，已在 Action 计数）、Status 另有 16 条范围外记录只计数（见下文）。旧表同名物品可能有不同 ID，记录数不等于唯一名称数。21,856 条记录有可用假名；其余仅提供日文表记，不生成猜测读音。Title 称号表的固定日文来源没有日文内容，整表不收录（见 0.5.0 小节）。

362 条国服物品在固定日文来源的同 ID 下没有名称（核对后均为国服独有物品，见下文）；4 条地名为占位符或无有效触发；50 条 Completion 为分类标题／动态索引。每个排除 ID、原名和原因都在 full/coverage.json。不会将这些记录静默当成已翻译。

“全量”指这些固定来源表在收录范围内均完整处理，不代表全游戏所有文本：任务对白、说明全文、敌人／NPC 技能、称号、后来新增版本内容仍未包括。国服固定提交为 3ba064a64e21ff62b587d886701bbe6e74d8d224（提交标记 2026.09.01.0000.0000）；日英固定提交 a67c23b00fe8cb254855d06b59845958b55d28f3 标记 7.56。两个数据版本不宣称与用户服务器完全同步。

## 输入示例

| 输入 | 日文候选 |
| --- | --- |
| huifuyao / potion | ポーション |
| limusaluominsa / haidu | リムサ・ロミンサ |
| gelidaniya / sendu | グリダニア |
| wuerdaha / shadu | ウルダハ |
| nihao | こんにちは。 |
| xiexie / xx | ありがとう。 |
| qingduoguanzhao / qdgz | よろしくお願いします！ |
| xinkule | お疲れ様でした。 |
| chujian | ここに来るのは初めてです。 |
| chuansong | テレポ |
| xianfengjian / fastblade | ファストブレード（先锋剑） |
| xuechou / reprisal | リプライザル（雪仇） |
| oumijiajuejingyanzhengzhan | 絶オメガ検証戦（欧米茄绝境验证战） |
| jueou | 绝欧（主题中文）／絶オメガ |
| risui | 日随／ルーレット |
| alisai / alisaie | アリゼー（阿莉塞，NPC） |
| aohou | オポオポ（奥猴，怪物） |
| yichanluxingniao | レガシーチョコボ（遗产陆行鸟，坐骑） |
| zhaomumaoxianzhe | 求められる者（招募冒险者，任务） |
| tuanben | 团本（主题中文）／レイド |
| guangdai | 光呆（主题中文）／ヒカセン |

海都／森都／沙都等输入便利别名由项目显式添加，不冒充官方名称，逐条保存在 input_aliases。一般中文名使用无声调全拼；Completion 优先使用其中文检索字段。其余名称由固定 pypinyin 0.55.0 自动转拼音，0.3.1 起按读音修订表纠正有证据的多音字，其余仍可能误读。英文去空格和标点；数字按单个数字转成中文拼音／英文单词。

正式名称和可用假名分别选择。同一触发对应多条时需翻页，不保证每个同名物品都在第一页。输出普通文字，不生成游戏自动翻译聊天标记。

## 导入与更新

1. 安装 alpha.9，重新打开输入应用；旧版无法完整读取本包的新容量和无假名词条。
2. 打开“晴词主题词库”，点击“预览 FF14 词库”，或解压独立 Harukoto-FF14-0.5.0.zip 后选择 manifest.json。
3. 导入，新用户手动启用；从 0.1.1／0.2.0／0.3.x／0.4.0 更新保留原启用状态，可回退一版。
4. 重新打开记事本或游戏，输入上述例词。alpha.34 及更早的管理工具需要 Python 3.10+ 与 tkinter，下一个试用包起不再需要；日常输入不需要。

## 维护与复现

```powershell
python -m pip install -r scripts/requirements-ff14.txt
python scripts/fetch-ff14-sources.py .build/ff14-full-sources
python scripts/build-ff14-full.py .build/ff14-full-sources Resources/DictionaryPackages/ff14/catalog.json .build/ff14-full-new
# 读音与缺口审计（需先构建出 .build/windows/ime/native-pinyin.tsv）
python scripts/audit-ff14-readings.py .build/ff14-full-sources .build/windows/ime/native-pinyin.tsv --write-overrides --bundle .build/ff14-full-new
# 修订表有新增行时，删除 .build/ff14-full-new 后重新构建
```

下载脚本按固定链接和 SHA-256 校验，只保存到缓存；生成脚本离线执行，拒绝覆盖已有输出。仓库不提交源 CSV 的说明正文、图标等，只保存名称对应、简短定型表达和审计记录。

full/reviewed-source.json 使用 ff14-bulk-audit-v1，明确为程序对齐，未逐项人工复核；它与精选 catalog.json 的 verified 审核格式不同。来源摘要、数据版本、拼音工具版本和覆盖账本随独立 ZIP 分发。

测试对全部导出记录核对来源账本与触发集合，真实 C++ 加载全部记录；真实中文词库／SQLite 候选测试跨全表抽样，验证繁简模式、中文候选保留、无假名表记、升级和回退。未将抽样候选检查称为全部词条上屏验收；实际游戏内输入、跨 DPI 长词显示和大包持续使用体验仍待实机验收。

## alpha.10 中文名称与升级检查

alpha.10 提供主题中文正式名：wuerdaha → 乌尔达哈／烏爾達哈，同时可选ウルダハ。zhidaole 的完整知道了候选恢复首选，未完成拼音仍能分段选词。

词库仍为 0.3.0，无需重新导入。更新后必须彻底退出并重开应用；QQ 需右键托盘图标退出，只关聊天窗口不够。已观察到用户 QQ 持有旧 DLL，即使注册版本和词库均已更新，旧进程仍不能使用新功能。实机验证 xinkule 的日文／假名、wuerdaha 的繁简中文及普通中文分段；alpha.10 实机上屏仍待执行。

## 0.3.1 读音质量与缺口（FF14-03，2026-11-20）

记录数与来源不变（57,672 条、113,463 组触发），只改 796 条记录的拼音触发，故为补丁版本 0.3.1。

**缺口**：362 条物品逐条核对固定来源，日文和英文同 ID 行整行为空（所有字段为空或 0），同名在已对齐物品中 0 条，没有改名、删除或 ID 错位的证据，判为国服独有物品。按名称分组：复制品 330、节日／活动／赛事 17（东节、东方武人／佳人、群狼盛宴锦标赛、乐谱等）、冒险录 11、兑换券／教材 4。全部不收录、不猜译名；复制品若原型名已对齐，在 DataSources/FF14/item-gaps.tsv 只列原型 ID 供人工参考。

**读音**：pypinyin 对不在其词组表中的“长”默认读 zhang，长靴、长椅、长杆、长颈驼、长风等全部误读。审计按名称逐个多音字，取基础词库 native-pinyin.tsv 中覆盖该字的最长词的读音作证据：

| 结果 | 位置数 |
| --- | ---: |
| 与词证据一致 | 66,381 |
| 与词证据不一致，按证据修订 | 790（738＋48 个名称，796 条记录） |
| 需人工复核（高风险字无词覆盖、词证据冲突或证据被拒收） | 2,997（2,863 个名称） |
| 其他多音字无词覆盖（低风险，未核） | 29,864 |

修订写入 DataSources/FF14/reading-overrides.tsv（列 id／text／reading／status／note；id=* 作用于全部同名记录），构建时替换自动拼音；修订表有未匹配的行则构建报错。status=evidence 是程序按词证据判定，并经代理逐类核对（拒收跨词边界误配，如“骑士长的”中的“长的”、“米卡的”中的“卡的”，以及读音有争议的劲舞、熟成、利爪、伽玛、巴扎、之长），不是人工复核；status=reviewed 留给维护者。例：长颈驼革 changjingtuoge（原 zhangjingtuoge）、褪色晶簇 tuisejingcu（原 tuishaijingcu）、绝妙的调配用品 tiao、红海参 shen、薄礼 bo、颠茄 qie、乌勒尔 le。

每条记录的 pinyin_review 标明来源：auto 自动拼音未复核（55,425）、evidence 词证据修订（796）、reviewed 人工复核（0）、completion-search-key 定型文原生检索词（1,247）、curated-0.2.0 精选包（204）。计数在 coverage.json。

**待人工**：[读音人工复核清单](FF14_READING_REVIEW.md) 按影响面列前 50 组（如 长弓、长杖、长外衣、长须的“长”，水晶都的“都”应为 du，卡扎纳尔的“扎”，角尊／咒角的“角”），完整列表在 DataSources/FF14/reading-audit.tsv。确认后加 status=reviewed 行并重建。

验证：重建可逐字节复现；Python 测试核对每条修订都生效、来源计数、跨边界拒收（总骑士长的肖像画仍为 zhang）与缺口不收录；候选诊断工具用新包主题快照确认 changjingtuoge、tuisejingcu 出主题词，zhangjingtuoge、tuishaijingcu 不再出；0.3.0→0.3.1 更新保持启用、可回退。游戏内实机抽查未做。

## 0.4.0 技能、副本与玩家简称（FF14-04，2026-11-21）

新增两张表和一个人工简称表，旧 57,672 条记录逐条不变（新增 10 行读音修订只落在新记录上），故为次版本 0.4.0。

**来源**：与 Item 相同的两个固定提交（国服 thewakingsands/ffxiv-datamining-cn 3ba064a6，日英 xivapi/ffxiv-datamining a67c23b0 = 7.56），新增 cn／ja／en 的 Action.csv 与 ContentFinderCondition.csv 六个文件，SHA-256 写在 build-ff14-full.py 的 ADDED_PINS 和 full/source-checks.json（访问日期 2026-11-20），fetch-ff14-sources.py 照常按其下载校验。每张表的来源版本记在 coverage.json 的 sources.<表>.revision；记录类别由 ID 前缀表示（action-、contentfindercondition-、abbr-），不逐条重复版本字段。

**Action 收录范围**：国服 Action.csv 的列名是旧结构、与数据错位，不能用来筛选；改按同 ID 的日服行判断：非 PvP、类别为魔法／战技／能力（ActionCategory 2／3／4），且为玩家技能（IsPlayerAction）或有职业限制（ClassJobCategory≠0）。国服非空 45,490 行中范围内 1,923 行；范围外只计数不列 ID：敌人／NPC／剧情 41,287、PvP 417、道具／坐骑／事件等 1,863。范围内收录 1,905，排除 18：同 ID 缺日文 8、占位 1、触发全部与常用词同读音 9（阵风、士风、拍掌、哔哩哔哩、输血×2 等）。同名技能（不同等级、不同场景的同名动作）保留各自 ID。

**ContentFinderCondition**：863 行全部有同 ID 日文名，全部收录，含 PvP 场地名（只是名称，不涉及 PvP 技能重复）。

**常用词冲突**：技能名常是日常词（捕食 bushi＝不是，注药 zhuyao＝主要，保护 baohu）。主题触发会在第 3 位起插入日文候选，所以凡技能／副本的触发与基础词库同读音且该词权重 ≥3000，就丢弃这个触发。冲突表 DataSources/FF14/common-reading-collisions.tsv（230 个读音）由 audit-ff14-readings.py --write-overrides 生成，构建时离线读取；313 条记录去掉部分触发，记录在 reviewed-source.json 的 dropped_triggers；全部触发被去掉的 9 条排除。重跑审计结果不变。旧三表不做此过滤（保持 0.3.1 不变）。

**玩家简称**：DataSources/FF14/player-abbreviations.tsv，人工维护（curated），列为简称／触发拼音／日服对应说法／假名／指代／说明／状态。收录 51 条，例：绝欧→絶オメガ、绝亚→絶アレキ、绝龙诗→絶竜詩、零式、极神→極、日随→ルーレット、死宫→死者の宮殿、主坦→メインタンク、盾奶→バリアヒーラー、远敏→物理レンジ、幻化→ミラプリ、团灭→全滅。简称作为主题中文词出现，日文侧为日服玩家说法。另有 24 条 excluded 只记录决定：纯字母（MT、ST、DC、DPS、LB、T；mt 是“明天”的简拼，直接英文输入即可）、单音节（奶）、与日常词同字或同读音（奶妈、坦克、分摊、分散、死刑、副本、主线、仇恨、亲信；近战＝进展、减伤＝鉴赏、练级＝联机、理符＝礼服、诗人＝私人、野队＝也对）、日服无确定对应说法（开荒），以及候选探查中让日常词降位的大区（大区从第 1 降到第 3）。

**候选探查**（harukoto_candidate_explain，--bundled、关模糊音／纠错／选词记忆，0.3.1 与 0.4.0 主题快照对比）：bushi、zhuyao、baohu 前 5 项完全不变；lingshi 前 3 仍为零食／领事／零时，零式在第 4；jishen 前 2 跻身／机身不变；gudingdui、huanhua、tuanmie、dutiao 首选仍为原词；jueou、juelongshi 首选变为绝欧、绝龙诗（原来只有拆分组句），risui 日随从第 3 到第 1，aolong、guangzhan 主题词排在拆分组句前。低于阈值的技能冲突保留，例 gehou 歌喉／歌后之后出现割喉，xuechou 血仇之后出现雪仇。

**更新与回退**：0.3.1→0.4.0 导入保持启用，回退得到 0.3.1（113,463 组触发）；测试用新包去掉新类别合成 0.3.1，与真实 0.3.1 词表逐字节相同。重建逐字节一致。

**大小与加载**：ff14-words.txt 8,147,287→8,475,231 字节（+4.0%），reviewed-source.json 17,300,635→18,088,854 字节，coverage.json 47,924→51,693 字节；编译后触发 113,463→117,507。中文评测启动时 FF14 开启的加载约 786–815 ms→796–822 ms（三套各一次，未控制系统负载，差异在波动范围内），进程私有内存 161.9→163.2 MB。

**评测**：三套中文评测（tuned、holdout、words-dev）与 DICT-02a 报告相比，简繁 × FF14 开／关全部 12 组首选、前 7、缺失计数相同，逐条名次变化 0（独立集只看总数和变化数）；组字代价不变。报告 docs/benchmarks/chinese-*-ff1404.json。完整 CTest 60 项通过。

**未做**：游戏内实机抽查；技能说明、NPC 名称、敌人技能与 PvP 技能不收；阈值以下的技能同音冲突（如割喉、雪仇）仍会在第 3 位起出现；简称表由代理整理，日文对应说法待维护者复核（status 仍为 curated，非 reviewed）；旧三表未做常用词冲突过滤。

## 0.5.0 NPC、任务、状态等十张表与玩家用语（FF14-05，2026-11-23）

新增十张表（ClassJob、PvPAction、Trait、Status、Mount、Companion、Ornament、Emote、Quest、ENpcResident、BNpcName；PvPAction 取自已有 Action 表）并扩充玩家简称与用语。0.4.0 的 60,491 条记录逐条不变（按 ID 对比 reviewed-source.json 全字段相同；由新包去掉 FF14-05 类别和新增简称合成的词表与 0.4.0 词表逐字节相同），新增 30,043 条，故为次版本 0.5.0。

**来源**：仍为国服 thewakingsands/ffxiv-datamining-cn 3ba064a6、日英 xivapi/ffxiv-datamining a67c23b0（7.56）两个固定提交。新增 10 张表 × 中日英 30 个 CSV，SHA-256 写在 build-ff14-full.py 的 ADDED_PINS_05 与 full/source-checks.json（访问日期 2026-11-23）；fetch-ff14-sources.py 照 source-checks.json 下载校验。各表名称列先实际检查 CSV 再定：ENpcResident／BNpcName／Mount／Companion／Ornament 用 Singular，Status／Trait／Quest／Emote／ClassJob 用 Name；国服与日服表头结构不同（如 Ornament 名称列在国服是第 10 列、日服是第 2 列），一律按列名取，范围标志只读日服行。

**不收的表**：Title（称号）的固定日文来源 xivapi csv/ja/Title.csv 没有任何假名或汉字，内容是英文且与英文表同 ID 行不一致，无法对齐日文称号，整表不收录，记在 coverage.json 的 unavailable_sheets。

**范围与排除规则**（新表统一）：
- 同 ID 缺日文名排除（不猜译）。
- 国服名无汉字（代号或符号，如 N-7000、2B、？？？？、CHL P-0005）排除；国服名含假名（未汉化的日文占位，如 ガルキマセラ、ラベル削除予定）排除；日文名无假名或汉字（日文表里是英文，如 Prishe of the Distant Chains）排除。
- NPC 两表名以“？”结尾的是剧情中身份未揭示时的临时称呼（仙女？、陆行鸟？），排除。
- Status 只收日服有图标的（玩家可见），无图标 16 条只计数。PvPAction 按日服行标志 IsPvP=True，且类别为魔法／战技／能力、为玩家技能或有职业限制（同 Action 规则）；PvE 技能已在 Action 计数，其他只计数。
- **同名去重**：新表中（中文，日文）完全相同的只收第一次出现的一条，含与旧表记录、精选职业和简称相同的（例：宠物 590 条中 587 条与同名召唤物品相同，时尚配饰 56 条全部与物品相同，PvP 技能 310 条与 PvE 同名，NPC 30,779 行中 21,118 行为重复的通用称呼或同一角色的不同出场）。去重只计数（coverage.json sources.<表>.duplicates），不列 ID。
- 撞音过滤扩展到所有新表（COLLISION_SHEETS）：触发与基础词库权重≥3000 的词同读音即丢弃，冲突表由 230 扩为 1,241 个读音；1,110 条新记录去掉部分触发（例：情感动作“惊讶”jingya、PvP“防御”fangyu、状态“昏厥”hunjue 只留英文触发），1,094 条只剩英文触发；触发全部被去掉的 27 条排除（如 安娜、孟子、玄武、绝地）。

**新增计数**：NPC 9,417、怪物／Boss 11,690、任务 5,164、状态 2,621、特性 454、坐骑 308、情感动作 292、PvP 技能 56、基础与生产采集职业 20（剑术师、刻木匠、采矿工等；既有 23 个职业同名不重复）、宠物 3、时尚配饰 0。各表排除原因与条数在 coverage.json 的 exclusions（每个排除 ID 一行）。

**读音**：新名称同样用固定 pypinyin 0.55.0 自动拼音，审计（audit-ff14-readings.py）扩到新表：新增 174 行词证据修订（共 970 行），需人工复核的名称由 0.4.0 的 3,013 个增至 4,930 个（5,205 个位置），复核清单前 50 组与完整列表见 [读音复核清单](FF14_READING_REVIEW.md) 和 DataSources/FF14/reading-audit.tsv。已知 NPC 音译名中的多音字（如“缪恩”自动拼音为 mouen，人名应读 miao）不在高风险字表内，未逐个复核。审计清单中“例”的排序原依赖哈希顺序，改为按长度再按字排序，重跑结果稳定。

**玩家简称与用语**：DataSources/FF14/player-abbreviations.tsv 新增 43 行（说明以“FF14-05”开头），收录 20 条：团辅→シナジー、单刷→ソロ、团本→レイド、4人本→インスタンスダンジョン、A怪／B怪→Aモブ／Bモブ、部队房→FCハウス、个人房→個人宅、幻卡→トリプルトライアド、赛鸟→チョコボレース、风脉→風脈、回归者→復帰者、光呆→ヒカセン、钐镰→リーパー、绘灵→ピクトマンサー、拉怪→まとめ、猫娘→ミコッテ、兔娘→ヴィエラ、诗学→詩学（诗学神典石）、枪刃→ガンブレ（绝枪战士）（共 71 条；诗学、枪刃由维护者复核时决定收录，常用词 史学、强忍 仍为首选）。23 条只记录不收录：与基础词库同字（灭团、招募板、萌新、部队、雇员、满级、老板、下班、毕业、消化、豆芽），与常用词同读音（蛮神＝满身、古武＝鼓舞、散人＝三人、踩塔＝踩踏、导师＝倒是、带新＝带薪／歹心），日服无对应说法（摸了、咕了、鸽了、团减），含字母且触发是常用拼音前缀（主T、换T）。野队、开荒、大区等原有 excluded 保持。全部新增行列在[读音复核清单](FF14_READING_REVIEW.md)末尾的“FF14-05 新增玩家简称与用语”小节，维护者已于 2026-11-23 复核通过。

**行数上限**：词表 90,536 行，低于 japanese_dictionary.parse_word_list 的 100,000 行上限（余 9,464 行），没有拆包，也没有放宽上限；更新时当前＋回退两版 176,400＋117,507＝293,905 条触发，低于主题状态 500,000 条上限。之后再加大表（如对白、称号）需先评估拆分可选子包或放宽上限。

**候选探查**（harukoto_candidate_explain，--bundled、关模糊音／纠错／选词记忆，0.4.0 与 0.5.0 主题快照对比前 6 项）：bushi、zhuyao、baohu、jingya、fangyu、nihao、xiexie、shangren、maoxianzhe、shimin、laoshi、gongzhu、wangzi、yisheng、xuesheng、dashi、zhanshi、shouwei、haizi、mama、tiancai、zhanzheng、xiaoxin、xingyun、shengli、yongqi 共 26 个常用读音完全不变。18 个新简称：有同读音常用词的保持原首选（tuanfu 团服、huanka 换卡、guangdai 光带、shanlian 山链、huiling 灰领、laguai 拉怪、tuanben 团本），简称在第 2 位、日文在其后；原来只有拆分组句的由简称成为首选（danshua 单刷、sirenben 4人本、fengmai 风脉、sainiao 赛鸟、maoniang 猫娘等）。

**评测**：三套中文评测（tuned、holdout、words-dev）与 FF14-04 报告（chinese-*-ff1404.json）相比，简繁 × FF14 开／关 12 组的首选、前 7、缺失计数全部相同，逐条名次变化 0（独立集只看总数和变化条数）；组字代价不变。FF14 开时 words-dev 有 3 条前 7 列表内容变化（名次不变）：jieya 解压之后出现 捷亚／ゼーヤ，chouka 抽卡之后出现 ドロー，tuanben 团本之后出现 レイド，均排在期望词之后。报告 docs/benchmarks/chinese-*-ff1405.json。完整 CTest 69 项通过。

**大小与加载**（LF 字节）：ff14-words.txt 8,414,739→11,859,873（+40.9%），reviewed-source.json 18,088,853→26,182,133，coverage.json 51,692→118,688，source-checks.json 3,878→11,485。中文评测启动时 FF14 开启的加载 796–822 ms→974–982 ms（约 +170 ms），进程私有内存 163.2→178.3 MB（+15.1 MB），逐键中位延迟约 2.1–3.2 ms→2.7–3.9 ms（+0.5–0.8 ms，p95 +0.3–0.9 ms）；FF14 关闭时不变。管理工具 Python 读包（load_package）中位 1.87 s→2.81 s、峰值分配 80→122 MB。各一次测量，未控制系统负载。

**更新与回退**：0.4.0→0.5.0 导入保持启用，回退得到 0.4.0（117,507 组触发）。重建逐字节一致，审计重跑稳定。

**未做**：游戏内实机抽查；称号（Title 无日文来源）、任务对白、敌人技能不收；NPC 音译名多音字（如缪 miao）与新增约 1,900 个待复核名称未人工复核；新增 18 条简称及其日文说法由代理整理，待维护者复核（status 仍为 curated）；阈值以下同读音冲突仍会在第 2～3 位起出现（如 jieya 捷亚）。
