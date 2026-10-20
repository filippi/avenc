"""Relocate ecCodes/eckit dependencies into the wheel using standard repair tools."""
import os
from importlib.util import find_spec
from pathlib import Path
import subprocess
import sys

wheel, destination = sys.argv[1:]
paths = [str(Path(spec.origin).parent / "lib") for name in ("eccodeslib", "eckitlib") if (spec := find_spec(name)) is not None]
env = os.environ.copy()
key = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
env[key] = os.pathsep.join(paths + [env.get(key, "")])
if sys.platform == "darwin":
    command = ["delocate-wheel", "--require-archs", "arm64" if os.uname().machine == "arm64" else "x86_64", "-v", "-w", destination, wheel]
else:
    command = ["auditwheel", "repair", "--plat", "manylinux_2_28_" + os.uname().machine, "-w", destination, wheel]
subprocess.run(command, env=env, check=True)
