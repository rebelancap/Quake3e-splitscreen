#!/bin/sh
# usage: scripts/tests/r28-run.sh <local|urtlocal|remote> [lan ip]
# R28 (upstream sync ec431ef9: hunk banks, CL_ShutdownVMs, CM_ClearMap) lifecycle checks, Vulkan Release exe.
#   local    : r18-run.sh q3 r28-local.cfg on devmap q3dm1 -> q3dm2      (log work/r28-q3-local.log)
#   urtlocal : r18-run.sh urt (home work/urttest) on ut4_casa -> ut4_turnpike (log work/r28-urt-local.log)
#   remote   : dedicated baseq3 server (quake3e-ss.ded) on <lan ip>:27970 + client running r28-remote.cfg
#              (logs work/r28-q3-remote.log, work/r28-q3-remote-server.log); kills every process it started.
# Screenshots -> work/r28-<run>-*.jpg.  Refuses to start while any quake3e process runs.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
T="$REPO/scripts/tests"
if tasklist 2>/dev/null | grep -i "quake3e" >/dev/null; then
  echo "r28-run: a quake3e process is running; not starting" >&2; tasklist | grep -i quake3e >&2; exit 1
fi
shots() {   # $1 = run name: r18-r28-* (from r18-run.sh) -> work/r28-<run>-*
  for f in "$REPO"/work/r18-r28-*.jpg; do [ -f "$f" ] && mv "$f" "$REPO/work/r28-$1-$(basename "$f" | sed 's/^r18-r28-//')"; done
}
case "$1" in
  local)
    "$T/r18-run.sh" q3 r28-local.cfg r28-q3-local q3dm1 +set r28map2 "devmap q3dm2"
    mv "$REPO/work/r18-r28-q3-local.log" "$REPO/work/r28-q3-local.log"; shots q3-local ;;
  urtlocal)
    R18_URTHOME="$REPO/work/urttest" "$T/r18-run.sh" urt r28-local.cfg r28-urt-local ut4_casa +set r28map2 "devmap ut4_turnpike"
    mv "$REPO/work/r18-r28-urt-local.log" "$REPO/work/r28-urt-local.log"
    mv "$REPO/work/r18-r28-urt-local-home.txt" "$REPO/work/r28-urt-local-home.txt" 2>/dev/null; shots urt-local ;;
  remote)
    IP="${2:-$(ipconfig | tr -d '\r' | grep -A8 '10G Ethernet' | grep IPv4 | sed 's/.*: //')}"
    [ -n "$IP" ] || { echo "no LAN IP (pass it as the 2nd argument)"; exit 1; }
    BASE="${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}"
    HOME_Q3="$REPO/work/q3test"; HOME_DED="$REPO/work/q3home-ded"
    mkdir -p "$HOME_Q3/baseq3/screenshots" "$HOME_DED/baseq3"
    cp "$T/"*.cfg "$HOME_Q3/baseq3/"
    rm -f "$HOME_Q3/baseq3/qconsole.log" "$HOME_DED/baseq3/qconsole.log" "$HOME_Q3"/baseq3/screenshots/r28-remote-*.jpg
    echo "server address $IP:27970"
    "$REPO/build/Release/quake3e-ss.ded.x64.exe" +set fs_basepath "$BASE" +set fs_homepath "$(cygpath -w "$HOME_DED")" \
      +set dedicated 1 +set net_ip "$IP" +set net_port 27970 +set sv_pure 1 +set sv_maxclientsPerIP 3 +set rconpassword r9test \
      +set logfile 4 +set developer 1 +set sv_hostname r28-remote +map q3dm17 &
    SERVER=$!
    sleep 6
    "$REPO/build/Release/quake3e-vulkan-ss.x64.exe" --noactivate +set fs_basepath "$BASE" +set fs_homepath "$(cygpath -w "$HOME_Q3")" \
      +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set vid_xpos 64 +set vid_ypos 64 \
      +set logfile 4 +set com_introplayed 1 +set in_gamepad 0 +set developer 1 +set r9connect "connect $IP:27970" +wait 200 +exec r28-remote.cfg
    sleep 2
    kill $SERVER 2>/dev/null; taskkill //F //IM quake3e-ss.ded.x64.exe >/dev/null 2>&1
    cp "$HOME_DED/baseq3/qconsole.log" "$REPO/work/r28-q3-remote-server.log" 2>/dev/null
    cp "$HOME_Q3/baseq3/qconsole.log" "$REPO/work/r28-q3-remote.log"
    for s in "$HOME_Q3"/baseq3/screenshots/r28-remote-*.jpg; do [ -f "$s" ] && mv "$s" "$REPO/work/r28-q3-$(basename "$s" | sed 's/^r28-//')"; done ;;
  *) echo "usage: $0 <local|urtlocal|remote> [lan ip]"; exit 1 ;;
esac
tasklist 2>/dev/null | grep -i quake3e && echo "r28-run: WARNING quake3e still running" >&2
exit 0
