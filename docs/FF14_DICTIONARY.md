# FF14 职业与物品术语词库 0.1.1

核对日期：2026-11-09。首批包含 23 个职业及 10 个物品相关定型文术语，共 89 组显式触发词，覆盖官方列表中的常规战斗职业、青魔法师和驯兽师。游戏版本范围记录为 7.56，仅描述职业名称，不包含技能数值或攻略。

## 开启方式

安装 alpha.7 后，打开“晴词主题词库”，点击“预览 FF14 词库”→“导入此预览”→在列表选择该包并点击“启用”。重新打开输入应用后生效。新包默认关闭，更新输入法也不会自动启用或覆盖用户已导入的版本。

单独下载词库 ZIP 时，解压后在管理窗口选择 manifest.json。无需更新已支持主题包的输入核心；旧版管理窗口没有快捷按钮时也可以手动选择文件。

## 首批词条

每行三个触发入口：中文全称拼音、英文名、职业缩写。输入时使用小写；英文名称去掉空格，例如 whitemage。日文表记和假名是两个独立候选，按对应数字选择。尚未添加中文玩家俗称、简称拼音或技能／副本名称。

| 中文名称 | 日文表记 | 假名 | 拼音 | 英文／缩写 |
| --- | --- | --- | --- | --- |
| 骑士 | ナイト | ないと | qishi | Paladin / PLD |
| 战士 | 戦士 | せんし | zhanshi | Warrior / WAR |
| 暗黑骑士 | 暗黒騎士 | あんこくきし | anheiqishi | Dark Knight / DRK |
| 绝枪战士 | ガンブレイカー | がんぶれいかー | jueqiangzhanshi | Gunbreaker / GNB |
| 白魔法师 | 白魔道士 | しろまどうし | baimofashi | White Mage / WHM |
| 学者 | 学者 | がくしゃ | xuezhe | Scholar / SCH |
| 占星术士 | 占星術師 | せんせいじゅつし | zhanxingshushi | Astrologian / AST |
| 贤者 | 賢者 | けんじゃ | xianzhe | Sage / SGE |
| 武僧 | モンク | もんく | wuseng | Monk / MNK |
| 龙骑士 | 竜騎士 | りゅうきし | longqishi | Dragoon / DRG |
| 忍者 | 忍者 | にんじゃ | renzhe | Ninja / NIN |
| 武士 | 侍 | さむらい | wushi | Samurai / SAM |
| 钐镰客 | リーパー | りーぱー | shanlianke | Reaper / RPR |
| 蝰蛇剑士 | ヴァイパー | ゔぁいぱー | kuishejianshi | Viper / VPR |
| 吟游诗人 | 吟遊詩人 | ぎんゆうしじん | yinyoushiren | Bard / BRD |
| 机工士 | 機工士 | きこうし | jigongshi | Machinist / MCH |
| 舞者 | 踊り子 | おどりこ | wuzhe | Dancer / DNC |
| 黑魔法师 | 黒魔道士 | くろまどうし | heimofashi | Black Mage / BLM |
| 召唤师 | 召喚士 | しょうかんし | zhaohuanshi | Summoner / SMN |
| 赤魔法师 | 赤魔道士 | あかまどうし | chimofashi | Red Mage / RDM |
| 绘灵法师 | ピクトマンサー | ぴくとまんさー | huilingfashi | Pictomancer / PCT |
| 青魔法师 | 青魔道士 | あおまどうし | qingmofashi | Blue Mage / BLU |
| 驯兽师 | 魔獣使い | まじゅうつかい | xunshoushi | Beastmaster / BST |

## 定型文物品相关术语

按中日英 Completion 的行 ID、Group 和 Key 对齐，不按同形汉字猜测对应。以下是材料／货币相关通用术语，不是 Item 表的全量具体物品，输出是普通日文文字，不生成游戏的自动翻译聊天标记。

| 中文 | 日文 | 假名 | 拼音 | Completion ID |
| --- | --- | --- | --- | --- |
| 灵魂水晶 | ソウルクリスタル | そうるくりすたる | linghunshuijing | 1305 |
| 碎晶 | シャード | しゃーど | suijing | 1601 |
| 水晶 | クリスタル | くりすたる | shuijing | 1602 |
| 晶簇 | クラスター | くらすたー | jingcu | 1603 |
| 魔晶石 | マテリア | まてりあ | mojingshi | 1604 |
| 魔素石 | 魔晶石 | ましょうせき | mosushi | 4002 |
| 文理碎晶 | ロゴスシャード | ろごすしゃーど | wenlisuijing | 4005 |
| 失传碎晶 | ロストシャード | ろすとしゃーど | shichuansuijing | 4025 |
| 博兹雅晶簇 | ボズヤクラスター | ぼずやくらすたー | boziyajingcu | 4030 |
| 战利水晶 | トロフィークリスタル | とろふぃーくりすたる | zhanlishuijing | 4038 |

注意：中文“魔晶石”对应マテリア；日文“魔晶石”对应中文“魔素石”。日文读音取 GroupTitle 明示假名，或对纯片假名逐字转换；缺失读音的汉字项拒绝自动收录。

来源为 thewakingsands/ffxiv-datamining-cn 与 xivapi/ffxiv-datamining 的 Completion CSV；逐条记录固定提交 URL。对齐工具 scripts/ff14_completion.py 支持两种 CSV 表头，拒绝分组键不一致、索引行及读音不明条目。没有直接展开 LookupTable，也没有导入全部游戏数据。

## 来源与审核范围

- [国服官方职业指南](https://actff1.web.sdo.com/project/20190917jobguid/index.html)：中文名称与职业路径。
- [日服官方职业指南](https://jp.finalfantasyxiv.com/jobguide/battle/)和[英文官方职业指南](https://na.finalfantasyxiv.com/jobguide/battle/)：日文、英文正式名称。
- [FF14 Online Wiki](https://ff14wiki.info/)：汉字职业名的显式假名读音；踊り子读音另对照[コトバンク](https://kotobank.jp/word/%E8%B8%8A%E3%82%8A%E5%AD%90)。片假名职业名直接按字符转为平假名。
- [XIVAPI ClassJob](https://v2.xivapi.com/api/sheet/ClassJob?fields=Name,Abbreviation&limit=100)：英文名与游戏缩写。部分社区页面把暗黑骑士记为 DKN，与 ClassJob 不符，本包采用 DRK；机工士使用 MCH。

逐条链接、核对日期及方法保存在 catalog.json 和随包 reviewed-source.json；source-checks.json 记录本次来源文本摘要值。本批 verified 指 Codex 已在线交叉核对，不声称经过第二位人工审核。没有复制页面正文、图标或整库；名称、商标与游戏素材权利仍归各权利人，本包没有官方授权背书。

## 验证与后续

真实词库测试覆盖全部 89 组触发词的首屏表记／假名、简繁双模式、包关闭、中文锁定及普通中文候选不变。窗口按钮测试覆盖预览、默认关闭导入与启用；实际应用上屏和跨 DPI 桌面验收仍待执行。

后续优先补充主城／地名与常用副本，再整理技能及玩家别名；每批独立核对来源和读音，更新包版本，不修改用户自定义词表。格式及审核方法见 [FF14 词条规范](FF14_TERMS.md)。
