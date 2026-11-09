import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from japanese_packages import load_package, change, read_state

if '--reader' in sys.argv:
    i = sys.argv.index('--reader')
    os.environ['HARUKOTO_FULL_READER'] = sys.argv[i + 1]
    os.environ['HARUKOTO_FULL_LEXICON'] = sys.argv[i + 2]
    del sys.argv[i:i + 3]

BUNDLE = ROOT / 'Resources/DictionaryPackages/ff14/full'


class FullFF14Tests(unittest.TestCase):
    def test_coverage_and_every_exported_row(self):
        report = json.loads((BUNDLE / 'coverage.json').read_text(encoding='utf-8'))
        audit = json.loads((BUNDLE / 'reviewed-source.json').read_text(encoding='utf-8'))
        package = load_package(BUNDLE / 'manifest.json')
        self.assertEqual(report['total_records'], 57672)
        self.assertEqual(len(audit['entries']), report['total_records'])
        self.assertEqual(len(package['entries']), report['trigger_rows'])
        from japanese_dictionary import trigger
        exported = {(r[0], r[1], r[3], r[4], r[5]) for r in package['entries']}
        expected = {(trigger(key), e['ja'], e['kana'], kind, e['zh_cn'])
                    for e in audit['entries'] for kind, keys in e['triggers'].items() for key in keys}
        self.assertEqual(exported, expected)
        for sheet, stats in report['sources'].items():
            self.assertEqual(stats['included'] + stats['excluded'], stats['source_named'])
            self.assertEqual(sum(e['id'].startswith(sheet.lower() + '-') for e in audit['entries']), stats['included'])
        self.assertTrue(any(r[0] == 'limusaluominsa' for r in package['entries']))
        self.assertTrue(any(r[0] == 'huifuyao' and r[1] == 'ポーション' for r in package['entries']))
        self.assertTrue(any(r[0] == 'nihao' for r in package['entries']))
        for key in ('haidu', 'sendu', 'shadu', 'xiexie', 'qingduoguanzhao', 'xinkule', 'chujian'):
            self.assertTrue(any(r[0] == key for r in package['entries']), key)
        self.assertTrue(any(r[0] == 'xiexie' and r[3] == 'ありがとう' for r in package['entries']))
        self.assertTrue(any(not r[3] for r in package['entries']))

    def test_large_update_enable_and_rollback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            change(root, 'import', ROOT / 'Resources/DictionaryPackages/ff14/jobs/manifest.json')
            change(root, 'enable', 'harukoto-ff14')
            state = change(root, 'import', BUNDLE / 'manifest.json')
            item = state['packages']['harukoto-ff14']
            self.assertTrue(item['enabled'])
            self.assertEqual(len(item['current']['entries']), 113463)
            self.assertEqual(len(item['previous']['entries']), 431)
            self.assertEqual(read_state(root), state)
            if os.environ.get('HARUKOTO_FULL_READER'):
                command = [os.environ['HARUKOTO_FULL_READER'], os.environ['HARUKOTO_FULL_LEXICON'], str(root), '113463', '--bulk']
                result = subprocess.run(command, capture_output=True, timeout=180)
                self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
            state = change(root, 'rollback', 'harukoto-ff14')
            self.assertEqual(len(state['packages']['harukoto-ff14']['current']['entries']), 431)
            self.assertTrue(state['packages']['harukoto-ff14']['enabled'])
            state = change(root, 'disable', 'harukoto-ff14')
            self.assertFalse(state['packages']['harukoto-ff14']['enabled'])


if __name__ == '__main__': unittest.main()
