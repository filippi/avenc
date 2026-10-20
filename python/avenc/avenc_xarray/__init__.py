"""AVENC xarray engine built on top of the MetFLIX codec."""

from .backend import AvencBackendEntrypoint, register_avenc_engine
from .codec import compress_dataarray, decompress_to_dataarray
from .grib_dataset import AvencDataset
from .presets import AvencPreset, get_presets, PRESET_CATALOG

__all__ = [
    "AvencDataset",
    "AvencBackendEntrypoint",
    "compress_dataarray",
    "decompress_to_dataarray",
    "register_avenc_engine",
    "AvencPreset",
    "get_presets",
    "PRESET_CATALOG",
]
