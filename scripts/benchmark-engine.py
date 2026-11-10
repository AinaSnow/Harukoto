"""真实引擎逐键查询分阶段测量；仅使用临时快照，不接触用户安装。"""
import argparse
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

import base_dictionary as bundle
from japanese_packages import change

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ('setup_ms', 'replay_ms', 'sqlite_open_ms', 'merge_ms', 'expansion_ms',
          'conversion_ms', 'cleanup_ms', 'total_ms')
BASE_CASES = ('nihao', 'xinkule', 'riben', 'eat', 'nihaoz')


def validate(events, runs, themes):
    cases = (*BASE_CASES, 'tejihuifuyao') if themes else BASE_CASES
    if not events or events[0].get('type') != 'ready':
        raise ValueError('缺少引擎准备记录')
    ready = events[0]
    if (set(ready) != {'type', 'generation', 'load_ms'} or
            not isinstance(ready['generation'], str) or len(ready['generation']) != 64 or
            any(c not in '0123456789abcdef' for c in ready['generation']) or
            type(ready['load_ms']) not in (int, float) or not math.isfinite(ready['load_ms']) or ready['load_ms'] < 0):
        raise ValueError('引擎准备记录无效')
    samples, commits = {}, set()
    for event in events[1:]:
        case, run = event.get('case'), event.get('run')
        if case not in cases or type(run) is not int or not 0 <= run < runs:
            raise ValueError('测量场景或轮次无效')
        if event.get('type') == 'sample':
            key, metrics = event.get('key'), event.get('query')
            if (set(event) != {'type', 'case', 'run', 'key', 'request_ms', 'query'} or
                    type(key) is not int or not 1 <= key <= len(case) or not isinstance(metrics, dict) or set(metrics) != set(FIELDS)):
                raise ValueError('逐键记录字段无效')
            values = [event['request_ms'], *metrics.values()]
            if any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in values):
                raise ValueError('测量时间必须有限且非负')
            if (abs(sum(metrics[f] for f in FIELDS[:-1]) - metrics['total_ms']) > 0.001 or
                    metrics['total_ms'] > event['request_ms'] + 0.001):
                raise ValueError('分阶段时间不能超过总时间')
            identity = (case, run, key)
            if identity in samples:
                raise ValueError('逐键记录重复')
            samples[identity] = event
        elif event.get('type') == 'commit':
            if set(event) != {'type', 'case', 'run', 'verified'} or event['verified'] is not True or (case, run) in commits:
                raise ValueError('选词验证缺失或重复')
            commits.add((case, run))
        else:
            raise ValueError('未知引擎输出')
    expected = {(case, run, key) for case in cases for run in range(runs) for key in range(1, len(case)+1)}
    if set(samples) != expected or commits != {(case, run) for case in cases for run in range(runs)}:
        raise ValueError('测量记录不完整')
    return ready, list(samples.values())


def summarize(rows):
    summary = []
    for scenario, mode in sorted({(r['scenario'], r['mode']) for r in rows}):
        selected = [r for r in rows if (r['scenario'], r['mode']) == (scenario, mode)]
        metrics = {}
        for name in ('request_ms', *FIELDS):
            values = [r['request_ms'] if name == 'request_ms' else r['query'][name] for r in selected]
            metrics[name] = dict(median=statistics.median(values), maximum=max(values), sum=sum(values))
        summary.append(dict(scenario=scenario, mode=mode, samples=len(selected), metrics=metrics))
    return summary


def run(reader, lexicon, runs):
    reader, lexicon = Path(reader).resolve(), Path(lexicon).resolve()
    files = {name: dict(size=(lexicon / name).stat().st_size, sha256=bundle.digest(lexicon / name)) for name in bundle.FILES}
    tracked = ('Windows/EngineWindows.h', 'Windows/EngineExecutor.h', 'Windows/EngineServer.h',
               'Windows/tests/EngineBenchmark.cpp', 'scripts/benchmark-engine.py',
               'Core/MeowpuutCore.cpp', 'Windows/MixedComposition.h', 'Windows/WordEvidence.h')
    source = {name: bundle.digest(ROOT / name) for name in tracked}
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip())
    theme = ROOT / 'Resources/DictionaryPackages/ff14/full'
    provenance = dict(revision=revision, source_dirty=dirty, sources=source, reader_sha256=bundle.digest(reader),
                      base_files=files, ff14_files={name: bundle.digest(theme / name) for name in ('manifest.json', 'ff14-words.txt')},
                      platform=platform.platform(), runs=runs)
    rows, loads = [], []
    with tempfile.TemporaryDirectory(prefix='harukoto-engine-benchmark-') as temporary:
        folder = Path(temporary)
        bundle.create(lexicon, folder / 'base', '1.0.0-engine-benchmark')
        change(folder / 'themes', 'import', theme / 'manifest.json')
        change(folder / 'themes', 'enable', 'harukoto-ff14')
        for scenario in ('base', 'ff14'):
            for mode in ('direct', 'executor'):
                process = subprocess.run([str(reader), str(folder / 'base'), str(folder / 'themes') if scenario == 'ff14' else '-',
                                          str(runs), mode], capture_output=True, text=True, encoding='utf-8', timeout=600, check=True)
                ready, samples = validate([json.loads(line) for line in process.stdout.splitlines()], runs, scenario == 'ff14')
                loads.append(dict(scenario=scenario, mode=mode, verified_commits=runs * (len(BASE_CASES) + (scenario == 'ff14')), **ready))
                rows.extend(dict(scenario=scenario, mode=mode, **sample) for sample in samples)
    if source != {name: bundle.digest(ROOT / name) for name in tracked} or provenance['reader_sha256'] != bundle.digest(reader):
        raise ValueError('测量期间程序或源文件改变')
    if files != {name: dict(size=(lexicon / name).stat().st_size, sha256=bundle.digest(lexicon / name)) for name in bundle.FILES}:
        raise ValueError('测量期间基础词库改变')
    if provenance['ff14_files'] != {name: bundle.digest(theme / name) for name in provenance['ff14_files']}:
        raise ValueError('测量期间 FF14 词库改变')
    return dict(format=1, provenance=provenance, loads=loads, samples=rows, summary=summarize(rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reader', type=Path, required=True)
    parser.add_argument('--lexicon', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=3, choices=range(1, 21))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = run(args.reader, args.lexicon, args.runs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['summary'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
