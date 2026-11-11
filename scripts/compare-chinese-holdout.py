"""CN-01d: manual first-candidate comparison against another input method.

Only observed results (first candidate text and a rank) are recorded; no
dictionary, frequency or other data of the other product is copied.
"""
import argparse
import csv
import io
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COLUMNS = ['编号', '类别', '输入', '期望', '晴词首选', '晴词名次', '对照首选', '期望词名次', '备注']
CONDITIONS = ['#对照输入法', '#版本', '#云候选是否开启', '#个人学习是否开启', '#记录日期']
RUN = 'hans-ff14-on'  # Simplified, with the theme samples included.


def baseline(path):
    report = json.loads(path.read_text(encoding='utf-8'))
    if report.get('corpus_set') != 'holdout':
        raise ValueError('基线报告不是独立评测集')
    return report['runs'][RUN]['quality']


def template(rows):
    out = io.StringIO()
    writer = csv.writer(out, lineterminator='\r\n')
    writer.writerow(COLUMNS)
    for name in CONDITIONS:
        writer.writerow([name, ''])
    for row in rows:
        writer.writerow([row['id'], row['category'], row['input'], ';'.join(row['expected']),
                         row['first']['text'] if row['first'] else '', row['rank'] or '', '', '', ''])
    return out.getvalue()


def read(path):
    data = path.read_bytes()
    for encoding in ('utf-8-sig', 'gb18030'):  # Excel may save either.
        try:
            text = data.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        raise ValueError('无法识别记录表编码')
    rows = list(csv.reader(io.StringIO(text)))
    if not rows or rows[0][:len(COLUMNS)] != COLUMNS:
        raise ValueError('记录表表头不符')
    return [row + [''] * (len(COLUMNS) - len(row)) for row in rows[1:] if any(cell.strip() for cell in row)]


def summarize(sheet, rows):
    conditions = {r[0]: r[1].strip() for r in sheet if r[0].startswith('#')}
    filled = {r[0]: r for r in sheet if not r[0].startswith('#')}
    if set(filled) != {r['id'] for r in rows}:
        raise ValueError('记录表的编号与基线不一致')
    items, pending = [], []
    for row in rows:
        entry = filled[row['id']]
        if entry[2] != row['input'] or entry[3].split(';') != row['expected']:
            raise ValueError(f'{row["id"]} 的输入或期望被改动')
        first, rank = entry[6].strip(), entry[7].strip()
        if not first:
            pending.append(row['id'])
            continue
        hit = first in row['expected']
        if rank and (not rank.isdigit() or (hit and rank != '1') or (not hit and rank == '1')):
            raise ValueError(f'{row["id"]} 的期望词名次无效：首选命中时留空或填 1，否则填 2 及以上，首屏没有填 0')
        items.append(dict(id=row['id'], category=row['category'], input=row['input'], expected=row['expected'],
                          harukoto_first=entry[4], harukoto_rank=row['rank'], other_first=first,
                          other_rank=1 if hit else (int(rank) or None) if rank else None,
                          other_rank_recorded=hit or bool(rank), note=entry[8].strip()))

    def count(group):
        ours = [i['harukoto_rank'] == 1 for i in group]
        theirs = [i['other_rank'] == 1 for i in group]
        return dict(samples=len(group), harukoto_first=sum(ours), other_first=sum(theirs),
                    both=sum(a and b for a, b in zip(ours, theirs)), only_harukoto=sum(a and not b for a, b in zip(ours, theirs)),
                    only_other=sum(b and not a for a, b in zip(ours, theirs)), neither=sum(not a and not b for a, b in zip(ours, theirs)))
    return dict(format=1, conditions=conditions, pending=pending, summary=count(items),
                by_category={c: count([i for i in items if i['category'] == c]) for c in sorted({i['category'] for i in items})},
                only_other=[i for i in items if i['other_rank'] == 1 and i['harukoto_rank'] != 1],
                only_harukoto=[i for i in items if i['harukoto_rank'] == 1 and i['other_rank'] != 1],
                neither=[i for i in items if i['harukoto_rank'] != 1 and i['other_rank'] != 1])


def main():
    parser = argparse.ArgumentParser(description='独立评测集与另一款输入法的人工结果对照')
    parser.add_argument('--baseline', type=Path, default=ROOT / 'docs/benchmarks/chinese-holdout-v1-baseline.json')
    parser.add_argument('--template', type=Path, help='生成空白记录表（已存在则拒绝覆盖）')
    parser.add_argument('--sheet', type=Path, help='已填写的记录表')
    parser.add_argument('--output', type=Path, help='汇总 JSON 输出路径')
    args = parser.parse_args()
    if bool(args.template) == bool(args.sheet):
        parser.error('--template 与 --sheet 二选一')
    rows = baseline(args.baseline)
    if args.template:
        with args.template.open('x', encoding='utf-8-sig', newline='') as stream:  # BOM: Excel opens it as UTF-8.
            stream.write(template(rows))
        print(args.template)
        return
    result = summarize(read(args.sheet), rows)
    if args.output:
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('conditions', 'summary', 'by_category')}, ensure_ascii=False, indent=2))
    if result['pending']:
        print(f'尚未填写 {len(result["pending"])} 条：' + '、'.join(result['pending']))
    for item in result['only_other']:
        print(f'仅对照首选命中 {item["id"]} {item["input"]}：期望 {item["expected"][0]}，晴词首选 {item["harukoto_first"]}（名次 {item["harukoto_rank"]}）')


if __name__ == '__main__':
    main()
