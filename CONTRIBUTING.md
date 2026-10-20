# Contributing

Open an issue or pull request at https://github.com/filippi/avenc.
Describe the input, intended result, actual behavior, platform, exact command,
AVENC revision and FFmpeg/ecCodes versions. Use a small synthetic reproducer
rather than committing research datasets, credentials or workstation paths.

Keep codec, quantization and scientific metadata compatibility explicit. Compare
fields by identity, preserve authoritative masks, and distinguish video-lossless
coding from source-exact scientific reconstruction. Do not silently change a
profile used by an archived experiment.

Build and run focused tests as described in README.md. Report skips as skips;
large data-dependent or external-network tests are opt-in. Native code is C++17;
Python packaging and its array codec are a separate implementation. Supporting
changes should preserve Linux/macOS operation. Changes are submitted under the
repository's Apache-2.0 licence.
