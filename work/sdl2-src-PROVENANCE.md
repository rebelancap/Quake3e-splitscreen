# SDL2 source (static gamepad backend of the Windows clients) — provenance

- What: SDL **2.32.10** source release (same version as the old runtime
  DLL, `work/sdl2-PROVENANCE.md`), built by `scripts\build.ps1` into the
  static libraries `work/sdl2-src/lib/SDL2-static.lib` (~9.0 MB, Release|x64, /MT) and
  `SDL2-static-debug.lib` (~31 MB, Debug|x64, /MTd; for Debug builds), VS2019
  v142 (objects in `work/sdl2-src/obj/<Cfg>/`); both Windows client exes
  link the one for their configuration (R15, 0.0.0.20). The dedicated exe does not.
- From: official libsdl-org GitHub release
  https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip
- Downloaded: 2026-10-06 into `work/sdl2-src/dl/`, extracted to
  `work/sdl2-src/SDL2-2.32.10/`.
- SHA-256 zip: `12b2dc2eb8f2836100a7916b5d394a0c82f1f7e32693f95f98305403af242f08`
  (pinned in `scripts\build.ps1`, which refuses and deletes a zip that does
  not match). libsdl-org also publishes `SDL2-2.32.10.zip.sig` (GPG); not
  checked here.
- Build: SDL's own unmodified `VisualC\SDL\SDL.vcxproj` with
  `/p:ConfigurationType=StaticLibrary /p:TargetName=SDL2-static` and
  `scripts\sdl2-static.props` injected (`SDL_STATIC_LIB`, `HAVE_LIBC`, no
  `DLL_EXPORT`, `/MT`, a few symbols renamed that the engine also defines).
- Licence: zlib (`work/sdl2-src/SDL2-2.32.10/LICENSE.txt`); static linking
  needs no notice in the binary distribution (an acknowledgement is
  appreciated, not required).
- Re-fetch: delete `work/sdl2-src/` and run `scripts\build.ps1` (it
  downloads, verifies and builds), or download the URL above, verify the
  SHA-256 and unzip into `work/sdl2-src/`.
- Not committed (work/ is gitignored except PROVENANCE files).
