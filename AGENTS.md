# AVENC software workspace

This is the cleaned software repository for https://github.com/filippi/avenc.
It contains C++ CLI tools, Python bindings and browser tools. The article and
large datasets are separate. Do not import their generated outputs or old
repository history. Do not commit or push until the author requests it.

Read README.md and skills.md, inspect Git status when a repository exists, and
preserve author edits. Use current source/help for behavior, not historical
profile names or performance numbers. No large compression sweeps, downloads,
model runs or publication are implied by routine development.

- Native build: CMake/C++17, ecCodes, zlib; FFmpeg/ffprobe at runtime.
- Python: root pyproject.toml packages python/avenc modules, pybind11 array codec,
  xarray backend and all native CLI tools in one avenc distribution.
- Browser: www/; metadata uses range reads, viewer/extractor currently loads the
  full file. Do not claim browser streaming is equivalent to Python access.
- Tests: CTest metrics, tests/test_avenc_compare.py, Python metadata/HTTP tests
  and Node browser tests. Missing large fixture tests must be reported skipped.

Scientific metadata, grid geometry, field identities and masks are part of the
format. Video losslessness is not scientific losslessness. Keep fast-math off.
Default is lossless video with auto quantization; shared-bitmap fill is taper3.
Do not change numerical algorithms while doing packaging or documentation work.

Do not retain virtual environments, compiled modules, test outputs, raw data,
workstation paths, credentials or private infrastructure in the public tree.
Use temporary build/install directories for release checks. Test a Python sdist
and a clean install, not only an editable checkout, before declaring packaging
ready. Never change the distribution name or version without author intent.
