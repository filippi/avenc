# Release readiness

See [installation](docs/INSTALL.md), [maintainer procedure](docs/RELEASE.md) and
[the current verification report](docs/RELEASE_READINESS.md).

The earlier cleanup-only checks are retained in docs/PRE_PACKAGING_CHECKS.md;
they predate unified native/Python packaging and do not establish release CI
success. Git is initialized with origin=https://github.com/filippi/avenc.git.
The initial source and CI fixes have been committed and pushed. PyPI publication
remains gated on completed hosted validation and release approval.
