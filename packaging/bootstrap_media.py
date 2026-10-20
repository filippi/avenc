"""Install test-only FFmpeg inside manylinux, verified against conda metadata.

Only CI calls this script. No FFmpeg binary enters a wheel or native archive.
"""
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen

arch = "linux-aarch64" if platform.machine() == "aarch64" else "linux-64"
with urlopen("https://api.anaconda.org/package/conda-forge/micromamba", timeout=60) as response:
    metadata = json.load(response)
candidates = [f for f in metadata["files"] if f["version"] == "2.3.3" and f["basename"].startswith(arch + "/") and f["basename"].endswith(".tar.bz2")]
entry = sorted(candidates, key=lambda f:f["basename"])[-1]
url = "https:" + entry["download_url"] if entry["download_url"].startswith("//") else entry["download_url"]
with tempfile.TemporaryDirectory() as tmp:
    archive = Path(tmp)/"micromamba.tar.bz2"
    with urlopen(url, timeout=60) as response: archive.write_bytes(response.read())
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == entry["sha256"]
    with tarfile.open(archive) as package:
        member = package.getmember("bin/micromamba")
        member.name = "micromamba"
        package.extract(member, tmp, filter="data")
    subprocess.run([str(Path(tmp)/"micromamba"), "create", "-y", "-p", "/opt/avenc-ci-media", "-c", "conda-forge", "--override-channels", "ffmpeg=8.0.1"], check=True)
