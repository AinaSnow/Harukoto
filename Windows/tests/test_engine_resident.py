"""常驻生命周期：真实常驻所有者（测试构建）、带／不带 breakaway 的 Job 中的启动方子进程；只写 HKCU 测试键，不读写 HKLM、Run 键或计划任务。"""
from pathlib import Path
import sys

from test_engine_host import run

if __name__ == '__main__':
    run(Path(sys.argv[1]), Path(sys.argv[2]))
