"""CN-02 真实延迟与内存对比测试（WIN-03 第 8 步）：
微型词库＋完整 FF14，真实独立子进程（TextService 瘦客户端 vs 进程内路径），
覆盖启动、并发按键＋待机应用总内存、深页翻页一致性、大个人表与学习重载，并验证坏报告被拒。
默认走 resident（真实 Explorer 启动所有者，需普通权限 Medium IL）；设 HARUKOTO_CN02_MODE=direct 可在提权环境做直连宿主冒烟验证。
"""
from contextlib import closing
import copy
import os
from pathlib import Path
import runpy
import sqlite3
import sys
import tempfile
import unittest

from test_base_dictionary_native import fixture, ROOT

BENCHMARK = Path(sys.argv.pop(1))
OWNER = Path(sys.argv.pop(1))
HOST = Path(sys.argv.pop(1))
CN02 = runpy.run_path(str(ROOT / 'scripts/benchmark-cn02.py'))


def source_fixture(source):
    fixture(source, '甲')
    rows = sorted([
        'diannao\tdian4nao3\t电脑\t電腦\t9500\t9500',
        'gongzuo\tgong1zuo4\t工作\t工作\t9500\t9500',
        'ji\tji1\t机\t機\t9500\t9500',
        'keyi\tke3yi3\t可以\t可以\t9500\t9500',
        'li\tli4\t力\t力\t9500\t9500',
        'nihao\tni3hao3\t你好\t你好\t9500\t9500',
        'pengyou\tpeng2you3\t朋友\t朋友\t9500\t9500',
        'shi\tshi4\t是\t是\t9500\t9500',
        'shi\tshi2\t十\t十\t9000\t9000',
        'shi\tshi4\t事\t事\t8500\t8500',
        'shi\tshi1\t师\t師\t8000\t8000',
        'shijian\tshi2jian1\t时间\t時間\t9500\t9500',
        'shijie\tshi4jie4\t世界\t世界\t9500\t9500',
        'tianqi\ttian1qi4\t天气\t天氣\t9500\t9500',
        'wenti\twen4ti2\t问题\t問題\t9500\t9500',
        'wozaiditieshang\two3zai4di4tie3shang4\t我在地铁上\t我在地鐵上\t9500\t9500',
        'xianzai\txian4zai4\t现在\t現在\t9500\t9500',
        'xuexi\txue2xi2\t学习\t學習\t9500\t9500',
        'yi\tyi1\t一\t一\t9500\t9500',
        'zhi\tzhi1\t之\t之\t9500\t9500',
        'zhongguo\tzhong1guo2\t中国\t中國\t9500\t9500',
    ], key=lambda r: r.split('\t')[0])
    (source / 'native-pinyin.tsv').write_text('MEOWPUUT-PINYIN-2\n' + '\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
    with closing(sqlite3.connect(source / 'language-evidence.sqlite')) as db:
        db.execute('DELETE FROM lexicon')
        for reading, word, toned in [
            ('nihao', '你好', 'ni3hao3'),
            ('zhongguo', '中国', 'zhong1guo2'),
            ('shi', '是', 'shi4'),
        ]:
            db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', (reading, word, word, 9500, toned, 9500))
        db.commit()


class Cn02BenchmarkTests(unittest.TestCase):
    def test_real_run_and_validation(self):
        mode = os.environ.get('HARUKOTO_CN02_MODE', 'resident')
        engine_exe = OWNER if mode == 'resident' else HOST
        with tempfile.TemporaryDirectory(prefix='harukoto-cn02-test-') as temporary:
            source = Path(temporary) / '基础 词表'
            source_fixture(source)
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            report = CN02['run'](
                BENCHMARK, engine_exe, source,
                mode=mode, counts=(1, 2), standby=2, cold=1, warm=2,
                corpus_limit=3, themes=True, large_entries=200,
            )
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})

        summary = report['summary']
        self.assertIn('inproc', summary['startup'])
        self.assertIn('engine-warm', summary['startup'])
        if mode == 'resident':
            self.assertIn('engine-cold', summary['startup'])
            self.assertEqual(summary['startup']['engine-cold']['n'], 1)
        # Thin-client startup must stay far below full local lexicon + FF14.
        self.assertLess(summary['startup']['engine-warm']['client_init_mib'],
                        summary['startup']['inproc']['client_init_mib'] * 0.5)
        self.assertEqual({(r['mode'], r['clients']) for r in summary['keys']},
                         {('inproc', 1), ('inproc', 2), ('engine', 1), ('engine', 2)})
        self.assertTrue(summary['deep_page']['engine']['parity_verified'])
        self.assertEqual(summary['large_personal']['engine']['fallbacks'], 0)
        self.assertEqual(summary['large_personal']['engine']['memory_behind'], 0)

        events = report['events']
        rows = CN02['corpus_rows'](True, 3)
        keys_expected = sum(len(t) for _, t in rows)
        validate = CN02['validate']
        validate(events, mode, (1, 2), 2, 1, 2, keys_expected, len(rows), has_large=True)

        # Ensure broken / tampered reports are always rejected.
        for damage in ('drop_group', 'fallback', 'memory_sum', 'deep_mismatch', 'order'):
            broken = copy.deepcopy(events)
            if damage == 'drop_group':
                broken.remove(next(e for e in broken if e['type'] == 'keys'))
            elif damage == 'fallback':
                next(e for e in broken if e['type'] == 'keys' and e['mode'] == 'engine')['fallbacks'] = 1
            elif damage == 'memory_sum':
                next(e for e in broken if e['type'] == 'keys' and e['mode'] == 'engine')['memory']['held']['total'] += 1
            elif damage == 'deep_mismatch':
                next(e for e in broken if e['type'] == 'deep_page' and e['result']['mode'] == 'engine')['result']['inputs'][0]['list_hash'] ^= 1
            elif damage == 'order':
                broken[0], broken[1] = broken[1], broken[0]
            with self.assertRaises(ValueError, msg=damage):
                validate(broken, mode, (1, 2), 2, 1, 2, keys_expected, len(rows), has_large=True)


if __name__ == '__main__':
    unittest.main()
