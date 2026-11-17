#
# PlatformIO 构建前钩子（被 platformio.ini 的 extra_scripts 调用）
#
# 作用：每次编译前运行 tools/gen_secrets.py，把 secrets.ini 加密成
#       src/05_secrets/gen/secret_blob.h，供固件解密使用。
#       secrets.ini 不存在时自动生成「演示模式」头文件，工程仍可编译。
#
Import("env")

import os
import subprocess
import sys
from pathlib import Path

project_dir = Path(env.subst("$PROJECT_DIR"))
script = project_dir / "tools" / "gen_secrets.py"

if not script.exists():
    print("[secrets] ✗ 找不到 tools/gen_secrets.py")
    env.Exit(1)

# 让子进程用 UTF-8 输出，Windows 控制台也不会把中文提示打成乱码
child_env = dict(os.environ)
child_env.setdefault("PYTHONUTF8", "1")
child_env.setdefault("PYTHONIOENCODING", "utf-8")

result = subprocess.call([sys.executable, str(script)], cwd=str(project_dir),
                         env=child_env)
if result != 0:
    env.Exit(result)
