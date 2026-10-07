#!/bin/bash
# Builds Quake3e-splitscreen for Linux x86_64 into build/release-linux-x86_64/
# (or build/debug-linux-x86_64/ with "debug") using the upstream Makefile, in
# upstream's release layout: plain executables with the renderer linked in
# (USE_RENDERER_DLOPEN=0), one client per renderer, no renderer .so files.
#
# Bazzite is immutable, so the compile runs inside the "q3dev" distrobox
# (Fedora 43 + gcc/make/SDL2-devel/...; see docs/LINUX-BOOTSTRAP.md for the
# create line).  Run from the host: the script re-enters itself in the box.
# Without distrobox (e.g. a CI runner with the -dev packages installed) it runs
# make directly.  The executables link the host's libSDL2-2.0.so.0 /
# libvulkan.so.1 / libGL at run time, so they run on the host.
#
# Outputs (release: stripped, like upstream's "make install"):
#   quake3e-vulkan-ss.x64    client, Vulkan renderer built in (the one to launch)
#   quake3e-ss.x64           client, OpenGL renderer built in
#   quake3e-ss.ded.x64       dedicated server
#   BUILD-INFO.txt           our record only, not shipped
# Objects: build/obj/vk/<cfg>-linux-x86_64/ (Vulkan client) and
# build/obj/gl/<cfg>-linux-x86_64/ (OpenGL client + server): two object dirs,
# so neither link forces a rebuild of the other.
#
# usage: scripts/build.sh [debug] [extra make args...]
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BOX="${Q3DEV_BOX:-q3dev}"

CFG=release
if [ "$1" = "debug" ]; then CFG=debug; shift; fi

VERSION="$(head -n1 "$REPO/VERSION" | tr -d '[:space:]')"
case "$VERSION" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "Bad VERSION '$VERSION'" >&2; exit 1 ;;
esac
if [ -n "$Q3_BUILD_COMMIT" ]; then
  COMMIT="$Q3_BUILD_COMMIT"
else
  COMMIT="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  [ -n "$(git -C "$REPO" status --porcelain --untracked-files=no 2>/dev/null)" ] && COMMIT="$COMMIT-dirty"
fi

if [ -z "$CONTAINER_ID" ] && command -v distrobox >/dev/null 2>&1; then
  if ! distrobox list 2>/dev/null | grep -q " $BOX "; then
    echo "distrobox '$BOX' not found; create it as in docs/LINUX-BOOTSTRAP.md" >&2
    exit 1
  fi
  echo "== Quake3e-splitscreen $VERSION ($COMMIT) $CFG|linux-x86_64 (in distrobox $BOX) =="
  # the box has no git: hand the commit in
  exec distrobox enter "$BOX" -- env Q3_BUILD_COMMIT="$COMMIT" "$REPO/scripts/build.sh" $([ "$CFG" = debug ] && echo debug) "$@"
fi

OUT="$REPO/build/$CFG-linux-x86_64"
OBJVK="build/obj/vk"            # Makefile BUILD_DIR (relative to the repo)
OBJGL="build/obj/gl"
TARGET=release
[ "$CFG" = debug ] && TARGET=debug
JOBS="$(nproc 2>/dev/null || echo 4)"
VK="$REPO/$OBJVK/$CFG-linux-x86_64"
GL="$REPO/$OBJGL/$CFG-linux-x86_64"

cd "$REPO"
# make does not track CFLAGS: rebuild the objects that embed Q3_VERSION so the
# stamp follows VERSION
rm -f "$VK"/client/{common,cl_curl,cl_main,cl_console}.o "$VK"/rendv/vk.o \
      "$GL"/client/{common,cl_curl,cl_main,cl_console}.o "$GL"/ded/common.o
# upstream's release links (.github/workflows/build.yml), our names
make -j"$JOBS" "$TARGET" BUILD_DIR="$OBJVK" USE_SDL=1 USE_RENDERER_DLOPEN=0 RENDERER_DEFAULT=vulkan \
  CNAME=quake3e-vulkan-ss BUILD_SERVER=0 SPLITSCREEN_VERSION="$VERSION" "$@"
make -j"$JOBS" "$TARGET" BUILD_DIR="$OBJGL" USE_SDL=1 USE_RENDERER_DLOPEN=0 RENDERER_DEFAULT=opengl \
  CNAME=quake3e-ss DNAME=quake3e-ss.ded SPLITSCREEN_VERSION="$VERSION" "$@"

mkdir -p "$OUT"
# earlier layouts (<= 0.0.0.15: dlopen'd renderers, objects in the output dir)
rm -f "$OUT"/quake3e.x64 "$OUT"/quake3e.ded.x64 "$OUT"/quake3e_*_x86_64.so
rm -rf "$OUT"/client "$OUT"/ded "$OUT"/rend1 "$OUT"/rend2 "$OUT"/rendv
for pair in "$VK/quake3e-vulkan-ss.x64" "$GL/quake3e-ss.x64" "$GL/quake3e-ss.ded.x64"; do
  install -m 0755 "$pair" "$OUT/"
  [ "$CFG" = release ] && strip "$OUT/$(basename "$pair")"
done

{
  echo "Quake3e-splitscreen"
  echo "version: $VERSION"
  echo "commit: $COMMIT"
  echo "configuration: $CFG|linux-x86_64"
  echo "built: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "toolchain: $(gcc --version | head -n1), SDL2 $(pkg-config --modversion sdl2 2>/dev/null || echo '?') headers, ${CONTAINER_ID:+distrobox }${CONTAINER_ID:-host}"
  echo "quake3e-vulkan-ss.x64: client, Vulkan renderer built in; quake3e-ss.x64: client, OpenGL renderer built in; quake3e-ss.ded.x64: dedicated server$([ "$CFG" = release ] && echo ' (all stripped)')"
  echo "runtime: host libSDL2-2.0.so.0, libvulkan.so.1 / libGL.so.1 (gamepads through the engine's own SDL2); fs_basepath defaults to the executable's folder"
  echo "release archive: the three executables only (scripts/package.sh); this file is not shipped"
} > "$OUT/BUILD-INFO.txt"

echo "== OK: $OUT"
ls -l "$OUT"
