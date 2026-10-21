# AVENC operating guide for agents

Use this guide to help a user compress meteorological data, construct a reusable
AVENC dataset, or read local/remote scientific fields. It describes the **AVENC
1.0.0** interfaces checked against CLI help and source on **21 October 2026**.
Recheck installed help when versions differ. The executable names are
`grib2avenc`, `avenc2grib`, `avenc-compare`, and the lower-level `avpack`.

This is a usage guide, not permission to run large experiments or change the
codec. Work within the user's requested data, resources and output scope.
Preserve source files and existing measurements. Read `AGENTS.md` and inspect
Git status before editing the software repository. Do not modify numerical
algorithms, publish archives or push changes as a side effect of data exploration.

## Contents

- [Part 1 — Compressing data](#part-1--compressing-data)
  - [1.1 Establish the intended use](#11-establish-the-intended-use)
  - [1.2 Install and inspect the environment](#12-install-and-inspect-the-environment)
  - [1.3 Inspect the source and choose a calibration](#13-inspect-the-source-and-choose-a-calibration)
  - [1.4 Screen profiles, then validate the candidate](#14-screen-profiles-then-validate-the-candidate)
  - [1.5 All grib2avenc options and built-in profiles](#15-all-grib2avenc-options-and-built-in-profiles)
  - [1.6 Quantization, missing values and logarithmic scaling](#16-quantization-missing-values-and-logarithmic-scaling)
  - [1.7 Advanced packer, custom CSV and hardware](#17-advanced-packer-custom-csv-and-hardware)
  - [1.8 Compare errors and inspect the extrema](#18-compare-errors-and-inspect-the-extrema)
  - [1.9 All avenc-compare options and metric definitions](#19-all-avenc-compare-options-and-metric-definitions)
  - [1.10 Direct Python array encoding](#110-direct-python-array-encoding)
- [Part 2 — Building a dataset](#part-2--building-a-dataset)
  - [2.1 Grouping and file organization](#21-grouping-and-file-organization)
  - [2.2 Archive and publication records](#22-archive-and-publication-records)
  - [2.3 Application-specific validation](#23-application-specific-validation)
- [Part 3 — Accessing data](#part-3--accessing-data)
  - [3.1 Inspect before decoding](#31-inspect-before-decoding)
  - [3.2 GRIB reconstruction and stream extraction](#32-grib-reconstruction-and-stream-extraction)
  - [3.3 Float32 windows](#33-float32-windows)
  - [3.4 Zarr exports](#34-zarr-exports)
  - [3.5 All avenc2grib options](#35-all-avenc2grib-options)
  - [3.6 Local and remote xarray](#36-local-and-remote-xarray)
  - [3.7 HTTP Range configuration and measurements](#37-http-range-configuration-and-measurements)
  - [3.8 JavaScript and browser tools](#38-javascript-and-browser-tools)
  - [3.9 Troubleshooting and handoff](#39-troubleshooting-and-handoff)

## Essential distinctions

| Distinction | Operational consequence |
| --- | --- |
| Video-lossless versus scientific losslessness | Integer video samples may be preserved while quantization, format conversion or GRIB packing changes physical values. Validate against source values. |
| Encoder `--fast` versus comparison `--fast` | One selects an AVC encoding profile; the other samples fields/points during evaluation. Neither is a general accuracy guarantee. |
| Direct MP4 comparison versus reconstructed-GRIB comparison | The second includes GRIB repacking. Identify the measured path and do not silently combine the results. |
| Native GRIB encoder versus Python array encoder | They use different implementations, schemas and option vocabularies. Native `avenc-compare` supports the GRIB-packed schema, not the separate array package. |
| Frame index versus forecast time | A frame can represent a time, level, or other record within a stream. Inspect metadata before selecting. |
| Quantization group versus codec GOP | A common scientific scaling range and video prediction dependencies are different concepts. |
| Lazy output versus cheap point decoding | A one-point result may require preceding GOP frames and complete spatial frames to be decoded. Measure actual transfer and work. |
| Metadata parser versus numerical verification | Successful inspection proves readability, not preservation of masks, extrema, distributions or model behaviour. |

# Part 1 — Compressing data

## 1.1 Establish the intended use

When a user says “compress my dataset”, first determine the scientific purpose.
Ask only for missing information that changes the experiment. While waiting,
inspect headers, sizes and available encoders without starting a large sweep.
Do not ask again for facts already established in the conversation.

Useful questions, phrased in the user's terms:

- Is this for maps/animation, quantitative analysis, AI training, or forcing a
  numerical model? Will the original remain available?
- Which variables, units, grids, times, levels and members are involved?
- What errors matter: typical absolute error, local extremes, exact zeros,
  thresholds, gradients, temporal variability, or distribution/spread changes?
- Is there a physical tolerance, and must it hold everywhere or only on average?
- Is the priority smaller storage, encoding speed, selective HTTP access,
  decoding speed, or compatibility with a specific device?
- What calibration size and compute budget are acceptable?

Use the answers to define acceptance **before** choosing the winning profile.
If the user has not set tolerances, present measured trade-offs and recommend a
candidate for their review; do not invent an “operationally acceptable” threshold.

| Intended use | Minimum useful evaluation |
| --- | --- |
| Quick visualization | Check recognizable structures, mask boundaries, extrema and selected frames at fixed colour limits. Do not present interpolated/colour-mapped display as numerical verification. |
| Scientific plotting or diagnostics | Physical-unit RMSE/MAE/bias, tails and maximum error; verify the derived quantity actually plotted. |
| Thresholds/extremes | Full comparison, exact source/output threshold classifications and local maps/time series around the largest departures. |
| Gradients or spectral analysis | Add the actual derivative/spectral operator, including stencil, boundary handling and time cadence. Low RMSE alone is insufficient. |
| Ensemble information | Match member/time/variable first; evaluate distributions and pointwise spread, not pooled group variance alone. |
| Numerical-model forcing | Check all required variables/levels/masks; run the relevant preprocessing and model test only within the authorized scope. Encoding success is insufficient. |
| Scientific archiving with exact values | Do not equate `--lossless` with exact source recovery. If exact values/bytes are required, retain a verified source-exact representation unless an explicit full check proves the required equality. |
| ML/AI data | Verify dtype conversion, coordinate/mask handling, train/test consistency and the downstream preprocessing target. Zarr export does not establish model equivalence. |

## 1.2 Install and inspect the environment

The normal installation is:

```bash
python -m pip install avenc
```

FFmpeg/ffprobe are separate runtime dependencies, version 5.1 or newer. For example:

```bash
# Debian/Ubuntu
sudo apt install ffmpeg

# macOS: include the software encoders used by the standard profiles
brew install ffmpeg-full
export PATH="$(brew --prefix ffmpeg-full)/bin:$PATH"
```

Supported wheels cover Python 3.11–3.14, Linux x86_64/aarch64 (glibc >=2.28),
and macOS Intel/Apple Silicon (15+). Windows users can use WSL. Wheels bundle
native tools and ecCodes libraries; an additional ecCodes Python package is
needed only for workflows that import `eccodes` themselves.

Check the actual installation before interpreting profile names:

```bash
grib2avenc --version
avenc2grib --version
avenc-compare --version
grib2avenc --help
avenc2grib --help
avenc-compare --help
avpack --help
grib2avenc --list-options
ffmpeg -version
ffprobe -version
ffmpeg -hide_banner -encoders
```

Use `ffmpeg -hide_banner -h encoder=libx265`, for example, to inspect that
encoder's supported pixel formats. Scientific precision and negotiated video
bit depth are separate records. Some combinations fail; the native wrapper can
fall back from requested 12-bit AVC/libx264 to 10 bits. Capture verbose output
and inspect the resulting stream instead of assuming the profile name is proof.

For a source build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_FAST_MATH=OFF
cmake --build build --parallel 4
export PATH="$PWD/bin:$PATH"
```

Use `ECCODES_HOME` or `CMAKE_PREFIX_PATH` for a nonstandard ecCodes installation.
`ENABLE_NATIVE_ARCH=ON` trades portability for CPU-specific optimization.
Keep `ENABLE_FAST_MATH=OFF` for finite/missing-value correctness. See
[docs/INSTALL.md](docs/INSTALL.md) for source installation and standalone archives.

## 1.3 Inspect the source and choose a calibration

For GRIB input, inventory identities and field counts before encoding. Include
parameter/short name, units, type of level, level, initialization/valid time,
accumulation interval, ensemble member, grid geometry/scanning and bitmap
presence. ecCodes command-line tools are useful when installed:

```bash
grib_count source.grib
grib_ls -p shortName,units,typeOfLevel,level,dataDate,dataTime,stepRange,number,Ni,Nj,bitmapPresent source.grib
```

Some keys do not exist for every product. Missing inventory keys are not licence
to replace levels or member identities with guessed values. Preserve metadata
alongside every chosen calibration selection.

Select a bounded, scientifically representative development subset. Include
several consecutive times for temporal prediction, energetic fields, low and
high values, relevant vertical levels and mask boundaries. For accumulations,
include a reset or cycle transition. Use the **same input** for every candidate.
A first-frame-only trial does not represent a long inter-frame sequence.

Keep calibration results separate from full-dataset results. A profile selected
on one month, variable or region is not automatically appropriate everywhere.
Do not mix calibration-only and full-archive points on one matched Pareto curve.

## 1.4 Screen profiles, then validate the candidate

A useful first comparison includes Video-lossless, High, Medium, Compact and
Fast when the available encoders support them. Use fewer candidates if the
user's purpose already narrows the choice. These commands illustrate an
experiment, not an instruction to run all profiles on an unbounded archive:

```bash
mkdir -p trial reports

grib2avenc --verbose --lossless calibration.grib trial/lossless.mp4
grib2avenc --verbose --high calibration.grib trial/high.mp4
grib2avenc --verbose --medium calibration.grib trial/medium.mp4
grib2avenc --verbose --compact calibration.grib trial/compact.mp4
grib2avenc --verbose --fast calibration.grib trial/fast.mp4

avenc-compare --fast calibration.grib trial/high.mp4 --json reports/high.fast.json --csv reports/high.groups.csv
avenc-compare --fast calibration.grib trial/compact.mp4 --json reports/compact.fast.json --csv reports/compact.groups.csv
```

Compare file size, resolved encoder/pixel format, measured encode time, RMSE,
MAE, bias, mask counts and the sampled tails. For promising candidates, inspect
full extremes on the calibration and then validate the intended full output:

```bash
avenc-compare --full calibration.grib trial/high.mp4 --json reports/high.full.json --csv reports/high.full.groups.csv --messages-csv reports/high.full.fields.csv --top 10
```

Use `--full` when claiming that a maximum covers the entire input. Full mode
still estimates quantiles if the reservoir is capped; maxima/moments and
quantiles have different coverage semantics. Do not eliminate the full extreme
check merely because sampled RMSEs look similar.

Choose a nondominated candidate that meets the user's acceptance criteria. If
none does, report the measured limitation, raise precision/change the encoder or
retain the original. Do not assume that reaching a target such as 100× proves
scientific suitability. Complete GRIB/MP4 ratio, raw-array/MP4 ratio and the
amount downloaded for a selection are three different quantities.

Before a long run, provide the exact command/configuration, target filenames,
estimated workload from calibration and planned checks. Do not launch a large
sweep solely because the guide contains one.

## 1.5 All grib2avenc options and built-in profiles

Syntax:

```text
grib2avenc [options] input1.grib [input2.grib ...] [output.mp4]
```

One input without an explicit output writes `<input>.mp4`. Multiple inputs
require an explicit MP4 output. Native encoding takes local source files; do
not assume that an HTTP source GRIB is accepted because the decoder accepts URLs.
Use a source-only directory or explicit file list, not `*` in a mixed directory
containing scripts, old outputs and GRIB files.

| Option | Meaning / operational note |
| --- | --- |
| `--enc NAME` | Select a built-in or CSV-defined profile. `<name>_crfNN` overrides generated CRF; e.g. `hevc_good10_crf42`. CRF scales are codec-specific. |
| `--lossless` | Default `av1_lossless12_auto_400`: libaom AV1, gray12le, CRF 0, bitrate 0, cpu-used 4, row-MT, GOP 16, no lookahead. Scientific quantization still applies. |
| `--high`, `-high` | `hevc_vgood12`, HEVC, 12-bit request, auto quantization. |
| `--medium`, `-medium` | `vp9_hq10_auto_420`, VP9, 10 bits, auto, 4:2:0. |
| `--compact`, `-compact` | `av1_good10_auto_400`, AV1, 10 bits, auto, grayscale. |
| `--fast`, `-fast` | `avc_xcr8`, AVC, 8 bits, auto, 4:2:0. |
| `--low`, `-low` | Alias of Compact; not an additional configuration. |
| `--apple`, `-apple` | `hevc_xcr8`; this wrapper alias is **not** automatic VideoToolbox acceleration. |
| `--enc_config_table PATH` | Load/add/override profiles from CSV. See 1.7. |
| `--mask-fill min\|taper3` | Default `taper3` for shared GRIB bitmaps. `min` is the constant-fill alternative. |
| `--threads N`, `-threads N` | Override FFmpeg thread count; 0 leaves the encoder default. Record thread policy for timings. |
| `--headers_only_scan` | Default: scan headers first and defer template extraction. |
| `--no_headers_only_scan` | Extract templates eagerly during scanning. |
| `--fast-path`, `--fast_path` | Enable packed-integer fast path; disabled by default. Validate separately before using in comparative results. |
| `--no_fast_path`, `--no-fast-path` | Disable that fast path. |
| `--metadata-version 1\|2` | Payload format; default 2, compact binary metadata. Preserve decoder compatibility. |
| `--metadata-v1`, `--metadata-v2` | Metadata-version shortcuts. |
| `--list-options` | Show resolved profile definitions, including custom CSV definitions when supplied. |
| `--verbose`, `-v` | Progress, resolved command/configuration and detailed statistics. Preserve stderr when timing. |
| `--help`, `-h` | Usage and profiles. |
| `--version` | Common native version flag. |

All five main shortcuts use `auto` range selection. This does not mean they
have identical codecs, bit depths, chroma layout or prediction settings.

### Built-in profile catalogue

The following is a 1.0.0 snapshot. Run `grib2avenc --list-options` for the local
build; an available profile is not proof that FFmpeg supports its requested
format. Quality words here are encoder configuration families, not error bounds.

| Name | Codec | Quality | Bits | Range | Chroma | Encoder |
| --- | --- | --- | ---: | --- | --- | --- |
| `avc_xcr8` | avc | verylow | 8 | auto | 420 | `libx264` |
| `avc_fast10` | avc | fast | 10 | auto | 420 | `libx264` |
| `avc_good8` | avc | average | 8 | auto | 420 | `libx264` |
| `avc_vgood10` | avc | high | 10 | auto | 420 | `libx264` |
| `avc_vgood12` | avc | veryhigh | 12 | auto | 420 | `libx264` |
| `hevc_xcr8` | hevc | verylow | 8 | auto | 420 | `libx265` |
| `hevc_fast10` | hevc | fast | 10 | auto | 420 | `libx265` |
| `hevc_good10` | hevc | average | 10 | auto | 420 | `libx265` |
| `hevc_hq10` | hevc | high | 10 | auto | 420 | `libx265` |
| `hevc_vgood12` | hevc | veryhigh | 12 | auto | 420 | `libx265` |
| `av1_xcr10` | av1 | verylow | 10 | auto | 420 | `libsvtav1` |
| `av1_fast` | av1 | fast | 10 | auto | 420 | `libsvtav1` |
| `av1_good10` | av1 | average | 10 | auto | 420 | `libsvtav1` |
| `av1_hq10` | av1 | high | 10 | auto | 420 | `libsvtav1` |
| `av1_vgood12` | av1 | veryhigh | 12 | auto | 420 | `libsvtav1` |
| `av1_lossless12_frameMinMax_400` | av1 | lossless | 12 | frameMinMax | 400 | `libaom-av1` |
| `av1_lossless12_auto_400` | av1 | lossless | 12 | auto | 400 | `libaom-av1` |
| `av1_good10_auto_400` | av1 | average | 10 | auto | 400 | `libsvtav1` |
| `vp9_hq10_auto_420` | vp9 | high | 10 | auto | 420 | `libvpx-vp9` |
| `vp9_fast8` | vp9 | fast | 8 | auto | 420 | `libvpx-vp9` |
| `vp9_fast10` | vp9 | fast | 10 | auto | 420 | `libvpx-vp9` |
| `vp9_xcr10` | vp9 | verylow | 10 | auto | 420 | `libvpx-vp9` |
| `vp9_bal8` | vp9 | average | 8 | auto | 420 | `libvpx-vp9` |
| `vp9_bal10` | vp9 | average | 10 | auto | 420 | `libvpx-vp9` |
| `vp9_good10` | vp9 | average | 10 | auto | 420 | `libvpx-vp9` |
| `vp9_bal12` | vp9 | average | 12 | auto | 420 | `libvpx-vp9` |
| `vp9_hq10` | vp9 | high | 10 | auto | 420 | `libvpx-vp9` |
| `vp9_hq12` | vp9 | high | 12 | auto | 420 | `libvpx-vp9` |
| `vp9_vgood12` | vp9 | high | 12 | auto | 420 | `libvpx-vp9` |
| `vp9_uhq12` | vp9 | veryhigh | 12 | auto | 420 | `libvpx-vp9` |
| `vp9_lossless12` | vp9 | lossless | 12 | auto | 420 | `libvpx-vp9` |
| `av1_good10_frameMinMax_400` | av1 | average | 10 | frameMinMax | 400 | `libsvtav1` |
| `vp9_hq10_frameMinMax_400` | vp9 | high | 10 | frameMinMax | 400 | `libvpx-vp9` |
| `av1_good10_groupMinMax_400` | av1 | average | 10 | groupMinMax | 400 | `libsvtav1` |
| `av1_fast_groupMinMax_400` | av1 | fast | 10 | groupMinMax | 400 | `libsvtav1` |
| `vp9_hq10_groupMinMax_400` | vp9 | high | 10 | groupMinMax | 400 | `libvpx-vp9` |
| `av1_good10_frameMinMax_420` | av1 | average | 10 | frameMinMax | 420 | `libsvtav1` |
| `vp9_hq10_frameMinMax_420` | vp9 | high | 10 | frameMinMax | 420 | `libvpx-vp9` |
| `av1_good10_groupMinMax_420` | av1 | average | 10 | groupMinMax | 420 | `libsvtav1` |
| `vp9_hq10_groupMinMax_420` | vp9 | high | 10 | groupMinMax | 420 | `libvpx-vp9` |

## 1.6 Quantization, missing values and logarithmic scaling

### Numerical ranges

Scientific values are quantized before video coding. With linear scaling, an
approximate unmasked quantization interval is `(max - min) / (2**bits - 1)`;
reserving a missing-value code changes the number of finite intervals. The
half-interval rounding bound excludes subsequent conversion and codec errors.

- `frameMinMax`: source-derived minimum/maximum per field. Finer resolution for
  a narrow individual frame, but integer scales can change from frame to frame.
- `groupMinMax`: one common range across a scientific stream. A consistent
  numerical scale can help video prediction; a single broad-range field can
  coarsen quantization of the others.
- `auto`: frame ranges for groups spanning multiple levels; group ranges for
  single-level streams. It follows grouping, not a fitted error tolerance.

One MP4 may contain both policies. Inspect `avenc2grib --list` and per-record
metadata instead of inferring the actual range from a filename. A pressure
variable with large baseline values can have a small fractional range but still
have physically significant absolute reconstruction errors.

To isolate range choice, hold source selection, codec, bit depth and quality
fixed. For example, compare `av1_good10_frameMinMax_400`,
`av1_good10_groupMinMax_400` and `av1_good10_auto_400`. An auto single-level
stream can resolve to groupMinMax, so coincident aliases should not be counted
as independent experiments.

### Bitmaps and boundary filling

The actual GRIB bitmap is the authority for missingness. Do not infer that a
legitimate value equal to 9999 or zero is missing. During the scan, bitmaps are
identified and checked for sharing within groups. Shared masks are stored
compactly and reapplied on reconstruction. Fields with changing/incompatible
masks are not eligible for shared-mask Taper3 processing.

Default `taper3` fills only masked video cells: extend nearest valid neighbours
into a short halo and blend progressively towards the lower bound. This reduces
an artificial sharp edge compared with a constant minimum. It does **not** alter
the source valid-data mask or guarantee that lossy predictions leave adjacent
valid cells untouched. `--mask-fill min` selects constant fill. Record the choice
when comparing older and newer encodings; replacing a file invalidates its old
size, error and timing records.

Inspect error maps near coastlines, terrain/mask edges, zeros and isolated
valid cells. A lower global RMSE does not rule out a local boundary artifact.

### Precipitation, radiation and logarithmic candidates

For strongly skewed nonnegative fields, a reversible, documented log transform
is worth testing against linear scaling. Test both low values and extremes,
wet/dry classifications, thresholds and integral/accumulation diagnostics.
A log choice that helps light precipitation can hurt peak amounts or totals.
Select a dimensional scale/offset explicitly for any custom transformation.

**Current native support is narrow:** `src/avpack.cpp` applies `log2(value + 1)`
(with nonnegative clipping) only to lowercased short names `rdp` and `precip`.
There is no general native `--log`, `--apply-log2` or CSV transform column.
ERA5 `tp` or `ssrd` does not trigger this rule merely because it is precipitation
or radiation. An arbitrary FFmpeg option cannot change scientific scaling metadata.
Check the actual `log2` flag in `--list` and the stored transform fields.

If a user wants a linear-versus-log experiment on another GRIB parameter:

1. Report the native interface limitation rather than inventing a flag.
2. Use the separate Python array interface only if its format and validation
   path fit the task; it exposes `apply_log2`, but its implementation is not the
   native physical `log2(x+1)` path. Check numerical behaviour on a tiny known array.
3. A custom preprocessing/encoder change must preserve the original variable
   identity, inverse transform, masks and reference values. Agree on that expanded
   scope; do not relabel a variable `precip` to trigger hidden behaviour.
4. Compare decoded **physical values** against the original, not against log
   values, and include any additional reconstruction metadata in storage accounting.

Cumulative fields can reset each forecast cycle/day. Preserve `stepType`, start
and end steps, units and reset times. Differencing to hourly increments changes
the representation; do it only as a separately defined reversible experiment,
retaining the reconstruction anchors. Do not assume a codec will automatically
insert a B-frame/keyframe at the reset or infer the field's accumulation semantics.

## 1.7 Advanced packer, custom CSV and hardware

Use `grib2avenc` for ordinary GRIB workflows. Use `avpack` when explicit grouping,
quantization, chroma or FFmpeg tokens are required. **`avpack --preset` names do
not map to the `grib2avenc` shortcuts.** In particular, `avpack --preset apple`
requests VideoToolbox, while `grib2avenc --apple` is the software profile alias
listed above. Prefer explicit advanced settings in reproducible experiments.

The lower-level preset mapping is:

| avpack preset | Codec / quality / scientific bits | Explicit encoder override |
| --- | --- | --- |
| `low` | AVC / average / 8 | None |
| `medium` | HEVC / average / 10 | None |
| `high` | HEVC / high / 12 | None |
| `fast` | AVC / fast / 8 | None |
| `apple` | HEVC / compatible / 10 | `hevc_videotoolbox` |

The preset is applied after parsing and overwrites its codec/quality/bit choices.
Do not combine it with contradictory explicit settings and assume the last
command-line argument wins.

### All avpack options

| Option | Values / use |
| --- | --- |
| `--out PATH` | Required output MP4; inputs are positional local GRIB files. |
| `--group-by MODE` | `auto`, `var`, `var-level`, `level`; default `auto`. Inspect resulting groups and compatibility. |
| `--preset NAME` | `low`, `medium`, `high`, `fast`, `apple`; lower-level preset semantics, not wrapper profile names. |
| `--codec NAME` | `avc`, `hevc`, `av1`, `vp9`; default `hevc`. |
| `--quality NAME` | `lossless`, `veryhigh`, `high`, `average`, `fast`, `verylow`, `compatible`, `multiset`; default `high`. |
| `--bits N` | `8`, `10`, `12`, `16`; default 12. Not every codec/pixel format supports every value. |
| `--quantization MODE` | `auto`, `frameMinMax`, `groupMinMax`; default `auto`. |
| `--mask-fill MODE` | `min`, `taper3`; shared-bitmap default `taper3`. |
| `--chroma MODE` | `400` grayscale or `420` luma with neutral chroma; default `420`. 420 rejects scientific bits above 12. |
| `--encoder NAME` | Explicit FFmpeg implementation, e.g. `libx265`, `libaom-av1`, `hevc_nvenc`, `hevc_videotoolbox`. |
| `--ffopt TOKEN` | Repeat once per raw FFmpeg output token, including separate values. Not a whole shell command. |
| `--force-crf N` | Override generated CRF; nonnegative integer, otherwise omitted. Meaning/support depends on encoder. |
| `--threads N`, `-threads N` | FFmpeg thread override; default leaves encoder choice. |
| `--headers_only_scan`, `--no_headers_only_scan` | Deferred template extraction (default) versus eager scan. |
| `--fast-path`, `--fast_path` | Enable packed-integer optimization; default off. |
| `--no-fast-path`, `--no_fast_path` | Disable packed-integer optimization. |
| `--metadata-version 1\|2`, `--metadata-v1`, `--metadata-v2` | Default v2; use v1 only for an identified compatibility need. |
| `--verbose`, `-v` | Detailed output. |
| `--quiet`, `-q` | Suppress ordinary progress. |
| `--help`, `-h`, `--version` | Discovery/version. |

For example, compare grouping with explicit common settings:

```bash
avpack --out grouped.mp4 --group-by var-level --codec hevc --quality high --bits 12 --quantization auto --chroma 420 calibration.grib
```

Scientific values use luma, not chroma. 4:2:0 does not spatially subsample those
values. Do not add multi-variable RGB/chroma packing and assume the same schema.

### Custom CSV structure

The first six columns are `name,note,codec,quality,bits,encoder`. Subsequent
cells containing `auto`, `frameMinMax`, `groupMinMax`, `400` or `420` set those
properties; other cells become individual FFmpeg option tokens. Blank tokens
are skipped. Quote cells containing commas. Put the header on the first line.
Rows override built-ins with the same name; retain the exact CSV used.

```csv
name,note,codec,quality,bits,encoder,quantization,chroma,option1,value1
hevc_trial,Controlled HEVC trial,hevc,average,10,libx265,auto,420,-crf,30
```

```bash
grib2avenc --enc_config_table profiles.csv --list-options
grib2avenc --verbose --enc_config_table profiles.csv --enc hevc_trial calibration.grib hevc_trial.mp4
```

A CSV changes the encoder configuration, not the source identity or arbitrary
physical transform. Extra tokens augment generated FFmpeg settings; inspect
verbose resolved commands for conflicting/overridden options. `--ffopt` belongs
to `avpack`, not to the `grib2avenc` option parser.

### Hardware choices

Check FFmpeg's compiled encoders, pixel formats and an actual available device.
Listing NVENC is not proof of an allocated GPU, suitable driver or working
codec session. On a cluster, follow its scheduler/allocation instructions;
do not assume the login node has GPU access.

For an authorized hardware calibration, explicitly select the implementation
(e.g. `hevc_nvenc`), 10-bit precision and 4:2:0, then inspect the negotiated
pixel format (often `p010le`) and device utilization. NVENC quality controls
such as CQ and preset p1–p7 are not software CRF controls. Query them with
`ffmpeg -hide_banner -h encoder=hevc_nvenc`. Pass each verified option/value
as separate CSV cells or `--ffopt` tokens.

The standard High profile requests 12-bit HEVC software settings; it is not
a generic “use my GPU” switch. A 10-bit hardware experiment is a different
configuration and needs its own fidelity check. For a target storage ratio,
bitrate can be calibrated using duration and complete-container size, but
fixed CQ/preset alone does not guarantee 100×. A high bitrate reduction can
introduce rare large errors even if typical errors remain low.

Measure CPU and hardware runs separately. Keep thread/device/FFmpeg settings,
input hashes, complete-file sizes, wall/CPU times and memory. Do not label GPU
availability as measured energy savings or assert that all decoding is GPU
accelerated; the normal subset decoder need not use a hardware decoder.

## 1.8 Compare errors and inspect the extrema

Prefer `avenc-compare` to the historical positional `ComputeError` workflow.
It matches scientific identities, not message order. The original side is a
local GRIB file or a directory of immediate GRIB files; the candidate is a
local GRIB-packed AVENC MP4, GRIB file, or equivalent GRIB directory. Directory
inputs are nonrecursive and accept `.grib`, `.grb`, `.grib2`, `.grb2`, not an
arbitrary glob of every file. Do not assume native comparison accepts HTTP URLs.

For multiple original files, use a clean source directory directly:

```bash
avenc-compare --full source/ compressed.mp4 --json errors.json --csv groups.csv --messages-csv fields.csv --top 10
```

Or concatenate them without changing GRIB messages:

```bash
cat source/*.grib > reference.grib
avenc-compare --full reference.grib compressed.mp4 --json direct-errors.json
```

When the user's operational path writes GRIB, evaluate that path as well:

```bash
avenc2grib compressed.mp4 reconstructed.grib
avenc-compare --full reference.grib reconstructed.grib --json repacked-errors.json
```

Keep the original until the workflow succeeds; remove temporary reconstructions
only if they are disposable outputs in the user's scope. Direct and repacked
results can be close but are not defined to be identical.

### Reading the error report

Check matching/missing/duplicate counts before metrics. Verify bitmap mismatch
counts explicitly: finite-pair RMSE can look good even when a mask changed.
For each variable/level/grid (and member if requested), review:

- RMSE, MAE, signed bias, error standard deviation and P95/P99 absolute errors;
- maximum absolute error, reference/decoded extrema and extrema shifts;
- spread/variance ratios, correlation and normalized errors;
- exact versus reservoir-estimated quantiles and examined coverage;
- the top fields for RMSE, normalized error, maximum error, bias and variance change.

The error sign is **candidate minus original**. Some historical article plots
use a different sign; label it explicitly when reusing cached plotting scripts.
A maximum absolute error is not the same as the difference between the two field
maxima, which may occur at different locations.

JSON `worst_cases` entries are indices into `messages`, not complete field
objects. Every message carries its scientific coordinates, original path/message,
MP4 `stream`/`frame` where applicable, and `max_error_point` in grid scan order.
This example locates the highest finite absolute-error field in the report:

```python
import json

with open("errors.json") as handle:
    report = json.load(handle)

ranked = report["worst_cases"]["max_abs_error"]
fields = [report["messages"][index] for index in ranked]
fields = [field for field in fields if field["metrics"]["max_abs_error"] is not None]
if not fields:
    raise ValueError("No finite error in the reported worst fields")
worst = max(fields, key=lambda field: field["metrics"]["max_abs_error"])
print(worst["short_name"], worst["units"], worst["coordinates"])
print(worst["reference_path"], worst["reference_message"])
print(worst["stream"], worst["frame"], worst["max_error_point"])
print(worst["metrics"]["max_abs_error"])
```

Within a group summary, `max_error_point` belongs to the field identified by
`max_error_identity`. Do not assign it to the first field of the group. On a
verified regular row-major grid, `row, column = divmod(point_index, Ni)`;
use the source scanning flags/coordinates to convert this into a geographic
location. Do not assume all grids scan north-to-south or use the same longitude
convention. Retain original field identity, valid time, member and level.

### Plots to diagnose spurious values

Extract only the selected fields/windows initially and cache the numerical data
so styling does not re-read the archive. A useful diagnostic set is:

1. Reference and decoded fields with common colour limits, plus signed/absolute
   error maps. On a logarithmic absolute-error colour scale, mask exact zeros.
2. A zoom around the maximum-error point, including coastlines/mask edges and
   missing cells. Show reference and decoded values numerically at that point.
3. Its time series over a meaningful period and a short zoom around the event,
   matching level/member and cadence exactly.
4. Error distributions with typical error, P95/P99 and maximum. If logarithmic
   bins are unequal, density is `count / (N * bin_width)`, not percent per bin.
5. The derived diagnostic that motivated the request: wind speed/direction,
   threshold maps, gradients, spectra, ensemble spread or model-input constraints.

For U/V, derive wind speed independently after decoding. A useful vector error
is `sqrt((u_dec-u_ref)**2 + (v_dec-v_ref)**2)`; it differs from absolute wind-speed
error. Relative pointwise error can explode near zero: use a stated physical
threshold, a documented denominator mask, or both. Do not silently add an epsilon
and imply the resulting percentages are physically meaningful.

A worst-error point selected from the candidate is an intentional stress test,
not a representative sample. A high-reference-spread ensemble point must be
selected from the reference alone if claimed as such. Full-grid maxima need
full comparisons; a ten-frame plot cannot supply a whole-month maximum.

## 1.9 All avenc-compare options and metric definitions

| Option | Meaning / default |
| --- | --- |
| `--fast` | Aggressive reference-selected screening: 7 chronological-stratum fields/group, 8192 points/field, quantile cap 16384; hashes off unless requested. |
| `--full` | Every matched field/point. Mutually exclusive with `--fast`. Quantiles can still be capped. |
| Neither flag | Sampled mode: up to 8 fields/group and 10000 points/field. **Not full.** |
| `--frames-per-group N` | Field budget in sampled modes; explicit setting overrides fast defaults. A “field” preserves member/level/time identity. |
| `--points-per-frame N` | Sampled spatial budget, bounded by available points. Explicit setting overrides fast default. |
| `--seed N` | Deterministic seed, default 5489. Use the same reference/seed/budgets across profiles. |
| `--group-by-member` | Separate member summaries/sampling groups; matching always preserves member identity even without this flag. |
| `--quantile-cap N` | Reservoir cap per group and per field, normally 200000; fast default 16384. `0` retains all finite pairs for exact quantiles with potentially large RAM use. |
| `--decode-batch N` | Maximum consecutive MP4 frames per read, default 8; affects buffering/work, not scientific identity. |
| `--allow-unmatched` | Compare only the intersection, explicitly recording missing/extra identities. Default rejects unmatched fields. Not a way to hide incomplete input. |
| `--duplicate-policy first` | Explicitly retain first duplicate identity; default rejects duplicates. Does not prove duplicate values are equal. |
| `--hash` | SHA-256 source provenance, including in fast mode. |
| `--no-hash` | Skip SHA-256; sizes/mtimes still checked. Normally hashes are enabled outside fast mode. |
| `--json PATH` | `avenc.compare.v1`: configuration, matching, provenance, group/message metrics, worst cases, times/RSS. |
| `--csv PATH` | Scientific-group metrics. |
| `--messages-csv PATH` | Metrics and identities for selected fields. |
| `--top N` | Worst fields per criterion, default 5. |
| `--help`, `-h`, `--version` | Discovery/version. |

Full mode examines masks/moments/extrema across all matched values. Sampled
mode reports those properties only for examined points. Reservoir limits affect
quantiles, not the maximum over examined pairs. Avoid `--quantile-cap 0` on a
large month/ensemble unless the memory cost is acceptable; each retained pair
stores three doubles. Inspect `quantiles_exact` and `quantile_sample_size`.

| Metric | Definition / interpretation |
| --- | --- |
| RMSE / MAE / bias | Physical units, per scientific group; bias uses decoded minus reference. |
| Relative RMSE | RMSE / reference RMS, not a per-cell relative bound. |
| Range NRMSE | RMSE / (reference max − reference min). |
| Robust NRMSE | RMSE / (reference q99 − reference q01); quantile denominator may be estimated. JSON key `nrmse_q01_q99`. |
| `snr_rms_db` | `20 log10(reference RMS / RMSE)`. A large mean can make this generous. |
| `snr_std_db` | `20 log10(reference population std / RMSE)`, relative to signal variability. |
| PSNR | `20 log10((reference max − min) / RMSE)`. |
| std / variance ratios | Decoded/reference population spread across the examined group/field values, not pointwise ensemble spread. |
| `global_normalized_summary` | Equal-group mean of defined robust NRMSEs, with contributing-group count. Not a pooled physical-unit RMSE or the article's distinct variable-balanced score. |

Population standard deviations use `ddof=0`. Zero denominators are not patched
with arbitrary epsilons: undefined values are JSON `null` / blank CSV cells.
Perfect reconstruction has a separate `perfect_on_finite_pairs` marker; infinite
dB values can also be null. A null SNR is therefore not automatically a failure.
Do not pool pressure, temperature, wind and humidity in dimensional RMSE.

Identity matching includes parameter, units, level/layer, step semantics,
initialization and forecast interval, ensemble/probability keys, point count
and ecCodes grid-section MD5. No regridding occurs. A changed GRIB edition/grid
section can fail strict identity matching despite similar geographic coordinates;
investigate the source rather than matching unrelated fields by position.

Timing fields distinguish indexing/hashing from selected full-field value reads.
MP4 reads include subprocess/metadata work. Peak parent RSS and largest child
RSS are separate, not a simultaneous process-tree total. `avenc-compare` does
not supply encoder wall time; log encoding separately. These are single-run
measurements without guaranteed cold-cache conditions, not matched point-read
or GPU-throughput benchmarks. See [avenc-compare.md](avenc-compare.md) for details.

## 1.10 Direct Python array encoding

Use this when a user already has a three-dimensional DataArray and does not
need the native GRIB-packed schema. There is no generic `Dataset.to_avenc()`
writer that automatically handles multiple arbitrary datacubes as one native
multi-stream GRIB archive. Do not promise a Zarr-compatible writer for all
xarray datasets merely because the reader supports multiple GRIB streams.

```python
import numpy as np
import xarray as xr
from avenc_xarray import compress_dataarray, decompress_to_dataarray

values = np.arange(4 * 32 * 64, dtype=np.float32).reshape(4, 32, 64)
field = xr.DataArray(values, dims=("time", "lat", "lon"), name="example")
summary = compress_dataarray(
    field, "array-package", codec="hevc", quality="high", bits=12,
    bitmask_enabled=False,
)
restored = decompress_to_dataarray("array-package")
print(summary.video_path)
```

The output argument is a **package directory**, normally containing `payload.mp4`,
not a plain target video pathname. The helper stores shape/dims/coordinates and
attributes in its separate `avenc.v1` schema. It materializes contiguous Float32
values, so Float64-to-Float32 error counts against original data. It is not a
streaming Dask encoder. Constant/all-nonfinite arrays are rejected by the helper.

### Python encoder keyword reference

| Keyword | Default / meaning |
| --- | --- |
| `bits` | 12; supported requests 8, 10, 12, 16, subject to encoder format. |
| `codec` | `hevc`; separate codec family selection. |
| `quality` | `high`; extension vocabulary includes `lossless`, `veryhigh`, `high`, `average`, `low`, `verylow`, `fast`, `compatible`, `multiset`. Support is codec-specific; these are not native CLI shortcut names. |
| `encoder` | `None`; explicit FFmpeg encoder override when supported. |
| `apply_log2` | `False`; separate array-encoder nonlinear path, not native `rdp`/`precip` scaling. Verify on known values before scientific use. |
| `allow_nan` | `None`, infer from NaNs; codepoint handling is not proof of an exact changing mask under lossy coding. |
| `frame_rate` | 24; video timing, not a replacement for scientific time coordinates. |
| `flip_ud` | `False`; video row-order handling. Verify coordinate alignment. |
| `min_value`, `max_value` | `None`, infer global finite bounds; explicit bounds can clip out-of-range values. |
| `video_name` | `None`, normally `payload.mp4` in the output directory. |
| `bitmask_enabled` | `True`; separate special-value bitmap, not the authoritative native GRIB bitmap mechanism. |
| `bitmask_value` | `0.0`; uses `np.isclose`, so near-zero values can be restored as exact zero. Set `bitmask_enabled=False` when that is inappropriate. |
| `preview_lut` | `None`; optional separately generated colour preview. |
| `preview_codec` | `libvpx-vp9`. |
| `preview_output` | `None`, default preview filename chosen by helper. |
| `preview_crf` | 10; preview encoding control. |
| `preview_alpha_threshold` | 10; preview alpha handling. |
| `color_encode` | `False`; optional colour-mapping path, not a default scientific recommendation. |
| `color_lut` | `None`; optional LUT. |
| `color_distribution` | `linear`; LUT distributions include `log`, `sqrt`/`square_root`, `cuberoot`, `weibull` or `weibull:k` (default shape 1.5). Unknown modes currently fall back to linear. These are colour mappings, not native scientific log scaling; verify reconstructed values. |

Input must have three dimensions including `time`. Put it explicitly in
`(time, y, x)` or `(time, lat, lon)` order. For a 2D field, adding one time/frame
can be appropriate; preserve that original layout. Flattening levels/members
into frames requires a separately recorded reversible mapping. Arbitrary 4D/5D
Dataset writing is not implemented by this function.

For this schema, compare original and restored arrays directly with aligned
coordinates, dtype, masks and physical-unit metrics. Native `avenc-compare`
is not its validator. Test odd dimensions, NaNs, near-zero values, signed inputs,
constant-field handling, clipping and any nonlinear path on small known arrays.
Do not describe a preview video as an independent scientific reconstruction.

# Part 2 — Building a dataset

## 2.1 Grouping and file organization

Decide the scientific unit of an MP4 using the intended access pattern. One file
per variable/year, one per forecast initialization, and one multi-variable forcing
sequence can all be reasonable. Do not pick a single huge file solely to improve
one ratio, or one file per frame solely to avoid temporal dependencies.

For example, combine compatible source files into one output:

```bash
grib2avenc --high source/2025_01.grib source/2025_02.grib wind.high.mp4
grib2avenc --compact source/*.grib forcing.compact.mp4
```

The wrapper uses variable-oriented grouping with member and grid compatibility.
A parameter can occupy multiple streams; one stream can contain multiple levels.
At the advanced level, `var` separates parameter/level type/member/grid;
`var-level` also separates the individual level. `level` can group different
parameters at the same level, so use it only after checking their numerical
compatibility and preserved identities.
Stream order is not a reliable member index or variable name. `avpack --group-by
var-level` can separate levels explicitly when that is the actual experiment.
Inspect each resulting stream and validate message identities after changing
organization. Reordering can change video prediction and compression efficiency.

Scientific validity times come from stored GRIB descriptors, not an MP4's display
frame rate. Preserve cycle/lead/valid time, accumulation intervals, ensemble
identity, level units, scan order, latitude/longitude convention and padding/crop
information. Keep U/V corresponding fields alignable when users need wind speed.
Do not combine different forecast cycles and then silently interpret frame order
as a single uniformly spaced forecast trajectory.

Choose storage layout with these trade-offs explicit:

| Need | Dataset-design implication |
| --- | --- |
| Many short remote time windows | Compare shorter GOPs/file partitions against storage overhead using measured access requests. |
| Complete sequential processing | Longer coherent streams may exploit temporal prediction; validate fidelity and memory. |
| Per-variable services | Separate variable objects or retain clear multi-stream metadata; do not require clients to guess stream IDs. |
| Ensembles | Retain exact member identities in metadata, including deterministic versus perturbed/control origins. |
| Mixed vertical levels | Inspect auto's frame-wise range choice and any benefits/costs of separate level streams. |
| Static land mask | Shared bitmap representation/default taper may help; verify sharing rather than presuming it. |
| Changing masks | Preserve per-record validity; do not force an invalid shared mask to improve the ratio. |

An MP4 is multi-stream and includes its reconstruction metadata. Do not use
ordinary FFmpeg remux/edit commands that drop metadata and assume the result
remains a scientific AVENC object. Use `avenc2grib --group ... input.mp4
subset.mp4` for supported whole-stream extraction and re-inspect the result.
This is not an arbitrary time/crop MP4 editor.

## 2.2 Archive and publication records

Keep four kinds of artifact distinguishable:

- immutable reference GRIBs or precise retrieval recipes/checksums;
- compressed objects with exact encoder configurations;
- comparison/resource records tied to those exact objects;
- extraction/plot scripts and numerical caches, tied to the reported figures.

A practical local working layout is:

```text
experiment/
  source/          reference GRIB files only
  compressed/      distinct profile/configuration MP4s
  reports/         fast/full JSON, group CSV, field CSV
  provenance/      hashes, versions, commands, resource logs
  plots/           extraction scripts, small caches, PNG/PDF
```

Avoid overwriting an MP4 while keeping an old report with the same name. Use a
manifest that maps scientific case/profile to filenames, file hashes and reports.
Count aliases as aliases. Record the underlying encoder, precision, negotiated
pixel format, range mode, bitmap/fill handling, custom CSV, FFmpeg tokens, GOP,
threads and hardware. Store failed configurations separately with their actual
failure, not as successful measurements or silent substitutions.

For resources, retain encoding and decoding wall/CPU times separately and state
what they include: source scan, metadata, numerical conversion, codec work,
GRIB repacking and disk writes. Preserve compression progress on stderr rather
than redirecting it into an unreadable timing file. If structured JSON logs
already exist, reuse them; name the command and accounting method. On Linux,
`/usr/bin/time -v -o resources.txt ...` separates resource logging from the child
progress stream. Distinguish peak individual-process RSS from a simultaneous
process-tree peak. Do not compare differently scoped timings as one benchmark.

For an HTTP-access archive, publish MP4s as individual objects, not only inside
ZIPs. Validate the actual public endpoint after upload. Retain licences and
provider attribution, a citation/DOI, SHA-256 checksums and a downloadable file
manifest. Server Range/CORS behaviour is independent of the dataset licence.
No publication is implied by merely preparing these files.

Concatenating GRIB messages can reduce file-count overhead without recoding
their values. Preserve the ordered source list, per-file hashes and byte offsets
if exact original-file recovery matters. Scientific extraction with `grib_copy`
need not reproduce padding or original byte boundaries; document which kind of
restoration is provided. Do not concatenate MP4 bytes to merge AVENC datasets.

### Published AVENC reference examples

The public dataset is [Zenodo 23502851](https://doi.org/10.5281/zenodo.23502851).
Use it for authorized demonstrations without downloading the entire record:

| File family | Scientific contents |
| --- | --- |
| `ERALANDWIND.*.mp4` | February 2022 ERA5-Land U10/V10, 672 hourly frames per component, 3600×1801 grid. In the Compact file, `10u` is stream 0 and `10v` stream 1. |
| `LAMBC.*.mp4` | Heterogeneous ECMWF forcing, multiple parameters/levels, initialization 30 September 2026; seven forcing leads +6 to +24 h. |
| `ENSEMBLE.*.mp4` | U10/V10/T2m, +0–144 h every 3 h. This specific collection intentionally combines 50 perturbed ENS members with the deterministic operational forecast; its historical “control” filename is not an ENS control. |

Individual MP4s support the CLI/Python examples below. The original source
GRIBs, merged source collections, error ZIPs, reproduction bundle and model
case/results are separate archive objects. Read its manifest/README for actual
names; do not invent an unlisted variable or medium ensemble file.

Examples are also available at [filippi.github.io/avenc](https://filippi.github.io/avenc/).
The paper's observations illustrate dataset-specific choices, not universal
profile tolerances. In particular, small typical wind errors can coexist with
large localized errors or altered high-frequency variability under Compact.

## 2.3 Application-specific validation

### Forcing a limited-area model

Validate every parameter/level and the required thermodynamic/geometric fields,
including pressure/geopotential/humidity masks and admissible ranges. A small
numerical perturbation can cause a preprocessing/solver problem even if other
profiles work. Do not attribute a model failure to a codec bug solely from a
pressure-solver traceback. First verify field identity, finite values, masks,
extrema and physical constraints, then isolate which field/path triggers it.

If a workaround restores an original field such as `lnsp`, retain that exact
patch/provenance and account for it in the experiment. Do not claim all forcing
fields were compressed when some were restored. Model-output rainfall or
spectra are downstream sensitivity tests, not direct codec-error metrics.
A diffusion perturbation is one stated comparison, not an estimate of all
model-configuration uncertainty. No observations means no forecast-skill claim.

### Ensemble diagnostics

Match member, variable, level, grid and lead before computing U/V speed. Compute
reference spread and reconstruction errors independently at each point and lead:

```text
spread_ref(x,y,t) = std_member(X_ref, ddof=0)
compression_RMSE(x,y,t) = sqrt(mean_member((X_dec-X_ref)^2))
relative_perturbation = compression_RMSE / spread_ref
spread_change = std_member(X_dec, ddof=0) - spread_ref
```

Mask near-zero reference spread using a stated physically meaningful threshold.
Report valid coverage as well as ratio statistics. No Gaussian assumption is
needed. Summarize pointwise mean absolute spread change and its maximum with the
worst location/time; signed averages can cancel. `avenc-compare` pooled std is
not this ensemble calculation and does not compute these diagnostics for you.

For representative plumes, select points using a reproducible reference-only
criterion, such as largest mean later-lead spread away from boundaries. Plot
reference quantile bands and common reference boundaries with decoded medians,
quantile differences and spread differences. Keep an intentionally candidate-
selected worst case labelled as such.

For threshold-probability maps, choose a stated physical threshold or a documented
reference-derived threshold, not one optimized to hide errors. Compare member
exceedance counts directly. With N members, one net count difference is
`100/N` percentage points; use discrete absolute-error classes when helpful.
A zero net probability change does not imply no individual member switched
classification. These tests assess preservation of probabilistic information,
not skill against observations.

### Accumulations, energy and spectra

For precipitation/radiation, inspect reset times, totals, extrema and wet/dry or
other scientifically relevant thresholds. Do not infer water/energy conservation
from low field RMSE. A local `(u**2 + v**2)/2` time integral is a kinetic-energy
proxy, not a domain-wide closed atmospheric budget.

If temporal variability matters, keep the actual cadence/valid times, missing
intervals, detrending, Welch window, overlap and PSD normalization explicit.
A temporal point spectrum is not a spatial energy cascade; a spectral ratio can
look large where reference spectral energy is nearly zero. Plot absolute spectra
alongside ratios. Cache extracted series and avoid re-reading large MP4s during
styling. A small targeted scientific diagnostic is preferable to unrelated
additional plots with no acceptance question.

# Part 3 — Accessing data

## 3.1 Inspect before decoding

Begin with metadata, especially for an unfamiliar multi-stream file:

```bash
avenc2grib --list input.mp4
avenc2grib --head input.mp4
avenc2grib --stats input.mp4
avenc2grib --group 'stream:0' --list input.mp4
```

`--list` summarizes groups/streams or lists selected records; `--head` prints
metadata details; `--stats` reports metadata/storage/parsing information, not
source-versus-decoded error. Choose one information mode per invocation.
Inspect group aliases before using a short-name selector; multiple levels/members
may match one parameter. Stream IDs apply only to that object.

For remote examples, use the actual public file URL without shell-escaped colons:

```bash
URL=https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4
avenc2grib --http-stats --no-http-fallback --list "$URL"
```

The `URL` variable is reused below. Quoting is sufficient; `https\://` is
unnecessary. Inspecting a file is much cheaper than deliberately reconstructing
all its fields, but do not invent metadata-only latency measurements.

## 3.2 GRIB reconstruction and stream extraction

Full reconstruction:

```bash
avenc2grib input.mp4 reconstructed.grib
avenc2grib "$URL" uv.grib
```

The second command reconstructs the **entire** remote archive into GRIB. Check
disk space and the user's intended scope first. An approximately 10 MB input
can expand to gigabytes of GRIB or a much larger logical Float32 dataset.

Whole-stream reconstruction or MP4 extraction:

```bash
avenc2grib --group 'stream:0' input.mp4 u10.grib
avenc2grib --group 'stream:0' input.mp4 u10.mp4
```

A positional `.mp4` output extracts selected encoded streams and updates the
scientific metadata. It does not request a new lossy encoding. Do not combine
Float32 subset selectors with a positional GRIB filename and expect cropped
GRIB output: subset mode is a different output path.

GRIB reconstruction uses an internal page-size budget, default 8 GiB. On a
memory-limited machine, reduce it, retaining headroom for codec/ecCodes buffers:

```bash
avenc2grib --page-size 256MiB input.mp4 reconstructed.grib
```

This is a page budget, not a guaranteed whole-process memory ceiling. At least
one frame and decoder working state still need to fit in memory.

## 3.3 Float32 windows

Select a **single stream**, zero-based frame range and pixel window, then redirect
stdout. Do not pass a positional output filename or `--group` in this mode.

```bash
avenc2grib --verbose --stream-id 0 --frame-start 49 --frame-count 10 --ni-start 0 --nj-start 360 --width 60 --height 60 "$URL" > u10.f32
```

Here the source wind file's stream 0 is U10. Binary output remains on stdout;
verbose diagnostics go to stderr. Never capture stdout in a text-mode API or
mix it with diagnostic text.

The layout is little-endian Float32 `(frame, row, column)`, with flat index
`((t * height) + row) * width + column`. The example is 10×60×60 samples:

```python
import numpy as np

u10 = np.fromfile("u10.f32", dtype="<f4").reshape(10, 60, 60)
print(u10.shape)
```

Save the selection, units, grid coordinates and record identities separately;
raw arrays contain none of that framing. For a small subprocess result, binary
`subprocess.check_output([...])` and `np.frombuffer(..., dtype="<f4")` are valid;
for large output, stream to disk rather than retaining two whole copies in RAM.
Default extents mean the remaining grid/frames; for reproducibility, specify
positive sizes and counts after inspecting metadata. Subset calls can allocate
the requested output cube even if the source dataset is opened lazily elsewhere.

## 3.4 Zarr exports

Full conversion uses a positional `.zarr` output and can cover selected streams:

```bash
avenc2grib --zarr-compress gzip input.mp4 decoded.zarr
avenc2grib --group 'stream:0' input.mp4 u10.zarr
```

A cropped/time-limited export uses `--zarr-out` and exactly one resolved stream:

```bash
avenc2grib --verbose --stream-id 0 --frame-start 49 --frame-count 10 --ni-start 0 --nj-start 360 --width 60 --height 60 --zarr-compress gzip "$URL" --zarr-out u10_subset.zarr
```

Use a new destination path. The native exporter writes **experimental Zarr v2**;
compression choices are `none` or `gzip`, not arbitrary codecs. A subset store
includes physical `data`, `quantized` samples, `frame_min`, `frame_max` and
`time_ms` arrays. Full multi-stream export uses a grouped layout. Inspect the
actual store rather than assuming the root is a CF-compliant xarray Dataset or
that every level/member/time has already been reshaped into an ML-ready cube.

For inspection with a separately installed compatible Zarr reader:

```python
import zarr

root = zarr.open_group("u10_subset.zarr", mode="r")
print(root.tree())
print(root["data"].shape)
```

The schema is Zarr v2 even if a newer Zarr Python release is used as the reader.
Do not overwrite attrs/coordinates to make `xr.open_zarr()` appear to work
without checking their meanings. Native Zarr export and xarray `.to_zarr()`
are different paths. The latter needs its own compatible Zarr dependencies and
can trigger all selected data loads; choose its shape/chunks explicitly.
Zarr export does not recover information already lost in the AVENC representation.

## 3.5 All avenc2grib options

| Option / output | Meaning / restriction |
| --- | --- |
| Positional `INPUT OUTPUT.grib` | Reconstruct selected/full GRIB records. HTTP(S) input supported. |
| Positional `INPUT OUTPUT.mp4` | Extract whole selected streams with matching metadata; no new scientific encoding. |
| Positional `INPUT OUTPUT.zarr` | Full/selected-group Zarr v2 export. |
| `--list` | Group summary or selected record listing; no output path. |
| `--head` | Metadata head/details; no output path. |
| `--stats` | Metadata/storage/parsing statistics; no output path. |
| `--group SELECTOR` | Group alias/name or `stream:IDX` selection; comma-separated selectors supported. Inspect aliases first. |
| `--stream-id N` | One stream for raw Float32 subset or `--zarr-out`. Alone it activates subset mode; not a full-GRIB stream selector. |
| `--ni-start N` | Zero-based column start, default 0. |
| `--nj-start N` | Zero-based row start, default 0. |
| `--width N`, `--height N` | Requested spatial size; omitted means remaining extent. |
| `--frame-start N` | Zero-based frame start, default 0. |
| `--frame-count N` | Requested frame count; omitted means remaining records. Inspect frame semantics first. |
| `--zarr-out PATH` | Explicit subset Zarr destination; do not also provide positional output. Resolve exactly one stream via ID/unique group. |
| `--zarr-compress none\|gzip` | Native Zarr compression; default `none`. |
| `--page-size SIZE` | GRIB reconstruction page budget; default 8 GiB. Accept bytes or size suffixes such as MiB/GiB. Not a full-process RSS cap. |
| `--http-stats` | Print source transfer/cache statistics; not numerical reconstruction errors. |
| `--no-http-fallback` | Refuse whole-file fallback if Range access is unsuitable. |
| `--http-block-size SIZE` | Upstream block size; default 1 MiB. |
| `--http-timeout SECONDS` | Positive network timeout; default 30. |
| `--http-fallback-limit SIZE` | Maximum allowed whole-file fallback; default 1 GiB. |
| `--verbose` | Progress and decode/subset diagnostics. This decoder does not advertise `-v`. |
| `--help`, `-h` | Standalone help invocation. |
| `--version` | Common native version flag. |

`--list`, `--head` and `--stats` are mutually exclusive. Raw subset mode requires
`--stream-id`, rejects `--group` and writes only stdout binary data. Zarr subset
mode can resolve a unique `--group`; a multi-stream match needs an explicit ID.
Changing `.zarr` to an arbitrary extension does not select positional Zarr mode;
prefer the normal extension or `--zarr-out`.

## 3.6 Local and remote xarray

Use readable, direct extraction examples when demonstrating the interface.
Separate data extraction from plotting; load only the needed fields and cache
those results. Do not wrap a simple selection in a large abstraction layer.

A local file:

```python
import xarray as xr

with xr.open_dataset("output.mp4", engine="avenc", cache=False) as ds:
    print(ds)  # inspect names, dimensions and coordinates before choosing
```

A known remote wind window:

```python
import numpy as np
import xarray as xr

url = "https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4"
with xr.open_dataset(url, engine="avenc", cache=False) as ds:
    uv = ds[["10u", "10v"]].isel(
        time=49, lat=slice(360, 420), lon=slice(0, 60)
    ).load()
    speed = np.hypot(uv["10u"], uv["10v"])
```

A point series uses the same field selection and preserves time:

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

That request reads a month of values for one point; it is not a metadata-only
query. In this archive, latitudes descend and longitudes use 0–360°. Convert
1.59° W to 358.41° E rather than querying negative longitude blindly. Check the
returned nearest coordinate and missingness. A land-only mask can make an
ocean point missing even when the geographic selection succeeds.

Coordinates need not be CF-decoded datetime64 values; packed metadata can expose
ISO strings. Inspect `ds.coords` and each variable's `dims` before `.sel()`.
Streams with incompatible time coordinates can have separate time dimensions;
do not assume all variables share the name `time` or the same number of frames.
Member/level axes may be represented through separate stream-backed variables,
not a ready-made `(member, lead, level, lat, lon)` cube. Build that mapping from
scientific metadata with explicit alignment.

Useful open parameters:

| Parameter | Use |
| --- | --- |
| `engine="avenc"` | xarray backend entry point registered by the package. |
| `drop_variables=...` | Exclude named variables from the opened Dataset. |
| `decode_coords`, `decode_times`, `mask_and_scale` | Accepted backend parameters, currently ignored in its custom reconstruction path. Do not assume standard CF decoding or raw unscaled video output from toggling them. |
| `cache=False` | Avoid xarray's retained decoded-array cache; does not disable the HTTP block cache or codec buffers. |
| `chunks=...` | Optional Dask orchestration; requires Dask. Chunking is not a scientific encoder option or free random access. |
| `backend_kwargs={"avenc2grib_bin": ...}` | Select a specific decoder binary when required. Normally use the wheel-local/default resolver. |
| `backend_kwargs={"http_options": {...}}` | Remote transfer/cache/fallback policy; see 3.7. |
| `backend_kwargs={"use_cli_subset_for_packed": True}` | Default native packed-GRIB reconstruction path. Disable only for an identified compatibility/debugging need. |

`from avenc import open_avenc` is a convenience wrapper for the same backend.
Always close datasets or use `with`. Metadata/coordinate arrays are loaded, and
`.load()`, `.values`, conversion/export and some plotting operations materialize
selected numerical values. Do not call `.load()` on an entire large Dataset as
part of a point/window demonstration. Read-only access does not imply a generic
writer for arbitrary xarray Datasets.

## 3.7 HTTP Range configuration and measurements

Remote access uses Python's source/range adapter and FFmpeg. The CLI HTTP helper
also needs `python3` on PATH. A Range-capable server should return `206 Partial
Content` and appropriate `Content-Range` values. MP4 metadata/indexing and codec
reference dependencies determine which ranges are needed. Merely accepting an
HTTPS URL is not proof that the entire object was avoided.

For strict selective-access checks, disable full-file fallback:

```bash
avenc2grib --http-stats --no-http-fallback --http-block-size 1MiB --http-timeout 30 --stream-id 0 --frame-start 49 --frame-count 1 --ni-start 0 --nj-start 360 --width 60 --height 60 "$URL" > frame49.f32
```

For Python, explicitly configure bounded caching and print live upstream stats
while the dataset is open:

```python
from avenc import open_avenc

url = "https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4"
with open_avenc(
    url, cache=False,
    backend_kwargs={"http_options": {
        "block_size": 1024**2,
        "cache_blocks": 8,
        "timeout": 30,
        "fallback": False,
        "fallback_limit": 1024**3,
    }},
) as ds:
    frame = ds["10u"].isel(time=49, lat=slice(360, 420), lon=slice(0, 60)).load()
    print(ds.avenc.http_stats)
```

Python HTTP defaults are `block_size=1 MiB`, `cache_blocks=8`, `timeout=30`,
`fallback=True`, `fallback_limit=1 GiB`. Sizes in Python are integer bytes,
not CLI suffix strings. A server ignoring ranges can trigger an allowed bounded
full download. Set `fallback=False` when testing that no such substitution occurs.
Fallback limits are not compressed-byte budgets for normal range access.

Measure early and late frames, a single point, a small window and a contiguous
time slice if selective access is the scientific question. Record requested
scientific output bytes separately from transferred compressed bytes, requests,
cache hits and decoder work. Repeated reads of one open object benefit from its
cache; a fresh object is a different measurement. Account for initialization,
metadata and coordinates rather than timing only a warmed `.load()` call.

Do not equate network efficiency, wall time, retained memory and disk size.
Dask does not remove GOP dependencies; extremely fragmented chunk requests can
repeat decoding. Full GRIB reconstruction can naturally transfer most or all
of the MP4 even through a Range adapter. For private sources, follow the site's
authentication setup and never embed tokens into public examples/manifests.

## 3.8 JavaScript and browser tools

`www/` contains a metadata inspector plus viewer/extractor interfaces. Serve
these over HTTP/HTTPS instead of opening local HTML as `file://`:

```bash
python3 -m http.server 8000 --directory www
```

The metadata module exports `inspectAvencDataset`. For a page that has the
repository's JavaScript modules under `./js/`, for example:

```javascript
import { inspectAvencDataset } from './js/avenc_metadata.js';

const url = 'https://zenodo.org/records/23502851/files/ERALANDWIND.compact.mp4';
const info = await inspectAvencDataset(url, { fast: true });
console.log(info.stream_summaries, info.bytes_fetched);
```

Relevant metadata-inspection options are `fast` (default true), `scanBytes`,
`rangeChunk`, `maxPayloadChars`, and an AbortSignal in `signal`. Full inspection
can also probe container codecs. The parser uses bounded Range reads and rejects
unsuitable full responses; JavaScript fetch additionally requires compatible
CORS. A URL that works in the CLI can fail in the browser for CORS reasons.

The shared viewer/extractor model currently **downloads the complete compressed
MP4 before decoding**. Bounded queues and frame caches reduce decoded-state
retention, not that initial transfer. WebCodecs support depends on browser,
codec, pixel format and device. Do not claim that every published 12-bit or
software-encoded file will render in every browser, or that the browser model
is equivalent to the native selective reader. Runtime MP4Box/fflate modules are
CDN dependencies with their own licences.

For ordinary GRIB viewing, link users to [GRIBView](https://filippi.github.io/gribview/).
It is a companion viewing tool, not evidence that AVENC numerical errors meet
an application's tolerance. See [www/README.md](www/README.md) for serving details.

## 3.9 Troubleshooting and handoff

| Symptom | First checks / bounded response |
| --- | --- |
| Missing command or wrong binary | Active environment, `--version`, PATH and wheel-local decoder resolution. Do not silently use an old build for new measurements. |
| Missing encoder or unsupported pixel format | `ffmpeg -encoders` and `ffmpeg -h encoder=...`; inspect negotiated format/fallback and try a supported explicitly named candidate. |
| GPU encoder listed but fails | Actual device allocation, driver/FFmpeg compatibility and supported bit depth. No automatic hardware fallback claim. |
| Output is unreadable binary in a terminal | Raw subset mode writes Float32 to stdout; redirect or capture as bytes. |
| Requested GRIB crop fails | Native frame/window selectors activate raw subset mode, or Zarr with `--zarr-out`; use whole-stream GRIB reconstruction or an explicit subsequent GRIB selection workflow. |
| Comparison reports missing/duplicate identities | Check source directory contents, cycles, member/levels, grid and step types. Do not automatically add `--allow-unmatched` or keep-first to conceal the issue. |
| Low average error but visible spurious point | Full maxima, mask edges, local frame/time series and the actual derived diagnostic. Increase fidelity/change configuration if needed. |
| Large percentage errors near calm/zero values | Check physical-unit errors and denominator mask; avoid undefined relative-error interpretations. |
| Null normalized metric or SNR | Zero variance/range, no finite pairs, or perfect reconstruction; inspect the metric definition and perfect marker. |
| Remote read downloads everything | Server Range support, fallback policy, object size, request/cache stats and selection scope. |
| Remote CLI works but browser fails | CORS, WebCodecs/device support and the browser model's full-file download. |
| Unexpected xarray dimensions or missing point | Stream-specific metadata, coordinate direction/convention, levels/members, ISO-string times and valid-data mask. |
| High RAM on a small requested point series | Full-frame/GOP decode, requested time count, subprocess buffers, caches and simultaneous Dask tasks. |
| Zarr does not open as a Dataset | Inspect per-stream native v2 layout and dimension attrs; distinguish it from an xarray-written store. |
| Pressure/model preprocessing fails after small errors | Isolate relevant fields, masks, constraints and exact input provenance; no unverified codec-versus-model diagnosis. |

At handoff, provide the selected command/configuration, measured complete-file
sizes/ratio, comparison scope and seed, variable-level errors including maxima,
mask/identity status, resource scope, representative diagnostic plots if requested,
and any unsupported combination. Distinguish a recommended candidate from an
approved scientific tolerance and distinguish source-only documentation checks
from an executed experiment.

For software changes, run focused existing checks appropriate to the changed path:

```bash
ctest --test-dir build --output-on-failure
python -m pytest -q python/avenc/tests/test_metadata_v2.py
python -m unittest discover -s tests -p test_avenc_compare.py
python -m unittest discover -s python/avenc/tests -p test_http_input.py
node www/tests/metadata.mjs
node www/tests/decoder.mjs
```

Data-dependent tests use `AVENC_TEST_MP4`, `AVENC_TRUTH_GRIB`, `AVENC2GRIB_BIN`
and `AVENC_BIN_DIR` where supported. External HTTP checks require
`AVENC_HTTP_INTEGRATION=1` and `AVENC_HTTP_TEST_URL`. Report skips as skips.
Do not run a large extraction, profile sweep or model integration just to verify
an edit to this guide. The current README, source/help and actual measured
records take precedence over historical article examples when behaviour differs.
