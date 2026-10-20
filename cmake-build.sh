#!/usr/bin/env bash
# Compatibility entry point; use build.sh for new commands.
set -euo pipefail
exec "$(dirname -- "$0")/build.sh" "$@"
