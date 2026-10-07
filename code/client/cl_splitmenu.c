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
// cl_splitmenu.c -- engine-drawn per-player overlay (design doc 12.2, 13.4, 14.4)
//
// A small widget framework: every local player has a stack of pages; a page
// is a builder function that (re)creates its rows from the current state
// each time they are drawn or used, so values are always live and nothing
// needs invalidating.  Row types: action, toggle/choice, slider, submenu,
// key capture ("press a button"), static text, text entry (stub until the
// on-screen keyboard).  Drawn with the console font and filled rects into
// the owning player's cell (the whole screen when that player is alone or
// at the main menu), after every view and menu, so it is mod-agnostic.
//
// Input comes from the player's pad through in_gamepad.c (design 13.2):
// d-pad / left stick = arrows with repeat, A = select, B = back, right stick
// = this overlay's cursor, RT = click, Start = close.  A page can be pushed
// for any player slot at any time; it needs no cgame or ui VM.
//
// Pages here: pause (Resume / Game menu / Controls / Splitscreen (P1) /
// Leave), Controls (look, aim feel, toggles), Buttons (rebind by capture),
// host Splitscreen settings, leave confirmation.

#include "client.h"

#define SM_MAX_ROWS		256		// R14b: the map list (Change map lists at most this many)
#define SM_MAX_DEPTH	6
#define SM_COLS			46		// panel width in characters (fewer in a narrow cell)
#define SM_CAPTURE_TIME	8000	// msec a "press a button" prompt waits
#define SM_NOTE_TIME	3000

typedef enum {
	SMR_ACTION,		// A runs func
	SMR_CHOICE,		// left/right (and A) cycle the cvar through values[]
	SMR_SLIDER,		// left/right step the cvar between min and max
	SMR_SUBMENU,	// A pushes page
	SMR_CAPTURE,	// A asks for a pad button to bind cmd to
	SMR_TEXT,		// static, never selected
	SMR_TEXTENTRY	// A edits the text on the on-screen keyboard (a cvar, or func gets it)
} smRowType_t;

typedef struct smPage_s smPage_t;

typedef struct {
	smRowType_t		type;
	char			label[48];
	char			cvar[MAX_CVAR_VALUE_STRING];	// choice/slider/text entry: cvar; capture: command
	float			min, max, step;	// slider
	float			show;			// slider: displayed value = value * show
	const char		*fmt;			// slider: printf for the displayed value
	const char		*const *values;	// choice: cvar strings ...
	const char		*const *labels;	// ... and what is shown for them
	int				numValues;
	const smPage_t	*page;			// submenu
	void			(*func)( int n, int arg );	// action
	int				arg;
	int				target;			// submenu: settings target of the page (-1 = inherit)
	qboolean		(*textDone)( int n, const char *text );	// text entry without a cvar
	qboolean		feel;			// a per-player look/aim setting (kept in the profile)
	const char		*help;			// R14b: one line at the bottom while selected
	qboolean		helpWrap;		// R19: a longer help wraps onto a second line (else it is cut)
	qboolean		disabled;
} smRow_t;

struct smPage_s {
	const char		*name;
	void			(*build)( int n );
	void			(*enter)( int n );	// R14b: pushed
	void			(*leave)( int n );	// R14b: popped (back, or the overlay closed)
};

typedef struct {
	int				depth;			// 0 = closed
	struct {
		const smPage_t	*page;
		int			sel;
		int			top;
		int			target;		// whose settings the page edits: a player or SPLIT_GUEST_DEFAULTS
	} stack[SM_MAX_DEPTH];
	qboolean		inGame;			// opened while player 1 was in a game

	qboolean		capture;		// "press a button" prompt up
	char			captureCmd[MAX_CVAR_VALUE_STRING];
	char			captureName[48];
	int				captureTime;

	qboolean		cursorOn;		// the right stick moved the cursor
	float			cx, cy;			// cursor, pixels relative to the cell

	char			note[96];		// transient footer message
	int				noteTime;

	qboolean		joining;		// join-time profile picker: closing it cancels the join
	char			joinGuid[40];	// the joining pad (pre-highlight of its last profile)
	char			argName[32];	// the profile a confirmation page is about

	struct {						// on-screen keyboard (design 12.4), one per player
		char		title[48];
		char		text[64];
		int			max;
		int			row, col;
		qboolean	shift;
		qboolean	live;			// types straight into the player's mod menu
		char		cvar[MAX_CVAR_VALUE_STRING];	// done: set this cvar ...
		qboolean	(*done)( int n, const char *text );	// ... or call this (qfalse: stay open)
		int			x, y, kw, kh;	// last draw (cursor hit tests)
	} kb;

	// the top page's rows, rebuilt before every use
	char			title[64];
	smRow_t			rows[SM_MAX_ROWS];
	int				numRows;

	// last draw (cursor hit tests)
	viewRect_t		cell;
	int				rowX, rowY, rowW, lineH, visRows;
} smPlayer_t;

static smPlayer_t	sm[MAX_SPLITVIEW];
static smPlayer_t	*smb;			// the player whose page is being built
static smRow_t		smScratch;		// rows past SM_MAX_ROWS land here

static smPage_t smPause, smControls, smButtons, smHost, smLeave, smWindow;
static smPage_t smProfile, smPicker, smDeleteList, smDeleteConfirm, smKeyboard;
static smPage_t smServer, smSrvMaps, smSrvSets, smSrvDelete, smSrvDeleteConfirm;

static void SM_KeyboardKey( int n, int key );
static void SM_KeyboardMouse( int n );
static void SM_DrawKeyboard( int n, const viewRect_t *cell, int cw, int ch, int lineH, int cols );


// whose settings the top page edits (a player, or the Guest defaults)
static int SM_Target( int n ) {
	const smPlayer_t *s = &sm[n];
	return s->depth > 0 ? s->stack[ s->depth - 1 ].target : n;
}


// the player number shown for slot n (Independent mode: a player window's own player is its number)
static int SM_Num( int n ) {
	return ( n == 0 && CL_IndepChild() ) ? CL_IndepChildPlayer() : n + 1;
}


// "Player 2" / "Guest defaults": whose settings the top page edits
static const char *SM_Who( int n ) {
	static char buf[32];
	const int t = SM_Target( n );

	if ( t == SPLIT_GUEST_DEFAULTS ) {
		return "Guest defaults";
	}
	Com_sprintf( buf, sizeof( buf ), "Player %i", SM_Num( t ) );
	return buf;
}


static qboolean SM_OnKeyboard( int n ) {
	const smPlayer_t *s = &sm[n];
	return ( s->depth > 0 && s->stack[ s->depth - 1 ].page == &smKeyboard ) ? qtrue : qfalse;
}


static smPlayer_t *SM_Get( int n ) {
	return ( (unsigned)n < MAX_SPLITVIEW ) ? &sm[n] : NULL;
}


qboolean CL_SplitMenuOpen( int n ) {
	const smPlayer_t *s = SM_Get( n );
	return ( s && s->depth > 0 ) ? qtrue : qfalse;
}


qboolean CL_SplitMenuCapturing( int n ) {
	const smPlayer_t *s = SM_Get( n );
	return ( s && s->depth > 0 && s->capture ) ? qtrue : qfalse;
}


static void SM_Note( int n, const char *text ) {
	Q_strncpyz( sm[n].note, text, sizeof( sm[n].note ) );
	sm[n].noteTime = cls.realtime;
	Com_Printf( "P%i menu: %s\n", n + 1, text );
}


/*
=============================================================================

ROW BUILDERS (used by the page build functions)

=============================================================================
*/

static smRow_t *SM_Row( smRowType_t type, const char *label ) {
	smRow_t *r;

	if ( smb->numRows >= SM_MAX_ROWS ) {
		r = &smScratch;
	} else {
		r = &smb->rows[ smb->numRows++ ];
	}
	Com_Memset( r, 0, sizeof( *r ) );
	r->type = type;
	Q_strncpyz( r->label, label, sizeof( r->label ) );
	return r;
}

static void SM_Title( const char *title ) {
	Q_strncpyz( smb->title, title, sizeof( smb->title ) );
}

static smRow_t *SM_Action( const char *label, void (*func)( int n, int arg ), int arg ) {
	smRow_t *r = SM_Row( SMR_ACTION, label );
	r->func = func;
	r->arg = arg;
	return r;
}

static smRow_t *SM_Sub( const char *label, const smPage_t *page ) {
	smRow_t *r = SM_Row( SMR_SUBMENU, label );
	r->page = page;
	r->target = -1;
	return r;
}

static smRow_t *SM_Text( const char *text ) {
	return SM_Row( SMR_TEXT, text );
}

static smRow_t *SM_Choice( const char *label, const char *cvar, const char *const *values, const char *const *labels, int num ) {
	smRow_t *r = SM_Row( SMR_CHOICE, label );
	Q_strncpyz( r->cvar, cvar, sizeof( r->cvar ) );
	r->values = values;
	r->labels = labels;
	r->numValues = num;
	return r;
}

static smRow_t *SM_Slider( const char *label, const char *cvar, float min, float max, float step, const char *fmt, float show ) {
	smRow_t *r = SM_Row( SMR_SLIDER, label );
	Q_strncpyz( r->cvar, cvar, sizeof( r->cvar ) );
	r->min = min;
	r->max = max;
	r->step = step;
	r->fmt = fmt;
	r->show = show;
	return r;
}

static smRow_t *SM_Capture( const char *label, const char *cmd ) {
	smRow_t *r = SM_Row( SMR_CAPTURE, label );
	Q_strncpyz( r->cvar, cmd, sizeof( r->cvar ) );
	return r;
}

// text entry (profile name, chat...): the row type exists; editing comes with
// the on-screen keyboard (design 12.4)
static smRow_t *SM_TextEntry( const char *label, const char *cvar ) {
	smRow_t *r = SM_Row( SMR_TEXTENTRY, label );
	Q_strncpyz( r->cvar, cvar, sizeof( r->cvar ) );
	return r;
}

// player n's shadow of a look/aim setting
static const char *SM_Feel( int n, const char *name ) {
	cvar_t *v = IN_PadFeelCvar( SM_Target( n ), name );
	return v ? v->name : name;
}

static smRow_t *SM_FeelRow( smRow_t *r ) {
	r->feel = qtrue;
	return r;
}

static smRow_t *SM_Help( smRow_t *r, const char *help ) {
	r->help = help;
	return r;
}


static qboolean SM_Selectable( const smRow_t *r ) {
	return ( r->type != SMR_TEXT && !r->disabled ) ? qtrue : qfalse;
}


// rebuild player n's top page and keep its selection on a usable row
static void SM_Build( int n ) {
	smPlayer_t *s = &sm[n];
	int *sel, i;

	if ( s->depth <= 0 ) {
		return;
	}
	smb = s;
	s->numRows = 0;
	s->title[0] = '\0';
	s->stack[ s->depth - 1 ].page->build( n );
	smb = NULL;

	sel = &s->stack[ s->depth - 1 ].sel;
	if ( *sel >= s->numRows ) {
		*sel = s->numRows - 1;
	}
	if ( *sel < 0 ) {
		*sel = 0;
	}
	for ( i = 0; i < s->numRows && !SM_Selectable( &s->rows[ ( *sel + i ) % s->numRows ] ); i++ )
		;
	if ( i < s->numRows ) {
		*sel = ( *sel + i ) % s->numRows;
	}
}


/*
=============================================================================

ROW VALUES AND CHANGES

=============================================================================
*/

static int SM_ChoiceIndex( const smRow_t *r ) {
	const char *v = Cvar_VariableString( r->cvar );
	int i;

	for ( i = 0; i < r->numValues; i++ ) {
		if ( !Q_stricmp( v, r->values[i] ) ) {
			return i;
		}
	}
	for ( i = 0; i < r->numValues; i++ ) {
		if ( v[0] && Q_isanumber( v ) && Q_isanumber( r->values[i] ) && atof( v ) == atof( r->values[i] ) ) {
			return i;
		}
	}
	return -1;
}


// the value shown on the right of row r for player n
static void SM_RowValue( int n, const smRow_t *r, char *buf, int size ) {
	int i, k, len;

	buf[0] = '\0';
	switch ( r->type ) {
	case SMR_CHOICE:
		i = SM_ChoiceIndex( r );
		Q_strncpyz( buf, i >= 0 ? r->labels[i] : Cvar_VariableString( r->cvar ), size );
		break;
	case SMR_SLIDER:
		Com_sprintf( buf, size, r->fmt, Cvar_VariableValue( r->cvar ) * r->show );
		break;
	case SMR_TEXTENTRY:
		Q_strncpyz( buf, Cvar_VariableString( r->cvar ), size );
		break;
	case SMR_SUBMENU:
		Q_strncpyz( buf, ">", size );
		break;
	case SMR_CAPTURE:
		// the buttons bound to this command ("A, X, +2" when they don't fit)
		for ( k = 0, len = 0, i = 0; k < IN_PadNumKeys(); k++ ) {
			const char *b = IN_PadGetBind( SM_Target( n ), k );
			const char *label;
			if ( !b || Q_stricmp( b, r->cvar ) ) {
				continue;
			}
			label = IN_PadKeyLabel( n, k );
			if ( i == 0 && len + (int)strlen( label ) + 2 + 4 < size ) {
				len += Com_sprintf( buf + len, size - len, "%s%s", len ? ", " : "", label );
			} else {
				i++;	// counted, not shown
			}
		}
		if ( i > 0 ) {
			Com_sprintf( buf + len, size - len, "%s+%i", len ? ", " : "", i );
		} else if ( !len ) {
			Q_strncpyz( buf, "--", size );
		}
		break;
	default:
		break;
	}
}


static void SM_SetCvar( int n, const smRow_t *r, const char *value ) {
	char shown[64];

	Cvar_Set( r->cvar, value );
	if ( r->feel ) {
		IN_PadFeelChanged( SM_Target( n ) );
	}
	SM_RowValue( n, r, shown, sizeof( shown ) );
	Com_Printf( "P%i menu: %s = %s (%s %s)\n", n + 1, r->label, shown, r->cvar, Cvar_VariableString( r->cvar ) );
	if ( strstr( r->cvar, "joy_cursorSpeed" ) && SM_Target( n ) < MAX_SPLITVIEW ) {
		Com_Printf( "P%i cursor speed: these menus %.0f, the game's menus %.0f menu units/s at full push\n", SM_Target( n ) + 1,
			IN_PadCursorSpeed( SM_Target( n ), qfalse ), IN_PadCursorSpeed( SM_Target( n ), qtrue ) );
	}
}


// left/right on a row (dir -1 / +1)
static void SM_Change( int n, const smRow_t *r, int dir ) {
	float v;
	int i;

	switch ( r->type ) {
	case SMR_CHOICE:
		if ( r->numValues <= 0 ) {
			return;
		}
		i = SM_ChoiceIndex( r );
		i = ( i < 0 ) ? 0 : ( i + dir + r->numValues ) % r->numValues;
		SM_SetCvar( n, r, r->values[i] );
		break;
	case SMR_SLIDER:
		v = Cvar_VariableValue( r->cvar ) + dir * r->step;
		v = r->min + (float)floor( ( v - r->min ) / r->step + 0.5f ) * r->step;
		v = Com_Clamp( r->min, r->max, v );
		SM_SetCvar( n, r, va( "%g", v ) );
		break;
	default:
		break;
	}
}


// push a page; target -1: edits the same settings as the page below it
static void SM_PushTarget( int n, const smPage_t *page, int target ) {
	smPlayer_t *s = &sm[n];

	if ( s->depth >= SM_MAX_DEPTH ) {
		return;
	}
	if ( target < 0 ) {
		target = s->depth > 0 ? s->stack[ s->depth - 1 ].target : n;
	}
	s->stack[ s->depth ].page = page;
	s->stack[ s->depth ].sel = 0;
	s->stack[ s->depth ].top = 0;
	s->stack[ s->depth ].target = target;
	if ( page == &smPicker || page == &smDeleteList ) {
		CL_ProfileRefresh();	// files may have changed
	}
	s->depth++;
	s->capture = qfalse;
	if ( page->enter ) {
		page->enter( n );
	}
	SM_Build( n );
	Com_Printf( "P%i menu: %s\n", n + 1, s->title );
}


// pop pages down to 'depth', running their leave hooks (R14b: Server options applies)
static void SM_PopTo( int n, int depth ) {
	smPlayer_t *s = &sm[n];
	const smPage_t *page;

	while ( s->depth > depth ) {
		page = s->stack[ s->depth - 1 ].page;
		s->depth--;
		if ( page->leave ) {
			page->leave( n );
		}
	}
}


static void SM_Push( int n, const smPage_t *page ) {
	SM_PushTarget( n, page, -1 );
}


static void SM_Back( int n ) {
	smPlayer_t *s = &sm[n];

	if ( s->depth <= 1 ) {
		CL_SplitMenuClose( n );
		return;
	}
	s->capture = qfalse;
	SM_PopTo( n, s->depth - 1 );
}


/*
==================
SM_OpenKeyboard

Push the on-screen keyboard for player n: 'text' to start from, at most
'max' characters; done() gets the result (or the cvar is set).
==================
*/
static void SM_OpenKeyboard( int n, const char *title, const char *text, int max,
	const char *cvar, qboolean (*done)( int n, const char *text ) ) {
	smPlayer_t *s = &sm[n];

	Com_Memset( &s->kb, 0, sizeof( s->kb ) );
	Q_strncpyz( s->kb.title, title, sizeof( s->kb.title ) );
	s->kb.max = MAX( 1, MIN( max, (int)sizeof( s->kb.text ) - 1 ) );
	Q_strncpyz( s->kb.text, text ? text : "", s->kb.max + 1 );
	Q_strncpyz( s->kb.cvar, cvar ? cvar : "", sizeof( s->kb.cvar ) );
	s->kb.done = done;
	s->kb.row = 1;		// on the 'q'
	SM_Push( n, &smKeyboard );
}


static void SM_Activate( int n, const smRow_t *r ) {
	smPlayer_t *s = &sm[n];

	if ( !SM_Selectable( r ) ) {
		return;
	}
	switch ( r->type ) {
	case SMR_ACTION:
		r->func( n, r->arg );
		break;
	case SMR_SUBMENU:
		SM_PushTarget( n, r->page, r->target );
		break;
	case SMR_CHOICE:
		SM_Change( n, r, 1 );
		break;
	case SMR_CAPTURE:
		s->capture = qtrue;
		s->captureTime = cls.realtime;
		Q_strncpyz( s->captureCmd, r->cvar, sizeof( s->captureCmd ) );
		Q_strncpyz( s->captureName, r->label, sizeof( s->captureName ) );
		Com_Printf( "P%i menu: press a button for %s\n", n + 1, r->label );
		break;
	case SMR_TEXTENTRY:
		SM_OpenKeyboard( n, r->label, r->cvar[0] ? Cvar_VariableString( r->cvar ) : "", r->arg > 0 ? r->arg : 31,
			r->cvar[0] ? r->cvar : NULL, r->textDone );
		break;
	default:
		break;
	}
}


/*
=============================================================================

OPEN / CLOSE / INPUT

=============================================================================
*/

static void SM_Open( int n, const smPage_t *page ) {
	smPlayer_t *s = SM_Get( n );

	if ( !s ) {
		return;
	}
	if ( s->joining ) {
		CL_SplitMenuClose( n );	// a pending join is cancelled, not hidden
	}
	SM_PopTo( n, 0 );
	s->inGame = ( cls.state == CA_ACTIVE ) ? qtrue : qfalse;
	s->capture = qfalse;
	s->cursorOn = qfalse;
	s->note[0] = '\0';
	s->cx = s->cy = -1.0f;	// centered on the first draw
	s->joining = qfalse;
	s->kb.live = qfalse;
	SM_PushTarget( n, page, n );
}


void CL_SplitMenuPause( int n ) {
	if ( !SM_Get( n ) ) {
		return;
	}
	if ( CL_SplitMenuOpen( n ) ) {
		CL_SplitMenuClose( n );
		return;
	}
	SM_Open( n, &smPause );
}


void CL_SplitMenuSettings( int n ) {
	if ( n != 0 ) {
		Com_Printf( "P%i: the splitscreen settings belong to player 1 (the host)\n", n + 1 );
		return;
	}
	if ( CL_IndepChild() ) {
		SM_Open( 0, &smWindow );	// host settings come from player 1's window
		return;
	}
	SM_Open( 0, &smHost );
}


/*
==================
CL_SplitMenuClose

Closing the join-time picker (B at its top, Start, the pad unplugged)
cancels that join: the held slot and its cell go away.
==================
*/
void CL_SplitMenuClose( int n ) {
	smPlayer_t *s = SM_Get( n );

	if ( !s || s->depth <= 0 ) {
		return;
	}
	s->capture = qfalse;
	SM_PopTo( n, 0 );
	s->kb.live = qfalse;
	Com_Printf( "P%i menu: closed\n", n + 1 );
	if ( s->joining ) {
		s->joining = qfalse;
		Com_Printf( "P%i: join cancelled\n", n + 1 );
		if ( CL_IndepPicking( n ) ) {
			CL_IndepCancel( n );	// Independent mode: no window
		} else {
			CL_SplitRequestDrop( n, "join cancelled" );
		}
	}
}


// a profile picked at the join-time picker: the player connects (Independent
// mode: its window starts with that profile)
static void SM_JoinDone( int n ) {
	smPlayer_t *s = &sm[n];

	s->joining = qfalse;
	CL_SplitMenuClose( n );
	if ( CL_IndepPicking( n ) ) {
		CL_IndepSpawn( n, CL_ProfileIsGuest( n ) ? NULL : CL_ProfileName( n ) );
		return;
	}
	CL_SplitConnectPlayer( n );
}


// set the selection of the top page to the action row (func, arg)
static void SM_Select( int n, void (*func)( int n, int arg ), int arg ) {
	smPlayer_t *s = &sm[n];
	int i;

	SM_Build( n );
	for ( i = 0; i < s->numRows; i++ ) {
		if ( s->rows[i].type == SMR_ACTION && s->rows[i].func == func && s->rows[i].arg == arg && SM_Selectable( &s->rows[i] ) ) {
			s->stack[ s->depth - 1 ].sel = i;
			return;
		}
	}
}

static void SM_Pick( int n, int arg );


/*
==================
CL_SplitMenuJoin

A pad finished the join hold: its held slot shows the profile picker
(Guest / saved profiles / New profile) before the player connects.  The
pad's last profile is pre-highlighted.
==================
*/
void CL_SplitMenuJoin( int n, const char *guid ) {
	smPlayer_t *s = SM_Get( n );
	int last;

	if ( !s || n <= 0 ) {
		return;
	}
	CL_ProfileRefresh();
	SM_Open( n, &smPicker );
	s->joining = qtrue;
	Q_strncpyz( s->joinGuid, guid ? guid : "", sizeof( s->joinGuid ) );
	last = CL_ProfilePadLast( guid );
	SM_Select( n, SM_Pick, ( last >= 0 && CL_ProfileListUser( last ) < 0 ) ? last : -1 );
	SM_Build( n );
	Com_Printf( "P%i menu: %s (pad's last profile: %s)\n", n + 1, s->title, last >= 0 ? CL_ProfileListName( last ) : "none" );
}


// hold Y in a mod menu: the keyboard types straight into it
void CL_SplitMenuKeyboard( int n ) {
	smPlayer_t *s = SM_Get( n );

	if ( !s || CL_SplitMenuOpen( n ) || !CL_SplitUIMenuOpen( n ) ) {
		return;
	}
	SM_Open( n, &smKeyboard );
	Com_Memset( &s->kb, 0, sizeof( s->kb ) );
	Q_strncpyz( s->kb.title, "Type into the menu", sizeof( s->kb.title ) );
	s->kb.max = 31;
	s->kb.row = 1;
	s->kb.live = qtrue;
	SM_Build( n );
	Com_Printf( "P%i menu: keyboard for the game menu\n", n + 1 );
}


// the row under the cursor (last draw's geometry), or -1
static int SM_HoverRow( const smPlayer_t *s ) {
	int i;

	if ( !s->cursorOn || s->lineH <= 0 || s->cx < s->rowX - s->cell.x || s->cx >= s->rowX - s->cell.x + s->rowW ) {
		return -1;
	}
	i = (int)floor( ( s->cy - ( s->rowY - s->cell.y ) ) / s->lineH );
	if ( i < 0 || i >= s->visRows ) {
		return -1;
	}
	i += s->stack[ s->depth - 1 ].top;
	return ( i < s->numRows && SM_Selectable( &s->rows[i] ) ) ? i : -1;
}


void CL_SplitMenuKey( int n, int key ) {
	smPlayer_t *s = SM_Get( n );
	int *sel, i, h;

	if ( !s || s->depth <= 0 || s->capture ) {
		return;
	}
	if ( SM_OnKeyboard( n ) ) {
		SM_KeyboardKey( n, key );
		return;
	}
	SM_Build( n );
	sel = &s->stack[ s->depth - 1 ].sel;

	switch ( key ) {
	case K_UPARROW:
	case K_DOWNARROW:
		for ( i = 1; i <= s->numRows; i++ ) {
			h = ( *sel + ( key == K_UPARROW ? -i : i ) + s->numRows * 2 ) % s->numRows;
			if ( SM_Selectable( &s->rows[h] ) ) {
				*sel = h;
				break;
			}
		}
		break;
	case K_LEFTARROW:
	case K_RIGHTARROW:
		if ( *sel < s->numRows ) {
			SM_Change( n, &s->rows[ *sel ], key == K_LEFTARROW ? -1 : 1 );
		}
		break;
	case K_ENTER:
		if ( *sel < s->numRows ) {
			SM_Activate( n, &s->rows[ *sel ] );
		}
		break;
	case K_MOUSE1:
		h = SM_HoverRow( s );
		if ( h >= 0 ) {
			*sel = h;
			SM_Activate( n, &s->rows[h] );
		}
		break;
	case K_ESCAPE:
	case K_MOUSE2:
		SM_Back( n );
		break;
	default:
		break;
	}
}


void CL_SplitMenuMouse( int n, int dx, int dy ) {
	smPlayer_t *s = SM_Get( n );
	float scale;
	int h;

	if ( !s || s->depth <= 0 || s->cell.w <= 0 ) {
		return;
	}
	scale = s->cell.w / 640.0f;	// menu units: 640 = cell width
	s->cursorOn = qtrue;
	s->cx = Com_Clamp( 0.0f, s->cell.w - 1.0f, s->cx + dx * scale );
	s->cy = Com_Clamp( 0.0f, s->cell.h - 1.0f, s->cy + dy * scale );
	if ( SM_OnKeyboard( n ) ) {
		SM_KeyboardMouse( n );
		return;
	}
	h = SM_HoverRow( s );
	if ( h >= 0 && !s->capture ) {
		s->stack[ s->depth - 1 ].sel = h;
	}
}


/*
==================
CL_SplitMenuCapture

The "press a button" prompt got pad key 'key'.  It takes the action; the
action's previous button gets what 'key' did before (a swap), or nothing.
Start / Guide cancel.
==================
*/
void CL_SplitMenuCapture( int n, int key ) {
	smPlayer_t *s = SM_Get( n );
	char old[MAX_CVAR_VALUE_STRING];
	const char *b;
	int k, prev, t;

	if ( !s || !s->capture ) {
		return;
	}
	s->capture = qfalse;
	t = SM_Target( n );	// a player, or the Guest defaults

	if ( !IN_PadBindableKey( key ) ) {
		SM_Note( n, "Cancelled" );
		return;
	}
	b = IN_PadGetBind( t, key );
	Q_strncpyz( old, b ? b : "", sizeof( old ) );
	if ( !Q_stricmp( old, s->captureCmd ) ) {
		SM_Note( n, va( "%s is already on %s", s->captureName, IN_PadKeyLabel( n, key ) ) );
		return;
	}
	if ( !Q_stricmp( old, "padmenu" ) ) {
		SM_Note( n, va( "%s opens this menu: pick another button", IN_PadKeyLabel( n, key ) ) );
		return;
	}

	// the action's first other button
	for ( prev = -1, k = 0; k < IN_PadNumKeys(); k++ ) {
		b = IN_PadGetBind( t, k );
		if ( k != key && b && !Q_stricmp( b, s->captureCmd ) ) {
			prev = k;
			break;
		}
	}

	IN_PadSetBind( t, key, s->captureCmd );
	if ( prev >= 0 ) {
		IN_PadSetBind( t, prev, old[0] ? old : NULL );
	}
	if ( old[0] && prev >= 0 ) {
		SM_Note( n, va( "%s: %s (swapped with %s)", s->captureName, IN_PadKeyLabel( n, key ), IN_PadKeyLabel( n, prev ) ) );
	} else {
		SM_Note( n, va( "%s: %s", s->captureName, IN_PadKeyLabel( n, key ) ) );
	}
}


static int SM_PlayerTeam( int n, int *gametype );
static const char *SM_TeamName( int team );

// R17: one line whenever a player's team (PERS_TEAM) changes
static void SM_TeamWatch( void ) {
	static int last[MAX_SPLITVIEW] = { -2, -2, -2, -2, -2, -2, -2, -2 };
	int n, team, gt = 0;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		team = ( n == 0 || CL_SplitSlotActive( n ) ) ? SM_PlayerTeam( n, &gt ) : -1;
		if ( team != last[n] ) {
			if ( team >= 0 ) {
				Com_Printf( "P%i team: %s (PERS_TEAM %i, g_gametype %i)\n", n + 1, SM_TeamName( team ), team, gt );
			}
			last[n] = team;
		}
	}
}


/*
==================
CL_SplitMenuFrame

Close overlays whose owner is gone or whose moment passed.
==================
*/
void CL_SplitMenuFrame( void ) {
	smPlayer_t *s;
	int n;

	CL_SplitUIModelCheck();	// R14a: "Change player model" reached the model page?
	SM_TeamWatch();			// R17: "P<n> team: ..." lines
	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		s = &sm[n];
		if ( s->depth <= 0 ) {
			continue;
		}
		if ( n > 0 && !CL_SplitSlotActive( n ) && !CL_IndepPicking( n ) ) {
			s->depth = 0;	// the player left (or its join was cancelled)
			s->joining = qfalse;
			continue;
		}
		// the game ended / started under it (an in-place cgame restart or a
		// level change keeps the overlay; only the main menu state counts)
		if ( n == 0 && ( s->inGame == ( cls.state <= CA_DISCONNECTED ) ) ) {
			CL_SplitMenuClose( 0 );
			continue;
		}
		if ( s->kb.live && !CL_SplitUIMenuOpen( n ) ) {
			CL_SplitMenuClose( n );	// the menu it typed into is gone
			continue;
		}
		if ( n == 0 && s->inGame && ( Key_GetCatcher() & KEYCATCH_UI ) && !s->kb.live ) {
			CL_SplitMenuClose( 0 );	// the mod's menu took over (keyboard Esc)
			continue;
		}
		if ( s->capture && cls.realtime - s->captureTime > SM_CAPTURE_TIME ) {
			s->capture = qfalse;
			SM_Note( n, "No button pressed" );
		}
	}
}


/*
=============================================================================

PAGES

=============================================================================
*/

static const char *smOffOn[] = { "Off", "On" };
static const char *sm01[] = { "0", "1" };
static const char *sm10[] = { "1", "0" };

// --- pause ---

static void SM_Resume( int n, int arg ) {
	CL_SplitMenuClose( n );
}

static void SM_GameMenu( int n, int arg ) {
	if ( n == 0 ) {
		if ( cls.state == CA_ACTIVE && uivm && !uivm->callLevel ) {
			CL_SplitMenuClose( 0 );
			VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_INGAME );
		}
		return;
	}
	CL_SplitUIOpen( n, UIMENU_INGAME );
	if ( CL_SplitUIMenuOpen( n ) ) {
		CL_SplitMenuClose( n );
	} else {
		SM_Note( n, "The game menu is not available" );
	}
}

/*
==================
R17: join / spectate rows (the maintainer's Team Arena pass: every player came in as
a spectator in a team game -- g_teamAutoJoin 0 -- and the game's own menu
was no way in with a pad).  Baseq3 / missionpack layout: the player's team
is its snapshot's ps.persistant[PERS_TEAM] (index 3; TEAM_FREE 0, RED 1,
BLUE 2, SPECTATOR 3), the game type the server info's g_gametype (team
games >= 3).  The rows send "team <x>" as that player (its own reliable
commands); ioquake3 SetTeam takes free / red / blue / spectator.  "Auto"
counts the teams in the player configstrings (key "t") and joins the smaller
one (red on a tie).  Not in Urban Terror (its own team menu).
==================
*/
#define SM_PERS_TEAM		3
#define SM_TEAM_SPECTATOR	3
#define SM_CS_PLAYERS		544		// baseq3 and missionpack

// player n's team (-1: unknown / not in a game) and the server's game type
static int SM_PlayerTeam( int n, int *gametype ) {
	int team = -1, gt = 0;

	*gametype = 0;
	if ( (unsigned)n >= MAX_SPLITVIEW || !clx[ n ] || CL_SplitSrvUrT() ) {
		return -1;
	}
	CL_PushContext( n );
	if ( cls.state == CA_ACTIVE && cl.snap.valid ) {
		const int ofs = cl.gameState.stringOffsets[ CS_SERVERINFO ];
		gt = ofs ? atoi( Info_ValueForKey( cl.gameState.stringData + ofs, "g_gametype" ) ) : 0;
		team = cl.snap.ps.persistant[ SM_PERS_TEAM ];
		if ( cl.snap.ps.clientNum != clc.clientNum ) {
			// following someone: the snapshot is theirs; our own configstring says spectator
			const int pofs = cl.gameState.stringOffsets[ SM_CS_PLAYERS + clc.clientNum ];
			team = pofs ? atoi( Info_ValueForKey( cl.gameState.stringData + pofs, "t" ) ) : SM_TEAM_SPECTATOR;
		}
	}
	CL_PopContext();
	*gametype = gt;
	return team;
}

static const char *SM_TeamName( int team ) {
	switch ( team ) {
	case 0: return "free";
	case 1: return "red";
	case 2: return "blue";
	case SM_TEAM_SPECTATOR: return "spectator";
	default: return "?";
	}
}

// arg: 0 free, 1 red, 2 blue, 3 spectator, 4 auto (the smaller team)
static void SM_JoinTeam( int n, int arg ) {
	int team = arg, red = 0, blue = 0, i, gt;
	const int before = SM_PlayerTeam( n, &gt );

	if ( arg == 4 ) {
		CL_PushContext( n );
		for ( i = 0; i < MAX_CLIENTS; i++ ) {
			const int ofs = cl.gameState.stringOffsets[ SM_CS_PLAYERS + i ];
			const char *info = cl.gameState.stringData + ofs;
			if ( !ofs || !info[0] || i == clc.clientNum ) {
				continue;
			}
			switch ( atoi( Info_ValueForKey( info, "t" ) ) ) {
			case 1: red++; break;
			case 2: blue++; break;
			default: break;
			}
		}
		CL_PopContext();
		team = ( blue < red ) ? 2 : 1;
		Com_Printf( "P%i menu: auto join: red %i, blue %i -> %s\n", n + 1, red, blue, SM_TeamName( team ) );
	}
	CL_SplitMenuClose( n );
	Com_Printf( "P%i menu: team %s (now %s, PERS_TEAM %i)\n", n + 1, SM_TeamName( team ), SM_TeamName( before ), before );
	CL_PushContext( n );
	CL_AddReliableCommand( va( "team %s", SM_TeamName( team ) ), qfalse );
	CL_PopContext();
}

// the pause page's join rows for a spectating player n
static void SM_TeamRows( int n ) {
	int gt;
	const int team = SM_PlayerTeam( n, &gt );

	if ( team < 0 ) {
		return;
	}
	if ( team == SM_TEAM_SPECTATOR ) {
		if ( gt >= 3 ) {
			SM_Help( SM_Action( "Join red", SM_JoinTeam, 1 ), "You are spectating: join the red team." );
			SM_Help( SM_Action( "Join blue", SM_JoinTeam, 2 ), "You are spectating: join the blue team." );
			SM_Help( SM_Action( "Auto join (smaller team)", SM_JoinTeam, 4 ), "You are spectating: join the team with fewer players." );
		} else {
			SM_Help( SM_Action( "Join game", SM_JoinTeam, 0 ), gt == 1 ? "You are spectating: wait for the next match." : "You are spectating: join the game." );
		}
	}
}

// a player on a team in a team game can go back to spectating (above Leave)
static void SM_SpectateRow( int n ) {
	int gt;
	const int team = SM_PlayerTeam( n, &gt );

	if ( team >= 0 && team != SM_TEAM_SPECTATOR && gt >= 3 ) {
		SM_Help( SM_Action( "Spectate", SM_JoinTeam, SM_TEAM_SPECTATOR ), "Leave your team and watch." );
	}
}

static void SM_BuildPause( int n ) {
	SM_Title( va( "Player %i", SM_Num( n ) ) );
	SM_Action( "Resume", SM_Resume, 0 );
	SM_TeamRows( n );	// R17: spectators join here
	SM_Action( "Game menu", SM_GameMenu, 0 );
	SM_Sub( "Controls", &smControls );
	if ( n > 0 || CL_ProfileActive( 0 ) || Q_stricmp( Cvar_VariableString( "cl_splitP1Input" ), "kbm" ) ) {
		SM_Sub( va( "Profile: %s", CL_ProfileActive( n ) ? CL_ProfileName( n ) : "none" ), &smProfile );
	}
	if ( CL_IndepActive() ) {
		// Independent mode: every window mixes its own sound
		SM_Slider( "Window volume", "s_volume", 0, 1, 0.05f, "%.0f%%", 100 );
	}
	if ( n == 0 && !CL_IndepChild() ) {
		SM_Sub( "Splitscreen settings", &smHost );
	}
	if ( n == 0 ) {
		// R14b: the local listen server's options (every window's own match in Independent mode)
		if ( CL_SplitRemoteServer() ) {
			SM_Sub( "Server options - host only", &smServer )->disabled = qtrue;
		} else {
			SM_Sub( "Server options", &smServer );
		}
	}
	SM_SpectateRow( n );	// R17
	SM_Sub( n == 0 ? "End game" : "Leave game", &smLeave );
}

static smPage_t smPause = { "pause", SM_BuildPause };

// --- Independent mode, a player's own window at its main menu (Start) ---

static void SM_ExitWindow( int n, int arg ) {
	CL_SplitMenuClose( n );
	Com_Printf( "P%i menu: Exit game: this window closes\n", CL_IndepChildPlayer() );
	Cbuf_AddText( "quit\n" );
}

static void SM_BuildWindow( int n ) {
	SM_Title( va( "Player %i - own window", CL_IndepChildPlayer() ) );
	SM_Sub( "Controls", &smControls );
	SM_Sub( va( "Profile: %s", CL_ProfileActive( n ) ? CL_ProfileName( n ) : "none" ), &smProfile );
	SM_Slider( "Window volume", "s_volume", 0, 1, 0.05f, "%.0f%%", 100 );
	SM_Action( "Exit game (close this window)", SM_ExitWindow, 0 );
	SM_Text( "" );
	SM_Text( "Splitscreen settings: in player 1's window" );
}

static smPage_t smWindow = { "window", SM_BuildWindow };

// --- leave ---

static void SM_DoLeave( int n, int arg ) {
	CL_SplitMenuClose( n );
	if ( n == 0 ) {
		Com_Printf( "P1 menu: ending the game for everyone\n" );
		Cbuf_ExecuteText( EXEC_INSERT, "disconnect\n" );	// next, before anything queued
	} else {
		CL_SplitRequestDrop( n, "left (menu)" );
	}
}

static void SM_Cancel( int n, int arg ) {
	SM_Back( n );
}

static void SM_BuildLeave( int n ) {
	if ( n == 0 ) {
		SM_Title( "End the game?" );
		SM_Text( "Everyone leaves; back to the main menu." );
		SM_Action( "Cancel", SM_Cancel, 0 );
		SM_Action( "End game for everyone", SM_DoLeave, 0 );
	} else {
		SM_Title( va( "Player %i: leave the game?", SM_Num( n ) ) );
		SM_Text( "The others keep playing." );
		SM_Action( "Cancel", SM_Cancel, 0 );
		SM_Action( "Leave game", SM_DoLeave, 0 );
	}
}

static smPage_t smLeave = { "leave", SM_BuildLeave };

// --- controls (per player, design 12.2 / 14.4) ---

static const char *smCurveValues[] = { "linear", "standard", "dynamic", "custom" };
static const char *smCurveLabels[] = { "Linear", "Standard", "Dynamic", "Custom" };
static const char *smCrouchLabels[] = { "Hold", "Toggle" };
static const char *smRunLabels[] = { "Always run", "Walk (L3 runs)" };
static const char *smAssistValues[] = { "0", "1", "2" };
static const char *smAssistLabels[] = { "Off", "Low", "Standard" };

static void SM_ResetFeel( int n, int arg ) {
	IN_PadResetFeel( SM_Target( n ) );
	SM_Note( n, "Controls reset to defaults" );
}

static void SM_BuildControls( int n ) {
	smRow_t *r;

	SM_Title( va( "%s - Controls", SM_Who( n ) ) );
	SM_FeelRow( SM_Slider( "Look speed horizontal", SM_Feel( n, "joy_yawSpeed" ), 60, 900, 20, "%.0f deg/s", 1 ) );
	SM_FeelRow( SM_Slider( "Look speed vertical", SM_Feel( n, "joy_pitchSpeed" ), 40, 600, 20, "%.0f deg/s", 1 ) );
	SM_FeelRow( SM_Choice( "Invert look up/down", SM_Feel( n, "joy_invertPitch" ), sm01, smOffOn, 2 ) );
	SM_FeelRow( SM_Choice( "Aim curve", SM_Feel( n, "joy_aimCurve" ), smCurveValues, smCurveLabels, 4 ) );
	r = SM_FeelRow( SM_Slider( "Custom curve exponent", SM_Feel( n, "joy_aimExponent" ), 0.5f, 4, 0.1f, "%.1f", 1 ) );
	r->disabled = Q_stricmp( Cvar_VariableString( SM_Feel( n, "joy_aimCurve" ) ), "custom" ) ? qtrue : qfalse;
	SM_FeelRow( SM_Slider( "Turn boost (full stick)", SM_Feel( n, "joy_turnBoost" ), 1, 3, 0.1f, "%.1fx", 1 ) );
	SM_FeelRow( SM_Slider( "Turn boost ramp", SM_Feel( n, "joy_turnBoostTime" ), 0, 1000, 50, "%.0f ms", 1 ) );
	SM_FeelRow( SM_Slider( "Look speed zoomed", SM_Feel( n, "joy_zoomScale" ), 0.1f, 1, 0.05f, "%.0f%%", 100 ) );
	SM_FeelRow( SM_Choice( "Aim smoothing", SM_Feel( n, "joy_aimSmooth" ), sm01, smOffOn, 2 ) );
	// aim assist (design 15): a player's row is greyed on another machine's server
	// or when the host forbids it; the Guest defaults stay editable
	if ( SM_Target( n ) != SPLIT_GUEST_DEFAULTS && CL_SplitRemoteServer() ) {
		r = SM_FeelRow( SM_Choice( "Aim assist - local games only", SM_Feel( n, "joy_aimAssist" ), smAssistValues, smAssistLabels, 3 ) );
		r->disabled = qtrue;
	} else if ( SM_Target( n ) != SPLIT_GUEST_DEFAULTS && !Cvar_VariableIntegerValue( "cl_aimAssistAllow" ) ) {
		r = SM_FeelRow( SM_Choice( "Aim assist - host: not allowed", SM_Feel( n, "joy_aimAssist" ), smAssistValues, smAssistLabels, 3 ) );
		r->disabled = qtrue;
	} else {
		SM_FeelRow( SM_Choice( "Aim assist", SM_Feel( n, "joy_aimAssist" ), smAssistValues, smAssistLabels, 3 ) );
	}
	SM_FeelRow( SM_Slider( "Inner deadzone", SM_Feel( n, "joy_deadzone" ), 0, 0.5f, 0.01f, "%.0f%%", 100 ) );
	SM_FeelRow( SM_Slider( "Outer deadzone", SM_Feel( n, "joy_deadzoneOuter" ), 0.5f, 1, 0.01f, "%.0f%%", 100 ) )->help =
		"Full speed at this stick tilt; lower = sooner.";
	SM_FeelRow( SM_Choice( "Crouch", SM_Feel( n, "joy_crouchToggle" ), sm01, smCrouchLabels, 2 ) );
	if ( CL_SplitSrvUrT() ) {	// R18: UrT sprint (+button8); crouch and sprint are per game
		SM_Help( SM_FeelRow( SM_Choice( "Sprint", SM_Feel( n, "joy_sprintToggle" ), sm01, smCrouchLabels, 2 ) ),
			"Toggle: click to sprint; stops when you stop." );
	}
	SM_FeelRow( SM_Choice( "Movement", SM_Feel( n, "joy_alwaysRun" ), sm10, smRunLabels, 2 ) );
	// R17: both right-stick cursors (the game's menus and these)
	SM_Help( SM_FeelRow( SM_Slider( "Cursor speed", SM_Feel( n, "joy_cursorSpeed" ), 0.5f, 2, 0.1f, "%.0f%%", 100 ) ),
		"Right-stick cursor, game menus and these menus." );
	SM_FeelRow( SM_Slider( "Field of view", SM_Feel( n, "joy_fov" ), 80, 130, 5, "%.0f", 1 ) );
	SM_Sub( "Button bindings", &smButtons );
	SM_Action( "Reset controls to defaults", SM_ResetFeel, 0 );
}

static smPage_t smControls = { "controls", SM_BuildControls };

// --- buttons ---

static const struct {
	const char	*cmd;
	const char	*name;
} smActionNames[] = {
	{ "+attack", "Fire" },
	{ "+zoom", "Zoom" },
	{ "+moveup", "Jump" },
	{ "+movedown", "Crouch" },
	{ "weapnext", "Next weapon" },
	{ "weapprev", "Previous weapon" },
	{ "+button2", "Use item" },
	{ "+button3", "Gesture" },
	{ "+scores", "Scores" },
	{ "+speed", "Walk / run" },
	{ "centerview", "Center view" },
	{ "+button4", "Walk" },
	{ "+button14", "Use" },
	{ "+button12", "Reload" },
	{ "+button5", "Throw" },
	{ "+lookup", "Look up" },
	{ "+lookdown", "Look down" },
	{ "+forward", "Forward" },
	{ "+back", "Back" },
	{ "+moveleft", "Strafe left" },
	{ "+moveright", "Strafe right" },
};

// R18: Urban Terror's actions (its ui/controls.menu and default.cfg), every one
// listed, bound or not, in this order (the maintainer's layout first)
static const struct {
	const char	*cmd;
	const char	*name;
} smUrTActions[] = {
	{ "+attack", "Fire" },
	{ "ut_zoomin", "Zoom in" },
	{ "+moveup", "Jump" },
	{ "+movedown", "Crouch" },
	{ "+button5", "Reload" },
	{ "+button6", "Bandage" },
	{ "ut_itemdrop", "Drop item" },
	{ "ut_weapdrop", "Drop weapon" },
	{ "ut_itemuse nvg", "IR vision (NVGs)" },
	{ "+button3", "Weapon mode" },
	{ "ut_weaptoggle knife", "Knife" },
	{ "ut_zoomreset", "Reset zoom" },
	{ "weapnext", "Next weapon" },
	{ "+button8", "Sprint" },
	{ "+scores", "Scores" },
	{ "ut_itemuse", "Use item" },
	{ "ut_zoomout", "Zoom out" },
	{ "weapprev", "Previous weapon" },
	{ "ut_weaptoggle primary", "Primary weapon" },
	{ "ut_weaptoggle secondary", "Secondary weapon" },
	{ "ut_weaptoggle sidearm", "Sidearm" },
	{ "ut_weaptoggle grenade", "Grenade" },
	{ "ut_weaptoggle bomb", "Bomb" },
	{ "+button7", "Interact / pick up" },
	{ "ut_itemnext", "Next item" },
	{ "ut_itemprev", "Previous item" },
	{ "ut_itemuse laser", "Laser sight" },
	{ "ut_itemdrop medkit", "Drop medkit" },
	{ "ut_itemdrop kevlar", "Drop kevlar" },
	{ "ut_itemdrop flag", "Drop flag" },
	{ "+speed", "Walk / run" },
	{ "maptoggle", "Minimap" },
	{ "ui_selectteam", "Team menu" },
	{ "ui_selectgear", "Gear menu" },
	{ "ui_radio", "Radio menu" },
	{ "ut_radio 1 1", "Radio: affirmative" },
	{ "ut_radio 1 2", "Radio: negative" },
	{ "ut_radio 2 6", "Radio: need backup" },
	{ "ut_radio 5 1", "Radio: enemy spotted" },
	{ "ut_radio 5 5", "Radio: incoming" },
	{ "ut_radio 9 9", "Radio: thanks" },
	{ "messagemode", "Chat" },
	{ "messagemode2", "Team chat" },
	{ "vote yes", "Vote yes" },
	{ "vote no", "Vote no" },
	{ "centerview", "Center view" },
};

static void SM_ResetBinds( int n, int arg ) {
	IN_PadResetBinds( SM_Target( n ) );
	SM_Note( n, "Buttons reset to defaults" );
}

// commands in the default set and the player's binds, known ones first
static void SM_BuildButtons( int n ) {
	const char *cmds[64];
	const char *b;
	int numCmds = 0, i, j, k, pass;

	SM_Title( va( "%s - Buttons", SM_Who( n ) ) );
	SM_Text( "Pick an action, then press its button." );

	for ( pass = 0; pass < 2; pass++ ) {
		for ( k = 0; k < IN_PadNumKeys(); k++ ) {
			b = pass ? IN_PadGetBind( SM_Target( n ), k ) : IN_PadDefaultBind( k );
			if ( !b || !b[0] || !Q_stricmp( b, "padmenu" ) ) {
				continue;
			}
			for ( i = 0; i < numCmds && Q_stricmp( cmds[i], b ); i++ )
				;
			if ( i == numCmds && numCmds < (int)ARRAY_LEN( cmds ) ) {
				cmds[ numCmds++ ] = b;
			}
		}
	}

	// R18: Urban Terror lists all its actions (unbound ones show "--")
	if ( CL_SplitSrvUrT() ) {
		for ( j = 0; j < (int)ARRAY_LEN( smUrTActions ); j++ ) {
			SM_Capture( smUrTActions[j].name, smUrTActions[j].cmd );
			for ( i = 0; i < numCmds; i++ ) {
				if ( cmds[i] && !Q_stricmp( cmds[i], smUrTActions[j].cmd ) ) {
					cmds[i] = NULL;
				}
			}
		}
	}

	for ( j = 0; j < (int)ARRAY_LEN( smActionNames ); j++ ) {
		for ( i = 0; i < numCmds; i++ ) {
			if ( cmds[i] && !Q_stricmp( cmds[i], smActionNames[j].cmd ) ) {
				SM_Capture( smActionNames[j].name, cmds[i] );
				cmds[i] = NULL;
			}
		}
	}
	for ( i = 0; i < numCmds; i++ ) {
		if ( cmds[i] ) {
			SM_Capture( cmds[i], cmds[i] );
		}
	}
	SM_Action( "Reset buttons to defaults", SM_ResetBinds, 0 );
}

static smPage_t smButtons = { "buttons", SM_BuildButtons };

// --- host splitscreen settings (design 13.4, 14.1, 14.2) ---

static const char *smFillLabels[] = { "Fill screen", "Equal grid" };
static const char *smVertLabels[] = { "Top / bottom", "Side by side" };
static const char *smAspectLabels[] = { "4:3 Centered", "Stretched" };
static const char *smJoinKeys[] = { "PAD_A", "PAD_B", "PAD_X", "PAD_Y", "PAD_LB", "PAD_RB", "PAD_BACK", "PAD_START" };
static const char *smHoldValues[] = { "0", "300", "500", "700", "1000", "1500" };
static const char *smHoldLabels[] = { "Press", "Hold 0.3 s", "Hold 0.5 s", "Hold 0.7 s", "Hold 1 s", "Hold 1.5 s" };
static const char *smHintValues[] = { "0", "1", "2" };
static const char *smHintLabels[] = { "Off", "When a pad is used", "10 s at start" };
static const char *smModeLabels[] = { "Together", "Independent (experimental)" };	// R19: Independent is experimental
static const char *smP1Values[] = { "pad", "kbm" };
static const char *smP1Labels[] = { "Gamepad", "Keyboard + mouse" };
static const char *smWideValues[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8" };
static const char *smWideLabels[] = { "Last joined", "Player 1", "Player 2", "Player 3", "Player 4",
	"Player 5", "Player 6", "Player 7", "Player 8" };

static void SM_BuildHost( int n ) {
	static char joinLabels[ ARRAY_LEN( smJoinKeys ) ][16];
	static const char *joinLabelPtrs[ ARRAY_LEN( smJoinKeys ) ];
	char join[64], leave[64];
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( smJoinKeys ); i++ ) {
		Q_strncpyz( joinLabels[i], IN_PadButtonName( 0, smJoinKeys[i] ), sizeof( joinLabels[i] ) );
		joinLabelPtrs[i] = joinLabels[i];
	}

	SM_Title( "Splitscreen settings" );
	SM_Choice( "Screen use", "cl_splitFill", sm10, smFillLabels, 2 );
	SM_Choice( "Wide view goes to", "cl_splitWidePlayer", smWideValues, smWideLabels,
		1 + MIN( Cvar_VariableIntegerValue( "cl_splitMaxPlayers" ), MAX_SPLITVIEW ) );
	SM_Choice( "Two players", "cl_splitVertical", sm01, smVertLabels, 2 );
	SM_Choice( "HUD shape", "cl_splitAspect", sm10, smAspectLabels, 2 );
	SM_Slider( "Menu size", "cl_splitMenuSize", 0.5f, 1.5f, 0.25f, "%.2fx", 1 );
	SM_Choice( "Join button", "cl_splitJoinButton", smJoinKeys, joinLabelPtrs, ARRAY_LEN( smJoinKeys ) );
	SM_Choice( "Join by", "cl_splitJoinHold", smHoldValues, smHoldLabels, ARRAY_LEN( smHoldValues ) );
	SM_Choice( "Join hint", "cl_splitJoinHint", smHintValues, smHintLabels, ARRAY_LEN( smHintValues ) );
	SM_Slider( "Max players", "cl_splitMaxPlayers", 1, MAX_SPLITVIEW, 1, "%.0f", 1 );
	SM_Choice( "Player 1 uses", "cl_splitP1Input", smP1Values, smP1Labels, 2 );
	SM_Choice( "Allow aim assist (local games)", "cl_aimAssistAllow", sm01, smOffOn, 2 );
	SM_Sub( "Guest defaults (controls, buttons)", &smControls )->target = SPLIT_GUEST_DEFAULTS;
	if ( Q_stricmp( Cvar_VariableString( "cl_splitP1Input" ), "kbm" ) ) {
		SM_Sub( va( "Player 1 profile: %s", CL_ProfileActive( 0 ) ? CL_ProfileName( 0 ) : "none" ), &smProfile )->target = 0;
	}
	// Together (one window) / Independent (a window and game per player, design 17.2)
	SM_Help( SM_Choice( "Session mode", "cl_splitIndependent", sm01, smModeLabels, 2 ),
		"Separate windows per player; still being made reliable" )->helpWrap = qtrue;	// R19: Independent is experimental
	if ( CL_IndepModeNote()[0] ) {
		SM_Text( CL_IndepModeNote() );
	}
	IN_PadShortcuts( join, sizeof( join ), leave, sizeof( leave ) );
	SM_Text( "" );
	SM_Text( join );
	SM_Text( leave );
	SM_Text( va( "Menu: %s", IN_PadButtonName( 0, "PAD_START" ) ) );
}

static smPage_t smHost = { "splitscreen", SM_BuildHost };

// --- R14b: Server options (design 18): player 1, the local listen server ---

static const char *smGtQ3Values[] = { "0", "3", "1", "4" };
static const char *smGtQ3Labels[] = { "Free for all", "Team deathmatch", "Tournament", "Capture the flag" };
static const char *smGtUrtValues[] = { "0", "1", "3", "4", "5", "6", "7", "8", "9", "10", "11" };
static const char *smGtUrtLabels[] = { "Free for all", "Last man standing", "Team deathmatch", "Team survivor",
	"Follow the leader", "Capture and hold", "Capture the flag", "Bomb", "Jump", "Freeze tag", "Gun game" };
static const char *smWeapValues[] = { "default", "random", "gauntlet", "machinegun", "shotgun", "grenade", "rocket",
	"lightning", "railgun", "plasma", "bfg" };
static const char *smWeapLabels[] = { "Default", "Random", "Gauntlet only", "Machinegun only", "Shotgun only",
	"Grenades only", "Rockets only", "Lightning only", "Railguns only", "Plasma only", "BFG only" };
static const char *smWeapNames[] = { "", "", "Gauntlet", "Machinegun", "Shotgun", "Grenade launcher",
	"Rocket launcher", "Lightning gun", "Railgun", "Plasma gun", "BFG" };
static const char *smSpawnValues[] = { "mg", "all", "gauntlet" };
static const char *smSpawnLabels[] = { "Machinegun", "All weapons", "Gauntlet only" };
static const char *smSkillValues[] = { "1", "2", "3", "4", "5", "0" };
static const char *smSkillLabels[] = { "I can win", "Bring it on", "Hurt me plenty", "Hardcore", "Nightmare", "Random (per bot)" };
static const char *smSelfLabels[] = { "On", "Off" };
static const char *smGravValues[] = { "800", "600", "400", "200" };
static const char *smGravLabels[] = { "Off", "Low (600)", "Medium (400)", "High (200)" };
static const char *smGodValues[] = { "0", "all", "1", "2", "3", "4", "5", "6", "7", "8" };
static const char *smGodLabels[] = { "Off", "All players", "Player 1", "Player 2", "Player 3", "Player 4",
	"Player 5", "Player 6", "Player 7", "Player 8" };

// number lists made once: time 0, 5..60; frags 0, 5..100; captures 0..20; bots 0..20; health 0, 95..5;
// weapon respawn 0, 1..30 s; player speed 50..200 % (no zero entry)
static char smNumText[7][32][16], smNumVal[7][32][8];
static const char *smNumValues[7][32], *smNumLabels[7][32];
static int smNumCount[7];

static void SM_NumList( int list, int first, int step, int last, const char *zero, const char *unit ) {
	int v, k = 0;

	if ( smNumCount[list] ) {
		return;
	}
	if ( zero ) {
		smNumValues[list][k] = "0";
		smNumLabels[list][k++] = zero;
	}
	for ( v = first; step > 0 ? v <= last : v >= last; v += step ) {
		Com_sprintf( smNumText[list][k], sizeof( smNumText[0][0] ), "%i%s", v, unit );
		smNumLabels[list][k] = smNumText[list][k];
		Com_sprintf( smNumVal[list][k], sizeof( smNumVal[0][0] ), "%i", v );
		smNumValues[list][k] = smNumVal[list][k];
		k++;
	}
	smNumCount[list] = k;
}

static void SM_SrvRestart( int n, int arg ) {
	CL_SplitSrvRestartRound();
	CL_SplitMenuClose( n );
}

static void SM_SrvReset( int n, int arg ) {
	CL_SplitSrvReset();
	SM_Note( n, "Server options reset to defaults" );
}

static qboolean SM_SrvSaveDone( int n, const char *text ) {
	char err[96];

	if ( !CL_SplitSrvSaveSet( text, err, sizeof( err ) ) ) {
		SM_Note( n, err );
		return qfalse;
	}
	SM_Note( n, va( "Saved server settings %s", CL_SplitSrvActiveSet() ) );
	return qtrue;
}

static void SM_SrvEnter( int n ) {
	CL_SplitSrvOpen();
}

static void SM_SrvLeave( int n ) {
	CL_SplitSrvLeave();
}

static void SM_BuildServer( int n ) {
	const qboolean local = CL_SplitSrvLocalGame();
	const qboolean urt = CL_SplitSrvUrT();
	const qboolean rules = CL_SplitSrvLayout() != 0;	// [entities] / [ps] rows
	// R16: a game with its own instagib cvar (UrT g_instagib, OSP match_instagib): the row uses it
	const char *modInsta = CL_SplitSrvModInstagib();
	const char *instaCvar = modInsta[0] ? "cl_splitSrvModInstagib" : "cl_splitSrvInstagib";
	const qboolean instaOk = ( rules || modInsta[0] ) ? qtrue : qfalse;
	const qboolean instagib = ( instaOk && Cvar_VariableIntegerValue( instaCvar ) ) ? qtrue : qfalse;
	const char *weap = Cvar_VariableString( "cl_splitSrvWeapons" );
	const char *pending = CL_SplitSrvPending();
	const char *set = CL_SplitSrvActiveSet();
	qboolean speedOk, respawnOk, powerupsOff;
	int i, w;
	smRow_t *r;

	SM_NumList( 0, 5, 5, 60, "None", " min" );
	SM_NumList( 1, 5, 5, 100, "None", "" );
	SM_NumList( 2, 1, 1, 20, "None", "" );
	SM_NumList( 3, 1, 1, 20, "Off", "" );
	SM_NumList( 4, 95, -5, 5, "Default (own handicap)", "" );
	SM_NumList( 5, 1, 1, 30, "Default", " s" );
	SM_NumList( 6, 50, 10, 200, NULL, "%" );

	SM_Title( set[0] ? va( "Server options - %s", set ) : "Server options" );

	r = SM_Help( SM_Action( "Restart round", SM_SrvRestart, 0 ), "Starts this map again now (scores reset)." );
	r->disabled = !local;
	r = SM_Help( SM_Sub( Cvar_VariableString( "cl_splitSrvMap" )[0] ? va( "Change map: %s", Cvar_VariableString( "cl_splitSrvMap" ) ) : "Change map", &smSrvMaps ),
		"Pick a map; it loads when you leave." );
	r->disabled = !local;
	if ( urt ) {
		r = SM_Choice( "Game type", "cl_splitSrvGametype", smGtUrtValues, smGtUrtLabels, ARRAY_LEN( smGtUrtValues ) );
	} else {
		// the four stock types; a current one outside them (single player 2, missionpack 5-7)
		// is shown by name first and can be left but not cycled back to
		static char curValue[8], curLabel[24];
		static const char *values[ ARRAY_LEN( smGtQ3Values ) + 1 ], *labels[ ARRAY_LEN( smGtQ3Values ) + 1 ];
		const int gt = Cvar_VariableIntegerValue( "cl_splitSrvGametype" );
		int k = 0;
		if ( gt != 0 && gt != 1 && gt != 3 && gt != 4 ) {
			Com_sprintf( curValue, sizeof( curValue ), "%i", gt );
			Q_strncpyz( curLabel, gt == 2 ? "Single player" : gt == 5 ? "One flag CTF" : gt == 6 ? "Overload" : gt == 7 ? "Harvester" : va( "Type %i", gt ), sizeof( curLabel ) );
			values[k] = curValue;
			labels[k++] = curLabel;
		}
		for ( i = 0; i < (int)ARRAY_LEN( smGtQ3Values ); i++, k++ ) {
			values[k] = smGtQ3Values[i];
			labels[k] = smGtQ3Labels[i];
		}
		r = SM_Choice( "Game type", "cl_splitSrvGametype", values, labels, k );
	}
	SM_Help( r, "Reloads the map when you leave the page." );
	r->disabled = !local;
	SM_Help( SM_Choice( "Time limit", "timelimit", smNumValues[0], smNumLabels[0], smNumCount[0] ), "Minutes per match; None = no limit. Now." );
	if ( CL_SplitSrvCTF() ) {
		SM_Help( SM_Choice( "Capture limit", "capturelimit", smNumValues[2], smNumLabels[2], smNumCount[2] ), "Captures to win; None = no limit. Now." );
	} else {
		SM_Help( SM_Choice( "Frag limit", "fraglimit", smNumValues[1], smNumLabels[1], smNumCount[1] ), "Frags to win; None = no limit. Now." );
	}

	r = SM_Help( SM_Choice( "Instagib", instaCvar, sm01, smOffOn, 2 ),
		modInsta[0] ? "Uses the game's own instagib. Restarts." : rules ? "Railguns, 1-hit kills, no pickups. Restarts." : "Not available in this game." );
	r->disabled = !instaOk;
	r = SM_Help( SM_Choice( "Weapons", "cl_splitSrvWeapons", smWeapValues, smWeapLabels, ARRAY_LEN( smWeapValues ) ),
		rules ? ( instagib ? "Instagib is on." : "Weapons on the map. Random: new each restart." ) : "Not available in this game." );
	r->disabled = !rules || instagib;

	for ( w = 2; w < (int)ARRAY_LEN( smWeapValues ) && Q_stricmp( weap, smWeapValues[w] ); w++ )
		;
	if ( instagib ) {
		SM_Action( modInsta[0] ? "Player weapons at spawn: instagib" : "Player weapons at spawn: Railgun", SM_SrvReset, 0 )->disabled = qtrue;
	} else if ( rules && w < (int)ARRAY_LEN( smWeapValues ) ) {
		SM_Action( va( "Player weapons at spawn: %s", smWeapNames[w] ), SM_SrvReset, 0 )->disabled = qtrue;
	} else {
		r = SM_Help( SM_Choice( "Player weapons at spawn", "cl_splitSrvSpawnWeapons", smSpawnValues, smSpawnLabels, ARRAY_LEN( smSpawnValues ) ),
			rules ? "What everybody holds after (re)spawning." : "Not available in this game." );
		r->disabled = !rules;
	}
	if ( instagib ) {
		SM_Action( modInsta[0] ? "Infinite ammo: instagib" : "Infinite ammo: On (instagib)", SM_SrvReset, 0 )->disabled = qtrue;
	} else {
		r = SM_Help( SM_Choice( "Infinite ammo", "cl_splitSrvInfiniteAmmo", sm01, smOffOn, 2 ),
			rules ? "Ammo never runs out. Now." : "Not available in this game." );
		r->disabled = !rules;
	}

	SM_Help( SM_Choice( "Bots", "cl_splitSrvBots", smNumValues[3], smNumLabels[3], smNumCount[3] ),
		"Computer players, kept across maps. Added now." );
	SM_Help( SM_Choice( "Bot difficulty", "cl_splitSrvBotSkill", smSkillValues, smSkillLabels, ARRAY_LEN( smSkillValues ) ),
		"Bots rejoin at the new level. Now." );
	SM_Help( SM_Choice( "Friendly fire", "g_friendlyFire", sm01, smOffOn, 2 ), "Team games: teammates hurt each other. Now." );
	r = SM_Help( SM_Choice( "Self-damage", "cl_splitSrvSelfDamage", sm10, smSelfLabels, 2 ),
		rules ? "Off: own rockets don't hurt; rocket jumps stay" : "Not available in this game." );
	r->disabled = !rules;
	i = 2 + MIN( Cvar_VariableIntegerValue( "cl_splitMaxPlayers" ), MAX_SPLITVIEW );
	r = SM_Help( SM_Choice( "God mode (enables cheats)", "cl_splitSrvGod", smGodValues, smGodLabels, MIN( i, (int)ARRAY_LEN( smGodValues ) ) ),
		rules ? "Can't be hurt. Turns cheats on (local game)." : "Not available in this game." );
	r->disabled = !rules;
	if ( instagib ) {
		// one hit kills anyway: no handicap while instagib is on (CL_SplitSrvUserinfo)
		SM_Action( modInsta[0] ? "Player health: instagib" : "Player health: 100 (instagib)", SM_SrvReset, 0 )->disabled = qtrue;
	} else {
		SM_Help( SM_Choice( "Player health", "cl_splitSrvHealth", smNumValues[4], smNumLabels[4], smNumCount[4] ),
			"Handicap for all; also lowers damage dealt." );
	}
	SM_Help( SM_Choice( "Low gravity", "g_gravity", smGravValues, smGravLabels, ARRAY_LEN( smGravValues ) ), "Higher jumps, slower falls. Now." );
	// R16: quad pickups [entities]; player speed / weapon respawn [native], greyed when the
	// running game has no such cvar; never in UrT (its movement ignores g_speed: ps.speed stays 220
	// at any g_speed, R16 test r16-urtspeed; no weapon pickups)
	// R17: every power-up and holdable item [entities]; Off greys Quad damage (removed with them)
	powerupsOff = ( rules && !Cvar_VariableIntegerValue( "cl_splitSrvPowerups" ) ) ? qtrue : qfalse;
	r = SM_Help( SM_Choice( "Power-ups", "cl_splitSrvPowerups", sm10, smSelfLabels, 2 ),
		!rules ? "Not available in this game." : "Quad, haste, invisibility, flight... and holdables. Restarts." );
	r->disabled = !rules;
	if ( powerupsOff ) {
		SM_Help( SM_Action( "Quad damage: Off (power-ups off)", SM_SrvReset, 0 ), "Power-ups are off." )->disabled = qtrue;
	} else {
		r = SM_Help( SM_Choice( "Quad damage", "cl_splitSrvQuad", sm10, smSelfLabels, 2 ),
			!rules ? "Not available in this game." : instagib ? "Instagib removes it anyway." : "Quad damage pickups on the map. Restarts." );
		r->disabled = !rules;
	}
	speedOk = ( !urt && ( !local || Cvar_Flags( "g_speed" ) != CVAR_NONEXISTENT ) ) ? qtrue : qfalse;
	r = SM_Help( SM_Choice( "Player speed", "cl_splitSrvSpeed", smNumValues[6], smNumLabels[6], smNumCount[6] ),
		speedOk ? "Run speed, % of the game's own. Now." : "Not available in this game." );
	r->disabled = !speedOk;
	respawnOk = ( !urt && ( !local || Cvar_Flags( "g_weaponrespawn" ) != CVAR_NONEXISTENT ) ) ? qtrue : qfalse;
	r = SM_Help( SM_Choice( "Weapon respawn", "cl_splitSrvWeaponRespawn", smNumValues[5], smNumLabels[5], smNumCount[5] ),
		respawnOk ? "Seconds until a taken weapon comes back. Now." : "Not available in this game." );
	r->disabled = !respawnOk;
	SM_Help( SM_Slider( "Game volume", "s_volume", 0, 1.5f, 0.1f, "%.0f%%", 100 ), "Master volume of this game window. Now." );
	SM_Help( SM_Action( "Reset to defaults", SM_SrvReset, 0 ), "Every option back to default; the map stays." );
	SM_Help( SM_Sub( "Select server settings", &smSrvSets ), "Load a saved set of these options." );
	r = SM_Help( SM_TextEntry( "Save server settings...", "" ), "Save these options under a name." );
	r->textDone = SM_SrvSaveDone;
	r->arg = PROFILE_NAME_LEN;
	SM_Help( SM_Sub( "Delete server settings", &smSrvDelete ), "Remove a saved set." );

	if ( !local ) {
		SM_Text( "Not in a game: they apply to the next one." );
	} else if ( pending[0] ) {
		SM_Text( va( "On leaving: %s", pending ) );
	}
	if ( local && Cvar_VariableIntegerValue( "sv_splitGod" ) ) {
		SM_Text( "Cheats are on while god mode is on." );
	}
}

static smPage_t smServer = { "server", SM_BuildServer, SM_SrvEnter, SM_SrvLeave };

// --- Change map: every map, the current one first; its levelshot beside the list ---

static void SM_SrvPickMap( int n, int arg ) {
	Cvar_Set( "cl_splitSrvMap", CL_SplitSrvMapName( arg ) );
	SM_Back( n );
	SM_Note( n, va( "Next map: %s (loads when you leave)", CL_SplitSrvMapName( arg ) ) );
}

static void SM_BuildSrvMaps( int n ) {
	const int count = CL_SplitSrvNumMaps();
	const char *cur = CL_SplitSrvCurrentMap();
	const char *next = Cvar_VariableString( "cl_splitSrvMap" );
	const char *name;
	int i;

	SM_Title( "Change map" );
	if ( !count ) {
		SM_Text( "No maps found." );
	}
	for ( i = 0; i < count; i++ ) {
		name = CL_SplitSrvMapName( i );
		SM_Action( !Q_stricmp( name, cur ) ? va( "%s (now)", name ) : !Q_stricmp( name, next ) ? va( "%s (next)", name ) : name,
			SM_SrvPickMap, i );
	}
}

static smPage_t smSrvMaps = { "maps", SM_BuildSrvMaps };

// --- saved sets ---

static void SM_SrvLoad( int n, int arg ) {
	const char *name = CL_SplitSrvSetName( arg );

	if ( !CL_SplitSrvLoadSet( arg ) ) {
		SM_Note( n, "That set could not be read" );
		return;
	}
	SM_Back( n );
	SM_Note( n, va( "Loaded %s: applies when you leave", name ) );
}

static void SM_BuildSrvSets( int n ) {
	const int count = CL_SplitSrvNumSets();
	int i;

	SM_Title( "Select server settings" );
	if ( !count ) {
		SM_Text( "No saved server settings." );
	}
	for ( i = 0; i < count; i++ ) {
		SM_Action( CL_SplitSrvSetName( i ), SM_SrvLoad, i );
	}
}

static void SM_SrvSetsEnter( int n ) {
	CL_SplitSrvRefreshSets();
}

static smPage_t smSrvSets = { "serversets", SM_BuildSrvSets, SM_SrvSetsEnter };

static void SM_SrvDeleteAsk( int n, int arg ) {
	Q_strncpyz( sm[n].argName, CL_SplitSrvSetName( arg ), sizeof( sm[n].argName ) );
	SM_Push( n, &smSrvDeleteConfirm );
}

static void SM_BuildSrvDelete( int n ) {
	const int count = CL_SplitSrvNumSets();
	int i;

	SM_Title( "Delete server settings" );
	if ( !count ) {
		SM_Text( "No saved server settings." );
	}
	for ( i = 0; i < count; i++ ) {
		SM_Action( CL_SplitSrvSetName( i ), SM_SrvDeleteAsk, i );
	}
}

static smPage_t smSrvDelete = { "serverdelete", SM_BuildSrvDelete, SM_SrvSetsEnter };

static void SM_SrvDoDelete( int n, int arg ) {
	int i;

	for ( i = 0; i < CL_SplitSrvNumSets() && Q_stricmp( CL_SplitSrvSetName( i ), sm[n].argName ); i++ )
		;
	if ( !CL_SplitSrvDeleteSet( i ) ) {
		SM_Note( n, "Not found" );
		return;
	}
	SM_Back( n );
	SM_Back( n );
	SM_Note( n, va( "Deleted %s", sm[n].argName ) );
}

static void SM_BuildSrvDeleteConfirm( int n ) {
	SM_Title( va( "Delete %s?", smb->argName ) );
	SM_Text( "The saved server settings are gone for good." );
	SM_Action( "Cancel", SM_Cancel, 0 );
	SM_Action( "Delete", SM_SrvDoDelete, 0 );
}

static smPage_t smSrvDeleteConfirm = { "serverdelete?", SM_BuildSrvDeleteConfirm };

// --- R14a: player model / handicap rows of the profile page ---

static const char *smHandicapValues[] = { "100", "95", "90", "85", "80", "75", "70", "65", "60", "55",
	"50", "45", "40", "35", "30", "25", "20", "15", "10", "5" };
static const char *smHandicapLabels[] = { "None", "95", "90", "85", "80", "75", "70", "65", "60", "55",
	"50", "45", "40", "35", "30", "25", "20", "15", "10", "5" };

// the mod's player model page in the player's own cell (stock Quake 3: straight to it;
// other games: their game menu, where the player goes on)
static void SM_ChangeModel( int n, int arg ) {
	qboolean exact;

	CL_SplitMenuClose( n );
	if ( !CL_SplitUIOpenModelPage( n, &exact ) ) {
		CL_SplitMenuPause( n );
		SM_Note( n, "The game menu is not available" );
		return;
	}
	Com_Printf( "P%i: %s\n", n + 1, exact ? "player model page (from the game menu)" : "game menu: pick the player model there" );
}

// --- profile (design 12.3 / 13.1): the player's own slot ---

static qboolean SM_RenameDone( int n, const char *text ) {
	char err[96];

	if ( !CL_ProfileRename( n, text, err, sizeof( err ) ) ) {
		SM_Note( n, err );
		return qfalse;
	}
	SM_Note( n, va( "Renamed to %s", CL_ProfileName( n ) ) );
	return qtrue;
}

static void SM_RenameStart( int n, int arg ) {
	SM_OpenKeyboard( n, "Rename profile", CL_ProfileName( n ), PROFILE_NAME_LEN, NULL, SM_RenameDone );
}

static qboolean SM_SaveAsDone( int n, const char *text ) {
	char err[96];

	if ( !CL_ProfileSaveAs( n, text, err, sizeof( err ) ) ) {
		SM_Note( n, err );
		return qfalse;
	}
	SM_Note( n, va( "Saved as %s", CL_ProfileName( n ) ) );
	return qtrue;
}

static void SM_BuildProfile( int n ) {
	smRow_t *r;

	SM_Title( va( "Player %i - Profile", SM_Num( n ) ) );
	if ( !CL_ProfileActive( n ) ) {
		SM_Text( "Playing without a profile (q3config)." );
	} else if ( CL_ProfileIsGuest( n ) ) {
		SM_Text( "Playing as Guest: changes last until" );
		SM_Text( "you leave." );
	} else {
		SM_Text( va( "Playing as %s: changes are saved.", CL_ProfileName( n ) ) );
	}
	SM_Sub( "Switch profile", &smPicker );
	if ( CL_ProfileActive( n ) && !CL_ProfileIsGuest( n ) ) {
		SM_Action( "Rename...", SM_RenameStart, 0 );
	}
	if ( CL_ProfileActive( n ) ) {
		r = SM_TextEntry( CL_ProfileIsGuest( n ) ? "Keep my settings as a profile..." : "Save as new profile...", "" );
		r->textDone = SM_SaveAsDone;
		r->arg = PROFILE_NAME_LEN;
	}
	SM_Sub( "Delete a profile...", &smDeleteList );
	// R14a: the in-game look of this player (per-player userinfo, saved in the profile)
	r = SM_Action( "Change player model", SM_ChangeModel, 0 );
	r->disabled = ( cls.state != CA_ACTIVE ) ? qtrue : qfalse;
	SM_Choice( "Handicap", n == 0 ? "handicap" : CL_SplitShadow( n, "handicap", NULL )->name,
		smHandicapValues, smHandicapLabels, ARRAY_LEN( smHandicapValues ) );
}

static smPage_t smProfile = { "profile", SM_BuildProfile };

// --- picker: at the join (before connecting) and "Switch profile" ---

static void SM_Pick( int n, int arg ) {
	smPlayer_t *s = &sm[n];
	const char *name = ( arg >= 0 ) ? CL_ProfileListName( arg ) : NULL;

	if ( arg >= 0 && !name[0] ) {
		return;
	}
	if ( s->joining ) {
		if ( !CL_ProfileLoad( n, name, qtrue ) ) {
			SM_Note( n, "That profile is not available" );
			return;
		}
		SM_JoinDone( n );
		return;
	}
	if ( ( arg >= 0 && CL_ProfileListUser( arg ) == n ) || ( arg < 0 && CL_ProfileIsGuest( n ) ) ) {
		SM_Note( n, va( "Already playing as %s", CL_ProfileName( n ) ) );
		return;
	}
	if ( !CL_ProfileLoad( n, name, qfalse ) ) {
		SM_Note( n, "That profile is not available" );
		return;
	}
	SM_Back( n );
	SM_Note( n, va( "Now playing as %s", CL_ProfileName( n ) ) );
}

static qboolean SM_NewDone( int n, const char *text ) {
	smPlayer_t *s = &sm[n];
	char clean[PROFILE_NAME_LEN + 1], err[96], prev[PROFILE_NAME_LEN + 2];
	int i;

	if ( !CL_ProfileSanitize( text, clean, sizeof( clean ) ) ) {
		SM_Note( n, "Type a name (letters, digits, - _)" );
		return qfalse;
	}
	i = CL_ProfileFind( clean );
	if ( i >= 0 ) {
		SM_Note( n, va( "%s exists: pick it or another name", CL_ProfileListName( i ) ) );
		return qfalse;
	}
	prev[0] = '\0';
	if ( !s->joining ) {
		if ( !CL_ProfileIsGuest( n ) ) {
			Q_strncpyz( prev, CL_ProfileName( n ), sizeof( prev ) );	// its changes are saved by the switch
		}
		CL_ProfileLoad( n, NULL, qfalse );	// a new profile starts from the Guest defaults
	}
	if ( !CL_ProfileSaveAs( n, clean, err, sizeof( err ) ) ) {
		if ( prev[0] ) {
			CL_ProfileLoad( n, prev, qfalse );	// the save failed: back to the profile it had
		}
		SM_Note( n, err );
		return qfalse;
	}
	if ( s->joining ) {
		SM_JoinDone( n );
		return qtrue;
	}
	SM_Back( n );	// the keyboard
	SM_Back( n );	// the picker
	SM_Note( n, va( "Now playing as %s", CL_ProfileName( n ) ) );
	return qtrue;
}

static void SM_NewStart( int n, int arg ) {
	SM_OpenKeyboard( n, "New profile: your name", "", PROFILE_NAME_LEN, NULL, SM_NewDone );
}

static void SM_BuildPicker( int n ) {
	const int count = CL_ProfileCount();
	const char *name;
	smRow_t *r;
	int i, user;

	SM_Title( smb->joining ? va( "Player %i - who's playing?", SM_Num( n ) ) : va( "Player %i - switch profile", SM_Num( n ) ) );
	SM_Action( ( !smb->joining && CL_ProfileIsGuest( n ) ) ? "Guest (you)" : "Guest", SM_Pick, -1 );
	for ( i = 0; i < count; i++ ) {
		name = CL_ProfileListName( i );
		user = CL_ProfileListUser( i );
		if ( user != n && !CL_IndepProfileAllowed( name ) ) {
			continue;	// Independent mode, a player's own window: its own profile and Guest only
		}
		if ( user == n ) {
			r = SM_Action( va( "%s (you)", name ), SM_Pick, i );
		} else if ( user >= 0 ) {
			r = SM_Action( va( "%s (player %i)", name, user + 1 ), SM_Pick, i );
			r->disabled = qtrue;	// in use by someone else
		} else {
			SM_Action( name, SM_Pick, i );
		}
	}
	SM_Action( "New profile...", SM_NewStart, 0 );
}

static smPage_t smPicker = { "picker", SM_BuildPicker };

// --- delete ---

static void SM_DeleteAsk( int n, int arg ) {
	Q_strncpyz( sm[n].argName, CL_ProfileListName( arg ), sizeof( sm[n].argName ) );
	SM_Push( n, &smDeleteConfirm );
}

static void SM_BuildDeleteList( int n ) {
	const int count = CL_ProfileCount();
	smRow_t *r;
	int i, user;

	SM_Title( va( "Player %i - delete a profile", SM_Num( n ) ) );
	if ( !count ) {
		SM_Text( "No saved profiles." );
	}
	for ( i = 0; i < count; i++ ) {
		user = CL_ProfileListUser( i );
		if ( user != n && !CL_IndepProfileAllowed( CL_ProfileListName( i ) ) ) {
			continue;	// Independent mode, a player's own window: other profiles are player 1's to manage
		}
		if ( user >= 0 && user != n ) {
			r = SM_Action( va( "%s (player %i)", CL_ProfileListName( i ), user + 1 ), SM_DeleteAsk, i );
			r->disabled = qtrue;
		} else {
			SM_Action( user == n ? va( "%s (you)", CL_ProfileListName( i ) ) : CL_ProfileListName( i ), SM_DeleteAsk, i );
		}
	}
}

static smPage_t smDeleteList = { "delete", SM_BuildDeleteList };

static void SM_DoDelete( int n, int arg ) {
	char err[96];

	if ( !CL_ProfileDelete( CL_ProfileFind( sm[n].argName ), n, err, sizeof( err ) ) ) {
		SM_Note( n, err );
		return;
	}
	SM_Back( n );
	SM_Back( n );
	SM_Note( n, va( "Deleted %s", sm[n].argName ) );
}

static void SM_BuildDeleteConfirm( int n ) {
	const int i = CL_ProfileFind( smb->argName );

	SM_Title( va( "Delete %s?", smb->argName ) );
	SM_Text( "Its saved settings are gone for good." );
	if ( i >= 0 && CL_ProfileListUser( i ) == n ) {
		SM_Text( "You go on as a Guest." );
	}
	SM_Action( "Cancel", SM_Cancel, 0 );
	SM_Action( "Delete", SM_DoDelete, 0 );
}

static smPage_t smDeleteConfirm = { "delete?", SM_BuildDeleteConfirm };


/*
=============================================================================

ON-SCREEN KEYBOARD (design 12.4)

A grid in the player's cell: d-pad / left stick move (with repeat, wrapping),
A types, X deletes, Y types a space, L3 or the Shift key switches case,
Start is done, B cancels; the right-stick cursor + RT presses a key too.

=============================================================================
*/

#define KB_ROWS		5
#define KB_COLS		10
#define KB_DEL		1
#define KB_SHIFT	2
#define KB_DONE		3

// every row is KB_COLS columns; a key spanning columns repeats its code
static const char *smKbRows[KB_ROWS] = {
	"1234567890",
	"qwertyuiop",
	"asdfghjkl-",
	"zxcvbnm_\001\001",
	"\002\002\002    \003\003\003"
};

static int SM_KbCode( int row, int col ) {
	return (unsigned char)smKbRows[row][col];
}

static void SM_BuildKeyboard( int n ) {
	SM_Title( smb->kb.title );
}

static smPage_t smKeyboard = { "keyboard", SM_BuildKeyboard };


// one character to the mod menu the keyboard types into
static void SM_KbSend( int n, int ch ) {
	Com_DPrintf( "P%i keyboard: char %i to the game menu\n", n + 1, ch );
	if ( n == 0 ) {
		Sys_QueEvent( Sys_Milliseconds(), SE_CHAR, ch, 0, 0, NULL );
	} else {
		CL_SplitUIKeyEvent( n, ch | K_CHAR_FLAG, qtrue );
	}
}


static void SM_KbDone( int n ) {
	smPlayer_t *s = &sm[n];
	char text[sizeof( s->kb.text )];
	qboolean ok = qtrue;

	if ( s->kb.live ) {
		CL_SplitMenuClose( n );
		return;
	}
	Q_strncpyz( text, s->kb.text, sizeof( text ) );
	Com_Printf( "P%i keyboard: done \"%s\"\n", n + 1, text );
	if ( s->kb.cvar[0] ) {
		Cvar_Set( s->kb.cvar, text );
	}
	if ( s->kb.done ) {
		ok = s->kb.done( n, text );
	}
	if ( ok && SM_OnKeyboard( n ) ) {
		SM_Back( n );
	}
}


static void SM_KbPress( int n, int code ) {
	smPlayer_t *s = &sm[n];
	int len = (int)strlen( s->kb.text );

	switch ( code ) {
	case KB_DEL:
		if ( len > 0 ) {
			s->kb.text[len - 1] = '\0';
		}
		if ( s->kb.live ) {
			SM_KbSend( n, 'h' - 'a' + 1 );	// ctrl-h: backspace in every Q3 text field
		}
		break;
	case KB_SHIFT:
		s->kb.shift = !s->kb.shift;
		break;
	case KB_DONE:
		SM_KbDone( n );
		return;
	default:
		if ( s->kb.shift && code >= 'a' && code <= 'z' ) {
			code += 'A' - 'a';
		}
		if ( len >= s->kb.max ) {
			SM_Note( n, va( "At most %i characters", s->kb.max ) );
			return;
		}
		s->kb.text[len] = (char)code;
		s->kb.text[len + 1] = '\0';
		if ( s->kb.live ) {
			SM_KbSend( n, code );
		}
		break;
	}
	if ( SM_OnKeyboard( n ) && com_developer && com_developer->integer ) {
		Com_Printf( "P%i keyboard: \"%s\"%s\n", n + 1, s->kb.text, s->kb.shift ? " (shift)" : "" );
	}
}


// the key under the cursor (last draw's geometry)
static qboolean SM_KbHover( int n, int *row, int *col ) {
	const smPlayer_t *s = &sm[n];
	const int x = s->cell.x + (int)s->cx, y = s->cell.y + (int)s->cy;

	if ( !s->cursorOn || s->kb.kw <= 0 || s->kb.kh <= 0 || x < s->kb.x || y < s->kb.y ) {
		return qfalse;
	}
	*col = ( x - s->kb.x ) / s->kb.kw;
	*row = ( y - s->kb.y ) / s->kb.kh;
	return ( *row < KB_ROWS && *col < KB_COLS ) ? qtrue : qfalse;
}


static void SM_KeyboardMouse( int n ) {
	int row, col;

	if ( SM_KbHover( n, &row, &col ) ) {
		sm[n].kb.row = row;
		sm[n].kb.col = col;
	}
}


static void SM_KeyboardKey( int n, int key ) {
	smPlayer_t *s = &sm[n];
	int code, i, row, col;

	switch ( key ) {
	case K_UPARROW:
	case K_DOWNARROW:
		s->kb.row = ( s->kb.row + ( key == K_UPARROW ? KB_ROWS - 1 : 1 ) ) % KB_ROWS;
		break;
	case K_LEFTARROW:
	case K_RIGHTARROW:
		// skip the rest of a wide key
		code = SM_KbCode( s->kb.row, s->kb.col );
		for ( i = 0; i < KB_COLS; i++ ) {
			s->kb.col = ( s->kb.col + ( key == K_LEFTARROW ? KB_COLS - 1 : 1 ) ) % KB_COLS;
			if ( SM_KbCode( s->kb.row, s->kb.col ) != code ) {
				break;
			}
		}
		// land on the first column of a wide key
		code = SM_KbCode( s->kb.row, s->kb.col );
		while ( s->kb.col > 0 && SM_KbCode( s->kb.row, s->kb.col - 1 ) == code ) {
			s->kb.col--;
		}
		break;
	case K_ENTER:
		SM_KbPress( n, SM_KbCode( s->kb.row, s->kb.col ) );
		break;
	case K_MOUSE1:
		if ( SM_KbHover( n, &row, &col ) ) {
			s->kb.row = row;
			s->kb.col = col;
			SM_KbPress( n, SM_KbCode( row, col ) );
		}
		break;
	case K_ESCAPE:
		Com_Printf( "P%i keyboard: cancelled\n", n + 1 );
		SM_Back( n );
		break;
	default:
		break;
	}
}


/*
==================
CL_SplitMenuPadKey

Raw pad keys for the on-screen keyboard: X delete, Y space, L3 shift,
Start done.  qtrue if the key was used.
==================
*/
qboolean CL_SplitMenuPadKey( int n, int padKey ) {
	if ( !SM_Get( n ) || !SM_OnKeyboard( n ) ) {
		return qfalse;
	}
	if ( padKey == IN_PadKeyNum( "PAD_X" ) ) {
		SM_KbPress( n, KB_DEL );
	} else if ( padKey == IN_PadKeyNum( "PAD_Y" ) ) {
		SM_KbPress( n, ' ' );
	} else if ( padKey == IN_PadKeyNum( "PAD_L3" ) ) {
		SM_KbPress( n, KB_SHIFT );
	} else if ( padKey == IN_PadKeyNum( "PAD_START" ) ) {
		SM_KbPress( n, KB_DONE );
	} else {
		return qfalse;
	}
	return qtrue;
}


/*
=============================================================================

DRAWING

=============================================================================
*/

/*
==================
Theme

One place for the overlay's colours.  The accent comes from the archived
cvar cl_splitMenuColor ("r g b", 0..1; default "auto": Quake 3 menu red,
Urban Terror blue (R18) -- a per-game default the cvar overrides); the rest
is derived from it or fixed for legibility on the dark panel.
==================
*/
static cvar_t	*cl_splitMenuColor;
static cvar_t	*cl_splitMenuSize;	// R14a: overlay text size; R17: 0.5 .. 1.5, default 1

static struct {
	float	accent[4];		// highlight stripe, capture box border
	float	accentDim[4];	// title bar
	float	selBar[4];		// selected row background
	float	selText[4];		// selected row text, notes
	float	panel[4];		// panel background
	float	text[4];		// row labels
	float	value[4];		// row values
	float	dim[4];			// static text, footer
	float	off[4];			// disabled rows
	float	footBg[4];
} smTheme;

static const float smWhite[4]	= { 1.00f, 1.00f, 1.00f, 1.00f };
static const float smBlack[4]	= { 0.00f, 0.00f, 0.00f, 1.00f };

static void SM_SetColor4( float *c, float r, float g, float b, float a ) {
	c[0] = r; c[1] = g; c[2] = b; c[3] = a;
}

static void SM_UpdateTheme( void ) {
	float r, g, b;

	if ( !cl_splitMenuColor || sscanf( cl_splitMenuColor->string, "%f %f %f", &r, &g, &b ) != 3 ) {
		if ( CL_SplitSrvUrT() ) {
			r = 0.16f; g = 0.50f; b = 1.0f;	// "auto" in Urban Terror: UrT menu blue
		} else {
			r = 1.0f; g = b = 0.0f;			// "auto": Quake 3 menu red
		}
	}
	r = Com_Clamp( 0.0f, 1.0f, r );
	g = Com_Clamp( 0.0f, 1.0f, g );
	b = Com_Clamp( 0.0f, 1.0f, b );

	SM_SetColor4( smTheme.accent, r, g, b, 1.0f );
	SM_SetColor4( smTheme.accentDim, r * 0.5f, g * 0.5f, b * 0.5f, 0.95f );
	SM_SetColor4( smTheme.selBar, r, g, b, 0.30f );
	SM_SetColor4( smTheme.selText, r + ( 1.0f - r ) * 0.35f, g + ( 1.0f - g ) * 0.35f, b + ( 1.0f - b ) * 0.35f, 1.0f );
	SM_SetColor4( smTheme.panel, 0.04f, 0.03f, 0.03f, 0.88f );
	SM_SetColor4( smTheme.text, 0.92f, 0.92f, 0.92f, 1.0f );
	SM_SetColor4( smTheme.value, 0.78f, 0.78f, 0.78f, 1.0f );
	SM_SetColor4( smTheme.dim, 0.60f, 0.60f, 0.60f, 1.0f );
	SM_SetColor4( smTheme.off, 0.40f, 0.40f, 0.40f, 1.0f );
	SM_SetColor4( smTheme.footBg, 0.0f, 0.0f, 0.0f, 0.55f );
}


static void SM_Fill( int x, int y, int w, int h, const float *color ) {
	re.SetColor( color );
	re.DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}


// console-font text, at most maxChars characters, color codes shown as is
static void SM_DrawText( int x, int y, int cw, int ch, const char *s, const float *color, int maxChars ) {
	float frow, fcol;
	int i, c;

	re.SetColor( color );
	for ( i = 0; s[i] && i < maxChars; i++ ) {
		c = s[i] & 255;
		if ( c == ' ' ) {
			continue;
		}
		frow = ( c >> 4 ) * 0.0625f;
		fcol = ( c & 15 ) * 0.0625f;
		re.DrawStretchPic( x + i * cw, y, cw, ch, fcol, frow, fcol + 0.0625f, frow + 0.0625f, cls.charSetShader );
	}
	re.SetColor( NULL );
}


/*
==================
SM_DrawKeyboard

The on-screen keyboard in the owner's cell: title, the text with a caret
and its length limit, the key grid, what the buttons do.
==================
*/
static void SM_DrawKeyboard( int n, const viewRect_t *cell, int cw, int ch, int lineH, int cols ) {
	static const float keyBg[4] = { 0.16f, 0.15f, 0.15f, 0.95f };
	static const float fieldBg[4] = { 0.0f, 0.0f, 0.0f, 0.85f };
	smPlayer_t *s = &sm[n];
	char text[96], label[16];
	const char *lbl;
	int panelW, panelH, titleH, fieldH, kw, kh, px, py, x, y, r, c, e, code, len, footH;
	qboolean sel;

	panelW = ( cols + 2 ) * cw;
	titleH = lineH + ch / 2;
	fieldH = lineH + ch / 2;
	kw = ( panelW - 2 * cw ) / KB_COLS;
	kh = lineH + ch / 3;
	footH = lineH * 2;
	if ( s->kb.live ) {
		titleH = 0;		// compact, at the bottom: the menu's own text field stays visible
		footH = lineH;
	}
	panelH = titleH + fieldH + KB_ROWS * kh + ch / 2 + footH;
	px = cell->x + ( cell->w - panelW ) / 2;
	py = s->kb.live ? cell->y + cell->h - panelH : cell->y + ( cell->h - panelH ) / 2;

	SM_Fill( px, py, panelW, panelH, smTheme.panel );
	if ( titleH ) {
		SM_Fill( px, py, panelW, titleH, smTheme.accentDim );
		SM_Fill( px, py + titleH - 2, panelW, 2, smTheme.accent );
		SM_DrawText( px + cw, py + ( titleH - ch ) / 2, cw, ch, s->kb.title, smWhite, cols );
	} else {
		SM_Fill( px, py, panelW, 2, smTheme.accent );
	}

	// the text, a caret, n/max
	y = py + titleH + ch / 4;
	SM_Fill( px + cw, y, panelW - 2 * cw, lineH, fieldBg );
	Com_sprintf( label, sizeof( label ), "%i/%i", (int)strlen( s->kb.text ), s->kb.max );
	len = (int)strlen( s->kb.text );
	c = cols - 3 - (int)strlen( label );		// room for the text
	Com_sprintf( text, sizeof( text ), "%s%s", len > c ? s->kb.text + len - c : s->kb.text,
		( ( cls.realtime >> 9 ) & 1 ) ? "_" : "" );
	SM_DrawText( px + cw + cw / 2, y + ( lineH - ch ) / 2, cw, ch, text, smWhite, c + 1 );
	SM_DrawText( px + panelW - cw - cw / 2 - (int)strlen( label ) * cw, y + ( lineH - ch ) / 2, cw, ch, label, smTheme.dim, 8 );

	// keys
	s->kb.x = px + cw;
	s->kb.y = py + titleH + fieldH;
	s->kb.kw = kw;
	s->kb.kh = kh;
	for ( r = 0; r < KB_ROWS; r++ ) {
		for ( c = 0; c < KB_COLS; c = e ) {
			code = SM_KbCode( r, c );
			for ( e = c + 1; e < KB_COLS && SM_KbCode( r, e ) == code; e++ )
				;
			x = s->kb.x + c * kw;
			y = s->kb.y + r * kh;
			sel = ( s->kb.row == r && s->kb.col >= c && s->kb.col < e ) ? qtrue : qfalse;
			if ( sel ) {
				SM_Fill( x + 1, y + 1, ( e - c ) * kw - 2, kh - 2, smTheme.accent );
				SM_Fill( x + 3, y + 3, ( e - c ) * kw - 6, kh - 6, smTheme.selBar );
			} else {
				SM_Fill( x + 1, y + 1, ( e - c ) * kw - 2, kh - 2, keyBg );
			}
			switch ( code ) {
			case KB_DEL: lbl = "Del"; break;
			case KB_SHIFT: lbl = s->kb.shift ? "SHIFT" : "Shift"; break;
			case KB_DONE: lbl = "Done"; break;
			case ' ': lbl = "Space"; break;
			default:
				label[0] = ( s->kb.shift && code >= 'a' && code <= 'z' ) ? code + 'A' - 'a' : code;
				label[1] = '\0';
				lbl = label;
				break;
			}
			len = MIN( (int)strlen( lbl ), ( ( e - c ) * kw - 4 ) / cw );
			SM_DrawText( x + ( ( e - c ) * kw - len * cw ) / 2, y + ( kh - ch ) / 2, cw, ch, lbl,
				( code == KB_SHIFT && s->kb.shift ) ? smTheme.selText : ( sel ? smWhite : smTheme.text ), len );
		}
	}

	// footer: the note, or the buttons
	y = py + panelH - footH;
	SM_Fill( px, y, panelW, footH, smTheme.footBg );
	if ( s->kb.live ) {
		Com_sprintf( text, sizeof( text ), "%s type  %s del  %s space  %s done", IN_PadButtonName( n, "PAD_A" ),
			IN_PadButtonName( n, "PAD_X" ), IN_PadButtonName( n, "PAD_Y" ), IN_PadButtonName( n, "PAD_START" ) );
		SM_DrawText( px + cw, y + ( lineH - ch ) / 2, cw, ch, text, smTheme.dim, cols );
		return;
	}
	if ( s->note[0] && cls.realtime - s->noteTime < SM_NOTE_TIME ) {
		SM_DrawText( px + cw, y + ( lineH - ch ) / 2, cw, ch, s->note, smTheme.selText, cols );
	} else {
		Com_sprintf( text, sizeof( text ), "%s type  %s delete  %s space  L3 shift", IN_PadButtonName( n, "PAD_A" ),
			IN_PadButtonName( n, "PAD_X" ), IN_PadButtonName( n, "PAD_Y" ) );
		SM_DrawText( px + cw, y + ( lineH - ch ) / 2, cw, ch, text, smTheme.dim, cols );
	}
	Com_sprintf( text, sizeof( text ), "%s done  %s cancel", IN_PadButtonName( n, "PAD_START" ), IN_PadButtonName( n, "PAD_B" ) );
	SM_DrawText( px + cw, y + lineH + ( lineH - ch ) / 2, cw, ch, text, smTheme.dim, cols );
}


static void SM_DrawPlayer( int n ) {
	smPlayer_t *s = &sm[n];
	viewRect_t cell;
	char value[64], foot[96];
	const smRow_t *r;
	const float *col;
	int ch, cw, lineH, cols, panelW, panelH, titleH, footH, maxVis, vis, px, py, x, y, i, idx, len, avail;
	int *sel, *top;
	qboolean hasHelp;
	int helpLines;

	SM_Build( n );
	if ( s->depth <= 0 ) {
		return;
	}
	sel = &s->stack[ s->depth - 1 ].sel;
	top = &s->stack[ s->depth - 1 ].top;

	// the owner's cell; the whole screen for player 1 outside a game
	if ( n == 0 && !s->inGame ) {
		cell.x = cell.y = 0;
		cell.w = cls.glconfig.vidWidth;
		cell.h = cls.glconfig.vidHeight;
	} else {
		CL_SplitViewRect( n, &cell );
	}
	s->cell = cell;

	// console font scaled to the cell: 8x16 in a 360-line cell, up to 12x24 at 720
	// lines (R14a: that cap now grows with the screen -- at 3840x2160 it held the
	// overlay to a third of its size), times the host's Menu size (cl_splitMenuSize);
	// then shrunk until at least 26 columns and the title, 3 rows and the footer
	// (the keyboard: all of it) fit the cell.  Taller pages scroll.
	{
		const float res = MAX( 1.0f, cls.glconfig.vidHeight / 720.0f );
		const int maxH = SM_OnKeyboard( n ) ? cell.h * 2 / 31 : cell.h * 3 / 26;

		ch = cell.h / 22;
		ch = MAX( 12, MIN( (int)( 24 * res ), ch ) );
		ch = (int)( ch * Com_Clamp( 0.5f, 1.5f, cl_splitMenuSize ? cl_splitMenuSize->value : 1.0f ) );
		ch = MIN( ch, 2 * ( cell.w / 28 ) );
		ch = MIN( ch, maxH );
		ch = MAX( 12, ch & ~1 );
	}
	cw = ch / 2;
	lineH = ch + ch / 3;
	cols = MIN( SM_COLS, cell.w / cw - 4 );
	if ( cols < 16 ) {
		cols = MAX( 8, cell.w / cw - 2 );
	}
	if ( SM_OnKeyboard( n ) ) {
		SM_DrawKeyboard( n, &cell, cw, ch, lineH, cols );
		goto cursor;
	}
	panelW = ( cols + 2 ) * cw;
	titleH = lineH + ch / 2;
	footH = lineH;
	for ( i = 0, hasHelp = qfalse, helpLines = 1; i < s->numRows; i++ ) {
		if ( s->rows[i].help ) {
			hasHelp = qtrue;	// R14b: a help line above the buttons
			if ( s->rows[i].helpWrap && (int)strlen( s->rows[i].help ) > cols ) {
				helpLines = 2;	// R19: a longer one wraps once
			}
		}
	}
	if ( hasHelp ) {
		footH = lineH * ( 1 + helpLines );
	}
	maxVis = ( cell.h - 2 * cw - titleH - footH - ch / 2 ) / lineH;
	maxVis = MAX( 3, maxVis );
	vis = MIN( s->numRows, maxVis );
	panelH = titleH + vis * lineH + ch / 2 + footH;
	px = cell.x + ( cell.w - panelW ) / 2;
	py = cell.y + ( cell.h - panelH ) / 2;

	// keep the selection visible
	if ( *sel < *top ) {
		*top = *sel;
	}
	if ( *sel >= *top + vis ) {
		*top = *sel - vis + 1;
	}
	// R14b: on the last selectable row, the text rows after it (notes) scroll into view too
	for ( i = *sel + 1; i < s->numRows && s->rows[i].type == SMR_TEXT; i++ )
		;
	if ( i == s->numRows && *sel >= *top + vis - 1 ) {
		*top = MIN( s->numRows - vis, *sel );
	}
	*top = MAX( 0, MIN( *top, s->numRows - vis ) );
	// a text row right above the first visible selectable one stays visible
	if ( *top > 0 && *top == *sel && s->rows[ *top - 1 ].type == SMR_TEXT && *sel - *top + 1 < vis ) {
		( *top )--;
	}

	s->rowX = px;
	s->rowY = py + titleH + ch / 4;
	s->rowW = panelW;
	s->lineH = lineH;
	s->visRows = vis;
	if ( s->cx < 0.0f ) {
		s->cx = cell.w * 0.5f;
		s->cy = cell.h * 0.5f;
	}

	// panel, title bar
	SM_Fill( px, py, panelW, panelH, smTheme.panel );
	SM_Fill( px, py, panelW, titleH, smTheme.accentDim );
	SM_Fill( px, py + titleH - 2, panelW, 2, smTheme.accent );
	SM_DrawText( px + cw, py + ( titleH - ch ) / 2, cw, ch, s->title, smWhite, cols );

	// rows
	for ( i = 0; i < vis; i++ ) {
		idx = *top + i;
		if ( idx >= s->numRows ) {
			break;
		}
		r = &s->rows[idx];
		y = s->rowY + i * lineH;
		x = px + cw;

		if ( idx == *sel && !s->capture ) {
			SM_Fill( px + 2, y, panelW - 4, lineH, smTheme.selBar );
			SM_Fill( px + 2, y, MAX( 2, cw / 3 ), lineH, smTheme.accent );
		}

		SM_RowValue( n, r, value, sizeof( value ) );
		if ( idx == *sel && ( r->type == SMR_CHOICE || r->type == SMR_SLIDER ) && !r->disabled ) {
			Q_strncpyz( foot, value, sizeof( foot ) );
			Com_sprintf( value, sizeof( value ), "< %s >", foot );
		}
		len = (int)strlen( value );
		// a value gets half the width, or (R19) all that its label leaves free
		if ( len > MAX( cols / 2, cols - (int)strlen( r->label ) - 1 ) && r->type != SMR_TEXT ) {
			len = MAX( cols / 2, cols - (int)strlen( r->label ) - 1 );
			value[len] = '\0';
		}
		avail = cols - ( len ? len + 1 : 0 );

		if ( r->type == SMR_TEXT ) {
			col = smTheme.dim;
		} else if ( r->disabled ) {
			col = smTheme.off;
		} else {
			col = ( idx == *sel ) ? smTheme.selText : smTheme.text;
		}
		SM_DrawText( x, y + ( lineH - ch ) / 2, cw, ch, r->label, col, r->type == SMR_TEXT ? cols : avail );
		if ( len ) {
			SM_DrawText( px + panelW - cw - len * cw, y + ( lineH - ch ) / 2, cw, ch, value,
				r->disabled ? smTheme.off : smTheme.value, len );
		}
	}

	// more rows above / below
	if ( *top > 0 ) {
		SM_DrawText( px + panelW - cw, s->rowY - ch / 4 - ch / 2, cw, ch / 2 + 2, "^", smTheme.dim, 1 );
	}
	if ( *top + vis < s->numRows ) {
		SM_DrawText( px + panelW - cw, s->rowY + vis * lineH, cw, ch / 2 + 2, "v", smTheme.dim, 1 );
	}

	// R14b: the selected map's levelshot beside the map list (skipped in small cells)
	if ( s->stack[ s->depth - 1 ].page == &smSrvMaps && *sel < s->numRows && s->rows[ *sel ].type == SMR_ACTION ) {
		const qhandle_t shot = CL_SplitSrvLevelshot( CL_SplitSrvMapName( s->rows[ *sel ].arg ) );
		int tw = panelW / 2 - cw, th = tw * 3 / 4;
		if ( th > vis * lineH - ch / 2 ) {
			th = vis * lineH - ch / 2;
			tw = th * 4 / 3;
		}
		if ( th >= lineH * 3 ) {
			x = px + panelW - cw - tw;
			y = s->rowY;
			SM_Fill( x - 2, y - 2, tw + 4, th + 4, smTheme.accent );
			if ( shot ) {
				re.SetColor( NULL );
				re.DrawStretchPic( x, y, tw, th, 0, 0, 1, 1, shot );
			} else {
				SM_Fill( x, y, tw, th, smBlack );
				len = MIN( 11, tw / cw - 1 );
				SM_DrawText( x + ( tw - len * cw ) / 2, y + ( th - ch ) / 2, cw, ch, "no picture", smTheme.dim, len );
			}
		}
	}

	// footer: what the buttons do, or the last note (R14b: the selected row's help above them)
	y = py + panelH - footH;
	SM_Fill( px, y, panelW, footH, smTheme.footBg );
	if ( hasHelp ) {
		r = ( *sel < s->numRows ) ? &s->rows[ *sel ] : NULL;
		if ( r && r->help && r->helpWrap && helpLines > 1 && (int)strlen( r->help ) > cols ) {
			// R19: wrapped at the last space that fits
			int split = cols;
			while ( split > 0 && r->help[split] != ' ' ) {
				split--;
			}
			if ( split <= 0 ) {
				split = cols;
			}
			SM_DrawText( px + cw, y + ( lineH - ch ) / 2, cw, ch, r->help, smTheme.value, split );
			SM_DrawText( px + cw, y + lineH + ( lineH - ch ) / 2, cw, ch, r->help + split + ( r->help[split] == ' ' ? 1 : 0 ),
				smTheme.value, cols );
		} else if ( r && r->help ) {
			SM_DrawText( px + cw, y + ( lineH - ch ) / 2, cw, ch, r->help, smTheme.value, cols );
		}
		y += lineH * helpLines;
		footH = lineH;
	}
	if ( s->note[0] && cls.realtime - s->noteTime < SM_NOTE_TIME ) {
		SM_DrawText( px + cw, y + ( footH - ch ) / 2, cw, ch, s->note, smTheme.selText, cols );
	} else {
		r = ( *sel < s->numRows ) ? &s->rows[ *sel ] : NULL;
		Com_sprintf( foot, sizeof( foot ), "%s select  %s back%s", IN_PadButtonName( n, "PAD_A" ),
			IN_PadButtonName( n, "PAD_B" ), ( r && ( r->type == SMR_CHOICE || r->type == SMR_SLIDER ) ) ? "  <> change" : "" );
		SM_DrawText( px + cw, y + ( footH - ch ) / 2, cw, ch, foot, smTheme.dim, cols );
	}

	// "press a button" prompt over the rows
	if ( s->capture ) {
		const int bw = panelW - 4 * cw, bh = lineH * 3 + ch;
		const int bx = px + 2 * cw, by = s->rowY + ( vis * lineH - bh ) / 2;
		SM_Fill( bx - 2, by - 2, bw + 4, bh + 4, smTheme.accent );
		SM_Fill( bx, by, bw, bh, smBlack );
		Com_sprintf( foot, sizeof( foot ), "Press a button for %s", s->captureName );
		len = MIN( (int)strlen( foot ), bw / cw - 2 );
		SM_DrawText( bx + ( bw - len * cw ) / 2, by + ch / 2, cw, ch, foot, smWhite, len );
		Com_sprintf( foot, sizeof( foot ), "(%s cancels)", IN_PadButtonName( n, "PAD_START" ) );
		len = MIN( (int)strlen( foot ), bw / cw - 2 );
		SM_DrawText( bx + ( bw - len * cw ) / 2, by + ch / 2 + lineH * 2, cw, ch, foot, smTheme.dim, len );
	}

cursor:
	// right-stick cursor
	if ( s->cursorOn ) {
		x = cell.x + (int)s->cx;
		y = cell.y + (int)s->cy;
		SM_Fill( x - 1, y - 1, cw / 2 + 4, cw / 2 + 4, smBlack );
		SM_Fill( x, y, cw / 2 + 2, cw / 2 + 2, smWhite );
	}
}


// every open overlay, in its owner's cell, over the views and menus
void CL_SplitMenuDraw( void ) {
	int n;

	SM_UpdateTheme();
	if ( CL_IndepCoordinator() && cls.state == CA_DISCONNECTED ) {
		IN_GamepadDrawHint();	// Independent mode: pads join from the main menu too
	}
	IN_GamepadDrawFirstHint();	// R20: Together main menu, first launch only
	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( sm[n].depth > 0 ) {
			SM_DrawPlayer( n );
		}
	}
}


/*
=============================================================================

COMMANDS

=============================================================================
*/

// splitsettings: the host's splitscreen page (player 1; full screen at the main menu)
static void SM_Settings_f( void ) {
	CL_SplitMenuSettings( 0 );
}

// serveroptions: player 1's Server options page (R14b; keyboard players and test scripts)
static void SM_ServerOptions_f( void ) {
	if ( CL_SplitRemoteServer() ) {
		Com_Printf( "Server options: host only (this is another machine's server)\n" );
		return;
	}
	SM_Open( 0, &smServer );
}


// splitcursor <player>: where its cursors are (tests: R17 Cursor speed)
static void SM_Cursor_f( void ) {
	const int n = atoi( Cmd_Argv( 1 ) ) - 1;
	float x, y;

	if ( (unsigned)n >= MAX_SPLITVIEW ) {
		Com_Printf( "usage: splitcursor <player 1-%i>\n", MAX_SPLITVIEW );
		return;
	}
	Com_Printf( "P%i cursor speed: these menus %.0f, the game's menus %.0f menu units/s at full push\n", n + 1,
		IN_PadCursorSpeed( n, qfalse ), IN_PadCursorSpeed( n, qtrue ) );
	if ( sm[n].depth > 0 && sm[n].cursorOn && sm[n].cell.w > 0 ) {
		Com_Printf( "P%i overlay cursor: %.1f,%.1f menu units (640 = cell width)\n", n + 1,
			sm[n].cx * 640.0f / sm[n].cell.w, sm[n].cy * 640.0f / sm[n].cell.w );
	}
	if ( CL_SplitUICursor( n, &x, &y ) ) {
		Com_Printf( "P%i game menu cursor drawn at %.0f,%.0f (its screen)\n", n + 1, x, y );
	}
}


void CL_SplitMenuInit( void ) {
	Com_Memset( sm, 0, sizeof( sm ) );
	Cmd_AddCommand( "splitcursor", SM_Cursor_f );
	Cmd_AddCommand( "splitsettings", SM_Settings_f );
	Cmd_AddCommand( "serveroptions", SM_ServerOptions_f );
	cl_splitMenuColor = Cvar_Get( "cl_splitMenuColor", "auto", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_splitMenuColor, "Splitscreen overlay accent colour, \"r g b\" 0..1; \"auto\" (default) = the game's: Quake 3 red, Urban Terror blue." );
	cl_splitMenuSize = Cvar_Get( "cl_splitMenuSize", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitMenuSize, "0.5", "1.5", CV_FLOAT );
	Cvar_SetDescription( cl_splitMenuSize, "Splitscreen overlay (pause menu, profiles, keyboard) text size, 0.5 .. 1.5, default 1 (Menu size in Splitscreen settings)." );
}


void CL_SplitMenuShutdown( void ) {
	Com_Memset( sm, 0, sizeof( sm ) );
	Cmd_RemoveCommand( "splitsettings" );
	Cmd_RemoveCommand( "serveroptions" );
	Cmd_RemoveCommand( "splitcursor" );
}
