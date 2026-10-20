"""Extract an audited wheel's native payload into a Python-independent tarball."""
import argparse
from pathlib import Path
import shutil
import tarfile
import tempfile
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("wheel", type=Path)
parser.add_argument("--output", type=Path, default=Path("dist"))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
version = (repo/"VERSION").read_text().strip()
tag = args.wheel.stem.split("-")[-1]
if "manylinux" in tag:
    platform = "linux-" + ("aarch64" if "aarch64" in tag else "x86_64")
elif "macosx" in tag:
    platform = "macos-" + ("arm64" if "arm64" in tag else "x86_64")
else:
    raise SystemExit(f"Unsupported native archive platform: {tag}")
name = f"avenc-{version}-{platform}"
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)/name
    payload = root/"libexec/avenc"
    with zipfile.ZipFile(args.wheel) as wheel:
        for item in wheel.infolist():
            if item.is_dir(): continue
            if ".dist-info/licenses/" in item.filename:
                target = payload/"licenses"/item.filename.split(".dist-info/licenses/",1)[1]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(wheel.read(item))
            if item.filename.startswith(("avenc/_native/", "avenc/.dylibs/", "avenc.libs/")):
                target = payload/item.filename
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(wheel.read(item))
                target.chmod((item.external_attr >> 16) & 0o777 or 0o644)
                if "/bin/" in item.filename: target.chmod(0o755)
    assert (payload/"avenc/_native/bin/grib2avenc").is_file()
    (root/"bin").mkdir(parents=True)
    tools = ("grib2avenc", "avenc2grib", "avpack", "avenc-compare", "ComputeError", "grib_pair_extract", "avenc_test_generator")
    for tool in tools:
        launcher = root/"bin"/tool
        launcher.write_text('#!/bin/sh\n# AVENC native release launcher\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec "$HERE/../libexec/avenc/avenc/_native/bin/' + tool + '" "$@"\n')
        launcher.chmod(0o755)
    for file in ("LICENSE", "THIRD_PARTY.md"):
        shutil.copy2(repo/file, root/file)
    for file in ("install.sh", "uninstall.sh"):
        shutil.copy2(repo/"packaging"/file, root/file)
        (root/file).chmod(0o755)
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output/(name+".tar.gz")
    with tarfile.open(output, "w:gz") as archive: archive.add(root, arcname=name)
    print(output)
