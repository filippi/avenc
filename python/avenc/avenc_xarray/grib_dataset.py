from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Tuple

import numpy as np

from .source import is_http_url
from .codec import _decode_array_payload, _decode_metadata_comment, _probe_comment, _resolve_video_file


@dataclass(frozen=True)
class AvencVariable:
    """Logical variable mapping for one encoded video stream."""

    name: str
    stream_id: int
    frame_count: int
    width: int
    height: int
    time_dim: str
    time_values: np.ndarray
    group_meta: Mapping[str, Any]


class AvencDataset:
    """Lightweight AVENC metadata wrapper with lazy chunk bridge to `avenc2grib`.

    The class loads metadata without decoding payload frames and exposes stream-to-variable
    mappings, coordinate vectors and subset decoding for requested chunks.
    """

    def __init__(
        self,
        filename_or_obj: str | Path,
        *,
        avenc2grib_bin: str | Path | None = None,
        metadata: Optional[Mapping[str, Any]] = None,
        video_path: str | Path | None = None,
    ) -> None:
        self._input_path = str(filename_or_obj) if is_http_url(filename_or_obj) else Path(filename_or_obj)
        self._sidecar_path: Optional[Path] = None

        if metadata is None:
            self.video_path, self.metadata = self._load_metadata(self._input_path)
        else:
            self.metadata = dict(metadata)
            if video_path is not None:
                self.video_path = str(video_path) if is_http_url(video_path) else Path(video_path)
            elif is_http_url(self._input_path):
                self.video_path = str(self._input_path)
            else:
                self.video_path = self._infer_video_from_sidecar(self._input_path, self.metadata)

        schema = self.metadata.get("schema")
        if schema != "avenc.pack.v1":
            raise ValueError(f"Unsupported metadata schema for AvencDataset: {schema!r}")

        self.avenc2grib_bin = self._resolve_avenc2grib_bin(avenc2grib_bin)
        self.width = int(self.metadata.get("width", 0))
        self.height = int(self.metadata.get("height", 0))
        if self.width <= 0 or self.height <= 0:
            shape = self.metadata.get("shape", [])
            if len(shape) >= 3:
                self.height = int(shape[1])
                self.width = int(shape[2])
        if self.width <= 0 or self.height <= 0:
            raise ValueError("Invalid metadata width/height")

        self.latitudes, self.longitudes = self._build_coordinates(self.metadata)
        self.variables = self._build_variables(self.metadata, self.width, self.height)
        if not self.variables:
            raise ValueError("No variables found in AVENC metadata")

    @staticmethod
    def _resolve_avenc2grib_bin(avenc2grib_bin: str | Path | None) -> str:
        if avenc2grib_bin is not None:
            return str(avenc2grib_bin)

        bundled = Path(__file__).resolve().parent.parent / "avenc" / "_native" / "bin" / "avenc2grib"
        if bundled.is_file():
            return str(bundled)

        repo_root = Path(__file__).resolve().parents[3]
        local_bin = repo_root / "bin" / "avenc2grib"
        if local_bin.exists():
            return str(local_bin)

        found = shutil.which("avenc2grib")
        if found:
            return found

        raise RuntimeError(
            "Unable to locate avenc2grib binary. Set backend_kwargs={'avenc2grib_bin': '/path/to/avenc2grib'}."
        )

    @staticmethod
    def _load_metadata(path: str | Path) -> Tuple[str | Path, Dict[str, Any]]:
        if not is_http_url(path) and path.suffix.lower() == ".json":
            sidecar_path = path
            with sidecar_path.open("r", encoding="utf-8") as fh:
                metadata = json.load(fh)
            video_path = AvencDataset._infer_video_from_sidecar(sidecar_path, metadata)
            return video_path, metadata

        video_path = _resolve_video_file(path)
        comment = _probe_comment(video_path)
        metadata = _decode_metadata_comment(comment)
        return video_path, metadata

    @staticmethod
    def _infer_video_from_sidecar(sidecar_path: Path, metadata: Mapping[str, Any]) -> Path:
        for key in ("video_path", "video", "input_video", "source_video", "payload"):
            value = metadata.get(key)
            if isinstance(value, str) and value:
                candidate = Path(value)
                if not candidate.is_absolute():
                    candidate = (sidecar_path.parent / candidate).resolve()
                if candidate.exists():
                    return candidate

        if sidecar_path.name.lower().endswith(".mp4.json"):
            candidate = sidecar_path.with_suffix("")
            if candidate.exists():
                return candidate

        stem = sidecar_path.stem
        for ext in (".mp4", ".mkv", ".webm", ".mov", ".avi"):
            candidate = sidecar_path.with_name(stem + ext)
            if candidate.exists():
                return candidate

        payload = sidecar_path.parent / "payload.mp4"
        if payload.exists():
            return payload

        raise FileNotFoundError(f"Unable to infer video path for sidecar: {sidecar_path}")

    @staticmethod
    def _decode_coord_array(info: Optional[Mapping[str, Any]]) -> Optional[np.ndarray]:
        if not info:
            return None
        payload = info.get("data")
        if not isinstance(payload, Mapping):
            return None
        try:
            values = _decode_array_payload(payload)
        except Exception:
            return None
        return np.asarray(values)

    @classmethod
    def _build_coordinates(cls, metadata: Mapping[str, Any]) -> Tuple[np.ndarray, np.ndarray]:
        coords = metadata.get("coords", {})
        height = int(metadata.get("height", 0))
        width = int(metadata.get("width", 0))

        lat = cls._decode_coord_array(coords.get("lat"))
        lon = cls._decode_coord_array(coords.get("lon"))

        if lat is None or lat.size != height:
            lat = np.arange(height, dtype=np.float32)
        else:
            lat = lat.astype(np.float32, copy=False).reshape(height)

        if lon is None or lon.size != width:
            lon = np.arange(width, dtype=np.float32)
        else:
            lon = lon.astype(np.float32, copy=False).reshape(width)

        return lat, lon

    @classmethod
    def _build_variables(
        cls,
        metadata: Mapping[str, Any],
        width: int,
        height: int,
    ) -> Dict[str, AvencVariable]:
        variables: Dict[str, AvencVariable] = {}
        name_counts: Dict[str, int] = {}

        groups = metadata.get("groups", [])
        for gi, group in enumerate(groups):
            stream_id = int(group.get("stream_index", gi))
            frame_count = int(group.get("frame_count", 0))
            if frame_count <= 0:
                continue

            short_name = str(group.get("short_name", "")).strip()
            label_name = str(group.get("name", "")).strip()
            base_name = short_name or label_name or f"stream_{stream_id}"

            count = name_counts.get(base_name, 0)
            name_counts[base_name] = count + 1
            name = base_name if count == 0 else f"{base_name}_s{stream_id}"

            dims = group.get("dims", [])
            time_dim = str(dims[0]) if dims else f"time_{name}"

            time_values = None
            group_coords = group.get("coords", {})
            if isinstance(group_coords, Mapping) and time_dim in group_coords:
                time_values = cls._decode_coord_array(group_coords.get(time_dim))

            if time_values is None or int(time_values.size) != frame_count:
                time_values = np.arange(frame_count, dtype=np.int64)
            else:
                time_values = np.asarray(time_values)

            variables[name] = AvencVariable(
                name=name,
                stream_id=stream_id,
                frame_count=frame_count,
                width=width,
                height=height,
                time_dim=time_dim,
                time_values=time_values,
                group_meta=group,
            )

        return variables

    @property
    def variable_names(self) -> Tuple[str, ...]:
        return tuple(self.variables.keys())

    def get_variable(self, var_name: str) -> AvencVariable:
        try:
            return self.variables[var_name]
        except KeyError as exc:
            available = ", ".join(self.variable_names)
            raise KeyError(f"Unknown variable {var_name!r}. Available: {available}") from exc

    @staticmethod
    def _normalize_slice(value: slice | int | None, size: int, axis_name: str) -> Tuple[int, int]:
        if isinstance(value, (int, np.integer)):
            idx = int(value)
            if idx < 0:
                idx += size
            if idx < 0 or idx >= size:
                raise IndexError(f"{axis_name} index out of bounds: {value}")
            return idx, idx + 1

        if value is None:
            return 0, size

        if not isinstance(value, slice):
            raise TypeError(f"Unsupported index type on {axis_name}: {type(value)!r}")

        step = 1 if value.step is None else int(value.step)
        if step != 1:
            raise NotImplementedError(f"{axis_name} slice step {step} is not supported")

        start = 0 if value.start is None else int(value.start)
        stop = size if value.stop is None else int(value.stop)
        if start < 0:
            start += size
        if stop < 0:
            stop += size
        start = max(0, min(size, start))
        stop = max(0, min(size, stop))
        if stop < start:
            raise IndexError(f"{axis_name} slice stop < start")
        return start, stop

    def build_chunk_command(
        self,
        *,
        stream_id: int,
        ni_start: int,
        nj_start: int,
        width: int,
        height: int,
        frame_start: int,
        frame_count: int,
    ) -> list[str]:
        return [
            self.avenc2grib_bin,
            "--stream-id",
            str(stream_id),
            "--ni-start",
            str(ni_start),
            "--nj-start",
            str(nj_start),
            "--width",
            str(width),
            "--height",
            str(height),
            "--frame-start",
            str(frame_start),
            "--frame-count",
            str(frame_count),
            str(self.video_path),
        ]

    def _get_chunk(
        self,
        var_name: str,
        time_slice: slice | int | None,
        lat_slice: slice | int | None,
        lon_slice: slice | int | None,
    ) -> np.ndarray:
        """Decode one chunk as float32 with layout (time, lat, lon)."""

        var = self.get_variable(var_name)
        t0, t1 = self._normalize_slice(time_slice, var.frame_count, "time")
        j0, j1 = self._normalize_slice(lat_slice, var.height, "lat")
        i0, i1 = self._normalize_slice(lon_slice, var.width, "lon")

        out_t = t1 - t0
        out_h = j1 - j0
        out_w = i1 - i0
        if out_t <= 0 or out_h <= 0 or out_w <= 0:
            return np.empty((max(0, out_t), max(0, out_h), max(0, out_w)), dtype=np.float32)

        cmd = self.build_chunk_command(
            stream_id=var.stream_id,
            ni_start=i0,
            nj_start=j0,
            width=out_w,
            height=out_h,
            frame_start=t0,
            frame_count=out_t,
        )
        env = os.environ.copy()
        if str(self.video_path).startswith("http://127.0.0.1:"):
            env["AVENC_RESOLVED_HTTP_INPUT"] = str(self.video_path)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, env=env)
        if proc.returncode != 0:
            stderr = proc.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"avenc2grib subset decode failed (code={proc.returncode}): {stderr}")

        expected = out_t * out_h * out_w
        arr = np.frombuffer(proc.stdout, dtype="<f4")
        if arr.size != expected:
            raise RuntimeError(
                f"Unexpected subset byte count from avenc2grib: got {arr.size} float32 values, expected {expected}"
            )
        return arr.reshape(out_t, out_h, out_w)

    def write_sidecar(self, output_json: str | Path, *, include_video_path: bool = True) -> Path:
        """Write metadata as JSON sidecar (optionally adding `video_path`)."""

        dst = Path(output_json)
        payload = dict(self.metadata)
        if include_video_path:
            payload["video_path"] = str(self.video_path)
        with dst.open("w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=True, indent=2, sort_keys=True)
            fh.write("\n")
        return dst

