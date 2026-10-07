# ~/dev — program-wide rules (the Linux test box: GPD Win Mini 2025, Bazzite)

Linux counterpart of the Windows `~/dev/CLAUDE.md` on the Windows dev box. Same
program structure: orchestrator / executor / reviewer, STATUS.md,
versioning, git safety. Copy this file to `~/dev/CLAUDE.md` on this box.

## Machine facts (2026-10-06)

- Bazzite 43 (Kinoite, immutable Fedora, KDE Plasma), default user,
  home `~`. Steam Deck is the target this box stands in
  for; it also runs Steam in game mode (Gamescope).
- Toolchain: git, gcc, make, podman, distrobox, flatpak, Claude Code. No
  cmake/ninja/clang, no -devel headers on the host. **Build inside a
  distrobox** (`q3dev`, Fedora 43) — never `rpm-ostree install` or layer
  packages without the maintainer's OK (needs a reboot). Project prerequisites
  are documented in the project's STATUS.md.
- Shell: bash. Scripts are `.sh`.

## Delivery

GitHub releases (tar.gz of the binary + launcher/install script) once
the maintainer creates the repo; until then local builds in the repo's `build/`.
Install scripts copy beside the game data and never touch original files.
Every build's "what changed" goes in `CHANGELOG.md`.

## Testing

- Agents: windowed run on the desktop session, evidence via the app's own
  screenshot path and logs into `work/`, cited in STATUS.md. Nothing is
  "verified" on a green build alone.
- Only one instance of a game at a time; `pgrep` before launching; never
  kill a process you did not start (the maintainer may be playing). Never take
  desktop screenshots or force a window to the foreground.
- Hands-on checks (real pads, game mode, Steam Input) are the maintainer's: leave
  the build installed and the checklist in STATUS.md Next steps.
- Always clean up: kill what you launched, restore any settings you
  edited, delete temp captures not cited by STATUS.md.

## Versioning, orchestration, STATUS.md, end-of-session, git safety, asking the maintainer

Identical to the Windows rules — summarised:

- Versioning: dev builds `0.0.0.N` until the maintainer picks a release number;
  patch bumps for small updates; release numbers and timing are the maintainer's.
- Roles: Fable orchestrates/plans in the lead session; Opus executes as
  background agents with self-contained briefs; Sonnet reviews lightly
  (real bugs only). Fresh agent per round. Executors verify their own
  work; nothing ships unseen.
- STATUS.md sections: Current state, Last round, Next steps, Open
  questions (each with a default), Live claims — updated at the end of
  every round.
- End of session: processes stopped, work committed on a branch (or
  stash-backed), STATUS.md current, no agents left running, scratch
  deleted.
- Git: commit early on branches; stash-backup before rebases; never
  force-push shared branches; never commit game data. Commit as
  `rebelancap <rebelancap@protonmail.com>`.
  Public repo: no personal names, user paths, e-mails or host names in
  files or commit messages; use `<repo>`, `<Quake3 install>`, 'the
  maintainer', `$Q3_BASEPATH`.
- The maintainer is around but not watching: batch questions, state the default,
  never block holding resources. Always wait for the maintainer on: release
  numbers/timing, system-wide software (rpm-ostree, drivers), anything
  destructive outside the project tree, anything touching online play.
- Disk: build output in `build/`; big downloads get a
  `work/<name>-PROVENANCE.md`; never duplicate a game install.
