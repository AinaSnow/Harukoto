import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from japanese_conjugation import conjugate


class ConjugationTests(unittest.TestCase):
    def forms(self, text, reading, pos, **kwargs):
        return {f.name: (f.text, f.reading) for f in conjugate(text, reading, pos, **kwargs)}

    def test_ichidan(self):
        forms = self.forms("食べる", "たべる", "ichidan")
        self.assertEqual(forms["て形"], ("食べて", "たべて"))
        self.assertEqual(forms["可能形"], ("食べられる", "たべられる"))
        self.assertEqual(forms["使役形"], ("食べさせる", "たべさせる"))
        self.assertEqual(forms["意向形"], ("食べよう", "たべよう"))

    def test_godan_all_endings(self):
        rows = [("買う", "かう", "買って", "かった"), ("書く", "かく", "書いて", "かいた"),
                ("泳ぐ", "およぐ", "泳いで", "およいだ"), ("話す", "はなす", "話して", "はなした"),
                ("待つ", "まつ", "待って", "まった"), ("死ぬ", "しぬ", "死んで", "しんだ"),
                ("遊ぶ", "あそぶ", "遊んで", "あそんだ"), ("読む", "よむ", "読んで", "よんだ"),
                ("帰る", "かえる", "帰って", "かえった")]
        for text, reading, te, ta_reading in rows:
            with self.subTest(text=text):
                forms = self.forms(text, reading, "godan")
                self.assertEqual(forms["て形"][0], te)
                self.assertEqual(forms["た形"][1], ta_reading)
        self.assertEqual(self.forms("買う", "かう", "godan")["ない形"], ("買わない", "かわない"))

    def test_suru_and_kuru(self):
        self.assertEqual(self.forms("勉強する", "べんきょうする", "suru")["可能形"], ("勉強できる", "べんきょうできる"))
        forms = self.forms("来る", "くる", "kuru")
        self.assertEqual(forms["ます形"], ("来ます", "きます"))
        self.assertEqual(forms["ない形"], ("来ない", "こない"))
        self.assertEqual(forms["ば形"], ("来れば", "くれば"))

    def test_explicit_exceptions(self):
        self.assertEqual(self.forms("行く", "いく", "godan", irregular="iku")["て形"], ("行って", "いって"))
        self.assertEqual(self.forms("有る", "ある", "godan", irregular="aru")["ない形"], ("ない", "ない"))
        self.assertEqual(self.forms("いい", "いい", "i-adjective", irregular="ii")["た形"], ("よかった", "よかった"))

    def test_adjectives(self):
        self.assertEqual(self.forms("高い", "たかい", "i-adjective")["ない形"], ("高くない", "たかくない"))
        self.assertEqual(self.forms("静か", "しずか", "na-adjective")["连体形"], ("静かな", "しずかな"))
        self.assertEqual(self.forms("きれい", "きれい", "na-adjective")["丁宁形"], ("きれいです", "きれいです"))

    def test_invalid_input(self):
        for args, kwargs in [(("食べる", "taberu", "ichidan"), {}), (("書く", "かく", "ichidan"), {}),
                             (("行く", "いく", "godan"), {}), (("ある", "ある", "godan"), {}),
                             (("いい", "いい", "i-adjective"), {}), (("食べる", "たべる", "ichidan"), {"irregular": "iku"})]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                conjugate(*args, **kwargs)

    def test_adjective_exception_boundaries(self):
        self.assertEqual(self.forms("可愛い", "かわいい", "i-adjective")["た形"], ("可愛かった", "かわいかった"))
        self.assertEqual(self.forms("良い", "いい", "i-adjective", irregular="ii")["た形"], ("良かった", "よかった"))
        self.assertEqual(self.forms("かっこいい", "かっこいい", "i-adjective", irregular="ii")["ない形"], ("かっこよくない", "かっこよくない"))

    def test_aru_and_mismatched_endings(self):
        forms = self.forms("ある", "ある", "godan", irregular="aru")
        self.assertNotIn("可能形", forms)
        self.assertNotIn("被动形", forms)
        self.assertNotIn("使役形", forms)
        for args, kwargs in [(("書く", "およぐ", "godan"), {}),
                             (("書く", "かく", "godan"), {"irregular": "iku"})]:
            with self.assertRaises(ValueError):
                conjugate(*args, **kwargs)


if __name__ == "__main__":
    unittest.main()
