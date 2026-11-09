# FF14 词条格式与审核规范 v1

全量 0.3.0 另用 ff14-bulk-audit-v1 记录程序对齐，见 [全量说明](FF14_FULL_DICTIONARY.md)，不将批量数据冒充本规范 verified 核查。此处仍用于维护精选和逐项修订。


目标是准确输入已收录术语。首批建议从职业名称开始，然后加入地名／副本；技能、玩家别名和社区表达另行审核。当前 0.2.0 包已有 204 个词条、431 组触发，含职业、道具货币、制作采集及深层迷宫术语，见 [词库清单](FF14_DICTIONARY.md)。verified 表示本轮已对照在线来源，审核人字段明确为 Codex，不宣称第二位人工审核。

资料保存在 Resources/DictionaryPackages/ff14/catalog.json。package 沿用[主题包清单 v1](DICTIONARY_PACKAGES.md)，entries 为逐词条审核记录：

| 字段 | 内容 |
| --- | --- |
| id | 稳定小写 ID；同名不同对象使用不同 ID |
| category | job 职业、action 技能、duty 副本、place 地名、character 角色、item 物品、community 社区表达 |
| zh_cn | 国服正式中文名 |
| ja、kana | 日服正式名、显式平假名读音，不能由汉字猜测读音 |
| english | 英文正式名列表，仅用于审核记录 |
| aliases | 玩家别名列表，与正式名分开；不会自动变成触发词 |
| triggers | pinyin 与 english 两个列表，显式选择允许输入的触发词 |
| game_version | 核对所针对的游戏版本，如 7.0；不能用查询日期代替 |
| sources | role、url、accessed、note；记录来源用途、HTTPS 链接、YYYY-MM-DD 核对日期和依据 |
| review | status、reviewer、date、rights_note；审核状态、审核人、审核日期、使用／分发依据 |

来源 role 可为 zh_cn、ja、kana、alias。verified 条目必须覆盖国服名、日服名和读音来源，可使用同一页面支持多个字段，但要分别解释依据。链接不代表页面内容已被核对，格式校验也不能判断翻译是否正确。

优先查阅国服／日服官方职业指南、官方游戏数据库及官方更新说明，记录具体页面；辅助社区来源需单独记录许可。不得因网页可访问就视作允许整库再分发。图标、立绘、描述全文和攻略正文不纳入本词库。来源不足、读音不明、版本差异未解决时保持 draft。

## 审核流程

1. 为对象建立稳定 ID，分别记录国服名、日服名、读音及游戏版本。
2. 记录每个字段的证据链接、日期和简短依据。玩家别名与正式名称分开；同一简称命中多个对象时保留多条。
3. 选择触发词，避免过短缩写抢占常用中文；输入引擎目前要求 2–256 个规范化英文字母。
4. 人工核对对应关系和使用依据，填写 reviewer、date、rights_note 后才改为 verified。
5. 导出到新目录，使用词库管理窗口预览冲突，导入后手动启用；检查中文候选以及日文表记／假名上屏，再记录实机验收。

```powershell
python scripts/compile-ff14-dictionary.py Resources/DictionaryPackages/ff14/catalog.json
python scripts/compile-ff14-dictionary.py Resources/DictionaryPackages/ff14/catalog.json --output .build/ff14-reviewed
```

上述维护命令在源码仓库根目录执行，审核编译器未随普通用户安装包分发。第一条命令只校验。导出只包含 verified 词条；全为草稿或空表时拒绝生成，目标目录已存在时也拒绝覆盖。生成 manifest.json、words.txt 与 reviewed-source.json，最后一份保留本次已导出词条的来源和审核资料。导入器不会联网，不会上传任何词库。

当前运行时仍使用通用词条格式，分类、对象 ID、别名审核和来源详情保留在审核文件中，不在候选窗内展示；首批来源提示可在管理窗口查看；候选来源标记和实机验证仍待完成。
