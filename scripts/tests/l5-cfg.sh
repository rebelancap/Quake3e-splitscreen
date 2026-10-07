#!/bin/bash
# usage: scripts/tests/l5-cfg.sh <case>
# L5 (configs side by side with upstream): runs our build (or upstream's copied exe) against a scratch
# home path under work/l5-home-<case>/ (fresh unless L5_KEEP=1), base path ~/dev/q3data (Q3DATA) or
# work/l5-side/ for the side-by-side case.  Evidence: work/l5-<case>.log (+ -ls.txt before/after
# listing, sha256 sums).  Cases: fresh seed noover autoexec autoexec-none side mod ded.
# Refuses to start beside a running quake3e client; kills nothing it did not start.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CASE="$1"
BIN="$REPO/build/release-linux-x86_64"
BASE="${Q3DATA:-$HOME/dev/q3data}"
W="$REPO/work"
for p in /proc/[0-9]*; do
  e="$(readlink "$p/exe" 2>/dev/null)" || continue; e="${e##*/}"; e="${e% (deleted)}"
  case "$e" in quake3e-vulkan-ss.x64|quake3e-ss.x64|quake3e.x64|quake3e-vulkan.x64|quake3e-ss.ded.x64|quake3e.ded.x64)
    echo "a quake3e process is already running (${p#/proc/} $e); not starting another" >&2; exit 3;; esac
done
if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; then
  P="$(pgrep -o plasmashell || pgrep -o kwin_wayland)"
  [ -n "$P" ] && export $(tr '\0' '\n' < /proc/$P/environ | grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR|XDG_SESSION_TYPE)=' | xargs)
fi
WIN=(+set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set logfile 3 +set com_introplayed 1 +set in_gamepad 0)
# run <exe> <home> <game> <logname> [args...]: one client run, log appended to work/<logname>.log
run() {
  local exe="$1" home="$2" game="$3" log="$4"; shift 4
  echo "=== $(date +%T) run $(basename "$exe") home=${home#$REPO/} $*" >> "$W/$log-ls.txt"
  timeout 120 "$exe" +set fs_homepath "$home" "$@"
  echo "rc $?" >> "$W/$log-ls.txt"
  [ -f "$home/$game/qconsole.log" ] && { cat "$home/$game/qconsole.log" >> "$W/$log.log"; rm -f "$home/$game/qconsole.log"; }
}
snap() {	# snap <home> <log> <label>
  { echo "--- $3: ls -lR (mtime) + sha256"; (cd "$1" && find . -type f ! -name '*.log' -printf '%TT %s %p\n' | sort -k3; find . -name 'q3config*.cfg' -exec sha256sum {} +); } >> "$W/$2-ls.txt"
}
H="$W/l5-home-$CASE"
[ "$L5_KEEP" = 1 ] || rm -rf "$H"
mkdir -p "$H/baseq3"
L="l5-$CASE"; [ "$L5_KEEP" = 1 ] || rm -f "$W/$L.log" "$W/$L-ls.txt"
VK="$BIN/quake3e-vulkan-ss.x64"
case "$CASE" in
fresh)
  snap "$H" "$L" before
  run "$VK" "$H" baseq3 "$L" +set fs_basepath "$BASE" "${WIN[@]}" +wait 200 +quit
  snap "$H" "$L" after ;;
seed|noover)
  if [ "$CASE" = seed ] || [ ! -f "$W/l5-home-seed/baseq3/q3config.cfg" ]; then
    H="$W/l5-home-seed"; rm -rf "$H"; mkdir -p "$H/baseq3"
    printf '// upstream-style config\nunbindall\nbind x "say seeded"\nseta name "SeedTest"\nseta sensitivity "3"\n' > "$H/baseq3/q3config.cfg"
  else H="$W/l5-home-seed"; fi
  snap "$H" "$L" before
  if [ "$CASE" = seed ]; then
    run "$VK" "$H" baseq3 "$L" +set fs_basepath "$BASE" "${WIN[@]}" +wait 200 +name +bind x +quit
  else
    # first run changes a cvar, the second must keep it and print no seed line
    run "$VK" "$H" baseq3 "$L" +set fs_basepath "$BASE" "${WIN[@]}" +wait 100 +seta sensitivity 7 +wait 20 +quit
    snap "$H" "$L" "after run 1 (sensitivity 7)"
    run "$VK" "$H" baseq3 "$L" +set fs_basepath "$BASE" "${WIN[@]}" +wait 100 +sensitivity +name +quit
  fi
  snap "$H" "$L" after ;;
autoexec|autoexec-none)
  echo 'seta cg_fov 100' > "$H/baseq3/autoexec.cfg"
  [ "$CASE" = autoexec ] && echo 'seta cg_fov 110' > "$H/baseq3/autoexec-ss.cfg"
  snap "$H" "$L" before
  run "$VK" "$H" baseq3 "$L" +set fs_basepath "$BASE" "${WIN[@]}" +wait 100 +cg_fov +quit
  snap "$H" "$L" after ;;
side)
  S="$W/l5-side"; rm -rf "$S"; mkdir -p "$S"
  cp "$HOME/Games/quake3/quake3e/quake3e-vulkan.x64" "$S/"; cp "$VK" "$S/"
  ln -s "$BASE/baseq3" "$S/baseq3"
  printf 'seta cg_fov 110\n' > "$H/baseq3/autoexec-ss.cfg"
  mkdir -p "$H/profiles"; printf '// test profile\nname "Ada"\n' > "$H/profiles/ada.cfg"
  snap "$H" "$L" "before"
  # no fs_basepath: both use their own folder, like a user's install
  run "$S/quake3e-vulkan.x64" "$H" baseq3 "$L" "${WIN[@]}" +wait 100 +seta name UpstreamOne +quit
  snap "$H" "$L" "after upstream run 1 (writes q3config.cfg)"
  run "$S/quake3e-vulkan-ss.x64" "$H" baseq3 "$L" "${WIN[@]}" +wait 100 +seta sensitivity 7 +wait 20 +quit
  snap "$H" "$L" "after ours run 1 (seeds q3config-ss.cfg, sensitivity 7)"
  run "$S/quake3e-vulkan.x64" "$H" baseq3 "$L" "${WIN[@]}" +wait 100 +sensitivity +name +quit
  snap "$H" "$L" "after upstream run 2"
  run "$S/quake3e-vulkan-ss.x64" "$H" baseq3 "$L" "${WIN[@]}" +wait 100 +sensitivity +name +quit
  snap "$H" "$L" "after ours run 2" ;;
mod)
  # a scratch mod folder in the home path (no missionpack in ~/dev/q3data): baseq3 data + l5mod/q3config.cfg
  mkdir -p "$H/l5mod"
  printf 'seta name "ModSeed"\nseta sensitivity "4"\n' > "$H/l5mod/q3config.cfg"
  printf 'seta name "BaseSeed"\n' > "$H/baseq3/q3config.cfg"
  snap "$H" "$L" before
  # start in baseq3 (seeds baseq3), then switch to the mod at run time (game_restart) and back
  # (game_restart resets the CVAR_TEMP logfile, so the engine's stdout is the log here)
  run "$VK" "$H" baseq3 "$L" +set fs_basepath "$BASE" "${WIN[@]}" +wait 100 +name +game_restart l5mod +wait 100 +name +quit > "$W/$L-stdout.log" 2>&1
  snap "$H" "$L" after ;;
ded)
  printf 'seta sv_hostname "SeedServer"\nseta g_motd "seeded"\n' > "$H/baseq3/q3config_server.cfg"
  snap "$H" "$L" before
  run "$BIN/quake3e-ss.ded.x64" "$H" baseq3 "$L" +set fs_basepath "$BASE" +set dedicated 1 +set logfile 3 +map q3dm1 +sv_hostname +quit
  snap "$H" "$L" after ;;
*) echo "unknown case" >&2; exit 2 ;;
esac
