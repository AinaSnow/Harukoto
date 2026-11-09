import copy
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from ff14_dictionary import validate_catalog, word_list
from japanese_packages import load_package
spec = importlib.util.spec_from_file_location('ff14_export', ROOT / 'scripts/compile-ff14-dictionary.py')
compiler = importlib.util.module_from_spec(spec); spec.loader.exec_module(compiler)


class FF14Tests(unittest.TestCase):
    def catalog(self):
        data = json.loads((ROOT / 'Resources/DictionaryPackages/ff14/catalog.json').read_text(encoding='utf-8'))
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


if __name__ == '__main__': unittest.main()
