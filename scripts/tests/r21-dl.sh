#!/bin/sh
# usage: scripts/tests/r21-dl.sh <logname> [client args...]
# R21: Urban Terror map download, reproduced locally.  Starts
#   - a static HTTP server (r21-http.py, 127.0.0.1:27990) serving work/r21www,
#     so "<sv_dlURL>/q3ut4/ut4_r21dl.pk3" = work/r21www/maps/q3ut4/ut4_r21dl.pk3,
#   - our dedicated server like a real UrT 4.3 server: fs_game q3ut4 (the UrT engine's default
#     fs_game, so it is in systeminfo), map ut4_r21dl from work/r21srv/q3ut4/ut4_r21dl.pk3
#     (a stock bsp renamed), sv_pure 0, sv_allowDownload $SV_ALLOWDL (default 0, as
#     74.91.113.242), sv_dlURL $DLURL (default scheme-less "127.0.0.1:27990/maps", as that server),
#   - the client in a fresh home work/urttest seeded from a copy of the maintainer's
#     work/urthome/q3ut4/q3config-ss.cfg (read only), running r21-dl.cfg (connects, waits).
# RATE = HTTP bytes/s (default 2000000; the test pak is 8.6 MB, so the download screen shows ~4 s).
# Kills every process it started.  Logs: work/<logname>.log (client), -server.log, -http.log;
# screenshots r21-* -> work/<logname>-*.jpg.  EXE_DIR overrides build/Release.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="$1"; shift
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
DED="${EXE_DIR:-$REPO/build/Release}/quake3e-ss.ded.x64.exe"
IP="${IP:-$(ipconfig | tr -d '\r' | grep -A8 '10G Ethernet' | grep IPv4 | sed 's/.*: //')}"
DLURL="${DLURL-127.0.0.1:27990/maps}"; SV_ALLOWDL="${SV_ALLOWDL:-0}"; MAP="${MAP:-ut4_r21dl}"
URT="${URT_BASEPATH:?set URT_BASEPATH to your Urban Terror 4.3 folder}"
HC="$REPO/work/urttest"; HS="$REPO/work/r21srv"
# fixture (work/ only, never committed): ut4_r21dl.pk3 = UrT's stock ut4_dressingroom.bsp renamed
# + 8 MB of incompressible padding, in the server's home and in the HTTP root
if [ ! -f "$HS/q3ut4/ut4_r21dl.pk3" ] || [ ! -f "$REPO/work/r21www/maps/q3ut4/ut4_r21dl.pk3" ]; then
  mkdir -p "$HS/q3ut4" "$REPO/work/r21www/maps/q3ut4"
  powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; Add-Type -AssemblyName System.IO.Compression;
    \$src = [IO.Compression.ZipFile]::OpenRead('$URT\q3ut4\zUrT43_002.pk3'); \$e = \$src.GetEntry('maps/ut4_dressingroom.bsp');
    \$out = '$(cygpath -w "$HS/q3ut4/ut4_r21dl.pk3")'; if (Test-Path \$out) { Remove-Item \$out };
    \$z = [IO.Compression.ZipFile]::Open(\$out, 'Create'); \$w = \$z.CreateEntry('maps/ut4_r21dl.bsp').Open(); \$r = \$e.Open(); \$r.CopyTo(\$w); \$r.Close(); \$w.Close();
    \$w = \$z.CreateEntry('r21/pad.bin', [IO.Compression.CompressionLevel]::NoCompression).Open(); \$b = New-Object byte[] 8388608; (New-Object Random 21).NextBytes(\$b); \$w.Write(\$b, 0, \$b.Length); \$w.Close();
    \$z.Dispose(); \$src.Dispose()"
  cp "$HS/q3ut4/ut4_r21dl.pk3" "$REPO/work/r21www/maps/q3ut4/ut4_r21dl.pk3"
fi
rm -rf "$HC"; mkdir -p "$HC/q3ut4"
cp "$REPO/work/urthome/q3ut4/q3config-ss.cfg" "$HC/q3ut4/q3config-ss.cfg"
[ -n "$SEED_EXTRA" ] && echo "$SEED_EXTRA" >> "$HC/q3ut4/q3config-ss.cfg"
cp "$REPO/scripts/tests/r21-dl.cfg" "$REPO/scripts/tests/r21-dl2.cfg" "$REPO/scripts/tests/r10-keys.cfg" "$HC/q3ut4/"
rm -f "$HS/q3ut4/qconsole.log" "$HS/q3ut4/q3config_server-ss.cfg"
python -u "$(cygpath -w "$REPO/scripts/tests/r21-http.py")" 27990 "$(cygpath -w "$REPO/work/r21www")" "${RATE:-2000000}" > "$REPO/work/$LOG-http.log" 2>&1 &
HTTP=$!
"$DED" +set fs_basepath "$URT" +set fs_homepath "$(cygpath -w "$HS")" +set fs_game q3ut4 \
  +set dedicated 1 +set net_ip "$IP" +set net_port 27970 +set sv_pure 0 +set sv_allowDownload "$SV_ALLOWDL" \
  +set sv_dlURL "$DLURL" +set logfile 4 +set developer 1 +set sv_hostname r21-dl +map "$MAP" &
SERVER=$!
sleep 6
timeout 100 "$EXE" +set fs_basepath "$URT" +set fs_homepath "$(cygpath -w "$HC")" +set fs_basegame q3ut4 \
  +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 \
  +set logfile 4 +set com_introplayed 1 +set in_gamepad 0 +set developer 1 \
  +set r21connect "connect $IP:27970" "$@" +wait 200 +exec r21-dl.cfg
sleep 1
kill $SERVER 2>/dev/null; taskkill //F //IM quake3e-ss.ded.x64.exe >/dev/null 2>&1
kill $HTTP 2>/dev/null
cp "$HS/q3ut4/qconsole.log" "$REPO/work/$LOG-server.log" 2>/dev/null
cp "$HC/q3ut4/qconsole.log" "$REPO/work/$LOG.log"
grep -n -i "allowdownload\|urtDownload\|autodownload" "$HC/q3ut4/q3config-ss.cfg" > "$REPO/work/$LOG-cfg.txt"
find "$HC" -name "*.pk3*" >> "$REPO/work/$LOG-cfg.txt"
for s in "$HC"/q3ut4/screenshots/r21-*.jpg; do [ -f "$s" ] && mv "$s" "$REPO/work/$LOG-$(basename "$s" .jpg | sed "s/^r21-//").jpg"; done
