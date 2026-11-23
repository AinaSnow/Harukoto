import importlib.machinery
import importlib.util
import json
import os
import tempfile
import time
import tkinter as tk
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader('harukoto_manager', os.environ.get('HARUKOTO_GUI_MODULE', str(ROOT / 'Windows/dictionary-manager.pyw')))
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)


class ManagerWindowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.window = tk.Tk(); self.window.withdraw()
        self.app = module.DictionaryManager(self.window, self.folder / 'store')
        self.wait()

    def tearDown(self):
        self.window.destroy()

    def wait(self):
        deadline = time.monotonic() + 60
        while self.app.busy and time.monotonic() < deadline:
            self.window.update(); time.sleep(.01)
        self.window.update()
        self.assertFalse(self.app.busy)

    def package(self, version='1.0', word='日本 | にほん | pinyin: riben'):
        path = self.folder / 'manifest.json'
        path.write_text(json.dumps({'format': 1, 'id': 'gui-test', 'name': '窗口测试', 'version': version,
                                    'source': '人工测试', 'license': 'MIT', 'words': 'words.txt'}), encoding='utf-8')
        (self.folder / 'words.txt').write_text('# 测试\n' + word + '\n', encoding='utf-8')
        return path

    def test_preview_import_enable_disable_update_rollback(self):
        self.assertTrue(self.app.buttons['import'].instate(['disabled']))
        self.app.preview_file(self.package()); self.wait()
        self.assertFalse((self.folder / 'store').exists())
        self.assertIn('来源：人工测试', self.app.details.get('1.0', 'end'))
        self.window.deiconify()
        for size in ('720x540', '860x640'):
            self.window.geometry(size); self.window.update()
            for button in self.app.buttons.values():
                self.assertLessEqual(button.winfo_rootx() + button.winfo_width(), self.window.winfo_rootx() + self.window.winfo_width())
        self.window.withdraw()
        self.app.buttons['import'].invoke(); self.wait()
        self.assertFalse(self.app.state['packages']['gui-test']['enabled'])
        self.app.buttons['enable'].invoke(); self.wait()
        self.assertTrue(self.app.state['packages']['gui-test']['enabled'])
        self.app.preview_file(self.package('2.0', '東京 | とうきょう | pinyin: dongjing')); self.wait()
        self.app.buttons['import'].invoke(); self.wait()
        self.assertEqual(self.app.state['packages']['gui-test']['current']['manifest']['version'], '2.0')
        self.app.buttons['rollback'].invoke(); self.wait()
        self.assertEqual(self.app.state['packages']['gui-test']['current']['manifest']['version'], '1.0')
        self.app.buttons['disable'].invoke(); self.wait()
        self.assertFalse(self.app.state['packages']['gui-test']['enabled'])

    def test_changed_preview_rejected_and_error_recovers(self):
        self.app.preview_file(self.package()); self.wait()
        self.package('2.0')
        self.app.buttons['import'].invoke(); self.wait()
        self.assertIn('重新预览', self.app.status.get())
        self.assertFalse((self.folder / 'store').exists())
        self.assertTrue(self.app.buttons['preview'].instate(['!disabled']))
        self.app.preview_file(self.folder / 'missing.json'); self.wait()
        self.assertTrue(self.app.buttons['import'].instate(['disabled']))

    def test_bundled_ff14_button_imports_disabled(self):
        self.app.buttons['ff14'].invoke(); self.wait()
        self.assertEqual(self.app.preview['manifest']['id'], 'harukoto-ff14')
        self.assertEqual(len(self.app.preview['entries']), 176400)
        self.assertFalse((self.folder / 'store').exists())
        self.app.buttons['import'].invoke(); self.wait()
        self.assertFalse(self.app.state['packages']['harukoto-ff14']['enabled'])
        self.app.buttons['enable'].invoke(); self.wait()
        self.assertTrue(self.app.state['packages']['harukoto-ff14']['enabled'])


if __name__ == '__main__':
    unittest.main()
