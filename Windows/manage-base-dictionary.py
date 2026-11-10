#!/usr/bin/env python3
"""开发用基础词库快照管理，不改变已安装或运行中的输入法。"""
import argparse
import json
from pathlib import Path
import sys

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
    stage = commands.add_parser('stage', help='暂存完整快照并更新开发仓库指针')
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
        elif args.command == 'stage':
            result = bundle.stage(args.snapshot, args.store)
        elif args.command == 'rollback':
            result = bundle.rollback(args.store)
        else:
            result = bundle.state(args.store)
            for key in ('current', 'previous'):
                if result[key]:
                    bundle.check_generation(args.store, result[key])
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(dict(result=result, runtime_activated=False), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
