from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path

import numpy as np
import pytest
import xarray as xr

from avenc_xarray import register_avenc_engine


REPO_ROOT = Path(__file__).resolve().parents[3]
MP4_PATH = Path(__import__("os").environ.get("AVENC_TEST_MP4", str(REPO_ROOT / "benchmark" / "era_all.mp4")))
AVENC2GRIB_BIN = Path(
    __import__("os").environ.get("AVENC2GRIB_BIN", str(REPO_ROOT / "bin" / "avenc2grib"))
)


def _require_inputs() -> None:
    if not MP4_PATH.exists():
        pytest.skip(f"Missing AVENC MP4 test file: {MP4_PATH}")
    if not AVENC2GRIB_BIN.exists():
        pytest.skip(f"Missing avenc2grib binary: {AVENC2GRIB_BIN}")


def _pick_variable(ds: xr.Dataset) -> str:
    if "2t" in ds.data_vars:
        return "2t"
    if "2d" in ds.data_vars:
        return "2d"
    if not ds.data_vars:
        raise AssertionError("Dataset has no variables")
    return next(iter(ds.data_vars))


def _find_dim_name(dims: tuple[str, ...], candidates: tuple[str, ...]) -> str:
    for name in candidates:
        if name in dims:
            return name
    raise KeyError(f"None of {candidates} found in dims={dims}")


def _open_dataset(*, chunks=None) -> xr.Dataset:
    register_avenc_engine()
    return xr.open_dataset(
        MP4_PATH,
        engine="avenc",
        decode_times=False,
        chunks=chunks,
        avenc2grib_bin=str(AVENC2GRIB_BIN),
    )


def _maybe_limit_time(da: xr.DataArray) -> xr.DataArray:
    """Limit time span for practical test runtime on very large datasets.

    Set AVENC_TEST_MAX_TIME=0 (or negative) to force full-time validation.
    """

    tdim = _find_dim_name(tuple(da.dims), ("time", "valid_time", "step", "time_g0"))
    limit = int(os.environ.get("AVENC_TEST_MAX_TIME", "8"))
    if limit > 0 and da.sizes[tdim] > limit:
        return da.isel({tdim: slice(0, limit)})
    return da


def _maybe_limit_spatial(da: xr.DataArray) -> xr.DataArray:
    """Limit spatial extent for practical runtime on huge domains.

    Set AVENC_TEST_MAX_LAT / AVENC_TEST_MAX_LON <= 0 to disable.
    """

    ydim = _find_dim_name(tuple(da.dims), ("latitude", "lat", "y"))
    xdim = _find_dim_name(tuple(da.dims), ("longitude", "lon", "x"))
    max_lat = int(os.environ.get("AVENC_TEST_MAX_LAT", "256"))
    max_lon = int(os.environ.get("AVENC_TEST_MAX_LON", "256"))

    indexers = {}
    if max_lat > 0 and da.sizes[ydim] > max_lat:
        indexers[ydim] = slice(0, max_lat)
    if max_lon > 0 and da.sizes[xdim] > max_lon:
        indexers[xdim] = slice(0, max_lon)
    if indexers:
        return da.isel(indexers)
    return da


def _streaming_mean_no_dask(da: xr.DataArray, t_chunk: int = 1) -> float:
    """Compute global mean via backend slices without dask.

    This avoids materializing the full dataset in memory while still exercising
    backend indexing over the full domain.
    """

    tdim = _find_dim_name(tuple(da.dims), ("time", "valid_time", "step", "time_g0"))
    ntime = da.sizes[tdim]
    total_sum = 0.0
    total_count = 0
    for t0 in range(0, ntime, t_chunk):
        t1 = min(ntime, t0 + t_chunk)
        arr = da.isel({tdim: slice(t0, t1)}).values
        arr = np.asarray(arr, dtype=np.float64)
        valid = np.isfinite(arr)
        if valid.any():
            total_sum += float(arr[valid].sum())
            total_count += int(valid.sum())
    if total_count == 0:
        return float("nan")
    return total_sum / total_count


def test_phase4_dask_graph_validation() -> None:
    _require_inputs()
    da_mod = pytest.importorskip("dask.array")
    ds = _open_dataset(chunks={"time": 1, "lat": 42, "lon": 96})
    var_name = _pick_variable(ds)
    assert isinstance(ds[var_name].data, da_mod.Array)


def test_phase4_dask_mean_equivalence() -> None:
    _require_inputs()
    pytest.importorskip("dask.array")

    ds_no_dask = _open_dataset(chunks=None)
    # Use coarser spatial chunks here to keep runtime practical on very large domains.
    ds_dask = _open_dataset(chunks={"time": 1, "lat": 300, "lon": 300})

    var_name = _pick_variable(ds_no_dask)
    da_no_dask = _maybe_limit_spatial(_maybe_limit_time(ds_no_dask[var_name]))
    da_dask = _maybe_limit_spatial(_maybe_limit_time(ds_dask[var_name]))
    mean_no_dask = _streaming_mean_no_dask(da_no_dask, t_chunk=1)
    mean_dask = float(da_dask.mean(skipna=True).compute().item())

    assert np.isfinite(mean_no_dask)
    assert np.isfinite(mean_dask)
    assert abs(mean_no_dask - mean_dask) <= 1e-5


def test_phase4_thread_safety_stress_parallel_isel() -> None:
    _require_inputs()
    ds = _open_dataset(chunks=None)
    var_name = _pick_variable(ds)
    da = ds[var_name]

    tdim = _find_dim_name(tuple(da.dims), ("time", "valid_time", "step", "time_g0"))
    ydim = _find_dim_name(tuple(da.dims), ("latitude", "lat", "y"))
    xdim = _find_dim_name(tuple(da.dims), ("longitude", "lon", "x"))

    nt = da.sizes[tdim]
    ny = da.sizes[ydim]
    nx = da.sizes[xdim]
    rng = np.random.default_rng(1234)

    req_count = int(os.environ.get("AVENC_THREAD_STRESS_REQUESTS", "100"))
    max_workers = int(os.environ.get("AVENC_THREAD_STRESS_WORKERS", "16"))
    time_window = int(os.environ.get("AVENC_THREAD_STRESS_TIME_WINDOW", "32"))
    if time_window > 0:
        nt_eff = min(nt, time_window)
    else:
        nt_eff = nt

    requests = []
    for _ in range(req_count):
        t0 = int(rng.integers(0, max(1, nt_eff - 1)))
        dt = int(rng.integers(1, 3))
        y0 = int(rng.integers(0, max(1, ny - 16)))
        dy = int(rng.integers(8, 17))
        x0 = int(rng.integers(0, max(1, nx - 32)))
        dx = int(rng.integers(16, 33))
        t1 = min(nt_eff, t0 + dt)
        y1 = min(ny, y0 + dy)
        x1 = min(nx, x0 + dx)
        requests.append((t0, t1, y0, y1, x0, x1))

    def fetch_shape(req: tuple[int, int, int, int, int, int]) -> tuple[int, int, int]:
        t0, t1, y0, y1, x0, x1 = req
        arr = da.isel({tdim: slice(t0, t1), ydim: slice(y0, y1), xdim: slice(x0, x1)}).values
        return tuple(int(v) for v in np.asarray(arr).shape)

    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        shapes = list(pool.map(fetch_shape, requests))

    for req, shape in zip(requests, shapes):
        t0, t1, y0, y1, x0, x1 = req
        expected = (t1 - t0, y1 - y0, x1 - x0)
        assert shape == expected
