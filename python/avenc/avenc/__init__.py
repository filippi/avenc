"""Convenient local/HTTP AVENC opening with lazy xarray arrays."""
from __future__ import annotations
from pathlib import Path
from typing import Any
from importlib.metadata import version, PackageNotFoundError
try:
    __version__ = version("avenc")
except PackageNotFoundError:
    __version__ = "unknown"

import xarray as xr
from avenc_xarray import register_avenc_engine

register_avenc_engine()


def open_avenc(path_or_url: str | Path, **kwargs: Any) -> xr.Dataset:
    """Open AVENC lazily; close the dataset to release its HTTP source/cache.

    Pass ``backend_kwargs={"http_options": {...}}`` to configure block_size,
    cache_blocks, timeout, fallback and fallback_limit. Live upstream statistics
    are available via ``dataset.avenc.http_stats``.
    """
    return xr.open_dataset(path_or_url, engine="avenc", **kwargs)


@xr.register_dataset_accessor("avenc")
class AvencAccessor:
    """HTTP measurements for datasets opened with the AVENC backend."""
    def __init__(self, dataset: xr.Dataset) -> None:
        self._dataset = dataset

    @property
    def http_stats(self) -> dict:
        source = self._dataset.encoding.get("avenc_source")
        return source.stats if source else {"mode": "local"}
