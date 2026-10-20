"""Fail on oversized distributions and write SHA256SUMS without publishing."""
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1] if len(sys.argv)>1 else "dist")
files = sorted(f for f in root.iterdir() if f.name.endswith((".whl", ".tar.gz")))
if not files: raise SystemExit("No release artifacts")
for f in files:
    if f.suffix == ".whl" and f.stat().st_size >= 100_000_000:
        raise SystemExit(f"Wheel exceeds conservative PyPI 100 MB limit: {f}")
(root/"SHA256SUMS").write_text("".join(f"{hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()}  {f.name}\n" for f in files))
