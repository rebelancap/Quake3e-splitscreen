# Urban Terror 4.3 with Quake3e-splitscreen

Quake3e-splitscreen runs Urban Terror 4.3 as it is, with no changes to the game:
local splitscreen for 2 to 8 players with gamepads, and online play.

## Launching

**The easy way:** copy the three executables (`quake3e-vulkan-ss.x64.exe`,
`quake3e-ss.x64.exe`, `quake3e-ss.ded.x64.exe`) into your Urban Terror 4.3
folder, the one that holds `q3ut4\`, and start `quake3e-vulkan-ss.x64.exe`. Use
`quake3e-ss.x64.exe` if you want OpenGL instead of Vulkan.

The engine sees a `q3ut4` folder and no `baseq3` folder beside itself, so it
starts Urban Terror. The console log says
`urt: detected q3ut4 install, fs_basegame q3ut4`. Your Urban Terror files are
not changed. Our settings are saved to `q3ut4\q3config-ss.cfg`, which is
created from UrT's own `q3config.cfg` the first time you run it. UrT's own
`Quake3-UrT.exe` keeps working beside ours.

**The explicit way** works from anywhere. This is the documented flag:

    quake3e-vulkan-ss.x64.exe +set fs_basepath "C:\Games\UrbanTerror43" +set fs_basegame q3ut4

**From a Quake 3 install:** if `q3ut4` is inside your Quake 3 folder next to
`baseq3`, pick Urban Terror in the game's Mods menu, or start with
`+set fs_game q3ut4`.

**Memory:** Urban Terror needs a larger hunk than Quake 3. When the game is
UrT, the engine raises `com_hunkMegs` to 1024 MB, which is enough for 8 players
(the log says `urt: com_hunkMegs <old> -> 1024`). It never lowers the value.
`+set com_hunkMegs <n>` on the command line still wins. If you switch to UrT
through the Mods menu, the hunk already in use stays the same size; start
directly into UrT if you want 8 players.

## Gamepad layout (built in)

| Button | Action |
|---|---|
| A | Jump |
| B | Crouch (toggle) |
| X | Reload |
| Y | Bandage |
| D-pad up | Drop item |
| D-pad down | Drop weapon |
| D-pad left | IR vision (night vision goggles) |
| D-pad right | Weapon mode |
| Right stick click | Knife |
| Left stick click | Sprint (toggle) |
| LB | Reset zoom |
| RB | Next weapon |
| LT | Zoom in (each press zooms one step further) |
| RT | Fire |
| Start | Pause menu |
| Back / View | Scores |
| Share / Misc | Use current item |
| Left / right stick | Move / look |

**Toggle or hold:** crouch and sprint are toggles by default. Click once to
crouch or sprint and click again to stop. Sprint also stops by itself when you
let go of the left stick. Each player can switch either one to Hold on the pause
menu's Controls page (rows **Crouch** and **Sprint**). The setting is saved in
that player's profile, separately for each game, so Quake 3 keeps crouch on Hold.

**Rebinding:** pause menu (Start) -> Controls -> Button bindings lists every
Urban Terror action. Unbound actions show `--`, for example interact / pick up,
zoom out, previous weapon, weapon slots, items, team, gear and radio menus,
radio calls, chat and votes. Pick an action and press the button you want for
it. If that button already had an action, the two swap. **Reset buttons to
defaults** goes back to the layout above.

Profiles keep their own buttons for each game. If a profile still has the
earlier built-in UrT layout from before this version and was never edited, it
switches to the new layout by itself. A profile you edited keeps your buttons;
use **Reset buttons to defaults** on it to get the new layout.

## What else is different from Quake 3

- The pause menus are blue in Urban Terror (red in Quake 3). Set your own
  colour with `cl_splitMenuColor "r g b"` (values 0 to 1), or `auto` for the
  game's colour.
- Sniper scopes cover each player's whole view. In a wide or tall view they no
  longer show the zoomed world beside the scope.
- Some Server options rows that do nothing in UrT are greyed out:
  - Quad damage, Power-ups, Player speed and Weapon respawn (UrT has no such
    items, and its movement ignores `g_speed`)
  - Weapons, Player weapons at spawn, Infinite ammo, Self-damage and God mode
  - Instagib uses UrT's own `g_instagib`.
- Each player's team and gear menus open in that player's own view.

## Online

You can join online UrT 4.3 servers, including with extra local players, if
the server allows that many players from one address. Some UrT servers run an
admin bot that kicks clients it does not recognise. The message is
`Non whitelist client ...`. It reads a connect field that Quake3e sends and
UrT's own client does not, so in Urban Terror our client no longer sends it.
The server then sees the same thing UrT's own client sends.

**Missing maps download automatically, the way UrT 4.3's own client does it.**
When a server runs a map you do not have, the game downloads that map's pak
and then joins:

- **The switch is UrT's own `cl_autodownload`** (default 1; it is the
  "auto download" option in UrT's settings). UrT's settings file sets
  `cl_allowdownload 0`; that setting is not the switch in Urban Terror and is
  left alone.
- **Only the map's pak is downloaded** (`<map>.pk3`). Other paks the server
  lists but you lack are skipped, as UrT's client does.
- **It downloads over HTTP from the server's web address (`sv_dlURL`).** Most
  UrT servers have one (for example `74.91.113.242/maps`). The server's
  `sv_allowDownload` is 0 on UrT servers (UrT's server game sets it to 0), and
  that only matters for downloads through the game connection. Those are
  used only when a server has no `sv_dlURL` but allows them.
- **The file is saved to `q3ut4\download\`** in your home folder, like UrT's
  client. When the exes sit in your UrT folder, that is UrT's own
  `q3ut4\download\`. The maps UrT's client already downloaded there are used
  too, so they are not downloaded again. Nothing that runs code or changes
  settings (`.qvm`, `.menu`, `.cfg`) is loaded from that folder.
- **While downloading,** the loading screen shows the file name, the speed and
  the percentage. Afterwards the game reconnects by itself and joins the map.
- **If the server offers no download** (no `sv_dlURL` and no game-connection
  downloads), the error says so and names the map, instead of
  `CM_LoadMap: couldn't load maps/<map>.bsp`. Get the map from an Urban Terror
  map site and put it in `q3ut4\download\`.

The console log shows every decision. At start:
`download: Urban Terror: map downloads on (cl_autodownload 1; ...)`. When
joining: `download: server sv_allowDownload=0 sv_dlURL=<url>; missing: <paks>;
method: http (...)`, then `download: URL <url>` while downloading. After the
reconnect it shows `missing: none`.

**Why 0.0.0.26 still failed:** a UrT server tells the client its game folder
is `q3ut4`. Our client runs UrT as the base game, which it names
differently, so it restarted the whole game at every join. That restart
re-ran UrT's settings file, which turned downloads off again, and it also
stopped the console log. Joining no longer restarts anything
(`fs_game: server's "q3ut4" is our base game, no game restart`), and the log
now continues after any game restart.

**The auth message.** At every start the console says
`This game client is not auth capable. You will have to use the official game
client instead to be able to auth.` That is UrT's account-login notice: it
does not stop you joining servers that do not require an account.

## Known limits

- **No UrT account authentication.** Quake3e is not UrT's engine fork, so
  `auth` / `authc` login does not work. Servers that require an authenticated
  account will refuse you. A server hosted from this game shows
  `auth_status off (bad engine)`.
- Joining online servers and the map download have not been tested on a live
  server yet (they were tested against a local server set up like one).
- `Quake3-UrT.exe` and our executables keep separate settings files
  (`q3config.cfg` and `q3config-ss.cfg`).
