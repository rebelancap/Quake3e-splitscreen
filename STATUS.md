# STATUS

## Current state

**1.0.0 published 2026-10-07:**
https://github.com/rebelancap/Quake3e-splitscreen/releases/tag/v1.0.0
(GitHub Actions on tag `v1.0.0`: Windows zip and Linux tar.gz, both jobs
green on the first run). Next: the maintainer smoke-tests the downloaded
archives on both boxes; later work continues on `main` as `1.0.1.N` dev
builds until the next release number is chosen.

**1.0.0 (2026-10-07): first public release.** Tree = 0.0.0.27 + the L6
Linux check (build, headless suite, portability review: notes only) + the
public-repo scrub (no personal names, user paths or host names; test
runners take `$Q3_BASEPATH` / `$URT_BASEPATH`). The maintainer's Windows
passes of 0.0.0.26/27 were clean, including the live UrT server map
download. Public branch `main` = upstream's history + one squashed commit
(our 180 dev commits stay on the private `splitscreen` branch and the Linux
box mirror; their messages contain personal info and are never pushed to
GitHub). Releases are built by `.github/workflows/release.yml` on tag
`v<VERSION>` (Linux inside an Ubuntu 22.04 container for older glibc).
Work continues on `main`.

**R21 (0.0.0.27, 2026-10-07, verified against a local UrT-like server; the
maintainer later confirmed it on a live server): Urban Terror map download.** Joining a UrT server no longer
restarts the game: UrT servers send `fs_game q3ut4`, which is our base game
(`fs_game: server's "q3ut4" is our base game, no game restart`). Maps
download like UrT 4.3's client: switch `cl_autodownload` (UrT's, default 1;
`cl_allowdownload 0` from UrT's default.cfg is not the switch), only
`<map>.pk3`, HTTP from the server's `sv_dlURL` even with `sv_allowDownload 0`
(UrT's qagame forces 0), UDP only if a server allows it, else a clear error;
saved to `q3ut4/download/` (a search path below q3ut4 in every UrT run, so
UrT's own downloaded maps load too; no qvm/menu/cfg read from it). Progress on
UrT's loading screen, then the engine reconnects and joins. Every decision is
logged (`download: server sv_allowDownload=<n> sv_dlURL=<url>; missing: ...;
method: http|udp|none (...)`). **The console log survives a game restart**
(closed before `FS_Shutdown`, reopened in append mode; `logfile` is
`CVAR_NORESTART`). R20's one-time `cl_allowDownload` switch is removed.

**R20 (0.0.0.26, 2026-10-07, headless-verified only): The maintainer's third
real-pad pass.** **Independent tiles render like the same-size Together
cell:** a window that is one of several tiles (`CL_IndepTiled`: every child,
P1's window once it is a share of the area) goes through the viewport layer
as one cell (`CL_SplitCells`): its cgame is told 640x480 with HUD shape 4:3
Centered, the vertical FOV is kept and the horizontal widened, exactly like
Together (was: pass-through, the cgame got the tile's own wide size -> very
narrow vertical FOV and a stretched HUD; log `window: tile WxH is a cell:
cgame screen 640x480 ...`). The R19 **backdrop window is removed** (cvar,
restack, shell-state logging); tiles stay marked fullscreen; Joyxoff:
disable its bindings for this mode. **Single Player arenas** take extra
local players (`SV_GetChallenge` ignored every getchallenge in SP; now a
listen server answers 127.x; `spmap` latches `sv_maxclients` 8 +
`cl_splitMaxPlayers` - 1). **UrT downloads:** UrT's `default.cfg` sets
`cl_allowdownload 0` -- turned on once in q3ut4 (`cl_urtDownloadDefault`),
libcurl static as before, start log `download: cl_allowDownload 1;
HTTP/FTP ... via libcurl/8.4.0 Schannel (built in)`. **Any pad button or
trigger skips a cinematic** (intro idlogo.roq, Q3 and UrT), consumed.
**First-launch hint** at the Together main menu with a pad: "Start a game
first, then friends hold A to join" until the first game starts
(`cl_splitSeenHint 1`).


**R19b (0.0.0.25, 2026-10-07, headless-verified only): review fix to R19's
duplicate rule.** A pad taken for a second listing of another pad (mirrored
presses) now joins a window's key list only after matching for 1 s plus a
confirming press; undone (150 ms of different input) it is taken back out
(`CL_IndepPadUnalias`; the window closes it), so two people who hold A
together both join. Together mode skips a second device with the same
device path (`pad: skipped duplicate listing of <key>`), nothing else.

**R19 (0.0.0.24, 2026-10-07, headless-verified only): Independent mode is
"experimental" and its pads are exclusive** (design 17.5). The Session mode
row reads **Independent (experimental)**, help "Separate windows per
player; still being made reliable". The maintainer's 3-pad failure was ours: pads
were handed to windows as "the n-th device with this GUID", an order that
differs per process (P2's window opened P1's pad), and the coordinator saw
his 8BitDo 2C twice (both listings joined; one window got a device it does
not see). Now every device has a **key = GUID + hash of its device path**
(the same in every process; `guid#n` only without a path); a window opens
exactly one device from its key list, never a second; the coordinator alone
joins, never gives a playing pad a second window, ignores a second listing
of one pad (same path, or **mirrored input**: identical presses within
100 ms, undone after 150 ms of different input) and passes its key to that
window as an alias; a replugged pad returns to its window (by key, or by
GUID for a path-less device); every decision is logged (`indep: pad <key>
-> P<n> (pid ...)`). Keyboard/mouse already reach only the focused window
(audited). **Backdrop:** a black never-active window under the tiles
(`cl_splitIndepBackdrop` 1), marked fullscreen; the coordinator logs
`SHQueryUserNotificationState` and the foreground rect -- a rect-based
check (Joyxoff, likely) cannot pass with a quarter-screen tile: fallback is
disabling Joyxoff's bindings. Test device bus `padbus` / `in_padBus` /
`in_padBusOrder` (shared `padbus.txt`), runner `scripts/tests/r19-run.sh`,
cases `r19-pads/-dedupe/-menu.cfg`. Also: a profile with `toggle` lines
keeps an R11 UrT table; UrT honours `+seta/+sets/+setu com_hunkMegs`.

**R18 (0.0.0.23, 2026-10-07, headless-verified only): Urban Terror pass
after the maintainer's real-pad UrT test** (design 20, user doc
`docs/URBAN-TERROR.md`). **Drop-in launch:** with no `fs_basegame` /
`fs_game` given and a `q3ut4` folder but no `baseq3` in the base path (the
exe's folder by default) the engine plays UrT (`urt: detected q3ut4
install, fs_basegame q3ut4`); in UrT `com_hunkMegs` is raised to 1024
unless the command line sets it. **Online kick "Non whitelist client Q3
1.32e splitscreen":** it comes from UrT servers' B3/B4 `vpncheck` plugin,
which bans any `client` connect key not on its list; UrT 4.3's own client
sends none, so in UrT ours leaves it out too (`urt: connect userinfo to
<addr>: no "client" key ...`; `version` cvar / log / BUILD-INFO keep our
version). **Scope "black square":** with HUD shape 4:3 Centered a
full-screen overlay now also covers the cell's bars (UrT scope black around
the lens in 2/4-player and side-by-side cells; `cl_splitOverlayBars 0`
turns it off). **Menus blue in UrT** (`cl_splitMenuColor auto`, red in
baseq3). **UrT pad layout = the maintainer's** (A jump, B crouch, X reload, Y
bandage, d-pad drop item / drop weapon / IR vision / weapon mode, R3 knife,
L3 sprint, LB reset zoom, RB next weapon, LT zoom, RT fire); **crouch and
sprint toggle by default in UrT** (per game, with the binds: Controls rows
Crouch / Sprint; a latched sprint ends when the left stick rests 0.3 s);
the Buttons page lists every UrT action (unbound ones `--`); an untouched
R11-layout profile loads as the new layout. Runner `scripts/tests/r18-run.sh
<urt|urtauto|urtdrop|q3> <cfg> <name> <map|-> [args]` (backs up and restores
`work/urthome`'s configs/profiles byte-identically; `R18_PRE` / `R18_POST`
shell hooks); tests `r18-zoom / -binds / -auto / -q3zoom.cfg`,
`r18-oldprof.sh`.

**R17 (0.0.0.22, 2026-10-07, headless-verified only): The maintainer's second
real-pad pass.** Player 1's **LB / RB on the stock player model page**
turn the pages at any resolution (the click used to land in P1's bottom
right corner above ~720 lines: the maintainer's a51 ui scales mouse deltas its own
way; the cursor is now walked onto the arrow from where the menu draws it,
`CL_SplitUITurnStep`). **Spectators** (Team Arena / baseq3 team games start
everyone as one, `g_teamAutoJoin 0`) get **Join red / Join blue / Auto join
(smaller team)** on their pause page (FFA: **Join game**); a player on a
team gets **Spectate**; sent as `team <x>` on that player's connection; log
`P<n> team: ...`. Server options **Power-ups** On/Off (Off drops every
power-up + `holdable_*`, restart; greys Quad damage; UrT greyed). Controls
**Cursor speed** 50-200 % (feel setting `joy_cursorSpeed`, per player /
profile) scales both pad cursors; game-menu cursor 1.4x
(`cl_padModCursorScale`, was 1.25). Aim assist **Low** 0.64 slow / 0.375
follow (Standard unchanged); `joy_aimAssist` default **0**: Guests and new
profiles start Off; host "Allow aim assist" stays On. **Menu size** default
1.00x, 0.50-1.50 by 0.25 (same scale as before; the maintainer had already set 1).
"4:3 Centered", "Center view", "95%". New runner `scripts/tests/r17-run.sh`
(= r16-run with `r17-` names); tests
`r17-model/-aimdef/-cursor/-powerups/-team/-strings/-urt.cfg`;
`splitcursor <player>` prints both cursor speeds and positions.

**R16 (0.0.0.21, 2026-10-06, headless-verified only): The maintainer's R14-R15
feedback.** Server options page: new rows **Quad damage** (On/Off,
`item_quad` removed, restart), **Player speed** (50-200 % of the game's
own `g_speed`, live) and **Weapon respawn** (Default / 1-30 s,
`g_weaponrespawn` + `g_weaponTeamRespawn`, live) after Low gravity; UrT
greys all three (UrT's movement ignores `g_speed`). **Reset to defaults**
resets every row incl. game type (FFA) and volume (100 %); the map stays.
**Instagib** uses the game's own cvar when the game VM registers one
(UrT 4.3 `g_instagib`, OSP `match_instagib`; stock baseq3 has none, so
our preset stays there), help line "Uses the game's own instagib";
instagib greys Weapons / Spawn / Ammo / **Player health** (no handicap).
**Bots 0-20**, and bots that need more slots restart the map again
(pre-existing bug: the latched `sv_maxclients` was compared). Controls
row "**Outer deadzone**" (explanation in its help line). **Independent
mode:** every tile window is marked fullscreen for the shell
(`ITaskbarList2::MarkFullscreenWindow`, `cl_splitIndepMarkFullscreen` 1;
log `window: P<n> marked fullscreen for the shell`). FOV checked, no
change (pads see `joy_fov` 90, not autoexec's 115). New runner
`scripts/tests/r16-run.sh` (= r14b-run with `r16-` names); tests
`r16-page/-bots/-fov/-osp/-ospcvars/-urt/-urtspeed/-urtfov.cfg`.

**R15 (0.0.0.20, 2026-10-06, headless-verified only): SDL2 built into the
Windows clients + exe renames (L-9).** `scripts\build.ps1` -> `build\<Cfg>\`
**`quake3e-vulkan-ss.x64.exe`** (Vulkan, 6.6 MB), `quake3e-ss.x64.exe`
(OpenGL, 6.3 MB), `quake3e-ss.ded.x64.exe` (dedicated, 0.96 MB, no SDL
imports), `BUILD-INFO.txt` -- **no `SDL2.dll`**: SDL2 2.32.10 is linked
statically into both clients (source release in `work\sdl2-src\`,
downloaded + sha256-checked + built by build.ps1 on a fresh clone; details
in **Build** below). A client exe copied alone into an empty folder runs,
finds pads and plays sound. Release = the three exes (`release.yml`'s
Windows job now runs build.ps1; unverified on GitHub). Independent mode
children start from whatever the exe is called (`GetModuleFileNameW`).
R14b review fixes: god mode's client mask is guarded for client numbers
>= 31, and cheats that god mode forced go off live once nobody has god.

**R14b (0.0.0.19, 2026-10-06, headless-verified only): Server options page**
(design 18 / 18.4, `code/client/cl_splitsrv.c` state + apply, page in
`cl_splitmenu.c`, levers in `code/server/sv_splitrules.c`). Player 1's
Start menu has **Server options** above End game (greyed "host only" on
another machine's server; in Independent mode every window's P1 page has
it; console `serveroptions` opens it, also for keyboard players): Restart
round, Change map (alphabetical, current first, levelshot beside the list),
Game type (baseq3 4 stock types; UrT its 11), Time / Frag (Capture) limit,
Instagib, Weapons (Default / Random / X only), Player weapons at spawn,
Infinite ammo, Bots 0-16 + difficulty, Friendly fire, Self-damage, God
mode (forces `sv_cheats 1` on the local server), Player health (handicap
5-95 for every local player), Low gravity, Game volume 0-150 %
(`s_volume` max now 1.5), Reset, Select / Save / Delete sets
(`<homepath>/<game>/serversettings/<key>.cfg`). Every row has a help line;
the page shows what leaving will do ("On leaving: restarts the map");
leaving applies with at most one `map`/`devmap` or `map_restart 0`. Server
debug: `srvdebug ps|ents|health`. UrT greys Instagib / Weapons / Spawn /
Ammo / Self-damage / God. Dedicated exe: inert (checked with the cvars
forced). Cvars: `cl_splitSrvInstagib Weapons SpawnWeapons InfiniteAmmo
Bots BotSkill SelfDamage God Health Set` (archived), `cl_splitSrvMap /
Gametype` (temp, pending), `sv_split*` (set by the client every frame,
never archived). Runner `scripts/tests/r14b-run.sh <cfg> <name> <map|->
[args]` (Vulkan Release, `work/q3test`, `--noactivate`, refuses to start
beside a running quake3e; log `work/r14b-<name>.log`, shots `r14b-*` ->
`work/`; pass `+set logfile 4` for long scripts). Tests: `r14b-page`,
`-ents`, `-ps`, `-god`, `-instagib`, `-menu`, `-urt` (via `r11-run.sh`),
and `r13-child2b.cfg` now also opens the child's page. Commands the page
queues (map, map_restart, kickbots, addbot) use `EXEC_INSERT`, so they run
ahead of a waiting test script.

**R14a (0.0.0.18, 2026-10-06, headless-verified only): Windows + Linux
merged** (branch `linux` L1-L5 in `splitscreen`; the Windows exes now save
to `q3config-ss.cfg` / `q3config_server-ss.cfg`, seeded once from
upstream's file, `autoexec-ss.cfg` exec'd; verified on Windows with
`scripts/tests/r14a-cfg.sh`). Fixed/added after the maintainer's first real-pad
session: **sound works with pads on** (SDL's COM init made the engine's
`CoInitialize` return S_FALSE, which upstream treated as failure), **P1 as
Guest = "Player 1"**, **aim assist retuned** (it was too weak, no gate
problem) with an `AA P<n>: <state>` log line and a yellow/green `+`,
**overlay 1.5x and resolution-proportional** (`cl_splitMenuSize`, host
row Menu size), **Change player model + Handicap** on the profile page,
**Field of view** (`joy_fov`, per player, `cg_fov` per player) and a
clearer outer-deadzone label on the Controls page, **mod-menu cursor
1.25x** (`cl_padModCursorScale`), **X/Y stop a refreshing server
browser**. **Test home changed:** `work/q3home` is the maintainer's live home
now (his `q3config.cfg` = the seed; his live config becomes
`baseq3\q3config-ss.cfg` on his next start; his profiles); every runner
defaults to the separate test home **`work/q3test`** (`Q3HOME=` overrides),
whose seed `q3config.cfg` is the old test config (`work/r10-q3config-orig.cfg`).
Mentions of `work/q3home` in the runner notes below mean `work/q3test` now.

Upstream ec-/Quake3e (`2b375bd1`, remote `upstream`) builds on this box
with VS2019 (v142, x64) via one command (version 0.0.0.12). **R13 (this
round, headless-verified only): Independent mode** (design 17.2/17.3,
`code/client/cl_splitindep.c` + `code/win32/win_splitproc.c`): with
`--independent` (or host page **Session mode: Independent**, switches in
place) the game window is borderless at its tile and every pad that holds
A picks a profile and gets **its own process and window** (own main menu,
match, server, mod), tiled with the splitscreen layouts (2 top/bottom, 3 =
2 + 1 wide, 4 = 2x2 ..., `cl_splitFill` / `cl_splitWidePlayer` /
`cl_splitVertical`) inside the monitor or `cl_splitIndepArea`; windows
re-tile on join/leave/crash, all close with player 1's; children never
steal focus, open only their pad, write only their own profile file;
Windows only. **R12: pad aim assist, local games only** (design
15/15.1, `code/client/cl_aimassist.c`): slowdown in a target's bubble +
rotational follow while the player moves/looks, per player Off / Low
(default) / Standard, host switch, `+` name marker and cell glyph; exactly
zero on any other machine's server. **Splitscreen
works for 2-8 players on a local listen server (2-4, or as many as the
server's per-IP cap allows, on a remote server), on Vulkan and OpenGL,
in baseq3 and in Urban Terror 4.3 (the maintainer's install, unmodified),
driven by gamepads or console commands, and a lone player can go from
launch to a match and back to quit with only a pad.** **R11: Urban Terror (M3).** Stock-path Quake3e boots UrT
with `fs_basegame q3ut4` alone; each extra player's UrT team and gear menus
open in its own cell (UrT's cgame runs `ui_selectteam`: unclaimed console
commands of players 2-8 now go to their own ui VM), each picks team and
gear independently (gear/colours/weapmodes are per-player userinfo; UrT's
gear menu's `ui_gear*` are per player via `playercvar` data); built-in
q3ut4 pad layout; profiles carry feel across mods and start UrT with UrT's
buttons; up to 7 UrT players in UrT's default 800 MB hunk, 8 with
`com_hunkMegs 1024`. Online: pads join remote servers too; a remote refusal
reads "Player N could not join: this server allows only K players from one
connection" for 10 s. R10: layouts 5-8, renderer room for 8 views, no
cgame restarts between 2 and 8 players, our listen server exempts
127.x.x.x from `sv_maxclientsPerIP`, `cl_splitMaxPlayers` default 8.
M1: `addplayer` / `dropplayer`, `p<N> <cmd>`, layouts, per-player
userinfo, sound, error isolation. M2: SDL2 gamepads, per-player binds,
analog move/look, join, auto-heal, join hint. S6: one mod menu (ui VM) per
player in its cell. R7: engine pause overlay (`cl_splitmenu.c`). R8:
profiles, Guest, Guest defaults, join-time picker, on-screen keyboard.
R9: robustness. Joins, leaves,
picker cancels, layout changes and mod-menu restarts no longer grow the
hunk (QVM hunk blocks are reused, `VM_HunkAlloc`); a held join slot takes
no cell until the player connects (its picker is drawn where the cell will
be; B cancels without restarting anybody). Extra players join **remote
servers**: one at a time, a refusal ("Too many connections.") drops only
that player and shows the server's words in its cell; kicks/timeouts/leaves
show their reason there too. Pad keys reach a cgame that catches keys
(KEYCATCH_CGAME), BUTTON_TALK is set while a player's overlay is open, a
name changed in the mod's menu renames the profile, player 1's q3config
identity survives a crash. With one player and no pads (or `cl_splitP1Input
kbm`) the engine behaves as upstream and no profile file is created. No
GitHub repo of our own (local).

**Build:** `powershell -ExecutionPolicy Bypass -File scripts\build.ps1`
(`-Configuration Debug` optional). Outputs in `build\<Cfg>\`:
**`quake3e-vulkan-ss.x64.exe`** (Vulkan, the one to launch),
`quake3e-ss.x64.exe` (OpenGL), `quake3e-ss.ded.x64.exe` (dedicated, no
client/pad/menu/profile code, no SDL), `BUILD-INFO.txt` (version, commit,
"SDL2 2.32.10 static"). Names = upstream's + `-ss`, same as Linux (R15;
`quake3e-splitscreen*.exe` / `quake3e.ded.exe` before; build.ps1 deletes
those and any old `SDL2.dll` from `build\<Cfg>\`). **No SDL2.dll (R15):**
both clients link SDL2 2.32.10 statically: build.ps1 downloads the SDL2
source release once into `work\sdl2-src\` (sha256-pinned; see
`work\sdl2-src-PROVENANCE.md`), builds SDL's own `VisualC\SDL\SDL.vcxproj`
as `work\sdl2-src\lib\SDL2-static.lib` (9.0 MB, /MT; Debug builds:
`SDL2-static-debug.lib`, 31 MB, /MTd; settings in
`scripts\sdl2-static.props`, rebuilt when that file is newer) and passes
it to `quake3e.vcxproj` only (`scripts\splitscreen.props`: define
`Q3E_SDL_STATIC` + the lib + setupapi/imm32/version/winmm/ole32/oleaut32/
advapi32/shell32/user32/gdi32). `in_gamepad.c` binds SDL directly with
`Q3E_SDL_STATIC` and keeps the run-time loading path for every other
build (Linux, MinGW, CMake). Log line: `gamepad: SDL 2.32.10 static (built
in), ...`; pads are disabled if SDL init fails. `-PlatformToolset v143` is
for CI. A release is the three exes only. Prereqs: VS2019 C++ workload +
Windows SDK + network on the first build (or the SDL zip in
`work\sdl2-src\dl\`). The vcxproj files are untouched. New
client files go into `Makefile` and `code/win32/msvc2017/quake3e.vcxproj`
(+ `.filters`); CMake globs `code/client` (client-only win32 files go in
its `Q3_UI_SRCS`, not `Q3_SRCS`, which the dedicated exe shares). The
vcxproj is stored with CRLF and git has `core.autocrlf true`: edit it
binary-safe and stage with `git hash-object -w --no-filters` +
`git update-index --cacheinfo` or the whole file shows as changed (R13).

**Linux build (L1, 2026-10-06, Linux test box, branch `linux`; layout since
L4, version 0.0.0.27 since L6):** 0.0.0.27 (Windows rounds R14a-R21)
builds on Linux with 0 warnings and passes the Linux regression pass (L6,
Last round). `scripts/build.sh [debug]` from the host builds
inside the `q3dev` distrobox (Fedora 43; create line in
`docs/LINUX-BOOTSTRAP.md`; without distrobox, e.g. on a CI runner, it runs
make directly) in **upstream's release layout** (`USE_RENDERER_DLOPEN=0`,
renderer linked in, one client per renderer, objects in
`build/obj/vk/` and `build/obj/gl/` so neither link rebuilds the other) ->
`build/release-linux-x86_64/`: `quake3e-vulkan-ss.x64` (Vulkan client,
the one to launch), `quake3e-ss.x64` (OpenGL client), `quake3e-ss.ded.x64`
(dedicated server), all stripped for release, + `BUILD-INFO.txt` (version +
commit stamped; our record, not shipped). Names = upstream's
(`quake3e-vulkan.x64` / `quake3e.x64` / `quake3e.ded.x64`) + `-ss`. No
renderer `.so`, no `cl_renderer`: pick the executable. Runs on the host against its own SDL2 2.32,
RADV Vulkan and Mesa GL. Pads go through the engine's SDL2
(`IN_GamepadFrame` from `sdl_input.c`'s `IN_Frame`). **Independent mode
works on the Linux desktop (L3)**: `code/unix/unix_splitproc.c` (fork/exec
of `/proc/self/exe`, `PR_SET_PDEATHSIG`, loopback UDP, SDL window queries);
in this mode (coordinator and children, `--independent`/`--child`/
`--noactivate`, or `cl_splitIndependent 1`) SDL runs on its **x11 driver
(XWayland)** because Wayland cannot place windows; Together mode stays on
native wayland. Tiles use the display's usable area (1280x670 here: the
Plasma panel is excluded). Children are created hidden with
`_NET_WM_USER_TIME 0` and never take the focus. Under Gamescope (Deck game
mode) or without a DISPLAY the Session mode row reads "Unavailable under
Gamescope (Deck game mode)" and `--independent` falls back to Together.
Package (L4, the maintainer's decision): `scripts/package.sh` ->
`build/quake3e-splitscreen-<ver>-linux-x86_64.tar.gz` holding **only the
three executables at the archive root**, like upstream's Linux zip (refuses
when `BUILD-INFO.txt` is not a release build of `VERSION`). Users unpack
next to `baseq3/` and start `quake3e-vulkan-ss.x64`. **No launcher and no
install script -- do not reintroduce one:** the engine's default
`fs_basepath` is the executable's own folder (`Sys_DefaultBasePath` ->
`Sys_Pwd()` = dirname of `/proc/self/exe`, code/unix/unix_main.c, same as
upstream; L4 started it from `~` and it loaded the paks beside it), and
Independent mode sets `SDL_VIDEODRIVER=x11` itself
(code/unix/unix_splitproc.c `Split_UseX11`). Releases:
`.github/workflows/release.yml` (tag `v<VERSION>` -> Linux tar.gz + Windows
x64 zip (three exes, SDL2 built in; since R15 the Windows job runs
`scriptsuild.ps1 -PlatformToolset v143`) on the GitHub release; upstream's `build.yml`
removed); **not run yet** (no GitHub repo), the Linux job was replayed in
an `ubuntu:24.04` container. **Boots q3dm1 on both renderers** (L4 build: `work/l4-vk-single*`,
`work/l4-gl-single*`; L1: Vulkan: RADV STRIX1, `work/l1-vk-single*`;
OpenGL: radeonsi Mesa 26.0.1 GL 4.6, `work/l1-gl-single*`; SDL video driver
"wayland", audio "pipewire"), no `profiles/` created with one kbm player.
The box's built-in controller (Handheld Daemon -> "Xbox 360 Controller") is
detected through the engine's SDL and survives `vid_restart`
(`work/l2-padlist.log`). Game data: `~/dev/q3data/baseq3/pak0-8.pk3`
(`work/q3data-PROVENANCE.md`). Runner: `scripts/tests/linux-run.sh <vk|gl>
<cfg> <name> <map|-> [+set ...]` (vk = `quake3e-vulkan-ss.x64`, gl =
`quake3e-ss.x64`; log `work/<name>.log`, every shot of the run ->
`work/<name>-<x>.jpg`;
refuses to start beside a running client -- ours or upstream's
`quake3e.x64` / `quake3e-vulkan.x64`, checked by `/proc/<pid>/exe` because
process names are cut to 15 characters (`quake3e-vulkan-`), so `pgrep -x
quake3e-vulkan-ss.x64` never matches; exports the desktop display when run
from SSH). `l3-run.sh` takes the same vk|gl; `l6-r19-run.sh` = `l3-run.sh`
with the R19 test device bus (Linux `r19-run.sh`). Hand run line:
`build/release-linux-x86_64/quake3e-vulkan-ss.x64 +set fs_basepath
~/dev/q3data +set fs_homepath <repo>/work/q3home +set r_fullscreen 0 +set
r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set logfile 3
+set in_gamepad 1 +set in_padDebug 1` (OpenGL: `quake3e-ss.x64`).

**Configs side by side (L5, 0.0.0.17; the maintainer's decision):** users keep
vanilla Quake3e and our `-ss` executables in the same game folder. Shared
exactly as upstream: base path, home path (Windows: the exe's folder;
Linux: `~/.q3a`), paks, downloads, `autoexec.cfg`, `profiles/`. Only the
archived settings file is ours: clients write **`q3config-ss.cfg`**, the
dedicated server **`q3config_server-ss.cfg`** (`Q3CONFIG_CFG` in
`code/qcommon/qcommon.h`; `Q3CONFIG_CFG_UPSTREAM` = upstream's name). First
run in a game dir (baseq3 or a mod) without `<homepath>/<game>/q3config-ss.cfg`
copies that dir's `q3config.cfg` (home path first, then base path) as-is and
prints `config: q3config-ss.cfg created from q3config.cfg`
(`Com_SeedSplitConfig`, common.c; called before every exec of the config:
startup, `game_restart`, files.c fallback; never in an Independent-mode
child or safe mode, never overwrites). `autoexec-ss.cfg` runs after
`autoexec.cfg` when it exists (silent when absent). Upstream never reads
our files. Test runners create a stub `q3config-ss.cfg` only when the home
dir has neither file (an existing `q3config.cfg` seeds it).

**Run (test):** `build\Release\quake3e-vulkan-ss.x64.exe +set fs_basepath
"<Quake3 install>" +set fs_homepath
<repo>\work\q3home +set r_fullscreen 0 +set r_mode -1 +set r_customwidth
1280 +set r_customheight 720 +set logfile 2 +devmap q3dm1` (log at
`work\q3home\baseq3\qconsole.log`, screenshots in `...\screenshots\`).
Runner (Git Bash): `scripts/tests/s6-run.sh <exe> <cfg> <logname> <map|->
[args]` copies `scripts/tests/*.cfg` into the home dir, runs `+devmap <map>
+wait 300 +exec <cfg>` (map `-` = main menu), copies the log to
`work/<logname>.log`; always add `+set in_gamepad 0 +set developer 1`
**and now `+set logfile 3`** (see lessons). **R8 scripts** (each header has
its exact command; clean `work/q3home/profiles` and
`work/q3home/baseq3/profiles` first): `r8-keys.cfg` (helpers: `vstr k<pad><btn>`
taps a virtual pad button, `vstr join<pad>` holds A 1 s at 60 fps),
`r8-join.cfg`, `r8-persist-a.cfg` then `r8-persist-b.cfg`, `r8-switch.cfg`,
`r8-life.cfg`, `r8-robust-setup.sh` + `r8-robust.cfg`,
`r8-migrate-setup.sh [keep]` + `r8-migrate.cfg`, `r8-single.cfg`.
**R9 scripts:** `r9-hunk.cfg` (meminfo across 30 join/leave, 10 4th-player,
30 picker cancels, 30 layout toggles, 10 ui restarts), `r9-remote.sh <exe>
<logname> [lan ip]` (starts/restarts/kills the dedicated server itself,
runs `r9-remote.cfg`), `r9-polish-setup.sh` + `r9-polish.cfg`,
`r9-p1crash.cfg` (killed by the caller) then `r9-p1crash-b.cfg`,
`r9-bloom.cfg`, `r9-perf.cfg` + `r9-perf-avg.sh <log>`, `r9-single.cfg`,
`r9-gamedir.cfg` / `r9-gr0.cfg` (`game_restart` crash repro).
**R10 scripts** (runner `scripts/tests/r10-run.sh <vk|gl> <cfg> <name> <map>
[args]`: Release exe or `EXE_DIR=...`, adds `in_gamepad 0 developer 1
logfile 3 com_hunkMegs 128 cl_splitMaxPlayers 8 sv_maxclients 16`, log ->
`work/r10-<r>-<name>.log`, screenshots `r10-x` -> `work/r10-<r>-x.jpg`):
`r10-layouts.cfg` (q3dm7), `r10-pads.cfg` (q3dm7; 8 pads via
`r10-keys.cfg`), `r10-caps.cfg` + `r10-caps-max.sh` (q3dm17), `r10-sound.cfg`
+ `r10-sound-count.sh`, `r10-perf.cfg` + `r10-perf-avg.sh` (pass `+set
com_maxfps 0 +set r_swapInterval 0`; r9-perf-avg averaged stall frames in),
`r10-hunk.cfg` + `r10-hunk-sum.sh` (q3dm12), `r10-perip.cfg`, `r10-single.cfg`
(s6-run). **R11 (Urban Terror):** run line `build\Release\quake3e-vulkan-ss.x64.exe
+set fs_basepath "<UrbanTerror43 install>" +set
fs_homepath <repo>\work\urthome +set fs_basegame q3ut4 +set r_fullscreen 0
+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set
logfile 3 +set com_hunkMegs 1024 +devmap ut4_casa` (fs_basegame is
CVAR_INIT: command line only; no `com_standalone`/`fs_game` needed; bots:
`+set bot_enable 1` on the command line, it is latched, then `addbot boa 3
blue`; maps: `dir maps bsp`, e.g. ut4_casa, ut4_turnpike, ut4_abbey). Runner
`scripts/tests/r11-run.sh <vk|gl> <cfg> <name> <map|-> [args]` (UrT folder
read-only, homepath `work/urthome`, log `work/r11-<r>-<name>.log`, shots
`r11-x` -> `work/r11-<r>-x.jpg`): `r11-boot.cfg` (`-`, `+set bot_enable 1
+set cl_splitP1Input kbm`), `r11-2p.cfg` (ut4_casa, `+set cl_splitP1Input
kbm`), `r11-4p.cfg` (ut4_casa, `+set in_padDebug 1 +set cl_splitP1Input
pad`; parts b/c/d chained), `r11-hunk.cfg` (ut4_casa; `+set com_hunkMegs
1024` for 8), `r11-radio.cfg` / `r11-radio2.cfg` (radio menu probes);
`scripts/tests/r11-profiles.sh` (baseq3 then UrT, shared homepath
`work/r11prof`); `scripts/tests/r11-remote.sh <exe> <logname>` (baseq3
dedicated server on the LAN IP, per-IP cap 3, runs `r11-remote.cfg`).
**R12 (aim assist):** runner `scripts/tests/r12-run.sh <q3|urt> <cfg> <name>
<map|-> [args]` (Vulkan; `R12_GL=1` for GL; q3 = Quake 3 folder + homepath
`work/q3home` + hunk 128, urt = UrT folder + `work/urthome` + hunk 1024; log
`work/r12-<g>-<name>.log`, shots `r12-x` -> `work/r12-<g>-x.jpg`):
`r12-aim.cfg` (exact command lines in its header: baseq3 on q3dm7 with
`+set r12home "setviewpos 1850 -1475 -128 80"`; UrT on ut4_casa with
`g_gametype 3`, `r12team1/2`, `r12home "setviewpos -448 368 -16 120"`,
`r12mate`), summarised by `scripts/tests/r12-table.sh <log>`;
`r12-remote.sh <exe> <logname>` + `r12-remote.cfg`; `r12-perf.cfg` (+
`r10-perf-avg.sh`); `r12-urt-ents.cfg`.
**R13 (Independent mode):** runner `scripts/tests/r13-run.sh <cfg> <name>
<map|-> [args]` (Vulkan Release; `R13_GL=1` GL; starts `--independent
--noactivate` with `cl_splitIndepArea "64 64 1280 720"` and a forced
1280x720 window at 64,64; `R13_FLAGS` replaces the flags, e.g.
`--noactivate` for a Together start; children get `+set developer 1 +exec
r13-child@@.cfg` (`@@` = player number) unless `R13_CHILDARGS`; logs
`work/r13-<name>.log` + `-child<N>.log`, shots `r13-x` ->
`work/r13-<name>-x.jpg`, q3config watcher `-q3config.txt`, leftover check
`-tasklist.txt` (kills leftovers)): `r13-two.sh [name]` (r13-two.cfg on
q3dm7 + r13-child2/2b.cfg; fresh "Ada" profile), `r13-tiles.sh`
(r13-tiles.cfg + r13-tchild2-4.cfg; taskkills P3 when the log says so),
`r13-ckill.sh` (kills the coordinator), `r13-mode.cfg` (command in its
header). `--child` windows also run standalone for menu experiments:
`quake3e-vulkan-ss.x64.exe --child --noactivate +set cl_splitChild 2 +set
cl_splitChildPad virtual-pad-1 +set cl_splitWindowRect "64 424 1280 360"
...`. Older runners (`s6-run.sh`, `r12-run.sh`, `r11-run.sh`) take
`PRE_ARGS=--noactivate` (launch flags must come before the first `+`).
**Never use "`vid_restart fast`" after resizing the window** (Vulkan GPU
fault + driver reset on this box, R13); check `Get-WinEvent` System log,
provider `nvlddmkm`, after renderer experiments. Test commands (developer or
sv_cheats): `aimassist_testplace <player 2-8> <dist> <view offset> [wall]`
(player 1 turns to face open floor, the other player is `setviewpos`ed
there, or behind a wall), `aimassist_ents` (player entities of the active
player's snapshot: eType/eFlags/team), `aimassist_stats` (microseconds per
assist call while `cl_aimAssistDebug` is non-zero), `cl_aimAssistDebug 1`
(one "AA P<n>" line per frame with input or a target: target, distance,
angle, bubble, fade, slowdown, angular velocity, rotation, stick vs output
yaw rate), 2 (every frame), 3 (+ candidates), -1 (timing only).
**Commands a cgame queues while a script waits run after the script's
remaining text** (player 1's; extras' now have their own queue): end a
script part after the joins with `splitappend exec <next.cfg>`. `splitdebug
cursor <player> <x> <y>` (cheats) puts a player's menu cursor at x,y (640x480
menu units). **The test q3config had `com_hunkMegs 512`**: earlier rounds'
runs were not at the default; r10-run passes 128. `r_speeds 1` now also
prints `frame: N scenes, ents a/b, dlights ...` (shared per-frame arrays).
Script rules learned: put `wait 5` after every `screenshotJPEG`; a menu's
EXEC_APPEND commands run only after the whole script (`splitappend exec
<next.cfg>`, developer only); **set `com_maxfps 60` in every script that
holds buttons by frame count** (q3config has 125: a 60-frame hold is then
too short to join); **use `+set logfile 3`** -- on this box a config write now happens
at shutdown (cause not pinned down; the pre-M2 baseline exe does it too),
and with `logfile 2` + `developer 1` it closes and re-opens (truncates) the
log to 2 lines (upstream: `FS_SV_FOpenFileWrite`'s debug print reuses the
log's file handle); the runner deletes the log
before each run, so append mode is safe (`logfile 3` is append *buffered*;
a script that watches the log while the game runs needs `logfile 4`). An
error / server disconnect clears the command buffer: continue a script
with `set cl_splitTestOnDisconnect "vstr <next>"` (developer). `+set` of an
archived cvar (r_fbo, r_bloom, com_maxfps ...) ends up in q3config: remove
it after such runs. Overlay rows are navigated by
index: a page whose rows change (Profile page after switching) keeps the
old index. The stock Q3 player-settings name field has no keyboard focus
on entry: click it with the right-stick cursor + RT (r8-life has the
stick timings) before typing. The m2/s6/r7 scripts predate profiles (the
pause page gained a Profile row; joins now stop at the picker) --
superseded by r8-*.

**Console reference:** `padlist`, `pbind <player> <padkey> [cmd]`, `punbind
<player> <padkey|all|defaults>` (defaults = the Guest defaults), `pbindlist
[player]`, `padinject <pad> connect [xbox|playstation|nintendo] [guid] |
disconnect | button <padkey> <0|1> | axis <lx|ly|rx|ry|lt|rt> <v>`
(sv_cheats or developer), `padmenu`, `splitsettings`, `splitplayers` (now
shows `joining` and the profile), `splitdebug freeze|error|packeterror|uierror|cgamecatch <n>`, `splitappend <text>`.
**Profiles:** `profile_list`, `profile_load <player> <name|guest>` (live
switch), `profile_save <player> [new name]` (no name: save now; with a
name: save as a new profile and use it), `profile_rename <player> <name>`,
`profile_delete <name>`, `addplayer [n] [profile|guest]` (console join,
no picker). Player 1 with `cl_splitP1Input kbm` has no profile (commands
refuse). Pad keys: `PAD_A B X Y BACK GUIDE START L3 R3 LB RB
DPAD_UP/DOWN/LEFT/RIGHT MISC1 PADDLE1-4 TOUCHPAD LT RT LSTICK_* RSTICK_*`.
Host cvars (archived): `cl_splitP1Input pad|kbm`, `cl_splitJoinButton
PAD_A`, `cl_splitJoinHold 700`, `cl_splitJoinHint 1`, `cl_splitMaxPlayers
8`, `cl_splitFill`, `cl_splitWidePlayer`, `cl_splitVertical`,
`cl_splitAspect`, `cl_splitLeaveHold 2000`, `cl_padCursorSpeed 500`,
`cl_splitMenuColor "1 0 0"`. Per-player feel lives in `p<N>_joy_*` (P1
too), the Guest defaults in `guest_joy_*` (created from the shared
`joy_*` cvars, which remain the fallback when `_guest.cfg` is absent):
`joy_yawSpeed 320`, `joy_pitchSpeed 200`, `joy_invertPitch 0`,
`joy_aimCurve standard`, `joy_aimExponent 2`, `joy_turnBoost 1.5`,
`joy_turnBoostTime 250`, `joy_zoomScale 0.5`, `joy_aimSmooth 0`,
`joy_deadzone 0.15`, `joy_deadzoneOuter 0.95`, `joy_walkThreshold 0.5`,
`joy_crouchToggle 0`, `joy_alwaysRun 1`, `joy_aimAssist 1` (R12: 0 Off,
1 Low, 2 Standard; a feel key in profiles). **Aim assist host cvars (R12,
archived, all tunable; defaults):** `cl_aimAssistAllow 1` (0 = nobody this
session), `cl_aimAssistMarker auto` (baseq3 ` ^3+`, UrT `+`; `0` = none; or
the text), `joy_aimRange 1500` (units), `joy_aimBubble 40` (bubble radius in
units at the target), `joy_aimBubbleMax 8` (deg, close targets),
`joy_aimSlowLow 0.8` / `joy_aimSlowStandard 0.6` (look speed at the bubble
centre), `joy_aimRotLow 0.18` / `joy_aimRotStandard 0.35` (share of the
target's angular velocity, max 0.35), `joy_aimRotCap 60` (deg/s),
`joy_aimFade 100` (ms), `joy_aimFlick 300` (deg/s look rate = flick),
`joy_aimFlickTime 150` (ms both components pause after a flick).
**Independent mode (R13):** `cl_splitIndependent 0|1` (archived host
setting; `--independent` = this run only), `cl_splitIndepArea "x y w h"`
(temp; empty = whole monitor), `indepstatus` (windows, pids, ports, pads,
profiles, rects), `indepfocus <player>` (untested: takes the foreground),
`addplayer [n] [profile]` in Independent mode = a pad-less window. Child
side (command line only, set by the coordinator): `cl_splitChild <player>`,
`cl_splitChildIpc`, `cl_splitChildPad "<key> [<alias>...]"` (R19 device keys, design 17.5),
`cl_splitChildProfile`, `cl_splitWindowRect`; `cl_splitChildArgs` (temp,
developer: extra child command line). Every window logs `window: P<n> rect
x,y wxh (GetWindowRect)` when its rect changes; the coordinator logs
`indep: tiles (...)` per re-tile. Device-level:
`joy_triggerThreshold 0.3`, `joy_stickThreshold 0.5`. `in_padDebug 1`
prints pad buttons and bind commands; 2 adds look rates and moves.
**R19 test device bus:** `padbus clear | list | <id> connect <guid> [path|-]
[coord] [mirror <id>] | <id> disconnect | <id> button <padkey> <0|1> | <id>
axis <ax> <v>` (developer) writes `<fs_homepath>/padbus.txt`; `in_padBus 1`
makes a process open those devices like SDL ones, `in_padBusOrder 1`
reverses its order. Runner `scripts/tests/r19-run.sh <cfg> <name> <map|->`
(= r13-run.sh with the bus on everywhere, children reversed + `in_padDebug
1`, child script `r19-child.cfg`; `R13_PREFIX` default r19). `padlist`
shows each pad's key and "duplicate of pad N"; `indepstatus` the keys.
`cl_splitIndepBackdrop 0|1` (archived, default 1).

**Profile files** (homepath only, plain `keyword "value"` lines, unknown or
invalid lines skipped with one warning per file, missing keys = Guest
defaults; the first save rewrites a damaged file clean):
- `profiles/<key>.cfg` (all mods): `name "Ada"` + one line per feel
  setting (`joy_yawSpeed "380"` ...), each range-checked on load.
- `<game>/profiles/<key>.cfg`: `bind PAD_X "+attack"` lines (the whole
  table; none = the Guest defaults' binds, e.g. first use in a new mod) +
  `userinfo model "sarge/blue"` lines (userinfo values that differ from the
  cvar default; name, cl_guid and connection keys excluded).
- `profiles/_guest.cfg` (feel) + `<game>/profiles/_guest.cfg` (binds): the
  Guest defaults; absent = shared `joy_*` / `default_pad.cfg` / built-in.
- `profiles/_padlast.cfg`: `pad "<guid>" "<key>"` -- the last named
  profile per pad (a Guest visit does not erase it).
- `<key>` = sanitised name (letters, digits, space, `_`, `-`, max 20, no
  leading `_`/`-`, colour codes dropped) lower-cased with `_` for spaces,
  so names differing only by case are the same profile; "guest" is
  reserved. Hand-made files whose name is not a key are ignored (warned).
- Migration: at start, if `<game>/splitpads.cfg` (R7) exists and there are
  no profiles and no `_guest.cfg`, player 1's `pset 1`/`pbind 1` entries
  become the Guest defaults and the file is deleted; otherwise it is left
  alone with a one-line note.
- Player 1 on a named profile writes the real `name`/`model`... cvars
  (they are archived); its q3config values are snapshotted first and
  restored (and q3config rewritten) when it goes back to Guest, goes
  keyboard/mouse, or the game quits (verified R9); while P1 plays a profile its own values are
  also in `profiles/_p1q3config.cfg`, restored at the next start after a
  crash. R14a: P1 as Guest is "Player 1" with default player settings and its
  q3config identity (and `cg_fov`) is kept/restored the same way.
- R14a feel key `joy_fov` (80..130, default 90): the player's `cg_fov`
  (P1: the real cvar; others: `p<N>_cg_fov`, which their cgames read).
  Host cvars: `cl_splitMenuSize 1.5` (1..2.5), `cl_padModCursorScale 1.25`.
  Aim assist defaults now: `joy_aimBubble 72`, `joy_aimBubbleMax 12`,
  `joy_aimSlowLow 0.7`, `joy_aimSlowStandard 0.45`, `joy_aimRotLow 0.3`,
  `joy_aimRotStandard 0.6`, `joy_aimRotCap 120` (full slowdown over the
  inner 40 % of the bubble); the Aim assist host-cvar list above is R12's.

## Last round

**R21 (Windows, 2026-10-07; Opus executor; VERSION 0.0.0.27;
commits `c0764a42`..`2d30ac9a` + this docs commit).** The maintainer's join to
74.91.113.242 with 0.0.0.26 still ended in `couldn't load
maps/ut4_asylum_b1.bsp` and his log stopped at the connect.
**Server query** (one `getstatus`, read-only, 2026-10-07): `sv_allowdownload
0`, `sv_dlURL 74.91.113.242/maps` (no scheme), `version ioQ3 1.35 urt 4.3.4`,
`g_modversion 4.3.4`, `gamename q3urt43`, map ut4_subway at the time (fs_game
and sv_pure are not in serverinfo). One HEAD request:
`http://74.91.113.242/maps/q3ut4/ut4_asylum_b1.pk3` -> 200, 32.5 MB.
**Root cause (two bugs, one trigger).** The UrT engine's default `fs_game` is
`q3ut4`, so UrT servers send systeminfo `fs_game q3ut4`; our client runs UrT
as `fs_basegame q3ut4` with `fs_game ""`, so `CL_ParseGamestate` saw a game
change and ran `Com_GameRestart` at every join: (a) `Cvar_Restart` +
`default.cfg` again set `cl_allowdownload 0` (R20's switch had already set
`cl_urtDownloadDefault 1`, so it never came back) -> "missing files"
warning, no download, CM_LoadMap error; the maintainer's `q3config-ss.cfg` now holds
`cl_allowdownload 0` + `cl_urtDownloadDefault 1`; (b) `FS_Shutdown( qtrue )`
closed every handle including the log's, common.c kept the dead handle, and
nothing after the restart was logged (exactly where his log ends:
`RE_Shutdown( 2 )`, `Hunk_Clear`). Reproduced: `work/r21-before.log`
(0.0.0.26 exe, his config: downloads off, log ends at Hunk_Clear),
`work/r21-logfix.log` (log fix only, first-start state: `urt: cl_allowDownload
0 -> 1` at start, after the restart `"cl_allowdownload" is:"0"`, `WARNING: You
are missing ... q3ut4/ut4_r21dl.pk3`, `ERROR: CM_LoadMap: couldn't load
maps/ut4_r21dl.bsp` = the maintainer's failure). UrT 4.3 client source read
(FrozenSand ioq3-for-UrbanTerror-4 @dae99817, shallow clone in the session
scratchpad, deleted): `cl_autoDownload`, `CL_FirstDownload` (map pak only),
HTTP-only `CL_NextDownload`, `cl_curl.c` saves `q3ut4/download/<name>`,
files.c adds `<path>/q3ut4/download` with qvm/menu blocked.
**Fix:** design 21 (R21 note) / URBAN-TERROR.md. **Test rig**
`scripts/tests/r21-dl.sh` (our dedicated server as a UrT server: `fs_game
q3ut4`, map `ut4_r21dl` = a stock bsp renamed + 8 MB padding, `sv_allowDownload
0`, scheme-less `sv_dlURL 127.0.0.1:27990/maps`; `r21-http.py` throttled to 2
MB/s; client in a fresh `work/urttest` seeded from a copy of the maintainer's
`q3config-ss.cfg`). **Evidence:** `work/r21-http.log` (`fs_game: server's
"q3ut4" is our base game, no game restart`; `download: server
sv_allowDownload=0 sv_dlURL=127.0.0.1:27990/maps; missing:
q3ut4/ut4_r21dl.pk3; method: http (server sv_dlURL, as UrT 4.3's client)`;
`-> q3ut4/download/ut4_r21dl.pk3`; FS restart `43 pk3 files` (23 + the maintainer's
19 UrT downloads in the basepath's q3ut4/download + ours); reconnect `missing:
none`; `which maps/ut4_r21dl.bsp` -> `...\urttest\q3ut4\download\ut4_r21dl.pk3`),
`work/r21-http-during.jpg` (UrT loading screen: 3.50 MB/Sec, 3.50 of 8.21 MB,
`q3ut4/download/ut4_r21dl.pk3 (42%)`), `work/r21-http-joined.jpg` (in the map,
UrT team menu), `work/r21-http-http.log` (one GET, 8610013 bytes),
`work/r21-http-server.log` (connect, disconnect for the download, connect,
`ClientBegin`). No-download server: `work/r21-none.log` (`method: none (server
has no sv_dlURL and its sv_allowDownload is off)`, `ERROR: Can not download
q3ut4/ut4_r21dl.pk3: ... Get the map from an Urban Terror map site and put it
in q3ut4/download/.`). `work/r21-udp.log`: started with `sv_allowDownload 1`
and no sv_dlURL, the client still sees 0 -- UrT's qagame sets
`sv_allowDownload 0` at InitGame (checked on the dedicated server:
`"sv_allowDownload" is:"0"` after the map start), so UDP downloads never
happen on a UrT server; the UDP branch is untested. `cl_autodownload 0`:
`work/r21-off.log` (`downloads off on this client (cl_allowDownload 0,
cl_autodownload 0)`, then the CM_LoadMap error, as UrT). Log reopen:
`work/r21-logreopen.log` (`game_restart q3ut4`: `logfile: closing for the
game restart ...`, `logfile reopened (append) ... after a filesystem
restart`, the script's later lines present). The maintainer's log copied to
`work/r21-maintainer-qconsole.log`.
**Regressions:** r18-binds (UrT) in a copy home (`R18_URTHOME=work/urttest`,
new runner option; his `work/urthome` untouched) `work/r18-r21-reg-binds.log`:
bind/TEST/profile/toggle lines identical to `work/r20-reg-binds.log` apart
from the removed `urt: cl_allowDownload 0 -> 1` line, the home path and one
timing number (the runner rewrote `work/r18-binds-*.jpg`); r10-single
`work/r21-reg-single.log` + `-devmap.jpg` (no profiles dir); r8-join
`work/r21-reg-r8join.log` (TEST/P/profile/pad lines identical to
`work/r20-reg-r8join.log`, `in_gamepad 0`). Release build: 0 C warnings.
**Not verified:** a live UrT server (the maintainer's item 106), a UDP download, a
download with extra local players already joined, a pure (`sv_pure 1`) UrT
server, a same-name map with another checksum already in q3ut4/download.

**R20 (Windows, 2026-10-07; Opus executor; VERSION 0.0.0.26;
commits `18f403d0`..`bf2de6c6` + this docs commit).** The maintainer's third
real-pad pass on 0.0.0.25 (design 21).
**(1) Backdrop removed** (`win_splitproc.c` section, `Indep_Backdrop`,
`cl_splitIndepBackdrop`, `Sys_SplitBackdrop` / `Sys_SplitShellReport`);
`MarkFullscreenWindow` kept (`window: P1 marked fullscreen for the shell`
still in every tile log). No backdrop lines in any R20 log.
**(2) Tile aspect. Root cause:** a tile window has one player, so the
viewport trap layer was pass-through: the cgame saw the tile's own size
(the maintainer's full-width, half-height tiles: baseq3 derives fov_y from `cg_fov` 90 across
3.6:1 = ~31 degrees, and stretches its 640x480 HUD over the tile), while a
Together cell's cgame is told 640x480 (`cl_splitAspect` 1) with fov_y kept
and fov_x widened. Fix: `CL_IndepTiled` + `CL_SplitCells` (design 21).
**Evidence** (1280x720 area, two 1280x360 tiles, same spot `setviewpos
1850 -1475 -128 80` on q3dm7, `cg_fov 90`): before (0.0.0.25 exe rebuilt
from `9c784102`, deleted after): `work/r20-before-p1.jpg` /
`-p2.jpg` (HUD numbers twice as wide, narrow vertical view); after:
`work/r20-tile-p2.jpg` (the child tile) is pixel-for-pixel the Together top
cell `work/r20-together-tog.jpg` (arch edge x~450, stairs, weapon at
x~1200, HUD `100`/`118` at x 405-610, fps at 818) -- `work/r20-tile-p1.jpg`
(P1's tile) has the same HUD geometry, its view a few units off (P1 had not
settled after `setviewpos`); logs `work/r20-tile.log` / `-child2.log`
(`window: tile 1280x360 is a cell: cgame screen 640x480 (HUD shape 4:3
centered)`), `work/r20-together.log`. Menus in a tile were already 4:3
(`work/r20reg-two-c2-menu.jpg`).
**(3) First-launch hint:** `work/r20-cin-hint.jpg` (pads connected,
`cl_splitSeenHint 0`: bottom right "Start a game first, then friends hold A
to join"), `-nopad.jpg` (no pad: none), `-after.jpg` (after `map q3dm7` +
disconnect: none); `work/r20-cin.log` `pad: first game started,
first-launch hint done (cl_splitSeenHint 1)`, `seta cl_splitSeenHint "1"`
in `work/q3test/baseq3/q3config-ss.cfg`. Also shown at UrT's first-start
nickname dialog (`work/r20-urt-skipped.jpg`).
**(4) Single Player arena. Root cause:** `SV_GetChallenge` returns without
answering while `g_gametype` is GT_SINGLE_PLAYER or `ui_singlePlayerActive`
(upstream ioq3 rule) -> P2's getchallenge forever = "Player 2
connecting...". The game VM has no SP gate. Fix: answer 127.x on a listen
server; `spmap` latches 15 slots. Before: `work/r20-sp-before.log` (P2
`connecting client -1`, 4 unanswered getchallenge); after:
`work/r20-sp.log` (`sv_maxclients 15`, P2 in game 112 ms after its slot,
`status` lists Ranger, P1, Player 2), `work/r20-sp-2p.jpg` (both views,
scoreboard with three players). Not tested: the UI's own arena start
(`ui_singlePlayerActive` 1, same gate) and an arena's end/podium with two
humans.
**(5) UrT online.** The maintainer's log (`work/urthome/q3ut4/qconsole.log`,
read only): the connect to 74.91.113.242 (01:47) is the last line of that
run (the next line is a fresh start at 01:47:45); the
`CM_LoadMap ... ut4_asylum_b1.bsp` error itself is not in the file. His
`q3config-ss.cfg` has `seta cl_allowdownload "0"`, written from UrT's
`default.cfg` (zUrT43_020.pk3: `seta cl_allowdownload "0"`, `seta
cl_autodownload "1"` -- UrT's own client downloads through its own
mechanism): with downloads off a missing map can only end in that error.
The auth notice is printed at every start by UrT's ui (also at the R18
local games that worked) and is not a kick. Fix: turn `cl_allowDownload` on
once in q3ut4. libcurl: already static in the Windows build (`USE_CURL;
CURL_STATICLIB`, `code/libcurl/windows`, as upstream's release).
`work/r20-urt.log` (the maintainer's urthome configs, backed up and restored
byte-identical, `work/r20-urt-home.txt`): `urt: cl_allowDownload 0 -> 1
...`, `download: cl_allowDownload 1; HTTP/FTP (server sv_dlURL, dlmap) via
libcurl/8.4.0 Schannel (built in)`, `cl_urtDownloadDefault 1`. Code path:
`CL_InitDownloads` -> `CL_NextDownload` (cl_main.c: `sv_dlURL` redirect via
`CL_cURL_BeginDownload` unless `cl_allowDownload & 2`, else UDP). Not
tested online (the maintainer's item 103).
**(6) Cinematic skip:** `work/r20-cin.log`: pad 1 (nobody's) `PAD_B skips
the cinematic`, pad 0 holding A 1.5 s skips it and joins nothing (`padlist`:
both `player none`), a stick does not skip (`work/r20-cin-stick.jpg` still
the video), RT does; UrT: `work/r20-urt.log` `pad 0: PAD_A skips the
cinematic` (`work/r20-urt-playing.jpg` / `-skipped.jpg`).
**Regressions:** r13-two `work/r20reg-two*` (Independent: picker, Ada's
window reaches its match as a cell, pause overlay, quit closes it, only
ada.cfg + _padlast.cfg written); r10-single `work/r20-reg-single.log` +
`-devmap.jpg` (no profiles dir); r8-join `work/r20-reg-r8join.log` (TEST/P/
profile/pad lines identical to `work/r19b-reg-r8join.log` apart from qports
/ ms; run with `in_gamepad 0`: this box now lists two real "Xbox 360
Controller" devices, which take pad slots 0/1 with `in_gamepad 1` and shift
the script's virtual pads); r17-strings `work/r20-reg-strings.log` + jpgs
(Menu size 1 / 0.5 / 1.5 / 1 as R19); r18-binds (UrT)
`work/r20-reg-binds.log` (bind/TEST/profile lines identical to
`work/r18-binds.log`; urthome restored byte-identical,
`work/r20-reg-binds-home.txt`; the runner rewrote `work/r18-binds-*.jpg`
with this run's equal shots, copies `work/r20-reg-binds-*.jpg`). Release
build 0 C warnings (`work/r20-build.log`).
**Not verified:** real pads, a real UrT server download, Joyxoff, the SP
podium with two humans, OpenGL, Linux (code is portable; the backdrop was
Windows-only). **Note:** while this round ran, someone else edited
`README.md` and staged a new `docs/README.upstream.md` in this tree; R20
commits never included them (left as found).

**R19b (Windows, 2026-10-07; Opus executor; VERSION 0.0.0.25;
commits `c4968c06`, `6ba28c48` + this docs commit).** Review finding: an
undone "duplicate" kept its key in the other window's key list (second
person could never join; that window could open the friend's pad). Fix:
(1) `CL_IndepPadUnalias` removes the key (never the first) and re-sends
`pad ...`; a window whose open device is no longer listed closes it; called
where the duplicate mark is undone; (2) a duplicate is aliased only after
1 s of matching and a confirming press (`... is confirmed as a second
listing ...`); while merged its presses are ignored, the undo log says
"press again to join". Together mode: same-path second listing skipped.
**Verified (bus):** `work/r19b-twohold.log` + `-child2/3.log` (6 and 7 hold
A in one frame -> merged, undone ~150 ms after 6 lets go first, never
aliased; 6 -> P2 window with only its key, 7 holds A again -> P3; LB only in
P2's window, RB only in P3's); `work/r19b-pads*` (the coordinator-only
duplicate is confirmed at the picker press, P3's window opens the listing
it sees; P1/P2/P4/replug as R19); `work/r19b-dedupe*` (as R19);
`work/r19b-together.log` (Together: same-path device 5 skipped, device 6
opened). Regressions: `work/r19b-reg-single.log` (no profiles dir),
`work/r19b-reg-r8join.log` (`in_gamepad 1`, TEST/P/profile/pad lines
identical to R18 apart from qports/ms). Release build 0 C warnings
(`work/r19b-build.log`). Not verified: real pads (none connected). Note:
The maintainer's real 2C listings may have two different paths (two HID
collections); the Together same-path rule does not catch that -- only
Independent mode's mirrored-input rule does.

**R19 (Windows, 2026-10-07; Opus executor; VERSION 0.0.0.24;
commits `204c2f98`..`49778944` + this docs commit).** Independent mode
robustness + experimental label (design 17.5).
**Root causes** (from the maintainer's own logs of the 0.0.0.23 test,
`work/q3home/baseq3/qconsole.log` + `qconsole-child2..4.log`, 22:38-22:41):
(1) windows got `guid#ordinal` and each process orders identical pads
differently -> P2's window opened P1's 8BitDo Ultimate 2 ("P1 moves all
instances", "P1 was controlling P2"); (2) the coordinator listed the 8BitDo
2C as two devices (same GUID, identical input in the log), the window
listed it once: one A hold joined P3 and P4, P3's window got `#1` (absent
there) -> no pad, P4 got `#0` and worked ("P3 joined as P4"); (3) a replug
appends to SDL's list, so the ordinal-based reclaim missed. Joyxoff
injects into the foreground window only (not a cause of the cross-window
movement).
**Before (bus, old keys; commit `06c5a7ad`):** `work/r19before-pads.log` +
`-child2..5.log`: P2's window opened device 0 = P1's pad and logged P1's X
press; the 2C (+ its coordinator-only duplicate) joined twice, P4's window
opened nothing; the late pad's window (P5) also opened P1's pad; P2's
replugged pad was not reclaimed and its A hold opened a P6 picker.
**After:** `work/r19-pads.log` + `-child2..4.log` (summary of both:
`work/r19-pads-summary.txt`): one `indep: pad <key> -> P<n>` per pad, each
window opens exactly its key's device and logs only its own pad's press
(P2 LB, P3 RB, late P4 Y), the duplicate is ignored and aliased (P3's
window opens the listing it sees), the late pad joins once as P4, the
replug -> `-> P2 (...): reconnected` and P2's window reopens it, its A hold
opens nothing. `work/r19-dedupe.log` + children: a same-path device is not
opened; two people pressing A in one frame -> duplicate, undone 150 ms
after one presses X alone, then that pad joins once; a path-less device
replugged under a new order key -> matched by GUID, its window told the
new key, input arrives there.
**Backdrop / detection:** `work/r19-menu.log`, `work/r19-pads.log`:
`indep: backdrop window over 64,64 1280x720`, `backdrop stacking: 4 tile
windows above it, 0 below`, off/on via the cvar; `indep: shell
QUNS_ACCEPTS_NOTIFICATIONS (5); foreground another program's window ...
does not cover the monitor` (headless runs never focus a tile -- the value
with a focused tile is the maintainer's item 97). Plainly: a check that compares
the foreground window with the monitor cannot pass in this mode, the
focused tile is a quarter/half of the screen.
**Label:** `work/r19-menu-session-row.jpg` (Together value, wrapped help),
`work/r19-menu-session-indep.jpg` ("< Independent (experimental) >").
**Regressions:** `work/r19reg-two*` (r13-two, Independent with virtual
pads: picker, Ada's own window reaches its match, quit closes it, only
ada.cfg written), `work/r19-reg-single.log` + `-r10-single-devmap.jpg` (no
profiles dir), `work/r19-reg-r8join.log` (`in_gamepad 1`; TEST/P/profile/
pad lines identical to R18 except random qports and ms),
`work/r19-reg-strings.log` + `r19-reg-strings-*.jpg` (as R17/R18). Release
build 0 C warnings (`work/r19-build.log`). Review add-ons (compile-only):
profile files with `toggle` lines skip the R11->R18 UrT layout upgrade;
`Com_UrTHunkDefault` honours `+seta/+sets/+setu com_hunkMegs`.
**Not verified:** real pads / real SDL device paths (no pad was connected
to this box during the round: SDL listed 0 devices), Joyxoff, a focused
tile's notification state (taking the foreground is not allowed headless),
OpenGL, Linux (bus code is portable; the backdrop is Windows-only).

**R18 (Windows, 2026-10-07; Opus executor; VERSION 0.0.0.23;
commits `aabbaa46`..`04d42080` + this docs commit).** The maintainer's UrT
real-pad list, all six items. (1) **Auto-detect + hunk:** `work/r18-auto.log`
(`fs_basepath` = the UrT install, no `fs_basegame`, the home's
`com_hunkMegs` set to 128 by `R18_PRE`: `urt: detected q3ut4 install,
fs_basegame q3ut4`, `urt: com_hunkMegs 128 -> 1024 (Urban Terror)`, 1 GB
hunk, `mapname ut4_turnpike`) + `r18-auto-turnpike.jpg` (UrT's CTF team
menu on turnpike); `work/r18-drop.log` + `r18-drop-turnpike.jpg` +
`r18-drop-dropdir.txt` (the exe copied into `work/r18-urtdrop/` beside a
`q3ut4` junction to the UrT install, no `fs_basepath` either: detected,
`fs_basepath` = that folder, ut4_turnpike loads; junction and folder
removed after; nothing new in the maintainer's UrT folder: newest file there is
his own q3config.cfg of 2026-08-19). (2) **Whitelist kick** -- research
(background Sonnet agent): the string is not in FrozenSand
ioq3-for-UrbanTerror-4 (its `SV_DirectConnect` checks only protocol /
challenge / qport) and not explained by the QVM; it is B3/B4's `vpncheck`
plugin (dkman123/B4 master 9cf0e7eb, `b4/extplugins/vpncheck/__init__.py`
`checkClient()`: `if sclient.app != "" and sclient.app not in
self._whitelist_client_list: ... reason = 'Non whitelist client ' + ...`;
`b4/parsers/iourt43.py`: `bclient['app'] = bclient['client'][0:32]` from
the ClientUserinfo line). Quake3e's `CL_CheckForResend` adds `client
Q3_VERSION`; UrT's own client adds no `client` key (its version string
"ioQ3 1.35 urt 4.3.4 <platform> <date>", `q_shared.h` / `common.c`, is
the `version` serverinfo cvar, never sent at connect; the UrT 4.3.4 exe in
the maintainer's folder carries "ioQ3 1.35 urt 4.3.4"). Fix: no `client` key in
q3ut4 -- what UrT's client sends. Evidence: every UrT log, e.g.
`work/r18-auto.log` line `urt: connect userinfo to loopback: no "client"
key (as UrT 4.3's client), version "Q3 1.32e splitscreen-0.0.0.23
win_msvc-x86_64 Oct  7 2026"`, `work/r18-zoom.log` (P2-P4 to 127.0.0.1
the same; all four played on our listen server without the key). Not
tested against a live server (maintainer, checklist 87). (3) **Scope:**
reproduced (`r18-zoom-*-before.jpg`: the scope only in the cell's 4:3
area, zoomed world left and right); cause: our 4:3-centered 2D (UrT draws
the scope as one full-screen pic), not a scissor bug. Fix in
`CL_SplitDrawStretchPic`. `work/r18-zoom.log` + `r18-zoom-4p-before/
-after.jpg` (2x2, 16:9 cells, all four PSG-1 / SR8 zoomed),
`r18-zoom-2p-before/-after.jpg` (top/bottom 32:9),
`r18-zoom-2pv-before/-after.jpg` (side by side, tall cells: bars above
and below now black), `r18-zoom-2p-stretched.jpg` (HUD shape Stretched:
UrT stretches its own scope, unchanged). baseq3: `work/r18-q3zoom.log` +
`r18-q3zoom-before/-after.jpg` (+zoom, no overlay, identical) and
`r18-q3zoom-menu(-bars0).jpg` (red Server options page). (4) **Blue
menus:** `r18-binds-controls*.jpg`, `-buttons*.jpg` (UrT); baseq3 red:
`r18-strings-r17-strings-host.jpg`, `r18-q3zoom-menu.jpg`, log
`cl_splitMenuColor "auto"`. (5) **Binds:** `work/r18-binds.log`
(`in_padDebug 1`, Guest P1 on virtual pad 0, ut4_turnpike FFA): each
button -> `pad bind: p1 <cmd>` (A +moveup, X +button5, Y +button6, d-pad
ut_itemdrop / ut_weapdrop / ut_itemuse nvg / +button3, R3 ut_weaptoggle
knife, LB ut_zoomreset, RB weapnext, LT ut_zoomin, RT +attack, Back
+scores, Misc ut_itemuse); `p1_` and `guest_joy_crouchToggle /
joy_sprintToggle` 1; crouch toggle (B: `+movedown` down on the first tap,
`-movedown` on the second), hold with the row on Hold (down at press, up
at release); sprint toggle (L3 with the stick forward: `+button8` latched
2 s, stick released -> "P1 stopped moving" -> `-button8` 0.3 s later; a
second L3 ends it; Hold: down/up with the button); Controls rows Crouch /
Sprint switch `p1_joy_*Toggle` 0/1 (`r18-binds-controls.jpg`); Buttons
page (`r18-binds-buttons.jpg`, `-buttons2.jpg`, `-buttons3.jpg`: every UrT
action, unbound `--`); rebind "Interact / pick up" -> Misc (`pbindlist`:
`PAD_MISC1 "+button7"`, `r18-binds-rebound.jpg`); `profile_save 1
r18prof` -> `work/r18-binds-r18prof.txt` (per-game file ends with `toggle
crouch "1"` / `toggle sprint "1"`, no toggle in the shared file); a planted
R11-layout profile (`r18-oldprof.sh` via `R18_PRE`) loads as "had the R11
built-in q3ut4 pad layout; now the current one" with the new binds and
toggles on. (6) Server options UrT greying untouched. **Regressions
(baseq3, `work/q3test`):** `work/r18-single.log` +
`r18-single-r10-single-devmap.jpg` (r10-single: no profiles dir),
`work/r18-r8join.log` (r8-join, `in_gamepad 1`: TEST/P/profile/pad lines
identical to `work/r17-r8join.log`), `work/r18-strings.log` (r17-strings:
Menu size 1 / 0.5 / 1.5 / 1 as R17) + `r18-strings-*.jpg`. Every UrT run:
`work/urthome` configs and profiles restored byte-identical
(`work/r18-*-home.txt`). One self-inflicted slip, fixed: an r18-q3zoom
run with `+set cl_splitP1Input kbm` archived it into
`work/q3test/baseq3/q3config-ss.cfg` (the first r8-join / strings reruns
then had no pad P1); the line was removed and both reran clean. Release
build 0 C warnings, 3 exes, BUILD-INFO 0.0.0.23 (`work/r18-build.log`).
**Not verified:** real pads, a live online UrT server (both the maintainer,
87-95); OpenGL; Independent mode with UrT; the overlay-bar rule in other
mods (any full-height pic touching the screen edge now extends: by design).

**R17 (Windows, 2026-10-06/07; Opus executor; VERSION 0.0.0.22;
commits `42d80dc7`..`04db259d` + this docs commit).** The maintainer's pass-2 list,
all nine items. (3) **P1 model page:** reproduced headlessly at 2560x1440
(P1's cursor drawn at 1413,1392 = bottom edge instead of 1032,1020; at
1280x720 it worked, which is why R14a passed). Measured with a temporary
`uimovetest` command (removed; cfg kept as `work/r17-mousetest.cfg`, logs
`work/r17-mt-1280/-1920/-2560.log`): a +300 mouse delta moves the maintainer's
a51 ui cursor 300 / 338 / 450 screen pixels at 720 / 1080 / 1440 lines
(the open-loop move assumed 1.5 / 2.25 / 3 per unit), big deltas
non-linear. Fix: closed loop on the menu's own cursor draw (design 19).
Evidence `work/r17-model-1440.log` (P1 at 2560x1440: corner -> 380,480 ->
218,356 -> "next page", first portrait sarge/icon_blue -> biker -> doom ->
biker for RB, RB, LB; P2 / P3 in their 640x480 menus the same, 2 steps),
`work/r17-model-1280.log`, `work/r17-model-720.log` (pre-fix, 720p),
`r17-model-p1-page/-p1-next/-p2-next.jpg`. (1)(2) **Aim assist:**
`work/r17-aim.log` + `work/r17-aim-table.txt` (r12-aim: Low A min 39.20
mean 45.16 of 61.25, C rot -11.15 of -35.81; Standard 27.56 / 35.08, C
-19.91 / -37.10 = R14a; B idle 0; E 226 ms; F 0); `work/r17-aimdef.log`
(defaults `joy_aimAssist 0`, `cl_aimAssistAllow 1`; P1/P2 Guests
`AA P1: off (Guest)`, `guest_joy_aimAssist 0`; `profile_save 2 r17newbie`
-> "off (profile r17newbie)", set 2, reload from the file -> 0; profile
deleted after). **The maintainer's live home (`work/q3home`, read only):** his
profiles `charlie` / `samuel` have `joy_aimAssist "2"` (stay Standard) and
his Guest defaults file `profiles/_guest.cfg` has `joy_aimAssist "2"` --
his own log shows he set Guest defaults -> Aim assist Standard in an
earlier session -- so his Guests and new profiles still start Standard
until he sets Guest defaults -> Aim assist Off (open question).
(4)(5)(6) **Strings / Menu size:** `work/r17-strings.log`
(`cl_splitMenuSize` 1 default, left stops at 0.50, right at 1.50) +
`r17-strings-host.jpg` ("4:3 Centered", "1.00x"), `-size050.jpg`,
`-size150.jpg`, `-controls.jpg`; `work/r17-strings-grep.txt` (no
"centred"/"centre" or "N %" left in our files and the design doc;
CHANGELOG / STATUS history untouched). Menu size kept its R14a meaning
because the maintainer's live `q3config-ss.cfg` already has `cl_splitMenuSize "1"`
(he turned the 1.5 default down) -- the lead's "rebase 1.00 = R14a's 1.5"
was conditional on him liking 1.5. (7) **Power-ups:**
`work/r17-powerups.log` (q3dm7: map's own `item_quad x2`,
`holdable_teleporter x1`; Off -> one map_restart, "default weapons, no
power-ups: 552 entities, 9 weapon pickups, 22 ammo, 3 removed", `srvdebug
ps ... no-powerups 1`; On -> map_restart, the map's own again) +
`r17-powerups-off/-quadgrey.jpg`; UrT `work/r11-vk-r17-urt.log` +
`r11-vk-r17-urt-powerups.jpg` (Power-ups and Quad greyed). (8) **Cursor
speed:** `work/r17-cursor.log` (100 %: overlay 500, game menus 700
units/s; the pad steps 110..200 % with a log line each; 200 %: 1000 /
1400; in 10 frames the overlay dot moved 91 -> 183 menu units, the red
cursor 223 -> 447 px, both 2.0x; with `cl_padModCursorScale` 1 it was 159
px = 1.4x less) + `r17-cursor-row.jpg`, `-200.jpg`. There is no
cursor-style choice anywhere in our menus, so no "(default)" labels were
added (open question). (9) **Team Arena spectators:**
`work/r17-team-mp.log` (missionpack, mpq3ctf1, `g_gametype 4`,
`g_teamAutoJoin 0`: `P1 team: spectator (PERS_TEAM 3, g_gametype 4)`, P2
the same; P1 Join red -> `P1 team: red (PERS_TEAM 1)` "joined the red
team"; P2 Auto join "red 1, blue 0 -> blue" -> `P2 team: blue (PERS_TEAM
2)`; P2 Spectate (after the game's 5 s team-change limit; a first try
inside it got "May not switch teams more than once per 5 seconds") -> `P2
team: spectator`) + `r17-team-mp-p1-pause.jpg` (Join red / Join blue /
Auto join under Resume), `-p1-gamemenu.jpg` (Team Arena's own in-game
menu: a button bar at the top with "Join", cursor-driven: the stock Esc
path a pad player could not use), `-p2-pause.jpg`, `-joined.jpg`,
`-p2-spectate.jpg` (Spectate above Leave game); baseq3 TDM
`work/r17-team-q3.log` + `-q3-*.jpg` (same lines). **Regressions:**
`work/r17-single.log` + `-devmap.jpg` (r10-single: no profiles dir; one
new line `P1 team: free`), `work/r17-r8join.log` (r8-join, `in_gamepad 1`:
TEST/P/profile/pad lines identical to `work/r16-r8join.log` except the new
`P<n> team: free` lines and a 1 ms timing), `work/r17-reg16-page.log` +
`r17-reg16-*.jpg` (r16-page: same results as R16: g_speed 480, respawn 10,
no quad 2 removed, instagib 57 removed, Reset). Release build: 0 C
warnings, 3 exes, BUILD-INFO 0.0.0.22 (`work/r17-build.log`). **Not
verified:** real pads (maintainer, 81-86); OpenGL; Independent mode (untouched
code paths); the join / spectate rows on a remote server and in mods other
than baseq3 / missionpack.

**R16 (Windows, 2026-10-06; Opus executor; VERSION 0.0.0.21;
commits `9a2e4f95`..`67be739c` + docs commit).** The maintainer's feedback on the
R14-R15 reports, one commit per item: (1) Controls row "Outer deadzone"
(+ help line "Full speed at this stick tilt; lower = sooner."). (2)
Server options rows Quad damage / Player speed / Weapon respawn
(`cl_splitSrvQuad` -> `sv_splitQuad` -> [entities] drops `item_quad`;
`cl_splitSrvSpeed` scales `g_speed` from the game's own value;
`cl_splitSrvWeaponRespawn` sets `g_weaponrespawn`/`g_weaponTeamRespawn`,
Default = `Cvar_Reset` once). (3) Reset resets everything but the map.
(4) The game's own instagib: `SV_SplitGameCvar` hook in
`G_CVAR_REGISTER`; found UrT 4.3 `g_instagib` and OSP 1.03a
`match_instagib` (OSP's qagame also has `instagib_reload`,
`vote_allow_instagib`; no plain `instagib` cvar); a follow-up commit
makes our archived preset give way at a game's first load (the entity
string is rebuilt when the cvar registers, before G_InitGame parses).
(5) Bots 0-20 + fix: the page compared the bots' need with the
`sv_maxclients` cvar, which the server had already raised (latched)
while the page was open, so "no restart needed" and 20 bots stopped at
15 -- now `SV_SplitMaxClients()` (running slots). (6) FOV: no change.
(7) Taskbar: `GLW_SplitMarkShell` / `GLW_SplitUnmarkShell`
(`win_splitproc.c`, `win_glimp.c`). Player speed in UrT found useless
(`r16-urtspeed`: playing player's `ps.speed` 220 at `g_speed` 240 and
480) -> greyed there, `g_speed` untouched. **Evidence (`work/`):**
`r16-page.log` + `r16-page-speed/-quad.jpg`, `r16-instagib-grey/-grey2.jpg`,
`r16-reset-row/-done.jpg` (g_speed 320->480 live, respawn 5->10 and back,
quad off: "default weapons, no quad ... 2 removed", item_quad gone and
back after Reset; handicap 50 -> 100 under instagib; after Reset every
cvar at its default, gametype 0, s_volume 1, mapname q3dm7);
`r16-osp.log` + `r16-osp-row/-grey/-instagib.jpg` (OSP: row "Uses the
game's own instagib", leaving -> `match_instagib 1` + one map_restart,
"Instagib ENABLED!", gauntlet + railgun 999; off again -> DISABLED);
`r16-ospcvars.log` (cvarlist `*insta*`; preset archived on -> "our preset
is off", map's own entities); `r11-vk-r16-urt.log` +
`-bottom/-instagib-row/-instagib-grey/-instagib-game.jpg` (UrT: Quad /
speed / respawn greyed, g_speed stays 240, `g_instagib 1` via the row +
one restart, Player health greyed); `r11-vk-r16-urtspeed.log`;
`r16-bots.log` + `r16-bots-20.jpg` (sv_maxclients 16 -> one map_restart
-> 21, 20 bots, hunk 81.5 of 128 MB on q3dm7; Off kicks them);
`r16-fov.log` (`cg_fov` 115 before pads, 90 for P1/P2) +
`r11-vk-r16-urtfov.log` (UrT cg_fov default 90); `r16-two*.log/.jpg`
(r13-two: coordinator and child "marked fullscreen for the shell",
unmarked before every re-tile vid_restart and at quit, 0 processes left,
profiles as R15); `r16-mode.log` (r13-mode: marked when switched to
Independent, "no longer marked" on the Together switch, not re-marked);
`r16-single.log` + `-devmap.jpg` (r10-single: no mark line, no
profiles/); `r16-r8join.log` (r8-join, `in_gamepad 1`: TEST/P/profile/pad
lines identical to R15's but qports). Release build 0 C warnings (both
clients + dedicated). UrT home configs restored (`q3config.cfg`
byte-identical, the created `q3config-ss.cfg` deleted, profiles
unchanged). **Not verified:** real pads, the visual taskbar effect and
Joyxoff (maintainer, 73-80); missionpack (not run).

**R15 (Windows, 2026-10-06; Opus executor; VERSION 0.0.0.20;
commits `c2aadb08` (R14b review fixes), `d5027032` (SDL static + renames),
docs commit).** (1) **R14b review fixes** (`code/server/sv_splitrules.c`):
`godMask & (1 << i)` only for `i < 31`; when cheats were forced for god
mode and god goes off mid-game, `SV_SplitRulesFrame` waits until every
client's god toggle is off (the toggle itself needs cheats; a respawn
clears a stale `godSpawn`) and then sets `sv_cheats 0` live. New
`scripts/tests/r15-godcheats.cfg` (plain `map`, not devmap):
`work/r15-godcheats.log` -- cheats 0 -> "cheats on for god mode" -> god on
for clients 0/1 -> off -> "cheats off again (god mode is off)" -> cheats 0;
`r14b-god` (devmap) still keeps the devmap's own cheats. (2) **SDL2
static:** source `SDL2-2.32.10.zip` from libsdl-org's release (sha256
`12b2dc2e...af242f08`, `work/sdl2-src-PROVENANCE.md`), SDL's own
`VisualC\SDL\SDL.vcxproj` with `/p:ConfigurationType=StaticLibrary` +
`scripts\sdl2-static.props` -> `work\sdl2-src\lib\SDL2-static.lib`
(9,025,770 bytes, 0 warnings). First link failed with LNK2005 on
`MatrixMultiply` (SDL_d3dmath vs q_math.c) and 8 DirectSound/DirectInput
GUIDs (SDL_windowsjoystick vs win_snd.c / win_input.c): renamed on SDL's
side by defines in the props (`SDL2_` prefix). Linked into
`quake3e.vcxproj` only through `scripts\splitscreen.props`
(`SplitscreenSdlLib`, `Q3E_SDL_STATIC`); `in_gamepad.c` assigns the SDL
functions directly under that define (own prototypes, this file's types;
`SDLF( field, SDL_Name )` stringifies for the dynamic path). Exe sizes:
Vulkan 5,118,464 -> 6,620,160, OpenGL 4,834,304 -> 6,334,976, dedicated
962,048 unchanged; `SDL2.dll` (1,586,176) gone. (3) **Renames (L-9)**:
build.ps1 copies to `quake3e-vulkan-ss.x64.exe` / `quake3e-ss.x64.exe` /
`quake3e-ss.ded.x64.exe` (vcxproj untouched; intermediates keep upstream's
names), deletes the old names + `SDL2.dll`; 38 runners/cfg comments
renamed; `r13-run.sh` cleanup greps/kills both client names. (4)
`release.yml` Windows job = checkout + `scripts\build.ps1 -PlatformToolset
v143` + zip of the three exes (BUILD-INFO version check); the SDL2.dll
download step and pins removed. **Verified:** Release build 0 C/LNK
warnings (`work/r15-build.log`; only the 14 MSB8012/8028 notes as before);
**fresh clone** in a scratch dir: build.ps1 downloaded the SDL zip,
verified it, built everything (`work/r15-fresh-clone-build.log`, 0 C/LNK
warnings; MSB8029 only because the scratch dir was under %TEMP%).
**Exe alone** (`work/r15-alone/` holds only the two client exes; runner
`EXE_DIR=work/r15-alone scripts/tests/r10-run.sh vk|gl r15-alone.cfg alone
q3dm1 +set in_gamepad 1`): `work/r15-alone-vk.log/.jpg` (final 0.0.0.20
build) and `work/r15-alone-gl.log/.jpg`: `gamepad: SDL 2.32.10 static
(built in) ...`, `padlist` shows SDL, two virtual pads connect, pad 2 holds
A and joins (2 cells in the shot), "Using DirectSound subsystem" + "Sound
initialization successful" with pads on (R14a COM fix holds). **r8-join**
with `in_gamepad 1` (`work/r15-r8join.log`, `r15-r8join-r8-j-*.jpg`): the
66 TEST/P/profile/pad lines identical to `work/r14b-reg-r8join.log`.
**r13-two** (`R13_PREFIX=r15`, children with `in_gamepad 1`):
`work/r15-two*` from `build\Release` and `work/r15-two-alone*` from
`work/r15-alone` (`EXE_DIR`): the child starts from the renamed exe, SDL
static + DirectSound in the child, reaches its match, 0 processes left,
profiles as before. **Dedicated:** `r14a-cfg.sh ded` -> `work/r15-ded.log`
(`quake3e-ss.ded.x64.exe` loads q3dm1, seeds `q3config_server-ss.cfg`);
`dumpbin /imports` -> `work/r15-ded-imports.txt` (no SDL, SETUPAPI, IMM32,
VERSION, ole32, OLEAUT32, SHELL32), `work/r15-vk-imports.txt`,
`work/r15-gl-dependents.txt` (system DLLs only, no SDL2.dll). Exports of
the client: only `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance`
(`work/r15-vk-exports.txt`, no SDL symbols). **Not verified:** real pads
(maintainer, checklist 69-72); `release.yml` on GitHub (VS2022/v143);
`docs/LINUX-BOOTSTRAP.md` names no Windows exe (unchanged).
**R15 review fixes (`c4a31f85`, Sonnet review via the lead):** (a) a Debug
build would have linked the Release `/MT` SDL lib into `/MTd` clients:
`scripts\sdl2-static.props` now has Release (`/MT`, NDEBUG) and Debug
(`/MTd`, `_DEBUG`) settings and build.ps1 builds/passes
`SDL2-static.lib` or `SDL2-static-debug.lib` (31,048,724 bytes, /Z7)
per `-Configuration` (objects in `work\sdl2-src\obj\<Cfg>\`).
`build.ps1 -Configuration Debug` links cleanly (`work/r15-debug-build.log`:
0 LNK warnings; the 123 C warnings are all upstream libvorbis/libogg/
libjpeg/botlib in their Debug config) and the Debug Vulkan exe runs
`r15-alone.cfg` from `build\Debug` (`work/r15-debug-vk.log/.jpg`: SDL static,
pad 2 joins, DirectSound); Release rebuilt after it (`work/r15-build.log`,
0 C/LNK warnings). (b) `godSpawn` is now reset outside the alive gate
when god is no longer wanted, so a dead player no longer delays the
cheats-off until a respawn: `r15-godcheats.cfg` gained "god on, `p2 kill`,
god off while P2 is dead" -> `work/r15-godcheats.log`: "cheats off again"
with P2 still dead (pm 3, health -999), cheats 0.

**R14b (Windows, 2026-10-06; Opus executor; VERSION 0.0.0.19;
commits `a2b8ec3c` .. the docs commit).** Server options page per design
18 (as built + corrections: 18.4). **Spec corrections found in ioquake3:**
`PERS_SPAWN_COUNT` = 4 and `PERS_ATTACKER` = 6 (not 8/2); missionpack
shifts `STAT_WEAPONS/ARMOR/MAX_HEALTH` to 3/4/7; the real health is the
game-private `gentity->health` (STAT_HEALTH is a per-frame copy), so the
server locates that field by elimination (+732 in baseq3 1.32's 808-byte
gentity). Bugs found and fixed on the way: `Q_rand` returns negative
values (Random weapons crashed with "Q_strncpyz: NULL src" + a fatal
dialog: index now unsigned); commands queued with `Cbuf_AddText` ran only
after a test script's remaining text (now `EXEC_INSERT`); bot adds piled
up behind a script `wait` (9 bots for 2: now one pending addbot at a
time); trailing note rows never scrolled into view (fixed in the overlay).
**Evidence:** `work/r14b-page.log` + `r14b-pause/page-top/page-bottom/
maps-current/maps-next/page-pending/after-load.jpg` (pause row above End
game, help lines, levelshot thumbnails, "On leaving: loads ...", leaving =
exactly one `Server options: applied; devmap q3dm1`); `work/r14b-ents.log`
(each Weapons option: what the game parsed, e.g. "rocket only: 9 weapon
pickups, 22 ammo" -> `weapon_rocketlauncher x9, ammo_rockets x22`; gauntlet
only removes the 22 ammo boxes; Random differs on every restart; instagib:
`weapon_railgun x9` only, 57 pickups removed, spawn rail 999 slugs, health
100; open/leave without a change: "no restart needed") +
`r14b-rocket-only.jpg` (spawned holding the rocket launcher, 25 rockets);
`work/r14b-ps.log` (spawn All = weapons 0x3fe, Gauntlet only = 0x2 weapon
1; infinite ammo off 100 -> 90, on stays 999; self-damage on: health 121
-> 71 and z 0 -> 80; off: "self-damage 50/0 undone", health 71 kept, z 0
-> 82; player health 50 -> handicap 50 + max health 50 for P1/P2, back to
100; bots by pad: exactly 2, difficulty 3 = kickbots + 2 new at skill 3,
kept across the page's map change to q3dm17, Off kicks them; friendly fire
/ gravity 600 / volume 150 % by pad; set "a" saved by the on-screen
keyboard, selected (16 settings), deleted) + `r14b-gauntlet-only/bots-row/
rows/saved/select/delete-confirm.jpg` (its god section missed: superseded
by r14b-god); `work/r14b-god.log` (rail from P1 knocks P2 back with health
unchanged, again after P2's respawn; off: 121 -> 21) + `r14b-god-rail/
god-off.jpg`; `work/r14b-instagib.log` (one rail: P2 dead, P1 score 1) +
`r14b-instagib-page/kill.jpg`; `work/r14b-menu.log` + `r14b-mainmenu.jpg`
(main menu: map / type / restart greyed); `work/r14b-ded.log` (dedicated
exe with `sv_splitRules/Instagib/God/Bots` forced: map untouched, no bots,
no cheats); `work/r11-vk-r14b-urt.log` + `r11-vk-r14b-urt-page/page2/
pending/turnpike.jpg` (UrT: six rows greyed, game types by UrT name; map
change to ut4_turnpike with bots: `bot_enable` latched on by that load, 2
bots in (one extra added then kicked by the count keeper), Off kicks).
**Regressions:** `work/r14b-reg-single.log/.jpg` and
`work/r14b-final-single.*` (no profiles dir), `work/r14b-reg-r8join.log`
(menu/join/profile lines identical to `r14a-final-r8join.log`),
`work/r13-r14b-two*` (child reaches its match, its own Server options page
`r13-r14b-two-c2-server.jpg`, 3 bots on its own server then removed; 0
processes left; profiles only ada + _padlast). Release build: both client
exes + dedicated, 0 C warnings. Not run: GL, missionpack (layout 2 is
code-only), a real pad, r12 / r10-pads. No Sonnet review findings arrived
during this round.

**R14a (Windows, 2026-10-06; Opus executor; VERSION 0.0.0.18;
commits `19f37943` merge .. `3cc1299e`).** (0) **Merge** `<linux-box>/linux`
(L1-L5): one conflict (STATUS next-steps 0: both kept, Windows queue then
the Linux checklist); CHANGELOG auto-merged; VERSION 0.0.0.18; L4 replaced
`.github/workflows/build.yml` with `release.yml` (came with the merge).
Release build 0 C warnings. Regression after the merge: r10-single
(`work/r14a-merge-single.log/.jpg`: q3config seeded, no profiles dir),
r13-two (`work/r14a-merge-two*.log`; the child had "Sound initialization
failed" = the sound bug). **L5 on Windows** (`scripts/tests/r14a-cfg.sh
seed|noover|autoexec|ded|child`, evidence `work/r14a-cfg-<case>.log` +
`-ls.txt` with sha256): seed prints `config: q3config-ss.cfg created from
q3config.cfg`, upstream's file byte-identical after two runs and an
archived change, change kept in -ss; an existing -ss never overwritten;
`autoexec-ss.cfg` runs after `autoexec.cfg`; the dedicated exe seeds and
writes only `q3config_server-ss.cfg`; a `--child` window neither seeds nor
writes any q3config. (the maintainer's basepath `autoexec.cfg` sets `sensitivity`
and `cg_fov 115`: test with other cvars.) **Runners now use `work/q3test`**
(the classifier refused deleting the maintainer's profiles in `work/q3home`, which
`r13-two.sh` etc. do): `Q3HOME` env overrides; the maintainer's `work/q3home` was
never written this round (q3config sha256 and profiles identical to the
backup `work/r14a-maintainer-backup/`).
(1) **Sound root cause:** SDL's joystick init (in_gamepad.c, before
`S_Init`) puts the main thread in a single-threaded COM apartment, so
`win_snd.c`'s `CoInitialize(NULL)` returns **S_FALSE**, and upstream's
`!= S_OK` check returned "no sound" before trying WASAPI or DirectSound.
Fix: accept S_FALSE (balanced `CoUninitialize`) and RPC_E_CHANGED_MODE
(use as is), print the HRESULT otherwise. On this box (8-channel default
device): `sound: COM already initialised on this thread (S_FALSE)` ->
WASAPI declines 8 channels -> DirectSound 2 ch 22 kHz -> "Sound
initialization successful" with `in_gamepad 1` (`work/r14a-sound.log`)
and in the Independent child (`work/r14a-sound-two-child2.log`,
`work/r14a-final-two-child2.log`). (2) P1 Guest = "Player 1"
(`work/r14a-guest.log`: q3config name before the pad, Player 1 as
Guest, Zed on a profile, Player 1 again, q3config-ss keeps "rebelancap").
(3) **Aim assist finding:** The maintainer's log has `rebelancap ^3+` etc., i.e.
`CL_AimAssistOn` was true for his real 8BitDo pads (real and virtual pads
share `CL_GamepadMove` -> `CL_AimAssistApply`; local loopback game; FFA
team 0 = everyone a foe; his targets were the other local players, no
bots): **no gate was a no-op; the R12 numbers were too weak** -- r12-aim
before (`work/r14a-aim-before-table.txt`, same as R12/R13): Low 9 % /
Standard 19 % average slowdown inside a 40-unit (~3-4 deg) bubble, follow
4 / 8 deg/s. Retuned (design 15.1 "R14a retune"): bubble 72 / 12 deg,
full slowdown over the inner 40 %, slow 0.7 / 0.45, follow 0.3 / 0.6, cap
120 deg/s. r12-aim after (`work/r14a-aim.log`, `work/r14a-aim-table.txt`):
A sweep Low min 42.88 mean 47.96 (slow 0.700), Standard min 27.56 mean
35.10 (0.450) of 61.25 deg/s; C track Low rot -8.84 of omega -35.79,
Standard -19.94 of -38.80; D Low -6.44/-21.48, Standard -13.63/-22.72; B
idle 0/0; E flick 229 ms; F wall 0 targets. New log line on every state
change (`AA P1: Low (Guest)`, `AA P2: off: not a local game ...`); marker
` ^2+` on Standard (`work/r14a-aim-scores.jpg`: green P1, yellow P2).
(4) Overlay size (`work/r14a-menu-*.jpg`: full, host-scrolled, quarter,
quarter-size25, eighth, eighth-kb, eighth-size1/25): the font cap was a
fixed 24 px (one third of the intended size at 2160 lines); now
resolution-proportional x `cl_splitMenuSize`, shrunk to keep 26 columns
and title + 3 rows (the whole keyboard) in the cell; at 2.5 an eighth cell
is width-limited (same as 1.5). (5) Profile page (`work/r14a-profile*`):
Change player model reaches q3_ui's model page by keys (in-game menu ->
SETUP -> PLAYER -> MODEL), recognised by its art; LB/RB click its page
arrows (works for P2 in its cell and P1 at full resolution:
`-model-next.jpg`, `-model-p1-next.jpg`); arrow keys alone did not turn
pages in the maintainer's a51 UI build (`zzz-a51-ui.pk3`); Handicap 90 ->
`hc\90` in P2's configstring, saved as `userinfo handicap "90"`. (6)
Field of view 100 for P2 -> `p2_cg_fov 100` (`-fov-views.jpg`), P1 Guest
90, saved `joy_fov "100"`; P1's own 115 back in q3config-ss after quit.
(7) Mod-menu cursor 1.25x (`work/r14a-cursor-125.jpg` vs `-100.jpg`: ~298
vs ~238 px after 15 frames); old scripts that steer that cursor by frames
set `cl_padModCursorScale 1`. (8) X/Y in a refreshing server browser
(`work/r14a-browser.log`, `-refreshing/-stopped.jpg`: "Press SPACE to
stop" -> X -> "No Servers Found."; a second X sends nothing).
**Final regression on the last build:** r10-single
(`work/r14a-final-single.*`), r8-join (`work/r14a-final-r8join.log`,
all picker/keyboard/guest steps as before), r10-layouts
(`work/r14a-final-layouts*`: 0 errors, P1's cgame restarts only at
1<->2), r13-two (`work/r14a-final-two*`: child reaches its match, sound
on, q3config never touched by the child, 0 processes left). Not run: UrT,
GL, r12-remote, r10-pads, a real pad. Oops noted: two cleanup/rename steps
overwrote or deleted older evidence (`work/r12-q3-*.jpg` from R12 and
`work/r10-vk-l-*.jpg` from R10 -- the latter replaced by this round's
equivalents); none was cited by name in STATUS.

**L6 (Linux release-candidate check of 0.0.0.27, Linux test box, 2026-10-07;
Opus executor; VERSION 0.0.0.27, no code change).** `linux` fast-forwarded
to `splitscreen` (882d0d38, R14a-R21); `scripts/build.sh` -> all three exes
+ `BUILD-INFO.txt` (version 0.0.0.27, commit 882d0d38), 0 errors, 0
warnings. **No Linux-specific bug; no engine code changed.** All runs one
at a time on the desktop session, `in_gamepad 0` (Independent windows:
SDL on, own pad keys only) + virtual / bus pads,
each quit by its cfg, 0 quake3e processes after every run. Version line
of the runs: `work/l6-version.txt` (`Q3 1.32e splitscreen-0.0.0.27
linux-x86_64`). Script changes: `linux-run.sh` moves every shot of the run
(shot name without `<name>-`, without the part of `<name>` after its first
dash, or else without the round prefix `r<N>[a-z]-` and `<name>`'s last
word); new `scripts/tests/l6-r19-run.sh` (Linux `r19-run.sh`: `l3-run.sh`
+ `in_padBus 1 in_padDebug 1`, children `in_padBusOrder 1` + `r19-child.cfg`,
bus file copied to `work/l6-<name>-padbus.txt`). Evidence:
- **Boot smoke** `r10-single` q3dm1: vk `work/l6-vk-single.log` +
  `-devmap.jpg`, gl `work/l6-gl-single.log` + `-devmap.jpg` (radeonsi GL
  4.6 Mesa 26.0.1): map loaded, HUD, new R17 lines `P1 team: free`, `AA P1:
  off: no pad (keyboard/mouse)`; no `profiles/` dir.
- **r8-join** q3dm1 vk (`work/l6-r8-join.log`, `-j-*.jpg`): TEST/P/
  profile/pad lines identical to `work/l2-r8-join.log` apart from qports /
  ms, the R19 `key guid-padN` field on the pad lines and one new
  `profile: P1's settings saved to "Cleo"` at P2's join.
- **r10-layouts** q3dm7 vk (`work/l6-r10-layouts.log`, 16 `-l-*.jpg`): the
  125 `P<n>: <state> client <n> cell ...` lines identical to
  `work/l2-layouts-vk.log`; the same 3 P1 cgame restarts (1<->2); 0 ERROR.
- **r12-aim** q3dm7 vk (`work/l6-r12-aim.log`, `-table.txt`, `+set r12home
  "setviewpos 1850 -1475 -128 80"`): A sweep Low min 39.20 mean 45.11 (slow
  0.640), Standard 27.56 / 35.12 (0.450) of 61.25; C track Low rot -11.27 of
  omega -34.32, Standard -19.27 / -37.39; D Low -8.91 / -23.75, Standard
  -12.68 / -21.13; B idle 0/0; E flick 229 ms; F 20 traces 0 targets --
  Windows R17 (`work/r17-aim-table.txt`): Low 39.20 / 45.16, C -11.15 /
  -35.81; Standard 27.56 / 35.08, C -19.91 / -37.10. Same assist; the small
  C/D omega differences are the L2 frame-timing item (P2's scripted strafe).
- **r10-perf** q3dm17 vk uncapped (`work/l6-r10-perf.log`, `-avg.txt`,
  `-1/4/8.jpg`): com_speeds all 1/4/8 views 0.00 / 0.40 / 1.92 ms (L2
  0.00 / 0.46 / 1.94); the test home's q3config restored after.
- **r17-strings** q3dm7 (`linux-run.sh` with `PRE_ARGS=--noactivate`: SDL
  driver x11) `work/l6-r17-strings.log` + `-host/-size050/-size150/
  -controls.jpg`: `cl_splitMenuSize` 1 default, left stops at 0.50, right
  at 1.50, back to 1; "4:3 Centered", "1.00x", Controls "95%" / "15%" /
  "50%" / "100%" -- as Windows R17.
- **r17-team**: no missionpack on this box -> the baseq3 TDM variant (as
  Windows `work/r17-team-q3.log`): q3dm7 `+set g_gametype 3`
  (`work/l6-r17-team.log`, `-p1-pause/-p1-gamemenu/-p2-pause/-joined/
  -p2-spectate.jpg`): `P1 team: spectator (PERS_TEAM 3, g_gametype 3)`, P2
  the same, `g_teamAutoJoin 0`; P1 Join red -> `P1 team: red`; P2 `auto
  join: red 1, blue 0 -> blue` -> `P2 team: blue`; P2 Spectate -> `P2
  team: spectator`; pause page Join red / Join blue / Auto join (smaller
  team). Team Arena variant not run (no missionpack).
- **r20-cin** main menu (`work/l6-r20-cin.log`, `-nopad/-hint/-playing/
  -skipped/-skipped2/-stick/-trigger/-after.jpg`): no hint without pads;
  with pads + `cl_splitSeenHint 0` "Start a game first, then friends hold A
  to join" bottom right (over the stock CD KEY page: the test home has no
  q3key); `pad 1: PAD_B skips the cinematic`; pad 0's 1.5 s A hold `PAD_A
  skips the cinematic` and joins nothing (`padlist`: both `player none`);
  stick: still the video (`-stick.jpg`); `pad 0: PAD_RT skips the
  cinematic`; after `map q3dm7`: `pad: first game started, first-launch
  hint done (cl_splitSeenHint 1)`, no hint on the menu again, `seta
  cl_splitSeenHint "1"` in the test home's `q3config-ss.cfg`.
- **r20-sp** main menu (`work/l6-r20-sp.log`, `-2p.jpg`): `spmap q3dm1` ->
  `g_gametype 2`, `sv_maxclients 15`; `addplayer 2` -> loopback
  `getchallenge` answered, `P2: in game 112 ms after taking its slot`;
  `status` lists Ranger (bot), UnnamedPlayer (loopback), Player 2
  (127.0.0.1); the shot has both views.
- **r19-pads** Independent mode on the bus (`scripts/tests/l6-r19-run.sh vk
  r19-pads.cfg r19-pads -`; `work/l6-r19-pads.log`, `-child2/3/4.log`,
  `-three/-end.jpg`, `-padbus.txt`, `-x11.txt`, `-procs.txt`; summary
  `work/l6-r19-pads-summary.txt`): one `indep: pad <key> -> P<n>` per pad
  (P1 coordinator, P2, P3, late P4); pad 3 `duplicates pad 2` -> ignored,
  then `confirmed as a second listing of P3's pad`, its key sent to P3's
  window, which opens the listing it sees (reverse bus order); each window
  opens exactly its own key (every other bus device and the box's real
  "Xbox 360 Controller" `belongs to another window: not opened`) and logs
  only its own pad's press (P2 LB, P3 RB, P4 Y); P2's pad unplugged -> `went
  away`, replugged -> `-> P2 (...): reconnected`, P2's window reopens it
  and sees LB, its A hold opens no window; quit closes all three windows
  (20-40 ms), 0 processes after; `_NET_ACTIVE_WINDOW` never a game window,
  no game window ever focused.
- **Independent 2p smoke** `L3_PREFIX=l6 scripts/tests/l3-two.sh vk`
  (`work/l6-two.log`, `-child2.log`, `-*.jpg`, `-x11.txt`, `-q3config.txt`,
  `-procs.txt`, `-profiles.txt`): tiles P1 0,0 1280x335 | P2 0,335 1280x335
  in the 1280x670 usable area; both processes log `window: tile 1280x335 is
  a cell: cgame screen 640x480 (HUD shape 4:3 centered)` (the R20 tile
  aspect line); `-c2-game.jpg` / `-coord-play.jpg` look like Together
  top/bottom cells (HUD centred 4:3, wide view); `q3config-ss.cfg` mtime
  unchanged while P2 ran, child marker never present; no focus taken; 0
  processes after; profile files as L3/L5 (`profiles/ada.cfg`,
  `_padlast.cfg`, `baseq3/profiles/ada.cfg`).
Skipped: UrT variants (R18/R20/R21: UrT is not on this box), Team Arena
(no missionpack), r21-dl (UrT-like download test, needs `fs_game q3ut4`
data). Not verified: real pads (maintainer, L-12), OpenGL beyond the boot
smoke, Deck game mode.

**L5 (configs side by side with upstream, Linux test box, 2026-10-06; Opus
executor; VERSION 0.0.0.17).** Change: `code/qcommon/qcommon.h`
(`Q3CONFIG_CFG` -> `q3config-ss.cfg` / `q3config_server-ss.cfg`,
`Q3CONFIG_CFG_UPSTREAM`, `AUTOEXEC_SS_CFG`, `Com_SeedSplitConfig` decl),
`code/qcommon/common.c` (`Com_SeedSplitConfig`, `Com_FileReadable`, hooks in
`Com_ExecuteCfg`), `code/qcommon/files.c` (seed before the fallback exec;
`FS_BannedPakFile` also bans upstream's name and `autoexec-ss.cfg` in
pk3s), a comment in `cl_splitindep.c`; no new source file. The
`autoexec-ss.cfg` probe opens/closes the file (not `FS_ReadFile`, which
would desync a `journal 2` playback); `exec`/`execq` both print "couldn't
exec" for a missing file, hence the probe. Test runners
(`linux-run.sh`, `l3-run.sh`, `r11/r12/r13-run.sh`, `r11-profiles.sh`,
`r11/r12-remote.sh`, `r9-polish-setup.sh`, watcher/echo texts) now name the
`-ss` files. New `scripts/tests/l5-cfg.sh <case>` (scratch home
`work/l5-home-<case>/`, deleted after). Build: 0 warnings. Evidence (each
`work/l5-<case>.log` + `work/l5-<case>-ls.txt` = file list with mtimes +
sha256 before/after):
- **fresh** (empty home, main menu, quit): `baseq3/q3config-ss.cfg`
  written, no `q3config.cfg`, no seed line (log: `couldn't exec
  q3config-ss.cfg`, `couldn't exec autoexec.cfg`, nothing about
  autoexec-ss).
- **seed** (home `baseq3/q3config.cfg` with `seta name "SeedTest"`, `bind x
  "say seeded"`): `config: q3config-ss.cfg created from q3config.cfg`,
  `"name" is:"SeedTest"`, `"x" = "say seeded"`; `q3config.cfg` sha256
  `aac8eb8c...` identical before/after, file mtime unchanged.
- **noover** (same home again): run 1 `seta sensitivity 7`, run 2 prints no
  seed line and reads `sensitivity 7`, name SeedTest; `q3config.cfg` still
  `aac8eb8c...`.
- **autoexec** (`autoexec.cfg` cg_fov 100, `autoexec-ss.cfg` 110):
  `execing autoexec.cfg`, `execing autoexec-ss.cfg`, `"cg_fov" is:"110"`.
  **autoexec-none** (no -ss file): `cg_fov 100`, no line about
  `autoexec-ss.cfg`.
- **side** (`work/l5-side/`: copy of upstream's
  `~/Games/quake3/quake3e/quake3e-vulkan.x64` (read only, copied) + ours +
  `baseq3` symlink to `~/dev/q3data/baseq3`, no `fs_basepath`, one shared
  scratch home with `profiles/ada.cfg` and `autoexec-ss.cfg`): upstream run 1
  writes `q3config.cfg` (name UpstreamOne); ours seeds `q3config-ss.cfg`
  from it, execs `autoexec-ss.cfg`, finds the profile, saves sensitivity 7;
  upstream run 2: `sensitivity 5`, its `q3config.cfg` sha256 `b80bbfc8...`
  unchanged across all four runs, its log has no `autoexec-ss` and no
  profile line; ours run 2: `sensitivity 7`, no seed line.
- **mod** (`~/dev/q3data` has no `missionpack`: a scratch mod `l5mod` in
  the home path with its own `q3config.cfg` instead; `+game_restart l5mod`
  at run time, `work/l5-mod-stdout.log` because `game_restart` resets the
  temp `logfile` cvar): baseq3 seeded (name BaseSeed), then the mod seeded
  (`config: ...` again, name ModSeed), `l5mod/q3config-ss.cfg` holds
  ModSeed / sensitivity 4. (The command-line `+name +quit` after
  `game_restart` ran inside the restart's own `Cbuf_Execute`, so this run
  quit before reaching autoexec there -- an artifact of `+` sequencing, not
  the change.) The files.c "invalid game folder" fallback hook was not
  exercised (reasoned: same helper, same guards).
- **ded** (`quake3e-ss.ded.x64 +set dedicated 1 +map q3dm1 +quit`, home
  with `q3config_server.cfg` sv_hostname SeedServer):
  `config: q3config_server-ss.cfg created from q3config_server.cfg`,
  `"sv_hostname" is:"SeedServer"`, `q3config_server-ss.cfg` written,
  upstream's file sha256 unchanged.
- **Independent child**: `L3_PREFIX=l5 scripts/tests/l3-two.sh vk` (desktop,
  X11): `work/l5-two.log`, `work/l5-two-child2.log` (child execs
  `q3config-ss.cfg`, no seed line, sets the archived marker
  `cl_splitMenuColor "0 1 0"`), `work/l5-two-q3config.txt` (0.5 s samples
  of `q3config-ss.cfg`: mtime unchanged for 54 s while P2's window ran,
  one change at 09:51:27 = the coordinator's own write at quit, marker
  never present), `work/l5-two-procs.txt` (0 processes after). The child
  log's TEST echo still says "q3config.cfg" (l3-child2.cfg text, fixed
  after the run).
Not verified: Windows (same shared code; Next steps L-11); the files.c
fallback path.

**L4 (Linux release layout, Linux test box, 2026-10-06; Opus executor;
VERSION 0.0.0.16).** The maintainer's decisions: Linux ships like upstream
(plain executables, renderer built in, nothing else in the archive), names
`quake3e-vulkan-ss.x64` / `quake3e-ss.x64` / `quake3e-ss.ded.x64`,
dedicated server kept, a GitHub release workflow now. (A) `scripts/build.sh`:
two static client links (`RENDERER_DEFAULT=vulkan CNAME=quake3e-vulkan-ss
BUILD_SERVER=0`, then `RENDERER_DEFAULT=opengl CNAME=quake3e-ss
DNAME=quake3e-ss.ded`, both `USE_SDL=1 USE_RENDERER_DLOPEN=0`) in separate
`BUILD_DIR`s `build/obj/vk` / `build/obj/gl` instead of upstream's `make
clean` between them; installs + strips into `build/release-linux-x86_64/`;
removes the old `quake3e.x64`, `quake3e.ded.x64`, `quake3e_*_x86_64.so`
and object dirs there; the version-stamp `rm -f` covers both object dirs
(an unchanged rebuild recompiles only the 4+1+4+1 stamp objects,
`work/l4-build-incr.log`). Build `work/l4-build.log`: 0 warnings in our
files (the 2 upstream libvorbis `-Wmaybe-uninitialized` per variant).
`work/l4-outputs.txt`: exactly the three executables + `BUILD-INFO.txt`,
`file` = stripped, `ldd` of both clients shows `libSDL2-2.0.so.0` and no
renderer `.so`. (B) `scripts/package.sh` -> `build/quake3e-splitscreen-
0.0.0.16-linux-x86_64.tar.gz` (2.4 MB; `work/l4-package.txt`: `quake3e-
vulkan-ss.x64`, `quake3e-ss.x64`, `quake3e-ss.ded.x64`, 0755, root/root, at
the root). (C) `scripts/linux/` (launcher + `install.sh`) removed. (D)
`linux-run.sh`, `l3-run.sh` (+ `l3-ckill.sh`, `l3-tiles.sh` process
listings): new exe per vk|gl, no `cl_renderer`, guard by `/proc/<pid>/exe`.
(E) `.github/workflows/build.yml` (upstream's) deleted,
`.github/workflows/release.yml` added: `version` job (tag minus `v` must
equal VERSION; dispatch skips the check), `linux-x86_64` (ubuntu-24.04,
upstream's apt list, runs `scripts/build.sh` = the same make lines, tar of
the three exes), `windows-x64` (windows-2022, upstream's msvc x64 msbuild
sequence with `/p:PlatformToolset=v143` + `scripts/splitscreen.props` via
`/p:ForceImportBeforeCppTargets` and `/p:SplitscreenVersion`, outputs
`quake3e-vulkan-ss.x64.exe` / `quake3e-ss.x64.exe` /
`quake3e-ss.ded.x64.exe` + `SDL2.dll` 2.32.10 from libsdl-org's release with
zip and dll sha256 checks), `release` (tag pushes only, `contents: write`,
`softprops/action-gh-release@v2` attaches both archives, creating the
release if missing). actionlint 1.7.12 (+ shellcheck) via podman: 0 errors
(`work/l4-actionlint.txt`). **Linux job replayed** in `ubuntu:24.04`
(podman, same apt list, `scripts/build.sh` without distrobox, same tar
line: `work/l4-ci-ubuntu-sim.log`): builds, tar lists the three exes, the
Ubuntu binaries need at most GLIBC_2.34 and ran on this host (dedicated
loaded q3dm1). **Windows job not run** (cannot be from here). **Runs**
(`pgrep` empty before each; each quit by its cfg; nothing left running):
`linux-run.sh vk|gl r10-single.cfg` q3dm1 -> `work/l4-vk-single.log`,
`-stdout.txt`, `l4-vk-single-single-devmap.jpg` (RADV STRIX1) and
`work/l4-gl-single*` (radeonsi, GL 4.6 Mesa 26.0.1); both log
`"version" is:"Q3 1.32e splitscreen-0.0.0.16 linux-x86_64"`, SDL on
wayland, no profiles. Dedicated `quake3e-ss.ded.x64 +set dedicated 1 +map
q3dm1 ... +quit` -> `work/l4-ded.log`, `-stdout.txt` (Server: q3dm1, bots'
aas loaded, clean "Server quit"). **Archive as shipped:** unpacked into
`work/l4-unpack/` (deleted after) and run from there: with `+set
fs_basepath ~/dev/q3data` (`work/l4-unpack-basepath*`), with no
`fs_basepath` and a `baseq3` symlink beside the exes started from its own
folder (`l4-unpack-cwd*`) and **started from `~`**
(`work/l4-unpack-othercwd*`: "Working directory" = the exe's folder, paks
from `l4-unpack/baseq3`) -- all three boot q3dm1 on Vulkan at 0.0.0.16.
Deviation from the brief: the default basepath is the executable's folder
(dirname of `/proc/self/exe`), not the working directory -- better for a
Steam shortcut.

**L3 (Independent mode on the Linux desktop, Linux test box, 2026-10-06; Opus
executor, commits `af7024d5` + `552e76a1`; Sonnet review: no high
findings, no Together regression; three lows fixed by the lead and the
tiles scenario rerun on the fixed build, `work/l3f-tiles*` identical
behaviour: oversized IPC datagrams are dropped (`MSG_TRUNC`) instead of
delivered truncated, a child command line of more than 1024 words refuses
to spawn instead of being cut, the wayland->x11 switch is evaluated only
when the window is being recreated; left as noted: `Sys_SplitProcClose` on
a still-running child leaves a zombie until exit (callers kill first),
`Sys_SplitUnavailable` trusts a non-empty `DISPLAY`; VERSION 0.0.0.15;
`work/l3-build.log` 0 warnings).** Every R13 scenario passes
headless on Vulkan, the main ones on OpenGL too (runner
`scripts/tests/l3-run.sh` = r13-run with an X11 watcher: xwininfo rects,
`_NET_WM_STATE_FOCUSED`, `_NET_ACTIVE_WINDOW` sampled every 0.25 s;
wrappers `l3-tiles.sh`, `l3-two.sh`, `l3-ckill.sh`). **Tiles**
(`work/l3-tiles.log`, `-child2..4.log`, `-x11.txt`, `-kill.txt`, `-3win.jpg`,
`-4win.jpg`, `-c2-menu.jpg`, `-c4-exit.jpg`; gl: `l3-tiles-gl*`): 2 windows
P1 0,0 1280x335 | P2 0,335 1280x335; 3 = 640x335 + 640x335 + 1280x335 wide;
`cl_splitWidePlayer 2` moves P2 to the wide slot; 4 = 2x2; P4 Exit game ->
"closed (pid exited)" -> 3; `kill -9` P3 -> "ended by signal 9" -> 2, pad
freed, picker cancels with B; quit closes P2 within 20 ms; the X server's
rects match every tile; hello ~240 ms after spawn, first heartbeat ~450 ms.
**Two** (`work/l3-two*`, gl `l3-two-gl-c2-game.jpg`): picker -> Ada -> own
window playing its own q3dm1 match (61 `AA P1` aim lines, Window volume
row); coordinator keeps playing q3dm7; the child wrote only
`profiles/ada.cfg` + `baseq3/profiles/ada.cfg`, its archived test value
never reached q3config. **Coordinator killed** (`l3-ckill-kill.txt`): 3
processes -> 0 within 250 ms (children log "Signal caught (15)" = parent
death signal). **Hang clock** (`l3-hb*`): 25 s "loading" stall survived
("heard again after 25040 ms"), silent stall closed at 15008 ms (SIGTERM,
re-tile to 1); pad unplug/heal/return as on Windows. **In-place mode
switch** (`l3-mode.log`, `l3-mode-gl.log`, `-host-*.jpg`): Together on
wayland -> row to Independent -> "video driver wayland -> x11", window
recreated on x11, re-tile; back to Together is "pending" while P2's window
is open. **Fake Gamescope** (`GAMESCOPE_WAYLAND_DISPLAY=x`,
`l3-gamescope*`): "--independent is unavailable under Gamescope (Deck game
mode): Together", row note "Unavailable under Gamescope (Deck game mode)",
join hold opens no window, SDL stays on wayland. **Regression**
(`l3-r10single.log`): a plain start stays on wayland, no indep lines.
**Focus:** every `--child`/`--noactivate` window logged "does not have the
input focus"; `_NET_ACTIVE_WINDOW` never changed during tiles/two/hb/ckill;
the method does detect focus (the mode test's coordinator, started without
`--noactivate`, showed `_NET_WM_STATE_FOCUSED` and its child did not take
it). Code: `unix_splitproc.c` (669 lines: spawn splits the one-string
command line at spaces outside quotes keeping the quotes so the child's
`main()` rebuilds the Windows-style line; child closes inherited fds,
stdin from /dev/null, restores SIGINT/mask, `waitpid(WNOHANG)`, SIGTERM
2 s then SIGKILL, UDP 1:1, `Sys_SplitUnavailable()`); `cl_splitindep.c`
stubs are now weak symbols (macOS/BSD keep them), unavailable -> Together
in `CL_IndepInit`/`Indep_ModeFrame` without touching the archived cvar,
`CL_IndepAreaChanged()`; `sdl_glimp.c` +33 (never exclusive fullscreen in
this mode, wayland->x11 switch with the GL context deleted first,
borderless window at the tile created hidden then shown, no pointer
warp); `sdl_input.c` no `vid_xpos/ypos` save for tiled windows;
`unix_main.c` calls `Sys_SplitParseFlags`; Makefile (non-MINGW SDL
client list only) + CMake. No Windows files changed (**Windows compile
check still needed on the Windows dev box**: `cl_splitindep.c`/`cl_splitscreen.h`
changed). Differences from Windows: parent-death signal + pid watch
instead of a job object; tiles in the work area not the whole monitor;
`Sys_SplitAllowFocus` is a no-op (X11 has none); `indepfocus` untested.
Test-data quirks: no `q3key` in `~/dev/q3data` so each child's UI opens on
the CD KEY page (`l3-child2.cfg` presses A twice); the stick-timed route
to FIGHT misses on a 1280x335 tile, so the child's match starts from the
console. Packaging (lead): `scripts/package.sh`, `scripts/linux/
quake3e-splitscreen.sh`, `scripts/linux/install.sh` (dry-run: install,
reinstall, foreign exe refused, no paks refused) ->
`build/quake3e-splitscreen-0.0.0.15-linux-x86_64.tar.gz` (1.7 MB).

**L2 (Linux headless suite, Linux test box, 2026-10-06; Opus executor, commit
`40422d7a`; VERSION 0.0.0.14).** Runner `scripts/tests/linux-run.sh`, all
on the L1 build, `in_gamepad 0` + virtual pads, each run quit by its cfg,
one at a time. **No Linux-specific bug; no engine code changed.**
`r8-join` q3dm1 (`work/l2-r8-join.log`, `-j-*.jpg`): Guest/Bob/Cleo
picker, keyboard "Ad_" -> Ada, Guest look speeds 112.5/180, P4 500 ->
drop -> 320 again, Ada pre-highlighted on rejoin -- as Windows.
`r10-layouts` q3dm7 vk + gl (`work/l2-layouts-{vk,gl}*`, 16 shots each):
2x2, 5 fill 3+2, 5 wide P3, 5 grid, 3x2, 7 fill 4+3, 7 grid, 8 grid, 8
fill 4x2, map_restart / vid_restart / drop+rejoin at 8, down to 1,
devmap q3dm17; only P1's cgame restarts at 1<->2; cell lists identical
vk/gl. `r12-aim` q3dm7 (`work/l2-r12-aim.log`, `-table.txt`; clean-profile
rerun byte-identical): sweep 61.25, Low 49.66/55.67 slow 0.811, Standard
37.92/49.52 slow 0.619, B 0/0, E flick 229 ms, F 20 traces 0 targets --
same as Windows; **C/D mean target angular velocity differs** (C Low
-23.75 vs -30.0, Standard -26.67 vs -28.8; D Low -22.59 vs -19.6, Standard
-20.75 vs -22.5) with the rot/omega shares inside each level's cap (Low
0.149/0.150, Standard 0.307/0.284), i.e. P2's scripted strafe moves
differently per frame (Linux `com_maxfps 60` frames: 16 ms x743, 17 x235,
14 x119), not the assist -- open item below. `r10-perf` q3dm17 uncapped vk
+ gl (`work/l2-perf-*`): com_speeds all 1/4/8 views vk 0.00/0.46/1.94 ms,
gl 0.00/0.52/2.01 ms (Windows R10 0.02/0.82/1.99 and 1.27/1.90/3.29);
cgame fps 1000 at 1 and 4 views (engine's 1 ms floor; stock cgame shows
"Connection Interrupted" at that rate, also with P1 alone -- not ours), 444
at 8. `r10-pads` q3dm7 vk (`work/l2-pads-vk*`): 8 virtual pads join (96 ms
connect / ~656 ms from slot), P8 picker in its future cell, profile "b",
unplug/replug takes over P6, drop/rejoin P4, mod menu in P7's 320x360
cell, 3x3 menus/keyboard in 426x240 cells (footer cut after "L3" as on
Windows). Script changes only: `linux-run.sh` strips `r8-` shot prefixes;
`r10-perf.cfg` gained 1- and 4-view screenshots + Linux header line.
Not run on gl: r8-join, r12-aim, r10-pads.

**L1 (Linux, Linux test box, 2026-10-06; Fable lead executing directly, Sonnet
review; VERSION 0.0.0.13; branch `linux`).** Builds and starts; booting a
map waits for the paks. (1) `q3dev` distrobox created (Fedora 43 + gcc 15.3,
SDL2-devel 2.32.72, X11/GL/Vulkan/ogg/vorbis/curl -devel;
`work/q3dev-setup.log`). (2) Upstream `Makefile` built our tree unchanged
on the first try (`work/l1-make-first.log`: 7 s at -j24, 0 warnings in our
files, 2 upstream libvorbis `-Wmaybe-uninitialized`); the `Sys_Split*`
stubs under `#ifndef _WIN32` in `cl_splitindep.c` already covered the
platform layer. (3) Port fixes: `code/sdl/sdl_input.c` calls
`IN_GamepadFrame()` from `IN_Frame()` (nothing called it on SDL builds, so
pads were dead) and `IN_InitJoystick` / `IN_ShutdownJoystick` quit only
the SDL subsystems they started themselves (`joyOwnJoystick` /
`joyOwnGameController`): `IN_GamepadInit` runs before the renderer's
`IN_Init`, so upstream's `SDL_WasInit` guards took no references yet it
quit the gamecontroller subsystem at init (`in_joystick 0`) and at every
`vid_restart`, which would have torn it down under `in_gamepad.c` (review
finding; verified at runtime: `work/l2-padlist.log` lists the built-in
pad before and after a `vid_restart`). Known
limitation: `in_gamepad.c` sets joystick/controller events to
`SDL_IGNORE`, so upstream's `in_joystick 1` single-pad path loses its
hot-plug re-init while `in_gamepad` is on (superseded by it);
`in_gamepad.c` flushes only joystick/controller events (0x600..0x6FF, it
flushed the whole SDL queue = the engine's keyboard/mouse/window events on
SDL builds) and names `libSDL2-2.0.so.0` in its not-found message. (4)
`Makefile`: `SPLITSCREEN_VERSION=x` -> `Q3E_SPLITSCREEN_VERSION` (quoted
like upstream's `RENDERER_PREFIX`); `scripts/build.sh` re-enters the box,
deletes the objects that embed `Q3_VERSION` so the stamp follows
`VERSION`, writes `BUILD-INFO.txt` (`work/l1-make.log`). Banner: `Q3 1.32e
splitscreen-0.0.0.13 linux-x86_64`. (5) Host run without data on the
desktop display (`work/l1-boot-vk-nodata.txt`): clean `Sys_Error` at the
pak check, no process left. Design 14.5's "no dlopen of its own" was not
done literally: `in_gamepad.c` still dlopens `libSDL2-2.0.so.0`, which is
the engine's already-loaded library (same handle, refcounted subsystems),
so one SDL is in use; the hints it sets (`SDL_JOYSTICK_THREAD`,
`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, `SDL_NO_SIGNAL_HANDLERS`) are set
before the engine's `SDL_Init(VIDEO)` (CL_InitSplitscreen runs first), so
they apply to the engine's SDL too -- to be checked in L2 with real pads
(`in_joystick 1`, upstream's single-pad path, is superseded by `in_gamepad`
on Linux).

**Round 13b (fix-only: five review findings on Independent mode; Opus
executor; VERSION stays 0.0.0.12).** Commit `57da47df`, then docs. Release
build 0 compiler warnings, all three exes (`work/r13b-build.log`).
(1) Child command line: `sys_cmdline` is `MAX_CMDLINE_CHARS` 8192 (was
1024; only consumers are `Sys_SplitParseFlags`, `Com_EarlyParseCmdLine`,
`Com_Init`), `MAX_CONSOLE_LINES` 64 (both now in `qcommon.h`);
`CL_IndepSpawn` refuses to start a window with a red console line when its
line would exceed 8191 bytes / 64 `+` commands / has a stray quote; the
fs paths are passed only when not the default (typical line 790 bytes, 26
commands); `cl_splitChildArgs` no longer cut at 255; `Sys_SplitSpawn` uses
`GetModuleFileNameW` + `CreateProcessW` (args converted with `CP_ACP`, the
engine's path encoding -- a path outside the ANSI code page still fails
engine-wide, not just here). (2) Hang clock starts at the first `hb`; `hb`
now carries a loading flag (cls.state AUTHORIZING..PRIMED, or forced right
before a re-tile `vid_restart`) and is also sent from `SCR_UpdateScreen`
(loading screens); limit 120 s while loading / before the first hb, else
15 s; process death stays the main signal. Test hook `indepdebug stall <ms>
[loading]` (developer, child). (3) Socket open failure drops the request
(`cl_splitIndependent 0`), one message, no `vid_restart` loop (code-read
only: not inducible here). (4) Coordinator: a running window's pad that is
unplugged keeps the slot reserved (no `lostPad`, `Pad_HealTarget` skips
window slots); when it is plugged back it is that window's again
(`CL_IndepPadOwner`) -- before, it came back unassigned and could heal P1
or open a second window with a join hold. (5) `SetInformationJobObject`
checked (no job + warning on failure); `Com_SplitChildTag` 2..8.
**Verified (headless, virtual pads, 64,64 1280x720, `--noactivate`):**
`r13-two` -> `work/r13b-two*` (picker, Ada, child plays, 0 processes),
`r13-ckill` -> `work/r13b-ckill-taskkill.txt` (3 -> 0 within 1 s);
`r13b-hb` q3 (`work/r13b-hb.log`, `-child2.log`, `-c2-map.jpg`): child
loads q3dm12, then a 25 s "loading" stall -> "P2 heard again after 25024
ms of silence (it was loading)", not killed; then a 20 s silent stall ->
"silent for 15007 ms (heartbeat lost): closing it"; pad 1 unplugged ->
"P2's window handles it (slot kept)", pad 2 buttons heal nobody, pad 1 back
-> "back -- it plays in P2's window", join hold makes no window. Same on
**UrT** with the child loading ut4_turnpike (`work/r13b-urt*`). Item 1
(`scripts/tests/r13b-long.sh`, `work/r13b-long*`): 200-char homepath under
`work/`; a 7400-char `cl_splitChildArgs` -> "NOT started ... 8333 bytes";
65 commands -> "NOT started"; a normal child boots from the long homepath
with `net_port 27981`, `net_qport`, `logfile 4` (child log). `r10-single`
(`work/r13b-r10single.log`, `-devmap.jpg`): normal bordered 1280x720 window,
no indep line, no profiles. No nvlddmkm events during these runs. Runner:
`r13-run.sh` takes `R13_PREFIX`, `R13_BASE`, `R13_HOME`, `R13_GAMEDIR`.

Round 13 (Independent mode, design 17.2; Opus executor). Commits `942460b8`
(the mode), `deca107b` (vcxproj CRLF fix), `d43d0d2b` (overlay player numbers,
pid in `indepstatus`), `edf0d8fd` (tests), then docs. Release build, 0
compiler warnings (`work/r13-build-4.log`; the same 14 MSBuild
MSB8012/8028 notes), all three exes; the dedicated exe links none of it
(the shim is client-only; `win_main`/`win_syscon` hooks are `#ifndef
DEDICATED`). New files: `cl_splitindep.c` 996 lines, `win_splitproc.c` 403.
Hooks: `in_gamepad.c` +103/-4 (child filter, coordinator spawn path, inert
pads, hint), `cl_splitmenu.c` +69/-11 (Session mode row, Window volume row,
child own-window page, picker spawn/cancel, child profile limits, player
numbers), `cl_splitprofile.c` +39/-5 (child never writes shared files;
profiles in use in other windows), `cl_splitscreen.c` +31, `.h` +53;
upstream files: `cl_main.c` +6 (`CL_GetModeInfo`), `snd_mix.c` +2,
`common.c` +28/-1 (log name, no q3config write, `Com_SplitChildTag`),
`files.c` +2/-1 (pk3cache name), `qcommon.h` +1, `win_glimp.c` +19/-12 (no
exclusive fullscreen / borderless / x,y from the hook; show without
activation, symmetric for VK and GL — one file), `win_wndproc.c` +2/-1,
`win_syscon.c` +8/-1, `win_main.c` +3, `win_local.h` +3; Makefile, CMake
(client-only list), vcxproj + filters.

**Verified (headless; virtual pads; every window inside 64,64 1280x720,
`--noactivate`; no desktop screenshots):**
- **2 windows** (`work/r13-two.log`, `-child2.log`, Vulkan; GL:
  `r13-two-gl*`): coordinator on q3dm7 with 2 bots; pad 1 holds A ->
  picker in the coordinator (`r13-two-picker.jpg`, `-picker-ada.jpg`) ->
  Ada -> `indep: tiles (P2 joins) ... P1 64,64 1280x360 | P2 64,424
  1280x360`, child hello 2.3 s after start; each process logs its own
  `window: P<n> rect` (GetWindowRect) = its tile. Child: own main menu
  (`r13-two-c2-menu.jpg`), own-window page (`-c2-window.jpg`), pad-only
  Single Player -> FIGHT -> FIGHT into its own q3dm1 match vs Ranger
  (`-c2-game.jpg`; its `status`: `Ada ^3+` on loopback), aim assist on
  (`AA P1 lvl 1 ...` lines, marker `+`), pause page with **Window
  volume** and title "Player 2" (`r13-two-gl-c2-pause.jpg`). Coordinator
  kept playing: viewpos moved, `status` still on q3dm7 later
  (`-coord-play.jpg`, `-coord-late.jpg`); pad 1 inert in the coordinator
  once the window runs. Files (`r13-two-profiles.txt`): the child wrote
  only `profiles/ada.cfg` + `baseq3/profiles/ada.cfg`; `_padlast.cfg` by
  the coordinator; q3config watcher (`r13-two-q3config.txt`): mtime
  unchanged and the child's archived marker (`cl_splitMenuColor "0 1 0"`)
  never in q3config during the child's whole life. Quit: child gone in
  ~150 ms.
- **3/4 windows** (`work/r13-tiles.log`, `-child2..4.log`,
  `r13-tiles-3win.jpg`, `-4win.jpg`): 3 = P1 64,64 640x360 | P2 704,64
  640x360 | P3 64,424 1280x360 (last joined wide); `cl_splitWidePlayer 2`
  -> P2 wide, P3 top right (children moved, logged by each); 4 = 2x2. P4
  picked **Exit game** on its own-window page (pad only,
  `r13-tiles-c4-exit.jpg`) -> `bye`, `closed (exited)`, re-tile to 3 (P3
  wide again), pad 3 freed. **taskkill /F P3** (`r13-tiles-taskkill.txt`)
  -> `P3 window closed (pid exited)` next frame, re-tile to 2 windows,
  pad 2 freed; pad 2 holds A again -> picker -> B: cancelled, no window.
  Quit -> P2 quit in 142 ms. `tasklist` after every run: 0 processes.
- **Coordinator killed** (`r13-ckill-taskkill.txt`): 3 processes before;
  1 s after `taskkill /F` of the coordinator: none (kill-on-close job).
- **Session mode in place** (`work/r13-mode.log`, Together start): row ->
  Independent at the main menu: switched at once (bordered 1308x799 ->
  borderless 1280x720); a window joined; row -> Together while it ran:
  `(switch pending)`; its Exit game -> re-tile -> switched to Together
  (bordered again); pad hold at the Together main menu: "join needs
  player 1 in a game" as before.
- **Regression:** r10-single (`work/r13-r10single.log`,
  `r13-r10single-devmap.jpg`): one keyboard player, normal bordered
  window, no `indep` line, no `profiles/`. r12-aim baseq3
  (`work/r13-r12aim.log`, `r13-r12aim-table.txt`): same numbers as R12
  within run noise (e.g. Low sweep min 49.66 vs 49.62, Standard 37.94 vs
  37.93), host page shows the new last row (`r13-r12aim-host.jpg`).
  r11-4p UrT (`work/r13-urt-r11-4p.log`, `r13-urt-r11-4p-*.jpg`): the
  identical 15-step TEST sequence, no ERR_DROP/Unknown/overflow.

**Incident:** the first re-tile design ("move the window + `vid_restart
fast`") made the Vulkan coordinator draw past its new height: NVIDIA Xid
13 "3D HEIGHT CT Violation", VK_ERROR_DEVICE_LOST and a **driver reset at
00:59:45 and 01:02:05** (System log, `nvlddmkm` 13/153; the screen may have
blinked for the maintainer). Replaced by a full `vid_restart`; no further driver
events in any later run (checked after each).

Earlier rounds' details: `git show d0aa2376:STATUS.md` (R12) and older.

## Next steps

0. **Linux / Steam Deck (M6) kicked off 2026-10-06 on the Linux test box** (GPD
   Win Mini 2025, Bazzite 43). The repo with full history is at
   `~/dev/Quake3e-splitscreen` there (branch `splitscreen` checked out,
   pushed from this box via remote `<linux-box>`; the Linux work goes on
   branch `linux` there and is fetched/merged here). The brief for the
   Linux orchestrator is `docs/LINUX-BOOTSTRAP.md`; its program rules are
   `docs/dev-CLAUDE.linux.md` (to be copied to `~/dev/CLAUDE.md` on that
   box). **The maintainer, before the Linux session can test anything:**
   - copy the retail paks to the Win Mini (the Windows dev box agent is not
     allowed to copy game data): from a Windows dev box shell
     `scp "<Quake3 install>\baseq3\pak[0-8].pk3" <linux-box>:dev/q3data/baseq3/`
   - on the Win Mini, in Konsole inside the desktop session (not SSH, the
     game needs a display): `cp ~/dev/Quake3e-splitscreen/docs/dev-CLAUDE.linux.md ~/dev/CLAUDE.md`,
     then `cd ~/dev/Quake3e-splitscreen && claude` and tell it to read
     `docs/LINUX-BOOTSTRAP.md` and start round L1.
   Windows rounds continue here independently; each one gets pushed to
   `<linux-box>` (`git push <linux-box> splitscreen`) so the Linux branch
   can merge it.
   **Queued after the maintainer's tests (2026-10-06):** (a) ~~merge `linux`~~
   **done R14a** (0.0.0.18; L5 configs verified on Windows; the L-9 exe
   renames to `quake3e-vulkan-ss.x64.exe` etc. and the `release.yml`
   Windows compile check were **not** done -- **done R15**, 0.0.0.20). (b) **Done R15:** Embed SDL2 in the Windows exes:
   The maintainer wants the release to be the three exes only, like upstream's.
   Build SDL2 as a static lib with VS2019 (SDL2 ships `VisualC/SDL.sln`; no
   CMake on this box), link it into the client exes, make `in_gamepad.c`
   bind the SDL functions directly on Windows instead of loading
   `SDL2.dll`; the dedicated exe stays SDL-free. (c) Fullscreen is the
   default: the test home config now archives `r_fullscreen 1`, `r_mode -2`
   (desktop resolution), `in_gamepad 1`, so the maintainer's launch line needs only
   the two paths, `logfile 3` and `in_padDebug 1`.
   **L1 done on the Linux test box (branch `linux`, 0.0.0.13): builds, starts,
   stops at the missing paks.** Still needed from the maintainer: the paks
   (scp line above). Then L1's boot evidence (Vulkan + OpenGL screenshot of
   q3dm1, `qconsole.log`) and L2 (pads through the engine's SDL on real
   pads, `padinject` suite: port `s6-run.sh`/`r10-run.sh`/`r12-run.sh`
   with an `EXE` variable and `pgrep`/`pkill`). Linux-only items for
   the maintainer's hands-on pass later (L2+): pads detected under Plasma/Wayland
   (SDL2 joystick thread + background events), `vid_restart` keeps pads,
   keyboard/mouse unaffected while pads are plugged.
   **Update (same day, after the maintainer granted SSH to the Windows dev box):** paks
   copied by the agent, L1 boot evidence done on both renderers, **L2 suite
   passed** (Last round). Open from L2: the r12-aim C/D omega gap -- rerun
   `r12-aim` on Windows and diff the per-frame `AA P1` lines / frame-dt
   histogram against `work/l2-r12-aim.log` (the Windows dev box, next Windows
   round); real built-in pad in game is the maintainer's check (below). **L3 done**
   (Independent mode on the desktop, x11 driver in that mode only; Last
   round). **The maintainer, Linux hands-on checklist (Win Mini desktop session,
   real pads; the Windows items 1-24, 32-46 apply unchanged with the
   launch line below):**
   ```
   cd ~/dev/Quake3e-splitscreen && build/release-linux-x86_64/quake3e-vulkan-ss.x64 +set fs_basepath ~/dev/q3data +set fs_homepath ~/dev/Quake3e-splitscreen/work/q3home +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set logfile 3 +set in_gamepad 1 +set in_padDebug 1
   ```
   (OpenGL: `quake3e-ss.x64`; add `--independent` before the first `+` for
   Independent mode).
   - L-1. Built-in pad (shows as "Xbox 360 Controller") + a USB/BT pad:
     both detected at the main menu (`padlist`), hold A joins, sticks and
     triggers feel as on Windows; `vid_restart` keeps the pads.
   - L-2. Keyboard/mouse keep working while pads are plugged (no eaten
     keys, mouse grab/release on console toggle).
   - L-3. `--independent`: hold A on the second pad -> picker -> a window
     at its tile; each window answers only its own pad; the Plasma panel
     does not overlap the tiles.
   - L-4. Click into a child window: keyboard/mouse follow it; click back.
   - L-5. Sharpness: the tiled windows render at XWayland's logical size
     (1280x720 here) and Plasma scales them up; try KDE System Settings ->
     Display -> "Legacy X11 apps: apply scaling themselves" and say which
     looks right.
   - L-6. Session mode row Together <-> Independent in game (short blink,
     windows re-tile).
   - L-7. Close the coordinator from the taskbar: all windows close.
   - L-8. **Game mode (L4 build, 0.0.0.16):** copy
     `build/release-linux-x86_64/quake3e-vulkan-ss.x64` (or unpack
     `build/quake3e-splitscreen-0.0.0.16-linux-x86_64.tar.gz`) into your
     `~/Games/quake3/quake3e/` beside its `baseq3/` (the agent did not touch
     that folder), then in Steam change the non-Steam shortcut's target from
     `~/Games/quake3/quake3e/quake3e-vulkan.x64` to
     `~/Games/quake3/quake3e/quake3e-vulkan-ss.x64` (start-in: that folder;
     not required, the engine uses its own folder). Launch under Gamescope:
     boots fullscreen at 1280x800, Steam Input pads join (hold A), 2-4
     player layouts readable at 1280x800 (`cl_splitFill`, wide player),
     Steam's on-screen keyboard vs ours on "New profile", suspend/resume,
     Session mode row reads "Unavailable under Gamescope (Deck game mode)".
     Note: its configs/profiles go to `~/.q3a/baseq3/` (the default
     homepath), shared with upstream's exe there.
   - L-9. **Done R15 (0.0.0.20)** except the CI run itself (L-10); the
     SDL2.dll pin is gone (SDL2 is linked in; the source zip's sha256 is
     pinned in `scripts\build.ps1`). Original item: rename the
     Windows outputs in `scripts/build.ps1` to `quake3e-vulkan-ss.x64.exe`,
     `quake3e-ss.x64.exe`, `quake3e-ss.ded.x64.exe` (matching Linux and
     `release.yml`; L4 left build.ps1 alone) and update the Windows run
     lines/runners that name `quake3e-splitscreen.exe`; compile-check
     `release.yml`'s Windows msbuild sequence locally (upstream's
     `TargetName=quake3e` + `output\quake3e.exe` path with our props) --
     the `windows-x64` job is **unverified** until then; confirm the SDL2
     pin (2.32.10, zip sha256 `6cf9706e...e18fb`, dll `b37740a7...cacb4`)
     against `work/sdl2-PROVENANCE.md` there (the hashes were copied from
     it and the zip re-hashed from the Linux test box 2026-10-06: they match).
   - L-11. **Configs side by side (L5, 0.0.0.17), the maintainer:** drop
     `build/release-linux-x86_64/quake3e-vulkan-ss.x64` beside upstream's
     exe in `~/Games/quake3/quake3e/` (or unpack the 0.0.0.17 tar.gz
     there); first start prints `config: q3config-ss.cfg created from
     q3config.cfg` and `~/.q3a/baseq3/q3config-ss.cfg` appears with your
     binds/settings; change a setting in ours, start upstream's
     `quake3e-vulkan.x64`: it keeps its own; point the Steam shortcut at
     the `-ss` exe (L-8). Optional: `~/.q3a/baseq3/autoexec-ss.cfg` for
     -ss-only settings. **At the merge (lead, the Windows dev box):** verify the
     Windows build does the same with the exe-folder home path:
     `baseq3\q3config-ss.cfg` written next to the exe's `baseq3\`, seeded
     once from `baseq3\q3config.cfg`, `autoexec-ss.cfg` exec'd, upstream's
     `quake3e.exe` beside it unaffected; the dedicated exe writes
     `q3config_server-ss.cfg` (seeded from `q3config_server.cfg`). The
     Windows runners (`r11/r12/r13-run.sh`, `r11/r12-remote.sh`) already
     name the -ss files.
   - L-10. **First CI run** once the maintainer creates the GitHub repo: push the
     branch, run `release` by workflow_dispatch (artifacts only), check
     both archives' contents, then tag `v<VERSION>` for a real release
     (release numbers/timing are the maintainer's).
   - L-12. **Play check of the 0.0.0.27 release candidate on the Win Mini
     (L6), the maintainer:** the build is installed in
     `build/release-linux-x86_64/` (0.0.0.27, commit 882d0d38); launch with
     the Linux hands-on line above (desktop session; `--independent` before
     the first `+` for the Independent item). Check: (a) Together with 2,
     3 and 4 pads (built-in + USB/BT): hold A joins, layouts, each pad
     moves only its player; (b) menus: Splitscreen settings Cursor speed
     and Menu size rows, the Power-ups row (server options), Controls page
     values; in a team game (e.g. `+set g_gametype 3 +devmap q3dm7`) a
     spectator's pause page offers Join red / Join blue / Auto join; (c)
     Server options (P1's Start menu, above End game): change map, game
     type, bots, Power-ups Off/On -- on leaving, the game restarts as the
     page said; (d) Independent mode with 2 players: the two tiles look
     like Together's top/bottom cells (4:3 HUD in the middle, normal wide
     view, not stretched); (e) intro cinematic: start once with `+set
     com_introplayed 0` and skip it with any pad button (a stick must not
     skip); (f) first-launch hint: with `+set cl_splitSeenHint 0` and a
     pad connected, the main menu shows "Start a game first, then friends
     hold A to join" bottom right, gone after the first game. **UrT
     (R18/R20/R21: UrT detection, binds, zoom, downloads) is untested on
     Linux:** UrT is not on this box.
1. **Maintainer pad checklist (S4 + M2-T5 + S6 + R7 + R8 + R9 + R10 + R11 UrT 25-31 + R12 aim assist 32-37 + R13 Independent mode 38-46 + R14a 47-56 below).** Quit any game first. Your config now archives fullscreen at desktop resolution and pads on (`r_fullscreen 1`, `r_mode -2`, `in_gamepad` default 1), so the launch line needs only the paths and logging. **On your first start of this build** the console says `config: q3config-ss.cfg created from q3config.cfg`: from then on your settings live in `work\q3home\baseq3\q3config-ss.cfg` (`q3config.cfg` there is left as it is).
   Launch:
   ```
   build\Release\quake3e-vulkan-ss.x64.exe +set fs_basepath "<Quake3 install>" +set fs_homepath <repo>\work\q3home +set logfile 3 +set in_padDebug 1
   ```
   1. Main menu: press any button on pad 1 (it becomes P1, as Guest); d-pad
      moves, A opens, B goes back; right stick moves the cursor, RT clicks.
      Start: Splitscreen settings full screen; B closes. Cursor speed OK?
   2. With the pad only: Single Player -> FIGHT -> FIGHT; play a bit.
   3. Idle connected pads that nobody touches: **no** join hint. Touch an
      unjoined pad -> "Player 2: hold A to join", gone after ~10 s.
   4. Hold A ~0.7 s on pad 2: its quarter shows **"who's playing?"**
      (Guest / New profile...) before it joins; a short tap must not do
      anything. Press **B**: the quarter disappears. Hold A again, pick
      **New profile...**: type your name on the keyboard (d-pad + A, X
      deletes, Y space, L3 shift, Start done) -> you join under that name.
   5. Pads 3 and 4 hold A -> pick **Guest** -> they join as Player 3/4.
   6. Each player: move, look, jump (A), crouch (B), fire (RT), zoom (LT),
      weapons (LB/RB, d-pad left/right), scores (Back).
   7. **Aim feel:** each player Start -> Controls -> Aim curve, Turn boost,
      Look speed zoomed, deadzones, look speeds, invert. Only that player's
      aim may change. Note values you like.
   8. P2 Start: menu only in P2's view; Game menu -> the mod's menu there;
      B at its top returns to play. In the mod menu go SETUP -> PLAYER,
      put the cursor (right stick) on the name and press RT, then **hold Y**:
      a keyboard appears at the bottom; type, Start closes it. Did the name
      change?
   9. P2 Controls -> Button bindings -> Fire -> press X: X fires for P2 only.
   10. P2 Start -> **Profile**: Rename (keyboard), Save as new, Switch
       profile (pick Guest, then your profile again: name on the scoreboard
       changes, buttons/aim swap, nothing keeps firing or moving), Delete a
       profile (try one in use by someone else: greyed).
   11. P1 Start -> Splitscreen settings -> **Guest defaults**: change look
       speed and a button. A Guest who joins after that gets them; a Guest
       already playing does not. Also try Screen use, Wide view, Two
       players, HUD shape there.
   12. P2 Leave game -> confirm -> only P2 leaves; hold A again -> P2's
       picker has **your profile pre-highlighted**: one A press rejoins.
       P3 holds Back+Start 2 s -> leaves; as a Guest, its changes are gone
       when it rejoins.
   13. Unplug a pad while its picker or keyboard is up: the join is
       cancelled / the menu closes; plug it back and hold A: fine.
   14. Quit, relaunch, join with the same pad (any player number): your
       profile is highlighted; your look speed, buttons and model are back.
   15. P1 Start -> End game -> main menu; EXIT -> YES with the pad.
   16. **R9:** with 1 player, hold A on pad 2: the "who's playing?" list
       appears over the bottom half while your view stays full screen;
       press B: nothing else moves. Hold A again and pick Guest: only now
       the screen splits.
   17. Join and leave with pads 2-4 many times (20+) on one map: joins must
       keep working (before R9 they stopped after ~30 changes).
   18. P2 in its pause menu or the game menu: P1 sees a chat icon over P2's
       head (look at P2).
   19. P2 Leave game: "Player 2 left" shows in P2's area for a few seconds.
   20. **R10 (5-8 players; as many pads as you have, the rest from the
       console).** Same launch line; on a level, join pads 2..N by holding A.
       For missing pads, open the console and type `addplayer` once per
       extra player (they join as Guests). Expect: 5 = 3 on top + 2 wide
       below, 6 = 3x2, 7 = 4 on top + 3 below, 8 = 4x2. Each join (2 to 8)
       must **not** freeze or flicker anybody else's view (only going from 1
       to 2 players does). For 8 players at once from the console:
       `addplayer; addplayer; addplayer; addplayer; addplayer; addplayer; addplayer`.
   21. With 6-8 players: P1 Start -> Splitscreen settings -> Screen use off
       (3x3 grid with black cells) and back on; Wide view = a player number
       (5 or 7 players): that player moves to the wide bottom row.
   22. In a small (eighth) cell: Start menu, Controls, the "who's playing?"
       list and the keyboard -- readable from your couch distance?
   23. A big fight with 8 views (`addbot` a few bots): every view keeps its
       weapon, HUD head and rocket/plasma lights. Smooth?
   24. **Listen:** with 5-8 players and lots of firing, do sounds cut out or
       pile up (the mixer has 96 channels; they fill up in an all-out fight)?
   Report back: `work\q3home\baseq3\qconsole.log`, which pads were real vs
   Sunshine/Parsec virtual, hint behaviour, cursor speed, preferred aim
   values, anything hard to read (keyboard size in a quarter, red contrast),
   any button that did the wrong thing. Afterwards your profiles are in
   `work\q3home\profiles` -- keep them or delete the folder.

   **Online (remote server) -- what to expect.** P1 connects as usual
   (server browser or `connect`); the others join with the A hold as at
   home (R11: this now works on a remote server too); the join hint then
   has a second line "online servers may limit extra players". They join
   one after another (a few seconds each). Many servers allow only 3
   players from one address (`sv_maxclientsPerIP`, Quake3e default 3; every
   server picks its own number and the game cannot know it in advance): the
   4th then sees **"Player 4 could not join: this server allows only 3
   players from one connection"** in its part of the screen for 10 s, P1
   sees "Player 4 could not join this server" at the top of its view, and
   the others keep playing. A full server, a banned address, a wrong
   password or a mod's own check shows "Player N could not join: <the
   server's words>". Urban Terror servers that require auth will refuse
   the extra players (only one account per login) with their own message.
   Each extra player is an ordinary player to the server and its admins
   (own name, own GUID); this is not a way around server limits. Kicks and
   timeouts show their reason in that player's area; when the server shuts
   down or changes map everyone goes with player 1.

   **Urban Terror (R11).** Quit any game first. Launch (your UrT folder is
   only read; settings/logs go to `work\urthome`):
   ```
   build\Release\quake3e-vulkan-ss.x64.exe +set fs_basepath "<UrbanTerror43 install>" +set fs_homepath <repo>\work\urthome +set fs_basegame q3ut4 +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set logfile 3 +set in_gamepad 1 +set in_padDebug 1 +set com_hunkMegs 1024 +set bot_enable 1
   ```
   25. Main menu: set your nickname, start a local game (Create Server, or
       console `devmap ut4_casa`). Pad 1 = P1 (any button).
   26. Pads 2-4 hold A -> Guest: each player's **team menu opens in their
       own quarter**. Move each cursor with the right stick, RT on Red/Blue:
       only that player changes team. The gear menu follows in the same
       quarter: pick a different primary per player, Close.
   27. After the warm-up each player holds the weapon they picked; Back
       (scores) shows everyone on the team they chose.
   28. Buttons per player: RT fire, LT zoom (each pull = next zoom step), A
       jump, B crouch, X reload, Y use/pick up, LB/RB weapons, L3 (hold)
       sprint, R3 bandage, d-pad left "Affirmative", up "Requesting backup",
       right "Enemy spotted" (radio), d-pad down fire mode, Back scores,
       Start pause menu. Do they feel right? (UrT's own radio *menu*,
       keyboard `u`, takes number keys only -- with the keyboard does `u`
       then `1` work for you on this engine? It did not respond in tests.)
   29. `addbot boa 3 red`, `addbot cheetah 3 blue` (console): bots play.
   30. 8 players (`addplayer` x4 more in the console): all fit with the
       launch line's 1024 MB; at UrT's own 800 only 7 fit (the 8th is
       refused with a message naming `com_hunkMegs`).
   31. Your Quake 3 profile in UrT: profiles live in each homepath, so this
       needs one homepath for both (skip unless you want to try: launch Q3
       with `fs_homepath ...\work\urthome`, make a profile, then UrT).
   **Aim assist (R12) -- items 32-37.** Quake 3 launch line above (add
   `+devmap q3dm7` or start a match from the menu); a few bots: console
   `addbot sarge 2` `addbot grunt 2` `addbot visor 2`. Every pad player starts
   on **Low** (Start -> Controls -> **Aim assist**: Off / Low / Standard).
   32. Fight bots with Aim assist **Off**, then **Low**, then **Standard**.
       Low should feel like "the stick gets a little heavier over an enemy"
       and a small pull while you strafe-track one; Standard clearly more,
       never a snap. Let go of both sticks over an enemy: your view must not
       move at all. A fast flick across an enemy must not stick.
   33. Your name gets a ` +` (yellow in Quake 3) on the scoreboard (Back) and
       in kill messages while your assist is on; switch to Off: it goes. A
       small yellow `+` sits in the top right corner of your own view.
   34. P1 Start -> Splitscreen settings -> **Allow aim assist (local games)**
       Off: everyone's Controls row turns grey ("host: not allowed"), the
       `+` marks go away; On again restores them.
   35. Online (any server not started on this PC): the Controls row reads
       "Aim assist - local games only" in grey and nothing helps your aim;
       no `+` on your name.
   36. Urban Terror (launch line above, `addbot boa 3 blue`): same as 32-33;
       there the marker is a plain `+` after the name (UrT removes spaces and
       colours). Teammates never pull your aim.
   37. Tell us: Low/Standard strength, bubble size (`joy_aimBubble`, 40
       units; `joy_aimBubbleMax` 8 deg), whether Low should be the default,
       and whether `+` is a good marker. All constants are console cvars
       (list in Current state) -- try e.g. `joy_aimSlowStandard 0.5` or
       `joy_aimRotStandard 0.3` live and note what you liked.
   **Independent mode (R13) -- items 38-46: every player gets their own
   game in their own window.** Quit any game first. Launch (borderless on
   your whole monitor; add `+set cl_splitIndepArea "0 0 1920 1080"` to use
   only part of it):
   ```
   build\Release\quake3e-vulkan-ss.x64.exe --independent +set fs_basepath "<Quake3 install>" +set fs_homepath <repo>\work\q3home +set logfile 3 +set in_gamepad 1 +set in_padDebug 1
   ```
   38. Pad 1: any button = player 1 (your window fills the screen,
       borderless). Pad 2 **holds A** at the main menu: the "who's
       playing?" list appears in the middle of your window; pick a profile
       or Guest -> a second window opens in the bottom half and yours
       moves to the top half (a short black blink in your window: the
       renderer restarts at the new size).
   39. In the new window, pad 2 alone: its own main menu, Single Player ->
       a match of its own, while you keep playing yours (start one from
       your menu, or `devmap q3dm7` in the console). Does pad 2 do
       *anything* in your window, or pad 1 in its window? (Must not.)
   40. Both windows' sound plays at the same time. Start -> **Window
       volume** in each window changes only that window.
   41. Pads 3 and 4 hold A: 3 windows = 2 on top + 1 wide below, 4 = 2x2.
       Splitscreen settings (your window) -> Wide view goes to Player 2:
       player 2's window takes the wide bottom slot (3 windows).
   42. A player leaves: in their window Start at the main menu -> **Exit
       game (close this window)**, or Back+Start held 2 s: the window
       closes, the others fill the screen again; that pad can hold A
       again for a new window.
   43. Click into one of the other windows and type in its console (~):
       keyboard and mouse belong to the window you clicked; pads keep
       working in all of them. Did any window ever grab the focus by
       itself when it opened or moved? (It should not.)
   44. A profile played in one window is greyed in the next pad's
       "who's playing?" list; Profile -> Switch in a player's own window
       offers only Guest and their own profile.
   45. Quit your window (main menu EXIT): every other window closes within
       a second or two; Task Manager shows no quake3e-vulkan-ss left.
   46. Splitscreen settings -> **Session mode** (last row): Together /
       Independent switches the mode right away when nobody else is
       playing (else it says when it will). Afterwards a normal start
       (without `--independent`) must be exactly as before.
   Tell us: does the blink on every join/leave bother you, should the
   windows run at full frame rate when not focused (now 60 fps, see Open
   questions), and is a real pad's ordinal right with two identical pads
   (each window must get its own pad: logs say `belongs to another
   window: not opened` for the others).
   Known gaps: no UrT auth (online UrT auth servers refuse extras); the
   radio menu by pad; a 10-minute bot soak over several map changes was
   not run.
   **R14a (0.0.0.18) -- items 47-56.** Same launch line (the Independent
   mode line above also works without the window flags).
   47. **Sound with pads:** the game has sound from the main menu on, with
       your pads plugged in; the log near the top says `sound: COM already
       initialised ...` and `Sound initialization successful`. In
       Independent mode every window has sound.
   48. **Player 1 as Guest:** your pad claims P1 -> the scoreboard shows
       **Player 1** (default model), not "rebelancap". Switch P1 to your
       profile: your profile's name; back to Guest: Player 1. Quit:
       `q3config-ss.cfg` still has `seta name "rebelancap"`.
   49. **Aim assist proof in the log:** on a local match every pad player
       gets a line `AA P1: Low (Guest)` / `AA P2: Standard (profile
       "charlie")`, and a new one on every change (`AA P1: off (...)`,
       `off: not a local game ...` online).
   50. **Aim assist feel:** Off vs Low vs Standard against moving
       opponents (bots or each other): Low = the stick clearly heavier
       over an enemy plus a gentle follow while you strafe/track;
       Standard = obvious. Still: let go of both sticks over an enemy ->
       the view never moves; a fast flick never sticks. The `+` after your
       name: yellow = Low, green = Standard (corner glyph too). Tell us if
       Standard is now too strong (all numbers are cvars, Current state).
   51. **Menu size:** Start menus are 1.5x and readable at 4K from the couch
       (they used to be a third of the size at 4K); Splitscreen settings ->
       **Menu size** 1.00x .. 2.50x changes it live; long pages (Controls,
       Splitscreen settings) scroll with the selection, arrows at the edge.
   52. **Change player model:** Start -> Profile -> **Change player model**
       (last rows): Quake 3's player model page opens in your part of the
       screen; **LB / RB** turn its pages; pick a model, B back to the game.
       Check it for P1 and for P2.
   53. **Handicap** (same page, last row): set 90 -> scoreboard/HUD health
       cap follows (Quake 3: max health 90); quit, rejoin with that
       profile: still 90.
   54. **Controls page:** the deadzone row now reads "Outer deadzone: full
       speed at 95 %"; new **Field of view** (80-130, default 90) per
       player: P2 at 110 sees wider, P1 unchanged. Note: P1 on a pad now
       uses this FOV too (default 90), not your autoexec's `cg_fov 115`:
       set 115 in your profile / Guest defaults; keyboard/mouse P1 keeps
       115, and quitting puts 115 back in the config.
   55. **Cursor speed:** in Quake 3's own menus the red crosshair cursor is
       25 % faster (`cl_padModCursorScale`); our menus' dot unchanged.
   56. **Server browser:** Multiplayer -> while "Press SPACE to stop"
       shows, **X** or **Y** stops the refresh (P1, an extra player's
       menu, and an Independent-mode window).
   **R14b (0.0.0.19) -- items 57-68: Server options.** Same launch line;
   start a local game (Single Player / Start Server, or `devmap q3dm7`).
   57. Start -> **Server options** (above End game). Every row shows a
       one-line explanation at the bottom when selected; the page scrolls.
       On someone else's server the row reads "Server options - host only"
       and is greyed. (Keyboard: console `serveroptions`.)
   58. **Change map:** the list starts with the current map "(now)", then
       all maps A-Z; the picture of the highlighted map is on the right
       ("no picture" box when a map has none). Pick one: the page says "On
       leaving: loads <map>"; B out: the map loads once, everybody stays.
   59. **Game type** + **Frag limit / Capture limit / Time limit**: change
       the type -> "On leaving: reloads the map (game type)"; limits apply
       at once (capture games show Capture limit).
   60. **Instagib On**, leave: the map restarts once; only railguns lie
       around, no armor/health/ammo; everybody spawns with a railgun and
       100 health; one hit kills. Weapons / spawn / ammo rows grey while on.
   61. **Weapons:** Rockets only (or any other) -> after the restart every
       weapon pickup is that weapon and you spawn holding it; **Random** ->
       a mix that changes at every restart; Default -> the map's own.
   62. **Player weapons at spawn** All / Gauntlet only, **Infinite ammo**
       On: take effect at your next spawn / at once.
   63. **Bots** 1..16 join within seconds (names from the game) and stay
       across map changes; **Bot difficulty** brings them back at the new
       level; Off removes them. (Bots added from Quake 3's own Start
       Server menu are left alone while this row is Off.)
   64. **Self-damage Off:** rocket-jump: you fly, your health does not drop.
   65. **God mode** (Player N / All players): that player cannot be hurt,
       also after dying and respawning; the page says cheats are on.
       Turning it off puts cheats off again right away (once nobody has
       god mode; 0.0.0.20; a `devmap` game keeps its own cheats).
   66. **Player health** 50: everybody's max health is 50 at the next spawn
       (and, as Quake 3's handicap does, they deal half damage); Default
       gives each player their own handicap back.
   67. **Low gravity**, **Friendly fire**, **Game volume** (up to 150 %:
       listen for crackle/distortion above 100 %, tell us).
   68. **Save server settings...** -> type a name on the keyboard; change
       something; **Select server settings** -> the name: the values come
       back (title shows the set's name); **Delete** removes it. In Urban
       Terror: Instagib / Weapons / Spawn / Ammo / Self-damage / God are
       greyed; map change (e.g. ut4_turnpike) and bots work.
   **R15 (0.0.0.20): no SDL2.dll, new exe names.** The launch lines above
   now start `build\Release\quake3e-vulkan-ss.x64.exe` (OpenGL:
   `quake3e-ss.x64.exe`; dedicated: `quake3e-ss.ded.x64.exe`); update any
   shortcut that pointed at `quake3e-splitscreen.exe`.
   69. Copy **only** `build\Release\quake3e-vulkan-ss.x64.exe` into an
       empty folder (no `SDL2.dll` anywhere near it) and start it from
       there with the launch line's `+set` arguments: pads are detected
       (`padlist` lists them; the console says `gamepad: SDL 2.32.10 static
       (built in)`), P1 on pad 1, hold A on pad 2 joins; sound plays.
   70. The same with `quake3e-ss.x64.exe` (OpenGL).
   71. `--independent` from that folder: a joining pad's window opens
       (children start from the same exe), its pad works there.
   72. Your 4 pads together (XInput + PlayStation/Switch if you have them):
       all detected, same as with the old `SDL2.dll` build.
   **R16 (0.0.0.21): The maintainer's R14-R15 feedback.**
   73. Controls page (Start -> Controls): the deadzone row reads just
       **Outer deadzone**; its help line explains it.
   74. Server options: after Low gravity come **Quad damage**, **Player
       speed**, **Weapon respawn**. Player speed 150 %: you run visibly
       faster at once; 100 % back to normal. Weapon respawn 1 s: a taken
       weapon comes back after a second. Quad damage Off -> leave the page
       (one restart): no quad on q3dm7 (or any quad map); On again brings
       it back.
   75. Change several rows (time/frag limit, gravity, bots, game type,
       volume 50 %, the three new rows), then **Reset to defaults**: every
       row shows its default, game type Free for all, volume 100 %; the
       map stays (leaving reloads only if the game type changed).
   76. Instagib On (baseq3): Weapons, Player weapons at spawn, Infinite
       ammo and **Player health** are greyed; after leaving, everyone has
       railguns, one hit kills.
   77. Urban Terror: Server options -> Instagib says "Uses the game's own
       instagib"; On + leave: one restart into UrT's instagib mode; Quad
       damage / Player speed / Weapon respawn are greyed. (OSP: the same
       with OSP's own instagib, "Instagib ENABLED!" in the console.)
   78. Bots 20 (q3dm7 or bigger): one restart, 20 bots arrive one by one.
   79. **Independent mode taskbar:** `--independent`, two pads in (two
       windows): the taskbar is hidden while either game window is the
       active one, and comes back when you click the desktop / alt-tab
       out. Switch to Together (host page Session mode): the taskbar
       behaves as before.
   80. **Joyxoff** (or whatever reads "fullscreen app running") stays
       quiet while you play in an Independent window. If the taskbar still
       shows or Joyxoff still pops up, say so: `cl_splitIndepMarkFullscreen
       0` turns the marking off (`window: P<n> marked fullscreen for the
       shell` in the console shows it was done).
   **R17 (0.0.0.22): your pass-2 list.**
   81. 3 players at your 4K: P1 Start -> Profile -> Change player model,
       then **LB / RB**: the page turns (the red cursor jumps onto the
       arrow for a moment, then the next/previous models show); same for
       P2 / P3. Press RB twice quickly: two pages.
   82. **Team Arena** (CTF or Team DM): everyone starts as a spectator.
       Each player: Start -> **Join red** / **Join blue** / **Auto join**
       right under Resume -> on that team. On a team, Start shows
       **Spectate** above Leave game (the game allows one switch per 5 s).
       Same in Quake 3 Team DM / CTF.
   83. Server options -> **Power-ups Off** (above Quad damage): Quad damage
       turns grey; leaving restarts the map; no quad / haste / invisibility
       / regen / battle suit / flight (TA: scout / guard / doubler / ammo
       regen) and no holdable items (teleporter, medkit, kamikaze ...) on
       the map. On again brings them back. Urban Terror: the row is grey.
   84. Controls -> **Cursor speed** (between Movement and Field of view):
       200 % makes both the red cursor in the game's menus (Start -> Game
       menu) and our menus' white dot twice as fast, 50 % half; it is kept
       in your profile. At 100 % the red cursor should feel a bit faster
       than before (1.4x vs 1.25x).
   85. Aim assist **Low** should feel a little stickier than before;
       Standard unchanged. A **new profile** starts with aim assist Off.
       (Your Guest defaults say Standard -- you set that -- so Guests start
       Standard until you set Splitscreen settings -> Guest defaults -> Aim
       assist Off.)
   86. Splitscreen settings: **Menu size** 1.00x is the size you had,
       0.50x .. 1.50x; HUD shape reads **4:3 Centered**; Controls shows
       "95%" (no space).
   **R18 (0.0.0.23): Urban Terror.** Copy the three new exes into your
   UrbanTerror43 folder (beside `q3ut4\`) for 87-88.
   87. **Online:** join the UrT 4.3 server that kicked you ("Non whitelist
       client Q3 1.32e splitscreen"): you should stay in. The console shows
       `urt: connect userinfo to <server>: no "client" key ...`. If you are
       still kicked, note the exact message (and whether the server asks
       for auth: UrT login does not work with Quake3e). Then a second pad
       joins too (if the server allows 2 per address).
   88. **Drop-in launch:** start `quake3e-vulkan-ss.x64.exe` from the
       UrbanTerror43 folder with **no** `+set fs_basegame`: UrT starts
       (console: `urt: detected q3ut4 install, fs_basegame q3ut4`; `meminfo`
       says 1024 MB hunk unless you set one). Your Quake 3 folder still
       starts Quake 3.
   89. **Scope:** 2 and 4 players, SR8 / PSG-1 / G36, LT to zoom: black all
       around the lens to the edges of your view, no zoomed picture beside
       a square. Also Two players: Side by side.
   90. **Menus are blue** in UrT (Start), red again in Quake 3.
   91. **Your layout** (a Guest, or Start -> Controls -> Button bindings ->
       Reset buttons to defaults on your old UrT profile; an untouched old
       UrT profile switches by itself): A jump, B crouch, X reload, Y
       bandage, d-pad up drop item, down drop weapon, left IR vision
       (needs NVGs in your gear), right weapon mode, R3 knife, LB reset
       zoom, RB next weapon, L3 sprint, LT zoom, RT fire, Back scores.
   92. **Crouch toggle:** B once = crouched until B again. **Sprint
       toggle:** push forward, click L3 = sprint without holding; stops
       when you click L3 again or let go of the stick. Controls -> Crouch /
       Sprint -> Hold: both work held instead. Quake 3 still holds crouch.
   93. **Buttons page** (Controls -> Button bindings) in UrT: every UrT
       action listed (interact, zoom out, previous weapon, weapon slots,
       items, team / gear / radio menus, radio calls, chat, votes, ...),
       unbound ones show `--`; pick one, press a button: bound (the
       button's old action swaps over).
   94. Two players in UrT, each with their own profile: P2 changes Sprint
       to Hold, P1 keeps Toggle; after a restart each profile still has its
       own choice.
   95. RB = **next** weapon as you asked (`weapnext`); UrT's own mouse
       wheel down is `weapprev` -- say if you meant UrT's wheel-down
       direction instead.
   **R19 (0.0.0.24): Independent mode (experimental) -- your 3-pad test
   again.** Launch with `--independent` as in item 38's line plus
   `+set in_padDebug 1`; after each run send `qconsole.log` and every
   `qconsole-child<N>.log` from `work\q3home\baseq3`.
   96. **3 pads, Joyxoff closed:** P1 presses A (P1 = the main window);
       the second and third pads hold A, pick a profile: two more windows.
       Each pad moves only its own window (P1 never moves P2's). The
       main window's console shows one `indep: pad <key> -> P<n>` line per
       pad, and for the 2C possibly `... duplicates pad ...: one pad
       listed twice`; no "press A to join" for a pad that already plays.
       The Session mode row reads **Independent (experimental)** with
       "Separate windows per player; still being made reliable" below.
   97. **The same with Joyxoff running:** click into P2's window, then
       P1's; does Joyxoff still act on the pads? The main window's log
       has `indep: shell <state> ...; foreground ...` lines -- send them.
       If Joyxoff still turns pads into mouse/keyboard, turn its bindings
       off for this mode (a tile is never screen-sized, so its check
       cannot pass).
   98. **Late join:** with two windows running, switch on (or plug in) the
       third pad: it holds A -> exactly one new window, no second picker.
   99. **Reconnect:** switch P2's pad off and on again (or unplug and
       replug its receiver): the main window logs `... went away ...` and
       `-> P2 (...): reconnected`; P2's window is controlled by it again,
       no join prompt for it.
   100. Behind the windows the screen is black (backdrop); clicking it
       does nothing; the taskbar stays hidden while a tile has focus.
   **R20 (0.0.0.26): your third pass.** Send `qconsole.log` (and every
   `qconsole-child<N>.log` for Independent mode) after each run.
   101. **Independent, 2 players top/bottom** (your screenshot's setup):
       both tiles look like Together's 2-player top/bottom views -- HUD
       numbers normal width, the same field of view as Together (much more
       visible above and below than in 0.0.0.25). Each window's log has
       `window: tile <W>x<H> is a cell: cgame screen 640x480 (HUD shape
       4:3 centered)`. Behind/around the tiles there is no black backdrop
       any more (item 100 is obsolete); the taskbar still hides behind a
       focused tile. Joyxoff: turn its bindings off for this mode.
   102. **Single Player arena with 2 players:** Single Player -> pick an
       arena -> Fight; P2 holds A: P2 joins the arena (no endless "Player
       2 connecting..."); both play against the arena's bots. Tell me what
       happens at the end of the arena (podium / next arena) with two
       people -- untested here.
   103. **UrT online:** join a server running a map you do not have (e.g.
       the one with `ut4_asylum_b1`). The log shows at start `urt:
       cl_allowDownload 0 -> 1 ...` (first start only) and `download:
       cl_allowDownload 1; HTTP/FTP ... via libcurl ... (built in)`; when
       joining: `URL: http://...` (the server's download site) or a UDP
       download, `<map>.pk3 downloaded`, then the map loads. Send the log
       if you get `couldn't load maps/...` again or a download error. The
       "not auth capable" line at start is normal; a server that requires
       a UrT account will still refuse you.
   104. **Intro video:** start the exe with no options (Quake 3: the id
       logo plays; UrT: its intro) and press any pad button (any pad, also
       one that has not joined): the video stops at the main menu and the
       press does nothing else (no menu move, no join). Sticks do not skip.
   105. **First-launch hint:** on the first start of 0.0.0.26 (per game
       folder) with a pad connected, the main menu shows "Start a game
       first, then friends hold A to join" at the bottom right; once you
       start any game it never shows again (`cl_splitSeenHint 1` in
       `q3config-ss.cfg`; set 0 to see it again).
   106. **UrT map download (0.0.0.27):** join 74.91.113.242 again (or any
       server on a map you do not have; your config still has
       `cl_allowdownload 0`, which no longer matters -- `cl_autodownload 1`
       does). Expected: the loading screen shows the download (name, MB/s,
       percent), then the game reconnects and you are in the map. The log
       must show, in this order: `fs_game: server's "q3ut4" is our base
       game, no game restart` (and **no** `RE_Shutdown( 2 )` /
       `logfile: closing` right after the connect), `download: server
       sv_allowDownload=0 sv_dlURL=74.91.113.242/maps; missing:
       q3ut4/<map>.pk3; method: http (server sv_dlURL, as UrT 4.3's
       client)`, `download: URL 74.91.113.242/maps/q3ut4/<map>.pk3`, then
       after the reconnect `missing: none`; the file lands in
       `work\urthome\q3ut4\download\` (or `UrbanTerror43\q3ut4\download\`
       when the exes sit in the UrT folder). The log now continues to the
       end of the session; send it if anything fails.
2a. **R14b follow-ups.** (a) the maintainer's checklist 57-68 above. (b)
   missionpack (layout 2: stats 3/4/7, weapons 11-13) is code-only,
   untested (no Team Arena run). (c) A bot overshoot by one in UrT right
   after a map load (added, then kicked by the count keeper) -- cosmetic.
   (d) Self-damage that kills still kills (the game decides death inside
   G_Damage). (e) The health field is found by watching STAT_HEALTH (~2 s
   after the first spawn); before that instagib players have 125 and
   self-damage restores armor only. (f) Change map lists at most 255 maps
   (overlay row cap). (g) ~~Sonnet review of R14b~~ done: two findings
   fixed in R15 (`c2aadb08`).
2. **R14a follow-ups.** (a) ~~L-9 exe renames~~ **done R15**; the
   `release.yml` Windows job now runs `scripts\build.ps1 -PlatformToolset
   v143` -- still **unverified on GitHub's runner** (VS2022 + v143 never
   built here; first CI run = L-10). (b) ~~Embed SDL2~~ **done R15**.
   (c) "Change player model" goes straight to the page only in baseq3's
   q3_ui-style menus (verified with the maintainer's `zzz-a51-ui.pk3` build);
   Team Arena / UrT / other mods get their game menu (the log line says
   so); it is greyed outside a match. (d) Rows were added at the end of
   the Profile page, but the Controls page gained **Field of view before
   "Button bindings"**: old scripts that reach Button bindings / Reset by a
   down-count from the top (`r7-overlay`, `r7/r8-persist-a`, `r13b-hb`)
   need one more step -- not re-run this round. (e) UrT not run this round
   (the cg_fov shadow and the marker change touch it: run `r11-2p`,
   `r12-aim` urt). (f) Sonnet review of the R14a commits.
2b. **R13 follow-ups (Independent mode, design 17.3).** (a) Real pads are
   the maintainer's (checklist 38-46): SDL opens the same XInput/HID device in two
   processes (the coordinator keeps it open but inert) -- confirm no
   double input and that identical pads map by ordinal. (b) Aim assist in a
   child that *joins the coordinator's server* over UDP is "remote" by the
   R12 gate (no assist); only its own local game gets assist -- extend the
   gate (spawned-by-us server + nonce) if the maintainer wants it. (c) Unfocused
   windows are capped by `com_maxfpsUnfocused` (60). (d) Picker is centred
   in the coordinator's window, not where the new window will go. (e)
   `indepfocus` untested (it takes the foreground). (f) Linux/Deck: see
   design 17.3 last bullet (fork/posix_spawn + PDEATHSIG, UDP as is; no
   window placement on Wayland/Gamescope).
3. Menus, deferred: keyboard/mouse cannot drive the overlay (pads only);
   UI `Key_SetBinding` for extras is a no-op (the mod's controls page shows
   "???" for P2 and cannot change anyone's binds -- verified); text typed
   into a mod field is inserted at that field's caret; vibration not
   implemented. (R11: an extra player's unclaimed console commands now reach
   its own ui -- `UI_CONSOLE_COMMAND` is no longer P1-only.)
   Aim assist, deferred (R12): no per-game `aimassist.cfg` (built-in table
   only; unknown mods get baseq3 conventions); UrT crouch/prone target
   heights not modelled (body axis -16..+26 above the origin); no strength
   change while zoomed/scoped; bots are targets but never use it; the
   marker can be cut by a mod name-length limit (not seen: UrT kept
   "New_UrT_Player+").
4. Remaining polish: per-viewport bloom; `CG_CIN_*` placement untested (no
   stock cgame plays cinematics); profile re-read on a mod change untested
   (upstream `game_restart` crashes on this box -- worth a look/upstream
   report); a `game_restart` while P1 plays a profile leaves the *old*
   mod's q3config with the profile's name (the engine writes q3config every
   frame an archived cvar changes; the crash-safety file restores it only
   into the mod that starts next); the on-screen keyboard's first footer
   line is cut in a 426-wide cell (3x3 grid); the 96-channel sound mixer
   fills up in an 8-player all-out fight (oldest sounds cut; raising
   `MAX_CHANNELS` is a one-line upstream change if the maintainer hears it);
   per-frame renderer arrays cost +2.6 MB hunk even with one player.
5. Pads: rumble; SDL backend (Linux/mac `sdl_input.c`) never calls
   `IN_GamepadFrame` yet.
6. M3 (UrT: done headless in R11; Maintainer checklist 25-31; bot soak and
   UrT auth deferred), M4 (online, verified against a Quake3e server; real
   public-server pass is the maintainer's), M5 (8 players: done headless; real pads
   5-8 are the maintainer's, checklist 20-24), M6 (Linux / Steam Deck, 14.5).

## Open questions

- **R21 (0.0.0.27) -- defaults taken:** (1) **No "download this map?"
  prompt:** UrT 4.3's client asks "press ANY key to download" before
  fetching; ours downloads right away (switch: `cl_autodownload 0` in
  UrT's settings). Default: no prompt. (2) **Downloaded maps go to
  `q3ut4/download/`** (UrT's folder) and that folder is now loaded in every
  UrT run, also UrT's own downloads in the install folder. Default: keep.
  (3) **Only the map's pak is downloaded** (as UrT); other missing paks a
  server lists are skipped and logged. Default: keep. (4) **No log by
  default:** `logfile` still defaults to 0; with `+set logfile 1..4` the log
  now covers the whole session (modes 1/2 truncate only at process start).
  Default: keep 0 (say if every -ss run should log).
- **R20 (0.0.0.26) -- defaults taken:** (1) **Hint wording:** "Start a game
  first, then friends hold A to join" -- "hold" (not "press") because joining
  is a hold (`cl_splitJoinHold` 700 ms); with `cl_splitJoinHold 0` it says
  "press". Shown on every game folder's first start of 0.0.0.26 even if you
  played before (the flag is new). Default: keep. (2) **SP arena slots:**
  `spmap` now always latches 15 slots (8 + up to 7 local players), also when
  you play alone; it changes nothing in the arena. Default: keep. (3)
  **UrT downloads** are turned on once even though UrT's `default.cfg` says
  off; `cl_allowDownload 0` afterwards is kept. Default: keep. (4) **Sticks
  do not skip** a cinematic (buttons, d-pad and triggers do). Default:
  keep. (5) **Tile = cell only with several windows:** a single
  Independent window covering the screen renders like one Together player
  (its own aspect, no 4:3 HUD). Default: keep.

- **R19 (0.0.0.24) -- defaults taken:** (1) **Duplicate rule** (one pad
  listed twice): two pads, at least one nobody's, with identical buttons /
  stick keys pressed within 100 ms are one pad; undone after 150 ms of
  different input. Two people who join in the same 100 ms with the same
  buttons: the second one presses anything else and holds A again.
  Default: keep. (2) **Only in Independent mode:** Together mode has the
  same duplicate listing (your log: the 2C joined as P3 and P4 in Together
  too, you cancelled P4) but was left untouched as asked. Default: leave
  Together alone; say if the rule should apply there too. (3) **Backdrop
  on by default** (`cl_splitIndepBackdrop 1`): a black window over the
  whole monitor behind the tiles. Default: on. (4) **Joyxoff:** a tool
  that compares the focused window with the screen cannot see a fullscreen
  game in this mode (a tile is smaller than the screen); the fallback is
  turning off Joyxoff's bindings (or excluding the game) for Independent
  mode. Default: that fallback; no further attempts unless your log (item
  97) shows `QUNS_BUSY` / `QUNS_RUNNING_D3D_FULL_SCREEN` and Joyxoff still
  misbehaves. (5) The help line wraps (two lines) for the Session mode row
  only; other long help lines stay cut as before. Default: keep. (6) With
  `--independent` the row still shows the archived value (Together) as in
  R13; the note under it says "(--independent)". Default: keep.
- **R18 (0.0.0.23) -- defaults taken:** (1) **RB = `weapnext`** (your
  words "next weapon / weapnext"); UrT's own wheel-down is `weapprev`.
  Default: weapnext. (2) **Interact / pick up** (`+button7`, was Y in R11)
  is unbound now (your layout has no slot for it); it is on the Buttons
  page. Default: unbound. (3) **Misc / Share** keeps "use current item"
  (`ut_itemuse`) from R11 -- not in your list but no conflict. Default:
  keep. (4) A **toggled sprint ends when the left stick rests 0.3 s**
  (console-game behaviour; otherwise UrT resumes sprinting whenever
  stamina returns). Default: keep. (5) **Whitelist fix = no `client`
  key** (what UrT's own client sends) rather than a fake `version`: the
  kick reads only that key, and `version` is never part of a connect.
  Default: keep. (6) **Crouch / Sprint toggle are per game** now (kept
  with the buttons; UrT Toggle, Quake 3 Hold); a Quake 3 profile that had
  crouch Toggle in its shared file goes back to Hold once (yours all had
  Hold). Default: keep. (7) **Full-screen overlays cover the 4:3 bars in
  every mod** (any full-height picture at the screen edge), not only UrT's
  scope; `cl_splitOverlayBars 0` turns it off. Default: keep for all mods.
- **R17 (0.0.0.22) -- defaults taken:** (1) **Menu size** keeps its
  scale (1.00x = what you had set; 1.50x = R14a's old default), not
  rebased. Default: keep. (2) **Your Guest defaults file** says aim
  assist Standard (you chose it), so your Guests / new profiles still
  start Standard; the engine default is Off for everyone else. Default:
  your file stays (one row to change). (3) **Cursor "(default)" labels:**
  there is no cursor-style choice (the game's menus always draw their red
  crosshair, ours a white dot), so nothing was labelled. Default: none; a
  style option (e.g. our menus with the red crosshair) only if you ask.
  (4) **Auto join** picks the team with fewer players, red on a tie (it
  does not use the game's own auto pick). Default: keep. (5) **Power-ups
  Off** also removes holdables (medkit, teleporter, ...) and works under
  instagib too (instagib alone keeps teleporters etc.). Default: keep.
  (6) The join / spectate rows are only for Quake 3 / Team Arena style
  games (not Urban Terror, which has its own team menu). Default: keep.

- **R16 (0.0.0.21) -- defaults taken:** (1) **Player speed** scales
  the game's own `g_speed` (baseq3 / OSP 320); greyed in UrT because UrT
  ignores `g_speed`. Default: keep. (2) **Weapon respawn** sets both
  `g_weaponrespawn` and the team games' `g_weaponTeamRespawn` (baseq3 30
  s) to the same value; Default restores both. Default: keep. (3) **Quad
  damage** row stays editable while Instagib is on (its help line says
  instagib removes quads anyway). Default: keep. (4) **Game's own
  instagib:** used whenever the game registers one; the row shows the
  game's current value when the page opens (a mod menu's choice is kept
  unless you change the row); our preset remains for games without one.
  Default: keep. (5) **Reset to defaults** sets game type 0 (Free for
  all): leaving the page then reloads the map if it was another type.
  Default: keep. (6) **Taskbar marking** is on for every Independent
  window (`cl_splitIndepMarkFullscreen 1`). Default: keep unless 79/80
  show a problem.

- **R15 (0.0.0.20) -- defaults taken:** (1) Windows names copy Linux's
  exactly (`quake3e-vulkan-ss.x64.exe`, `quake3e-ss.x64.exe`,
  `quake3e-ss.ded.x64.exe`); the vcxproj files are untouched (build.ps1
  renames when collecting), so the exes have no version resource (they
  never had one; the version is in BUILD-INFO.txt and the console). Default:
  keep. (2) The first build downloads the SDL2 source (9 MB) from GitHub;
  offline, put the zip in `work\sdl2-src\dl\`. Default: keep. (3) The SDL
  zip's GPG signature is not checked (sha256 pin only). Default: keep. (4)
  `work\sdl2\` (old DLL) is kept until the maintainer says delete. Default: keep
  one more round.

Defaults below are being taken; full list in design doc 11.

- **Server options (R14b) -- defaults taken:** (1) **Volume up to 150 %**
  kept: the mixer saturates cleanly (and a channel x master product is
  clamped so it cannot wrap); if the maintainer hears distortion above 100 %, cap
  at 100. (2) **Bots max 20** (R16; was 16); Off = the engine does not touch
  bots (a mod menu's bots stay); 1-20 keeps the *total* bot count. (3)
  **Cheats:** god mode turns `sv_cheats 1` on for the local server only
  while it is on; turning it off clears them live once every player's god is
  off again (R14b review fix; a `devmap` game keeps its own cheats). (4) **Native rows edit
  the game's cvars directly** (limits, friendly fire, gravity, volume):
  not archived by us, so Quake 3's own Start Server menu is never
  overridden; `g_gravity` is not archived by the game (800 again after an
  exe restart unless a set is loaded). (5) **Sets are per game**
  (`<game>/serversettings/`: UrT's game types differ) and hold map + type
  + limits + our levers, **not volume**. (6) **Capture limit 0-20** (frag
  0-100 by 5 as specified). (7) **Reset to defaults** (R16: maintainer) resets every row, game
  type (Free for all) and volume (100 %) too; only the map stays. (8) Player health uses handicap, so it also scales the
  damage a player deals (Quake 3's own rule). (9) X-only weapons give the
  gauntlet too at spawn; instagib gives the railgun alone. (10) "X only"
  lists the 9 baseq3 weapons (missionpack's three are recognised on maps
  and used by Random only). (11) Game type cycles the 4 stock baseq3 types
  (a current single-player / missionpack type is shown by name); UrT lists
  its 11.
- **Profile name vs in-game name (changed R9):** a name changed in the
  mod's own menu now renames the profile (if it sanitises to a free name;
  the in-game name keeps colours). Default: keep.
- **P1 as Guest -- decided by the maintainer 2026-10-06 (R14a):** a Guest is
  "Player 1" with default player settings like every other Guest; the
  profile (Guest included) defines the in-game name/model; q3config's own
  name/model/`cg_fov` are used only for keyboard/mouse P1 and come back at
  quit/kbm (and after a crash, at the next start).
- **Field of view (R14a):** a pad player's FOV is its profile's / the Guest
  default's `joy_fov` (default 90 = the game's default), also for P1, so
  the maintainer's autoexec `cg_fov 115` no longer applies while P1 plays on a pad
  (it does for keyboard/mouse P1, and is restored at quit). Default: keep;
  the maintainer sets 115 in his profile / Guest defaults.
- **Aim assist strength (R14a retune):** Low = 0.7 slowdown + 30 % follow,
  Standard = 0.45 + 60 % (bubble 72 units, full strength in its inner
  40 %, follow cap 120 deg/s); Low stays the default. Default: keep until
  the maintainer's next feel report.
- **Overlay size (R14a):** proportional to the screen height at every
  resolution (at 4K it used to be capped to a third), x1.5 by default,
  host row Menu size 1..2.5. Default: keep.
- **Change player model (R14a):** straight to the page only for baseq3
  (q3_ui menus, by keys); other games open their game menu. Default: keep;
  a UrT gear/model shortcut would be a later round.
- **Extras connect one at a time** (honours servers' per-IP caps and rate
  limits; on our own server each takes ~0.1 s, so 8 quick joins finish in
  ~0.6 s; a player stuck >15 s is dropped). Default: keep.
- **Up to 8 local players by default** (`cl_splitMaxPlayers 8`, R10); our
  own server never counts 127.x.x.x players against `sv_maxclientsPerIP`.
  Default: keep.
- **Guest defaults fallback:** absent `_guest.cfg` = the shared `joy_*`
  cvars / `default_pad.cfg`. Default: keep.
- **Reset to defaults** on a player's Controls/Buttons page resets to the
  Guest defaults; on the Guest defaults pages to the factory set. Default:
  keep.
- **Join cancel**: Start at the picker also cancels (like B). Default: keep.
- **Keyboard for mod menus**: hold Y 0.5 s. Default: keep.
- **Overlay look:** red accent, console font 8x16 in a quarter cell.
  Default: keep; tune after the maintainer's pass.
- **Aim defaults:** standard = m^2, dynamic = cubic S-curve, boost 1.5x
  over 250 ms at >= 90 % stick, zoom 0.5. Default: keep until the maintainer's
  feel report.
- **Aim assist default (R12):** every pad player starts on **Low** (mild:
  look speed 0.8 at an enemy's centre, 18 % follow) in games this PC hosts;
  the host switch "Allow aim assist" defaults On and is saved like the
  other host settings. The maintainer may prefer Off by default -- say so.
  Default: keep Low / On.
- **Aim assist marker (R12):** ` ^3+` after the name in Quake 3 / Team
  Arena, plain `+` in UrT (it strips spaces and colours); on whenever the
  assist is in effect (`cl_aimAssistMarker 0` hides it). Default: keep.
- **Flick pauses both assist components (R12)** for 150 ms (design said
  rotation only), so a fast sweep is never slowed. Default: keep.
- **Main-menu Start** opens the host page. **Player 1 with a pad:** any
  button on the first pad claims P1. **Rebind semantics:** swap.
  **Extra players' menu commands:** host/session commands refused.
  **Join hint:** mode 1. **Default pad binds:** The maintainer's Spearmint layout.
  **Split aspect:** 1. Default: keep all.
- **Bloom with several views:** P1 only until a per-viewport bloom pass is
  written. Default: leave for after R10.
- Online: derived GUIDs, no UrT auth, `sv_maxclientsPerIP 3`. Default: as
  stated.
- **UrT hunk (R11):** engine default `com_hunkMegs` stays 128 (baseq3: 8
  players fit); UrT's own default.cfg gives 800 = 7 UrT players; our UrT
  launch line / `work\urthome` config set 1024 for 8. Alternative: an
  engine-side per-game minimum (touches upstream `common.c`). Default: keep
  (launch line).
- **UrT pad layout (R11):** built into the exe (a `q3ut4\default_pad.cfg`
  would win if present); LT = UrT's zoom step, d-pad = quick radio + fire
  mode, UrT's radio menu not bound (number keys only). Default: keep until
  the maintainer's feel report.
- **Remote refusal number (R11):** "allows only K players" uses the number
  of our players that got in (exact with one-at-a-time joins unless other
  people share your address). Default: keep.
- **Independent mode (R13), defaults taken:** (a) every join/leave
  re-creates each moved window (full `vid_restart`, a short blink; the
  in-place resize crashed the GPU driver). Default: keep. (b) Windows that
  don't have the focus run at `com_maxfpsUnfocused` (60) -- in this mode
  that is every window but one. Default: keep; say if all windows should
  run uncapped. (c) The join-time picker shows in the middle of player 1's
  window, not where the new window will appear. Default: keep. (d) In a
  player's own window, Profile -> Switch/Delete offer only Guest and their
  own profile (the rest are managed from player 1's window). Default:
  keep. (e) The Session mode row switches immediately when nobody else is
  playing. Default: keep. (f) Windows only; on the Deck likely desktop
  mode only (Gamescope/Wayland cannot place windows). Default: as stated.
- GitHub repo. Default: local-only until the maintainer says (2026-10-06: he wants
  clean Windows **and** Linux releases first); a release is the three
  exes only (SDL2 static since R15).
- **Linux (2026-10-06):** (a) build in a distrobox (Fedora 43 + SDL2-devel
  etc.) rather than layering packages with rpm-ostree. Default: distrobox.
  (b) Only baseq3 paks 0-8 go to the Win Mini (no UrT/missionpack/custom
  maps) until baseq3 works. Default: as stated. (c) Linux work is run by a
  separate orchestrator session on the Win Mini, not driven over SSH from
  here. Default: separate session (the game needs that box's display and
  agents need local tools; SSH from here was also refused by the
  permission classifier for probing and copying). (d, L1) The Linux
  session is driven over SSH from the Windows dev box's Konsole with the desktop
  session's display variables exported (works: the window opens on the
  Win Mini's Plasma/Wayland desktop via XWayland `:0`); the maintainer's note said
  to use Konsole on the box. Default: SSH + exported display is fine as
  long as one game instance / no focus stealing rules hold. (e, L1) Deck
  game mode (L4) is not the current session (Plasma desktop, no
  Gamescope); when the maintainer wants L4 tested he switches the box to game mode
  and says so. Default: L1-L3 on the desktop session. (f, L3) Independent
  mode tiles in the display's **usable area** (Plasma panel excluded,
  1280x670 of 1280x720) rather than the whole monitor as on Windows.
  Default: keep (the panel would cover a tile otherwise). (g, L3) In
  Independent mode the engine forces SDL's x11 driver (XWayland) for the
  coordinator too, so that window is also an X11 window scaled by Plasma;
  Together mode stays native wayland. Default: keep; alternative is
  Together-only on Linux. (h, L3) Linux release format. **Closed (maintainer, L4):**
  upstream's layout -- a tar.gz of only `quake3e-vulkan-ss.x64`,
  `quake3e-ss.x64`, `quake3e-ss.ded.x64` (no launcher, no install script,
  no text files); Windows gets the matching `-ss` names at the merge.
- Sibling repos' local commits still carry a personal e-mail author. Default:
  leave history alone until the maintainer confirms a rewrite.
- **L6 Linux portability review of R14a-R21 (Sonnet, 2026-10-07; no real
  bug, notes only, for the Windows lead):** (a) `sv_client.c` Single Player
  loopback getchallenge exemption matches only `NA_IP` 127.x, not `::1`;
  local players always connect via `127.0.0.1` (`cl_splitscreen.c`), so it
  does not bite today. Default: leave; use `NET_IsLocalAddress` if a local
  player ever connects over IPv6. (b) `files.c` `FS_BaseHasDir`:
  `Sys_ListFiles` with `"/"` also returns plain files on unix, so a regular
  file named `q3ut4`/`baseq3` would count as a directory (very unlikely).
  Default: leave; `stat` + `S_ISDIR` if it ever matters. (c) Linux stable
  pad keys hash `/dev/input/eventN`, which can renumber across replugs /
  Steam Input reshuffles; the code already falls back and logs it.
  Default: watch in the maintainer's L-12 pass, no change.

## Live claims

- **R21 (2026-10-07):** none live. No game/server/HTTP process running
  (`tasklist` before every launch; every client run quit by its cfg or was
  killed by the runner's `timeout 100`; the dedicated server and
  `r21-http.py` are killed by `r21-dl.sh`; the `pythonw.exe` 10252 seen
  all session was not ours). `work/urthome` (the maintainer's) only read (log
  copied to `work/r21-maintainer-qconsole.log`); tests ran in `work/urttest`
  (fresh, seeded from a copy of his `q3config-ss.cfg` and profiles) and
  `work/q3test`. Test fixtures `work/r21srv/`, `work/r21www/` (a renamed
  stock bsp, rebuilt by `r21-dl.sh`; never committed). Clean tree.
- **R20 (2026-10-07):** none live. No game/server process running
  (`tasklist` before every launch; every run quit by its cfg or was killed
  by its runner's timeout check, 0 quake3e after each, children included).
  `work/q3home` not touched; `work/urthome` (the maintainer's) read, and its
  configs/profiles backed up and restored byte-identical around the two
  UrT runs (`work/r20-urt-home.txt`, `work/r20-reg-binds-home.txt`).
  `work/q3test`: profiles removed for the regression runs as before;
  `cl_splitSeenHint "1"` now archived there. The 0.0.0.25 exe rebuilt for
  the before shots was deleted after use. No
  settings outside the repo, no background agents, not pushed. Tree: R20
  committed; **not ours:** a modified `README.md` and a staged
  `docs/README.upstream.md` appeared during the round (another session);
  left untouched and uncommitted.

- **R19 / R19b (2026-10-07):** none live. No game/server process running
  (`tasklist` before every launch; every run quit by its cfg, 0 quake3e
  after each, children included). `work/q3home` (the maintainer's) only read (his
  0.0.0.23 Independent-mode logs). `work/q3test`: profiles backed up and
  put back after the regression runs; `padbus.txt` removed by the runner;
  one `seta cl_splitIndependent "1"` written by a menu test was set back to
  0 (the test now resets it from the console). No settings outside the
  repo (the backdrop window lives only while a test runs), no background
  agents, not pushed. Tree clean after the docs commit.

- **R18 (2026-10-07):** none live. No game/server process running
  (`tasklist` before every launch, each run quit by its cfg; 0 quake3e
  after the last). `work/q3home` (the maintainer's) not touched. `work/urthome`:
  q3config / profiles backed up and restored byte-identical around every
  run (`work/r18-*-home.txt`), the planted `r18old` and saved `r18prof`
  test profiles gone with the restore; test cfgs copied into
  `q3ut4/` as before. `work/q3test`: archived test values only (the leaked
  `cl_splitP1Input kbm` line removed), profiles from r8-join. The maintainer's UrT
  folder only read (the drop-in test used a junction in
  `work/r18-urtdrop/`, removed). No settings outside the repo, no
  background agents (the research agent finished), not pushed. Tree clean
  after the docs commit.

- **R17 (2026-10-07):** none live. No game/server process running
  (`tasklist` before every launch; each run quit by its cfg). `work/q3home`
  (the maintainer's) only read. `work/q3test`: archived test values only (e.g.
  `cl_padModCursorScale 1` from r8-join, `cl_splitSrv*` from r16-page's
  Reset), profiles created / removed by the scripts (r8-join's removed at
  the end), a `missionpack/` game dir from the Team Arena runs.
  `work/urthome/q3ut4`: `q3config-ss.cfg` restored byte-identical after
  the UrT runs, `q3config.cfg` untouched. No settings outside the repo,
  no background agents, not pushed. Tree clean after the docs commit.

- **R16 (2026-10-06):** no game/server process running (`tasklist`
  before and after every run: 0 quake3e). `work/q3home` (the maintainer's) never
  touched. `work/q3test`: archived test values only (`cl_splitSrv*` at
  their defaults after the last r16-page run's Reset, `com_maxfps 60`,
  ...), a new `osp/` game dir (q3config-ss.cfg + logs of the OSP runs);
  profiles created/removed by the scripts. `work/urthome/q3ut4`:
  `q3config.cfg` byte-identical to the pre-run copy, the `q3config-ss.cfg`
  the runs created deleted, profiles unchanged. The maintainer's OSP / UrT pk3s
  were only read (qagame strings extracted to the session scratchpad,
  nothing kept in the repo). No settings outside the repo touched, no
  background agents, not pushed to the Linux test box. Tree clean after the docs
  commit.

- **R15 (2026-10-06):** no game/server process running (`tasklist` before
  and after every run: 0 quake3e). `work/q3home` (the maintainer's) never touched.
  `work/q3test`: archived test values only (e.g. `com_maxfps 60` from the runs), profiles created/removed by the scripts. New
  `work/sdl2-src/` (SDL2 2.32.10 source + static lib; provenance in
  `work/sdl2-src-PROVENANCE.md`; needed by build.ps1 -- keep);
  `work/r15-alone/` (copies of the two client exes for the no-DLL test; may
  be deleted). The scratch fresh clone was deleted. No settings outside the
  repo touched, no background agents, not pushed to the Linux test box. Tree clean
  after the docs commit.

- **R14b (2026-10-06):** no game/server process running (`tasklist` after
  every run; two hung runs -- a fatal-error dialog and a slow custom-map
  load -- were processes I had started and were killed by PID).
  `work/q3home` (the maintainer's) never touched. `work/q3test` holds this round's
  archived test values in `baseq3/q3config-ss.cfg` (timelimit reset to 0,
  `cl_pitchspeed` / bot-skill lines removed; `s_volume 0.6`, `vid_xpos/ypos
  64` remain) and an empty `baseq3/serversettings/`. `work/urthome/q3ut4`:
  `q3config.cfg` restored byte-identical (sha256 147bc36a...), the
  `q3config-ss.cfg` the run created deleted (copy kept as
  `work/r14b-urthome-q3config-tested.cfg`). `work/q3home-ded` got one
  dedicated run (its qconsole.log removed; games.log appended). No
  background agents; not pushed to the Linux test box. Tree clean after the docs
  commit.
- **R14a (2026-10-06):** `work/q3home/` = **the maintainer's live home** (his
  profiles charlie/samuel/_guest/_padlast, his `baseq3/q3config.cfg` with
  `r_fullscreen 1`, `r_mode -2`, `in_gamepad` at its default 1, his
  `qconsole.log` of today). This round never wrote it (sha256 of
  q3config.cfg and both profiles dirs identical to the backup
  `work/r14a-maintainer-backup/`, which can be deleted once he has played the
  new build). It has **no `q3config-ss.cfg` yet**: his first start of
  0.0.0.18 seeds it from `q3config.cfg`, and from then on
  `q3config-ss.cfg` is his live config (`q3config.cfg` stays untouched).
  His log of today is also copied to `work/r14a-maintainer-qconsole-20261006.log`.
- `work/q3test/` = **the agents' test home** (new R14a; all runners default
  to it, `Q3HOME=` overrides): seed `baseq3/q3config.cfg` = the old test
  config (`work/r10-q3config-orig.cfg`), plus the `q3config-ss.cfg` the R14a
  runs wrote (archived test values, e.g. `ui_browserMaster 0`); profiles
  are created/deleted by the scripts. Keep.
- No game/server process running (`tasklist` after every run: 0 quake3e);
  no settings outside the repo touched; no background agents; not pushed
  to the Linux test box. Tree clean after the docs commit.
- (pre-R14a) `work/q3home/` held the agents' test config; the notes below
  about restoring it are historical.
- `work/urthome/` = our Urban Terror fs_homepath (R11), keep. Its
  `q3ut4/q3config.cfg` was reset to one line (`com_hunkMegs 1024`) for
  the maintainer's first run (UrT's default.cfg supplies the rest); the config the
  tests ended with is `work/r12-urthome-q3config-tested.cfg` (R11: `work/r11-urthome-q3config-tested.cfg`,
  R13: `work/r13-urthome-q3config-tested.cfg`); reset again after R13 (`seta com_hunkMegs "1024"`). Holds copies
  of the test cfgs and test screenshots dir; no profiles.
- `work/r11prof/` = the cross-mod profile test's homepath (recreated by
  `r11-profiles.sh`), keep or delete.
- `work/q3home-ded/` = the remote-test dedicated server's homepath, keep.
- `work/sdl2-src/` (R15: SDL2 2.32.10 source + `lib/SDL2-static.lib`,
  gitignored, `work/sdl2-src-PROVENANCE.md`; build.ps1 needs it -- keep).
  `work/sdl2/` (old SDL2.dll, superseded, safe to delete), `work/m2-baseline/`,
  `work/r9-baseline/`, `work/r10-baseline/` (older exes for before/after
  runs) -- keep. `work/r10-q3config-orig.cfg` = backup of the baseq3 test
  q3config.
- No game or server process running (`tasklist` after every R13 run: 0
  quake3e processes); the maintainer's UrT and Quake 3 folders were only read; no
  settings outside the repo touched; no background agents. The two NVIDIA
  driver resets of 00:59/01:02 (see Last round) are in the System event
  log; none since. Tree clean after the docs commit.
- R13b: `tasklist` before and after every run (0 quake3e processes left);
  `work/q3home` q3config restored from `work/r10-q3config-orig.cfg` and
  `work/urthome/q3ut4/q3config.cfg` reset to `seta com_hunkMegs "1024"`
  after the runs; children's `pk3cache-child*.dat` deleted; the 200-char
  test homepath under `work/` was deleted by `r13b-long.sh`. Not pushed.
- **Linux test box (2026-10-06):** git remote `<linux-box>`
  (`<linux-box>:dev/Quake3e-splitscreen`, SSH over a private network, host key
  accepted into `~/.ssh/known_hosts`); that repo has
  `receive.denyCurrentBranch updateInstead` with `splitscreen` checked
  out, so pushes from here land in its working tree as long as the Linux
  session keeps `splitscreen` clean (it works on `linux`). Nothing was
  started or left running on that box; `~/dev/q3data/baseq3/` was created
  there, empty (the maintainer copies the paks). No GitHub remote anywhere.
- **L6 on the Linux test box (2026-10-07):** the 0.0.0.27 build in
  `build/release-linux-x86_64/` (built by the lead, commit 882d0d38) is left
  installed for the maintainer's L-12. Ten windowed Together client runs (nine
  vk, one gl) and two Independent runs with their windows, one at a time,
  each quit by its cfg; the `/proc/<pid>/exe` check before every run found
  no quake3e process, every `-procs.txt` and the final check show 0. The
  test home `work/q3home`: profiles dirs removed after each run,
  `baseq3/q3config-ss.cfg` (seeded by the first run) deleted at the end,
  `padbus.txt` removed by `l6-r19-run.sh`. Kept evidence: `work/l6-*`
  (cited in Last round L6). Not pushed.
- **L5 on the Linux test box (2026-10-06):** thirteen short client runs (two of them upstream's copy), one
  dedicated run and one l3-two Independent run, one at a time, each quit by
  `+quit`/its cfg; the `/proc/<pid>/exe` check before every run found no
  quake3e process, `work/l5-two-procs.txt` 0 after. Upstream's
  `~/Games/quake3/quake3e/quake3e-vulkan.x64` was only copied (read) into
  `work/l5-side/`, which was deleted with all `work/l5-home-*` scratch
  homes. `work/q3home/baseq3/q3config-ss.cfg` (written by the l3-two run)
  was deleted: the next run seeds it from the `// linux test config`
  `q3config.cfg` there; profiles dirs removed by `l3-two.sh`. Kept
  evidence: `work/l5-*.log`, `work/l5-*-ls.txt`, `work/l5-two-q3config.txt`,
  `work/l5-two-procs.txt`. 0.0.0.17 tar.gz in `build/` (0.0.0.16 one
  deleted). Not pushed.
- **L4 on the Linux test box (2026-10-06):** five short windowed client runs
  (vk, gl, unpacked archive x3) and two dedicated smokes, one at a time,
  each quit by its cfg/`+quit`; `pgrep` empty before and after every run;
  nothing of the maintainer's touched (`~/Games/quake3/quake3e/` not read or
  written; his Steam shortcut unchanged). `work/q3home/baseq3/q3config.cfg`
  reset to `// linux test config`, the dedicated run's
  `q3config_server.cfg` and the logs removed; `work/l4-unpack/` deleted.
  New gitignored build state: `build/obj/{vk,gl}/` (objects),
  `build/release-linux-x86_64/` (three exes + BUILD-INFO), the 0.0.0.16
  tar.gz; the 0.0.0.15 tar.gz and the old objects in the output dir were
  deleted. Podman images pulled for checks: `docker.io/rhysd/actionlint`,
  `docker.io/library/ubuntu:24.04` (`podman rmi` either). Not pushed (no
  remote for `linux` here).
- **L3 on the Linux test box (2026-10-06):** all Independent-mode runs were
  coordinator + children started by `l3-run.sh`, which kills only pids it
  started; `pgrep quake3e.x64` empty after every run; `work/q3home/baseq3/
  q3config.cfg` reset; profiles dirs, `pk3cache-child*.dat`,
  `qconsole-child*.log` removed; only cited `work/l3-*` kept. X11 state was
  read with `xwininfo`/`xprop` (queries only, no screenshots, no focus
  changes). Package built into `build/` (gitignored). `splitscreen`
  untouched.
- **L2 on the Linux test box (2026-10-06):** six headless runs, sequential, each
  quit by its cfg; `pgrep quake3e` empty after; `work/q3home/baseq3/q3config.cfg`
  reset to `// linux test config`; profiles dirs removed; evidence
  `work/l2-*` only. No engine code touched.
- **L1 on the Linux test box (2026-10-06):** branch `linux` carries the round
  (`48ea45ee`, `bf32ad35`, + docs); `splitscreen` untouched and clean for
  the Windows dev box's pushes. `~/dev/q3data/baseq3/` holds the maintainer's paks 0-8
  (483 MB, scp'd from the Windows dev box read-only; `work/q3data-PROVENANCE.md`);
  nothing else of his copied. Test runs: three short windowed launches on
  the desktop (q3dm1 vk, q3dm1 gl, main-menu pad probe), each quit by its
  cfg, `pgrep quake3e.x64` empty after each. Distrobox `q3dev` exists (podman image fedora:43,
  ~400 packages; `distrobox rm q3dev` removes it). Nothing layered with
  rpm-ostree. `~/dev/CLAUDE.md` on the box is identical to
  `docs/dev-CLAUDE.linux.md`. One short host launch of `quake3e.x64`
  without data on the desktop display (exited by itself at the pak check,
  `pgrep quake3e` empty after); `work/q3home/` created, empty; no
  screenshot taken; nothing of the maintainer's touched; no agents left running.

## Roadmap (lead-maintained — executors: do not edit this section)

The maintainer (2026-10-05): "keep building everything asked for"; he tests when
the whole list is in. One executor round at a time (one game instance on
this box); a light Sonnet review of each round's commits runs alongside
the next round and its findings are relayed to that executor. Bump
`VERSION` by one dev step per round. Design doc section in brackets.

- [x] R1 M0 build · R2 contexts + 2nd connection · R3 two views ·
      R4 2–4 players/layouts/userinfo/sound · R5 gamepads/join ·
      R6 per-player mod menus + pad menu control (0.0.0.5)
- [x] R7 (0.0.0.6) engine pause overlay: Resume / Game menu / Controls
      (incl. aim-feel settings §14.4 + rebinding) / Leave; host
      Splitscreen page (§13.4, §14.1 note); P1 pad-default setting
      (§14.2); join = hold A default; exe named
      `quake3e-splitscreen.exe` (§17.1)
- [x] R8 profiles + Guest + host-editable Guest defaults + join-time
      profile picker + on-screen keyboard (§12.3, §12.4, §13.1, §14.3)
- [x] R9 robustness: cgame/UI hunk growth on join/leave (reuse VM
      allocations / avoid restarts), M1 polish list in Next steps,
      remote-server join for extras tested against a second local
      dedicated server (M4-T1/T2), per-IP cap handling
- [x] R10 up to 8 players: layouts 5–8, 8 pads, perf (§10 M5)
- [x] R11 aim assist (done as R12, 0.0.0.11), local-only (§15)
- [x] R12 Independent mode (done as R13, 0.0.0.12): coordinator + child processes (§17.2)
- [x] R14a (0.0.0.18): Linux L1-L5 merged (side-by-side `-ss` configs on both OSes), sound+pads COM fix, P1 Guest = "Player 1", aim assist retune + state log + marker colours, menu size/scroll, model/handicap/FOV rows, cursor speed, browser X/Y
- [x] R14b (0.0.0.19): Server options page (§18)
- [x] R15 (0.0.0.20): SDL2 2.32.10 static in the Windows client exes (release = 3 exes), `-ss` exe names on both OSes, R14b review fixes; CI `release.yml` Windows job unverified (L-10)
- [x] R16 (0.0.0.21): maintainer follow-ups — Outer deadzone label, Quad/Player speed/Weapon respawn rows, full Reset, instagib via the mod's own cvar (UrT `g_instagib`, OSP `match_instagib`) + greying, bots 20, FOV 90 check, Independent-mode windows marked fullscreen for the shell (taskbar/Joyxoff, option 1)
- [x] R17 (0.0.0.22): maintainer pass 2 — aim Low nudge, aim Off default for new profiles/Guests, P1 model page at 1440p, "Centered", menu size 1.0 (0.5-1.5), "95%", Power-ups row, Cursor speed row (both cursors, red 1.4x), Team Arena spectator Join/Spectate rows
- [x] R18 (0.0.0.23): Urban Terror — q3ut4 auto-detect + hunk 1024, no "client" userinfo key in UrT (B3/B4 vpncheck whitelist), scope over the 4:3 bars, blue menus, the maintainer's UrT pad layout + Crouch/Sprint toggle rows, docs/URBAN-TERROR.md
- [x] R19 (0.0.0.24) + R19b (0.0.0.25): Independent mode — stable pad keys (guid@path hash), one pad per window, coordinator-only join, duplicate-listing rule, replug reclaim, black backdrop, "(experimental)" label; R19b: unalias fix (review), Together same-path dedupe
- [x] R20 (0.0.0.26): backdrop removed, Independent tiles render as Together cells (aspect fix), first-launch hint, Single Player arena joins for local players, UrT downloads on by default (curl built in), pad skips cinematics. README.md drafted (lead) with docs/images/splitscreen-2x2.jpg; upstream README kept as docs/README.upstream.md
- [x] R21 (0.0.0.27): UrT map download — server fs_game == our base game no longer restarts the game, log survives Com_GameRestart, UrT download rules (cl_autodownload, map pak only, HTTP from sv_dlURL, q3ut4/download search path). README launch options table (lead). Review clean; lead widened the local pak-name buffer
- [x] Taskbar / Joyxoff in Independent mode: MarkFullscreenWindow (R16) + backdrop (R19). A foreground-rect check cannot pass with quarter-screen tiles; fallback = disable Joyxoff bindings for this mode (documented)
- [x] Urban Terror (S5/M3) — R11, 0.0.0.10 — needs the maintainer's OK to download UrT 4.3
      (~1.4 GB) into `work/`; slot it in as soon as he answers
- [ ] M6 Linux / Steam Deck (§14.5) — **started 2026-10-06** without a
      GitHub repo: repo mirrored to the Linux test box over SSH, Linux rounds
      L1-L4 run by a separate orchestrator there per
      `docs/LINUX-BOOTSTRAP.md`; the Windows lead merges branch `linux`
      (`git fetch <linux-box> linux`). GitHub repo + releases come when
      both platforms are clean (maintainer, 2026-10-06).
- Maintainer-only, pending: real-pad checklist (Next steps), listening
  check, adapter join-hint test, aim-feel tuning, public-server test,
  GitHub repo creation/visibility, any release.
- Carry into R11 (from the R10 review): (a) `CL_SplitConnectWaiting`
  15 s stuck-join timeout spans CA_LOADING/CA_PRIMED too — a healthy
  extra player whose map/cgame load exceeds 15 s gets dropped; refresh
  the timer while loading progresses (or exempt loading states).
  (b) `sv_client.c` 127/8 per-IP-cap exemption should apply only when
  the server is a listen server (`!com_dedicated`), so a dedicated
  server behind a same-host proxy keeps the cap. (c) minor: mirror/
  portal views copy scene dlights per view and can exhaust the shared
  512 array across several mirror scenes — degrades safely; note only.
