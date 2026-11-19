# 句子语料统计

组句时用到的两份统计表由公开授权的句子语料计算得到。仓库只保存统计结果，不保存任何句子原文；语料压缩包放在 `.build/corpora/`，不纳入版本管理。

| 语料 | 用途 | 授权 |
| --- | --- | --- |
| Leipzig Corpora Collection：`zho-cn_web_2015_1M`、`zho_news_2007-2009_1M` | 书面语（网页与新闻各 100 万句） | CC BY |
| Tatoeba 中文句子导出 `cmn_sentences.tsv.bz2` | 口语短句 | CC BY 2.0 FR |

来源地址、下载日期与压缩包哈希见 [SOURCE.json](SOURCE.json)，每次生成的计数见 [collocations-report.json](collocations-report.json)。

## 生成的文件

- `Resources/Lexicon/collocations.tsv.gz`：相邻词搭配，`左词<TAB>右词<TAB>分数`。分数是折扣后的点互信息，单位为百分之一奈特；`^` 和 `$` 表示短语的开头和结尾（行首行尾或标点处）。
- `Resources/Lexicon/word-priors.tsv.gz`：词在连续文本中的频率，`词<TAB>分数<TAB>出现次数`，分数与词库权重换算后的刻度相同。

两份表都以简体词面为键，用晴词自己编译出的词表切分句子，所以与词库同步；词库删掉的词在编译时自动忽略。

## 重新生成

```powershell
python .\scripts\build-collocations.py `
  --pool .build\corpora\zho-cn_web_2015_1M.tar.gz .build\corpora\zho_news_2007-2009_1M.tar.gz `
  --pool .build\corpora\cmn_sentences.tsv.bz2
python .\Windows\compile-lexicon.py .build\windows\ime
```

需要先有一份编译好的词库（`.build/windows/ime/native-pinyin.tsv`）。最后一组 `--pool` 视为口语：搭配分数在每组内分别估计后取较大值，词频按 `--spoken-share`（默认 0.3）混合。本机 6 个进程约 5 分钟。

调参时用 `--exclude-modulus 400 12` 之类的参数留出一部分句子作开发集，正式数据不留出（报告里的 `excluded_modulus` 应为 0）。

## 局限

- 书面语料是 2007 至 2015 年的新闻和网页，较新的词和聊天、游戏用语偏少。因此词频只作有界修正：比词库权重低的部分只按一半计入，上下限为 6 奈特，出现不足 20 次的词不修正。
- 口语语料只有约 51 万词，且是翻译练习句，人名（汤姆、玛丽）偏多。
- 统计主要影响组句时词与词之间的取舍。唯一的例外在编译阶段：word-priors 的出现次数被用作“简体里确实在用”的依据，把没有简体来源、语料中也未出现的繁体独有词在简体中排到已出现的有来源同音词之后（有界，见 [ENGINE](../../docs/ENGINE.md)）；其他单独输入的同音词顺序不受语料影响。
