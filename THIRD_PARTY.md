# Third-party components and binary redistribution

AVENC's own source is Apache-2.0. That licence does not replace third-party
licences. No external FFmpeg or ecCodes binaries are checked into this source
repository. Release wheels/native archives do bundle the ecCodes dependency
closure; FFmpeg stays external and is never copied into release artifacts.

| Component | Licence / role | Notices / upstream |
| --- | --- | --- |
| ecCodes / eccodeslib | Apache-2.0, GRIB reconstruction, embedded definitions | Upstream LICENSE/AUTHORS copied into avenc/_native/licenses; https://github.com/ecmwf/eccodes |
| eckit / eckitlib | Apache-2.0, macOS ecCodes dependency | Upstream LICENSE/AUTHORS copied; https://github.com/ecmwf/eckit |
| libaec | BSD-2-Clause, GRIB packing | python/avenc/LICENSES/libaec.txt |
| OpenJPEG | BSD-2-Clause and retained contributors | python/avenc/LICENSES/openjpeg.txt |
| libpng | libpng licence | python/avenc/LICENSES/libpng.txt |
| PROJ | MIT and notices retained in COPYING | python/avenc/LICENSES/proj.txt |
| SQLite | Public-domain dedication | python/avenc/LICENSES/sqlite.txt |
| JPEG / libjpeg-turbo | IJG, BSD-3-Clause, Zlib; Intel macOS transitive library | python/avenc/LICENSES/libjpeg-turbo.md and ijg.txt |
| libtiff | libtiff permissive licence; Intel macOS transitive library | python/avenc/LICENSES/libtiff.md |
| liblzma | Public-domain/0BSD library; Intel macOS transitive library | python/avenc/LICENSES/xz.txt and lzma-0bsd.txt |
| Zstandard | BSD-3-Clause option; Intel macOS transitive library | python/avenc/LICENSES/zstd.txt |
| LZ4 | BSD-2-Clause (library) | python/avenc/LICENSES/lz4.txt |
| zlib | zlib licence, metadata compression | python/avenc/LICENSES/zlib.txt |
| pybind11 | BSD-3-Clause, compiled binding headers | python/avenc/LICENSES/pybind11.txt |
| FFmpeg/codecs | External process; build-dependent LGPL/GPL and patent considerations | https://ffmpeg.org/legal.html |
| xarray / NumPy / pandas | Separately installed Python dependencies | Their installed distribution notices |
| MP4Box.js / fflate | BSD-3-Clause / MIT, loaded by browser from CDN | https://github.com/gpac/mp4box.js / https://github.com/101arrowz/fflate |

Curated upstream notice snapshots and download checksums are recorded in
python/avenc/LICENSES/sources.json. Wheels retain these in dist-info/licenses;
standalone archives retain them under libexec/avenc/licenses. Native upstream
licences are also retained within the native payload. Dependencies vary across
platforms: review auditwheel/delocate output and bundled library inventory on
each architecture before publication, including any new transitive libraries.
The dependency inventory is generated before the final repair pass; retain its
log as well if the repair pass adds or renames libraries.

FFmpeg is invoked as a separate installed executable, not linked or distributed
in the wheel. AVC/HEVC are not automatically available: libx264/libx265 and other
codec components change FFmpeg's licence/build obligations. Their presence in an
independently installed FFmpeg is not an Apache patent licence for those codecs.
Consult the actual distributor/build configuration and applicable patent terms;
this packaging work does not establish patent clearance. CI-only FFmpeg is an
external test prerequisite, not a release artifact.

Maintainers: do not introduce a bundled FFmpeg build without a new licence and
source-distribution review. Do not infer redistribution rights from a codec's
technical availability. This file is an engineering inventory, not a legal opinion.

## Verified minimal Linux build

Linux packaging now compiles pinned ecCodes/libaec sources listed in
packaging/native_sources.json, retaining PNG, OpenJPEG, AEC and embedded
definitions. The optional eckit-geo backend is disabled, as it is by default
in upstream ecCodes. Unexpected non-system Linux dependency names fail the
bundling step. The previously identified eckit binary-wheel closure is excluded
from this Linux build. Its retained audit file is historical evidence. Hosted CI validated all eight Linux wheels and both Linux native installer
variants. The existing release-approval gate is retained and must be enabled
explicitly for the verified bundle before publication. AVENC's source remains Apache-2.0; wheel metadata also records
the permissive licences of its bundled dependencies.

The bundled JPEG library is based in part on the work of the Independent JPEG
Group. No endorsement by its authors is implied. Only the permissively licensed
liblzma library is bundled, not XZ command-line/build tools. Zstandard is used
under its BSD-3-Clause licensing option.

## ECFlix globe land background

`www/data/ne_110m_land.geojson` is Natural Earth 1:110m land geometry from
https://github.com/nvkelso/natural-earth-vector. Natural Earth data are public
domain (https://www.naturalearthdata.com/about/terms-of-use/). Source/provenance
are recorded in `www/data/NATURAL_EARTH.md`; the globe credits Natural Earth.
This is static cartographic background data, not a forecast or satellite image.
