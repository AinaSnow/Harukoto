"""临时真实进程验证客户端部署校验，不连接安装中的输入法。"""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='harukoto-engine-verified-peer-') as temporary:
    subprocess.run([str(Path(sys.argv[1]).resolve()), temporary], check=True, timeout=45)
