"""WIN-03 第 6a／6b 步：文本服务的独立引擎开关、异步与回退，进程内／引擎路径逐项对照，
以及学习记录（选词记忆、个人词频、个人词）经引擎只读加载后的一致性。

用法：test_engine_tsf.py <harukoto_engine_tsf_test> <harukoto_engine_host> <词库目录> [pwsh]
词库目录做成临时基础快照，加 FF14 全量主题与一张个人表；学习记录都放在临时目录，
不碰 %LOCALAPPDATA%；结束时删除临时目录。给出 pwsh 时用真实管理脚本做清除与个人词改动。
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
pwsh = sys.argv[4] if len(sys.argv) > 4 else None
DATA = ROOT / 'Windows/tests/data'
SCRIPT = ROOT / 'Windows/manage-selection-memory.ps1'


def steps(base, themes, personal, root):
    """Drives one native process: services A (in-process) and B (engine) on two copies of the same learning."""
    a, b = root / 'steps-a', root / 'steps-b'
    a.mkdir(); b.mkdir()
    process = subprocess.Popen([str(reader), 'steps', str(base), str(themes), str(personal), str(a), str(b)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
    try:
        assert process.stdout.readline().strip() == 'ready'

        def send(command):
            process.stdin.write(command + '\n'); process.stdin.flush()
            line = process.stdout.readline()
            assert line, f'native steps stopped at: {command}'
            return json.loads(line)

        def check(reading):
            result = send(f'check {reading}')
            # The final prefix is listed by the engine, identical to in-process (asserted natively).
            assert result['path'] == 'engine' and result['reason'] == 'none', result
            return result['texts'].split('|')

        def script(*arguments):
            for folder in (a, b):  # The same edit on both copies, as every client would see it.
                subprocess.run([pwsh, '-NoProfile', '-File', str(SCRIPT), *arguments, '-Path', str(folder / 'selection-memory.tsv')],
                               check=True, capture_output=True, timeout=120)

        reading = 'zhongguo'
        initial = check(reading)
        assert len(initial) > 3
        # Learning just made applies to the very next composition, through the engine.
        send(f'learn {reading} 2')
        learned = check(reading)
        assert learned[0] == initial[2], (learned, initial)
        # Ctrl+Delete forgets it again (phrase and words).
        assert send(f'forget {reading} 0')['extra'] == 'forgot'
        assert check(reading) == initial
        # The switch: off ignores what was learned; on applies it again.
        send(f'learn {reading} 2')
        assert check(reading)[0] == initial[2]
        send('memory off')
        assert check(reading) == initial
        send('memory on')
        assert check(reading)[0] == initial[2]
        checked = ['learn', 'forget', 'switch']
        if pwsh:
            script('-Clear')
            assert check(reading) == initial
            script('-AddWord', reading, '-Text', '中锅')
            assert '中锅' in check(reading)
            script('-RemoveWord', reading, '-Text', '中锅')
            assert '中锅' not in check(reading)
            script('-AddWord', reading, '-Text', '中锅')
            assert '中锅' in check(reading)
            script('-ClearWords')
            assert check(reading) == initial
            checked += ['Clear', 'AddWord', 'RemoveWord', 'ClearWords']
        process.stdin.write('exit\n'); process.stdin.flush()
        assert process.wait(timeout=60) == 0
        print('steps:', ', '.join(checked))
    finally:
        if process.poll() is None:
            process.kill(); process.wait()


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
    steps(base, themes, personal, root)
    scratch = root / 'parity'
    scratch.mkdir()
    result = subprocess.run([str(reader), 'parity', str(base), str(themes), str(personal), str(scratch),
                             *(str(DATA / name) for name in ('chinese-evaluation.tsv', 'chinese-words-dev.tsv',
                                                             'chinese-fuzzy-dev.tsv', 'chinese-typo-dev.tsv',
                                                             'chinese-abbrev-dev.tsv'))],
                            check=True, timeout=1800, capture_output=True, text=True, encoding='utf-8')
    summary = json.loads(result.stdout.strip().splitlines()[-1])
    print(json.dumps(summary, ensure_ascii=False))
    for name, run in summary.items():
        assert run['mismatches'] == 0 and run['served'] > run['prefixes'] // 2, (name, run)
        # Only lists the engine cannot represent yet stay in-process; nothing fails, nothing lags.
        assert set(run['fallbacks']) <= {'long_list', 'partial', 'unrepresentable'}, (name, run)
