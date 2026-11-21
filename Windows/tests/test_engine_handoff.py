"""竞争失败后的交接：真实所有者／宿主与客户端子进程；不读取 HKLM，不改动安装。"""
from pathlib import Path
import sys

from test_engine_host import run

if __name__ == '__main__':
    run(Path(sys.argv[1]), Path(sys.argv[2]))
