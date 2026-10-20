// These examples use the published record, not local development data.
export const base = 'https://zenodo.org/records/23502851/files/';
export const wind = `${base}ERALANDWIND.compact.mp4`;
export const examples = [
  {
    tab: 'Encode', language: 'bash', file: 'terminal', type: 'MP4', transfer: 'ENCODE',
    kicker: '01 · BUILD YOUR OWN ARCHIVE', title: 'Your GRIBs, in a new container.',
    description: 'Download an original source from Zenodo, then choose a profile. Related GRIB fields are grouped into video streams, with reconstruction metadata embedded in the MP4.',
    note: 'This is a full 3.52 GB source download. The alternatives below each perform a separate encoding; codec support depends on FFmpeg.', source: `${base}ERA5_Land_10m_u_component_of_wind_2022_02.grib`,
    code: `curl -fL --retry 3 ${base}ERA5_Land_10m_u_component_of_wind_2022_02.grib -o u10.grib

grib2avenc --compact u10.grib u10.compact.mp4

# Alternative profiles (choose one):
# grib2avenc --high u10.grib u10.high.mp4
# grib2avenc --lossless u10.grib u10.lossless.mp4
# grib2avenc --fast u10.grib u10.fast.mp4
# grib2avenc --medium u10.grib u10.medium.mp4`,
  },
  {
    tab: 'Remote → GRIB', language: 'bash', file: 'terminal', type: 'GRIB', transfer: 'HTTP',
    kicker: '02 · DROP A URL INTO YOUR PIPELINE', title: 'The URL is the input.',
    description: 'Reconstruct the two wind components from the published MP4 directly into a GRIB file. No separate MP4 download command needed.',
    note: 'This command reconstructs the complete archive. The output GRIB can be much larger than the compressed input.', source: wind,
    code: `avenc2grib ${wind} uv.grib

# Inspect your reconstructed fields
# with GRIBView (link below).`,
  },
  {
    tab: 'Inspect', language: 'bash', file: 'terminal', type: 'METADATA', transfer: 'RANGE',
    kicker: '03 · FIND THE RIGHT STREAM', title: 'Look before you load.',
    description: 'List variables, stream IDs, frame counts, quantization ranges and shared masks. The wind archive contains 10u in stream 0 and 10v in stream 1, with 672 hourly frames each.',
    note: 'Frames are zero-indexed. Stream IDs are file-specific: inspect a new archive before choosing one.', source: wind,
    code: `avenc2grib --list ${wind}

# The same interface works for the ensemble archive.
avenc2grib --list ${base}ENSEMBLE.high.mp4`,
  },
  {
    tab: 'Time window → Zarr', language: 'bash', file: 'terminal', type: 'ZARR', transfer: 'SELECT',
    kicker: '04 · SELECT TEN FRAMES', title: 'A window, not the whole month.',
    description: 'Export ten U10 frames starting at index 49 to a Zarr store. For this hourly wind file, that is 3 February 2022, 01–10 UTC. The same switches can crop a spatial window too.',
    note: 'Stream 0 means U10 here, not an ensemble member. Zarr export is experimental; inspect the per-stream layout.', source: wind,
    code: `avenc2grib --verbose --stream-id 0 --frame-start 49 --frame-count 10 ${wind} --zarr-out u10_frames_49_58.zarr

# Optional spatial crop (add to the command above):
# --ni-start 0 --nj-start 360 --width 60 --height 60`,
  },
  {
    tab: 'Lazy xarray', language: 'python', file: 'window.py', type: 'XARRAY', transfer: 'LAZY',
    kicker: '05 · FAMILIAR PYTHON. REMOTE DATA.', title: 'Open now. Decode on selection.',
    description: 'Open the remote dataset lazily, then load one 60 × 60 window from both wind components. Compute wind speed with the decoded U and V values.',
    note: 'This file has latitudes from north to south and longitudes from 0° to 359.9°. Indices select the same area in both components.', source: wind,
    code: `import numpy as np
import xarray as xr

url = "${wind}"

with xr.open_dataset(url, engine="avenc", cache=False) as ds:
    print(ds)  # metadata first; values remain lazy
    uv = ds[["10u", "10v"]].isel(
        time=49, lat=slice(360, 420), lon=slice(0, 60)
    ).load()
    speed = np.hypot(uv["10u"], uv["10v"])
    print(speed)`,
  },
  {
    tab: 'Point series', language: 'python', file: 'point.py', type: 'SERIES', transfer: 'LAZY',
    kicker: '06 · A MONTH AT ONE LOCATION', title: 'Pick a point. Keep the time axis.',
    description: 'Select the nearest grid point to 50.66° N, 1.59° W and read both wind components through February 2022. The coordinate selection happens before loading the values.',
    note: 'A point request still needs video frames to be decoded. Small output size does not imply a point-sized decode or transfer.', source: wind,
    code: `import numpy as np
import xarray as xr

url = "${wind}"

with xr.open_dataset(url, engine="avenc", cache=False) as ds:
    uv = ds[["10u", "10v"]].sel(
        lat=50.66, lon=358.41, method="nearest"
    ).load()
    speed = np.hypot(uv["10u"], uv["10v"])
    print(speed.to_series())`,
  },
  {
    tab: 'Raw arrays', language: 'bash', file: 'terminal', type: 'FLOAT32', transfer: 'SELECT',
    kicker: '07 · NUMPY, WITHOUT A GRIB INTERMEDIATE', title: 'Straight to an array.',
    description: 'Write a cropped, ten-frame U10 window as little-endian Float32 values. Read it directly in NumPy with the documented frame, row, column layout.',
    note: 'Raw output has no embedded coordinates. Keep the source metadata and selection alongside the array.', source: wind,
    code: `avenc2grib --stream-id 0 --frame-start 49 --frame-count 10 --ni-start 0 --nj-start 360 --width 60 --height 60 ${wind} > u10.f32

python - <<'PY'
import numpy as np
u10 = np.fromfile("u10.f32", dtype="<f4").reshape(10, 60, 60)
print(u10.shape)
PY`,
  },
  {
    tab: 'Validate', language: 'bash', file: 'terminal', type: 'METRICS', transfer: 'COMPARE',
    kicker: '08 · MEASURE THE SCIENTIFIC TRADE-OFF', title: 'A profile is a choice. Test it.',
    description: 'Measure reconstruction errors using avenc-compare. This example continues from the encoding example, so the reference and encoded file contain the same U10 fields.',
    note: 'Fast mode samples the data and may miss extremes. Full mode evaluates all matched values; it costs more time.', source: `${base}ERA5_Land_10m_u_component_of_wind_2022_02.grib`,
    code: `# First run the Encode example to create these files.
avenc-compare --fast u10.grib u10.compact.mp4 --json u10.sampled-errors.json

# For a full comparison instead:
avenc-compare --full u10.grib u10.compact.mp4 --json u10.full-errors.json

# Archived reports are also available on Zenodo:
# ${base}ERALANDWIND.error.zip`,
  },
];
