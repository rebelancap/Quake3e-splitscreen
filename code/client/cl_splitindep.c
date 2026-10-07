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
// cl_splitindep.c -- splitscreen Independent mode (design 17.2): every player
// gets a completely separate game in its own borderless window.
//
// The process the user started is the COORDINATOR (player 1).  It plays as a
// normal single-player instance, owns pad detection and the join hint, and
// when an unjoined pad finishes the join hold it shows the profile picker and
// starts a CHILD process of the same executable (--child), bound to that pad
// (GUID + ordinal) and the picked profile.  All windows are tiled with the
// splitscreen layout function (cl_splitFill / cl_splitWidePlayer /
// cl_splitVertical) inside the monitor (or cl_splitIndepArea); a child's exit
// re-tiles the rest, the coordinator's exit closes the children.
//
// Messages are text datagrams on 127.0.0.1 (one ephemeral UDP port per
// process), each starting with the session token from the child's command
// line:
//   coordinator -> child   rect <x> <y> <w> <h>   (every 500 ms: also the heartbeat)
//                          quit | focus
//   child -> coordinator   hello <player> <pid>
//                          hb <player> <pid> <profile key|guest> <loading 0|1>   (every 500 ms,
//                             and from the loading screen / before a vid_restart)
//                          bye <player>
// A child that exits (or crashes: its process handle -- the main signal) or, once
// it has sent its first heartbeat, stops sending for INDEP_HANG_TIMEOUT
// (INDEP_LOAD_TIMEOUT while it says it is loading) is dropped and the tiles re-flow.  Children are in a
// kill-on-close job (win_splitproc.c) and also quit by themselves when the
// coordinator's process is gone.
//
// A child never writes or seeds q3config-ss.cfg (common.c), _p1q3config.cfg, _padlast.cfg
// or _guest.cfg (cl_splitprofile.c); it logs to qconsole-child<N>.log and keeps
// pk3cache-child<N>.dat.  Nothing here runs unless Independent mode is on.

#include "client.h"

#define INDEP_SEND_INTERVAL	500		// msec between rect/heartbeat messages
#define INDEP_HELLO_TIMEOUT	30000	// a started child must say hello within this
#define INDEP_HANG_TIMEOUT	15000	// a child silent this long is hung: killed
#define INDEP_LOAD_TIMEOUT	120000	// the same while it says it is loading (or before its first heartbeat)
#define INDEP_QUIT_GRACE	3000	// msec children get to quit with the coordinator
#define INDEP_RECT_POLL		500		// msec between window-rect checks (log)

typedef enum { IC_FREE, IC_PICKING, IC_STARTING, IC_RUNNING } icState_t;

typedef struct {
	icState_t	state;
	int			proc;			// shim process handle
	int			pid;
	int			port;			// its IPC port (from its first message)
	int			spawnTime;
	int			lastHeard;
	int			lastSent;
	int			joinSeq;
	qboolean	hbSeen;		// its first heartbeat came (the hang clock runs from then)
	qboolean	loading;	// its last heartbeat said "loading" (map load, vid_restart)
	char		padId[MAX_CVAR_VALUE_STRING];	// R19: its pad's key(s), space separated (in_gamepad.c Pad_MakeKey: the
									// pad + devices found to duplicate it); "" = none (console addplayer)
	int			hasPad;			// R19: its last heartbeat said it has its pad open (-1 = not said yet)
	int			noPadSince;		// R19: msec since it says it has none (0 = has one / not said)
	qboolean	noPadLogged;
	char		profileKey[PROFILE_NAME_LEN + 2];	// key or "guest"
	viewRect_t	rect;			// screen rect it was told
} indepChild_t;

static indepChild_t	ic[MAX_SPLITVIEW];		// [1..7] = players 2..8; [0] unused (the coordinator)

static qboolean	indepActive;		// coordinator: Independent mode is on in this process
static qboolean	indepWant;			// coordinator: the mode asked for (switch pending while players remain)
static qboolean	indepChild;			// this process is a child window
static int		indepJoinCounter;
static viewRect_t	indepWin;		// this window's screen rect (w 0 = not set)
static viewRect_t	indepApplied;	// the rect last given to the window
static int		indepTiles;			// coordinator: windows in the last re-tile (indepWin's layout)
static int		indepAppliedTiles;	// coordinator: the same when indepApplied was taken (R20: > 1 = a tile)
static viewRect_t	indepLogged;	// the window rect last logged
static int		indepRectPoll;
static char		indepToken[20];
static int		indepTileFill = -1, indepTileVert = -1, indepTileWide = -1;
static char		indepTileArea[MAX_CVAR_VALUE_STRING];

// child
static int		childPlayer;		// 2..8
static int		childCoordPort;
static int		childCoordPid;
static int		childLastSent;
static int		childPidCheck;
static qboolean	childQuitting;
static char		childPadKeys[MAX_CVAR_VALUE_STRING];	// R19: the keys of the one pad this window may open
static char		childStartKey[PROFILE_NAME_LEN + 2];

static cvar_t	*cl_splitIndependent;
static cvar_t	*cl_splitIndepArea;
static cvar_t	*cl_splitChild;
static cvar_t	*cl_splitChildIpc;
static cvar_t	*cl_splitChildPad;
static cvar_t	*cl_splitChildProfile;
static cvar_t	*cl_splitWindowRectCvar;
static cvar_t	*cl_splitChildArgs;

static void Indep_ChildBeat( qboolean forceLoading );


#ifdef _WIN32
const char *Sys_SplitUnavailable( void ) { return NULL; }
#else
/*
=============================================================================

PLATFORM STUBS (no Independent mode; design 14.5)

Weak: Linux SDL builds link code/unix/unix_splitproc.c, whose definitions
replace these; every other non-Windows build (macOS, BSD, Linux without
SDL) keeps them.

=============================================================================
*/
#define SPLIT_STUB	__attribute__((weak))
SPLIT_STUB void Sys_SplitParseFlags( const char *cmdline ) { }
SPLIT_STUB const char *Sys_SplitUnavailable( void ) { return "here (Windows/Linux only)"; }
SPLIT_STUB int Sys_SplitLaunchFlags( void ) { return 0; }
SPLIT_STUB int Sys_SplitPid( void ) { return 0; }
SPLIT_STUB int Sys_SplitSpawn( const char *args ) { return -1; }
SPLIT_STUB int Sys_SplitProcPid( int h ) { return 0; }
SPLIT_STUB qboolean Sys_SplitProcRunning( int h ) { return qfalse; }
SPLIT_STUB void Sys_SplitProcKill( int h ) { }
SPLIT_STUB void Sys_SplitProcClose( int h ) { }
SPLIT_STUB qboolean Sys_SplitPidRunning( int pid ) { return qfalse; }
SPLIT_STUB void Sys_SplitAllowFocus( int pid ) { }
SPLIT_STUB void Sys_SplitFocus( void ) { }
SPLIT_STUB int Sys_SplitSockOpen( void ) { return 0; }
SPLIT_STUB void Sys_SplitSockClose( void ) { }
SPLIT_STUB void Sys_SplitSockSend( int port, const char *text ) { }
SPLIT_STUB int Sys_SplitSockRecv( char *buf, int size, int *fromPort ) { return -1; }
SPLIT_STUB qboolean Sys_SplitWindowGet( int *x, int *y, int *w, int *h ) { return qfalse; }
SPLIT_STUB void Sys_SplitKeepFocus( void ) { }
SPLIT_STUB qboolean Sys_SplitMonitorArea( int *x, int *y, int *w, int *h ) { return qfalse; }
#endif


/*
=============================================================================

QUERIES

=============================================================================
*/

qboolean CL_IndepCoordinator( void ) {
	return indepActive;
}


qboolean CL_IndepChild( void ) {
	return indepChild;
}


qboolean CL_IndepActive( void ) {
	return ( indepActive || indepChild ) ? qtrue : qfalse;
}


int CL_IndepChildPlayer( void ) {
	return indepChild ? childPlayer : 0;
}


static qboolean Indep_Live( int n ) {
	return ( n > 0 && n < MAX_SPLITVIEW && ( ic[n].state == IC_STARTING || ic[n].state == IC_RUNNING ) ) ? qtrue : qfalse;
}


qboolean CL_IndepSlotLive( int n ) {
	return ( indepActive && n > 0 && n < MAX_SPLITVIEW && ic[n].state != IC_FREE ) ? qtrue : qfalse;
}


// R19: is 'key' one of the space-separated keys in 'list'?
static qboolean Indep_KeyIn( const char *list, const char *key ) {
	const int len = (int)strlen( key );
	const char *p = list;

	if ( !len ) {
		return qfalse;
	}
	while ( *p ) {
		while ( *p == ' ' ) {
			p++;
		}
		if ( !Q_stricmpn( p, key, len ) && ( p[len] == ' ' || p[len] == '\0' ) ) {
			return qtrue;
		}
		while ( *p && *p != ' ' ) {
			p++;
		}
	}
	return qfalse;
}


// coordinator: the slot (picking or with a window) that plays with the pad of this key
// (that pad is the window's, never a join or heal here), -1 = none
int CL_IndepPadOwner( const char *key ) {
	int n;

	if ( !indepActive || !key || !key[0] ) {
		return -1;
	}
	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( ic[n].state != IC_FREE && Indep_KeyIn( ic[n].padId, key ) ) {
			return n;
		}
	}
	return -1;
}


// R19, coordinator: slot n's pad keys / pid
const char *CL_IndepSlotKeys( int n ) {
	return ( indepActive && n > 0 && n < MAX_SPLITVIEW && ic[n].state != IC_FREE ) ? ic[n].padId : NULL;
}


int CL_IndepSlotPid( int n ) {
	return ( indepActive && n > 0 && n < MAX_SPLITVIEW ) ? ic[n].pid : 0;
}


static void Indep_Send( int n, const char *text );

// R19b, coordinator: take an alias back (it moved on its own: another person's pad); the first
// key (the pad that joined) is never removed
void CL_IndepPadUnalias( int n, const char *key ) {
	char list[MAX_CVAR_VALUE_STRING], word[64];
	const char *p;
	int len;

	if ( !indepActive || n <= 0 || n >= MAX_SPLITVIEW || ic[n].state == IC_FREE || !key || !key[0] ) {
		return;
	}
	list[0] = '\0';
	for ( p = ic[n].padId; *p; ) {
		while ( *p == ' ' ) {
			p++;
		}
		for ( len = 0; p[len] && p[len] != ' '; len++ )
			;
		if ( !len ) {
			break;
		}
		Q_strncpyz( word, p, MIN( len + 1, (int)sizeof( word ) ) );
		p += len;
		if ( list[0] && !Q_stricmp( word, key ) ) {
			continue;	// (an alias; the first key stays)
		}
		if ( list[0] ) {
			Q_strcat( list, sizeof( list ), " " );
		}
		Q_strcat( list, sizeof( list ), word );
	}
	if ( !strcmp( list, ic[n].padId ) ) {
		return;
	}
	Q_strncpyz( ic[n].padId, list, sizeof( ic[n].padId ) );
	Com_Printf( "indep: pad %s is not P%i's after all; P%i's pad keys now: %s\n", key, n + 1, n + 1, ic[n].padId );
	if ( ic[n].state == IC_RUNNING ) {
		Indep_Send( n, va( "pad %s", ic[n].padId ) );
	}
}


// R19, coordinator: slot n's window may also open the device of this key (a second listing
// of its pad, or its pad back under a new key); told at once and with every rect message
void CL_IndepPadAlias( int n, const char *key ) {
	if ( !indepActive || n <= 0 || n >= MAX_SPLITVIEW || ic[n].state == IC_FREE || !key || !key[0]
		|| Indep_KeyIn( ic[n].padId, key ) ) {
		return;
	}
	if ( strlen( ic[n].padId ) + strlen( key ) + 2 > 200 ) {
		Com_Printf( S_COLOR_YELLOW "indep: P%i: no room for another key of its pad (%s)\n", n + 1, key );
		return;
	}
	if ( ic[n].padId[0] ) {
		Q_strcat( ic[n].padId, sizeof( ic[n].padId ), " " );
	}
	Q_strcat( ic[n].padId, sizeof( ic[n].padId ), key );
	Com_Printf( "indep: pad %s -> P%i (pid %i): the same pad, another device; P%i's pad keys now: %s\n", key, n + 1, ic[n].pid,
		n + 1, ic[n].padId );
	if ( ic[n].state == IC_RUNNING ) {
		Indep_Send( n, va( "pad %s", ic[n].padId ) );
	}
}
qboolean CL_IndepPicking( int n ) {
	return ( indepActive && n > 0 && n < MAX_SPLITVIEW && ic[n].state == IC_PICKING ) ? qtrue : qfalse;
}


int CL_IndepFreeSlot( void ) {
	int n, max;

	if ( !indepActive ) {
		return -1;
	}
	max = Cvar_VariableIntegerValue( "cl_splitMaxPlayers" );
	for ( n = 1; n < max && n < MAX_SPLITVIEW; n++ ) {
		if ( ic[n].state == IC_FREE ) {
			return n;
		}
	}
	return -1;
}


int CL_IndepProfileUser( const char *key ) {
	int n;

	if ( !indepActive || !key || !key[0] ) {
		return -1;
	}
	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( Indep_Live( n ) && Q_stricmp( ic[n].profileKey, "guest" ) && !Q_stricmp( ic[n].profileKey, key ) ) {
			return n;
		}
	}
	return -1;
}


// child: a device of this key is this window's pad (R19: one of the keys the coordinator gave it)
qboolean CL_IndepChildPadMatch( const char *key ) {
	if ( !indepChild ) {
		return qtrue;
	}
	return ( childPadKeys[0] && key && Indep_KeyIn( childPadKeys, key ) ) ? qtrue : qfalse;
}


// a child's own pages offer Guest, the profile it was started with and the one it plays
qboolean CL_IndepProfileAllowed( const char *name ) {
	char key[PROFILE_NAME_LEN + 2];

	if ( !indepChild ) {
		return qtrue;
	}
	CL_ProfileKeyFor( name, key, sizeof( key ) );
	if ( !key[0] ) {
		return qfalse;
	}
	return ( !Q_stricmp( key, childStartKey ) || !Q_stricmp( key, CL_ProfileSlotKey( 0 ) ) ) ? qtrue : qfalse;
}


const char *CL_IndepModeNote( void ) {
	if ( indepChild ) {
		return "";
	}
	if ( !indepActive && Sys_SplitUnavailable() ) {
		return va( "Unavailable %s", Sys_SplitUnavailable() );	// under the Session mode row
	}
	if ( indepWant && !indepActive ) {
		return "Independent (experimental) starts when the others have left";
	}
	if ( !indepWant && indepActive ) {
		return "Together starts when the other windows close";
	}
	if ( indepActive ) {
		return ( Sys_SplitLaunchFlags() & SPLIT_FLAG_INDEPENDENT ) ? "Each joining pad gets a window (--independent)"
			: "Each joining pad gets its own window";
	}
	return "";
}


qboolean CL_SplitWindowRect( int *x, int *y, int *w, int *h ) {
	if ( !( indepActive || indepChild ) || indepWin.w <= 0 || indepWin.h <= 0 ) {
		return qfalse;
	}
	if ( x ) *x = indepWin.x;
	if ( y ) *y = indepWin.y;
	if ( w ) *w = indepWin.w;
	if ( h ) *h = indepWin.h;
	return qtrue;
}


/*
==================
CL_IndepTiled

R20: this window is one tile of several (a child window always; player 1's
window once its applied rect is a share of the area).  The viewport layer
then treats the whole window as a Together cell of that size (cl_splitAspect,
FOV widening, HUD placement), so a tile looks like the same Together cell.
A single Independent window that covers the area renders like one Together
player (pass-through).
==================
*/
qboolean CL_IndepTiled( void ) {
	if ( indepApplied.w <= 0 || indepApplied.h <= 0 ) {
		return qfalse;
	}
	if ( indepChild ) {
		return qtrue;
	}
	return ( indepActive && indepAppliedTiles > 1 ) ? qtrue : qfalse;
}


/*
=============================================================================

WINDOW RECTS

=============================================================================
*/

static qboolean Indep_SameRect( const viewRect_t *a, const viewRect_t *b ) {
	return ( a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h ) ? qtrue : qfalse;
}


// the region all windows share: cl_splitIndepArea "x y w h", else the whole monitor
static void Indep_Area( viewRect_t *a ) {
	if ( sscanf( cl_splitIndepArea->string, "%i %i %i %i", &a->x, &a->y, &a->w, &a->h ) == 4 && a->w >= 160 && a->h >= 120 ) {
		return;
	}
	if ( !Sys_SplitMonitorArea( &a->x, &a->y, &a->w, &a->h ) ) {
		a->x = a->y = 0;
		a->w = 1280;
		a->h = 720;
	}
}


/*
==================
Indep_Retile

Coordinator: one cell per window (itself + every started child; a pad still
at the profile picker gets none yet), the same layout as Together mode.
==================
*/
static void Indep_Retile( const char *why ) {
	int slots[MAX_SPLITVIEW], i, n, wide, lastSeq, wp;
	viewRect_t rects[MAX_SPLITVIEW], area;
	char line[MAX_STRING_CHARS];

	Indep_Area( &area );
	n = 0;
	slots[n++] = 0;
	wide = -1;
	lastSeq = 0;
	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		if ( Indep_Live( i ) ) {
			slots[n++] = i;
			if ( ic[i].joinSeq > lastSeq ) {
				lastSeq = ic[i].joinSeq;
				wide = i;	// default: the last joined
			}
		}
	}
	wp = Cvar_VariableIntegerValue( "cl_splitWidePlayer" );
	if ( wp == 1 || ( wp > 1 && Indep_Live( wp - 1 ) ) ) {
		wide = wp - 1;
	}

	Com_Memset( rects, 0, sizeof( rects ) );
	CL_SplitTileLayout( n, slots, wide, area.w, area.h, rects );
	indepTiles = n;

	indepWin = rects[0];
	indepWin.x += area.x;
	indepWin.y += area.y;
	Com_sprintf( line, sizeof( line ), "indep: tiles (%s) for %i window%s in %i,%i %ix%i: P1 %i,%i %ix%i", why, n, n > 1 ? "s" : "",
		area.x, area.y, area.w, area.h, indepWin.x, indepWin.y, indepWin.w, indepWin.h );
	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		if ( !Indep_Live( i ) ) {
			continue;
		}
		ic[i].rect = rects[i];
		ic[i].rect.x += area.x;
		ic[i].rect.y += area.y;
		ic[i].lastSent = 0;		// tell it now
		Q_strcat( line, sizeof( line ), va( " | P%i %i,%i %ix%i", i + 1, ic[i].rect.x, ic[i].rect.y, ic[i].rect.w, ic[i].rect.h ) );
	}
	Com_Printf( "%s\n", line );

	indepTileFill = Cvar_VariableIntegerValue( "cl_splitFill" );
	indepTileVert = Cvar_VariableIntegerValue( "cl_splitVertical" );
	indepTileWide = wp;
	Q_strncpyz( indepTileArea, cl_splitIndepArea->string, sizeof( indepTileArea ) );
}


// the platform layer changed the display under Independent mode (Linux: SDL video restarted on
// its X11 driver, whose screen is XWayland's): measure the tiles again; the window about to be
// created takes the new rect (sdl_glimp.c)
void CL_IndepAreaChanged( void ) {
	if ( indepActive ) {
		Indep_Retile( "display changed" );
		indepApplied = indepWin;
		indepAppliedTiles = indepTiles;
	}
}


/*
==================
Indep_ApplyOwn

Move this window to its rect once nothing is loading: a full vid_restart,
which recreates the borderless window at the new rect (CL_SplitWindowRect)
-- the same path as a normal start.  (Resizing the kept window and
"vid_restart fast" made the Vulkan renderer draw past its new height: a GPU
fault and driver reset on this box, R13.)  The new window takes the
foreground only if the old one had it.  Run from the frame, not queued: a
waiting command buffer (a script's 'wait') must not hold the re-tile back.
==================
*/
static void Indep_ApplyOwn( void ) {
	int x, y, w, h;

	if ( !CL_SplitWindowRect( NULL, NULL, NULL, NULL ) || Indep_SameRect( &indepWin, &indepApplied ) ) {
		return;
	}
	if ( ( cls.state != CA_DISCONNECTED && cls.state != CA_ACTIVE ) || !Sys_SplitWindowGet( &x, &y, &w, &h ) ) {
		return;
	}
	if ( cls.lastVidRestart && abs( cls.lastVidRestart - Sys_Milliseconds() ) < 600 ) {
		return;		// vid_restart refuses this soon after a cgame start (OSP hack): next frame
	}
	indepApplied = indepWin;
	indepAppliedTiles = indepTiles;
	Com_Printf( "indep: window -> %i,%i %ix%i (vid_restart)\n", indepWin.x, indepWin.y, indepWin.w, indepWin.h );
	if ( indepChild ) {
		Indep_ChildBeat( qtrue );	// "loading": a full vid_restart (Vulkan pipelines) sends nothing for a while
	}
	Sys_SplitKeepFocus();
	Cbuf_ExecuteText( EXEC_NOW, "vid_restart\n" );
}


// every window logs its own rect from the OS whenever it changes
static void Indep_LogRect( void ) {
	viewRect_t r;
	const int now = Sys_Milliseconds();

	if ( now - indepRectPoll < INDEP_RECT_POLL ) {
		return;
	}
	indepRectPoll = now;
	if ( !Sys_SplitWindowGet( &r.x, &r.y, &r.w, &r.h ) || Indep_SameRect( &r, &indepLogged ) ) {
		return;
	}
	indepLogged = r;
	Com_Printf( "window: P%i rect %i,%i %ix%i (GetWindowRect), drawable %ix%i\n", indepChild ? childPlayer : 1,
		r.x, r.y, r.w, r.h, cls.glconfig.vidWidth, cls.glconfig.vidHeight );
}


/*
=============================================================================

COORDINATOR

=============================================================================
*/

static void Indep_Send( int n, const char *text ) {
	if ( ic[n].port > 0 ) {
		Sys_SplitSockSend( ic[n].port, va( "%s %s", indepToken, text ) );
	}
}


static void Indep_Free( int n ) {
	if ( ic[n].proc >= 0 ) {
		Sys_SplitProcClose( ic[n].proc );
	}
	Com_Memset( &ic[n], 0, sizeof( ic[n] ) );
	ic[n].proc = -1;
	ic[n].state = IC_FREE;
}


int CL_IndepReserve( const char *padId ) {
	const int n = CL_IndepFreeSlot();
	char first[64];
	int owner;

	// R19: a pad that already plays somewhere never gets a second window
	if ( padId && padId[0] ) {
		Q_strncpyz( first, padId, sizeof( first ) );
		if ( strchr( first, ' ' ) ) {
			*strchr( first, ' ' ) = '\0';
		}
		owner = CL_IndepPadOwner( first );
		if ( owner >= 0 && strchr( first, '@' ) ) {	// (an order key "#n" can name another device by now)
			Com_Printf( "indep: pad %s already plays as P%i: no second window\n", first, owner + 1 );
			return -1;
		}
	}
	if ( n < 0 ) {
		Com_Printf( "indep: no room for another window (cl_splitMaxPlayers %i)\n", Cvar_VariableIntegerValue( "cl_splitMaxPlayers" ) );
		return -1;
	}
	Indep_Free( n );
	ic[n].state = IC_PICKING;
	Q_strncpyz( ic[n].padId, padId ? padId : "", sizeof( ic[n].padId ) );
	ic[n].hasPad = -1;
	Com_Printf( "indep: P%i (pad %s) is choosing a profile for its window\n", n + 1, ic[n].padId[0] ? ic[n].padId : "none" );
	return n;
}


void CL_IndepCancel( int n ) {
	if ( CL_IndepPicking( n ) ) {
		Com_Printf( "indep: P%i's window cancelled at the picker\n", n + 1 );
		Indep_Free( n );
	}
}


// the child's command line: built with an overflow check (it must fit what the child's
// WinMain keeps, MAX_CMDLINE_CHARS, in at most MAX_CONSOLE_LINES "+" commands)
static qboolean	indepArgsBad;

static void Indep_Cat( char *args, int size, const char *text ) {
	if ( (int)( strlen( args ) + strlen( text ) ) >= size ) {
		indepArgsBad = qtrue;
		return;
	}
	strcat( args, text );
}


static void Indep_Arg( char *args, int size, const char *name, const char *value ) {
	if ( strchr( value, '"' ) ) {
		Com_Printf( S_COLOR_YELLOW "indep: %s holds a '\"': it cannot go on a window's command line\n", name );
		indepArgsBad = qtrue;
		return;
	}
	Indep_Cat( args, size, va( " +set %s \"%s\"", name, value ) );
}


// a setting the child would get anyway (its default) is left off the line
static void Indep_ArgIfSet( char *args, int size, const char *name ) {
	const char *s = Cvar_VariableString( name );

	if ( strcmp( s, Cvar_DefaultString( name ) ) ) {
		Indep_Arg( args, size, name, s );
	}
}


// the "+" commands (outside quotes) the child's Com_ParseCommandLine will see
static int Indep_CountLines( const char *s ) {
	int n = 0, inq = 0;

	for ( ; *s; s++ ) {
		if ( *s == '"' ) {
			inq = !inq;
		} else if ( ( *s == '+' && !inq ) || *s == '\n' || *s == '\r' ) {
			n++;
		}
	}
	return inq ? -1 : n;	// -1: unbalanced quote
}


/*
==================
CL_IndepSpawn

Start player n's window: the same executable with --child, its pad, its
profile (NULL = Guest), its rect, the shared data paths and the host
settings it needs.
==================
*/
qboolean CL_IndepSpawn( int n, const char *profile ) {
	static char args[MAX_CMDLINE_CHARS + 1024];
	char key[PROFILE_NAME_LEN + 2];
	const char *s, *p;
	int h, i, lines;

	if ( !indepActive || n <= 0 || n >= MAX_SPLITVIEW || ic[n].state != IC_PICKING ) {
		return qfalse;
	}
	key[0] = '\0';
	if ( profile && profile[0] && Q_stricmp( profile, "guest" ) ) {
		CL_ProfileKeyFor( profile, key, sizeof( key ) );
		i = CL_ProfileFind( profile );
		if ( !key[0] || i < 0 || ( CL_ProfileListUser( i ) >= 0 && CL_ProfileListUser( i ) != n ) ) {
			Com_Printf( "indep: profile \"%s\" is not available: P%i plays as a Guest\n", profile, n + 1 );
			key[0] = '\0';
		}
	}
	Q_strncpyz( ic[n].profileKey, key[0] ? key : "guest", sizeof( ic[n].profileKey ) );

	ic[n].state = IC_STARTING;
	ic[n].joinSeq = ++indepJoinCounter;
	ic[n].spawnTime = ic[n].lastHeard = Sys_Milliseconds();
	ic[n].port = 0;
	ic[n].hbSeen = qfalse;
	ic[n].loading = qfalse;
	Indep_Retile( va( "P%i joins", n + 1 ) );

	indepArgsBad = qfalse;
	args[0] = '\0';
	Indep_Cat( args, sizeof( args ), "--child" );
	if ( Sys_SplitLaunchFlags() & SPLIT_FLAG_BOTTOM ) {
		Indep_Cat( args, sizeof( args ), " --noactivate" );
	}
	Indep_Arg( args, sizeof( args ), "cl_title", va( "%s - Player %i", CLIENT_WINDOW_TITLE, n + 1 ) );
	Indep_Arg( args, sizeof( args ), "cl_splitChild", va( "%i", n + 1 ) );
	Indep_Arg( args, sizeof( args ), "cl_splitChildIpc", va( "%i %s %i", Sys_SplitSockOpen(), indepToken, Sys_SplitPid() ) );
	Indep_Arg( args, sizeof( args ), "cl_splitChildPad", ic[n].padId );
	Indep_Arg( args, sizeof( args ), "cl_splitChildProfile", key[0] ? key : "guest" );
	Indep_Arg( args, sizeof( args ), "cl_splitWindowRect", va( "%i %i %i %i", ic[n].rect.x, ic[n].rect.y, ic[n].rect.w, ic[n].rect.h ) );
	// the same data (the same exe: the same defaults, which need not be passed)
	Indep_ArgIfSet( args, sizeof( args ), "fs_basepath" );
	Indep_ArgIfSet( args, sizeof( args ), "fs_homepath" );
	Indep_ArgIfSet( args, sizeof( args ), "fs_basegame" );
	s = Cvar_VariableString( "fs_game" );
	if ( s[0] ) {
		Indep_Arg( args, sizeof( args ), "fs_game", s );
	}
	// borderless at its rect (the rect above wins; these keep a plain restart borderless too)
	Indep_Arg( args, sizeof( args ), "r_fullscreen", "0" );
	Indep_Arg( args, sizeof( args ), "r_noborder", "1" );
	Indep_Arg( args, sizeof( args ), "r_mode", "-1" );
	Indep_Arg( args, sizeof( args ), "r_customwidth", va( "%i", ic[n].rect.w ) );
	Indep_Arg( args, sizeof( args ), "r_customheight", va( "%i", ic[n].rect.h ) );
	Indep_Arg( args, sizeof( args ), "vid_xpos", va( "%i", ic[n].rect.x ) );
	Indep_Arg( args, sizeof( args ), "vid_ypos", va( "%i", ic[n].rect.y ) );
	// host settings: one pad player, no joins, the host's aim-assist switch
	Indep_Arg( args, sizeof( args ), "cl_splitP1Input", "pad" );
	Indep_Arg( args, sizeof( args ), "cl_splitMaxPlayers", "1" );
	Indep_Arg( args, sizeof( args ), "cl_splitJoinHint", "0" );
	Indep_Arg( args, sizeof( args ), "cl_aimAssistAllow", Cvar_VariableString( "cl_aimAssistAllow" ) );
	Indep_Arg( args, sizeof( args ), "in_gamepad", "1" );
	Indep_Arg( args, sizeof( args ), "s_muteWhenUnfocused", "0" );
	Indep_Arg( args, sizeof( args ), "net_port", va( "%i", Cvar_VariableIntegerValue( "net_port" ) + 20 + n ) );
	Indep_Arg( args, sizeof( args ), "net_qport", va( "%i", ( Cvar_VariableIntegerValue( "net_qport" ) + 1000 * n + 17 ) & 0xffff ) );
	if ( Cvar_VariableIntegerValue( "logfile" ) ) {
		Indep_Arg( args, sizeof( args ), "logfile", Cvar_VariableString( "logfile" ) );
	}
	// test runs (developer): extra command-line text, "@@" = the player number
	if ( cl_splitChildArgs->string[0] && com_developer && com_developer->integer ) {
		Indep_Cat( args, sizeof( args ), " " );
		for ( p = cl_splitChildArgs->string; *p && !indepArgsBad; p++ ) {
			if ( p[0] == '@' && p[1] == '@' ) {
				Indep_Cat( args, sizeof( args ), va( "%i", n + 1 ) );
				p += 1;
			} else {
				char c[2] = { *p, '\0' };
				Indep_Cat( args, sizeof( args ), c );
			}
		}
	}

	// the child keeps MAX_CMDLINE_CHARS - 1 bytes in MAX_CONSOLE_LINES commands: a longer line
	// would lose its tail (net_port, logfile ...) without a word, so no window at all
	lines = Indep_CountLines( args );
	h = -1;
	if ( indepArgsBad || (int)strlen( args ) >= MAX_CMDLINE_CHARS || lines < 0 || lines > MAX_CONSOLE_LINES ) {
		Com_Printf( S_COLOR_RED "indep: P%i's window NOT started: its command line is %s (%i bytes, %i commands; "
			"a window takes %i bytes, %i commands) -- shorter fs_basepath / fs_homepath / fs_game%s\n", n + 1,
			lines < 0 ? "unbalanced (a quote)" : "too long", indepArgsBad ? (int)sizeof( args ) : (int)strlen( args ), lines,
			MAX_CMDLINE_CHARS - 1, MAX_CONSOLE_LINES, cl_splitChildArgs->string[0] ? " / cl_splitChildArgs" : "" );
	} else {
		h = Sys_SplitSpawn( args );
	}
	if ( h < 0 ) {
		Com_Printf( S_COLOR_YELLOW "indep: could not start P%i's window\n", n + 1 );
		Indep_Free( n );
		Indep_Retile( va( "P%i failed to start", n + 1 ) );
		return qfalse;
	}
	ic[n].proc = h;
	ic[n].pid = Sys_SplitProcPid( h );
	Com_Printf( "indep: P%i window starting: pid %i, pad %s, profile %s, rect %i,%i %ix%i\n", n + 1, ic[n].pid,
		ic[n].padId[0] ? ic[n].padId : "none", ic[n].profileKey, ic[n].rect.x, ic[n].rect.y, ic[n].rect.w, ic[n].rect.h );
	if ( ic[n].padId[0] ) {
		Com_Printf( "indep: pad %s -> P%i (pid %i)\n", ic[n].padId, n + 1, ic[n].pid );
	}
	Com_DPrintf( "indep: args (%i bytes, %i commands) %s\n", (int)strlen( args ), lines, args );
	return qtrue;
}


// one message from a child
static void Indep_Message( const char *msg, int fromPort ) {
	char tok[24], cmd[16], key[PROFILE_NAME_LEN + 2];
	int player, pid, n, loading, now, gap, hasPad;

	key[0] = '\0';
	player = pid = loading = 0;
	hasPad = -1;
	if ( sscanf( msg, "%23s %15s %i %i %21s %i %i", tok, cmd, &player, &pid, key, &loading, &hasPad ) < 3 || strcmp( tok, indepToken ) ) {
		return;
	}
	n = player - 1;
	if ( !Indep_Live( n ) ) {
		return;
	}
	now = Sys_Milliseconds();
	if ( !Q_stricmp( cmd, "bye" ) ) {
		Com_Printf( "indep: P%i window says bye\n", n + 1 );
		ic[n].lastHeard = now;
		return;
	}
	if ( pid != ic[n].pid ) {
		return;
	}
	gap = now - ic[n].lastHeard;
	ic[n].port = fromPort;
	ic[n].lastHeard = now;
	if ( ic[n].state == IC_STARTING ) {
		ic[n].state = IC_RUNNING;
		ic[n].lastSent = 0;
		Com_Printf( "indep: P%i window up: hello from pid %i (port %i), %i ms after start\n", n + 1, pid, fromPort,
			ic[n].lastHeard - ic[n].spawnTime );
	}
	if ( Q_stricmp( cmd, "hb" ) ) {
		return;
	}
	if ( !ic[n].hbSeen ) {
		ic[n].hbSeen = qtrue;
		Com_Printf( "indep: P%i window running: first heartbeat %i ms after start\n", n + 1, now - ic[n].spawnTime );
	} else if ( gap > 2 * INDEP_SEND_INTERVAL + 1000 ) {
		Com_Printf( "indep: P%i heard again after %i ms of silence%s\n", n + 1, gap, ic[n].loading ? " (it was loading)" : "" );
	}
	if ( ( loading != 0 ) != ic[n].loading ) {
		ic[n].loading = loading ? qtrue : qfalse;
		Com_DPrintf( "indep: P%i %s\n", n + 1, ic[n].loading ? "is loading (hang timeout 120 s)" : "finished loading" );
	}
	// R19: does the window have its pad open?  (a window with a pad key that finds no such
	// device for a while says so here, once; and when it finds it)
	if ( hasPad >= 0 && ic[n].padId[0] ) {
		if ( hasPad ) {
			if ( ic[n].hasPad == 0 && ic[n].noPadLogged ) {
				Com_Printf( "indep: P%i's window has its pad again (%s)\n", n + 1, ic[n].padId );
			}
			ic[n].noPadSince = 0;
			ic[n].noPadLogged = qfalse;
		} else if ( !ic[n].noPadSince ) {
			ic[n].noPadSince = now | 1;
		} else if ( !ic[n].noPadLogged && now - ic[n].noPadSince > 5000 ) {
			ic[n].noPadLogged = qtrue;
			Com_Printf( S_COLOR_YELLOW "indep: P%i's window has found no device of its pad for %i s (keys %s): unplugged, "
				"or listed differently there -- it comes back to P%i when it shows up\n", n + 1, ( now - ic[n].noPadSince ) / 1000,
				ic[n].padId, n + 1 );
		}
		ic[n].hasPad = hasPad;
	}
	if ( key[0] && Q_stricmp( key, ic[n].profileKey ) ) {
		Com_Printf( "indep: P%i now plays as %s (was %s)\n", n + 1, key, ic[n].profileKey );
		Q_strncpyz( ic[n].profileKey, key, sizeof( ic[n].profileKey ) );
	}
}


// msec of silence after which a running child counts as hung
static int Indep_HangLimit( int n ) {
	return ( !ic[n].hbSeen || ic[n].loading ) ? INDEP_LOAD_TIMEOUT : INDEP_HANG_TIMEOUT;
}



static void Indep_CoordinatorFrame( void ) {
	char buf[256];
	int n, port, now;
	qboolean retile = qfalse;
	const char *why = "";

	while ( Sys_SplitSockRecv( buf, sizeof( buf ), &port ) >= 0 ) {
		Indep_Message( buf, port );
	}

	now = Sys_Milliseconds();
	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( !Indep_Live( n ) ) {
			continue;
		}
		if ( !Sys_SplitProcRunning( ic[n].proc ) ) {
			Com_Printf( "indep: P%i window closed (pid %i exited)\n", n + 1, ic[n].pid );
			Indep_Free( n );
			retile = qtrue;
			why = va( "P%i left", n + 1 );
			continue;
		}
		if ( ( ic[n].state == IC_RUNNING && now - ic[n].lastHeard > Indep_HangLimit( n ) )
			|| ( ic[n].state == IC_STARTING && now - ic[n].spawnTime > INDEP_HELLO_TIMEOUT ) ) {
			Com_Printf( S_COLOR_YELLOW "indep: P%i window %s: closing it\n", n + 1, ic[n].state == IC_RUNNING
				? va( "silent for %i ms (heartbeat lost%s)", now - ic[n].lastHeard, !ic[n].hbSeen ? ", never ran" : ic[n].loading ? ", while loading" : "" ) : "never said hello" );
			Sys_SplitProcKill( ic[n].proc );
			Indep_Free( n );
			retile = qtrue;
			why = va( "P%i lost", n + 1 );
			continue;
		}
		if ( ic[n].state == IC_RUNNING && ( !ic[n].lastSent || now - ic[n].lastSent >= INDEP_SEND_INTERVAL ) ) {
			ic[n].lastSent = now ? now : 1;
			Indep_Send( n, va( "rect %i %i %i %i", ic[n].rect.x, ic[n].rect.y, ic[n].rect.w, ic[n].rect.h ) );
			if ( ic[n].padId[0] ) {
				Indep_Send( n, va( "pad %s", ic[n].padId ) );	// R19: its pad's keys (a lost datagram heals itself)
			}
		}
	}

	// layout options changed (host page / console)
	if ( !retile && ( Cvar_VariableIntegerValue( "cl_splitFill" ) != indepTileFill
		|| Cvar_VariableIntegerValue( "cl_splitVertical" ) != indepTileVert
		|| Cvar_VariableIntegerValue( "cl_splitWidePlayer" ) != indepTileWide
		|| strcmp( cl_splitIndepArea->string, indepTileArea ) ) ) {
		retile = qtrue;
		why = "layout settings";
	}
	if ( retile ) {
		Indep_Retile( why );
	}
}


static qboolean Indep_AnyChild( void ) {
	int n;
	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( ic[n].state != IC_FREE ) {
			return qtrue;
		}
	}
	return qfalse;
}


// tell every window to quit, give them INDEP_QUIT_GRACE, kill the rest
static void Indep_CloseChildren( void ) {
	int n, start, left;

	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( Indep_Live( n ) ) {
			Indep_Send( n, "quit 0 0" );
			Indep_Send( n, "quit 0 0" );
		} else if ( ic[n].state == IC_PICKING ) {
			Indep_Free( n );
		}
	}
	start = Sys_Milliseconds();
	do {
		left = 0;
		for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
			if ( !Indep_Live( n ) ) {
				continue;
			}
			if ( !Sys_SplitProcRunning( ic[n].proc ) ) {
				Com_Printf( "indep: P%i window quit (pid %i) after %i ms\n", n + 1, ic[n].pid, Sys_Milliseconds() - start );
				Indep_Free( n );
			} else {
				left++;
			}
		}
		if ( left ) {
			Sys_Sleep( 20 );
		}
	} while ( left && Sys_Milliseconds() - start < INDEP_QUIT_GRACE );

	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( Indep_Live( n ) ) {
			Com_Printf( S_COLOR_YELLOW "indep: P%i window (pid %i) did not quit within %i ms: killed\n", n + 1, ic[n].pid, INDEP_QUIT_GRACE );
			Sys_SplitProcKill( ic[n].proc );
			Indep_Free( n );
		}
	}
}


static void Indep_SetActive( qboolean on ) {
	indepActive = on;
	if ( on ) {
		if ( !Sys_SplitSockOpen() ) {
			// give up the request too (Indep_ModeFrame would retry -- and vid_restart -- every frame)
			Com_Printf( S_COLOR_YELLOW "indep: no loopback socket: Independent mode is off (cl_splitIndependent 0)\n" );
			indepActive = qfalse;
			indepWant = qfalse;
			if ( cl_splitIndependent->integer ) {
				Cvar_Set( "cl_splitIndependent", "0" );
			}
			cl_splitIndependent->modified = qfalse;
			return;
		}
		Indep_Retile( "Independent mode on" );
	} else {
		Sys_SplitSockClose();
		Com_Memset( &indepWin, 0, sizeof( indepWin ) );
	}
}


/*
==================
Indep_ModeFrame

The host page's Session mode row: switches in place (a full vid_restart for
the new window style) once nobody else is playing in the old mode.
==================
*/
static void Indep_ModeFrame( void ) {
	qboolean want;

	if ( cl_splitIndependent->modified ) {
		cl_splitIndependent->modified = qfalse;
		indepWant = cl_splitIndependent->integer ? qtrue : qfalse;
		if ( indepWant && !indepActive && Sys_SplitUnavailable() ) {
			// the archived choice stays (the desktop has it); here the game stays Together
			Com_Printf( "indep: Independent mode is unavailable %s: Together\n", Sys_SplitUnavailable() );
			indepWant = qfalse;
		}
		if ( indepWant != indepActive ) {
			Com_Printf( "indep: session mode %s requested\n", indepWant ? "Independent" : "Together" );
		}
	}
	if ( indepWant == indepActive ) {
		return;
	}
	if ( indepWant && CL_SplitNumViews() > 1 ) {
		return;		// Together players still in: wait until they left
	}
	if ( !indepWant && Indep_AnyChild() ) {
		return;		// windows still open
	}
	if ( cls.state != CA_DISCONNECTED && cls.state != CA_ACTIVE ) {
		return;
	}
	want = indepWant;
	Indep_SetActive( want );
	if ( indepActive != want ) {
		return;		// could not switch (said so once; the request is dropped, no vid_restart)
	}
	Com_Printf( "indep: session mode is now %s\n", indepActive ? "Independent" : "Together" );
	indepApplied = indepWin;
	indepAppliedTiles = indepTiles;
	Cbuf_ExecuteText( EXEC_NOW, "vid_restart\n" );	// new window style
}


/*
=============================================================================

CHILD

=============================================================================
*/

static void Indep_ChildSend( const char *text ) {
	if ( childCoordPort > 0 ) {
		Sys_SplitSockSend( childCoordPort, va( "%s %s", indepToken, text ) );
	}
}


/*
==================
Indep_ChildBeat

The heartbeat: every INDEP_SEND_INTERVAL from the frame and from the loading
screen (SCR_UpdateScreen runs during a map load), at once when the loading
state changes.  'loading' (a level loading, or forced before a vid_restart)
gives the coordinator INDEP_LOAD_TIMEOUT instead of INDEP_HANG_TIMEOUT.
==================
*/
static int		childLastLoading = -1;

static void Indep_ChildBeat( qboolean forceLoading ) {
	const int now = Sys_Milliseconds();
	const int loading = ( forceLoading || ( cls.state >= CA_AUTHORIZING && cls.state <= CA_PRIMED ) ) ? 1 : 0;
	const char *key;

	if ( childLastSent && now - childLastSent < INDEP_SEND_INTERVAL && loading == childLastLoading ) {
		return;
	}
	childLastSent = now ? now : 1;
	childLastLoading = loading;
	key = CL_ProfileSlotKey( 0 );
	Indep_ChildSend( va( "hb %i %i %s %i %i", childPlayer, Sys_SplitPid(), key[0] ? key : "guest", loading,
		IN_PadDeviceId( 0 ) ? 1 : 0 ) );
}


// SCR_UpdateScreen (also the loading screen's refresh): a child keeps beating through a long load
void CL_IndepLoadBeat( void ) {
	if ( indepChild && !childQuitting ) {
		Indep_ChildBeat( qfalse );
	}
}


static void Indep_ChildFrame( void ) {
	char buf[256], tok[24], cmd[16];
	viewRect_t r;
	int port, now;

	now = Sys_Milliseconds();
	while ( Sys_SplitSockRecv( buf, sizeof( buf ), &port ) >= 0 ) {
		if ( port != childCoordPort || sscanf( buf, "%23s %15s", tok, cmd ) != 2 || strcmp( tok, indepToken ) ) {
			continue;
		}
		if ( !Q_stricmp( cmd, "rect" ) ) {
			if ( sscanf( buf, "%*s %*s %i %i %i %i", &r.x, &r.y, &r.w, &r.h ) == 4 && r.w >= 64 && r.h >= 48
				&& !Indep_SameRect( &r, &indepWin ) ) {
				Com_Printf( "indep: coordinator moves this window to %i,%i %ix%i\n", r.x, r.y, r.w, r.h );
				indepWin = r;
			}
		} else if ( !Q_stricmp( cmd, "quit" ) ) {
			if ( !childQuitting ) {
				childQuitting = qtrue;
				Com_Printf( "indep: the coordinator closes this window\n" );
			}
		} else if ( !Q_stricmp( cmd, "focus" ) ) {
			Sys_SplitFocus();
		} else if ( !Q_stricmp( cmd, "pad" ) ) {
			// R19: the keys of this window's pad (one more listing of it, or it is back under a new key)
			const char *newKeys = strstr( buf, " pad " ) ? strstr( buf, " pad " ) + 5 : "";
			if ( newKeys[0] && Q_stricmp( newKeys, childPadKeys ) ) {
				Q_strncpyz( childPadKeys, newKeys, sizeof( childPadKeys ) );
				Com_Printf( "indep: the coordinator says this window's pad is %s\n", childPadKeys );
				IN_PadRescan();
			}
		}
	}

	Indep_ChildBeat( qfalse );

	// the coordinator is gone (crashed, killed): this window goes too
	if ( now - childPidCheck >= 1000 ) {
		childPidCheck = now;
		if ( !childQuitting && childCoordPid > 0 && !Sys_SplitPidRunning( childCoordPid ) ) {
			childQuitting = qtrue;
			Com_Printf( "indep: the coordinator (pid %i) is gone: quitting\n", childCoordPid );
		}
	}

	if ( childQuitting ) {
		Cbuf_ExecuteText( EXEC_NOW, "quit\n" );	// now: a waiting command buffer must not keep the window open
	}
}


static void Indep_ChildInit( void ) {
	const char *profile;

	indepChild = qtrue;
	childPlayer = cl_splitChild->integer;
	if ( sscanf( cl_splitChildIpc->string, "%i %19s %i", &childCoordPort, indepToken, &childCoordPid ) != 3 ) {
		Com_Printf( S_COLOR_YELLOW "indep: child P%i without a coordinator address (cl_splitChildIpc)\n", childPlayer );
		childCoordPort = 0;
	}
	if ( sscanf( cl_splitWindowRectCvar->string, "%i %i %i %i", &indepWin.x, &indepWin.y, &indepWin.w, &indepWin.h ) != 4 ) {
		Com_Memset( &indepWin, 0, sizeof( indepWin ) );
	}
	indepApplied = indepWin;
	indepAppliedTiles = indepTiles;

	// its pad: one or more keys (R19, in_gamepad.c Pad_MakeKey: "<guid>@<path hash>", or
	// "<guid>#<n>" for a device without a path), space separated: the devices of that one pad
	Q_strncpyz( childPadKeys, cl_splitChildPad->string, sizeof( childPadKeys ) );

	Sys_SplitSockOpen();
	Indep_ChildSend( va( "hello %i %i", childPlayer, Sys_SplitPid() ) );

	Com_Printf( "indep: this is P%i's window (pid %i): coordinator pid %i port %i, pad %s, profile %s, rect %i,%i %ix%i\n",
		childPlayer, Sys_SplitPid(), childCoordPid, childCoordPort, childPadKeys[0] ? childPadKeys : "none",
		cl_splitChildProfile->string, indepWin.x, indepWin.y, indepWin.w, indepWin.h );

	// its profile (a child writes only its own profile file)
	profile = cl_splitChildProfile->string;
	if ( profile[0] && Q_stricmp( profile, "guest" ) ) {
		CL_ProfileKeyFor( profile, childStartKey, sizeof( childStartKey ) );
		if ( !CL_ProfileLoad( 0, profile, qtrue ) ) {
			CL_ProfileLoad( 0, NULL, qtrue );
		}
	} else {
		CL_ProfileLoad( 0, NULL, qtrue );
	}

	// a test pad of the coordinator stands for a real one here
	if ( !Q_stricmpn( childPadKeys, "virtual-", 8 ) ) {
		IN_PadAttachVirtual( childPadKeys );
	}
}


/*
=============================================================================

COMMANDS, INIT, FRAME

=============================================================================
*/

static void Indep_Status_f( void ) {
	static const char *names[] = { "free", "picking", "starting", "running" };
	int n;

	if ( indepChild ) {
		Com_Printf( "P%i's window (child of pid %i), pad %s (open: %s), profile %s, rect %i,%i %ix%i\n", childPlayer, childCoordPid,
			childPadKeys, IN_PadDeviceId( 0 ) ? IN_PadDeviceId( 0 ) : "none", CL_ProfileSlotKey( 0 ), indepWin.x, indepWin.y, indepWin.w, indepWin.h );
		return;
	}
	Com_Printf( "Independent mode %s%s (this process: pid %i)\n", indepActive ? "on" : "off", indepWant != indepActive ? " (switch pending)" : "", Sys_SplitPid() );
	if ( !indepActive ) {
		return;
	}
	Com_Printf( "P1: this window %i,%i %ix%i\n", indepWin.x, indepWin.y, indepWin.w, indepWin.h );
	for ( n = 1; n < MAX_SPLITVIEW; n++ ) {
		if ( ic[n].state != IC_FREE ) {
			Com_Printf( "P%i: %s pid %i port %i pad %s profile %s rect %i,%i %ix%i\n", n + 1, names[ ic[n].state ], ic[n].pid,
				ic[n].port, ic[n].padId[0] ? ic[n].padId : "none", ic[n].profileKey[0] ? ic[n].profileKey : "-",
				ic[n].rect.x, ic[n].rect.y, ic[n].rect.w, ic[n].rect.h );
		}
	}
}


// indepfocus <player>: give that window the keyboard and mouse
static void Indep_Focus_f( void ) {
	const int n = atoi( Cmd_Argv( 1 ) ) - 1;

	if ( !indepActive ) {
		return;
	}
	if ( n == 0 ) {
		Sys_SplitFocus();
		return;
	}
	if ( !Indep_Live( n ) ) {
		Com_Printf( "usage: indepfocus <player with a window>\n" );
		return;
	}
	Sys_SplitAllowFocus( ic[n].pid );
	Indep_Send( n, "focus 0 0" );
}


// indepdebug stall <ms> [loading] (developer, in a child window): stop for <ms> without a
// heartbeat -- a hung window, or (loading) one inside a long load -- to test the hang timeout
static void Indep_Debug_f( void ) {
	int ms, start;

	if ( !indepChild || !com_developer || !com_developer->integer || Q_stricmp( Cmd_Argv( 1 ), "stall" ) ) {
		Com_Printf( "usage: indepdebug stall <ms> [loading]  (developer, in a player's window)\n" );
		return;
	}
	ms = atoi( Cmd_Argv( 2 ) );
	if ( !Q_stricmp( Cmd_Argv( 3 ), "loading" ) ) {
		Indep_ChildBeat( qtrue );
	}
	Com_Printf( "indep: debug stall %i ms%s\n", ms, !Q_stricmp( Cmd_Argv( 3 ), "loading" ) ? " (said: loading)" : "" );
	for ( start = Sys_Milliseconds(); Sys_Milliseconds() - start < ms; ) {
		Sys_Sleep( 50 );
	}
	Com_Printf( "indep: debug stall over\n" );
}


void CL_IndepInit( void ) {
	int n;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		Com_Memset( &ic[n], 0, sizeof( ic[n] ) );
		ic[n].proc = -1;
	}
	indepActive = indepChild = qfalse;
	childQuitting = qfalse;
	Com_Memset( &indepWin, 0, sizeof( indepWin ) );
	Com_Memset( &indepApplied, 0, sizeof( indepApplied ) );
	indepTiles = indepAppliedTiles = 0;
	Com_Memset( &indepLogged, 0, sizeof( indepLogged ) );

	cl_splitIndependent = Cvar_Get( "cl_splitIndependent", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitIndependent, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_splitIndependent, "Splitscreen session mode (host setting).\n"
		" 0 - Together: every player shares this window and game\n"
		" 1 - Independent (experimental): a pad that joins gets its own borderless window running its own game (--independent: this run only)" );
	cl_splitIndepArea = Cvar_Get( "cl_splitIndepArea", "", CVAR_TEMP );
	Cvar_SetDescription( cl_splitIndepArea, "Independent mode: screen region \"x y w h\" the windows are tiled in (empty - the whole monitor)." );
	cl_splitChild = Cvar_Get( "cl_splitChild", "0", CVAR_INIT | CVAR_PROTECTED );
	cl_splitChildIpc = Cvar_Get( "cl_splitChildIpc", "", CVAR_INIT | CVAR_PROTECTED );
	cl_splitChildPad = Cvar_Get( "cl_splitChildPad", "", CVAR_INIT | CVAR_PROTECTED );
	cl_splitChildProfile = Cvar_Get( "cl_splitChildProfile", "", CVAR_INIT | CVAR_PROTECTED );
	cl_splitWindowRectCvar = Cvar_Get( "cl_splitWindowRect", "", CVAR_INIT | CVAR_PROTECTED );
	cl_splitChildArgs = Cvar_Get( "cl_splitChildArgs", "", CVAR_TEMP );
	Cvar_SetDescription( cl_splitChildArgs, "Test runs (developer): text added to every spawned window's command line; @@ = its player number." );

	Cmd_AddCommand( "indepstatus", Indep_Status_f );
	Cmd_AddCommand( "indepfocus", Indep_Focus_f );
	Cmd_AddCommand( "indepdebug", Indep_Debug_f );

	if ( ( Sys_SplitLaunchFlags() & SPLIT_FLAG_CHILD ) || cl_splitChild->integer > 1 ) {
		if ( cl_splitChild->integer > 1 && cl_splitChild->integer <= MAX_SPLITVIEW ) {
			Indep_ChildInit();
		} else {
			Com_Printf( S_COLOR_YELLOW "indep: --child without a valid cl_splitChild: a normal window\n" );
		}
		return;
	}

	Com_RandomBytes( (byte *)&n, sizeof( n ) );
	Com_sprintf( indepToken, sizeof( indepToken ), "q3s%08x", (unsigned)n );
	indepWant = ( ( Sys_SplitLaunchFlags() & SPLIT_FLAG_INDEPENDENT ) || cl_splitIndependent->integer ) ? qtrue : qfalse;
	cl_splitIndependent->modified = qfalse;
	if ( indepWant && Sys_SplitUnavailable() ) {
		Com_Printf( "indep: Independent mode%s is unavailable %s: Together\n",
			( Sys_SplitLaunchFlags() & SPLIT_FLAG_INDEPENDENT ) ? " (--independent)" : "", Sys_SplitUnavailable() );
		indepWant = qfalse;
	}
	if ( indepWant ) {
		Indep_SetActive( qtrue );
		indepApplied = indepWin;	// the window is created at this rect
		indepAppliedTiles = indepTiles;
		Com_Printf( "indep: Independent mode%s: this window is player 1's, joining pads get their own\n",
			( Sys_SplitLaunchFlags() & SPLIT_FLAG_INDEPENDENT ) ? " (--independent)" : "" );
	}
}


void CL_IndepShutdown( void ) {
	if ( indepActive ) {
		Indep_CloseChildren();
		Sys_SplitSockClose();
		indepActive = qfalse;
	}
	if ( indepChild ) {
		Indep_ChildSend( va( "bye %i %i", childPlayer, Sys_SplitPid() ) );
		Indep_ChildSend( va( "bye %i %i", childPlayer, Sys_SplitPid() ) );
		Sys_SplitSockClose();
		indepChild = qfalse;
	}
	Cmd_RemoveCommand( "indepstatus" );
	Cmd_RemoveCommand( "indepfocus" );
	Cmd_RemoveCommand( "indepdebug" );
}


void CL_IndepFrame( void ) {
	if ( indepChild ) {
		Indep_ChildFrame();
	} else {
		Indep_ModeFrame();
		if ( !indepActive ) {
			return;
		}
		Indep_CoordinatorFrame();
	}
	Indep_ApplyOwn();
	Indep_LogRect();
}
