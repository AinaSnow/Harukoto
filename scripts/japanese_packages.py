"""Local theme packages: immutable snapshots and one atomic activation pointer."""
import contextlib
import copy
import json
import os
import re
import tempfile
import uuid
from pathlib import Path

from japanese_dictionary import parse_word_list, serialize

MAX_BYTES = 32 * 1024 * 1024
MAX_ENTRIES = 100_000
ID = re.compile(r'[a-z][a-z0-9-]{0,63}')
GENERATION = re.compile(r'[0-9a-f]{32}')
MANIFEST_FIELDS = {'format', 'id', 'name', 'version', 'source', 'license', 'words'}


def read_json(path, limit=MAX_BYTES):
    with Path(path).open('rb') as file:
        data = file.read(limit + 1)
    if len(data) > limit:
        raise ValueError('文件超过大小限制：' + str(path))
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('JSON 属性重复：' + key)
            result[key] = value
        return result
    return json.loads(data.decode('utf-8-sig'), object_pairs_hook=unique)


def validate_manifest(manifest):
    if not isinstance(manifest, dict) or set(manifest) != MANIFEST_FIELDS:
        raise ValueError('清单字段必须为 format、id、name、version、source、license、words')
    if type(manifest['format']) is not int or manifest['format'] != 1:
        raise ValueError('不支持的主题包格式')
    for field in MANIFEST_FIELDS - {'format'}:
        value = manifest[field]
        if not isinstance(value, str) or not value.strip() or len(value) > 1024 or any(ord(c) < 32 for c in value):
            raise ValueError('清单字段无效：' + field)
    if not ID.fullmatch(manifest['id']):
        raise ValueError('id 须以小写字母开头，只含小写字母、数字和短横线，最多 64 字')
    if len(manifest['name']) > 80 or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+-]{0,63}', manifest['version']):
        raise ValueError('名称最多 80 字；版本须为 1–64 个字母、数字、点、加号或短横线')
    # Only a sibling plain-text file; never interpret package data as a path outside its directory.
    if not re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]{0,79}[.]txt', manifest['words']):
        raise ValueError('words 须为同目录下的 .txt 文件名，不能包含路径')
    return manifest


def load_package(path):
    path = Path(path)
    manifest = validate_manifest(read_json(path, 64 * 1024))
    words = path.parent / manifest['words']
    if words.resolve().parent != path.parent.resolve():
        raise ValueError('词表不能链接到包目录之外')
    entries = parse_word_list(words)
    if not entries:
        raise ValueError('主题包没有有效的拼音／英语触发词')
    package = {'manifest': manifest, 'entries': [list(row) for row in entries]}
    validate_package(package)
    return package


def validate_package(package):
    if not isinstance(package, dict) or set(package) != {'manifest', 'entries'}:
        raise ValueError('主题包快照损坏')
    validate_manifest(package['manifest'])
    rows = package['entries']
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_ENTRIES:
        raise ValueError('主题包条目数无效')
    for row in rows:
        if not isinstance(row, list) or len(row) != 6:
            raise ValueError('主题包候选结构损坏')
        reading, text, weight, kana, source, chinese = row
        if (not all(isinstance(x, str) for x in (reading, text, kana, source, chinese))
                or not re.fullmatch('[a-z]{2,40}', reading)
                or not 1 <= len(text) <= 32 or not re.fullmatch('[ぁ-ゖー]{1,32}', kana)
                or source not in {'pinyin', 'english'} or len(chinese) > 32
                or type(weight) is not int or not 1 <= weight <= 9999
                or any(c in text + chinese for c in '|\t\r\n')
                or any(ord(c) < 32 for c in text + chinese)):
            raise ValueError('主题包候选内容损坏')


def validate_state(state):
    if not isinstance(state, dict) or set(state) != {'format', 'packages'} or type(state['format']) is not int or state['format'] != 1:
        raise ValueError('词库状态格式损坏')
    packages = state['packages']
    if not isinstance(packages, dict) or len(packages) > 128:
        raise ValueError('主题包不能超过 128 个')
    count = 0
    for key, item in packages.items():
        if not isinstance(item, dict) or set(item) != {'enabled', 'current', 'previous'} or type(item['enabled']) is not bool:
            raise ValueError('主题包状态损坏')
        for package in (item['current'], item['previous']):
            if package is None:
                continue
            validate_package(package)
            if package['manifest']['id'] != key:
                raise ValueError('主题包 ID 与索引不一致')
            count += len(package['entries'])
        if item['current'] is None:
            raise ValueError('主题包缺少当前版本')
    if count > MAX_ENTRIES:
        raise ValueError('主题包总条目（含回退版本）不能超过 100000 条')
    return state


def read_state(root):
    root = Path(root)
    pointer = root / 'CURRENT'
    try:
        with pointer.open('rb') as file:
            raw = file.read(34)
    except FileNotFoundError:
        return {'format': 1, 'packages': {}}
    try:
        generation = raw.decode('ascii')
    except UnicodeError as error:
        raise ValueError('主题包激活指针损坏') from error
    if not GENERATION.fullmatch(generation):
        raise ValueError('主题包激活指针损坏')
    return validate_state(read_json(root / generation / 'state.json'))


def compiled(state):
    rows = []
    for key, item in sorted(state['packages'].items()):
        if item['enabled']:
            rows.extend(item['current']['entries'])
    # Exact duplicates across packages need only one runtime row. Conflicting meanings coexist.
    rows = sorted(set(tuple(row) for row in rows), key=lambda row: (-row[2], row[0], row[1:2], row[3:]))
    result = serialize(rows).encode('utf-8')
    if len(result) > MAX_BYTES:
        raise ValueError('合并词库超过 32 MB')
    return result


@contextlib.contextmanager
def writer_lock(root):
    root.mkdir(parents=True, exist_ok=True)
    with (root / '.lock').open('a+b') as file:
        file.seek(0, 2)
        if file.tell() == 0:
            file.write(b'0'); file.flush()
        file.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError('其他词库管理操作正在执行，请稍后重试') from error
        try:
            yield
        finally:
            file.seek(0)
            if os.name == 'nt':
                msvcrt.locking(file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(file.fileno(), fcntl.LOCK_UN)


def write_state(root, state):
    validate_state(state)
    payload = compiled(state)
    metadata = (json.dumps(state, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')
    if len(metadata) > MAX_BYTES:
        raise ValueError('主题包状态超过 32 MB')
    generation = uuid.uuid4().hex
    folder = root / generation
    folder.mkdir()
    # Readers use one immutable snapshot, even while the next update is being prepared.
    for name, data in (('state.json', metadata), ('enabled.tsv', payload)):
        with (folder / name).open('xb') as file:
            file.write(data); file.flush(); os.fsync(file.fileno())
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=root, delete=False) as file:
            temporary = Path(file.name)
            file.write(generation.encode('ascii')); file.flush(); os.fsync(file.fileno())
        os.replace(temporary, root / 'CURRENT')
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def conflicts(state, package):
    index = {}
    current_id = package['manifest']['id']
    for key, item in sorted(state['packages'].items()):
        if key == current_id:
            continue
        for row in item['current']['entries']:
            index.setdefault(row[0], []).append((key, row, item['enabled']))
    result = []
    for row in package['entries']:
        for key, other, enabled in index.get(row[0], []):
            result.append({'trigger': row[0], 'package': key, 'enabled': enabled,
                           'kind': 'duplicate' if row[1:2] + row[3:4] == other[1:2] + other[3:4] else 'alternative',
                           'incoming': row[1], 'existing': other[1]})
            if len(result) == 100:
                return {'items': result, 'limited': True}
    return {'items': result, 'limited': False}


def change(root, operation, value):
    root = Path(root)
    incoming = load_package(value) if operation == 'import' else None
    with writer_lock(root):
        state = copy.deepcopy(read_state(root))
        packages = state['packages']
        if incoming is not None:
            key = incoming['manifest']['id']
            old = packages.get(key)
            if old and old['current'] == incoming:
                return state
            if old and old['current']['manifest']['version'] == incoming['manifest']['version']:
                raise ValueError('同一版本的内容发生变化，请先更新版本号')
            packages[key] = {'enabled': old['enabled'] if old else False, 'current': incoming,
                             'previous': old['current'] if old else None}
        else:
            if value not in packages:
                raise ValueError('找不到主题包：' + value)
            item = packages[value]
            if operation in {'enable', 'disable'}:
                item['enabled'] = operation == 'enable'
            elif operation == 'rollback':
                if item['previous'] is None:
                    raise ValueError('此主题包没有可回退版本')
                item['current'], item['previous'] = item['previous'], item['current']
            else:
                raise ValueError('未知操作：' + operation)
        write_state(root, state)
        return state
