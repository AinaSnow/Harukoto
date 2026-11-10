"""多进程逐键与总私有内存对照；固定文本、临时快照，不修改输入法安装。"""
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
BASE_CASES = ('nihao', 'xinkule', 'riben', 'eat', 'nihaoz')
MODES = ('local', 'isolated', 'shared')
TRACKED = ('Windows/EngineProcess.h', 'Windows/EngineHost.h', 'Windows/EngineHost.cpp',
           'Windows/EnginePipe.h', 'Windows/EngineProtocol.h', 'Windows/EngineMessages.h',
           'Windows/EngineExecutor.h', 'Windows/EngineServer.h', 'Windows/EngineWindows.h',
           'Windows/tests/EngineHostTestClient.h', 'Windows/tests/EngineIpcBenchmark.cpp',
           'Windows/MixedComposition.h', 'Windows/WordEvidence.h', 'Core/MeowpuutCore.cpp',
           'scripts/benchmark-engine-ipc.py', 'scripts/base_dictionary.py', 'scripts/japanese_packages.py')


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def validate(events, runs, count, mode, themes):
    cases = (*BASE_CASES, 'tejihuifuyao') if themes else BASE_CASES
    if not events or any(not isinstance(event, dict) for event in events) or events[0].get('type') != 'group':
        raise ValueError('缺少进程组记录')
    group = events[0]
    engines = 0 if mode == 'local' else 1 if mode == 'shared' else count
    if (set(group) != {'type', 'mode', 'clients', 'runs', 'engine_processes', 'prepare_ms', 'wall_ms', 'memory', 'release_counter', 'counter_frequency'} or
            group['mode'] != mode or type(group['clients']) is not int or group['clients'] != count or
            type(group['runs']) is not int or group['runs'] != runs or type(group['engine_processes']) is not int or
            group['engine_processes'] != engines or not finite(group['prepare_ms']) or not finite(group['wall_ms']) or
            any(type(group[n]) is not int or group[n] <= 0 for n in ('release_counter', 'counter_frequency'))):
        raise ValueError('进程组或时间记录无效')
    memory = group['memory']
    if (not isinstance(memory, dict) or set(memory) != {'ready', 'sampled_peak', 'held', 'samples', 'interval_ms', 'max_sweep_ms'} or
            type(memory['samples']) is not int or memory['samples'] < 3 or type(memory['interval_ms']) is not int or
            memory['interval_ms'] != 10 or not finite(memory['max_sweep_ms'])):
        raise ValueError('内存采样信息无效')
    for stage in ('ready', 'sampled_peak', 'held'):
        row = memory[stage]
        if (not isinstance(row, dict) or set(row) != {'controller', 'clients', 'engines', 'total'} or
                any(type(v) is not int or v < 0 for v in row.values()) or
                row['total'] != row['controller']+row['clients']+row['engines'] or
                not row['controller'] or not row['clients'] or bool(row['engines']) != bool(engines)):
            raise ValueError('内存总量必须来自同一轮采样求和')
    if memory['sampled_peak']['total'] < max(memory[s]['total'] for s in ('ready', 'held')):
        raise ValueError('采样峰值小于已观察值')
    samples, commits, clients = {}, {}, {}
    for event in events[1:]:
        client = event.get('client')
        if type(client) is not int or not 0 <= client < count:
            raise ValueError('客户端编号无效')
        if event.get('type') == 'client':
            if (set(event) != {'type', 'client', 'engine', 'verified_commits', 'started_counter', 'finished_counter'} or client in clients or
                    type(event['engine']) is not int or not 0 < event['engine'] < 2**64 or
                    type(event['verified_commits']) is not int or event['verified_commits'] != len(cases)*runs or
                    type(event['started_counter']) is not int or type(event['finished_counter']) is not int or
                    not group['release_counter'] <= event['started_counter'] <= event['finished_counter']):
                raise ValueError('客户端代次或选择验证无效')
            clients[client] = event
            continue
        case, run = event.get('case'), event.get('run')
        if case not in cases or type(run) is not int or not 0 <= run < runs:
            raise ValueError('场景或轮次无效')
        if event.get('type') == 'sample':
            key = event.get('key')
            if (set(event) != {'type', 'client', 'case', 'run', 'key', 'request_ms'} or type(key) is not int or
                    not 1 <= key <= len(case) or not finite(event['request_ms']) or (client, case, run, key) in samples):
                raise ValueError('逐键样本无效或重复')
            samples[client, case, run, key] = event
        elif event.get('type') == 'commit':
            if (set(event) != {'type', 'client', 'case', 'run', 'verified'} or event['verified'] is not True or
                    (client, case, run) in commits):
                raise ValueError('选词验证无效或重复')
            commits[client, case, run] = event
        else:
            raise ValueError('未知测量输出')
    expected = {(client, case, run) for client in range(count) for case in cases for run in range(runs)}
    if (set(commits) != expected or set(clients) != set(range(count)) or
            set(samples) != {(*identity, key) for identity in expected for key in range(1, len(identity[1])+1)}):
        raise ValueError('测量输出不完整')
    generations = {row['engine'] for row in clients.values()}
    if (mode == 'shared' and len(generations) != 1 or mode == 'isolated' and len(generations) != count or
            mode == 'local' and generations != {42}):
        raise ValueError('共享／独占进程代次不符合模式')
    return group, list(samples.values()), list(commits.values()), list(clients.values())


def summarize(group, samples, clients):
    values = [row['request_ms'] for row in samples]
    return dict(scenario=group['scenario'], mode=group['mode'], clients=group['clients'], samples=len(samples),
                request_ms=dict(median=statistics.median(values), maximum=max(values), sum=sum(values)),
                prepare_ms=group['prepare_ms'], wall_ms=group['wall_ms'], memory=group['memory'],
                start_spread_ms=(max(c['started_counter'] for c in clients)-min(c['started_counter'] for c in clients))*1000/group['counter_frequency'],
                common_work_ms=max(0, min(c['finished_counter'] for c in clients)-max(c['started_counter'] for c in clients))*1000/group['counter_frequency'])


def run(reader, host, lexicon, runs, counts=(1, 2, 4), modes=MODES):
    if (type(runs) is not int or not 1 <= runs <= 20 or not counts or len(counts) != len(set(counts)) or
            any(type(n) is not int or n not in (1, 2, 4) for n in counts) or not modes or
            len(modes) != len(set(modes)) or any(m not in MODES for m in modes)):
        raise ValueError('测量配置无效')
    reader, host, lexicon = Path(reader).resolve(), Path(host).resolve(), Path(lexicon).resolve()
    theme = ROOT / 'Resources/DictionaryPackages/ff14/full'
    def identities():
        return dict(sources={name: bundle.digest(ROOT / name) for name in TRACKED},
                    programs={name: bundle.digest(path) for name, path in [('reader', reader), ('host', host)]},
                    base_files={name: dict(size=(lexicon/name).stat().st_size, sha256=bundle.digest(lexicon/name)) for name in bundle.FILES},
                    ff14_files={name: bundle.digest(theme/name) for name in ('manifest.json', 'ff14-words.txt')})
    identity = identities()
    provenance = dict(revision=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                      source_dirty=bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip()),
                      platform=platform.platform(), logical_cpus=os.cpu_count(), runs=runs, counts=list(counts), modes=list(modes), **identity)
    groups, rows, commits, clients, summary = [], [], [], [], []
    with tempfile.TemporaryDirectory(prefix='harukoto-engine-ipc-') as temporary:
        root = Path(temporary)
        bundle.create(lexicon, root/'词库 快照', '1.0.0-engine-ipc-benchmark')
        change(root/'themes', 'import', theme/'manifest.json')
        change(root/'themes', 'enable', 'harukoto-ff14')
        for scenario in ('base', 'ff14'):
            for count in counts:
                for mode in modes:
                    output = root/f'{scenario}-{count}-{mode}'
                    output.mkdir()
                    result = subprocess.run([str(reader), str(host), str(root/'词库 快照'),
                        str(root/'themes') if scenario == 'ff14' else '-', str(count), str(runs), mode, str(output)],
                        capture_output=True, text=True, encoding='utf-8', timeout=240, check=True)
                    group, samples, confirmed, owners = validate([json.loads(line) for line in result.stdout.splitlines()], runs, count, mode, scenario == 'ff14')
                    group['scenario'] = scenario
                    groups.append(group); summary.append(summarize(group, samples, owners))
                    tags = dict(scenario=scenario, mode=mode, clients=count)
                    rows.extend(dict(**tags, **sample) for sample in samples)
                    commits.extend(dict(**tags, **row) for row in confirmed)
                    clients.extend(dict(**tags, **row) for row in owners)
        bundle.verify(root/'词库 快照')
    if identity != identities():
        raise ValueError('测量期间程序、源代码或词库改变')
    return dict(format=1, provenance=provenance, groups=groups, samples=rows, commits=commits, clients=clients, summary=summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reader', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--lexicon', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=3, choices=range(1, 21))
    parser.add_argument('--counts', type=int, nargs='+', default=[1, 2, 4], choices=(1, 2, 4))
    parser.add_argument('--modes', nargs='+', default=list(MODES), choices=MODES)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = run(args.reader, args.host, args.lexicon, args.runs, args.counts, args.modes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    bundle.atomic_json(args.output, report)
    print(json.dumps(report['summary'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
