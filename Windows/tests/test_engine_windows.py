"""准备真实快照／FF14 主题，运行独立引擎 Windows 候选合并回归。"""
from contextlib import closing
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import base_dictionary as bundle
from japanese_packages import change

reader, source = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='harukoto-engine-windows-') as temporary:
    root = Path(temporary)
    data = root / 'source'
    data.mkdir()
    for name in bundle.FILES:
        shutil.copy2(source / name, data / name)
    # More than two exact pages guarantee late-page selection independently of
    # the production dictionary's homophone count. Preserve context and ties.
    native = data / 'native-pinyin.tsv'
    header, *rows = native.read_text(encoding='utf-8').splitlines()
    trailer = []
    if '# context' in rows:
        index = rows.index('# context')
        rows, trailer = rows[:index], rows[index:]
    rows += [f'fanyeceshi\tfan1ye4ce4shi4\t翻頁測試{i}\t翻页测试{i}\t5000\t5000' for i in range(45)]
    rows.sort(key=lambda row: (row.split('\t')[0], -int(row.split('\t')[-2])))
    native.write_text(header+'\n'+'\n'.join(rows+trailer)+'\n', encoding='utf-8', newline='\n')
    # Source-empty mixed aliases belong to the base romaji table. Personal
    # triggers deliberately require an explicit pinyin/english source.
    with (data / 'japanese.tsv').open('a', encoding='utf-8', newline='\n') as words:
        words.write('\nabc|世界|5000\n'
                    'abc|世間|5000\n'
                    'abcd|光|4500\n'
                    'ab|英語|6000|えいご|english|英语\n')
    first, second = root / 'first', root / 'second'
    with closing(sqlite3.connect(data / 'language-evidence.sqlite')) as db:
        db.execute('INSERT INTO lexicon(reading,phonetic,bare,hant,hans,weight,toned,hans_weight) VALUES(?,?,?,?,?,?,?,?)',
                   ('ceshibuci', 'test', 'ceshibuci', '舊補詞', '旧补词', 9999, 'ce4shi4bu3ci2', 9999))
        db.commit()
    bundle.create(data, first, '1.0.0-engine.a')
    with closing(sqlite3.connect(data / 'language-evidence.sqlite')) as db:
        db.execute("UPDATE lexicon SET hant='新補詞',hans='新补词' WHERE reading='ceshibuci'")
        db.commit()
    bundle.create(data, second, '1.0.0-engine.b')
    change(root / 'themes', 'import', ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
    change(root / 'themes', 'enable', 'harukoto-ff14')
    personal = root / 'personal.tsv'
    personal.write_text('testcoffee|珈琲|5000|こーひー|english|咖啡\n', encoding='utf-8')
    subprocess.run([str(reader), str(first), str(second), str(root / 'themes'), str(personal)],
                   check=True, timeout=150)
