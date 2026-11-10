"""测量真实 TSF 服务代码的词库切换；使用临时仓库，不修改安装或用户配置。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import ctypes
from ctypes import wintypes
import json
import platform
from pathlib import Path
import queue
import statistics
import subprocess
import tempfile
import threading
import time

import base_dictionary as bundle
from japanese_packages import change

ROOT = Path(__file__).resolve().parents[1]
METRICS = ('dispatch_ms', 'background_wait_ms', 'apply_ms', 'private_before',
           'private_ready', 'private_after', 'sampled_peak_private')


def summarize(rows):
    result = []
    for scenario, operation in sorted({(r['scenario'], r['operation']) for r in rows}):
        samples = [r for r in rows if (r['scenario'], r['operation']) == (scenario, operation)]
        metrics = {}
        for name in (*METRICS, 'cleanup_wait_ms', 'private_settled'):
            if not all(name in r for r in samples):
                continue
            values = [r[name] for r in samples]
            metrics[name] = dict(median=statistics.median(values), maximum=max(values))
        result.append(dict(scenario=scenario, operation=operation, samples=len(samples), metrics=metrics))
    return result


class PrivateSampler:
    """Sample the sum at one sweep, never add unrelated per-process peaks."""
    def __init__(self, hosts):
        class Counters(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
                'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]
        self.Counters = Counters
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.kernel.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        self.kernel.K32GetProcessMemoryInfo.restype = wintypes.BOOL
        self.handles = []
        self.peak = self.count = 0
        self.max_sweep_ms = 0.0
        self.error = None
        self.stop = threading.Event()
        try:
            for host in hosts:
                handle = self.kernel.OpenProcess(0x410, False, host.process.pid)
                if not handle:
                    raise ctypes.WinError(ctypes.get_last_error())
                self.handles.append(handle)
        except BaseException:
            self.close()
            raise

    def sample(self):
        start = time.perf_counter()
        total = 0
        for handle in self.handles:
            info = self.Counters()
            info.cb = ctypes.sizeof(info)
            if not self.kernel.K32GetProcessMemoryInfo(handle, ctypes.byref(info), info.cb):
                raise ctypes.WinError(ctypes.get_last_error())
            total += info.PrivateUsage
        self.peak = max(self.peak, total)
        self.count += 1
        self.max_sweep_ms = max(self.max_sweep_ms, (time.perf_counter() - start) * 1000)
        return total

    def collect(self):
        try:
            while not self.stop.wait(0.005):
                self.sample()
        except BaseException as error:
            self.error = error

    def close(self):
        for handle in self.handles:
            self.kernel.CloseHandle(handle)
        self.handles.clear()


def measure_group(hosts):
    """Release commands together; report actual send spread, not exact simultaneity."""
    barrier = threading.Barrier(len(hosts) + 1, timeout=60)
    def measure(host):
        barrier.wait()
        started = time.perf_counter()
        result = host.send('measure')
        return started, result
    sampler = PrivateSampler(hosts)
    monitor = threading.Thread(target=sampler.collect, daemon=True)
    try:
        before = sampler.sample()
        monitor.start()
        with ThreadPoolExecutor(max_workers=len(hosts)) as pool:
            futures = [pool.submit(measure, host) for host in hosts]
            start = time.perf_counter()
            barrier.wait()
            pairs = [future.result() for future in futures]
            wall = (time.perf_counter() - start) * 1000
        sampler.stop.set()
        monitor.join()
        if sampler.error:
            raise sampler.error
        settled = sampler.sample()
        starts, results = zip(*pairs)
        return list(results), dict(host_count=len(hosts), command_start_spread_ms=(max(starts) - min(starts)) * 1000,
                                  group_wall_ms=wall, aggregate_private_before=before,
                                  aggregate_sampled_peak_private=sampler.peak, aggregate_private_settled=settled,
                                  aggregate_samples=sampler.count, max_sample_sweep_ms=sampler.max_sweep_ms)
    finally:
        sampler.stop.set()
        if monitor.ident is not None:
            monitor.join()
        sampler.close()


class Host:
    def __init__(self, reader, root, lexicon):
        self.process = subprocess.Popen([str(reader), str(root), str(lexicon)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8')
        self.lines = queue.Queue()
        def collect():
            for line in self.process.stdout:
                self.lines.put(line.rstrip('\n'))
            self.lines.put(None)
        self.thread = threading.Thread(target=collect, daemon=True)
        self.thread.start()

    def line(self):
        try:
            value = self.lines.get(timeout=60)
        except queue.Empty as error:
            raise RuntimeError('测量进程响应超时') from error
        if value is None:
            raise RuntimeError('测量进程意外退出')
        return value

    def send(self, command):
        self.process.stdin.write(command + '\n'); self.process.stdin.flush()
        return json.loads(self.line())

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write('exit\n'); self.process.stdin.flush()
                self.process.wait(timeout=10)
            if self.process.returncode:
                raise RuntimeError(f'测量进程退出码 {self.process.returncode}')
        finally:
            if self.process.poll() is None:
                self.process.kill(); self.process.wait(timeout=10)
            self.thread.join(timeout=5)
            self.process.stdin.close(); self.process.stdout.close()


def run(args):
    rows = []
    groups = []
    with tempfile.TemporaryDirectory(prefix='harukoto-reload-benchmark-') as temporary:
        root = Path(temporary)
        first, second = root / 'first', root / 'second'
        ka = bundle.create(args.lexicon, first, '1.0.0-benchmark.a')
        kb = bundle.create(args.lexicon, second, '1.0.0-benchmark.b')
        manifest = bundle.verify(first)
        for scenario in ('base', 'ff14'):
            for repetition in range(args.runs):
                case = root / f'{scenario}-{repetition}'
                case.mkdir()
                if scenario == 'ff14':
                    change(case / 'themes', 'import', args.theme)
                    change(case / 'themes', 'enable', 'harukoto-ff14')
                with ExitStack() as stack:
                    hosts = []
                    for _ in range(args.hosts):
                        host = Host(args.reader, case, args.lexicon)
                        stack.callback(host.close)
                        hosts.append(host)
                    for host in hosts:
                        if host.line() != 'ready':
                            raise RuntimeError('测量进程初始化失败')
                    for operation, snapshot, generation in (('initial', first, ka), ('update', second, kb),
                                                             ('rollback', None, ka), ('unchanged', None, ka)):
                        if operation == 'rollback':
                            bundle.rollback(case / 'store')
                        elif snapshot:
                            bundle.stage(snapshot, case / 'store')
                        results, group = measure_group(hosts)
                        groups.append(dict(scenario=scenario, repetition=repetition + 1, operation=operation, **group))
                        for number, (host, result) in enumerate(zip(hosts, results), 1):
                            if result['generation'] != generation or result['changed'] != (operation != 'unchanged'):
                                raise RuntimeError('未测到预期版本切换')
                            # Validate every host outside the timed section.
                            host.send('type')
                            if host.send('choose')['commit'] != '你好':
                                raise RuntimeError('切换后候选提交错误')
                            host.send('clear')
                            rows.append(dict(scenario=scenario, repetition=repetition + 1, host=number, operation=operation, **result))
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True).strip())
    return dict(format=2, source_revision=revision, source_dirty=dirty,
                environment=dict(system=platform.system(), release=platform.release(), version=platform.version(),
                                 machine=platform.machine(), processor=platform.processor()),
                reader_sha256=bundle.digest(args.reader), benchmark_sha256=bundle.digest(Path(__file__)), base_files=manifest['files'],
                theme_manifest_sha256=bundle.digest(args.theme),
                theme_words_sha256=bundle.digest(args.theme.parent / 'ff14-words.txt'),
                method=dict(runs=args.runs, host_count=args.hosts, context_count=2, memory='process private bytes, ~1 ms samples during background wait',
                            concurrency='independent service test processes sharing one temporary store; barrier before measure commands; observed Python send spread is not native dispatch skew',
                            aggregate_memory='sum of private bytes per sequential process sweep, requested 5 ms sampling interval; records sample count and maximum sweep duration; excludes controller and file cache; not a strict or atomic peak',
                            group_wall='barrier release through all measure responses, including cleanup wait and pipe overhead; excludes publication and candidate validation',
                            cache='uncontrolled warm cache: snapshots copied and hashed before each publication',
                            background='wall wait after dispatch, includes scheduling/sampling; not pure worker CPU time',
                            apply='production pollDictionaries on owner thread; includes synchronous cleanup before optimization or cleanup dispatch after optimization, plus runtime status',
                            cleanup='when supported, wait for detached cleanup after timing apply; private_settled sampled afterwards',
                            limits='no TSF host insertion/rendering; peak is sampled lower bound; apply peak not continuously sampled; two versions share data bytes'),
                samples=rows, groups=groups, summary=summarize(rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reader', type=Path, required=True)
    parser.add_argument('--lexicon', type=Path, required=True)
    parser.add_argument('--theme', type=Path, default=ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--hosts', type=int, default=1, help='同时运行的独立服务测试进程数（1～8）')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.runs <= 20:
        parser.error('--runs 必须为 1～20')
    if not 1 <= args.hosts <= 8:
        parser.error('--hosts 必须为 1～8')
    for name in ('reader', 'lexicon', 'theme'):
        setattr(args, name, getattr(args, name).resolve())
    report = run(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for row in report['summary']:
        metrics = row['metrics']
        print(f"{row['scenario']}/{row['operation']}: apply median/max "
              f"{metrics['apply_ms']['median']:.3f}/{metrics['apply_ms']['maximum']:.3f} ms; "
              f"wait median {metrics['background_wait_ms']['median']:.1f} ms; "
              f"sampled peak max {metrics['sampled_peak_private']['maximum'] / 1048576:.1f} MiB")


if __name__ == '__main__':
    main()
