"""验证逐键基准的真实执行、分阶段数据和不完整报告拒绝。"""
from contextlib import closing
import copy
import math
from pathlib import Path
import runpy
import sqlite3
import sys
import tempfile
import unittest

from test_base_dictionary_native import fixture, ROOT

READER = Path(sys.argv.pop(1))
BENCHMARK = runpy.run_path(str(ROOT / 'scripts/benchmark-engine.py'))


class BenchmarkTests(unittest.TestCase):
    def test_real_driver_and_validation(self):
        with tempfile.TemporaryDirectory(prefix='harukoto-query-profile-test-') as temporary:
            source = Path(temporary) / 'source'
            fixture(source, '甲')
            (source / 'native-pinyin.tsv').write_text(
                'MEOWPUUT-PINYIN-2\nnihao\tni3hao3\t你好\t你好\t9500\t9500\n'
                'xinkule\txin1ku3le5\t辛苦了\t辛苦了\t9500\t9500\n', encoding='utf-8', newline='\n')
            with closing(sqlite3.connect(source / 'language-evidence.sqlite')) as db:
                db.execute('DELETE FROM lexicon')
                db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', ('nihao', '你好', '你好', 9500, 'ni3hao3', 9500))
                db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', ('xinkule', '辛苦了', '辛苦了', 9500, 'xin1ku3le5', 9500))
                db.commit()
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            report = BENCHMARK['run'](READER, source, 1)
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})
            self.assertEqual(report['format'], 1)
            self.assertEqual(len(report['summary']), 4)
            self.assertEqual(sum(row['verified_commits'] for row in report['loads']), 22)
            self.assertEqual(len(report['samples']), 128)
            self.assertEqual(report['provenance']['reader_sha256'], BENCHMARK['bundle'].digest(READER))
            self.assertEqual(set(report['provenance']['base_files']), set(before))
            for row in report['summary']:
                self.assertEqual(row['samples'], 38 if row['scenario'] == 'ff14' else 26)
                self.assertTrue(all(math.isfinite(m['sum']) and m['sum'] >= 0 for m in row['metrics'].values()))
            # Reconstruct a complete base/direct transcript; the driver has
            # already verified exact selection and duplicate-selection replay.
            ready = report['loads'][0]
            events = [{k: ready[k] for k in ('type', 'generation', 'load_ms')}]
            events += [{k: v for k, v in row.items() if k not in ('scenario', 'mode')}
                       for row in report['samples'] if row['scenario'] == 'base' and row['mode'] == 'direct']
            events += [dict(type='commit', case=case, run=0, verified=True) for case in BENCHMARK['BASE_CASES']]
            validate = BENCHMARK['validate']
            validate(events, 1, False)
            variants = []
            missing = copy.deepcopy(events); missing.pop(1); variants.append(missing)
            duplicate = copy.deepcopy(events); duplicate.append(duplicate[1]); variants.append(duplicate)
            invalid = copy.deepcopy(events); invalid[1]['query']['total_ms'] = float('nan'); variants.append(invalid)
            invalid = copy.deepcopy(events); invalid[1]['query']['total_ms'] += 10; variants.append(invalid)
            invalid = copy.deepcopy(events); invalid[-1]['verified'] = False; variants.append(invalid)
            invalid = copy.deepcopy(events); invalid[1]['key'] = True; variants.append(invalid)
            for malformed in variants:
                with self.assertRaises(ValueError):
                    validate(malformed, 1, False)


if __name__ == '__main__':
    unittest.main()
