#!/bin/sh
# usage: scripts/tests/r11-remote.sh <client exe> <logname> [lan ip]
# R11: online refusal messaging.  Like r9-remote.sh (dedicated baseq3 server on <lan ip>:27970,
# sv_maxclientsPerIP 3, homepath work/q3home-ded) but runs r11-remote.cfg: pads join to P4,
# which is refused; no server restart.  Kills every process it started.
# Logs: work/<logname>.log (client), work/<logname>-server.log; screenshots r11-remote-* -> work/<logname>-*.jpg.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$1"; LOG="$2"; IP="${3:-$(ipconfig | tr -d '\r' | grep -A8 '10G Ethernet' | grep IPv4 | sed 's/.*: //')}"
if [ -z "$IP" ]; then echo "no LAN IP (pass it as the 3rd argument)"; exit 1; fi
BASE="${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}"
HOME_Q3="${Q3HOME:-$REPO/work/q3test}"; HOME_DED="$REPO/work/q3home-ded"
mkdir -p "$HOME_Q3/baseq3" "$HOME_DED/baseq3"
cp "$REPO/scripts/tests/"*.cfg "$HOME_Q3/baseq3/"
rm -f "$HOME_Q3/baseq3/qconsole.log" "$HOME_DED/baseq3/qconsole.log" "$HOME_DED/baseq3/q3config_server-ss.cfg" "$REPO/work/$LOG-server.log"
rm -f "$HOME_Q3"/baseq3/screenshots/r11-remote-*.jpg
echo "server address $IP:27970"
"$REPO/build/Release/quake3e-ss.ded.x64.exe" +set fs_basepath "$BASE" +set fs_homepath "$(cygpath -w "$HOME_DED")" \
  +set dedicated 1 +set net_ip "$IP" +set net_port 27970 +set sv_pure 1 +set sv_maxclientsPerIP 3 +set rconpassword r9test \
  +set logfile 4 +set developer 1 +set sv_hostname r11-remote +map q3dm17 &
SERVER=$!
sleep 6
"$EXE" +set fs_basepath "$BASE" +set fs_homepath "$(cygpath -w "$HOME_Q3")" +set r_fullscreen 0 +set r_mode -1 \
  +set r_customwidth 1280 +set r_customheight 720 +set logfile 4 +set com_introplayed 1 +set in_gamepad 0 \
  +set developer 1 +set r9connect "connect $IP:27970" +wait 200 +exec r11-remote.cfg
sleep 2
kill $SERVER 2>/dev/null; taskkill //F //IM quake3e-ss.ded.x64.exe >/dev/null 2>&1
cat "$HOME_DED/baseq3/qconsole.log" >> "$REPO/work/$LOG-server.log" 2>/dev/null
cp "$HOME_Q3/baseq3/qconsole.log" "$REPO/work/$LOG.log"
for s in "$HOME_Q3"/baseq3/screenshots/r11-remote-*.jpg; do [ -f "$s" ] && mv "$s" "$REPO/work/$LOG-$(basename "$s" .jpg | sed "s/^r11-remote-//").jpg"; done
