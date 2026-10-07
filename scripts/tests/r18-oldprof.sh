#!/bin/sh
# R18: plants profile "r18old" with the R11 built-in q3ut4 pad layout in the current dir (a homepath);
# used as R18_PRE by r18-binds (the runner's restore removes it).
mkdir -p profiles q3ut4/profiles
printf 'name "r18old"\njoy_crouchToggle "0"\n' > profiles/r18old.cfg
cat > q3ut4/profiles/r18old.cfg <<'X'
// Quake3e-splitscreen player profile "r18old": q3ut4 pad buttons and player settings. Written by the engine.
bind PAD_A "+moveup"
bind PAD_B "+movedown"
bind PAD_X "+button5"
bind PAD_Y "+button7"
bind PAD_BACK "+scores"
bind PAD_START "padmenu"
bind PAD_L3 "+button8"
bind PAD_R3 "+button6"
bind PAD_LB "weapprev"
bind PAD_RB "weapnext"
bind PAD_DPAD_UP "ut_radio 2 6"
bind PAD_DPAD_DOWN "+button3"
bind PAD_DPAD_LEFT "ut_radio 1 1"
bind PAD_DPAD_RIGHT "ut_radio 5 1"
bind PAD_MISC1 "ut_itemuse"
bind PAD_LT "ut_zoomin"
bind PAD_RT "+attack"
X
