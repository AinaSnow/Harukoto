"""用临时副本核验部署固定及真实子进程，不改安装目录。"""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='harukoto-engine-deployment-') as temporary:
    subprocess.run([str(Path(sys.argv[1]).resolve()), temporary], check=True, timeout=45)
