"""常驻引擎成本测量（WIN-03 第 5 步）：微型词库＋完整 FF14，真实 Explorer 启动的测试所有者与真实客户端进程；只写 HKCU 测试键，不读写 HKLM。另验证坏报告被拒。"""
from contextlib import closing
import copy
from pathlib import Path
import runpy
import sqlite3
import sys
import tempfile
import unittest

from test_base_dictionary_native import fixture, ROOT

BENCHMARK = Path(sys.argv.pop(1))
OWNER = Path(sys.argv.pop(1))
COST = runpy.run_path(str(ROOT / 'scripts/benchmark-engine-cost.py'))


def source_fixture(source):
    fixture(source, '甲')
    (source / 'native-pinyin.tsv').write_text(
        'MEOWPUUT-PINYIN-2\nnihao\tni3hao3\t你好\t你好\t9500\t9500\n'
        'xinkule\txin1ku3le5\t辛苦了\t辛苦了\t9500\t9500\n', encoding='utf-8', newline='\n')
    with closing(sqlite3.connect(source / 'language-evidence.sqlite')) as db:
        db.execute('DELETE FROM lexicon')
        for reading, word, toned in [('nihao', '你好', 'ni3hao3'), ('xinkule', '辛苦了', 'xin1ku3le5')]:
            db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', (reading, word, word, 9500, toned, 9500))
        db.commit()


class CostBenchmarkTests(unittest.TestCase):
    def test_real_run_and_validation(self):
        with tempfile.TemporaryDirectory(prefix='harukoto-cost-test-') as temporary:
            source = Path(temporary) / '基础 词表'
            source_fixture(source)
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            report = COST['run'](BENCHMARK, OWNER, source, cold=2, handshake=3, runs=1, counts=(1, 2), threads=(2, 4), corpus_limit=3)
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})
        summary = report['summary']
        self.assertEqual(summary['cold']['ensure_ms']['n'], 2)
        self.assertGreater(summary['cold']['memory_mib']['host']['private'], 0)
        self.assertEqual({(r['mode'], r['threads'], r['clients']) for r in summary['keys']},
                         {('inproc', 0, 1), ('inproc', 0, 2), ('engine', 2, 1), ('engine', 2, 2), ('engine', 4, 1), ('engine', 4, 2)})
        events = report['events']
        handshakes = [e for e in events if e['type'] == 'handshake']
        self.assertEqual([e['held'] for e in handshakes], [False, True])
        self.assertEqual([e['count'] for e in events if e['type'] == 'sessions'], [0, 1, 2, 4, 8, 0])
        keys = sum(len(text) for _, text in COST['corpus_rows'](True, 3))
        validate = COST['validate']
        sections = ('cold', 'warm', 'keys')
        validate(events, 2, (1, 2), (2, 4), keys, sections)
        for damage in ('drop_cold', 'drop_group', 'sample_count', 'memory_sum', 'order', 'stats'):
            broken = copy.deepcopy(events)
            if damage == 'drop_cold':
                broken.remove(next(e for e in broken if e['type'] == 'cold'))
            elif damage == 'drop_group':
                broken.remove(next(e for e in broken if e['type'] == 'keys'))
            elif damage == 'sample_count':
                next(e for e in broken if e['type'] == 'keys')['overall']['n'] += 1
            elif damage == 'memory_sum':
                next(e for e in broken if e['type'] == 'keys' and e['mode'] == 'engine')['memory']['held']['total'] += 1
            elif damage == 'order':
                row = next(e for e in broken if e['type'] == 'cold'); row['host_created_ms'] = row['pipe_visible_ms'] + 1
            else:
                next(e for e in broken if e['type'] == 'handshake')['stats']['admit_ms']['median'] = -1
            with self.assertRaises(ValueError, msg=damage):
                validate(broken, 2, (1, 2), (2, 4), keys, sections)


if __name__ == '__main__':
    unittest.main()
