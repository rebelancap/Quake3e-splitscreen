#!/bin/bash
# usage: scripts/tests/l3-run.sh <vk|gl> <cfg> <name> <map|-> [extra args...]
# L3 (Independent mode on the Linux desktop) runner: the Linux counterpart of r13-run.sh.  Runs
#   build/release-linux-x86_64/quake3e-vulkan-ss.x64 (vk) or quake3e-ss.x64 (gl) --independent --noactivate ...
# (L3_FLAGS replaces the flags, e.g. "--noactivate" for a Together start) on the desktop session
# (XWayland :0; the engine switches SDL to its x11 driver itself), game data read-only from
# ~/dev/q3data, homepath work/q3home.  Windows are tiled in the display's usable area (L3_AREA
# "x y w h" sets cl_splitIndepArea).  Children get "+set developer 1 +exec r13-child@@.cfg"
# unless L3_CHILDARGS (passed with literal quotes: Linux's main() joins argv without them).
# Evidence (prefix L3_PREFIX, default l3): work/<pfx>-<name>.log (coordinator),
# work/<pfx>-<name>-child<N>.log, screenshots r13-* -> work/<pfx>-<name>-*.jpg,
# work/<pfx>-<name>-x11.txt (every change of the X server's view: each game window's xwininfo
# rect and focused state + _NET_ACTIVE_WINDOW, sampled every 0.25 s), work/<pfx>-<name>-q3config.txt (q3config
# mtime / child marker "0 1 0"), work/<pfx>-<name>-procs.txt (quake3e processes after the run).
# Refuses to start beside a running quake3e client (ours or upstream's); kills only the pids it started (coordinator +
# the children its log names) if any outlive the run.  L3_TIMEOUT (default 300 s).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
PFX="${L3_PREFIX:-l3}"
BASE="${Q3DATA:-$HOME/dev/q3data}"
HOMEDIR="$REPO/work/q3home"; GAMEDIR="baseq3"
CHILDARGS="${L3_CHILDARGS:-+set developer 1 +exec r13-child@@.cfg}"
case "$R" in vk) EXENAME=quake3e-vulkan-ss.x64;; gl) EXENAME=quake3e-ss.x64;; *) echo "renderer: vk|gl" >&2; exit 2;; esac
EXE="${EXE_DIR:-$REPO/build/release-linux-x86_64}/$EXENAME"
# process names are cut to 15 characters ("quake3e-vulkan-"): match our two clients
Q3PAT='^quake3e-(vulkan-|ss\.x64$)'
for p in /proc/[0-9]*; do
  e="$(readlink "$p/exe" 2>/dev/null)" || continue; e="${e##*/}"; e="${e% (deleted)}"
  case "$e" in quake3e-vulkan-ss.x64|quake3e-ss.x64|quake3e.x64|quake3e-vulkan.x64)
    echo "a quake3e client is already running (${p#/proc/} $e); not starting another" >&2; exit 3;; esac
done
P="$(pgrep -o plasmashell || pgrep -o kwin_wayland)"
if [ -n "$P" ]; then
  export $(tr '\0' '\n' < /proc/$P/environ | grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP)=' | xargs)
fi
mkdir -p "$HOMEDIR/$GAMEDIR"
[ -f "$HOMEDIR/$GAMEDIR/q3config-ss.cfg" ] || [ -f "$HOMEDIR/$GAMEDIR/q3config.cfg" ] || echo "// linux test config" > "$HOMEDIR/$GAMEDIR/q3config-ss.cfg"
cp "$REPO/scripts/tests/"*.cfg "$HOMEDIR/$GAMEDIR/"
SHOTS="$HOMEDIR/$GAMEDIR/screenshots"
rm -f "$SHOTS"/*.jpg "$HOMEDIR/$GAMEDIR"/qconsole*.log
if [ "$MAP" = "-" ]; then START="+wait 200"; else START="+devmap $MAP +wait 300"; fi
AREA=(); [ -n "$L3_AREA" ] && AREA=(+set cl_splitIndepArea "\"$L3_AREA\"")

STOP="$REPO/work/.l3-stop"; rm -f "$STOP"
# q3config watcher (a child must never write it)
Q3C="$HOMEDIR/$GAMEDIR/q3config-ss.cfg"
WATCH="$REPO/work/$PFX-$NAME-q3config.txt"
echo "# t(0.5 s) mtime marker(0 1 0)" > "$WATCH"
( t=0; while [ ! -f "$STOP" ]; do
    m=$(stat -c %Y "$Q3C" 2>/dev/null); k=no; grep -q '"0 1 0"' "$Q3C" 2>/dev/null && k=yes
    echo "$t $m $k" >> "$WATCH"; sleep 0.5; t=$((t+1)); done ) &
# X server watcher: the game windows' geometry and the active window, on every change
X11="$REPO/work/$PFX-$NAME-x11.txt"
echo "# $(date +%T.%N | cut -c1-12) X11 view of :0 (each game window: id, xwininfo absolute x,y wxh, _NET_WM_STATE_FOCUSED or -, title; then _NET_ACTIVE_WINDOW)" > "$X11"
( last=""; while [ ! -f "$STOP" ]; do
    cur="$(wmctrl -l 2>/dev/null | grep -i 'quake 3' | sort | while read -r id _desk _host title; do
             echo "$id $(xwininfo -id "$id" 2>/dev/null | awk -F: '/Absolute upper-left X/{x=$2}/Absolute upper-left Y/{y=$2}/Width/{w=$2}/Height/{h=$2}END{gsub(/ /,"",x);gsub(/ /,"",y);gsub(/ /,"",w);gsub(/ /,"",h);printf "%s,%s %sx%s",x,y,w,h}') $(xprop -id "$id" _NET_WM_STATE 2>/dev/null | grep -o '_NET_WM_STATE_FOCUSED' || echo -n '-') $title"
           done; xprop -root _NET_ACTIVE_WINDOW 2>/dev/null)"
    if [ "$cur" != "$last" ]; then { echo "== $(date +%T.%N | cut -c1-12)"; echo "$cur"; } >> "$X11"; last="$cur"; fi
    sleep 0.25; done ) &

timeout "${L3_TIMEOUT:-300}" "$EXE" ${L3_FLAGS---independent --noactivate} +set fs_basepath "$BASE" +set fs_homepath "$HOMEDIR" \
  +set com_hunkMegs 128 "${AREA[@]}" \
  +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 \
  +set in_gamepad 0 +set developer 1 +set logfile 4 +set com_introplayed 1 \
  +set cl_splitChildArgs "\"$CHILDARGS\"" "$@" \
  $START +exec "$CFG" &
COORD=$!
echo "$COORD" > "$REPO/work/.l3-coord-pid"
wait $COORD
RC=$?
sleep 2
touch "$STOP"; sleep 1
LOG="$HOMEDIR/$GAMEDIR/qconsole.log"
{ echo "# $(date +%T) quake3e processes 2 s after the coordinator (pid $COORD, rc $RC) ended:"; pgrep -a "$Q3PAT"; echo "(end: $(pgrep -c "$Q3PAT") process(es))"; } > "$REPO/work/$PFX-$NAME-procs.txt"
# never leave one of ours behind: the coordinator and the children its log names
for pid in $COORD $(grep -o "window starting: pid [0-9]*" "$LOG" 2>/dev/null | grep -o "[0-9]*$"); do
  if kill -0 "$pid" 2>/dev/null && grep -q quake3e "/proc/$pid/cmdline" 2>/dev/null; then
    echo "killing leftover pid $pid" | tee -a "$REPO/work/$PFX-$NAME-procs.txt"; kill -9 "$pid"
  fi
done
rm -f "$STOP" "$REPO/work/.l3-coord-pid"
cp "$LOG" "$REPO/work/$PFX-$NAME.log" 2>/dev/null
for f in "$HOMEDIR/$GAMEDIR"/qconsole-child*.log; do
  [ -f "$f" ] || continue
  b="$(basename "$f" .log)"
  cp "$f" "$REPO/work/$PFX-$NAME-${b#qconsole-}.log"
done
for f in "$SHOTS"/*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f" .jpg)"; b="${b#r13-}"
  mv "$f" "$REPO/work/$PFX-$NAME-$b.jpg"
done
rm -f "$HOMEDIR"/pk3cache-child*.dat "$HOMEDIR/$GAMEDIR"/pk3cache-child*.dat
exit $RC
