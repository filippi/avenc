# AVENC: public Zenodo examples

Install AVENC with `pip install avenc` and FFmpeg 5.1+ separately.
These commands execute on your machine; some reconstruct or download large files.

## Encode: Your GRIBs, in a new container.

Download an original source from Zenodo, then choose a profile. Related GRIB fields are grouped into video streams, with reconstruction metadata embedded in the MP4.

```bash
curl -fL --retry 3 https://zenodo.org/records/23502851/files/ERA5_Land_10m_u_component_of_wind_2022_02.grib -o u10.grib

grib2avenc --compact u10.grib u10.compact.mp4

# Alternative profiles (choose one):
# grib2avenc --high u10.grib u10.high.mp4
# grib2avenc --lossless u10.grib u10.lossless.mp4
# grib2avenc --fast u10.grib u10.fast.mp4
# grib2avenc --medium u10.grib u10.medium.mp4
```

This is a full 3.52 GB source download. The alternatives below each perform a separate encoding; codec support depends on FFmpeg.

[Source file](https://zenodo.org/records/23502851/files/ERA5_Land_10m_u_component_of_wind_2022_02.grib)

## Remote → GRIB: The URL is the input.

Reconstruct the two wind components from the published MP4 directly into a GRIB file. No separate MP4 download command needed.

```bash
avenc2grib https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4 uv.grib

# Inspect your reconstructed fields
# with GRIBView (link below).
```

This command reconstructs the complete archive. The output GRIB can be much larger than the compressed input.

[Source file](https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4)

## Inspect: Look before you load.

List variables, stream IDs, frame counts, quantization ranges and shared masks. The wind archive contains 10u in stream 0 and 10v in stream 1, with 672 hourly frames each.

```bash
avenc2grib --list https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4

# The same interface works for the ensemble archive.
avenc2grib --list https://zenodo.org/records/23502851/files/ENSEMBLE.high.mp4
```

Frames are zero-indexed. Stream IDs are file-specific: inspect a new archive before choosing one.

[Source file](https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4)

## Time window → Zarr: A window, not the whole month.

Export ten U10 frames starting at index 49 to a Zarr store. For this hourly wind file, that is 3 February 2022, 01–10 UTC. The same switches can crop a spatial window too.

```bash
avenc2grib --verbose --stream-id 0 --frame-start 49 --frame-count 10 https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4 --zarr-out u10_frames_49_58.zarr

# Optional spatial crop (add to the command above):
# --ni-start 0 --nj-start 360 --width 60 --height 60
```

Stream 0 means U10 here, not an ensemble member. Zarr export is experimental; inspect the per-stream layout.

[Source file](https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4)

## Lazy xarray: Open now. Decode on selection.

Open the remote dataset lazily, then load one 60 × 60 window from both wind components. Compute wind speed with the decoded U and V values.

```python
import numpy as np
import xarray as xr

url = "https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4"

with xr.open_dataset(url, engine="avenc", cache=False) as ds:
    print(ds)  # metadata first; values remain lazy
    uv = ds[["10u", "10v"]].isel(
        time=49, lat=slice(360, 420), lon=slice(0, 60)
    ).load()
    speed = np.hypot(uv["10u"], uv["10v"])
    print(speed)
```

This file has latitudes from north to south and longitudes from 0° to 359.9°. Indices select the same area in both components.

[Source file](https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4)

## Point series: Pick a point. Keep the time axis.

Select the nearest grid point to 50.66° N, 1.59° W and read both wind components through February 2022. The coordinate selection happens before loading the values.

```python
import numpy as np
import xarray as xr

url = "https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4"

with xr.open_dataset(url, engine="avenc", cache=False) as ds:
    uv = ds[["10u", "10v"]].sel(
        lat=50.66, lon=358.41, method="nearest"
    ).load()
    speed = np.hypot(uv["10u"], uv["10v"])
    print(speed.to_series())
```

A point request still needs video frames to be decoded. Small output size does not imply a point-sized decode or transfer.

[Source file](https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4)

## Raw arrays: Straight to an array.

Write a cropped, ten-frame U10 window as little-endian Float32 values. Read it directly in NumPy with the documented frame, row, column layout.

```bash
avenc2grib --stream-id 0 --frame-start 49 --frame-count 10 --ni-start 0 --nj-start 360 --width 60 --height 60 https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4 > u10.f32

python - <<'PY'
import numpy as np
u10 = np.fromfile("u10.f32", dtype="<f4").reshape(10, 60, 60)
print(u10.shape)
PY
```

Raw output has no embedded coordinates. Keep the source metadata and selection alongside the array.

[Source file](https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4)

## Validate: A profile is a choice. Test it.

Measure reconstruction errors using avenc-compare. This example continues from the encoding example, so the reference and encoded file contain the same U10 fields.

```bash
# First run the Encode example to create these files.
avenc-compare --fast u10.grib u10.compact.mp4 --json u10.sampled-errors.json

# For a full comparison instead:
avenc-compare --full u10.grib u10.compact.mp4 --json u10.full-errors.json

# Archived reports are also available on Zenodo:
# https://zenodo.org/records/23502851/files/ERALANDWIND.error.zip
```

Fast mode samples the data and may miss extremes. Full mode evaluates all matched values; it costs more time.

[Source file](https://zenodo.org/records/23502851/files/ERA5_Land_10m_u_component_of_wind_2022_02.grib)
