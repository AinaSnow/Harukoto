"""构建固定版本全部 Item／PlaceName／Completion 名称，输出覆盖账本，不猜日语读音。"""
import argparse
import csv
import hashlib
import json
import re
import unicodedata
from pathlib import Path

from pypinyin import lazy_pinyin, Style, __version__ as pinyin_version
from ff14_completion import completion_reading
from japanese_dictionary import kana, label, trigger
from japanese_packages import load_package

CN_REV = '3ba064a64e21ff62b587d886701bbe6e74d8d224'
JP_REV = 'a67c23b00fe8cb254855d06b59845958b55d28f3'
SHEETS = ('Item', 'PlaceName', 'Completion')
# Explicit input conveniences, not official names or source-transcribed aliases.
ALIASES = {
    'placename-27': ['haidu'], 'placename-39': ['sendu'], 'placename-51': ['shadu'],
    'completion-201': ['chucijianmian'], 'completion-203': ['nihao'],
    'completion-216': ['xiexie', 'xx'], 'completion-219': ['baoqian'],
    'completion-224': ['chujian', 'diyici'], 'completion-226': ['xinkule'],
    'completion-227': ['qingduoguanzhao', 'qdgz'],
    'completion-312': ['bangmang'], 'completion-327': ['zuge', 'zudui'],
    'completion-328': ['zuwo'], 'completion-401': ['buzhidao'],
}


def source_url(language, sheet):
    if language == 'cn':
        return f'https://raw.githubusercontent.com/thewakingsands/ffxiv-datamining-cn/{CN_REV}/{sheet}.csv'
    return f'https://raw.githubusercontent.com/xivapi/ffxiv-datamining/{JP_REV}/csv/{language}/{sheet}.csv'


def read_sheet(path, sheet):
    with Path(path).open(encoding='utf-8-sig', newline='') as stream:
        rows = csv.reader(stream)
        for _ in range(4):
            header = next(rows)
            if header and header[0] == '#': break
        else: raise ValueError('找不到表头：' + str(path))
        fields = ('Text', 'GroupTitle', 'LookupTable', 'Group', 'Key') if sheet == 'Completion' else ('Name',)
        indices = {field: header.index(field) for field in fields}
        result = {}
        for row in rows:
            if not row or row[0] == 'int32': continue
            row_id = int(row[0])
            if row_id in result: raise ValueError('重复行 ID')
            result[row_id] = {field: row[index] for field, index in indices.items()}
        return result


def clean_name(text):
    # Source map labels can contain line breaks. Preserve words, flatten whitespace.
    return label(re.sub(r'\s+', ' ', text).strip())


def input_key(text, chinese=False):
    text = unicodedata.normalize('NFKC', text)
    digits = ('ling yi er san si wu liu qi ba jiu' if chinese else
              'zero one two three four five six seven eight nine').split()
    text = ''.join(digits[int(c)] if '0' <= c <= '9' else c for c in text)
    if chinese: text = ''.join(lazy_pinyin(text, style=Style.NORMAL, errors=lambda s: list(s)))
    text = text.replace('ü', 'v')
    text = ''.join(c for c in unicodedata.normalize('NFKD', text.lower()) if 'a' <= c <= 'z')
    return trigger(text)


def build(folder, curated_path, output):
    if pinyin_version != '0.55.0': raise ValueError('请安装 pypinyin==0.55.0 以保持拼音可复现')
    if output.exists(): raise ValueError('输出目录已存在，请使用新目录')
    curated = json.loads(curated_path.read_text(encoding='utf-8'))
    known = {entry['id']: entry for entry in curated['entries']}
    pinned = Path(__file__).resolve().parents[1] / 'Resources/DictionaryPackages/ff14/full/source-checks.json'
    expected = {s['file']: s['sha256'] for s in json.loads(pinned.read_text(encoding='utf-8'))['sources']}
    records, exclusions, sources, counts = [], [], [], {}
    for sheet in SHEETS:
        tables = {}
        for lang in ('cn', 'ja', 'en'):
            path = folder / f'{lang}-{sheet}.csv'
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != expected.get(path.name): raise ValueError('固定来源摘要不匹配：' + path.name)
            sources.append({'file': path.name, 'url': source_url(lang, sheet),
                            'sha256': digest})
            tables[lang] = read_sheet(path, sheet)
        column = 'Text' if sheet == 'Completion' else 'Name'
        stats = {'source_named': 0, 'included': 0, 'excluded': 0}
        for row_id, cn in sorted(tables['cn'].items()):
            if not cn[column].strip(): continue
            stats['source_named'] += 1
            eid = sheet.lower() + '-' + str(row_id)
            ja, en = (tables[lang].get(row_id, {}) for lang in ('ja', 'en'))
            try:
                if not ja.get(column, '').strip(): raise ValueError('该 ID 缺少日文名称，不能猜译')
                if sheet == 'Completion':
                    if any(r.get('LookupTable') or r.get('Key') == '0' for r in (cn, ja)):
                        raise ValueError('动态索引／分类标题，名称由关联表提供')
                    if (cn['Group'], cn['Key']) != (ja['Group'], ja['Key']):
                        raise ValueError('中日文分组键不一致')
                chinese, japanese = clean_name(cn[column]), clean_name(ja[column])
                english = re.sub(r'\s+', ' ', en.get(column, '')).strip()
                keys = {'pinyin': [], 'english': []}
                # Completion supplies native pinyin search keys. Other sheets use
                # reproducible automatic pinyin; these are not human-reviewed readings.
                key = cn.get('GroupTitle', '') if sheet == 'Completion' else ''
                try: keys['pinyin'] = [trigger(key)] if key else [input_key(chinese, True)]
                except ValueError:
                    try: keys['pinyin'] = [input_key(chinese, True)]
                    except ValueError: pass
                if english:
                    try: keys['english'] = [input_key(english)]
                    except ValueError: pass
                keys['pinyin'] = sorted(set(keys['pinyin'] + ALIASES.get(eid, [])))
                if not any(keys.values()): raise ValueError('占位名或无有效中英文触发词')
                reading, reading_source = '', 'unavailable'
                direct = kana(japanese)
                if re.fullmatch('[ぁ-ゖー]{1,128}', direct):
                    reading, reading_source = direct, 'katakana'
                if sheet == 'Completion':
                    try:
                        reading = completion_reading(japanese, ja['GroupTitle'])
                        reading_source = 'completion-search-key'
                    except ValueError:
                        # Sentence punctuation is not part of the spoken kana.
                        # Only full sentence search keys qualify; no kanji/Latin is guessed.
                        if re.search('[。！？!?]$', japanese):
                            sentence = re.sub(r'[。！？!?、，,・\s]', '', kana(ja['GroupTitle']))
                            if re.fullmatch('[ぁ-ゖー]{1,128}', sentence):
                                reading, reading_source = sentence, 'completion-sentence-key'
                previous = known.get(eid)
                if previous and previous['ja'] == japanese and previous['zh_cn'] == chinese:
                    reading, reading_source = previous['kana'], 'curated-0.2.0'
                    for kind in keys: keys[kind] = sorted(set(keys[kind] + [trigger(x) for x in previous['triggers'][kind]]))
                records.append({'id': eid, 'zh_cn': chinese, 'ja': japanese, 'kana': reading,
                                'english': english, 'triggers': keys, 'reading_source': reading_source,
                                'input_aliases': ALIASES.get(eid, [])})
                stats['included'] += 1
            except ValueError as error:
                exclusions.append({'id': eid, 'zh_cn': cn[column], 'reason': str(error)})
                stats['excluded'] += 1
        counts[sheet] = stats
    for entry in curated['entries']:
        if entry['category'] == 'job':
            records.append({key: entry[key] for key in ('id', 'zh_cn', 'ja', 'kana', 'english', 'triggers')})
    manifest = dict(curated['package'], name='FF14 全量名称与定型文', version='0.3.0',
                    source='固定中日英 Item、PlaceName、Completion 全表按 ID 对齐；保留职业。需晴词 alpha.9+；缺假名仅输出表记，拼音自动生成；覆盖与缺口见 coverage.json。',
                    license='术语及简短定型表达，游戏数据权利归 SQUARE ENIX；无官方背书；来源、版本和排除项见随包记录。')
    lines = ['# ' + manifest['name']]
    for row in records:
        parts = [row['ja'], row['kana'] or '-', 'chinese: ' + row['zh_cn']]
        for kind, keys in row['triggers'].items():
            parts.extend(kind + ': ' + trigger(key) for key in keys)
        lines.append(' | '.join(parts))
    output.mkdir(parents=True)
    (output / manifest['words']).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    def write(name, obj):
        (output / name).write_text(json.dumps(obj, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')
    write('manifest.json', manifest)
    package = load_package(output / 'manifest.json')
    report = {'version': '0.3.0', 'scope': list(SHEETS), 'sources': counts, 'total_records': len(records),
              'trigger_rows': len(package['entries']), 'with_kana': sum(bool(r['kana']) for r in records),
              'exclusions': exclusions, 'limits': '全量指固定来源三个表的全部非空名称均已处理；缺日文或动态索引排除。非全游戏全部文本。自动拼音可能有多音字误读；无假名不伪造。'}
    write('coverage.json', report)
    write('source-checks.json', {'accessed': '2026-11-10', 'sources': sources, 'pypinyin': pinyin_version,
                               'cn_revision': CN_REV, 'ja_en_revision': JP_REV, 'ja_en_version': '7.56'})
    write('reviewed-source.json', {'format': 'ff14-bulk-audit-v1', 'review': '程序按同表 ID 对齐；未逐项人工审核，非原精选审核格式', 'entries': records})
    print(json.dumps({k: v for k, v in report.items() if k != 'exclusions'}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path)
    parser.add_argument('curated', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.sources, args.curated, args.output)
