/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_splitscreen.h -- per-local-player client contexts
//
// Every local player is a complete virtual client: its own clientActive_t,
// clientConnection_t, cgame VM, per-connection slice of cls and input
// button state.  The historical globals 'cl', 'clc' and 'cgvm' are macros
// that resolve through the active context pointer 'cla', so all existing
// code works unchanged.  Context 0 is static and always in use; with only
// context 0 the engine behaves exactly like upstream.
//
// Included from client.h only (after clientActive_t / clientConnection_t /
// clientStatic_t are defined).  Never include from server code.

#ifndef CL_SPLITSCREEN_H
#define CL_SPLITSCREEN_H

#define MAX_SPLITVIEW	8

// continuous button state (+forward, +attack, ...), see cl_input.c
typedef struct {
	int			down[2];		// key nums holding it down
	unsigned	downtime;		// msec timestamp
	unsigned	msec;			// msec down this frame if both a down and up happened
	qboolean	active;			// current state
	qboolean	wasPressed;		// set when down, not cleared when up
} kbutton_t;

// per-player input state formerly held in cl_input.c statics
typedef struct {
	kbutton_t	left, right, forward, back;
	kbutton_t	lookup, lookdown, moveleft, moveright;
	kbutton_t	strafe, speed;
	kbutton_t	up, down;
	kbutton_t	buttons[16];
	qboolean	mlooking;
} clInputState_t;

// the per-connection slice of clientStatic_t; swapped in/out of 'cls'
// by CL_SetContext so the ~150 'cls.state' users stay untouched
typedef struct {
	connstate_t	state;
	qboolean	gameSwitch;
	qboolean	cgameStarted;
	qboolean	startCgame;
} clsShadow_t;

// a viewport cell in real framebuffer pixels
typedef struct {
	int		x, y, w, h;
} viewRect_t;

typedef struct clientContext_s {
	clientActive_t		clActive;	// accessed as 'cl'
	clientConnection_t	clConn;		// accessed as 'clc'
	vm_t				*cgameVM;	// accessed as 'cgvm'
	clsShadow_t			clsShadow;	// valid only while NOT the active context
	clInputState_t		in;
	int					playerNum;	// 0-based local player index == clx[] slot
	qboolean			inUse;		// qfalse once dropped; freed at the next safe point
	netsrc_t			sock;		// NS_CLIENT for player 0, NS_CLIENT2 + n - 1 for player n
	int					qport;		// netchan qport of players > 0 (player 0 uses net_qport)

	// players > 0 only (a player > 0 occupies a viewport cell from addplayer on)
	qboolean			cgamePending;	// start the cgame once player 1 is in game
	qboolean			dropRequested;	// drop at the next CL_SplitscreenFrame
	char				dropReason[MAX_STRING_CHARS];
	int					joinSeq;		// join order (cl_splitWidePlayer 0 = last joined)
	int					joinTime;		// cls.realtime of addplayer (connect timeout)
	qboolean			joinHeld;		// slot taken, not connecting yet (join-time profile picker)
	qboolean			connectQueued;	// waits for an earlier extra player's connect (one at a time)
	int					turnTime;		// cls.realtime its connect turn began (bounded wait for the ones after it)
	qboolean			turnRefresh;	// restart turnTime next frame (its cgame just loaded)
	int					loadSince;		// cls.realtime its current loading wait began (0 = not loading)
	int					slotTime;		// cls.realtime the slot was taken (pad hold done / addplayer)
	int					connectStart;	// cls.realtime it started connecting; 0 once reported in game
	int					cgameCatcher;	// this player's KEYCATCH_CGAME bit
	char				cgameCmds[MAX_STRING_CHARS * 2];	// console commands its cgame queued ('\n'-separated)
	int					cgameCmdsLen;	// run at its next frame (CL_SplitRunCgameCmds)
	int					userinfoCheckTime;
	char				lastUserinfo[MAX_INFO_STRING];	// last userinfo sent
	qboolean			debugFreeze;	// splitdebug: ignore this player's packets
	int					debugError;		// splitdebug: 1 = fail in its cgame, 2 = in its packet

	int					cgameWidth;		// screen size the running cgame was told
	int					cgameHeight;	// (0 = it never asked for the glconfig)

	// players > 0 only: this player's own menu (a ui VM instance, design 13.3)
	vm_t				*uiVM;			// created on the first open, kept until the level ends
	int					uiCatcher;		// KEYCATCH_UI while its menu is open
	int					uiWidth;		// screen size its ui was told
	int					uiHeight;
	qboolean			uiFailed;		// its ui raised an error: no menu until the next level
	byte				uiKeyDown[MAX_KEYS];	// menu keys its pad holds (UI_KEY_ISDOWN)

	// every player (P1 too): cls.realtime of its ui's last server-browser refresh trap
	// (LAN ping / server count): the stock browser is refreshing (CL_SplitUIBrowserRefreshing)
	int					uiLanTime;
	int					uiModelPageTime;	// ... its ui last drew the stock player model page
	int					uiModelCheck;		// cls.realtime to check that page came up (0 = none)
} clientContext_t;

extern clientContext_t	*clx[MAX_SPLITVIEW];
extern clientContext_t	*cla;		// active context, never NULL

#define cl		(cla->clActive)
#define clc		(cla->clConn)
#define cgvm	(cla->cgameVM)

// nonzero while a cgame restarts on an already loaded level (layout change,
// extra player joining): world/collision map loads are skipped
extern int		cl_splitInPlaceInit;

// nonzero when more than one local player exists (or a cgame restarts in
// place): the cgame trap layer is consulted; zero = upstream behaviour
extern int		cl_splitTraps;

void	CL_InitSplitscreen( void );
void	CL_ShutdownSplitscreen( void );
void	CL_SetContext( int ctxNum );
void	CL_PushContext( int ctxNum );
void	CL_PopContext( void );
void	CL_SplitscreenFrame( void );
void	CL_SplitscreenDisconnect( void );
void	CL_SplitscreenGamestate( void );
int		CL_ContextQport( void );
const char *CL_SplitUserinfo( qboolean *truncated );
qboolean CL_SplitSkipUserinfo( const char *info );	// player 1's update is unchanged

// lifecycle hooks
void	CL_SplitShutdownCGames( void );
void	CL_SplitCGameShutdown( void );
void	CL_SplitMapLoading( void );
void	CL_SplitRequestDrop( int ctxNum, const char *reason );
qboolean CL_SplitGameCommand( void );
void	CL_SplitServerPrint( const char *message );	// OOB print from the server to an extra player
qboolean CL_SplitRemoteServer( void );		// player 1 is on another machine's server
int		CL_SplitPlayersInGame( void );
qboolean CL_SplitRefusalShowing( void );	// a remote server's "could not join" note is up

// viewports: layout, cgame trap layer, screen loop
void	CL_SplitViewRect( int ctxNum, viewRect_t *rect );
void	CL_SplitGlconfig( glconfig_t *glconfig );
void	CL_SplitRenderScene( const refdef_t *fd );
void	CL_SplitDrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );
void	CL_SplitCGameRendering( stereoFrame_t stereo );
qboolean CL_SplitSyscall( intptr_t *args, intptr_t *ret );

// shared with cl_splitui.c
int		CL_SplitNumViews( void );
void	CL_SplitToldSize( int ctxNum, int *w, int *h );
qboolean CL_SplitShadowed( const char *name, int extraFlags );
cvar_t	*CL_SplitShadow( int n, const char *name, const char *defaultValue );
const char *CL_SplitBuildUserinfo( int n, qboolean *truncated );
qboolean CL_SplitRunUI( int n, void (*func)( void ) );	// guarded: an error closes only n's menu
int		CL_SplitKeyCatcher( void );	// key catchers as the active player sees them

// cl_splitui.c: per-player menus (design 13.2 / 13.3)
qboolean CL_SplitUISyscall( intptr_t *args, intptr_t *ret );
qboolean CL_SplitUIInCell( void );		// player 1's menu is drawn into its cell
qboolean CL_SplitUIMenuOpen( int n );	// player n has a menu open
qboolean CL_SplitUIBrowserRefreshing( int n );	// its stock server browser is refreshing (R14a: X/Y = SPACE)
qhandle_t CL_SplitUINoteShader( const char *name, qhandle_t h );	// (cl_ui.c) spots the model page's art
void	CL_SplitUINotePic( qhandle_t h, float x, float y );			// (cl_ui.c) the active player's ui drew h
qboolean CL_SplitUICursor( int n, float *x, float *y );	// R17: where its stock menu last drew its cursor
qboolean CL_SplitUIModelPage( int n );			// its menu shows the stock player model page
qboolean CL_SplitUIOpenModelPage( int n, qboolean *exact );	// game menu -> setup -> player -> model
void	CL_SplitUIModelCheck( void );				// once per frame: did that page come up
void	CL_SplitUIModelTurn( int n, int dir );		// LB / RB there: previous / next page
void	CL_SplitUIOpen( int n, int menu );	// UIMENU_INGAME ...
void	CL_SplitUIClose( int n );
qboolean CL_SplitUIGameCommand( void );	// a console command for the active player > 1's menu
void	CL_SplitUIKeyEvent( int n, int key, qboolean down );
void	CL_SplitUIMouseEvent( int n, int dx, int dy );
void	CL_SplitUIRefresh( int n );
void	CL_SplitUIFree( int n );			// player n leaves
void	CL_SplitUIError( int n, const char *message );	// from CL_SplitCatchError
void	CL_SplitShutdownUIs( void );		// with player 1's ui (level change, vid_restart)
void	CL_SplitUIRestoreP1( void );		// 'uivm' back to player 1 after an uncaught error
void	CL_SplitUIInfo( int n, char *buf, int size );	// splitplayers
vm_t	*CL_SplitCreateUIVM( vmIndex_t index );	// cl_ui.c

// players for the pad layer
int		CL_SplitAddPlayer( int slot );		// -1 = lowest free; returns the slot or -1
int		CL_SplitAddPlayerEx( int slot, qboolean hold );	// hold: take the slot, connect later
qboolean CL_SplitConnectPlayer( int slot );
qboolean CL_SplitSlotJoining( int slot );	// held for the profile picker
int		CL_SplitFreeSlot( void );
qboolean CL_SplitSlotActive( int slot );	// in use and not leaving
void	CL_SplitReleaseInput( int slot );	// release every held button of an idle player
qboolean CL_SplitCgameCatches( int slot );	// its cgame set KEYCATCH_CGAME
void	CL_SplitCgameKey( int slot, int key, qboolean down );	// CG_KEY_EVENT to it
qboolean CL_SplitEmptyCell( viewRect_t *rect );

// in_gamepad.c: SDL2 gamepads, per-player pad binds, join/leave by pad
void	IN_GamepadInit( void );
void	IN_GamepadShutdown( void );
void	IN_GamepadFrame( void );			// once per frame, from the platform input frame
void	IN_GamepadDrawHint( void );
void	IN_GamepadDrawFirstHint( void );	// R20: main menu, first launch: "Start a game first, then friends press A to join"
void	CL_GamepadMove( usercmd_t *cmd );	// CL_CreateCmd, active context
// ... for the overlay: pad keys, per-player binds and look/aim settings
int		IN_PadNumKeys( void );
qboolean IN_PadBindableKey( int key );
const char *IN_PadKeyLabel( int n, int key );
const char *IN_PadButtonName( int n, const char *padKey );	// "PAD_A" -> "A" / "Cross" ...
const char *IN_PadGetBind( int n, int key );
const char *IN_PadDefaultBind( int key );
void	IN_PadSetBind( int n, int key, const char *cmd );
void	IN_PadResetBinds( int n );
cvar_t	*IN_PadFeelCvar( int n, const char *name );	// p<N>_joy_* shadow, NULL if not a feel cvar
void	IN_PadFeelChanged( int n );
void	IN_PadResetFeel( int n );
void	IN_PadShortcuts( char *join, int joinSize, char *leave, int leaveSize );
// ... for profiles: n = a player slot or SPLIT_GUEST_DEFAULTS (the set every Guest starts from)
#define SPLIT_GUEST_DEFAULTS	MAX_SPLITVIEW
int		IN_PadNumFeel( void );
const char *IN_PadFeelName( int i );
float	IN_PadCursorSpeed( int n, qboolean gameMenu );	// R17: right-stick cursor speed (menu units/s)
qboolean IN_PadFeelSet( int n, const char *name, const char *value );	// range-checked
void	IN_PadCopyFeel( int dst, int src );		// src -1 = the shared joy_* cvars
void	IN_PadCopyBinds( int dst, int src );	// src -1 = default_pad.cfg / built-in
void	IN_PadClearBinds( int n );
qboolean IN_PadSetBindByName( int n, const char *padKey, const char *cmd );
qboolean IN_PadSetToggleByName( int n, const char *word, const char *value );	// R18: 'toggle crouch "1"' profile lines
const char *IN_PadUpgradeBinds( int n );		// R18: an untouched earlier built-in layout -> the current one
void	IN_PadWriteBinds( int n, fileHandle_t f );
int		IN_PadKeyNum( const char *padKey );		// "PAD_X" -> key number, -1
const char *IN_PadGuid( int n );				// player n's pad, NULL = none
void	IN_PadReleasePlayer( int n );
qboolean IN_PadMigrateLegacy( const char *splitpadsText );
qboolean IN_PadPlayerCvar( const char *name );	// a per-player non-userinfo cvar of this game ('playercvar')

// cl_splitprofile.c: profiles, Guest, Guest defaults (design 12.3 / 13.1 / 14.3)
#define PROFILE_NAME_LEN	20
void	CL_ProfileInit( void );
void	CL_ProfileShutdown( void );
void	CL_ProfileFrame( void );
qboolean CL_ProfileLoad( int n, const char *name, qboolean joining );	// name NULL / "guest" = Guest
void	CL_ProfileChanged( int n );		// feel / binds of n (or SPLIT_GUEST_DEFAULTS) changed
void	CL_ProfileP1Pad( const char *guid );	// a pad became player 1
const char *CL_ProfileDescribe( int n );	// "Guest", "\"Sarge\"", "none (q3config)"
qboolean CL_ProfileActive( int n );		// n plays as a profile or a guest
qboolean CL_ProfileIsGuest( int n );
const char *CL_ProfileName( int n );		// display name ("Guest" for guests)
void	CL_ProfileRefresh( void );			// re-read the profiles directory
int		CL_ProfileCount( void );
const char *CL_ProfileListName( int i );
int		CL_ProfileListUser( int i );		// slot playing it, -1 = nobody
int		CL_ProfileFind( const char *name );	// list index, -1
int		CL_ProfilePadLast( const char *guid );	// list index of the pad's last profile, -1
qboolean CL_ProfileSaveAs( int n, const char *name, char *err, int errSize );	// new profile from n's settings, n uses it
qboolean CL_ProfileRename( int n, const char *name, char *err, int errSize );
qboolean CL_ProfileDelete( int i, int bySlot, char *err, int errSize );	// list index
qboolean CL_ProfileSanitize( const char *in, char *out, int size );

// cl_splitmenu.c: engine-drawn per-player overlay (design 12.2 / 13.4 / 14.4)
void	CL_SplitMenuInit( void );
void	CL_SplitMenuShutdown( void );
qboolean CL_SplitMenuOpen( int n );
void	CL_SplitMenuPause( int n );			// Start in game: the pause page
void	CL_SplitMenuSettings( int n );		// host splitscreen page (player 1)
void	CL_SplitMenuClose( int n );
void	CL_SplitMenuKey( int n, int menuKey );	// K_UPARROW ... K_ENTER, K_ESCAPE, K_MOUSE1/2 presses
void	CL_SplitMenuMouse( int n, int dx, int dy );	// right-stick cursor, 640-unit menu space
qboolean CL_SplitMenuCapturing( int n );		// a "press a button" prompt is up
void	CL_SplitMenuCapture( int n, int padKey );
void	CL_SplitMenuFrame( void );			// housekeeping, once per frame
void	CL_SplitMenuDraw( void );			// every open overlay, after the menus (cl_scrn.c)
void	CL_SplitMenuJoin( int n, const char *guid );	// join-time profile picker in a held slot
qboolean CL_SplitMenuPadKey( int n, int padKey );	// raw pad key for the on-screen keyboard
void	CL_SplitMenuKeyboard( int n );		// keyboard typing into n's mod menu

// cl_splitsrv.c: player 1's Server options page state (design 18; levers in server/sv_splitrules.c)
void	CL_SplitSrvInit( void );
void	CL_SplitSrvShutdown( void );
void	CL_SplitSrvFrame( void );			// once per frame: settings -> sv_split* for the local server
const char *CL_SplitSrvUserinfo( int n, const char *info );	// player health = handicap (local games)
qboolean CL_SplitSrvLocalGame( void );	// player 1 plays on this process's server
int		CL_SplitSrvLayout( void );			// 0 = no [entities]/[ps] rows (UrT, unknown), 1 baseq3, 2 missionpack
qboolean CL_SplitSrvUrT( void );
qboolean CL_SplitSrvCTF( void );			// the pending game type is a capture game
const char *CL_SplitSrvModInstagib( void );	// R16: the game's own instagib cvar, "" = none
qboolean CL_SplitSrvInstagibOn( void );		// R16: instagib on in the running local game (preset or the game's)
void	CL_SplitSrvOpen( void );			// the page opened: pending map / game type = current
void	CL_SplitSrvLeave( void );			// the page left: apply, at most one restart
const char *CL_SplitSrvPending( void );	// what leaving will do ("" = no restart)
void	CL_SplitSrvRestartRound( void );
void	CL_SplitSrvReset( void );
void	CL_SplitSrvRefreshMaps( void );
int		CL_SplitSrvNumMaps( void );
const char *CL_SplitSrvMapName( int i );	// current map first
const char *CL_SplitSrvCurrentMap( void );
qhandle_t CL_SplitSrvLevelshot( const char *map );	// 0 = none
void	CL_SplitSrvRefreshSets( void );
int		CL_SplitSrvNumSets( void );
const char *CL_SplitSrvSetName( int i );
const char *CL_SplitSrvActiveSet( void );
qboolean CL_SplitSrvSaveSet( const char *name, char *err, int errSize );
qboolean CL_SplitSrvLoadSet( int i );
qboolean CL_SplitSrvDeleteSet( int i );

// cl_aimassist.c: pad aim assist, local games only (design 15)
void	CL_AimAssistInit( void );
void	CL_AimAssistShutdown( void );
void	CL_AimAssistFrame( void );			// once per frame (player 1's marker -> userinfo)
void	CL_AimAssistDraw( void );			// cell glyphs, screen pass
void	CL_AimAssistReset( int n );
void	CL_AimAssistApply( int n, qboolean moveInput, qboolean lookInput, float *yawRate, float *pitchRate, float msec );
qboolean CL_AimAssistLocal( void );		// the hard gate: this process hosts the game
qboolean CL_AimAssistOn( int n );			// player n's assist is in effect
int		CL_AimAssistLevel( int n );			// its chosen strength 0..2
const char *CL_AimAssistUserinfo( int n, const char *info );	// name + marker while on

// cl_splitindep.c: Independent mode, one process per player (design 17.2)
void	CL_IndepInit( void );
void	CL_IndepShutdown( void );
void	CL_IndepFrame( void );				// once per frame (CL_SplitscreenFrame)
void	CL_IndepLoadBeat( void );			// child: heartbeat from SCR_UpdateScreen (loading screens too)
int		CL_IndepPadOwner( const char *key );	// coordinator: the slot (picking or with a window) playing with the pad of that key, or -1
const char *CL_IndepSlotKeys( int n );	// R19 coordinator: slot n's pad keys (space separated), NULL = free
int		CL_IndepSlotPid( int n );	// R19 coordinator: slot n's window pid (0 = none)
void	CL_IndepPadAlias( int n, const char *key );	// R19 coordinator: slot n's window may also open that device
void	CL_IndepPadUnalias( int n, const char *key );	// R19b coordinator: take that alias back (never the first key)
qboolean CL_IndepCoordinator( void );		// this process is player 1's, spawning a window per joining pad
qboolean CL_IndepChild( void );			// this process is a spawned player window
qboolean CL_IndepActive( void );			// either: audio keeps playing unfocused
int		CL_IndepFreeSlot( void );			// player slot for the next window, -1 = full
qboolean CL_IndepSlotLive( int n );		// slot n is a window (or is choosing a profile for one)
qboolean CL_IndepPicking( int n );			// slot n's pad is at the join-time profile picker
int		CL_IndepReserve( const char *padId );	// pad key(s) finished the join hold: slot or -1 (R19: -1 if the pad plays already)
void	CL_IndepCancel( int n );			// picker closed: no window
qboolean CL_IndepSpawn( int n, const char *profile );	// start slot n's window (profile NULL = Guest)
int		CL_IndepProfileUser( const char *key );	// the player whose window plays profile 'key', -1
qboolean CL_IndepChildPadMatch( const char *key );	// child: a device of this key is its pad (R19)
qboolean CL_IndepProfileAllowed( const char *name );	// child: a profile its own pages may offer
int		CL_IndepChildPlayer( void );		// child: the player number its window is (2..8), 0 = not a child
const char *CL_IndepModeNote( void );		// host page status line ("" = none)
void	CL_SplitTileLayout( int n, const int *slots, int wideSlot, int width, int height, viewRect_t *rects );	// cl_splitscreen.c

// window placement for the platform layer (win_glimp.c, CL_GetModeInfo): qtrue = Independent
// mode's borderless rect (any argument may be NULL)
qboolean CL_SplitWindowRect( int *x, int *y, int *w, int *h );
qboolean CL_IndepTiled( void );	// R20: this window is one tile of several (viewport layer: a Together cell)
void	CL_IndepAreaChanged( void );	// the platform's display changed (Linux: SDL video moved to X11): re-tile

// platform shim (win32/win_splitproc.c, unix/unix_splitproc.c on Linux SDL builds; failing stubs elsewhere)
#define SPLIT_FLAG_INDEPENDENT	1	// --independent
#define SPLIT_FLAG_CHILD		2	// --child
#define SPLIT_FLAG_NOACTIVATE	4	// --noactivate (and --child): windows never take the foreground
#define SPLIT_FLAG_BOTTOM		8	// --noactivate: and they open below other windows (test runs)
int		Sys_SplitLaunchFlags( void );
int		Sys_SplitPid( void );
int		Sys_SplitSpawn( const char *args );
int		Sys_SplitProcPid( int h );
qboolean Sys_SplitProcRunning( int h );
void	Sys_SplitProcKill( int h );
void	Sys_SplitProcClose( int h );
qboolean Sys_SplitPidRunning( int pid );
void	Sys_SplitAllowFocus( int pid );
void	Sys_SplitFocus( void );
int		Sys_SplitSockOpen( void );
void	Sys_SplitSockClose( void );
void	Sys_SplitSockSend( int port, const char *text );
int		Sys_SplitSockRecv( char *buf, int size, int *fromPort );
qboolean Sys_SplitWindowGet( int *x, int *y, int *w, int *h );
void	Sys_SplitKeepFocus( void );		// the window is about to be recreated: it gets the foreground back only if it has it now
qboolean Sys_SplitMonitorArea( int *x, int *y, int *w, int *h );
const char *Sys_SplitUnavailable( void );	// why Independent mode cannot run here (Gamescope ...), NULL = it can

// in_gamepad.c / cl_splitprofile.c helpers for Independent mode
const char *IN_PadDeviceId( int n );		// player n's pad key (R19 Pad_MakeKey), NULL = none
void	IN_PadRescan( void );				// R19 child: look at the devices it left closed again
void	IN_PadAttachVirtual( const char *guid );	// child: its test pad
const char *CL_ProfileSlotKey( int n );		// file key of n's profile, "guest", "" = none
void	CL_ProfileKeyFor( const char *name, char *key, int size );	// "" if not a valid name

// defined in cl_main.c
void	CL_CheckForResend( void );
void	CL_SendPureChecksums( void );

// defined in snd_dma.c: listeners of local players 2..8 (nearest wins)
void	S_SplitListener( int slot, int entityNum, const vec3_t origin, vec3_t axis[3] );

#endif // CL_SPLITSCREEN_H
