"""Reproducible exploratory quality/latency report; never loads user dictionaries."""
import argparse
import hashlib
import json
import math
import platform
from pathlib import Path
import re
import subprocess
import tempfile
import time
from datetime import datetime, timezone

from japanese_packages import change

ROOT = Path(__file__).resolve().parents[1]
# tuned: fixed regression set that ranking work has already been adjusted against.
# holdout: written without querying candidates; report it, never tune against it.
SETS = {'tuned': ('Windows/tests/data/chinese-evaluation.tsv', '固定回归集：已参与排序调优，结果不是独立准确率。'),
        'holdout': ('Windows/tests/data/chinese-holdout.tsv', '独立评测集：编写时未查询候选，不得据此逐条调优；仍是手写小样本，不是外部盲测。')}
EDITORIAL = ('chinese.tsv', 'daily-chinese.tsv', 'context.tsv', 'simplified-priorities.tsv', 'simplified-traditional-only-keep.tsv')


def leaks(holdout, tuned, lexicon=ROOT / 'Resources/Lexicon'):
    """Holdout rows that repeat a tuned input/target or a hand-edited lexicon entry."""
    inputs = {r['input'] for r in tuned}
    known = {t for r in tuned for t in r['hans'] + r['hant']}
    for name in EDITORIAL:
        for line in (lexicon / name).read_text(encoding='utf-8-sig').splitlines():
            if line and not line.startswith('#'):
                known.update(line.split('|')[:1 if name in ('context.tsv', 'simplified-priorities.tsv', 'simplified-traditional-only-keep.tsv') else 2])
    return sorted(r['id'] for r in holdout if r['input'] in inputs or known.intersection(r['hans'] + r['hant']))


def corpus(path):
    rows, ids = [], set()
    for number, line in enumerate(path.read_text(encoding='utf-8-sig').splitlines(), 1):
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) != 6:
            raise ValueError(f'语料第 {number} 行应有 6 列')
        key, category, reading, hans, hant, scope = fields
        if key in ids or not re.fullmatch('[a-z]+', reading) or not hans or not hant or scope not in ('all', 'ff14'):
            raise ValueError(f'语料第 {number} 行无效')
        ids.add(key)
        rows.append(dict(id=key, category=category, input=reading, hans=hans.split(';'), hant=hant.split(';'), scope=scope))
    if not rows:
        raise ValueError('语料不能为空')
    return rows


def percentile(values, p):
    return round(sorted(values)[max(0, math.ceil(len(values) * p) - 1)], 4) if values else None


def score(sample, result, script):
    choices = result['candidates']
    expected = sample[script]
    rank = next((i + 1 for i, c in enumerate(choices) if c['text'] in expected), None)
    first = choices[0] if choices else None
    return dict(id=sample['id'], category=sample['category'], input=sample['input'], expected=expected,
                rank=rank, first=first, top7=choices[:7], candidate_count=len(choices),
                missing_from_candidates=rank is None,
                partial_first=bool(first and first['consumed'] < len(sample['input'])),
                correction_first=bool(first and first['kind'] == 4),
                unexpected_correction_first=bool(first and first['kind'] == 4 and rank != 1))


def summarize(rows):
    return dict(samples=len(rows), first=sum(r['rank'] == 1 for r in rows),
                top7=sum(r['rank'] is not None and r['rank'] <= 7 for r in rows),
                missing=sum(r['missing_from_candidates'] for r in rows),
                partial_first=sum(r['partial_first'] for r in rows),
                unexpected_correction_first=sum(r['unexpected_correction_first'] for r in rows))


def run(probe, lexicon, rows, script, theme, repeats, fuzzy=0):
    command = [str(probe), str(lexicon), script] + ([str(theme)] if theme else []) + ([f'--fuzzy={fuzzy}'] if fuzzy else [])
    start = time.perf_counter()
    result = subprocess.run(command, input=''.join(r['input'] + '\n' for _ in range(repeats) for r in rows),
                            text=True, encoding='utf-8', capture_output=True, timeout=600, check=True)
    wall = (time.perf_counter() - start) * 1000
    records = [json.loads(line) for line in result.stdout.splitlines()]
    if len(records) != 1 + len(rows) * repeats or records[0]['type'] != 'startup':
        raise ValueError('探针返回条数不匹配')
    timings, long_timings, common_timings, quality = [], [], [], []
    for i, record in enumerate(records[1:]):
        sample = rows[i % len(rows)]
        if record['type'] != 'sample' or record['input'] != sample['input'] or len(record['key_ms']) != len(sample['input']):
            raise ValueError('探针返回输入或按键数不匹配')
        timings.extend(record['key_ms'])
        if sample['scope'] == 'all':
            common_timings.extend(record['key_ms'])
        if len(sample['input']) >= 16:
            long_timings.extend(record['key_ms'])
        scored = score(sample, record, script)
        if i < len(rows):
            quality.append(scored)
        elif quality[i % len(rows)] != scored:
            raise ValueError('同一进程重复运行的候选不一致')
    return dict(startup=records[0], process_wall_ms=round(wall, 3), repeats=repeats,
                latency=dict(keys=len(timings), p50_ms=percentile(timings, .5), p95_ms=percentile(timings, .95),
                             max_ms=max(timings), long_keys=len(long_timings), long_p95_ms=percentile(long_timings, .95)),
                sampled_private_max_bytes=max(r['private_bytes'] for r in records),
                common_latency=dict(keys=len(common_timings), p50_ms=percentile(common_timings, .5), p95_ms=percentile(common_timings, .95)),
                summary=summarize(quality), common_summary=summarize([r for r in quality if r['category'] != 'theme']),
                by_category={c: summarize([r for r in quality if r['category'] == c]) for c in sorted({r['category'] for r in quality})},
                quality=quality, raw=records)


def compose(probe, lexicon, rows, script='hans', fuzzy=0):
    """Can each text be produced by selection alone, and at what paging cost?

    Uses the first acceptable text of each complete sample. A selection costs
    one unit and so does every PageDown needed to reach it; the probe returns
    the cheapest sequence over fully expanded candidate lists.
    """
    rows = [r for r in rows if r['scope'] == 'all' and r['category'] != 'partial']
    result = subprocess.run([str(probe), str(lexicon), script] + ([f'--fuzzy={fuzzy}'] if fuzzy else []), input=''.join(f"{r['input']}\t{r[script][0]}\n" for r in rows),
                            text=True, encoding='utf-8', capture_output=True, timeout=900, check=True)
    records = [json.loads(line) for line in result.stdout.splitlines()][1:]
    if len(records) != len(rows) or any(a['type'] != 'compose' or a['input'] != b['input'] for a, b in zip(records, rows)):
        raise ValueError('探针返回的分段选词记录不匹配')
    items = [dict(id=row['id'], ok=record['ok'], selections=len(record['steps']),
                  page_downs=record['cost'] - len(record['steps']), steps=record['steps']) for row, record in zip(rows, records)]
    done = [i for i in items if i['ok']]
    return dict(script=script, samples=len(items), reachable=len(done), unreachable=[i['id'] for i in items if not i['ok']],
                single_selection=sum(i['selections'] == 1 for i in done), no_paging=sum(i['page_downs'] == 0 for i in done),
                max_selections=max((i['selections'] for i in done), default=0), max_page_downs=max((i['page_downs'] for i in done), default=0),
                items=items)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description='中文候选质量与逐键计算基线（不包含 TSF／渲染）')
    parser.add_argument('--probe', type=Path, default=ROOT / '.build/windows/msvc/Release/harukoto_candidate_probe.exe')
    parser.add_argument('--lexicon', type=Path, default=ROOT / '.build/windows/ime')
    parser.add_argument('--set', choices=sorted(SETS), default='tuned', help='tuned 为已调优的固定回归集，holdout 为独立评测集')
    parser.add_argument('--corpus', type=Path, help='自定义语料；默认按 --set 选择')
    parser.add_argument('--compact', action='store_true', help='省略逐键原始记录，用于归档')
    parser.add_argument('--ff14', type=Path, default=ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fuzzy', type=lambda value: int(value, 0), default=0,
                        help='模糊音位掩码（CN-03，MPN_FUZZY_*；2047 为全部开启）；默认 0 即关闭。输入仍是语料里的精确拼音')
    args = parser.parse_args()
    if not 1 <= args.repeats <= 20:
        parser.error('--repeats 须为 1–20')
    if not 0 <= args.fuzzy <= 2047:
        parser.error('--fuzzy 须为 0–2047')
    args.corpus = args.corpus or ROOT / SETS[args.set][0]
    samples = corpus(args.corpus)
    if args.set == 'holdout' and (leaked := leaks(samples, corpus(ROOT / SETS['tuned'][0]))):
        parser.error('独立集与调优集或人工词表重复：' + '、'.join(leaked))
    report = dict(format=1, corpus_set=args.set, timestamp=datetime.now(timezone.utc).isoformat(),
                  machine=dict(os=platform.platform(), cpu=platform.processor(), architecture=platform.machine()),
                  revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  dirty=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
                  **(dict(fuzzy=args.fuzzy) if args.fuzzy else {}),
                  hashes={str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else p.name: sha(p) for p in [args.probe.resolve(), args.corpus.resolve(), args.ff14.resolve(), (args.ff14.parent / 'ff14-words.txt').resolve(),
                          Path(__file__).resolve(), ROOT / 'Windows/tests/CandidateProbe.cpp', ROOT / 'Core/MeowpuutCore.cpp', ROOT / 'Windows/MixedComposition.h'] +
                          [args.lexicon / n for n in ('native-pinyin.tsv', 'japanese.tsv', 'japanese-conjugations.bin', 'language-evidence.sqlite')]},
                  notes=['新进程加载时间；未清空系统文件缓存，不是磁盘冷启动。',
                         '逐键为核心计算＋Windows 候选合并；不包含真实 TSF、IPC、候选绘制或按键到显示。',
                         '缺失指本次候选列表未命中，不证明词库缺词；分词和误纠错需人工复核。',
                         SETS[args.set][1],
                         'compose 为简体、FF14 关闭下只靠选词打出首个可接受文字的最小代价：每次选择和每次翻页各计 1；不含 partial 类样本。'] +
                        ([f'模糊音位掩码 {args.fuzzy}（CN-03）：输入仍是语料里的精确拼音，衡量开启模糊音后精确输入是否回退。'] if args.fuzzy else []), runs={})
    with tempfile.TemporaryDirectory(prefix='harukoto-eval-') as temporary:
        theme = Path(temporary)
        change(theme, 'import', args.ff14)
        change(theme, 'enable', 'harukoto-ff14')
        for enabled in (False, True):
            selected = [s for s in samples if s['scope'] == 'all' or enabled]
            for script in ('hans', 'hant'):
                key = f'{script}-ff14-{"on" if enabled else "off"}'
                report['runs'][key] = run(args.probe.resolve(), args.lexicon.resolve(), selected, script, theme if enabled else None, args.repeats, args.fuzzy)
                print(key, json.dumps({k: report['runs'][key][k] for k in ('summary', 'latency')}, ensure_ascii=False), flush=True)
    report['compose'] = compose(args.probe.resolve(), args.lexicon.resolve(), samples, fuzzy=args.fuzzy)
    print('compose', json.dumps({k: v for k, v in report['compose'].items() if k != 'items'}, ensure_ascii=False), flush=True)
    if args.compact:
        for value in report['runs'].values():
            del value['raw']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(args.output)


if __name__ == '__main__':
    main()
