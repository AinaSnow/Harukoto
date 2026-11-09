"""下载全量词库的固定 CSV 来源，只写指定缓存目录，不修改安装或个人词库。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import urllib.request


def fetch(source, directory):
    name = source['file']
    if Path(name).name != name or not name.endswith('.csv'): raise ValueError('来源文件名无效')
    path = directory / name
    if path.exists():
        if hashlib.sha256(path.read_bytes()).hexdigest() != source['sha256']:
            raise ValueError('缓存校验失败，请另选空目录：' + name)
        return name
    request = urllib.request.Request(source['url'], headers={'User-Agent': 'Harukoto-dictionary/0.3'})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read(64 * 1024 * 1024 + 1)
    if hashlib.sha256(data).hexdigest() != source['sha256']: raise ValueError('下载摘要不匹配：' + name)
    # Write only after integrity verification; never overwrite an existing cache.
    with path.open('xb') as stream: stream.write(data)
    return name


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    index = Path(__file__).resolve().parents[1] / 'Resources/DictionaryPackages/ff14/full/source-checks.json'
    sources = json.loads(index.read_text(encoding='utf-8'))['sources']
    args.output.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=3) as pool:
        for name in pool.map(lambda source: fetch(source, args.output), sources): print(name)
