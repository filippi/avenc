"""PATH entry points for the native executables shipped in the wheel."""
import os
from pathlib import Path
import shutil
import sys


def _run(name):
    binary = Path(__file__).parent / "_native" / "bin" / name
    if not binary.is_file():
        raise SystemExit(f"AVENC native executable missing: {binary}. Reinstall a supported binary wheel.")
    informational = any(a in {"--help", "-h", "--version", "--list-options"} for a in sys.argv[1:])
    if not informational and name in {"grib2avenc", "avenc2grib", "avpack", "avenc-compare"}:
        missing = [tool for tool in ("ffmpeg", "ffprobe") if not shutil.which(tool)]
        if missing:
            raise SystemExit("AVENC requires FFmpeg/ffprobe on PATH. Install prebuilt FFmpeg "
                             "with `brew install ffmpeg` or `sudo apt install ffmpeg`. Missing: " + ", ".join(missing))
    os.execv(str(binary), [str(binary), *sys.argv[1:]])


def grib2avenc(): _run("grib2avenc")
def avenc2grib(): _run("avenc2grib")
def avpack(): _run("avpack")
def compare(): _run("avenc-compare")
def grib_pair_extract(): _run("grib_pair_extract")
def avenc_test_generator(): _run("avenc_test_generator")
def compute_error(): _run("ComputeError")
