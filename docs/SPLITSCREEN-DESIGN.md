# Splitscreen design — multi-client-instance

Status: design, 2026-10-05, against upstream `2b375bd1`. Line numbers are
for that commit and will drift; function names are the stable anchor.
Architecture decision is in `CLAUDE.md`: each local player is a complete
virtual client (own `cl`/`clc`, own cgame VM, own netchan + UDP port) in
one process; renderer, sound device, FS, UI VM, console are shared.
Nothing in this survey contradicts that decision.

Terms: **context** = one virtual client (`clx[i]`, i = 0..N-1; context 0
is the "primary"). **Active context** = the one the `cl`/`clc`/`cgvm`
names currently resolve to. `MAX_SPLITVIEW` = 8 from day one (arrays are
sized for 8; M1–M4 just never start more than 4).

---

## 1. Client context model

### 1.1 What exists

All client state is three globals plus statics:

- `clientActive_t cl` (`cl_main.c:97`, struct `client.h:92-145`) — per
  gamestate: snapshots, `parseEntities` (~1.7 MB), baselines, usercmds,
  viewangles, mouse/joystick accumulators. **Per player.**
- `clientConnection_t clc` (`cl_main.c:98`, `client.h:165-255`) —
  per connection: `clientNum`, challenge, reliable cmds, server cmds,
  download, demo, `netchan`. **Per player.** (~150 KB)
- `clientStatic_t cls` (`cl_main.c:99`, `client.h:293-355`) — mostly
  shared (renderer/sound/ui started flags, glconfig, realtime, server
  browser), **but** `cls.state`, `cls.servername`, `cls.cgameStarted`,
  `cls.gameSwitch`, `cls.startCgame` are really per connection.
- `vm_t *cgvm` (`cl_main.c:100`) — **per player.**
- File statics that are per player:
  - `cl_input.c:57-61` kbuttons (`in_left` … `in_buttons[16]`),
    `in_mlooking` (:89), `frame_msec`/`old_com_frameTime` (:26-27, used
    by `CL_CreateNewCommands`; a second context in the same frame would
    see 0 msec).
  - `cl_keys.c:789` `keyCatchers` — shared, except the `KEYCATCH_CGAME`
    bit, which is per context.
  - `qcommon/keys.c:28` `keys[MAX_KEYS]` (down state + bindings) — see §6.
  - `cl_parse.c:330-331` `cl_connectedToPureServer`,
    `cl_connectedToCheatServer` — same server for all → shared.
  - `cl_cgame.c` has no per-player statics beyond what it reads from
    `cl`/`clc`/`cgvm`.
  - `snd_dma.c:70-72` `listener_number/origin/axis` — see §7.
- Shared and staying shared: `uivm`, `re`, console, `download` (cURL),
  server browser lists, `cl_pinglist`, cvars, command buffer, FS.

### 1.2 Mechanism: context array + active-pointer macros

**Recommended.** In `client.h`, replace the two `extern` lines and the
`cgvm` extern:

```c
#define MAX_SPLITVIEW 8
typedef struct clientContext_s {
    clientActive_t      cl;        // big; allocate lazily (see below)
    clientConnection_t  clc;
    vm_t               *cgvm;
    clsShadow_t         s;         // per-connection slice of cls (1.3)
    inputState_t        in;        // kbuttons etc. (section 6)
    viewRect_t          view;      // viewport in real pixels (section 3)
    int                 playerNum; // 0-based local player index
    qboolean            inUse;
    netsrc_t            sock;      // NS_CLIENT + n (section 4)
    int                 qport;
} clientContext_t;

extern clientContext_t *clx[MAX_SPLITVIEW];
extern clientContext_t *cla;           // active context, never NULL
#define cl   (cla->cl)
#define clc  (cla->clc)
#define cgvm (cla->cgvm)
```

- Diff at use sites: **zero** — the ~1500 existing `cl.` / `clc.` /
  `cgvm` references compile unchanged. Only the definitions in
  `cl_main.c:97-100` change.
- Contexts are `Z_Malloc`'d (or `calloc`) on first use, ~2 MB each;
  context 0 is static so the engine behaves exactly as upstream when
  nobody joins.
- Token-collision check (spike S1): the macros must only be visible to
  files that include `client.h`. Known local variables named `cl` in
  those files: `win32/win_snd.c:315,321` (`NotificationClient_t *cl`) —
  rename the local if `win_snd.c` sees `client.h`. `server/*.c` uses
  `cl` for `client_t*` everywhere but does not include `client.h`
  (verify: `grep -l client.h code/server` must stay empty).
  `Com_Memset( &clc, 0, sizeof( clc ) )` (`CL_Disconnect`) and
  `CL_ClearState` still work through the macro.
- Switching: `CL_SetContext( int i )` — sets `cla`, swaps the cls shadow
  (1.3) and input statics. All per-context work is wrapped as
  `for each inUse context: CL_SetContext(i); <existing function>();`
  and the previous context is restored on exit (`CL_PushContext` /
  `CL_PopContext` pair, depth-1 stack is enough).

Alternative rejected: passing a context pointer through every function
(huge diff, unmergeable); memcpy-swapping 2 MB structs (too slow at 8
players × several switches per frame).

### 1.3 Per-connection fields living in `cls`

`cls.state` is referenced ~150× and is also read by the UI
(`UI_GETCLIENTSTATE` in `cl_ui.c`) and renderer-facing code. Do **not**
rename. Instead keep `cls` as is and save/restore a small shadow on
context switch:

```c
typedef struct { connstate_t state; qboolean gameSwitch, cgameStarted,
                 startCgame; char servername[MAX_OSPATH]; } clsShadow_t;
```

`CL_SetContext` copies `cls.{state,...}` out to the old context's shadow
and in from the new one (a few bytes + one 256-byte string; skip the
string copy by keeping `servername` shared — all contexts always target
the same server). Outside any per-context loop the active context is
context 0, so shared code (`SCR_DrawScreenField`'s `switch(cls.state)`,
UI connect screen, `CL_Frame`'s main-menu check `cl_main.c:~3020`) sees
the primary's state, which is the desired behaviour.

`cls.rendererStarted / soundStarted / soundRegistered / uiStarted`,
`glconfig`, `realtime`, `frametime`, `framecount` stay shared.

### 1.4 Frame flow (`CL_Frame`, `cl_main.c:2985`)

Per-context steps, each wrapped in the context loop, same order as today:
`CL_CheckUserinfo` → `CL_CheckTimeout` → `CL_SendCmd` →
`CL_CheckForResend` → `CL_SetCGameTime`. Then one shared
`SCR_UpdateScreen` (which loops contexts for the cgame draw, §3), one
`S_Update`, one `Con_RunConsole`. `cls.realtime += msec` happens once.
`frame_msec` in `cl_input.c` is computed once per frame, before the loop.

Packet receive: `CL_PacketEvent( from, msg )` gains the context via the
socket it arrived on (§4): `Com_EventLoop` (`common.c:~2975`) calls
`CL_PacketEventCtx( ctxIndex, from, msg )` which does
`CL_SetContext(i); CL_PacketEvent(...)`.

Secondary contexts never: play/record demos, download, run cinematics,
request authorization, restart the FS, or change `fs_game` (they connect
only after context 0 is `CA_ACTIVE`, so the game dir and paks are already
right; `CL_SystemInfoChanged`/`CL_ParseGamestate` for i>0 must skip the
`fs_game`/pure-pak/`Cvar_SetSafe` side effects — guard with
`if ( cla->playerNum == 0 )`).

---

## 2. VM instancing

### 2.1 What exists

- `vm.c:215` `static struct vm_s vmTable[VM_COUNT]`, one slot per
  `vmIndex_t` (`qcommon.h:389-395`: `VM_GAME, VM_CGAME, VM_UI`).
  `VM_Create` (`vm.c:1832`) returns the existing vm if the slot's `name`
  is set — a second `VM_Create( VM_CGAME, … )` yields the **same** VM.
- QVM: data segment `Hunk_Alloc` (`vm.c:866`), jump tables on hunk;
  compiled code via `VirtualAlloc` (`vm_x86.c:4502`). The x64 compiler's
  statics (`vm_x86.c:213` `instructionPointers`, etc.) are compile-time
  scratch only; generated code addresses its own `vm_t` (`vm_x86.c:2421`
  calls through `&vm->systemCall`). So N compiled instances of the same
  `.qvm` are independent. Interpreter (`vm_interpreted.c`) likewise
  takes `vm_t*`.
- DLL: `VM_LoadDll` → `Sys_LoadLibrary` → `LoadLibrary` (`win_main.c:
  490`). Loading the same path twice returns the **same module** with
  one set of globals → a native `cgame` DLL cannot be instanced N times.
- Syscall dispatch: `vm->systemCall( args )`; `CL_CgameSystemCalls`
  (`cl_cgame.c:477`) receives only `args` — no vm pointer.
  `VM_CHECKBOUNDS( cgvm, … )` and `VMA()` (`VM_ArgPtr`, `cl_cgame.c:412`)
  read the global `cgvm`.

### 2.2 Design

- Extend the table, not the API: add `VM_CGAME2 … VM_CGAME8` to
  `vmIndex_t` after `VM_UI` (so existing values are unchanged) and map
  them all to the name `"cgame"` in `vmName[]`. `VM_Create( VM_CGAME +
  slot, … )` then gives an independent instance. `vm.c:1677`'s
  `vm->index == VM_CGAME` checks (trap-range validation) become an
  `IS_CGAME_INDEX()` macro. ~20 lines in `vm.c`/`qcommon.h`.
- Syscall routing: **the active context is the caller.** Every entry
  into a cgame VM goes through `VM_Call( cgvm, … )` with `cgvm` being
  the macro → all such calls are already inside a `CL_SetContext`
  scope, so inside `CL_CgameSystemCalls` `cl`/`clc`/`cgvm` resolve to
  the calling player with no changes. Invariant to enforce (assert in
  debug): `VM_Call` on a cgame vm only while `cla->cgvm == vm`.
- Native DLL cgame: for secondary contexts force QVM
  (`interpret = VMI_COMPILED`) like the pure-server path already does
  (`cl_cgame.c:873-879`). If a mod ships **only** a DLL: copy it to
  `<homepath>/<game>/cgame_p<N>.dll` and load the copy (distinct module
  → distinct globals). Implement only if a target mod needs it; UrT 4.x
  ships QVMs.
- Memory: baseq3 cgame ≈ 3–5 MB hunk per instance (data + jump targets)
  plus compiled code off-hunk; UrT's is larger (~10–16 MB). Raise
  `DEF_COMHUNKMEGS` (`common.c:49`, 128) to 256 and keep
  `com_hunkMegs` user-settable; 8 UrT cgames + renderer data need
  headroom. `CG_MEMORY_REMAINING` is shared hunk — fine.
- Hunk lifetime: cgame VMs are allocated on the low hunk and freed by
  `Hunk_Clear` (`CL_ClearMemory`) at map change — same as today, just N
  of them. A **mid-map join** allocates a new cgame on the hunk after
  the level is loaded; that is legal (h_low keeps growing) but the
  memory is only reclaimed at next map load. Drop-out calls
  `CG_SHUTDOWN` + `VM_Free` (frees compiled code; hunk part leaks until
  map change — acceptable, bounded by 8).
  **As built (R9):** unbounded in practice — every in-place restart
  (join, leave, layout change, a player's ui VM restarting for a new cell
  size) took a fresh copy (~4 MB cgame, ~2 MB ui), so ~30 changes exhausted
  the hunk. Fix: `VM_HunkAlloc` (`vm.c`) — `VM_Free` hands a QVM's hunk
  blocks (data segment, jump-table targets, interpreter code /
  instruction pointers) to a per-`vmIndex_t` cache stamped with
  `Hunk_Generation()` (bumped by `Hunk_Clear`/`Hunk_ClearToMark`); the
  next `VM_Create` of the same index in the same generation reuses them
  (zeroed) instead of `Hunk_Alloc`. Generic, behind the unchanged
  `VM_Create` interface; growth is now one block set per slot, the first
  time that slot is used on a level (measured flat over 30 join/leave, 30
  picker cancels, 30 layout toggles, 10 ui restarts).
  `VM_LoadSymbols` (only with a `.map` file) still allocates per create.
- `Com_TouchMemory`, `Cvar_SetCheatState`, `re.EndRegistration` in
  `CL_InitCGame` (`cl_cgame.c:851`) run per context — harmless.

---

## 3. Viewport virtualization

### 3.1 Principle

Each cgame believes the screen is exactly its viewport. Everything it
draws goes through traps in **pixel coordinates of that believed
screen**; the trap layer adds the viewport origin.

`viewRect_t { int x, y, w, h; }` per context, in real framebuffer
pixels (`cls.glconfig.vidWidth/vidHeight`).

| Trap (`cl_cgame.c`) | Change |
|---|---|
| `CG_GETGLCONFIG` → `CL_GetGlconfig` (:49) | copy `cls.glconfig`, set `vidWidth=view.w`, `vidHeight=view.h`, `windowAspect=(float)w/h` |
| `CG_R_RENDERSCENE` (:644) | copy refdef to a local, `x += view.x; y += view.y`, clamp to rect, then `re.RenderScene` |
| `CG_R_DRAWSTRETCHPIC` (:650) | `x += view.x; y += view.y` |
| `CG_CIN_SETEXTENTS` / `CG_CIN_DRAWCINEMATIC` (:753-760) | offset x,y |
| `CG_R_SETCOLOR`, register*, scene-add traps | unchanged |
| `CG_TRAP_GETVALUE` (`CL_GetValue` :424) | audit: any value exposing real resolution/scale must return viewport values |

Also a **scissor** per context so HUD elements that overdraw the
believed screen edge (wide HUD mods, `cg_fov` weapon overdraw) do not
bleed into neighbours: add one refexport `re.SetScissor( x, y, w, h )`
(or pass through an RC_ command) used around each context's draw. This
is the only renderer API addition; implement in `renderervk` and
`renderer` (GL1); `renderer2` optional (stub = full-screen scissor).
`RE_StretchPic` (`tr_cmds.c:215`) just queues `RC_STRETCH_PIC`; the
backend 2D path sets scissor to the full window
(`tr_backend.c:1052,1668` `qglScissor( 0, 0, vidWidth, vidHeight )`) —
that is where the current 2D clip rect must be honoured instead.

`SCR_AdjustFrom640` / `cls.scale/biasX/biasY` (`cl_scrn.c:59`) are used
by engine-drawn 2D (console, demo-recording icon) — stay full-screen.

### 3.2 Layouts (N players → rects)

`CL_ComputeLayout( n )` in `cl_splitscreen.c`:

| N | Layout | Notes |
|---|---|---|
| 1 | full | identical to upstream |
| 2 | top/bottom (`cl_splitVertical 0`) or left/right (`1`) | default top/bottom on 16:9 |
| 3 | `cl_splitFill 1` (default): two half-width cells on top, one full-width cell on the bottom. `0`: 2×2 with one black cell | see "Fill mode" below |
| 4 | 2×2 | each cell keeps the screen aspect |
| 5–6 | 3×2; with fill and N=5: rows of 3 + 2 (bottom row cells wider) | |
| 7–8 | fill `1`: N=7 rows of 4 + 3, N=8 4×2; fill `0`: 3×3 (cells keep screen aspect, 1–2 empty) | |

Unused cells are cleared black by the engine each frame.

**Fill mode (maintainer, 2026-10-05).** A splitscreen setting chooses between
equal cells with empty space and using the whole screen:

- `cl_splitFill` (archive, default 1): `1` = rows may hold different
  numbers of cells and every row is stretched to full width, so no black
  cell (3 players = 2 on top + 1 wide on the bottom). `0` = strict equal
  grid with black cells. The wider view is a gameplay advantage; that is
  the user's choice, which is why it is an option.
- `cl_splitWidePlayer` (archive, default 0 = last joined): which player
  number gets a cell in the emptier (wider) row. E.g. 3 players with
  `cl_splitWidePlayer 2` → P1 and P3 on top, P2 full-width on the bottom.
  With more than one wide cell (N=5: two; N=7: three) the priority player
  takes the first and the rest go in slot order.
- The wide row is the bottom row. Both cvars are exposed in the
  splitscreen settings menu next to each other, and changing either
  re-runs the layout live (same path as join/leave).
- The cgame for a wide cell is simply told the wider size, so FOV and HUD
  follow the usual widescreen behaviour of the mod.

Aspect/FOV: cgame derives `fov_y` from its believed `vidWidth/vidHeight`
(`CG_CalcFov` in mod code), so lying about the size gives correct
projection for free. Quake3e-specific widescreen cvars
(`cg_fovAdjust`-style behaviour lives in mods, not the engine) need no
handling.

**Layout change (join/leave)** changes rect sizes, and cgame read
glconfig only in `CG_INIT`. Default: when the layout changes, every
context whose rect changed gets a cgame restart — the same path
`CL_Vid_Restart` (`cl_main.c:1786`) uses mid-game minus the renderer
restart: `CL_ShutdownCGame()` → `CL_InitCGame()` → (state stays
`CA_PRIMED`→`CA_ACTIVE` on next snapshot) + `CL_SendPureChecksums()`.
Media is already cached in the renderer/sound, so this is a sub-second
hitch. Later polish (M5): an affine scale in the trap layer so rects
with unchanged aspect (1→4, 4→9 cells) resize without a restart.

**As built (R9):** a restart happens only when the size the cgame was
*told* changes (`CL_SplitCheckLayout` compares). With `cl_splitAspect 1`
(default) on 16:9, every 2–4-player cell (half-height, quarter, wide
bottom) is told 480×360, so 2↔3↔4 and `cl_splitFill`/`cl_splitWidePlayer`
changes restart nobody; restarts remain for 1↔2 (player 1 goes from full
window to a cell), `cl_splitVertical` at 2 players (640×480 vs 480×360),
`cl_splitAspect` toggles and window resizes. A slot held for the
join-time profile picker gets **no** cell: the live layout excludes held
slots; the picker is drawn where the slot's cell will be (a preview layout
that includes it) over the current views, and the layout changes once,
when the player connects. A cancelled join restarts nobody.

**As built (R10), corrects the R9 note:** with `cl_splitAspect 1` every
cell's cgame and menu are told one fixed 640×480 screen (`SPLIT_TOLD_W/H`)
and the trap layer scales their 2D and model-only scenes by
`fitted4:3width / 640` into the 4:3 area centered in the cell
(`CL_SplitFitted`); world views map the 640×480 refdef onto the whole cell
as before. So 2↔8 players, fill, wide player, `cl_splitVertical` and window
resizes restart **no** cgame/ui; only 1↔2 players and `cl_splitAspect`
toggles do (the "affine scale" polish above, done). With `cl_splitAspect 0`
the told size is still the cell and changes restart as before. Layouts 5–8
are as in the table; the wide row is the bottom one and with N=7 fill the
default wide player (last joined) takes the first bottom cell. At 1280×720
the smallest cells are 320×360 (4×2) and 426×240 (3×3); the engine overlay
already scales its font with the cell height (8×16 at 360 lines, minimum
6×12), verified readable in both.

### 3.3 Frame sequencing (`SCR_DrawScreenField`, `cl_scrn.c:522`)

```
re.BeginFrame
if N>1: clear unused cells
case CA_ACTIVE / CA_LOADING / CA_PRIMED:
    for each context i with cgvm:
        CL_SetContext(i); re.SetScissor(view); CL_CGameRendering(stereo)
    re.SetScissor(full)
UI (shared, full screen), console, debug graphs — unchanged
re.EndFrame
```

A context that is still connecting/loading while others play draws a
small engine-side "P3 connecting…" text in its cell instead of the UI
connect screen.

Renderer facts that make this work:

- Multiple scenes per frame are already supported: `RE_RenderScene`
  (`renderervk/tr_scene.c:383`) advances `r_firstScene*` at exit
  (:558-565), bumps `tr.frameSceneNum` for flare visibility (:474), and
  converts refdef x/y to viewport+scissor (:478-487). Stock cgame
  already issues several scenes per frame (world + HUD head models).
- Budget: all scenes of a frame share `backEndData` arrays
  (`MAX_REFENTITIES`, `MAX_DLIGHTS`, `MAX_POLYS`, `MAX_POLYVERTS`,
  drawsurf buffer). With 8 views the entity/poly/drawsurf caps will be
  hit → raise the caps (compile-time constants in `tr_local.h`;
  drawsurfs ×4, polys ×4, entities are per-scene-windowed but the
  backing array is shared → ×8). Spike S3 measures this.
  **As built (R10, measured with the new `r_speeds 1` "frame:" line):**
  Quake3e's caps are already large (128K drawsurfs, 8192 polys, 32768
  polyverts, 512 KB commands) and 8 busy views use ≤ 12K / 3K / 12K /
  116 KB of them — unchanged. Entities (4095/frame) and dlights (64/frame)
  were hit (4095/4095, 64/64; later views lost weapons/HUD models/lights):
  now `MAX_FRAME_REFENTITIES` = 4×4095 and the dlight array 8× (512), each
  scene still limited to the stock 4095 / 64 (sort-key bits, dlight bits).
  Peak with 8 views + 6 bots all firing: 4770 entities, 149 dlights. Same
  edit in `renderervk` and `renderer`; +2.6 MB hunk. Vulkan's geometry
  buffer grows on its own (no resize was needed).
- `RE_LoadWorldMap` (`tr_bsp.c:2370/2380`): a second call is
  `ERR_DROP "attempted to redundantly load world map"`. Trap layer:
  `CG_R_LOADWORLDMAP` for a context whose map name equals the already
  loaded one becomes a no-op (track loaded map name in
  `cl_splitscreen.c`). Same for `CG_CM_LOADMAP`: `CM_LoadMap` already
  early-outs for `clientload` with same name (`cm_load.c:713`).
- `re.BeginRegistration` (`tr_model.c:937`) calls `R_Init` and clears
  scene state; it is called from `CL_InitRenderer`, not from cgame, so N
  cgames do not re-trigger it. Shader/model/skin handles are global to
  the renderer and name-keyed → N cgames registering the same media get
  the same handles (cheap after the first).
- `CG_R_REMAP_SHADER` is global: a remap by one cgame affects all views.
  All contexts get the same server-driven remaps, so acceptable.
- `re.VertexLighting`, `CL_ForceFixedDlights` (`cl_cgame.c:460`) —
  global toggles, set identically by every instance.
- Per-view state to verify in spike S3: `tr.viewCluster` (PVS cache —
  recomputed when the leaf changes; alternating views thrash it but stay
  correct), flare fade state keyed by `frameSceneNum` (may flicker when
  scene count per frame changes on join), portal/mirror recursion (per
  scene, OK), sky (per view, OK), `r_fbo`/bloom (`RC_FINISHBLOOM`,
  `tr_backend.c:1202,1689`: bloom runs once over the whole frame when 3D
  is finished → cgame #1's first 2D draw triggers "done with 3D" and
  later views render after bloom; either defer bloom to after the last
  context or accept; `backEnd.doneSurfaces`), Vulkan depth-clear between
  views (each scene clears depth inside its scissor — confirm the VK
  path's `vk_clear_depth`/attachment-clear honours the viewport rect,
  otherwise view 2 wipes view 1's depth only, which is harmless, but a
  full **color** clear (`r_clear`, `r_fastsky`) must be rect-limited).
- `CG_UPDATESCREEN` during a secondary context's `CG_INIT` calls
  `SCR_UpdateScreen` re-entrantly → that would loop over all contexts
  and `VM_Call` into running VMs. Rule: `SCR_DrawScreenField` skips any
  context whose `cgvm->callLevel > 0` except the active one (needs a
  tiny `VM_IsRunning( vm )` accessor), and restores `cla` afterwards.

---

## 4. Networking

### 4.1 What exists

- One UDP socket (`net_ip.c:182` `ip_socket`), bound to `net_port`;
  `NET_GetPacket` (:647) reads it; `Sys_SendPacket` (:769) writes it.
- Loopback: `net_chan.c:527` `loopbacks[2]` indexed by `netsrc_t`
  (`NS_CLIENT`, `NS_SERVER`); `NET_GetLoopPacket` stamps
  `NA_LOOPBACK` with no port; `Com_EventLoop` (`common.c:2975-2982`)
  drains both.
- qport is a **global**: `net_chan.c:55,74` cvar `net_qport`; the
  client header writes `qport->integer` (`net_chan.c:122,185,256,311`),
  not `chan->qport`. `CL_CheckForResend` (`cl_main.c:2306`) and
  `Netchan_Setup` call (`:2756`) read the cvar.
- Server demux: `SV_PacketEvent` (`sv_main.c:968`) matches
  `NET_CompareBaseAdr` **and** `netchan.qport`, then fixes up the port.
- `SV_DirectConnect` (`sv_client.c:~478`):
  - per-IP cap: `sv_maxclientsPerIP` (default **3**, `sv_init.c:754`),
    counted with `NET_CompareBaseAdr`, bots excluded (:509-524). All
    `NA_LOOPBACK` addresses compare equal, so it applies to local
    players too.
  - "quick reject": same `ip:port` (`NET_CompareAdr`) within
    `sv_reconnectlimit` seconds → "Reconnecting, please wait" (:650-668).
  - reconnect/slot reuse: same `ip:port` **and** same qport (:676).
    (ioquake3-derived servers are looser: same IP and *either* same
    qport *or* same port takes over the existing slot.)
  - `NET_IsLocalAddress` = `NA_LOOPBACK` only (`net_ip.c:633`); local
    clients skip the challenge (:540) and get LAN rate (:1792).

### 4.2 Design: one socket path for offline and online

**Recommended: every secondary context gets its own UDP socket (own
ephemeral source port) and its own qport, and uses it for both local
listen servers and remote servers.** Context 0 keeps the stock path
(loopback to a local server, `ip_socket` to remote ones).

Rationale: one code path, and it is exactly what a remote server needs
(distinct `ip:port` per player dodges both the quick-reject and the
ioq3 slot-takeover logic). Extending loopback to N channels would be a
second mechanism that only helps offline, and `NA_LOOPBACK` has no port
to tell channels apart on the server side.

Changes:

- `netsrc_t`: add `NS_CLIENT2 … NS_CLIENT8` after `NS_SERVER`.
  `net_ip.c`: `static SOCKET cl_sockets[MAX_SPLITVIEW-1]`, opened on
  demand with `NET_IPSocket( net_ip->string, 0 /*ephemeral*/, &err )`
  (plus IPv6 twin only if the server address is `NA_IP6`), closed on
  context teardown and in `NET_Config`. `NET_GetPacket` / the
  `NET_Sleep` fd_set include them and report which socket a packet came
  from (new out-param or `net_from`-side tag) → `Com_EventLoop` routes
  to `CL_PacketEventCtx( i, … )`. `Sys_SendPacket` takes the `netsrc_t`
  (it currently drops it; `NET_SendPacket` in `net_chan.c:~690` has
  `sock`) and picks the socket.
- Secondary → local listen server: connect to `127.0.0.1:net_port`
  (resolve via `NET_StringToAdr( "127.0.0.1" )`), full challenge flow.
  Requires the local server's `ip_socket` to be open: with
  `net_enabled 0` or a failed bind there is no splitscreen for i>0
  (print a clear error). Windows Firewall does not prompt for
  loopback-only traffic.
- Our own server treats `127.0.0.1` as local: in `sv_client.c`, extend
  the local test used for (a) the per-IP cap, (b) LAN rate, (c)
  `sv_reconnectlimit` to "`NA_LOOPBACK` or IPv4 127.0.0.0/8". With that
  the default `sv_maxclientsPerIP 3` no longer blocks local players
  and they get uncapped snapshot rate. `Sys_IsLANAddress` already
  returns true for 127/8 (verify in spike S2).
- qport per context: `Netchan_Transmit`/`TransmitNextFragment` write
  `chan->qport` instead of `qport->integer` when `chan->sock !=
  NS_SERVER` (`chan->qport` is already filled by `Netchan_Setup`).
  `CL_CheckForResend` and the `connectResponse` handler use
  `cla->qport` (= `net_qport + playerNum`, wrapped to 16 bits).
- `CL_ConnectionlessPacket` (`cl_main.c:2620`) — `challengeResponse`
  and `connectResponse` are validated against `clc.serverAddress` /
  `clc.challenge`; because routing is by socket they land in the right
  context unchanged. OOB traffic unrelated to a connection (server
  browser, rcon, `getstatus`) stays on context 0 / `ip_socket`.

Connect sequence for a secondary (`CL_SplitJoin( i )`): requires context
0 ≥ `CA_PRIMED`. Copy `clc.serverAddress` from context 0 (or 127.0.0.1
if context 0 is on loopback), set `clc.challenge` random, state
`CA_CONNECTING`, let the per-context `CL_CheckForResend` run. The
existing state machine then proceeds: challenge → connect → gamestate →
`CL_InitCGame` → `CA_PRIMED` → first snapshot → `CA_ACTIVE`. The
download step (`CL_InitDownloads`, `cl_main.c:2203`) is skipped for i>0
(go straight to `CL_DownloadsComplete`'s tail), and so is
`CL_RequestAuthorization`.

### 4.3 Online: what an unmodified remote server may reject

| Check | Effect on siblings | Mitigation |
|---|---|---|
| `sv_maxclientsPerIP` (Quake3e servers, default 3; many mods/admin tools have their own limit) | 4th+ player (often 3rd+) refused "Too many connections" | none client-side; surface the server's message in that player's cell; document |
| `sv_reconnectlimit` quick-reject (same ip:port) | avoided by separate source ports — unless a NAT maps them to one port (rare; symmetric NAT gives distinct ports) | stagger joins by `sv_reconnectlimit`+1 s on "please wait" replies |
| ioq3 same-IP+same-port/qport slot takeover | avoided by distinct ports **and** distinct qports | — |
| getchallenge/connect rate limiting (`SVC_RateLimitAddress`, per IP) | bursts of N simultaneous connects get dropped | serialize: start context i+1 only after context i reaches `CA_CONNECTED` |
| Pure check (`cp` command, `CL_SendPureChecksums` `cl_main.c:1751`) | each context sends its own `cp` from the shared FS state — valid, since all loaded the same cgame/ui QVM from the same paks | context i>0 must not reset pak references; send after its `CL_InitCGame` |
| `cl_guid` (userinfo, `cl_main.c:4081`, derived from qkey) | identical GUID ×N — ban/admin tools (B3, UrT auth, PunkBuster-era checks) may kick duplicates | derive per-player GUID: `MD5( qkey + playerNum )` for i>0 |
| Duplicate names | game renames or admin bots kick | per-player `name` (§5) with distinct defaults |
| UrT 4.3 auth (`cl_auth`, authserver login key in userinfo) and "auth required" servers | only one account per login; siblings appear unauthenticated → rejected on auth-required servers, fine on `auth_enable 0`/notoriety 0 servers | none; document. Per-player auth keys are a maintainer decision (§11) |
| Server-side anti-multiaccount / max-per-IP plugins | policy | none — this is the server operator's call; do not evade |

Online play is honest multi-connection from one IP, exactly like N PCs
behind one NAT. No spoofing, no evasion of bans/limits.

**As built / verified (R9, dedicated Quake3e server on the LAN IP):**
- Quake3e counts a client for `sv_maxclientsPerIP` only once it is past
  `justConnected` (its first usercmd). Extras that connect while an
  earlier one is still loading would therefore slip past the cap — so
  extras connect strictly one at a time: each waits until the one before
  it is `CA_ACTIVE` (`CL_SplitConnectWaiting`). Result: P1 + 2 extras in,
  the 4th gets `print "Too many connections."`.
- An OOB `print` to an extra while it is `CA_CONNECTING`/`CA_CHALLENGING`
  is a refusal (`CL_SplitServerPrint`): that player is dropped with
  "server refused: <text>", shown in its cell for 5 s; "Reconnecting, please
  wait N seconds" holds its next attempt N+1 s; "Incorrect challenge" retries.
  (Stock behaviour for player 1 is unchanged.)
- Verified: cap raised over rcon → 4th joins; `sv_pure 1` (every player
  sends `cp`, no unpure kicks); server `map` with 4 attached (all back);
  `clientkick` of one extra ("was kicked" in its cell); `killserver` with 4
  attached (all to the main menu, no ERR_DROP) and reconnect.
- The "please wait" path and UrT auth servers are untested (no such
  server here); an auth-required server will refuse extras with its own
  print text, which is shown as is.

**As built (R11), refusal wording on a remote server** (P1 not on loopback;
our own server unchanged): the client cannot know a server's
`sv_maxclientsPerIP` in advance, so the refusal itself is explained:
"Player N could not join: this server allows only K players from one
connection" for a "Too many connections…" print (K = our players in game
at that moment — exact because extras connect one at a time), "Player N
could not join: <server text>" otherwise; in that player's cell for 10 s,
plus one line at the top of P1's view for 6 s; no join hint over it. On a
remote server the join hint has a second line "online servers may limit
extra players". Pad hold-to-join works on remote servers (it was limited
to our own server since M2 although `addplayer` worked). Our listen server
exempts 127.x.x.x from the per-IP cap; a dedicated server never does (R11).

---

## 5. Cvars and userinfo

### 5.1 What exists

Userinfo is every cvar with `CVAR_USERINFO`, serialized by
`Cvar_InfoString( CVAR_USERINFO, … )` at connect
(`CL_CheckForResend`, `cl_main.c:2309`) and on change
(`CL_CheckUserinfo`, :2952, driven by the global
`cvar_modifiedFlags & CVAR_USERINFO`). Engine-registered userinfo:
`name rate snaps model headmodel team_model team_headmodel color1
color2 handicap sex cl_anonymous password cg_predictItems cl_guid`
(`cl_main.c:4012-4081`); mods add more via `CG_CVAR_REGISTER` with the
flag (`cvar.c:1068`), e.g. UrT `gear`, `racered`, `funred`, `cg_rgb`.

### 5.2 Design

- Naming: **`p2_<cvar>` … `p8_<cvar>`** for per-player overrides
  (player 1 uses the unprefixed cvar — existing configs keep working).
- `CL_BuildUserinfo( ctx, buf )` in `cl_splitscreen.c`: start from
  `Cvar_InfoString( CVAR_USERINFO )`; for i>0, for each key in the
  string, if cvar `p<N>_<key>` exists and is non-empty, substitute its
  value. Then force `cl_guid` (per-player, §4.3) and default
  `name` to `"<name>_<N>"` if `p<N>_name` is unset. Both call sites
  (`cl_main.c:2309`, `:2970`) call this instead of `Cvar_InfoString`.
- Change detection: `p<N>_*` cvars are created `CVAR_ARCHIVE` with a
  new private "per-player userinfo dirty" bit — simplest: register them
  through `Cvar_Get(…, CVAR_ARCHIVE | CVAR_USERINFO_PN)` is invasive;
  instead `CL_CheckUserinfo` for i>0 rebuilds the string every 1 s and
  compares against the last one sent (kept per context, 1 KB). The
  global `cvar_modifiedFlags` bit is cleared only after **all**
  contexts have consumed it (latch it into a per-context flag at the
  top of the frame).
- Auto-creation: `p<N>_name/model/headmodel/team_model/team_headmodel/
  color1/color2/handicap/sex` are registered at init for N=2..8 with
  defaults copied from distinct stock models (visor, major, keel, …)
  so players are visually distinct out of the box. Any other key
  (mod-defined) can be overridden by the user simply creating
  `seta p2_gear "…"`; a console helper `psetu <player> <key> <value>`
  wraps that.
- Shared cgame cvars: all N cgames register the same `vmCvar_t`s
  against one cvar system (`CG_CVAR_REGISTER` → `Cvar_Register`,
  `CG_CVAR_UPDATE` → modification-count compare per `vmCvar_t`, which
  lives in each VM's own memory) → every instance sees every change.
  `cg_fov`, `cg_drawGun`, HUD options are therefore common to all
  players in M1–M4. Per-player cgame cvars (e.g. per-player
  `cg_fov`/`sensitivity`) can be added later with the same `p<N>_`
  substitution inside `CG_CVAR_REGISTER/UPDATE/VARIABLESTRINGBUFFER`
  for the active context — designed-for, not in the first milestones.
- Cgame-written cvars (`CG_CVAR_SET`): last writer wins. Known stock
  cases are status mirrors (`cg_thirdPerson` toggles, `ui_*` score
  cvars, `teamoverlay`) — harmless but note `teamoverlay` is userinfo:
  it is re-sent for whichever context builds userinfo next; acceptable.
- Engine input cvars are **per player** via the same scheme, read
  directly by `cl_input.c` for the active context: `p<N>_sensitivity`,
  `p<N>_m_pitch` (invert look), `p<N>_cl_run`, plus the pad cvars in §6.
- Persistence: everything above is ordinary `CVAR_ARCHIVE` in
  the current game dir's archive file (`q3config-ss.cfg` since L5, §17.4);
  per-player binds are written
  by an extended `Key_WriteBindings` (§6).

---

## 6. Input

### 6.1 What exists

- `keys[MAX_KEYS]` (`qcommon/keys.c:28`) holds down-state and one
  binding string per key. `Key_ParseBinding` (:627) turns a press into
  console text: `+forward <key> <time>` → `IN_ForwardDown` →
  `IN_KeyDown( &in_forward )` (`cl_input.c:108`). kbuttons are file
  statics (`cl_input.c:57-61`).
- `CL_CreateCmd` (`cl_input.c:590`) = `CL_AdjustAngles` +
  `CL_CmdButtons` + `CL_KeyMove` + `CL_MouseMove` + `CL_JoystickMove`
  (:403; reads `cl.joystickAxis[]` set by `CL_JoystickEvent` :389 from
  `SE_JOYSTICK_AXIS` events) + `CL_FinishMove`.
- Key namespace already has `K_JOY1..K_JOY32`, `K_AUX1..K_AUX16`
  (`keycodes.h:145-193`), and `K_PAD0_*` gamepad names exist in the
  SDL backend's mapping (`sdl_input.c:494-515`).
- Windows build uses the native backend (`win32/win_input.c`): legacy
  winmm joystick behind `USE_JOYSTICK` (:61-105, one device, not built
  by default). SDL backend (`sdl/sdl_input.c:35-49,533+`) opens a single
  `SDL_GameController` (`in_joystickNo`). Bundled SDL headers are
  2.0.10 (`libsdl/include/SDL2`); only mingw import libs are bundled
  (`libsdl/windows/mingw/lib64/SDL264.dll`), no MSVC `.lib`.
- Event dispatch: `CL_KeyDownEvent` (`cl_keys.c:546`) routes to
  console / UI / cgame (`CG_KEY_EVENT`, :651) / chat by
  `Key_GetCatcher`, else `Key_ParseBinding`.

### 6.2 Design

**Per-player bind tables and button state**

- Add a player dimension to bindings only: `char *bindings[MAX_SPLITVIEW]
  [MAX_KEYS]` conceptually; implemented as a second table
  `padBinds[MAX_SPLITVIEW][PAD_KEY_COUNT]` in the new `in_gamepad.c`,
  **not** by widening `keys[]`. Keyboard/mouse (the existing `keys[]`)
  always belong to one player (`cl_kbmPlayer`, default 1; 0 = nobody).
  Each gamepad has its own virtual key set
  (`PAD_A, PAD_B, …, PAD_LSTICK_UP…` ≈ 32 entries) with per-player
  binding strings. Commands: `pbind <player> <padkey> <command>`,
  `punbind`, `pbindlist`; written to config by a hook in
  `Key_WriteBindings`.
- Executing a pad binding for player N: wrap with a context prefix
  understood by the command system — `Cbuf_AddText( va( "p%d %s\n", N,
  cmd ) )` where the new command `p<N>` (registered `p1..p8`) does
  `CL_PushContext(N-1); Cmd_ExecuteString( rest ); CL_PopContext()`.
  That makes **every** existing command per-player for free:
  `+attack`, `weapnext`, cgame commands (`CL_GameCommand` → that
  context's `cgvm`), server-forwarded commands
  (`CL_ForwardCommandToServer` → that context's reliable queue), `say`,
  `team`, `kill`, mod commands (UrT `ut_weapdrop`, `ui_radio`…).
  Button commands keep the `<key> <time>` suffix convention so
  `IN_KeyDown`'s two-key tracking still works (use `MAX_KEYS + padKey`
  as the key number to avoid collisions).
- kbuttons per context: move the statics at `cl_input.c:57-61,89` into
  `inputState_t` inside the context and `#define in_forward
  (cla->in.forward)` etc. at the top of `cl_input.c` (file-local
  macros; function bodies untouched).
- Analog: per-context `cl.joystickAxis[]` already exists.
  `in_gamepad.c` writes axes directly into the owning context
  (bypassing the global `SE_JOYSTICK_AXIS` queue, which has no device
  id). `CL_JoystickMove` is replaced for pad-owning contexts by
  `CL_GamepadMove( cmd )`: left stick → `forwardmove/rightmove`
  (scaled, radial deadzone), right stick → `cl.viewangles` at
  `p<N>_joy_yawSpeed/_pitchSpeed` deg/s with a response curve
  (`joy_exponent`, default 2) and optional `joy_invertPitch`; triggers
  are digital virtual keys with `joy_triggerThreshold`. Cvars:
  `joy_deadzone 0.2`, `joy_yawSpeed 240`, `joy_pitchSpeed 160`,
  `joy_exponent 2`, each overridable as `p<N>_…`.
  `cl.cgameSensitivity` (zoom scale from cgame) multiplies look speed
  like it does for the mouse.

**Gamepad backend (Windows)**

Recommended: **SDL2 GameController, used for pads only**, in a new
`code/client/in_gamepad.c`, native win32 window/keyboard/mouse backend
untouched. XInput is capped at 4 devices and has no stable identity;
SDL gives 8+ pads (XInput + HIDAPI/RawInput: DualShock/DualSense/Switch
Pro/8BitDo), hot-plug events, GUIDs, a mapping DB, rumble, and the same
code works on the SDL (Linux/mac) builds.

- **As built since R15 (0.0.0.20):** the Windows clients link SDL2
  2.32.10 **statically** (`Q3E_SDL_STATIC`; `scriptsuild.ps1` builds
  the lib from the SDL source release, `work/sdl2-src-PROVENANCE.md`), so
  a release is the three exes only; the dynamic path below remains for
  every other build (Linux binds the engine's own SDL).
- Bind SDL **dynamically** at runtime (`LoadLibrary("SDL2.dll")` +
  `GetProcAddress` for the ~25 functions used; headers from
  `libsdl/include/SDL2`). No MSVC import lib needed, and the engine
  still runs (without pads) if the DLL is missing. Ship a current
  SDL2.dll (≥ 2.26 for good HIDAPI + >4 XInput-class pads through
  RawInput/WGI) beside the exe — download recorded in
  `work/SDL2-PROVENANCE.md`; the DLL is a build/release artifact, not
  committed. On SDL builds of the engine, `in_gamepad.c` links SDL
  normally and `sdl_input.c`'s single-stick code is compiled out
  (`USE_JOYSTICK` off).
- `SDL_Init( SDL_INIT_GAMECONTROLLER )` only (no video/events
  subsystem conflict with the win32 window — SDL's joystick thread
  uses its own hidden message window; hints:
  `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`,
  `SDL_HINT_JOYSTICK_THREAD=1`). Poll with `SDL_GameControllerUpdate()`
  + state getters once per frame from `IN_Frame` — no SDL event pump
  needed, except hot-plug detection: compare `SDL_NumJoysticks()` /
  instance ids each frame (or pump `SDL_PumpEvents` and peep only
  controller device events).
- Fallback if SDL proves troublesome in spike S4: XInput for pads 1–4
  (zero dependencies) + SDL optional for 5–8. Keep `in_gamepad.c`'s
  internal device interface small so this is a backend swap.

**Stable device → player mapping (fixes the Spearmint "re-select the
pad after restart" bug)**

- Identity = SDL joystick GUID string + ordinal among identical GUIDs
  (two identical Xbox pads share a GUID; XInput gives no serial). Where
  available use `SDL_JoystickGetSerial` / device path (SDL ≥ 2.0.14)
  as a stronger key.
- Config: `p<N>_padId "<guid>[#ordinal|:serial]"` (`CVAR_ARCHIVE`).
- Assignment algorithm, run at startup and on every hot-plug:
  1. each connected pad whose id matches a `p<N>_padId` → that player;
  2. remaining pads → lowest-numbered player slot that has no pad and
     is "wanted" (in game or in the join lobby), in connection order;
  3. never steal a pad from a player who has one; never leave an
     in-game player without input if an unassigned pad exists
     (auto-heal: on disconnect of a player's pad, pause-free: that
     player idles; the next unassigned pad that presses **any button**
     takes over that slot and its id is saved).
  No menu trip is ever required; `padlist` / `padassign <player>
  <index>` exist for manual override.
- Identical-GUID ordinals are inherently unstable across reboots
  (enumeration order); rule 3's "press any button to claim" makes that
  self-correcting within a second.

**Default bindings** (applied when a player slot has no pad binds at
all — first run, or new player number): left stick move, right stick
look, RT `+attack`, LT `+zoom`, A `+moveup`, B `+movedown`, RB
`weapnext`, LB `weapprev`, X `+button2` (use item), Y `+button3`
(gesture) , L3 `+speed`, R3 `+zoom` alt, Start = menu/pause for that
player, Back `+scores`, D-pad = `weapon N` shortcuts. Mod-specific
default sets are data: `default_pad.cfg` looked up through the VFS
(`<game>/default_pad.cfg`, falling back to an engine-embedded baseq3
set), so a UrT set (reload, sprint, bandage, gear) ships as a small pk3
/ loose cfg without engine changes.
(As built: the baseq3 set is in `in_gamepad.c` `padDefaultBinds`; Urban Terror's
built-in set and its hold-or-toggle defaults are in section 20.)

**Join / drop flow**

- **Player numbers follow join order (maintainer, 2026-10-05).** Up to 8
  pads may be connected; nobody is a player until they act. The first
  pad to press any button becomes P1 (at the menu or in game). After
  that, any unjoined pad that presses the join button becomes the lowest
  free player number, gets `CL_SplitJoin( slot )`, and the layout is
  recomputed.
- Join button and style are options: `cl_splitJoinButton` (default `A`
  = bottom face button, i.e. Cross on PlayStation pads; any pad key
  name, e.g. `START`) and `cl_splitJoinHold` (ms, default 0 = a press;
  e.g. 1000 = hold one second). Both in the splitscreen settings menu.
- Prompt, Call of Duty style: while at least one connected pad is not
  joined and a slot is free, the engine draws a small blurb in a screen
  corner (or centered in an empty cell when there is one): "Player 3:
  press A to join". The button name follows the pad type SDL reports
  (A / Cross / B on Nintendo layout) and the hold variant reads "hold".
  `cl_splitJoinHint 0` hides it.
- Consequence for "stable mapping" above: the saved `p<N>_padId` is no
  longer what decides who is which player at startup — join order is.
  It is kept only for auto-heal (a joined player's pad drops and
  reconnects mid-session → same slot) and for an optional
  `cl_splitAutoRejoin` that re-seats remembered pads without a button
  press. Per-player settings (name, model, binds, sensitivity) belong
  to the player number, not to the physical controller.
- Drop: that player holds **Back+Start for 2 s**, or console
  `dropplayer <n>` → reliable `disconnect` on that context, free VM and
  socket, recompute layout. Player numbers of others do not shift
  (viewport cells are filled in slot order, compacted).
- Player 1 can be KB+mouse or a pad. If `cl_kbmPlayer 1` and a pad is
  also assigned to player 1, both drive context 0.
- Also scriptable for headless tests: `addplayer [n]`, `dropplayer n`,
  `p<N> <cmd>` — this is how agents verify without pads.

**Focus rules (console / menus / chat)**

- Console and UI menus are single, keyboard/mouse driven, and always
  act as context 0 (commands typed in the console run in context 0
  unless prefixed `p<N>`).
- While `KEYCATCH_UI`/`CONSOLE` is up, pads of players 2+ keep playing
  (their input path bypasses `keyCatchers`); player-1's pad drives the
  menu (D-pad/left stick → arrow keys, A → `K_ENTER`, B → `K_ESCAPE`
  injected as ordinary key events) so the game is couch-navigable.
- `KEYCATCH_CGAME` (cgame-owned cursor, e.g. UrT/Team Arena in-cgame
  menus, scoreboard mouse) is per context: bit stored in the cls
  shadow; `CG_KEY_SETCATCHER` only changes the active context's bit.
  For a pad-driven context with that bit set, pad buttons are delivered
  as `CG_KEY_EVENT`s (mapped to `K_JOY*`/arrows/enter/escape) and the
  left stick as `CG_MOUSE_EVENT` deltas. `CG_KEY_ISDOWN` /
  `CG_KEY_GETKEY` answer from the active context's device.
- Chat (`KEYCATCH_MESSAGE`, `messagemode`) is keyboard-only → context 0
  (or `p<N> messagemode` sets `chat_playerNum`-style target context).
  Pad players get quick-chat binds via `pbind` (`say_team`, UrT radio).

---

## 7. Sound

`snd_dma.c` has one listener (`listener_number/origin/axis`, :70-72) set
by `S_Base_Respatialize` (:1032) — called by each cgame every frame via
`CG_S_RESPATIALIZE`; `S_SpatializeOrigin` (:393) attenuates/pans against
it; `StartLocalSound` (:605) plays on `listener_number` at full volume;
sounds with `entityNum == listener_number` are unattenuated (:516).

Design (one mixer, multi-listener, nearest wins):

- Keep an array `listeners[MAX_SPLITVIEW]` {entnum, origin, axis,
  valid}. `S_Respatialize` gains the context index (new
  `S_RespatializeEx( idx, … )`; the trap passes `cla->playerNum`), marks
  the listener valid for this frame, and **defers** the channel
  re-spatialization loop (:1050-1070) to `S_Update`, once per frame.
- `S_SpatializeOrigin`: compute volume for each valid listener, take
  the **loudest** (= nearest); stereo pan from that listener's axis
  when N ≤ 2, **mono (center)** when N ≥ 3 (pan is meaningless with 3+
  views on one pair of speakers; cvar `s_splitPan` to override).
- "Is local" test (`entnum == listener_number`) becomes "entnum is any
  valid listener's entnum" → every player hears their own weapon,
  footsteps, pain at full volume.
- Local/announcer sounds (`CG_S_STARTLOCALSOUND`, `CHAN_ANNOUNCER`,
  "excellent", countdown, UrT radio): N cgames each start the same
  sample in the same frame → N× volume/phasing. Dedupe in
  `S_StartLocalSound`: drop a local sound if the same `sfxHandle` was
  started as a local sound within the last 50 ms by a different
  context — except personal-feedback channels where each player's
  instance is legitimately distinct in time anyway (hit beeps arrive
  on different frames; no special-casing needed beyond the 50 ms
  window).
- World sounds from server events are also started once per cgame
  (each instance processes the same entity event). Dedupe key:
  (`sfxHandle`, `entityNum`, `entchannel`) within the same
  `cls.framecount`+1 window → keep one, spatialized by nearest
  listener.
- Looping sounds (`CG_S_ADDLOOPINGSOUND`, cleared per cgame frame by
  `CG_S_CLEARLOOPINGSOUNDS`): `loopSounds[]` is indexed by entity
  number, so N adds for one entity collapse into one slot naturally;
  `ClearLoopingSounds` must only clear at the first context of a frame
  (otherwise context 2 wipes context 1's adds — which are the same
  set, but entities visible only to player 1's PVS would be lost).
  Doppler (:755-757 uses `loopSounds[listener_number]`) → disable when
  N > 1.
- Music (`CG_S_STARTBACKGROUNDTRACK`): identical request from each
  instance; ignore if the same intro/loop is already playing; honour
  only context 0's `STOPBACKGROUNDTRACK`.
- `S_UpdateEntityPosition`, `S_RegisterSound`: shared, idempotent.
- `inwater` reverb flag: use context 0's (or any-listener OR).
- All of this is inside `snd_dma.c` + a 2-line change in
  `snd_main.c`; no cgame-visible API change.

---

## 8. UI, console, lifecycle

- **One UI VM, one console**, owned by context 0. The UI reads
  `cls.state`/`clc`/`cl` through `cl_ui.c` traps → always executed with
  context 0 active (the main `CL_Frame`/`SCR` code runs in context 0
  outside explicit loops; assert in `CL_UISystemCalls`).
- **Connect** (menu or `connect`/`map`/`devmap`): context 0 as
  upstream. Secondaries that were present in the previous session
  (their pads are connected and `cl_splitAutoRejoin 1`) or that join
  later run `CL_SplitJoin` once context 0 reaches `CA_PRIMED`.
- **Map change / `map_restart` / new gamestate**: each context receives
  its own `svc_gamestate` → `CL_ParseGamestate` (`cl_parse.c:500`) →
  per-context `CL_ClearState`, cgame shutdown, re-init. Ordering hazard:
  `CL_ParseGamestate`/`CL_DownloadsComplete` (`cl_main.c:1993`) end in
  `CL_FlushMemory` (:1056) → `CL_ShutdownAll`/`Hunk_Clear` + renderer
  re-registration, which would destroy the *other* contexts' freshly
  loaded cgames. Rule: **only context 0 flushes memory.** When context
  0 gets a new gamestate it first shuts down *all* cgame VMs
  (`CL_ShutdownCGame` per context) and marks secondaries
  "awaiting gamestate"; secondaries' gamestates are parsed normally but
  their `CL_InitCGame` is deferred until context 0 has finished loading
  (state `CA_PRIMED`), then run one per frame (keeps the loading screen
  responsive). A secondary's gamestate that arrives *before* context
  0's (server sends in client order, loopback first, so normally it
  does not) is simply held in `CA_CONNECTED`-with-gamestate until then.
  On a local server (`SV_SpawnServer`), `CL_MapLoading`
  (`cl_main.c:1076`) is called for context 0; secondaries are dropped
  to "awaiting gamestate" there as well.
- **`vid_restart` / `snd_restart`** (`CL_Vid_Restart`, :1786): shuts
  down cgame+UI and renderer, restarts, then `CL_InitCGame` if
  `cls.state > CA_CONNECTED`. Extend the cgame parts to loop over
  contexts; layout is recomputed from the new resolution first.
- **One secondary is dropped by the server** (kick, timeout,
  `svc_disconnect`, "Too many connections"): `Com_Error( ERR_SERVERDISCONNECT
  / ERR_DROP )` is the stock reaction and tears down the whole client.
  In per-context code paths for i>0, convert to
  `CL_SplitDrop( i, reason )` (free that context, show the reason in
  its cell for 5 s, recompute layout). Implementation: a
  `cla->playerNum > 0` check at the few `Com_Error( ERR_SERVERDISCONNECT
  … )`/`ERR_DROP` sites in `cl_parse.c`, `cl_main.c`
  (`CL_CheckTimeout` :2891, disconnect OOB in
  `CL_ConnectionlessPacket`), and a `setjmp`-free guard for errors
  thrown from inside a secondary's cgame (`CG_ERROR` trap → drop that
  context only).
- **Context 0 disconnects** (`disconnect`, error, menu): all
  secondaries are disconnected first (reliable `disconnect` ×N), then
  stock `CL_Disconnect`.
- **Demos**: record/playback context 0 only (`CL_Record_f`,
  `CL_PlayDemo_f` refuse/ignore for i>0; during demo playback
  splitscreen is off). `cl_autoRecordDemo` → context 0.
- **AVI capture**: full frame — works unchanged.
- **Pause**: `cl_paused`/`sv_paused` are global; `CL_CheckPaused`
  applies to all. Single-player pause with N>1 clients does not engage
  (server sees >1 client) — expected.
- **`cl_maxpackets`/`snaps`/`rate`**: per context but same cvars; local
  server gives LAN rate (§4.2).
- **Engine-drawn per-cell overlays** (small, in `cl_splitscreen.c`):
  1-px cell borders, "P<n> connecting / dropped: reason / hold START",
  drawn after all cgames and before the UI.
  **As built (R9):** the layout closes the dropped player's cell at once
  (others may restart for the new size); its reason ("Player 3 dropped:
  was kicked", "... server refused: Too many connections.", "Player 2
  left") is drawn for 5 s over the area that was its cell (bottom quarter
  if it never had one); not shown when player 1's whole session ends or a
  held join is cancelled.
- **`game_restart`** (mod change) crashes upstream Quake3e on this box
  too (upstream `2b375bd1` and the maintainer's release exe, from a level or the
  main menu) — not ours; profile re-read on a mod change is implemented
  but untestable here until that is fixed.

---

## 9. Mod compatibility risks

Works by construction: anything a mod does through the standard
cgame/game trap API, because each instance is an ordinary client.

Risks and mitigations:

| Risk | Mitigation |
|---|---|
| Mod ships cgame as DLL only | per-player DLL copies (§2.2); UrT/OSP/CPMA/Defrag/ExcessivePlus ship QVMs |
| cgame reads real resolution from cvars (`r_customwidth`, `r_mode`, `cg_*aspect`) instead of glconfig | optional per-context substitution in `CG_CVAR_VARIABLESTRINGBUFFER`/`CVAR_REGISTER` for `r_customwidth/height` (off by default, enable per mod if seen) |
| cgame writes its own config/state files (`CG_FS_WRITE`: UrT gear/weaponmodes cfg, Defrag ghosts) | N instances write the same path; last wins. Optional `p<N>_` filename prefix for i>0 writes if a mod misbehaves |
| cgame-registered console commands (`CG_ADDCOMMAND`) collide across instances — `Cmd_AddCommand` ignores duplicates (`cmd.c:762`) and `Cmd_RemoveCgameCommands` on any context's disconnect removes them for all | refcount cgame commands: remove only when the last context goes away (`cl_splitscreen.c` wrapper) |
| Mods assuming one local client for `KEYCATCH_CGAME`/mouse UI (UrT in-cgame menus, Team Arena HUD scripts) | per-context catcher bit + pad→key/mouse synthesis (§6); mouse-only widgets usable by player 1 only is acceptable |
| Shared `vmCvar_t` state (UrT `cg_*`, radio/gear cvars set by its UI) | by design shared in M1–M4; per-player substitution hook exists (§5) |
| **Urban Terror 4.3**: closed QVMs (fine); expects its own engine fork (ioUrbanTerror) for auth (`cl_auth*`, `authc` cvars, `AUTH:CL` OOB packets), `+button` extensions (sprint = `+button8`? etc. — within 16 `in_buttons`), `r_` cvars, pure/pk3 naming (`zUrT43_qvm.pk3`) and `com_standalone`/`fs_basegame q3ut4` | Quake3e already runs UrT as a standalone basegame (verified R11: `fs_basegame q3ut4` only, §10 M3 as-built). Auth: unauthenticated play only (§4.3). Any UrT-engine-only trap the QVM calls surfaces as `Bad cgame system trap` → stub per trap as found |
| UrT server-side per-IP limits / auth-required servers | policy; document |
| Anti-cheat style mods comparing client state (pure checksums, `cl_guid`) | per-player guid; identical pure checksums are correct |
| Performance: N× cgame frame + N scene renders | cgame CPU is small; renderer front-end is the cost. 4 views at 1080p on this hardware is trivial; 8 needs the raised scene caps (§3.3) |
| Mods with client-side prediction tied to `com_maxfps`/`pmove_fixed` | unchanged — each context predicts independently |

---

## 10. Milestones and executor tasks

Every task: build with `scripts\build.ps1`, windowed flat run per the
charter, evidence = `screenshot` output + `qconsole.log` excerpt copied
to `work/`. "Verify" lines assume test data per STATUS.md (baseq3 or a
free stand-in). No task needs a physical gamepad until M2-T5 (maintainer).

### Spikes (front-loaded unknowns; each ≤ half a day, throwaway code OK)

- **S1 — macro contexts compile.** Apply §1.2 macros with N fixed at 1.
  Done: full client builds, runs a map, behaviour identical. Report any
  token collisions. *(Becomes M1-T1 if clean.)*
- **S2 — second connection from the same process.** Hack: a second
  `clientConnection_t`+socket that only does challenge/connect/gamestate
  parsing against the local listen server over 127.0.0.1 and logs
  "P2 got gamestate, clientNum=…". Proves §4.2 (socket routing, qport,
  server-side acceptance, `Sys_IsLANAddress(127/8)`).
- **S3 — two cgame VMs, two viewports.** With S1+S2: second
  `VM_Create( VM_CGAME2 )`, `CG_INIT`, per-frame
  `CG_DRAW_ACTIVE_FRAME` for both into top/bottom rects via the trap
  offsets; skip duplicate `LoadWorldMap`. Proves §2 and §3 (renderer
  multi-scene limits, bloom/FBO ordering, VK depth/color clears,
  re-entrancy of `CG_UPDATESCREEN`). Screenshot with two different
  POVs = pass. Test both `renderervk` and `renderer` (GL).
- **S4 — SDL pads alongside the win32 backend.** Tiny standalone test
  inside the engine: dynamic-load SDL2.dll, init GAMECONTROLLER only,
  log GUID/name/buttons for every device at 1 Hz. The maintainer plugs in 2+
  pads once and sends the log (the only pad-dependent spike). Proves
  §6 backend choice; fallback = XInput.
- **S5 — Urban Terror boots on unmodified Quake3e** (needs UrT data):
  `+set fs_basegame q3ut4 +set com_standalone 1 +devmap ut4_casa`
  (exact flags to be determined), list any bad-trap / missing-cvar
  errors. Informs M3 scope early.

### M1 — two keyboard-driven players on a local server (the refactor)

- **T1 Context plumbing.** `client.h` macros, `clx[]`, `CL_SetContext/
  Push/Pop`, cls shadow, per-context input statics, `cl_splitscreen.c`
  skeleton. No behaviour change at N=1. Files: `client.h`, `cl_main.c`
  (defs), `cl_input.c` (macros), new `cl_splitscreen.c`, vcxproj/
  Makefile/CMake lists. Verify: plays normally; `timedemo` of a stock
  demo within noise of baseline.
- **T2 Net: per-context sockets + qport.** §4.2. Files: `net_ip.c`,
  `net_chan.c`, `qcommon.h`, `common.c` (event loop), `sv_client.c`
  (local-address test). Verify: S2's log line via the real code path.
- **T3 VM slots.** §2.2. Files: `vm.c`, `qcommon.h`, `vm_local.h`.
  Verify: `vminfo` shows two cgame entries after T4.
- **T4 Frame loop + join/drop commands.** Per-context `CL_Frame` steps,
  packet routing, `CL_SplitJoin/Drop`, `addplayer`/`dropplayer`/
  `p<N>` commands, gamestate ordering rules (§8), secondaries skip
  downloads/auth/FS side effects. Files: `cl_main.c`, `cl_parse.c`,
  `cl_cgame.c`, `cl_splitscreen.c`. Verify: `devmap q3dm1; addplayer`
  → server `status` shows 2 humans; `p2 +forward` moves player 2
  (visible from P1's view / `p2 kill` obituary in log).
- **T5 Viewports.** Trap offsets, glconfig lie, scissor refexport,
  layout for N≤4, per-cell overlays, cgame restart on layout change,
  `SCR_DrawScreenField` loop + re-entrancy guard. Files: `cl_cgame.c`,
  `cl_scrn.c`, `cl_splitscreen.c`, `tr_public.h`, `renderervk/
  tr_cmds.c|tr_backend.c|tr_init.c`, same in `renderer/`. Verify:
  screenshots at N=2,3,4 (use `addplayer` ×3 + bots for motion);
  `map_restart`, `map q3dm17`, `vid_restart`, `dropplayer 2` mid-game
  all survive (log has no ERR_DROP).
- **T6 Per-player userinfo.** §5.2. Verify: `p2_name`, `p2_model`
  visible in server `status`/scoreboard screenshot; changing
  `p2_name` live renames only player 2.
- **T7 Sound multi-listener.** §7. Verify: no doubled announcer (log
  `s_show 1` counts), agent-checkable only roughly → put a listening
  check on the maintainer's list.
- Diff footprint M1: ~2,000–2,500 added lines, ~70% in new
  `cl_splitscreen.c`; ~250 changed lines across ~14 upstream files.
  PR-ability: T1–T3 are behaviour-neutral refactors and plausibly
  upstreamable on their own; the rest is one feature PR behind
  `USE_SPLITSCREEN` if upstream ever wants it.

### M2 — gamepads, auto-assignment, hold-to-join, 1–4 layout polish

- **T1 `in_gamepad.c` backend** (SDL dynamic load, device table,
  hot-plug, polling) per S4 outcome. Verify (agent): with no pads,
  clean init/shutdown in log; with the SteamVR/virtual devices on this
  box nothing bogus is opened as a pad.
- **T2 Pad binds + analog move/look + cvars + defaults**
  (`pbind`, `default_pad.cfg` lookup, `CL_GamepadMove`). Verify
  (agent): a debug `padinject <player> <padkey|axis> <value>` command
  feeds the same code path → `p2` walks/turns/fires via injected input.
- **T3 Stable mapping + auto-assign + auto-heal** (`p<N>_padId`).
  Verify (agent): unit-style test command `padsim` simulating
  plug/unplug orders against saved ids; log the resulting assignments.
- **T4 Hold-to-join / hold-to-drop + hints + menu navigation by
  player-1 pad + per-context `KEYCATCH_CGAME` routing.**
- **T5 the maintainer headset-free pad pass** (checklist in STATUS.md): fresh
  config → P1..P4 join by holding START, defaults sane, quit/restart →
  everyone controls the same slot with no menu visit, unplug/replug,
  swap two identical pads.
- Diff: ~1,500 lines new (`in_gamepad.c`, cfg), <80 in upstream files
  (`win_input.c` init/frame hooks, `keys.c` write hook, `cl_input.c`).
  PR-able separately as "SDL gamepad support on Windows" (the upstream
  dev's own stated gap) if kept independent of splitscreen for N=1.

### M3 — Urban Terror 4.3 offline splitscreen

- T1 Fix whatever S5 found (traps, cvars, basegame flags); document the
  launch line in STATUS.md.
- T2 UrT-specific pass: per-context cgame-menu input, gear/config file
  collisions, radio/quick-chat pad binds, `default_pad.cfg` for UrT,
  UrT bots (`addbot`) for a 4-view soak test (10 min, no leaks:
  `meminfo` hunk stable across 5 map changes).
- Diff: expected <200 lines in engine; mostly data.
- **As built (R11).** S5: stock-path Quake3e boots UrT 4.3 with only
  `+set fs_basepath <UrT dir> +set fs_basegame q3ut4` (Quake3e has no
  `com_standalone`; `fs_game` stays empty): no bad trap, no missing-cvar
  error, QVMs from `zUrT43_qvm.pk3`, bots (`bot_enable 1` on the command
  line — it is latched — then `addbot boa 3 blue`), "not auth capable"
  notice only. M3 engine diff ≈ 230 lines in our files + 6 in `cmd.c` (a
  latent upstream `Cbuf_InsertText` / nested-command bug UrT's per-frame
  commands exposed); the UrT-specific part is data (`q3ut4` pad defaults,
  `playercvar ui_gear*`). Hunk: UrT's QVMs are big — qagame 64 MB, cgame 64
  MB, ui 16 MB; 1 player uses 325 MB, each extra player +84 MB (cgame + its
  menu): UrT's own `default.cfg` (`com_hunkmegs 800`) fits 7 players,
  `com_hunkMegs 1024` fits 8 (912 MB, 903 MB after a map change). The
  engine default stays 128 (baseq3 fits 8 in it); a join that does not fit
  is refused with the needed size and `com_hunkMegs` named.

### M4 — online

- T1 Remote connect path for secondaries (serialize joins, per-player
  guid, reject handling per §4.3, per-cell server messages).
  Verify (agent): a **second Quake3e dedicated server process on this
  box bound to the LAN IP** (not loopback) with default
  `sv_maxclientsPerIP 3`: 3 players join, 4th shows the refusal in its
  cell and the other three keep playing; with the cvar raised, 4 join.
  Also `sv_pure 1`. Also against an ioquake3 dedicated server build if
  obtainable (slot-takeover logic differs).
- T2 Robustness: one sibling times out / is kicked (`clientkick`) /
  server map-rotates / server restarts — others unaffected or all
  reconnect cleanly (`cl_splitAutoRejoin`).
- T3 maintainer: real public server test (UrT + a Q3 server) — his call
  which servers; touches online components, so it waits for him.
- Diff: ~300 lines, all in `cl_splitscreen.c`/`cl_main.c` hooks.

### M5 — up to 8

- T1 Layouts 5–8, raised renderer scene caps, hunk default, perf pass
  (front-end time per view via `r_speeds`, target ≥ 60 fps at 8 views
  1440p on the 5090 — expected trivial).
- T2 Pads 5–8 through SDL (HIDAPI/RawInput path), the maintainer check.
- T3 No-restart layout scaling (§3.2 polish), mono/pan sound rule
  check at N≥3.
- Diff: ~300 lines.
- **As built (R10):** T1 done (layouts, caps above, hunk default 128 MB
  kept: 8 cgames use ~29 MB, 59 MB left on q3dm12; perf: 8 views cost
  ~2 ms CPU on Vulkan / ~3.3 ms on GL per frame, mostly the 8 cgame VMs,
  renderer front end ~0.35 ms); T3 done (no-restart scaling, 3.2; the
  nearest-listener / mono rule works at 8, de-dup drops ~8 copies per event;
  with 14 shooters the 96 mixer channels saturate and the oldest are cut).
  Our server exempts 127.x.x.x from `sv_maxclientsPerIP` (4.2). Local
  joins stay serialized (~0.1 s each). `cl_splitMaxPlayers` default 8.
  T2 (real pads 5–8) is the maintainer's.

---

## 11. Open decisions for the maintainer (default taken if unanswered)

1. **Gamepad backend**: SDL2.dll shipped beside the exe (dynamic load)
   vs. XInput-only (max 4 pads, no PlayStation/Switch pads without
   Steam Input). *Default: SDL2, XInput fallback only if S4 fails.*
2. **2-player layout**: top/bottom vs. left/right. *Default:
   top/bottom, cvar to switch.*
3. **Uneven player counts** — DECIDED (maintainer 2026-10-05): option
   `cl_splitFill` (use the whole screen, wide cells) plus
   `cl_splitWidePlayer` (who gets the wide cell); see §3.2.
4. **Join flow** — DIRECTION (maintainer 2026-10-05, details still open):
   player number = join order; join button and press/hold configurable;
   on-screen "Player N: press A to join" blurb; see §6. Defaults taken:
   fill on, wide = last joined, join = press A, hint on.
5. **Keyboard+mouse ownership**: always player 1 vs. assignable.
   *Default: `cl_kbmPlayer 1`, assignable by cvar.*
6. **Shared vs. per-player cgame settings** (fov, HUD, crosshair).
   *Default: shared through M4; per-player substitution later only for
   settings the maintainer asks for.*
7. **Online identity**: siblings connect with derived per-player GUIDs
   and no UrT auth; we do nothing to get around per-IP limits or
   auth-required servers. *Default: yes, exactly that.*
8. **Join hitch**: sub-second cgame restart of existing views when the
   layout changes (simple, robust) vs. investing early in seamless
   rescale. *Default: accept the hitch until M5.*
9. **Renderer scope**: implement scissor/caps in `renderervk` + GL1
   `renderer`; `renderer2` best-effort. *Default: as stated.*
10. **Upstream PR**: offer the behaviour-neutral pieces (context
    macros, VM slots, Windows gamepad backend) upstream after M2;
    keep the feature in our fork. *Default: decide after M2.*

---

## 12. Leave, per-player menu, profiles, on-screen keyboard (maintainer, 2026-10-05)

These supersede §6 "stable mapping" / per-slot settings where they
conflict, and add tasks to M2.

### 12.1 Leaving mid-game

- Any player can leave while the game runs: their pause menu has
  **Leave game**. It is `CL_SplitDrop( slot )` — a normal disconnect of
  that one context (server sees an ordinary player leaving), VM and
  socket freed, layout recomputed for the remaining views.
- **Slots are sparse and never renumber.** P2 leaves → P1, P3, P4 stay
  P1, P3, P4 (names, colours, binds, scores untouched). Views are
  compacted in slot order. The next joiner takes the **lowest free**
  number (P2), not P5.
- **Player 1 is the host slot** (context 0 owns the listen server /
  primary connection, UI VM, downloads). P1's exit is the mod's normal
  "leave arena" and ends the session for everyone. If P1's *person*
  wants out while others keep playing, a later nicety is "hand host to
  P<n>" (swap contexts' pads/profiles, not connections). Not in M2.

### 12.2 Per-player pause menu (engine-drawn, mod-agnostic)

The mod's own menu is the single full-screen UI VM, so it cannot be a
per-player menu. Players 2+ (and P1's pad, by option) get a small
engine-drawn overlay inside their own cell, opened with Start, drawn
with the engine console font, navigated with d-pad/stick + A/B; the
game keeps running for everyone else and that player's usercmds are
idle while it is open. Items:

- Resume
- Controls: look sensitivity X/Y, invert Y, deadzones, crouch toggle,
  walk/run default (always-run), vibration if available; rebind
  (press-a-button capture) for the bind list in `default_pad.cfg`
- Profile: name, switch/save
- Leave game

P1 with keyboard/Esc still gets the mod's full menu as today. The
splitscreen settings page (`cl_splitFill`, `cl_splitWidePlayer`, join
button/hold, hint) is reachable from P1's overlay and as cvars.

### 12.3 Profiles (settings follow the person, not the player number)

- On join (press the join button) the cell shows a profile picker
  before connecting: **Default (guest)**, saved profiles, **New
  profile…**. The profile last used with this physical pad (by GUID) is
  pre-highlighted, so returning players join with two presses. A
  profile already in use by another player is greyed out.
- **Default (guest):** engine/mod default binds and settings; changes
  made in the pause menu apply for the session and are discarded when
  the player leaves or the game quits. Name "Player N".
- **Named profile:** changes are saved immediately. Stored outside
  `q3config.cfg` so slots stay stateless:
  - `<homepath>/profiles/<name>.cfg` — identity and feel, shared by all
    mods: name, sensitivity, invert, deadzones, toggles.
  - `<homepath>/<game>/profiles/<name>.cfg` — per-mod: pad binds,
    model/skin/team colours and other userinfo. First use in a new mod
    starts from that mod's `default_pad.cfg`.
    *(As built R11: "that mod's defaults" = `<game>/default_pad.cfg` through
    the VFS if present, else an engine-embedded per-game entry keyed by the
    game dir (`q3ut4` built in, `in_gamepad.c` `padGameDefaults`), else the
    baseq3 set — so UrT's layout ships inside our exe and nothing is written
    into the game folder. Userinfo cvars that are also serverinfo (UrT's
    `g_gametype`) are shared, never saved in profiles.)*
- Loading a profile into slot N fills that slot's `p<N>_*` cvars and
  `pbind` table in memory; nothing per-slot is archived any more.
  `p<N>_padId` persistence is replaced by a small `padlast.cfg` map of
  pad GUID → last profile (pre-highlight only) plus in-session
  auto-heal.
- P1 on keyboard/mouse keeps using the archive file (`q3config-ss.cfg`
  since L5, §17.4) exactly as upstream uses `q3config.cfg`
  (profile optional), so single-player behaviour is unchanged.
- Console/test hooks: `profile_list`, `profile_load <slot> <name>`,
  `profile_save <slot>`, `addplayer [n] [profile]`.

### 12.4 On-screen keyboard

Engine-drawn grid in the player's cell (letters, digits, space, shift,
backspace, done), d-pad/stick to move, A = type, X = backspace, Y =
space, Start = done, B = cancel. Used for the profile name (which is
also the player name; sanitised for a filename, max 20 chars). Built
as a reusable per-player text-entry widget so it can later feed chat.

### 12.5 Added M2 tasks

- **M2-T6** per-player overlay framework (cell-local draw + pad
  navigation + input capture) and pause menu incl. Leave game.
- **M2-T7** profiles (files, picker on join, guest semantics,
  pad→last-profile pre-highlight).
- **M2-T8** on-screen keyboard.
- M2-T3 shrinks to in-session auto-heal; M2-T5 (the maintainer's pad pass)
  gains: leave as P2 with four players → P1/P3/P4 remain, rejoin
  becomes P2; create a profile with the on-screen keyboard, change
  sensitivity, quit, restart, join on a different player number →
  settings follow the profile; guest changes do not persist.
- Agent verification without pads: `padinject` drives the overlay,
  picker and keyboard; screenshots of each.

---

## 13. Guests, menus with a pad, virtual mouse, master settings (maintainer, 2026-10-05)

Amends §12.

### 13.1 Guest profile

"Default" is named **Guest**. Any number of players may be guests at
once; each guest's settings live only in that slot's in-memory
`p<N>_*` cvars / bind table, so P1 at sensitivity 80 and P2 at 150 do
not affect each other, and nothing persists. Guest name = "Player N".

### 13.2 Pad control of menus (applies to every menu: mod UI, engine overlay)

Only while that player has a menu open — never in game:

| Pad | Menu action |
|---|---|
| Left stick / d-pad | step through items (sent as arrow keys, with repeat) |
| A | accept (`K_ENTER`) |
| B | back (`K_ESCAPE`) |
| Right stick | moves that player's own cursor (mouse deltas, speed cvar `cl_padCursorSpeed`, mild acceleration) |
| Right trigger | left click (`K_MOUSE1`); left trigger = right click (`K_MOUSE2`) |
| Start | close menu |

Each player has an independent cursor position, clamped to their cell.
The cursor is whatever the menu itself draws (mod UI draws its own).

### 13.3 Per-player mod menus = one UI VM per player (CORRECTS §8/§12.2)

§12.2 assumed the mod's menu had to stay a single full-screen UI VM.
That is not good enough: Urban Terror's team/gear/weapon selection is
in the mod's UI module and is mouse-driven, and every player needs it.
Use the cgame technique again:

- `VM_UI2..8` slots; in game, each joined player gets their own UI VM
  instance, told (GetGlconfig) that their cell is the screen, with
  draw calls offset/scissored into the cell and `UI_MOUSE_EVENT` /
  `UI_KEY_EVENT` fed from that player's pad (§13.2) — so each player
  opens the mod's real in-game menu in their own view, with their own
  cursor, while the others keep playing.
- UI syscalls execute in the caller's context: `Cbuf`/`ExecuteText`
  commands (`team red`, `disconnect`→ leave for that player only) and
  key-catcher state are per context.
- **Cvar virtualisation for VM syscalls from contexts > 0:** reads and
  writes of `CVAR_USERINFO` cvars (name, model, UrT `gear`, `racered`,
  …) plus a small configurable list (`sensitivity`, `cg_fov`-type only
  if the maintainer asks) are redirected to the slot's `p<N>_<cvar>` shadow;
  everything else stays shared. Same hook serves cgame (§5.2).
- UI `Key_SetBinding`/`Key_GetBinding` traps from contexts > 0 map to
  that player's pad bind table where the key is a pad key; keyboard
  keys are ignored for them.
- Before a game (main menu, server browser, map select) there is one
  full-screen UI VM as upstream, driven by P1's pad or keyboard/mouse.
- The engine overlay (§12.2) stays, but only for what mod menus cannot
  know: Start opens it → Resume / **Game menu** (opens the mod's menu
  in this cell) / Controls / Profile / Leave game. Mods that pop their
  own menu (UrT team select on connect) open straight in the cell.
- **Spike S6 (new, before M2-T6):** two UI VM instances, P2's in-game
  menu in the bottom cell driven by injected pad input, `team`/model
  change affects only P2. Risks: UI modules that assume full-screen
  cinematics/model previews (extra RenderScene calls — covered by the
  same offset), UI memory per instance, cvar redirect completeness,
  mods whose UI reads server-info or `cl_*` state through traps.
  Fallback if S6 fails: single UI VM lent to one player at a time,
  drawn in their cell.

**As built (R11, Urban Terror).** Mods open their in-game menus with
console commands, not only through the engine: UrT's cgame runs
`ui_selectteam` on connect. A command that no engine/cgame code claims, run
as player 2–8, is offered to that player's own UI VM
(`UI_CONSOLE_COMMAND`, `CL_SplitUIGameCommand`; the instance starts on
first use — a mod's UI registers no command names, so any unclaimed command
starts it once). Extra players' cgame console commands go to a per-player
queue run at their next frame (UrT queues `-button13;-button14;` every
frame; through the shared buffer four players overflowed it during a
`wait`). The "small configurable list" of per-player non-userinfo cvars is
data: `playercvar <name|prefix*>` lines in the game's factory pad data
(UrT: `ui_gear*` — its gear menu edits the choice in shared `ui_gear*`
cvars — and `weapmodes_save`). UrT's radio menu (`ui_radio`) is drawn by its
cgame and catches no keys (no `KEYCATCH_CGAME`, no `Key_IsDown` of digits
seen; its digit binds `ut_weaptoggle …` do not select in it): with a pad,
quick radio calls (`ut_radio g m`) are bound instead.

### 13.4 Master splitscreen settings

Not per profile and not per player. They belong to the **host (P1)**:
a "Splitscreen" page in P1's overlay (drawn in P1's cell in game, full
screen at the main menu), containing `cl_splitFill`,
`cl_splitWidePlayer`, 2-player orientation, join button / press-or-hold,
join hint, max players. Other players do not get the page, so there is
no conflict; changes apply live to everyone and are saved in
`q3config-ss.cfg` (§17.4).

---

## 14. Join hint, P1 input, guest defaults, aim feel, Linux (maintainer, 2026-10-05)

### 14.1 Join hint must never sit on screen permanently

Receivers such as 8BitDo adapters (and ViGEm virtual pads) report a
connected controller with nobody holding one. `cl_splitJoinHint`:

- `1` (default) — activity-based: shown only for an unjoined pad that
  produced real input in the last ~10 s.
- `2` — timed: "Press A on a controller to join" flashes for 10 s at
  game launch / map start only. Fallback if the maintainer's adapter test shows
  mode 1 still flashing (adapters emitting noise).
- `0` — off.

The host Splitscreen settings page (§13.4) always shows a static note
with the current join and leave shortcuts ("Join: press A · Leave: hold
Back+Start"), so the hint is not the only place to learn them.

### 14.2 Player 1 input device

Most splitscreen sessions are pads only. `cl_splitP1Input` (host
setting, archived):

- `pad` (default) — the first pad to join is P1. Keyboard/mouse still
  drive the console and full-screen menus, and still work as P1's game
  input (harmless, useful for testing).
- `kbm` — P1 is the keyboard/mouse player; the first pad to join becomes
  P2.

Replaces `cl_kbmPlayer` / `cl_splitPadSharesP1`.

### 14.3 Guest defaults editable by the host

Host settings get a **Guest defaults** page: sensitivity, invert, aim
curve (§14.4), toggles and the pad bind set that every Guest starts
from. Stored in `<homepath>/profiles/_guest.cfg` (feel) and
`<homepath>/<game>/profiles/_guest.cfg` (binds), falling back to
`default_pad.cfg`. A guest's own in-session changes still vanish when
they leave (§13.1); editing Guest defaults affects guests who join
afterwards.

### 14.4 Aim feel (native-pad aiming, not mouse emulation)

Per player/profile, with presets so nobody has to tune five numbers:

- `joy_aimCurve` preset: `linear`, `standard` (power curve ~1.8–2.0),
  `dynamic` (S-curve: fine near center, fast ramp mid-stick) — default
  `standard`. `joy_aimExponent` overrides for custom.
- Separate horizontal / vertical max turn speed (deg/s); vertical lower
  by default.
- **Turn acceleration:** when the stick is held near full deflection
  (> ~90 %) turn speed ramps from the base max to `joy_turnBoost` ×
  over `joy_turnBoostTime` ms (defaults ~1.5× over 250 ms), resetting
  on release — the Call of Duty-style "extra yaw".
- Radial inner deadzone with rescale (no jump at the edge), axial
  snap off, outer deadzone.
- Zoom/ADS multiplier (`joy_zoomScale`, default ~0.5) applied while the
  player's zoom button is held (and, where detectable, when the mod
  reports a reduced fov).
- Light smoothing (1–2 frame) optional, off by default.
- No aim assist in the aim-feel layer itself; the separate, local-games-only
  assist is section 15 (as built R12: hooked into `CL_GamepadMove` after
  the curve/boost/zoom/smoothing, before the angles are applied).

All exposed in the per-player Controls menu and in Guest defaults.
Defaults get tuned from the maintainer's real-pad feedback.

### 14.5 Linux / Steam Deck (new milestone M6, after Windows is done)

- Quake3e's Linux client already uses SDL2 (`code/sdl/`); `in_gamepad.c`
  must use the engine's SDL there (no dlopen/SDL_Quit of its own) and be
  polled from `sdl_input.c`. Steam Input presents Deck controls and
  docked pads as standard controllers.
- Build with the upstream Makefile/CMake on the target (Bazzite is
  immutable: build inside distrobox/toolbox or a flatpak SDK); check
  Vulkan renderer on the Deck GPU, 1280x800 layouts, Gamescope
  fullscreen, on-screen keyboard vs Steam's.
- Test box: the maintainer's GPD Win Mini 2025 running Bazzite — clone the
  GitHub repo there and run an agent locally for build + headless
  `padinject` tests; the maintainer does the hands-on pass.
- Prerequisite: the fork exists on GitHub (the maintainer's call when).

---

## 15. Aim assist, local games only (maintainer, 2026-10-05) — milestone after aim feel (§14.4)

The maintainer wants it only if it is the subtle Call of Duty kind, not a
snap/aimbot. Engine-side and mod-agnostic, per player, pad input only.

**Hard gate (code, not a cvar):** active only when this process hosts
the game (`com_sv_running` and the context's server address is the
local listen server). Any connection to a remote server forces it off
for every player and the option is shown greyed out with "local games
only". Never sent to or detectable by a server because it only shapes
the local player's own stick input.

**Inputs available without mod changes:** each context's snapshot
(player entities: origin, velocity via trajectory, eFlags dead bit),
own playerState (view origin/angles, team via persistant/configstring
`t` key), and the loaded collision map for line-of-sight traces
(`CM_BoxTrace`). Target = visible, alive, non-teammate player entity
(bots included). Mods with unusual entity types get a per-game
`aimassist.cfg` (entity type / team key overrides); default baseq3
conventions, which Urban Terror shares.

**Components (both scale with a per-player strength 0–100, default
mild):**

1. *Aim slowdown* — when the crosshair is inside a target's "bubble"
   (angular radius scaled by distance, larger at close range, capped),
   stick look speed is multiplied down (to ~0.6 at center, smooth
   falloff to 1.0 at the bubble edge). Needs line of sight; fades in/out
   over ~100 ms so there is no stickiness when sweeping past.
2. *Rotational assist* — while the player is giving movement or look
   input (never when both sticks are idle: it must not aim for you), a
   fraction (≤ ~35 %) of the target's angular velocity relative to the
   view (from both players' motion) is added to yaw/pitch while the
   crosshair is inside the bubble. This is what makes strafing fights
   feel "sticky" without snapping. Hard cap in deg/s; zero outside the
   bubble; disabled for ~150 ms after a 180° flick.
3. No snap-to-target on zoom, no bullet magnetism (would need server
   changes).

Range limit (cvar, default ~1500 units), reduced strength while zoomed
with a scoped weapon is not attempted (weapon knowledge is mod-side).

**Fairness / visibility:**
- Host setting "Allow aim assist" (default on for local games); each
  player's Controls menu has Aim assist: Off / Low / Standard.
- Marker so everyone can see who uses it: the player's name gets a
  short suffix (e.g. ` ^3+`) through the userinfo name — the only
  mod-agnostic way to show it in every mod's scoreboard and kill feed.
  Engine also shows a small glyph in that player's own cell.
- Tuning needs the maintainer with a real pad against bots; ship defaults weak.

**Verification without pads:** `padinject` stick input + bots on a
fixed path; log yaw-rate with assist on/off inside and outside the
bubble; show it is exactly zero when connected to a second (remote)
server process.

### 15.1 As built (R12, `code/client/cl_aimassist.c`) — corrections to the above

- **Strength is 0/1/2 (Off/Low/Standard), not 0–100**: `p<N>_joy_aimAssist`,
  a pad feel setting (profiles, Guest defaults); default **Low**. Low =
  slowdown to 0.8 at the center + 18 % rotation; Standard = 0.6 + 35 %.
  Every other constant is a host cvar (`joy_aimRange`, `joy_aimBubble`,
  `joy_aimBubbleMax`, `joy_aimSlowLow/Standard`, `joy_aimRotLow/Standard`,
  `joy_aimRotCap`, `joy_aimFade`, `joy_aimFlick`, `joy_aimFlickTime`).
- **Per-game data is a built-in table** keyed by game dir (baseq3,
  missionpack, q3ut4: player eType, dead flag, team key, spectator value,
  chest height, body axis, gametypes without foes, marker), not an
  `aimassist.cfg`; unknown games use baseq3's. UrT verified: players are
  eType 1 with eFlags 0 alive, team `t` 1/2/3; a dead UrT player is not in
  the others' snapshots (or turns into eType 15 with eFlags 0x4040), EF_DEAD
  was never seen; UrT jump mode (`g_gametype 9`) has no foes.
- **Bubble angle is measured to the target's vertical body axis** (the
  view ray's height at the target's distance, clamped feet..head), not to
  a single chest point: a close target below eye level would otherwise
  sit outside its own bubble. Line of sight and angular velocity use the
  chest point.
- **A flick (look-stick rate > `joy_aimFlick`) switches off both
  components** for `joy_aimFlickTime` ms (the doc said rotation only):
  sweeping fast past a target is never slowed.
- **Gate:** `com_sv_running` and player 1's address `NA_LOOPBACK` and not a
  demo and `!CL_SplitRemoteServer()`; plus the host cvar `cl_aimAssistAllow`
  (archived) and the player having a pad (keyboard/mouse never assisted).
- **Marker:** added to the *outgoing userinfo* name only (connect + every
  update; player 1 forces a resend when its state changes); the `name` cvar,
  profiles and q3config never carry it. baseq3/missionpack ` ^3+`; UrT `+`
  (UrT strips spaces and colour codes from names). `cl_aimAssistMarker`
  (`auto`, `0` = none, or the text). Engine glyph: a small yellow `+` in the
  top right corner of the player's cell.
- Target positions are lerped between the previous and the latest snapshot
  at `cl.serverTime` (extrapolated with `trDelta` at most 100 ms past the
  latest); angular velocity is the low-passed (~50 ms) derivative of the
  world angle from the eye to the chest point, so the player's own turning
  is not in it; a jump > 720 deg/s (teleport, respawn) restarts it.
- **R14a retune (the maintainer felt nothing on Low or Standard with a real pad).**
  No gate was a no-op for a real SDL pad (his log shows the ` ^3+` marker
  on, i.e. `CL_AimAssistOn` true; real and virtual pads share
  `CL_GamepadMove`): the R12 numbers were simply too weak -- a 40-unit
  bubble (~3-4 deg at fighting range), full slowdown only at the exact
  center (smoothstep from 0), 18 / 35 % follow capped at 60 deg/s:
  `r12-aim` averaged 9 % (Low) / 19 % (Standard) slowdown inside the bubble
  and 4 / 8 deg/s of follow. New defaults: `joy_aimBubble 72`,
  `joy_aimBubbleMax 12`, full slowdown over the inner 40 % of the bubble
  (`AA_CORE`, smoothstep outside it), `joy_aimSlowLow 0.7`,
  `joy_aimSlowStandard 0.45`, `joy_aimRotLow 0.3`, `joy_aimRotStandard
  0.6` (range now 0..0.8), `joy_aimRotCap 120`. `r12-aim` after: Low 22 %
  average slowdown, 25 % follow; Standard 43 %, 51 %; idle input still
  exactly 0, flick still pauses both. Exceeds this section's original
  "<= ~35 %" on purpose (maintainer: Standard must be obvious). Marker colour
  per level: ` ^3+` Low, ` ^2+` Standard (glyph yellow / green; UrT plain
  `+`). One log line per change of a player's effective state: `AA P1:
  Low (Guest)`, `AA P2: off: not a local game ...`.
- **R17 (the maintainer's pass 2): Low a little stronger, default Off.** Standard
  unchanged. Low and Standard share the bubble (`joy_aimBubble 72`,
  `joy_aimBubbleMax 12`, `AA_CORE` 0.4), so only the two strength numbers
  differ; Low moved a quarter of the gap towards Standard:
  `joy_aimSlowLow` 0.7 -> **0.64** (Standard 0.45), `joy_aimRotLow` 0.3 ->
  **0.375** (Standard 0.6). `r12-aim` (`work/r17-aim-table.txt`): Low A
  sweep min 39.20 / mean 45.16 of 61.25 deg/s (R14a 42.88 / 47.96, i.e.
  26 % average slowdown, was 22 %), C track rot -11.15 of omega -35.81
  (31 % follow, was 25 %), D -9.57 / -25.51; Standard A 27.56 / 35.08, C
  -19.91 / -37.10 (same as R14a); B idle 0, E flick 226 ms, F wall 0
  targets. **Default strength `joy_aimAssist` 1 -> 0 (Off):** Guests and
  new profiles start Off (a new profile copies the Guest defaults, which
  come from `joy_aimAssist` unless `profiles/_guest.cfg` says otherwise);
  saved profiles and a saved Guest defaults file keep their value. The
  host switch `cl_aimAssistAllow` stays 1 (Allowed).

---

## 16. Independent sessions per player (the maintainer asked, 2026-10-05) — NOT planned in-engine

Idea: after "Leave Arena" a splitscreen player gets their own main menu
and can join a different server or start a private bot match.

Networking would allow it (each context already has its own
connection), but everything below is single-instance per process and
assumes one level: renderer world (`tr.world`, lightmaps, vis, VK
buffers), collision map (`cm`, used by every cgame's prediction),
filesystem search path / `fs_game` / pure-server pak list, the local
server (`sv`, one per process → one private bot match at most), sound
and hunk level lifetime. Different servers mean different maps and
often different mods, so this is a renderer/CM/FS multi-world rewrite —
far outside the "small diff" goal.

Decision: in-engine, all local players share one server. For players
2+, the mod menu's "Leave Arena" and "Exit Game" both mean *this player
leaves* (never quits the process); their cell disappears and the pad can
press join again.

Possible later option if the maintainer wants it ("independent mode"): a
launcher switch that starts N separate engine processes in borderless
windows tiled on the screen, each restricted to one pad (by GUID) and
its own profile/homepath config — Nucleus-style but built in, no
hooks. Cost: N× memory and N× map load (small for Q3-era games), N
sound mixers (only the focused or all — to decide). Roughly one to two
rounds of work, independent of the rest. Not scheduled.

---

## 17. Product shape and Independent mode (maintainer, 2026-10-05)

### 17.1 The main exe is the game, never a launcher

- Shipped client is named `quake3e-vulkan-ss.x64.exe` (Vulkan;
  OpenGL `quake3e-ss.x64.exe` beside it; upstream's names + `-ss`, same
  as Linux, since R15; `quake3e-splitscreen.exe` before). Double-click → fullscreen main menu, P1
  on a gamepad by default (§14.2), usable start to finish without
  keyboard or mouse. Others join in game by **holding A**
  (`cl_splitJoinHold` default becomes ~700 ms; still configurable to a
  press) and the screen makes room. No pre-launch configuration of
  player count, windows or devices, ever.

### 17.2 Independent mode (scheduled: after M5, before Linux M6)

Each player gets a completely separate game (own main menu, server,
mod, bot match) — §16's multi-process idea — but with the same
join-in-place behaviour as the normal mode and no launcher UI.

- **Entry:** host Splitscreen settings → "Session mode: Together /
  Independent" (applies on next start of a game or via an in-place
  relaunch), or start directly with `--independent`
  (`+set cl_splitIndependent 1`; e.g. a second Steam/desktop shortcut).
- **Coordinator = P1's process** (the one the user launched). It runs
  borderless instead of exclusive fullscreen, owns detection of
  unjoined pads and shows the same join hint. When an unjoined pad
  holds A, the coordinator spawns a **child process** of the same exe
  (`--child`, borderless, bound to that pad's GUID, started with the
  picked profile) and re-tiles all windows with the same layout
  function and options (`cl_splitFill`, wide player, orientation) as
  Together mode. Player count and placement are therefore never chosen
  up front; they adjust as people join and leave.
- **Re-tile protocol:** coordinator → child over a local named pipe (or
  loopback UDP): `rect x y w h`, `quit`, `focus`; child → coordinator:
  `hello`, `bye`. A child applies a new rect by moving/resizing its
  borderless window + `vid_restart` (the same sub-second hitch as a
  join in Together mode). Child exit (its player picks Exit Game, or
  holds Back+Start) → coordinator re-tiles. Coordinator exit → children
  are told to quit (or optionally promoted; not in v1).
- **Input:** each process opens only its own pad (GUID + ordinal filter
  in `in_gamepad.c`; background input already works). Keyboard/mouse go
  to whichever window has focus (coordinator by default); never
  required.
- **Per-process state:** same `fs_basepath`; shared `fs_homepath` but
  children do not write `q3config-ss.cfg` (profile files only; guests
  write nothing) and each uses its own `qconsole`/pk3cache name to
  avoid write races. Each child has its own net port (already
  automatic).
- **Sound:** every process mixes its own audio and they all play (as
  with Nucleus); per-player volume in the pause overlay. Windows must
  keep audio when unfocused (`s_muteWhenUnfocused 0` forced in this
  mode).
- **Limits to state plainly:** N× memory and N× map loads; exclusive
  fullscreen is unavailable (borderless only); a child cannot join the
  coordinator's Together session without restarting into that mode.
- **Verification without pads:** `padinject` in the coordinator to
  simulate join holds; children started with a virtual-pad flag;
  evidence = each process's own `screenshotJPEG` + logs showing rects
  after 2, 3, 4 joins and after a leave (no desktop screenshots).

### 17.3 As built (R13, `code/client/cl_splitindep.c` + `code/win32/win_splitproc.c`) — corrections to 17.2

- **Entry.** `cl_splitIndependent` (archived; host page row "Session
  mode", last selectable row so earlier rows keep their index) or the
  launch flag `--independent` (this run only; the cvar is not touched).
  Switching **in place**: the row takes effect at once (full `vid_restart`
  for the new window style) when nobody else plays in the old mode;
  otherwise it is pending ("Independent starts when the others have
  left" / "Together starts when the other windows close") and applies the
  frame after. In Independent mode pads join from the coordinator's main
  menu too (the join hint is drawn there as well); `addplayer [n]
  [profile]` spawns a pad-less window (keyboard when focused).
- **Picker** runs in the coordinator, **centered** in its window (the slot
  has no client context; `CL_SplitViewRect` gives the whole window), not
  where the new window will appear. The picked profile is passed on the
  command line; the coordinator knows every window's profile (each child
  reports its key in its heartbeat), so pickers there grey out profiles
  played in other windows. A child's own Switch/Delete pages list only
  Guest + the profile it was started with / plays (no `inuse?` query).
- **IPC = loopback UDP, not a named pipe**: one non-blocking datagram
  socket per process on 127.0.0.1 (ephemeral port), text messages
  prefixed with a per-session token from the child's command line
  (`cl_splitChildIpc "port token coordinatorPid"`). Chosen because it is
  polled from the frame with no overlapped I/O, no per-child pipe
  instances or connect races, one socket serves all children, and the same
  BSD-socket code works on Linux (M6). Coordinator -> child `rect x y w h`
  every 500 ms (doubles as heartbeat; a lost datagram heals itself),
  `quit`, `focus`; child -> coordinator `hello`, `hb <player> <pid>
  <profile key>` every 500 ms, `bye`. A child's death is seen through its
  **process handle** at once (taskkill, crash); silence for 15 s (hung) or
  no hello within 30 s -> killed. Children run in a kill-on-close **job
  object**, so they die with the coordinator however it ends; they also
  quit when the coordinator's pid disappears. Coordinator quit: `quit` to
  all, 3 s grace (measured: ~150 ms), then kill.
- **Re-tile = full `vid_restart`** (the window is destroyed and created at
  the new rect, the normal start path). **Not** "move the window +
  `vid_restart fast`": with the Vulkan renderer that drew past the new
  window height (NVIDIA Xid 13 "3D HEIGHT CT Violation", device lost,
  driver reset) on this box. Cost: the same sub-second hitch as a Together
  join; a recreated window takes the foreground only if the old one had it.
- **Window placement** goes through `CL_SplitWindowRect()` (hooks in
  `CL_GetModeInfo` and `win_glimp.c`: no exclusive fullscreen, borderless,
  x/y), fed from `cl_splitIndepArea "x y w h"` (default: the whole
  monitor) via the layout function — nothing is written to the archived
  `r_*` / `vid_*` cvars, so a later normal start is unchanged (WM_MOVE no
  longer saves vid_xpos/ypos while the hook is active). Children also get
  the `+set r_fullscreen 0 r_noborder 1 r_mode -1 ...` the design listed.
- **No focus stealing:** `--child` (and the test flag `--noactivate`,
  which also opens windows at the bottom of the z-order) shows the game
  window with SW_SHOWNOACTIVATE, never shows/foregrounds the early
  console, and starts with `gw_active` off (no mouse grab) until clicked.
- **Pads:** the child opens only `cl_splitChildPad "guid#ordinal"` (R19: replaced by device keys, 17.5)
  (ordinal = which of several same-GUID devices in SDL order); every other
  SDL device is skipped unopened; its pad is its player 1 at once; it
  never joins, heals or draws the hint; Back+Start in a child = quit. The
  coordinator **keeps the device open but inert** (the pad stays assigned
  to the child's player number; only its picker took keys) and frees it
  when the window is gone (exit, kill, cancel). Virtual test pads: a
  `virtual-*` GUID makes the child create that virtual pad itself.
- **Files:** a child never writes (or seeds, §17.4) `q3config-ss.cfg`
  (`Com_WriteConfiguration` returns for `cl_splitChild N`), `_p1q3config.cfg` (not written, not
  restored/removed), `_padlast.cfg`, `_guest.cfg`, or migrates
  `splitpads.cfg`; it writes only its own profile files. Logs
  `qconsole-child<N>.log`, pk3 cache `pk3cache-child<N>.dat`.
- **Sound:** every window mixes its own (WASAPI shared); in Independent
  mode `s_muteWhenUnfocused` is ignored in `snd_mix.c` (the archived cvar
  is left alone); "Window volume" (`s_volume`) row on the pause page and
  on a child's own-window page (Start at its main menu: Controls, Profile,
  Window volume, Exit game).
- **Aim assist** in a child: its own local game passes the gate as is
  (verified: `AA P1 lvl 1`, name marker `+`).
- **Not done / limits:** unfocused windows run at `com_maxfpsUnfocused`
  (60) — all but the focused one; exclusive fullscreen is unavailable; the
  coordinator's picker is not drawn where the new window will go; `focus`
  IPC (`indepfocus <player>`) is implemented but untested (it would take
  the foreground); Windows only (stubs elsewhere).
- **Linux / Steam Deck (14.5) must differ:** spawning = `fork`/`posix_spawn`
  of `/proc/self/exe` (or the AppImage/flatpak entry point) with the same
  arguments, children in the coordinator's process group plus
  `prctl(PR_SET_PDEATHSIG, SIGTERM)` instead of a job object, `waitpid
  (WNOHANG)` instead of process handles; the UDP code is portable as is.
  Window placement: X11 can position borderless windows (`_MOTIF_WM_HINTS`
  / SDL borderless + `SDL_SetWindowPosition`); **Wayland cannot** position
  client windows at all, and **Gamescope** (Steam Deck game mode) shows one
  focused fullscreen surface — tiling separate processes there needs
  Gamescope's own multi-window support or a nested compositor, so on the
  Deck Independent mode is likely desktop-mode (X11/XWayland) only, or a
  single-window fallback (Together). SDL background joystick events stay on in
  every process (`SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, already set), and Steam Input may expose
  each pad only to the process Steam launched.
  **As built on Linux (L3, 2026-10-06, `code/unix/unix_splitproc.c`):**
  fork + exec of `/proc/self/exe` (child stays in the coordinator's
  process group, `PR_SET_PDEATHSIG` SIGTERM + `getppid` re-check, fds
  closed, stdin /dev/null); `waitpid(WNOHANG)`; kill = SIGTERM, 2 s,
  SIGKILL; UDP as on Windows. In this mode (`--independent`, `--child`,
  `--noactivate`, or `cl_splitIndependent 1`) `SDL_VIDEODRIVER=x11` is set
  before SDL starts, and an in-place switch restarts SDL video on x11;
  Together mode stays on native wayland. Tiles = SDL's usable display
  bounds (panel excluded). No-focus windows are created hidden, get
  `_NET_WM_USER_TIME` 0 (libX11 loaded at run time) and SDL's no-activate
  hints, then shown; `--noactivate` also lowers them. Gamescope
  (`GAMESCOPE_WAYLAND_DISPLAY`) or no DISPLAY -> `Sys_SplitUnavailable()`
  -> Together with a console line and the row note.

- **Taskbar / fullscreen detection (R16, Windows, the maintainer picked option 1):** every borderless tile
  window (coordinator and children) is marked with `ITaskbarList2::MarkFullscreenWindow(hwnd, TRUE)`
  after it is shown (`GLW_SplitMarkShell` in `win_splitproc.c`, called from `GLW_SplitShowWindow`)
  and unmarked before `DestroyWindow` (vid_restart re-tile, the Together switch, quit:
  `GLW_SplitUnmarkShell` in `win_glimp.c`); a Together window is never marked. The shell then puts
  the taskbar behind the active tile and `SHQueryUserNotificationState` reports a fullscreen app
  (Joyxoff and similar tools). `cl_splitIndepMarkFullscreen` (default 1, next vid_restart). Log:
  `window: P<n> marked fullscreen for the shell` / `no longer marked ...`; a failed COM call logs its
  HRESULT. Linux: nothing (no equivalent asked for).

### 17.4 Configs side by side with upstream (maintainer, 2026-10-06; as built L5)

Users keep vanilla Quake3e and our `-ss` executables in the same game
folder. Everything stays shared exactly as upstream lays it out — base
path, home path (Windows: the exe's folder; Linux: `~/.q3a`), paks,
downloads, `autoexec.cfg`, `profiles/` — **except the archived settings
file**:

- `Q3CONFIG_CFG` (`code/qcommon/qcommon.h`) is `q3config-ss.cfg` for our
  clients and `q3config_server-ss.cfg` for our dedicated server; every user
  of the define follows (startup exec, game-restart exec, the files.c
  fallback exec, `Com_WriteConfiguration`, `exec`'s DELAY_WRITECONFIG
  check, `FS_BannedPakFile`). `Q3CONFIG_CFG_UPSTREAM` names upstream's
  file (`q3config.cfg` / `q3config_server.cfg`); pk3s may carry neither,
  nor `autoexec-ss.cfg`.
- **Seed** (`Com_SeedSplitConfig`, common.c): right before
  `exec Q3CONFIG_CFG` (Com_ExecuteCfg: startup and `game_restart`; files.c
  invalid-game fallback), if `<homepath>/<game>/q3config-ss.cfg` does not
  exist and upstream's file exists **in the current game dir** (home path,
  then base path), copy it byte for byte and print
  `config: q3config-ss.cfg created from q3config.cfg`. A mod without its
  own `q3config.cfg` is not seeded from baseq3's (upstream would exec
  baseq3's file there; ours execs baseq3's `q3config-ss.cfg` the same
  way). Never in an Independent-mode child, never in safe mode, never over
  an existing `q3config-ss.cfg`; upstream's file is only read.
- **`autoexec-ss.cfg`**: exec'd after `autoexec.cfg` (same safe-mode
  guard) when it exists — an existence probe, so a missing file prints
  nothing — for `-ss`-only overrides. Upstream never reads it.

### 17.5 As built (R19): pad ownership, backdrop (removed R20), experimental status

**Status: experimental.** The Session mode row reads "Independent
(experimental)" with the help line "Separate windows per player; still being
made reliable" (the help footer wraps for that row only; a choice value may
use the width its label leaves). `cl_splitIndependent`'s description and the
"... starts when the others have left" note say the same. Together mode is
unchanged.

**What went wrong (the maintainer's 3-pad test, 0.0.0.23; his logs
`work/q3home/baseq3/qconsole*.log`, 2026-10-06 22:38-22:41).** Pads were
handed to windows as `guid#ordinal` = "the n-th device with this GUID in
this process's SDL list". (1) Two identical 8BitDo Ultimate 2 pads share a
GUID and SDL lists them in a different order in each process: P2's window
got `#1`, which in its own list was P1's pad -> P1 moved two windows, P2
moved none. (2) The coordinator listed the 8BitDo 2C twice (two raw-input
devices, same GUID; its two listings logged identical input), a window
listed it once: holding A joined both listings (P3 and P4 pickers), P3's
window was given `#1`, which does not exist in its list -> no pad; the
second listing stayed nobody's -> "press A to join" -> P4 got `#0` and
worked. (3) A replugged pad comes back at the end of SDL's list, so its
ordinal (and with it the old reclaim) no longer matched.

**Keys (`Pad_MakeKey`, in_gamepad.c).** Every device gets a key when it
appears: `<guid>@<FNV-1a of the lower-cased device path>` when SDL has a
path (`SDL_JoystickPathForIndex`: on Windows the raw-input / HID device
interface path -- one per physical device, identical in every process,
unchanged by a replug on the same port; XInput-driver devices "XInput#<slot>",
slots are global), else `<guid>#<n>` (order; fragile across processes,
logged as such). Virtual `padinject` pads keep their name as key.

**Ownership rules.**
- The coordinator alone joins pads, shows the hint and runs the picker. A
  window is handed its pad's key(s) (`cl_splitChildPad "<key> [<alias> ...]"`,
  and again with every 500 ms `rect` as `pad <keys>`; a change -> the window
  looks at the devices it left closed again). A window opens the **first**
  device whose key is in that list and **no second** one ("another device
  of this window's pad: not opened (duplicate)"); every other device is left
  closed ("belongs to another window"). Its heartbeat says whether it has its
  pad (`hb ... <haspad>`); 5 s without -> one yellow coordinator line.
- A pad whose key belongs to a slot (picking or with a window) never joins
  again (`CL_IndepReserve` refuses an owned path key; `Pad_Join` reclaims
  first). Order keys (`#n`) are not trusted for this: after a replug they
  name another device.
- **Duplicates** (one physical pad listed twice), coordinator only:
  (a) same path key as an open device -> not opened; (b) **mirrored input**:
  two pads, at least one nobody's, whose every button and stick key is
  equal while something is held, each held key down within 100 ms on both,
  are one pad -- the nobody's one (of two such, the later device) is the
  duplicate: its input is ignored (no join, no hint, no heal) and its key
  goes into the key list of the other's window (`CL_IndepPadAlias`) only
  after (R19b) the two matched for 1 s and a later press went down on both
  within 100 ms (`... is confirmed as a second listing of P<n>'s pad`); two
  people who pressed together never get that far. Undone when the two
  differ for 150 ms (log: "... moves on its own ... press again to join":
  its presses while merged were ignored, the next join needs a fresh
  press); an alias already given is taken back (`CL_IndepPadUnalias`, the
  window closes that device: "no longer this window's"); the first key is
  never removed. Also undone when the original goes away (an alias then
  stays: the same pad, now one listing). Log `indep: pad <i> (key ..)
  duplicates pad <j> ...`. (a) also applies in **Together mode** (R19b):
  `pad: skipped duplicate listing of <key>`; nothing else changes there.
- **Replug:** the coordinator keeps a window's slot (`... went away; it goes
  back to P<n> when it returns`); a device that comes back with a key the
  window owns is that window's again; a device with a new key (no path) is
  matched by GUID to the one window that lost a pad of that GUID before the
  device appeared (exactly one, else nothing) and the window is told the new
  key. In the window, the device with its key is reopened and auto-heal
  gives it back to its player.
- Log of every decision: `indep: pad <key> -> P1 (pid <coordinator>, the
  coordinator)`, `-> P<n> (pid <child>)` at spawn, `-> P<n> (pid ..,
  this window)` in the window, `-> P<n> (pid ..): reconnected`, `... the
  same pad, another device`.
- **Keyboard/mouse** (audited, unchanged): Windows delivers WM_KEY* and the
  raw mouse (registered without RIDEV_INPUTSINK) only to the foreground
  window, the mouse is active only while `gw_active`, the low-level
  keyboard hook (PrintScreen/Win keys only) is removed on deactivation, and
  remappers (Joyxoff) inject into the foreground window: nothing is
  broadcast to every window.
- Residual risk (SDL, not ours): SDL's raw-input driver takes an XInput pad's
  triggers and Guide from XInput by guessing which XInput slot matches; a
  window that opens only one pad can briefly guess wrong if two pads press
  the same buttons together (SDL corrects itself after 5 mismatching
  updates).

**Test device bus (headless reproduction).** `padbus` writes
`<fs_homepath>/padbus.txt` (one line per device: id, connection number,
guid, path, coordinator-only, mirror-of, buttons, axes); every process with
`in_padBus 1` reads it each frame and opens its devices through the same
attach / filter / key code as SDL devices; `in_padBusOrder 1` lists them in
reverse (SDL's per-process order); a reconnect is appended (SDL's
behaviour); a "coordinator-only" device is invisible to windows; a "mirror"
copies another device's input. Runner `scripts/tests/r19-run.sh`; cases
`r19-pads.cfg` (the maintainer's room), `r19-dedupe.cfg`, `r19-menu.cfg`.

**Backdrop: removed in R20.** R19 put a black never-active window under the
tiles (`cl_splitIndepBackdrop`) and logged the shell's notification state and
the foreground rect every 2 s. The maintainer's test: it does not fool Joyxoff, and
nothing can -- a rect-based "is a fullscreen game running?" check sees the
focused tile, a share of the screen. R20 deleted the window, the cvar, the
restacking and the notification-state logging. The tiles stay marked
fullscreen for the shell (R16 `MarkFullscreenWindow`,
`cl_splitIndepMarkFullscreen`). **Joyxoff (and any tool like it): disable its
bindings (or exclude the game) while playing Independent mode.**

---

## 18. Server options page (maintainer, 2026-10-06) — round R14b

The maintainer's request after his first real-pad pass: a **Server options >** row on
player 1's pause page, above End game, that configures the local listen
server from the couch. Only the host (P1, Together mode; in Independent
mode every window hosts its own match, so every window's P1 page has it).
Greyed on a remote server. The engine cannot change an unmodified mod's
game rules directly; every option below uses an engine-side lever that
works without mod changes. Levers are marked **[native]** (an existing
cvar/command), **[entities]** (the map's entity string is rewritten in
`SV_SpawnServer` before the game VM reads it through `GetEntityToken`),
**[ps]** (the server writes each client's shared `playerState_t`,
`SV_GameClientNum`, after the game frame — baseq3 layout:
`STAT_HEALTH 0, STAT_WEAPONS 2, STAT_ARMOR 3`, `ammo[weapon]`,
`PERS_ATTACKER 2`, `PERS_SPAWN_COUNT 8`, weapons `WP_GAUNTLET 1 ..
WP_BFG 9`; verify against ioquake3's `bg_public.h` and only enable for
baseq3-layout games: baseq3, missionpack, osp/cpma-style mods; **UrT and
unknown mods grey these rows**), **[cheat]** (needs `sv_cheats`: the local
listen server turns `sv_cheats` on internally when such an option is
active — local games only, never a dedicated or remote server; a visible
"cheats on" note on the page).

### 18.1 Rows (in this order)

| Row | Values | Lever | Applies |
|---|---|---|---|
| Restart round | action | `map_restart` [native] | now |
| Change map | every `.bsp` on disk/in pk3s (`FS_ListFiles maps *.bsp`), current first; the map's levelshot (`levelshots/<map>`) drawn as a thumbnail beside the list, placeholder when none | `map <name>` keeping the current gametype [native] | on exit |
| Game type | Free for all, Team DM, Tournament, CTF (+ the mod's extra gametypes only if cheap: skip) | `g_gametype` [native], needs a map load | on exit (restart) |
| Time limit / Frag limit (Capture limit for CTF) | 0 (none), 5..60 by 5 / 0, 5..100 by 5 | `timelimit`, `fraglimit`, `capturelimit` [native] | now |
| Instagib | Off (default), On | preset: Weapons = Railguns only, spawn weapon = rail, Infinite ammo on, no health/armor/powerup/ammo pickups (their entities removed) [entities + ps]; a rail hit (100) kills a 100-health player. Greys Weapons/Spawn/Infinite ammo rows while on | on exit (restart) |
| Weapons | Default, Random, then one "X only" per weapon the game has (Gauntlet, Machinegun, Shotgun, Grenade, Rocket, Lightning, Railgun, Plasma, BFG) | [entities]: all `weapon_*` spawns replaced (Random: a per-map-load seed, one random weapon per spawn point, re-rolled at every restart/map change; "X only": every spawn becomes X; matching `ammo_*` replaced likewise) | on exit (restart) |
| Player weapons at spawn | Machinegun (default), All, Gauntlet only; **when Weapons is "X only" this row shows "X" and is greyed** | [ps]: when `PERS_SPAWN_COUNT` changes, set `STAT_WEAPONS` bits and ammo; "Gauntlet only" zeroes machinegun ammo so the game switches away | at each spawn |
| Infinite ammo | Off (default), On | [ps]: every server frame, ammo of every owned weapon = 999 (gauntlet/grapple untouched) | now |
| Bots | Off, 1..20 (default Off; R16: was 16); `sv_maxclients` raised to fit local players + bots before the next map load | `addbot <random name> <skill>` / `kick` [native]; the count is kept across map changes and restarts | now (adds/kicks live) |
| Bot difficulty | 1 I can win, 2 Bring it on, 3 Hurt me plenty, 4 Hardcore, 5 Nightmare, Random (per bot) | `addbot` skill [native]; changing it re-adds the bots | now |
| Friendly fire | Off (default), On | `g_friendlyFire` [native] | now |
| Self-damage | On (default), Off | [ps]: when a client's health/armor dropped this frame and `PERS_ATTACKER` == its own client number, restore both (knockback stays: rocket jumps work). Verify `PERS_ATTACKER` semantics in ioquake3 `G_Damage` | now |
| God mode | Off (default), All players (humans only), P1 .. P8 | [cheat]: `god` sent as that client (`SV_ExecuteClientCommand`); ioquake3 `ClientSpawn` clears `FL_GODMODE` (`ent->flags = 0`) so it is re-sent after every spawn (`PERS_SPAWN_COUNT` change) — confirm in the source before relying on it; track our own on/off to never double-toggle | now |
| Player health | Default (each player's own handicap), 95 .. 5 by 5 | handicap userinfo for every local player, overriding profiles while set [native] | at next spawn |
| Low gravity | Off (default, 800), Low 600, Medium 400, High 200 | `g_gravity` [native] | now |
| Power-ups (R17) | On (default), Off | [entities]: every power-up and holdable removed: `item_quad`, `item_haste`, `item_invis`, `item_regen`, `item_enviro`, `item_flight`, missionpack `item_scout` / `item_guard` / `item_doubler` / `item_ammoregen`, every `holdable_*` (teleporter, medkit, kamikaze, portal, invulnerability); in any Weapons mode and under instagib too. Off greys Quad damage ("Quad damage: Off (power-ups off)"). Greyed where [entities] does not apply (UrT has no Quake 3 power-ups) | on exit (restart) |
| Quad damage (R16) | On (default), Off | [entities]: `item_quad` spawns removed; greyed where [entities] does not apply (UrT, unknown games) and while Power-ups is Off | on exit (restart) |
| Player speed (R16) | 50..200 % by 10, default 100 | `g_speed` [native], scaled from the game's own value (a value we did not write is the new base; 100 % puts it back); greyed when the game has no `g_speed` and in UrT (its pmove ignores it: `ps.speed` stays 220 at any `g_speed`) | now |
| Weapon respawn (R16) | Default, 1..30 s by 1 | `g_weaponrespawn` + `g_weaponTeamRespawn` [native]; Default = the game's own (`Cvar_Reset` once); greyed when the game has no such cvar (UrT) | now |
| Game volume | 0..150 % by 10, default 100 | master `s_volume` [native]; the cvar is clamped 0..1 today — extend the range to 1.5 only if the mixer scales without clipping into garbage (check `S_PaintChannels`); otherwise cap at 100 % and say so | now |
| Reset to defaults | action: every row back to its default, game type to Free for all and volume to 100 % too (R16); only the map stays | | now |
| Select server settings | list of saved sets | files `serversettings/<name>.cfg` in the homepath (like profiles) | on exit |
| Save server settings | keyboard -> name | | |
| Delete server settings | list; confirm | | |

### 18.2 Behaviour

- **Apply on back-out.** Leaving the page applies everything. Options
  marked "on exit (restart)" trigger one `map_restart` (or `map` when the
  map/gametype changed) with a 1-line notice on the page while editing
  ("applies when you leave: restarts the map"). Nothing restarts when no
  such option changed.
- **State** lives in `sv_split*`/`cl_splitSrv*` cvars (archived in the
  `-ss` config), restored at the next local game; a loaded "server
  settings" set replaces them all. The page title shows the active set's
  name.
- **Remote server:** the row is greyed with "host only". **Dedicated
  server:** none of this code runs (`#ifndef DEDICATED` for the menu; the
  [entities]/[ps] server hooks are in `code/server/` behind a cvar that
  only the client sets and that a dedicated server never sets).
- **Mods:** `[ps]` and `[entities]` rows are available when the game is
  baseq3/missionpack or a mod whose `gamename` is unknown but whose
  entity string contains baseq3 `weapon_*` classnames; **UrT** (`q3ut4`)
  greys Instagib, Weapons, Spawn weapons, Infinite ammo, Self-damage, God
  mode (its gear system and playerState use differ) and keeps the rest.
- **Cheats:** turning on God mode sets `sv_cheats 1` for the listen
  server (internal `Cvar_Set` on the ROM cvar, local only); turning it off
  clears it again once every player's god is off (R14b review fix; a
  devmap game keeps its own cheats). The page says "(enables cheats)".
- **Thumbnails:** draw the levelshot scaled into the page (Together: a
  box beside the rows; eighth cell: skip the picture).
- **Overlay rules as before:** navigated with the pad, scrolls (R14a),
  every row has a one-line help text at the bottom like the Controls page.

### 18.3 Verification (headless)

Virtual pads; evidence `work/r14b-*`: entity rewrite per weapon option
(count of `weapon_*` classnames seen by the game via a developer print of
the rewritten string summary; screenshot of a Rocket-only map), Random
re-rolled on restart, spawn weapons (`STAT_WEAPONS` printed via the
existing `aimassist_ents`-style debug command or a new `srvdebug ps`),
infinite ammo (ammo stays 999 after firing — `p2 +attack`), bots
added/kicked/re-added on map change and difficulty change, friendly fire
and gravity cvars, self-damage (P2 `setviewpos` next to a wall, rocket at
the wall: health unchanged, position moved), god mode (P2 takes a rail
from P1: alive; after respawn still god), player health (handicap shows in
`status`/userinfo), volume cvar, instagib preset (railgun spawns only, no
armor entities, 1-shot kill), save/select/delete sets (files), apply on
exit restarts exactly once, UrT page shows the greyed rows and the rest
works (map change to ut4_turnpike, bots). `r10-single`, `r8-join`,
`r13-two` regressions.

### 18.4 As built (R14b, `code/client/cl_splitsrv.c`, `code/server/sv_splitrules.c`) — corrections to the above

- **Layout numbers (ioquake3 `bg_public.h`, checked 2026-10-06):** `PERS_SPAWN_COUNT` is **4** and
  `PERS_ATTACKER` **6** (not 8 / 2). `STAT_WEAPONS 2`, `STAT_ARMOR 3`, `STAT_MAX_HEALTH 6` hold for
  baseq3 only; **missionpack** has `STAT_PERSISTANT_POWERUP 2`, so `STAT_WEAPONS 3`, `STAT_ARMOR 4`,
  `STAT_MAX_HEALTH 7`, and weapons 11-13 (nailgun, prox, chaingun). Layout = game dir (`baseq3` 1,
  `missionpack` 2, `q3ut4` none), else 1 when the map has baseq3 `weapon_*` classnames.
  `G_Damage` sets `PERS_ATTACKER = attacker->s.number` (`ENTITYNUM_WORLD` without one);
  `ClientSpawn` does `ent->flags = 0` (god cleared); `Cmd_God_f` toggles; `Cmd_Kill_f` clears god.
- **Health is game-private:** `STAT_HEALTH` is a copy of `gentity->health` (`ClientEndFrame`), so
  writing the playerState alone is undone next frame. The server finds the field itself: every int
  offset of a client's gentity whose value differs from `STAT_HEALTH` right after a game frame is
  struck off; after 3 different values the smallest offset left is used (baseq3 1.32 QVM: +732 of
  808). Only checked right after a game frame (client packets change health between frames).
  Self-damage restores armor without it; instagib's 100-health cap and self-damage's health need it
  (found within ~2 s of the first spawn: spawn health 125 counts down).
- **Self-damage off:** health / armor that dropped in a game frame while `PERS_ATTACKER` == the
  player is put back, except the 1-point-per-second count-down above the maximum. A self-hit that
  kills still kills (the game decides death inside `G_Damage`).
- **Hooks:** `SV_InitGameVM` (`sv.entityParsePoint = SV_SplitEntityString( restart )`, also for
  `map_restart`, so Random re-rolls), botlib's `BotImport_BSPEntityData` gets the same string,
  `SV_SplitRulesFrame()` after the `GAME_RUN_FRAME` loop in `SV_Frame`. Inert when
  `com_dedicated` or the client-set `sv_splitRules` is absent (verified with the cvars forced on
  the dedicated exe). `srvdebug ps|ents|health` (server command) prints the state.
- **State:** the page edits native cvars directly (`timelimit fraglimit capturelimit
  g_friendlyFire g_gravity s_volume`), so a mod's own start-server menu is never overridden; our
  settings are archived `cl_splitSrv*` (`Instagib Weapons SpawnWeapons InfiniteAmmo Bots BotSkill
  SelfDamage God Health Set`), copied each frame into `sv_split*` (Instagib / Weapons only while the
  page is closed); `cl_splitSrvMap` / `cl_splitSrvGametype` are the pending map / type (temp).
  Sets: `<homepath>/<game>/serversettings/<key>.cfg` (per game: UrT's game types differ), lines
  `cvar "value"`, unknown lines skipped; volume is not part of a set.
- **Apply on leaving:** one `map`/`devmap` (map or type changed, or bots asked in a game whose
  `bot_enable` is 0: it is latched to 1 and applies with that load) or one `map_restart 0`
  (instagib / weapons changed, or the bots need a bigger `sv_maxclients`), else nothing; `devmap`
  when cheats were already on and not forced by god mode. Queued with `EXEC_INSERT`.
- **Bots:** Off = the engine does not manage bots (a mod menu's bots stay); 1-16 keeps the total
  bot count (one `addbot <name> <skill>` at a time, names from `scripts/bots.txt` + `*.bot`; the
  extras kicked), difficulty change / Off = `kickbots`.
- **Rows:** capture limit 0-20 (CTF types: baseq3 4, missionpack 5, UrT 7); game types: baseq3
  four stock ones (a current SP / missionpack type is shown by name), UrT its eleven. Volume range
  0-150 %: `s_volume` max 1.5; the mixer clamps a channel's volume x master at 65535 so
  `sample * volume` stays inside an int (merged loop sounds at 255 stop at the old 1.0 level).

### 18.5 As built (R16, the maintainer's feedback on R14b-R15)

- **New rows** after Low gravity (18.1): Quad damage, Player speed, Weapon respawn. Archived
  `cl_splitSrvQuad` (held back while the page is open like Instagib / Weapons, copied to
  `sv_splitQuad`; the rewrite drops `item_quad`), `cl_splitSrvSpeed`, `cl_splitSrvWeaponRespawn`
  (written into the game's cvars by `CL_SplitSrvFrame` in local games only). All three are in sets.
- **Reset to defaults** resets every row incl. game type (0, Free for all in both games; it loads
  with the map when the page is left) and `s_volume 1`; the map stays.
- **The game's own instagib.** Stock baseq3 registers no `*insta*` cvar (the hook logs none in a
  baseq3 game, `work/r16-page.log`; ioquake3's `g_main.c` has none either; missionpack not run). Mods do: **Urban Terror 4.3
  `g_instagib`** (its readme: "affects all gametypes with the cvar g_instagib 1|0"), **OSP 1.03a
  `match_instagib`** (also `instagib_reload`, `vote_allow_instagib`; the game prints "Instagib
  ENABLED!" / "DISABLED!" at init). Every cvar the game VM registers passes `SV_SplitGameCvar`
  (`G_CVAR_REGISTER` hook); preferred names `g_instagib`, `instagib`, `match_instagib`, `g_insta`,
  `sv_instagib`, then the shortest other `*insta*` name that is not a `vote_` / `match_` / menu /
  `reload` / `allow` / `instant` cvar. With one, the Instagib row edits the temp
  `cl_splitSrvModInstagib` (opened from the game's cvar); leaving sets the game's cvar and does one
  `map_restart` (enough for both: UrT's `g_instagib` and OSP's `match_instagib` are read at
  `G_InitGame`); our preset and the weapon levers are never sent while the game has its own (the
  server also refuses the preset). At a game's first load the preset (archived, chosen before the
  game was known) gives way: `G_InitGame` registers its cvars before it parses the entities, so the
  entity string is rebuilt the moment the instagib cvar registers.
- **Instagib greys** Weapons, Player weapons at spawn, Infinite ammo and Player health (no handicap
  is sent while it is on: `CL_SplitSrvInstagibOn`).
- **Bots 0-20**; the page compares the bots' need with the running server's slots
  (`SV_SplitMaxClients`), not the `sv_maxclients` cvar, which the server raises (latched) while
  the page is open.
- **R17: Power-ups row** above Quad damage (18.1): archived `cl_splitSrvPowerups` (default 1), held
  back while the page is open and copied to `sv_splitPowerups`; in sets and Reset. The rewrite
  (`SR_IsPowerup`, checked before the instagib / weapons rules) drops the classes in 18.1; summary
  `default weapons, no power-ups: 552 entities, 9 weapon pickups, 22 ammo, 3 removed` on q3dm7
  (2 x `item_quad` + `holdable_teleporter`); `srvdebug ents` now lists `holdable_*` too and
  `srvdebug ps` prints `no-powerups`. Evidence `work/r17-powerups.log`, `r17-powerups-quadgrey.jpg`.
- **FOV (checked, no change):** `joy_fov` 90 = the `cg_fov` default both baseq3 and UrT 4.3 register;
  The maintainer's basepath `autoexec.cfg` `cg_fov 115` is in effect before any pad, and P1 / P2 on pads see
  with 90 (`work/r16-fov.log`, `work/r11-vk-r16-urtfov.log`).

---

## 19. R17: The maintainer's second real-pad pass (0.0.0.21) — as built

- **Aim assist** (15.1 R17): Low 0.64 slow / 0.375 follow (a quarter of the way to Standard,
  which is unchanged); `joy_aimAssist` default 0, so Guests and new profiles start **Off**; the host
  switch `cl_aimAssistAllow` stays 1. Saved profiles and a saved Guest defaults file keep theirs.
- **Model page LB / RB for player 1** (`cl_splitui.c`): R14a clicked the page arrows open-loop
  (corner, then one move of `menu units x height/480`). The maintainer's a51 ui (`zzz-a51-ui.pk3`) does not
  move its cursor by that scale: measured (`work/r17-mt-*.log`) a +300 move goes 300 / 338 / 450
  screen pixels at 720 / 1080 / 1440 lines (q3_ui's own scale 1.5 / 2.25 / 3), and big moves are
  non-linear -- so at the maintainer's 2160 lines the click ran into the bottom right corner (at 1440:
  cursor drawn at 1413,1392 for the arrow at 1032,1020). Players 2-8 have 640x480 menus where
  the deltas happen to be 1:1. Now the menu's own cursor draw (`menu/art/3_cursor2`, noted in
  `CL_SplitUINotePic`) is the feedback: corner, then steps of `error x learnt gain` once per frame
  (`CL_SplitUITurnStep`, at most 8 steps / 1.5 s; a press while turning queues one more), click
  when within 4 menu units. 2-3 frames per turn. Developer lines: `P<n> model page: cursor at
  x,y (menu units ...) step k`, `P<n> model page: next page`, and `first portrait <icon>` whenever
  the page's first portrait changes (= the page turned).
- **Strings:** HUD shape "4:3 Centered", button "Center view" (US spelling, also in cvar
  descriptions and this document); every percent value prints without a space ("95%").
- **Menu size** (`cl_splitMenuSize`): default 1, range 0.5 .. 1.5, step 0.25. The scale keeps its
  R14a meaning (resolution-proportional base x the value): the maintainer's own config already has it at
  1 (he turned R14a's 1.5 default down), so 1.00x is the look he picked; 1.5 is R14a's default.
- **Power-ups row** (18.1 / 18.5).
- **Cursor speed** (Controls, `joy_cursorSpeed`, a feel setting: per player, saved in profiles,
  Guest defaults): 50 .. 200 % by 10, default 100 %; scales the overlay's dot and the game menu's
  crosshair (`IN_PadCursorSpeed`: `cl_padCursorSpeed x joy_cursorSpeed`, game menus also x
  `cl_padModCursorScale`, now **1.4**, was 1.25). There is no cursor style choice anywhere (the
  stock menus always draw their crosshair, our menus their dot), so no "(default)" labels were
  added. `splitcursor <player>` prints both speeds and cursor positions.
- **Spectators** (`cl_splitmenu.c`): in a team game with `g_teamAutoJoin 0` (Team Arena's and
  baseq3's default) everyone starts as a spectator. The game's own way in is its menu (Team
  Arena: a cursor-driven button bar, "Join" -> team popup; reached with a pad only through Game
  menu + the right-stick cursor + RT), which a pad player does not find. The pause page now shows,
  to a player whose `ps.persistant[PERS_TEAM]` (3) is TEAM_SPECTATOR (a spectator following
  someone: its own configstring "t"), **Join red / Join blue / Auto join (smaller team)** in team
  games (`g_gametype` >= 3 from the server info) or **Join game** (`team free`) otherwise, right
  under Resume; a player on a team in a team game gets **Spectate** above Leave. Each sends
  `team <red|blue|free|spectator>` on that player's own connection; Auto counts red / blue in the
  player configstrings (key `t`, excluding the player) and picks the smaller (red on a tie)
  rather than relying on a mod's handling of unknown team names. The game's own rules apply (one
  change per 5 s, force balance). Not in Urban Terror (its own team menu opens by itself). A line
  `P<n> team: <team> (PERS_TEAM n, g_gametype g)` is logged at every change.

## 20. R18: Urban Terror pass (0.0.0.23) — as built

User-facing summary: `docs/URBAN-TERROR.md`.

- **Install auto-detect** (`files.c` `FS_UrTDetect`, before `FS_Startup`): no `fs_basegame` and no
  `fs_game` on the command line, and the base path (`fs_basepath`, default the exe's folder) holds a
  `q3ut4` folder but no `baseq3` -> `fs_basegame q3ut4`. `Com_Init` logs `urt: detected q3ut4
  install, fs_basegame q3ut4` once the log is open (`Com_UrTHunkDefault`). A Quake 3 folder with
  `q3ut4` inside is not touched: there UrT is in the Mods list or `+set fs_game q3ut4`.
- **Hunk:** whenever the game is UrT at startup (`FS_UrTGame`: `fs_basegame` or `fs_game` q3ut4),
  `com_hunkMegs` below 1024 is raised to 1024 (log `urt: com_hunkMegs <old> -> 1024`) unless the
  command line sets it. Never lowered. A switch to UrT through the Mods list keeps the running hunk
  (the hunk is allocated once per process).
- **Online "Non whitelist client" kick** (`cl_main.c` `CL_CheckForResend`): not the engine and not
  UrT's QVM. UrT servers that run the B3 / B4 admin bot's `vpncheck` plugin read the `client` key
  of the connect userinfo (B4 `parsers/iourt43.py`: `bclient['app'] = bclient['client'][0:32]`) and
  tempban any value not in the plugin's `whitelist_clients` list (exact match, per server; skipped
  when the key is absent): `extplugins/vpncheck/__init__.py` `checkClient()`, `reason = 'Non
  whitelist client ' + ...` (dkman123/B4 master 9cf0e7eb). Quake3e adds `client "Q3 1.32e ..."`
  (`Q3_VERSION`) to every connect; UrT 4.3's own client (FrozenSand ioq3-for-UrbanTerror-4
  `CL_CheckForResend`) sends only the userinfo cvars + `protocol` / `qport` / `challenge`, no
  `client`, and its `version` cvar ("ioQ3 1.35 urt 4.3.4 <platform> <date>") is serverinfo, never
  part of the connect. So in UrT our client leaves the `client` key out -- exactly what UrT's client
  presents -- and logs `urt: connect userinfo to <addr>: no "client" key (as UrT 4.3's client),
  version "<ours>"`. Our `version` cvar, log header and BUILD-INFO keep our real version. Side
  effect on a Quake3e server (our own listen server): without the key the server assumes a legacy
  client (`SV_DirectConnect` `longstr = qfalse`), which every ioq3 UrT client is anyway.
- **Scope / full-screen overlays** (`cl_splitscreen.c` `CL_SplitDrawStretchPic`): with HUD shape 4:3
  Centered (`cl_splitAspect 1`) each cgame draws a 640x480 screen scaled into the 4:3 area of its
  cell; the world fills the whole cell. UrT draws its scope as one full-screen pic (black around the
  lens), so a wide cell showed a 4:3 "square" of scope with the zoomed world beside it. Now a pic
  that spans the told screen's full height and touches its left / right edge is continued over the
  bar on that side with its own edge column of texels (2 thousandths of the texture span); for a
  tall cell the same vertically for full-width pics touching the top / bottom. Scope, flash-bang
  white, fades: as a real screen of the cell's shape would show them. `cl_splitOverlayBars 0`
  (temp) turns it off (the before/after switch of `r18-zoom.cfg`). Stretched mode
  (`cl_splitAspect 0`): the cgame is told the cell and UrT stretches its scope itself (an ellipse
  in a 32:9 cell; unchanged). baseq3's zoom draws no overlay (nothing changes).
- **Menu colour:** `cl_splitMenuColor` default `auto` = Quake 3 red, Urban Terror blue (0.16 0.50
  1.0; title bar half that, selected row 30 %); any "r g b" value still overrides.
- **UrT pad layout** (`in_gamepad.c` `padGameDefaults` "q3ut4"; the maintainer's, commands from UrT's
  `ui/controls.menu` / `default.cfg`):

  | Button | Command | Action |
  |---|---|---|
  | A | `+moveup` | Jump |
  | B | `+movedown` | Crouch (**toggle** by default) |
  | X | `+button5` | Reload |
  | Y | `+button6` | Bandage |
  | D-pad up | `ut_itemdrop` | Drop item |
  | D-pad down | `ut_weapdrop` | Drop weapon |
  | D-pad left | `ut_itemuse nvg` | IR vision (night vision goggles) |
  | D-pad right | `+button3` | Weapon (fire) mode |
  | R3 | `ut_weaptoggle knife` | Knife |
  | L3 | `+button8` | Sprint (**toggle** by default) |
  | LB | `ut_zoomreset` | Reset zoom |
  | RB | `weapnext` | Next weapon |
  | LT | `ut_zoomin` | Zoom in (steps; UrT has no hold-to-zoom) |
  | RT | `+attack` | Fire |
  | Start | `padmenu` | Pause menu |
  | Back | `+scores` | Scores |
  | Misc (Share) | `ut_itemuse` | Use current item (kept from R11) |
  | Sticks | | Move / look |

  The R11 layout's Interact (`+button7`, was Y) and the d-pad radio calls are no longer bound;
  they are on the Buttons page. A profile / Guest defaults file whose bind table is exactly the R11
  built-in layout (never edited) loads as the new layout (`IN_PadUpgradeBinds`, table
  `padOldDefaults`; log `profile: q3ut4/profiles/<key>.cfg had the R11 built-in q3ut4 pad layout;
  now the current one`). Edited tables are kept as they are.
- **Hold or toggle, per game** (`joy_crouchToggle`, new `joy_sprintToggle`): they belong to the
  layout, so they moved out of the shared feel settings into the bind table: factory data lines
  `toggle crouch 1` / `toggle sprint 1` (q3ut4; absent = hold, baseq3), profile / Guest defaults
  per-game files `toggle crouch "1"` lines after the binds, players' `p<N>_` / `guest_` shadows
  edited by the Controls rows **Crouch** and **Sprint** (Sprint only in UrT). A per-game file
  without toggle lines (all pre-R18 files) gets the game's defaults; the old shared-file line
  `joy_crouchToggle` is accepted and ignored. "Reset controls" resets them to the Guest defaults
  (Guest defaults: to the factory data), "Reset buttons" too. Sprint (`+button8`) latches like
  crouch; a latched sprint also ends when the left stick rests (inside the inner deadzone) for
  0.3 s, as console games do. `in_padDebug 1` logs `pad: P<n> crouch|sprint toggle on|off`.
- **Buttons page in UrT** lists every UrT action in a fixed order (the maintainer's layout first, then zoom
  out, previous weapon, weapon slots, interact, items, walk, minimap, team / gear / radio menus,
  six radio calls, chat, team chat, vote yes / no, center view), unbound ones as `--`; any other
  command a player bound follows. baseq3 keeps its R7 list.

---

## 21. R20: The maintainer's third real-pad pass (0.0.0.25) — as built (0.0.0.26)

- **Independent backdrop removed** (17.5): no backdrop window, no `cl_splitIndepBackdrop`, no
  restack or notification-state logging. Tiles keep `MarkFullscreenWindow` (R16). Joyxoff: disable
  its bindings for this mode.
- **Independent tiles render like the Together cell of the same size** (17.3 correction). Cause:
  a tile window holds one player, so the viewport layer (3.1: told size, FOV widening, 2D placement)
  was pass-through: the cgame was told the tile's own size (e.g. 2560x720), baseq3 derived a
  31-degree vertical FOV from `cg_fov` 90 across a 3.6:1 screen and stretched its 640x480 HUD over
  it (wide numbers, squashed look). Together tells a cell's cgame 640x480 (`cl_splitAspect` 1,
  "HUD shape 4:3 Centered"), keeps the 4:3 vertical FOV and widens the horizontal one.
  `CL_IndepTiled()` (cl_splitindep.c) is true in a child window always and in player 1's window
  once its applied rect is one of several tiles; `CL_SplitCells()` (cl_splitscreen.c) then treats
  the whole window as a cell for `CL_SplitToldSize` / `CL_SplitGlconfig` / `CL_SplitRenderScene` /
  `CL_SplitDrawStretchPic` (both HUD shapes, as in Together). A single Independent window that
  covers the area stays pass-through, like one Together player. Log, once per cgame start:
  `window: tile WxH is a cell: cgame screen 640x480 (HUD shape 4:3 centered)`. Menus (the UI VM)
  were already 4:3-centered by q3_ui's own widescreen bias; unchanged.
- **First-launch hint** (14.1 family): at the Together main menu with a pad connected,
  "Start a game first, then friends hold A to join" (the verb and button follow
  `cl_splitJoinHold` / `cl_splitJoinButton` and the pad type) in the join hint's style and corner,
  until the first game reaches CA_ACTIVE, which sets `cl_splitSeenHint 1` (archived in
  `q3config-ss.cfg`; log `pad: first game started, first-launch hint done`). Never again after
  that; `cl_splitSeenHint 0` shows it again. Not in Independent mode (pads join from its main menu)
  nor in a child window; `cl_splitJoinHint 0` hides it too.
- **Single Player arenas** take extra local players. Cause: `SV_GetChallenge` ignores every
  `getchallenge` while `g_gametype` is GT_SINGLE_PLAYER or `ui_singlePlayerActive` is set
  (upstream: nobody joins a solo game), so P2 sent getchallenge forever ("Player 2
  connecting..."). The game VM has no SP gate for humans. Now a listen server answers
  127.x.x.x (this machine's own local players, the same rule as the per-IP cap) in SP too; others
  are still ignored. `spmap` sets the latched `sv_maxclients` to 8 + (`cl_splitMaxPlayers` - 1)
  (15 by default) instead of 8 (dedicated: 8), so the arena's bots and every local player fit.
- **Urban Terror downloads** (20 / URBAN-TERROR.md): UrT's `default.cfg` sets `seta
  cl_allowdownload 0` (its own client uses `cl_autodownload` instead), so a server map we lack
  ended in `CM_LoadMap: couldn't load maps/<map>.bsp`. In q3ut4 `cl_allowDownload` is turned on
  once (`Com_UrTDownloadDefault`, recorded in `cl_urtDownloadDefault 1`; a later user 0 is kept;
  the command line wins). libcurl was already linked statically in the Windows build (`USE_CURL`,
  `CURL_STATICLIB`, as upstream's release); the client logs at start `download: cl_allowDownload
  1; HTTP/FTP (server sv_dlURL, dlmap) via libcurl/8.4.0 Schannel (built in)`. Downloads run on
  player 1's connection before any extra player joins (extras use the same files), so the stock
  full-window download screen is what every viewport shows. The auth notice ("This game client
  is not auth capable") is UrT's account-login line; only servers that require an account refuse
  us.
  **R21 (0.0.0.27) replaced this:** the one-time switch did not survive a join. UrT servers send
  systeminfo `fs_game q3ut4` (the UrT engine's default fs_game); our `""` + basegame q3ut4 differed,
  so every join ran `Com_GameRestart` (Cvar_Restart, `default.cfg` again -> `cl_allowdownload 0`,
  `cl_urtDownloadDefault` already 1) and `FS_Shutdown( qtrue )` closed the log's handle
  (common.c kept the dead handle: the rest of the session was not logged). Now:
  `CL_SystemInfoChanged` treats a base-game name as `""` (`FS_IsBaseGameName`: no restart);
  `Com_GameRestart` closes the log and the next print reopens it in append mode, `logfile` is
  `CVAR_NORESTART`; downloads in q3ut4 follow UrT 4.3's client (FrozenSand ioq3-for-UrbanTerror-4
  `CL_FirstDownload` / `CL_NextDownload` / `cl_curl.c`): switch `cl_autodownload`
  (`CL_DownloadsEnabled`), only `<mapname>.pk3` (`CL_UrTMapPakOnly`), HTTP `<sv_dlURL>/<remote
  name>` regardless of the server's `sv_allowDownload` (UrT's qagame forces it to 0 at InitGame),
  UDP only if the server allows it, else `ERR_DROP` naming the map; saved to
  `q3ut4/download/<name>.pk3` (homepath). `FS_AddUrTDownloadDir` adds `<path>/q3ut4/download`
  below each `<path>/q3ut4` (as UrT's files.c); `.qvm`/`.menu`/`.cfg` are never read from it
  (`FS_DownloadDirBlocks`) and its paks are listed as referenced only when used. Every decision
  is logged (`download: server sv_allowDownload=<n> sv_dlURL=<url>; missing: <paks>; method:
  http|udp|none (<reason>)`). baseq3 download behaviour is upstream's. Test: `r21-dl.sh`.
- **Pads skip cinematics**: while `cls.state == CA_CINEMATIC` any button or trigger of any pad
  (sticks excluded) does what a key press does there (`CL_KeyEvent( K_ESCAPE )` when no catcher is
  up and `com_cameraMode` is 0: stop, main menu); the press and its release are consumed (no
  join, no menu key, no bind, a held join button does not complete a hold-to-join). Log `pad N:
  PAD_X skips the cinematic`. Quake 3 and UrT play the same `idlogo.roq` path.
- Tests: `r20-aspect.cfg` + `r20-achild2.cfg` (tiles), `r20-aspect-tog.cfg` (Together reference),
  `r20-sp.cfg`, `r20-cin.cfg`, `r20-urt.cfg`.
