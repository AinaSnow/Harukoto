"""WIN-03 第 6a／6b 步：文本服务的独立引擎开关、异步与回退，进程内／引擎路径逐项对照，
以及学习记录（选词记忆、个人词频、个人词）经引擎只读加载后的一致性。

用法：test_engine_tsf.py <harukoto_engine_tsf_test> <harukoto_engine_host> <词库目录> [pwsh [owner_test]]
词库目录做成临时基础快照，加 FF14 全量主题与一张个人表；学习记录都放在临时目录，
不碰 %LOCALAPPDATA%；结束时删除临时目录。给出 pwsh 时用真实管理脚本做清除与个人词改动。
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import base_dictionary as bundle
from japanese_packages import change

reader, host, source = (Path(value).resolve() for value in sys.argv[1:4])
pwsh = sys.argv[4] if len(sys.argv) > 4 else None
owner_test = Path(sys.argv[5]).resolve() if len(sys.argv) > 5 else None
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


def layout(root):
    """第 6c 步：模拟安装布局（版本目录＋%LOCALAPPDATA%\\Meowpuut），核对数据身份、热发布与常驻路径。"""
    version = root / 'installed' / 'version'
    version.mkdir(parents=True)
    for name in bundle.FILES:
        shutil.copy2(source / name, version / name)
    bundle.describe(version, '0.4.4-tsf.1')  # As package-ime.ps1 does for the payload.
    data = root / 'local' / 'Meowpuut'
    data.mkdir(parents=True)
    change(data / 'Dictionaries', 'import', ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
    change(data / 'Dictionaries', 'enable', 'harukoto-ff14')
    (data / 'japanese-custom.tsv').write_text('testcoffee|珈琲|5000|こーひー|english|咖啡\n', encoding='utf-8')
    staged = root / 'staged'
    bundle.create(source, staged, '0.4.4-tsf.2')
    damaged = root / 'damaged'
    bundle.create(source, damaged, '0.4.4-tsf.3')
    command = [str(reader), 'layout', str(version), str(data), str(host)] + ([str(owner_test)] if owner_test else [])
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
    try:
        while True:
            line = process.stdout.readline()
            assert line, 'layout check stopped early'
            print(line.rstrip())
            if line.strip() == 'ready-publish':
                bundle.stage(staged, data / 'BaseDictionaries')  # As Dictionaries.cmd publishes a snapshot.
                process.stdin.write('published\n'); process.stdin.flush()
            elif line.strip() == 'ready-corrupt':
                # A third publication, damaged after staging: no loader may adopt it.
                generation = bundle.stage(damaged, data / 'BaseDictionaries')
                (data / 'BaseDictionaries' / generation / 'japanese.tsv').write_text('ai|恋|1\n', encoding='utf-8')
                process.stdin.write('corrupted\n'); process.stdin.flush()
                break
        for line in process.stdout:
            print(line.rstrip())
        assert process.wait(timeout=600) == 0
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
    only = os.environ.get('HARUKOTO_ENGINE_TSF_ONLY')  # Development: run one phase.
    if only in (None, 'unit'):
        subprocess.run([str(reader), 'unit', str(base), str(themes), str(personal), str(other), str(host)],
                       check=True, timeout=300)
    if only in (None, 'steps'):
        steps(base, themes, personal, root)
    if only in (None, 'layout'):
        layout(root)
    if only not in (None, 'parity'):
        raise SystemExit(0)
    scratch = root / 'parity'
    scratch.mkdir()
    result = subprocess.run([str(reader), 'parity', str(base), str(themes), str(personal), str(scratch),
                             *(str(DATA / name) for name in ('chinese-evaluation.tsv', 'chinese-words-dev.tsv',
                                                             'chinese-fuzzy-dev.tsv', 'chinese-typo-dev.tsv',
                                                             'chinese-abbrev-dev.tsv'))],
                            timeout=3000, capture_output=True, text=True, encoding='utf-8')
    if result.returncode:
        print(result.stderr[-4000:], file=sys.stderr)
        raise SystemExit(f'parity failed: {result.returncode}')
    summary = json.loads(result.stdout.strip().splitlines()[-1])
    print(json.dumps(summary, ensure_ascii=False))
    for name, run in summary.items():
        assert run['mismatches'] == 0 and run['served'] > run['prefixes'] // 2 and run['f6'] > 0 and run['segments'] > 50, (name, run)
        # Only lists the engine cannot represent yet stay in-process; nothing fails, nothing lags.
        assert set(run['fallbacks']) <= {'long_list', 'context', 'unrepresentable'}, (name, run)
