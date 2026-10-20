"""Catalog of AVENC encoding presets used by examples and tests."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional


@dataclass(frozen=True)
class AvencPreset:
    """Named configuration for :func:`compress_dataarray`.

    Parameters mirror the arguments accepted by the encoder. ``options``
    can hold additional keyword arguments (e.g. ``apply_log2``).
    """

    label: str
    codec: str
    quality: str
    bits: int
    encoder: Optional[str] = None
    description: str = ""
    options: Dict[str, Any] = field(default_factory=dict)


PRESET_CATALOG: List[AvencPreset] = [
    AvencPreset(
        label="avc_avg_8",
        codec="avc",
        quality="average",
        bits=8,
        description="H.264/AVC average-quality baseline at 8 bits.",
    ),
    AvencPreset(
        label="avc_high_8",
        codec="avc",
        quality="high",
        bits=8,
        description="H.264/AVC high-quality 8-bit (tighter GOP, B-frames).",
    ),
    AvencPreset(
        label="avc_lossless_8",
        codec="avc",
        quality="lossless",
        bits=8,
        description="H.264 lossless mode (useful for reference comparisons).",
    ),
    AvencPreset(
        label="avc_hw_8",
        codec="avc",
        quality="compatible",
        bits=8,
        encoder="h264_videotoolbox",
        description="Hardware-accelerated AVC encode via VideoToolbox (macOS).",
    ),
    AvencPreset(
        label="avc_hw_colorized_8",
        codec="avc",
        quality="compatible",
        bits=8,
        encoder="h264_videotoolbox",
        description="VideoToolbox H.264 with 24-bit LUT colorization for quick previews.",
        options={
            "color_encode": True,
            "color_distribution": "log",
        },
    ),
    AvencPreset(
        label="hevc_hw_10",
        codec="hevc",
        quality="average",
        bits=10,
        encoder="hevc_videotoolbox",
        description="Hardware HEVC 10-bit encode via VideoToolbox (macOS).",
    ),
    AvencPreset(
        label="hevc_hw_colorized_10",
        codec="hevc",
        quality="average",
        bits=10,
        encoder="hevc_videotoolbox",
        description="VideoToolbox HEVC 10-bit with LUT-based color mapping.",
        options={
            "color_encode": True,
            "color_distribution": "log",
        },
    ),
    AvencPreset(
        label="hevc_avg_10",
        codec="hevc",
        quality="average",
        bits=10,
        description="HEVC 10-bit average quality (good trade-off).",
    ),
    AvencPreset(
        label="hevc_high_12",
        codec="hevc",
        quality="high",
        bits=12,
        description="HEVC 12-bit high quality for scientific fields.",
    ),
    AvencPreset(
        label="hevc_multiset_12",
        codec="hevc",
        quality="multiset",
        bits=12,
        description="HEVC 12-bit with short GOP (good for temporal scrubbing).",
    ),
    AvencPreset(
        label="hevc_lossless_12",
        codec="hevc",
        quality="lossless",
        bits=12,
        description="HEVC near-lossless/visually lossless 12-bit output.",
    ),
    AvencPreset(
        label="av1_high_10",
        codec="av1",
        quality="high",
        bits=10,
        description="AV1 10-bit high-quality encode (CPU intensive).",
    ),
    AvencPreset(
        label="av1_avg_12",
        codec="av1",
        quality="average",
        bits=12,
        description="AV1 12-bit average quality (balances ratio vs. cost).",
    ),
    AvencPreset(
        label="vp9_colorized_8",
        codec="avc",
        quality="average",
        bits=8,
        description="AVC stream using 24-bit color mapping (8-bit scalar).",
        options={
            "color_encode": True,
            "color_distribution": "log",
        },
    ),
    AvencPreset(
        label="av1_colorized_10",
        codec="av1",
        quality="high",
        bits=10,
        description="AV1 stream with color cube quantization (10-bit scalar).",
        options={
            "color_encode": True,
            "color_distribution": "log",
        },
    ),
    AvencPreset(
        label="hevc_turbo_12",
        codec="hevc",
        quality="high",
        bits=12,
        description="HEVC 12-bit stream with color cube mapping metadata.",
        options={
            "color_encode": True,
            "color_distribution": "log",
        },
    ),
    AvencPreset(
        label="ffv1_lossless_16",
        codec="ffv1",
        quality="lossless",
        bits=16,
        description="FFV1 mathematically lossless 16-bit archival output.",
        options={"video_name": "payload_ffv1.mkv"},
    ),
]

PRESET_LOOKUP = {preset.label: preset for preset in PRESET_CATALOG}


def get_presets(labels: Optional[Iterable[str]] = None) -> List[AvencPreset]:
    """Return presets by label or the default catalog order."""

    if labels is None:
        return list(PRESET_CATALOG)
    result: List[AvencPreset] = []
    for label in labels:
        try:
            result.append(PRESET_LOOKUP[label])
        except KeyError as exc:  # pragma: no cover - defensive
            raise ValueError(f"Unknown preset label: {label}") from exc
    return result
