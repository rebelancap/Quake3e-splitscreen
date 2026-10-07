# Changelog

## 1.0.0

First public release of Quake3e-splitscreen: local splitscreen for Quake III
Arena on the Quake3e engine.

- **Splitscreen for up to 8 players** on one screen, layouts following the
  player count, each viewport with its own HUD.
- **Gamepads:** pads are assigned automatically; friends hold **A** to join.
  Player 1 can use keyboard and mouse or a pad.
- **Profiles** per player: name, model, aim feel, aim assist, controls, FOV
  and deadzones, saved and reused.
- **Aim assist** for pad players in local games (Off / Low / Standard; the
  host can turn it off).
- **Pause menu on every pad**, with server options for the host (map, game
  type, limits, bots, instagib, weapons, power-ups and more).
- **Unmodified mods and servers:** every local player is a full client, so
  mods and online servers see ordinary players.
- **Urban Terror 4.3** support with its own pad layout, menu style and map
  downloads.
- **Independent mode (experimental):** one window per player.
- **Windows x64 and Linux x64 / Steam Deck** builds.

The dev builds that led up to this release follow below.

## 0.0.0.27 (dev, R21)

- **Urban Terror: missing server maps really download now.** 0.0.0.26
  restarted the game at every join to a UrT server, and that restart turned
  downloads off again. Joining no longer restarts. Maps download the way UrT's
  own client does it: switched by UrT's "auto download" setting, from the
  server's web address, into `q3ut4\download\`. The maps UrT's own client
  already downloaded are used too.
- **The console log keeps the whole session**, also after a game or mod
  switch (it used to stop there). Every download decision is in it.
- Linux round L6: built and regression-tested on Linux, no code change.

## 0.0.0.26 (dev, R20)

- **Independent mode: each window now looks like the same-size Together
  view.** The 3D view and HUD were stretched (wide numbers, very narrow
  vertical view); a window that is one of several now gets Together's HUD
  shape and field of view. The black backdrop window is gone (it did not
  help Joyxoff: turn Joyxoff's bindings off while playing this mode).
- **Single Player arenas:** extra players can join (they stayed at "Player 2
  connecting..." before).
- **Urban Terror:** missing server maps download automatically (UrT's own
  settings had downloads off, so joining such a server failed with
  "couldn't load maps/...").
- **Any pad button skips the intro video** (Quake 3 and Urban Terror); the
  press does nothing else.
- **First start:** the main menu says "Start a game first, then friends hold
  A to join" until you start your first game.

## 0.0.0.25 (dev, R19b)

- **Independent mode: two people who press A at the same moment both get
  their own window.** They are briefly taken for one pad listed twice
  until they press differently (the second one then presses A again);
  only a pad that keeps matching for a second and confirms it with another
  press is treated as a second listing of a pad already playing.
- **Together mode:** a pad Windows lists twice under the same device path
  is opened once (`pad: skipped duplicate listing of ...`).

## 0.0.0.24 (dev, R19)

- **Independent mode is marked experimental:** the Session mode row reads
  "Independent (experimental)" ("Separate windows per player; still being
  made reliable"). Together mode is unchanged.
- **Independent mode: each window now gets exactly its own pad.** Pads are
  told apart by their device path (the same in every window), not by their
  order, which differs between windows: no more P1 moving P2's window, no
  window without a pad. One pad that Windows lists twice joins once (the
  second listing is ignored, logged); a pad that already plays never opens a
  second window; an unplugged pad goes back to its window when it returns.
  Every assignment is logged (`indep: pad <key> -> P<n> (pid ...)`).
- **Independent mode: a black backdrop window** under the tiles, marked
  fullscreen too (`cl_splitIndepBackdrop`); the log says what fullscreen
  detection (Joyxoff) sees. A tool that compares the focused window with the
  screen still sees no fullscreen game (a tile is smaller than the screen):
  turn off Joyxoff's bindings for this mode.
- Profiles saved by R18 or later keep a deliberately kept R11 UrT pad layout
  and their crouch/sprint choices (no automatic layout upgrade); in UrT any
  command-line `seta`/`sets`/`setu com_hunkMegs` is respected like `set`.

## 0.0.0.23 (dev, R18)

- **Urban Terror, easy start:** put the exes in your UrbanTerror43 folder
  and run them; UrT starts by itself (no `fs_basegame` needed) with enough
  memory for 8 players. See `docs/URBAN-TERROR.md`.
- **Online UrT servers** should no longer kick you with "Non whitelist
  client": in UrT the client now connects like UrT's own client.
- **Sniper scopes** in splitscreen: black all around the lens, no zoomed
  picture beside a square.
- **Your UrT pad layout:** A jump, B crouch, X reload, Y bandage, d-pad
  drop item / drop weapon / IR vision / weapon mode, R3 knife, L3 sprint,
  LB reset zoom, RB next weapon. **Crouch and sprint toggle** in UrT
  (Controls: Crouch / Sprint, Toggle or Hold); every UrT action is on the
  Buttons page to bind. Menus are blue in UrT.

## 0.0.0.22 (dev, R17)

- **Player 1's LB / RB on the player model page** turn the pages again
  (on a big screen they clicked the bottom right corner): the cursor is now
  walked onto the page arrow by where the menu draws it.
- **Team games: joining from the pause menu.** A spectator (Team Arena and
  Quake 3 team games start you as one) gets **Join red / Join blue / Auto
  join** (free for all: **Join game**) under Resume; a player on a team
  gets **Spectate**.
- **Server options: Power-ups** On/Off (Off removes every power-up and
  holdable item, restarts the map; greys Quad damage; greyed in UrT).
- **Controls: Cursor speed** 50-200 % (per player, saved in profiles) for
  both the game menus' red cursor and our menus' dot; the red cursor is
  also a bit faster by default (1.4x, was 1.25x).
- **Aim assist:** Low is a little stronger (Standard unchanged); Guests and
  new profiles start with aim assist **Off** (saved profiles keep theirs).
- **Menu size** default 1.00x, range 0.50x-1.50x; "4:3 Centered", "Center
  view", and percentages print as "95%".

## 0.0.0.21 (dev, R16)

- **Server options, new rows** (after Low gravity): **Quad damage** On/Off
  (Off removes the quad pickups; restarts the map when you leave),
  **Player speed** 50-200 % of the game's own run speed (live), **Weapon
  respawn** Default or 1-30 s (live). Urban Terror greys all three (its
  movement ignores the speed setting; it has no weapon pickups).
- **Reset to defaults** now resets every row, including game type (free
  for all) and volume (100 %); only the map stays.
- **Instagib:** in a game with its own instagib (Urban Terror
  `g_instagib`, OSP `match_instagib`) the row switches that on/off instead
  of our preset ("Uses the game's own instagib"). While instagib is on,
  Player health is greyed too (no handicap).
- **Bots up to 20** (was 16); asking for more bots than the server has
  room for restarts the map once again (it silently stopped at 15 before).
- Controls page: the row is just **Outer deadzone** (the explanation is
  its help line).
- **Independent mode:** every game window is marked fullscreen for
  Windows, so the taskbar hides behind the active window and tools like
  Joyxoff see a game running (`cl_splitIndepMarkFullscreen 0` turns it off).

## 0.0.0.20 (dev, R15)

- **No more SDL2.dll:** the gamepad library is built into the two game
  exes, so the Windows build is just three exes like upstream's. The
  console says `gamepad: SDL 2.32.10 static (built in)`.
- **New exe names** (same as the Linux build): `quake3e-vulkan-ss.x64.exe`
  (Vulkan, the one to start), `quake3e-ss.x64.exe` (OpenGL),
  `quake3e-ss.ded.x64.exe` (dedicated server). The old
  `quake3e-splitscreen*.exe`, `quake3e.ded.exe` and `SDL2.dll` are gone
  from `build\Release`; update shortcuts.
- Server options fixes: god mode no longer leaves cheats on after you
  turn it off mid-game (cheats go off as soon as nobody has god mode,
  also while a player is dead).
- Debug builds (`build.ps1 -Configuration Debug`) link a debug-CRT SDL2.

## 0.0.0.19 (dev, R14b)

- **Server options** (player 1's Start menu, above End game; also the
  console command `serveroptions`): restart the round, change map (every
  map, with its picture), game type, time / frag (capture) limit,
  instagib, map weapons (default, random, or one weapon only), weapons at
  spawn, infinite ammo, bots (up to 16) and their difficulty, friendly
  fire, self-damage off (rocket jumps stay), god mode (turns cheats on for
  this local game), everyone's health (handicap), low gravity, game volume
  (now up to 150 %), reset, and saved sets (save / select / delete).
  Leaving the page applies everything; map, game type, instagib and
  weapons restart the map once (the page says so before you leave).
  Greyed on someone else's server; in Urban Terror the weapon / instagib /
  ammo / self-damage / god rows are greyed, the rest works. Each row has a
  one-line explanation at the bottom.

## 0.0.0.18 (dev, R14a)

- **Windows and Linux merged:** the Linux work (0.0.0.13-0.0.0.17) is in
  the Windows build. On Windows too our exe now saves its settings in
  `baseq3\q3config-ss.cfg` (copied once from `q3config.cfg` on first
  start; `q3config.cfg` is never written again) and runs `autoexec-ss.cfg`
  when present; the dedicated server uses `q3config_server-ss.cfg`.
- **Sound with pads on:** sound failed to start whenever pads were enabled
  (SDL had already set up Windows COM, which the engine took as an error);
  it now starts normally, in Independent-mode windows too. The log says
  `sound: COM already initialised ...` when that happens.
- **Player 1 as Guest is "Player 1"** (default model), like every other
  Guest; your own name/model from the config is used only when player 1
  plays with keyboard and mouse, and is put back when you quit.
- **Aim assist you can feel:** Low and Standard were far too weak (the
  "sticky" zone was tiny and only its exact centre slowed you). Now Low =
  clearly heavier over an enemy and a gentle follow, Standard = obvious
  slow-down and strong follow, still never moves your view without input.
  The `+` after your name is yellow on Low, green on Standard (corner glyph
  too). The log says `AA P1: Standard (Guest)` / `AA P1: off: ...` whenever
  a player's aim assist state changes.
- **Bigger menus:** our own menus (pause menu, profiles, the "who's
  playing?" list, keyboard) are 1.5x bigger, and on a 4K screen no longer
  shrink to a third of that; **Splitscreen settings -> Menu size** (1.0 to
  2.5) changes it. Pages taller than their part of the screen scroll (arrows
  show more rows); in small cells the text shrinks only as far as it must.
- **Server browser:** while the server list is refreshing, **X or Y** stops
  it (like SPACE on the keyboard), for every player and in every
  Independent-mode window.
- **Faster cursor in the game's menus:** the right-stick cursor (the red
  crosshair) moves 25 % faster (`cl_padModCursorScale 1.25`); our own
  menus' cursor dot is unchanged.
- **Profile page:** **Change player model** opens the game's player model
  page in your own part of the screen (Quake 3: straight to it; other games:
  their game menu); there **LB / RB** turn the pages of models.
  **Handicap** (None, 95 ... 5, like Quake 3's player setup) is saved in
  your profile.
- **Controls page:** the "Full speed at" row is now **Outer deadzone: full
  speed at 95 %**; new **Field of view** row (80 to 130 in steps of 5,
  default 90 = the game's default), per player and saved in the profile.
  Player 1 on a pad or as Guest uses it too; your own config's FOV comes
  back when you quit or switch player 1 to keyboard/mouse.

## 0.0.0.17 (dev, Linux round L5)

- **Settings side by side with vanilla Quake3e:** our clients now save
  their settings in `q3config-ss.cfg` (instead of `q3config.cfg`) and our
  dedicated server in `q3config_server-ss.cfg` (instead of
  `q3config_server.cfg`), so `quake3e-vulkan-ss.x64` and upstream's
  `quake3e-vulkan.x64` can live in the same folder without overwriting each
  other's settings. Applies to Windows too (the code is shared).
- **First run copies your settings:** when a game folder (baseq3 or a mod)
  has no `q3config-ss.cfg` yet but has upstream's `q3config.cfg`, it is
  copied once as-is (binds and settings) and the console says
  `config: q3config-ss.cfg created from q3config.cfg`. Upstream's file is
  never changed; an existing `q3config-ss.cfg` is never overwritten; the
  same for `q3config_server.cfg` -> `q3config_server-ss.cfg`.
- **`autoexec-ss.cfg`:** runs after `autoexec.cfg` (when present; silent
  when not), for settings that only our build should use. Vanilla Quake3e
  ignores it.
- **Still shared, exactly as upstream:** game folder and paks, home path
  (Windows: the exe's folder; Linux: `~/.q3a`), downloads, `autoexec.cfg`,
  `profiles/` (only our build reads it), screenshots and demos.

## 0.0.0.16 (dev, Linux round L4)

- **Linux release = upstream's layout:** plain executables with the
  renderer built in (no renderer `.so` files): `quake3e-vulkan-ss.x64`
  (Vulkan, the one to launch), `quake3e-ss.x64` (OpenGL) and
  `quake3e-ss.ded.x64` (dedicated server) -- upstream's names plus `-ss`.
  `+set cl_renderer` is no longer needed (or used): pick the executable.
- The Linux archive `quake3e-splitscreen-<version>-linux-x86_64.tar.gz`
  holds only those three executables, at its root: unpack it next to your
  `baseq3/` and start `quake3e-vulkan-ss.x64` (from Steam: add it as a
  non-Steam game).
- The launcher script and `install.sh` are gone: the engine already uses
  the folder its executable is in as the game folder (wherever it is
  started from), and switches itself to X11 for Independent mode, so the
  launcher added nothing.
- GitHub release workflow (`.github/workflows/release.yml`): a `v<version>`
  tag builds the Linux tar.gz and the Windows x64 zip (with `SDL2.dll`)
  and attaches both to the release. Upstream's build workflow was removed.

## 0.0.0.15 (dev, Linux round L3)

- **Independent mode on Linux** (desktop session): `--independent` or
  Session mode -> Independent gives each joining pad its own window, tiled
  in the screen's work area, exactly as on Windows; windows close with
  player 1's, never take the focus. Under Steam Deck game mode (Gamescope)
  the mode reads "Unavailable" and the game stays in Together mode.
- Linux package: `quake3e-splitscreen-<version>-linux-x86_64.tar.gz` with
  a launcher (`quake3e-splitscreen.sh`, add it to Steam as a non-Steam
  game) and `install.sh` (copies beside your `baseq3/`, never touches it).

## 0.0.0.14 (dev, Linux round L2)

- Linux: the headless splitscreen test suite (join/picker/profiles,
  layouts 2-8 on Vulkan and OpenGL, aim assist, 8 virtual pads, perf)
  behaves as on Windows; no engine changes. `scripts/tests/linux-run.sh`
  is the Linux test runner.

## 0.0.0.13 (dev, Linux round L1)

- **Linux build** (Bazzite / Steam Deck work starts): `scripts/build.sh`
  builds the client (`quake3e.x64` + Vulkan/OpenGL renderer `.so`) and the
  dedicated server inside the `q3dev` distrobox; the binary runs on the host
  against its own SDL2/Vulkan/GL. Gamepads use the engine's own SDL2 (no
  second SDL), so `vid_restart` no longer tears the pads down under the
  engine on SDL builds, and the pad code no longer flushes the engine's
  keyboard/mouse/window events. Independent mode is Windows-only for now
  (reports unavailable on Linux). Boots on Vulkan (RADV) and OpenGL (Mesa)
  on the GPD Win Mini; its built-in controller is detected.

## 0.0.0.12 (dev)

- **Independent mode**: every player gets a completely separate game in a
  window of their own (own menus, own match/server/mod). Start with
  `--independent`, or switch Splitscreen settings -> **Session mode** to
  Independent. A pad that holds A picks its profile, then its window opens
  and all windows re-tile across the screen like the splitscreen layouts
  (2 = top/bottom, 3 = 2 + 1 wide, 4 = 2x2 ...). Leaving (Exit game, or
  Back+Start) closes that window and the rest re-tile; quitting player 1's
  window closes them all. Windows are borderless (no exclusive
  fullscreen); every window keeps its sound and has its own volume.
- Spawned windows never steal the keyboard focus; click a window to type
  in it. Pads work in whichever window they belong to.
- Fixes (review): long install/home paths no longer silently break a
  player's window (it refuses to open with a message instead); a window
  loading a big map is no longer closed as "hung"; unplugging a window's
  pad no longer lets another pad take that player's slot.

## 0.0.0.11 (dev)

- Aim assist for pad players, **only in games this PC hosts** (never on
  someone else's server): a gentle slowdown of your look stick over an
  enemy and a little help following a moving one while you move or aim.
  No snapping; it never aims for you when both sticks are idle. Per player
  in Start -> Controls: Off / Low (default) / Standard; the host can switch
  it off for everyone in Splitscreen settings. Players using it get a `+`
  after their name and a small `+` in the corner of their view.
- A player who never finishes loading can no longer hold up the others'
  joins forever; extra players' chat/team/kill commands go straight to the
  server.

## 0.0.0.10 (dev)

- Urban Terror 4.3 runs (your UrT folder, unchanged) with up to 8 players
  on one screen: each player gets UrT's own team and gear menus in their
  part of the screen, driven by their pad's cursor, and picks team and
  weapons independently. 8 UrT players need `com_hunkMegs 1024` (UrT's
  default 800 fits 7).
- UrT pad layout built in: RT fire, LT zoom, A jump, B crouch, X reload,
  Y use, LB/RB weapons, L3 sprint, R3 bandage, d-pad radio calls + fire
  mode. A profile made in Quake 3 starts UrT with these buttons and keeps
  its aim settings.
- Online: pads can join on someone else's server too. If the server turns a
  player away it now says so plainly in that player's area for 10 s
  ("Player 4 could not join: this server allows only 3 players from one
  connection"), and the join hint warns that online servers may limit
  extra players.
- A slow-loading extra player is no longer dropped as "stuck".

## 0.0.0.9 (dev)

- Up to 8 local players (now the default maximum): 5 = 3 on top + 2 wide
  below, 6 = 3x2, 7 = 4 on top + 3 below, 8 = 4x2; with "Screen
  use" off, a 3x3 grid with black cells. Joining/leaving between 2 and 8
  players no longer restarts anybody's game view (no hitch).
- With 8 views, busy fights no longer lose weapons, HUD heads or lights in
  some players' views.
- Our own server no longer refuses the 5th+ local player ("Too many
  connections.").
- A player stuck joining no longer blocks the players after it (15 s).
## 0.0.0.8 (dev)

- Joining, leaving and changing the split layout no longer eat memory: you
  can join/leave as often as you like on one map (before, ~30 joins or
  layout changes ran out and further joins were refused until a new map).
- Holding A to join no longer shakes everyone's view: the "who's
  playing?" list shows over the screen and the views change only once you
  pick; pressing B there changes nothing for the others.
- Online: extra players can join a real server. They join one after
  another; if the server refuses one (e.g. "Too many connections." -- many
  servers allow only 3 players from one address), that player's part of
  the screen says why for a few seconds and the others keep playing. Kicks,
  timeouts and leaving also show their reason there.
- Pad buttons reach a mod's own in-game menus/vote screens; the "chat"
  icon shows over a player whose pause menu or game menu is open.
- Profiles: a name changed in the game's own player menu renames your
  profile; player 1's own name/model come back even after a crash.

## 0.0.0.7 (dev)

- Profiles: your settings follow you, not your player number. After the
  join hold, your part of the screen asks "who's playing?": Guest, your
  saved profiles (the one you used last with that pad is already
  highlighted), or New profile (type your name on the on-screen keyboard).
  B there cancels the join. Your profile name is your name in the game.
- A profile keeps look/aim settings, buttons and your model/colours, saved
  as you change them. Guests start from the Guest defaults and forget their
  changes when they leave. Start -> Profile: switch, rename, keep your
  Guest settings as a profile, delete. A profile someone is using is greyed.
- Player 1 -> Splitscreen settings -> Guest defaults: the controls and
  buttons every new Guest starts with.
- On-screen keyboard: d-pad + A to type, X delete, Y space, L3 shift,
  Start done, B cancel. Hold Y in the game's own menu to type into it.
- R7's per-player-number settings file is gone (player 1's settings became
  the Guest defaults).

## 0.0.0.6 (dev)

- The game is now `quake3e-splitscreen.exe` (OpenGL: `quake3e-splitscreen-gl.exe`).
- Start opens a small red menu in your own part of the screen: Resume, Game
  menu (the mod's menu), Controls, Leave game. The game keeps running for
  everyone else. Player 1 also gets Splitscreen settings and "End game".
- Controls per player: look speed, invert, aim curve (Linear / Standard /
  Dynamic), turn boost when the stick is pushed all the way, slower aim
  while zoomed, deadzones, crouch toggle, walk/run, and button rebinding
  (pick an action, press a button). Kept across restarts for now.
- Splitscreen settings (player 1): layout, wide view, 2-player split,
  HUD shape, join button / hold time, join hint, max players, and whether
  player 1 is a pad or keyboard+mouse. Start on the main menu opens it too.
- Joining now means holding A for 0.7 s; the first pad to press anything
  is player 1, so you can play start to finish with just a pad.

## 0.0.0.5 (dev)

- Menus work with a pad: d-pad / left stick move, A selects, B goes back,
  right stick moves the mouse cursor, RT clicks (LT right-clicks), Start
  closes. Player 1's pad now drives the main menu and server browser too.
- In a game, Start opens the mod's own menu for that player only, inside
  their own part of the screen, with their own cursor; everyone else keeps
  playing. Team / player changes affect only that player; "Leave arena"
  removes only that player. Player 1's menu also stays in player 1's view.
- The "press A to join" hint now only appears for a pad someone actually
  used in the last 10 s (idle virtual pads no longer keep it on screen).
  `cl_splitJoinHint 2` = show it for 10 s at level start only; 0 = off.

## 0.0.0.4 (dev)

- Gamepads (Xbox, PlayStation, Switch, 8BitDo... up to 8) via `SDL2.dll`
  beside the exe. In a game, press A (Cross on PlayStation, B on Switch)
  to join: the first pad shares player 1 with keyboard/mouse, the next
  ones become P2, P3, P4. A corner hint says who joins next.
- Left stick moves (gentle push walks), right stick looks, RT fires,
  LT zoom, A jump, B crouch, LB/RB and d-pad left/right change weapon,
  X gesture, Y use item, Back scores, L3 run/walk, R3 centre view.
  Change one with e.g. `pbind 2 PAD_X weapnext`; look speed with
  `joy_yawSpeed` / `joy_pitchSpeed` (`p2 joy_yawSpeed 250` for P2 only).
- Hold Back+Start for 2 s to leave; unplug a pad and that player just
  stands still until it is plugged back in (or another pad presses a
  button). `padlist` shows every pad the game sees.

## 0.0.0.3 (dev)

- Up to 4 players on one screen, keyboard on player 1, the others driven
  from the console. In a `devmap`/`map` game type `addplayer` (repeat for
  P3, P4), `dropplayer 3` to remove one; `p2 +forward`, `p3 +attack`,
  `p4 kill` drive them. Players keep their number when others leave; the
  next `addplayer` fills the lowest free number. They follow map changes.
- Layouts: 2 = top/bottom (`cl_splitVertical 1` = side by side);
  3 = two on top + one wide below (`cl_splitFill 0` = 2x2 with a black
  cell; `cl_splitWidePlayer 2` gives P2 the wide view); 4 = 2x2. Changes
  apply live.
- Names: `p2_name Alice` (or `p2 name Alice`); other players default to
  "Player N". Sound now comes from all players (nearest one wins);
  announcer lines like "Fight!" play once. One player's failure drops only
  that player.

## 0.0.0.2 (dev)

- Internal: client state (`cl`, `clc`, cgame VM, connection state, input
  buttons) now lives in per-player contexts (`cl_splitscreen.c`). One
  player only so far; gameplay should be unchanged.
- Dev spike: console command `splitspike_connect` (while in a `devmap`)
  joins a headless second player over 127.0.0.1 with its own UDP socket
  and qport; `splitspike_disconnect` removes it. Netchan now uses a
  per-channel qport. Nothing changes unless the command is typed.
- Splitscreen spike: `splitspike_connect` now gives player 2 its own
  cgame and view — top half = player 1, bottom half = player 2, each with
  its own HUD (Vulkan and OpenGL). Drive player 2 from the console with
  `p2 <command>` (`p2 +forward`, `p2 -forward`, `p2 +attack`, `p2 kill`).
  `cl_splitAspect 1` (default) keeps Q3-style HUDs at 4:3 in each half
  with a wider view; `0` hands the mod the raw half-screen size. Only
  player 1's sounds play for now.

## 0.0.0.1 (dev)

- Baseline: unmodified upstream Quake3e built with VS2019 via
  `scripts\build.ps1`; version shown as `Q3 1.32e splitscreen-0.0.0.1`.
  Ships `quake3e.exe` (Vulkan), `quake3e-gl.exe` (OpenGL), `quake3e.ded.exe`.
