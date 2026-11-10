"""Two processes exercise the same production TSF reload code and real publisher."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

from test_base_dictionary_native import fixture, bundle

READER = Path(sys.argv.pop(1))
PRODUCTION = None
if '--lexicon' in sys.argv:
    index = sys.argv.index('--lexicon')
    PRODUCTION = Path(sys.argv[index + 1])
    del sys.argv[index:index + 2]


class ServiceReloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='harukoto-service-reload-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root / 'store'
        self.a, self.b = self.root / 'a', self.root / 'b'
        first, second = self.root / 'first', self.root / 'second'
        fixture(first, '甲'); fixture(second, '乙')
        # Make every base component observably different in the second generation.
        (second / 'japanese.tsv').write_text('ai|恋|8500\n', encoding='utf-8')
        conjugations = second / 'japanese-conjugations.bin'
        conjugations.write_bytes(conjugations.read_bytes().replace('食べる'.encode(), '食べた'.encode()))
        self.ka = bundle.create(first, self.a, '1.0.0')
        self.kb = bundle.create(second, self.b, '1.0.1')
        self.initial = first
        self.processes = []
        self.addCleanup(self.stop_processes)

    def start(self):
        process = subprocess.Popen([str(READER), str(self.root), str(self.initial)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        self.processes.append(process)
        self.assertEqual(process.stdout.readline().strip(), 'ready')
        return process

    def send(self, process, command):
        process.stdin.write(command + '\n'); process.stdin.flush()
        line = process.stdout.readline()
        self.assertTrue(line, 'native service stopped unexpectedly')
        return json.loads(line)

    def stop_processes(self):
        for process in self.processes:
            try:
                if process.poll() is None:
                    _, error = process.communicate('exit\n', timeout=15)
                    self.assertEqual(process.returncode, 0, error)
            finally:
                if process.poll() is None:
                    process.kill(); process.communicate(timeout=10)
                for pipe in (process.stdin, process.stdout, process.stderr):
                    pipe.close()

    def test_two_hosts_composition_commits_and_rollback(self):
        one, two = self.start(), self.start()
        self.assertEqual(self.send(one, 'query')['generation'], '')
        self.assertFalse(self.send(one, 'poll')['failed'])  # No user base store yet.
        bundle.stage(self.a, self.store)
        self.assertEqual(self.send(one, 'poll')['generation'], self.ka)
        self.assertEqual(self.send(two, 'poll')['generation'], self.ka)
        self.send(one, 'type')
        bundle.stage(self.b, self.store)
        still = self.send(one, 'poll')
        self.assertEqual((still['generation'], still['input'], still['first']), (self.ka, 'nihao', '测试甲'))
        updated = self.send(two, 'poll')
        self.assertEqual((updated['generation'], updated['first'], updated['conjugation']), (self.kb, '测试乙', '食べた'))
        self.assertIn('恋', updated['words']); self.assertNotIn('愛', updated['words'])
        self.assertEqual(self.send(one, 'choose')['commit'], '测试甲')
        self.assertEqual(self.send(one, 'poll')['generation'], self.ka)
        self.send(one, 'clear')
        self.assertEqual(self.send(one, 'poll')['generation'], self.kb)
        # Suspended contexts are also replaced and retain their script mode.
        self.send(one, 'saved-type')
        self.assertEqual(self.send(one, 'saved-choose')['saved_commit'], '測試乙')
        bundle.rollback(self.store)
        self.assertEqual(self.send(one, 'poll')['generation'], self.kb)
        self.send(one, 'saved-clear')
        self.assertEqual(self.send(one, 'poll')['generation'], self.ka)
        self.assertEqual(self.send(two, 'poll')['generation'], self.ka)

    def test_atomic_merge_failure_recovery_and_missing_state(self):
        host = self.start()
        personal = self.root / 'personal.tsv'
        personal.write_text('testcoffee|コーヒー|5000|こーひー|english|咖啡\n', encoding='utf-8')
        theme = self.root / 'themes'; generation = 'a' * 32
        (theme / generation).mkdir(parents=True)
        (theme / generation / 'enabled.tsv').write_text('tokyo|東京|5000|とうきょう|pinyin|东京\n', encoding='utf-8', newline='\n')
        (theme / 'CURRENT').write_text(generation, encoding='ascii')
        bundle.stage(self.a, self.store)
        self.assertIn('東京', self.send(host, 'poll')['words'])
        personal.write_text('broken', encoding='utf-8')
        bundle.stage(self.b, self.store)
        failed = self.send(host, 'poll')
        self.assertTrue(failed['failed']); self.assertEqual(failed['generation'], self.ka)
        self.assertIn('コーヒー', failed['words']); self.assertIn('愛', failed['words'])
        personal.write_text('testcoffee|珈琲|5000|こーひー|english|咖啡\n', encoding='utf-8')
        updated = self.send(host, 'poll')
        self.assertFalse(updated['failed']); self.assertEqual(updated['generation'], self.kb)
        self.assertIn('珈琲', updated['words']); self.assertIn('東京', updated['words']); self.assertIn('恋', updated['words'])
        self.assertNotIn('コーヒー', updated['words']); self.assertNotIn('愛', updated['words'])
        # Later personal-only changes must keep the newest base, not resurrect A.
        personal.write_text('', encoding='utf-8')
        updated = self.send(host, 'poll')
        self.assertIn('恋', updated['words']); self.assertNotIn('珈琲', updated['words'])
        state = (self.store / 'STATE.json').read_bytes()
        (self.store / 'STATE.json').unlink()
        self.assertTrue(self.send(host, 'poll')['failed'])
        (self.store / 'STATE.json').write_bytes(state)
        self.assertFalse(self.send(host, 'poll')['failed'])
        # Publish a container-valid but semantically broken new native lexicon.
        source = self.root / 'bad-source'; fixture(source, '丙')
        (source / 'native-pinyin.tsv').write_bytes(b'MEOWPUUT-PINYIN-2\nbroken\n')
        bad = self.root / 'bad'; bundle.create(source, bad, '1.0.2')
        bundle.stage(bad, self.store)
        failed = self.send(host, 'poll')
        self.assertTrue(failed['failed']); self.assertEqual(failed['generation'], self.kb)
        bundle.rollback(self.store)
        self.assertFalse(self.send(host, 'poll')['failed'])

    def test_focus_cancel_and_shutdown_during_loading(self):
        host = self.start()
        bundle.stage(self.a, self.store); self.send(host, 'poll')
        self.send(host, 'type'); self.send(host, 'saved-type')
        bundle.stage(self.b, self.store)
        self.assertEqual(self.send(host, 'poll')['generation'], self.ka)
        self.assertEqual(self.send(host, 'focus')['input'], '')
        self.assertEqual(self.send(host, 'poll')['generation'], self.kb)
        # No wait in the service destructor; a worker only owns independent inputs.
        fresh = self.start()
        _, error = fresh.communicate('start-exit\n', timeout=15)
        self.assertEqual(fresh.returncode, 0, error)

    @unittest.skipUnless(PRODUCTION, 'production lexicon not supplied')
    def test_production_dictionary(self):
        self.initial = PRODUCTION
        first, second = self.root / 'full-first', self.root / 'full-second'
        ka = bundle.create(PRODUCTION, first, '0.4.4-alpha.20')
        kb = bundle.create(PRODUCTION, second, '0.4.4-alpha.20.test')
        host = self.start()
        bundle.stage(first, self.store)
        state = self.send(host, 'poll')
        self.assertEqual((state['generation'], state['first']), (ka, '你好'))
        self.send(host, 'type')
        bundle.stage(second, self.store)
        self.assertEqual(self.send(host, 'poll')['generation'], ka)
        self.assertEqual(self.send(host, 'choose')['commit'], '你好')
        self.send(host, 'clear')
        self.assertEqual(self.send(host, 'poll')['generation'], kb)
        bundle.rollback(self.store)
        self.assertEqual(self.send(host, 'poll')['generation'], ka)


if __name__ == '__main__':
    unittest.main()
