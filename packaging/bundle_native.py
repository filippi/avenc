"""Bundle the native dependency closure, including for `pip install .`.

The release repair pass additionally verifies platform tags and relocation.
FFmpeg is never copied. System C/C++ runtimes remain system dependencies.
"""
import importlib.metadata
import hashlib
import json
from importlib.util import find_spec
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

root = Path(sys.argv[1])
bindir = root / "avenc/_native/bin"
roots = [Path(find_spec(name).origin).parent for name in ("eccodeslib", "eckitlib")]
search = [str(r / "lib") for r in roots] + [str(r / ".dylibs") for r in roots] + [str(bindir)]
key = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
os.environ[key] = os.pathsep.join(search + [os.environ.get(key, "")])
if sys.platform == "darwin":
    from delocate.delocating import delocate_path
    delocate_path(str(bindir), str(bindir), sanitize_rpaths=True)
else:
    # These are part of the manylinux ABI. Everything else is copied and later
    # audited by auditwheel. ldd is run only on our freshly built ELF binaries.
    system = re.compile(r"^(lib(c|m|dl|pthread|rt|util|resolv|z)\.so|libstdc\+\+\.so|libgcc_s\.so|ld-linux)")
    pending = list(bindir.iterdir())
    seen = set()
    while pending:
        binary = pending.pop()
        if binary in seen: continue
        seen.add(binary)
        output = subprocess.check_output(["ldd", str(binary)], text=True)
        if "not found" in output: raise RuntimeError(output)
        for line in output.splitlines():
            match = re.match(r"\s*(\S+) => (/\S+)", line)
            if not match: continue
            name, path = match.groups()
            if system.match(name): continue
            target = bindir / Path(name).name
            if not target.exists():
                shutil.copy2(path, target)
                target.chmod(0o755)
                pending.append(target)
            if "/" in name:
                subprocess.run(["patchelf", "--replace-needed", name, target.name, str(binary)], check=True)
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", str(binary)], check=True)
# Retain upstream copyright/licence files in addition to curated dependency notices.
licenses = root / "avenc/_native/licenses"
for name in ("eccodeslib", "eckitlib"):
    distribution = importlib.metadata.distribution(name)
    for item in distribution.files or []:
        if ".dist-info/licenses/" in str(item):
            target = licenses / name / Path(str(item).split(".dist-info/licenses/", 1)[1])
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(distribution.locate_file(item), target)

(root/"avenc/_native/dependencies.json").write_text(json.dumps({
    "build_inputs": {name: importlib.metadata.version(name) for name in ("eccodeslib", "eckitlib")},
    "libraries": [{"file": f.name, "sha256": hashlib.sha256(f.read_bytes()).hexdigest()} for f in sorted(bindir.iterdir()) if ".so" in f.name or f.suffix == ".dylib"],
}, indent=2) + "\n")
