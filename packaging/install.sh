#!/bin/sh
# Usage: ./install.sh "$HOME/.local"  (or sudo ./install.sh /usr/local)
set -eu
prefix=${1:?Pass an installation prefix}
case "$prefix" in /*) ;; *) echo "Use an absolute prefix" >&2; exit 1;; esac
case "$prefix" in /) echo "Refusing root prefix" >&2; exit 1;; esac
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ ! -e "$prefix/libexec/avenc" ] || { echo "Uninstall the existing AVENC installation first" >&2; exit 1; }
for launcher in "$here"/bin/*; do
  [ ! -e "$prefix/bin/$(basename "$launcher")" ] || { echo "Existing command: $launcher" >&2; exit 1; }
done
mkdir -p "$prefix/bin" "$prefix/libexec" "$prefix/share/avenc"
cp -R "$here/libexec/avenc" "$prefix/libexec/avenc"
cp "$here"/bin/* "$prefix/bin/"
cp "$here/LICENSE" "$here/THIRD_PARTY.md" "$here/uninstall.sh" "$prefix/share/avenc/"
printf '%s\n' "AVENC installed in $prefix. Add $prefix/bin to PATH; install FFmpeg separately."
