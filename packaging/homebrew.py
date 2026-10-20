"""Generate a stable tap formula from a downloaded, checksummed release archive."""
import argparse
import hashlib
import tarfile
from pathlib import Path
parser = argparse.ArgumentParser()
parser.add_argument("archive", type=Path, help="GitHub vVERSION source .tar.gz")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
version = (repo/"VERSION").read_text().strip()
with tarfile.open(args.archive) as source:
    candidates = [m for m in source.getmembers() if m.name.endswith("/VERSION") and m.isfile()]
    if len(candidates) != 1 or source.extractfile(candidates[0]).read().decode().strip() != version:
        raise SystemExit("The source archive VERSION must match this checkout")
digest = hashlib.file_digest(args.archive.open("rb"), "sha256").hexdigest()
text = (repo/"Formula/avenc.rb").read_text().split("\n",1)[1]
text = text.replace('  head "https://github.com/filippi/avenc.git", branch: "main"', f'  url "https://github.com/filippi/avenc/archive/refs/tags/v{version}.tar.gz"\n  sha256 "{digest}"')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(text)
