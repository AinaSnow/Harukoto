"""Independent adapter for documented Madamada-style explicit word triggers."""
import re
import unicodedata
from pathlib import Path

POS = {'n', 'v1', 'vk', 'vs', 'adj-i', 'adj-na', 'adv', 'prt', 'other',
       'v5k', 'v5g', 'v5s', 'v5t', 'v5n', 'v5b', 'v5m', 'v5r', 'v5u'}


def kana(text):
    return ''.join(chr(ord(c) - 0x60) if 'ァ' <= c <= 'ヶ' else c for c in text)


def trigger(text):
    text = unicodedata.normalize('NFC', text.lower())
    text = text.replace('ü', 'v').replace('u:', 'v')
    for toned in 'ǖǘǚǜ':
        text = text.replace(toned, 'v')
    text = ''.join(c for c in unicodedata.normalize('NFD', text)
                   if unicodedata.category(c) != 'Mn')
    text = re.sub(r'[\s-]', '', text)
    if not re.fullmatch('[a-z]{2,40}', text):
        raise ValueError('触发词须为 2–40 个字母，可含空格、连字符或拼音声调符号')
    return text


def label(text):
    if not 1 <= len(text) <= 32 or text[0] in '=+-@':
        raise ValueError('词或中文名须为 1–32 字，且不能以 = + - @ 开头')
    if any(c in text for c in '|,"\t\r\n') or any(ord(c) < 32 for c in text):
        raise ValueError('词或中文名含不支持的分隔符或控制字符')
    return text


def parse_word_list(path):
    path = Path(path)
    if path.stat().st_size > 8 * 1024 * 1024:
        raise ValueError('词表不得超过 8 MB')
    lines = path.read_text(encoding='utf-8-sig').splitlines()
    if not lines or not lines[0].startswith('# ') or not 1 <= len(lines[0][2:].strip()) <= 40:
        raise ValueError('首行须为 # 加空格及 1–40 字的词库名称')
    entries = {}
    count = 0
    for number, line in enumerate(lines[1:], 2):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        try:
            if len(line.encode('utf-8')) > 1024:
                raise ValueError('每行不得超过 1024 字节')
            count += 1
            if count > 100_000:
                raise ValueError('词表不得超过 100000 行词条')
            parts = [part.strip() for part in line.replace('｜', '|').replace('：', ':').split('|')]
            word = label(parts.pop(0))
            reading = parts.pop(0) if parts and ':' not in parts[0] else kana(word)
            if not re.fullmatch('[ぁ-ゖー]{1,32}', reading):
                raise ValueError('含汉字的词必须填写平假名读音（最多 32 字）')
            fields = {}
            for part in parts:
                if ':' not in part:
                    raise ValueError('属性须使用 key: value 格式')
                key, value = (s.strip() for s in part.split(':', 1))
                if key not in {'pinyin', 'english', 'chinese', 'pos', 'priority'}:
                    continue
                if key not in {'pinyin', 'english'} and key in fields:
                    raise ValueError('属性重复：' + key)
                fields.setdefault(key, []).append(value)
            pos = fields.get('pos', ['n'])[0]
            if pos not in POS:
                raise ValueError('未知词性：' + pos)
            endings = dict(zip(['v5k', 'v5g', 'v5s', 'v5t', 'v5n', 'v5b', 'v5m', 'v5r', 'v5u'], 'くぐすつぬぶむるう'))
            if pos in endings and not reading.endswith(endings[pos]):
                raise ValueError('读音结尾与五段动词词性不符')
            if pos == 'v1' and (len(reading) < 2 or reading[-1] != 'る' or reading[-2] not in 'いきぎしじちぢにひびぴみりえけげせぜてでねへべぺめれ'):
                raise ValueError('一段动词读音须以い段或え段假名加る结尾')
            if pos == 'vk' and not reading.endswith('くる') or pos == 'adj-i' and not reading.endswith('い'):
                raise ValueError('读音结尾与词性不符')
            priority = fields.get('priority', ['5000'])[0]
            if not re.fullmatch('[0-9]{1,4}', priority) or not 1 <= int(priority) <= 9999:
                raise ValueError('priority 须为 1–9999 的整数')
            chinese = label(fields['chinese'][0]) if 'chinese' in fields else ''
            for kind in ('pinyin', 'english'):
                for raw in fields.get(kind, []):
                    key = (trigger(raw), word, reading, kind, chinese)
                    entries[key] = max(entries.get(key, 0), int(priority))
        except ValueError as error:
            raise ValueError(f'{path.name}:{number}: {error}') from error
    return [(key[0], key[1], weight, *key[2:]) for key, weight in
            sorted(entries.items(), key=lambda item: (item[0][0], -item[1], item[0][1:]))]


def serialize(entries):
    return ''.join('|'.join(map(str, entry)) + '\n' for entry in entries)
