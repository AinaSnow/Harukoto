"""Read both community CSV layouts and align explicit Completion rows by ID and keys."""
import csv
import io
import re
from pathlib import Path
from japanese_dictionary import kana, trigger


def completion_reading(text, index):
    """GroupTitle is a search key, sometimes only the suffix after a katakana prefix."""
    direct = kana(text)
    if re.fullmatch('[ぁ-ゖー]{1,32}', direct):
        return direct
    if ':' in text:
        prefix, suffix = text.split(':', 1)
        known_prefixes = {'魔土器': 'まどき', '魔科学器': 'まかがくき', '魔紋起動証': 'まもんきどうしょう'}
        if prefix in known_prefixes:
            reading = kana(index)
            if reading.startswith(known_prefixes[prefix]) and re.fullmatch('[ぁ-ゖー]{1,32}', reading):
                return reading
            raise ValueError('检索读音未包含已核对的道具前缀')
        prefix = kana(prefix)
        # A terminal dot distinguishes duplicate search keys in Completion. Only
        # strip it for this explicit all-katakana-prefix + named-suffix structure.
        reading = kana(index.removesuffix('.'))
        if re.fullmatch('[ぁ-ゖー]+', prefix) and re.fullmatch('[ぁ-ゖー]+', reading):
            if not reading.startswith(prefix):
                reading = prefix + reading
            if len(reading) <= 32: return reading
        raise ValueError('名称前缀缺少完整读音，不能只使用检索后缀')
    reading = kana(index)
    if not re.fullmatch('[ぁ-ゖー]{1,32}', reading):
        raise ValueError('没有可核对假名读音')
    return reading


def english_trigger(text):
    # Possessive apostrophes occur in official English terminology, not in IME input.
    return trigger(text.replace("'", '').replace('’', ''))


def read_completion(path):
    path = Path(path)
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('Completion CSV 超过 16 MB')
    rows = [row for row in csv.reader(io.StringIO(path.read_text(encoding='utf-8-sig'))) if row]
    header_index = next((i for i, row in enumerate(rows[:4]) if row and row[0] == '#' and 'LookupTable' in row), None)
    if header_index is None: raise ValueError('无法识别 Completion CSV 表头')
    header = rows[header_index]
    if set(header) != {'#', 'Text', 'GroupTitle', 'LookupTable', 'Group', 'Key'} or len(header) != 6:
        raise ValueError('Completion CSV 字段缺失或重复')
    result = {}
    for number, values in enumerate(rows[header_index + 1:], header_index + 2):
        if values[0] == 'int32': continue
        if len(values) != len(header): raise ValueError('Completion CSV 第 %d 行列数错误' % number)
        row = dict(zip(header, values))
        try: row_id, group, key = int(row['#']), int(row['Group']), int(row['Key'])
        except ValueError as error: raise ValueError('Completion CSV ID 或分组键无效') from error
        if row_id < 0 or group < 0 or key < 0 or row_id in result: raise ValueError('Completion CSV ID 重复或无效')
        row['Group'], row['Key'] = group, key
        result[row_id] = row
    return result


def align_terms(chinese, japanese, english, ids):
    """Select only curated IDs; refuse lookup groups, missing translations or guessed readings."""
    if len(ids) != len(set(ids)): raise ValueError('选择的定型文 ID 重复')
    result = []
    for row_id in ids:
        if any(row_id not in table for table in (chinese, japanese, english)):
            raise ValueError('定型文 %d 缺少语言对应' % row_id)
        cn, ja, en = (table[row_id] for table in (chinese, japanese, english))
        if len({(row['Group'], row['Key']) for row in (cn, ja, en)}) != 1:
            raise ValueError('定型文 %d 的分组／键不一致' % row_id)
        if any(row['LookupTable'] or not row['Text'] or row['Key'] == 0 for row in (cn, ja, en)):
            raise ValueError('定型文 %d 是索引分组或缺少文本，不能作为物品术语' % row_id)
        try: reading = completion_reading(ja['Text'], ja['GroupTitle'])
        except ValueError as error: raise ValueError('定型文 %d：%s' % (row_id, error)) from error
        result.append({'row_id': row_id, 'group': cn['Group'], 'key': cn['Key'],
                       'zh_cn': cn['Text'], 'ja': ja['Text'], 'kana': reading,
                       'pinyin': trigger(cn['GroupTitle']), 'english': en['Text']})
    return result
