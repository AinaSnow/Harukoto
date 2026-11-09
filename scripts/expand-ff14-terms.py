"""从固定来源的本地 CSV/JSON 扩充经筛选的制作、道具和货币术语；不自动联网。"""
import argparse
import csv
import io
import json
import re
from pathlib import Path
from ff14_completion import read_completion, align_terms, english_trigger
from ff14_dictionary import validate_catalog
from japanese_dictionary import kana, label

EXTRA_IDS = [1343,1344,1353,1364,1365,1366,1367,1368,1369,1370,1375,1377,1378,1379,1380,1381,1387,1390,1392,
             4009,4012,4013,4020,4023,4024,4032,4043,4044,4047,4049,4055,4056,4059,4060,4061,4062,4068,4069,4070]
ITEM_PINYIN = ['huifuyao','gaojihuifuyao','chaojihuifuyao','tejihuifuyao','yitaiyao','gaojiyitaiyao',
               'chaojiyitaiyao','tejiyitaiyao','shenglingyao','gaojishenglingyao']


def expand(catalog, folder):
    data = json.loads(Path(catalog).read_text(encoding='utf-8'))
    folder = Path(folder)
    tables = [read_completion(folder / (lang + '-completion-pinned.csv')) for lang in ('cn','jp','en')]
    existing = {e['id']: e for e in data['entries']}
    sample = existing['completion-1604']
    urls = {role: next(s['url'] for s in sample['sources'] if s['role'] == role) for role in ('zh_cn','ja','alias')}
    selected = sorted(set(EXTRA_IDS + [id for id,r in tables[0].items() if r['Group'] in (22,27) and r['Key'] > 0]))
    report = {'scope': 'Completion 制作采集组 22、深层迷宫组 27、显式选择的货币与道具 ID；Item 4551–4560',
              'included': [], 'excluded': [], 'not_in_scope': '其余定型文组、动态 LookupTable 以及其余 Item 行；不宣称全游戏词库补全'}
    for row_id in selected:
        try:
            term = align_terms(*tables, [row_id])[0]
            for field in ('zh_cn','ja','english'): label(term[field])
            en = english_trigger(term['english'])
        except ValueError as error:
            report['excluded'].append({'row_id': row_id, 'zh_cn': tables[0][row_id]['Text'], 'reason': str(error)})
            continue
        category = 'item'
        if term['group'] == 22 and row_id not in {1601,1602,1603,1604,1612,1618,1632,1637,1639,1640,1642,1649,1650,1653,1655,1656,1660,1661,1663,1664,1665,1666,1667,1668,1669,1670,1671}: category = 'community'
        if term['group'] == 27 and row_id in {3601,3602,3626,3627,3628,3629,3655}: category = 'place'
        ref = 'Completion 行 %d，Group=%d，Key=%d；按三语言键对齐。' % (row_id,term['group'],term['key'])
        notes = [('zh_cn',urls['zh_cn'],ref+'Text/GroupTitle 为中文名称／拼音。'),
                 ('ja',urls['ja'],ref+'Text 为日文名称。'),
                 ('kana',urls['ja'],ref+'纯片假名直接转换；检索词只含冒号后缀时，拼接已知片假名前缀。魔土器／魔科学器／魔紋起動証要求检索词包含完整已核对前缀。'),
                 ('alias',urls['alias'],ref+'英文名来自 Text，触发时移除空格、连字符和所有格撇号。')]
        existing['completion-'+str(row_id)] = make_entry('completion-'+str(row_id),category,term['zh_cn'],term['ja'],term['kana'],term['pinyin'],term['english'],en,notes)
        report['included'].append({'row_id': row_id, 'zh_cn': term['zh_cn'], 'ja': term['ja'], 'kana':term['kana']})
    rows = [r for r in csv.reader(io.StringIO((folder/'items-cn.txt').read_text(encoding='utf-8'))) if r]
    header = next(i for i,r in enumerate(rows[:4]) if r[0]=='#')
    col = rows[header].index('Name')
    cn = {int(r[0]): r[col] for r in rows[header+1:] if r[0].isdigit()}
    raw_ja = json.loads((folder/'items-ja.txt').read_text(encoding='utf-8'))
    raw_en = json.loads((folder/'items-en.txt').read_text(encoding='utf-8'))
    if raw_ja['version'] != raw_en['version']: raise ValueError('Item 日英数据版本不一致')
    ja = {r['row_id']:r['fields']['Name'] for r in raw_ja['rows']}
    en = {r['row_id']:r['fields']['Name'] for r in raw_en['rows']}
    for row_id, pinyin in zip(range(4551,4561),ITEM_PINYIN):
        reading = kana(ja[row_id])
        if not re.fullmatch('[ぁ-ゖー]{1,32}',reading): raise ValueError('物品缺少明确假名读音')
        cn_url=urls['zh_cn'].replace('Completion.csv','Item.csv')
        base='https://v2.xivapi.com/api/sheet/Item/%d?fields=Name&version=%s&language=' % (row_id, raw_ja['version'])
        notes=[('zh_cn',cn_url,'Item 行 %d 的 Name；只核对名称，拼音按该中文名显式填写。'%row_id),
               ('ja',base+'ja','Item 同一行 ID 和数据版本的日文 Name。'),
               ('kana',base+'ja','日文名称全部为片假名，逐字转换为平假名。'),
               ('alias',base+'en','Item 同一行 ID 和数据版本的英文 Name。')]
        existing['item-'+str(row_id)]=make_entry('item-'+str(row_id),'item',cn[row_id],ja[row_id],reading,pinyin,en[row_id],english_trigger(en[row_id]),notes)
    data['entries'] = sorted(existing.values(),key=lambda e:e['id'])
    data['package']['name']='FF14 职业、道具与制作术语'
    data['package']['version']='0.2.0'
    data['package']['source']='官方职业指南；固定提交 Completion 制作采集、深层迷宫及货币术语；按 Item ID 对齐基础恢复药。范围、遗漏与逐条来源见随包记录。核对日期 2026-11-09。'
    report['total_entries']=len(data['entries'])
    report['total_triggers']=sum(len(e['triggers']['pinyin'])+len(e['triggers']['english']) for e in data['entries'])
    return validate_catalog(data),report


def make_entry(id,category,cn,ja,kana,pinyin,english,en_trigger,notes):
    return {'id':id,'category':category,'zh_cn':cn,'ja':ja,'kana':kana,'english':[english],'aliases':[],
            'triggers':{'pinyin':[pinyin],'english':[en_trigger]},'game_version':'7.56',
            'sources':[{'role':role,'url':url,'accessed':'2026-11-09','note':note} for role,url,note in notes],
            'review':{'status':'verified','reviewer':'Codex：固定来源逐项对齐、读音规则核查；未经过第二位人工审核','date':'2026-11-09',
                      'rights_note':'独立整理的术语名称与对应事实，不复制游戏说明、图标或全量物品库；游戏权利归原权利人，无官方授权背书。'}}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('catalog',type=Path);parser.add_argument('sources',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args()
    data,report=expand(args.catalog,args.sources)
    args.output.mkdir(parents=True,exist_ok=False)
    for name,value in [('catalog.json',data),('coverage.json',report)]:
        (args.output/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(report['total_entries'],report['total_triggers'])


if __name__=='__main__': main()
