import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from japanese_dictionary import parse_word_list, trigger


class WordListTests(unittest.TestCase):
    def test_explicit_spelling_only_and_long_game_names(self):
        words = self.parse('# 游戏\nリムサ・ロミンサ | - | pinyin: limusaluominsa\n')
        self.assertEqual(words[0][3], '')
        long_name = 'オーケストリオン譜:' + 'Song, Name ' * 5
        words = self.parse('# 游戏\n' + long_name + ' | - | english: ' + 'longname' * 10 + '\n')
        self.assertEqual(words[0][1], long_name.strip())
        with self.assertRaises(ValueError): self.parse('# 游戏\n来る | - | pos: vk | pinyin: lai\n')
        with self.assertRaises(ValueError): self.parse('# 游戏\n日本 | | pinyin: riben\n')

    def test_fullwidth_colon_preserved_in_names(self):
        words = self.parse('# 测试\n魔土器:自己強化 | まどきじこきょうか | chinese: 魔陶器：强化自身 | pinyin：motaqiqianghuazishen\n')
        self.assertEqual(words[0][1], '魔土器:自己強化')
        self.assertEqual(words[0][5], '魔陶器：强化自身')
        words = self.parse('# 测试\n魔土器:自己強化｜まどきじこきょうか｜chinese：魔陶器:强化自身｜pinyin：motaqiqianghuazishen\n')
        self.assertEqual(words[0][5], '魔陶器:强化自身')

    def parse(self, content):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'words.txt'
            path.write_text(content, encoding='utf-8-sig')
            return parse_word_list(path)

    def test_fields_variants_and_unicode(self):
        words = self.parse('# 测试\n新宿｜しんじゅく｜pinyin：xinsu｜priority: 9000\n'
                           '新宿 | にいじゅく | pinyin: xinsu\n'
                           'ポイントカード | english: point card | english: point-card\n')
        self.assertEqual(len(words), 3)
        self.assertIn(('pointcard', 'ポイントカード', 5000, 'ぽいんとかーど', 'english', ''), words)
        self.assertEqual(trigger('nǚ xìng'), 'nvxing')
        self.assertEqual(trigger('nu\u0308\u030c xing'), 'nvxing')
        self.assertEqual([w[3] for w in words if w[0] == 'xinsu'], ['しんじゅく', 'にいじゅく'])

    def test_reject_invalid_records(self):
        for line in ['日本 | pinyin: riben', '日本 | nihon | pinyin: riben',
                     '日本 | にほん | priority: 0', '日本 | にほん | priority: 5000x',
                     '日本 | にほん | pos: unknown', '食べる | たべる | pos: v5k',
                     '日本 | にほん | pinyin: a', '日本 | にほん | priority: 1 | priority: 2',
                     '日\t本 | にほん | pinyin: riben']:
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.parse('# 测试\n' + line)

    def test_failed_import_preserves_previous_file(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / 'input.txt', Path(folder) / 'compiled.tsv'
            source.write_text('# 测试\nコーヒー | english: coffee\n', encoding='utf-8')
            command = [sys.executable, str(ROOT / 'Windows/import-japanese-dictionary.py'), str(source), '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            previous = output.read_bytes()
            source.write_text('# 测试\n日本 | pinyin: riben\n', encoding='utf-8')
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), previous)

    def test_bundled_dictionary(self):
        words = parse_word_list(ROOT / 'Resources/Lexicon/japanese-bridge.txt')
        self.assertTrue(any(w[0] == 'tushuguan' and w[1] == '図書館' for w in words))
        self.assertTrue(any(w[0] == 'coffee' and w[1] == 'コーヒー' for w in words))

    def test_explicit_clear_and_argument_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'personal.tsv'
            output.write_bytes(b'previous')
            command = [sys.executable, str(ROOT / 'Windows/import-japanese-dictionary.py'), '--output', str(output)]
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertNotEqual(subprocess.run(command + ['--clear', 'input.txt'], capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), b'previous')
            subprocess.run(command + ['--clear', '--check'], check=True, capture_output=True)
            self.assertEqual(output.read_bytes(), b'previous')
            subprocess.run(command + ['--clear'], check=True, capture_output=True)
            self.assertEqual(output.read_bytes(), b'')


if __name__ == '__main__':
    unittest.main()
