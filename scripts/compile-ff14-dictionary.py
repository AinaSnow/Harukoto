"""校验 FF14 审核资料，仅将 verified 词条导出为本地主题包。"""
import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from ff14_dictionary import validate_catalog, word_list
from japanese_packages import read_json, load_package


def export(source, destination):
    data = validate_catalog(read_json(source))
    words, verified = word_list(data)
    destination = Path(destination)
    if destination.exists(): raise ValueError('输出目录已存在，请使用新的目录；不会覆盖旧包')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.ff14-', dir=destination.parent))
    try:
        (stage / 'manifest.json').write_text(json.dumps(data['package'], ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        (stage / data['package']['words']).write_text(words, encoding='utf-8')
        (stage / 'reviewed-source.json').write_text(json.dumps(dict(data, entries=[row for row in data['entries'] if row['id'] in verified]), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        load_package(stage / 'manifest.json')
        # Windows rename refuses an existing destination; the package is published as a whole.
        if destination.exists(): raise ValueError('输出目录已存在')
        os.rename(stage, destination)
    finally:
        if stage.exists(): shutil.rmtree(stage)
    return len(verified)


def main():
    for stream in (sys.stdout, sys.stderr):
        if not stream.isatty() and hasattr(stream, 'reconfigure'): stream.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        data = validate_catalog(read_json(args.source))
        if args.output: print('已导出 %d 条已核对词条。主题导入后默认关闭。' % export(args.source, args.output))
        else: print('校验通过：%d 条；未导出。' % len(data['entries']))
    except (OSError, ValueError) as error: parser.error(str(error))


if __name__ == '__main__': main()
