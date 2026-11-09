import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from ff14_dictionary import validate_catalog, word_list
from ff14_completion import read_completion, align_terms
from japanese_packages import load_package, change
spec = importlib.util.spec_from_file_location('ff14_export', ROOT / 'scripts/compile-ff14-dictionary.py')
compiler = importlib.util.module_from_spec(spec); spec.loader.exec_module(compiler)
if '--reader' in sys.argv:
    index = sys.argv.index('--reader')
    os.environ['HARUKOTO_FF14_READER'] = sys.argv[index + 1]
    os.environ['HARUKOTO_FF14_LEXICON'] = sys.argv[index + 2]
    del sys.argv[index:index + 3]


class FF14Tests(unittest.TestCase):
    def test_completion_headers_and_identity_checks(self):
        with tempfile.TemporaryDirectory() as folder:
            cn = Path(folder) / 'cn.csv'; ja = Path(folder) / 'ja.csv'; en = Path(folder) / 'en.csv'
            cn.write_text('key,0,1,2,3,4\n#,Group,Key,LookupTable,Text,GroupTitle\nint32,uint16,uint16,str,str,str\n1604,22,20,,魔晶石,mojingshi\n', encoding='utf-8')
            ja.write_text('#,Text,GroupTitle,LookupTable,Group,Key\n1604,マテリア,-,,22,20\n', encoding='utf-8')
            en.write_text('#,Text,GroupTitle,LookupTable,Group,Key\n1604,materia,-,,22,20\n', encoding='utf-8')
            tables = [read_completion(path) for path in (cn, ja, en)]
            self.assertEqual(align_terms(*tables, [1604])[0]['kana'], 'まてりあ')
            tables[1][1604]['Key'] = 21
            with self.assertRaises(ValueError): align_terms(*tables, [1604])
            tables[1][1604]['Key'] = 20; tables[1][1604]['LookupTable'] = 'Item'
            with self.assertRaises(ValueError): align_terms(*tables, [1604])
            tables[1][1604]['LookupTable'] = ''; tables[1][1604]['Text'] = '未核对汉字'
            with self.assertRaises(ValueError): align_terms(*tables, [1604])

    def test_bundled_jobs_match_reviewed_catalog(self):
        directory = ROOT / 'Resources/DictionaryPackages/ff14'
        data = json.loads((directory / 'catalog.json').read_text(encoding='utf-8'))
        words, ids = word_list(data)
        self.assertEqual(len(ids), 33)
        self.assertEqual((directory / 'jobs/ff14-words.txt').read_text(encoding='utf-8'), words)
        package = load_package(directory / 'jobs/manifest.json')
        self.assertEqual(package['manifest'], data['package'])
        self.assertEqual(len(package['entries']), 89)
        mappings = {e['zh_cn']: e['ja'] for e in data['entries']}
        self.assertEqual(mappings['魔晶石'], 'マテリア')
        self.assertEqual(mappings['魔素石'], '魔晶石')
        self.assertEqual(json.loads((directory / 'jobs/reviewed-source.json').read_text(encoding='utf-8')), data)
        self.assertIn(('vpr', 'ヴァイパー', 'ゔぁいぱー'), [(row[0], row[1], row[3]) for row in package['entries']])

    @unittest.skipUnless(os.environ.get('HARUKOTO_FF14_READER'), '由 CTest 提供真实词库读取器')
    def test_production_candidates_with_enabled_and_disabled_package(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def check(count):
                result = subprocess.run([os.environ['HARUKOTO_FF14_READER'], os.environ['HARUKOTO_FF14_LEXICON'], str(root), str(count)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
            change(root, 'import', ROOT / 'Resources/DictionaryPackages/ff14/jobs/manifest.json')
            check(0)
            change(root, 'enable', 'harukoto-ff14'); check(89)
            change(root, 'disable', 'harukoto-ff14'); check(0)
    def catalog(self):
        data = json.loads((ROOT / 'Resources/DictionaryPackages/ff14/catalog.json').read_text(encoding='utf-8'))
        data['package']['words'] = 'words.txt'
        # Synthetic generic nouns only: this fixture is not verified game terminology.
        data['entries'] = [{'id': 'test-library', 'category': 'place', 'zh_cn': '图书馆', 'ja': '図書館', 'kana': 'としょかん',
            'english': ['library'], 'aliases': ['测试别名'], 'triggers': {'pinyin': ['tushuguan'], 'english': ['library']},
            'game_version': '7.0', 'sources': [{'role': role, 'url': 'https://example.com/test', 'accessed': '2026-11-09', 'note': '仅供校验测试'} for role in ('zh_cn', 'ja', 'kana')],
            'review': {'status': 'verified', 'reviewer': 'test', 'date': '2026-11-09', 'rights_note': '测试数据'}}]
        return data

    def test_only_reviewed_terms_export_and_aliases_not_inferred(self):
        data = self.catalog()
        draft = copy.deepcopy(data['entries'][0]); draft['id'] = 'draft'; draft['review']['status'] = 'draft'
        draft['triggers']['english'] = ['unverified']
        data['entries'].append(draft)
        words, ids = word_list(data)
        self.assertEqual(ids, ['test-library'])
        self.assertNotIn('unverified', words)
        self.assertNotIn('测试别名', words)
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'catalog.json'; output = Path(folder) / 'output'
            source.write_text(json.dumps(data), encoding='utf-8')
            self.assertEqual(compiler.export(source, output), 1)
            self.assertEqual(len(load_package(output / 'manifest.json')['entries']), 2)
            self.assertEqual(len(json.loads((output / 'reviewed-source.json').read_text(encoding='utf-8'))['entries']), 1)
            previous = (output / 'words.txt').read_bytes()
            with self.assertRaises(ValueError): compiler.export(source, output)
            self.assertEqual((output / 'words.txt').read_bytes(), previous)

    def test_empty_and_draft_cannot_publish(self):
        data = self.catalog(); data['entries'][0]['review']['status'] = 'draft'
        with self.assertRaises(ValueError): word_list(data)
        data['entries'] = []
        validate_catalog(data)
        with self.assertRaises(ValueError): word_list(data)

    def test_missing_sources_reading_review_and_duplicate_ids(self):
        for field, value in [('kana', 'toshokan'), ('game_version', 'latest'), ('category', 'unknown')]:
            data = self.catalog(); data['entries'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): validate_catalog(data)
        data = self.catalog(); data['entries'][0]['sources'].pop()
        with self.assertRaises(ValueError): validate_catalog(data)
        data = self.catalog(); data['entries'][0]['review']['rights_note'] = ''
        with self.assertRaises(ValueError): validate_catalog(data)
        data = self.catalog(); data['entries'].append(copy.deepcopy(data['entries'][0]))
        with self.assertRaises(ValueError): validate_catalog(data)


if __name__ == '__main__':
    unittest.main()
