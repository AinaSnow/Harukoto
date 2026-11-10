"""存储协议测试；小型夹具只验证容器，不冒充原生词库内容验证。"""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts'))
import base_dictionary as bundle


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        for name, data in zip(bundle.FILES, (
                b'MEOWPUUT-PINYIN-2\nfixture', b'SQLite format 3\x00fixture',
                '测试\n'.encode(), b'HARUKOTO-CONJ-1\nfixture')):
            (self.source / name).write_bytes(data)
        self.a, self.b = self.root / 'a', self.root / 'b'
        self.store = self.root / 'store'
        self.ka = bundle.create(self.source, self.a, '1.0.0')
        self.kb = bundle.create(self.source, self.b, '1.0.1')

    def test_stage_duplicate_and_rollback(self):
        bundle.stage(self.a, self.store)
        bundle.stage(self.b, self.store)
        expected = dict(format=1, current=self.kb, previous=self.ka)
        bundle.stage(self.b, self.store)
        self.assertEqual(bundle.state(self.store), expected)
        self.assertEqual(bundle.rollback(self.store), self.ka)
        self.assertEqual(bundle.rollback(self.store), self.kb)
        self.assertEqual(bundle.state(self.store), expected)
        self.assertEqual(bundle.verify(self.b), bundle.verify(self.store / self.kb))

    def test_corrupt_or_missing_source_preserves_state(self):
        bundle.stage(self.a, self.store)
        old = (self.store / 'STATE.json').read_bytes()
        path = self.b / 'japanese.tsv'
        path.write_bytes(b'x' * path.stat().st_size)
        with self.assertRaises(ValueError):
            bundle.stage(self.b, self.store)
        path.unlink()
        with self.assertRaises(ValueError):
            bundle.stage(self.b, self.store)
        self.assertEqual((self.store / 'STATE.json').read_bytes(), old)

    def test_invalid_manifest_and_container(self):
        original = bundle.verify(self.b)
        for field, value in [('engine_abi', 2), ('format', True), ('version', '../bad'),
                             ('files', {'../japanese.tsv': {}})]:
            with self.subTest(field=field):
                changed = dict(original, **{field: value})
                bundle.atomic_json(self.b / bundle.MANIFEST, changed)
                with self.assertRaises(ValueError):
                    bundle.verify(self.b)
        bundle.atomic_json(self.b / bundle.MANIFEST, original)
        path = self.b / 'native-pinyin.tsv'
        path.write_bytes(b'x' * path.stat().st_size)
        original['files'][path.name]['sha256'] = bundle.digest(path)
        bundle.atomic_json(self.b / bundle.MANIFEST, original)
        with self.assertRaisesRegex(ValueError, '格式'):
            bundle.verify(self.b)

    def test_failed_pointer_replace_is_retryable(self):
        bundle.stage(self.a, self.store)
        old = (self.store / 'STATE.json').read_bytes()
        with mock.patch.object(bundle.os, 'replace', side_effect=PermissionError('locked')):
            with self.assertRaises(PermissionError):
                bundle.stage(self.b, self.store)
        self.assertEqual((self.store / 'STATE.json').read_bytes(), old)
        self.assertFalse((self.store / '.write.lock').exists())
        self.assertFalse(list(self.store.glob('*.tmp')))
        self.assertEqual(bundle.stage(self.b, self.store), self.kb)

    def test_copy_mutation_never_publishes(self):
        copy = shutil.copyfile

        def corrupt(source, destination):
            result = copy(source, destination)
            if Path(destination).name == 'japanese.tsv':
                Path(destination).write_bytes(b'changed')
            return result

        with mock.patch.object(bundle.shutil, 'copyfile', side_effect=corrupt):
            with self.assertRaises(ValueError):
                bundle.stage(self.b, self.store)
            with self.assertRaises(ValueError):
                bundle.create(self.source, self.root / 'bad', '1.0.2')
        self.assertFalse((self.store / 'STATE.json').exists())
        self.assertFalse((self.store / self.kb).exists())
        self.assertFalse((self.root / 'bad').exists())
        self.assertFalse(list(self.store.iterdir()))

    def test_lock_and_malformed_state_are_preserved(self):
        self.store.mkdir()
        lock = self.store / '.write.lock'
        lock.touch()
        with self.assertRaises(FileExistsError):
            bundle.stage(self.a, self.store)
        self.assertTrue(lock.exists())
        lock.unlink()
        state = self.store / 'STATE.json'
        state.write_text('{broken', encoding='utf-8')
        with self.assertRaises(ValueError):
            bundle.stage(self.a, self.store)
        self.assertEqual(state.read_text(), '{broken')
        self.assertFalse(lock.exists())

    def test_rollback_requires_two_valid_generations(self):
        bundle.stage(self.a, self.store)
        with self.assertRaises(ValueError):
            bundle.rollback(self.store)
        bundle.stage(self.b, self.store)
        old = bundle.state(self.store)
        (self.store / self.ka / 'japanese.tsv').unlink()
        with self.assertRaises(ValueError):
            bundle.rollback(self.store)
        self.assertEqual(bundle.state(self.store), old)

    def test_create_never_overwrites(self):
        original = bundle.verify(self.a)
        with self.assertRaises(ValueError):
            bundle.create(self.source, self.a, '1.0.2')
        with self.assertRaises(ValueError):
            bundle.create(self.source, self.root / 'invalid', 'bad')
        self.assertFalse((self.root / 'invalid').exists())
        self.assertEqual(bundle.verify(self.a), original)

    def test_semantic_versions(self):
        for version in ('1.0.0', '0.4.4-alpha.19.snapshot-test', '1.0.0-rc.1+build.2'):
            self.assertIsNotNone(bundle.VERSION.fullmatch(version))
        for version in ('01.0.0', '1.0', '1.0.0-', '1.0.0-a..b', '1.0.0-01', '../1.0.0'):
            self.assertIsNone(bundle.VERSION.fullmatch(version))

    def test_cli_reports_staging_only_and_nonzero_failure(self):
        command = [sys.executable, '-X', 'utf8', str(REPO / 'Windows/manage-base-dictionary.py')]
        result = subprocess.run(command + ['stage', str(self.a), '--store', str(self.store)],
                                capture_output=True, encoding='utf-8', check=True)
        self.assertEqual(json.loads(result.stdout), dict(result=self.ka, runtime_activated=False))
        result = subprocess.run(command + ['verify', str(self.root / 'missing')],
                                capture_output=True, encoding='utf-8')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('error:', result.stderr)


if __name__ == '__main__':
    unittest.main()
