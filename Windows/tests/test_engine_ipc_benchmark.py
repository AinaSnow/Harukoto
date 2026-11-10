"""验证多进程三种模式、总内存口径与不完整／失真报告拒绝。"""
from contextlib import closing
import copy
import json
from pathlib import Path
import runpy
import sqlite3
import sys
import tempfile
import unittest

from test_base_dictionary_native import fixture, ROOT

READER = Path(sys.argv.pop(1))
HOST = Path(sys.argv.pop(1))
BENCHMARK = runpy.run_path(str(ROOT/'scripts/benchmark-engine-ipc.py'))


class IpcBenchmarkTests(unittest.TestCase):
    def test_real_matrix_and_validation(self):
        with tempfile.TemporaryDirectory(prefix='harukoto-ipc-profile-test-') as temporary:
            source = Path(temporary)/'基础 词表'
            fixture(source, '甲')
            (source/'native-pinyin.tsv').write_text(
                'MEOWPUUT-PINYIN-2\nnihao\tni3hao3\t你好\t你好\t9500\t9500\n'
                'xinkule\txin1ku3le5\t辛苦了\t辛苦了\t9500\t9500\n', encoding='utf-8', newline='\n')
            with closing(sqlite3.connect(source/'language-evidence.sqlite')) as db:
                db.execute('DELETE FROM lexicon')
                for reading, word, toned in [('nihao', '你好', 'ni3hao3'), ('xinkule', '辛苦了', 'xin1ku3le5')]:
                    db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', (reading, word, word, 9500, toned, 9500))
                db.commit()
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            report = BENCHMARK['run'](READER, HOST, source, 1)
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})
            self.assertEqual(len(report['groups']), 18)
            self.assertEqual(len(report['summary']), 18)
            self.assertEqual(len(report['samples']), 1344)
            self.assertEqual(len(report['commits']), 231)
            self.assertEqual(len(report['clients']), 42)
            self.assertEqual(set(report['provenance']['base_files']), set(before))
            self.assertEqual(report['provenance']['programs']['host'], BENCHMARK['bundle'].digest(HOST))
            self.assertNotIn(temporary, json.dumps(report))
            for row in report['summary']:
                expected = (38 if row['scenario'] == 'ff14' else 26)*row['clients']
                self.assertEqual(row['samples'], expected)
                self.assertGreaterEqual(row['common_work_ms'], 0)
                self.assertGreaterEqual(row['memory']['sampled_peak']['total'], row['memory']['ready']['total'])
            group = next(row for row in report['groups'] if row['scenario'] == 'ff14' and row['mode'] == 'shared' and row['clients'] == 4)
            tags = {'scenario', 'mode', 'clients'}
            events = [{k: v for k, v in group.items() if k != 'scenario'}]
            for name in ('samples', 'commits', 'clients'):
                events.extend({k: v for k, v in row.items() if k not in tags} for row in report[name]
                    if (row['scenario'], row['mode'], row['clients']) == ('ff14', 'shared', 4))
            validate = BENCHMARK['validate']
            validate(events, 1, 4, 'shared', True)
            variants = []
            missing = copy.deepcopy(events); missing.pop(1); variants.append(missing)
            duplicate = copy.deepcopy(events); duplicate.append(duplicate[1]); variants.append(duplicate)
            malformed = copy.deepcopy(events); malformed[1]['request_ms'] = float('nan'); variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[1]['key'] = True; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[0]['memory']['held']['total'] += 1; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[0]['memory']['sampled_peak']['total'] = 0; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[0]['clients'] = True; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[-1]['engine'] += 1; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[-1]['verified_commits'] -= 1; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[-1]['started_counter'] = 0; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[-5]['verified'] = False; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed.pop(); variants.append(malformed)
            variants.append([None])
            for malformed in variants:
                with self.assertRaises(ValueError):
                    validate(malformed, 1, 4, 'shared', True)
            for config in [(0, (1,), ('shared',)), (1, (1, 1), ('shared',)), (1, (True,), ('shared',)), (1, (1,), ('fake',))]:
                with self.assertRaises(ValueError):
                    BENCHMARK['run'](READER, HOST, source, *config)


if __name__ == '__main__':
    unittest.main()
