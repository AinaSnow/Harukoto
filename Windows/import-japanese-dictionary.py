"""导入公开词表格式的拼音／英语日语触发词（Windows 当前用户）。"""
import argparse
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from japanese_dictionary import parse_word_list, serialize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dictionary', type=Path)
    parser.add_argument('--check', action='store_true', help='只校验，不写入')
    parser.add_argument('--output', type=Path, help='导出编译结果到指定文件')
    args = parser.parse_args()
    try:
        entries = parse_word_list(args.dictionary)
        if not entries:
            raise ValueError('没有可导入的 pinyin 或 english 触发词；本阶段不生成罗马字或动词变形')
        if not args.check:
            if args.output:
                destination = args.output
            else:
                if not os.environ.get('LOCALAPPDATA'):
                    raise ValueError('缺少 LOCALAPPDATA；请使用 --output 指定输出文件')
                destination = Path(os.environ['LOCALAPPDATA']) / 'Meowpuut' / 'japanese-custom.tsv'
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                             dir=destination.parent, delete=False) as file:
                temporary = Path(file.name)
                file.write(serialize(entries))
            try:
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
            print(f'已写入 {destination}；请重新打开使用输入法的应用。')
        print(f'校验通过：{len(entries)} 组触发词／日语读音。')
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
