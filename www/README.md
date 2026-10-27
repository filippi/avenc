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
background, paired U/V winds and surface currents (sve/svn), particles/arrows, temperature, accumulated rain,
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

Globe wind trails are seeded in the visible viewport and become denser on zoom,
with bounded particle counts (4,200 desktop / 1,200 mobile). Trails reseed on
rotation and zoom, respect missing winds and reduced-motion preferences, and
use an illustrative visual clock rather than forecast-time trajectories. Close
views use modest supersampling within a six-million-pixel canvas budget. This
sharpens rendering, not the scientific grid: source cells remain nearest-sampled
at their native resolution, and zooming does not fetch additional weather data.

## Flow sources and diffusion

Choose **Wind** or **Currents**, then **Sources → Place source on map**. Pick a
colour and click/tap a valid flow cell. Up to six sources can be placed, each
anchored to its own selected forecast time. The current layer requires paired
`sve` and `svn` fields with matching level/member and valid times; unavailable
layers are disabled. **▶** advances the forecast, **◀** plays backward; playback
stops at the forecast ends. Both projections use the same geographic paths.

Source particles use actual elapsed forecast seconds and velocities in m/s:
bilinear spatial interpolation, linear interpolation between valid times, and
midpoint integration with a 600-second / quarter-cell step limit. By default 16 particles
are released per forecast interval, staggered through that interval. These are
illustrative continuous emissions, not concentration estimates. Memory is bounded
at 768 particles/source and 24 track points/particle.

**Brownian diffusion**, in m²/s, defaults to zero (advection alone). It adds
independent east/north displacements with variance `2 K |dt|` per component.
For example, `K=1000` gives a no-flow planar radial RMS displacement of about
3.79 km after one hour; the panel shows this reference scale. Diffusion uses a
repeatable counter-based random generator, so seeking and replaying reproduce
the same paths within a source's forward or backward branch. A diffusion change
clears trajectory caches and recomputes from each release. It does not modify
the data, decorative flow animation, or downloads. Backward diffusion illustrates
possible origins; it does not invert physical mixing or infer source probabilities.

The source integrator stops particles at missing velocity cells; it has no
coastline collision/reflection model, vertical transport, or subgrid turbulence
calibration. These are trajectories of compressed preview fields, not a validated
transport forecast. Decorative white flow trails retain an illustrative visual
clock (enhanced for slow currents); coloured sources never use that multiplier.
Changing field, level or dataset clears sources. Rotating, zooming or switching
2D/3D preserves them. Sources are session-local and are not included in shared URLs.

A time seek with active sources reads intervening paired forecast times over
HTTP Range, with progress/cancellation and bounded grid/trajectory caches. It
can therefore transfer more data than inspecting a single field without sources.

Focused checks:

```bash
node www/tests/met_trajectories.mjs
# Optional: existing local MP4 fixture, range server, Playwright and Chrome
NODE_PATH=/path/to/node_modules CHROME_EXECUTABLE=/path/to/chrome \
  node www/tests/met_sources.cjs
```

### Smoke / dye display

Coloured source particles are drawn as overlapping radial-gradient puffs with
normal alpha blending. **Trajectory lines** is off by default and can be toggled
independently of the plume. **Emission density** selects 8, 16 or 32 staggered
releases per forecast interval. **Particle lifetime** defaults to **Forever** (no age-based expiry or fading),
with **One week** as the alternative, measured in forecast time in either direction.
The 768-particle/source memory cap still replaces the oldest particles when full,
including in Forever mode. Emission and lifetime changes recompute the source's cached states.
**Puff size** and **Plume opacity** change only the appearance, not diffusion or
velocity.

A six-sprite colour cache avoids generating gradients every frame. Rendering is
capped at 30 fps and device-pixel ratio 2, skips hidden tabs and draws no paths
unless requested. Neighbouring forecast frames interpolate positions over 650 ms;
long seeks snap to the calculated state. No extra simulation or emission occurs
while paused. Rotation and zoom only change projection. The puffs suggest smoke
or dye; their brightness and pixel radius are not a calibrated concentration or
physical plume width. Existing diffusion remains the geographic spreading model.

Additional render checks: `node www/tests/met_plume.cjs` (same browser prerequisites
as `met_sources.cjs`).
