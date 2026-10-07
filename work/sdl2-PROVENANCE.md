# SDL2 runtime (gamepad backend) — provenance

**Superseded in R15 (0.0.0.20):** the Windows clients link SDL2 statically
(`work/sdl2-src-PROVENANCE.md`); `SDL2.dll` is no longer copied or shipped.
`work/sdl2/` is kept only as the reference the old builds used (safe to delete).

- What: `SDL2.dll` x64 runtime, SDL **2.32.10** (latest SDL 2.x release at download time).
- From: official libsdl-org GitHub release
  https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10-win32-x64.zip
- Downloaded: 2026-10-05 into `work/sdl2/dl/`, extracted to `work/sdl2/`.
- SHA-256 zip: `6cf9706eefd0a4a06dc764007934d428afaf029fabdd408a9e646048c91e18fb`
- SHA-256 SDL2.dll: `b37740a72a7a9706216df9f0134894bb7a850b356fd149398c67d874cbcfacb4`
- Upstream git hash (`.git-hash` in the zip): see `work/sdl2/.git-hash`.
- Licence: zlib (see `work/sdl2/README-SDL.txt`; https://www.libsdl.org/license.php).
  Redistributing the DLL beside our exe is allowed; a release zip must include it.
- Re-fetch: download the URL above, verify the SHA-256, unzip into `work/sdl2/`.
  `scripts\build.ps1` copies `work\sdl2\SDL2.dll` into `build\<Cfg>\`.
- Not committed (work/ is gitignored except PROVENANCE files).
