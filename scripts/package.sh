#!/bin/bash
# usage: scripts/package.sh            -> build/quake3e-splitscreen-<version>-linux-x86_64.tar.gz
# Packs the current release build (scripts/build.sh output) like upstream's
# release archives: only the three executables, at the archive root (no
# folder, no launcher, no text files).  Users unpack next to baseq3/ and start
# quake3e-vulkan-ss.x64.  Run scripts/build.sh first.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/build/release-linux-x86_64"
VERSION="$(head -n1 "$REPO/VERSION" | tr -d '[:space:]')"
NAME="quake3e-splitscreen-$VERSION-linux-x86_64"
FILES="quake3e-vulkan-ss.x64 quake3e-ss.x64 quake3e-ss.ded.x64"
for f in $FILES BUILD-INFO.txt; do
  [ -f "$OUT/$f" ] || { echo "missing $OUT/$f -- run scripts/build.sh" >&2; exit 1; }
done
grep -q "^version: $VERSION\$" "$OUT/BUILD-INFO.txt" || { echo "BUILD-INFO.txt is not version $VERSION -- rebuild" >&2; exit 1; }
grep -q "^configuration: release|" "$OUT/BUILD-INFO.txt" || { echo "BUILD-INFO.txt is not a release build -- rebuild" >&2; exit 1; }
STAGE="$REPO/build/pkg"
rm -rf "$STAGE"; mkdir -p "$STAGE"
for f in $FILES; do install -m 0755 "$OUT/$f" "$STAGE/$f"; done
tar -C "$STAGE" --owner=0 --group=0 -czf "$REPO/build/$NAME.tar.gz" $FILES
rm -rf "$STAGE"
ls -l "$REPO/build/$NAME.tar.gz"
