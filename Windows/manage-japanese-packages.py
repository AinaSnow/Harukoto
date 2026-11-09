"""晴词本地主题词库：预览、导入、启停和版本回退。"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from japanese_packages import change, conflicts, load_package, read_state


def main():
    # Keep redirected diagnostics readable in modern Windows terminals and build logs.
    for stream in (sys.stdout, sys.stderr):
        if not stream.isatty() and hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, help='指定隔离存储目录；默认当前用户主题词库目录')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list', help='列出主题包及当前、回退版本')
    for name in ('preview', 'import'):
        commands.add_parser(name, help='校验并预览' if name == 'preview' else '导入或更新，新主题默认关闭').add_argument('manifest', type=Path)
    for name in ('enable', 'disable', 'rollback'):
        commands.add_parser(name).add_argument('id')
    args = parser.parse_args()
    try:
        root = args.root
        if root is None:
            local = os.environ.get('LOCALAPPDATA')
            if not local:
                raise ValueError('缺少 LOCALAPPDATA，请使用 --root 指定目录')
            root = Path(local) / 'Meowpuut' / 'Dictionaries'
        state = read_state(root)
        if args.command in {'preview', 'import'}:
            package = load_package(args.manifest)
            print(json.dumps({'manifest': package['manifest'], 'entries': len(package['entries']),
                              'conflicts': conflicts(state, package)}, ensure_ascii=False, indent=2))
            if args.command == 'preview':
                print('仅预览，未写入。冲突仅比较已导入主题包；基础词库和旧自定义词表不在此报告内。')
                return
        if args.command != 'list':
            state = change(root, args.command, args.manifest if args.command == 'import' else args.id)
            print('已保存；请重新打开使用输入法的应用。新主题默认关闭，需执行 enable 才参与候选。')
        for key, item in sorted(state['packages'].items()):
            manifest = item['current']['manifest']
            previous = item['previous']['manifest']['version'] if item['previous'] else '无'
            print(f"{key} | {manifest['name']} | {manifest['version']} | {'启用' if item['enabled'] else '关闭'} | 回退：{previous}")
        if not state['packages']:
            print('尚未导入主题包。')
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
