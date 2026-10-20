from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import xarray as xr
import xarray.testing as xrt

from avenc_xarray import register_avenc_engine
from avenc_xarray.backend import AvencCliSubsetBackendArray


REPO_ROOT = Path(__file__).resolve().parents[3]
MP4_PATH = Path(__import__("os").environ.get("AVENC_TEST_MP4", str(REPO_ROOT / "benchmark" / "era_all.mp4")))
TRUTH_GRIB_PATH = Path(
    __import__("os").environ.get("AVENC_TRUTH_GRIB", str(REPO_ROOT / "benchmark" / "era_land.grib"))
)
AVENC2GRIB_BIN = Path(
    __import__("os").environ.get("AVENC2GRIB_BIN", str(REPO_ROOT / "bin" / "avenc2grib"))
)


def _require_inputs() -> None:
    if not MP4_PATH.exists():
        pytest.skip(f"Missing AVENC MP4 test file: {MP4_PATH}")
    if not AVENC2GRIB_BIN.exists():
        pytest.skip(f"Missing avenc2grib binary: {AVENC2GRIB_BIN}")


def _open_avenc_dataset() -> xr.Dataset:
    register_avenc_engine()
    return xr.open_dataset(
        MP4_PATH,
        engine="avenc",
        decode_times=False,
        avenc2grib_bin=str(AVENC2GRIB_BIN),
    )


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


def _open_truth_for_var(var_name: str) -> xr.Dataset:
    cfgrib = pytest.importorskip("cfgrib")
    _ = cfgrib
    if not TRUTH_GRIB_PATH.exists():
        pytest.skip(f"Missing truth GRIB file: {TRUTH_GRIB_PATH}")

    # First try plain open; if variable is absent, try shortName filter.
    try:
        ds = xr.open_dataset(TRUTH_GRIB_PATH, engine="cfgrib", decode_times=False)
    except Exception as exc:
        pytest.skip(f"Unable to open truth GRIB with cfgrib: {exc}")

    if var_name not in ds.data_vars:
        try:
            ds = xr.open_dataset(
                TRUTH_GRIB_PATH,
                engine="cfgrib",
                decode_times=False,
                backend_kwargs={"filter_by_keys": {"shortName": var_name}, "indexpath": ""},
            )
        except Exception as exc:
            pytest.skip(f"Unable to filter truth GRIB for shortName={var_name}: {exc}")
    return ds


def test_phase3_engine_registration_open_dataset() -> None:
    _require_inputs()
    ds = _open_avenc_dataset()
    assert isinstance(ds, xr.Dataset)
    assert len(ds.data_vars) > 0


def test_phase3_lazy_loading_backend_array_not_numpy() -> None:
    _require_inputs()
    ds = _open_avenc_dataset()
    var_name = _pick_variable(ds)
    da = ds[var_name]

    # Avoid da.data here because for giant arrays this can materialize eagerly.
    backend = da.variable._data.array.array.array
    assert isinstance(backend, AvencCliSubsetBackendArray)
    assert not isinstance(backend, np.ndarray)


def test_phase3_odd_unaligned_subset_matches_full_decode() -> None:
    """Chroma-aligned decoding must not shift or pad an arbitrary xarray slice."""
    _require_inputs()
    ds = _open_avenc_dataset()
    da = ds[_pick_variable(ds)]
    time_dim = _find_dim_name(tuple(da.dims), ("time", "valid_time", "step", "time_g0"))
    lat_dim = _find_dim_name(tuple(da.dims), ("latitude", "lat", "y"))
    lon_dim = _find_dim_name(tuple(da.dims), ("longitude", "lon", "x"))

    full = np.asarray(da.isel({time_dim: slice(2, 5)}).values)
    subset = np.asarray(
        da.isel(
            {
                time_dim: slice(2, 5),
                lat_dim: slice(3, 10),
                lon_dim: slice(5, 14),
            }
        ).values
    )

    np.testing.assert_allclose(subset, full[:, 3:10, 5:14], equal_nan=True)


def test_phase3_data_fetching_matches_cfgrib_truth() -> None:
    _require_inputs()
    ds_av = _open_avenc_dataset()
    var_name = _pick_variable(ds_av)

    ds_truth = _open_truth_for_var(var_name)
    if var_name not in ds_truth.data_vars:
        pytest.skip(f"Truth dataset does not contain variable {var_name}")

    da_av = ds_av[var_name]
    da_truth = ds_truth[var_name]

    av_t = _find_dim_name(tuple(da_av.dims), ("time", "valid_time", "step", "time_g0"))
    av_lat = _find_dim_name(tuple(da_av.dims), ("latitude", "lat", "y"))
    av_lon = _find_dim_name(tuple(da_av.dims), ("longitude", "lon", "x"))

    tr_t = _find_dim_name(tuple(da_truth.dims), ("time", "valid_time", "step"))
    tr_lat = _find_dim_name(tuple(da_truth.dims), ("latitude", "lat", "y"))
    tr_lon = _find_dim_name(tuple(da_truth.dims), ("longitude", "lon", "x"))

    subset_truth = da_truth.isel({tr_t: 0, tr_lat: slice(10, 20), tr_lon: slice(10, 20)})
    subset_av = da_av.isel({av_t: 0, av_lat: slice(10, 20), av_lon: slice(10, 20)})

    # Align names/coords before numerical comparison.
    subset_av = subset_av.rename({av_lat: tr_lat, av_lon: tr_lon})
    if tr_lat in subset_truth.coords:
        subset_av = subset_av.assign_coords({tr_lat: subset_truth[tr_lat]})
    if tr_lon in subset_truth.coords:
        subset_av = subset_av.assign_coords({tr_lon: subset_truth[tr_lon]})
    if tr_t in subset_av.dims and tr_t in subset_truth.dims:
        subset_av = subset_av.transpose(*subset_truth.dims)

    xrt.assert_allclose(subset_truth, subset_av, rtol=1e-3, atol=0.5)
