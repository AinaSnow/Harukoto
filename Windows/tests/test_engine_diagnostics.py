"""实验引擎诊断命令的跨进程检查，不接触安装和用户数据。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

WRITER, PROBE = map(Path, sys.argv[1:3])
del sys.argv[1:3]


class EngineDiagnosticsTests(unittest.TestCase):
    def probe(self, pid, code=0):
        result = subprocess.run([PROBE, '--process', str(pid), '--json'],
                                capture_output=True, text=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, code, result.stderr)
        return json.loads(result.stdout)

    def test_native_contract(self):
        subprocess.run([WRITER], check=True, timeout=15)

    def test_cli_observations(self):
        with subprocess.Popen([WRITER, '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              text=True, encoding='utf-8') as writer:
            try:
                pid = int(writer.stdout.readline())
                self.assertEqual(pid, writer.pid)
                before = self.probe(pid)
                self.assertEqual(before['scope'], 'experimental_engine')
                self.assertIs(before['trusted_discovery'], False)
                self.assertEqual(before['availability'], 'observed')
                snapshot = before['snapshot']
                self.assertEqual(snapshot['root']['pid'], pid)
                self.assertEqual(snapshot['state'], 'ready')
                self.assertIsNone(snapshot['last_inspect'])
                self.assertEqual(snapshot['control']['state'], 'disabled')
                time.sleep(0.04)
                after = self.probe(pid)['snapshot']
                self.assertEqual(after['published_tick_ms'], snapshot['published_tick_ms'])
                self.assertGreater(after['publication_age_ms'], snapshot['publication_age_ms'])
                self.assertEqual(after['polled_tick_ms'], snapshot['polled_tick_ms'])
                # Reading cannot signal a request, refresh a publication or
                # mistake frozen metadata for a heartbeat.
                for key in ('input', 'candidates', 'pipe', 'handle', 'path', 'personal'):
                    self.assertNotIn(f'"{key}"', json.dumps(after))
                text = subprocess.run([PROBE, '--process', str(pid)], capture_output=True,
                                      text=True, encoding='utf-8', timeout=10)
                self.assertEqual(text.returncode, 0)
                self.assertIn('只读诊断', text.stdout)
                writer.stdin.write('burst\n'); writer.stdin.flush()
                self.assertEqual(writer.stdout.readline().strip(), 'bursting')
                for _ in range(8):
                    result = subprocess.run([PROBE, '--process', str(pid), '--json'], capture_output=True,
                                            text=True, encoding='utf-8', timeout=10)
                    row = json.loads(result.stdout)
                    self.assertIn(row['availability'], ('observed', 'inconsistent'))
                    self.assertEqual(result.returncode, 0 if row['availability'] == 'observed' else 2)
                    if row['snapshot'] is not None:
                        s = row['snapshot']
                        self.assertIn((s['state'], s['supervisor_claimed_total']), (('ready', 345), ('stopping', 987)))
                writer.stdin.write('stop-burst\n'); writer.stdin.flush()
                self.assertEqual(writer.stdout.readline().strip(), 'stopped')
                writer.stdin.write('update\n'); writer.stdin.flush()
                self.assertEqual(writer.stdout.readline().strip(), 'updated')
                self.assertEqual(self.probe(pid)['snapshot']['state'], 'stopping')
                writer.stdin.write('exit\n'); writer.stdin.flush(); writer.wait(timeout=10)
                self.assertEqual(self.probe(pid, 2)['availability'], 'unavailable')
            finally:
                if writer.poll() is None:
                    writer.kill(); writer.wait(timeout=10)

    def test_missing_and_arguments(self):
        row = self.probe(os.getpid(), 2)  # Process alive, mapping absent.
        self.assertEqual(row['availability'], 'unavailable')
        self.assertIsNone(row['snapshot'])
        for args in ([], ['--process', '0'], ['--process', '-1'], ['--process', '4294967296'],
                     ['--process', '1z'], ['--process', '1', '--process', '2'], ['--json']):
            result = subprocess.run([PROBE, *args], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 1)


if __name__ == '__main__':
    unittest.main()
