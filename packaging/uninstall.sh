#!/bin/sh
set -eu
prefix=${1:?Pass the same absolute prefix used at installation}
case "$prefix" in /|"") exit 1;; /*) ;; *) exit 1;; esac
[ -f "$prefix/share/avenc/THIRD_PARTY.md" ] || { echo "No AVENC installation at $prefix" >&2; exit 1; }
for tool in grib2avenc avenc2grib avpack avenc-compare ComputeError grib_pair_extract avenc_test_generator; do
  file="$prefix/bin/$tool"
  if [ -f "$file" ] && grep -q '^# AVENC native release launcher$' "$file"; then rm "$file"; fi
done
rm -rf "$prefix/libexec/avenc"
rm -f "$prefix/share/avenc/LICENSE" "$prefix/share/avenc/THIRD_PARTY.md" "$prefix/share/avenc/uninstall.sh"
rmdir "$prefix/share/avenc"
