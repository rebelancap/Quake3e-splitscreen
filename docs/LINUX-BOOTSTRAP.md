# Linux / Steam Deck bootstrap (M6) — brief for the Linux test box orchestrator

Written 2026-10-06 on the Windows dev box (Windows lead session). Read this first,
then `CLAUDE.md` (project charter), `STATUS.md` (state of the Windows
build: Current state, Open questions, Roadmap), and design
`docs/SPLITSCREEN-DESIGN.md` §14.5 (Linux/Deck) and the Linux paragraph at
the end of §17.2 (Independent mode). The program-wide rules for this box
are `docs/dev-CLAUDE.linux.md` — copy it to `~/dev/CLAUDE.md` on
the Linux test box if it is not there yet.

## Where things are

- **This box (the Linux test box):** GPD Win Mini 2025, Bazzite 43 (Kinoite,
  immutable Fedora, KDE Plasma), default user, home `~`,
  24 threads, 23 GB RAM, ~26 GB free. Toolchain found 2026-10-06: git
  2.53, gcc, make, podman, distrobox, flatpak, Claude Code
  (`~/.local/bin/claude`). **Missing:** cmake, ninja, clang, SDL2 headers
  (`pkg-config sdl2` fails). Reachable from the Windows dev box as `ssh <linux-box>`
  (over a private network). **Checked in L1 (2026-10-06):** the desktop session is KDE
  Plasma on **Wayland** (`kwin_wayland --xwayland`, XWayland on `DISPLAY=:0`,
  `WAYLAND_DISPLAY=wayland-0`), no Gamescope running; GPU AMD Radeon 890M
  (Strix, RADV `radeon_icd.x86_64.json`, Vulkan loader 1.4.341, llvmpipe
  also present); host runtime `libSDL2-2.0.so.0` = 2.32.x, `libGL.so.1`,
  `libvulkan.so.1` are all there. A session started over SSH has no display
  variables; export `DISPLAY=:0 WAYLAND_DISPLAY=wayland-0
  XDG_RUNTIME_DIR=/run/user/1000 XAUTHORITY=$(tr '\0' '\n'
  </proc/$(pgrep -o plasmashell)/environ | sed -n 's/^XAUTHORITY=//p')`
  to put the game window on the desktop session (or run from Konsole).
  Screen mode not read yet (xrandr is not on the host). The `q3dev`
  distrobox exists (created in L1; no git inside it, `scripts/build.sh`
  hands the commit in).
- **Repo:** `~/dev/Quake3e-splitscreen`, full history, branch
  `splitscreen` checked out = the Windows dev box's `splitscreen` at `b20b6bc0`
  (0.0.0.12) plus this brief. **No GitHub remote yet** (the maintainer creates the
  repo when both platforms have clean releases). Remote `upstream`
  (ec-/Quake3e) is not configured here; add it only if a round needs it.
- **Game data:** not on this box yet. The maintainer copies the retail baseq3 paks
  to `~/dev/q3data/baseq3/` (pak0.pk3 .. pak8.pk3). Use it read-only as
  `+set fs_basepath ~/dev/q3data` and always `+set fs_homepath
  <repo>/work/q3home`. Urban Terror (1.7 GB) and missionpack are not
  copied; UrT on Linux waits until baseq3 works.

## Sync model between the two boxes (no GitHub yet)

- The Windows dev box has this repo as remote `<linux-box>` and **pushes into it**
  (`receive.denyCurrentBranch = updateInstead`). A push onto the
  checked-out branch fails if that branch's working tree is dirty, so:
- **Work on branch `linux` here, never commit on `splitscreen` here.**
  `git switch -c linux` at the start; keep `splitscreen` clean so
  the Windows dev box can keep pushing Windows rounds into it. Merge `splitscreen`
  into `linux` whenever a new Windows round lands (`git merge splitscreen`).
- The Windows dev box fetches your work with `git fetch <linux-box> linux` and
  merges it into `splitscreen` there. Linux never needs to reach
  the Windows dev box (it has no sshd).
- Tell the maintainer at the end of each round what is on `linux` and whether it
  is ready to merge; the Windows lead merges.

## Versioning

Shared counter: dev builds stay `0.0.0.N`. Bump `VERSION` by one per Linux
round as on Windows; if both boxes bumped to the same number, the merge on
the Windows dev box re-numbers the Linux one. Stamp it into `BUILD-INFO.txt` and
the log header as the Windows build does.

## Build plan (default; the maintainer may prefer rpm-ostree layering — ask once)

Bazzite is immutable: do not `dnf install` on the host. Default = a
distrobox:

```
distrobox create -n q3dev -i registry.fedoraproject.org/fedora:43 -Y
distrobox enter q3dev -- sudo dnf install -y gcc make pkgconf-pkg-config \
    SDL2-devel libX11-devel libXext-devel mesa-libGL-devel \
    vulkan-headers vulkan-loader-devel libogg-devel libvorbis-devel \
    libcurl-devel
distrobox enter q3dev -- make -j24 BUILD_SERVER=1 BUILD_CLIENT=1
```

Upstream `Makefile` already lists our client files (`cl_splitscreen.o`,
`cl_splitui.o`, `cl_splitmenu.o`, `cl_splitprofile.o`, `in_gamepad.o`,
`cl_aimassist.o`, `cl_splitindep.o`); `win_splitproc.o` is win32-only.
Output (L4 onwards, upstream's release layout, `USE_RENDERER_DLOPEN=0`):
`build/release-linux-x86_64/quake3e-vulkan-ss.x64` (Vulkan built in),
`quake3e-ss.x64` (OpenGL built in), `quake3e-ss.ded.x64` (stripped) +
`BUILD-INFO.txt`; objects in `build/obj/{vk,gl}/`. (L1-L3 built one
`quake3e.x64` with dlopen'd renderer `.so` files and `+set cl_renderer`.)
**L1 made this one command: `scripts/build.sh [debug]`** (re-enters the
box, stamps `VERSION` into the binary via `SPLITSCREEN_VERSION`, writes
`BUILD-INFO.txt`); binaries stay in `build/` (gitignored). The built
binary runs **on the host**: `ldd` shows only the host's
`libSDL2-2.0.so.0` beyond libc.

## Expected porting work (from a code survey on 2026-10-06)

- `code/client/in_gamepad.c` already dlopens `libSDL2-2.0.so.0` on
  non-Windows and `code/sdl/sdl_input.c` already calls `IN_GamepadMove`
  (line ~878). Design §14.5 says: on Linux the engine itself uses SDL, so
  the pad code must share the engine's SDL (no second init / no
  `SDL_Quit` of its own). Audit `SDL_InitSubSystem`/`SDL_Quit` use.
- `code/client/cl_splitindep.c` has ~54 Windows references and
  `cl_splitprofile.c` ~9; `cl_splitscreen.h:318-327` declares the
  `Sys_Split*` process functions that `code/win32/win_splitproc.c`
  implements. **Round 1:** a `code/unix/unix_splitproc.c` with stubs that
  make Independent mode report "not available on this platform" and keep
  Together mode working; `#ifdef` the rest. **Later (L3):** real
  implementation per §17.2's Linux paragraph (fork/posix_spawn of
  `/proc/self/exe`, `PR_SET_PDEATHSIG`, `waitpid`, SDL borderless window
  positioning on X11/XWayland; Gamescope = Together-only fallback).
- The UDP IPC in Independent mode is BSD-socket code and should port as
  is.
- Test runners under `scripts/tests/` are Git Bash scripts with Windows
  exe paths (`build\Release\...`, `tasklist`/`taskkill`); port the
  handful you need (`s6-run.sh`, `r10-run.sh`, `r12-run.sh`) with an
  `EXE` variable and `pgrep`/`pkill`; the `.cfg` scripts are portable.

## Rounds (one executor at a time; Opus executes, Sonnet reviews lightly)

Status 2026-10-07: **L1-L5 done; L6 done (2026-10-07)**: 0.0.0.27 builds
and passes the Linux regression pass (STATUS.md Last round). L4's game-mode
checks and L6's play check are the maintainer's (STATUS.md Next steps L-8, L-12).
Linux runners: `scripts/tests/linux-run.sh` (Together),
`scripts/tests/l3-run.sh` + `l3-*.sh` (Independent mode),
`scripts/tests/l6-r19-run.sh` (Independent mode on the R19 test device bus).

- **L1 — builds and boots.** distrobox + `make`; stubs for `Sys_Split*`;
  flat windowed run (`+set r_fullscreen 0 +set r_mode -1 +set
  r_customwidth 1280 +set r_customheight 720 +set logfile 3 +set
  in_gamepad 0 +set developer 1 +devmap q3dm1`), Vulkan and OpenGL;
  engine `screenshotJPEG` + `qconsole.log` into `work/`. Done = both
  renderers boot a map, 0 new compiler warnings in our files.
- **L2 — pads and the headless suite.** Pads through the engine's SDL;
  `padinject` virtual pads; port and run `r10-single`, `r8-join`,
  `r10-layouts` (q3dm7), `r12-aim` (compare `r12-table.sh` numbers with
  Windows' in STATUS.md), `r10-perf`. Done = same behaviour as the Windows
  log for each, cited in STATUS.md.
- **L3 — Independent mode on the desktop session** (X11/XWayland only),
  per §17.2 Linux paragraph; Gamescope detected -> Session mode row says
  unavailable.
- **L4 — Deck game mode.** Launch as a non-Steam game under Gamescope:
  Steam Input pads, 1280x800 layouts (`cl_splitFill`, wide player),
  Steam's on-screen keyboard vs ours, suspend/resume. Packaging (decided
  by the maintainer in L4, replaces the earlier launcher + install-script plan):
  exactly like upstream's Linux zip -- `scripts/package.sh` makes a tar.gz
  holding only the three executables at its root (no launcher, no install
  script, no text files); users drop them next to `baseq3/` (the engine's
  default `fs_basepath` is the executable's own folder, `Sys_Pwd()` =
  dirname of `/proc/self/exe`) and launch
  `quake3e-vulkan-ss.x64`. Releases are built by
  `.github/workflows/release.yml` on a `v<VERSION>` tag.
- **The maintainer's hands-on pass** on the Win Mini: the Windows pad checklist in
  STATUS.md Next steps applies unchanged (items 1-24, 32-46); add Deck
  items (game mode, docked pads, Steam Input layouts).

## Testing rules on this box

Same as the charter: windowed run, evidence via the engine's own
screenshot command and `qconsole.log` copied into `work/`, one game
instance at a time, `pgrep quake3e` before launching, never take desktop
screenshots or steal focus (the maintainer may be using the Win Mini), kill what
you launched, STATUS.md updated at the end of every round. The game needs
a display: run the orchestrator session in Konsole inside the desktop
session (not over plain SSH) so `DISPLAY`/`WAYLAND_DISPLAY` are set.

## Transferring the maintainer's Windows feedback

The maintainer has not yet done the Windows pad pass. Everything it will tune
(aim-feel defaults, Guest defaults, pad layouts, overlay look, join hint,
aim assist Low/Off) lives in shared client code and cvar defaults
(`cl_split*`, `joy_*`, `cl_aimAssist*`, `default_pad.cfg` data), not in
Windows code, so it lands here through a normal `git merge splitscreen`.
Only Independent mode's window/process layer is platform-specific. Do
not wait for his pass; do not pre-empt it either (keep defaults as they
are on `splitscreen`).

## Round L6 (2026-10-07) — release candidate check of 0.0.0.27

Windows rounds R14a-R21 (0.0.0.18 -> 0.0.0.27, 82 commits) landed on
`splitscreen` after the last Linux merge and have never been built on Linux.
The maintainer's Windows passes are clean; the GitHub repo and first release come
right after this round. Do, in order:

1. `git merge splitscreen` into `linux` (fast-forward: `linux` has nothing
   newer). `scripts/build.sh`; fix any Linux-only compile error in our files
   (new since L5: `code/qcommon/files.c` `FS_UrTDetect`, `common.c`
   `Com_UrTHunkDefault`/log reopen in `Com_GameRestart`, `cl_main.c` UrT
   download rules, `in_gamepad.c` stable pad keys from the SDL device path
   (`SDL_JoystickPathForIndex`, SDL >= 2.24) + cinematic skip + `padbus`,
   `cl_splitindep.c` tile-as-cell + pad key lists, `cl_splitui.c` first-launch
   hint, `sv_client.c` loopback getchallenge in Single Player). Commit fixes
   on `linux`; keep them minimal and `#ifdef`-free where possible.
2. Run the headless runners that exist for Linux (`linux-run.sh`, `l3-run.sh`)
   plus the portable `.cfg` tests from R17-R21 that need no Windows paths
   (`r17-strings`, `r17-team`, `r19-pads` via `padbus`, `r20-cin`,
   `r20-sp`); evidence in `work/`, cited in STATUS.md.
3. Leave the build installed for the maintainer's play check on the Win Mini
   (Together 2-4 pads, menus, server options, Independent 2p tile aspect,
   intro skip). UrT is not on this box; skip it.
4. Report what is on `linux` and whether it is ready; the Windows lead
   merges, then tags the release and GitHub Actions builds both platforms
   (the Linux job now builds inside an Ubuntu 22.04 container for older
   glibc). The Win Mini stays the Linux play-test box, not the release
   builder.
