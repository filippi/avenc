"""Utilities for handling color lookup tables (3D LUTs)."""

from __future__ import annotations

import base64
import math
import re
from pathlib import Path
from typing import Tuple

import numpy as np


def _import_matplotlib_pyplot():
    """Import matplotlib lazily for optional LUT generation paths."""

    try:
        import matplotlib.pyplot as plt  # type: ignore import-not-found
    except Exception as exc:  # pragma: no cover - runtime dependency
        raise RuntimeError(
            "matplotlib is required only for builtin LUT generation "
            "(e.g. ensure_lut_file with a colormap name)."
        ) from exc
    return plt


def apply_distribution(values: np.ndarray, mode: str | None) -> np.ndarray:
    """Transform ``values`` in-place according to the named distribution."""

    mode_normalized = (mode or "linear").lower()
    if mode_normalized == "log":
        return np.log1p(values * 9.0) / math.log(10.0)
    if mode_normalized in {"sqrt", "square_root"}:
        return np.sqrt(values)
    if mode_normalized == "cuberoot":
        return np.cbrt(values)
    if mode_normalized.startswith("weibull"):
        parts = mode_normalized.split(":", 1)
        k = float(parts[1]) if len(parts) == 2 else 1.5
        return 1.0 - np.exp(-(values ** k))
    return values


def generate_builtin_lut(
    name: str,
    path: Path,
    *,
    size: int = 17,
    distribution: str | None = None,
) -> Path:
    """Generate a 3D LUT cube file for a named colormap.

    Parameters
    ----------
    name : str
        Matplotlib colormap name (e.g. ``"turbo"``).
    path : Path
        Destination file (.cube).
    size : int
        Dimension per channel (typical values 17 or 33).
    """

    plt = _import_matplotlib_pyplot()
    cmap = plt.get_cmap(name)
    with path.open("w", encoding="utf-8") as f:
        f.write(f"TITLE \"{name}\"\n")
        f.write(f"LUT_3D_SIZE {size}\n")
        f.write("DOMAIN_MIN 0.0 0.0 0.0\n")
        f.write("DOMAIN_MAX 1.0 1.0 1.0\n")
        rng = np.linspace(0.0, 1.0, size)
        for b in rng:
            for g in rng:
                for r in rng:
                    val = (r + g + b) / 3.0
                    val = float(apply_distribution(np.array([val]), distribution)[0])
                    color = cmap(val)
                    f.write(f"{color[0]:.6f} {color[1]:.6f} {color[2]:.6f}\n")
    return path


def load_lut_bytes(path: Path) -> bytes:
    return path.read_bytes()


def lut_bytes_to_base64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def base64_to_lut_bytes(data_b64: str) -> bytes:
    return base64.b64decode(data_b64.encode("ascii"))


def _sanitize_name_fragment(text: str) -> str:
    return re.sub(r"[^0-9A-Za-z._-]+", "_", text)


def ensure_lut_file(
    lut: str | Path,
    workdir: Path,
    default_size: int = 17,
    distribution: str | None = None,
) -> Tuple[Path, str]:
    """Return a cube file path and the LUT name.

    ``lut`` may be a file path or a builtin colormap name.
    """

    path = Path(lut)
    if path.exists():
        return path, path.stem

    name = str(lut)
    dist_label = "" if not distribution or distribution.lower() == "linear" else f"_{_sanitize_name_fragment(distribution)}"
    out_path = workdir / f"{_sanitize_name_fragment(name)}{dist_label}_{default_size}.cube"
    if not out_path.exists():
        generate_builtin_lut(name, out_path, size=default_size, distribution=distribution)
    return out_path, name
