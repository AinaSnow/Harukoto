# FF14 全量名称与定型文词库 0.4.0

更新：2026-11-21（0.4.0 新增技能、副本与玩家简称，见下文“0.4.0 技能、副本与玩家简称”）；0.3.1 读音修订为 2026-11-20，0.3.0 为 2026-11-10。要求晴词 **0.4.4-alpha.9 或更新版本**。本包处理固定来源的 Item、PlaceName、Completion、Action、ContentFinderCondition 五张表，按同表行 ID 对齐中日英名称；另附人工维护的玩家简称。原精选 0.2.0 保留作审核和回归基准。

## 覆盖情况

| 类别 | 国服非空记录 | 收录记录 | 排除记录 |
| --- | ---: | ---: | ---: |
| Item 物品（装备、材料、料理、药品等） | 51,225 | 50,863 | 362 |
| PlaceName 地名（区域、地点、设施等） | 5,299 | 5,295 | 4 |
| Completion 定型文（问候、组队、交易、常用术语等） | 1,541 | 1,491 | 50 |
| Action 技能（玩家可用 PvE 魔法／战技／能力） | 1,923 | 1,905 | 18 |
| ContentFinderCondition 副本／任务搜索名 | 863 | 863 | 0 |
| 既有职业 | 23 | 23 | 0 |
| 玩家简称（人工维护） | — | 51 | — |

合计 **60,491 条记录、117,507 组去重后的触发记录**（0.3.1 为 57,672／113,463）。Action 另有 43,567 条范围外记录只计数（见下文）。同名物品可能有不同 ID，记录数不等于唯一名称数。13,360 条记录有可用假名；其余 44,312 条仅提供日文表记，不生成猜测读音。

362 条国服物品在固定日文来源的同 ID 下没有名称（核对后均为国服独有物品，见下文）；4 条地名为占位符或无有效触发；50 条 Completion 为分类标题／动态索引。每个排除 ID、原名和原因都在 full/coverage.json。不会将这些记录静默当成已翻译。

“全量”指这些固定来源表在收录范围内均完整处理，不代表全游戏所有文本：任务对白、说明全文、敌人／NPC／PvP 技能、NPC 名称、后来新增版本内容仍未包括。国服固定提交为 3ba064a64e21ff62b587d886701bbe6e74d8d224（提交标记 2026.09.01.0000.0000）；日英固定提交 a67c23b00fe8cb254855d06b59845958b55d28f3 标记 7.56。两个数据版本不宣称与用户服务器完全同步。

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

海都／森都／沙都等输入便利别名由项目显式添加，不冒充官方名称，逐条保存在 input_aliases。一般中文名使用无声调全拼；Completion 优先使用其中文检索字段。其余名称由固定 pypinyin 0.55.0 自动转拼音，0.3.1 起按读音修订表纠正有证据的多音字，其余仍可能误读。英文去空格和标点；数字按单个数字转成中文拼音／英文单词。

正式名称和可用假名分别选择。同一触发对应多条时需翻页，不保证每个同名物品都在第一页。输出普通文字，不生成游戏自动翻译聊天标记。

## 导入与更新

1. 安装 alpha.9，重新打开输入应用；旧版无法完整读取本包的新容量和无假名词条。
2. 打开“晴词主题词库”，点击“预览 FF14 词库”，或解压独立 Harukoto-FF14-0.4.0.zip 后选择 manifest.json。
3. 导入，新用户手动启用；从 0.1.1／0.2.0／0.3.x 更新保留原启用状态，可回退一版。
4. 重新打开记事本或游戏，输入上述例词。管理工具仍需 Python 3.10+ 与 tkinter，日常输入不需要。

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
