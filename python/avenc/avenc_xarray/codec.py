from __future__ import annotations

import json
import base64
import struct
import subprocess
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Tuple

import numpy as np
import xarray as xr

from . import _avenc_ext
from .source import is_http_url
from .lut import apply_distribution, ensure_lut_file, load_lut_bytes, lut_bytes_to_base64

SUPPORTED_BITS = {8, 10, 12, 16}
DEFAULT_VIDEO_NAME = "payload.mp4"
COMMENT_PREFIX = "AVENCv1|"
COMMENT_PREFIX_V2 = "AVENCv2|"
_V2_MAGIC = b"AV2META\0"
_V2_FORMAT_VERSION = 2
_V2_MAX_TABLE_ENTRIES = 10_000_000


@dataclass
class EncodeSummary:
    frames: int
    height: int
    width: int
    video_path: Path
    options: Mapping[str, Any]
    metadata: Mapping[str, Any]


def _prepare_output_dir(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)


def _resolve_video_file(path: str | Path) -> str | Path:
    if is_http_url(path):
        return str(path)
    path = Path(path)
    if path.is_file():
        return path
    candidate = path / DEFAULT_VIDEO_NAME
    if candidate.exists():
        return candidate

    payload_matches = sorted(path.glob("payload.*"))
    for match in payload_matches:
        if match.is_file():
            return match

    for pattern in ("*.mp4", "*.mkv", "*.webm", "*.mov", "*.avi"):
        matches = sorted(path.glob(pattern))
        for match in matches:
            if match.is_file():
                return match

    raise FileNotFoundError(f"No video payload found in {path}")


def _ensure_json_serializable(value: Any) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, (np.datetime64, np.timedelta64)):
        return value.astype("datetime64[ns]").astype("int64")
    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, (list, tuple)):
        return [_ensure_json_serializable(v) for v in value]
    if isinstance(value, dict):
        return {k: _ensure_json_serializable(v) for k, v in value.items()}
    return str(value)


def _json_dumps(payload: Mapping[str, Any]) -> bytes:
    return json.dumps(
        payload,
        default=_ensure_json_serializable,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")


def _json_loads(data: bytes) -> Dict[str, Any]:
    return json.loads(data.decode("utf-8"))


def _encode_array_payload(array: np.ndarray) -> Dict[str, Any]:
    arr = np.asarray(array)
    if arr.dtype.kind == "O":
        values = [_ensure_json_serializable(v) for v in arr.tolist()]
        return {
            "encoding": "python-list",
            "values": values,
            "dtype": str(arr.dtype),
            "shape": list(arr.shape),
        }

    contiguous = np.ascontiguousarray(arr)
    compressed = zlib.compress(contiguous.tobytes(), level=9)
    encoded = base64.b64encode(compressed).decode("ascii")
    return {
        "encoding": "zlib+base64",
        "dtype": str(contiguous.dtype),
        "shape": list(contiguous.shape),
        "data": encoded,
    }


def _encode_bitmask(mask: np.ndarray) -> Dict[str, Any]:
    packed = np.packbits(mask.astype(np.uint8))
    compressed = zlib.compress(packed.tobytes(), level=9)
    encoded = base64.b64encode(compressed).decode("ascii")
    return {
        "encoding": "zlib+base64",
        "shape": list(mask.shape),
        "data": encoded,
    }


def _decode_bitmask(bitmask_info: Mapping[str, Any]) -> np.ndarray:
    if not bitmask_info:
        return np.array([], dtype=bool)
    if bitmask_info.get("encoding") != "zlib+base64":
        raise ValueError("Unsupported bitmask encoding")
    shape = tuple(bitmask_info.get("shape", []))
    if not shape:
        return np.array([], dtype=bool)
    data = base64.b64decode(bitmask_info["data"].encode("ascii"))
    decompressed = zlib.decompress(data)
    total_bits = int(np.prod(shape))
    unpacked = np.unpackbits(np.frombuffer(decompressed, dtype=np.uint8))[:total_bits]
    return unpacked.reshape(shape).astype(bool)


def _decode_array_payload(payload: Mapping[str, Any]) -> np.ndarray:
    encoding = payload.get("encoding", "python-list")
    if encoding == "python-list":
        values = payload.get("values", [])
        dtype = payload.get("dtype", "object")
        shape = tuple(payload.get("shape", [len(values)]))
        target_dtype = np.dtype(dtype)
        array = np.array(values, dtype=target_dtype)
        return array.reshape(shape)
    if encoding == "zlib+base64":
        data_b64 = payload["data"]
        compressed = base64.b64decode(data_b64.encode("ascii"))
        raw = zlib.decompress(compressed)
        dtype = np.dtype(payload["dtype"])
        shape = tuple(payload["shape"])
        array = np.frombuffer(raw, dtype=dtype)
        return array.reshape(shape)
    raise ValueError(f"Unsupported coordinate encoding: {encoding}")


def _encode_metadata_comment(metadata: Mapping[str, Any]) -> str:
    json_bytes = _json_dumps(metadata)
    compressed = zlib.compress(json_bytes, level=9)
    b64 = base64.b64encode(compressed).decode("ascii")
    return f"{COMMENT_PREFIX}{b64}"


def _decode_metadata_comment(comment: str) -> Dict[str, Any]:
    if comment.startswith(COMMENT_PREFIX):
        payload = comment[len(COMMENT_PREFIX) :]
        raw = base64.b64decode(payload.encode("ascii"))
        data = zlib.decompress(raw)
        return _json_loads(data)
    if comment.startswith(COMMENT_PREFIX_V2):
        payload = comment[len(COMMENT_PREFIX_V2) :]
        try:
            compressed = base64.b64decode(payload.encode("ascii"), validate=True)
            data = zlib.decompress(compressed)
        except (ValueError, UnicodeEncodeError, zlib.error) as exc:
            raise ValueError("Invalid AVENCv2 metadata encoding") from exc
        return _decode_metadata_v2_binary(data)
    if comment.strip().startswith("{"):
        return json.loads(comment)
    raise ValueError("Unsupported metadata comment format")


class _V2Reader:
    """Bounds-checked reader for the AVENCv2 little-endian metadata payload."""

    def __init__(self, data: bytes):
        self.data = memoryview(data)
        self.offset = 0

    def _unpack(self, fmt: str, field: str):
        size = struct.calcsize(fmt)
        if self.offset + size > len(self.data):
            raise ValueError(f"Truncated AVENCv2 field: {field}")
        value = struct.unpack_from(fmt, self.data, self.offset)[0]
        self.offset += size
        return value

    def u32(self, field: str) -> int:
        return int(self._unpack("<I", field))

    def i32(self, field: str) -> int:
        return int(self._unpack("<i", field))

    def u64(self, field: str) -> int:
        return int(self._unpack("<Q", field))

    def f64(self, field: str) -> float:
        return float(self._unpack("<d", field))

    def string(self, field: str) -> str:
        size = self.u32(f"{field}.length")
        if self.offset + size > len(self.data):
            raise ValueError(f"Truncated AVENCv2 string: {field}")
        raw = self.data[self.offset : self.offset + size].tobytes()
        self.offset += size
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"Invalid UTF-8 in AVENCv2 string: {field}") from exc


def _decode_metadata_v2_binary(data: bytes) -> Dict[str, Any]:
    """Convert the compact AVENCv2 tables to the v1-compatible metadata model."""

    reader = _V2Reader(data)
    if len(data) < 36 or reader.data[:8].tobytes() != _V2_MAGIC:
        raise ValueError("Invalid AVENCv2 metadata magic")
    reader.offset = 8
    version = reader.u32("format_version")
    if version not in (_V2_FORMAT_VERSION, 3, 4):
        raise ValueError(f"Unsupported AVENCv2 format version: {version}")
    flags = reader.u32("flags")
    width = reader.u32("nx")
    height = reader.u32("ny")
    stream_count = reader.u32("stream_count")
    record_count = reader.u32("record_count")
    reader.u32("reserved")

    if width <= 0 or height <= 0:
        raise ValueError(f"Invalid AVENCv2 grid: {width}x{height}")
    if not 0 < stream_count <= _V2_MAX_TABLE_ENTRIES:
        raise ValueError(f"Invalid AVENCv2 stream count: {stream_count}")
    if not 0 < record_count <= _V2_MAX_TABLE_ENTRIES:
        raise ValueError(f"Invalid AVENCv2 record count: {record_count}")

    streams: Dict[int, Dict[str, Any]] = {}
    for _ in range(stream_count):
        stream_index = reader.i32("stream.index")
        frame_count = reader.u32("stream.frame_count")
        if frame_count <= 0:
            raise ValueError(f"Invalid AVENCv2 frame count for stream {stream_index}: {frame_count}")
        bits = reader.u32("stream.bits")
        storage_shift = reader.i32("stream.storage_shift")
        min_value = reader.f64("stream.min_value")
        max_value = reader.f64("stream.max_value")
        transform = reader.u32("stream.transform")
        if transform > (4 if version == 4 else 2 if version == 3 else 1):
            raise ValueError(f"Unsupported stream transform: {transform}")
        apply_log2 = transform == 1
        apply_asinh = transform in (2, 3)
        log_offset = reader.f64("stream.log_offset")
        transform_scale = reader.f64("stream.transform_scale") if version >= 4 else 1.0
        transform_origin = reader.f64("stream.transform_origin") if version >= 4 else 0.0
        transform_span = reader.f64("stream.transform_span") if version >= 4 else 1.0
        if not (np.isfinite(transform_scale) and transform_scale > 0 and
                np.isfinite(transform_origin) and np.isfinite(transform_span) and transform_span > 0):
            raise ValueError("Invalid AVENC transform scale/origin/span")
        encoded_width = reader.u32("stream.encoded_nx") or width
        encoded_height = reader.u32("stream.encoded_ny") or height
        crop_width = reader.u32("stream.crop_nx") or width
        crop_height = reader.u32("stream.crop_ny") or height
        bitmap_payload = reader.string("stream.bitmap_payload")
        template_b64 = reader.string("stream.template_b64")
        if stream_index in streams:
            raise ValueError(f"Duplicate AVENCv2 stream index: {stream_index}")
        streams[stream_index] = {
            "stream_index": stream_index,
            "frame_count": frame_count,
            "bits": bits,
            "storage_shift": storage_shift,
            "min_value": min_value,
            "max_value": max_value,
            "apply_log2": apply_log2,
            "apply_asinh": apply_asinh,
            "log_offset": log_offset,
            "apply_shifted_log": transform == 4,
            "transform_scale": transform_scale,
            "transform_origin": transform_origin,
            "transform_span": transform_span,
            "encoded_width": encoded_width,
            "encoded_height": encoded_height,
            "crop_width": crop_width,
            "crop_height": crop_height,
            "bitmap_payload": bitmap_payload,
            "template_b64": template_b64,
            "records": [],
        }

    for _ in range(record_count):
        record = {
            "order": reader.u64("record.order"),
            "stream_index": reader.i32("record.stream_index"),
            "frame_index": reader.u32("record.frame_index"),
            "file_index": reader.i32("record.file_index"),
            "message_index": reader.u64("record.message_index"),
            "quantization_min": reader.f64("record.frame_min"),
            "quantization_max": reader.f64("record.frame_max"),
            "bitmapPresent": bool(reader.u32("record.bitmap_present")),
            "bits_per_value": reader.i32("record.bits_per_value"),
            "binary_scale_shift": reader.i32("record.binary_scale_shift"),
            "short_name": reader.string("record.short_name"),
            "type_of_level": reader.string("record.type_of_level"),
            "level": reader.string("record.level"),
            "data_date": reader.string("record.data_date"),
            "data_time": reader.string("record.data_time"),
            "step_range": reader.string("record.step_range"),
            "timestamp": reader.string("record.timestamp"),
            "ensemble_number": reader.string("record.ensemble_number"),
            "perturbation_number": reader.string("record.perturbation_number"),
            "template_b64": reader.string("record.template_b64"),
        }
        stream_index = record["stream_index"]
        if stream_index not in streams:
            raise ValueError(f"AVENCv2 record references unknown stream: {stream_index}")
        streams[stream_index]["records"].append(record)

    coords = {}
    if flags & 1:
        latitude_count = reader.u32("latitude_count")
        if latitude_count != height:
            raise ValueError(
                f"AVENCv2 latitude count {latitude_count} does not match height {height}"
            )
        latitudes = [reader.f64("latitude") for _ in range(latitude_count)]
        longitude_count = reader.u32("longitude_count")
        if longitude_count != width:
            raise ValueError(
                f"AVENCv2 longitude count {longitude_count} does not match width {width}"
            )
        longitudes = [reader.f64("longitude") for _ in range(longitude_count)]
        coords = {
            "lat": {
                "data": {
                    "encoding": "python-list",
                    "values": latitudes,
                    "dtype": "float64",
                    "shape": [latitude_count],
                },
                "attrs": {"units": "degrees_north"},
            },
            "lon": {
                "data": {
                    "encoding": "python-list",
                    "values": longitudes,
                    "dtype": "float64",
                    "shape": [longitude_count],
                },
                "attrs": {"units": "degrees_east"},
            },
        }

    groups = []
    for stream_index, stream in sorted(streams.items()):
        records = sorted(stream["records"], key=lambda item: item["frame_index"])
        if len(records) != stream["frame_count"]:
            raise ValueError(
                f"AVENCv2 stream {stream_index} declares {stream['frame_count']} frames "
                f"but contains {len(records)} records"
            )
        if any(item["frame_index"] != index for index, item in enumerate(records)):
            raise ValueError(f"AVENCv2 stream {stream_index} frame indices are not contiguous")

        first = records[0]
        short_name = first["short_name"] or f"stream_{stream_index}"
        type_of_level = first["type_of_level"]
        level = first["level"]
        time_dim = f"time_g{stream_index}"
        time_values = [item["timestamp"] for item in records]
        group = {
            "id": stream_index,
            "name": "_".join(part for part in (short_name, type_of_level, level) if part),
            "short_name": short_name,
            "type_of_level": type_of_level,
            "level": level,
            "stream_index": stream_index,
            "frame_start": 0,
            "frame_count": stream["frame_count"],
            "value_min": stream["min_value"],
            "value_max": stream["max_value"],
            "dims": [time_dim, "lat", "lon"],
            "coords": {
                time_dim: {
                    "data": {
                        "encoding": "python-list",
                        "values": time_values,
                        "dtype": "<U32",
                        "shape": [len(time_values)],
                    },
                    "attrs": {"standard_name": "time"},
                }
            },
            "options": {
                "bits": stream["bits"],
                "storage_shift": stream["storage_shift"],
                "min_value": stream["min_value"],
                "max_value": stream["max_value"],
                "apply_log2": stream["apply_log2"],
                "apply_asinh": stream["apply_asinh"],
                "log_offset": stream["log_offset"],
                "apply_shifted_log": stream["apply_shifted_log"],
                "transform_scale": stream["transform_scale"],
                "transform_origin": stream["transform_origin"],
                "transform_span": stream["transform_span"],
                "encoded_width": stream["encoded_width"],
                "encoded_height": stream["encoded_height"],
                "crop_width": stream["crop_width"],
                "crop_height": stream["crop_height"],
                "allow_nan": True,
            },
            "records": records,
        }
        if stream["bitmap_payload"]:
            group["options"]["shared_bitmap"] = {
                "encoding": "zlib+base64",
                "dtype": "uint8",
                "shape": [width * height],
                "data": stream["bitmap_payload"],
            }
        groups.append(group)

    return {
        "schema": "avenc.pack.v1",
        "metadata_version": 2,
        "binary_format_version": version,
        "metadata_format": "binary",
        "metadata_flags": flags,
        "frames": sum(stream["frame_count"] for stream in streams.values()),
        "height": height,
        "width": width,
        "shape": [sum(stream["frame_count"] for stream in streams.values()), height, width],
        "dtype": "float32",
        "coords": coords,
        "groups": groups,
    }


def _probe_comment(video_path: Path) -> str:
    cmd = [
        "ffprobe",
        "-loglevel",
        "error",
        "-show_entries",
        "stream_tags=comment:format_tags=comment",
        "-of",
        "json",
        str(video_path),
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, check=True, text=True)
    except FileNotFoundError as exc:  # pragma: no cover - runtime dependency
        raise RuntimeError("ffprobe executable not found in PATH") from exc
    except subprocess.CalledProcessError as exc:  # pragma: no cover - runtime dependency
        raise RuntimeError(f"ffprobe failed: {exc.stderr.strip()}") from exc
    payload = json.loads(result.stdout)

    for stream in payload.get("streams", []):
        tags = stream.get("tags", {})
        comment = tags.get("comment") or tags.get("COMMENT") or tags.get("Comment")
        if comment:
            return comment

    fmt_tags = payload.get("format", {}).get("tags", {})
    comment = fmt_tags.get("comment") or fmt_tags.get("COMMENT") or fmt_tags.get("Comment")
    if comment:
        return comment

    raise ValueError(f"No comment metadata embedded in {video_path}")


def _validate_dataarray(dataarray: xr.DataArray) -> xr.DataArray:
    if dataarray.ndim != 3:
        raise ValueError("AVENC encoder expects a 3D DataArray (time, y, x)")
    if "time" not in dataarray.dims:
        raise ValueError("First dimension must be named 'time'")
    order = list(dataarray.dims)
    if order[0] != "time":
        dataarray = dataarray.transpose("time", ...)
    return dataarray


def _infer_nan_handling(data: np.ndarray, allow_nan: bool | None) -> bool:
    if allow_nan is not None:
        return allow_nan
    return np.isnan(data).any()


def compress_dataarray(
    dataarray: xr.DataArray,
    output_dir: str | Path,
    *,
    bits: int = 12,
    codec: str = "hevc",
    quality: str = "high",
    encoder: str | None = None,
    apply_log2: bool = False,
    allow_nan: bool | None = None,
    frame_rate: int = 24,
    flip_ud: bool = False,
    min_value: float | None = None,
    max_value: float | None = None,
    video_name: str | None = None,
    preview_lut: str | Path | None = None,
    preview_codec: str = "libvpx-vp9",
    preview_output: str | None = None,
    preview_crf: int = 10,
    preview_alpha_threshold: int = 10,
    color_encode: bool = False,
    color_lut: str | Path | None = None,
    color_distribution: str = "linear",
    bitmask_value: float | None = 0.0,
    bitmask_enabled: bool = True,
) -> EncodeSummary:
    """Compress a DataArray into an AVENC package on disk."""

    if bits not in SUPPORTED_BITS:
        raise ValueError(f"bits must be one of {sorted(SUPPORTED_BITS)}")

    da = _validate_dataarray(dataarray)
    arr = np.asarray(da.transpose("time", da.dims[1], da.dims[2]).values, dtype=np.float32, order="C")

    allow_nan_final = _infer_nan_handling(arr, allow_nan)
    finite_mask = np.isfinite(arr)
    if not finite_mask.any():
        raise ValueError("Input array does not contain any finite values")
    if min_value is None:
        if allow_nan_final:
            min_value = float(np.nanmin(arr))
        else:
            min_value = float(arr[finite_mask].min())
    if max_value is None:
        if allow_nan_final:
            max_value = float(np.nanmax(arr))
        else:
            max_value = float(arr[finite_mask].max())

    if max_value <= min_value:
        raise ValueError("max_value must be greater than min_value")

    frames, height, width = arr.shape
    out_dir = Path(output_dir)
    _prepare_output_dir(out_dir)

    target_video_name = video_name or DEFAULT_VIDEO_NAME
    video_path = out_dir / target_video_name

    preview_info: Optional[Dict[str, Any]] = None
    preview_lut_path: Optional[Path] = None
    if preview_lut is not None:
        preview_output_name = preview_output or "payload_preview.webm"
        _prepare_output_dir(out_dir)
        preview_lut_path, lut_name = ensure_lut_file(
            preview_lut,
            out_dir,
            distribution=color_distribution,
        )
        lut_bytes = load_lut_bytes(preview_lut_path)
        preview_info = {
            "video": preview_output_name,
            "codec": preview_codec,
            "crf": preview_crf,
            "alpha_threshold": preview_alpha_threshold,
            "lut_name": lut_name,
            "lut_base64": lut_bytes_to_base64(lut_bytes),
            "distribution": color_distribution,
        }

    color_mapping_info: Optional[Dict[str, Any]] = None
    if color_encode:
        mapping_bytes, mapping_mode = _create_color_mapping(bits, color_lut, color_distribution)
        color_mapping_info = {
            "mode": mapping_mode,
            "mapping": base64.b64encode(mapping_bytes).decode("ascii"),
            "bits": bits,
            "distribution": color_distribution,
            "lut": color_lut or "linear",
            "storage": "luma",
        }

    bitmask_info = None
    if bitmask_enabled and bitmask_value is not None:
        mask = np.isclose(arr, bitmask_value, equal_nan=False)
        bitmask_info = _encode_bitmask(mask)

    encoder_options: Dict[str, Any] = {
        "bits": bits,
        "min_value": float(min_value),
        "max_value": float(max_value),
        "apply_log2": bool(apply_log2),
        "allow_nan": bool(allow_nan_final),
        "frame_rate": int(frame_rate),
        "flip_ud": bool(flip_ud),
        "codec": codec,
        "quality": quality,
        "video_name": target_video_name,
    }
    if encoder is not None:
        encoder_options["encoder"] = encoder
    coords = {}
    for dim in da.dims:
        coord = da.coords[dim]
        coords[dim] = {
            "data": _encode_array_payload(coord.values),
            "attrs": {k: _ensure_json_serializable(v) for k, v in coord.attrs.items()},
        }

    projection_meta: Dict[str, Any] = {}
    for dim in da.dims:
        coord = da.coords.get(dim)
        if coord is None:
            continue
        values = np.asarray(coord.values)
        if values.ndim != 1 or values.size == 0:
            continue
        proj_entry: Dict[str, Any] = {
            "min": _ensure_json_serializable(np.nanmin(values)),
            "max": _ensure_json_serializable(np.nanmax(values)),
            "count": int(values.size),
        }
        diffs = np.diff(values)
        if diffs.size > 0:
            proj_entry["step"] = _ensure_json_serializable(np.nanmedian(diffs))
        units = coord.attrs.get("units")
        if units:
            proj_entry["units"] = units
        projection_meta[dim] = proj_entry

    metadata: Dict[str, Any] = {
        "schema": "avenc.v1",
        "frames": frames,
        "height": height,
        "width": width,
        "shape": [frames, height, width],
        "dtype": "float32",
        "variable": {
            "name": da.name or "data",
            "dims": list(da.dims),
            "attrs": {k: _ensure_json_serializable(v) for k, v in da.attrs.items()},
        },
        "coords": coords,
        "options": encoder_options,
    }
    if preview_info is not None:
        metadata["preview"] = preview_info
    if color_mapping_info is not None:
        metadata["color_mapping"] = color_mapping_info
    if bitmask_info is not None:
        metadata["bitmask"] = {
            "value": bitmask_value,
            **bitmask_info,
        }
    if projection_meta:
        metadata["projection"] = projection_meta

    comment = _encode_metadata_comment(metadata)
    encoder_options_with_comment = dict(encoder_options)
    encoder_options_with_comment["comment_metadata"] = comment

    _avenc_ext.encode(arr, str(video_path), encoder_options_with_comment)

    if preview_info is not None and preview_lut_path is not None:
        preview_path = out_dir / preview_info["video"]
        _generate_preview_video(
            input_path=video_path,
            output_path=preview_path,
            lut_path=preview_lut_path,
            frame_rate=frame_rate,
            codec=preview_info["codec"],
            crf=preview_info["crf"],
            alpha_threshold=preview_info["alpha_threshold"],
            distribution=preview_info.get("distribution"),
        )

    return EncodeSummary(
        frames=frames,
        height=height,
        width=width,
        video_path=video_path,
        options=encoder_options,
        metadata=metadata,
    )


def _generate_preview_video(
    *,
    input_path: Path,
    output_path: Path,
    lut_path: Path,
    frame_rate: int,
    codec: str,
    crf: int,
    alpha_threshold: int,
    distribution: str | None = None,
) -> None:
    """Generate a colorized preview video using a LUT."""

    _ = distribution  # Reserved for future use in filter parametrisation.
    lut_arg = str(lut_path).replace("'", "\\'")
    alpha_expr = f"if(gt(val,{alpha_threshold}),255,0)"
    filter_complex = (
        "[0:v]transpose=2,format=gray16le,split=2[luma16][alpha_src];"
        f"[alpha_src]format=gray,lut='{alpha_expr}'[alpha];"
        f"[luma16]format=gbrp16le,lut3d='{lut_arg}'[color16];"
        "[color16][alpha]alphamerge,format=yuva420p[out]"
    )

    cmd = [
        "ffmpeg",
        "-y",
        "-loglevel",
        "error",
        "-i",
        str(input_path),
        "-filter_complex",
        filter_complex,
        "-map",
        "[out]",
        "-crf",
        str(crf),
        "-c:v",
        codec,
        "-auto-alt-ref",
        "0",
        str(output_path),
    ]
    subprocess.run(cmd, check=True)


def _create_color_mapping(bits: int, color_lut: str | Path | None, distribution: str) -> Tuple[bytes, str]:
    max_int = (1 << bits) - 1
    if max_int <= 0:
        raise ValueError("Invalid bit depth for color mapping")
    values = np.linspace(0.0, 1.0, max_int + 1, dtype=np.float64)
    values = np.clip(apply_distribution(values, distribution), 0.0, 1.0)
    if color_lut:
        from matplotlib import cm

        cmap = cm.get_cmap(str(color_lut))
        colors = np.clip(np.round(cmap(values)[:, :3] * 255.0), 0, 255).astype(np.uint8)
        mapping = colors
        mode = f"cmap:{color_lut}:{distribution}"
    else:
        encoded = np.clip(np.round(values * 0xFFFFFF), 0, 0xFFFFFF).astype(np.uint32)
        r = ((encoded >> 16) & 0xFF).astype(np.uint8)
        g = ((encoded >> 8) & 0xFF).astype(np.uint8)
        b = (encoded & 0xFF).astype(np.uint8)
        mapping = np.stack([r, g, b], axis=1).astype(np.uint8)
        mode = f"linear24:{distribution}"
    return mapping.tobytes(), mode


def decompress_to_dataarray(package_dir: str | Path) -> xr.DataArray:
    """Reconstruct an xarray.DataArray from an AVENC package."""

    path = Path(package_dir)
    video_file = _resolve_video_file(path)

    comment = _probe_comment(video_file)
    metadata = _decode_metadata_comment(comment)
    if metadata.get("schema") not in {"avenc.v1", "avenc"}:
        raise ValueError("Unsupported AVENC metadata schema")

    frames = int(metadata["frames"])
    height = int(metadata["height"])
    width = int(metadata["width"])
    options = metadata.get("options", {})

    color_mapping = metadata.get("color_mapping")
    if color_mapping is not None:
        storage_mode = color_mapping.get("storage", "legacy")
        mapping_payload = color_mapping.get("mapping")
        if storage_mode in {"legacy", "rgb24"} and mapping_payload:
            options = dict(options)
            options.setdefault("color_mapping", mapping_payload)
            if "pix_fmt" not in options:
                options["pix_fmt"] = "rgb24"

    array = _avenc_ext.decode(
        str(video_file),
        frames,
        height,
        width,
        options,
    )

    variable = metadata.get("variable", {})
    dims = variable.get("dims", ["time", "y", "x"])
    name = variable.get("name") or "data"

    data = xr.DataArray(array, dims=dims, name=name)
    data.attrs.update(variable.get("attrs", {}))

    coords_payload = metadata.get("coords", {})
    for dim, info in coords_payload.items():
        payload = info.get("data")
        if payload is None:
            continue
        values = _decode_array_payload(payload)
        data = data.assign_coords({dim: values})
        data.coords[dim].attrs.update(info.get("attrs", {}))

    bitmask_info = metadata.get("bitmask")
    if bitmask_info:
        mask = _decode_bitmask(bitmask_info)
        data = data.copy()
        data.values[mask] = np.nan

    return data
