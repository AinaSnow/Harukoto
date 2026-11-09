"""晴词主题词库窗口；所有修改复用经过测试的快照管理器。"""
import os
import queue
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'scripts'))
from japanese_packages import change, conflicts, load_package, read_state


class DictionaryManager(ttk.Frame):
    def __init__(self, master, root):
        super().__init__(master, padding=20)
        self.root = Path(root)
        self.state = {'format': 1, 'packages': {}}
        self.preview = None
        self.preview_path = None
        self.busy = False
        self.results = queue.Queue()
        self.pack(fill='both', expand=True)
        master.title('晴词 · 主题词库')
        master.geometry('860x640')
        master.minsize(720, 540)
        master.protocol('WM_DELETE_WINDOW', self.close)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=2)
        self.rowconfigure(4, weight=3)
        ttk.Label(self, text='主题词库', font=('Microsoft YaHei UI', 20)).grid(row=0, column=0, sticky='w')
        ttk.Label(self, text='先预览再导入。新主题默认关闭；修改后重新打开输入应用。', wraplength=760).grid(row=1, column=0, sticky='w', pady=(8, 16))
        self.table = ttk.Treeview(self, columns=('name', 'version', 'status', 'previous'), show='headings', selectmode='browse', height=7)
        for key, label, width in [('name', '主题', 300), ('version', '版本', 130), ('status', '状态', 100), ('previous', '可回退版本', 140)]:
            self.table.heading(key, text=label); self.table.column(key, width=width, minwidth=80)
        self.table.grid(row=2, column=0, sticky='nsew')
        table_scroll = ttk.Scrollbar(self, orient='vertical', command=self.table.yview)
        table_scroll.grid(row=2, column=1, sticky='ns'); self.table.configure(yscrollcommand=table_scroll.set)
        self.table.bind('<<TreeviewSelect>>', self.selected)
        bar = ttk.Frame(self)
        bar.grid(row=3, column=0, sticky='ew', pady=12)
        self.buttons = {}
        for index, (key, text, callback) in enumerate([('preview', '选择词库并预览', self.choose), ('import', '导入此预览', self.import_preview),
                ('enable', '启用', lambda: self.modify('enable')), ('disable', '关闭', lambda: self.modify('disable')),
                ('rollback', '回退上一版', lambda: self.modify('rollback')), ('refresh', '刷新', self.refresh)]):
            button = ttk.Button(bar, text=text, command=callback)
            button.grid(row=index // 3, column=index % 3, sticky='ew', padx=(0, 8), pady=3)
            bar.columnconfigure(index % 3, weight=1); self.buttons[key] = button
        self.details = tk.Text(self, wrap='word', height=12, padx=12, pady=12, font=('Microsoft YaHei UI', 10), relief='solid', borderwidth=1)
        self.details.grid(row=4, column=0, sticky='nsew')
        details_scroll = ttk.Scrollbar(self, orient='vertical', command=self.details.yview)
        details_scroll.grid(row=4, column=1, sticky='ns'); self.details.configure(yscrollcommand=details_scroll.set)
        self.status = tk.StringVar(value='正在读取本地词库…')
        ttk.Label(self, textvariable=self.status, wraplength=780).grid(row=5, column=0, sticky='ew', pady=(12, 0))
        self.show_details('选择 manifest.json 可查看名称、来源、许可和冲突。\n当前不内置 FF14 词库。')
        self.poll_id = self.after(40, self.poll)
        self.bind('<Destroy>', self.destroyed)
        self.refresh()

    def destroyed(self, event):
        if event.widget is self:
            self.after_cancel(self.poll_id)

    def close(self):
        if self.busy:
            self.status.set('正在完成词库操作，请稍候再关闭。')
        else:
            self.master.destroy()

    def show_details(self, text):
        self.details.configure(state='normal'); self.details.delete('1.0', 'end')
        self.details.insert('1.0', text); self.details.configure(state='disabled')

    def update_buttons(self):
        selected = self.table.selection()
        item = self.state['packages'].get(selected[0]) if selected else None
        for key, button in self.buttons.items():
            enabled = not self.busy
            if key == 'import': enabled = enabled and self.preview is not None
            if key == 'enable': enabled = enabled and item is not None and not item['enabled']
            if key == 'disable': enabled = enabled and item is not None and item['enabled']
            if key == 'rollback': enabled = enabled and item is not None and item['previous'] is not None
            button.configure(state='normal' if enabled else 'disabled')

    def run(self, work, done):
        if self.busy: return
        self.busy = True; self.update_buttons(); self.status.set('正在处理…')
        def worker():
            try: self.results.put((done, work(), None))
            except Exception as error: self.results.put((done, None, str(error)))
        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        try:
            done, result, error = self.results.get_nowait()
            self.busy = False
            if error: self.status.set('操作未完成：' + error)
            else: done(result)
            self.update_buttons()
        except queue.Empty: pass
        self.poll_id = self.after(40, self.poll)

    def show_state(self, state):
        self.state = state
        selection = self.table.selection()
        self.table.delete(*self.table.get_children())
        for key, item in sorted(state['packages'].items()):
            manifest = item['current']['manifest']
            previous = item['previous']['manifest']['version'] if item['previous'] else '无'
            self.table.insert('', 'end', iid=key, values=(manifest['name'], manifest['version'], '启用' if item['enabled'] else '关闭', previous))
        if selection and selection[0] in state['packages']: self.table.selection_set(selection[0])
        self.status.set('共 %d 个主题。修改后请重新打开输入应用。' % len(state['packages']))

    def refresh(self):
        self.run(lambda: read_state(self.root), self.show_state)

    def selected(self, event=None):
        selection = self.table.selection()
        if selection and self.preview is None:
            item = self.state['packages'][selection[0]]
            self.show_details(self.describe(item['current']))
        self.update_buttons()

    @staticmethod
    def describe(package):
        m = package['manifest']
        return '%s\nID：%s    版本：%s\n来源：%s\n许可：%s\n触发记录：%d' % (m['name'], m['id'], m['version'], m['source'], m['license'], len(package['entries']))

    def choose(self):
        path = filedialog.askopenfilename(parent=self.master, title='选择主题词库清单', filetypes=[('词库清单', '*.json')])
        if path: self.preview_file(Path(path))

    def preview_file(self, path):
        self.preview = None; self.preview_path = None
        def work():
            package = load_package(path)
            state = read_state(self.root)
            return package, conflicts(state, package)
        def done(result):
            self.preview, report = result; self.preview_path = path
            text = self.describe(self.preview) + '\n\n主题包之间的冲突（不含基础词库和旧自定义表）：\n'
            for item in report['items']:
                text += '%s：%s ↔ %s（%s，%s）\n' % (item['trigger'], item['incoming'], item['existing'], item['package'], '启用' if item['enabled'] else '关闭')
            if not report['items']: text += '未发现冲突。\n'
            if report['limited']: text += '仅显示前 100 项。\n'
            self.show_details(text); self.status.set('预览完成，尚未写入。点击“导入此预览”保存。')
        self.run(work, done)

    def import_preview(self):
        if self.preview is None: return
        package, path = self.preview, self.preview_path
        def done(state):
            self.preview = None; self.preview_path = None; self.show_state(state)
            self.table.selection_set(package['manifest']['id']); self.selected()
            self.status.set('已导入。新主题默认关闭；选择主题后点击“启用”。')
        self.run(lambda: change(self.root, 'import', path, expected_package=package), done)

    def modify(self, operation):
        selection = self.table.selection()
        if not selection: return
        self.preview = None; self.preview_path = None
        self.run(lambda: change(self.root, operation, selection[0]), self.show_state)


def main():
    from tkinter import messagebox
    window = tk.Tk()
    try:
        local = os.environ.get('LOCALAPPDATA')
        if not local: raise ValueError('缺少 LOCALAPPDATA，无法定位当前用户的词库目录')
        DictionaryManager(window, Path(local) / 'Meowpuut' / 'Dictionaries')
        window.mainloop()
    except Exception as error:
        messagebox.showerror('晴词主题词库', str(error), parent=window)
        window.destroy()


if __name__ == '__main__':
    main()
