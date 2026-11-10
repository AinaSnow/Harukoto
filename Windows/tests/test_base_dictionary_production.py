"""用真实四文件快照运行现有原生加载与候选回归；不修改用户安装。"""
import json
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import base_dictionary as bundle


def main():
    source, reader, probe = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='harukoto-base-production-') as temporary:
        root = Path(temporary)
        first, second, store = root / 'first', root / 'second', root / 'store'
        ka = bundle.create(source, first, '0.4.4-alpha.19')
        kb = bundle.create(source, second, '0.4.4-alpha.19.snapshot-test')
        bundle.stage(first, store)
        bundle.stage(second, store)
        assert bundle.state(store) == dict(format=1, current=kb, previous=ka)
        assert bundle.rollback(store) == ka
        snapshot = store / ka
        bundle.check_generation(store, ka)
        with closing(sqlite3.connect((snapshot / 'language-evidence.sqlite').as_uri() + '?mode=ro', uri=True)) as db:
            assert db.execute('PRAGMA integrity_check').fetchall() == [('ok',)]
        subprocess.run([str(reader), str(snapshot)], check=True, timeout=60)
        for script in ('hans', 'hant'):
            result = subprocess.run([str(probe), str(snapshot), script],
                                    input='nihao\nzhidaole\nchi\nriben\n',
                                    capture_output=True, encoding='utf-8', check=True, timeout=60)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            assert rows[0]['theme_rows'] == 0
            assert rows[1]['candidates'][0]['text'] == '你好'
            assert rows[2]['candidates'][0]['text'] == '知道了'
            assert any(c['text'] == '吃' for c in rows[3]['candidates'])
            assert any(c['text'] == '日本' for c in rows[4]['candidates'])
        # A rejected update leaves the previously verified pointer intact.
        (second / 'japanese-conjugations.bin').write_bytes(b'broken')
        try:
            bundle.stage(second, store)
        except ValueError:
            pass
        else:
            raise AssertionError('Corrupted update accepted')
        assert bundle.state(store)['current'] == ka
        print('PASS: full snapshots, SQLite integrity, native loaders, hans/hant candidates, rollback and rejected update')


if __name__ == '__main__':
    main()
