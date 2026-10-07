#!/bin/sh
# usage: scripts/tests/r11-profiles.sh
# R11: a profile made in baseq3 reused in Urban Terror.  Both runs share the homepath work/r11prof
# (fresh each time), so the all-mods file profiles/ada.cfg is common and the per-mod files are
# baseq3/profiles/ada.cfg and q3ut4/profiles/ada.cfg.  Logs: work/r11-prof-a.log, work/r11-prof-b.log;
# the profile files are copied to work/r11-prof-files.txt.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
H="$REPO/work/r11prof"; HW="$(cygpath -w "$H")"
rm -rf "$H"; mkdir -p "$H/baseq3" "$H/q3ut4"
cp "$REPO/scripts/tests/r11-prof-a.cfg" "$H/baseq3/"; cp "$REPO/scripts/tests/r11-prof-b.cfg" "$H/q3ut4/"
echo "// r11 test config" > "$H/q3ut4/q3config-ss.cfg"
COMMON="+set fs_homepath $HW +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set in_gamepad 0 +set developer 1 +set logfile 3 +set com_introplayed 1 +set cl_splitP1Input kbm"
"$EXE" +set fs_basepath "${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}" $COMMON +devmap q3dm1 +wait 300 +exec r11-prof-a.cfg
cp "$H/baseq3/qconsole.log" "$REPO/work/r11-prof-a.log"
"$EXE" +set fs_basepath "${URT_BASEPATH:?set URT_BASEPATH to your Urban Terror 4.3 folder}" +set fs_basegame q3ut4 $COMMON +devmap ut4_casa +wait 300 +exec r11-prof-b.cfg
cp "$H/q3ut4/qconsole.log" "$REPO/work/r11-prof-b.log"
{ for f in profiles/ada.cfg baseq3/profiles/ada.cfg q3ut4/profiles/ada.cfg; do echo "=== $f"; cat "$H/$f" 2>/dev/null || echo "(missing)"; done; } > "$REPO/work/r11-prof-files.txt"
cat "$REPO/work/r11-prof-files.txt"
