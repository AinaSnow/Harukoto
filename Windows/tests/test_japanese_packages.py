import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from japanese_packages import change, compiled, conflicts, load_package, read_state, writer_lock


class PackagesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.store = self.folder / 'store'

    def package(self, key='example', version='1.0', text='日本 | にほん | pinyin: riben'):
        folder = self.folder / (key + '-' + version)
        folder.mkdir(exist_ok=True)
        manifest = {'format': 1, 'id': key, 'name': '测试主题', 'version': version,
                    'source': '人工编写的测试数据', 'license': 'MIT', 'words': 'words.txt'}
        (folder / 'words.txt').write_text('# 测试\n' + text + '\n', encoding='utf-8')
        path = folder / 'manifest.json'
        path.write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
        return path

    def pointer(self):
        return (self.store / 'CURRENT').read_bytes()

    def test_preview_does_not_write(self):
        command = [sys.executable, str(ROOT / 'Windows/manage-japanese-packages.py'), '--root', str(self.store), 'preview', str(self.package())]
        result = subprocess.run(command, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.store.exists())

    def test_import_enable_update_disable_and_rollback(self):
        first = self.package()
        state = change(self.store, 'import', first)
        self.assertEqual(compiled(state), b'')
        self.assertFalse(state['packages']['example']['enabled'])
        state = change(self.store, 'enable', 'example')
        self.assertIn('日本', compiled(state).decode())
        old_generation = self.pointer().decode()
        old_payload = (self.store / old_generation / 'enabled.tsv').read_bytes()
        state = change(self.store, 'import', self.package(version='2.0', text='東京 | とうきょう | pinyin: dongjing'))
        self.assertTrue(state['packages']['example']['enabled'])
        self.assertIn('東京', compiled(state).decode())
        self.assertNotIn('日本', compiled(state).decode())
        self.assertEqual((self.store / old_generation / 'enabled.tsv').read_bytes(), old_payload)
        state = change(self.store, 'rollback', 'example')
        self.assertEqual(compiled(state), old_payload)
        self.assertEqual(state['packages']['example']['current']['manifest']['version'], '1.0')
        self.assertEqual(compiled(change(self.store, 'disable', 'example')), b'')
        self.assertEqual(read_state(self.store), change(self.store, 'import', first))

    def test_failed_parse_and_same_version_keep_previous(self):
        path = self.package()
        change(self.store, 'import', path)
        original = self.pointer()
        (path.parent / 'words.txt').write_text('# 测试\n日本 | pinyin: riben', encoding='utf-8')
        with self.assertRaises(ValueError):
            change(self.store, 'import', path)
        self.package(text='東京 | とうきょう | pinyin: dongjing')
        with self.assertRaisesRegex(ValueError, '版本'):
            change(self.store, 'import', path)
        self.assertEqual(self.pointer(), original)

    def test_failed_activation_preserves_all_previous_state(self):
        change(self.store, 'import', self.package())
        original, state = self.pointer(), read_state(self.store)
        with patch('japanese_packages.os.replace', side_effect=PermissionError('locked')):
            with self.assertRaises(PermissionError):
                change(self.store, 'enable', 'example')
        self.assertEqual(self.pointer(), original)
        self.assertEqual(read_state(self.store), state)
        self.assertEqual(compiled(state), b'')

    def test_conflicts_coexist_and_packages_are_independent(self):
        change(self.store, 'import', self.package())
        other = self.package('alternative', text='二本 | にほん | pinyin: riben')
        report = conflicts(read_state(self.store), load_package(other))
        self.assertEqual(report['items'][0]['kind'], 'alternative')
        change(self.store, 'import', other)
        change(self.store, 'enable', 'example')
        state = change(self.store, 'enable', 'alternative')
        self.assertEqual(len(compiled(state).splitlines()), 2)
        state = change(self.store, 'disable', 'alternative')
        self.assertIn('日本', compiled(state).decode())
        self.assertNotIn('二本', compiled(state).decode())
        # Legacy custom data is outside the package store and must remain untouched.
        legacy = self.store.parent / 'japanese-custom.tsv'
        legacy.write_bytes(b'legacy')
        change(self.store, 'disable', 'example')
        self.assertEqual(legacy.read_bytes(), b'legacy')

    def test_invalid_manifest_and_pointer(self):
        path = self.package()
        original = json.loads(path.read_text(encoding='utf-8'))
        for field, value in [('id', '../escape'), ('words', '../words.txt'), ('words', 'C:/words.txt'), ('format', True), ('source', '')]:
            data = dict(original, **{field: value})
            path.write_text(json.dumps(data), encoding='utf-8')
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                load_package(path)
        self.store.mkdir()
        (self.store / 'CURRENT').write_text('../escape', encoding='ascii')
        with self.assertRaises(ValueError):
            read_state(self.store)

    def test_writer_contention_and_release(self):
        self.package()
        with writer_lock(self.store):
            with self.assertRaisesRegex(ValueError, '其他词库'):
                change(self.store, 'import', self.package())
        change(self.store, 'import', self.package())
        self.assertIn('example', read_state(self.store)['packages'])

    def test_missing_rollback_does_not_mutate(self):
        change(self.store, 'import', self.package())
        original = self.pointer()
        with self.assertRaises(ValueError):
            change(self.store, 'rollback', 'example')
        self.assertEqual(self.pointer(), original)

    def test_partial_snapshot_and_corrupt_state(self):
        change(self.store, 'import', self.package())
        original = self.pointer()
        with patch('japanese_packages.os.fsync', side_effect=OSError('disk failure')):
            with self.assertRaises(OSError):
                change(self.store, 'enable', 'example')
        self.assertEqual(self.pointer(), original)
        state_file = self.store / original.decode() / 'state.json'
        state = read_state(self.store)
        state['packages']['example']['current']['entries'][0][0] = '../escape'
        state_file.write_text(json.dumps(state), encoding='utf-8')
        with self.assertRaises(ValueError):
            change(self.store, 'enable', 'example')
        self.assertEqual(self.pointer(), original)

    def test_duplicate_keys_and_conflict_report_limit(self):
        path = self.package()
        path.write_text('{"format":1,"format":1}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, '重复'):
            load_package(path)
        change(self.store, 'import', self.package())
        package = load_package(self.package('second'))
        package['entries'] *= 101
        report = conflicts(read_state(self.store), package)
        self.assertTrue(report['limited'])
        self.assertEqual(len(report['items']), 100)
        self.assertEqual(report['items'][0]['kind'], 'duplicate')

    @unittest.skipUnless(os.environ.get('HARUKOTO_PACKAGE_READER'), '由 CTest 提供 C++ 读取器')
    def test_cpp_runtime_tracks_atomic_snapshot(self):
        reader = os.environ['HARUKOTO_PACKAGE_READER']
        def check(count):
            result = subprocess.run([reader, str(self.store), str(count)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        check(0)
        change(self.store, 'import', self.package())
        check(0)
        change(self.store, 'enable', 'example')
        check(1)
        change(self.store, 'import', self.package('second', text='東京 | とうきょう | pinyin: dongjing'))
        change(self.store, 'enable', 'second')
        check(2)
        change(self.store, 'import', self.package(version='2.0', text='学校 | がっこう | pinyin: xuexiao'))
        check(2)
        change(self.store, 'rollback', 'example')
        check(2)
        change(self.store, 'disable', 'second')
        check(1)
        change(self.store, 'disable', 'example')
        check(0)


if __name__ == '__main__':
    if '--reader' in sys.argv:
        index = sys.argv.index('--reader')
        os.environ['HARUKOTO_PACKAGE_READER'] = sys.argv[index + 1]
        del sys.argv[index:index + 2]
    unittest.main()
