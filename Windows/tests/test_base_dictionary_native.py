"""Python 发布协议与 Windows 原生加载器交叉测试；不安装输入法。"""
from contextlib import closing
import json
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import base_dictionary as bundle

CHECKER, READER = map(Path, sys.argv[1:3])
del sys.argv[1:3]


def string(value):
    data = value.encode('utf-8')
    return struct.pack('<I', len(data)) + data


def fixture(path, suffix):
    path.mkdir()
    (path / 'native-pinyin.tsv').write_text(
        f'MEOWPUUT-PINYIN-2\nnihao\tni3hao3\t測試{suffix}\t测试{suffix}\t9000\t9000\n', encoding='utf-8', newline='\n')
    (path / 'japanese.tsv').write_text('ai|愛|8500\nriben|日本|9500|にほん|pinyin|日本\n', encoding='utf-8')
    binary = b'HARUKOTO-CONJ-1\n' + struct.pack('<I', 1)
    binary += string('eat') + string('食べる') + string('たべる') + struct.pack('<I', 1)
    binary += string('english') + string('eat') + struct.pack('<I', 1)
    binary += string('辞书形') + string('食べる') + string('たべる')
    (path / 'japanese-conjugations.bin').write_bytes(binary)
    with closing(sqlite3.connect(path / 'language-evidence.sqlite')) as db:
        db.execute('CREATE TABLE lexicon(reading TEXT, hant TEXT, hans TEXT, weight INTEGER, toned TEXT, hans_weight INTEGER NOT NULL)')
        db.execute('INSERT INTO lexicon VALUES(?,?,?,?,?,?)', ('nihao', f'測試{suffix}', f'测试{suffix}', 9000, 'ni3hao3', 9000))
        db.commit()


class NativeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='harukoto-native-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = self.root / 'source'
        fixture(source, '甲')
        self.snapshot = self.root / '词库 快照'
        self.key = bundle.create(source, self.snapshot, '1.2.3-rc.test-1+build.2')

    def run_checker(self, path=None, valid=True, mode='--snapshot'):
        result = subprocess.run([str(CHECKER), mode, str(path or self.snapshot)],
                                capture_output=True, encoding='utf-8', timeout=30)
        if valid:
            self.assertEqual(result.returncode, 0, result.stderr)
            value = json.loads(result.stdout)
            self.assertTrue(value['native_loaded'])
            self.assertFalse(value['runtime_activated'])
            return value
        self.assertNotEqual(result.returncode, 0)
        return result.stderr

    def update_hash(self, name):
        path = self.snapshot / name
        manifest_path = self.snapshot / bundle.MANIFEST
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        manifest['files'][name] = dict(size=path.stat().st_size, sha256=bundle.digest(path))
        bundle.atomic_json(manifest_path, manifest)

    def test_canonical_identity_and_cli(self):
        self.assertEqual(self.run_checker()['generation'], self.key)
        manifest = bundle.verify(self.snapshot)
        # Reorder keys and use escaped ASCII; identity must be semantic.
        text = json.dumps(dict(reversed(list(manifest.items()))), ensure_ascii=True, separators=(',', ':'))
        (self.snapshot / bundle.MANIFEST).write_text(text.replace('format', r'\u0066ormat'), encoding='utf-8')
        self.assertEqual(self.run_checker()['generation'], self.key)
        result = subprocess.run([sys.executable, '-X', 'utf8', str(ROOT / 'Windows/manage-base-dictionary.py'),
                                 'verify', str(self.snapshot), '--native-loader', str(CHECKER)],
                                capture_output=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['result']['native']['generation'], self.key)

    def test_native_content_errors_with_valid_hashes(self):
        changes = {
            'native-pinyin.tsv': b'MEOWPUUT-PINYIN-2\ninvalid row\n',
            'japanese.tsv': b'ai|ok|8500\nbad|row\n',
            'japanese-conjugations.bin': b'HARUKOTO-CONJ-1\ninvalid',
            'language-evidence.sqlite': b'SQLite format 3\x00invalid',
        }
        for name, bad in changes.items():
            with self.subTest(name=name):
                path = self.snapshot / name
                original = path.read_bytes()
                try:
                    path.write_bytes(bad); self.update_hash(name)
                    bundle.verify(self.snapshot)  # Container-only checks pass.
                    self.run_checker(valid=False)
                finally:
                    path.write_bytes(original); self.update_hash(name)
        self.run_checker()

    def test_aliases_unicode_and_trailing_fields_are_not_skipped(self):
        path = self.snapshot / 'japanese.tsv'
        for bad in (b'ai,INVALID|ok|8500\n', b'ai,|ok|8500\n', b'ai|ok|8500|\n', b'ai|\xff|8500\n',
                    'ai|愛|8500|wrong|pinyin|\n'.encode(), b'# only comments\n'):
            with self.subTest(bad=bad):
                path.write_bytes(bad); self.update_hash(path.name)
                self.run_checker(valid=False)

    def test_schema_and_sidecars(self):
        database = self.snapshot / 'language-evidence.sqlite'
        with closing(sqlite3.connect(database)) as db:
            db.execute('DROP TABLE lexicon'); db.commit()
        self.update_hash(database.name)
        self.assertIn('schema', self.run_checker(valid=False))
        (self.snapshot / 'language-evidence.sqlite-wal').touch()
        self.assertIn('sidecar', self.run_checker(valid=False))

    def test_wal_database_is_not_a_standalone_snapshot(self):
        database = self.snapshot / 'language-evidence.sqlite'
        with closing(sqlite3.connect(database)) as db:
            self.assertEqual(db.execute('PRAGMA journal_mode=WAL').fetchone()[0], 'wal')
        self.update_hash(database.name)
        self.assertIn('not WAL', self.run_checker(valid=False))

    def test_manifest_rejections(self):
        path = self.snapshot / bundle.MANIFEST
        original = path.read_bytes()
        data = json.loads(original)
        invalid = [dict(data, format=True), dict(data, engine_abi=2), dict(data, version='01.0.0'),
                   dict(data, version='1.0.0-' + 'a' * 129), dict(data, files={'../bad': {}})]
        for value in invalid:
            bundle.atomic_json(path, value)
            self.run_checker(valid=False)
        for value in ('{"format":1,"format":1}', '{"a":' * 10 + 'null' + '}' * 10,
                      '{"a":18446744073709551616}', '{"a":01}', '{"a":"\\u0000"}', '{} trailing'):
            path.write_text(value, encoding='utf-8')
            self.run_checker(valid=False)
        path.write_bytes(original)
        path.write_text(original.decode().replace('"format": 1', '"format": 1, "format": 1'), encoding='utf-8')
        with self.assertRaises(ValueError):
            bundle.verify(self.snapshot)
        self.run_checker(valid=False)

    def test_state_identity_and_rollback(self):
        store = self.root / 'store'
        bundle.stage(self.snapshot, store)
        self.assertEqual(self.run_checker(store, mode='--store')['generation'], self.key)
        source = self.root / 'source-b'; fixture(source, '乙')
        second = self.root / 'second'; kb = bundle.create(source, second, '1.2.4')
        bundle.stage(second, store)
        self.assertEqual(self.run_checker(store, mode='--store')['generation'], kb)
        bundle.rollback(store)
        self.assertEqual(self.run_checker(store, mode='--store')['generation'], self.key)
        manifest = bundle.verify(store / self.key)
        manifest['version'] = '9.0.0'
        bundle.atomic_json(store / self.key / bundle.MANIFEST, manifest)
        self.assertIn('generation mismatch', self.run_checker(store, valid=False, mode='--store'))
        bundle.atomic_json(store / 'STATE.json', dict(format=1, current='../escape', previous=None))
        self.run_checker(store, valid=False, mode='--store')

    def test_snapshot_guards_and_release(self):
        process = subprocess.Popen([str(READER), '--hold', str(self.snapshot)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        try:
            self.assertEqual(process.stdout.readline().strip(), 'ready')
            for name in (*bundle.FILES, bundle.MANIFEST):
                path = self.snapshot / name
                with self.assertRaises(PermissionError):
                    path.open('r+b')
                with self.assertRaises(PermissionError):
                    path.rename(path.with_suffix('.moved'))
            self.run_checker()  # Multiple read-only consumers coexist.
            _, error = process.communicate('\n', timeout=10)
            self.assertEqual(process.returncode, 0, error)
        finally:
            if process.poll() is None:
                process.kill(); process.communicate(timeout=10)
            for pipe in (process.stdin, process.stdout, process.stderr):
                pipe.close()
        for name in (*bundle.FILES, bundle.MANIFEST):
            with (self.snapshot / name).open('r+b'):
                pass

    def test_busy_missing_and_same_size_corruption(self):
        path = self.snapshot / 'japanese.tsv'
        with path.open('r+b'):
            self.assertIn('busy', self.run_checker(valid=False))
        original = path.read_bytes()
        path.write_bytes(b'x' * len(original))
        self.assertIn('hash mismatch', self.run_checker(valid=False))
        path.unlink()
        self.assertIn('missing', self.run_checker(valid=False))
        path.write_bytes(original)
        self.run_checker()

    def test_independent_generations_and_rejected_replacement(self):
        source = self.root / 'source-b'; fixture(source, '乙')
        second = self.root / 'second'; bundle.create(source, second, '1.2.4')
        bad = self.root / 'bad'; shutil.copytree(second, bad)
        (bad / 'japanese.tsv').write_text('corrupted', encoding='utf-8')
        subprocess.run([str(READER), '--pair', str(self.snapshot), str(second), str(bad)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
