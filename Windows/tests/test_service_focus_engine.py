"""WIN-03 第 6d 步：阶段一 TSF 功能测试在独立引擎开关强制开启、连到真实引擎宿主时照常通过。

用法：test_service_focus_engine.py <harukoto_focus_test> <构建目录> <harukoto_engine_host> <词库目录>
词库目录做成临时基础快照供宿主加载；结束时删除临时目录。
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import base_dictionary as bundle

focus, build, host, source = (Path(value).resolve() for value in sys.argv[1:5])
with tempfile.TemporaryDirectory(prefix='harukoto-focus-engine-') as temporary:
    base = Path(temporary) / 'base'
    bundle.create(source, base, '1.0.0-focus')
    result = subprocess.run([str(focus), str(build), str(host), str(base)], timeout=600,
                            capture_output=True, text=True, encoding='utf-8', errors='replace')
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    assert result.returncode == 0, result.returncode
    assert 'engine switch on:' in result.stdout
