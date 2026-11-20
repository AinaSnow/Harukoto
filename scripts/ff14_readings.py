"""FF14 全量包的中文拼音触发：自动拼音、按字修订和读音修订表。"""
import csv
import re
import unicodedata
from pathlib import Path

DIGITS = 'ling yi er san si wu liu qi ba jiu'.split()
OVERRIDES = Path(__file__).resolve().parents[1] / 'DataSources/FF14/reading-overrides.tsv'
STATUSES = ('evidence', 'reviewed')
COLUMNS = ['id', 'text', 'reading', 'status', 'note']


def syllables(text):
    """逐字对齐的自动读音：汉字为 pypinyin 结果，数字为中文数字拼音，其余原样。"""
    from pypinyin import lazy_pinyin, Style  # 仅构建／审计需要；测试读取修订表不依赖 pypinyin
    text = unicodedata.normalize('NFKC', text)
    result = lazy_pinyin(text, style=Style.NORMAL, errors=lambda s: list(s))
    if len(result) != len(text): raise ValueError('拼音与字符无法逐字对齐')
    return [DIGITS[int(c)] if '0' <= c <= '9' else p for c, p in zip(text, result)]


def chinese_key(text, fixes=None):
    """fixes 为 {字符位置: 无声调拼音}；无修订时与 0.3.0 自动拼音一致。"""
    parts = syllables(text)
    for index, reading in (fixes or {}).items(): parts[index] = reading
    joined = ''.join(parts).replace('ü', 'v')
    return ''.join(c for c in unicodedata.normalize('NFKD', joined.lower()) if 'a' <= c <= 'z')


def load_overrides(path=OVERRIDES):
    """返回 {(id, text): row}；id 为 * 时适用于所有同名自动拼音记录。"""
    if not path.exists(): return {}
    result = {}
    with path.open(encoding='utf-8', newline='') as stream:
        rows = csv.DictReader((line for line in stream if not line.startswith('#')), delimiter='\t')
        if rows.fieldnames != COLUMNS: raise ValueError('读音修订表列名应为 ' + '\t'.join(COLUMNS))
        for row in rows:
            if row['status'] not in STATUSES: raise ValueError('未知修订状态：' + row['status'])
            if not re.fullmatch('[a-z]{1,256}', row['reading']): raise ValueError('修订读音须为小写无声调拼音：' + row['text'])
            key = (row['id'], row['text'])
            if key in result: raise ValueError('读音修订重复：' + row['text'])
            result[key] = row
    return result
