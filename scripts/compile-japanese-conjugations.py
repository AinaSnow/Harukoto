"""编译晴词自有活用词表 v1；输出独立 JSON，不修改当前 TSF 词库。"""
import argparse
import json
import os
import tempfile
from dataclasses import asdict
from pathlib import Path

from japanese_conjugation import conjugate
from japanese_dictionary import label, trigger


def compile_dictionary(path):
    path = Path(path)
    if path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("活用词表不得超过 2 MB")
    source = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(source, dict) or set(source) != {"schema", "entries"} or type(source["schema"]) is not int or source["schema"] != 1:
        raise ValueError("根对象须为 schema: 1 和 entries 数组")
    entries = source["entries"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= 10000:
        raise ValueError("entries 须包含 1–10000 个词条")
    result, ids = [], set()
    for index, entry in enumerate(entries, 1):
        try:
            required = {"id", "text", "reading", "pos", "triggers"}
            if not isinstance(entry, dict) or not required <= entry.keys() or entry.keys() - required - {"irregular"}:
                raise ValueError("词条字段缺失或含未知字段")
            for key in required - {"triggers"} | {"irregular"}:
                if not isinstance(entry.get(key, ""), str):
                    raise ValueError(key + " 必须为字符串")
            identity = entry["id"]
            if not identity.isascii() or not identity or len(identity) > 64 or any(not (c.isalnum() or c in "-_") for c in identity):
                raise ValueError("id 须为 1–64 位 ASCII 字母、数字、连字符或下划线")
            if identity in ids:
                raise ValueError("重复 id：" + identity)
            ids.add(identity)
            label(entry["text"])
            if len(entry["reading"]) > 32:
                raise ValueError("读音不得超过 32 字")
            triggers = entry["triggers"]
            if not isinstance(triggers, dict) or not triggers or triggers.keys() - {"pinyin", "english"}:
                raise ValueError("triggers 只支持 pinyin／english 数组")
            normalized = {}
            for kind, values in sorted(triggers.items()):
                if not isinstance(values, list) or not 1 <= len(values) <= 16 or any(not isinstance(v, str) for v in values):
                    raise ValueError("每种触发方式须为 1–16 个字符串")
                normalized[kind] = sorted({trigger(v) for v in values})
            forms = conjugate(entry["text"], entry["reading"], entry["pos"], irregular=entry.get("irregular", ""))
            result.append({**entry, "triggers": normalized, "forms": [asdict(form) for form in forms]})
        except ValueError as error:
            raise ValueError(f"词条 {index}: {error}") from error
    return {"schema": 1, "entries": sorted(result, key=lambda entry: entry["id"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = compile_dictionary(args.source)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=args.output.parent, delete=False) as stream:
                    temporary = Path(stream.name)
                    json.dump(result, stream, ensure_ascii=False, indent=2)
                    stream.write("\n")
                os.replace(temporary, args.output)
            finally:
                if temporary:
                    temporary.unlink(missing_ok=True)
        print(f"校验通过：{len(result['entries'])} 个词条；尚未接入输入法。")
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
