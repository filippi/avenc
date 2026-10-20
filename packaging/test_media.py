"""Verify CI's external media tools; never download them into an AVENC wheel."""
import shutil
import subprocess
for name in ("ffmpeg", "ffprobe"):
    if not shutil.which(name):
        raise SystemExit(f"CI prerequisite missing: {name}; see packaging/bootstrap_media.py")
    subprocess.run([name, "-version"], check=True, stdout=subprocess.DEVNULL)

encoders = subprocess.check_output(["ffmpeg", "-hide_banner", "-encoders"], text=True, stderr=subprocess.STDOUT)
for encoder in ("libaom-av1", "libx264"):
    if encoder not in encoders:
        raise SystemExit(f"FFmpeg lacks {encoder}; on macOS use ffmpeg-full and put its bin directory on PATH")
