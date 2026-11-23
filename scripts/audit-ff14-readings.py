"""FF14-03 审计：物品缺口分类与自动拼音多音字核对。

只读固定来源和基础词库，不联网、不猜译名。输出：
- item-gaps.tsv：缺同 ID 日文名的物品及分类依据；
- reading-audit.tsv：多音字位置与基础词库词读音不一致、冲突或无证据（高风险字）的名称；
- reading-overrides.tsv（仅 --write-overrides）：依据基础词库词读音的修订，保留已有 reviewed 行；
- FF14_READING_REVIEW.md：按影响面排序的人工复核清单（前 50 组）。
"""
import argparse
import collections
import csv
import json
import re
from pathlib import Path

from pypinyin import pinyin, Style
from pypinyin.pinyin_dict import pinyin_dict
from pypinyin.style import convert

from ff14_readings import COLUMNS, OVERRIDES, chinese_key, load_overrides, syllables

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = ROOT / 'Resources/DictionaryPackages/ff14/full'
OUT = ROOT / 'DataSources/FF14'
# 任务清单中的高频易误读多音字，复核清单优先；RISK 另含常见专名易误读字。
PRIMARY = set('行长重还乐曾给角朝调降藏弹度着了传便省差率薄解')
# 其他多音字只在词证据不一致／冲突时报告；卡、叶、血、落等在专名中几乎总按默认读音，不列入。
RISK = PRIMARY | set('数发种处只都将相当更应空难正背冲称露曲宿节结塞散尽量磨模削恶参盛似尾斗横划几假间强圈'
                     '番车什佛系扎泊剥兴觉教供担倒炮仔沉吐粘臭奔爪伽勒熟劲茄色朴啜校济缝晕载转折扇挑看漂哄')
# 代理核对后拒收的证据词：跨词边界误配或与规范读音不符，原因写入审计。
REJECT = {
    '长的': '跨词边界（骑士长的…），长应读 zhang',
    '卡的': '跨词边界（米卡的…），的应读 de',
    '要的': '跨词边界（需要的…），的应读 de',
    '之长': '“之长”可指首领（zhang），不能作为长弓等的证据',
    '劲舞': '规范读音 jin wu，词库条目为 jing',
    '熟成': '规范读音 shu cheng，shou 为口语读音',
    '利爪': '爪在此读 zhao／zhua 均常见，需人工决定是否两读',
    '伽玛': '伽马规范读 ga，词库为 jia，需人工确认',
    '大巴扎': '巴扎读音 zha／za 未定，需人工确认',
}


def heteronyms(char, cache={}):
    if char not in cache:
        cache[char] = sorted({x for x in pinyin(char, style=Style.NORMAL, heteronym=True)[0]})
    return cache[char]


def valid_syllables():
    return {convert(x, Style.NORMAL, strict=True).replace('ü', 'v')
            for value in pinyin_dict.values() for x in value.split(',')}


def load_lexicon(path, wanted):
    """读取 native-pinyin.tsv 中完整拼音的中文词（不含简拼行），按简体词聚合读音和最大权重。"""
    valid, lexicon = valid_syllables(), collections.defaultdict(dict)
    pattern = re.compile(r'([a-zv]+)([1-5])')
    with path.open(encoding='utf-8') as stream:
        if next(stream).strip() != 'MEOWPUUT-PINYIN-2': raise ValueError('不是 native-pinyin.tsv')
        for line in stream:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != 6 or parts[3] not in wanted: continue
            found = pattern.findall(parts[1])
            if len(found) != len(parts[3]) or ''.join(a + b for a, b in found) != parts[1]: continue
            reading = [a for a, _ in found]
            if parts[0] != ''.join(reading) or not all(s in valid for s in reading): continue
            key = ' '.join(reading)
            lexicon[parts[3]][key] = max(lexicon[parts[3]].get(key, 0), int(parts[4]))
    return lexicon


def evidence(text, index, lexicon):
    """覆盖该字的最长词；返回 [(词, 起点, 读音集合)]。"""
    for length in range(min(8, len(text)), 1, -1):
        found = [(text[s:s + length], s) for s in range(max(0, index - length + 1), min(index, len(text) - length) + 1)
                 if text[s:s + length] in lexicon]
        if found:
            return [(w, s, {k.split()[index - s] for k in lexicon[w]}) for w, s in found]
    return []


def audit_readings(records, lexicon, existing):
    groups = collections.defaultdict(list)
    for r in records: groups[r['zh_cn']].append(r)
    rows, overrides, counts = [], [], collections.Counter()
    for text in sorted(groups):
        ids = [r['id'] for r in groups[text]]
        try: auto = syllables(text)
        except ValueError: continue
        fixes, notes, review = {}, [], []
        for i, c in enumerate(text):
            if not '一' <= c <= '鿿' or len(heteronyms(c)) < 2: continue
            counts['positions'] += 1
            found = evidence(text, i, lexicon)
            accepted = [(w, s, rs) for w, s, rs in found if w not in REJECT]
            rejected = [w for w, _, _ in found if w in REJECT]
            reads = set().union(*(rs for _, _, rs in accepted)) if accepted else set()
            if len(reads) == 1:
                reading = next(iter(reads))
                words = '、'.join(w for w, _, _ in accepted)
                if reading == auto[i]: counts['agree'] += 1
                else:
                    counts['evidence'] += 1
                    fixes[i] = reading
                    notes.append(f'{c}@{i} {auto[i]}→{reading}（词：{words}）')
                continue
            if c not in RISK and not rejected and not reads:
                counts['low_risk_unverified'] += 1
                continue
            counts['needs_review'] += 1
            if len(reads) > 1: why = '词证据冲突：' + '、'.join(f'{w}={"/".join(sorted(rs))}' for w, _, rs in accepted)
            elif rejected: why = '证据拒收：' + '；'.join(f'{w}（{REJECT[w]}）' for w in rejected)
            else: why = '基础词库无覆盖词'
            context = text[max(0, i - 1):i + 2]
            review.append({'char': c, 'pos': i, 'auto': auto[i], 'options': '/'.join(heteronyms(c)),
                           'context': context, 'why': why})
        if not fixes and not review: continue
        sheet = ids[0].split('-')[0]
        status = ('evidence+needs-review' if fixes and review else 'evidence' if fixes else 'needs-review')
        reading = chinese_key(text, fixes)
        rows.append({'text': text, 'records': len(ids), 'sheet': sheet, 'ids': ','.join(ids[:3]),
                     'auto': chinese_key(text), 'suggested': reading if fixes else '', 'status': status,
                     'evidence': '；'.join(notes),
                     'review': '；'.join(f'{x["char"]}@{x["pos"]} 自动 {x["auto"]}，可读 {x["options"]}，{x["why"]}' for x in review),
                     '_review': review})
        if fixes and ('*', text) not in existing:
            overrides.append({'id': '*', 'text': text, 'reading': reading, 'status': 'evidence',
                              'note': '依据基础词库词读音：' + '；'.join(notes)})
    return rows, overrides, counts


COLLISION_WEIGHT = 3000
# FF14-04 技能／副本；FF14-05 起所有新表（build-ff14-full.py 的 COLLISION_SHEETS）。
COLLISION_PREFIXES = ('action-', 'contentfindercondition-', 'classjob-', 'pvpaction-', 'trait-', 'status-', 'mount-',
                      'companion-', 'ornament-', 'emote-', 'quest-', 'enpcresident-', 'bnpcname-')
AUDIT_PREFIXES = ('item-', 'placename-') + COLLISION_PREFIXES
ABBREVIATIONS = OUT / 'player-abbreviations.tsv'


def common_collisions(audit, coverage, native_pinyin):
    """FF14-04：技能／副本触发与基础词库常用词（权重≥3000）同读音的列表，供构建时丢弃这些触发。
    含已丢弃的触发（dropped_triggers），重跑结果稳定。"""
    keys = collections.defaultdict(list)
    for r in list(audit) + coverage['exclusions']:
        if not r['id'].startswith(COLLISION_PREFIXES): continue
        for group in (r.get('triggers', {}), r.get('dropped_triggers', {})):
            for values in group.values():
                for k in values: keys[k].append(r['zh_cn'])
    best = {}
    with native_pinyin.open(encoding='utf-8') as stream:
        if next(stream).strip() != 'MEOWPUUT-PINYIN-2': raise ValueError('不是 native-pinyin.tsv')
        for line in stream:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != 6 or parts[0] not in keys: continue
            weight = int(parts[4])
            if weight >= COLLISION_WEIGHT and (parts[0] not in best or weight > best[parts[0]][1]):
                best[parts[0]] = (parts[3], weight)
    return [{'reading': k, 'word': w, 'weight': n, 'example': sorted(set(keys[k]))[0]} for k, (w, n) in sorted(best.items())]


def read_sheet(path):
    with path.open(encoding='utf-8-sig', newline='') as stream: rows = list(csv.reader(stream))
    header = next(h for h in rows if h and h[0] == '#')
    return header, {int(r[0]): r for r in rows if r and r[0].isdigit()}


def classify_gaps(sources, coverage, aligned_names):
    hc, cn = read_sheet(sources / 'cn-Item.csv')
    hj, ja = read_sheet(sources / 'ja-Item.csv')
    he, en = read_sheet(sources / 'en-Item.csv')
    name = hc.index('Name')
    rows = []
    for item in coverage['exclusions']:
        if not item['id'].startswith('item-'): continue
        row_id = int(item['id'][5:])
        jrow, erow = ja.get(row_id), en.get(row_id)
        def state(row):
            if row is None: return 'missing-row'
            return 'empty-row' if all(x in ('', '0', 'False') for x in row[1:]) else 'row-without-name'
        text = cn[row_id][name].strip()
        if '复制品' in text: group = '复制品（国服名称标注）'
        elif '冒险录' in text: group = '冒险录'
        elif re.search('兑换券|教材', text): group = '兑换券／教材'
        else: group = '节日／活动／赛事'
        base = re.sub('（复制品）$|^复制品', '', text)
        same = aligned_names.get(text)
        reference = aligned_names.get(base) if base != text else None
        if same: verdict = '同名另有已对齐 ID，可能为 ID 不一致'
        elif state(jrow) == 'empty-row' and state(erow) == 'empty-row':
            verdict = '国服独有：日英同 ID 整行为空'
        else: verdict = '待查：日英行存在但无名称'
        rows.append({'id': item['id'], 'zh_cn': text, 'ja_row': state(jrow), 'en_row': state(erow), 'group': group,
                     'verdict': verdict, 'same_name_aligned': ','.join(same or []),
                     'base_item_reference': ('原型名对齐 ID：' + ','.join(reference[:3])) if reference else '',
                     'resolution': '未确认日文译名，不收录'})
    return rows


def write_tsv(path, rows, columns, header=None):
    with path.open('w', encoding='utf-8', newline='') as stream:
        if header: stream.write(header)
        writer = csv.DictWriter(stream, columns, delimiter='\t', extrasaction='ignore', lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def checklist(rows, limit=50):
    groups = collections.defaultdict(lambda: {'records': 0, 'names': [], 'score': 0})
    for row in rows:
        for x in row['_review']:
            key = (x['context'], x['char'], x['auto'], x['options'])
            g = groups[key]
            g['records'] += row['records']
            # 影响面：记录数；地名 ×5；任务清单字 ×4、其他高风险字 ×2，其余（仅冲突）×1。
            weight = 4 if x['char'] in PRIMARY else 2 if x['char'] in RISK else 1
            g['score'] += row['records'] * weight * (5 if row['sheet'] == 'placename' else 1)
            g['names'].append(row['text']); g['why'] = x['why'].split('：')[0]
    ranked = sorted(groups.items(), key=lambda kv: (-kv[1]['score'], kv[0]))
    lines = ['# FF14 读音人工复核清单', '',
             '由 scripts/audit-ff14-readings.py 生成，勿手改。按影响面排序（记录数 × 地名 5 × 任务清单多音字 4／其他高风险字 2），列出前 '
             f'{limit} 组需要人工确认的多音字。确认后在 DataSources/FF14/reading-overrides.tsv 加 status=reviewed 行（可用 id=* 作用于同名记录），重建词库。',
             '', '| # | 上下文 | 字 | 自动 | 可读 | 记录数 | 原因 | 例 |', '| ---: | --- | --- | --- | --- | ---: | --- | --- |']
    for n, ((context, char, auto, options), g) in enumerate(ranked[:limit], 1):
        examples = '、'.join(sorted(set(g['names']), key=lambda x: (len(x), x))[:3])
        lines.append(f'| {n} | {context} | {char} | {auto} | {options} | {g["records"]} | {g["why"]} | {examples} |')
    lines.append('')
    lines.append(f'共 {len(groups)} 组上下文、{sum(len(r["_review"]) for r in rows)} 个位置待复核；完整列表见 DataSources/FF14/reading-audit.tsv。')
    lines.extend(abbreviation_checklist())
    return '\n'.join(lines) + '\n', len(groups)


def abbreviation_checklist(path=ABBREVIATIONS):
    """FF14-05：说明以“FF14-05”开头的简称／用语行（代理整理）列给维护者逐条复核。"""
    with path.open(encoding='utf-8', newline='') as stream:
        rows = list(csv.DictReader((line for line in stream if not line.startswith('#')), delimiter='	'))
    rows = [r for r in rows if r['note'].startswith('FF14-05')]
    if not rows: return []
    lines = ['', '## FF14-05 新增玩家简称与用语（维护者已复核）', '',
             '由代理整理，维护者 2026-11-23 复核通过，并改为收录诗学（诗学神典石）与枪刃（绝枪战士）。'
             '需要改动直接编辑 DataSources/FF14/player-abbreviations.tsv 后重建。',
             '', '| # | 简称 | 触发 | 日服说法 | 假名 | 指代 | 说明 | 状态 |', '| ---: | --- | --- | --- | --- | --- | --- | --- |']
    for n, r in enumerate(rows, 1):
        note = r['note'][len('FF14-05'):].lstrip('：: ')
        lines.append(f'| {n} | {r["abbr"]} | {r["pinyin"]} | {r["ja"]} | {r["kana"]} | {r["target"]} | {note} | {r["status"]} |')
    lines.append('')
    lines.append(f'共 {len(rows)} 行（收录 {sum(r["status"] == "curated" for r in rows)}，只记录不收录 {sum(r["status"] == "excluded" for r in rows)}）。')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path, help='fetch-ff14-sources.py 缓存目录')
    parser.add_argument('native_pinyin', type=Path, help='构建产物 native-pinyin.tsv')
    parser.add_argument('--write-overrides', action='store_true')
    parser.add_argument('--bundle', type=Path, default=BUNDLE, help='已构建的全量包目录（默认仓库内）')
    args = parser.parse_args()
    audit = json.loads((args.bundle / 'reviewed-source.json').read_text(encoding='utf-8'))['entries']
    coverage = json.loads((args.bundle / 'coverage.json').read_text(encoding='utf-8'))
    aligned = collections.defaultdict(list)
    for r in audit:
        if r['id'].startswith('item-'): aligned[r['zh_cn']].append(r['id'])
    # FF14-04: 技能、副本名同样用自动拼音，纳入审计。
    records = [r for r in audit if r['id'].startswith(AUDIT_PREFIXES)
               and r.get('reading_source') != 'curated-0.2.0']
    wanted = {r['zh_cn'][i:j] for r in records for i in range(len(r['zh_cn']))
              for j in range(i + 2, min(len(r['zh_cn']), i + 8) + 1)}
    lexicon = load_lexicon(args.native_pinyin, wanted)
    existing = {k: v for k, v in load_overrides().items() if v['status'] == 'reviewed'}
    rows, overrides, counts = audit_readings(records, lexicon, existing)
    gaps = classify_gaps(args.sources, coverage, aligned)
    OUT.mkdir(parents=True, exist_ok=True)
    write_tsv(OUT / 'item-gaps.tsv', gaps, ['id', 'zh_cn', 'ja_row', 'en_row', 'group', 'verdict',
                                            'same_name_aligned', 'base_item_reference', 'resolution'],
              '# 生成：scripts/audit-ff14-readings.py。缺同 ID 日文名的物品；不猜译名，原型对齐仅供人工参考。\n')
    write_tsv(OUT / 'reading-audit.tsv', rows, ['text', 'records', 'sheet', 'ids', 'auto', 'suggested', 'status',
                                                'evidence', 'review'],
              '# 生成：scripts/audit-ff14-readings.py。evidence=依据基础词库词读音修订；needs-review=需人工确认。\n')
    if args.write_overrides:
        merged = sorted(list(existing.values()) + overrides, key=lambda r: (r['text'], r['id']))
        write_tsv(OVERRIDES, merged, COLUMNS,
                  '# FF14 全量包读音修订。status: evidence=依据基础词库（native-pinyin）词读音，程序判定、代理核对拒收表；'
                  'reviewed=人工复核。id=* 作用于所有同名自动拼音记录。由 audit-ff14-readings.py --write-overrides 重建 evidence 行，reviewed 行保留。\n')
        collisions = common_collisions(audit, coverage, args.native_pinyin)
        write_tsv(OUT / 'common-reading-collisions.tsv', collisions, ['reading', 'word', 'weight', 'example'],
                  f'# 生成：scripts/audit-ff14-readings.py。技能／副本触发与基础词库常用词（权重≥{COLLISION_WEIGHT}）同读音；'
                  'build-ff14-full.py 丢弃这些触发，避免日常输入出现技能／副本候选。word 为基础词库最高权重的同读音词。\n')
    text, group_count = checklist(rows)
    (ROOT / 'docs/FF14_READING_REVIEW.md').write_text(text, encoding='utf-8')
    status = collections.Counter(r['status'] for r in rows)
    print(json.dumps({'gaps': len(gaps), 'gap_groups': collections.Counter(g['group'] for g in gaps),
                      'gap_verdicts': collections.Counter(g['verdict'] for g in gaps),
                      'positions': counts, 'names': status, 'override_rows': len(existing) + len(overrides),
                      'review_groups': group_count}, ensure_ascii=False))


if __name__ == '__main__': main()
