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
TAGS = {'scenario', 'mode', 'clients', 'threads', 'profile', 'workload'}


def source_fixture(source):
    fixture(source, '甲')
    (source/'native-pinyin.tsv').write_text(
        'MEOWPUUT-PINYIN-2\nnihao\tni3hao3\t你好\t你好\t9500\t9500\n'
        'xinkule\txin1ku3le5\t辛苦了\t辛苦了\t9500\t9500\n', encoding='utf-8', newline='\n')
    with closing(sqlite3.connect(source/'language-evidence.sqlite')) as db:
        db.execute('DELETE FROM lexicon')
        for reading, word, toned in [('nihao', '你好', 'ni3hao3'), ('xinkule', '辛苦了', 'xin1ku3le5')]:
            db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', (reading, word, word, 9500, toned, 9500))
        db.commit()


def events_for(report, group):
    events = [{k: v for k, v in group.items() if k != 'scenario'}]
    for name in ('samples', 'commits', 'clients', 'traces', 'trace_status'):
        for row in report[name]:
            if all(row[k] == group[k] for k in TAGS):
                events.append({k: v for k, v in row.items() if k not in TAGS or k == 'threads' and name == 'trace_status'})
    return events


class IpcBenchmarkTests(unittest.TestCase):
    def test_real_matrix_and_validation(self):
        with tempfile.TemporaryDirectory(prefix='harukoto-ipc-profile-test-') as temporary:
            source = Path(temporary)/'基础 词表'
            source_fixture(source)
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            report = BENCHMARK['run'](READER, HOST, source, 1)
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})
            self.assertEqual(len(report['groups']), 18)
            self.assertEqual(len(report['summary']), 18)
            self.assertEqual(len(report['samples']), 1344)
            self.assertEqual(len(report['commits']), 231)
            self.assertEqual(len(report['clients']), 42)
            self.assertEqual(report['format'], 2)
            self.assertEqual(report['traces'], [])
            self.assertEqual(report['trace_status'], [])
            self.assertEqual(set(report['provenance']['base_files']), set(before))
            self.assertEqual(report['provenance']['programs']['host'], BENCHMARK['bundle'].digest(HOST))
            self.assertIn('Windows/EngineControl.h', report['provenance']['sources'])
            self.assertIn('Windows/EngineOwnerAnchor.h', report['provenance']['sources'])
            self.assertNotIn(temporary, json.dumps(report))
            for row in report['summary']:
                expected = (38 if row['scenario'] == 'ff14' else 26)*row['clients']
                self.assertEqual(row['samples'], expected)
                self.assertGreaterEqual(row['common_work_ms'], 0)
                self.assertGreaterEqual(row['memory']['sampled_peak']['total'], row['memory']['ready']['total'])
            group = next(row for row in report['groups'] if row['scenario'] == 'ff14' and row['mode'] == 'shared' and row['clients'] == 4)
            events = events_for(report, group)
            validate = BENCHMARK['validate']
            validate(events, 1, 4, 'shared', True)
            variants = []
            missing = copy.deepcopy(events); missing.pop(1); variants.append(missing)
            duplicate = copy.deepcopy(events); duplicate.append(duplicate[1]); variants.append(duplicate)
            malformed = copy.deepcopy(events); malformed[1]['request_ms'] = float('nan'); variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[1]['key'] = True; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[1]['request'] = True; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[1]['engine'] += 1; variants.append(malformed)
            malformed = copy.deepcopy(events)
            for name in BENCHMARK['IDENTITY']:
                malformed[2][name] = malformed[1][name]
            variants.append(malformed)
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

    def test_service_timings_and_mixed_workload(self):
        with tempfile.TemporaryDirectory(prefix='harukoto-ipc-timing-test-') as temporary:
            source = Path(temporary)/'基础 词表'
            source_fixture(source)
            report = BENCHMARK['run'](READER, HOST, source, 1, (4,), ('local', 'isolated', 'shared'), (2, 4), True, ('uniform', 'mixed'))
            self.assertEqual(len(report['groups']), 20)
            self.assertEqual(len(report['samples']), 2050)
            self.assertEqual(len(report['traces']), 1640)
            self.assertEqual(len(report['trace_status']), 40)
            for group, summary in zip(report['groups'], report['summary']):
                events = events_for(report, group)
                BENCHMARK['validate'](events, 1, 4, group['mode'], group['scenario'] == 'ff14',
                    group['threads'], group['profile'], group['workload'])
                self.assertEqual(summary['samples'], (104 if group['scenario'] == 'base' else 152) if group['workload'] == 'uniform'
                    else (64 if group['scenario'] == 'base' else 90))
                self.assertEqual('service_ms' in summary, group['profile'])
                self.assertEqual(set(summary['cohorts']), {'long', 'short'})
                if group['profile']:
                    service = summary['service_ms']
                    self.assertAlmostEqual(service['total']['sum'], sum(service[n]['sum'] for n in
                        ('queue', 'execution', 'publication', 'mailbox', 'owner')), places=6)
                    self.assertAlmostEqual(summary['request_ms']['sum'], service['total']['sum']+service['remainder']['sum'], places=3)
            group = next(g for g in report['groups'] if g['scenario'] == 'ff14' and g['mode'] == 'shared' and g['threads'] == 4 and g['workload'] == 'mixed')
            events = events_for(report, group)
            trace_index = next(i for i, e in enumerate(events) if e['type'] == 'trace')
            status_index = next(i for i, e in enumerate(events) if e['type'] == 'trace_status')
            variants = []
            for index in (trace_index, status_index):
                missing = copy.deepcopy(events); missing.pop(index); variants.append(missing)
                duplicate = copy.deepcopy(events); duplicate.append(copy.deepcopy(duplicate[index])); variants.append(duplicate)
            for field, value in [('endpoint', 4), ('endpoint', True), ('connection', 0), ('request', 2**64),
                                 ('engine', 42), ('started', 0), ('finished', 1), ('enqueued', 2**64-1)]:
                malformed = copy.deepcopy(events); malformed[trace_index][field] = value; variants.append(malformed)
            for field, value in [('threads', 2), ('dropped', 1), ('records', 0), ('engine', 42)]:
                malformed = copy.deepcopy(events); malformed[status_index][field] = value; variants.append(malformed)
            malformed = copy.deepcopy(events); malformed[0]['workload'] = 'uniform'; variants.append(malformed)
            # Valid endpoint range still cannot route a trace to another client.
            malformed = copy.deepcopy(events); malformed[trace_index]['endpoint'] = (malformed[trace_index]['endpoint']+1)%4; variants.append(malformed)
            for malformed in variants:
                with self.assertRaises(ValueError):
                    BENCHMARK['validate'](malformed, 1, 4, 'shared', True, 4, True, 'mixed')
            with self.assertRaises(ValueError):
                BENCHMARK['validate'](events, 1, 4, 'shared', True, 4, False, 'mixed')
            for config in ({'threads': (0,)}, {'threads': (17,)}, {'threads': (True,)}, {'threads': (2, 2)},
                           {'profile': 1}, {'workloads': ('wrong',)}, {'workloads': ('mixed', 'mixed')}):
                with self.assertRaises(ValueError):
                    BENCHMARK['run'](READER, HOST, source, 1, **config)


if __name__ == '__main__':
    unittest.main()
