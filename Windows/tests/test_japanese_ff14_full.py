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
        self.assertEqual(report['total_records'], 60491)
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

    def test_reading_overrides_and_provenance(self):
        from ff14_readings import load_overrides
        report = json.loads((BUNDLE / 'coverage.json').read_text(encoding='utf-8'))
        audit = json.loads((BUNDLE / 'reviewed-source.json').read_text(encoding='utf-8'))['entries']
        self.assertEqual(json.loads((BUNDLE / 'manifest.json').read_text(encoding='utf-8'))['version'], report['version'])
        self.assertEqual(sum(report['pinyin_review'].values()), report['total_records'])
        allowed = {'auto', 'evidence', 'reviewed', 'completion-search-key', 'curated-0.2.0', 'curated-abbreviation'}
        self.assertTrue(all(e['pinyin_review'] in allowed for e in audit))
        overrides = load_overrides()
        self.assertEqual(report['reading_overrides'], len(overrides))
        by_text = {}
        for e in audit: by_text.setdefault(e['zh_cn'], []).append(e)
        for (eid, text), row in overrides.items():
            targets = [e for e in by_text[text] if eid in ('*', e['id']) and e['pinyin_review'] == row['status']]
            self.assertTrue(targets, text)
            for e in targets: self.assertIn(row['reading'], e['triggers']['pinyin'])
        # 长 defaults to zhang in pypinyin; base-lexicon word evidence (长靴/长椅/褪色/调配) fixes it.
        package = load_package(BUNDLE / 'manifest.json')
        keys = {(r[0], r[5]) for r in package['entries']}
        for good, bad, text in (('yuzhouchangyi', 'yuzhouzhangyi', '宇宙长椅'), ('tuisejingcu', 'tuishaijingcu', '褪色晶簇'),
                                ('juemiaodetiaopeiyongpin', 'juemiaodediaopeiyongpin', '绝妙的调配用品')):
            self.assertTrue((good, text) in keys, good)
            self.assertFalse((bad, text) in keys, bad)
        # Cross-boundary evidence is rejected: 骑士长 keeps zhang.
        self.assertTrue(('zongqishizhangdexiaoxianghua', '总骑士长的肖像画') in keys)
        # Item gaps are classified, never translated.
        gaps = (ROOT / 'DataSources/FF14/item-gaps.tsv').read_text(encoding='utf-8').splitlines()
        rows = [line.split('\t') for line in gaps if not line.startswith(('#', 'id\t'))]
        self.assertEqual(len(rows), report['sources']['Item']['excluded'])
        self.assertTrue(all(r[-1] == '未确认日文译名，不收录' for r in rows))

    def test_actions_and_duties_aligned_by_id(self):
        # FF14-04: Action (player PvE skills) and ContentFinderCondition (duties),
        # same pinned commits as Item; category is the ID prefix.
        report = json.loads((BUNDLE / 'coverage.json').read_text(encoding='utf-8'))
        checks = json.loads((BUNDLE / 'source-checks.json').read_text(encoding='utf-8'))
        audit = {e['id']: e for e in json.loads((BUNDLE / 'reviewed-source.json').read_text(encoding='utf-8'))['entries']}
        self.assertEqual(report['version'], '0.4.0')
        self.assertEqual(report['scope'], ['Item', 'PlaceName', 'Completion', 'Action', 'ContentFinderCondition'])
        action, duty = report['sources']['Action'], report['sources']['ContentFinderCondition']
        self.assertEqual((action['included'], action['excluded']), (1905, 18))
        self.assertEqual((duty['included'], duty['excluded']), (863, 0))
        self.assertEqual(sum(action['out_of_scope'].values()), 43567)
        self.assertNotIn('out_of_scope', duty)
        for stats in report['sources'].values():
            self.assertEqual(stats['revision'], {'cn': checks['cn_revision'], 'ja_en': checks['ja_en_revision'], 'ja_en_version': '7.56'})
        files = {s['file'] for s in checks['sources']}
        for sheet in report['scope']:
            for lang in ('cn', 'ja', 'en'): self.assertIn(f'{lang}-{sheet}.csv', files)
        self.assertTrue(all(len(s['sha256']) == 64 for s in checks['sources']))
        for eid, zh, ja, en in (('action-9', '先锋剑', 'ファストブレード', 'Fast Blade'), ('action-7535', '雪仇', 'リプライザル', 'Reprisal'),
                                ('action-7561', '即刻咏唱', '迅速魔', 'Swiftcast'),
                                ('contentfindercondition-1', '监狱废墟托托·拉克千狱', '監獄廃墟 トトラクの千獄', 'the Thousand Maws of Toto–Rak')):
            self.assertEqual((audit[eid]['zh_cn'], audit[eid]['ja'], audit[eid]['english']), (zh, ja, en))
        self.assertIn('xianfengjian', audit['action-9']['triggers']['pinyin'])
        self.assertIn('fastblade', audit['action-9']['triggers']['english'])
        # Out-of-scope rows (enemy/NPC, PvP) are counted, never exported or listed.
        self.assertNotIn('action-7', audit)  # 攻击：auto-attack, not a player skill
        self.assertFalse(any(e['id'].startswith('action-') for e in report['exclusions'] if e['reason'] not in
                             ('该 ID 缺少日文名称，不能猜译', '占位名或无有效中英文触发词', '触发均与常用词同读音，不收录')))
        # Triggers equal to common base-lexicon readings (weight >= 3000) are
        # dropped for skills/duties so everyday input keeps its candidates.
        lines = (ROOT / 'DataSources/FF14/common-reading-collisions.tsv').read_text(encoding='utf-8').splitlines()
        collisions = {line.split('	')[0]: int(line.split('	')[2]) for line in lines[2:]}
        self.assertEqual(report['common_reading_collisions']['readings'], len(collisions))
        self.assertTrue(all(w >= 3000 for w in collisions.values()))
        for key in ('bushi', 'zhuyao', 'baohu', 'shifu'): self.assertIn(key, collisions)
        new = [e for e in audit.values() if e['id'].startswith(('action-', 'contentfindercondition-'))]
        self.assertFalse(any(k in collisions for e in new for v in e['triggers'].values() for k in v))
        self.assertEqual(sum('dropped_triggers' in e for e in new), report['common_reading_collisions']['records_with_dropped_triggers'])
        self.assertEqual(audit['action-24283']['triggers']['english'], ['dosis'])  # 注药: zhuyao (主要) dropped
        self.assertEqual(audit['action-24283']['dropped_triggers'], {'pinyin': ['zhuyao']})
        dropped_out = [e for e in report['exclusions'] if e['reason'] == '触发均与常用词同读音，不收录']
        self.assertEqual(len(dropped_out), 9)
        # New names are covered by the reading audit (都灵 du, 重置 chong).
        self.assertIn('juemiaokuileishiduling', [k for e in audit.values() if e['zh_cn'] == '绝妙傀儡师——都灵' for k in e['triggers']['pinyin']])

    def test_player_abbreviations_file(self):
        path = ROOT / 'DataSources/FF14/player-abbreviations.tsv'
        lines = [line for line in path.read_text(encoding='utf-8').splitlines() if not line.startswith('#')]
        columns = lines[0].split('\t')
        self.assertEqual(columns, ['abbr', 'pinyin', 'ja', 'kana', 'target', 'note', 'status'])
        rows = [dict(zip(columns, line.split('\t'))) for line in lines[1:]]
        self.assertTrue(all(len(line.split('\t')) == len(columns) for line in lines[1:]))
        curated = [r for r in rows if r['status'] == 'curated']
        self.assertTrue(50 <= len(curated) <= 150)
        self.assertTrue(all(r['status'] in ('curated', 'excluded') and r['note'] for r in rows))
        audit = {e['id']: e for e in json.loads((BUNDLE / 'reviewed-source.json').read_text(encoding='utf-8'))['entries']}
        report = json.loads((BUNDLE / 'coverage.json').read_text(encoding='utf-8'))
        self.assertEqual(report['abbreviations']['curated'], len(curated))
        self.assertEqual(sum(e['id'].startswith('abbr-') for e in audit.values()), len(curated))
        import re
        for r in curated:
            self.assertRegex(r['pinyin'], '^[a-z]{4,64}$')
            self.assertRegex(r['abbr'], '[一-鿿]')  # pure-Latin abbreviations are excluded
            e = audit['abbr-' + r['pinyin']]
            self.assertEqual((e['zh_cn'], e['ja'], e['kana'] or '-', e['triggers']['pinyin']), (r['abbr'], r['ja'], r['kana'], [r['pinyin']]))
            self.assertEqual(e['pinyin_review'], 'curated-abbreviation')
        exported = {e['zh_cn'] for e in audit.values() if e['id'].startswith('abbr-')}
        for r in rows:
            if r['status'] == 'excluded': self.assertNotIn(r['abbr'], exported)
        for word in ('绝欧', '绝亚', '零式', '极神', '日随', '主坦'): self.assertIn(word, exported)
        for word in ('MT', 'ST', 'DC', '奶', '奶妈', '野队', '大区'): self.assertNotIn(word, exported)

    def test_update_from_031_and_rollback(self):
        # 0.3.1 is exactly 0.4.0 without the new categories (existing records unchanged).
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            old = root / 'old'
            old.mkdir()
            manifest = json.loads((BUNDLE / 'manifest.json').read_text(encoding='utf-8'))
            manifest['version'] = '0.3.1'
            (old / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
            from japanese_dictionary import trigger
            audit = json.loads((BUNDLE / 'reviewed-source.json').read_text(encoding='utf-8'))['entries']
            kept = ['# ' + manifest['name']]
            for e in audit:
                if e['id'].startswith(('action-', 'contentfindercondition-', 'abbr-')): continue
                parts = [e['ja'], e['kana'] or '-', 'chinese: ' + e['zh_cn']]
                for kind, keys in e['triggers'].items(): parts.extend(kind + ': ' + trigger(k) for k in keys)
                kept.append(' | '.join(parts))
            (old / 'ff14-words.txt').write_text('\n'.join(kept) + '\n', encoding='utf-8')
            themes = root / 'themes'
            change(themes, 'import', old / 'manifest.json')
            change(themes, 'enable', 'harukoto-ff14')
            self.assertEqual(len(read_state(themes)['packages']['harukoto-ff14']['current']['entries']), 113463)
            state = change(themes, 'import', BUNDLE / 'manifest.json')
            item = state['packages']['harukoto-ff14']
            self.assertTrue(item['enabled'])
            self.assertEqual((item['current']['manifest']['version'], item['previous']['manifest']['version']), ('0.4.0', '0.3.1'))
            self.assertEqual(len(item['current']['entries']), 117507)
            state = change(themes, 'rollback', 'harukoto-ff14')
            item = state['packages']['harukoto-ff14']
            self.assertEqual((item['current']['manifest']['version'], len(item['current']['entries'])), ('0.3.1', 113463))
            self.assertTrue(item['enabled'])

    def test_large_update_enable_and_rollback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            change(root, 'import', ROOT / 'Resources/DictionaryPackages/ff14/jobs/manifest.json')
            change(root, 'enable', 'harukoto-ff14')
            state = change(root, 'import', BUNDLE / 'manifest.json')
            item = state['packages']['harukoto-ff14']
            self.assertTrue(item['enabled'])
            self.assertEqual(len(item['current']['entries']), 117507)
            self.assertEqual(len(item['previous']['entries']), 431)
            self.assertEqual(read_state(root), state)
            if os.environ.get('HARUKOTO_FULL_READER'):
                command = [os.environ['HARUKOTO_FULL_READER'], os.environ['HARUKOTO_FULL_LEXICON'], str(root), '117507', '--bulk']
                result = subprocess.run(command, capture_output=True, timeout=180)
                self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
            state = change(root, 'rollback', 'harukoto-ff14')
            self.assertEqual(len(state['packages']['harukoto-ff14']['current']['entries']), 431)
            self.assertTrue(state['packages']['harukoto-ff14']['enabled'])
            state = change(root, 'disable', 'harukoto-ff14')
            self.assertFalse(state['packages']['harukoto-ff14']['enabled'])


if __name__ == '__main__': unittest.main()
