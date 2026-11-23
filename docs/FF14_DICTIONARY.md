# FF14 职业、道具与制作术语词库 0.2.0

**当前默认随包版本已升级为 [全量包 0.5.0](FF14_FULL_DICTIONARY.md)：90,536 条记录、176,400 组触发，处理物品、地名、定型文、玩家 PvE／PvP 技能、副本、NPC、怪物、坐骑、宠物、状态、特性、任务、情感动作与基础职业表，另附 69 条人工维护的玩家简称与用语（FF14-05；0.4.0 为 60,491 条，FF14-04）；0.3.1 修订有词证据的多音字拼音，362 条缺日文物品核对为国服独有、不猜译名（FF14-03）。以下内容为 0.2.0 精选包历史审核基准。**


更新日期：2026-11-09。本版共 **204 个词条、431 组触发词**，包含 23 个职业、135 个道具／货币相关术语、39 个制作采集相关术语、7 个深层迷宫设施名称。新增 10 个按 Item ID 对齐的基础恢复药名称。

## 使用

alpha.8 安装后打开“晴词主题词库”→“预览 FF14 词库”→“导入此预览”→“启用”。已有 0.1.1 时导入会更新同一个 harukoto-ff14 包，保留启用状态，可回退。重新打开输入应用后生效。

独立 ZIP 解压后选择 manifest.json，支持主题包的旧版也能使用；无需为纯词库更新重装输入法。旧版解析器可能将中文冒号显示为英文冒号，此问题在 alpha.8 修复。游戏中输出的是普通文字，不是自动翻译聊天标记。英文触发不输入空格、连字符或所有格撇号。

## 覆盖范围与缺口

- 完成所选固定来源 Completion 的制作采集组 22、深层迷宫组 27，以及显式挑选的货币／道具 ID。
- 保留此前职业及材料术语；加入 Item 4551–4560 的恢复药、以太药及圣灵药系列。
- 金碟币（Completion 4012）当前没有可核对的完整假名，未放入运行包；记录在 coverage.json。
- 其余定型文组、动态 LookupTable、其余 Item 装备／料理／消耗品仍不在本次范围。这里的覆盖完成仅针对上述选择，不表示全游戏词库完整。
- 当前固定来源可能与玩家所在服务器版本不同；逐条记录版本范围、来源和日期，不把更新日期当作当前服务器版本证明。

## 读音与同形词处理

Completion 的 GroupTitle 是检索词，可能只含名称后半段。本版在明确的片假名前缀下补回完整读音，例如亚拉戈诗学神典石对应「あらがんとーむすとーんしがく」，不是只输出「しがく」。魔土器／魔科学器的检索词须带完整已核对前缀；其他读音不明项拒绝自动加入。

中文魔晶石对应マテリア；中文魔素石对应日文魔晶石／ましょうせき。两者保持不同条目。名称里的中文冒号保留，不再被导入器全局替换。

## 全部词条

| 中文 | 日文 | 假名 | 拼音触发 | 英文触发 |
| --- | --- | --- | --- | --- |
| 灵魂水晶 | ソウルクリスタル | そうるくりすたる | linghunshuijing | soulcrystal |
| 货币 | 通貨 | つうか | huobi | currency |
| 金币 | ギル | ぎる | jinbi | gil |
| 亚拉戈神典石 | アラガントームストーン | あらがんとーむすとーん | yalageshendianshi | allagantomestone |
| 魂晶 | アートマ | あーとま | hunjing | atma |
| 黄道武器 | ゾディアックウェポン | ぞでぃあっくうぇぽん | huangdaowuqi | zodiacweapon |
| 星光变石 | アレキサンドライト | あれきさんどらいと | xingguangbianshi | alexandrite |
| 魂灵 | アニムス | あにむす | hunling | animus |
| 新星 | ノウス | のうす | xinxing | novus |
| 同盟徽章 | 同盟記章 | どうめいきしょう | tongmenghuizhang | alliedseals |
| 怪物通缉令 | モブ手配書 | もぶてはいしょ | guaiwutongjiling | markbill |
| 亚拉戈诗学神典石 | アラガントームストーン:詩学 | あらがんとーむすとーんしがく | yalageshixueshendianshi | allagantomestoneofpoetics |
| 军票 | 軍票 | ぐんぴょう | junpiao | companyseals |
| 狼印战绩 | 対人戦績 | たいじんせんせき | langyinzhanji | wolfmarks |
| 天极 | ゼニス | ぜにす | tianji | zenith |
| 镇魂 | ネクサス | ねくさす | zhenhun | nexus |
| 兵团徽章 | セントリオ記章 | せんとりおきしょう | bingtuanhuizhang | centurioseal |
| 元灵武器 | アニマウェポン | あにまうぇぽん | yuanlingwuqi | animaweapon |
| 物品品级 | アイテムレベル | あいてむれべる | wupinpinji | itemlevel |
| 天书奇谈 | クロの空想帳 | くろのくうそうちょう | tianshuqitan | wondroustails |
| 碎晶 | シャード | しゃーど | suijing | shard |
| 水晶 | クリスタル | くりすたる | shuijing | crystal |
| 晶簇 | クラスター | くらすたー | jingcu | cluster |
| 魔晶石 | マテリア | まてりあ | mojingshi | materia |
| 精制魔晶石 | マテリア精製 | まてりあせいせい | jingzhimojingshi | materiaextraction |
| 魔晶石工艺 | マテリアクラフト | まてりあくらふと | mojingshigongyi | materiacraft |
| 市场 | マーケット | まーけっと | shichang | markets |
| 搜索道具 | マーケットサーチ | まーけっとさーち | sousuodaoju | marketsearch |
| 优质 | HQ | はいくおりてぃ | youzhi | hq |
| 珍稀 | Rare | れあ | zhenxi | unique |
| 独占 | Ex | えくすくるーしぶ | duzhan | untradable |
| 素材 | 素材 | そざい | sucai | material |
| 简易制作 | 簡易製作 | かんいせいさく | jianyizhizuo | quicksynthesis |
| 品质 | 品質 | ひんしつ | pinzhi | quality |
| 高品质 | 高品質 | こうひんしつ | gaopinzhi | highquality |
| 最高品质 | 最高品質 | さいこうひんしつ | zuigaopinzhi | highestquality |
| 修理 | 修理 | しゅうり | xiuli | repair |
| 修理品 | 修理材 | しゅうりざい | xiulipin | repairmaterials |
| 染色 | 染色 | せんしょく | ranse | geardyeing |
| 良材 | 良木 | りょうぎ | liangcai | maturetree |
| 采伐 | 伐採 | ばっさい | caifa | logging |
| 割草 | 草刈 | くさかり | gecao | harvesting |
| 矿脉 | 採掘場 | さいくつば | kuangmai | mineraldeposit |
| 采掘 | 採掘 | さいくつ | caijue | mining |
| 碎石 | 砕岩 | さいがん | suishi | quarrying |
| 钓鱼 | 釣り | つり | diaoyu | fishing |
| 镶嵌魔晶石 | マテリア装着 | まてりあそうちゃく | xiangqianmojingshi | materiamelding |
| 禁忌镶嵌 | 禁断のマテリア装着 | きんだんのまてりあそうちゃく | jinjixiangqian | advancedmateriamelding |
| 栽培 | 栽培 | さいばい | zaipei | gardening |
| 培育 | 育成 | いくせい | peiyu | raising |
| 收获 | 収穫 | しゅうかく | shouhuo | harvesting |
| 秘籍 | 秘伝書 | ひでんしょ | miji | masterrecipebook |
| 分解 | 分解 | ぶんかい | fenjie | desynthesis |
| 钓场之王 | ヌシ釣り | ぬしづり | diaochangzhiwang | bigfishing |
| 放生 | リリース | りりーす | fangsheng | release |
| 未知的采集地点 | 未知の採集場所 | みちのさいしゅうばしょ | weizhidecaijididian | unspoiledlocations |
| 收藏品 | 収集品 | しゅうしゅうひん | shoucangpin | collectable |
| 专家 | マイスター | まいすたー | zhuanjia | specialist |
| 护符 | 護符 | ごふ | hufu | dolfavors |
| 传承录 | 伝承録 | でんしょうろく | chuanchenglu | tomeofregionalfolklore |
| 大地工票 | ギャザラースクリップ | ぎゃざらーすくりっぷ | dadigongpiao | gatherersscrip |
| 巧手白票 | クラフタースクリップ:白貨 | くらふたーすくりっぷはくか | qiaoshoubaipiao | whitecraftersscrip |
| 大地白票 | ギャザラースクリップ:白貨 | ぎゃざらーすくりっぷはくか | dadibaipiao | whitegatherersscrip |
| 简易采集 | 簡易採集 | かんいさいしゅう | jianyicaiji | quickgathering |
| 制作练习 | 製作練習 | せいさくれんしゅう | zhizuolianxi | trialsynthesis |
| 天穹街振兴票 | 蒼天街振興券 | そうてんがいしんこうけん | tianqiongjiezhenxingpiao | skybuildersscrip |
| 收藏价值 | 収集価値 | しゅうしゅうかち | shoucangjiazhi | collectability |
| 巧手紫票 | クラフタースクリップ:紫貨 | くらふたーすくりっぷしか | qiaoshouzipiao | purplecraftersscrip |
| 大地紫票 | ギャザラースクリップ:紫貨 | ぎゃざらーすくりっぷしか | dadizipiao | purplegatherersscrip |
| 刺鱼 | 刺突漁 | しとつりょう | ciyu | spearfishing |
| 开拓采集 | 開拓採集 | かいたくさいしゅう | kaituocaiji | sanctuarygathering |
| 开拓制作 | 開拓製作 | かいたくせいさく | kaituozhizuo | sanctuarycrafting |
| 谢尔达莱青船币 | シェルダレースクリップ:青船貨 | しぇるだれーすくりっぷせいせんか | xieerdalaiqingchuanbi | seafarerscowrie |
| 谢尔达莱绿岛币 | シェルダレースクリップ:緑島貨 | しぇるだれーすくりっぷりょくとうか | xieerdalailvdaobi | islanderscowrie |
| 大地使者职能技能 | ギャザラーロールアクション | ぎゃざらーろーるあくしょん | dadishizhezhinengjineng | gathererroleaction |
| 巧手橙票 | クラフタースクリップ:橙貨 | くらふたーすくりっぷとうか | qiaoshouchengpiao | orangecraftersscrip |
| 大地橙票 | ギャザラースクリップ:橙貨 | ぎゃざらーすくりっぷとうか | dadichengpiao | orangegatherersscrip |
| 宇宙信用点 | コスモクレジット | こすもくれじっと | yuzhouxinyongdian | cosmocredit |
| 月球信用点 | ムーンクレジット | むーんくれじっと | yueqiuxinyongdian | lunarcredit |
| 法恩娜信用点 | パエンナクレジット | ぱえんなくれじっと | faennaxinyongdian | phaennacredit |
| 俄匊斯信用点 | オイジュスクレジット | おいじゅすくれじっと | ejusixinyongdian | oizyscredit |
| 俄匊斯无人机晶片 | オイジュス・ドローンチップ | おいじゅすどろーんちっぷ | ejusiwurenjijingpian | oizysdronebit |
| 奥克塞西亚信用点 | アウクセシアクレジット | あうくせしあくれじっと | aokesaixiyaxinyongdian | auxesiacredit |
| 奥克塞西亚无人机晶片 | アウクセシア・ドローンチップ | あうくせしあどろーんちっぷ | aokesaixiyawurenjijingpian | auxesiadronebit |
| 熟练探索任务 | マスターシップミッション | ますたーしっぷみっしょん | shuliantansuorenwu | toolmasterymission |
| 熟练点 | マスターシップポイント | ますたーしっぷぽいんと | shuliandian | toolmasterypoint |
| 传送石冢 | 転移の石塔 | てんいのせきとう | chuansongshizhong | cairnofpassage |
| 再生石冢 | 再生の石塔 | さいせいのせきとう | zaishengshizhong | cairnofreturn |
| 魔陶器：咒印解除 | 魔土器:呪印解除 | まどきじゅいんかいじょ | motaoqizhouyinjiechu | pomanderofsafety |
| 魔陶器：全景 | 魔土器:サイトロ | まどきさいとろ | motaoqiquanjing | pomanderofsight |
| 魔陶器：强化自身 | 魔土器:自己強化 | まどきじこきょうか | motaoqiqianghuazishen | pomanderofstrength |
| 魔陶器：强化防御 | 魔土器:防御強化 | まどきぼうぎょきょうか | motaoqiqianghuafangyu | pomanderofsteel |
| 魔陶器：宝箱增加 | 魔土器:宝箱増加 | まどきたからばこぞうか | motaoqibaoxiangzengjia | pomanderofaffluence |
| 魔陶器：减少敌人 | 魔土器:敵排除 | まどきてきはいじょ | motaoqijianshaodiren | pomanderofflight |
| 魔陶器：改变敌人 | 魔土器:敵変化 | まどきてきへんか | motaoqigaibiandiren | pomanderofalteration |
| 魔陶器：解咒 | 魔土器:解呪 | まどきかいじゅ | motaoqijiezhou | pomanderofpurity |
| 魔陶器：运气上升 | 魔土器:運気上昇 | まどきうんきじょうしょう | motaoqiyunqishangsheng | pomanderoffortune |
| 魔陶器：形态变化 | 魔土器:形態変化 | まどきけいたいへんか | motaoqixingtaibianhua | pomanderofwitching |
| 魔陶器：魔法效果解除 | 魔土器:魔法効果解除 | まどきまほうこうかかいじょ | motaoqimofaxiaoguojiechu | pomanderofserenity |
| 魔陶器：曼提克化 | 魔土器:マンティコア化 | まどきまんてぃこあか | motaoqimantikehua | pomanderofrage |
| 魔陶器：梦魔化 | 魔土器:サキュバス化 | まどきさきゅばすか | motaoqimengmohua | pomanderoflust |
| 魔陶器：感知宝藏 | 魔土器:財宝感知 | まどきざいほうかんち | motaoqiganzhibaozang | pomanderofintuition |
| 魔陶器：重生 | 魔土器:リレイズ | まどきりれいず | motaoqichongsheng | pomanderofraising |
| 魔陶器：基路伯化 | 魔土器:クリブ化 | まどきくりぶか | motaoqijilubohua | pomanderofresolution |
| 魔陶器：弱化敌人 | 魔土器:敵弱体 | まどきてきじゃくたい | motaoqiruohuadiren | pomanderoffrailty |
| 魔陶器：隐形 | 魔土器:バニシュ | まどきばにしゅ | motaoqiyinxing | pomanderofconcealment |
| 魔陶器：石化敌人 | 魔土器:敵石化 | まどきてきせきか | motaoqishihuadiren | pomanderofpetrification |
| 伊弗利特魔石 | イフリートの魔石 | いふりーとのませき | yifulitemoshi | infernomagicite |
| 泰坦魔石 | タイタンの魔石 | たいたんのませき | taitanmoshi | cragmagicite |
| 迦楼罗魔石 | ガルーダの魔石 | がるーだのませき | jialouluomoshi | vortexmagicite |
| 奥丁魔石 | オーディンの魔石 | おーでぃんのませき | aodingmoshi | eldermagicite |
| 传送灯笼 | 転移の灯篭 | てんいのとうろう | chuansongdenglong | beaconofpassage |
| 再生灯笼 | 再生の灯篭 | さいせいのとうろう | zaishengdenglong | beaconofreturn |
| 再生装置 | 再生装置 | さいせいそうち | zaishengzhuangzhi | pylonofreturn |
| 传送装置 | 転移装置 | てんいそうち | chuansongzhuangzhi | pylonofpassage |
| 乌内亚灵复制体 | デミクローン・ウネ | でみくろーんうね | wuneiyalingfuzhiti | uneidemiclone |
| 多加亚灵复制体 | デミクローン・ドーガ | でみくろーんどーが | duojiayalingfuzhiti | dogademiclone |
| 洋葱剑士亚灵复制体 | デミクローン・オニオンナイト | でみくろーんおにおんないと | yangcongjianshiyalingfuzhiti | onionknightdemiclone |
| 魔科学器：缓速 | 魔科学器:スロウガ | まかがくきすろうが | mokexueqihuansu | protomanderoflethargy |
| 魔科学器：大漩涡 | 魔科学器:ミールストーム | まかがくきみーるすとーむ | mokexueqidaxuanwo | protomanderofstorms |
| 魔科学器：恐慌装甲化 | 魔科学器:ドレッドノート化 | まかがくきどれっどのーとか | mokexueqikonghuangzhuangjiahua | protomanderofdread |
| 魔科学器：咒印解除 | 魔科学器:呪印解除 | まかがくきじゅいんかいじょ | mokexueqizhouyinjiechu | protomanderofsafety |
| 魔科学器：全景 | 魔科学器:サイトロ | まかがくきさいとろ | mokexueqiquanjing | protomanderofsight |
| 魔科学器：强化自身 | 魔科学器:自己強化 | まかがくきじこきょうか | mokexueqiqianghuazishen | protomanderofstrength |
| 魔科学器：强化防御 | 魔科学器:防御強化 | まかがくきぼうぎょきょうか | mokexueqiqianghuafangyu | protomanderofsteel |
| 魔科学器：宝箱增加 | 魔科学器:宝箱増加 | まかがくきたからばこぞうか | mokexueqibaoxiangzengjia | protomanderofaffluence |
| 魔科学器：减少敌人 | 魔科学器:敵排除 | まかがくきてきはいじょ | mokexueqijianshaodiren | protomanderofflight |
| 魔科学器：改变敌人 | 魔科学器:敵変化 | まかがくきてきへんか | mokexueqigaibiandiren | protomanderofalteration |
| 魔科学器：解咒 | 魔科学器:解呪 | まかがくきかいじゅ | mokexueqijiezhou | protomanderofpurity |
| 魔科学器：运气上升 | 魔科学器:運気上昇 | まかがくきうんきじょうしょう | mokexueqiyunqishangsheng | protomanderoffortune |
| 魔科学器：形态变化 | 魔科学器:形態変化 | まかがくきけいたいへんか | mokexueqixingtaibianhua | protomanderofwitching |
| 魔科学器：魔法效果解除 | 魔科学器:魔法効果解除 | まかがくきまほうこうかかいじょ | mokexueqimofaxiaoguojiechu | protomanderofserenity |
| 魔科学器：感知宝藏 | 魔科学器:財宝感知 | まかがくきざいほうかんち | mokexueqiganzhibaozang | protomanderofintuition |
| 魔科学器：重生 | 魔科学器:リレイズ | まかがくきりれいず | mokexueqichongsheng | protomanderofraising |
| 魔陶器：加速 | 魔土器:ヘイスト | まどきへいすと | motaoqijiasu | pomanderofhaste |
| 魔陶器：净化护符 | 魔土器:浄化の守り | まどきじょうかのまもり | motaoqijinghuahufu | pomanderofpurification |
| 魔陶器：朝圣的指引 | 魔土器:巡礼の導き | まどきじゅんれいのみちびき | motaoqichaoshengdezhiyin | pomanderofdevotion |
| 杜松香·宿命 | ジュニパーの香・宿命 | じゅにぱーのこうしゅくめい | dusongxiangsuming | poisonfruitincense |
| 杜松香·敏慧 | ジュニパーの香・明敏 | じゅにぱーのこうめいびん | dusongxiangminhui | mazerootincense |
| 杜松香·安宁 | ジュニパーの香・安寧 | じゅにぱーのこうあんねい | dusongxianganning | barkbalmincense |
| 再生祭坛 | 再生の祭壇 | さいせいのさいだん | zaishengjitan | altarofreturn |
| 光耀圣酒 | 光耀の聖酒 | こうようのせいしゅ | guangyaoshengjiu | sacramentalspirits |
| 光耀香油 | 光耀の香油 | こうようのこうゆ | guangyaoxiangyou | sacramentalspices |
| 光耀谷物 | 光耀の穀物 | こうようのこくもつ | guangyaoguwu | sacramentalgrain |
| 光耀蜡烛 | 光耀の蝋燭 | こうようのろうそく | guangyaolazhu | sacramentallight |
| 光耀百合 | 光耀の百合 | こうようのゆり | guangyaobaihe | sacramentallilies |
| 光耀烛台 | 光耀の燭台 | こうようのしょくだい | guangyaozhutai | votivecandelabras |
| 魔素石 | 魔晶石 | ましょうせき | mosushi | magicite |
| 文理碎晶 | ロゴスシャード | ろごすしゃーど | wenlisuijing | logogram |
| 禁地兵装 | エウレカウェポン | えうれかうぇぽん | jindibingzhuang | eurekaweapon |
| 探险币 | ベンチャースクリップ | べんちゃーすくりっぷ | tanxianbi | venture |
| 天钢工具 | スカイスチールツール | すかいすちーるつーる | tianganggongju | skysteeltool |
| 失传技能库 | ロストホルダー | ろすとほるだー | shichuanjinengku | lostfindsholster |
| 失传技能保管箱 | ロストボックス | ろすとぼっくす | shichuanjinengbaoguanxiang | lostfindscache |
| 失传碎晶 | ロストシャード | ろすとしゃーど | shichuansuijing | forgottenfragment |
| 博兹雅晶簇 | ボズヤクラスター | ぼずやくらすたー | boziyajingcu | bozjancluster |
| 义军武器 | レジスタンス・ウェポン | れじすたんすうぇぽん | yijunwuqi | resistanceweapon |
| 战利水晶 | トロフィークリスタル | とろふぃーくりすたる | zhanlishuijing | trophycrystal |
| 曼德维尔武器 | マンダヴィルウェポン | まんだゔぃるうぇぽん | mandeweierwuqi | mandervilleweapon |
| 莫雯卓越工具 | モーエンツール | もーえんつーる | mowenzhuoyuegongju | splendoroustool |
| 亚拉戈天道神典石 | アラガントームストーン:天道 | あらがんとーむすとーんてんどう | yalagetiandaoshendianshi | allagantomestoneofheliometry |
| 宇宙工具 | コスモツール | こすもつーる | yuzhougongju | cosmictool |
| 幻境武器 | ファントムウェポン | ふぁんとむうぇぽん | huanjingwuqi | phantomweapon |
| 亚拉戈数理神典石 | アラガントームストーン:数理 | あらがんとーむすとーんすうり | yalageshulishendianshi | allagantomestoneofmathematics |
| 十二城邦银币 | 十二都市銀貨 | じゅうにとしぎんか | shierchengbangyinbi | enlightenmentsilverpiece |
| 十二城邦金币 | 十二都市金貨 | じゅうにとしきんか | shierchengbangjinbi | enlightenmentgoldpiece |
| 魔纹启动凭证：力之塔 | 魔紋起動証:力の塔 | まもんきどうしょうちからのとう | mowenqidongpingzhenglizhita | sanguinecipher |
| 亚拉戈记忆神典石 | アラガントームストーン:記憶 | あらがんとーむすとーんきおく | yalagejiyishendianshi | allagantomestoneofmnemonics |
| 十二城邦白银币 | 十二都市白銀貨 | じゅうにとしはくぎんか | shierchengbangbaiyinbi | enlightenmentsilverobol |
| 十二城邦白金币 | 十二都市白金貨 | じゅうにとしはっきんか | shierchengbangbaijinbi | enlightenmentgoldobol |
| 魔之护符 | 魔の護符 | まのごふ | mozhihufu | arcaneamulet |
| 恢复药 | ポーション | ぽーしょん | huifuyao | potion |
| 高级恢复药 | ハイポーション | はいぽーしょん | gaojihuifuyao | hipotion |
| 超级恢复药 | メガポーション | めがぽーしょん | chaojihuifuyao | megapotion |
| 特级恢复药 | エクスポーション | えくすぽーしょん | tejihuifuyao | xpotion |
| 以太药 | エーテル | えーてる | yitaiyao | ether |
| 高级以太药 | ハイエーテル | はいえーてる | gaojiyitaiyao | hiether |
| 超级以太药 | メガエーテル | めがえーてる | chaojiyitaiyao | megaether |
| 特级以太药 | エクスエーテル | えくすえーてる | tejiyitaiyao | xether |
| 圣灵药 | エリクサー | えりくさー | shenglingyao | elixir |
| 高级圣灵药 | ハイエリクサー | はいえりくさー | gaojishenglingyao | hielixir |
| 占星术士 | 占星術師 | せんせいじゅつし | zhanxingshushi | astrologian / ast |
| 吟游诗人 | 吟遊詩人 | ぎんゆうしじん | yinyoushiren | bard / brd |
| 驯兽师 | 魔獣使い | まじゅうつかい | xunshoushi | beastmaster / bst |
| 黑魔法师 | 黒魔道士 | くろまどうし | heimofashi | blackmage / blm |
| 青魔法师 | 青魔道士 | あおまどうし | qingmofashi | bluemage / blu |
| 舞者 | 踊り子 | おどりこ | wuzhe | dancer / dnc |
| 暗黑骑士 | 暗黒騎士 | あんこくきし | anheiqishi | darkknight / drk |
| 龙骑士 | 竜騎士 | りゅうきし | longqishi | dragoon / drg |
| 绝枪战士 | ガンブレイカー | がんぶれいかー | jueqiangzhanshi | gunbreaker / gnb |
| 机工士 | 機工士 | きこうし | jigongshi | machinist / mch |
| 武僧 | モンク | もんく | wuseng | monk / mnk |
| 忍者 | 忍者 | にんじゃ | renzhe | ninja / nin |
| 骑士 | ナイト | ないと | qishi | paladin / pld |
| 绘灵法师 | ピクトマンサー | ぴくとまんさー | huilingfashi | pictomancer / pct |
| 钐镰客 | リーパー | りーぱー | shanlianke | reaper / rpr |
| 赤魔法师 | 赤魔道士 | あかまどうし | chimofashi | redmage / rdm |
| 贤者 | 賢者 | けんじゃ | xianzhe | sage / sge |
| 武士 | 侍 | さむらい | wushi | samurai / sam |
| 学者 | 学者 | がくしゃ | xuezhe | scholar / sch |
| 召唤师 | 召喚士 | しょうかんし | zhaohuanshi | summoner / smn |
| 蝰蛇剑士 | ヴァイパー | ゔぁいぱー | kuishejianshi | viper / vpr |
| 战士 | 戦士 | せんし | zhanshi | warrior / war |
| 白魔法师 | 白魔道士 | しろまどうし | baimofashi | whitemage / whm |

## 来源、复现与验证

职业名称对照[国服官方指南](https://actff1.web.sdo.com/project/20190917jobguid/index.html)、[日服指南](https://jp.finalfantasyxiv.com/jobguide/battle/)与英文指南。定型文使用中日英 Completion 固定提交，三语言行 ID、Group、Key 必须一致；具体物品使用国服 Item 固定提交和相同 XIVAPI 数据版本的日英 Name。逐条链接与核对方法在 reviewed-source.json，来源摘要在源码 source-checks.json。

维护脚本 scripts/expand-ff14-terms.py 从本地核对文件生成新目录中的 catalog.json 和 coverage.json；scripts/compile-ff14-dictionary.py 再导出可导入包。脚本不联网，也不覆盖原目录。所需来源文件名、固定链接与版本见来源记录。

离线复现时将固定来源保存为 cn-completion-pinned.csv、jp-completion-pinned.csv、en-completion-pinned.csv，以及 items-cn.txt（Item.csv，至少包含 4551–4560 行）、items-ja.txt、items-en.txt（指定十个 ID 的 API JSON）。日英 JSON 的 version 必须一致。命令示例：

```powershell
python scripts/expand-ff14-terms.py Resources/DictionaryPackages/ff14/catalog.json .build/ff14-sources .build/ff14-expanded-new
python scripts/compile-ff14-dictionary.py .build/ff14-expanded-new/catalog.json --output .build/ff14-compiled-new
```

源文件不提交到仓库；复现者按 source-checks.json 的固定链接获取。coverage.json 与 source-checks.json 也随独立词库 ZIP 分发；词条逐条来源保存在 reviewed-source.json。

verified 指 Codex 已按记录的方法核查，不表示第二位人工审核或获得官方授权。不复制游戏说明正文、图标或全量物品数据库。

真实词库测试覆盖全部 431 组触发的首屏表记／假名、简繁双模式、启停和普通中文隔离；另覆盖部分读音补全、所有格规范化及名称冒号保真。实际游戏内输入、跨 DPI 和人工复核仍待完成。
