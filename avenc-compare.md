# avenc-compare: scientific reconstruction metrics

Implemented in **src/avenc_compare.cpp**, built as **bin/avenc-compare**.
This replaces the positional raw-buffer workflow for new experiments.
ComputeError remains unchanged for legacy scripts.

## Build and use

Uses the existing ecCodes, zlib and native AVENC subset library. MP4 comparison
also requires ffmpeg and ffprobe on PATH; Python is not required at runtime.

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_FAST_MATH=OFF
cmake --build build --target avenc-compare -j4

bin/avenc-compare original.grib compressed.mp4 \
  --json metrics.json --csv groups.csv --messages-csv fields.csv

bin/avenc-compare --full original.grib reconstructed.grib \
  --json metrics.json --csv groups.csv --messages-csv fields.csv
~~~

Reference: a GRIB file or directory. Candidate: a packed-GRIB AVENC MP4, GRIB
file, or directory. Directories contain immediate .grib/.grb/.grib2/.grb2 files
sorted by path; MP4s and subdirectories are ignored. The separate Python array
encoder metadata schema is not supported by this native tool.

## References with multiple source files

Use a directory containing only the reference GRIB files, or concatenate them:

~~~bash
cat source/*.grib > reference.grib
bin/avenc-compare --full reference.grib compressed.mp4 \
  --json metrics.json --csv groups.csv --messages-csv fields.csv
~~~

Research datasets and figure-generation scripts are distributed separately from
this software repository. Evaluate the exact source and candidate files used.

## Pairing and grouping

Scientific field identity includes parameter ID, short name, units, level
type/value and layer bounds, step type, ecCodes md5GridSection, point count,
initialization date/time, start/end step and step units, ensemble number,
perturbation number and ensemble forecast type, and percentile/probability keys
where present. Matching never assumes positional correspondence.

The grid MD5 identifies geometry/scanning; source checksums are SHA-256.
No regridding occurs. Different GRIB editions can have different grid-section
identities despite equivalent geographic coordinates and fail strict matching.

Groups retain parameter, units, level/layer bounds, step type and grid.
Members and times are pooled within each scientific group.
--group-by-member adds member identity to the group and sampling strata.
Pairing always preserves the member regardless of grouping.

Packed-GRIB AVENC v1/v2 metadata is read through a dedicated reader adapted from
avenc2grib. Embedded templates have per-record date/time/step/member restored.
Stream/frame uniqueness, template/record variable and level, crop geometry and
decoded dimensions are checked. Values come directly from the Float32 subset
API; original truth stays ecCodes Float64. There is no intermediate GRIB
repacking or Float32 truth conversion.

Missing/extra identities fail by default. --allow-unmatched explicitly compares
the intersection and records complete missing/extra identity lists.

Duplicates fail by default. --duplicate-policy first explicitly keeps the first
scientific identity in sorted-file/message order for GRIBs or metadata record
order for MP4. Counts are retained. This does not establish that duplicate
payload values are identical.

## Sampled quick screening: --fast

~~~bash
bin/avenc-compare --fast original.grib compressed.mp4 \
  --json quick.json --csv quick.csv
~~~

--fast selects **7 fields per scientific group and 8,192 points per field**.
All fields are retained for groups of seven or fewer fields, including the
seven-time LAM boundary dataset. Larger groups use chronological strata and
prefer distinct ensemble members within the sampled times.

Sampling provides a quick screening estimate, not an error guarantee. It can
miss localized extremes, uncommon members or short-lived fields. Use --full
when exhaustive verification is needed, and record the comparison scope.

For groups with more than seven distinct times, field selection splits the
reference times into chronological strata and draws one time/field per stratum.
Within each selected time, a previously unsampled member is preferred.
With a custom field budget larger than the number of times, remaining fields
are sampled without replacement. Seven fields do not cover an entire ensemble.

Spatial selection draws one index from each of 8,192 nearly equal contiguous
blocks of the grid's scan-order indices. This spreads samples across the grid
without consulting candidate values or compression errors. Tiny fields/groups
use all available points/fields. Seed 5489 remains deterministic; the same
reference selects the same identities and points across candidate profiles.

To avoid a complete extra read of large inputs, **--fast disables SHA-256 by
default**. Size/mtime checks and complete scientific identity matching remain
active. Add --hash for archived provenance:

~~~bash
bin/avenc-compare --fast --hash ref.grib test.mp4 --json quick.json

# Explicit budgets override fast defaults regardless of option order.
bin/avenc-compare --fast --frames-per-group 3 --points-per-frame 2048 \
  ref.grib test.mp4
~~~

The fast quantile reservoir cap is 16,384 pairs; --quantile-cap overrides it.
--fast and --full are mutually exclusive. JSON records mode=fast, effective
budgets, sampling methods and whether hashes were computed.

Nearby selected MP4 frames in a common stream/group are coalesced into one
decode window, including skipped frames when the window fits --decode-batch
(default 8). Only selected fields contribute to metrics; decoded-point counts
include the additional frames. This avoids repeated FFmpeg launches for short
boundary-forcing streams. Long temporal jumps can still require decoding
preceding frames, so wall time depends on codec/GOP structure and layout.

Use --fast for quick profile screening, RMSE/MAE and broad distribution
checks. Tail quantiles, extrema and mask counts are sample estimates; maxima
and rare outliers can be missed. It does not certify a global error bound or
replace a full run for final paper measurements.

## Fast and full modes

Without --fast or --full: up to 8 fields per group and 10,000 spatial indices per selected field.
Sampling is without replacement using std::mt19937_64, default seed 5489, stable
FNV-1a seed derivation, and an explicit bounded-integer algorithm. Selected
identities and spatial indices depend on reference identity, not candidate
errors or profile name.

~~~bash
bin/avenc-compare ref.grib test.mp4 \
  --frames-per-group 8 --points-per-frame 10000 --seed 5489
bin/avenc-compare --full ref.grib test.mp4
~~~

Every group is represented. A default 8-field ensemble sample does not cover all
members/times; use --full or sampled --group-by-member for that purpose.

Fast-mode counts, extrema, maximum errors and masks describe selected points
only. Sampling cannot prove a global error bound or complete mask preservation.
Small groups with fewer than 8 fields use all their fields.

Full mode examines every matched field and point. Reference arrays are unpacked
one at a time; --decode-batch (default 8) bounds consecutive MP4 frames per read.
Groups are finalized and quantile buffers released as processing advances.
No complete dataset cube is retained.

Spatial sampling still unpacks/decodes complete selected fields. The current
subset API may decode preceding frames. Fast mode is not a claim of inexpensive
pointwise random access.

### Exact versus estimated quantiles

Moments, sums, extrema, mask counts, RMSE, MAE, bias, correlation and RMS/range/std
normalizations use **every examined finite pair**.

Quantiles and quantile-derived metrics use a deterministic uniform reservoir
when a field or group exceeds --quantile-cap (default **200,000 pairs**).
Every output row records quantiles_exact and quantile_sample_size.
**--full does not imply exact quantiles.**

~~~bash
# Exact quantiles, with memory proportional to the largest scientific group.
bin/avenc-compare --full --quantile-cap 0 ref.grib test.mp4
~~~

Avoid unbounded quantiles on the complete month/ensemble without checking RAM.
Each retained pair stores three doubles. Quantiles use linear interpolation at
rank (n - 1) q. Reservoir draws follow processing order, so group estimates can
differ slightly when profiles arrange fields differently into streams. For very
small cross-profile quantile differences use exact quantiles on a defined subset
or assess reservoir-size sensitivity.

## Metrics and conventions

Errors are **candidate minus original**, in each group's native units.
Statistics use finite pairs only. GRIB bitmaps identify missing values, rather
than assuming that a particular numeric sentinel is always missing.

Per group and per selected field:

- Finite-pair count n, examined points, missing reference/decoded counts,
  both-missing count and finite/missing-mask mismatches.
- MAE, RMSE, signed bias, population error std, median absolute error, p95/p99
  absolute error, maximum absolute error and its spatial index.
- Reference/decoded min, max, mean and population std; reference RMS.
- Signed min/max shifts, std ratio, variance ratio and Pearson correlation.
- q01/q05/q50/q95/q99 reference, decoded and signed shifts.
- Relative RMSE = RMSE / reference RMS.
- Range NRMSE = RMSE / (reference max - reference min).
- Robust NRMSE = RMSE / (reference q99 - reference q01).
- SNR_RMS = 20 log10(reference RMS / RMSE).
- SNR_STD = 20 log10(reference std / RMSE).
- PSNR = 20 log10((reference max - reference min) / RMSE).

Population moments (ddof=0) use stable running updates and long-double
accumulators. No arbitrary epsilon is inserted into zero denominators.
Undefined results are JSON null and empty CSV cells. Perfect reconstruction
is marked perfect_on_finite_pairs=true; its infinite dB values are also null
rather than invalid JSON Infinity. Constant fields have undefined correlation
and spread ratios.

There is no dimensional pooled RMSE across unrelated variables. The global
normalized summary is an equal-group mean of defined robust NRMSEs with the
number of contributing groups.

Worst fields are ranked by RMSE, robust NRMSE, max error, absolute bias, and
absolute variance-ratio departure from 1. JSON lists indices into messages;
each field provides full identity, source path/message and MP4 stream/frame.
The group maximum-error spatial index is within the field named explicitly
by max_error_identity.

## Outputs and provenance

- Console: groups, complete-file size ratio, timings and worst fields.
- --json: avenc.compare.v1 schema, options/seed, compiler/ecCodes versions,
  source paths/bytes/mtime/SHA-256, pairing counts/lists, timings/memory,
  group/field metrics, quantile status and worst-field indices.
- --csv: group metrics and explicit parameter/level/grid columns.
- --messages-csv: field metrics plus date/time/step/member columns.

SHA-256 streams inputs and is enabled by default outside --fast. --hash restores
checksums in --fast. --no-hash is for development,
not archived paper measurements. Input size/mtime changes are checked.
Output paths cannot be inputs or duplicates. Outputs use temporary files
followed by rename.

Value-read timings cover selected **full fields**; AVENC includes subprocess
and metadata overhead. Indexing/hashing is timed separately. These are single
unflushed-cache measurements, not cold-storage or point-read benchmarks.
Parent peak RSS and largest child RSS are separate, not a simultaneous combined
process-tree peak.

## Scope and next step

Implemented now: generic reconstruction metrics and reproducible numerical
outputs for future rate–distortion/profile figures.

Not implemented here: plotting, encoder sweeps, energy spectra, gradients, mass
integrals, derived U/V wind speed, ensemble spread-relative error/probability
maps, or downstream Meso-NH NetCDF comparisons. Existing specialized scripts
remain available. These can be connected or extended after figure selection.
Pooled-group std is not ensemble spread at a fixed point/time.
Reconstruction metrics do not demonstrate unchanged forecast skill.

## Focused validation

~~~bash
c++ -std=c++17 -O2 tests/avenc_compare_metrics.cpp -o /tmp/avenc_compare_metrics_test
/tmp/avenc_compare_metrics_test
python -m unittest discover -s tests -p test_avenc_compare.py
~~~

Checks include analytical errors, stable moments, constant denominators,
missing masks, deterministic sampling/reservoirs, SHA-256 vectors and file
hashes, reordered GRIB, known +1 perturbations, directory/member grouping,
duplicate/missing/wrong-grid rejection, protected inputs, AVENC v1/v2 direct
decoding and independent NumPy RMSE verification. No large research datasets are included in the test suite.
