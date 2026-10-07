# ~/dev/q3data (outside the repo)

- `baseq3/pak0.pk3` .. `pak8.pk3` (483 MB): the maintainer's retail Quake III Arena
  1.32 paks, copied 2026-10-06 with `scp` from
  `<Windows dev box>:<Quake3 install>/baseq3/`
  (his install, read-only there; custom maps, autoexec/q3config and
  missionpack/UrT deliberately not copied -- STATUS Open questions, Linux (b)).
- Used read-only as `+set fs_basepath ~/dev/q3data`; homepath is always
  `<repo>/work/q3home`. Never committed. Delete with `rm -rf ~/dev/q3data`.
