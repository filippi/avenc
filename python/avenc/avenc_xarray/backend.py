from __future__ import annotations

from pathlib import Path
from typing import Iterable, Optional, Tuple

import numpy as np

import xarray as xr
from xarray.core import indexing as xr_indexing
from xarray.backends.common import BackendArray as XRBackendArray
from xarray.backends.common import BackendEntrypoint

from .codec import (
    DEFAULT_VIDEO_NAME,
    compress_dataarray,
    decompress_to_dataarray,
    _probe_comment,
    _decode_metadata_comment,
    _decode_array_payload,
    _resolve_video_file,
    _decode_bitmask,
)
from .grib_dataset import AvencDataset
from .source import FFmpegSource, is_http_url
from . import _avenc_ext


class AvencBackendArray(XRBackendArray):
    """Backend array that serves data by subsetting the MP4 via ffmpeg.

    Supports basic tuple indexing with slices and integers on
    (time, y, x) axes. Steps other than 1 and advanced indexing are not
    implemented.
    """

    def __init__(
        self,
        video_path: Path,
        shape: Tuple[int, int, int],
        options: dict,
        bitmask: Optional[np.ndarray] = None,
        frame_offset: int = 0,
    ):
        self._video_path = str(video_path) if is_http_url(video_path) else Path(video_path)
        self._shape = tuple(int(s) for s in shape)
        self._opts = dict(options or {})
        self._frame_offset = int(frame_offset)
        self._stream_index = int(self._opts.get("stream_index", 0))
        self._bits = int(self._opts.get("bits", 8))
        self._min = float(self._opts.get("min_value"))
        self._max = float(self._opts.get("max_value"))
        self._log2 = bool(self._opts.get("apply_log2", False))
        self._allow_nan = bool(self._opts.get("allow_nan", True))
        self._pix_fmt = self._opts.get("pix_fmt")
        self._color_mapping = self._opts.get("color_mapping")
        self._bitmask = bitmask

    @property
    def shape(self) -> Tuple[int, int, int]:
        return self._shape

    @property
    def dtype(self):  # xarray inspects dtype
        return np.dtype("float32")

    def _decode_quantized(self, q: np.ndarray) -> np.ndarray:
        max_int = float((1 << self._bits) - 1)
        if q.dtype == np.uint16 and self._bits == 8:
            q = (q.astype(np.float32) / 257.0).round().astype(np.uint16)
        qf = q.astype(np.float32)
        if self._allow_nan:
            mask_nan = qf <= 0.0
        else:
            mask_nan = np.zeros_like(qf, dtype=bool)

        x = qf / max_int
        if self._log2:
            eps = 1.0 / max_int
            x = np.where(x <= eps, 0.0, np.exp2(x))
        y = x * (self._max - self._min) + self._min
        y = y.astype(np.float32)
        if self._allow_nan:
            y = np.where(mask_nan, np.nan, y)
        return y

    def _ffmpeg_extract(self, t0: int, t1: int, j0: int, j1: int, i0: int, i1: int) -> np.ndarray:
        options = {
            "bits": self._bits,
            "min_value": self._min,
            "max_value": self._max,
            "apply_log2": self._log2,
            "allow_nan": self._allow_nan,
            "stream_index": self._stream_index,
        }
        if self._pix_fmt:
            options["pix_fmt"] = self._pix_fmt
        if self._color_mapping:
            options["color_mapping"] = self._color_mapping
        arr = _avenc_ext.decode_subset(
            str(self._video_path),
            int(t0 + self._frame_offset),
            int(t1 + self._frame_offset),
            int(i0),
            int(i1),
            int(j0),
            int(j1),
            options,
        )
        out = np.asarray(arr)
        if self._bitmask is not None:
            # bitmask is shaped (time, y, x) -> (t, i, j)
            mask_slice = self._bitmask[t0 + self._frame_offset:t1 + self._frame_offset, i0:i1, j0:j1]
            #out = np.where(mask_slice, np.nan, out)
        return out

    def __getitem__(self, key):
        # Normalize index to (slice/slice/slice) and handle ints by squeezing
        if hasattr(key, "tuple"):
            key_t = key.tuple  # ExplicitIndexer provided by xarray
        else:
            key_t = key if isinstance(key, tuple) else (key,)
        # Pad with full slices
        key_t = key_t + (slice(None),) * (3 - len(key_t))

        slices = []
        squeeze_axes = []
        for ax, (k, dim) in enumerate(zip(key_t, self._shape)):
            if isinstance(k, slice):
                start = 0 if k.start is None else k.start
                stop = dim if k.stop is None else k.stop
                step = 1 if k.step is None else k.step
                if step != 1:
                    raise NotImplementedError("step != 1 not supported")
                if start < 0 or stop < 0:
                    # negative indexing not supported
                    raise NotImplementedError("negative indices not supported")
                if start > dim or stop > dim or start > stop:
                    raise IndexError("slice out of bounds")
                slices.append((start, stop))
            elif isinstance(k, (int, np.integer)):
                idx = int(k)
                if not (0 <= idx < dim):
                    raise IndexError("index out of bounds")
                slices.append((idx, idx + 1))
                squeeze_axes.append(ax)
            else:
                raise NotImplementedError("Only basic slicing supported")

        (t0, t1), (i0, i1), (j0, j1) = slices

        # Short-circuit empty selections
        if t1 <= t0 or j1 <= j0 or i1 <= i0:
            out_shape = (max(0, t1 - t0), max(0, i1 - i0), max(0, j1 - j0))
            arr = np.empty(out_shape, dtype=np.float32)
            return np.squeeze(arr, axis=tuple(squeeze_axes))

        # Chroma-subsampled payloads require an even-aligned decode rectangle. Decode an
        # enclosing rectangle and crop it afterwards; shrinking the request would silently
        # replace the last row/column with NaNs and an odd origin can shift the returned ROI.
        want_h = i1 - i0  # y/height
        want_w = j1 - j0  # x/width
        i0_adj = i0 - (i0 % 2)
        j0_adj = j0 - (j0 % 2)
        i1_adj = min(self._shape[1], i1 + (i1 % 2))
        j1_adj = min(self._shape[2], j1 + (j1 % 2))

        # If an odd-sized domain prevents rounding the end upwards, extend backwards.
        if (i1_adj - i0_adj) % 2 and i0_adj > 0:
            i0_adj -= 1
        if (j1_adj - j0_adj) % 2 and j0_adj > 0:
            j0_adj -= 1

        out = self._ffmpeg_extract(t0, t1, j0_adj, j1_adj, i0_adj, i1_adj)

        if (j0_adj, j1_adj, i0_adj, i1_adj) != (j0, j1, i0, i1):
            y_offset = i0 - i0_adj
            x_offset = j0 - j0_adj
            out = out[:, y_offset:y_offset + want_h, x_offset:x_offset + want_w]

        if squeeze_axes:
            out = np.squeeze(out, axis=tuple(squeeze_axes))
        return out


class AvencCliSubsetBackendArray(XRBackendArray):
    """Backend array reading subsets through `avenc2grib --stream-id ...`."""

    def __init__(self, dataset: AvencDataset, variable_name: str):
        self._dataset = dataset
        self._variable_name = variable_name
        var = dataset.get_variable(variable_name)
        self._shape = (int(var.frame_count), int(var.height), int(var.width))

    @property
    def shape(self) -> Tuple[int, int, int]:
        return self._shape

    @property
    def dtype(self):
        return np.dtype("float32")

    @staticmethod
    def _normalize_key(key, shape: Tuple[int, int, int]):
        if hasattr(key, "tuple"):
            key_t = key.tuple
        else:
            key_t = key if isinstance(key, tuple) else (key,)
        key_t = key_t + (slice(None),) * (3 - len(key_t))

        slices = []
        squeeze_axes = []
        for ax, (k, dim) in enumerate(zip(key_t, shape)):
            if isinstance(k, slice):
                step = 1 if k.step is None else int(k.step)
                if step != 1:
                    raise NotImplementedError("step != 1 not supported")
                start = 0 if k.start is None else int(k.start)
                stop = dim if k.stop is None else int(k.stop)
                if start < 0:
                    start += dim
                if stop < 0:
                    stop += dim
                start = max(0, min(dim, start))
                stop = max(0, min(dim, stop))
                if stop < start:
                    raise IndexError("slice stop < start")
                slices.append((start, stop))
            elif isinstance(k, (int, np.integer)):
                idx = int(k)
                if idx < 0:
                    idx += dim
                if not (0 <= idx < dim):
                    raise IndexError("index out of bounds")
                slices.append((idx, idx + 1))
                squeeze_axes.append(ax)
            else:
                raise NotImplementedError("Only basic slicing supported")
        return slices, squeeze_axes

    def __getitem__(self, key):
        slices, squeeze_axes = self._normalize_key(key, self._shape)
        (t0, t1), (j0, j1), (i0, i1) = slices
        out = self._dataset._get_chunk(
            self._variable_name,
            slice(t0, t1),
            slice(j0, j1),
            slice(i0, i1),
        )
        if squeeze_axes:
            out = np.squeeze(out, axis=tuple(squeeze_axes))
        return out


class AvencBackendEntrypoint(BackendEntrypoint):
    description = "AVENC backend storing data as compressed MetFLIX video"
    open_dataset_parameters = ["filename_or_obj"]

    @staticmethod
    def _resolve_video_and_metadata(filename_or_obj) -> Tuple[str | Path, dict]:
        path = filename_or_obj
        try:
            video_file = _resolve_video_file(path)
        except FileNotFoundError as exc:
            raise FileNotFoundError(f"Missing video payload: {path}") from exc
        comment = _probe_comment(video_file)
        metadata = _decode_metadata_comment(comment)
        return video_file, metadata

    @staticmethod
    def _maybe_chunk(data: xr.DataArray, chunks):
        if chunks not in (None, {}):
            try:
                return data.chunk(chunks)
            except ValueError as exc:
                raise ValueError(
                    "Chunked access requires dask; install dask or pass chunks=None"
                ) from exc
        return data

    @classmethod
    def _build_single_dataarray(cls, video_file: Path, metadata: dict, chunks=None) -> xr.DataArray:
        frames = int(metadata["frames"])
        height = int(metadata["height"])
        width = int(metadata["width"])
        options = metadata.get("options", {})

        bitmask_array = None
        bitmask_info = metadata.get("bitmask")
        if bitmask_info:
            bitmask_array = _decode_bitmask(bitmask_info)

        backend_array = AvencBackendArray(video_file, (frames, height, width), options, bitmask_array)
        lazy_array = xr_indexing.LazilyOuterIndexedArray(backend_array)

        variable = metadata.get("variable", {})
        dims = tuple(variable.get("dims", ["time", "y", "x"]))
        name = variable.get("name") or "data"

        data = xr.DataArray(lazy_array, dims=dims, name=name)
        data.attrs.update(variable.get("attrs", {}))

        coords_payload = metadata.get("coords", {})
        for dim, info in coords_payload.items():
            payload = info.get("data")
            if payload is None:
                continue
            values = _decode_array_payload(payload)
            data = data.assign_coords({dim: values})
            if dim in data.coords:
                data.coords[dim].attrs.update(info.get("attrs", {}))

        return cls._maybe_chunk(data, chunks)

    @classmethod
    def _build_packed_dataset(cls, video_file: Path, metadata: dict, chunks=None) -> xr.Dataset:
        height = int(metadata["height"])
        width = int(metadata["width"])
        options = metadata.get("options", {})
        groups = metadata.get("groups", [])

        if not groups:
            raise ValueError("Packed AVENC metadata has no groups")

        global_bitmask = None
        bitmask_info = metadata.get("bitmask")
        if bitmask_info:
            global_bitmask = _decode_bitmask(bitmask_info)

        top_coords = metadata.get("coords", {})
        shared_lat = None
        shared_lon = None
        shared_lat_attrs = {}
        shared_lon_attrs = {}
        if "lat" in top_coords and top_coords["lat"].get("data") is not None:
            shared_lat = _decode_array_payload(top_coords["lat"]["data"])
            shared_lat_attrs = dict(top_coords["lat"].get("attrs", {}))
        if "lon" in top_coords and top_coords["lon"].get("data") is not None:
            shared_lon = _decode_array_payload(top_coords["lon"]["data"])
            shared_lon_attrs = dict(top_coords["lon"].get("attrs", {}))

        dataset = xr.Dataset()
        for gi, group in enumerate(groups):
            frame_start = int(group.get("frame_start", 0))
            frame_count = int(group.get("frame_count", 0))
            if frame_count <= 0:
                continue

            dims = tuple(group.get("dims", [f"time_g{gi}", "lat", "lon"]))
            if len(dims) != 3:
                raise ValueError(f"Packed group {gi} dims must have length 3")

            name = group.get("name") or f"group_{gi}"
            group_options = dict(options)
            group_options.update(group.get("options", {}))
            group_options["stream_index"] = int(group.get("stream_index", gi))

            backend_array = AvencBackendArray(
                video_file,
                (frame_count, height, width),
                group_options,
                bitmask=global_bitmask,
                frame_offset=frame_start,
            )
            lazy_array = xr_indexing.LazilyOuterIndexedArray(backend_array)
            data = xr.DataArray(lazy_array, dims=dims, name=name)

            data.attrs.update(
                {
                    "short_name": group.get("short_name"),
                    "type_of_level": group.get("type_of_level"),
                    "level": group.get("level"),
                    "frame_start": frame_start,
                    "frame_count": frame_count,
                    "group_id": int(group.get("id", gi)),
                    "value_min": group.get("value_min"),
                    "value_max": group.get("value_max"),
                }
            )

            group_coords = group.get("coords", {})
            for dim, info in group_coords.items():
                payload = info.get("data")
                if payload is None:
                    continue
                values = _decode_array_payload(payload)
                data = data.assign_coords({dim: values})
                if dim in data.coords:
                    data.coords[dim].attrs.update(info.get("attrs", {}))

            lat_dim = dims[1]
            lon_dim = dims[2]
            if shared_lat is not None and lat_dim not in data.coords:
                data = data.assign_coords({lat_dim: shared_lat})
                data.coords[lat_dim].attrs.update(shared_lat_attrs)
            if shared_lon is not None and lon_dim not in data.coords:
                data = data.assign_coords({lon_dim: shared_lon})
                data.coords[lon_dim].attrs.update(shared_lon_attrs)

            data = cls._maybe_chunk(data, chunks)
            dataset[name] = data

        return dataset

    @classmethod
    def _build_packed_dataset_via_avenc2grib(
        cls,
        avds: AvencDataset,
        chunks=None,
    ) -> xr.Dataset:
        dataset = xr.Dataset()
        dataset = dataset.assign_coords(
            lat=("lat", np.asarray(avds.latitudes)),
            lon=("lon", np.asarray(avds.longitudes)),
        )
        if "coords" in avds.metadata:
            lat_attrs = avds.metadata["coords"].get("lat", {}).get("attrs", {})
            lon_attrs = avds.metadata["coords"].get("lon", {}).get("attrs", {})
            if "lat" in dataset.coords:
                dataset.coords["lat"].attrs.update(lat_attrs)
            if "lon" in dataset.coords:
                dataset.coords["lon"].attrs.update(lon_attrs)

        common_time_values = None
        common_time_dim = "time"
        variable_infos = [avds.get_variable(vn) for vn in avds.variable_names]
        if variable_infos:
            first = variable_infos[0].time_values
            shared = True
            for info in variable_infos[1:]:
                if info.time_values.shape != first.shape:
                    shared = False
                    break
                if not np.array_equal(info.time_values, first):
                    shared = False
                    break
            if shared:
                common_time_values = first
                dataset = dataset.assign_coords({common_time_dim: (common_time_dim, first)})

        for var_name in avds.variable_names:
            info = avds.get_variable(var_name)
            time_dim = common_time_dim if common_time_values is not None else info.time_dim
            backend_array = AvencCliSubsetBackendArray(avds, var_name)
            lazy_array = xr_indexing.LazilyOuterIndexedArray(backend_array)

            data = xr.DataArray(lazy_array, dims=(time_dim, "lat", "lon"), name=var_name)
            if common_time_values is None:
                data = data.assign_coords({time_dim: info.time_values})

            data.attrs.update(
                {
                    "stream_id": info.stream_id,
                    "short_name": info.group_meta.get("short_name"),
                    "group_name": info.group_meta.get("name"),
                    "type_of_level": info.group_meta.get("type_of_level"),
                    "level": info.group_meta.get("level"),
                    "frame_count": info.frame_count,
                }
            )
            data = cls._maybe_chunk(data, chunks)
            dataset[var_name] = data

        dataset.attrs.update(
            {
                "schema": avds.metadata.get("schema"),
                "metadata_version": int(avds.metadata.get("metadata_version", 1)),
                "coordinates_embedded": bool(avds.metadata.get("coords")),
                "video_path": str(avds.video_path),
                "group_count": len(avds.variable_names),
                "decode_backend": "avenc2grib-cli",
            }
        )
        return dataset

    def open_dataset(
        self,
        filename_or_obj,
        *,
        drop_variables: Optional[Iterable[str]] = None,
        decode_coords: Optional[bool] = None,
        decode_times: Optional[bool] = None,
        mask_and_scale: Optional[bool] = None,
        chunks=None,
        cache: Optional[bool] = None,
        backend_kwargs: Optional[dict] = None,
        avenc2grib_bin: Optional[str] = None,
        use_cli_subset_for_packed: Optional[bool] = None,
        **kwargs,
    ) -> xr.Dataset:
        if is_http_url(filename_or_obj) and not kwargs.pop("_http_resolved", False):
            supplied = dict(backend_kwargs or {})
            http_options = kwargs.pop("http_options", supplied.pop("http_options", {}))
            adapter = FFmpegSource(filename_or_obj, **http_options)
            try:
                dataset = self.open_dataset(
                    adapter.path, drop_variables=drop_variables, decode_coords=decode_coords,
                    decode_times=decode_times, mask_and_scale=mask_and_scale, chunks=chunks,
                    cache=cache, backend_kwargs=supplied, avenc2grib_bin=avenc2grib_bin,
                    use_cli_subset_for_packed=use_cli_subset_for_packed, _http_resolved=True, **kwargs)
                dataset.attrs["video_path"] = str(filename_or_obj)
                dataset.encoding["avenc_source"] = adapter
                dataset.set_close(adapter.close)
                return dataset
            except Exception:
                adapter.close()
                raise
        _ = (decode_coords, decode_times, mask_and_scale, cache)
        backend_kwargs = dict(backend_kwargs or {})
        backend_kwargs.update(kwargs)

        if avenc2grib_bin is None:
            avenc2grib_bin = backend_kwargs.get("avenc2grib_bin")
        if use_cli_subset_for_packed is None:
            use_cli_subset_for_packed = bool(backend_kwargs.get("use_cli_subset_for_packed", True))
        else:
            use_cli_subset_for_packed = bool(use_cli_subset_for_packed)

        path = Path(filename_or_obj)
        if not is_http_url(filename_or_obj) and path.suffix.lower() == ".json":
            avds = AvencDataset(path, avenc2grib_bin=avenc2grib_bin)
            dataset = self._build_packed_dataset_via_avenc2grib(avds, chunks=chunks)
            if drop_variables:
                dataset = dataset.drop_vars(drop_variables)
            return dataset

        video_file, metadata = self._resolve_video_and_metadata(filename_or_obj)

        schema = metadata.get("schema")
        if schema in {"avenc.v1", "avenc"}:
            data_array = self._build_single_dataarray(video_file, metadata, chunks=chunks)
            dataset = data_array.to_dataset(name=data_array.name or "data")
        elif schema == "avenc.pack.v1":
            if use_cli_subset_for_packed:
                avds = AvencDataset(
                    video_file,
                    avenc2grib_bin=avenc2grib_bin,
                    metadata=metadata,
                    video_path=video_file,
                )
                dataset = self._build_packed_dataset_via_avenc2grib(avds, chunks=chunks)
            else:
                dataset = self._build_packed_dataset(video_file, metadata, chunks=chunks)
        else:
            raise ValueError("Unsupported AVENC metadata schema")

        if drop_variables:
            dataset = dataset.drop_vars(drop_variables)
        return dataset

    def open_dataarray(
        self,
        filename_or_obj,
        *,
        decode_coords: Optional[bool] = None,
        decode_times: Optional[bool] = None,
        mask_and_scale: Optional[bool] = None,
        chunks=None,
        cache: Optional[bool] = None,
        backend_kwargs: Optional[dict] = None,
        avenc2grib_bin: Optional[str] = None,
        use_cli_subset_for_packed: Optional[bool] = None,
        **kwargs,
    ) -> xr.DataArray:
        if is_http_url(filename_or_obj):
            dataset = self.open_dataset(filename_or_obj, chunks=chunks, cache=cache,
                                        backend_kwargs=backend_kwargs, avenc2grib_bin=avenc2grib_bin,
                                        use_cli_subset_for_packed=use_cli_subset_for_packed, **kwargs)
            array = dataset[next(iter(dataset.data_vars))]
            array.set_close(dataset.close)
            return array
        _ = (decode_coords, decode_times, mask_and_scale, cache)
        backend_kwargs = dict(backend_kwargs or {})
        backend_kwargs.update(kwargs)

        if avenc2grib_bin is None:
            avenc2grib_bin = backend_kwargs.get("avenc2grib_bin")
        if use_cli_subset_for_packed is None:
            use_cli_subset_for_packed = bool(backend_kwargs.get("use_cli_subset_for_packed", True))
        else:
            use_cli_subset_for_packed = bool(use_cli_subset_for_packed)

        path = Path(filename_or_obj)
        if not is_http_url(filename_or_obj) and path.suffix.lower() == ".json":
            ds = self._build_packed_dataset_via_avenc2grib(
                AvencDataset(path, avenc2grib_bin=avenc2grib_bin),
                chunks=chunks,
            )
            if not ds.data_vars:
                raise ValueError("Packed AVENC dataset has no readable variables")
            return ds[next(iter(ds.data_vars))]

        video_file, metadata = self._resolve_video_and_metadata(filename_or_obj)
        schema = metadata.get("schema")

        if schema in {"avenc.v1", "avenc"}:
            return self._build_single_dataarray(video_file, metadata, chunks=chunks)

        if schema == "avenc.pack.v1":
            if use_cli_subset_for_packed:
                ds = self._build_packed_dataset_via_avenc2grib(
                    AvencDataset(
                        video_file,
                        avenc2grib_bin=avenc2grib_bin,
                        metadata=metadata,
                        video_path=video_file,
                    ),
                    chunks=chunks,
                )
            else:
                ds = self._build_packed_dataset(video_file, metadata, chunks=chunks)
            if not ds.data_vars:
                raise ValueError("Packed AVENC dataset has no readable variables")
            first_name = next(iter(ds.data_vars))
            return ds[first_name]

        raise ValueError("Unsupported AVENC metadata schema")

    def guess_can_open(self, filename_or_obj) -> bool:
        if is_http_url(filename_or_obj):
            return True
        try:
            path = Path(filename_or_obj)
        except TypeError:
            return False
        if path.is_file() and path.suffix.lower() == ".json":
            return True
        if path.is_dir() and (path / DEFAULT_VIDEO_NAME).exists():
            return True
        if path.is_file() and path.name == DEFAULT_VIDEO_NAME:
            return True
        if path.is_file() and path.suffix.lower() in {".mp4", ".mkv", ".webm", ".mov", ".avi"}:
            return True
        return False


def register_avenc_engine() -> None:
    """Register the AVENC engine with xarray at runtime.

    Supports multiple xarray plugin APIs (older register helper, newer
    backend registry based on ``BACKEND_ENTRYPOINTS``).
    """

    plugins_module = getattr(xr.backends, "plugins", None)
    register = getattr(plugins_module, "register_backend", None)
    if register is None:
        register = getattr(xr.backends, "register_backend", None)
    if register is not None:
        register("avenc", AvencBackendEntrypoint)
        return

    try:
        from xarray.backends.common import BACKEND_ENTRYPOINTS
    except ImportError as exc:  # pragma: no cover - defensive
        raise AttributeError(
            "Unable to access xarray backend registry; incompatible xarray version"
        ) from exc

    BACKEND_ENTRYPOINTS["avenc"] = (None, AvencBackendEntrypoint)

    refresh = getattr(plugins_module, "refresh_engines", None)
    if callable(refresh):
        refresh()


__all__ = [
    "AvencBackendEntrypoint",
    "register_avenc_engine",
    "compress_dataarray",
    "decompress_to_dataarray",
]
