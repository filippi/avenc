"""Build standard ecCodes without the optional eckit-geo networking dependency.

Linux build prerequisites: CMake/C++17, zlib, libpng and OpenJPEG development
packages. Release images install them once; end users of wheels need none.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
from urllib.request import urlopen

parser = argparse.ArgumentParser()
parser.add_argument("--prefix", type=Path, required=True)
args = parser.parse_args()
prefix = args.prefix.resolve()
manifest = json.loads((Path(__file__).with_name("native_sources.json")).read_text())
marker = prefix/"avenc-native-build.json"
if marker.exists() and json.loads(marker.read_text()) == manifest:
    raise SystemExit(0)
work = prefix.parent/(prefix.name+".build")
work.mkdir(parents=True, exist_ok=True)
cache = Path(os.environ.get("AVENC_SOURCE_CACHE", str(work/"downloads")))
cache.mkdir(parents=True, exist_ok=True)
sources = {}
for name, spec in manifest.items():
    archive = cache/(name+".tar.gz")
    if not archive.exists():
        print(f"Downloading {name} {spec['version']}", flush=True)
        with urlopen(spec["url"], timeout=120) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
        raise SystemExit(f"Source checksum mismatch: {name}")
    folder = work/(name+"-source")
    if not folder.exists():
        folder.mkdir()
        with tarfile.open(archive) as package:
            package.extractall(folder, filter="data")
    sources[name] = next(f for f in folder.iterdir() if f.is_dir())

def build(name, options):
    directory = work/(name+"-build")
    subprocess.run(["cmake", "-S", str(sources[name]), "-B", str(directory),
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_SHARED_LIBS=ON",
        "-DCMAKE_INSTALL_RPATH=$ORIGIN", "-DBUILD_TESTING=OFF", *options], check=True)
    subprocess.run(["cmake", "--build", str(directory), "--parallel", str(min(os.cpu_count() or 2, 4)), *(["--target", "eccodes"] if name == "eccodes" else [])], check=True)
    # ecbuild generates installable .pc files in separate ALL targets.
    # Generate those explicitly when compiling only the eccodes library target.
    if name == "eccodes":
        for script in directory.glob("*-pkg-config-build.cmake"):
            subprocess.run(["cmake", "-P", str(script)], check=True)
    subprocess.run(["cmake", "--install", str(directory)], check=True)

build("libaec", [])
build("eccodes", [f"-Decbuild_DIR={sources['ecbuild'] / 'cmake'}",
    f"-DCMAKE_PREFIX_PATH={prefix}", "-DENABLE_ECKIT_GEO=OFF", "-DENABLE_FORTRAN=OFF",
    "-DENABLE_BUILD_TOOLS=OFF", "-DENABLE_EXAMPLES=OFF", "-DENABLE_TESTS=OFF",
    "-DENABLE_MEMFS=ON", "-DENABLE_PNG=ON", "-DENABLE_JPG_LIBOPENJPEG=ON",
    "-DENABLE_AEC=ON", "-DENABLE_USE_SHARED_LIB_AEC=ON", "-DENABLE_NETCDF=OFF"])
notices = prefix/"share/avenc-native-notices"
notices.mkdir(parents=True, exist_ok=True)
for name in ("eccodes", "libaec"):
    for filename in ("LICENSE", "LICENSE.txt", "COPYING", "AUTHORS"):
        source = sources[name]/filename
        if source.is_file(): shutil.copy2(source, notices/(name+"-"+filename))
marker.write_text(json.dumps(manifest, indent=2)+"\n")
