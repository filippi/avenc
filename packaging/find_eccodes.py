"""Print CMake's two build-only library prefixes; never record them in metadata."""
from importlib.util import find_spec
from pathlib import Path
print(";".join(str(Path(find_spec(name).origin).parent) for name in ("eccodeslib", "eckitlib")))
