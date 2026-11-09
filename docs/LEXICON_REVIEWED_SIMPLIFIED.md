# 简体显示的逐项修订

`Resources/Lexicon/reviewed-simplified-exclusions.tsv` 记录经审核的例外，每行依序为「繁体字形、完整带调拼音、要排除的简体显示、已存在的正确简体显示」。四栏必须同时匹配；这不是全域繁简转换规则。除被确认的那一笔显示外，繁体候选、其他读音、其他词组及个人词库均保留。

## 审核依据

第一批 11 笔从完整词库的同繁体、同读音、不同简体显示分组挑出；原始候选、编辑权重、来源证据和固定转换结果可用下列命令重新产生。报告仅供人工审核，不会修改词库：

```sh
python3 scripts/audit-mimo-corpus.py --resources .build/Meowpuut.app/Contents/Resources --output /tmp/mimo-review.json
```

第一批每笔都有相同繁体、读音及建议简体字形的 McBopomofo 来源记录，也与项目固定 ICU 转换结果一致。除「為 wei2」和「們 men2」外，Pinyin Pro 亦直接含建议简体字形的同调条目；这两项不能宣称已由 Pinyin Pro 独立证实同一声调。Pinyin Pro 的频率和项目排序权重是不同尺度，不能直接比较为真实使用次数。

| 繁体 | 读音 | 排除的简体显示 | 保留的简体显示 |
| --- | --- | --- | --- |
| 為 | wei2 | 為 | 为 |
| 們 | men2 | 們 | 们 |
| 裡 | li3 | 裡 | 里 |
| 係 | xi4 | 係 | 系 |
| 對於 | dui4yu2 | 对於 | 对于 |
| 治療 | zhi4liao2 | 治療 | 治疗 |
| 至於 | zhi4yu2 | 至於 | 至于 |
| 有利於 | you3li4yu2 | 有利於 | 有利于 |
| 遊 | you2 | 遊 | 游 |
| 複 | fu4 | 複 | 复 |
| 麵 | mian4 | 麵 | 面 |

此外，简体显示针对 `妳 ni3` 和 `妳好 ni3hao3` 采用「你／你好」，繁体仍保留「妳／妳好」。这是面向简体使用场景的词条显示决策，不对其他读音全局替换；[台湾教育部辞典](https://dict.revised.moe.edu.tw/dictView.jsp?ID=3027&la=1&powerMode=0)收录了其女性第二人称用法。目标字形由项目自定义词条明确提供，再按完整读音核对后移除旧显示。

对于其他词组，只在繁简字形、字数和带调读音逐字对齐，且对应位置明确为 `妳 ni3` 时，把简体显示的该字改为「你」。繁体显示保留原文；`妳 nai3` 和无法对齐的词条不做替换。若正确简体对应已存在，保留该词原有权重，不用旧字形的权重覆盖。每次构建均在资源的 `simplified-pronoun-audit.json` 记录实际处理的词条与权重；自定义词条若与规则冲突会停止构建以便审核。

可参考 [Unicode 17 Unihan 字形数据](https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip) 及中国教育部对 [《通用规范汉字表》的说明](https://www.moe.gov.cn/jyb_xwfb/xw_fbh/moe_2069/s7135/s7562/s7569/201308/t20130827_156343.html)。字形映射本身不能决定词义；台湾教育部辞典对 [「記錄」亦作「紀錄」](https://dict.revised.moe.edu.tw/dictView.jsp?ID=88669&la=0&powerMode=0) 的记录，就是需要保留变体的例子。[「對於」](https://dict.revised.moe.edu.tw/dictView.jsp?ID=48242&la=0&powerMode=0) 和 [「治療」](https://dict.revised.moe.edu.tw/dictView.jsp?ID=114294&la=0&powerMode=0) 亦可在其辞典查核。

「著／着」、「乾／干」、乾隆、紀錄／记录、俱樂部／俱乐部涉及语义或词组层面的判断，未依单字转换直接列入本批排除。其他未核定的组别仍留在审核报告，不能推论为正确或错误。

## 回归

重新构建词库后执行：

```sh
python3 Core/tests/check_reviewed_simplified.py .build/Meowpuut.app/Contents/Resources
```

测试核对每个精确项在 SQLite 和拼音、注音核心中的输出与选字，并保护 `著名`、`乾隆`、两种「紀錄」简体词形及 `俱乐部` 等有效词项；不要求任何已知错误字形在未来词库中保留。修改清单时应先核对字形、读音及正确候选确实存在，再补相应案例。
