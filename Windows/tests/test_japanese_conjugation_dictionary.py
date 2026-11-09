import copy
import json
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
COMPILER = ROOT / "scripts/compile-japanese-conjugations.py"
compile_dictionary = runpy.run_path(str(COMPILER))["compile_dictionary"]


class ConjugationDictionaryTests(unittest.TestCase):
    def test_bundled_forms_and_stable_output(self):
        path = ROOT / "Resources/Lexicon/japanese-conjugation.json"
        result = compile_dictionary(path)
        self.assertEqual(result, compile_dictionary(path))
        self.assertEqual(len(result["entries"]), 10)
        entries = {e["id"]: e for e in result["entries"]}
        self.assertIn({"name": "ない形", "text": "来ない", "reading": "こない"}, entries["kuru"]["forms"])
        self.assertIn({"name": "た形", "text": "可愛かった", "reading": "かわいかった"}, entries["kawaii"]["forms"])

    def test_diagnostics(self):
        valid = {"schema": 1, "entries": [{"id": "eat", "text": "食べる", "reading": "たべる", "pos": "ichidan", "triggers": {"english": ["eat"]}}]}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "input.json"
            variants = []
            duplicate = copy.deepcopy(valid)
            duplicate["entries"] *= 2
            variants.append(duplicate)
            for key, value in [("pos", "typo"), ("triggers", {"romaji": ["taberu"]}), ("extra", 1), ("reading", 123), ("id", "../escape")]:
                variant = copy.deepcopy(valid)
                variant["entries"][0][key] = value
                variants.append(variant)
            for variant in variants:
                path.write_text(json.dumps(variant), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "词条"):
                    compile_dictionary(path)

    def test_failed_compile_preserves_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "bad.json", Path(folder) / "forms.json"
            command = [sys.executable, str(COMPILER), str(ROOT / "Resources/Lexicon/japanese-conjugation.json"), "--output", str(output)]
            subprocess.run(command, check=True, capture_output=True)
            previous = output.read_bytes()
            self.assertEqual(json.loads(previous)["schema"], 1)
            source.write_text('{"schema": 2, "entries": []}', encoding="utf-8")
            command[2] = str(source)
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(previous, output.read_bytes())


if __name__ == "__main__":
    unittest.main()
