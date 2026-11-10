"""Publish the real FF14 package for native hot reload integration checks."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from japanese_packages import change

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='harukoto-reload-manager-') as folder:
        root = Path(folder)
        change(root, 'import', ROOT / 'Resources/DictionaryPackages/ff14/full/manifest.json')
        change(root, 'enable', 'harukoto-ff14')
        source, personal = root / 'personal.txt', root / 'personal.tsv'
        source.write_text('# 测试\nコーヒー | english: testcoffee\n', encoding='utf-8')
        subprocess.run([sys.executable, str(ROOT / 'Windows/import-japanese-dictionary.py'), str(source), '--output', str(personal)], check=True, capture_output=True)
        subprocess.run([sys.argv[1], str(root), sys.argv[2], str(personal)], check=True, timeout=60)
