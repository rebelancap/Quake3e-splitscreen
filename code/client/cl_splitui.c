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
// cl_splitui.c -- per-player menus (design doc 13.2 / 13.3)
//
// Player 1 keeps the stock ui VM ('uivm', full screen before a game).  In a
// game with several local players its menu is scaled into player 1's cell.
//
// Players 2..8 get their own ui VM instance (VM_UI2..8, always QVM), created
// the first time they open their menu and kept for the rest of the level
// (it lives on the hunk like their cgame; freed with player 1's ui at the
// next level load / vid_restart, or when the player leaves).  It is told its
// cell is the screen, draws into the cell, and gets key/mouse events only
// from that player's pad.  Every call into it runs with that player's
// context active and 'uivm' pointing at it, so the stock CL_UISystemCalls
// handles it; CL_SplitUISyscall below overrides the traps that must be per
// player.  Errors inside it close only that player's menu.

#include "client.h"

#define SPLIT_UI_HUNK_MARGIN		( 4 * 1024 * 1024 )
#define SPLIT_UI_HUNK_DEFAULT_COST	( 12 * 1024 * 1024 )	// until one start was measured

static int	splitUIHunkCost = -1;	// low-hunk bytes of one menu start (-1 unknown)

// arguments of the guarded call (read immediately by the function)
static int		uiCallNum;
static int		uiCallArgs;
static intptr_t	uiCallArg1, uiCallArg2;
static intptr_t	uiCallResult;
static int		uiOpenMenu;


static clientContext_t *CL_SplitUICtx( int n ) {
	if ( n <= 0 || n >= MAX_SPLITVIEW || !clx[ n ] || !clx[ n ]->inUse ) {
		return NULL;
	}
	return clx[ n ];
}


/*
==================
CL_SplitKeyCatcher

Key catchers as the active player sees them: player 1 has the real ones
(keyboard, console, its menu); another player only its own menu and cgame
bits.
==================
*/
int CL_SplitKeyCatcher( void ) {
	if ( cla->playerNum == 0 ) {
		return Key_GetCatcher();
	}
	return cla->uiCatcher | cla->cgameCatcher;
}


qboolean CL_SplitUIMenuOpen( int n ) {
	const clientContext_t *ctx;

	if ( n == 0 ) {
		return ( uivm && ( Key_GetCatcher() & KEYCATCH_UI ) && !( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) ? qtrue : qfalse;
	}
	ctx = CL_SplitUICtx( n );
	return ( ctx && ctx->uiVM && ( ctx->uiCatcher & KEYCATCH_UI ) ) ? qtrue : qfalse;
}


// R14a: player n's menu is a server browser that is refreshing: its ui asked for
// pings / the server count within the last 300 ms (q3_ui's ArenaServers_DoRefresh
// does every frame until the refresh ends; nothing else in the stock menus does)
qboolean CL_SplitUIBrowserRefreshing( int n ) {
	if ( (unsigned)n >= MAX_SPLITVIEW || !clx[ n ] || !CL_SplitUIMenuOpen( n ) ) {
		return qfalse;
	}
	return ( clx[ n ]->uiLanTime && cls.realtime - clx[ n ]->uiLanTime < 300 ) ? qtrue : qfalse;
}


/*
=============================================================================

R14a: "Change player model" -- the stock (q3_ui) player model page in a
player's own menu, reached with the menu's own keys (no screen positions:
custom q3_ui builds keep the item order but not always the layout), and
spotted by its unique art while it is up.

=============================================================================
*/

#define UI_MODEL_PAGE_ART	"menu/art/player_models_ports"

static qhandle_t	uiModelPageShader;	// that art's handle once a ui registered it

// R17 (developer log): where a player's stock menu draws its cursor, and which
// portrait its model page shows first (= which page it is on)
#define UI_CURSOR_ART		"menu/art/3_cursor2"
#define UI_MAX_PORTRAITS	256
static qhandle_t	uiCursorShader;
static struct {
	qhandle_t	h;
	char		name[MAX_QPATH];
} uiPortraits[UI_MAX_PORTRAITS];
static int			uiNumPortraits;
static struct {
	int			frame;				// cls.framecount of the first portrait seen
	char		first[MAX_QPATH];	// the page's first portrait (logged when it changes)
	// R17: where its menu last drew its cursor (its screen pixels) and when
	float		curX, curY;
	int			curFrame;
	// R17: a page turn in progress: the cursor is walked onto the arrow by what the
	// menu draws (custom q3_ui builds scale mouse deltas their own way, not always
	// by the screen's size: at 2560x1440 the maintainer's a51 ui moved 2 screen pixels per
	// unit instead of 3, so an open-loop move ran into the bottom right corner)
	int			turnDir;			// -1 / +1 while turning, 0 = none
	int			turnNext;			// one more press while turning
	int			turnSteps;
	int			turnMoveFrame;		// the frame of the last move (only later draws count)
	int			turnStart;			// cls.realtime
	float		turnGain;			// mouse units per menu unit (learnt)
	float		turnLastX, turnLastY;	// cursor (menu units) before the last move
	int			turnSentX, turnSentY;	// the last move
} uiModelLog[MAX_SPLITVIEW];

qhandle_t CL_SplitUINoteShader( const char *name, qhandle_t h ) {
	if ( !name ) {
		return h;
	}
	if ( !Q_stricmp( name, UI_MODEL_PAGE_ART ) ) {
		uiModelPageShader = h;
	} else if ( !Q_stricmp( name, UI_CURSOR_ART ) ) {
		uiCursorShader = h;
	} else if ( h && !Q_stricmpn( name, "models/players/", 15 ) && strstr( name, "/icon_" ) ) {
		int i;
		for ( i = 0; i < uiNumPortraits && uiPortraits[i].h != h; i++ )
			;
		if ( i == uiNumPortraits && i < UI_MAX_PORTRAITS ) {
			uiPortraits[i].h = h;
			Q_strncpyz( uiPortraits[i].name, name, sizeof( uiPortraits[i].name ) );
			uiNumPortraits++;
		}
	}
	return h;
}


void CL_SplitUINotePic( qhandle_t h, float x, float y ) {
	const int n = cla->playerNum;
	int i;

	if ( !h ) {
		return;
	}
	if ( h == uiModelPageShader ) {
		cla->uiModelPageTime = cls.realtime;
	}
	if ( (unsigned)n >= MAX_SPLITVIEW ) {
		return;
	}
	if ( h == uiCursorShader ) {
		uiModelLog[n].curX = x;
		uiModelLog[n].curY = y;
		uiModelLog[n].curFrame = cls.framecount;
		return;
	}
	if ( !com_developer || !com_developer->integer ) {
		return;
	}
	if ( !cla->uiModelPageTime || cls.realtime - cla->uiModelPageTime > 300 || uiModelLog[n].frame == cls.framecount ) {
		return;
	}
	for ( i = 0; i < uiNumPortraits && uiPortraits[i].h != h; i++ )
		;
	if ( i == uiNumPortraits ) {
		return;
	}
	uiModelLog[n].frame = cls.framecount;
	if ( Q_stricmp( uiModelLog[n].first, uiPortraits[i].name ) ) {
		Q_strncpyz( uiModelLog[n].first, uiPortraits[i].name, sizeof( uiModelLog[n].first ) );
		Com_Printf( "P%i model page: first portrait %s\n", n + 1, uiPortraits[i].name );
	}
}


// player n's menu shows the stock player model page (drawn within the last 300 ms)
qboolean CL_SplitUIModelPage( int n ) {
	if ( (unsigned)n >= MAX_SPLITVIEW || !clx[ n ] || !CL_SplitUIMenuOpen( n ) ) {
		return qfalse;
	}
	return ( clx[ n ]->uiModelPageTime && cls.realtime - clx[ n ]->uiModelPageTime < 300 ) ? qtrue : qfalse;
}


// one key press + release in player n's menu, right now
static void CL_SplitUITap( int n, int key ) {
	if ( n > 0 ) {
		CL_SplitUIKeyEvent( n, key, qtrue );
		CL_SplitUIKeyEvent( n, key, qfalse );
		return;
	}
	if ( !uivm || uivm->callLevel || !CL_SplitUIMenuOpen( 0 ) ) {
		return;
	}
	CL_PushContext( 0 );
	VM_Call( uivm, 2, UI_KEY_EVENT, key, qtrue );
	VM_Call( uivm, 2, UI_KEY_EVENT, key, qfalse );
	CL_PopContext();
}


/*
==================
CL_SplitUIOpenModelPage

Player n's in-game menu -> SETUP -> PLAYER -> MODEL with the arrow and Enter
keys (q3_ui: the cursor starts on the first active item; TEAM is always
active, ADD/REMOVE BOTS only on a server with bot_enable outside single
player, TEAM ORDERS only in team games for a non-spectator; SETUP's first
item is PLAYER; PLAYER's items are name, handicap, effects, model).  Only
for baseq3 (other games' menus are different programs): elsewhere, and if
the page does not show up (CL_SplitUIModelCheck), the game menu stays open.
qfalse: no game menu could be opened.
==================
*/
qboolean CL_SplitUIOpenModelPage( int n, qboolean *exact ) {
	clientContext_t *ctx;
	const char *info;
	int downs, gt, i;

	*exact = qfalse;
	if ( (unsigned)n >= MAX_SPLITVIEW || !clx[ n ] || cls.state != CA_ACTIVE ) {
		return qfalse;
	}
	ctx = clx[ n ];
	if ( n == 0 ) {
		if ( !uivm || uivm->callLevel ) {
			return qfalse;
		}
		CL_PushContext( 0 );
		VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_INGAME );
		CL_PopContext();
	} else {
		CL_SplitUIOpen( n, UIMENU_INGAME );
	}
	if ( !CL_SplitUIMenuOpen( n ) ) {
		return qfalse;
	}
	if ( Q_stricmp( FS_GetCurrentGameDir(), BASEGAME ) ) {
		return qtrue;	// another game's menu: the player goes on from its game menu
	}

	gt = Cvar_VariableIntegerValue( "g_gametype" );
	downs = 1;	// TEAM
	if ( Cvar_VariableIntegerValue( "sv_running" ) && Cvar_VariableIntegerValue( "bot_enable" ) && gt != 2 ) {	// not GT_SINGLE_PLAYER
		downs += 2;	// ADD BOTS, REMOVE BOTS
	}
	if ( gt >= 3 ) {	// GT_TEAM and up
		CL_PushContext( n );
		i = cl.gameState.stringOffsets[ 544 + clc.clientNum ];	// CS_PLAYERS (baseq3)
		info = cl.gameState.stringData + i;
		if ( atoi( Info_ValueForKey( info, "t" ) ) != 3 ) {	// not TEAM_SPECTATOR
			downs++;	// TEAM ORDERS
		}
		CL_PopContext();
	}
	for ( i = 0; i < downs; i++ ) {
		CL_SplitUITap( n, K_DOWNARROW );
	}
	CL_SplitUITap( n, K_ENTER );		// SETUP
	CL_SplitUITap( n, K_ENTER );		// PLAYER
	for ( i = 0; i < 3; i++ ) {
		CL_SplitUITap( n, K_DOWNARROW );	// name -> handicap -> effects -> model
	}
	CL_SplitUITap( n, K_ENTER );		// MODEL
	ctx->uiModelCheck = cls.realtime + 500;
	*exact = qtrue;
	return qtrue;
}


static void CL_SplitUIMove( int n, int dx, int dy );
static void CL_SplitUITurnStep( int n );

// where player n's stock menu last drew its cursor (its screen pixels), while its menu is open
qboolean CL_SplitUICursor( int n, float *x, float *y ) {
	if ( (unsigned)n >= MAX_SPLITVIEW || !uiModelLog[n].curFrame || !CL_SplitUIMenuOpen( n ) ) {
		return qfalse;
	}
	*x = uiModelLog[n].curX;
	*y = uiModelLog[n].curY;
	return qtrue;
}

// once per frame: a model page that was asked for and did not show up -> say so;
// R17: page turns in progress take their next step
void CL_SplitUIModelCheck( void ) {
	int n;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( uiModelLog[n].turnDir ) {
			CL_SplitUITurnStep( n );
		}
		if ( !clx[ n ] || !clx[ n ]->uiModelCheck || cls.realtime < clx[ n ]->uiModelCheck ) {
			continue;
		}
		clx[ n ]->uiModelCheck = 0;
		if ( CL_SplitUIModelPage( n ) ) {
			Com_Printf( "P%i: player model page open (LB / RB turn its pages)\n", n + 1 );
		} else if ( CL_SplitUIMenuOpen( n ) ) {
			Com_Printf( "P%i: this menu has no stock player model page where expected; pick it in the menu\n", n + 1 );
		}
	}
}


// one mouse event in player n's menu, right now
static void CL_SplitUIMove( int n, int dx, int dy ) {
	if ( n > 0 ) {
		CL_SplitUIMouseEvent( n, dx, dy );
		return;
	}
	if ( !uivm || uivm->callLevel || !CL_SplitUIMenuOpen( 0 ) ) {
		return;
	}
	CL_PushContext( 0 );
	VM_Call( uivm, 2, UI_MOUSE_EVENT, dx, dy );
	CL_PopContext();
}


/*
==================
CL_SplitUIModelTurn

LB / RB on the model page: a click on its previous / next page arrow
(q3_ui: 64x32 buttons at 125,340 and 186,340; aimed at 157,356 / 218,356).
R17: the cursor goes to the top left corner first (q3_ui clamps it at
x = -bias, the 4:3 menu's offset on a wide screen in menu units, y = 0),
then is walked onto the arrow over a few frames by where the menu draws
it (CL_SplitUITurnStep): how far a mouse delta moves the cursor is the
menu's own business (q3_ui: one menu unit per unit; the maintainer's a51 ui:
screen pixels with its own scale, non-linear for big moves).  Player 1's
menu at full resolution got the open-loop move wrong and clicked its
bottom right corner; players 2-8 (640x480 menus) were lucky.
==================
*/
#define UI_TURN_MAX_STEPS	8
#define UI_TURN_TIMEOUT		1500	// msec

// player n's menu screen, its 640x480 scale and the 4:3 offset (menu units)
static void CL_SplitUITurnGeometry( int n, int *w, int *h, float *scale, float *bias ) {
	*w = ( n == 0 ) ? cls.glconfig.vidWidth : clx[ n ]->uiWidth;
	*h = ( n == 0 ) ? cls.glconfig.vidHeight : clx[ n ]->uiHeight;
	*scale = 1.0f;
	*bias = 0.0f;
	if ( *w > 0 && *h > 0 ) {
		*scale = *h / 480.0f;
		if ( *w * 480 > *h * 640 ) {
			*bias = 0.5f * ( *w * 480.0f / *h - 640.0f );
		}
	}
}

static void CL_SplitUITurnMove( int n, int dx, int dy ) {
	uiModelLog[n].turnSentX = dx;
	uiModelLog[n].turnSentY = dy;
	uiModelLog[n].turnMoveFrame = cls.framecount;
	uiModelLog[n].turnSteps++;
	CL_SplitUIMove( n, dx, dy );
}

static void CL_SplitUITurnEnd( int n ) {
	const int next = uiModelLog[n].turnNext;

	uiModelLog[n].turnDir = 0;
	uiModelLog[n].turnNext = 0;
	if ( next && CL_SplitUIModelPage( n ) ) {
		CL_SplitUIModelTurn( n, next );		// a press that came while turning
	}
}

// one step of a page turn (once per frame, outside every VM call)
static void CL_SplitUITurnStep( int n ) {
	const int dir = uiModelLog[n].turnDir;
	const float tx = ( dir < 0 ) ? 157.0f : 218.0f, ty = 356.0f;
	float scale, bias, cx, cy, ex, ey, mx, my;
	int w, h, dx, dy;

	if ( !clx[ n ] || !CL_SplitUIMenuOpen( n ) || !uiModelLog[n].turnSteps ) {
		uiModelLog[n].turnDir = uiModelLog[n].turnNext = 0;
		return;
	}
	if ( ( n == 0 && ( !uivm || uivm->callLevel ) ) ) {
		return;
	}
	if ( cls.realtime - uiModelLog[n].turnStart > UI_TURN_TIMEOUT || uiModelLog[n].turnSteps > UI_TURN_MAX_STEPS ) {
		Com_Printf( "P%i model page: could not put the cursor on the %s arrow\n", n + 1, dir < 0 ? "previous" : "next" );
		CL_SplitUITurnEnd( n );
		return;
	}
	if ( uiModelLog[n].curFrame <= uiModelLog[n].turnMoveFrame ) {
		return;		// no draw since the last move yet
	}
	CL_SplitUITurnGeometry( n, &w, &h, &scale, &bias );
	// the cursor art is drawn 32x32 around the cursor (its top left = cursor - 16)
	cx = uiModelLog[n].curX / scale - bias + 16.0f;
	cy = uiModelLog[n].curY / scale + 16.0f;
	ex = tx - cx;
	ey = ty - cy;
	if ( com_developer && com_developer->integer ) {
		Com_Printf( "P%i model page: cursor at %.0f,%.0f (menu units, its screen %ix%i), step %i\n",
			n + 1, cx, cy, w, h, uiModelLog[n].turnSteps );
	}
	if ( fabs( ex ) <= 4.0f && fabs( ey ) <= 4.0f ) {
		CL_SplitUITap( n, K_MOUSE1 );
		Com_Printf( "P%i model page: %s page\n", n + 1, dir < 0 ? "previous" : "next" );
		CL_SplitUITurnEnd( n );
		return;
	}
	// learn how many mouse units move the cursor one menu unit (from the last move,
	// when it was not the corner move and moved far enough to measure)
	if ( uiModelLog[n].turnSteps > 1 ) {
		mx = cx - uiModelLog[n].turnLastX;
		my = cy - uiModelLog[n].turnLastY;
		if ( fabs( mx ) >= 8.0f && uiModelLog[n].turnSentX && fabs( (float)uiModelLog[n].turnSentX / mx ) < 50.0f ) {
			uiModelLog[n].turnGain = (float)uiModelLog[n].turnSentX / mx;
		} else if ( fabs( my ) >= 8.0f && uiModelLog[n].turnSentY && fabs( (float)uiModelLog[n].turnSentY / my ) < 50.0f ) {
			uiModelLog[n].turnGain = (float)uiModelLog[n].turnSentY / my;
		}
	}
	if ( uiModelLog[n].turnGain < 0.05f ) {
		uiModelLog[n].turnGain = scale;
	}
	uiModelLog[n].turnLastX = cx;
	uiModelLog[n].turnLastY = cy;
	dx = (int)( ex * uiModelLog[n].turnGain + ( ex < 0 ? -0.5f : 0.5f ) );
	dy = (int)( ey * uiModelLog[n].turnGain + ( ey < 0 ? -0.5f : 0.5f ) );
	if ( !dx && !dy ) {
		dx = ( ex > 0 ) - ( ex < 0 );
		dy = ( ey > 0 ) - ( ey < 0 );
	}
	CL_SplitUITurnMove( n, dx, dy );
}

void CL_SplitUIModelTurn( int n, int dir ) {
	if ( (unsigned)n >= MAX_SPLITVIEW || !clx[ n ] || !dir ) {
		return;
	}
	if ( uiModelLog[n].turnDir ) {
		uiModelLog[n].turnNext = dir;	// after this one
		return;
	}
	if ( n == 0 && ( !uivm || uivm->callLevel ) ) {
		return;
	}
	uiModelLog[n].turnDir = dir;
	uiModelLog[n].turnNext = 0;
	uiModelLog[n].turnSteps = 0;
	uiModelLog[n].turnStart = cls.realtime;
	uiModelLog[n].turnGain = 0.0f;
	if ( com_developer && com_developer->integer ) {
		int w, h;
		float scale, bias;
		CL_SplitUITurnGeometry( n, &w, &h, &scale, &bias );
		Com_Printf( "P%i model page: %s arrow (menu %i,356 on its %ix%i screen)\n", n + 1, dir < 0 ? "previous" : "next",
			dir < 0 ? 157 : 218, w, h );
	}
	CL_SplitUITurnMove( n, -8000, -8000 );	// the top left corner first
}


// player 1's menu is scaled into its cell (in game with others playing)
qboolean CL_SplitUIInCell( void ) {
	return ( CL_SplitNumViews() > 1 && cla->playerNum == 0 && cls.state == CA_ACTIVE ) ? qtrue : qfalse;
}


/*
=============================================================================

GUARDED CALLS INTO A PLAYER'S MENU

=============================================================================
*/

static void CL_SplitUICallFunc( void ) {
	switch ( uiCallArgs ) {
	case 0: uiCallResult = VM_Call( uivm, 0, uiCallNum ); break;
	case 1: uiCallResult = VM_Call( uivm, 1, uiCallNum, uiCallArg1 ); break;
	default: uiCallResult = VM_Call( uivm, 2, uiCallNum, uiCallArg1, uiCallArg2 ); break;
	}
}


/*
==================
CL_SplitUIRun

Run func in player n's context with 'uivm' = its menu instance.  qfalse if
its menu failed (and was closed/freed).
==================
*/
static vm_t	*uiP1VM;		// player 1's 'uivm' while a player's menu runs
static int	uiRunDepth;

static qboolean CL_SplitUIRun( int n, void (*func)( void ) ) {
	clientContext_t *ctx = CL_SplitUICtx( n );
	vm_t *saved;
	qboolean ok;

	if ( !ctx ) {
		return qfalse;
	}
	saved = uivm;
	if ( uiRunDepth++ == 0 ) {
		uiP1VM = uivm;
	}
	uivm = ctx->uiVM;
	ok = CL_SplitRunUI( n, func );
	if ( --uiRunDepth == 0 ) {
		uivm = uiP1VM;
	} else {
		uivm = saved;
	}
	return ok;
}


/*
==================
CL_SplitUIRestoreP1

An error not caught for a player's menu (ERR_FATAL, or it was not that
player's) unwound past CL_SplitUIRun: give 'uivm' back to player 1 before
the stock error path shuts the ui down.
==================
*/
void CL_SplitUIRestoreP1( void ) {
	if ( uiRunDepth ) {
		uivm = uiP1VM;
		uiRunDepth = 0;
	}
}


static qboolean CL_SplitUICall( int n, int callNum, int args, intptr_t a1, intptr_t a2 ) {
	clientContext_t *ctx = CL_SplitUICtx( n );

	if ( !ctx || !ctx->uiVM || ctx->uiVM->callLevel ) {
		return qfalse;	// never re-enter a menu that is inside a call
	}
	uiCallNum = callNum;
	uiCallArgs = args;
	uiCallArg1 = a1;
	uiCallArg2 = a2;
	uiCallResult = 0;
	return CL_SplitUIRun( n, CL_SplitUICallFunc );
}


/*
==================
CL_SplitUIError

An error inside player n's menu (called by CL_SplitCatchError, back in the
outer context): free that instance without calling into it again.  The
menu stays unavailable until the next level so a broken ui can't leak a
fresh hunk allocation on every try.
==================
*/
void CL_SplitUIError( int n, const char *message ) {
	clientContext_t *ctx = CL_SplitUICtx( n );

	Com_Printf( S_COLOR_YELLOW "P%i's menu failed: %s -- menu closed (available again on the next level); P%i keeps playing\n",
		n + 1, message, n + 1 );
	if ( !ctx ) {
		return;
	}
	if ( ctx->uiVM ) {
		VM_Forced_Unload_Start();
		VM_Free( ctx->uiVM );
		VM_Forced_Unload_Done();
		ctx->uiVM = NULL;
	}
	ctx->uiCatcher = 0;
	ctx->uiFailed = qtrue;
	Com_Memset( ctx->uiKeyDown, 0, sizeof( ctx->uiKeyDown ) );
}


/*
==================
CL_SplitUIFree

Shut player n's menu instance down (it leaves, the level ends, or its
cell size changed).  The hunk part is reclaimed at the next level load.
==================
*/
void CL_SplitUIFree( int n ) {
	clientContext_t *ctx = CL_SplitUICtx( n );

	if ( !ctx ) {
		ctx = ( n > 0 && n < MAX_SPLITVIEW ) ? clx[ n ] : NULL;	// leaving: inUse may be clear already
		if ( !ctx ) {
			return;
		}
	}
	if ( ctx->uiVM ) {
		if ( ctx->inUse && !ctx->uiVM->callLevel ) {
			CL_SplitUICall( n, UI_SHUTDOWN, 0, 0, 0 );	// may fail: then it is freed already
		}
		if ( ctx->uiVM ) {
			if ( ctx->uiVM->callLevel ) {
				VM_Forced_Unload_Start();
				VM_Free( ctx->uiVM );
				VM_Forced_Unload_Done();
			} else {
				VM_Free( ctx->uiVM );
			}
			ctx->uiVM = NULL;
		}
	}
	ctx->uiCatcher = 0;
	ctx->uiWidth = ctx->uiHeight = 0;
	Com_Memset( ctx->uiKeyDown, 0, sizeof( ctx->uiKeyDown ) );
}


// player 1's ui is going down (level change, vid_restart, disconnect)
void CL_SplitShutdownUIs( void ) {
	int i;

	CL_SplitUIRestoreP1();
	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		if ( clx[ i ] ) {
			CL_SplitUIFree( i );
			clx[ i ]->uiFailed = qfalse;	// a new level, a new chance
		}
	}
}


/*
==================
CL_SplitUIOpen

Player n (> 0) opens its menu ('padmenu' / Start): create its ui instance
on first use (or after its cell size changed), then show 'menu'.
==================
*/
static void CL_SplitUICreateFunc( void ) {
	const int n = cla->playerNum;
	int v;

	uivm = CL_SplitCreateUIVM( (vmIndex_t)( VM_UI2 + n - 1 ) );
	cla->uiVM = uivm;
	if ( !uivm ) {
		Com_Printf( "P%i: no menu -- the mod's ui could not be loaded as a QVM (a native-only ui can't run twice)\n", n + 1 );
		return;
	}
	v = VM_Call( uivm, 0, UI_GETAPIVERSION );
	if ( v != UI_API_VERSION && v != 4 /* UI_OLD_API_VERSION */ ) {
		Com_Printf( "P%i: no menu -- ui API version %i\n", n + 1, v );
		VM_Free( uivm );
		cla->uiVM = uivm = NULL;
		return;
	}
	// always in game: a Team Arena-style ui loads only its in-game menus
	VM_Call( uivm, 1, UI_INIT, qtrue );
}


static void CL_SplitUIOpenFunc( void ) {
	VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, uiOpenMenu );
}


/*
==================
CL_SplitUIStart

Make sure player n (> 0, in game) has a ui instance told its current cell
size.  qfalse (with a note unless 'quiet') if it can't have one now.
==================
*/
static qboolean CL_SplitUIStart( int n, qboolean quiet ) {
	clientContext_t *ctx = CL_SplitUICtx( n );
	int w, h, before, cost;

	if ( !ctx || ctx->dropRequested ) {
		return qfalse;
	}
	if ( ( ctx == cla ? cls.state : ctx->clsShadow.state ) != CA_ACTIVE || !ctx->cgameVM ) {
		if ( !quiet ) {
			Com_Printf( "P%i: menu needs P%i in the game\n", n + 1, n + 1 );
		}
		return qfalse;
	}
	if ( ctx->uiFailed ) {
		if ( !quiet ) {
			Com_Printf( "P%i: menu unavailable until the next level (it failed earlier)\n", n + 1 );
		}
		return qfalse;
	}
	if ( ctx->uiVM && ctx->uiVM->callLevel ) {
		return qfalse;
	}

	// it was told another screen size (layout changed since): start it again
	CL_SplitToldSize( n, &w, &h );
	if ( ctx->uiVM && ctx->uiWidth && ( ctx->uiWidth != w || ctx->uiHeight != h ) ) {
		Com_Printf( "P%i: menu screen %ix%i -> %ix%i, restarting its ui\n", n + 1, ctx->uiWidth, ctx->uiHeight, w, h );
		CL_SplitUIFree( n );
	}

	if ( !ctx->uiVM ) {
		const int need = ( splitUIHunkCost >= 0 ) ? splitUIHunkCost : SPLIT_UI_HUNK_DEFAULT_COST;
		if ( Hunk_MemoryRemaining() < need + SPLIT_UI_HUNK_MARGIN ) {
			Com_Printf( S_COLOR_YELLOW "P%i: not enough hunk memory for its menu (%i KB free); load a level to reclaim\n",
				n + 1, Hunk_MemoryRemaining() / 1024 );
			return qfalse;
		}
		before = Hunk_MemoryRemaining();
		if ( !CL_SplitUIRun( n, CL_SplitUICreateFunc ) || !ctx->uiVM ) {
			return qfalse;
		}
		cost = before - Hunk_MemoryRemaining();
		if ( cost > splitUIHunkCost ) {
			splitUIHunkCost = cost;
		}
		Com_Printf( "P%i: menu ui started (%ix%i, %i KB hunk)\n", n + 1, ctx->uiWidth, ctx->uiHeight, cost / 1024 );
	}
	return qtrue;
}


void CL_SplitUIOpen( int n, int menu ) {
	clientContext_t *ctx;

	if ( !CL_SplitUIStart( n, qfalse ) ) {
		return;
	}
	ctx = CL_SplitUICtx( n );

	CL_SplitReleaseInput( n );	// nothing it held in game keeps going
	Com_Memset( ctx->uiKeyDown, 0, sizeof( ctx->uiKeyDown ) );
	uiOpenMenu = menu;
	CL_SplitUIRun( n, CL_SplitUIOpenFunc );
}


void CL_SplitUIClose( int n ) {
	clientContext_t *ctx = CL_SplitUICtx( n );

	if ( !ctx || !ctx->uiVM ) {
		return;
	}
	if ( ctx->uiCatcher & KEYCATCH_UI ) {
		CL_SplitUICall( n, UI_SET_ACTIVE_MENU, 1, UIMENU_NONE, 0 );
	}
	ctx = CL_SplitUICtx( n );
	if ( ctx ) {
		ctx->uiCatcher &= ~KEYCATCH_UI;
		Com_Memset( ctx->uiKeyDown, 0, sizeof( ctx->uiKeyDown ) );
	}
}


// game commands every mod's server handles and no menu claims: an extra
// player typing them must not start a menu instance on the hunk first
static qboolean CL_SplitServerOnlyCommand( const char *cmd ) {
	static const char *serverCmds[] = {
		"say", "say_team", "tell", "vsay", "vsay_team", "vtell", "vosay", "vosay_team", "votell", "vtaunt",
		"kill", "team", "follow", "follownext", "followprev", "callvote", "vote", "callteamvote", "teamvote",
		"give", "god", "noclip", "notarget", "setviewpos", "where", "levelshot", "stats", "score", "userinfo"
	};
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( serverCmds ); i++ ) {
		if ( !Q_stricmp( cmd, serverCmds[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


/*
==================
CL_SplitUIGameCommand

UI_GameCommand for player n > 0 (active context): a console command no
engine/cgame code claimed is offered to that player's own menu, as player
1's go to its ui -- e.g. Urban Terror's cgame runs 'ui_selectteam' (and its
binds 'ui_selectgear') to pop the mod's team and gear menus.
A player without a menu instance yet gets one (hunk permitting).
==================
*/
qboolean CL_SplitUIGameCommand( void ) {
	const int n = cla->playerNum;
	clientContext_t *ctx = CL_SplitUICtx( n );
	char line[MAX_STRING_CHARS];
	qboolean wasOpen;

	if ( !ctx || ctx != cla || ctx->dropRequested || ctx->uiFailed || cls.state != CA_ACTIVE || !cgvm ) {
		return qfalse;
	}
	if ( ctx->uiVM && ctx->uiVM->callLevel ) {
		return qfalse;	// its menu is running this command itself
	}
	if ( CL_SplitServerOnlyCommand( Cmd_Argv( 0 ) ) ) {
		return qfalse;	// chat, team, kill, votes, cheats: straight to the server, no menu instance
	}
	// start it (or restart it for a changed cell size); that may tokenize other text.
	// Which commands a menu claims is known only by asking it (UrT's cgame even
	// registers 'ui_selectteam' for tab completion), so any unclaimed command
	// starts the menu instance once -- a player's menu is needed in any mod
	// whose team/gear selection is in its ui.
	Q_strncpyz( line, Cmd_Cmd(), sizeof( line ) );
	if ( !CL_SplitUIStart( n, qtrue ) ) {
		return qfalse;
	}
	Cmd_TokenizeString( line );

	wasOpen = ( ctx->uiCatcher & KEYCATCH_UI ) ? qtrue : qfalse;
	if ( !CL_SplitUICall( n, UI_CONSOLE_COMMAND, 1, cls.realtime, 0 ) ) {
		return qtrue;	// its menu failed (closed); the command was its
	}
	ctx = CL_SplitUICtx( n );
	if ( ctx && !wasOpen && ( ctx->uiCatcher & KEYCATCH_UI ) ) {
		// the command opened a menu: nothing it held in game keeps going
		CL_SplitReleaseInput( n );
		Com_Memset( ctx->uiKeyDown, 0, sizeof( ctx->uiKeyDown ) );
	}
	return uiCallResult ? qtrue : qfalse;
}


void CL_SplitUIKeyEvent( int n, int key, qboolean down ) {
	clientContext_t *ctx = CL_SplitUICtx( n );

	if ( !ctx ) {
		return;
	}
	if ( key & K_CHAR_FLAG ) {
		// a typed character (on-screen keyboard), like CL_CharEvent's for player 1
		Com_DPrintf( "P%i menu char %i\n", n + 1, key & ~K_CHAR_FLAG );
		if ( down && CL_SplitUIMenuOpen( n ) ) {
			CL_SplitUICall( n, UI_KEY_EVENT, 2, key, qtrue );
		}
		return;
	}
	if ( (unsigned)key >= MAX_KEYS ) {
		return;
	}
	ctx->uiKeyDown[ key ] = down ? 1 : 0;
	Com_DPrintf( "P%i menu key %s %s\n", n + 1, Key_KeynumToString( key ), down ? "down" : "up" );
	if ( CL_SplitUIMenuOpen( n ) ) {
		CL_SplitUICall( n, UI_KEY_EVENT, 2, key, down );
	}
}


void CL_SplitUIMouseEvent( int n, int dx, int dy ) {
	if ( CL_SplitUIMenuOpen( n ) && ( dx || dy ) ) {
		CL_SplitUICall( n, UI_MOUSE_EVENT, 2, dx, dy );
	}
}


// from the screen loop, right after player n's cgame drew its view
void CL_SplitUIRefresh( int n ) {
	if ( n > 0 && CL_SplitUIMenuOpen( n ) ) {
		CL_SplitUICall( n, UI_REFRESH, 1, cls.realtime, 0 );
	}
}


void CL_SplitUIInfo( int n, char *buf, int size ) {
	const clientContext_t *ctx = ( n > 0 && n < MAX_SPLITVIEW ) ? clx[ n ] : NULL;

	if ( n == 0 ) {
		Q_strncpyz( buf, CL_SplitUIMenuOpen( 0 ) ? "open" : "-", size );
	} else if ( !ctx ) {
		Q_strncpyz( buf, "-", size );
	} else if ( ctx->uiFailed ) {
		Q_strncpyz( buf, "failed", size );
	} else if ( !ctx->uiVM ) {
		Q_strncpyz( buf, "-", size );
	} else {
		Com_sprintf( buf, size, "%s(%ix%i)", ( ctx->uiCatcher & KEYCATCH_UI ) ? "open" : "loaded", ctx->uiWidth, ctx->uiHeight );
	}
}


/*
=============================================================================

COMMANDS A PLAYER'S MENU ISSUES

Each one runs as that player ('p<N> cmd'): 'cmd team red' goes over its own
connection, 'disconnect' drops only it, userinfo 'set's go to its p<N>_
shadow.  Commands that control the host / session are refused.

=============================================================================
*/

static const char *splitUIRefused[] = {
	"quit", "map", "devmap", "spmap", "spdevmap", "map_restart", "killserver", "nextmap",
	"vid_restart", "snd_restart", "in_restart", "fs_restart", "game_restart", "cvar_restart",
	"connect", "reconnect", "demo", "record", "stoprecord", "cinematic", "exec", "execq", "vstr",
	"kick", "kicknum", "kickall", "kickbots", "clientkick", "banuser", "banclient", "addbot",
	"rcon", "bind", "unbind", "unbindall", "writeconfig", "addplayer", "dropplayer",
	"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "padinject", "pbind", "punbind",
	"setenv", "reset", "toggle", "cvarAdd", "cvar_modified", "unset",
	"banaddr", "bandel", "exceptaddr", "exceptdel", "flushbans", "listbans", "heartbeat",
	"serverrecord", "serverstop", "net_restart", "minimize", "splitsettings", "splitdebug", "splitappend", NULL
};


/*
==================
CL_SplitUICvarPolicy

What a write to cvar 'name' from player n's menu (trap or command) does:
SPLIT_CVAR_SHADOW - a userinfo cvar: goes to that player's p<N>_ shadow
SPLIT_CVAR_SHARED - a menu/HUD preference (ui_* / cg_*, not protected): allowed
SPLIT_CVAR_IGNORE - everything else (cl_paused, sv_*, fs_game, ...)
==================
*/
enum { SPLIT_CVAR_IGNORE, SPLIT_CVAR_SHADOW, SPLIT_CVAR_SHARED };

static int CL_SplitUICvarPolicy( const char *name ) {
	unsigned flags;

	if ( !name || !name[0] ) {
		return SPLIT_CVAR_IGNORE;
	}
	if ( CL_SplitShadowed( name, 0 ) ) {
		return SPLIT_CVAR_SHADOW;
	}
	if ( Q_stricmpn( name, "ui_", 3 ) && Q_stricmpn( name, "cg_", 3 ) ) {
		return SPLIT_CVAR_IGNORE;
	}
	flags = Cvar_Flags( name );
	if ( flags != CVAR_NONEXISTENT && ( flags & ( CVAR_ROM | CVAR_INIT | CVAR_LATCH | CVAR_SERVERINFO
			| CVAR_SYSTEMINFO | CVAR_CHEAT | CVAR_PROTECTED | CVAR_PRIVATE ) ) ) {
		return SPLIT_CVAR_IGNORE;
	}
	return SPLIT_CVAR_SHARED;
}


static char	splitUIExec[MAX_STRING_CHARS * 2];	// the commands of one ExecuteText, in order
static int	splitUIExecLen;

static void CL_SplitUIQueue( const char *line ) {
	splitUIExecLen += Com_sprintf( splitUIExec + splitUIExecLen, sizeof( splitUIExec ) - splitUIExecLen, "%s", line );
}


static void CL_SplitUIOneCommand( int n, const char *cmd ) {
	char token[MAX_TOKEN_CHARS];
	char name[MAX_CVAR_VALUE_STRING];
	const char *p = cmd, *rest;
	int i;

	Q_strncpyz( token, COM_ParseExt( &p, qfalse ), sizeof( token ) );
	if ( !token[0] ) {
		return;
	}
	for ( i = 0; splitUIRefused[i]; i++ ) {
		if ( !Q_stricmp( token, splitUIRefused[i] ) ) {
			Com_Printf( "P%i's menu: '%s' is for player 1 only, ignored\n", n + 1, token );
			return;
		}
	}

	// cvar writes: set/seta/sets/setu <cvar> <value>, or a bare '<cvar> <value>'
	// (an existing cvar name is treated as a cvar even if a command shares it)
	if ( !Q_stricmp( token, "set" ) || !Q_stricmp( token, "seta" ) || !Q_stricmp( token, "sets" ) || !Q_stricmp( token, "setu" ) ) {
		Q_strncpyz( name, COM_ParseExt( &p, qfalse ), sizeof( name ) );
	} else if ( Cvar_Flags( token ) != CVAR_NONEXISTENT ) {
		Q_strncpyz( name, token, sizeof( name ) );
	} else {
		name[0] = '\0';
	}
	if ( name[0] ) {
		rest = p;
		while ( *rest == ' ' || *rest == '\t' ) {
			rest++;
		}
		switch ( CL_SplitUICvarPolicy( name ) ) {
		case SPLIT_CVAR_SHADOW:
			CL_SplitUIQueue( va( "p%i %s %s\n", n + 1, name, rest ) );
			return;
		case SPLIT_CVAR_SHARED:
			if ( !*rest ) {
				return;	// a read: nothing to do for a menu
			}
			CL_SplitUIQueue( va( "set %s %s\n", name, rest ) );
			return;
		default:
			Com_DPrintf( "P%i's menu: write to '%s' ignored\n", n + 1, name );
			return;
		}
	}

	CL_SplitUIQueue( va( "p%i %s\n", n + 1, cmd ) );
}


static void CL_SplitUIExecute( int n, const char *text ) {
	char seg[MAX_STRING_CHARS];
	int len = 0;
	qboolean quote = qfalse;
	const char *p;

	if ( !text ) {
		return;
	}
	Com_DPrintf( "P%i's menu runs: %s\n", n + 1, text );
	// the command buffer treats /* */ as a comment spanning lines, which would
	// let text after it run as player 1: refuse such text outright
	if ( strstr( text, "/*" ) || strstr( text, "*/" ) ) {
		Com_Printf( "P%i's menu: command text with /* */ ignored\n", n + 1 );
		return;
	}
	splitUIExec[0] = '\0';
	splitUIExecLen = 0;
	for ( p = text; ; p++ ) {
		if ( *p == '"' ) {
			quote = !quote;
		}
		// a line break ends a command even inside quotes (Cbuf_Execute does too)
		if ( *p == '\0' || *p == '\n' || *p == '\r' || ( !quote && *p == ';' ) ) {
			seg[len] = '\0';
			CL_SplitUIOneCommand( n, seg );
			len = 0;
			if ( *p != ';' ) {
				quote = qfalse;
			}
			if ( *p == '\0' ) {
				break;
			}
			continue;
		}
		if ( len < (int)sizeof( seg ) - 1 ) {
			seg[len++] = *p;
		}
	}

	// run right after the command that is executing now (a pad press from
	// the command buffer, or the next buffer run): the player's menu acts
	// on its own frame even when a script keeps the buffer busy
	if ( splitUIExecLen > 0 ) {
		splitUIExec[ splitUIExecLen - 1 ] = '\0';	// Cbuf_InsertText adds the \n
		Cbuf_InsertText( splitUIExec );
	}
}


/*
=============================================================================

TRAP LAYER

=============================================================================
*/

static void *CL_SplitUIVMA( intptr_t v ) {
	if ( !v || !uivm ) {
		return NULL;
	}
	if ( uivm->entryPoint ) {
		return (void *)v;
	}
	return (void *)( uivm->dataBase + ( v & uivm->dataMask ) );
}
#define UVMA(x) CL_SplitUIVMA( args[x] )


static int CL_SplitFloatAsInt( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}


// player 1's full-screen menu fitted (aspect kept) and centered in its cell
static void CL_SplitUIP1Fit( viewRect_t *r, float *s, float *ox, float *oy ) {
	const float W = cls.glconfig.vidWidth, H = cls.glconfig.vidHeight;

	CL_SplitViewRect( 0, r );
	*s = MIN( r->w / W, r->h / H );
	*ox = r->x + ( r->w - W * *s ) * 0.5f;
	*oy = r->y + ( r->h - H * *s ) * 0.5f;
}


static void CL_SplitUIP1Pic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t shader ) {
	viewRect_t r;
	float s, ox, oy, cut;

	CL_SplitUIP1Fit( &r, &s, &ox, &oy );
	x = ox + x * s;
	y = oy + y * s;
	w *= s;
	h *= s;
	if ( w <= 0.0f || h <= 0.0f ) {
		return;
	}
	if ( x < r.x ) {
		cut = ( r.x - x ) / w; s1 += ( s2 - s1 ) * cut; w -= r.x - x; x = r.x;
	}
	if ( x + w > r.x + r.w ) {
		cut = ( x + w - r.x - r.w ) / w; s2 -= ( s2 - s1 ) * cut; w = r.x + r.w - x;
	}
	if ( y < r.y ) {
		cut = ( r.y - y ) / h; t1 += ( t2 - t1 ) * cut; h -= r.y - y; y = r.y;
	}
	if ( y + h > r.y + r.h ) {
		cut = ( y + h - r.y - r.h ) / h; t2 -= ( t2 - t1 ) * cut; h = r.y + r.h - y;
	}
	if ( w <= 0.0f || h <= 0.0f ) {
		return;
	}
	re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, shader );
}


static void CL_SplitUIP1Scene( const refdef_t *fd ) {
	refdef_t rd;
	viewRect_t r;
	float s, ox, oy;
	int x0, y0, x1, y1;

	CL_SplitUIP1Fit( &r, &s, &ox, &oy );
	x0 = MAX( (int)( ox + fd->x * s ), r.x );
	y0 = MAX( (int)( oy + fd->y * s ), r.y );
	x1 = MIN( (int)( ox + ( fd->x + fd->width ) * s ), r.x + r.w );
	y1 = MIN( (int)( oy + ( fd->y + fd->height ) * s ), r.y + r.h );
	if ( x1 <= x0 || y1 <= y0 ) {
		return;
	}
	rd = *fd;
	rd.x = x0;
	rd.y = y0;
	rd.width = x1 - x0;
	rd.height = y1 - y0;
	re.RenderScene( &rd );
}


/*
==================
CL_SplitUISyscall

Consulted by CL_UISystemCalls while several players exist or a player > 1's
menu is calling.  Returns qtrue if the trap was handled here (*ret set).
==================
*/
qboolean CL_SplitUISyscall( intptr_t *args, intptr_t *ret ) {
	const int n = cla->playerNum;
	const char *name;
	cvar_t *shadow;

	*ret = 0;

	if ( n == 0 ) {
		if ( !CL_SplitUIInCell() ) {
			return qfalse;
		}
		switch ( args[0] ) {
		case UI_R_DRAWSTRETCHPIC:
			CL_SplitUIP1Pic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
			return qtrue;
		case UI_R_RENDERSCENE:
			CL_SplitUIP1Scene( UVMA(1) );
			return qtrue;
		default:
			return qfalse;
		}
	}

	switch ( args[0] ) {
	case UI_PRINT:
		if ( com_developer && com_developer->integer ) {
			Com_Printf( "[P%i menu] %s", n + 1, (const char *)UVMA(1) );
		}
		return qtrue;

	// the cell (4:3-fitted with cl_splitAspect, like its cgame) is the screen
	case UI_GETGLCONFIG: {
		glconfig_t *gl = UVMA(1);
		int w, h;
		VM_CHECKBOUNDS( uivm, args[1], sizeof( glconfig_t ) );
		*gl = *re.GetConfig();
		CL_SplitToldSize( n, &w, &h );
		gl->vidWidth = w;
		gl->vidHeight = h;
		gl->windowAspect = (float)w / (float)h;
		cla->uiWidth = w;
		cla->uiHeight = h;
		return qtrue;
	}
	case UI_R_DRAWSTRETCHPIC:
		if ( cla->debugError == 3 ) {
			cla->debugError = 0;
			Com_Error( ERR_DROP, "splitdebug: forced error inside P%i's menu", n + 1 );
		}
		CL_SplitDrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return qtrue;
	case UI_R_RENDERSCENE:	// model previews: same placement as its cgame's HUD models
		CL_SplitRenderScene( UVMA(1) );
		return qtrue;
	case UI_UPDATESCREEN:
		return qtrue;

	case UI_CMD_EXECUTETEXT:
		CL_SplitUIExecute( n, UVMA(2) );
		return qtrue;

	// userinfo cvars (name, model, team_model, UrT gear ...) are its p<N>_ shadows
	case UI_CVAR_REGISTER:
		name = UVMA(2);
		if ( name && CL_SplitShadowed( name, args[4] ) ) {
			Cvar_Register( NULL, name, UVMA(3), args[4], uivm->privateFlag );
			shadow = CL_SplitShadow( n, name, UVMA(3) );
			Cvar_Register( UVMA(1), shadow->name, shadow->resetString, 0, uivm->privateFlag );
			return qtrue;
		}
		return qfalse;
	case UI_CVAR_CREATE:
		name = UVMA(1);
		if ( name && CL_SplitShadowed( name, args[3] ) ) {
			Cvar_Register( NULL, name, UVMA(2), args[3], uivm->privateFlag );
			CL_SplitShadow( n, name, UVMA(2) );
			return qtrue;
		}
		return qfalse;
	case UI_CVAR_SET:
		name = UVMA(1);
		switch ( CL_SplitUICvarPolicy( name ) ) {
		case SPLIT_CVAR_SHADOW:
			Cvar_SetSafe( CL_SplitShadow( n, name, NULL )->name, UVMA(2) );
			return qtrue;
		case SPLIT_CVAR_SHARED:
			return qfalse;
		default:
			return qtrue;	// never cl_paused, server or engine cvars
		}
	case UI_CVAR_SETVALUE:
		name = UVMA(1);
		switch ( CL_SplitUICvarPolicy( name ) ) {
		case SPLIT_CVAR_SHADOW:
			Cvar_SetValueSafe( CL_SplitShadow( n, name, NULL )->name, VMF(2) );
			return qtrue;
		case SPLIT_CVAR_SHARED:
			return qfalse;
		default:
			return qtrue;
		}
	case UI_CVAR_RESET:
		name = UVMA(1);
		switch ( CL_SplitUICvarPolicy( name ) ) {
		case SPLIT_CVAR_SHADOW:
			Cvar_Reset( CL_SplitShadow( n, name, NULL )->name );
			return qtrue;
		case SPLIT_CVAR_SHARED:
			return qfalse;
		default:
			return qtrue;
		}
	case UI_CVAR_VARIABLEVALUE:
		name = UVMA(1);
		if ( name && CL_SplitShadowed( name, 0 ) ) {
			*ret = CL_SplitFloatAsInt( CL_SplitShadow( n, name, NULL )->value );
			return qtrue;
		}
		return qfalse;
	case UI_CVAR_VARIABLESTRINGBUFFER:
		name = UVMA(1);
		if ( name && CL_SplitShadowed( name, 0 ) ) {
			VM_CHECKBOUNDS( uivm, args[2], args[3] );
			Cvar_VariableStringBufferSafe( CL_SplitShadow( n, name, NULL )->name, UVMA(2), args[3], CVAR_PRIVATE );
			return qtrue;
		}
		return qfalse;
	case UI_CVAR_INFOSTRINGBUFFER:
		if ( args[1] & CVAR_USERINFO ) {
			VM_CHECKBOUNDS( uivm, args[2], args[3] );
			Q_strncpyz( UVMA(2), CL_SplitBuildUserinfo( n, NULL ), args[3] );
			return qtrue;
		}
		return qfalse;

	// keys: its own catcher and pad-held keys; the keyboard binds are player 1's
	case UI_KEY_GETCATCHER:
		*ret = CL_SplitKeyCatcher();
		return qtrue;
	case UI_KEY_SETCATCHER:
		cla->uiCatcher = args[1] & KEYCATCH_UI;
		return qtrue;
	case UI_KEY_ISDOWN:
		*ret = ( (unsigned)args[1] < MAX_KEYS ) ? cla->uiKeyDown[ args[1] ] : 0;
		return qtrue;
	case UI_KEY_CLEARSTATES:
		Com_Memset( cla->uiKeyDown, 0, sizeof( cla->uiKeyDown ) );
		return qtrue;
	case UI_KEY_GETBINDINGBUF:
		VM_CHECKBOUNDS( uivm, args[2], args[3] );
		if ( args[3] > 0 ) {
			*(char *)UVMA(2) = '\0';
		}
		return qtrue;
	case UI_KEY_SETBINDING:
		return qtrue;

	// music and cinematics belong to player 1's screen
	case UI_S_STARTBACKGROUNDTRACK:
	case UI_S_STOPBACKGROUNDTRACK:
	case UI_CIN_STOPCINEMATIC:
	case UI_CIN_DRAWCINEMATIC:
	case UI_CIN_SETEXTENTS:
	case UI_CIN_RUNCINEMATIC:
		return qtrue;
	case UI_CIN_PLAYCINEMATIC:
		*ret = -1;
		return qtrue;
	case UI_SET_CDKEY:
		return qtrue;

	default:
		return qfalse;
	}
}
