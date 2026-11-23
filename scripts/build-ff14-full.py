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
from ff14_readings import chinese_key, load_overrides
from japanese_dictionary import kana, label, trigger
from japanese_packages import load_package

CN_REV = '3ba064a64e21ff62b587d886701bbe6e74d8d224'
JP_REV = 'a67c23b00fe8cb254855d06b59845958b55d28f3'
# FF14-05: new sheets, in dedupe priority order (an identical Chinese/Japanese
# pair already taken by an earlier sheet or record counts as a duplicate).
NEW_SHEETS = ('ClassJob', 'PvPAction', 'Trait', 'Status', 'Mount', 'Companion', 'Ornament', 'Emote', 'Quest',
              'ENpcResident', 'BNpcName')
SHEETS = ('Item', 'PlaceName', 'Completion', 'Action', 'ContentFinderCondition') + NEW_SHEETS
SHEET_FILES = {'PvPAction': 'Action'}  # PvP skills come from the same pinned Action CSVs.
NAME_COLUMNS = {'Completion': 'Text', 'ENpcResident': 'Singular', 'BNpcName': 'Singular', 'Mount': 'Singular',
                'Companion': 'Singular', 'Ornament': 'Singular'}
NPC_SHEETS = ('ENpcResident', 'BNpcName')
# Scope flags read from the JA (xivapi) row; CN headers are older schemas.
SCOPE_FIELDS = {'Action': ('IsPvP', 'IsPlayerAction', 'ClassJobCategory', 'ActionCategory'),
                'PvPAction': ('IsPvP', 'IsPlayerAction', 'ClassJobCategory', 'ActionCategory'), 'Status': ('Icon',)}
# Pinned sheets with no usable Japanese text are recorded, not built.
UNAVAILABLE_SHEETS = {'Title': '固定日文来源 xivapi csv/ja/Title.csv 无日文（内容为英文且与英文表同 ID 行不一致），不能对齐日文称号，整表不收录'}
VERSION = '0.5.0'
# FF14-04: same pinned upstream commits as the original three sheets.
ADDED_PINS = {
    'cn-Action.csv': 'b71cb86dcb802ac86a83b3177f08163150cce86ee461c803ffbb23def59c85d6',
    'ja-Action.csv': 'fed6a3f9ff6f3bb42e094acdd176a0e4ba2d76adc4de31d95be88a1f241e59dc',
    'en-Action.csv': '2e10eb72eb961434893672215e29df98d39a909cc31d01629b95da9f656433db',
    'cn-ContentFinderCondition.csv': 'ef4af1a9bb43a24c7ca10ff7c3e24f0fda7a4fed66b8cd77925417f8207e7138',
    'ja-ContentFinderCondition.csv': '0472fa4995646feb9db711e36bafca132057da2b2a7026b9ec5ab87587ffbecb',
    'en-ContentFinderCondition.csv': 'ab282d73157101fa1b86dcb2efa73d0025b3c10caa588de0c5d41292190c4e79',
}
PIN_ACCESSED = dict.fromkeys(ADDED_PINS, '2026-11-20')
# FF14-05: same pinned commits.
ADDED_PINS_05 = {
    'cn-ClassJob.csv': '78a37e3ecf83f9263b241e65a44024696a7ad4ad3134ca39ae6d7f7254e10441',
    'cn-Trait.csv': 'bac46191c2c0287cb746c27a5fbd03d7b7abc0e444e246d2ab43b82dc2b7fee2',
    'cn-Status.csv': '7df85c10e4a22ae0c681e36c352763600e4fb630a28eca6b1124487ba4300963',
    'cn-Mount.csv': '7d3f6a86e6a8130b02becffe5e7c1932aed0309dca8b5705c1926d2e55211500',
    'cn-Companion.csv': 'cb7f02005ad7ebebfd93e4a5e7a5d471909267f93de4814d225c6fc8c70c0870',
    'cn-Ornament.csv': 'ec9795a94c02a18f197205f67d6e8388e2611eeb4d2d2dbce65205ebd83d84ea',
    'cn-Emote.csv': 'b73f548c6d4062a7a1cdecaaca221acc863da864e6b045acd945ead96b80d696',
    'cn-Quest.csv': '9fe6dce1ed40eae1b3128b146d9e2ce1c7e615d85e6d5994415020c402664446',
    'cn-ENpcResident.csv': 'a7385600e93894a9245886477c39e80dc2de5875d2fac03f9b8881d4c8d03f66',
    'cn-BNpcName.csv': '505e81326a6dcf11b8c015922c5eb9f4bc766c96e32488eda5daefcd885e6982',
    'ja-ClassJob.csv': '72ddc2a19f7b586856267dc491c313d77dd8f3878c3d10a23515b76cdd25b313',
    'ja-Trait.csv': 'f5064f6eb6c4f98a2bd69214ad84d9474eca055483940dfa65d160dd651191cd',
    'ja-Status.csv': '67aa432dd9480a60a4545134cb5650750ebd8f450ff141221bb8c4f88fb47d10',
    'ja-Mount.csv': 'fdd81e10f2c50e59169d4c84a8fa99d5908817873f9a3c7e534e21478b684e57',
    'ja-Companion.csv': '130d8232963323efa409b19dcbc11328a99d7da8fe1fe9348a620e13e46a5ab0',
    'ja-Ornament.csv': '692b73aa79f5577bdf904ca35070d17ce1027c5fda15ad5f3794472f6956e7a1',
    'ja-Emote.csv': 'a2cfbd44176a466ee0506b62b7ef7f4f8eb2c515ee436413695fbd9ccdbe322d',
    'ja-Quest.csv': '5a01b9352e0ef335ca26b0d5d6a24604a12a5d48de36dbf905219ab9a2222a13',
    'ja-ENpcResident.csv': 'e8347d58c6ba659424cdbce6524b8d7c9cf926467ef08a1782cbbd72c45de99a',
    'ja-BNpcName.csv': '489967ed12d0dcde73a98c3c33cab9ebe90388c8fdb49c58eeffb6d245acb9aa',
    'en-ClassJob.csv': 'da65a1a26fe11951fff4654a2fec1a0702375294a07ae270bbbcaf35cf22d691',
    'en-Trait.csv': '65f763cb667fada2d4aef49bc0551376819fda0aa968660a2b218e3cb70c2b91',
    'en-Status.csv': '6cf96a495ec9b577794e5b402074bf1d8a86b7bda16cabcc77bf20cfdda526ed',
    'en-Mount.csv': '28f6be970988f2d67ea2ddafdd63180533fa465dccc75a93995a4eda4c509e35',
    'en-Companion.csv': 'b02a0a246e1a53a8d0e3eabc4a064f8b10d084a435f0f022aee0170541a50824',
    'en-Ornament.csv': '370413e4c62a8fb19768f6c9109b144e7e6e2def44d307f442f6b8490bfa52e9',
    'en-Emote.csv': 'e4366a2773b611e8bcd9a47c4058f716a717ae85f9499c09cdd7285d013538ee',
    'en-Quest.csv': '5e13d3b17efef94a081b9c3b9dae15a2c7032a049c3d983f0d86ae9daca1af9d',
    'en-ENpcResident.csv': 'a32cb97aabd3e53d306d726bacbaf9ba53f31c784fec6dc355afed7848c63a03',
    'en-BNpcName.csv': '08614d48fd45ab0db1b8705425a363b71dc143cccb961a5b916723c81edc0449',
}
PIN_ACCESSED.update(dict.fromkeys(ADDED_PINS_05, '2026-11-23'))
ADDED_PINS.update(ADDED_PINS_05)
# The CN Action header names are shifted (old schema); scope flags come from
# the JA/EN (xivapi) row with the same ID.
ACTION_CATEGORIES = {'2', '3', '4'}  # 魔法、战技、能力
ABBREVIATIONS = Path(__file__).resolve().parents[1] / 'DataSources/FF14/player-abbreviations.tsv'
ABBREVIATION_COLUMNS = ['abbr', 'pinyin', 'ja', 'kana', 'target', 'note', 'status']
# Generated by audit-ff14-readings.py from the base lexicon (weight >= 3000).
COLLISIONS = Path(__file__).resolve().parents[1] / 'DataSources/FF14/common-reading-collisions.tsv'
COLLISION_COLUMNS = ['reading', 'word', 'weight', 'example']
COLLISION_SHEETS = ('Action', 'ContentFinderCondition') + NEW_SHEETS
COLLISION_REASON = '触发均与常用词同读音，不收录'


class CollisionError(ValueError):
    def __init__(self, dropped):
        super().__init__(COLLISION_REASON)
        self.dropped = dropped


def load_collisions(path=COLLISIONS):
    if not path.exists(): return {}
    with path.open(encoding='utf-8', newline='') as stream:
        reader = csv.DictReader((line for line in stream if not line.startswith('#')), delimiter='\t')
        if reader.fieldnames != COLLISION_COLUMNS: raise ValueError('冲突表列名应为 ' + '\t'.join(COLLISION_COLUMNS))
        return {row['reading']: row for row in reader}


def load_abbreviations(path=ABBREVIATIONS):
    """玩家简称（人工维护）。status=curated 收录；excluded 只记录决定，不收录。"""
    rows, seen = [], set()
    with path.open(encoding='utf-8', newline='') as stream:
        reader = csv.DictReader((line for line in stream if not line.startswith('#')), delimiter='\t')
        if reader.fieldnames != ABBREVIATION_COLUMNS: raise ValueError('简称表列名应为 ' + '\t'.join(ABBREVIATION_COLUMNS))
        for row in reader:
            if any(row[k] is None or '|' in row[k] for k in ABBREVIATION_COLUMNS): raise ValueError('简称表列数错误：' + str(row))
            if row['status'] not in ('curated', 'excluded'): raise ValueError('未知简称状态：' + row['status'])
            if not row['abbr'] or not row['note']: raise ValueError('简称和说明不能为空')
            if row['status'] == 'curated':
                if not re.fullmatch('[a-z]{4,64}', row['pinyin']): raise ValueError('简称触发须为 4 个以上小写字母：' + row['abbr'])
                if not row['ja'] or not re.fullmatch('[ぁ-ゖー]{1,128}|-', row['kana']): raise ValueError('简称日文或假名无效：' + row['abbr'])
                if not re.search('[一-鿿]', row['abbr']): raise ValueError('纯字母简称不收录：' + row['abbr'])
            key = (row['abbr'], row['pinyin'])
            if key in seen: raise ValueError('简称重复：' + row['abbr'])
            seen.add(key)
            rows.append(row)
    return rows


def out_of_scope(sheet, ja):
    """技能表只收玩家可用的 PvE／PvP 技能，状态只收有图标的；返回排除原因（只计数，不逐条列出）。"""
    if sheet == 'Status': return '无图标（玩家不可见）' if ja.get('Icon') == '0' else None
    if sheet == 'PvPAction':
        if ja.get('IsPvP') != 'True': return 'PvE 技能（见 Action）'
    elif sheet != 'Action': return None
    elif ja.get('IsPvP') == 'True': return 'PvP 技能'
    if ja.get('ActionCategory') not in ACTION_CATEGORIES: return '非魔法／战技／能力（道具、坐骑、事件等）'
    if ja.get('IsPlayerAction') != 'True' and ja.get('ClassJobCategory') == '0': return '非玩家技能（敌人／NPC／剧情）'
    return None
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


def read_sheet(path, sheet, language='cn'):
    with Path(path).open(encoding='utf-8-sig', newline='') as stream:
        rows = csv.reader(stream)
        for _ in range(4):
            header = next(rows)
            if header and header[0] == '#': break
        else: raise ValueError('找不到表头：' + str(path))
        fields = ('Text', 'GroupTitle', 'LookupTable', 'Group', 'Key') if sheet == 'Completion' else (NAME_COLUMNS.get(sheet, 'Name'),)
        if language != 'cn': fields += SCOPE_FIELDS.get(sheet, ())
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
    overrides, used = load_overrides(), set()
    collisions, collision_used = load_collisions(), set()
    pinned = Path(__file__).resolve().parents[1] / 'Resources/DictionaryPackages/ff14/full/source-checks.json'
    expected = dict(ADDED_PINS)
    expected.update({s['file']: s['sha256'] for s in json.loads(pinned.read_text(encoding='utf-8'))['sources']})
    records, exclusions, sources, counts = [], [], [], {}
    # FF14-05: (Chinese, Japanese) pairs already taken; new sheets skip them.
    seen = {(e['zh_cn'], e['ja']) for e in curated['entries'] if e['category'] == 'job'}
    seen.update((r['abbr'], label(r['ja'])) for r in load_abbreviations() if r['status'] == 'curated')
    for sheet in SHEETS:
        tables = {}
        for lang in ('cn', 'ja', 'en'):
            name = SHEET_FILES.get(sheet, sheet)
            path = folder / f'{lang}-{name}.csv'
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != expected.get(path.name): raise ValueError('固定来源摘要不匹配：' + path.name)
            if path.name not in {x['file'] for x in sources}:
                sources.append(dict({'file': path.name, 'url': source_url(lang, name), 'sha256': digest},
                                    **({'accessed': PIN_ACCESSED[path.name]} if path.name in ADDED_PINS else {})))
            tables[lang] = read_sheet(path, sheet, lang)
        column = NAME_COLUMNS.get(sheet, 'Name')
        stats = {'source_named': 0, 'included': 0, 'excluded': 0}
        if sheet in NEW_SHEETS: stats['duplicates'] = 0
        scope = {}
        for row_id, cn in sorted(tables['cn'].items()):
            if not cn[column].strip(): continue
            eid = sheet.lower() + '-' + str(row_id)
            ja, en = (tables[lang].get(row_id, {}) for lang in ('ja', 'en'))
            reason = out_of_scope(sheet, ja) if ja.get(column, '').strip() or sheet == 'PvPAction' else None
            if reason:
                scope[reason] = scope.get(reason, 0) + 1
                continue
            stats['source_named'] += 1
            try:
                if not ja.get(column, '').strip(): raise ValueError('该 ID 缺少日文名称，不能猜译')
                if sheet == 'Completion':
                    if any(r.get('LookupTable') or r.get('Key') == '0' for r in (cn, ja)):
                        raise ValueError('动态索引／分类标题，名称由关联表提供')
                    if (cn['Group'], cn['Key']) != (ja['Group'], ja['Key']):
                        raise ValueError('中日文分组键不一致')
                chinese, japanese = clean_name(cn[column]), clean_name(ja[column])
                if sheet in NEW_SHEETS:
                    if (chinese, japanese) in seen:
                        stats['duplicates'] += 1
                        continue
                    seen.add((chinese, japanese))
                    if not re.search('[一-鿿]', chinese): raise ValueError('国服名称无汉字（代号或符号）')
                    if re.search('[ぁ-ゖァ-ヺ]', chinese): raise ValueError('国服名称含假名（未汉化占位）')
                    if not re.search('[ぁ-ゖァ-ヺ一-鿿]', japanese): raise ValueError('日文名称无日文字符')
                    if sheet in NPC_SHEETS and chinese.endswith(('？', '?')): raise ValueError('身份未揭示的临时称呼')
                english = re.sub(r'\s+', ' ', en.get(column, '')).strip()
                keys = {'pinyin': [], 'english': []}
                # Completion supplies native pinyin search keys. Other sheets use
                # reproducible automatic pinyin; these are not human-reviewed readings.
                key = cn.get('GroupTitle', '') if sheet == 'Completion' else ''
                # FF14-03: reading-overrides.tsv replaces automatic pinyin
                # (evidence = base-lexicon word reading, reviewed = human review).
                fix = None if key else overrides.get((eid, chinese)) or overrides.get(('*', chinese))
                pinyin_review = 'completion-search-key' if key else fix['status'] if fix else 'auto'
                try: keys['pinyin'] = [trigger(key)] if key else [trigger(fix['reading'] if fix else chinese_key(chinese))]
                except ValueError:
                    pinyin_review = 'auto'
                    try: keys['pinyin'] = [trigger(chinese_key(chinese))]
                    except ValueError: pass
                if fix: used.add((fix['id'], fix['text']))
                if english:
                    try: keys['english'] = [input_key(english)]
                    except ValueError: pass
                keys['pinyin'] = sorted(set(keys['pinyin'] + ALIASES.get(eid, [])))
                if not any(keys.values()): raise ValueError('占位名或无有效中英文触发词')
                # FF14-04: skill/duty names are often everyday words (捕食 bushi,
                # 注药 zhuyao). Triggers equal to a common base-lexicon reading are
                # dropped so they cannot add Japanese candidates to everyday input.
                dropped = {}
                if sheet in COLLISION_SHEETS:
                    dropped = {kind: [k for k in keys[kind] if k in collisions] for kind in keys}
                    dropped = {kind: v for kind, v in dropped.items() if v}
                    for kind, v in dropped.items(): keys[kind] = [k for k in keys[kind] if k not in v]
                    if dropped: collision_used.update(k for v in dropped.values() for k in v)
                    if not any(keys.values()): raise CollisionError(dropped)
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
                    if not fix: pinyin_review = 'curated-0.2.0'
                    for kind in keys: keys[kind] = sorted(set(keys[kind] + [trigger(x) for x in previous['triggers'][kind]]))
                records.append({'id': eid, 'zh_cn': chinese, 'ja': japanese, 'kana': reading,
                                'english': english, 'triggers': keys, 'reading_source': reading_source,
                                'pinyin_review': pinyin_review, 'input_aliases': ALIASES.get(eid, []),
                                **({'dropped_triggers': dropped} if dropped else {})})
                stats['included'] += 1
                if sheet not in NEW_SHEETS: seen.add((chinese, japanese))
            except CollisionError as error:
                exclusions.append({'id': eid, 'zh_cn': cn[column], 'reason': COLLISION_REASON, 'dropped_triggers': error.dropped})
                stats['excluded'] += 1
            except ValueError as error:
                exclusions.append({'id': eid, 'zh_cn': cn[column], 'reason': str(error)})
                stats['excluded'] += 1
        stats['revision'] = {'cn': CN_REV, 'ja_en': JP_REV, 'ja_en_version': '7.56'}
        if scope: stats['out_of_scope'] = dict(sorted(scope.items()))
        counts[sheet] = stats
    stale = sorted(text for _, text in set(overrides) - used)
    if stale: raise ValueError('读音修订未匹配任何记录：' + '、'.join(stale[:20]))
    stale = sorted(set(collisions) - collision_used)
    if stale: raise ValueError('冲突表未匹配任何触发：' + '、'.join(stale[:20]))
    for entry in curated['entries']:
        if entry['category'] == 'job':
            records.append(dict({key: entry[key] for key in ('id', 'zh_cn', 'ja', 'kana', 'english', 'triggers')},
                                pinyin_review='curated-0.2.0'))
    # FF14-04: hand-curated player abbreviations; the Chinese abbreviation is the
    # theme name, the Japanese side is the JP-server player equivalent.
    abbreviations = [r for r in load_abbreviations() if r['status'] == 'curated']
    for row in abbreviations:
        records.append({'id': 'abbr-' + row['pinyin'], 'zh_cn': row['abbr'], 'ja': label(row['ja']),
                        'kana': '' if row['kana'] == '-' else row['kana'], 'english': '',
                        'triggers': {'pinyin': [row['pinyin']], 'english': []}, 'reading_source': 'curated-abbreviation',
                        'pinyin_review': 'curated-abbreviation', 'input_aliases': [], 'target': row['target']})
    if len({r['id'] for r in records}) != len(records): raise ValueError('记录 ID 重复')
    manifest = dict(curated['package'], name='FF14 全量名称与定型文', version=VERSION,
                    source='固定中日英 Item、PlaceName、Completion、Action（玩家 PvE／PvP 技能）、ContentFinderCondition（副本）、ClassJob、Trait、Status、Mount、Companion、Ornament、Emote、Quest、ENpcResident、BNpcName 按 ID 对齐，新表同名去重；保留职业；另附人工维护玩家简称与用语。需晴词 alpha.9+；缺假名仅输出表记；拼音自动生成，部分依基础词库词读音修订；覆盖与缺口见 coverage.json。',
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
    review_counts = {}
    for row in records: review_counts[row['pinyin_review']] = review_counts.get(row['pinyin_review'], 0) + 1
    report = {'version': VERSION, 'scope': list(SHEETS), 'unavailable_sheets': UNAVAILABLE_SHEETS, 'sources': counts, 'total_records': len(records),
              'trigger_rows': len(package['entries']), 'with_kana': sum(bool(r['kana']) for r in records),
              'pinyin_review': dict(sorted(review_counts.items())), 'reading_overrides': len(overrides),
              'abbreviations': {'curated': len(abbreviations), 'source': 'DataSources/FF14/player-abbreviations.tsv'},
              'common_reading_collisions': {'readings': len(collisions), 'sheets': list(COLLISION_SHEETS),
                                            'records_with_dropped_triggers': sum('dropped_triggers' in r for r in records),
                                            'source': 'DataSources/FF14/common-reading-collisions.tsv'},
              'exclusions': exclusions, 'limits': '全量指固定来源各表在收录范围内的全部非空名称均已处理；缺日文或动态索引排除。'
              'FF14-05 新表（ClassJob 至 BNpcName）按（中文，日文）对与已收录记录去重，只计数（duplicates）；国服名无汉字、含假名（未汉化）、日文名无日文字符、NPC 名以？结尾的排除；'
              'Status 只收有图标的（玩家可见）；PvPAction 来自同一 Action 表，规则同 Action 但为 PvP。Title 整表不可用，见 unavailable_sheets。'
              'Action 只收玩家可用 PvE 魔法／战技／能力（依日服行标志），敌人／NPC／PvP 技能只计数（out_of_scope）。'
              '类别由记录 ID 前缀表示（item-、placename-、completion-、action-、contentfindercondition-、classjob-、pvpaction-、trait-、status-、mount-、companion-、ornament-、emote-、quest-、enpcresident-、bnpcname-、job-、abbr-），来源版本见 sources.*.revision。非全游戏全部文本。'
              '自动拼音可能有多音字误读；无假名不伪造。pinyin_review：auto=自动拼音未复核，evidence=依基础词库词读音修订（程序判定，非人工），'
              'reviewed=人工复核。362 条物品缺口均为国服独有（日英同 ID 整行为空），分类见 DataSources/FF14/item-gaps.tsv，不猜译名。'}
    write('coverage.json', report)
    write('source-checks.json', {'accessed': '2026-11-10', 'sources': sources, 'pypinyin': pinyin_version,
                               'cn_revision': CN_REV, 'ja_en_revision': JP_REV, 'ja_en_version': '7.56'})
    write('reviewed-source.json', {'format': 'ff14-bulk-audit-v1', 'review': '程序按同表 ID 对齐；未逐项人工审核，非原精选审核格式。pinyin_review 标明拼音来源：auto 自动对齐、evidence 基础词库词证据修订、reviewed 人工复核','entries': records})
    print(json.dumps({k: v for k, v in report.items() if k != 'exclusions'}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path)
    parser.add_argument('curated', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.sources, args.curated, args.output)
