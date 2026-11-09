import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('evaluation', ROOT / 'scripts/evaluate-chinese.py')
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)
PROBE = LEXICON = None
if '--probe' in sys.argv:
    index = sys.argv.index('--probe')
    PROBE, LEXICON = sys.argv[index + 1:index + 3]
    del sys.argv[index:index + 3]


class EvaluationTests(unittest.TestCase):
    def test_rank_missing_and_partial_are_distinct(self):
        sample = dict(id='a', category='daily', input='yedui', hans=['野队', '也对'])
        choice = lambda text, consumed=5, kind=1: dict(text=text, consumed=consumed, kind=kind)
        scored = evaluation.score(sample, dict(candidates=[choice('也对'), choice('野队')]), 'hans')
        self.assertEqual(scored['rank'], 1)
        missing = evaluation.score(sample, dict(candidates=[choice('野', 2, 3)]), 'hans')
        self.assertTrue(missing['missing_from_candidates'])
        self.assertTrue(missing['partial_first'])
        self.assertFalse(missing['unexpected_correction_first'])
        corrected = evaluation.score(sample, dict(candidates=[choice('别的', 5, 4)]), 'hans')
        self.assertTrue(corrected['unexpected_correction_first'])
        late = evaluation.score(sample, dict(candidates=[choice('别的')] * 7 + [choice('野队')]), 'hans')
        summary = evaluation.summarize([scored, missing, late])
        self.assertEqual((summary['first'], summary['top7'], summary['missing']), (1, 1, 1))

    def test_corpus_and_percentiles(self):
        rows = evaluation.corpus(ROOT / 'Windows/tests/data/chinese-evaluation.tsv')
        self.assertEqual(len(rows), 48)
        self.assertEqual(sum(r['scope'] == 'all' for r in rows), 44)
        self.assertEqual(evaluation.percentile([5, 1, 4, 2, 3], .5), 3)
        self.assertIsNone(evaluation.percentile([], .95))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'invalid.tsv'
            path.write_text('x\tdaily\tab\t甲\t甲\tall\nx\tdaily\tab\t甲\t甲\tall\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                evaluation.corpus(path)

    @unittest.skipUnless(PROBE, 'production probe not supplied')
    def test_erhua_aliases_keep_full_readings_and_weights(self):
        pairs = {tuple(line.split('|')) for line in (ROOT / 'Resources/Lexicon/erhua.tsv').read_text(encoding='utf-8').splitlines() if line and not line.startswith('#')}
        rows = [line.split('\t') for line in (Path(LEXICON) / 'native-pinyin.tsv').read_text(encoding='utf-8').splitlines()[1:]]
        rows = [r for r in rows if len(r) == 6]  # Exclude the context section.
        by_key = {(r[1], r[2], r[3]): r for r in rows}
        matched = set()
        expected = {}
        import re
        for row in rows:
            sounds = re.findall(r'[a-z]+[1-5]', row[1])
            pair = tuple(row[2:4])
            if pair in pairs and len(sounds) >= 2 and sounds[-1][:-1] == 'er' and sounds[-2][:-1] != 'er':
                alias = ''.join(sounds[:-2]) + sounds[-2][:-1] + 'r' + sounds[-2][-1]
                key = (alias, *pair)
                expected[key] = tuple(max(a, int(b)) for a, b in zip(expected.get(key, (0, 0)), row[4:]))
                matched.add(pair)
            if sounds and sounds[-1][:-1].endswith('r') and sounds[-1][:-1] != 'er' and row[3].endswith('儿'):
                self.assertIn(pair, pairs, row)
        self.assertEqual(matched, pairs)
        for key, weights in expected.items():
            self.assertEqual(tuple(map(int, by_key[key][4:])), weights)

    @unittest.skipUnless(PROBE, 'production probe not supplied')
    def test_real_probe_and_invalid_theme(self):
        run = subprocess.run([PROBE, LEXICON, 'hans'], input='yedui\nzhidaole\n', text=True, encoding='utf-8', capture_output=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        rows = [json.loads(line) for line in run.stdout.splitlines()]
        self.assertEqual(rows[0]['theme_rows'], 0)
        self.assertEqual(len(rows[1]['key_ms']), 5)
        self.assertEqual(rows[2]['candidates'][0]['text'], '知道了')
        self.assertGreater(rows[0]['private_bytes'], 0)
        with tempfile.TemporaryDirectory() as folder:
            bad = subprocess.run([PROBE, LEXICON, 'hans', folder], capture_output=True, timeout=30)
            self.assertNotEqual(bad.returncode, 0)
        bad = subprocess.run([PROBE, LEXICON, 'hans'], input='ni hao\n', text=True, capture_output=True, timeout=30)
        self.assertNotEqual(bad.returncode, 0)


if __name__ == '__main__':
    unittest.main()
