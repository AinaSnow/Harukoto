#!/usr/bin/env python3
"""基础词库快照管理；发布到运行时仓库后，各应用可在新组字边界加载。"""
import argparse
import json
from pathlib import Path
import sys
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import base_dictionary as bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    create = commands.add_parser('create', help='从构建输出创建完整快照')
    create.add_argument('source', type=Path)
    create.add_argument('destination', type=Path)
    create.add_argument('--version', required=True)
    verify = commands.add_parser('verify', help='检查文件完整性与容器格式')
    verify.add_argument('snapshot', type=Path)
    verify.add_argument('--native-loader', type=Path, help='额外运行已构建的 harukoto_base_dictionary_check.exe')
    stage = commands.add_parser('stage', help='发布完整快照到指定仓库；使用运行时路径时输入法可自动加载')
    stage.add_argument('snapshot', type=Path)
    stage.add_argument('--store', type=Path, required=True)
    for name in ('rollback', 'status'):
        command = commands.add_parser(name)
        command.add_argument('--store', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'create':
            result = bundle.create(args.source, args.destination, args.version)
        elif args.command == 'verify':
            result = bundle.verify(args.snapshot)
            if args.native_loader:
                checked = subprocess.run([str(args.native_loader.resolve()), '--snapshot', str(args.snapshot.resolve())],
                                         capture_output=True, encoding='utf-8', errors='replace', timeout=120)
                if checked.returncode:
                    raise ValueError('原生加载失败：' + checked.stderr.strip())
                native = json.loads(checked.stdout)
                if (not isinstance(native, dict) or native.get('generation') != bundle.identity(result) or native.get('native_loaded') is not True
                        or native.get('runtime_activated') is not False):
                    raise ValueError('原生加载结果与快照不一致')
                result = dict(manifest=result, native=native)
        elif args.command == 'stage':
            result = bundle.stage(args.snapshot, args.store)
        elif args.command == 'rollback':
            result = bundle.rollback(args.store)
        else:
            result = bundle.state(args.store)
            for key in ('current', 'previous'):
                if result[key]:
                    bundle.check_generation(args.store, result[key])
    except (OSError, ValueError, TypeError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps(dict(result=result, runtime_activation_confirmed=False), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
