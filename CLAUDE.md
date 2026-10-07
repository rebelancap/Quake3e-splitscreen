# Quake3e-splitscreen — project charter

`~/dev/CLAUDE.md` applies unless overridden here. Versioning follows the
program-wide rules (dev builds `0.0.0.N` until the maintainer picks a release
number; the version lives in `VERSION` at repo root). `STATUS.md` at repo
root is kept current at the end of every round.

## What this is

A fork of [ec-/Quake3e](https://github.com/ec-/Quake3e) (remote
`upstream`, branch `splitscreen`) adding local splitscreen (4 players
first, then 8) and gamepad support with automatic pad assignment and
hold-to-join. Must work with unmodified mods (Urban Terror is the named
target) and, eventually, on unmodified online servers.

## Architecture decision (2026-10-05)

**Multi-client-instance, not Spearmint-style.** Spearmint changed the
cgame/game VM API so one client carries N local players; that breaks
every existing mod and every existing server. Instead each local player
is a complete virtual client inside one process: its own
`clientActive_t` / `clientConnection_t`, its own cgame VM instance, its
own netchan (own UDP source port + qport), rendered into its own
viewport. The mod and the server each see N ordinary players, so
closed-source mods and remote servers work unchanged. Renderer, sound
device, filesystem, UI VM, and console stay single and shared.
`work/spearmint-ref/` (gitignored, see PROVENANCE) is reference for
UX ideas only — do not port its VM API changes.

Keep the diff against upstream contained: prefer new files
(`cl_splitscreen.c`, `in_gamepad.c`, …) and thin hooks in existing ones,
so upstream merges stay cheap and a PR remains thinkable.

## Overrides of ~/dev/CLAUDE.md

- **Testing (overrides the VR testing paths):** this is not a VR
  project; ignore SteamVR/null-driver/simulator. Agents test with a
  windowed run (`+set r_fullscreen 0 +set r_mode -1 +set r_customwidth
  1280 +set r_customheight 720`), evidence via the engine's own
  `screenshot` command and `qconsole.log` (`+set logfile 2`), copied
  into `work/`. Extra local players can be driven without pads via
  console commands / bots. Real multi-gamepad checks are the maintainer's —
  put them in STATUS.md Next steps as a checklist. The "one game
  instance", "don't interrupt the maintainer's screen", and cleanup rules still
  apply.
- **Delivery:** no game folder is patched; the build is a standalone
  engine exe. Local dev builds land in `build/`; an install script (when
  a game dir is known) only copies our exe + `BUILD-INFO.txt` beside the
  game data and never touches existing files.

## Build

VS2019 (v142, x64) via `scripts\build.ps1` → `build\`. See STATUS.md for
the current exact command and any prerequisites.

## Game data

No game data is committed, ever. The maintainer's installs are configured
locally (outside the repo); scripts read them from `$Q3_BASEPATH` (a
retail Quake III folder: paks + osp, devotion, missionpack) and
`$URT_BASEPATH` (Urban Terror 4.3). Use `<Quake3 install>` read-only as
`+set fs_basepath`, and always pass
`+set fs_homepath <repo>\work\q3home` so configs, logs and screenshots
land in our tree and the install's `q3config.cfg` is never touched. The
maintainer's Spearmint setup (pad-bind reference:
`spearmint-4p-controller-binds.cfg`, `gamepad-controls.txt`) sits in a
sibling `Spearmint` folder, read-only. `<UrbanTerror43 install>` (`q3ut4\`)
is read-only as `+set fs_basepath` with `+set fs_basegame q3ut4` and
`+set fs_homepath <repo>\work\urthome` (never its own `Quake3-UrT.exe`).

## Git identity

Commit as `rebelancap <rebelancap@protonmail.com>` only.

Public repo: no personal names, user paths, e-mails or host names in files
or commit messages; use `<repo>`, `<Quake3 install>`, 'the maintainer',
`$Q3_BASEPATH`.

## Design

`docs/SPLITSCREEN-DESIGN.md` is the implementation design (context
model, VM instancing, viewports, networking, input, milestones/spikes).
Executors read the sections their brief names.
