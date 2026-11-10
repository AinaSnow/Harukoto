"""开发用基础词库快照：成套校验、暂存与回退，不修改运行中输入法。"""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import uuid

FILES = ('native-pinyin.tsv', 'language-evidence.sqlite', 'japanese.tsv', 'japanese-conjugations.bin')
MANIFEST = 'base-dictionary.json'
MAX_SIZE = 512 * 1024 * 1024
VERSION = re.compile(
    r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
    r'(?:-(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)'
    r'(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*)?'
    r'(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_json(path):
    if path.stat().st_size > 65536:
        raise ValueError('元数据超过大小限制')
    return json.loads(path.read_text(encoding='utf-8'))


def identity(manifest):
    return hashlib.sha256(json.dumps(manifest, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()).hexdigest()


def verify(folder):
    folder = Path(folder)
    manifest = read_json(folder / MANIFEST)
    if (not isinstance(manifest, dict) or set(manifest) != {'format', 'engine_abi', 'version', 'files'}
            or type(manifest['format']) is not int or manifest['format'] != 1
            or type(manifest['engine_abi']) is not int or manifest['engine_abi'] != 1
            or not isinstance(manifest['version'], str)
            or not VERSION.fullmatch(manifest['version'])
            or not isinstance(manifest['files'], dict) or set(manifest['files']) != set(FILES)):
        raise ValueError('基础词库清单格式、兼容版本或文件集合不正确')
    for name in FILES:
        entry = manifest['files'][name]
        if (not isinstance(entry, dict) or set(entry) != {'size', 'sha256'}
                or type(entry['size']) is not int or not 0 < entry['size'] <= MAX_SIZE
                or not isinstance(entry['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', entry['sha256'])):
            raise ValueError('无效的词库文件记录：' + name)
        path = folder / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != entry['size'] or digest(path) != entry['sha256']:
            raise ValueError('词库文件缺失或校验不匹配：' + name)
    # Container-level compatibility only. Native loaders must still validate
    # records before any future runtime switch; hashes are not a signature.
    for name, magic in [('native-pinyin.tsv', b'MEOWPUUT-PINYIN-2\n'),
                        ('language-evidence.sqlite', b'SQLite format 3\x00'),
                        ('japanese-conjugations.bin', b'HARUKOTO-CONJ-1\n')]:
        with (folder / name).open('rb') as stream:
            if stream.read(len(magic)) != magic:
                raise ValueError('不支持的词库文件格式：' + name)
    return manifest


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def create(source, destination, version):
    source, destination = Path(source), Path(destination)
    if destination.exists():
        raise ValueError('目标已存在，不覆盖快照')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.base-build-', dir=destination.parent) as temporary:
        stage = Path(temporary) / 'snapshot'; stage.mkdir()
        records = {}
        for name in FILES:
            path = source / name
            if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_SIZE:
                raise ValueError('构建文件缺失或超限：' + name)
            records[name] = dict(size=path.stat().st_size, sha256=digest(path))
            shutil.copyfile(path, stage / name)
        atomic_json(stage / MANIFEST, dict(format=1, engine_abi=1, version=version, files=records))
        manifest = verify(stage)
        stage.rename(destination)
    return identity(manifest)


@contextlib.contextmanager
def locked(root):
    root.mkdir(parents=True, exist_ok=True)
    lock = root / '.write.lock'
    stream = lock.open('x')
    try:
        yield
    finally:
        stream.close()
        lock.unlink()


def state(root):
    path = root / 'STATE.json'
    if not path.exists():
        return dict(format=1, current=None, previous=None)
    value = read_json(path)
    if (not isinstance(value, dict) or set(value) != {'format', 'current', 'previous'}
            or type(value['format']) is not int or value['format'] != 1
            or any(v is not None and (not isinstance(v, str) or not re.fullmatch('[0-9a-f]{64}', v))
                   for v in (value['current'], value['previous']))
            or value['current'] is None):
        raise ValueError('损坏的快照状态，拒绝覆盖')
    return value


def check_generation(root, key):
    manifest = verify(root / key)
    if identity(manifest) != key:
        raise ValueError('快照目录与内容标识不一致')
    return manifest


def stage(source, root):
    source, root = Path(source), Path(root)
    manifest = verify(source); key = identity(manifest)
    with locked(root):
        previous = state(root)
        if previous['current']:
            check_generation(root, previous['current'])
        if not (root / key).exists():
            with tempfile.TemporaryDirectory(prefix='.base-stage-', dir=root) as temporary:
                copied = Path(temporary) / 'snapshot'; copied.mkdir()
                for name in (*FILES, MANIFEST):
                    shutil.copyfile(source / name, copied / name)
                if verify(copied) != manifest:
                    raise ValueError('复制期间源快照发生变化')
                copied.rename(root / key)
        check_generation(root, key)
        if key != previous['current']:
            atomic_json(root / 'STATE.json', dict(format=1, current=key, previous=previous['current']))
    return key


def rollback(root):
    root = Path(root)
    with locked(root):
        previous = state(root)
        if not previous['previous']:
            raise ValueError('没有可回退快照')
        check_generation(root, previous['current'])
        check_generation(root, previous['previous'])
        atomic_json(root / 'STATE.json', dict(format=1, current=previous['previous'], previous=previous['current']))
    return previous['previous']
