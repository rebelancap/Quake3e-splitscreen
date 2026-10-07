#!/bin/bash
# usage: scripts/tests/r14a-cfg.sh <case>     (Windows port of l5-cfg.sh)
# L5 side-by-side configs on Windows: runs the Release exes against a scratch home path
# work/r14a-cfghome-<case>/ (always fresh), base path = the maintainer's Quake 3 folder (read-only).
# Evidence: work/r14a-cfg-<case>.log (engine logs, appended) + work/r14a-cfg-<case>-ls.txt
# (file listing + sha256 of every q3config*.cfg before/after each run).
# Cases: seed (upstream q3config.cfg -> q3config-ss.cfg; upstream file never written, even
#        after an archived change), noover (an existing q3config-ss.cfg is never overwritten),
#        autoexec (autoexec-ss.cfg runs after autoexec.cfg), ded (dedicated exe:
#        q3config_server.cfg -> q3config_server-ss.cfg), child (a --child window neither seeds
#        nor writes q3config-ss.cfg).
# Refuses to start beside a running quake3e process.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CASE="$1"
BIN="$REPO/build/Release"
BASE="${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}"
W="$REPO/work"
if tasklist 2>/dev/null | grep -qi quake3e; then echo "a quake3e process is running; not starting another" >&2; exit 3; fi
WIN=(--noactivate +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set vid_xpos 64 +set vid_ypos 64 +set logfile 3 +set com_introplayed 1 +set in_gamepad 0)
H="$W/r14a-cfghome-$CASE"; L="r14a-cfg-$CASE"
rm -rf "$H"; mkdir -p "$H/baseq3"; rm -f "$W/$L.log" "$W/$L-ls.txt"
run() {	# run <exe> <logname> [args...]
  local exe="$1" log="$2"; shift 2
  echo "=== $(date +%T) run $(basename "$exe") $*" >> "$W/$log-ls.txt"
  timeout 120 "$exe" "$@"
  echo "rc $?" >> "$W/$log-ls.txt"
  [ -f "$H/baseq3/qconsole.log" ] && { cat "$H/baseq3/qconsole.log" >> "$W/$log.log"; rm -f "$H/baseq3/qconsole.log"; }
}
snap() {	# snap <label>
  { echo "--- $1: files (mtime size path) + sha256"; (cd "$H" && find . -type f ! -name '*.log' -printf '%TT %s %p\n' | sort -k3; find . -name 'q3config*.cfg' -exec sha256sum {} +); } >> "$W/$L-ls.txt"
}
HP="$(cygpath -w "$H")"
case "$CASE" in
seed)
  printf '// upstream-style config\nunbindall\nbind x "say seeded"\nseta name "SeedTest"\nseta sensitivity "3"\n' > "$H/baseq3/q3config.cfg"
  snap before
  run "$BIN/quake3e-vulkan-ss.x64.exe" "$L" "${WIN[@]}" +set fs_basepath "$BASE" +set fs_homepath "$HP" +wait 100 +name +bind x +seta r14a_marker 7 +wait 20 +quit
  snap "after run 1 (seeds, r14a_marker 7)"
  run "$BIN/quake3e-vulkan-ss.x64.exe" "$L" "${WIN[@]}" +set fs_basepath "$BASE" +set fs_homepath "$HP" +wait 100 +r14a_marker +quit
  snap "after run 2 (no seed line; r14a_marker 7 kept; q3config.cfg unchanged)" ;;
noover)
  printf 'seta name "Upstream"\n' > "$H/baseq3/q3config.cfg"
  printf 'seta name "OursAlready"\n' > "$H/baseq3/q3config-ss.cfg"
  snap before
  run "$BIN/quake3e-vulkan-ss.x64.exe" "$L" "${WIN[@]}" +set fs_basepath "$BASE" +set fs_homepath "$HP" +wait 100 +name +quit
  snap after ;;
autoexec)
  echo 'seta cg_fov 100' > "$H/baseq3/autoexec.cfg"
  echo 'seta cg_fov 110' > "$H/baseq3/autoexec-ss.cfg"
  snap before
  run "$BIN/quake3e-vulkan-ss.x64.exe" "$L" "${WIN[@]}" +set fs_basepath "$BASE" +set fs_homepath "$HP" +wait 100 +cg_fov +quit
  snap after ;;
ded)
  printf 'seta sv_hostname "SeedServer"\nseta g_motd "seeded"\n' > "$H/baseq3/q3config_server.cfg"
  snap before
  run "$BIN/quake3e-ss.ded.x64.exe" "$L" +set fs_basepath "$BASE" +set fs_homepath "$HP" +set dedicated 1 +set logfile 3 +set net_port 27991 +map q3dm1 +sv_hostname +seta g_motd changed +wait 20 +quit
  snap after ;;
child)
  printf 'seta name "Upstream"\n' > "$H/baseq3/q3config.cfg"
  snap before
  run "$BIN/quake3e-vulkan-ss.x64.exe" "$L" --child --noactivate +set cl_splitChild 2 +set cl_splitWindowRect "64 424 1280 360" \
    +set fs_basepath "$BASE" +set fs_homepath "$HP" +set logfile 3 +set com_introplayed 1 +set in_gamepad 0 \
    +wait 100 +seta cl_splitMenuColor "0 1 0" +name +wait 20 +quit
  for f in "$H/baseq3"/qconsole-child*.log; do [ -f "$f" ] && { cat "$f" >> "$W/$L.log"; rm -f "$f"; }; done
  snap after ;;
*) echo "unknown case" >&2; exit 2 ;;
esac
tasklist 2>/dev/null | grep -ci quake3e | sed 's/^/quake3e processes after: /' >> "$W/$L-ls.txt"
