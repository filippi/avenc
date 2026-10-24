# Browser tools

Serve this folder on localhost (or HTTPS). WebCodecs support depends on browser,
codec, bit depth and device. FFmpeg is not used inside the browser.

```bash
python3 -m http.server 8000 --directory www
```

Place an AVENC MP4 under `www/dataset/` and open:
`http://localhost:8000/viewer.html?dataset=./dataset/example.mp4`.
`extractor.html` uses the same dataset parameter. The main `index.html`
page is the static ECFlix map; no PHP is required.

The metadata reader (`js/avenc_metadata.js`) issues HTTP Range requests. The
viewer/extractor share `js/avenc_model.js`, which currently downloads the entire
MP4 before decoding. Bounded decoder queues/frame caches reduce retained decoded
frames; they do not remove the initial whole-file download. JavaScript requests
to another origin also require suitable CORS headers. CDN-hosted MP4Box/fflate
are runtime dependencies and retain their own licences.

Run the synthetic tests with Node.js 18+:

```bash
node www/tests/metadata.mjs
node www/tests/decoder.mjs
```

They exercise metadata fetching and bounded decoder behavior with mocks; they do
not certify hardware codec support in every browser.


## ECFlix weather map (static, range-based)

`index.html` is ECFlix, an IFS weather explorer with a zoomable OpenStreetMap
background, paired U/V winds, particles/arrows, temperature, accumulated rain,
pressure levels, forecast playback and point values. It reads only metadata and
selected GOP byte ranges, with bounded caches; it never falls back to a complete
MP4 download. No PHP is required.

```bash
python3 www/serve.py --port 8000
```

Open `http://127.0.0.1:8000/index.html`. Its default file is
`www/dataset/20261021.browser_svt10.mp4` (Git-ignored). Other local or remote URLs can
be supplied with `?dataset=...` or the connection controls. Use a range-capable
static host, not a basic server that responds HTTP 200 to Range requests.

See [MET_VIEWER.md](MET_VIEWER.md) for operation, WebCodecs/FFV1 limitations,
scientific reconstruction details, cache limits and validation. The earlier
viewer/extractor's whole-file behaviour described above is unchanged.

ECFlix displays a permanent visualization-only notice and reads variable names,
source units and explicit display conversions from [data/ifs_variables.json](data/ifs_variables.json).

## Dataset picker and Python export

The top-bar picker reads [datasets/dataset.json](datasets/dataset.json). Refresh local
files after adding/removing MP4s:

```bash
python3 www/update_datasets.py
```

Edit each entry's `label`, `url` and optional `bytes`; set `default` to its URL.
Remote HTTP(S) entries can be added manually and survive local refreshes. The
MP4 host needs HTTP Range and cross-origin CORS when hosted elsewhere. This
manifest is static; files are not discovered automatically by the browser.

The collapsible icon rail includes atmospheric levels, **More** for all
variables/settings, and one **Download** button opening three workflows: a
whole-variable HTTP-to-GRIB command, exact-field xarray access, and a NumPy
archive with a loading/plotting example. A wind archive includes speed plus U/V,
latitude/longitude axes and UTF-8 JSON metadata, with no pickle objects.
`field` uses the displayed units; the source URL, valid time, original GRIB
record and display conversion are recorded. Missing values remain NaN. This
exports reconstructed preview data, not the original GRIB values.

`met.html` redirects old links to `index.html`, preserving query parameters.
The former `index.php` has been removed.

Unsupported geographic grid definitions automatically open in a **native-grid**
view with row/column axes and no basemap or wind directions. Playback, probes
and NPZ exports remain available; native exports use `row`/`column` coordinates.
This fallback displays the stored raster without reprojection.

The top-bar **3D / 2D** button switches recognized geographic fields to a WebGL
globe, with drag/scroll/pinch navigation, point inspection, wind arrows/trails
and the same playback/download controls. It reuses the decoded field rather
than fetching it again. Native-grid views remain 2D. Globe backgrounds use
locally served public-domain Natural Earth land geometry; see
[data/NATURAL_EARTH.md](data/NATURAL_EARTH.md).
