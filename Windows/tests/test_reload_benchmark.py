"""并发基准回归：共享发布、逐进程选词及采样失败不能伪装成成功。"""
from contextlib import ExitStack
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_base_dictionary_native import fixture, bundle, ROOT

READER = Path(sys.argv.pop(1))
BENCHMARK = runpy.run_path(str(ROOT / 'scripts/benchmark-reload.py'))
Host = BENCHMARK['Host']
measure_group = BENCHMARK['measure_group']
PrivateSampler = BENCHMARK['PrivateSampler']


class BenchmarkTests(unittest.TestCase):
    def test_shared_store_concurrent_versions_and_commits(self):
        with tempfile.TemporaryDirectory(prefix='harukoto-benchmark-test-') as temporary, ExitStack() as stack:
            root = Path(temporary)
            initial = root / 'initial'
            fixture(initial, '甲')
            first, second = root / 'first', root / 'second'
            ka = bundle.create(initial, first, '1.0.0')
            fixture(root / 'updated', '乙')
            kb = bundle.create(root / 'updated', second, '1.0.1')
            hosts = []
            for _ in range(3):
                host = Host(READER, root, initial)
                stack.callback(host.close)
                hosts.append(host)
            for host in hosts:
                self.assertEqual(host.line(), 'ready')
            for operation, snapshot, generation, expected in (
                    ('initial', first, ka, '测试甲'), ('update', second, kb, '测试乙'),
                    ('rollback', None, ka, '测试甲'), ('unchanged', None, ka, '测试甲')):
                if snapshot:
                    bundle.stage(snapshot, root / 'store')
                elif operation == 'rollback':
                    bundle.rollback(root / 'store')
                rows, group = measure_group(hosts)
                self.assertEqual(len(rows), 3)
                self.assertEqual(group['host_count'], 3)
                self.assertGreaterEqual(group['aggregate_samples'], 2)
                self.assertGreaterEqual(group['aggregate_sampled_peak_private'],
                                        max(group['aggregate_private_before'], group['aggregate_private_settled']))
                self.assertGreaterEqual(group['group_wall_ms'], group['command_start_spread_ms'])
                for host, row in zip(hosts, rows):
                    self.assertEqual(row['generation'], generation)
                    self.assertEqual(row['changed'], operation != 'unchanged')
                    host.send('type')
                    self.assertEqual(host.send('choose')['commit'], expected)
                    host.send('clear')
            # Failures are surfaced, and handles are closed even when sampling fails.
            samplers = []
            class FailedSampler(PrivateSampler):
                def __init__(self, processes):
                    super().__init__(processes)
                    samplers.append(self)
                def collect(self):
                    self.error = RuntimeError('injected sampler failure')
            with patch.dict(measure_group.__globals__, PrivateSampler=FailedSampler):
                with self.assertRaisesRegex(RuntimeError, 'injected sampler failure'):
                    measure_group(hosts)
            self.assertEqual(samplers[0].handles, [])
            self.assertTrue(all(host.process.poll() is None for host in hosts))


if __name__ == '__main__':
    unittest.main()
