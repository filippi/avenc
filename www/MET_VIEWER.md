# ECFlix - static IFS weather explorer

From the repository root:

```bash
python3 www/serve.py --port 8000
```

Open <http://127.0.0.1:8000/index.html>. The default input is
`www/dataset/20261021.browser_svt10.mp4`; this local data directory is Git-ignored.
To use another file, open **Dataset & connection**, or use:

```text
http://127.0.0.1:8000/index.html?dataset=./dataset/example.mp4
```

Remote HTTP(S) MP4 URLs work with HTTP 206 byte ranges and CORS permitting the
page origin and exposing `Content-Range`. Use HTTPS or localhost for WebCodecs.
The standard Python `http.server` does not reliably serve ranges across versions;
`serve.py` implements the required static GET/HEAD/range responses. Production
hosting can use nginx, Apache or another static range-capable host. No PHP,
FFmpeg server, conversion service or scientific sidecar files are required.

## Interaction

- **Wind** pairs U/V by exact level, member and valid timestamp, displaying
  speed in m/s. Choose 10 m or an available pressure level. Arrows point toward
  the flow; the point inspector reports meteorological direction **from**.
- **Temperature** shows 2 m or pressure-level temperature in degrees Celsius.
- **Precipitation** shows the original accumulated total in millimetres, not
  an hourly rate. Its actual GRIB step range appears next to initialization.
  Colours use a fixed log1p display scale; values below 0.05 mm are transparent.
  Values, including these small values, remain available in the point inspector.
- **Explore every field** lists all logical variable/level/member combinations.
  Known IFS variables use physical units; other fields use native units and a
  per-frame finite min/max display scale. Unsupported codecs are disabled.
- Click the map for nearest-native-cell values. Pan/zoom changes the map view
  without fetching the field again. The map repeats longitude and clips poles
  at Web Mercator's latitude limits; it is not an equal-area projection.
- Use the timeline, previous/next buttons or arrow keys; Space plays/pauses.
  Playback waits for each selected field to finish decoding. Dates are UTC.
- Wind particles animate the instantaneous velocity snapshot with accelerated
  visual time. They are flow cues, not time-integrated parcel trajectories.
  Reduced-motion preferences disable particles; the controls also disable them.
- On phones, **Layers** opens/closes the control panel. Share copies a URL with
  dataset, variable/layer, level, valid time and map position.

## Data and rendering path

`index.html` uses Leaflet 1.9.4, MP4Box 0.5.3, fflate 0.8.2 and WebCodecs.
Leaflet/MP4Box/fflate are pinned CDN dependencies; internet access is needed for
these assets and basemap tiles. OpenStreetMap tiles have visible attribution,
use browser caching and are requested only for visible map tiles (no tile
prefetch or offline scraping). Dark mode recolours the same basemap with CSS.
For substantial public deployment, use an appropriate tile-hosting arrangement.

The new reader in `js/avenc_range_model.js`:

1. Walks MP4 top-level box headers, skipping `mdat`, then fetches `ftyp` and
   `moov`. It parses the index and AVENC metadata without downloading the file.
2. Preserves every video-handler track, including FFV1 entries omitted from
   MP4Box's `videoTracks` summary, so mixed-codec stream indices remain correct.
3. Fetches only the selected stream's preceding keyframe/GOP. Adjacent samples
   in that stream are merged; requests never span other interleaved streams.
   B-frame presentation order follows MP4 composition timestamps. AV1 sequence
   headers from `av1C` are prepended at seeks, and a software decoder retry handles
   hardware decoders that advertise support but reject a scientific profile.
4. Copies raw luma planes at their native depth, reconstructs storage shifts,
   min/max and inverse linear/log/asinh/shifted-log transforms, and restores masks.
   It matches FFmpeg's full-range gray16 bit replication for shifted streams.
   It does not reconstruct values from RGB or silently downconvert bit depth.
5. Uses GRIB2 template 3.0 scan flags/increments for regular grid geometry. This
   handles the IFS grid starting at 180 E and wrapping to 179.75 E correctly.
   Other definitions fall back to native row/column indices, without geographic positioning.
6. Keeps at most six Float32 grids, two decoded VideoFrames, and a 32 MiB
   compressed GOP cache. Metadata/index memory is additional. A single movie
   index or selected GOP larger than 64 MiB is rejected. One next-time compressed
   GOP is prefetched for the selected layer (two components for wind), cancellable
   on another selection. It never preloads the forecast or basemap archive.

Spatial zoom is a rendering operation: AVENC still decodes the **whole selected
spatial frame and necessary temporal dependencies**, not a server-side spatial
tile. Colour rasters use nearest-neighbour native-grid samples; display ranges
are fixed for the standard weather layers and clip colours at their endpoints.
The point inspector retains the reconstructed physical value.

## Limits

The viewer requires GRIB-packed AVENC v2 files with an ordinary, nonfragmented
MP4 index. Recognized regular latitude/longitude grids use a geographic map;
other definitions or scanning modes use the stored raster with row/column
axes. It does not reproject rotated, projected or Gaussian grids, infer
geographic wind directions, or overlay heterogeneous grids. Selecting another
stream can change the active grid and reset the view to its extent.

Codec support is browser/device dependent. FFV1 is generally not available in
WebCodecs, and some 12/16-bit profiles are unsupported. The default SVT browser file uses AV1 10-bit 4:2:0 for every field, including
NSSS/EWSS and cloud cover. Other files can still contain FFV1 or unsupported
profiles; these are disabled. No FFV1/WASM decoder or server fallback is bundled. Use
native AVENC/Python for these fields or make a browser-compatible visualization
encode. This is a display tool, not a forecast-validation or error-analysis tool.

The existing `viewer.html` and `extractor.html` retain their whole-file loading
path; only this weather explorer uses the new range reader. Shared metadata
parsing now retains bitmap/template data and optional coordinate axes. Encoder,
metadata format and scientific C++ algorithms are unchanged.

## Validation

```bash
node www/tests/met_core.mjs
node www/tests/met_range.mjs
node www/tests/metadata.mjs
node www/tests/decoder.mjs
node www/tests/transforms.mjs
```

Optional real-file browser integration (install Playwright separately; no runtime
application dependency):

```bash
npm install --prefix /tmp/avenc-met-tools playwright
NODE_PATH=/tmp/avenc-met-tools/node_modules \
CHROME_EXECUTABLE=/path/to/chrome \
node www/tests/met_browser.cjs
```

Start `serve.py --port 8765` first. `AVENC_MET_URL`, `AVENC_MET_FIXTURE`,
`AVENC_MET_BIN` and `AVENC_MET_SCREENSHOTS` override the test URL, input MP4,
native decoder and screenshot folder. The optional test expects the corrected
IFS fixture and availability of 10u/10v/2t/tp and 850 hPa winds.

On 24 October 2026, isolated headless Chrome loaded the real 2,556,329,812-byte
file's initial +120 h wind view with **5,168,412 transferred MP4 bytes** (including
metadata and both required GOPs). All MP4 responses were HTTP 206. Browser
Float32 values for 10u, 10v, 2t and tp matched native decoding exactly at 1,000
sampled grid points per field. This is a sample check, not a whole-file scientific
validation. Forecast stepping, pressure-level pairing, point probing, zoom without
field requests, and desktop/mobile layouts were exercised. Synthetic tests cover
masks, constants, normalization, longitude wrapping, strict 206 rejection,
truncation/cancellation, interleaving and B-frame ordering.

References: [Leaflet](https://leafletjs.com/reference.html),
[WebCodecs](https://www.w3.org/TR/webcodecs/),
[AV1 WebCodecs registration](https://www.w3.org/TR/webcodecs-av1-codec-registration/),
[OpenStreetMap tile policy](https://operations.osmfoundation.org/policies/tiles/).


## ECFlix units and preview notice

The page defaults to the compact SVT visualization derivative. A permanent
amber notice warns that the data are highly compressed and must not be used
for scientific analysis or operational decisions. **About this demo** explains
possible changes to values/extremes and identifies this as an independent
AVENC demonstration, not an official ECMWF service.

`data/ifs_variables.json` is a static, editable catalogue covering all 53 source
variables (190 variable/level combinations). Names and source units come from
IFS GRIB headers decoded with ecCodes 2.44.0; provenance links are included.
Each entry records the native `units` and a `display` object containing a unit
label, numeric `factor` and `offset`, colour palette, optional range/ticks,
symmetry and precision. Display values equal `native_value * factor + offset`.
Source/reconstructed arrays are unchanged. Edit this JSON to extend the labels
or display conversions; it is served from the same static host without PHP.

Known short names take precedence. Unique raw parameter codes recover names
and units for the older encoder's `unknown` parameters, including NSSS/EWSS.
Ambiguous or missing entries remain **unit unknown**, rather than inferring
physical dimensions from numeric values. For example, accumulated stress uses
N·s/m², relative humidity already uses percent, and snow water equivalent is
labelled mm w.e. Radiation is displayed in MJ/m² and specific humidity in g/kg.

The reader retries software decoding if an advertised hardware decoder returns
an opaque surface without readable native-depth luma; RGB is never used to
approximate scientific values. This is browser/device dependent.

```bash
node www/tests/met_catalogue.mjs
```

The earlier 20261019 SVT file was browser-tested with all 190 field/level options. The
initial wind view transferred 721,190 MP4 bytes including metadata and U/V GOPs.
U10, V10, temperature and precipitation still matched native reconstruction at
1,000 sampled points each. This checks decoding consistency only: the compact
file itself intentionally accepts substantial compression error.

## Compact controls and playback

The timeline sits at the bottom of the map; the colour scale is vertical on the
right. Phones use a native parameter dropdown, with secondary controls under
Settings. During same-layer loading, the previous frame and its valid-time label
remain visible until the replacement is decoded. Wind trails persist between
forecast steps. A layer change clears the old field to avoid mixing labels.

The default preview is now `dataset/20261021.browser_svt10.mp4`. Transfer and
accuracy measurements above refer to their explicitly named older fixtures.

For a real-file playback check (including an artificially delayed decode to
verify that the visible map is retained), run `node www/tests/met_playback.cjs`
with Playwright and Chrome installed. `CHROME_EXECUTABLE` selects Chrome;
`AVENC_MET_URL` overrides the local page URL. Screenshots go to the system
temporary directory, or `AVENC_MET_SCREENSHOTS`. This check uses the new
0.1-degree pressure-only file. Wind defaults to 850 hPa when surface wind is
absent, and unavailable rain/cloud layers stay disabled. The older native
comparison test explicitly selects its separate 0.25-degree fixture.

The 20261021 default contains 140 streams on a 3600 × 1801 native grid. A local
Chrome check transferred 555,789 MP4 bytes for the initial 850-hPa wind view
(index plus U/V GOPs); this is one observation, not an access-cost guarantee.
Three playback steps and the mobile parameter switch passed. An artificially
delayed decode left the previously displayed scalar canvas byte-identical
until the replacement was ready.

## Main page, dataset catalogue and exports

ECFlix now lives at `index.html` (and the site root); the old `met.html` only
redirects, retaining its query/hash. No PHP is used. The icon rail is 76 px wide
and can be fully hidden. Main variable shortcuts appear only when available
(or are disabled for absent standard layers); levels also apply to custom
pressure-level fields. **More** opens the full field list and display settings.
Phones retain a native parameter dropdown and a collapsible level/export rail.

The header loads `datasets/dataset.json` with schema `ecflix.datasets.v1`, a
`default` URL and a `datasets` array of `{url, label, bytes}` entries. The
byte count is optional display metadata, not a decoding input. Refresh local
files using `python3 www/update_datasets.py`; labels and remote HTTP(S) entries
are retained. Add/edit remote entries directly in JSON. Relative URLs resolve
against the web page. Only the selected MP4 is opened. Direct `?dataset=...`
links continue to work and add a temporary Custom option. MP4 data files remain
Git-ignored; the JSON only references files deployed separately on the host.

**Download → This field → NumPy** downloads the already-decoded current field,
with no new field request.
It is a standard Deflate-compressed ZIP of numeric NumPy `.npy` arrays:

- `field`: Float32, `(latitude, longitude)` or `(row, column)`, in the displayed units.
- `latitude`, `longitude`: Float64 native coordinate axes in degrees, with
  longitude wrapped to `[-180, 180)`, without changing data column order.
- `u`, `v`: for derived wind-speed views only, native Float32 components in m/s.
- `metadata_json`: Uint8 UTF-8 JSON, including valid time, units, display
  conversion, source URL, GRIB record, dimensions and compression warning.

Missing cells remain NaN. No pickle is required (`allow_pickle=False`). Export
pauses playback and snapshots the displayed grid before asynchronous ZIP
compression. It materializes the full native field, not the viewport; large
exports require extra browser memory for arrays, NPY copies and ZIP buffers.
This is not an original-data or accuracy-preserving export. The **Download**
dialog provides a complete NumPy/Matplotlib loading example.

Focused checks:

```bash
node www/tests/met_npz.mjs
# Real fixtures, localhost range server, Playwright and Chrome required:
node www/tests/met_home.cjs
node www/tests/met_playback.cjs
```

The homepage check uses both local browser_svt10 files, exercises switching,
rail collapse, custom levels, mobile settings, and downloads a real full-grid
NPZ to the system temporary directory. A separate NumPy inspection verified
the Float32 dtype, `(1801, 3600)` shape, coordinates, metadata and sampled
speed/U/V values against the decoded browser arrays.

Deployment: copy static web assets while excluding the site's `datasets/`
directory. That directory contains the operator-managed MP4 files and
`datasets/dataset.json`; retain it across deployments. JSON URLs resolve
against the page, so deployed entries normally use `./datasets/name.mp4`.

## Native-grid fallback

When the GRIB grid definition cannot be safely interpreted as regular
latitude/longitude (including unsupported templates, scanning modes or missing
coordinates), ECFlix shows the stored raster on a plain background. This is
a presentation fallback, not reprojection or a new geographic grid decoder.
The notice explicitly says geographical positioning is unavailable. Row 0
is at the top; columns run from left to right in reconstructed array order.
Pan, zoom, fit-to-grid, scalar layers, pressure levels, timeline/playback,
point readings and NPZ downloads remain available. Wind particles/arrows,
the geographic scale and basemap controls are disabled. Geographic views
return automatically when selecting a recognized grid. Codec support is
still required; this does not add a browser decoder for FFV1.

Native NPZ archives contain `row` and `column` arrays instead of latitude
and longitude, with `geometry_mode: "native"` and dimensions `[row, column]`
in metadata. They never invent geographic positions. Python examples choose
the appropriate axes and put row 0 at the top. Native share links use
`grid_row` and `grid_column` rather than geographical `lat`/`lon` parameters.
The renderer retains the existing scientific reconstruction and bitmap path.

The previously failing 3600 × 1801 server file was checked using bounded
MP4 byte ranges: native display/probe, time stepping, NPZ download, mobile
fit and shared-view restoration passed. A NumPy inspection confirmed native
row/column arrays and no latitude/longitude arrays in the export. The normal
geographic fixture still matches native decoder samples exactly.

Optional native integration test (requires a range-capable unsupported-grid
fixture URL and the two local browser preview files): set
`AVENC_NATIVE_FIXTURE_URL` and run `node www/tests/met_native.cjs`. It routes
just the fixture's byte-range requests through the test browser, exercises
native/geographic switching, and stores screenshots/NPZ in the system temp
directory. No fixture is bundled or downloaded in full.

## One Download panel, three access workflows

The rail has one **Download** button. Its panel links to AVENC's project page
and source repository and offers, in order:

1. **Whole variable → GRIB:** one copyable `avenc2grib --group ... URL out.grib`
   command. The URL is absolute and the selection includes every available
   level/time for the selected variable (both components for derived wind).
   Known original short-name aliases select all related streams; unresolved
   aliases use explicit stream selectors to avoid selecting unrelated unknown
   variables. The command has no frame restriction and no trailing newline.
2. **This field → xarray:** an `engine="avenc"` example selects each exact
   stream by its `stream_id` attribute and the displayed frame by integer
   position. Loading happens while the dataset is open. Wind speed is derived
   from the selected U/V pair; display factor/offset and units match the UI.
   Native views replace geographic-looking coordinates with row/column indices.
3. **This field → NumPy:** the existing Float32 NPZ download and a collapsible
   NumPy/Matplotlib loading/plotting example. No additional field fetch is needed.

Python code blocks are expandable to keep all three options easy to find on
phones. Each example has its own copy button; only the NumPy option initiates
a browser download. Native CLI/Python examples require a compatible AVENC
installation and FFmpeg. The highly compressed-preview notice applies to all
three workflows. No original values are restored by changing the access method.

Run `node www/tests/met_access.mjs` for generated-code checks and, with the
local browser fixtures and Playwright, `node www/tests/met_download.cjs` for
the panel, copy actions and mobile layout. The generated xarray example was
executed against the local range-served forecast using the current backend;
it loaded and plotted the expected 721 × 1440 wind-speed field.
