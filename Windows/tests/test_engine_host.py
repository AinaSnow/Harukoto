"""在临时快照上运行真实引擎进程；不读取个人安装或发布运行时指针。"""
from contextlib import closing
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile

from test_base_dictionary_native import fixture, ROOT
import base_dictionary as bundle
from japanese_packages import change


def run(host, reader, source=None):
    with tempfile.TemporaryDirectory(prefix='harukoto-engine-host-') as temporary:
        root = Path(temporary)
        data = root / 'source'
        if source:
            data.mkdir()
            for name in bundle.FILES:
                shutil.copy2(source / name, data / name)
        else:
            fixture(data, '甲')
            (data / 'native-pinyin.tsv').write_text(
                'MEOWPUUT-PINYIN-2\nnihao\tni3hao3\t你好\t你好\t9500\t9500\n'
                'tejihuifuyao\tte4ji2hui1fu4yao4\t特級恢復藥\t特级恢复药\t9000\t9000\n'
                'zhongguo\tzhong1guo2\t中國\t中国\t9500\t9500\n', encoding='utf-8', newline='\n')
        # Controlled homophones guarantee multiple pages even for the tiny base.
        native = data / 'native-pinyin.tsv'
        header, *rows = native.read_text(encoding='utf-8').splitlines()
        trailer = []
        if '# context' in rows:
            index = rows.index('# context')
            rows, trailer = rows[:index], rows[index:]
        rows += [f'ceshi\tce4shi4\t測試{i}\t测试{i}\t5000\t5000' for i in range(45)]
        # Preserve context footer and stable ties; native rows require descending
        # traditional frequency inside each reading bucket.
        rows.sort(key=lambda row: (row.split('\t')[0], -int(row.split('\t')[-2])))
        native.write_text(header + '\n' + '\n'.join(rows + trailer) + '\n', encoding='utf-8', newline='\n')
        with closing(sqlite3.connect(data / 'language-evidence.sqlite')) as db:
            columns = {row[1] for row in db.execute('PRAGMA table_info(lexicon)')}
            values = dict(reading='ceshibuci', hant='舊補詞', hans='旧补词', weight=9999,
                          toned='ce4shi4bu3ci2', hans_weight=9999)
            if 'bare' in columns:
                values['bare'] = 'ceshibuci'
            if 'phonetic' in columns:
                values['phonetic'] = 'test'
            db.execute(f"INSERT INTO lexicon({','.join(values)}) VALUES({','.join('?' for _ in values)})", tuple(values.values()))
            db.commit()
        snapshot = root / '词库 快照'
        bundle.create(data, snapshot, '1.0.0-host.test')
        change(root / 'themes', 'import', ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
        change(root / 'themes', 'enable', 'harukoto-ff14')
        personal = root / '个人 词表.tsv'
        personal.write_text('testcoffee|珈琲|5000|こーひー|english|咖啡\n', encoding='utf-8')
        subprocess.run([str(reader), str(host), str(snapshot), str(root / 'themes'), str(personal)], check=True, timeout=150)
        bundle.verify(snapshot)


if __name__ == '__main__':
    run(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]) if len(sys.argv) == 4 else None)
