"""常驻引擎成本（WIN-03 第 5 步）：冷启动、握手、内存、状态写入与逐键延迟；固定文本、临时快照与 HKCU 测试键，不修改输入法安装。"""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

import base_dictionary as bundle
from japanese_packages import change

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'Windows/tests/data'
SETS = (('tuned', 'chinese-evaluation.tsv'), ('holdout', 'chinese-holdout.tsv'), ('words', 'chinese-words-dev.tsv'))
FF14_INPUTS = ('tejihuifuyao', 'jiaosedengji', 'gaojijingyanzhi')
TRACKED = ('Windows/EngineResident.h', 'Windows/EngineHandoff.h', 'Windows/EngineDiscovery.h', 'Windows/EngineDeployment.h',
           'Windows/EngineRegistration.h', 'Windows/EngineOwner.h', 'Windows/EngineOwner.cpp', 'Windows/EngineProcess.h',
           'Windows/EngineHost.h', 'Windows/EngineHost.cpp', 'Windows/EngineExecutor.h', 'Windows/EngineWindows.h',
           'Windows/tests/EngineCostBenchmark.cpp', 'scripts/benchmark-engine-cost.py')
TYPES = {'setup', 'pin', 'cold', 'handshake', 'ledger', 'sessions', 'idle', 'trim', 'keys', 'done'}


def corpus_rows(themes, limit=None):
    """(类别, 输入)；只保留 a-z，去重，保持文件顺序。"""
    rows, seen = [], set()
    for name, file in SETS:
        count = 0
        for line in (DATA / file).read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            text = line.split('\t')[2].strip()
            if not text or not text.isascii() or not text.isalpha() or not text.islower() or len(text) > 64 or (name, text) in seen:
                continue
            seen.add((name, text)); rows.append((name, text)); count += 1
            if limit and count >= limit:
                break
    if themes:
        rows += [('ff14', text) for text in FF14_INPUTS]
    return rows


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def check_stats(row):
    if not isinstance(row, dict) or set(row) != {'n', 'median', 'p95', 'p99', 'max', 'mean'} or type(row['n']) is not int or row['n'] < 1:
        raise ValueError('统计字段无效')
    if not all(finite(row[k]) for k in ('median', 'p95', 'p99', 'max', 'mean')) or not row['median'] <= row['p95'] <= row['p99'] <= row['max']:
        raise ValueError('统计值无效')


def validate(events, cold, counts, threads, keys_expected, sections):
    if not events or events[0].get('type') != 'setup' or events[-1].get('type') != 'done':
        raise ValueError('输出不完整')
    by = {}
    for event in events:
        if event.get('type') not in TYPES:
            raise ValueError('未知输出')
        by.setdefault(event['type'], []).append(event)
    for event in by.get('pin', []):
        for row in event['stats'].values():
            check_stats(row)
    if 'cold' in sections:
        rows = by.get('cold', [])
        if len(rows) != cold or any(r['launch'] != 'Started' for r in rows):
            raise ValueError('冷启动记录数量或结果不符')
        for r in rows:
            if not all(finite(r[k]) for k in ('ensure_ms', 'owner_created_ms', 'host_created_ms', 'pipe_visible_ms', 'allocate_ms', 'connect_ms', 'first_key_ms')):
                raise ValueError('冷启动时间无效')
            if not r['owner_created_ms'] <= r['host_created_ms'] <= r['pipe_visible_ms'] <= r['ensure_ms'] + 50:
                raise ValueError('冷启动时间顺序不符')
    if 'warm' in sections:
        if len(by.get('handshake', [])) != 2 or len(by.get('ledger', [])) != 1 or len(by.get('idle', [])) != 1 or len(by.get('trim', [])) != 5:
            raise ValueError('热路径记录缺失')
        for event in (*by['handshake'], *by['ledger']):
            for row in event['stats'].values():
                check_stats(row)
        if [r['count'] for r in by.get('sessions', [])] != [0, 1, 2, 4, 8, 0]:
            raise ValueError('会话内存记录不符')
    if 'keys' in sections:
        groups = {(r['mode'], r['threads'], r['clients']) for r in by.get('keys', [])}
        expected = {('inproc', 0, n) for n in counts} | {('engine', t, n) for t in threads for n in counts}
        if groups != expected or len(by['keys']) != len(expected):
            raise ValueError('逐键组不完整')
        for r in by['keys']:
            check_stats(r['overall'])
            if r['overall']['n'] != keys_expected * r['clients'] * r['runs']:
                raise ValueError('逐键样本数不符')
            for row in (*r['classes'].values(), *r['positions'].values()):
                check_stats(row)
            memory = r['memory']
            for stage in ('ready', 'held'):
                m = memory[stage]
                if m['total'] != m['clients'] + m['owner'] + m['host'] or (r['mode'] == 'engine') != bool(m['host']):
                    raise ValueError('内存求和无效')
    return by


def summarize(by):
    def agg(rows, key):
        values = [r[key] for r in rows]
        return dict(median=statistics.median(values), maximum=max(values), minimum=min(values), n=len(values))
    summary = {}
    if by.get('cold'):
        rows = by['cold']
        summary['cold'] = {k: agg(rows, k) for k in ('ensure_ms', 'owner_created_ms', 'host_created_ms', 'pipe_visible_ms',
                                                     'allocate_ms', 'connect_ms', 'first_key_ms', 'long_word_ms')}
        summary['cold']['host_load_ms'] = agg([dict(v=r['pipe_visible_ms'] - r['host_created_ms']) for r in rows], 'v')
        summary['cold']['owner_to_host_ms'] = agg([dict(v=r['host_created_ms'] - r['owner_created_ms']) for r in rows], 'v')
        summary['cold']['memory_mib'] = {role: {k: statistics.median(r[role][k] for r in rows) / 1048576 for k in ('private', 'working_set')}
                                         for role in ('owner', 'host')}
    if by.get('keys'):
        inproc = {r['clients']: r for r in by['keys'] if r['mode'] == 'inproc'}
        summary['keys'] = [dict(mode=r['mode'], threads=r['threads'], clients=r['clients'], median=r['overall']['median'],
                                p95=r['overall']['p95'], p99=r['overall']['p99'], max=r['overall']['max'],
                                delta_median=r['overall']['median'] - inproc[r['clients']]['overall']['median'] if r['clients'] in inproc else None,
                                total_private_mib=r['memory']['held']['total'] / 1048576, wall_ms=r['wall_ms'])
                           for r in by['keys']]
    return summary


def run(benchmark, owner, lexicon, cold=10, handshake=50, runs=1, counts=(1, 2, 4, 8), threads=(2, 4, 8), themes=True,
        sections=('cold', 'warm', 'keys'), corpus_limit=None, timeout=3600):
    benchmark, owner, lexicon = Path(benchmark).resolve(), Path(owner).resolve(), Path(lexicon).resolve()
    theme = ROOT / 'Resources/DictionaryPackages/ff14/full'
    rows = corpus_rows(themes, corpus_limit)
    keys_expected = sum(len(text) for _, text in rows)
    def identities():
        return dict(sources={name: bundle.digest(ROOT / name) for name in TRACKED},
                    programs={name: bundle.digest(path) for name, path in [('benchmark', benchmark), ('owner', owner), ('host', owner.parent / 'harukoto_engine_host.exe')]},
                    base_files={name: dict(size=(lexicon / name).stat().st_size, sha256=bundle.digest(lexicon / name)) for name in bundle.FILES},
                    ff14_files={name: bundle.digest(theme / name) for name in ('manifest.json', 'ff14-words.txt')} if themes else {})
    identity = identities()
    classes = {}
    for name, text in rows:
        entry = classes.setdefault(name, dict(inputs=0, keys=0, max_length=0))
        entry['inputs'] += 1; entry['keys'] += len(text); entry['max_length'] = max(entry['max_length'], len(text))
    provenance = dict(revision=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                      source_dirty=bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip()),
                      platform=platform.platform(), logical_cpus=os.cpu_count(), cold=cold, handshake=handshake, runs=runs,
                      counts=list(counts), threads=list(threads), themes=themes, sections=list(sections), corpus=classes, **identity)
    with tempfile.TemporaryDirectory(prefix='harukoto-engine-cost-') as temporary:
        root = Path(temporary)
        bundle.create(lexicon, root / '词库 快照', '1.0.0-engine-cost')
        if themes:
            change(root / 'themes', 'import', theme / 'manifest.json')
            change(root / 'themes', 'enable', 'harukoto-ff14')
        corpus = root / 'corpus.tsv'
        corpus.write_text(''.join(f'{name}\t{text}\n' for name, text in rows), encoding='utf-8', newline='\n')
        result = subprocess.run([str(benchmark), str(owner), str(root / '词库 快照'), str(root / 'themes') if themes else '-', str(corpus),
                                 str(cold), str(handshake), str(runs), ','.join(map(str, counts)), ','.join(map(str, threads)), ','.join(sections)],
                                capture_output=True, text=True, encoding='utf-8', timeout=timeout)
        if result.returncode != 0:
            raise RuntimeError(f'测量程序失败 {result.returncode}: {result.stderr[-2000:]}')
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        bundle.verify(root / '词库 快照')
    if identity != identities():
        raise ValueError('测量期间程序、源代码或词库改变')
    by = validate(events, cold, counts, threads, keys_expected, sections)
    return dict(format=1, provenance=provenance, events=events, summary=summarize(by))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark', type=Path, required=True)
    parser.add_argument('--owner', type=Path, required=True, help='harukoto_engine_owner_test.exe（宿主取同目录）')
    parser.add_argument('--lexicon', type=Path, required=True)
    parser.add_argument('--cold', type=int, default=10)
    parser.add_argument('--handshake', type=int, default=50)
    parser.add_argument('--runs', type=int, default=1)
    parser.add_argument('--counts', type=int, nargs='+', default=[1, 2, 4, 8], choices=range(1, 9))
    parser.add_argument('--threads', type=int, nargs='+', default=[2, 4, 8], choices=range(0, 17))
    parser.add_argument('--sections', nargs='+', default=['cold', 'warm', 'keys'], choices=('cold', 'warm', 'keys'))
    parser.add_argument('--no-ff14', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = run(args.benchmark, args.owner, args.lexicon, args.cold, args.handshake, args.runs, args.counts, args.threads,
                 not args.no_ff14, args.sections)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    bundle.atomic_json(args.output, report)
    print(json.dumps(report['summary'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
