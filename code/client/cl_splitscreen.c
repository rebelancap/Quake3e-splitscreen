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
// cl_splitscreen.c -- per-local-player client contexts (see cl_splitscreen.h)
//
// Local splitscreen: player 1 is context 0 (static, always present, owns the
// UI, console, downloads and demos).  'addplayer' creates players 2..N as
// complete extra clients of the same server (own socket, qport, cgame VM)
// drawn into their own viewport cell.  With only player 1 every hook below
// is pass-through and the engine behaves exactly like upstream.

#include <setjmp.h>
#include "client.h"

extern cvar_t	*cl_timeout;	// cl_main.c

// context 0 is static so a single-player session never allocates anything
static clientContext_t	cl_context0 = { .inUse = qtrue, .playerNum = 0 };

clientContext_t	*clx[MAX_SPLITVIEW] = { &cl_context0 };
clientContext_t	*cla = &cl_context0;

int				cl_splitInPlaceInit;
int				cl_splitTraps;

#define CTX_STACK_DEPTH ( MAX_SPLITVIEW * 2 )
static int		ctxStack[CTX_STACK_DEPTH];
static int		ctxStackDepth;

// number of local players (= viewport cells in use); 1 = pass-through
static int		splitNumViews = 1;
static int		splitJoinCounter;
static int		splitCgameHunkCost = -1;	// low-hunk bytes of one in-place cgame start (-1 unknown)

static cvar_t	*cl_splitAspect;
static cvar_t	*cl_splitOverlayBars;	// R18: full-height overlays continue over the 4:3 bars
static cvar_t	*cl_splitFill;
static cvar_t	*cl_splitVertical;
static cvar_t	*cl_splitWidePlayer;
static cvar_t	*cl_splitMaxPlayers;
static cvar_t	*cl_splitTestOnDisconnect;


/*
==================
CL_SaveClsShadow / CL_LoadClsShadow

Move the per-connection fields of cls in and out of a context.
==================
*/
static void CL_SaveClsShadow( clsShadow_t *s ) {
	s->state = cls.state;
	s->gameSwitch = cls.gameSwitch;
	s->cgameStarted = cls.cgameStarted;
	s->startCgame = cls.startCgame;
}

static void CL_LoadClsShadow( const clsShadow_t *s ) {
	cls.state = s->state;
	cls.gameSwitch = s->gameSwitch;
	cls.cgameStarted = s->cgameStarted;
	cls.startCgame = s->startCgame;
}


/*
==================
CL_SwitchTo

Make 'next' the active context (no validity checks).
==================
*/
static void CL_SwitchTo( clientContext_t *next ) {
	if ( next == cla ) {
		return;
	}
	CL_SaveClsShadow( &cla->clsShadow );
	cla = next;
	CL_LoadClsShadow( &cla->clsShadow );
}


/*
==================
CL_SetContext

Make context 'ctxNum' the one that cl / clc / cgvm / cls.state resolve to.
==================
*/
void CL_SetContext( int ctxNum ) {
	if ( (unsigned)ctxNum >= MAX_SPLITVIEW || !clx[ ctxNum ] || !clx[ ctxNum ]->inUse ) {
		Com_Error( ERR_FATAL, "CL_SetContext: bad context %i", ctxNum );
	}
	CL_SwitchTo( clx[ ctxNum ] );
}


/*
==================
CL_PushContext / CL_PopContext

Switch to a context and later restore the previous one.  A context that
was dropped in between is still allocated (contexts are only freed by
CL_SplitReap at a safe point), so popping back to it is harmless; it is
replaced by context 0 to keep the dead player from running anything.
==================
*/
void CL_PushContext( int ctxNum ) {
	if ( ctxStackDepth >= CTX_STACK_DEPTH ) {
		Com_Error( ERR_FATAL, "CL_PushContext: overflow" );
	}
	ctxStack[ ctxStackDepth++ ] = cla->playerNum;
	CL_SetContext( ctxNum );
}

void CL_PopContext( void ) {
	clientContext_t *prev;

	if ( ctxStackDepth <= 0 ) {
		Com_Error( ERR_FATAL, "CL_PopContext: underflow" );
	}
	prev = clx[ ctxStack[ --ctxStackDepth ] ];
	if ( !prev || !prev->inUse ) {
		prev = clx[ 0 ];
	}
	CL_SwitchTo( prev );
}


static connstate_t CL_ContextState( const clientContext_t *ctx ) {
	return ( ctx == cla ) ? cls.state : ctx->clsShadow.state;
}


static qboolean CL_SlotInUse( int n ) {
	return ( (unsigned)n < MAX_SPLITVIEW && clx[ n ] && clx[ n ]->inUse ) ? qtrue : qfalse;
}


qboolean CL_SplitSlotActive( int n ) {
	return ( CL_SlotInUse( n ) && !clx[ n ]->dropRequested ) ? qtrue : qfalse;
}


/*
==================
CL_SplitReleaseInput

A player lost its input device: no button it held stays down.  Player 1's
keyboard keys are its own and are left alone (the pad's own releases were
already sent as button-up commands).
==================
*/
void CL_SplitReleaseInput( int n ) {
	qboolean mlooking;

	if ( n <= 0 || !CL_SlotInUse( n ) ) {
		return;
	}
	mlooking = clx[ n ]->in.mlooking;
	Com_Memset( &clx[ n ]->in, 0, sizeof( clx[ n ]->in ) );
	clx[ n ]->in.mlooking = mlooking;
}


/*
==================
CL_ContextQport

qport for the active context's netchan.  Player 0 keeps the stock
net_qport cvar; extra players need distinct qports so the server can
tell their channels apart.
==================
*/
int CL_ContextQport( void ) {
	if ( cla->playerNum == 0 ) {
		return Cvar_VariableIntegerValue( "net_qport" );
	}
	return cla->qport;
}


/*
=============================================================================

ERROR ISOLATION

Work done on behalf of an extra player (its packets, its cgame, its
connection upkeep) runs through CL_SplitRun, which arms a jump target.
Com_Error calls CL_SplitCatchError first: an ERR_DROP-class error raised
while that player is the active context drops only that player and
resumes after the CL_SplitRun call instead of tearing down the client.

Not isolated (stock whole-client error): ERR_FATAL, errors while player 1
is the active context, and errors from engine/server commands typed with a
'p<N>' prefix (only the cgame's console commands are guarded).

=============================================================================
*/

typedef struct splitGuard_s {
	jmp_buf					env;
	int						ctxNum;
	int						stackDepth;
	clientContext_t			*outer;
	int						inPlaceInit;
	qboolean				uiCall;		// guarding a call into n's menu: an error closes only that
	struct splitGuard_s		*prev;
} splitGuard_t;

static splitGuard_t	*splitGuard;

typedef void ( *splitFunc_t )( void );

static void CL_DropContext( int n, const char *reason );

/*
==================
CL_SplitRun

Run 'func' in context n, isolating its errors.  Returns qfalse if the
player was dropped by an error.
==================
*/
static qboolean CL_SplitRunEx( int n, splitFunc_t func, qboolean uiCall ) {
	splitGuard_t g;

	g.ctxNum = n;
	g.uiCall = uiCall;
	g.stackDepth = ctxStackDepth;
	g.outer = cla;
	g.inPlaceInit = cl_splitInPlaceInit;
	g.prev = splitGuard;

	if ( Q_setjmp( g.env ) ) {
		// CL_SplitCatchError restored the context stack and dropped player n
		splitGuard = g.prev;
		return qfalse;
	}

	splitGuard = &g;
	CL_PushContext( n );
	func();
	CL_PopContext();
	splitGuard = g.prev;
	return qtrue;
}

static qboolean CL_SplitRun( int n, splitFunc_t func ) {
	return CL_SplitRunEx( n, func, qfalse );
}

// a call into player n's own menu: an error closes that menu, the player stays
qboolean CL_SplitRunUI( int n, void (*func)( void ) ) {
	return CL_SplitRunEx( n, func, qtrue );
}


/*
==================
CL_SplitCatchError

Called by Com_Error before anything is torn down.  Returns only if the
error is not an extra player's; otherwise drops that player and jumps
back to its CL_SplitRun.
==================
*/
void CL_SplitCatchError( int code, const char *message ) {
	splitGuard_t *g = splitGuard;
	int n;

	if ( !g || ( code != ERR_DROP && code != ERR_SERVERDISCONNECT && code != ERR_DISCONNECT ) ) {
		return;
	}
	n = g->ctxNum;
	if ( n <= 0 || cla->playerNum != n || !CL_SlotInUse( n ) ) {
		return;
	}

	if ( !g->uiCall ) {
		Com_Printf( "P%i error: %s -- dropping only player %i\n", n + 1, message, n + 1 );
	}

	splitGuard = g->prev;
	ctxStackDepth = g->stackDepth;
	cl_splitInPlaceInit = g->inPlaceInit;
	CL_SwitchTo( ( g->outer && g->outer->inUse ) ? g->outer : clx[ 0 ] );

	// a second Com_Error during the drop (packet write, CG_SHUTDOWN) must take
	// the stock whole-client ERR_DROP path, not the "recursive error" Sys_Error
	com_errorEntered = qfalse;

	if ( g->uiCall ) {
		CL_SplitUIError( n, message );	// its menu only
	} else {
		VM_Forced_Unload_Start();	// its cgame may be stuck inside a call
		CL_DropContext( n, va( "error: %s", message ) );
		VM_Forced_Unload_Done();
	}

	Q_longjmp( g->env, 1 );
}


/*
=============================================================================

VIEWPORT LAYOUT (design doc 3.2)

=============================================================================
*/

static viewRect_t	splitRects[MAX_SPLITVIEW];		// per player slot
static viewRect_t	splitPreview[MAX_SPLITVIEW];	// the same with held (joining) slots counted
static viewRect_t	splitEmpty[MAX_SPLITVIEW];		// black cells
static int			splitNumEmpty;
static qboolean		splitLayoutDirty = qtrue;
static int			splitLayoutW, splitLayoutH;


/*
==================
CL_LayoutRows

Cells per row for n players.  Returns the number of rows.  The total may
exceed n in grid mode (fill 0); the remaining cells stay black.
==================
*/
static int CL_LayoutRows( int n, qboolean fill, qboolean vertical, int *rows ) {
	switch ( n ) {
	case 0:
	case 1:	rows[0] = 1; return 1;
	case 2:
		if ( vertical ) {
			rows[0] = 2; return 1;
		}
		rows[0] = 1; rows[1] = 1; return 2;
	case 3:	rows[0] = 2; rows[1] = fill ? 1 : 2; return 2;
	case 4:	rows[0] = 2; rows[1] = 2; return 2;
	case 5:	rows[0] = 3; rows[1] = fill ? 2 : 3; return 2;
	case 6:	rows[0] = 3; rows[1] = 3; return 2;
	case 7:
		if ( fill ) {
			rows[0] = 4; rows[1] = 3; return 2;
		}
		rows[0] = 3; rows[1] = 3; rows[2] = 3; return 3;
	default:
		if ( fill ) {
			rows[0] = 4; rows[1] = 4; return 2;
		}
		rows[0] = 3; rows[1] = 3; rows[2] = 3; return 3;
	}
}


/*
==================
CL_ComputeLayout

Viewport cells for 'n' players whose slot numbers are 'slots[]' (in slot
order).  rects[] is indexed by slot.  In fill mode the bottom row may hold
fewer, wider cells: 'wideSlot' (or -1) takes the first of them and the
others follow in slot order.  Unused cells (grid mode) go to empty[].
==================
*/
static void CL_ComputeLayout( int n, const int *slots, int wideSlot, int width, int height,
		qboolean fill, qboolean vertical, viewRect_t *rects, viewRect_t *empty, int *numEmpty ) {
	int order[MAX_SPLITVIEW];
	int rows[4];
	int numRows, r, c, cell, i, j, widePos;
	viewRect_t *rc;

	numRows = CL_LayoutRows( n, fill, vertical, rows );

	for ( i = 0; i < n; i++ ) {
		order[i] = slots[i];
	}

	// the wide (bottom) row gets the priority player first
	if ( numRows > 1 && rows[numRows-1] < rows[0] && wideSlot >= 0 ) {
		widePos = n - rows[numRows-1];
		for ( i = 0; i < n && order[i] != wideSlot; i++ )
			;
		if ( i < n ) {
			if ( i < widePos ) {
				for ( j = i; j < widePos; j++ ) {
					order[j] = order[j+1];
				}
			} else {
				for ( j = i; j > widePos; j-- ) {
					order[j] = order[j-1];
				}
			}
			order[widePos] = wideSlot;
		}
	}

	*numEmpty = 0;
	cell = 0;
	for ( r = 0; r < numRows; r++ ) {
		for ( c = 0; c < rows[r]; c++, cell++ ) {
			rc = ( cell < n ) ? &rects[ order[cell] ] : &empty[ (*numEmpty)++ ];
			rc->x = c * width / rows[r];
			rc->w = ( c + 1 ) * width / rows[r] - rc->x;
			rc->y = r * height / numRows;
			rc->h = ( r + 1 ) * height / numRows - rc->y;
		}
	}
}


// Independent mode (cl_splitindep.c): the same cells for windows on the screen
void CL_SplitTileLayout( int n, const int *slots, int wideSlot, int width, int height, viewRect_t *rects ) {
	viewRect_t empty[MAX_SPLITVIEW];
	int numEmpty;

	CL_ComputeLayout( n, slots, wideSlot, width, height,
		cl_splitFill ? cl_splitFill->integer : 1, cl_splitVertical ? cl_splitVertical->integer : 0,
		rects, empty, &numEmpty );
}


/*
==================
CL_SplitUpdateLayout

Recompute every player's cell from the current players, window size and
layout cvars.
==================
*/
static int CL_SplitLayoutFor( qboolean withHeld, viewRect_t *rects, viewRect_t *empty, int *numEmpty ) {
	int slots[MAX_SPLITVIEW];
	int i, n, wide, lastJoin;

	wide = -1;
	lastJoin = 0;
	for ( i = 0, n = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( !CL_SlotInUse( i ) || ( clx[i]->joinHeld && !withHeld ) ) {
			continue;
		}
		slots[n++] = i;
		if ( i > 0 && clx[i]->joinSeq > lastJoin ) {
			lastJoin = clx[i]->joinSeq;
			wide = i;	// default: last joined
		}
	}

	if ( cl_splitWidePlayer && cl_splitWidePlayer->integer > 0 && CL_SlotInUse( cl_splitWidePlayer->integer - 1 )
		&& ( withHeld || !clx[ cl_splitWidePlayer->integer - 1 ]->joinHeld ) ) {
		wide = cl_splitWidePlayer->integer - 1;
	}

	Com_Memset( rects, 0, sizeof( viewRect_t ) * MAX_SPLITVIEW );
	CL_ComputeLayout( n, slots, wide, splitLayoutW, splitLayoutH,
		cl_splitFill ? cl_splitFill->integer : 1, cl_splitVertical ? cl_splitVertical->integer : 0,
		rects, empty, numEmpty );
	return n;
}

static void CL_SplitUpdateLayout( void ) {
	viewRect_t previewEmpty[MAX_SPLITVIEW];
	int previewNumEmpty;

	splitLayoutDirty = qfalse;
	splitLayoutW = cls.glconfig.vidWidth;
	splitLayoutH = cls.glconfig.vidHeight;

	// the players in game; a slot held for the join-time profile picker does
	// not change anybody's view until it actually connects
	splitNumViews = CL_SplitLayoutFor( qfalse, splitRects, splitEmpty, &splitNumEmpty );
	cl_splitTraps = ( splitNumViews > 1 ) ? 1 : 0;

	// held slots: the cell they will get, where their picker is drawn (over the current views)
	CL_SplitLayoutFor( qtrue, splitPreview, previewEmpty, &previewNumEmpty );
}


/*
==================
CL_SplitViewRect

Viewport cell of a context in real framebuffer pixels.
==================
*/
void CL_SplitViewRect( int ctxNum, viewRect_t *rect ) {
	if ( splitLayoutDirty || splitLayoutW != cls.glconfig.vidWidth || splitLayoutH != cls.glconfig.vidHeight ) {
		CL_SplitUpdateLayout();
	}

	if ( CL_SlotInUse( ctxNum ) && clx[ ctxNum ]->joinHeld && splitPreview[ ctxNum ].w > 0 ) {
		*rect = splitPreview[ ctxNum ];
		return;
	}
	if ( splitNumViews <= 1 || (unsigned)ctxNum >= MAX_SPLITVIEW || splitRects[ ctxNum ].w <= 0 ) {
		rect->x = 0; rect->y = 0;
		rect->w = cls.glconfig.vidWidth; rect->h = cls.glconfig.vidHeight;
		return;
	}
	*rect = splitRects[ ctxNum ];
}


// the first unused (black) grid cell, if the layout has one
qboolean CL_SplitEmptyCell( viewRect_t *rect ) {
	if ( splitNumViews <= 1 || splitNumEmpty <= 0 ) {
		return qfalse;
	}
	*rect = splitEmpty[ 0 ];
	return qtrue;
}


/*
=============================================================================

CGAME TRAP LAYER

Each cgame believes the screen is exactly its viewport; the traps below
move its drawing into the cell and clip it there.  All of them are
pass-through while there is only one player.

=============================================================================
*/

/*
==================
CL_SplitScreenSize

The screen size a cgame in viewport 'r' is told.  cl_splitAspect 0: the
cell itself.  cl_splitAspect 1: a 4:3 screen fitted into the cell; its 2D
is centered in the cell and its world view is widened to the whole cell
with the vertical (or horizontal, for tall cells) FOV kept.  Q3-era mods
derive fov_y from a fixed fov_x and stretch a 640x480 HUD over the
screen, so a 32:9 or 8:9 cell is unusable for them without this.
==================
*/
// cl_splitAspect 1: every cell's cgame is told this one 4:3 size and its
// drawing is scaled to the 4:3 area fitted into its cell, so a layout change
// (3 -> 5 players, fill, wide player, window size) restarts no cgame
#define SPLIT_TOLD_W	640
#define SPLIT_TOLD_H	480

static void CL_SplitScreenSize( const viewRect_t *r, int *w, int *h ) {
	if ( !cl_splitAspect || !cl_splitAspect->integer ) {
		*w = r->w;
		*h = r->h;
	} else {
		*w = SPLIT_TOLD_W;
		*h = SPLIT_TOLD_H;
	}
}


/*
==================
CL_SplitFitted

The area of cell 'r' the cgame's screen covers (cl_splitAspect 1: a 4:3
area centered in the cell; 0: the cell) as an offset from the cell's corner
and a scale from the cgame's screen pixels.
==================
*/
static void CL_SplitFitted( const viewRect_t *r, float *ox, float *oy, float *scale ) {
	int fw, fh;

	if ( !cl_splitAspect || !cl_splitAspect->integer ) {
		*ox = *oy = 0.0f;
		*scale = 1.0f;
		return;
	}
	if ( r->w * 3 > r->h * 4 ) {
		fw = r->h * 4 / 3;
		fh = r->h;
	} else {
		fw = r->w;
		fh = r->w * 3 / 4;
	}
	*ox = ( r->w - fw ) / 2;
	*oy = ( r->h - fh ) / 2;
	*scale = (float)fw / SPLIT_TOLD_W;
}


int CL_SplitNumViews( void ) {
	return splitNumViews;
}


// the cgame screen is a cell (told size, FOV widening, 2D placement): several views, or
// R20: an Independent-mode tile window (the whole window is the cell, like Together)
static qboolean CL_SplitCells( void ) {
	return ( splitNumViews > 1 || CL_IndepTiled() ) ? qtrue : qfalse;
}


void CL_SplitToldSize( int ctxNum, int *w, int *h ) {
	viewRect_t r;

	CL_SplitViewRect( ctxNum, &r );
	if ( !CL_SplitCells() ) {
		*w = cls.glconfig.vidWidth;
		*h = cls.glconfig.vidHeight;
		return;
	}
	CL_SplitScreenSize( &r, w, h );
}


/*
==================
CL_SplitGlconfig

CG_GETGLCONFIG: report the viewport as the screen.
==================
*/
void CL_SplitGlconfig( glconfig_t *glconfig ) {
	int w, h;

	CL_SplitToldSize( cla->playerNum, &w, &h );
	if ( CL_SplitCells() ) {
		glconfig->vidWidth = w;
		glconfig->vidHeight = h;
		glconfig->windowAspect = (float)w / (float)h;
		if ( splitNumViews <= 1 ) {
			Com_Printf( "window: tile %ix%i is a cell: cgame screen %ix%i (HUD shape %s)\n", cls.glconfig.vidWidth, cls.glconfig.vidHeight,
				w, h, cl_splitAspect->integer ? "4:3 centered" : "stretched" );
		}
	}

	// remember what this cgame was told, see CL_SplitCheckLayout
	cla->cgameWidth = glconfig->vidWidth;
	cla->cgameHeight = glconfig->vidHeight;
}


/*
==================
CL_SplitRenderScene

CG_R_RENDERSCENE: refdef x/y/width/height are in the cgame's screen
pixels.  The world view is scaled to the cell (FOV widened to match);
model-only scenes (HUD heads, RDF_NOWORLDMODEL) move with the 2D.
==================
*/
void CL_SplitRenderScene( const refdef_t *fd ) {
	refdef_t rd;
	viewRect_t r;
	int sw, sh, x0, y0, x1, y1;
	float sx, sy, ox, oy, scale;

	if ( !CL_SplitCells() ) {
		re.RenderScene( fd );
		return;
	}

	CL_SplitViewRect( cla->playerNum, &r );
	CL_SplitScreenSize( &r, &sw, &sh );

	if ( fd->rdflags & RDF_NOWORLDMODEL ) {
		CL_SplitFitted( &r, &ox, &oy, &scale );
		sx = sy = scale;
		ox += r.x;
		oy += r.y;
	} else {
		sx = (float)r.w / sw;
		sy = (float)r.h / sh;
		ox = r.x;
		oy = r.y;
	}

	x0 = MAX( (int)( ox + fd->x * sx ), r.x );
	y0 = MAX( (int)( oy + fd->y * sy ), r.y );
	x1 = MIN( (int)( ox + ( fd->x + fd->width ) * sx ), r.x + r.w );
	y1 = MIN( (int)( oy + ( fd->y + fd->height ) * sy ), r.y + r.h );
	if ( x1 <= x0 || y1 <= y0 ) {
		return;
	}

	rd = *fd;
	rd.x = x0;
	rd.y = y0;
	rd.width = x1 - x0;
	rd.height = y1 - y0;

	if ( sx > sy ) {
		// wider than the cgame thinks: keep fov_y, widen fov_x
		rd.fov_x = atanf( tanf( DEG2RAD( fd->fov_y * 0.5f ) ) * rd.width / rd.height ) * (float)( 360.0 / M_PI );
	} else if ( sy > sx ) {
		rd.fov_y = atanf( tanf( DEG2RAD( fd->fov_x * 0.5f ) ) * rd.height / rd.width ) * (float)( 360.0 / M_PI );
	}

	re.RenderScene( &rd );
}


/*
==================
CL_SplitDrawStretchPic

CG_R_DRAWSTRETCHPIC: offset into the cell and clip to it (texture
coordinates are cut proportionally), so a HUD element drawn past the
cgame's believed screen edge cannot spill into a neighbouring view.

R18: with cl_splitAspect 1 the cgame's 4:3 screen leaves bars in a wide (or
tall) cell where the world shows.  A pic that spans the screen's full
height and touches its left / right edge (full width + top / bottom for a
tall cell) -- a sniper scope, a flash, a fade -- is continued over the bar
with its own edge texels, as a full-screen overlay would cover a real
screen of that shape (UrT's scope: black around the lens, not the zoomed
world beside a 4:3 square).
==================
*/
static void CL_SplitDrawClipped( const viewRect_t *r, float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	float cut;

	if ( w <= 0.0f || h <= 0.0f ) {
		return;
	}
	// clip to the cell
	if ( x < 0.0f ) {
		cut = -x / w;
		s1 += ( s2 - s1 ) * cut;
		w += x;
		x = 0.0f;
	}
	if ( x + w > r->w ) {
		cut = ( x + w - r->w ) / w;
		s2 -= ( s2 - s1 ) * cut;
		w = r->w - x;
	}
	if ( y < 0.0f ) {
		cut = -y / h;
		t1 += ( t2 - t1 ) * cut;
		h += y;
		y = 0.0f;
	}
	if ( y + h > r->h ) {
		cut = ( y + h - r->h ) / h;
		t2 -= ( t2 - t1 ) * cut;
		h = r->h - y;
	}
	if ( w <= 0.0f || h <= 0.0f ) {
		return;
	}

	re.DrawStretchPic( x + r->x, y + r->y, w, h, s1, t1, s2, t2, hShader );
}


#define SPLIT_EDGE_EPS	0.5f		// cgame pixels: "touches the edge"
#define SPLIT_EDGE_TEX	0.002f		// share of the texture span sampled for an edge column / row

void CL_SplitDrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	viewRect_t r;
	float ox, oy, scale, sx, sy, sw, sh, e;

	if ( !CL_SplitCells() ) {
		re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
		return;
	}

	if ( cla->debugError == 1 ) {
		cla->debugError = 0;
		Com_Error( ERR_DROP, "splitdebug: forced error inside P%i's cgame", cla->playerNum + 1 );
	}

	if ( w <= 0.0f || h <= 0.0f ) {
		return;
	}

	// the cgame's screen is scaled into the 4:3 area centered in the cell (cl_splitAspect)
	CL_SplitViewRect( cla->playerNum, &r );
	CL_SplitFitted( &r, &ox, &oy, &scale );
	sx = x * scale + ox;
	sy = y * scale + oy;
	sw = w * scale;
	sh = h * scale;

	// R18: full-height (full-width) overlays continue over the cell's bars
	if ( !cl_splitOverlayBars->integer ) {
		;
	} else if ( ox >= 1.0f && y <= SPLIT_EDGE_EPS && y + h >= SPLIT_TOLD_H - SPLIT_EDGE_EPS ) {
		e = ( s2 - s1 ) * SPLIT_EDGE_TEX;
		if ( x <= SPLIT_EDGE_EPS && sx > 0.0f ) {
			CL_SplitDrawClipped( &r, 0.0f, sy, sx, sh, s1, t1, s1 + e, t2, hShader );
		}
		if ( x + w >= SPLIT_TOLD_W - SPLIT_EDGE_EPS && sx + sw < r.w ) {
			CL_SplitDrawClipped( &r, sx + sw, sy, r.w - ( sx + sw ), sh, s2 - e, t1, s2, t2, hShader );
		}
	} else if ( oy >= 1.0f && x <= SPLIT_EDGE_EPS && x + w >= SPLIT_TOLD_W - SPLIT_EDGE_EPS ) {
		e = ( t2 - t1 ) * SPLIT_EDGE_TEX;
		if ( y <= SPLIT_EDGE_EPS && sy > 0.0f ) {
			CL_SplitDrawClipped( &r, sx, 0.0f, sw, sy, s1, t1, s2, t1 + e, hShader );
		}
		if ( y + h >= SPLIT_TOLD_H - SPLIT_EDGE_EPS && sy + sh < r.h ) {
			CL_SplitDrawClipped( &r, sx, sy + sh, sw, r.h - ( sy + sh ), s1, t2 - e, s2, t2, hShader );
		}
	}

	CL_SplitDrawClipped( &r, sx, sy, sw, sh, s1, t1, s2, t2, hShader );
}


/*
==================
CL_SplitCinRect

CG_CIN_*: cinematic extents are virtual 640x480 coordinates that
CIN_DrawCinematic scales to the whole window (SCR_AdjustFrom640).  Map the
cgame's 640x480 onto its screen in the active player's cell, clamp to the
cell (a raw image can't be cut), and express that in the window's 640x480.
==================
*/
static void CL_SplitCinRect( int *x, int *y, int *w, int *h ) {
	viewRect_t r;
	int sw, sh;
	float x0, y0, x1, y1, ox, oy, scale;

	CL_SplitViewRect( cla->playerNum, &r );
	CL_SplitScreenSize( &r, &sw, &sh );
	CL_SplitFitted( &r, &ox, &oy, &scale );
	x0 = r.x + ox + *x * sw * scale / 640.0f;
	y0 = r.y + oy + *y * sh * scale / 480.0f;
	x1 = x0 + *w * sw * scale / 640.0f;
	y1 = y0 + *h * sh * scale / 480.0f;
	x0 = MAX( x0, r.x );
	y0 = MAX( y0, r.y );
	x1 = MIN( x1, r.x + r.w );
	y1 = MIN( y1, r.y + r.h );
	if ( x1 <= x0 || y1 <= y0 || cls.scale <= 0.0f ) {
		*x = *y = *w = *h = 0;
		return;
	}
	*x = (int)( ( x0 - cls.biasX ) / cls.scale + 0.5f );
	*y = (int)( ( y0 - cls.biasY ) / cls.scale + 0.5f );
	*w = (int)( ( x1 - x0 ) / cls.scale + 0.5f );
	*h = (int)( ( y1 - y0 ) / cls.scale + 0.5f );
}


/*
=============================================================================

PER-PLAYER USERINFO AND CVAR SHADOWS (design doc 5.2, 13.1, 13.3)

Player N>1 sends the userinfo cvars with each value taken from its
'p<N>_<cvar>' shadow (created on demand: name "Player N", anything else
the cvar's default).  The shadows are ordinary in-memory cvars (not
archived; profiles come later).  A few connection-level keys stay shared.

=============================================================================
*/

static const char *splitSharedKeys[] = {
	"password", "rate", "snaps", "cl_anonymous", "cl_guid", "cl_voip", "cl_voipProtocol", NULL
};

static qboolean CL_SplitSharedKey( const char *key ) {
	int i;
	for ( i = 0; splitSharedKeys[i]; i++ ) {
		if ( !Q_stricmp( key, splitSharedKeys[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


// a cvar whose reads/writes by players > 1 go to their p<N>_ shadow
qboolean CL_SplitShadowed( const char *name, int extraFlags ) {
	const unsigned flags = Cvar_Flags( name );

	if ( !name || !name[0] || CL_SplitSharedKey( name ) ) {
		return qfalse;
	}
	if ( IN_PadPlayerCvar( name ) ) {
		return qtrue;	// the game's per-player menu state (default_pad.cfg 'playercvar')
	}
	if ( !Q_stricmp( name, "cg_fov" ) ) {
		return qtrue;	// R14a: each player's field of view (Controls page, profiles)
	}
	if ( flags == CVAR_NONEXISTENT ) {
		return ( ( extraFlags & CVAR_USERINFO ) && !( extraFlags & CVAR_SERVERINFO ) ) ? qtrue : qfalse;
	}
	// a userinfo cvar the server sets (Urban Terror's g_gametype, Team Arena's
	// g_redteam) is the same for every player: shared, and not part of profiles
	if ( ( flags | extraFlags ) & CVAR_SERVERINFO ) {
		return qfalse;
	}
	return ( ( flags | extraFlags ) & CVAR_USERINFO ) ? qtrue : qfalse;
}


static const char *CL_SplitShadowName( int n, const char *name ) {
	static char buf[MAX_CVAR_VALUE_STRING];
	Com_sprintf( buf, sizeof( buf ), "p%i_%s", n + 1, name );
	return buf;
}


cvar_t *CL_SplitShadow( int n, const char *name, const char *defaultValue ) {
	char sname[MAX_CVAR_VALUE_STRING];
	const char *def;

	Q_strncpyz( sname, CL_SplitShadowName( n, name ), sizeof( sname ) );
	if ( !Q_stricmp( name, "name" ) ) {
		def = va( "Player %i", n + 1 );
	} else {
		def = Cvar_DefaultString( name );
		if ( !def ) {
			def = defaultValue ? defaultValue : "";
		}
	}
	return Cvar_Get( sname, def, 0 );
}


/*
==================
CL_SplitBuildUserinfo
==================
*/
const char *CL_SplitBuildUserinfo( int n, qboolean *truncated ) {
	static char info[MAX_INFO_STRING];
	static char key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	const char *s, *v;
	cvar_t *shadow;

	s = Cvar_InfoString( CVAR_USERINFO, truncated );
	if ( n == 0 ) {
		return s;
	}

	info[0] = '\0';
	while ( *s ) {
		s = Info_NextPair( s, key, value );
		if ( !key[0] ) {
			break;
		}
		if ( !Q_stricmp( key, "cl_guid" ) ) {
			// a distinct, stable guid per local player
			v = value[0] ? Com_MD5Buf( value, (int)strlen( value ), va( "p%i", n + 1 ), 2 ) : "";
		} else if ( CL_SplitSharedKey( key ) || !CL_SplitShadowed( key, CVAR_USERINFO ) ) {
			v = value;		// connection keys and server-set values are player 1's
		} else {
			shadow = CL_SplitShadow( n, key, value );
			v = shadow->string;
		}
		if ( !Info_SetValueForKey( info, key, v ) && truncated ) {
			*truncated = qtrue;
		}
	}
	return info;
}


/*
==================
CL_SplitUserinfo

Userinfo of the active context, used for its connect request.
==================
*/
const char *CL_SplitUserinfo( qboolean *truncated ) {
	const char *info = CL_SplitSrvUserinfo( cla->playerNum, CL_AimAssistUserinfo( cla->playerNum, CL_SplitBuildUserinfo( cla->playerNum, truncated ) ) );

	if ( cla->playerNum != 0 ) {
		Q_strncpyz( cla->lastUserinfo, info, sizeof( cla->lastUserinfo ) );
	}
	return info;
}


/*
==================
CL_SplitSkipUserinfo

Player 1's userinfo update (CL_CheckUserinfo): while several players exist,
cgames restart in place and re-register / briefly toggle userinfo cvars
(baseq3's teamoverlay), which would resend an unchanged userinfo each time
and quick restarts run into the server's flood protection.  qtrue = same
as the last one sent, skip it.  With one player: never skips.
==================
*/
static char	splitP1Userinfo[MAX_INFO_STRING];

qboolean CL_SplitSkipUserinfo( const char *info ) {
	if ( cl_splitTraps && !strcmp( info, splitP1Userinfo ) ) {
		return qtrue;
	}
	Q_strncpyz( splitP1Userinfo, info, sizeof( splitP1Userinfo ) );
	return qfalse;
}


/*
==================
CL_SplitCheckUserinfo

Extra players: resend the userinfo when it changed (a p<N>_ shadow or a
shared value).  Polled; the shadows carry no userinfo flag.
==================
*/
static void CL_SplitCheckUserinfo( void ) {
	const char *info;

	if ( cls.state < CA_CONNECTED || CL_CheckPaused() || cls.realtime < cla->userinfoCheckTime ) {
		return;
	}
	cla->userinfoCheckTime = cls.realtime + 250;

	info = CL_SplitSrvUserinfo( cla->playerNum, CL_AimAssistUserinfo( cla->playerNum, CL_SplitBuildUserinfo( cla->playerNum, NULL ) ) );
	if ( strcmp( info, cla->lastUserinfo ) ) {
		Q_strncpyz( cla->lastUserinfo, info, sizeof( cla->lastUserinfo ) );
		CL_AddReliableCommand( va( "userinfo \"%s\"", info ), qfalse );
	}
}


/*
=============================================================================

SYSCALLS OF CGAMES WHEN SEVERAL PLAYERS EXIST

=============================================================================
*/

// sound: player 1's listener is applied after every cgame has drawn
static struct {
	qboolean	valid;
	int			entityNum;
	vec3_t		origin;
	vec3_t		axis[3];
	int			inwater;
} splitListener0;

// local/world sounds every cgame starts for the same event are played once
typedef struct {
	int		time;
	int		sfx;
	int		entityNum;
	int		channel;
	int		ctxNum;
} splitSound_t;

#define SPLIT_SOUND_HISTORY	64
#define SPLIT_SOUND_WINDOW	50		// msec
static splitSound_t	splitSounds[SPLIT_SOUND_HISTORY];
static int			splitSoundHead;


static qboolean CL_SplitDuplicateSound( int sfx, int entityNum, int channel, const char *what ) {
	const int now = Sys_Milliseconds();
	splitSound_t *s;
	int i;

	for ( i = 0, s = splitSounds; i < SPLIT_SOUND_HISTORY; i++, s++ ) {
		if ( s->sfx == sfx && s->entityNum == entityNum && s->channel == channel
			&& s->ctxNum != cla->playerNum && now - s->time < SPLIT_SOUND_WINDOW && s->time ) {
			if ( Cvar_VariableIntegerValue( "s_show" ) ) {
				Com_Printf( "split: P%i %s sfx %i ent %i already started by P%i, played once\n",
					cla->playerNum + 1, what, sfx, entityNum, s->ctxNum + 1 );
			}
			return qtrue;
		}
	}

	s = &splitSounds[ splitSoundHead++ % SPLIT_SOUND_HISTORY ];
	s->time = now ? now : 1;
	s->sfx = sfx;
	s->entityNum = entityNum;
	s->channel = channel;
	s->ctxNum = cla->playerNum;
	return qfalse;
}


static void *CL_SplitVMA( intptr_t v ) {
	if ( !v || !cgvm ) {
		return NULL;
	}
	if ( cgvm->entryPoint ) {
		return (void *)v;
	}
	return (void *)( cgvm->dataBase + ( v & cgvm->dataMask ) );
}
#define SVMA(x) CL_SplitVMA( args[x] )


/*
==================
CL_SplitNestedAdd

An extra player's cgame queued console commands (trap_SendConsoleCommand).
They go to that player's own queue, run at its next frame as that player
('p<N> <cmd>': its buttons, cvars, cgame and menu commands go to it) --
not into the shared command buffer, where a 'wait' (a script, a bind)
would pile them up: UrT's cgame queues "-button13;-button14;" every frame,
and several players' copies overflowed the buffer within seconds.
==================
*/
static void CL_SplitNestedAdd( int n, const char *text ) {
	char *out = cla->cgameCmds;
	char seg[MAX_STRING_CHARS];
	int olen, slen, len;
	qboolean quote;
	const char *p;

	if ( text[0] != '+' && text[0] != '-' ) {	// UrT's cgame sends "-button13;-button14;" every frame
		Com_DPrintf( "P%i's cgame runs: %s\n", n + 1, text );
	}
	olen = cla->cgameCmdsLen;
	slen = 0;
	quote = qfalse;

	for ( p = text; ; p++ ) {
		if ( *p == '"' ) {
			quote = !quote;
		}
		if ( *p == '\0' || ( !quote && ( *p == ';' || *p == '\n' || *p == '\r' ) ) ) {
			seg[slen] = '\0';
			while ( slen > 0 && seg[slen-1] == ' ' ) {
				seg[--slen] = '\0';
			}
			if ( slen > 0 ) {
				const char *s = seg;
				while ( *s == ' ' || *s == '\t' ) {
					s++;
				}
				len = (int)strlen( s );
				if ( len > 0 && olen + len + 1 < (int)sizeof( cla->cgameCmds ) ) {
					Com_Memcpy( out + olen, s, len );
					out[ olen + len ] = '\n';
					olen += len + 1;
				} else if ( len > 0 ) {
					Com_DPrintf( S_COLOR_YELLOW "P%i: cgame command queue full, '%s' dropped\n", n + 1, s );
				}
			}
			slen = 0;
			if ( *p == '\0' ) {
				break;
			}
			continue;
		}
		if ( slen < (int)sizeof( seg ) - 1 ) {
			// one command per queue line: a line break inside quotes becomes a space
			seg[slen++] = ( *p == '\n' || *p == '\r' ) ? ' ' : *p;
		}
	}

	cla->cgameCmdsLen = olen;
}


/*
==================
CL_SplitRunCgameCmds

Run what player n's cgame queued since its last frame, as that player.
Called in player 1's context (each line is 'p<N> <cmd>', so 'disconnect'
drops only that player, like a player's menu commands).
==================
*/
static void CL_SplitRunCgameCmds( int n ) {
	char cmds[ sizeof( clx[0]->cgameCmds ) ];
	char *line, *next;
	int len;

	if ( !CL_SlotInUse( n ) || ( len = clx[ n ]->cgameCmdsLen ) <= 0 ) {
		return;
	}
	Com_Memcpy( cmds, clx[ n ]->cgameCmds, len );
	cmds[ len ] = '\0';
	clx[ n ]->cgameCmdsLen = 0;		// what these commands queue runs next frame

	for ( line = cmds; *line; line = next ) {
		next = strchr( line, '\n' );
		if ( next ) {
			*next++ = '\0';
		} else {
			next = line + strlen( line );
		}
		if ( !CL_SlotInUse( n ) || clx[ n ]->dropRequested ) {
			break;
		}
		Cmd_ExecuteString( va( "p%i %s", n + 1, line ) );
	}
}


/*
==================
CL_SplitSyscall

Consulted by CL_CgameSystemCalls while cl_splitTraps is set (several
players, or a cgame restarting in place).  Returns qtrue if the trap was
handled here, with its return value in *ret.
==================
*/
qboolean CL_SplitSyscall( intptr_t *args, intptr_t *ret ) {
	const int n = cla->playerNum;
	const char *name;

	*ret = 0;

	switch ( args[0] ) {
	// the level is already loaded by player 1 (the renderer ERR_DROPs on a
	// redundant world load; the collision map is shared)
	case CG_CM_LOADMAP:
	case CG_R_LOADWORLDMAP:
		return ( n || cl_splitInPlaceInit ) ? qtrue : qfalse;

	// the loading screen of an extra player is not drawn (it would
	// re-enter SCR_UpdateScreen from inside that player's CG_INIT), nor
	// that of a cgame restarting in place on a loaded level (no flash)
	case CG_UPDATESCREEN:
		return ( n || cl_splitInPlaceInit ) ? qtrue : qfalse;

	// obituaries, chat etc. are printed by every cgame: only player 1's reach
	// the console (tagged copies of the others in developer mode)
	case CG_PRINT:
		if ( n ) {
			if ( com_developer && com_developer->integer ) {
				Com_Printf( "[P%i] %s", n + 1, (const char *)SVMA(1) );
			}
			return qtrue;
		}
		return qfalse;

	// userinfo cvars of an extra player live in its p<N>_ shadows
	case CG_CVAR_REGISTER:
		name = SVMA(2);
		if ( n && name && CL_SplitShadowed( name, args[4] ) ) {
			cvar_t *shadow;
			Cvar_Register( NULL, name, SVMA(3), args[4], cgvm->privateFlag );
			shadow = CL_SplitShadow( n, name, SVMA(3) );
			Cvar_Register( SVMA(1), shadow->name, shadow->resetString, 0, cgvm->privateFlag );
			return qtrue;
		}
		return qfalse;
	case CG_CVAR_SET:
		name = SVMA(1);
		if ( n && name && CL_SplitShadowed( name, 0 ) ) {
			Cvar_SetSafe( CL_SplitShadow( n, name, NULL )->name, SVMA(2) );
			return qtrue;
		}
		return qfalse;
	case CG_CVAR_VARIABLESTRINGBUFFER:
		name = SVMA(1);
		if ( n && name && CL_SplitShadowed( name, 0 ) ) {
			VM_CHECKBOUNDS( cgvm, args[2], args[3] );
			Cvar_VariableStringBufferSafe( CL_SplitShadow( n, name, NULL )->name, SVMA(2), args[3], CVAR_PRIVATE );
			return qtrue;
		}
		return qfalse;

	// commands: an extra cgame registers commands like any cgame (they are
	// dispatched to the cgame of the context that runs them: 'p<N> cmd');
	// it may not unregister the ones player 1's cgame shares
	case CG_REMOVECOMMAND:
		return n ? qtrue : qfalse;
	case CG_SENDCONSOLECOMMAND:
		if ( n ) {
			CL_SplitNestedAdd( n, SVMA(1) );
			return qtrue;
		}
		return qfalse;

	// key catcher: the KEYCATCH_CGAME bit is per player; the keyboard
	// belongs to player 1
	case CG_KEY_GETCATCHER:
		if ( n ) {
			*ret = CL_SplitKeyCatcher();
			return qtrue;
		}
		return qfalse;
	case CG_KEY_SETCATCHER:
		if ( n ) {
			cla->cgameCatcher = args[1] & KEYCATCH_CGAME;
			return qtrue;
		}
		return qfalse;
	case CG_KEY_ISDOWN:
		return n ? qtrue : qfalse;

	// sound (design doc 7): one mixer, every player is a listener
	case CG_S_RESPATIALIZE:
		if ( splitNumViews <= 1 ) {
			return qfalse;
		}
		if ( n ) {
			S_SplitListener( n, args[1], SVMA(2), SVMA(3) );
		} else {
			const vec3_t *axis = SVMA(3);
			splitListener0.valid = qtrue;
			splitListener0.entityNum = args[1];
			VectorCopy( (const float *)SVMA(2), splitListener0.origin );
			VectorCopy( axis[0], splitListener0.axis[0] );
			VectorCopy( axis[1], splitListener0.axis[1] );
			VectorCopy( axis[2], splitListener0.axis[2] );
			splitListener0.inwater = args[4];
		}
		return qtrue;
	case CG_S_STARTLOCALSOUND:
		if ( splitNumViews > 1 && CL_SplitDuplicateSound( args[1], -1, args[2], "local sound" ) ) {
			return qtrue;
		}
		return qfalse;
	case CG_S_STARTSOUND:
		if ( splitNumViews > 1 && CL_SplitDuplicateSound( args[4], args[2], args[3], "sound" ) ) {
			return qtrue;
		}
		return qfalse;
	// cinematics (in-HUD videos, e.g. mod intros on a screen surface): extents are
	// cgame screen pixels -- placed in the cell like 2D, cut to it
	case CG_CIN_PLAYCINEMATIC:
		if ( splitNumViews > 1 ) {
			int x = args[2], y = args[3], w = args[4], h = args[5];
			CL_SplitCinRect( &x, &y, &w, &h );
			*ret = CIN_PlayCinematic( SVMA(1), x, y, w, h, args[6] );
			return qtrue;
		}
		return qfalse;
	case CG_CIN_SETEXTENTS:
		if ( splitNumViews > 1 ) {
			int x = args[2], y = args[3], w = args[4], h = args[5];
			CL_SplitCinRect( &x, &y, &w, &h );
			CIN_SetExtents( args[1], x, y, w, h );
			return qtrue;
		}
		return qfalse;

	case CG_S_CLEARLOOPINGSOUNDS:		// player 1 clears once per frame
	case CG_S_STARTBACKGROUNDTRACK:		// music follows player 1
	case CG_S_STOPBACKGROUNDTRACK:
		return n ? qtrue : qfalse;

	default:
		return qfalse;
	}
}


/*
=============================================================================

SCREEN

=============================================================================
*/

static stereoFrame_t splitStereo;

// a dropped player's reason, shown where its view was for a few seconds
#define SPLIT_DROP_NOTE_TIME	5000
#define SPLIT_REFUSED_NOTE_TIME	10000	// "could not join" on a remote server
typedef struct {
	int			endTime;	// cls.realtime; 0 = unused
	viewRect_t	rect;
	char		text[160];
	qboolean	refused;	// a remote server refused the join
} splitDropNote_t;
static splitDropNote_t	splitDropNotes[MAX_SPLITVIEW];

// one line at the top of player 1's view (a remote server refused a player)
#define SPLIT_HOST_NOTICE_TIME	6000
static struct {
	int		endTime;
	char	text[80];
} splitHostNotice;

// a remote server's refusal note is up: no join hint over it (a rejoin is refused again)
qboolean CL_SplitRefusalShowing( void ) {
	int i;
	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( splitDropNotes[i].endTime && splitDropNotes[i].refused && cls.realtime < splitDropNotes[i].endTime ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void CL_SplitDropNote( int n, const char *reason ) {
	splitDropNote_t *note = &splitDropNotes[ n ];

	note->refused = !Q_stricmpn( reason, "could not join", 14 ) ? qtrue : qfalse;
	if ( note->refused ) {
		Com_sprintf( splitHostNotice.text, sizeof( splitHostNotice.text ), "Player %i could not join this server", n + 1 );
		splitHostNotice.endTime = cls.realtime + SPLIT_HOST_NOTICE_TIME;
	}

	if ( splitNumViews <= 1 || splitRects[ n ].w <= 0 ) {
		// it never had a view (or was alone): the bottom part of the screen
		note->rect.x = 0;
		note->rect.w = cls.glconfig.vidWidth;
		note->rect.h = cls.glconfig.vidHeight / 4;
		note->rect.y = cls.glconfig.vidHeight - note->rect.h;
	} else {
		note->rect = splitRects[ n ];
	}
	Com_sprintf( note->text, sizeof( note->text ), "Player %i %s%s", n + 1,
		( !Q_stricmpn( reason, "left", 4 ) || !Q_stricmp( reason, "dropplayer" ) || !Q_stricmpn( reason, "could not join", 14 ) ) ? "" : "dropped: ",
		!Q_stricmp( reason, "dropplayer" ) ? "left" : reason );
	// a remote server's refusal stays longer: it explains why this player can't play there
	note->endTime = cls.realtime + ( note->refused ? SPLIT_REFUSED_NOTE_TIME : SPLIT_DROP_NOTE_TIME );
}


// the host notice: one line, top center of player 1's view
static void CL_SplitDrawHostNotice( void ) {
	static const vec4_t bg = { 0.0f, 0.0f, 0.0f, 0.75f };
	char line[80];
	viewRect_t r;
	int cols, w, x, y;

	if ( !splitHostNotice.endTime ) {
		return;
	}
	if ( cls.realtime >= splitHostNotice.endTime || cls.state != CA_ACTIVE ) {
		splitHostNotice.endTime = 0;
		return;
	}
	CL_SplitViewRect( 0, &r );
	cols = MIN( (int)sizeof( line ) - 1, r.w / smallchar_width - 2 );
	if ( cols < 8 ) {
		return;
	}
	Q_strncpyz( line, splitHostNotice.text, cols + 1 );
	w = (int)strlen( line ) * smallchar_width;
	x = r.x + ( r.w - w ) / 2;
	y = r.y + r.h / 8;
	re.SetColor( bg );
	re.DrawStretchPic( x - smallchar_width, y - smallchar_height / 2, w + 2 * smallchar_width, smallchar_height * 2,
		0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
	SCR_DrawSmallStringExt( x, y, line, g_color_table[ ColorIndex( COLOR_YELLOW ) ], qfalse, qtrue );
}


// up to 3 lines of the console font, wrapped at spaces, in a box centered in r
static void CL_SplitDrawDropNotes( void ) {
	static const vec4_t bg = { 0.0f, 0.0f, 0.0f, 0.75f };
	splitDropNote_t *note;
	char lines[3][80];
	const char *s;
	int i, j, k, numLines, cols, len, cut, w, h, x, y;

	for ( i = 0, note = splitDropNotes; i < MAX_SPLITVIEW; i++, note++ ) {
		if ( !note->endTime ) {
			continue;
		}
		if ( cls.realtime >= note->endTime || cls.state != CA_ACTIVE ) {
			note->endTime = 0;
			continue;
		}
		cols = MIN( (int)sizeof( lines[0] ) - 1, note->rect.w / smallchar_width - 2 );
		if ( cols < 8 ) {
			continue;
		}
		s = note->text;
		for ( numLines = 0; *s && numLines < 3; numLines++ ) {
			len = (int)strlen( s );
			cut = MIN( len, cols );
			if ( cut < len ) {
				for ( k = cut; k > cols / 2 && s[k] != ' '; k-- )
					;
				if ( s[k] == ' ' ) {
					cut = k;
				}
			}
			Q_strncpyz( lines[numLines], s, cut + 1 );
			s += cut;
			while ( *s == ' ' ) {
				s++;
			}
		}
		w = 0;
		for ( j = 0; j < numLines; j++ ) {
			w = MAX( w, (int)strlen( lines[j] ) * smallchar_width );
		}
		h = numLines * smallchar_height;
		x = note->rect.x + ( note->rect.w - w ) / 2;
		y = note->rect.y + ( note->rect.h - h ) / 2;
		re.SetColor( bg );
		re.DrawStretchPic( x - smallchar_width, y - smallchar_height / 2, w + 2 * smallchar_width, h + smallchar_height,
			0, 0, 0, 0, cls.whiteShader );
		re.SetColor( NULL );
		for ( j = 0; j < numLines; j++ ) {
			SCR_DrawSmallStringExt( note->rect.x + ( note->rect.w - (int)strlen( lines[j] ) * smallchar_width ) / 2,
				y + j * smallchar_height, lines[j], g_color_table[ ColorIndex( COLOR_YELLOW ) ], qfalse, qtrue );
		}
	}
}

static void CL_SplitDrawFunc( void ) {
	CL_CGameRendering( splitStereo );
}


static void CL_SplitFillCell( const viewRect_t *r ) {
	re.SetColor( g_color_table[ ColorIndex( COLOR_BLACK ) ] );
	re.DrawStretchPic( r->x, r->y, r->w, r->h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}


/*
==================
CL_SplitCGameRendering

The screen-update path: every local player's cgame draws its frame into
its own cell.  A player still connecting gets a black cell with a note;
unused grid cells are black.
==================
*/
void CL_SplitCGameRendering( stereoFrame_t stereo ) {
	clientContext_t *entry = cla;
	clientContext_t *ctx;
	viewRect_t r;
	const char *text;
	int i;

	if ( splitNumViews <= 1 ) {
		CL_CGameRendering( stereo );
		CL_SplitDrawDropNotes();
		CL_SplitDrawHostNotice();
		CL_AimAssistDraw();
		IN_GamepadDrawHint();
		return;
	}

	splitListener0.valid = qfalse;
	splitStereo = stereo;

	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( !CL_SlotInUse( i ) ) {
			continue;
		}
		ctx = clx[ i ];
		if ( ctx->joinHeld ) {
			continue;	// no cell yet: its picker is drawn over the current views
		}

		// a cgame already inside a VM call (it is loading and called
		// trap_UpdateScreen) may only be re-entered by itself, like upstream
		if ( ctx->cgameVM && ( ctx == entry || !ctx->cgameVM->callLevel )
			&& CL_ContextState( ctx ) >= CA_LOADING ) {
			if ( i == 0 ) {
				CL_PushContext( 0 );
				CL_CGameRendering( stereo );
				CL_PopContext();
			} else {
				if ( CL_SplitRun( i, CL_SplitDrawFunc ) ) {
					CL_SplitUIRefresh( i );	// its own menu over its own view
				}
			}
			continue;
		}

		CL_SplitViewRect( i, &r );
		CL_SplitFillCell( &r );
		if ( i > 0 && !CL_SplitMenuOpen( i ) ) {
			text = va( "Player %i connecting...", i + 1 );
			SCR_DrawSmallStringExt( r.x + ( r.w - (int)strlen( text ) * smallchar_width ) / 2,
				r.y + ( r.h - smallchar_height ) / 2, text, g_color_table[ ColorIndex( COLOR_WHITE ) ], qfalse, qtrue );
		}
	}

	for ( i = 0; i < splitNumEmpty; i++ ) {
		CL_SplitFillCell( &splitEmpty[ i ] );
	}

	CL_SplitDrawDropNotes();
	CL_SplitDrawHostNotice();
	CL_AimAssistDraw();
	IN_GamepadDrawHint();

	// all listeners are known now: spatialize once for the frame
	if ( splitListener0.valid ) {
		S_Respatialize( splitListener0.entityNum, splitListener0.origin, splitListener0.axis, splitListener0.inwater );
	}
}


/*
=============================================================================

LIFECYCLE

=============================================================================
*/

/*
==================
CL_SplitStartCGame

Start (or restart) the active context's cgame on the level player 1 has
already loaded: no world/collision-map load, media comes from the
renderer and sound caches.  The VM goes on the low hunk after the level
data; it is reclaimed with the rest of the client hunk at the next level
load or vid_restart.
==================
*/
static void CL_SplitStartCGame( void ) {
	int before, cost;

	if ( cgvm ) {
		CL_ShutdownCGame();
	}

	before = Hunk_MemoryRemaining();
	cl_splitInPlaceInit++;
	cls.cgameStarted = qtrue;
	CL_InitCGame();
	cl_splitInPlaceInit--;

	cost = before - Hunk_MemoryRemaining();
	if ( cost > splitCgameHunkCost ) {
		splitCgameHunkCost = cost;
	}
}


/*
==================
CL_SplitHunkHeadroom

Every in-place cgame (re)start puts a fresh QVM on the low hunk, reclaimed
only at the next level load or vid_restart.  True if 'starts' more of them
fit with a safety margin, so a join is refused instead of risking an
ERR_DROP (Hunk_Alloc failed) that would end the session.
==================
*/
#define SPLIT_HUNK_MARGIN		( 4 * 1024 * 1024 )
#define SPLIT_HUNK_DEFAULT_COST	( 8 * 1024 * 1024 )	// until one start was measured

static int CL_SplitCgameCost( void ) {
	int cost = ( splitCgameHunkCost >= 0 ) ? splitCgameHunkCost : SPLIT_HUNK_DEFAULT_COST;

	// a new cgame takes at least what player 1's QVM data segment is (Urban
	// Terror: 64 MB) -- before any extra start was measured, player 1's own
	// in-place restart (which reuses its blocks) reports 0
	if ( clx[ 0 ]->cgameVM && (int)clx[ 0 ]->cgameVM->dataLength > cost ) {
		cost = (int)clx[ 0 ]->cgameVM->dataLength;
	}
	return cost;
}

static qboolean CL_SplitHunkHeadroom( int starts ) {
	return ( Hunk_MemoryRemaining() >= starts * CL_SplitCgameCost() + SPLIT_HUNK_MARGIN ) ? qtrue : qfalse;
}


/*
==================
CL_SplitCGameShutdown

Called by CL_ShutdownCGame for any context: per-player cgame state.
==================
*/
void CL_SplitCGameShutdown( void ) {
	cla->cgameCatcher = 0;
	cla->cgameWidth = 0;
	cla->cgameHeight = 0;
}


/*
==================
CL_SplitGameCommand

A console command typed for an extra player ('p<N> cmd') offered to that
player's cgame, with its errors isolated.
==================
*/
static qboolean splitCmdResult;

static void CL_SplitGameCommandFunc( void ) {
	splitCmdResult = (qboolean)VM_Call( cgvm, 0, CG_CONSOLE_COMMAND );
	Cbuf_NestedReset();
}

qboolean CL_SplitGameCommand( void ) {
	if ( !cgvm ) {
		return qfalse;
	}
	splitCmdResult = qfalse;
	if ( !CL_SplitRun( cla->playerNum, CL_SplitGameCommandFunc ) ) {
		return qtrue;	// its cgame failed and the player was dropped
	}
	return splitCmdResult;
}


/*
==================
CL_SplitCgameCatches / CL_SplitCgameKey

A player whose cgame set KEYCATCH_CGAME (a mod's in-cgame menu or vote
screen) gets its pad keys delivered as CG_KEY_EVENT, as CL_KeyDownEvent /
CL_KeyUpEvent do for the keyboard; escape clears the catcher.
==================
*/
qboolean CL_SplitCgameCatches( int n ) {
	if ( n == 0 ) {
		return ( ( Key_GetCatcher() & KEYCATCH_CGAME ) && clx[0]->cgameVM ) ? qtrue : qfalse;
	}
	return ( CL_SplitSlotActive( n ) && ( clx[n]->cgameCatcher & KEYCATCH_CGAME ) && clx[n]->cgameVM ) ? qtrue : qfalse;
}

static int		splitCgameKey;
static qboolean	splitCgameKeyDown;

static void CL_SplitCgameKeyFunc( void ) {
	if ( !cgvm || cgvm->callLevel ) {
		return;
	}
	if ( splitCgameKeyDown && splitCgameKey == K_ESCAPE ) {
		if ( cla->playerNum == 0 ) {
			Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_CGAME );
		} else {
			cla->cgameCatcher = 0;
		}
		VM_Call( cgvm, 1, CG_EVENT_HANDLING, CGAME_EVENT_NONE );
		return;
	}
	VM_Call( cgvm, 2, CG_KEY_EVENT, splitCgameKey, splitCgameKeyDown );
}

void CL_SplitCgameKey( int n, int key, qboolean down ) {
	if ( !CL_SlotInUse( n ) || !clx[n]->cgameVM ) {
		return;
	}
	splitCgameKey = key;
	splitCgameKeyDown = down;
	Com_DPrintf( "P%i cgame key %s %s\n", n + 1, Key_KeynumToString( key ), down ? "down" : "up" );
	if ( n == 0 ) {
		CL_PushContext( 0 );
		CL_SplitCgameKeyFunc();
		CL_PopContext();
	} else {
		CL_SplitRun( n, CL_SplitCgameKeyFunc );
	}
}


/*
==================
CL_SplitShutdownCGames

Called from CL_ShutdownVMs (level change, vid_restart, disconnect) after
the active context's cgame went down: every other context's cgame lives
on the same hunk, so it goes down too.  Extra players that keep their
gamestate (vid_restart) restart their cgame once player 1 is back in game.
==================
*/
void CL_SplitShutdownCGames( void ) {
	int i;

	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		clientContext_t *ctx = clx[ i ];
		if ( !ctx || ctx == cla || !ctx->cgameVM ) {
			continue;
		}
		CL_PushContext( i );
		CL_ShutdownCGame();
		if ( i > 0 && ctx->inUse && cls.state >= CA_PRIMED ) {
			ctx->cgamePending = qtrue;
		}
		CL_PopContext();
	}
}


/*
==================
CL_SplitMapLoading

A local server is loading a new level: extra players stay connected (same
slot), drop back to awaiting the new gamestate and keep their viewport
cell so player 1's cgame starts with the split layout.
==================
*/
void CL_SplitMapLoading( void ) {
	int i;

	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		if ( !CL_SlotInUse( i ) ) {
			continue;
		}
		CL_PushContext( i );
		if ( cls.state >= CA_CONNECTED ) {
			cls.state = CA_CONNECTED;
			Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
			clc.lastPacketSentTime = cls.realtime - RETRANSMIT_TIMEOUT; // send a packet soon
			clc.lastPacketTime = cls.realtime;	// loading is not a timeout
		}
		cla->cgamePending = qfalse;	// wait for the new gamestate
		CL_PopContext();
	}
}


/*
==================
CL_SplitscreenGamestate

Called from CL_ParseGamestate for players > 0 once the configstrings,
baselines and clientNum are parsed.  Everything after that point in
CL_ParseGamestate (fs_game / pure paks / cvar side effects, downloads,
cgame start) belongs to player 0 only; this player's cgame is started by
CL_SplitscreenFrame once player 0 is in game on the same level.
==================
*/
void CL_SplitscreenGamestate( void ) {
	const char *systemInfo;

	if ( cgvm ) {
		CL_ShutdownCGame();
	}

	systemInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
	cl.serverId = atoi( Info_ValueForKey( systemInfo, "sv_serverid" ) );

	Com_Printf( "P%i got gamestate, clientNum=%i\n", cla->playerNum + 1, clc.clientNum );

	// a pure server keeps resending the gamestate until it gets this
	// player's pak checksums (same paks as player 1's)
	CL_SendPureChecksums();

	// usercmds are sent from CA_PRIMED on; they make the server put this
	// player in the game and start sending snapshots
	cls.state = CA_PRIMED;
	cla->cgamePending = qtrue;
}


/*
==================
CL_DropContext

Disconnect extra player n and release everything but the context memory
(freed by CL_SplitReap at a safe point, since this may run with the
context pushed).  The slot number becomes free; nobody renumbers.
==================
*/
static void CL_DropContext( int n, const char *reason ) {
	clientContext_t *ctx = clx[ n ];
	int i;

	if ( n <= 0 || !ctx || !ctx->inUse ) {
		return;
	}

	CL_SplitUIFree( n );

	CL_PushContext( n );

	// tell the server first: needs only cl/clc, not the cgame
	if ( cls.state >= CA_CONNECTED ) {
		CL_AddReliableCommand( "disconnect", qtrue );
		for ( i = 0; i < 3; i++ ) {
			CL_WritePacket( 0 );
		}
	}

	if ( cgvm ) {
		if ( cgvm->callLevel ) {
			// it failed inside a call (error isolation): don't call into it again
			CL_SplitCGameShutdown();
			cls.cgameStarted = qfalse;
			VM_Free( cgvm );
			cgvm = NULL;
		} else {
			CL_ShutdownCGame();
		}
	}
	cls.state = CA_DISCONNECTED;

	CL_PopContext();

	NET_CloseClientSocket( ctx->sock );
	Com_Printf( "P%i: disconnected (%s)\n", n + 1, reason );

	// the reason stays on screen where its view was for a few seconds (not for a
	// cancelled join, which never had a view, nor when the whole session ends)
	if ( !ctx->joinHeld && Q_stricmp( reason, "player 1 disconnected" ) ) {
		CL_SplitDropNote( n, reason );
	}

	ctx->inUse = qfalse;
	ctx->cgamePending = qfalse;
	ctx->dropRequested = qfalse;
	splitLayoutDirty = qtrue;
	CL_SplitUpdateLayout();
}


/*
==================
CL_SplitReap

Free dropped contexts.  Only at points where no context is pushed.
==================
*/
static void CL_SplitReap( void ) {
	int i;

	if ( ctxStackDepth ) {
		return;
	}
	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		clientContext_t *ctx = clx[ i ];
		if ( ctx && !ctx->inUse && ctx != cla ) {
			clx[ i ] = NULL;
			free( ctx );
		}
	}
}


/*
==================
CL_SplitRequestDrop

The server dropped an extra player (or it asked to leave).  Called from
packet parsing or from inside that player's cgame, so the drop itself
waits for the next CL_SplitscreenFrame.
==================
*/
void CL_SplitRequestDrop( int ctxNum, const char *reason ) {
	clientContext_t *ctx;

	if ( !CL_SlotInUse( ctxNum ) || ctxNum == 0 ) {
		return;
	}
	ctx = clx[ ctxNum ];
	if ( !ctx->dropRequested ) {
		ctx->dropRequested = qtrue;
		Q_strncpyz( ctx->dropReason, reason, sizeof( ctx->dropReason ) );
	}
}


/*
==================
CL_SplitProcessDrops

Carry out requested drops whose cgame is not inside a call.
==================
*/
static void CL_SplitProcessDrops( void ) {
	int i;

	if ( ctxStackDepth ) {
		return;
	}
	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		clientContext_t *ctx = clx[ i ];
		if ( ctx && ctx->inUse && ctx->dropRequested && ( !ctx->cgameVM || !ctx->cgameVM->callLevel ) ) {
			CL_DropContext( i, ctx->dropReason );
		}
	}
	CL_SplitReap();
}


/*
==================
CL_SplitCheckLayout

A running cgame whose believed screen size changed (join/leave, layout
cvars, window size) restarts on the loaded level.  Skipped while player 1
is not in game.  A cgame that never asked for the glconfig is left alone.
==================
*/
static void CL_SplitCheckLayout( void ) {
	int i, w, h;

	if ( cls.state != CA_ACTIVE ) {	// player 1
		return;
	}

	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		clientContext_t *ctx = clx[ i ];
		if ( !CL_SlotInUse( i ) || !ctx->cgameVM || ctx->cgameVM->callLevel || !ctx->cgameWidth ) {
			continue;
		}
		if ( CL_ContextState( ctx ) != CA_ACTIVE ) {
			continue;
		}
		CL_SplitToldSize( i, &w, &h );
		if ( w == ctx->cgameWidth && h == ctx->cgameHeight ) {
			continue;
		}
		if ( !CL_SplitHunkHeadroom( 1 ) ) {
			Com_Printf( S_COLOR_YELLOW "P%i: not enough hunk memory to restart its cgame for the new layout "
				"(keeps its old screen size until the next level load)\n", i + 1 );
			ctx->cgameWidth = w;	// don't retry every frame
			ctx->cgameHeight = h;
			continue;
		}
		Com_Printf( "P%i: cgame screen %ix%i -> %ix%i, restarting cgame\n", i + 1,
			ctx->cgameWidth, ctx->cgameHeight, w, h );
		if ( i == 0 ) {
			CL_PushContext( 0 );
			CL_SplitStartCGame();
			CL_PopContext();
		} else {
			CL_SplitRun( i, CL_SplitStartCGame );
		}
	}
}


/*
==================
CL_SplitStartPending

Start the cgame of one extra player whose gamestate is in, once player 1
is in game on the same level.  One per frame.
==================
*/
static void CL_SplitStartPending( void ) {
	char mapname[ MAX_QPATH ];
	const char *info;
	int i;

	if ( cls.state != CA_ACTIVE || !cgvm ) {	// player 1
		return;
	}

	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		clientContext_t *ctx = clx[ i ];
		if ( !CL_SlotInUse( i ) || !ctx->cgamePending || ctx->clsShadow.state < CA_PRIMED || ctx->dropRequested ) {
			continue;
		}

		CL_PushContext( i );
		info = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
		Com_sprintf( mapname, sizeof( mapname ), "maps/%s.bsp", Info_ValueForKey( info, "mapname" ) );
		CL_PopContext();

		if ( Q_stricmp( mapname, cl.mapname ) ) {
			continue;	// stale gamestate from the previous level; a new one is coming
		}

		ctx->cgamePending = qfalse;
		if ( !CL_SplitHunkHeadroom( 1 ) ) {
			CL_SplitRequestDrop( i, "not enough hunk memory for another cgame (load a level to reclaim)" );
			return;
		}
		CL_SplitRun( i, CL_SplitStartCGame );
		ctx->turnRefresh = qtrue;	// its load time is not "stuck" time (ctx is freed only by CL_SplitReap)
		return;
	}
}


/*
==================
CL_SplitCheckTimeout

Extra players: no packets for cl_timeout seconds, or no connection
within cl_timeout seconds of addplayer, drops that player.
==================
*/
static void CL_SplitCheckTimeout( void ) {
	if ( cls.state >= CA_CONNECTED ) {
		if ( ( !CL_CheckPaused() || !sv_paused->integer )
			&& cls.realtime - clc.lastPacketTime > cl_timeout->integer * 1000 ) {
			if ( ++cl.timeoutcount > 5 ) {
				CL_SplitRequestDrop( cla->playerNum, "server connection timed out" );
			}
		} else {
			cl.timeoutcount = 0;
		}
	} else if ( cls.state >= CA_CONNECTING && cls.realtime - cla->joinTime > cl_timeout->integer * 1000 ) {
		CL_SplitRequestDrop( cla->playerNum, "could not connect" );
	}
}


/*
==================
CL_SplitServerPrint

An out-of-band "print" from the server to an extra player that is still
connecting: the server refused it (design 4.3: "Too many connections.",
"Server is full.", a ban, a mod's own reason) -- drop that player with
the server's words, shown in its cell; the others play on.  "Reconnecting,
please wait N seconds" holds its next connect attempt N+1 seconds;
"Incorrect challenge" just retries.
==================
*/
void CL_SplitServerPrint( const char *message ) {
	char text[MAX_STRING_CHARS];
	const char *p;
	int len, wait;

	if ( cla->playerNum <= 0 || ( cls.state != CA_CONNECTING && cls.state != CA_CHALLENGING ) ) {
		return;
	}

	// one line, no trailing newline
	while ( *message == '\n' || *message == ' ' ) {
		message++;
	}
	Q_strncpyz( text, message, sizeof( text ) );
	for ( len = (int)strlen( text ); len > 0 && ( text[len-1] == '\n' || text[len-1] == ' ' ); ) {
		text[--len] = '\0';
	}
	if ( !text[0] ) {
		return;
	}

	if ( !Q_stricmpn( text, "Reconnecting, please wait", 25 ) ) {
		for ( p = text + 25; *p && ( *p < '0' || *p > '9' ); p++ )
			;
		wait = *p ? atoi( p ) : 1;
		wait = MAX( 1, MIN( wait, 30 ) );
		Com_Printf( "P%i: server asks to wait %i s before connecting again\n", cla->playerNum + 1, wait );
		clc.connectTime = cls.realtime + ( wait + 1 ) * 1000 - RECONNECT_TIMEOUT;
		cla->joinTime = cls.realtime + ( wait + 1 ) * 1000;	// not a connect timeout
		cla->turnTime = cla->joinTime;						// nor holding up the queue
		return;
	}
	if ( !Q_stricmpn( text, "Incorrect challenge", 19 ) ) {
		return;	// the next resend asks for a new one
	}

	Com_Printf( "P%i: the server refused this player: %s\n", cla->playerNum + 1, text );

	if ( CL_SplitRemoteServer() ) {
		// another machine's server: its limits are its own and unknown in advance --
		// say plainly that this player could not join (in its cell for 10 s, and
		// one line for player 1); the per-address cap's number is what got in
		char reason[MAX_STRING_CHARS];
		if ( !Q_stricmpn( text, "Too many connections", 20 ) ) {
			const int in = CL_SplitPlayersInGame();
			Com_sprintf( reason, sizeof( reason ), "could not join: this server allows only %i player%s from one connection",
				in, in == 1 ? "" : "s" );
		} else {
			Com_sprintf( reason, sizeof( reason ), "could not join: %s", text );
		}
		Com_Printf( "Player %i %s\n", cla->playerNum + 1, reason );
		CL_SplitRequestDrop( cla->playerNum, reason );
		return;
	}
	CL_SplitRequestDrop( cla->playerNum, va( "server refused: %s", text ) );
}


/*
==================
CL_SplitRemoteServer / CL_SplitPlayersInGame

Player 1 plays on another machine's server (not our own listen server,
which it reaches over loopback); the number of local players in game.
==================
*/
qboolean CL_SplitRemoteServer( void ) {
	const clientContext_t *p1 = clx[ 0 ];
	return ( CL_ContextState( p1 ) >= CA_CONNECTED && p1->clConn.serverAddress.type != NA_LOOPBACK
		&& !p1->clConn.demoplaying ) ? qtrue : qfalse;
}

int CL_SplitPlayersInGame( void ) {
	int i, n = 0;
	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( CL_SlotInUse( i ) && !clx[ i ]->dropRequested && CL_ContextState( clx[ i ] ) == CA_ACTIVE ) {
			n++;
		}
	}
	return n;
}


/*
==================
CL_SplitConnectWaiting

Extra players connect one at a time (design 4.3: a burst of getchallenge /
connect packets from one IP trips the server's rate limits and its
same-address reconnect checks): qtrue while another extra player that
started joining earlier is not in the game yet (the server counts a client
for sv_maxclientsPerIP only once it is in, so the cap is honoured too).
==================
*/
#define SPLIT_TURN_TIMEOUT	15000	// msec an extra player may hold up the ones queued after it
#define SPLIT_LOAD_TIMEOUT	60000	// ... of which at most this much loading is not counted

static qboolean CL_SplitConnectWaiting( void ) {
	clientContext_t *ctx;
	connstate_t st;
	int i;

	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		ctx = clx[ i ];
		if ( !ctx || ctx == cla || !ctx->inUse || ctx->dropRequested || ctx->joinHeld ) {
			continue;
		}
		st = CL_ContextState( ctx );
		if ( st >= CA_CONNECTING && st < CA_ACTIVE && ctx->joinSeq < cla->joinSeq ) {
			// a player stuck on the way in (packets still arriving, so no
			// cl_timeout) must not hold the queue forever
			if ( cls.realtime - ctx->turnTime > SPLIT_TURN_TIMEOUT ) {
				Com_Printf( "P%i: not in the game %i s after its turn began; dropping it so P%i can connect\n",
					i + 1, SPLIT_TURN_TIMEOUT / 1000, cla->playerNum + 1 );
				CL_SplitRequestDrop( i, "could not get into the game in time" );
				continue;
			}
			return qtrue;
		}
	}
	return qfalse;
}


/*
==================
CL_SplitRefreshTurns

The connect queue's bounded wait (SPLIT_TURN_TIMEOUT) is for a player stuck
on the way in, not for one that is loading: time spent loading the level
(CA_LOADING), waiting in CA_PRIMED for its cgame start (player 1 still
loading, or its turn among the pending cgames) and the cgame load itself
(turnRefresh, set by CL_SplitStartPending: that frame's time jump) does not
count.  Runs before any player's frame, so every queued player sees fresh
values whatever the slot order.
==================
*/
static void CL_SplitRefreshTurns( void ) {
	const connstate_t p1 = CL_ContextState( clx[ 0 ] );
	clientContext_t *ctx;
	connstate_t st;
	int i;

	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		ctx = clx[ i ];
		if ( !CL_SlotInUse( i ) ) {
			continue;
		}
		st = CL_ContextState( ctx );
		if ( st == CA_LOADING || ( st == CA_PRIMED && ( ctx->cgamePending || p1 != CA_ACTIVE ) ) ) {
			// loading is not stuck time -- up to SPLIT_LOAD_TIMEOUT: a server that never
			// lets it out of loading (packets keep coming, so no cl_timeout) must
			// still free the queue
			if ( !ctx->loadSince ) {
				ctx->loadSince = cls.realtime;
			}
			if ( cls.realtime - ctx->loadSince < SPLIT_LOAD_TIMEOUT ) {
				ctx->turnTime = cls.realtime;
			}
		} else {
			ctx->loadSince = 0;
		}
		if ( ctx->turnRefresh ) {
			ctx->turnTime = cls.realtime;	// the frame its cgame loaded in (one long frame)
			ctx->turnRefresh = qfalse;
		}
	}
}


/*
==================
CL_SplitPlayerFrame

One extra player's per-frame connection upkeep (CL_Frame's order).
==================
*/
static void CL_SplitPlayerFrame( void ) {
	int userinfoModified;

	CL_SplitCheckUserinfo();
	CL_SplitCheckTimeout();
	if ( cla->dropRequested ) {
		return;
	}

	if ( ( cls.state == CA_CONNECTING || cls.state == CA_CHALLENGING ) && CL_SplitConnectWaiting() ) {
		// its turn comes when the one before it is in: send at once then
		cla->joinTime = cls.realtime;
		cla->turnTime = cls.realtime;
		clc.connectTime = cls.realtime - RECONNECT_TIMEOUT;
		if ( !cla->connectQueued ) {
			cla->connectQueued = qtrue;
			Com_Printf( "P%i: waiting for the player before it to connect\n", cla->playerNum + 1 );
		}
	} else {
		cla->connectQueued = qfalse;
		// an extra player's connect must not swallow player 1's pending
		// userinfo change
		userinfoModified = cvar_modifiedFlags & CVAR_USERINFO;
		CL_CheckForResend();
		cvar_modifiedFlags |= userinfoModified;
	}

	CL_SendCmd();
	CL_SetCGameTime();

	if ( cls.state == CA_ACTIVE ) {
		// an in-place cgame restart (layout change) briefly leaves CA_ACTIVE:
		// the queue's bounded wait counts from the last frame it was in game
		cla->turnTime = cls.realtime;
	}
	if ( cla->connectStart && cls.state == CA_ACTIVE ) {
		Com_Printf( "P%i: in game %i ms after taking its slot (%i ms after it started connecting)\n", cla->playerNum + 1,
			cls.realtime - cla->slotTime, cls.realtime - cla->connectStart );
		cla->connectStart = 0;
	}
}


/*
==================
CL_SplitscreenFrame

Per-frame work for extra local players, called from CL_Frame in player
1's context right after player 1's CL_SendCmd / CL_CheckForResend.
==================
*/
void CL_SplitscreenFrame( void ) {
	int i;

	// a Com_Error longjmp out of a pushed context leaves stale entries
	splitGuard = NULL;
	if ( ctxStackDepth ) {
		ctxStackDepth = 0;
		CL_SwitchTo( clx[ 0 ] );
	}

	if ( cl_splitFill->modified || cl_splitVertical->modified || cl_splitWidePlayer->modified || cl_splitAspect->modified ) {
		cl_splitFill->modified = qfalse;
		cl_splitVertical->modified = qfalse;
		cl_splitWidePlayer->modified = qfalse;
		cl_splitAspect->modified = qfalse;
		splitLayoutDirty = qtrue;
	}

	CL_SplitProcessDrops();
	CL_SplitRefreshTurns();

	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		CL_SplitRunCgameCmds( i );
		if ( CL_SlotInUse( i ) ) {
			CL_SplitRun( i, CL_SplitPlayerFrame );
		}
	}

	CL_SplitCheckLayout();
	CL_SplitStartPending();
	CL_AimAssistFrame();
	CL_SplitSrvFrame();
	CL_IndepFrame();
}


/*
==================
CL_SplitscreenDisconnect

Called at the top of CL_Disconnect (player 1 leaving, Com_Error, quit):
return to context 0 and drop every extra player -- player 1 is the host
slot and ends the session for all.  The context stack is left alone: a
caller that pushed a context pops back into context 0.
==================
*/
void CL_SplitscreenDisconnect( void ) {
	int i;

	splitGuard = NULL;
	CL_SplitUIRestoreP1();	// an uncaught error inside a player's menu
	CL_SwitchTo( clx[ 0 ] );

	// the next connection's userinfo goes out in its connect packet, not
	// through CL_CheckUserinfo: forget the last one sent
	splitP1Userinfo[0] = '\0';

	for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
		if ( CL_SlotInUse( i ) ) {
			CL_DropContext( i, "player 1 disconnected" );
		}
	}

	// test scripts (developer only): go on after player 1's connection ended --
	// an error (server disconnect, kick) has just cleared the command buffer
	if ( com_developer && com_developer->integer && cl_splitTestOnDisconnect && cl_splitTestOnDisconnect->string[0] ) {
		Cbuf_AddText( va( "%s\n", cl_splitTestOnDisconnect->string ) );
		Cvar_Set( cl_splitTestOnDisconnect->name, "" );
	}
}


/*
=============================================================================

CONSOLE COMMANDS

=============================================================================
*/

/*
==================
CL_PacketEventSock

A packet arrived on an extra local player's own socket.
==================
*/
static const netadr_t	*splitPacketFrom;
static msg_t			*splitPacketMsg;

static void CL_SplitPacketFunc( void ) {
	if ( cla->debugError == 2 ) {
		cla->debugError = 0;
		Com_Error( ERR_DROP, "splitdebug: forced error parsing P%i's packet", cla->playerNum + 1 );
	}
	CL_PacketEvent( splitPacketFrom, splitPacketMsg );
}

void CL_PacketEventSock( netsrc_t sock, const netadr_t *from, msg_t *msg ) {
	const int n = sock - NS_CLIENT2 + 1;

	if ( n <= 0 || !CL_SlotInUse( n ) || clx[ n ]->debugFreeze ) {
		return;
	}

	splitPacketFrom = from;
	splitPacketMsg = msg;
	CL_SplitRun( n, CL_SplitPacketFunc );
}


/*
==================
CL_SplitPlayerCmd_f

p1 .. p8 <command>: run a console command as that local player
(button commands, cgame commands and server commands all go to that
player's context).  'p<N> disconnect' drops only that player;
'p<N> <userinfo cvar> [value]' reads/writes that player's p<N>_ shadow.
==================
*/
static void CL_SplitPlayerCmd_f( void ) {
	char text[ MAX_STRING_CHARS ];
	const int n = Cmd_Argv( 0 )[1] - '1';
	const char *cmd, *s;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: %s <command>\n", Cmd_Argv( 0 ) );
		return;
	}
	if ( !CL_SlotInUse( n ) ) {
		Com_Printf( "%s: no local player %i\n", Cmd_Argv( 0 ), n + 1 );
		return;
	}

	cmd = Cmd_Argv( 1 );
	if ( n > 0 && !Q_stricmp( cmd, "disconnect" ) ) {
		CL_SplitRequestDrop( n, "left" );
		return;
	}
	// look/aim/toggle settings: every player's own p<N>_joy_* (player 1 too)
	if ( !Q_stricmpn( cmd, "joy_", 4 ) && IN_PadFeelCvar( n, cmd ) ) {
		cvar_t *feel = IN_PadFeelCvar( n, cmd );
		if ( Cmd_Argc() == 2 ) {
			Com_Printf( "\"%s\" is \"%s^7\"\n", feel->name, feel->string );
		} else {
			Cvar_Set( feel->name, Cmd_ArgsFrom( 2 ) );
			IN_PadFeelChanged( n );
		}
		return;
	}
	if ( n > 0 && ( CL_SplitShadowed( cmd, 0 ) || !Q_stricmpn( cmd, "joy_", 4 ) ) ) {
		cvar_t *shadow = CL_SplitShadow( n, cmd, NULL );
		if ( Cmd_Argc() == 2 ) {
			Com_Printf( "\"%s\" is \"%s^7\"\n", shadow->name, shadow->string );
		} else {
			Cvar_Set( shadow->name, Cmd_ArgsFrom( 2 ) );
		}
		return;
	}

	// keep the original quoting: skip the 'pN' token of the raw text
	s = Cmd_Cmd();
	while ( *s == ' ' || *s == '\t' ) {
		s++;
	}
	while ( *s && *s != ' ' && *s != '\t' ) {
		s++;
	}
	while ( *s == ' ' || *s == '\t' ) {
		s++;
	}
	Q_strncpyz( text, s, sizeof( text ) );

	CL_PushContext( n );
	Cmd_ExecuteString( text );
	CL_PopContext();
}


/*
==================
CL_AddPlayer_f

addplayer [n]: join the server player 1 is playing on as local player n
(2..cl_splitMaxPlayers; default the lowest free number).
==================
*/
/*
==================
CL_SplitFreeSlot

Lowest free player slot below cl_splitMaxPlayers, or -1.
==================
*/
int CL_SplitFreeSlot( void ) {
	int n;

	for ( n = 1; n < cl_splitMaxPlayers->integer && n < MAX_SPLITVIEW; n++ ) {
		if ( !clx[ n ] ) {
			return n;
		}
	}
	return -1;
}


/*
==================
CL_SplitAddPlayer

Join the server player 1 is playing on as local player slot n (-1 = the
lowest free one).  Returns the slot, or -1 (reason printed).  Must be
called with player 1 active (console command or pad join).
==================
*/
int CL_SplitAddPlayer( int n ) {
	return CL_SplitAddPlayerEx( n, qfalse );
}


/*
==================
CL_SplitAddPlayerEx

hold: take the slot and its viewport cell but don't connect yet (the
join-time profile picker runs in the cell first); CL_SplitConnectPlayer
starts the connection, dropping the slot cancels the join.
==================
*/
int CL_SplitAddPlayerEx( int n, qboolean hold ) {
	clientContext_t *ctx;
	netadr_t adr;
	int maxPlayers;

	maxPlayers = cl_splitMaxPlayers->integer;

	if ( CL_IndepActive() ) {
		Com_Printf( "addplayer: Independent mode: every player plays in a window of their own\n" );
		return -1;
	}
	if ( cla->playerNum != 0 || cls.state != CA_ACTIVE || clc.demoplaying ) {
		Com_Printf( "addplayer: player 1 must be in a game first\n" );
		return -1;
	}

	// a 'dropplayer' earlier this frame frees its number now
	CL_SplitProcessDrops();

	if ( n >= 0 ) {
		if ( n < 1 || n >= maxPlayers ) {
			Com_Printf( "addplayer: player number must be 2..%i (cl_splitMaxPlayers)\n", maxPlayers );
			return -1;
		}
		if ( clx[ n ] ) {
			Com_Printf( "addplayer: player %i is already %s\n", n + 1, clx[ n ]->inUse ? "playing" : "leaving" );
			return -1;
		}
	} else {
		n = CL_SplitFreeSlot();
		if ( n < 0 ) {
			Com_Printf( "addplayer: all %i player slots are taken (cl_splitMaxPlayers)\n", maxPlayers );
			return -1;
		}
	}

	// the join starts one cgame; running cgames that restart for the new
	// layout reuse their own hunk blocks (VM_HunkAlloc)
	if ( !CL_SplitHunkHeadroom( 1 ) ) {
		Com_Printf( S_COLOR_YELLOW "addplayer: not enough hunk memory for another player (%i MB free, a player's "
			"game needs about %i MB): load a level to reclaim, or raise com_hunkMegs (now %i) and restart\n",
			Hunk_MemoryRemaining() / ( 1024 * 1024 ), CL_SplitCgameCost() / ( 1024 * 1024 ) + 4,
			Cvar_VariableIntegerValue( "com_hunkMegs" ) );
		return -1;
	}

	if ( clc.serverAddress.type == NA_LOOPBACK ) {
		// our own listen server: reach it over UDP like a LAN player
		if ( !NET_StringToAdr( va( "127.0.0.1:%i", Cvar_VariableIntegerValue( "net_port" ) ), &adr, NA_IP ) ) {
			Com_Printf( "addplayer: bad local server address\n" );
			return -1;
		}
	} else {
		adr = clc.serverAddress;
	}

	ctx = calloc( 1, sizeof( *ctx ) );
	if ( !ctx ) {
		Com_Printf( "addplayer: out of memory\n" );
		return -1;
	}
	ctx->playerNum = n;
	ctx->inUse = qtrue;
	ctx->sock = (netsrc_t)( NS_CLIENT2 + n - 1 );
	ctx->qport = ( Cvar_VariableIntegerValue( "net_qport" ) + n ) & 0xffff;
	ctx->clsShadow.state = CA_DISCONNECTED;
	ctx->joinSeq = ++splitJoinCounter;
	ctx->joinTime = cls.realtime;
	ctx->slotTime = cls.realtime;

	if ( !NET_OpenClientSocket( ctx->sock ) ) {
		Com_Printf( "addplayer: could not open a UDP socket for player %i\n", n + 1 );
		free( ctx );
		return -1;
	}

	clx[ n ] = ctx;

	CL_PushContext( n );
	clc.serverAddress = adr;
	CL_PopContext();

	ctx->joinHeld = hold;	// a held slot gets its cell only when it connects
	splitLayoutDirty = qtrue;
	CL_SplitUpdateLayout();

	if ( hold ) {
		Com_Printf( "P%i: slot reserved (choosing a profile)\n", n + 1 );
		return n;
	}
	CL_SplitConnectPlayer( n );
	return n;
}


/*
==================
CL_SplitConnectPlayer

Start connecting a slot taken by CL_SplitAddPlayerEx.
==================
*/
qboolean CL_SplitConnectPlayer( int n ) {
	clientContext_t *ctx;

	if ( n <= 0 || !CL_SplitSlotActive( n ) || CL_ContextState( clx[ n ] ) != CA_DISCONNECTED ) {
		return qfalse;
	}
	ctx = clx[ n ];
	if ( ctx->joinHeld ) {
		ctx->joinHeld = qfalse;
		splitLayoutDirty = qtrue;	// it takes its cell now
		CL_SplitUpdateLayout();
	}
	ctx->joinTime = cls.realtime;
	ctx->turnTime = cls.realtime;
	ctx->connectStart = cls.realtime ? cls.realtime : 1;

	CL_PushContext( n );
	Com_RandomBytes( (byte*)&clc.challenge, sizeof( clc.challenge ) );
	cls.state = CA_CONNECTING;
	clc.connectTime = cls.realtime - RECONNECT_TIMEOUT; // CL_CheckForResend() fires next frame
	clc.connectPacketCount = 0;
	Com_Printf( "P%i: connecting to %s, qport %i\n", n + 1, NET_AdrToStringwPort( &clc.serverAddress ), ctx->qport );
	CL_PopContext();
	return qtrue;
}


// a slot taken for a join whose player is still choosing a profile
qboolean CL_SplitSlotJoining( int n ) {
	return ( n > 0 && CL_SplitSlotActive( n ) && clx[ n ]->joinHeld ) ? qtrue : qfalse;
}


/*
==================
CL_AddPlayer_f

addplayer [n] [profile|guest]
==================
*/
static void CL_AddPlayer_f( void ) {
	const char *profile = NULL;
	int n = -1;

	if ( Cmd_Argc() > 1 && Q_isanumber( Cmd_Argv( 1 ) ) ) {
		n = atoi( Cmd_Argv( 1 ) ) - 1;
		if ( Cmd_Argc() > 2 ) {
			profile = Cmd_ArgsFrom( 2 );
		}
	} else if ( Cmd_Argc() > 1 ) {
		profile = Cmd_ArgsFrom( 1 );
	}
	if ( CL_IndepCoordinator() ) {
		// Independent mode: a window of its own, without a pad (keyboard/mouse when focused)
		if ( n >= 0 && CL_IndepSlotLive( n ) ) {
			Com_Printf( "addplayer: player %i already has a window\n", n + 1 );
			return;
		}
		n = CL_IndepReserve( "" );
		if ( n >= 0 ) {
			CL_IndepSpawn( n, profile );
		}
		return;
	}
	n = CL_SplitAddPlayerEx( n, qtrue );
	if ( n < 0 ) {
		return;
	}
	if ( !CL_ProfileLoad( n, profile, qtrue ) ) {
		CL_ProfileLoad( n, NULL, qtrue );	// unknown / in use: a guest
	}
	CL_SplitConnectPlayer( n );
}


/*
==================
CL_DropPlayer_f
==================
*/
static void CL_DropPlayer_f( void ) {
	int n;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: dropplayer <player number 2..%i>\n", MAX_SPLITVIEW );
		return;
	}
	n = atoi( Cmd_Argv( 1 ) ) - 1;
	if ( n == 0 ) {
		Com_Printf( "dropplayer: player 1 is the host; use disconnect\n" );
		return;
	}
	if ( !CL_SlotInUse( n ) ) {
		Com_Printf( "dropplayer: no local player %s\n", Cmd_Argv( 1 ) );
		return;
	}
	CL_SplitRequestDrop( n, "dropplayer" );
}


/*
==================
CL_SplitPlayers_f
==================
*/
static void CL_SplitPlayers_f( void ) {
	static const char *stateNames[] = { "uninitialized", "disconnected", "authorizing", "connecting",
		"challenging", "connected", "loading", "primed", "active", "cinematic" };
	viewRect_t r;
	char menu[64];
	int i, st;

	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( !CL_SlotInUse( i ) ) {
			continue;
		}
		st = CL_ContextState( clx[ i ] );
		CL_SplitViewRect( i, &r );
		CL_SplitUIInfo( i, menu, sizeof( menu ) );
		Com_Printf( "P%i: %-12s client %2i  cell %4i,%4i %4ix%-4i  cgame %s  menu %s  name \"%s^7\"  profile %s\n", i + 1,
			clx[ i ]->joinHeld ? "joining" : ( st >= 0 && st < (int)ARRAY_LEN( stateNames ) ) ? stateNames[ st ] : "?",
			( i == 0 || st >= CA_PRIMED ) ? clx[ i ]->clConn.clientNum : -1,
			r.x, r.y, r.w, r.h, clx[ i ]->cgameVM ? "yes" : "no", menu,
			i == 0 ? Cvar_VariableString( "name" ) : CL_SplitShadow( i, "name", NULL )->string,
			CL_ProfileDescribe( i ) );
	}
}


/*
==================
CL_SplitDebug_f

splitdebug freeze|error|packeterror <n> (cheats only): simulate a dead
link or a failure of extra player n, to exercise error isolation.
==================
*/
static void CL_SplitDebug_f( void ) {
	const char *what = Cmd_Argv( 1 );
	const int n = atoi( Cmd_Argv( 2 ) ) - 1;

	if ( !Cvar_VariableIntegerValue( "sv_cheats" ) ) {
		Com_Printf( "splitdebug: cheats only\n" );
		return;
	}
	if ( n <= 0 || !CL_SlotInUse( n ) ) {
		Com_Printf( "usage: splitdebug freeze|error|packeterror|uierror|cgamecatch <player 2..8> | cursor <player 2..8> <x> <y>\n" );
		return;
	}
	if ( !Q_stricmp( what, "freeze" ) ) {
		clx[ n ]->debugFreeze = qtrue;
	} else if ( !Q_stricmp( what, "error" ) ) {
		clx[ n ]->debugError = 1;
	} else if ( !Q_stricmp( what, "packeterror" ) ) {
		clx[ n ]->debugError = 2;
	} else if ( !Q_stricmp( what, "uierror" ) ) {
		clx[ n ]->debugError = 3;	// inside its menu (next menu draw)
	} else if ( !Q_stricmp( what, "cgamecatch" ) ) {
		clx[ n ]->cgameCatcher = KEYCATCH_CGAME;	// as if its cgame caught keys (trap_Key_SetCatcher)
	} else if ( !Q_stricmp( what, "cursor" ) ) {
		// put its menu cursor at x y (menu units) through the pad cursor path: corner first
		CL_SplitUIMouseEvent( n, -4000, -4000 );
		CL_SplitUIMouseEvent( n, atoi( Cmd_Argv( 3 ) ), atoi( Cmd_Argv( 4 ) ) );
	} else {
		Com_Printf( "splitdebug: unknown '%s'\n", what );
		return;
	}
	Com_Printf( "splitdebug: %s P%i\n", what, n + 1 );
}


/*
==================
CL_SplitAppend_f

splitappend <command text> (developer only): append to the end of the command
buffer, behind what menus queued with EXEC_APPEND (spmap, quit ...), so a
test script can continue after a menu's command ran.
==================
*/
static void CL_SplitAppend_f( void ) {
	if ( !com_developer || !com_developer->integer ) {
		Com_Printf( "splitappend: developer only\n" );
		return;
	}
	Cbuf_AddText( va( "%s\n", Cmd_ArgsFrom( 1 ) ) );
}


/*
==================
CL_InitSplitscreen
==================
*/
void CL_InitSplitscreen( void ) {
	int i;

	ctxStackDepth = 0;
	splitGuard = NULL;
	for ( i = 1; i <= MAX_SPLITVIEW; i++ ) {
		Cmd_AddCommand( va( "p%i", i ), CL_SplitPlayerCmd_f );
	}
	Cmd_AddCommand( "addplayer", CL_AddPlayer_f );
	Cmd_AddCommand( "dropplayer", CL_DropPlayer_f );
	Cmd_AddCommand( "splitplayers", CL_SplitPlayers_f );
	Cmd_AddCommand( "splitdebug", CL_SplitDebug_f );
	Cmd_AddCommand( "splitappend", CL_SplitAppend_f );

	cl_splitOverlayBars = Cvar_Get( "cl_splitOverlayBars", "1", CVAR_TEMP );
	Cvar_CheckRange( cl_splitOverlayBars, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_splitOverlayBars, "Splitscreen, HUD shape 4:3 Centered: 1 - a full-screen overlay of a player's game (scope, flash) also covers the bars beside its 4:3 area; 0 - off (R18 debug)." );
	cl_splitAspect = Cvar_Get( "cl_splitAspect", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitAspect, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_splitAspect, "Splitscreen: screen size each player's cgame is told.\n"
		" 0 - the viewport itself (wide/tall cells stretch Q3-era HUDs and narrow their FOV)\n"
		" 1 - a 4:3 screen centered in the viewport; the 3D view still fills it (wider FOV)" );

	cl_splitFill = Cvar_Get( "cl_splitFill", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitFill, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_splitFill, "Splitscreen: use the whole screen.\n"
		" 0 - equal grid cells (e.g. 3 players = 2x2 with a black cell)\n"
		" 1 - the bottom row may hold fewer, wider views (3 players = 2 on top, 1 full-width below)" );

	cl_splitVertical = Cvar_Get( "cl_splitVertical", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitVertical, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_splitVertical, "Splitscreen, 2 players: 0 - top/bottom, 1 - side by side." );

	cl_splitWidePlayer = Cvar_Get( "cl_splitWidePlayer", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitWidePlayer, "0", va( "%i", MAX_SPLITVIEW ), CV_INTEGER );
	Cvar_SetDescription( cl_splitWidePlayer, "Splitscreen with cl_splitFill 1: player number that gets the wide "
		"bottom cell (0 - the last player who joined)." );

	cl_splitMaxPlayers = Cvar_Get( "cl_splitMaxPlayers", "8", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitMaxPlayers, "1", va( "%i", MAX_SPLITVIEW ), CV_INTEGER );
	Cvar_SetDescription( cl_splitMaxPlayers, "Splitscreen: most local players (addplayer)." );

	cl_splitTestOnDisconnect = Cvar_Get( "cl_splitTestOnDisconnect", "", CVAR_TEMP );
	Cvar_SetDescription( cl_splitTestOnDisconnect, "Test scripts (developer only): command text run once when player 1's "
		"connection ends, after the error that ended it cleared the command buffer." );

	splitLayoutDirty = qtrue;

	IN_GamepadInit();
	CL_AimAssistInit();
	CL_SplitMenuInit();
	CL_SplitSrvInit();
	CL_ProfileInit();
	CL_IndepInit();
}


/*
==================
CL_ShutdownSplitscreen

Back to context 0; extra players are disconnected and released.
==================
*/
void CL_ShutdownSplitscreen( void ) {
	int i;

	CL_IndepShutdown();		// Independent mode: the other windows close first

	ctxStackDepth = 0;
	CL_SplitscreenDisconnect();
	CL_SplitReap();

	for ( i = 1; i <= MAX_SPLITVIEW; i++ ) {
		Cmd_RemoveCommand( va( "p%i", i ) );
	}
	Cmd_RemoveCommand( "addplayer" );
	Cmd_RemoveCommand( "dropplayer" );
	Cmd_RemoveCommand( "splitplayers" );
	Cmd_RemoveCommand( "splitdebug" );
	Cmd_RemoveCommand( "splitappend" );

	CL_ProfileShutdown();
	CL_SplitSrvShutdown();
	CL_SplitMenuShutdown();
	CL_AimAssistShutdown();
	IN_GamepadShutdown();
}
