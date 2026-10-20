# Release procedure

The repository has been pushed with author authorization. The author configured
the pending PyPI trusted publisher for release.yml, environment pypi. Hosted CI
and the existing release approval gates must pass before publication.

## Published release 1.0.0

Published on 20 October 2026 using the configured PyPI trusted publisher:

- Source tag: `v1.0.0` (`29e670df8548ca9b3cfc6751531d5c28b00b6d8b`).
- [Successful release workflow](https://github.com/filippi/avenc/actions/runs/39484296354).
- [PyPI](https://pypi.org/project/avenc/1.0.0/): 16 wheels and one source distribution.
- [GitHub release](https://github.com/filippi/avenc/releases/tag/v1.0.0): the same Python distributions,
  four native installers and SHA256SUMS.
- All Linux/macOS source, wheel and standalone installation checks passed.
- `AVENC_RELEASE_ENABLED` was reset to `false` after successful publication.

The configured GitHub environments restrict deployments to `v*` tags. They have
no required reviewer; explicit workflow dispatch and the two repository variables
remain the publication controls. TestPyPI is optional and has not been configured.

## One-time manual setup

1. Review the complete untracked tree; commit and push to filippi/avenc only when
   the author authorizes that step. Enable Actions and branch protection on main.
2. Require Source checks and all four Binary wheels jobs. Inspect their repaired
   wheels, native smoke tests and dependency notices. For 1.0.0 all four wheel jobs and both source-check jobs passed; retain
   fresh evidence for every subsequent release.
3. Create protected environments `testpypi`, `pypi`, `github-release`, requiring
   author approval and limiting deployment to version tags.
4. In TestPyPI and PyPI create pending trusted publishers: project `avenc`, owner
   `filippi`, repository `avenc`, workflow `release.yml`, environment `testpypi`
   or `pypi` respectively. A 404 name lookup is not a name reservation; recheck.
5. Review the actual generated native dependency inventory and retained notices.
   Linux now builds pinned standard ecCodes/libaec sources without eckit-geo;
   packaging/audits/linux_ecmwf_dependencies.json records the larger binary-wheel
   configuration that was excluded. Confirm the minimal closure in hosted CI
   before approving it for release. Only then set repository
   variable `AVENC_LINUX_DEPENDENCIES_APPROVED=true`. It is a separate publication
   gate: enable it only after reviewing the actual dependencies for the release.
6. Keep repository variable `AVENC_RELEASE_ENABLED` absent/false until approved.
   Publication needs BOTH this variable=true and explicit manual publish=true;
   pushing a tag only builds artifacts. OIDC is scoped to the publish job.
7. Create filippi/homebrew-avenc separately when ready for a tap. No tap push is
   performed automatically.

## Build and test locally

```sh
python -m pip install build twine
python -m build                         # creates sdist and a locally bundled wheel
python -m twine check dist/*
# Install the wheel in a separate venv, with FFmpeg already on PATH:
python -m pip install 'dist/avenc-VERSION-PLATFORM.whl[test]'
python packaging/smoke_test.py
```

Use cibuildwheel for distributable platform tags and independent install tests:

```sh
python -m pip install cibuildwheel==4.3.1
python -m cibuildwheel --output-dir wheelhouse
python packaging/make_native.py wheelhouse/EXACT_CP311_WHEEL.whl
python packaging/check_artifacts.py dist
```

Linux builds use manylinux_2_28 containers on native x86_64/aarch64 runners. Test
FFmpeg is installed separately inside containers with checksummed micromamba
2.3.3 and conda-forge ffmpeg8.0.1, and never enters an AVENC artifact. macOS uses
Homebrew FFmpeg. Wheel repair uses auditwheel/delocate; flags prohibit native-CPU
and fast-math optimizations. Native archives reuse the tested wheel payload.
Reproducibility means pinned major native inputs and recorded artifacts, not a
claim of byte-identical builds: compiler/images and transitive tools can change.

## First release, after explicit authorization

1. Check VERSION, CITATION.cff and release notes. VERSION is authoritative for
   Python metadata, native --version and package names.
2. With authorization to commit/push, create and push a matching version tag:

   ```sh
   git tag -a v1.0.0 -m 'AVENC 1.0.0'
   git push origin v1.0.0
   ```

3. Wait for all tag builds and scientific smoke tests. Inspect artifacts and
   SHA256SUMS. The conservative size check fails wheels >=100,000,000 bytes.
4. After author approval enable the variable, then dispatch the release workflow
   against the tag, target=testpypi, publish=true. Example with gh:

   ```sh
   gh workflow run release.yml --ref v1.0.0 -f publish=true -f target=testpypi
   ```

5. Verify a fresh TestPyPI installation (use --no-deps for AVENC from TestPyPI;
   install its declared dependencies from normal PyPI first, avoiding extra-index
   dependency ambiguity). Exercise tools/import/xarray and round-trip tests.
6. Obtain explicit production approval, dispatch again with target=pypi. Its
   successful publication permits the protected GitHub Release job. Disable the
   repository gate afterwards. Do not overwrite release files or reuse versions.
7. Download the exact GitHub tag source archive and generate the stable formula:

   ```sh
   curl -fL https://github.com/filippi/avenc/archive/refs/tags/v1.0.0.tar.gz -o avenc-v1.0.0.tar.gz
   python packaging/homebrew.py avenc-v1.0.0.tar.gz --output /tmp/avenc.rb
   ```

   Review/version/checksum, copy to the authorized tap's Formula/avenc.rb, run
   `brew install --build-from-source ./Formula/avenc.rb` and `brew test avenc`,
   then commit/push the tap with authorization. The generator never invents a
   checksum and does not publish.

For each release retain workflow links, dependency inventory/licences, tests,
SHA256SUMS and source revision. Run `python -m pip uninstall avenc` to verify
Python uninstall; native archives have a separate uninstall.sh.
