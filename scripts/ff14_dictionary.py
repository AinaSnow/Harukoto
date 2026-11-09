"""Validate reviewed FF14 terms and export an ordinary disabled-by-default theme."""
import datetime
import re
from urllib.parse import urlparse

from japanese_dictionary import label, trigger
from japanese_packages import ID, validate_manifest

CATEGORIES = {'job', 'action', 'duty', 'place', 'character', 'item', 'community'}
FIELDS = {'id', 'category', 'zh_cn', 'ja', 'kana', 'english', 'aliases', 'triggers',
          'game_version', 'sources', 'review'}


def validate_catalog(data):
    if not isinstance(data, dict) or set(data) != {'format', 'package', 'entries'} or type(data['format']) is not int or data['format'] != 1:
        raise ValueError('FF14 词条文件须包含 format=1、package、entries')
    validate_manifest(data['package'])
    entries = data['entries']
    if not isinstance(entries, list) or len(entries) > 10000:
        raise ValueError('FF14 词条不能超过 10000 条')
    ids = set()
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != FIELDS:
            raise ValueError('词条字段不完整或存在未知字段')
        eid = entry['id']
        if not isinstance(eid, str) or not ID.fullmatch(eid) or eid in ids:
            raise ValueError('词条 ID 非法或重复')
        ids.add(eid)
        def fail(message): raise ValueError(eid + '：' + message)
        if not isinstance(entry['category'], str) or entry['category'] not in CATEGORIES: fail('分类无效')
        for field in ('zh_cn', 'ja'):
            if not isinstance(entry[field], str): fail(field + ' 必须为字符串')
            label(entry[field])
        if not isinstance(entry['kana'], str) or not re.fullmatch('[ぁ-ゖー]{1,32}', entry['kana']): fail('须显式填写平假名读音')
        if not isinstance(entry['game_version'], str) or not re.fullmatch(r'[0-9]+[.][0-9]+(?:[.][0-9]+)?', entry['game_version']): fail('游戏版本须为数字版本号')
        for field in ('english', 'aliases'):
            if not isinstance(entry[field], list) or len(entry[field]) > 32: fail(field + ' 须为最多 32 项的列表')
            for value in entry[field]:
                if not isinstance(value, str): fail(field + ' 内容必须为字符串')
                label(value)
        triggers = entry['triggers']
        if not isinstance(triggers, dict) or set(triggers) != {'pinyin', 'english'}: fail('triggers 须分别记录 pinyin 与 english')
        count = 0
        for kind, values in triggers.items():
            if not isinstance(values, list) or len(values) > 32: fail('每类触发词最多 32 项')
            for value in values:
                if not isinstance(value, str): fail('触发词必须为字符串')
                trigger(value); count += 1
        if count == 0: fail('至少填写一个明确触发词，别名不会自动加入输入')
        sources = entry['sources']
        if not isinstance(sources, list) or not 1 <= len(sources) <= 16: fail('须记录 1–16 个来源')
        roles = set()
        for source in sources:
            if not isinstance(source, dict) or set(source) != {'role', 'url', 'accessed', 'note'}: fail('来源字段无效')
            if source['role'] not in ('zh_cn', 'ja', 'kana', 'alias'): fail('来源用途无效')
            roles.add(source['role'])
            if not isinstance(source['url'], str) or len(source['url']) > 2048: fail('来源 URL 无效')
            url = urlparse(source['url'])
            if url.scheme != 'https' or not url.hostname or url.username or url.password: fail('来源须为不含账户信息的 HTTPS 链接')
            try: datetime.date.fromisoformat(source['accessed'])
            except (ValueError, TypeError): fail('来源核对日期无效')
            if not isinstance(source['note'], str) or not source['note'].strip() or len(source['note']) > 1024: fail('来源须填写核对说明')
        review = entry['review']
        if not isinstance(review, dict) or set(review) != {'status', 'reviewer', 'date', 'rights_note'}: fail('审核字段无效')
        if review['status'] not in ('draft', 'verified'): fail('审核状态只能是 draft 或 verified')
        for field in ('reviewer', 'date', 'rights_note'):
            if not isinstance(review[field], str) or len(review[field]) > 2048: fail('审核文本无效')
        if review['status'] == 'verified':
            if not {'zh_cn', 'ja', 'kana'} <= roles: fail('已核对词条须提供国服名、日服名及读音来源')
            if not review['reviewer'].strip() or not review['rights_note'].strip(): fail('已核对词条须记录审核人及使用依据')
            try: datetime.date.fromisoformat(review['date'])
            except ValueError: fail('审核日期无效')
    return data


def word_list(data):
    validate_catalog(data)
    lines = ['# ' + data['package']['name'][:40]]
    verified = []
    for entry in sorted(data['entries'], key=lambda row: row['id']):
        if entry['review']['status'] != 'verified': continue
        parts = [entry['ja'], entry['kana'], 'chinese: ' + entry['zh_cn']]
        for kind in ('pinyin', 'english'):
            for value in entry['triggers'][kind]: parts.append(kind + ': ' + trigger(value))
        lines.append(' | '.join(parts)); verified.append(entry['id'])
    if not verified: raise ValueError('没有已核对词条，不生成可导入主题包')
    return '\n'.join(lines) + '\n', verified
