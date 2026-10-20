# Third-party components and binary redistribution

AVENC's own source is Apache-2.0. That licence does not replace third-party
licences. No external FFmpeg or ecCodes binaries are checked into this source
repository. Release wheels/native archives do bundle the ecCodes dependency
closure; FFmpeg stays external and is never copied into release artifacts.

| Component | Licence / role | Notices / upstream |
| --- | --- | --- |
| ecCodes / eccodeslib | Apache-2.0, GRIB reconstruction, embedded definitions | Upstream LICENSE/AUTHORS copied into avenc/_native/licenses; https://github.com/ecmwf/eccodes |
| eckit / eckitlib | Apache-2.0, ecCodes dependency | Upstream LICENSE/AUTHORS copied; https://github.com/ecmwf/eckit |
| libaec | BSD-2-Clause, GRIB packing | python/avenc/LICENSES/libaec.txt |
| OpenJPEG | BSD-2-Clause and retained contributors | python/avenc/LICENSES/openjpeg.txt |
| libpng | libpng licence | python/avenc/LICENSES/libpng.txt |
| PROJ | MIT and notices retained in COPYING | python/avenc/LICENSES/proj.txt |
| SQLite | Public-domain dedication | python/avenc/LICENSES/sqlite.txt |
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

## Open Linux redistribution issue

Inspection of the actual Linux eckit wheel found an additional curl/TLS/Kerberos/
LDAP/SSH/IDN dependency closure. See packaging/audits/linux_ecmwf_dependencies.json.
The notices above do not yet cover that closure. Some components have LGPL or
other source/notice obligations, and the older bundled TLS version also needs
review. Linux publication is blocked by the separate repository approval gate
until that review or a minimal upstream build removes the issue. This is not a
claim that the Linux closure is Apache-only or already cleared for redistribution.
