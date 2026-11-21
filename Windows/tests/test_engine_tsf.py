"""WIN-03 第 6a 步：文本服务的独立引擎开关、异步与回退，以及进程内／引擎路径逐项对照。

用法：test_engine_tsf.py <harukoto_engine_tsf_test> <harukoto_engine_host> <词库目录>
词库目录做成临时基础快照，加 FF14 全量主题与一张个人表；结束时删除临时目录。
"""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import base_dictionary as bundle
from japanese_packages import change

reader, host, source = (Path(value).resolve() for value in sys.argv[1:4])
with tempfile.TemporaryDirectory(prefix='harukoto-engine-tsf-') as temporary:
    root = Path(temporary)
    base = root / 'base'
    bundle.create(source, base, '1.0.0-tsf')
    themes = root / 'themes'
    change(themes, 'import', ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
    change(themes, 'enable', 'harukoto-ff14')
    personal, other = root / 'personal.tsv', root / 'other.tsv'
    personal.write_text('testcoffee|珈琲|5000|こーひー|english|咖啡\n', encoding='utf-8')
    other.write_text('testcoffee|珈琲|5100|こーひー|english|咖啡\n', encoding='utf-8')
    subprocess.run([str(reader), 'unit', str(base), str(themes), str(personal), str(other), str(host)],
                   check=True, timeout=300)
    result = subprocess.run([str(reader), 'parity', str(base), str(themes), str(personal),
                             str(ROOT / 'Windows/tests/data/chinese-evaluation.tsv'),
                             str(ROOT / 'Windows/tests/data/chinese-words-dev.tsv')],
                            check=True, timeout=900, capture_output=True, text=True, encoding='utf-8')
    summary = json.loads(result.stdout.strip().splitlines()[-1])
    print(json.dumps(summary, ensure_ascii=False))
    assert summary['mismatches'] == 0 and summary['served'] > summary['prefixes'] // 2, summary
