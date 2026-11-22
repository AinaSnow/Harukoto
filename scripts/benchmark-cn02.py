"""CN-02 真实延迟与内存对比（WIN-03 第 8 步）：
在真实 TextService（Service）与多进程条件下，对比「独立引擎（瘦客户端）」与「进程内路径」的：
  1. 冷启动与次应用热启动（激活耗时、首键到候选可见耗时、单进程与总私有内存）；
  2. 按键到候选可见延迟（含选词 KeyAction::Inspect）与 1/2/4/8 并发应用 + 待机应用总内存；
  3. 深页翻页与 3 轮扩展（延迟与候选列表哈希一致性）；
  4. 大个人表与大量学习记录（5,000 个人日语词 + 5,000 选词记忆 + 5,000 个人词频 + 1,000 手动个人词，
     启动加载、稳态查询与选词落盘后首键重载）。

只写临时目录与 HKCU 测试键，绝不修改 HKLM 或已安装的输入法。
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import base_dictionary as bundle
from japanese_packages import change

DATA = ROOT / 'Windows/tests/data'
SETS = (('tuned', 'chinese-evaluation.tsv'), ('holdout', 'chinese-holdout.tsv'), ('words', 'chinese-words-dev.tsv'))
FF14_INPUTS = ('tejihuifuyao', 'jiaosedengji', 'gaojijingyanzhi')
TRACKED = (
    'Core/MeowpuutCore.cpp', 'Windows/TextService.cpp', 'Windows/EngineTsf.h', 'Windows/EngineTsfResident.h',
    'Windows/EngineMessages.h', 'Windows/EngineServer.h', 'Windows/EngineWindows.h', 'Windows/EngineHost.h',
    'Windows/EngineOwner.h', 'Windows/EngineResident.h',
    'Windows/tests/EngineCn02Benchmark.cpp', 'scripts/benchmark-cn02.py',
)
TYPES = {'setup', 'startup', 'keys', 'deep_page', 'large_personal', 'done'}


def corpus_rows(themes, limit=None):
    """(类别, 输入)；只保留 a-z，去重，保持文件顺序（不输出任何 holdout 原文）。"""
    rows, seen = [], set()
    for name, file in SETS:
        count = 0
        for line in (DATA / file).read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            text = line.split('\t')[2].strip()
            if not text or not text.isascii() or not text.isalpha() or not text.islower() or len(text) > 64 or (name, text) in seen:
                continue
            seen.add((name, text))
            rows.append((name, text))
            count += 1
            if limit and count >= limit:
                break
    if themes:
        rows += [('ff14', text) for text in FF14_INPUTS]
    return rows


def _base26(n, width=4):
    chars = []
    for _ in range(width):
        chars.append(chr(ord('a') + (n % 26)))
        n //= 26
    return ''.join(reversed(chars))


def write_large_personal(folder: Path, jp_count=5000, phrase_count=5000, word_count=5000, user_count=1000):
    """生成规模可控且格式合法的大个人表与学习记录夹具。"""
    folder.mkdir(parents=True, exist_ok=True)
    hour = int(time.time() // 3600)

    # 1. japanese-custom.tsv (pipe-separated personal Japanese dictionary)
    jp_lines = [
        f'jpk{_base26(i, 3)}|カスタム{i}|4000|かすたむ|english|个人词{i}'
        for i in range(jp_count)
    ]
    (folder / 'japanese-custom.tsv').write_text('\n'.join(jp_lines) + '\n', encoding='utf-8', newline='\n')

    # 2. selection-memory.tsv (phrase memory journal: count\ts\treading\ttext)
    phrase_lines = [
        f'2\ts\tmemk{_base26(i, 3)}\t记忆短语{i}'
        for i in range(phrase_count)
    ]
    (folder / 'selection-memory.tsv').write_text('\n'.join(phrase_lines) + '\n', encoding='utf-8', newline='\n')

    # 3. selection-memory-words.tsv (word frequency journal: =NNN\ts\treading\ttext\thour)
    word_lines = [
        f'=250\ts\twdk{_base26(i, 3)}\t词频词{i}\t{hour}'
        for i in range(word_count)
    ]
    (folder / 'selection-memory-words.tsv').write_text('\n'.join(word_lines) + '\n', encoding='utf-8', newline='\n')

    # 4. selection-memory-user.tsv (manual user Chinese words: reading\tsimplified\ttraditional)
    user_lines = [
        f'usrk{_base26(i, 3)}\t自建词{i}\t自建詞{i}'
        for i in range(user_count)
    ]
    (folder / 'selection-memory-user.tsv').write_text('\n'.join(user_lines) + '\n', encoding='utf-8', newline='\n')
    return dict(jp=jp_count, phrases=phrase_count, words=word_count, user_words=user_count)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def check_stats(row, allow_empty=False):
    if not isinstance(row, dict) or set(row) != {'n', 'median', 'p95', 'p99', 'max', 'mean'} or type(row['n']) is not int:
        raise ValueError('统计字段无效')
    if row['n'] == 0 and allow_empty:
        return
    if row['n'] < 1:
        raise ValueError('统计样本数无效')
    if not all(finite(row[k]) for k in ('median', 'p95', 'p99', 'max', 'mean')):
        raise ValueError('统计值非有限正数')
    if not row['median'] <= row['p95'] <= row['p99'] <= row['max'] + 1e-6:
        raise ValueError('统计分位单调性无效')


def validate(events, mode, counts, standby, cold_runs, warm_runs, keys_expected, items_expected, has_large=True):
    if not events or events[0].get('type') != 'setup' or events[-1].get('type') != 'done':
        raise ValueError('输出不完整')
    by = {}
    for ev in events:
        if ev.get('type') not in TYPES:
            raise ValueError(f'未知事件类型: {ev.get("type")}')
        by.setdefault(ev['type'], []).append(ev)

    # 1. Startup validation
    startups = by.get('startup', [])
    by_kind = {}
    for s in startups:
        by_kind.setdefault(s['kind'], []).append(s)
    if len(by_kind.get('inproc', [])) != warm_runs or len(by_kind.get('engine-warm', [])) != warm_runs:
        raise ValueError('启动样本数量不符')
    if mode == 'resident' and len(by_kind.get('engine-cold', [])) != cold_runs:
        raise ValueError('冷启动样本数量不符')
    for s in startups:
        c = s['client']
        if not all(finite(c[k]) for k in ('init_ms', 'first_key_ms', 'total_to_first_ms', 'next_key_median_ms')):
            raise ValueError('启动耗时无效')
        if s['kind'] != 'inproc':
            if not c['thin'] or c['fallbacks'] != 0:
                raise ValueError('瘦客户端启动发生了回退或加载本地词库')
            if s['host']['private'] <= 0:
                raise ValueError('缺少引擎宿主内存')

    # 2. Keys & multi-app memory validation
    key_events = by.get('keys', [])
    groups = {(r['mode'], r['clients']) for r in key_events}
    expected_groups = {('inproc', n) for n in counts} | {('engine', n) for n in counts}
    if groups != expected_groups or len(key_events) != len(expected_groups):
        raise ValueError('逐键并发组不完整')
    for r in key_events:
        check_stats(r['overall'])
        check_stats(r['select_ms'])
        if r['overall']['n'] != keys_expected * r['clients']:
            raise ValueError('逐键样本数不符')
        if r['select_ms']['n'] != items_expected * r['clients']:
            raise ValueError('选词样本数不符')
        for row in (*r['classes'].values(), *r['positions'].values()):
            check_stats(row)
        if r['mode'] == 'engine':
            if not r['thin_verified'] or r['fallbacks'] != 0 or r['memory_behind'] != 0:
                raise ValueError('引擎并发组出现回退、非瘦客户端或学习滞后')
        expected_sb = standby if r['clients'] == counts[-1] else 0
        if r['standbys'] != expected_sb:
            raise ValueError('待机进程数量不符')
        for stage in ('ready', 'held'):
            m = r['memory'][stage]
            if m['total'] != m['active_clients'] + m['standby_clients'] + m['owner'] + m['host']:
                raise ValueError('内存求和不一致')
            if (r['mode'] == 'engine') != bool(m['host']):
                raise ValueError('引擎宿主内存归属不符')

    # 3. Deep page validation + exact inproc vs engine parity
    deep_events = by.get('deep_page', [])
    deep_by_mode = {e['result']['mode']: e['result'] for e in deep_events}
    if set(deep_by_mode) != {'inproc', 'engine'}:
        raise ValueError('深页测量缺少模式')
    for m, res in deep_by_mode.items():
        check_stats(res['initial_ms'])
        check_stats(res['expand_ms'], allow_empty=True)
        check_stats(res['page_step_ms'], allow_empty=True)
        if m == 'engine' and (not res['thin'] or res['fallbacks'] != 0):
            raise ValueError('深页引擎模式未保持瘦客户端或出现回退')
    in_inputs = deep_by_mode['inproc']['inputs']
    en_inputs = deep_by_mode['engine']['inputs']
    if len(in_inputs) != len(en_inputs):
        raise ValueError('深页输入数不一致')
    for a, b in zip(in_inputs, en_inputs):
        if (a['reading'] != b['reading'] or a['initial_count'] != b['initial_count'] or
                a['final_count'] != b['final_count'] or a['pages'] != b['pages'] or a['list_hash'] != b['list_hash']):
            raise ValueError(f'深页候选与进程内不一致: {a["reading"]}')

    # 4. Large personal table validation
    if has_large:
        large_events = by.get('large_personal', [])
        large_by_mode = {e['result']['mode']: e for e in large_events}
        if set(large_by_mode) != {'inproc', 'engine'}:
            raise ValueError('大个人表测量缺少模式')
        for m, ev in large_by_mode.items():
            res = ev['result']
            check_stats(res['steady_key_ms'])
            check_stats(res['learn_commit_ms'])
            check_stats(res['post_learn_first_key_ms'])
            if res['learn_commit_ms']['n'] != 10 or res['post_learn_first_key_ms']['n'] != 10:
                raise ValueError('大个人表学习轮次不符')
            if m == 'engine' and (not res['thin'] or res['fallbacks'] != 0 or res['memory_behind'] != 0):
                raise ValueError('大个人表引擎模式出现回退、非瘦客户端或学习滞后')
    return by


def mib(b):
    return round(b / 1048576.0, 2)


def summarize(by):
    def med(vals):
        return round(statistics.median(vals), 4) if vals else None

    summary = {}
    # 1. Startup
    startups = {}
    for ev in by.get('startup', []):
        startups.setdefault(ev['kind'], []).append(ev)
    summary['startup'] = {}
    for kind, rows in startups.items():
        summary['startup'][kind] = dict(
            n=len(rows),
            init_ms_median=med([r['client']['init_ms'] for r in rows]),
            first_key_ms_median=med([r['client']['first_key_ms'] for r in rows]),
            total_to_first_ms_median=med([r['client']['total_to_first_ms'] for r in rows]),
            total_to_first_ms_max=round(max(r['client']['total_to_first_ms'] for r in rows), 4),
            next_key_ms_median=med([r['client']['next_key_median_ms'] for r in rows]),
            client_init_mib=mib(statistics.median(r['client']['mem_init']['private'] for r in rows)),
            client_steady_mib=mib(statistics.median(r['client']['mem_steady']['private'] for r in rows)),
            owner_mib=mib(statistics.median(r['owner']['private'] for r in rows)),
            host_mib=mib(statistics.median(r['host']['private'] for r in rows)),
        )

    # 2. Keys & Multi-App Memory
    inproc_by_n = {r['clients']: r for r in by.get('keys', []) if r['mode'] == 'inproc'}
    summary['keys'] = []
    for r in by.get('keys', []):
        ref = inproc_by_n.get(r['clients'])
        held = r['memory']['held']
        ref_held = ref['memory']['held'] if ref else None
        summary['keys'].append(dict(
            mode=r['mode'],
            clients=r['clients'],
            standbys=r['standbys'],
            total_apps=r['clients'] + r['standbys'],
            wall_ms=round(r['wall_ms'], 2),
            median_ms=r['overall']['median'],
            p95_ms=r['overall']['p95'],
            p99_ms=r['overall']['p99'],
            max_ms=r['overall']['max'],
            delta_median_ms=round(r['overall']['median'] - ref['overall']['median'], 4) if ref else None,
            select_median_ms=r['select_ms']['median'],
            select_p95_ms=r['select_ms']['p95'],
            select_max_ms=r['select_ms']['max'],
            positions={k: dict(median=v['median'], p95=v['p95'], p99=v['p99'], max=v['max']) for k, v in r['positions'].items()},
            per_client_mib=round(held['per_active_client_mib']['median'], 2),
            standbys_mib=mib(held['standby_clients']),
            owner_mib=mib(held['owner']),
            host_mib=mib(held['host']),
            total_mib=mib(held['total']),
            memory_saved_pct=round((1.0 - held['total'] / ref_held['total']) * 100.0, 1) if ref_held and ref_held['total'] else None,
            fallbacks=r['fallbacks'],
        ))

    # 3. Deep page
    deep_by = {e['result']['mode']: e['result'] for e in by.get('deep_page', [])}
    if deep_by:
        summary['deep_page'] = {
            m: dict(
                initial_median_ms=res['initial_ms']['median'],
                expand_n=res['expand_ms']['n'],
                expand_median_ms=res['expand_ms']['median'],
                expand_p95_ms=res['expand_ms']['p95'],
                expand_max_ms=res['expand_ms']['max'],
                page_step_median_ms=res['page_step_ms']['median'],
                page_step_max_ms=res['page_step_ms']['max'],
                total_pages=sum(i['pages'] for i in res['inputs']),
                total_candidates=sum(i['final_count'] for i in res['inputs']),
                parity_verified=True,
            )
            for m, res in deep_by.items()
        }

    # 4. Large personal
    large_by = {e['result']['mode']: e for e in by.get('large_personal', [])}
    if large_by:
        summary['large_personal'] = {
            m: dict(
                ready_ms=ev['result']['ready_ms'],
                steady_key_median_ms=ev['result']['steady_key_ms']['median'],
                steady_key_p95_ms=ev['result']['steady_key_ms']['p95'],
                steady_key_max_ms=ev['result']['steady_key_ms']['max'],
                learn_commit_median_ms=ev['result']['learn_commit_ms']['median'],
                learn_commit_max_ms=ev['result']['learn_commit_ms']['max'],
                post_learn_first_key_median_ms=ev['result']['post_learn_first_key_ms']['median'],
                post_learn_first_key_max_ms=ev['result']['post_learn_first_key_ms']['max'],
                client_mib=mib(ev['result']['mem_end']['private']),
                owner_mib=mib(ev['owner']['private']),
                host_mib=mib(ev['host']['private']),
                total_mib=mib(ev['result']['mem_end']['private'] + ev['owner']['private'] + ev['host']['private']),
                memory_behind=ev['result']['memory_behind'],
                fallbacks=ev['result']['fallbacks'],
            )
            for m, ev in large_by.items()
        }
    return summary


def git_info():
    def run_git(*args):
        res = subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True, check=False)
        return res.stdout.strip() if res.returncode == 0 else ''
    return dict(
        revision=run_git('rev-parse', 'HEAD'),
        dirty=bool(run_git('status', '--porcelain')),
    )


def file_hashes():
    out = {}
    for rel in TRACKED:
        p = ROOT / rel
        if p.exists():
            out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def run(benchmark_exe, engine_exe, lexicon_dir, *, mode='resident', counts=(1, 2, 4, 8), standby=4,
        cold=5, warm=10, corpus_limit=None, themes=True, large_entries=5000):
    benchmark_exe = Path(benchmark_exe).resolve()
    engine_exe = Path(engine_exe).resolve()
    lexicon_dir = Path(lexicon_dir).resolve()

    with tempfile.TemporaryDirectory(prefix='harukoto-cn02-run-') as temporary:
        root = Path(temporary)
        base = root / 'base'
        bundle.create(lexicon_dir, base, '1.0.0-cn02')
        themes_dir = root / 'themes'
        if themes:
            change(themes_dir, 'import', ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
            change(themes_dir, 'enable', 'harukoto-ff14')

        rows = corpus_rows(themes, corpus_limit)
        corpus_path = root / 'corpus.tsv'
        corpus_path.write_text('\n'.join(f'{c}\t{t}' for c, t in rows) + '\n', encoding='utf-8', newline='\n')
        keys_expected = sum(len(t) for _, t in rows)

        large_dir = root / 'large-personal'
        large_meta = None
        if large_entries > 0:
            user_n = max(50, large_entries // 5)
            large_meta = write_large_personal(large_dir, large_entries, large_entries, large_entries, user_n)

        cmd = [
            str(benchmark_exe), mode, str(engine_exe), str(base),
            str(themes_dir) if themes else '-', '-', '-',
            str(corpus_path), ','.join(str(c) for c in counts), str(standby),
            str(cold), str(warm), str(large_dir) if large_entries > 0 else '-',
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', timeout=1800)
        if proc.returncode != 0:
            raise RuntimeError(f'cn02 benchmark exited {proc.returncode}:\nSTDOUT:\n{proc.stdout[-2000:]}\nSTDERR:\n{proc.stderr[-3000:]}')

        events = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
        by = validate(events, mode, counts, standby, cold, warm, keys_expected, len(rows), has_large=(large_entries > 0))
        return dict(
            format=1,
            step='WIN-03-step-8-CN-02',
            mode=mode,
            timestamp=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
            machine=dict(os=platform.platform(), cpu=platform.processor(), logical_cpus=os.cpu_count()),
            git=git_info(),
            hashes=file_hashes(),
            config=dict(counts=list(counts), standby=standby, cold=cold, warm=warm,
                        themes=themes, corpus_items=len(rows), corpus_keys=keys_expected, large=large_meta),
            summary=summarize(by),
            events=events,
        )


def main():
    p = argparse.ArgumentParser(description='WIN-03 第 8 步 CN-02 真实延迟与内存对比基准')
    p.add_argument('--benchmark', required=True, help='harukoto_engine_cn02_benchmark.exe')
    p.add_argument('--engine', required=True, help='resident 模式传 harukoto_engine_owner_test.exe；direct 模式传 harukoto_engine_host.exe')
    p.add_argument('--lexicon', required=True, help='词库目录（如 .build/windows/ime）')
    p.add_argument('--mode', choices=('resident', 'direct'), default='resident')
    p.add_argument('--counts', type=int, nargs='+', default=[1, 2, 4, 8])
    p.add_argument('--standby', type=int, default=4)
    p.add_argument('--cold', type=int, default=5)
    p.add_argument('--warm', type=int, default=10)
    p.add_argument('--large-entries', type=int, default=5000)
    p.add_argument('--corpus-limit', type=int, default=None)
    p.add_argument('--no-themes', action='store_true')
    p.add_argument('--output', type=Path, default=None)
    args = p.parse_args()

    report = run(
        args.benchmark, args.engine, args.lexicon,
        mode=args.mode, counts=tuple(args.counts), standby=args.standby,
        cold=args.cold, warm=args.warm, corpus_limit=args.corpus_limit,
        themes=not args.no_themes, large_entries=args.large_entries,
    )
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['summary'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
