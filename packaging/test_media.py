"""Verify CI's external media tools; never download them into an AVENC wheel."""
import shutil
import subprocess
for name in ("ffmpeg", "ffprobe"):
    if not shutil.which(name):
        raise SystemExit(f"CI prerequisite missing: {name}; see packaging/bootstrap_media.py")
    subprocess.run([name, "-version"], check=True, stdout=subprocess.DEVNULL)
