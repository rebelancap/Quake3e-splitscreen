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
// in_gamepad.c -- gamepads for local splitscreen players (design doc 6)
//
// Backend: SDL2's GameController API.  The Windows client exes link SDL2 in
// statically (Q3E_SDL_STATIC, set by scripts/splitscreen.props with the lib
// scripts/build.ps1 builds); other builds load it at run time (SDL2.dll beside
// the exe, libSDL2-2.0.so.0).  Pads are disabled if SDL is missing or fails.  Only the joystick/gamecontroller subsystems are started:
// SDL never sees the engine's window, keyboard or mouse.  Pads are polled
// once per frame (IN_GamepadFrame from the platform input frame).
//
// Every pad is a device slot in pads[].  Real (SDL) and virtual ('padinject',
// for headless tests) devices go through the same Pad_Attach / Pad_SetButton
// / Pad_SetAxis / Pad_Detach functions.  A pad belongs to at most one local
// player; its button presses execute that player's pad bindings in that
// player's context ('p<N> <command>'), its sticks feed CL_GamepadMove when
// that player's usercmd is built.
//
// All pad key events pass through Pad_RouteKey: the single place where a
// later per-player menu / overlay will take them instead of the binds.

#include "client.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define MAX_PADS			8

typedef enum {
	PAD_A, PAD_B, PAD_X, PAD_Y,
	PAD_BACK, PAD_GUIDE, PAD_START,
	PAD_L3, PAD_R3, PAD_LB, PAD_RB,
	PAD_DPAD_UP, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT,
	PAD_MISC1, PAD_PADDLE1, PAD_PADDLE2, PAD_PADDLE3, PAD_PADDLE4, PAD_TOUCHPAD,
	PAD_SDL_BUTTONS,	// = SDL_CONTROLLER_BUTTON_MAX (2.0.14+), same order as SDL's
	PAD_LT = PAD_SDL_BUTTONS, PAD_RT,
	PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT,
	PAD_RSTICK_UP, PAD_RSTICK_DOWN, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT,
	PAD_KEY_COUNT
} padKey_t;

static const char *padKeyNames[PAD_KEY_COUNT] = {
	"PAD_A", "PAD_B", "PAD_X", "PAD_Y",
	"PAD_BACK", "PAD_GUIDE", "PAD_START",
	"PAD_L3", "PAD_R3", "PAD_LB", "PAD_RB",
	"PAD_DPAD_UP", "PAD_DPAD_DOWN", "PAD_DPAD_LEFT", "PAD_DPAD_RIGHT",
	"PAD_MISC1", "PAD_PADDLE1", "PAD_PADDLE2", "PAD_PADDLE3", "PAD_PADDLE4", "PAD_TOUCHPAD",
	"PAD_LT", "PAD_RT",
	"PAD_LSTICK_UP", "PAD_LSTICK_DOWN", "PAD_LSTICK_LEFT", "PAD_LSTICK_RIGHT",
	"PAD_RSTICK_UP", "PAD_RSTICK_DOWN", "PAD_RSTICK_LEFT", "PAD_RSTICK_RIGHT",
};

typedef enum { PAX_LX, PAX_LY, PAX_RX, PAX_RY, PAX_LT, PAX_RT, PAX_COUNT } padAxis_t;	// SDL order
static const char *padAxisNames[PAX_COUNT] = { "lx", "ly", "rx", "ry", "lt", "rt" };

typedef enum { PADTYPE_OTHER, PADTYPE_XBOX, PADTYPE_PLAYSTATION, PADTYPE_NINTENDO } padType_t;
static const char *padTypeNames[] = { "other", "xbox", "playstation", "nintendo" };

typedef struct {
	qboolean	connected;
	qboolean	isVirtual;
	void		*ctrl;				// SDL_GameController *
	int			instanceId;			// SDL joystick instance id
	int			busKey;				// R19 test bus device: its id + 1 (0 = not a bus device)
	int			busGen;				// and its connection number there
	char		key[64];			// R19: stable device key, the same in every process (Pad_MakeKey)
	int			mirrorOf;			// R19: index + 1 of the pad this one duplicates (0 = none)
	int			mirrorDiff;			// R19: msec since its input stopped matching that pad's (0 = matching)
	int			mirrorSince;		// R19b: when it was taken for a duplicate
	int			mirrorEdges;		// R19b: later presses that matched the other pad's (confirmations)
	int			mirrorEdgeTime;		// R19b: downTime of the last press counted
	qboolean	mirrorAliased;		// R19b: its key was given to the other pad's window
	int			attachTime;			// R19: Sys_Milliseconds when it appeared
	char		guid[40];
	char		name[64];
	padType_t	type;

	int			player;				// local player slot (0-based) or -1 = not joined
	float		axis[PAX_COUNT];	// sticks -1..1 (y down = +), triggers 0..1
	qboolean	down[PAD_KEY_COUNT];
	qboolean	sentDown[PAD_KEY_COUNT];	// a bind down went to the player: send the up
	int			downTime[PAD_KEY_COUNT];
	int			leaveStart;			// Back+Start held since (0 = not)
	qboolean	leaveDone;
	qboolean	joinDone;			// join hold handled, wait for release
	int			lastInput;			// Sys_Milliseconds of its last real input (0 = never)

	// menu control (design 13.2): while its player has a menu open
	qboolean	inMenu;				// the pad is driving a menu
	qboolean	menuDown[PAD_KEY_COUNT];	// this down went to the menu: so does the up
	int			cgameDown[PAD_KEY_COUNT];	// key number sent down to a key-catching cgame (0 = none)
	int			repeatAt[PAD_KEY_COUNT];	// next auto-repeat of a held arrow
	qboolean	captured[PAD_KEY_COUNT];	// this down was taken by a "press a button" prompt: no repeats
	float		cursorRem[2];		// sub-unit cursor movement carried to the next frame
	float		cursorHold;			// seconds the right stick has been pushed far (acceleration)
} padDevice_t;

static padDevice_t	pads[MAX_PADS];

// per local player: pad bindings and auto-heal memory
typedef struct {
	char		*binds[PAD_KEY_COUNT];
	qboolean	initialized;		// binds hold defaults or a saved/custom table
	qboolean	custom;				// changed by pbind/punbind (saved to splitpads.cfg)
	qboolean	lostPad;			// its pad disconnected in game
	char		lostGuid[40];
	int			latched[2];		// crouch / sprint toggle (PADTOG_*): pad key + 1 that latched its command (0 = none)
	int			sprintIdle;		// R18: msec the left stick has rested while sprint is latched
	int			indepLostAt;	// R19, Independent mode coordinator: when this window's pad went away (0 = not lost)
} padSlot_t;

#define PAD_DEFAULTS	MAX_SPLITVIEW	// padSlots[PAD_DEFAULTS] = the default bind set (menu reference)
static padSlot_t	padSlots[MAX_SPLITVIEW + 1];
static qboolean		padInitialized;

static cvar_t	*in_gamepad;
static cvar_t	*in_padDebug;
static cvar_t	*cl_splitP1Input;
static cvar_t	*cl_splitJoinButton;
static cvar_t	*cl_splitJoinHold;
static cvar_t	*cl_splitJoinHint;
static cvar_t	*cl_splitSeenHint;		// R20: the first-launch main-menu hint was shown and a game has started
static cvar_t	*cl_splitLeaveHold;
static cvar_t	*joy_deadzone;
static cvar_t	*joy_deadzoneOuter;
static cvar_t	*joy_yawSpeed;
static cvar_t	*joy_pitchSpeed;
static cvar_t	*joy_aimCurve;
static cvar_t	*joy_aimExponent;
static cvar_t	*joy_turnBoost;
static cvar_t	*joy_turnBoostTime;
static cvar_t	*joy_zoomScale;
static cvar_t	*joy_aimSmooth;
static cvar_t	*joy_crouchToggle;
static cvar_t	*joy_sprintToggle;	// R18
static cvar_t	*joy_alwaysRun;
static cvar_t	*joy_invertPitch;
static cvar_t	*joy_triggerThreshold;
static cvar_t	*joy_stickThreshold;
static cvar_t	*joy_walkThreshold;
static cvar_t	*joy_aimAssist;		// per-player strength (cl_aimassist.c)
static cvar_t	*joy_fov;			// R14a: per-player field of view (its cgame's cg_fov)
static cvar_t	*joy_cursorSpeed;	// R17: per-player menu cursor speed (both cursors), 1 = 100 %
static cvar_t	*cl_padCursorSpeed;
static cvar_t	*cl_padModCursorScale;	// R14a: mod-menu cursor vs cl_padCursorSpeed (1.25)

// per-player look/aim/toggle settings (design 14.4): each player reads its
// p<N>_<name> shadow, created from the shared value (the guest default)
static cvar_t **const padFeelCvars[] = {
	&joy_yawSpeed, &joy_pitchSpeed, &joy_invertPitch, &joy_aimCurve, &joy_aimExponent,
	&joy_turnBoost, &joy_turnBoostTime, &joy_zoomScale, &joy_aimSmooth,
	&joy_deadzone, &joy_deadzoneOuter, &joy_walkThreshold, &joy_alwaysRun, &joy_aimAssist,
	&joy_fov, &joy_cursorSpeed
};

// valid range of each (profile files are checked against it; aim curve is a word)
static const float padFeelRange[][2] = {
	{ 10, 1000 }, { 10, 1000 }, { 0, 1 }, { 0, 0 }, { 0.5f, 4 },
	{ 1, 3 }, { 0, 2000 }, { 0.1f, 1 }, { 0, 1 },
	{ 0, 0.9f }, { 0.1f, 1 }, { 0, 1 }, { 0, 1 }, { 0, 2 },
	{ 80, 130 }, { 0.5f, 2 }
};


/*
R18: hold-or-toggle buttons.  Per player and per game: they belong to the
game's button layout (Urban Terror starts with crouch and sprint toggled,
baseq3 holds), so they live with the bind table -- 'toggle crouch 1' lines
in default_pad.cfg / the built-in data / <game>/profiles/<key>.cfg -- and in
the player's p<N>_joy_*Toggle shadow (guest_ for the Guest defaults), which
the Controls page edits like a feel setting.  Not in the shared profile file.
*/
typedef enum { PADTOG_CROUCH, PADTOG_SPRINT, PADTOG_COUNT } padToggle_t;
static cvar_t **const padToggleCvars[PADTOG_COUNT] = { &joy_crouchToggle, &joy_sprintToggle };
static const char *const padToggleWords[PADTOG_COUNT] = { "crouch", "sprint" };
static const char *const padToggleCmds[PADTOG_COUNT] = { "+movedown", "+button8" };	// +button8: UrT sprint
#define PADDEFAULT_FILE	"default_pad.cfg"

// engine-embedded baseq3 defaults (the maintainer's Spearmint layout where it maps)
static const char *padDefaultBinds =
	"bind PAD_A \"+moveup\"\n"
	"bind PAD_B \"+movedown\"\n"
	"bind PAD_X \"+button3\"\n"
	"bind PAD_Y \"+button2\"\n"
	"bind PAD_BACK \"+scores\"\n"
	"bind PAD_START \"padmenu\"\n"
	"bind PAD_LB \"weapprev\"\n"
	"bind PAD_RB \"weapnext\"\n"
	"bind PAD_DPAD_LEFT \"weapprev\"\n"
	"bind PAD_DPAD_RIGHT \"weapnext\"\n"
	"bind PAD_LT \"+zoom\"\n"
	"bind PAD_RT \"+attack\"\n"
	"bind PAD_L3 \"+speed\"\n"
	"bind PAD_R3 \"centerview\"\n";

/*
Per-game factory data, used when the game has no default_pad.cfg of its own
(<game>/default_pad.cfg through the VFS always wins).  Same format as that
file: 'bind <padkey> "<command>"' lines, 'toggle <crouch|sprint> <0|1>' lines (R18:
the default of that hold-or-toggle setting, 0 when absent), plus 'playercvar <name>' lines
(a trailing * matches a prefix): cvars that are per player for players 2-8
in their cgame and menu even though they are not userinfo (their p<N>_
shadows), for mod menus that keep a player's choices in shared cvars.
Keyed by the game directory (fs_game, else the base game).
*/
static const struct {
	const char	*game;
	const char	*text;
} padGameDefaults[] = {
	// Urban Terror 4.x, R18: the maintainer's layout (UrT's own commands, from its
	// ui/controls.menu and default.cfg).  LT steps the zoom (UrT has no
	// hold-to-zoom), LB resets it; crouch and sprint toggle by default.
	// Every other UrT action is on the Buttons page, unbound (cl_splitmenu.c).
	// Its gear menu keeps the choice being edited in ui_gear* while it is open.
	{ "q3ut4",
		"bind PAD_A \"+moveup\"\n"				// jump
		"bind PAD_B \"+movedown\"\n"			// crouch (toggle)
		"bind PAD_X \"+button5\"\n"			// reload
		"bind PAD_Y \"+button6\"\n"			// bandage
		"bind PAD_BACK \"+scores\"\n"
		"bind PAD_START \"padmenu\"\n"
		"bind PAD_LB \"ut_zoomreset\"\n"		// reset zoom
		"bind PAD_RB \"weapnext\"\n"			// next weapon
		"bind PAD_LT \"ut_zoomin\"\n"			// zoom in
		"bind PAD_RT \"+attack\"\n"
		"bind PAD_L3 \"+button8\"\n"			// sprint (toggle)
		"bind PAD_R3 \"ut_weaptoggle knife\"\n"	// knife
		"bind PAD_DPAD_UP \"ut_itemdrop\"\n"	// drop item
		"bind PAD_DPAD_DOWN \"ut_weapdrop\"\n"	// drop weapon
		"bind PAD_DPAD_LEFT \"ut_itemuse nvg\"\n"	// IR vision (night vision goggles)
		"bind PAD_DPAD_RIGHT \"+button3\"\n"	// weapon fire mode
		"bind PAD_MISC1 \"ut_itemuse\"\n"		// toggle the current item
		"toggle crouch 1\n"
		"toggle sprint 1\n"
		"playercvar ui_gear*\n"
		"playercvar weapmodes_save\n" },
};

/*
Earlier built-in layouts: a profile (or Guest defaults) file whose bind table
is exactly one of these was never edited by its player, so it is moved to the
current built-in layout when it loads (IN_PadUpgradeBinds).
*/
static const struct {
	const char	*game;
	const char	*round;
	const char	*text;
} padOldDefaults[] = {
	{ "q3ut4", "R11",
		"bind PAD_A \"+moveup\"\n"
		"bind PAD_B \"+movedown\"\n"
		"bind PAD_X \"+button5\"\n"
		"bind PAD_Y \"+button7\"\n"
		"bind PAD_BACK \"+scores\"\n"
		"bind PAD_START \"padmenu\"\n"
		"bind PAD_LB \"weapprev\"\n"
		"bind PAD_RB \"weapnext\"\n"
		"bind PAD_LT \"ut_zoomin\"\n"
		"bind PAD_RT \"+attack\"\n"
		"bind PAD_L3 \"+button8\"\n"
		"bind PAD_R3 \"+button6\"\n"
		"bind PAD_DPAD_UP \"ut_radio 2 6\"\n"
		"bind PAD_DPAD_DOWN \"+button3\"\n"
		"bind PAD_DPAD_LEFT \"ut_radio 1 1\"\n"
		"bind PAD_DPAD_RIGHT \"ut_radio 5 1\"\n"
		"bind PAD_MISC1 \"ut_itemuse\"\n" },
};

#define MAX_PAD_PLAYERCVARS	16
static char	padPlayerCvars[MAX_PAD_PLAYERCVARS][MAX_CVAR_VALUE_STRING];
static int	padNumPlayerCvars;
static char	padPlayerCvarsGame[MAX_QPATH];		// game dir they were read for
static qboolean	padPlayerCvarsRead;

/*
==================
Pad_FactoryText

The factory pad data of the current game: <game>/default_pad.cfg, else the
engine's entry for this game, else the baseq3 set.  *buf is set when the
text is a file that must be freed with FS_FreeFile.
==================
*/
static const char *Pad_FactoryText( void **buf, const char **source ) {
	static char label[MAX_QPATH + 32];
	const char *game = FS_GetCurrentGameDir();
	int i;

	*buf = NULL;
	if ( FS_ReadFile( PADDEFAULT_FILE, buf ) > 0 && *buf ) {
		*source = PADDEFAULT_FILE;
		return (const char *)*buf;
	}
	*buf = NULL;
	for ( i = 0; i < (int)ARRAY_LEN( padGameDefaults ); i++ ) {
		if ( !Q_stricmp( game, padGameDefaults[i].game ) ) {
			Com_sprintf( label, sizeof( label ), "built-in %s pad binds", padGameDefaults[i].game );
			*source = label;
			return padGameDefaults[i].text;
		}
	}
	*source = "built-in pad binds";
	return padDefaultBinds;
}


// the 'playercvar' lines of the current game's factory data, read once per game
static void Pad_ReadPlayerCvars( void ) {
	const char *text, *source, *p, *tok;
	void *buf;

	if ( padPlayerCvarsRead && !Q_stricmp( padPlayerCvarsGame, FS_GetCurrentGameDir() ) ) {
		return;
	}
	padPlayerCvarsRead = qtrue;
	Q_strncpyz( padPlayerCvarsGame, FS_GetCurrentGameDir(), sizeof( padPlayerCvarsGame ) );
	padNumPlayerCvars = 0;

	p = text = Pad_FactoryText( &buf, &source );
	while ( 1 ) {
		tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		if ( Q_stricmp( tok, "playercvar" ) ) {
			SkipRestOfLine( &p );
			continue;
		}
		tok = COM_ParseExt( &p, qfalse );
		if ( tok[0] && padNumPlayerCvars < MAX_PAD_PLAYERCVARS ) {
			Q_strncpyz( padPlayerCvars[ padNumPlayerCvars++ ], tok, sizeof( padPlayerCvars[0] ) );
		}
	}
	if ( buf ) {
		FS_FreeFile( buf );
	}
	if ( padNumPlayerCvars ) {
		Com_DPrintf( "%s: %i per-player cvar pattern(s)\n", source, padNumPlayerCvars );
	}
}


/*
==================
IN_PadPlayerCvar

A cvar the current game's factory data lists as per player ('playercvar').
==================
*/
qboolean IN_PadPlayerCvar( const char *name ) {
	int i, len;

	if ( !name || !name[0] ) {
		return qfalse;
	}
	Pad_ReadPlayerCvars();
	for ( i = 0; i < padNumPlayerCvars; i++ ) {
		len = (int)strlen( padPlayerCvars[i] );
		if ( len > 0 && padPlayerCvars[i][len-1] == '*' ) {
			if ( !Q_stricmpn( name, padPlayerCvars[i], len - 1 ) ) {
				return qtrue;
			}
		} else if ( !Q_stricmp( name, padPlayerCvars[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


/*
=============================================================================

SDL2 (Windows clients: linked in statically, Q3E_SDL_STATIC; elsewhere
loaded at run time.  Prototypes declared here, the bundled 2.0.10 headers
predate SDL_GameControllerTypeForIndex)

=============================================================================
*/

typedef struct { unsigned char data[16]; } sdlGUID_t;
typedef struct { unsigned char major, minor, patch; } sdlVersion_t;

#define SDL_INIT_JOYSTICK		0x00000200u
#define SDL_INIT_GAMECONTROLLER	0x00002000u
#define SDL_IGNORE				0

static struct {
	void		*lib;
	qboolean	active;
	sdlVersion_t version;

	int			( *InitSubSystem )( unsigned int flags );
	void		( *QuitSubSystem )( unsigned int flags );
	void		( *Quit )( void );
	int			( *SetHint )( const char *name, const char *value );
	void		( *GetVersion )( sdlVersion_t *ver );
	const char	*( *GetError )( void );
	void		( *SetMainReady )( void );
	void		( *FlushEvents )( unsigned int minType, unsigned int maxType );
	int			( *NumJoysticks )( void );
	int			( *IsGameController )( int index );
	const char	*( *JoystickNameForIndex )( int index );
	sdlGUID_t	( *JoystickGetDeviceGUID )( int index );
	void		( *JoystickGetGUIDString )( sdlGUID_t guid, char *out, int size );
	int			( *JoystickGetDeviceInstanceID )( int index );
	unsigned short ( *JoystickGetDeviceVendor )( int index );
	unsigned short ( *JoystickGetDeviceProduct )( int index );
	int			( *JoystickEventState )( int state );
	int			( *JoystickInstanceID )( void *joystick );
	int			( *GameControllerTypeForIndex )( int index );	// 2.0.12+, optional
	const char	*( *JoystickPathForIndex )( int index );		// 2.24+, optional (padlist)
	void		*( *GameControllerOpen )( int index );
	void		( *GameControllerClose )( void *ctrl );
	int			( *GameControllerGetAttached )( void *ctrl );
	void		*( *GameControllerGetJoystick )( void *ctrl );
	const char	*( *GameControllerName )( void *ctrl );
	void		( *GameControllerUpdate )( void );
	int			( *GameControllerEventState )( int state );
	unsigned char ( *GameControllerGetButton )( void *ctrl, int button );
	short		( *GameControllerGetAxis )( void *ctrl, int axis );
} sdl;

// SDL instance ids already reported as not usable (logged once)
#define MAX_IGNORED 32
static int		sdlIgnored[MAX_IGNORED];
static int		sdlNumIgnored;
static int		busIgnored[MAX_IGNORED];	// R19 test bus instances not opened here (logged once)
static int		busNumIgnored;


#ifdef Q3E_SDL_STATIC
// SDL2's own functions (SDLCALL = __cdecl), declared with this file's types:
// SDL_bool / enums = int, SDL_JoystickGUID = sdlGUID_t, SDL_Joystick * and
// SDL_GameController * = void *
int				SDL_InitSubSystem( unsigned int flags );
void			SDL_QuitSubSystem( unsigned int flags );
void			SDL_Quit( void );
int				SDL_SetHint( const char *name, const char *value );
void			SDL_GetVersion( sdlVersion_t *ver );
const char		*SDL_GetError( void );
void			SDL_SetMainReady( void );
void			SDL_FlushEvents( unsigned int minType, unsigned int maxType );
int				SDL_NumJoysticks( void );
int				SDL_IsGameController( int index );
const char		*SDL_JoystickNameForIndex( int index );
sdlGUID_t		SDL_JoystickGetDeviceGUID( int index );
void			SDL_JoystickGetGUIDString( sdlGUID_t guid, char *out, int size );
int				SDL_JoystickGetDeviceInstanceID( int index );
unsigned short	SDL_JoystickGetDeviceVendor( int index );
unsigned short	SDL_JoystickGetDeviceProduct( int index );
int				SDL_JoystickEventState( int state );
int				SDL_JoystickInstanceID( void *joystick );
int				SDL_GameControllerTypeForIndex( int index );
const char		*SDL_JoystickPathForIndex( int index );
void			*SDL_GameControllerOpen( int index );
void			SDL_GameControllerClose( void *ctrl );
int				SDL_GameControllerGetAttached( void *ctrl );
void			*SDL_GameControllerGetJoystick( void *ctrl );
const char		*SDL_GameControllerName( void *ctrl );
void			SDL_GameControllerUpdate( void );
int				SDL_GameControllerEventState( int state );
unsigned char	SDL_GameControllerGetButton( void *ctrl, int button );
short			SDL_GameControllerGetAxis( void *ctrl, int axis );

#define SDLG_KIND "static (built in)"

static void *SDLG_Load( void ) {
	return (void *)&sdl;	// linked in: always there
}


static void SDLG_Unload( void ) {
	Com_Memset( &sdl, 0, sizeof( sdl ) );
}

#else // !Q3E_SDL_STATIC

#define SDLG_KIND "loaded"

static void *SDLG_Load( void ) {
#ifdef _WIN32
	char path[MAX_OSPATH], *s;
	DWORD len;

	// only the copy beside the executable (no search path, no hijacking)
	len = GetModuleFileNameA( NULL, path, sizeof( path ) );
	if ( len == 0 || len >= sizeof( path ) ) {
		return NULL;
	}
	s = strrchr( path, '\\' );
	if ( !s ) {
		return NULL;
	}
	Q_strncpyz( s + 1, "SDL2.dll", sizeof( path ) - (int)( s + 1 - path ) );
	return (void *)LoadLibraryA( path );
#else
	return Sys_LoadLibrary( "libSDL2-2.0.so.0" );
#endif
}


static void SDLG_Unload( void ) {
	if ( sdl.lib ) {
#ifdef _WIN32
		FreeLibrary( (HMODULE)sdl.lib );
#else
		Sys_UnloadLibrary( sdl.lib );
#endif
	}
	Com_Memset( &sdl, 0, sizeof( sdl ) );
}


static void *SDLG_Func( const char *name, qboolean required, qboolean *ok ) {
	void *f;
#ifdef _WIN32
	f = (void *)GetProcAddress( (HMODULE)sdl.lib, name );
#else
	f = Sys_LoadFunction( sdl.lib, name );
#endif
	if ( !f && required ) {
		Com_Printf( "gamepad: SDL2 lacks %s\n", name );
		*ok = qfalse;
	}
	return f;
}

#endif // Q3E_SDL_STATIC


static qboolean SDLG_Init( void ) {
	qboolean ok = qtrue;

	sdl.lib = SDLG_Load();
	if ( !sdl.lib ) {
#ifdef _WIN32
		Com_Printf( "gamepad: SDL2.dll not found beside the executable -- gamepads disabled "
			"(keyboard/mouse and console players still work)\n" );
#else
		Com_Printf( "gamepad: libSDL2-2.0.so.0 not found -- gamepads disabled "
			"(keyboard/mouse and console players still work)\n" );
#endif
		return qfalse;
	}

#ifdef Q3E_SDL_STATIC
#define SDLF( field, name ) sdl.field = name
#define SDLF_OPT( field, name ) sdl.field = name
#else
#define SDLF( field, name ) *(void **)&sdl.field = SDLG_Func( #name, qtrue, &ok )
#define SDLF_OPT( field, name ) *(void **)&sdl.field = SDLG_Func( #name, qfalse, &ok )
#endif
	SDLF( InitSubSystem, SDL_InitSubSystem );
	SDLF( QuitSubSystem, SDL_QuitSubSystem );
	SDLF( Quit, SDL_Quit );
	SDLF( SetHint, SDL_SetHint );
	SDLF( GetVersion, SDL_GetVersion );
	SDLF( GetError, SDL_GetError );
	SDLF_OPT( SetMainReady, SDL_SetMainReady );
	SDLF( FlushEvents, SDL_FlushEvents );
	SDLF( NumJoysticks, SDL_NumJoysticks );
	SDLF( IsGameController, SDL_IsGameController );
	SDLF( JoystickNameForIndex, SDL_JoystickNameForIndex );
	SDLF( JoystickGetDeviceGUID, SDL_JoystickGetDeviceGUID );
	SDLF( JoystickGetGUIDString, SDL_JoystickGetGUIDString );
	SDLF( JoystickGetDeviceInstanceID, SDL_JoystickGetDeviceInstanceID );
	SDLF( JoystickGetDeviceVendor, SDL_JoystickGetDeviceVendor );
	SDLF( JoystickGetDeviceProduct, SDL_JoystickGetDeviceProduct );
	SDLF( JoystickEventState, SDL_JoystickEventState );
	SDLF( JoystickInstanceID, SDL_JoystickInstanceID );
	SDLF_OPT( GameControllerTypeForIndex, SDL_GameControllerTypeForIndex );
	SDLF_OPT( JoystickPathForIndex, SDL_JoystickPathForIndex );
	SDLF( GameControllerOpen, SDL_GameControllerOpen );
	SDLF( GameControllerClose, SDL_GameControllerClose );
	SDLF( GameControllerGetAttached, SDL_GameControllerGetAttached );
	SDLF( GameControllerGetJoystick, SDL_GameControllerGetJoystick );
	SDLF( GameControllerName, SDL_GameControllerName );
	SDLF( GameControllerUpdate, SDL_GameControllerUpdate );
	SDLF( GameControllerEventState, SDL_GameControllerEventState );
	SDLF( GameControllerGetButton, SDL_GameControllerGetButton );
	SDLF( GameControllerGetAxis, SDL_GameControllerGetAxis );
#undef SDLF
#undef SDLF_OPT

	if ( !ok ) {
		Com_Printf( "gamepad: the SDL2 library is too old or broken -- gamepads disabled\n" );
		SDLG_Unload();
		return qfalse;
	}

	sdl.GetVersion( &sdl.version );

	// pads only: work while the (native win32) window is unfocused, poll from
	// SDL's own joystick thread, positional face buttons (PAD_A = bottom),
	// leave the process's signal handlers alone
	sdl.SetHint( "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1" );
	sdl.SetHint( "SDL_JOYSTICK_THREAD", "1" );
	sdl.SetHint( "SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0" );
	sdl.SetHint( "SDL_NO_SIGNAL_HANDLERS", "1" );
	if ( sdl.SetMainReady ) {
		sdl.SetMainReady();
	}

	if ( sdl.InitSubSystem( SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER ) < 0 ) {
		Com_Printf( "gamepad: SDL %i.%i.%i joystick/gamecontroller init failed: %s\n",
			sdl.version.major, sdl.version.minor, sdl.version.patch, sdl.GetError() );
		SDLG_Unload();
		return qfalse;
	}

	// state is polled; no event queue is drained, so keep it empty
	sdl.JoystickEventState( SDL_IGNORE );
	sdl.GameControllerEventState( SDL_IGNORE );

	sdl.active = qtrue;
	Com_Printf( "gamepad: SDL %i.%i.%i " SDLG_KIND ", subsystems joystick+gamecontroller initialised "
		"(%i joystick device(s) present)\n", sdl.version.major, sdl.version.minor, sdl.version.patch,
		sdl.NumJoysticks() );
	return qtrue;
}


/*
=============================================================================

DEVICE TABLE

=============================================================================
*/

static void Pad_RouteKey( padDevice_t *pad, int key, qboolean down, int time );
static void Pad_Unassign( padDevice_t *pad );
static void Pad_ReleaseCrouch( int player, int key );
static void Pad_ReleaseHeld( int player, int key );
static float Pad_PlayerValue( int player, const cvar_t *base );
static float Pad_Radial( float x, float y, float inner, float outer, float *dx, float *dy );

static int Pad_Index( const padDevice_t *pad ) {
	return (int)( pad - pads );
}


static padDevice_t *Pad_ForPlayer( int player ) {
	int i;
	for ( i = 0; i < MAX_PADS; i++ ) {
		if ( pads[i].connected && pads[i].player == player ) {
			return &pads[i];
		}
	}
	return NULL;
}


static void Pad_SetButton( padDevice_t *pad, int key, qboolean down ) {
	const int now = Sys_Milliseconds();

	if ( pad->down[key] == down ) {
		return;
	}
	pad->down[key] = down;
	if ( down ) {
		pad->downTime[key] = now;
		pad->lastInput = now ? now : 1;
	}
	if ( in_padDebug->integer ) {
		Com_Printf( "pad %i: %s %s\n", Pad_Index( pad ), padKeyNames[key], down ? "down" : "up" );
	}
	Pad_RouteKey( pad, key, down, now );
}


// axis value with hysteresis-free threshold
static void Pad_AxisKey( padDevice_t *pad, int key, float v, float threshold ) {
	Pad_SetButton( pad, key, ( v >= threshold ) ? qtrue : qfalse );
}


static void Pad_SetAxis( padDevice_t *pad, int axis, float v ) {
	float st, tt;

	if ( axis == PAX_LT || axis == PAX_RT ) {
		v = Com_Clamp( 0.0f, 1.0f, v );
	} else {
		v = Com_Clamp( -1.0f, 1.0f, v );
	}
	pad->axis[axis] = v;
	if ( ( axis >= PAX_LT ) ? ( v >= joy_triggerThreshold->value ) : ( fabsf( v ) > joy_deadzone->value ) ) {
		pad->lastInput = Sys_Milliseconds() | 1;	// a real push, not an idle (phantom) pad
	}

	// the analog values also act as digital pad keys
	st = joy_stickThreshold->value;
	tt = joy_triggerThreshold->value;
	switch ( axis ) {
	case PAX_LX: Pad_AxisKey( pad, PAD_LSTICK_LEFT, -v, st ); Pad_AxisKey( pad, PAD_LSTICK_RIGHT, v, st ); break;
	case PAX_LY: Pad_AxisKey( pad, PAD_LSTICK_UP, -v, st ); Pad_AxisKey( pad, PAD_LSTICK_DOWN, v, st ); break;
	case PAX_RX: Pad_AxisKey( pad, PAD_RSTICK_LEFT, -v, st ); Pad_AxisKey( pad, PAD_RSTICK_RIGHT, v, st ); break;
	case PAX_RY: Pad_AxisKey( pad, PAD_RSTICK_UP, -v, st ); Pad_AxisKey( pad, PAD_RSTICK_DOWN, v, st ); break;
	case PAX_LT: Pad_AxisKey( pad, PAD_LT, v, tt ); break;
	case PAX_RT: Pad_AxisKey( pad, PAD_RT, v, tt ); break;
	}
}


static void Pad_ReleaseAll( padDevice_t *pad ) {
	int i;
	for ( i = 0; i < PAX_COUNT; i++ ) {
		Pad_SetAxis( pad, i, 0.0f );
	}
	for ( i = 0; i < PAD_KEY_COUNT; i++ ) {
		Pad_SetButton( pad, i, qfalse );
	}
}


static void Pad_Assign( padDevice_t *pad, int player );

/*
==================
Pad_Attach

A device appeared (SDL hot-plug or padinject).  A pad whose player lost it
earlier in this session (same GUID) takes that player back at once.
==================
*/
static padDevice_t *Pad_Attach( int slot, qboolean isVirtual, const char *guid, const char *key, const char *name, padType_t type ) {
	padDevice_t *pad;
	int i;

	if ( slot < 0 ) {
		for ( slot = 0; slot < MAX_PADS && pads[slot].connected; slot++ )
			;
	}
	if ( slot >= MAX_PADS || pads[slot].connected ) {
		Com_Printf( "gamepad: no free pad slot for \"%s\" (max %i)\n", name, MAX_PADS );
		return NULL;
	}
	// Independent mode: a player's window opens its own pad only (SDL pads are filtered in SDLG_Detect)
	if ( isVirtual && !CL_IndepChildPadMatch( key ) ) {
		Com_Printf( "gamepad: virtual pad %s is not this window's pad (Independent mode): ignored\n", guid );
		return NULL;
	}

	pad = &pads[slot];
	Com_Memset( pad, 0, sizeof( *pad ) );
	pad->connected = qtrue;
	pad->isVirtual = isVirtual;
	pad->player = -1;
	pad->type = type;
	pad->instanceId = -1;
	Q_strncpyz( pad->guid, guid, sizeof( pad->guid ) );
	Q_strncpyz( pad->key, key, sizeof( pad->key ) );
	pad->attachTime = Sys_Milliseconds() | 1;
	Q_strncpyz( pad->name, name, sizeof( pad->name ) );

	Com_Printf( "pad %i: connected %s\"%s\" (%s, guid %s, key %s)\n", slot, isVirtual ? "virtual " : "",
		pad->name, padTypeNames[type], pad->guid, pad->key );

	// auto-heal: the same pad coming back takes its player again
	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( padSlots[i].lostPad && !Q_stricmp( padSlots[i].lostGuid, pad->guid )
			&& CL_SplitSlotActive( i ) && !Pad_ForPlayer( i ) ) {
			Com_Printf( "pad %i: reconnected -- P%i has control again\n", slot, i + 1 );
			Pad_Assign( pad, i );
			break;
		}
	}
	// Independent mode: the window's own pad is its player 1 at once
	if ( CL_IndepChild() && pad->player < 0 && !Pad_ForPlayer( 0 ) ) {
		Com_Printf( "pad %i: this window's pad: P%i\n", slot, CL_IndepChildPlayer() );
		Pad_Assign( pad, 0 );
	}
	if ( CL_IndepChild() && pad->player == 0 ) {
		Com_Printf( "indep: pad %s -> P%i (pid %i, this window)\n", pad->key, CL_IndepChildPlayer(), Sys_SplitPid() );
	}
	return pad;
}


// R19, Independent mode, a player window: its pad keys changed -- devices it left closed are looked at again
static void Pad_Detach( padDevice_t *pad );

void IN_PadRescan( void ) {
	padDevice_t *own = Pad_ForPlayer( 0 );

	// R19b: a device that is no longer in its key list (a listing taken back) is closed
	if ( CL_IndepChild() && own && own->key[0] && !CL_IndepChildPadMatch( own->key ) ) {
		Com_Printf( "indep: pad %s is no longer this window's: closed\n", own->key );
		Pad_Detach( own );
	}
	sdlNumIgnored = 0;
	busNumIgnored = 0;
}


// Independent mode, a child's test pad (cl_splitChildPad virtual-...)
void IN_PadAttachVirtual( const char *guid ) {
	Pad_Attach( -1, qtrue, guid, guid, va( "Virtual %s pad", padTypeNames[PADTYPE_XBOX] ), PADTYPE_XBOX );
}


/*
==================
Pad_Detach

A device went away.  Its player keeps the slot and idles: every button the
pad held is released (no stuck movement) until the pad returns or another
pad claims the player.
==================
*/
static void Pad_Detach( padDevice_t *pad ) {
	const int player = pad->player;
	int i;

	Pad_ReleaseAll( pad );

	if ( player >= 0 && CL_IndepSlotLive( player ) && !CL_IndepPicking( player ) ) {
		// Independent mode, the coordinator: a pad playing in another window -- that window
		// handles its own pad loss; here the slot just stays reserved (never a heal target)
		Com_Printf( "pad %i: disconnected -- P%i's window handles it (slot kept)\n", Pad_Index( pad ), player + 1 );
		Com_Printf( "indep: pad %s (P%i's) went away; it goes back to P%i when it returns\n", pad->key, player + 1, player + 1 );
		padSlots[player].indepLostAt = Sys_Milliseconds() | 1;
	} else if ( player >= 0 ) {
		Pad_ReleaseCrouch( player, -1 );
		padSlots[player].lostPad = qtrue;
		Q_strncpyz( padSlots[player].lostGuid, pad->guid, sizeof( padSlots[player].lostGuid ) );
		CL_SplitReleaseInput( player );
		Com_Printf( "pad %i: disconnected -- P%i idles until it reconnects or another pad presses a button\n",
			Pad_Index( pad ), player + 1 );
		// nobody can drive its overlay any more: close it
		CL_SplitMenuClose( player );
	} else {
		Com_Printf( "pad %i: disconnected\n", Pad_Index( pad ) );
	}

	if ( pad->ctrl && sdl.active ) {
		sdl.GameControllerClose( pad->ctrl );
	}
	// R19: devices that duplicated this one are pads of their own again
	for ( i = 0; i < MAX_PADS; i++ ) {
		if ( pads[i].connected && pads[i].mirrorOf == Pad_Index( pad ) + 1 ) {
			Com_Printf( "indep: pad %i (key %s): the pad it duplicated (pad %i) went away -- a pad of its own again\n",
				i, pads[i].key, Pad_Index( pad ) );
			pads[i].mirrorOf = 0;
			pads[i].mirrorDiff = 0;
			pads[i].mirrorAliased = qfalse;	// (an aliased key stays its window's: the same pad, now one listing)
			pads[i].joinDone = qtrue;
		}
	}
	Com_Memset( pad, 0, sizeof( *pad ) );
	pad->player = -1;
}


static padType_t SDLG_Type( int index ) {
	unsigned short vendor;

	if ( sdl.GameControllerTypeForIndex ) {
		switch ( sdl.GameControllerTypeForIndex( index ) ) {
		case 1: case 2: return PADTYPE_XBOX;					// 360, One
		case 3: case 4: case 7: return PADTYPE_PLAYSTATION;	// PS3, PS4, PS5
		case 5: case 11: case 12: case 13: return PADTYPE_NINTENDO;	// Switch Pro, Joy-Cons
		default: break;
		}
	}
	vendor = sdl.JoystickGetDeviceVendor( index );
	if ( vendor == 0x045e ) return PADTYPE_XBOX;
	if ( vendor == 0x054c ) return PADTYPE_PLAYSTATION;
	if ( vendor == 0x057e ) return PADTYPE_NINTENDO;
	return PADTYPE_OTHER;
}


static qboolean SDLG_Known( int instanceId ) {
	int i;
	for ( i = 0; i < MAX_PADS; i++ ) {
		if ( pads[i].connected && pads[i].ctrl && pads[i].instanceId == instanceId ) {
			return qtrue;
		}
	}
	for ( i = 0; i < sdlNumIgnored; i++ ) {
		if ( sdlIgnored[i] == instanceId ) {
			return qtrue;
		}
	}
	return qfalse;
}


// which of several devices with this GUID SDL device 'index' is (0 = the first)
static int SDLG_Ordinal( int index, const char *guid ) {
	char g[40];
	int i, n = 0;

	for ( i = 0; i < index; i++ ) {
		sdl.JoystickGetGUIDString( sdl.JoystickGetDeviceGUID( i ), g, sizeof( g ) );
		if ( !Q_stricmp( g, guid ) ) {
			n++;
		}
	}
	return n;
}


static int Bus_Ordinal( int busKey, int busGen );
static int Pad_KeyFromName( const char *name );

/*
==================
Pad_MakeKey

R19: a device's key, the same in every process of the session, so the
coordinator can name the one device a player window opens:
"<guid>@<hash of the device path>" (on Windows the raw input / HID device
interface path, unique per physical device and stable across processes and
replugs); a device without a path gets "<guid>#<n>" (which of several
devices with that GUID, in this process's order -- that order can differ
between processes, so this is only the fallback, and logged as such).
==================
*/
static void Pad_MakeKey( char *out, int size, const char *guid, const char *path, int ordinal ) {
	unsigned h = 2166136261u;
	const char *p;

	if ( path && path[0] && strcmp( path, "-" ) ) {
		for ( p = path; *p; p++ ) {
			h = ( h ^ (unsigned)tolower( *(const unsigned char *)p ) ) * 16777619u;	// FNV-1a, case-insensitive
		}
		Com_sprintf( out, size, "%s@%08x", guid, h );
	} else {
		Com_sprintf( out, size, "%s#%i", guid, ordinal );
	}
}


// the key of every pad: Independent mode hands it to the window that plays with it
static const char *Pad_DeviceId( const padDevice_t *pad ) {
	return pad->key;
}


const char *IN_PadDeviceId( int n ) {
	const padDevice_t *pad = ( (unsigned)n < MAX_SPLITVIEW ) ? Pad_ForPlayer( n ) : NULL;
	return pad ? Pad_DeviceId( pad ) : NULL;
}


/*
==================
Pad_MayOpen

R19, Independent mode: may this process open a new device?  A player
window opens exactly one: a device whose key the coordinator gave it, and
no second device with such a key (one physical pad listed twice).  The
coordinator does not open a second device with the key of one already
open (the same device path listed twice); R19b: Together mode applies
that same-path rule too ("pad: skipped duplicate listing of <key>").
==================
*/
static qboolean Pad_MayOpen( const char *what, const char *key ) {
	const padDevice_t *own;
	int i;

	if ( CL_IndepChild() ) {
		if ( !CL_IndepChildPadMatch( key ) ) {
			Com_Printf( "gamepad: %s (key %s) belongs to another window: not opened\n", what, key );
			return qfalse;
		}
		own = Pad_ForPlayer( 0 );
		if ( own ) {
			Com_Printf( "indep: %s (key %s) is another device of this window's pad (pad %i, key %s): not opened (duplicate)\n",
				what, key, Pad_Index( own ), own->key );
			return qfalse;
		}
		return qtrue;
	}
	// the same device path listed twice: one device (R19b: in Together mode too)
	if ( strchr( key, '@' ) ) {	// (an order key "#n" names a different device after a replug)
		for ( i = 0; i < MAX_PADS; i++ ) {
			if ( pads[i].connected && !Q_stricmp( pads[i].key, key ) ) {
				if ( CL_IndepActive() ) {
					Com_Printf( "indep: %s (key %s) is the same device as pad %i (same path): not opened (duplicate)\n", what, key, i );
				} else {
					Com_Printf( "pad: skipped duplicate listing of %s (%s, the same device path as pad %i)\n", key, what, i );
				}
				return qfalse;
			}
		}
	}
	return qtrue;
}


static void SDLG_Ignore( int instanceId ) {
	if ( sdlNumIgnored < MAX_IGNORED ) {
		sdlIgnored[ sdlNumIgnored++ ] = instanceId;
	}
}


/*
==================
SDLG_Detect

Open every new SDL device that has a game controller mapping; log the
others once (virtual HID devices, wheels, VR controllers...) and leave
them closed.
==================
*/
static void SDLG_Detect( void ) {
	char guid[40], key[64];
	const char *path;
	const char *name;
	padDevice_t *pad;
	void *ctrl;
	int i, j, n, id;

	n = sdl.NumJoysticks();

	// forget ignored devices that went away (the table stays small)
	for ( j = 0; j < sdlNumIgnored; ) {
		for ( i = 0; i < n && sdl.JoystickGetDeviceInstanceID( i ) != sdlIgnored[j]; i++ )
			;
		if ( i == n ) {
			sdlIgnored[j] = sdlIgnored[ --sdlNumIgnored ];
		} else {
			j++;
		}
	}

	for ( i = 0; i < n; i++ ) {
		id = sdl.JoystickGetDeviceInstanceID( i );
		if ( id < 0 || SDLG_Known( id ) ) {
			continue;
		}
		if ( sdlNumIgnored >= MAX_IGNORED && !sdl.IsGameController( i ) ) {
			continue;	// too many unusable devices to remember: skip quietly
		}
		sdl.JoystickGetGUIDString( sdl.JoystickGetDeviceGUID( i ), guid, sizeof( guid ) );
		name = sdl.JoystickNameForIndex( i );
		if ( !name ) {
			name = "?";
		}
		// R19: the device's key (its path where SDL has one); Independent mode: a player's window
		// opens only its own pad, the coordinator never the same device twice
		path = sdl.JoystickPathForIndex ? sdl.JoystickPathForIndex( i ) : NULL;
		Pad_MakeKey( key, sizeof( key ), guid, path, SDLG_Ordinal( i, guid ) );
		if ( !Pad_MayOpen( va( "SDL device %i \"%s\"", i, name ), key ) ) {
			SDLG_Ignore( id );
			continue;
		}
		if ( !sdl.IsGameController( i ) ) {
			Com_Printf( "gamepad: SDL device %i \"%s\" (guid %s, vid %04x pid %04x) ignored: no game controller mapping\n",
				i, name, guid, sdl.JoystickGetDeviceVendor( i ), sdl.JoystickGetDeviceProduct( i ) );
			SDLG_Ignore( id );
			continue;
		}
		ctrl = sdl.GameControllerOpen( i );
		if ( !ctrl ) {
			Com_Printf( "gamepad: SDL device %i \"%s\" could not be opened: %s\n", i, name, sdl.GetError() );
			SDLG_Ignore( id );
			continue;
		}
		Com_Printf( "gamepad: SDL device %i \"%s\" (guid %s, vid %04x pid %04x) is a game controller\n",
			i, name, guid, sdl.JoystickGetDeviceVendor( i ), sdl.JoystickGetDeviceProduct( i ) );
		Com_Printf( "gamepad: SDL device %i key %s%s\n", i, key, path && path[0] ? va( " (path %s)", path ) : " (no device path: by order, fragile across processes)" );
		pad = Pad_Attach( -1, qfalse, guid, key, sdl.GameControllerName( ctrl ) ? sdl.GameControllerName( ctrl ) : name, SDLG_Type( i ) );
		if ( !pad ) {
			sdl.GameControllerClose( ctrl );
			SDLG_Ignore( id );
			continue;
		}
		pad->ctrl = ctrl;
		pad->instanceId = id;
	}
}


static void SDLG_Poll( void ) {
	padDevice_t *pad;
	int i, b;

	sdl.GameControllerUpdate();

	for ( i = 0; i < MAX_PADS; i++ ) {
		pad = &pads[i];
		if ( !pad->connected || !pad->ctrl ) {
			continue;
		}
		if ( !sdl.GameControllerGetAttached( pad->ctrl ) ) {
			Pad_Detach( pad );
			continue;
		}
		for ( b = 0; b < PAD_SDL_BUTTONS; b++ ) {
			Pad_SetButton( pad, b, sdl.GameControllerGetButton( pad->ctrl, b ) ? qtrue : qfalse );
		}
		for ( b = 0; b < PAX_COUNT; b++ ) {
			Pad_SetAxis( pad, b, sdl.GameControllerGetAxis( pad->ctrl, b ) / 32767.0f );
		}
	}

	SDLG_Detect();

	// joystick + gamecontroller events only (0x600..0x6FF): on SDL builds of the
	// engine the queue also carries its keyboard, mouse and window events
	sdl.FlushEvents( 0x600, 0x6FF );
}


/*
=============================================================================

TEST DEVICE BUS (R19, developer / sv_cheats)

A stand-in for the operating system's device list, shared by every process
of a session (coordinator and Independent-mode windows): <fs_homepath>/
padbus.txt, one line per device in enumeration order --
  dev <id> <gen> <guid> <path|-> <coordinator-only 0|1> <mirror id|-1> <buttons hex> <lx ly rx ry lt rt>
-- and an "end" line.  'padbus' (one process writes, normally the
coordinator) connects, disconnects and presses; every process with
in_padBus 1 reads the file each frame and opens its devices like SDL ones
(the same attach / detach / filter code), so one bus device can be open in
several processes at once, exactly like a real pad.  in_padBusOrder 1 lists
the devices in reverse order in this process (SDL's order differs between
processes); a reconnect goes to the end of the list (as SDL appends);
"coordinator-only" devices are invisible to player windows (a duplicate
only one process sees); a "mirror" device copies another's input (one
physical pad seen as two devices).

=============================================================================
*/

#define MAX_BUS_DEVS	8
#define BUS_FILE		"padbus.txt"

typedef struct {
	int			id;
	int			gen;
	char		guid[40];
	char		path[128];		// "-" = none
	int			coordOnly;
	int			mirror;			// id whose input this device copies, -1 = none
	unsigned	buttons;		// PAD_SDL_BUTTONS bits
	float		axis[PAX_COUNT];
} busDev_t;

static busDev_t	busDevs[MAX_BUS_DEVS];		// file order (last complete read)
static int		busNum;
static cvar_t	*in_padBus;
static cvar_t	*in_padBusOrder;


static const char *Bus_OSPath( const char *ext ) {
	return va( "%s%c" BUS_FILE "%s", Cvar_VariableString( "fs_homepath" ), PATH_SEP, ext );
}


static int Bus_Instance( const busDev_t *d ) {
	return 100000 + d->id * 1000 + ( d->gen % 1000 );
}


static qboolean Bus_Visible( const busDev_t *d ) {
	return ( !d->coordOnly || !CL_IndepChild() ) ? qtrue : qfalse;
}


// the file -> list (file order); qfalse = missing or caught half-written (keep the last one)
static qboolean Bus_Load( busDev_t *out, int *num ) {
	static char buf[8192];
	char *p, *e;
	FILE *f;
	int len, n = 0;
	qboolean end = qfalse;
	busDev_t *d;

	f = Sys_FOpen( Bus_OSPath( "" ), "rb" );
	if ( !f ) {
		return qfalse;
	}
	len = (int)fread( buf, 1, sizeof( buf ) - 1, f );
	fclose( f );
	if ( len <= 0 ) {
		return qfalse;
	}
	buf[len] = '\0';
	for ( p = buf; *p; p = e ) {
		e = strchr( p, '\n' );
		if ( e ) {
			*e++ = '\0';
		} else {
			e = p + strlen( p );
		}
		if ( !strncmp( p, "end", 3 ) ) {
			end = qtrue;
			break;
		}
		if ( n < MAX_BUS_DEVS ) {
			d = &out[n];
			Com_Memset( d, 0, sizeof( *d ) );
			if ( sscanf( p, "dev %i %i %39s %127s %i %i %x %f %f %f %f %f %f", &d->id, &d->gen, d->guid, d->path,
					&d->coordOnly, &d->mirror, &d->buttons, &d->axis[0], &d->axis[1], &d->axis[2], &d->axis[3],
					&d->axis[4], &d->axis[5] ) == 13 ) {
				n++;
			}
		}
	}
	if ( !end ) {
		return qfalse;
	}
	*num = n;
	return qtrue;
}


static void Bus_Save( const busDev_t *list, int num ) {
	char path[MAX_OSPATH], tmp[MAX_OSPATH];
	FILE *f;
	int i, tries;
	const busDev_t *d;

	Q_strncpyz( path, Bus_OSPath( "" ), sizeof( path ) );
	Q_strncpyz( tmp, Bus_OSPath( ".tmp" ), sizeof( tmp ) );
	f = Sys_FOpen( tmp, "wb" );
	if ( !f ) {
		Com_Printf( "padbus: cannot write %s\n", tmp );
		return;
	}
	for ( i = 0; i < num; i++ ) {
		d = &list[i];
		fprintf( f, "dev %i %i %s %s %i %i %x %.3f %.3f %.3f %.3f %.3f %.3f\n", d->id, d->gen, d->guid, d->path,
			d->coordOnly, d->mirror, d->buttons, d->axis[0], d->axis[1], d->axis[2], d->axis[3], d->axis[4], d->axis[5] );
	}
	fprintf( f, "end\n" );
	fclose( f );
	// replace in one step (a reader never sees half a file); a reader holding it open: retry
	for ( tries = 0; tries < 50; tries++ ) {
#ifdef _WIN32
		if ( MoveFileExA( tmp, path, MOVEFILE_REPLACE_EXISTING ) ) {
			return;
		}
#else
		if ( rename( tmp, path ) == 0 ) {
			return;
		}
#endif
		Sys_Sleep( 2 );
	}
	Com_Printf( "padbus: could not replace %s\n", path );
}


// position k of this process's enumeration -> index into busDevs
static int Bus_OrderIndex( int k ) {
	return ( in_padBusOrder && in_padBusOrder->integer ) ? busNum - 1 - k : k;
}


// which of several visible bus devices with its GUID this one is, in this process's order
static int Bus_Ordinal( int busKey, int busGen ) {
	const busDev_t *self = NULL, *d;
	int k, n = 0;

	for ( k = 0; k < busNum; k++ ) {
		d = &busDevs[ Bus_OrderIndex( k ) ];
		if ( d->id + 1 == busKey && d->gen == busGen ) {
			self = d;
			break;
		}
	}
	if ( !self ) {
		return 0;
	}
	for ( k = 0; k < busNum; k++ ) {
		d = &busDevs[ Bus_OrderIndex( k ) ];
		if ( d == self ) {
			break;
		}
		if ( Bus_Visible( d ) && !Q_stricmp( d->guid, self->guid ) ) {
			n++;
		}
	}
	return n;
}


static const busDev_t *Bus_Find( int busKey, int busGen ) {
	int i;
	for ( i = 0; i < busNum; i++ ) {
		if ( busDevs[i].id + 1 == busKey && busDevs[i].gen == busGen && Bus_Visible( &busDevs[i] ) ) {
			return &busDevs[i];
		}
	}
	return NULL;
}


static qboolean Bus_Ignored( int inst, qboolean add ) {
	int i;
	for ( i = 0; i < busNumIgnored; i++ ) {
		if ( busIgnored[i] == inst ) {
			return qtrue;
		}
	}
	if ( add && busNumIgnored < MAX_IGNORED ) {
		busIgnored[ busNumIgnored++ ] = inst;
	}
	return qfalse;
}


static void Bus_Frame( void ) {
	const busDev_t *d;
	padDevice_t *pad;
	int i, k, b, inst, ord;
	char name[64], key[64];

	if ( !in_padBus->integer ) {
		for ( i = 0; i < MAX_PADS; i++ ) {
			if ( pads[i].connected && pads[i].busKey ) {
				Pad_Detach( &pads[i] );
			}
		}
		return;
	}
	Bus_Load( busDevs, &busNum );

	// gone (disconnected, or reconnected: a new device)
	for ( i = 0; i < MAX_PADS; i++ ) {
		pad = &pads[i];
		if ( pad->connected && pad->busKey && !Bus_Find( pad->busKey, pad->busGen ) ) {
			Pad_Detach( pad );
		}
	}
	// input
	for ( i = 0; i < MAX_PADS; i++ ) {
		pad = &pads[i];
		if ( !pad->connected || !pad->busKey ) {
			continue;
		}
		d = Bus_Find( pad->busKey, pad->busGen );
		if ( !d ) {
			continue;
		}
		for ( b = 0; b < PAD_SDL_BUTTONS; b++ ) {
			Pad_SetButton( pad, b, ( d->buttons >> b ) & 1 ? qtrue : qfalse );
		}
		for ( b = 0; b < PAX_COUNT; b++ ) {
			Pad_SetAxis( pad, b, d->axis[b] );
		}
	}
	// new devices, in this process's order
	for ( i = 0; i < busNumIgnored; ) {
		for ( k = 0; k < busNum && Bus_Instance( &busDevs[k] ) != busIgnored[i]; k++ )
			;
		if ( k == busNum ) {
			busIgnored[i] = busIgnored[ --busNumIgnored ];
		} else {
			i++;
		}
	}
	for ( k = 0; k < busNum; k++ ) {
		d = &busDevs[ Bus_OrderIndex( k ) ];
		if ( !Bus_Visible( d ) ) {
			continue;
		}
		inst = Bus_Instance( d );
		for ( i = 0; i < MAX_PADS && !( pads[i].connected && pads[i].busKey == d->id + 1 && pads[i].busGen == d->gen ); i++ )
			;
		if ( i < MAX_PADS || Bus_Ignored( inst, qfalse ) ) {
			continue;
		}
		ord = Bus_Ordinal( d->id + 1, d->gen );
		Pad_MakeKey( key, sizeof( key ), d->guid, d->path, ord );
		if ( !Pad_MayOpen( va( "bus device %i (path %s)", d->id, d->path ), key ) ) {
			Bus_Ignored( inst, qtrue );
			continue;
		}
		Com_Printf( "gamepad: bus device %i (guid %s #%i, path %s) is a game controller, key %s\n", d->id, d->guid, ord, d->path, key );
		Com_sprintf( name, sizeof( name ), "Bus pad %i", d->id );
		pad = Pad_Attach( -1, qfalse, d->guid, key, name, PADTYPE_XBOX );
		if ( !pad ) {
			Bus_Ignored( inst, qtrue );
			continue;
		}
		pad->busKey = d->id + 1;
		pad->busGen = d->gen;
		pad->instanceId = inst;
	}
}


/*
==================
Pad_Bus_f

padbus clear | list
padbus <id> connect <guid> [path|-] [coord] [mirror <id>]
padbus <id> disconnect
padbus <id> button <padkey> <0|1>
padbus <id> axis <lx|ly|rx|ry|lt|rt> <-1..1>
==================
*/
static void Pad_Bus_f( void ) {
	static int gen;
	busDev_t list[MAX_BUS_DEVS];
	const char *what;
	int num = 0, i, j, k, id;
	qboolean changed = qfalse;

	if ( !Cvar_VariableIntegerValue( "sv_cheats" ) && !( com_developer && com_developer->integer ) ) {
		Com_Printf( "padbus: needs sv_cheats or developer\n" );
		return;
	}
	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: padbus clear | list | <id> connect <guid> [path|-] [coord] [mirror <id>] | <id> disconnect | "
			"<id> button <padkey> <0|1> | <id> axis <lx|ly|rx|ry|lt|rt> <-1..1>\n" );
		return;
	}
	if ( !Bus_Load( list, &num ) ) {
		num = 0;
	}
	if ( !Q_stricmp( Cmd_Argv( 1 ), "clear" ) ) {
		Bus_Save( list, 0 );
		return;
	}
	if ( !Q_stricmp( Cmd_Argv( 1 ), "list" ) ) {
		for ( i = 0; i < num; i++ ) {
			Com_Printf( "bus %i: dev %i gen %i guid %s path %s%s mirror %i buttons %x\n", i, list[i].id, list[i].gen,
				list[i].guid, list[i].path, list[i].coordOnly ? " coordinator-only" : "", list[i].mirror, list[i].buttons );
		}
		return;
	}
	id = atoi( Cmd_Argv( 1 ) );
	what = Cmd_Argv( 2 );
	for ( i = 0; i < num && list[i].id != id; i++ )
		;

	if ( !Q_stricmp( what, "connect" ) ) {
		if ( i < num || num >= MAX_BUS_DEVS || Cmd_Argc() < 4 ) {
			Com_Printf( "padbus: device %i is already there, the bus is full, or no guid\n", id );
			return;
		}
		Com_Memset( &list[num], 0, sizeof( list[num] ) );
		list[num].id = id;
		list[num].gen = ( ( Sys_Milliseconds() / 16 ) % 900 ) * 1000 + ( ++gen % 1000 );
		list[num].mirror = -1;
		Q_strncpyz( list[num].guid, Cmd_Argv( 3 ), sizeof( list[num].guid ) );
		Q_strncpyz( list[num].path, Cmd_Argc() > 4 && Cmd_Argv( 4 )[0] ? Cmd_Argv( 4 ) : "-", sizeof( list[num].path ) );
		for ( k = 5; k < Cmd_Argc(); k++ ) {
			if ( !Q_stricmp( Cmd_Argv( k ), "coord" ) ) {
				list[num].coordOnly = 1;
			} else if ( !Q_stricmp( Cmd_Argv( k ), "mirror" ) && k + 1 < Cmd_Argc() ) {
				list[num].mirror = atoi( Cmd_Argv( ++k ) );
			}
		}
		num++;
		changed = qtrue;
	} else if ( i == num ) {
		Com_Printf( "padbus: no device %i\n", id );
		return;
	} else if ( !Q_stricmp( what, "disconnect" ) ) {
		for ( j = i; j < num - 1; j++ ) {
			list[j] = list[j + 1];
		}
		num--;
		changed = qtrue;
	} else if ( !Q_stricmp( what, "button" ) || !Q_stricmp( what, "axis" ) ) {
		qboolean isAxis = !Q_stricmp( what, "axis" ) ? qtrue : qfalse;
		if ( isAxis ) {
			for ( k = 0; k < PAX_COUNT && Q_stricmp( Cmd_Argv( 3 ), padAxisNames[k] ); k++ )
				;
			if ( k == PAX_COUNT ) {
				Com_Printf( "padbus: axis must be lx ly rx ry lt rt\n" );
				return;
			}
		} else {
			k = Pad_KeyFromName( Cmd_Argv( 3 ) );
			if ( k < 0 || k >= PAD_SDL_BUTTONS ) {
				Com_Printf( "padbus: \"%s\" isn't a pad button\n", Cmd_Argv( 3 ) );
				return;
			}
		}
		// the device and every device mirroring it
		for ( j = 0; j < num; j++ ) {
			if ( j != i && list[j].mirror != id ) {
				continue;
			}
			if ( isAxis ) {
				list[j].axis[k] = atof( Cmd_Argv( 4 ) );
			} else if ( atoi( Cmd_Argv( 4 ) ) ) {
				list[j].buttons |= 1u << k;
			} else {
				list[j].buttons &= ~( 1u << k );
			}
		}
		changed = qtrue;
	} else {
		Com_Printf( "padbus: unknown '%s'\n", what );
	}
	if ( changed ) {
		Bus_Save( list, num );
	}
}


/*
=============================================================================

PER-PLAYER PAD BINDINGS

=============================================================================
*/

static int Pad_KeyFromName( const char *name ) {
	int i;
	char buf[32];

	if ( !name || !name[0] ) {
		return -1;
	}
	for ( i = 0; i < PAD_KEY_COUNT; i++ ) {
		if ( !Q_stricmp( name, padKeyNames[i] ) ) {
			return i;
		}
		// also accept the name without the PAD_ prefix ("A", "START")
		Q_strncpyz( buf, padKeyNames[i] + 4, sizeof( buf ) );
		if ( !Q_stricmp( name, buf ) ) {
			return i;
		}
	}
	return -1;
}


static void Pad_SetBind( int player, int key, const char *cmd ) {
	padSlot_t *s = &padSlots[player];

	if ( s->binds[key] ) {
		Z_Free( s->binds[key] );
		s->binds[key] = NULL;
	}
	if ( cmd && cmd[0] ) {
		s->binds[key] = CopyString( cmd );
	}
}


static void Pad_ClearBinds( int player ) {
	int k;
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		Pad_SetBind( player, k, NULL );
	}
}


// R18: player n's (or the Guest defaults') hold-or-toggle setting t
static cvar_t *Pad_ToggleCvar( int player, int t ) {
	return IN_PadFeelCvar( player, (*padToggleCvars[t])->name );
}


static void Pad_SetToggle( int player, int t, qboolean on ) {
	cvar_t *v = Pad_ToggleCvar( player, t );
	if ( v ) {
		Cvar_Set( v->name, on ? "1" : "0" );
	}
}


static int Pad_ToggleFromWord( const char *word ) {
	int t;
	for ( t = 0; t < PADTOG_COUNT; t++ ) {
		if ( !Q_stricmp( word, padToggleWords[t] ) ) {
			return t;
		}
	}
	return -1;
}


// "bind <padkey> <command>" and "toggle <crouch|sprint> <0|1>" lines (default_pad.cfg format)
static void Pad_ParseBindText( int player, const char *text, const char *source ) {
	const char *p = text, *tok;
	char keyName[64];
	int key, t;

	while ( 1 ) {
		tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		if ( !Q_stricmp( tok, "toggle" ) ) {
			t = Pad_ToggleFromWord( COM_ParseExt( &p, qfalse ) );
			tok = COM_ParseExt( &p, qfalse );
			if ( t >= 0 ) {
				Pad_SetToggle( player, t, atoi( tok ) ? qtrue : qfalse );
			}
			continue;
		}
		if ( Q_stricmp( tok, "bind" ) ) {
			SkipRestOfLine( &p );
			continue;
		}
		Q_strncpyz( keyName, COM_ParseExt( &p, qfalse ), sizeof( keyName ) );
		key = Pad_KeyFromName( keyName );
		tok = COM_ParseExt( &p, qfalse );
		if ( key < 0 ) {
			Com_Printf( "%s: unknown pad key '%s'\n", source, keyName );
			continue;
		}
		Pad_SetBind( player, key, tok );
	}
}


// R18: the factory data's toggle defaults for player n (its binds untouched)
static void Pad_FactoryToggles( int player ) {
	const char *text, *source, *p, *tok;
	void *buf;
	int t;

	for ( t = 0; t < PADTOG_COUNT; t++ ) {
		Pad_SetToggle( player, t, qfalse );
	}
	p = text = Pad_FactoryText( &buf, &source );
	while ( 1 ) {
		tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		if ( Q_stricmp( tok, "toggle" ) ) {
			SkipRestOfLine( &p );
			continue;
		}
		t = Pad_ToggleFromWord( COM_ParseExt( &p, qfalse ) );
		tok = COM_ParseExt( &p, qfalse );
		if ( t >= 0 ) {
			Pad_SetToggle( player, t, atoi( tok ) ? qtrue : qfalse );
		}
	}
	if ( buf ) {
		FS_FreeFile( buf );
	}
}


/*
==================
Pad_FactoryBinds / Pad_EnsureBinds

The factory set is <game>/default_pad.cfg through the VFS, else the
engine's baseq3 set.  The Guest defaults (padSlots[PAD_DEFAULTS], design
14.3) start from it unless the profile layer loaded the host's own set; a
player slot without pad bindings gets a copy of the Guest defaults.
==================
*/
static void Pad_FactoryBinds( int player ) {
	padSlot_t *s = &padSlots[player];
	const char *text, *source;
	void *buf;
	int t;

	s->initialized = qtrue;
	s->custom = qfalse;
	Pad_ClearBinds( player );
	for ( t = 0; t < PADTOG_COUNT; t++ ) {
		Pad_SetToggle( player, t, qfalse );
	}

	text = Pad_FactoryText( &buf, &source );
	Pad_ParseBindText( player, text, source );
	Com_DPrintf( "pad slot %i: %s%s\n", player + 1, buf ? "pad binds from " : "", source );
	if ( buf ) {
		FS_FreeFile( buf );
	}
}


static void Pad_CopyBindTable( int dst, int src ) {
	int k;

	padSlots[dst].initialized = qtrue;
	padSlots[dst].custom = padSlots[src].custom;
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		Pad_SetBind( dst, k, padSlots[src].binds[k] );
	}
	for ( k = 0; k < PADTOG_COUNT; k++ ) {
		Pad_SetToggle( dst, k, Pad_ToggleCvar( src, k ) && Pad_ToggleCvar( src, k )->integer );
	}
}


static void Pad_EnsureBinds( int player ) {
	if ( padSlots[player].initialized ) {
		return;
	}
	if ( player == PAD_DEFAULTS ) {
		Pad_FactoryBinds( player );
		return;
	}
	Pad_EnsureBinds( PAD_DEFAULTS );
	Pad_CopyBindTable( player, PAD_DEFAULTS );
}


// a bind table or feel setting changed: the profile layer saves it
static void Pad_Changed( int player ) {
	CL_ProfileChanged( player );
}


/*
==================
IN_PadFeelCvar

Player n's shadow of a look/aim/toggle setting ('p<N>_joy_yawSpeed'), or
the Guest defaults' ('guest_joy_yawSpeed', n = SPLIT_GUEST_DEFAULTS),
created from the shared value; NULL if 'name' is not one of them.
==================
*/
static cvar_t *Pad_Shadow( int n, const cvar_t *base ) {
	if ( n == SPLIT_GUEST_DEFAULTS ) {
		return Cvar_Get( va( "guest_%s", base->name ), base->string, 0 );
	}
	return Cvar_Get( va( "p%i_%s", n + 1, base->name ), base->string, 0 );
}


// R18: the crouch / sprint hold-or-toggle settings (per game, with the binds)
static qboolean Pad_IsToggleName( const char *name ) {
	int t;
	for ( t = 0; t < PADTOG_COUNT; t++ ) {
		if ( *padToggleCvars[t] && !Q_stricmp( name, (*padToggleCvars[t])->name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


cvar_t *IN_PadFeelCvar( int n, const char *name ) {
	int k;

	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS || !name ) {
		return NULL;
	}
	for ( k = 0; k < (int)ARRAY_LEN( padFeelCvars ); k++ ) {
		if ( *padFeelCvars[k] && !Q_stricmp( name, (*padFeelCvars[k])->name ) ) {
			return Pad_Shadow( n, *padFeelCvars[k] );
		}
	}
	for ( k = 0; k < PADTOG_COUNT; k++ ) {
		if ( *padToggleCvars[k] && !Q_stricmp( name, (*padToggleCvars[k])->name ) ) {
			return Pad_Shadow( n, *padToggleCvars[k] );
		}
	}
	return NULL;
}


// a look/aim/toggle setting of player n (or the Guest defaults) changed
void IN_PadFeelChanged( int n ) {
	if ( (unsigned)n <= SPLIT_GUEST_DEFAULTS ) {
		Pad_Changed( n );
	}
}


/*
==================
IN_PadResetFeel

Player n's look/aim/toggle settings back to the Guest defaults; the Guest
defaults back to the shared joy_* values (engine defaults / q3config).
==================
*/
void IN_PadResetFeel( int n ) {
	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS ) {
		return;
	}
	IN_PadCopyFeel( n, n == SPLIT_GUEST_DEFAULTS ? -1 : SPLIT_GUEST_DEFAULTS );
	// R18: and the crouch / sprint toggles (Controls page rows, kept with the binds)
	if ( n == SPLIT_GUEST_DEFAULTS ) {
		Pad_FactoryToggles( n );
	} else {
		int t;
		for ( t = 0; t < PADTOG_COUNT; t++ ) {
			Pad_SetToggle( n, t, Pad_ToggleCvar( SPLIT_GUEST_DEFAULTS, t )->integer ? qtrue : qfalse );
		}
	}
	Pad_Changed( n );
}


int IN_PadNumFeel( void ) {
	return ARRAY_LEN( padFeelCvars );
}


const char *IN_PadFeelName( int i ) {
	return ( i >= 0 && i < (int)ARRAY_LEN( padFeelCvars ) && *padFeelCvars[i] ) ? (*padFeelCvars[i])->name : NULL;
}


/*
==================
IN_PadFeelSet

Set one look/aim/toggle value of player n / the Guest defaults from a
profile file.  Values outside the setting's range or of the wrong kind are
refused (qfalse) and the current value stays.
==================
*/
qboolean IN_PadFeelSet( int n, const char *name, const char *value ) {
	static const char *curves[] = { "linear", "standard", "dynamic", "custom" };
	cvar_t *v = IN_PadFeelCvar( n, name );
	float f;
	int k, i;

	if ( !v || !value ) {
		return qfalse;
	}
	if ( Pad_IsToggleName( name ) ) {
		return qtrue;	// R18: a pre-R18 shared-file line; the toggle is per game now (bind table)
	}
	for ( k = 0; k < (int)ARRAY_LEN( padFeelCvars ) - 1 && Q_stricmp( name, (*padFeelCvars[k])->name ); k++ )
		;
	if ( padFeelCvars[k] == &joy_aimCurve ) {
		for ( i = 0; i < (int)ARRAY_LEN( curves ); i++ ) {
			if ( !Q_stricmp( value, curves[i] ) ) {
				Cvar_Set( v->name, curves[i] );
				return qtrue;
			}
		}
		return qfalse;
	}
	if ( !value[0] || !Q_isanumber( value ) ) {
		return qfalse;
	}
	f = atof( value );
	if ( f < padFeelRange[k][0] || f > padFeelRange[k][1] ) {
		return qfalse;
	}
	Cvar_Set( v->name, value );
	return qtrue;
}


// copy every feel value: src -1 = the shared joy_* cvars
void IN_PadCopyFeel( int dst, int src ) {
	cvar_t *d, *s;
	int k;

	for ( k = 0; k < (int)ARRAY_LEN( padFeelCvars ); k++ ) {
		d = IN_PadFeelCvar( dst, (*padFeelCvars[k])->name );
		s = ( src < 0 ) ? *padFeelCvars[k] : IN_PadFeelCvar( src, (*padFeelCvars[k])->name );
		if ( d && s ) {
			Cvar_Set( d->name, s->string );
		}
	}
}


// copy a bind table: src -1 = the factory set (default_pad.cfg / built-in)
void IN_PadCopyBinds( int dst, int src ) {
	if ( (unsigned)dst > SPLIT_GUEST_DEFAULTS || src > SPLIT_GUEST_DEFAULTS || src == dst ) {
		return;
	}
	if ( dst < MAX_SPLITVIEW ) {
		Pad_ReleaseHeld( dst, -1 );
	}
	if ( src < 0 ) {
		Pad_FactoryBinds( dst );
		return;
	}
	Pad_EnsureBinds( src );
	Pad_CopyBindTable( dst, src );
}


// an explicitly empty table, filled by IN_PadSetBindByName (profile files)
void IN_PadClearBinds( int n ) {
	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS ) {
		return;
	}
	if ( n < MAX_SPLITVIEW ) {
		Pad_ReleaseHeld( n, -1 );
	}
	Pad_ClearBinds( n );
	Pad_FactoryToggles( n );	// R18: a file without toggle lines keeps the game's defaults
	padSlots[n].initialized = qtrue;
	padSlots[n].custom = qtrue;
}


// R18: a 'toggle crouch "1"' line of a profile file
qboolean IN_PadSetToggleByName( int n, const char *word, const char *value ) {
	const int t = Pad_ToggleFromWord( word );

	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS || t < 0 || !value || ( Q_stricmp( value, "0" ) && Q_stricmp( value, "1" ) ) ) {
		return qfalse;
	}
	Pad_SetToggle( n, t, value[0] == '1' );
	return qtrue;
}


/*
==================
IN_PadUpgradeBinds

R18: player n's (or the Guest defaults') bind table, just read from a
profile file, is exactly an earlier built-in layout of this game: nobody
edited it, so it becomes the current built-in layout (binds and toggles).
Returns the round of the old layout ("R11") or NULL.
==================
*/
const char *IN_PadUpgradeBinds( int n ) {
	const char *game = FS_GetCurrentGameDir();
	const char *p, *tok;
	char keyName[64];
	int i, k, key, lines, bound;
	qboolean same;

	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS || !padSlots[n].initialized ) {
		return NULL;
	}
	for ( bound = 0, k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( padSlots[n].binds[k] ) {
			bound++;
		}
	}
	for ( i = 0; i < (int)ARRAY_LEN( padOldDefaults ); i++ ) {
		if ( Q_stricmp( game, padOldDefaults[i].game ) ) {
			continue;
		}
		same = qtrue;
		lines = 0;
		p = padOldDefaults[i].text;
		while ( same ) {
			tok = COM_ParseExt( &p, qtrue );
			if ( !tok[0] ) {
				break;
			}
			Q_strncpyz( keyName, COM_ParseExt( &p, qfalse ), sizeof( keyName ) );
			key = Pad_KeyFromName( keyName );
			tok = COM_ParseExt( &p, qfalse );
			lines++;
			if ( key < 0 || !padSlots[n].binds[key] || strcmp( padSlots[n].binds[key], tok ) ) {
				same = qfalse;
			}
		}
		if ( same && lines == bound ) {
			if ( n < MAX_SPLITVIEW ) {
				Pad_ReleaseHeld( n, -1 );
			}
			Pad_FactoryBinds( n );
			return padOldDefaults[i].round;
		}
	}
	return NULL;
}


qboolean IN_PadSetBindByName( int n, const char *keyName, const char *cmd ) {
	const int key = Pad_KeyFromName( keyName );

	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS || key < 0 || !cmd ) {
		return qfalse;
	}
	Pad_SetBind( n, key, cmd );
	return qtrue;
}


// 'bind PAD_A "+moveup"' lines (default_pad.cfg format)
void IN_PadWriteBinds( int n, fileHandle_t f ) {
	int k;

	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS ) {
		return;
	}
	Pad_EnsureBinds( n );
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( padSlots[n].binds[k] ) {
			FS_Printf( f, "bind %s \"%s\"\n", padKeyNames[k], padSlots[n].binds[k] );
		}
	}
	for ( k = 0; k < PADTOG_COUNT; k++ ) {
		FS_Printf( f, "toggle %s \"%i\"\n", padToggleWords[k], Pad_ToggleCvar( n, k )->integer ? 1 : 0 );
	}
}


int IN_PadKeyNum( const char *name ) {
	return Pad_KeyFromName( name );
}


// the GUID of player n's pad, or NULL
const char *IN_PadGuid( int n ) {
	const padDevice_t *pad = ( (unsigned)n < MAX_SPLITVIEW ) ? Pad_ForPlayer( n ) : NULL;
	return pad ? pad->guid : NULL;
}


// player n's settings are about to be swapped: nothing it holds stays down
void IN_PadReleasePlayer( int n ) {
	if ( (unsigned)n >= MAX_SPLITVIEW ) {
		return;
	}
	Pad_ReleaseHeld( n, -1 );
	CL_SplitReleaseInput( n );
}


/*
==================
IN_PadMigrateLegacy

R7's interim store <game>/splitpads.cfg: player 1's binds and feel ('pslot
1' / 'pbind 1' / 'pset 1') become the Guest defaults.  Returns qtrue if
anything was imported.  The profile layer decides when to call this and
removes the file.
==================
*/
qboolean IN_PadMigrateLegacy( const char *text ) {
	const char *p = text, *tok;
	char name[64];
	int player, imported = 0;
	qboolean binds = qfalse;

	while ( 1 ) {
		tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		if ( !Q_stricmp( tok, "pslot" ) ) {
			if ( atoi( COM_ParseExt( &p, qfalse ) ) == 1 ) {
				Pad_ClearBinds( PAD_DEFAULTS );
				padSlots[PAD_DEFAULTS].initialized = qtrue;
				padSlots[PAD_DEFAULTS].custom = qtrue;
				binds = qtrue;
				imported++;
			}
		} else if ( !Q_stricmp( tok, "pbind" ) ) {
			player = atoi( COM_ParseExt( &p, qfalse ) );
			Q_strncpyz( name, COM_ParseExt( &p, qfalse ), sizeof( name ) );
			tok = COM_ParseExt( &p, qfalse );
			if ( player == 1 && binds ) {
				IN_PadSetBindByName( PAD_DEFAULTS, name, tok );
			}
		} else if ( !Q_stricmp( tok, "pset" ) ) {
			player = atoi( COM_ParseExt( &p, qfalse ) );
			Q_strncpyz( name, COM_ParseExt( &p, qfalse ), sizeof( name ) );
			tok = COM_ParseExt( &p, qfalse );
			if ( player == 1 && IN_PadFeelSet( PAD_DEFAULTS, name, tok ) ) {
				imported++;
			}
		}
		SkipRestOfLine( &p );
	}
	return imported > 0 ? qtrue : qfalse;
}


/*
==================
Pad_ExecBind

Run player's binding for a pad key in that player's context.  Button
commands carry a key number (MAX_KEYS + pad key, never a keyboard key) and
the time, so kbutton two-key tracking and sub-frame timing work per player.
==================
*/
static qboolean padFromCommand;	// inside padinject: run right after it, not at the buffer end

static void Pad_ExecBind( int player, int key, qboolean down, int time ) {
	char buf[MAX_STRING_CHARS], out[MAX_STRING_CHARS * 2], *p, *end;
	const char *bind;
	int len = 0;

	Pad_EnsureBinds( player );
	bind = padSlots[player].binds[key];
	if ( !bind || !bind[0] ) {
		return;
	}

	out[0] = '\0';
	Q_strncpyz( buf, bind, sizeof( buf ) );
	p = buf;
	while ( 1 ) {
		qboolean quote = qfalse;
		while ( *p == ' ' || *p == '\t' ) {
			p++;
		}
		// next ';' outside quotes (say "a;b" stays one command)
		for ( end = p; *end; end++ ) {
			if ( *end == '"' ) {
				quote = !quote;
			} else if ( *end == ';' && !quote ) {
				break;
			}
		}
		if ( *end ) {
			*end = '\0';
		} else {
			end = NULL;
		}
		if ( len < (int)sizeof( out ) - 1 ) {
			if ( *p == '+' ) {
				len += Com_sprintf( out + len, sizeof( out ) - len, "p%i %c%s %i %i\n", player + 1, down ? '+' : '-', p + 1, MAX_KEYS + key, time );
			} else if ( down && *p ) {
				len += Com_sprintf( out + len, sizeof( out ) - len, "p%i %s\n", player + 1, p );
			}
		}
		if ( !end ) {
			break;
		}
		p = end + 1;
	}

	if ( len > 0 && in_padDebug->integer ) {
		Com_Printf( "pad bind: %s", out );
	}
	if ( len > 0 ) {
		if ( padFromCommand ) {
			out[len - 1] = '\0';	// Cbuf_InsertText adds the \n
			Cbuf_InsertText( out );
		} else {
			Cbuf_AddText( out );
		}
	}
}


/*
==================
Pad_ReleaseHeld

A bind is about to change: if the player's pad holds that key (key -1 =
every key) its current bind gets the up now, so a '+command' can't stick.
==================
*/
// a crouch / sprint latched by its toggle gets its up (key -1 = whatever key;
// t -1 = every toggle)
static void Pad_ReleaseToggle( int player, int key, int t ) {
	padSlot_t *s = &padSlots[player];
	int i, k;

	for ( i = 0; i < PADTOG_COUNT; i++ ) {
		k = s->latched[i] - 1;
		if ( k < 0 || ( key >= 0 && key != k ) || ( t >= 0 && t != i ) ) {
			continue;
		}
		s->latched[i] = 0;
		if ( in_padDebug && in_padDebug->integer ) {
			Com_Printf( "pad: P%i %s toggle off (%s up)\n", player + 1, padToggleWords[i], padToggleCmds[i] );
		}
		if ( CL_SplitSlotActive( player ) ) {
			Pad_ExecBind( player, k, qfalse, Sys_Milliseconds() );
		}
	}
}


static void Pad_ReleaseCrouch( int player, int key ) {
	Pad_ReleaseToggle( player, key, -1 );
}


static void Pad_ReleaseHeld( int player, int key ) {
	padDevice_t *pad = Pad_ForPlayer( player );
	int k;

	Pad_ReleaseCrouch( player, key );
	if ( !pad ) {
		return;
	}
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( ( key < 0 || k == key ) && pad->sentDown[k] ) {
			pad->sentDown[k] = qfalse;
			if ( CL_SplitSlotActive( player ) ) {
				Pad_ExecBind( player, k, qfalse, Sys_Milliseconds() );
			}
		}
	}
}


/*
=============================================================================

JOIN / LEAVE / AUTO-HEAL (design 6 join flow, 12.1)

=============================================================================
*/

static void Pad_Assign( padDevice_t *pad, int player ) {
	int k;

	pad->player = player;
	pad->joinDone = qtrue;		// the press that joined is not a game input
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		pad->sentDown[k] = qfalse;
	}
	padSlots[player].lostPad = qfalse;
	Pad_EnsureBinds( player );
	if ( player == 0 && CL_IndepCoordinator() ) {
		Com_Printf( "indep: pad %s -> P1 (pid %i, the coordinator)\n", pad->key, Sys_SplitPid() );
	}
}


static void Pad_Unassign( padDevice_t *pad ) {
	int k;

	if ( pad->player >= 0 ) {
		Pad_ReleaseCrouch( pad->player, -1 );
	}
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( pad->sentDown[k] && pad->player >= 0 && CL_SplitSlotActive( pad->player ) ) {
			Pad_ExecBind( pad->player, k, qfalse, Sys_Milliseconds() );
		}
		pad->sentDown[k] = qfalse;
	}
	pad->player = -1;
	pad->joinDone = qtrue;		// buttons still held don't join again
	pad->leaveStart = 0;
}


// cl_splitP1Input pad (default): the first pad to press a button is player 1
static qboolean Pad_P1IsPad( void ) {
	return Q_stricmp( cl_splitP1Input->string, "kbm" ) ? qtrue : qfalse;
}


// R19: a pad's key plus the keys of the devices confirmed to duplicate it (space separated): what
// the window that plays with it may open
static const char *Pad_KeysFor( const padDevice_t *pad ) {
	static char list[MAX_CVAR_VALUE_STRING];
	int i, n = 1;

	Q_strncpyz( list, pad->key, sizeof( list ) );
	for ( i = 0; i < MAX_PADS && n < 4; i++ ) {
		if ( pads[i].connected && pads[i].mirrorOf == Pad_Index( pad ) + 1 && pads[i].mirrorAliased && pads[i].key[0] ) {
			Q_strcat( list, sizeof( list ), va( " %s", pads[i].key ) );
			n++;
		}
	}
	return list;
}


// the GUID part of a key ("<guid>@<hash>" / "<guid>#<n>" / a virtual pad's name)
static qboolean Pad_KeyGuidIs( const char *key, const char *guid ) {
	const int len = (int)strlen( guid );
	return ( !Q_stricmpn( key, guid, len ) && ( key[len] == '@' || key[len] == '#' || key[len] == '\0' ) ) ? qtrue : qfalse;
}


/*
==================
Pad_IndepReclaim

Independent mode, the coordinator: an unassigned pad that a running window
plays with (it was unplugged and is back) goes straight back to that
window's slot -- no join, no heal of another player.  By key (R19: the
device path, the same after a replug); a device that comes back under a
new key (no path: its order changed) is matched by GUID, but only to a
window that lost a pad of that GUID before this device appeared, and only
if exactly one such window waits.  The window is told the new key.
==================
*/
static qboolean Pad_IndepReclaim( padDevice_t *pad ) {
	const char *slotKeys;
	int n, i, found, count;

	if ( !CL_IndepCoordinator() || pad->player >= 0 || pad->mirrorOf ) {
		return qfalse;
	}
	n = CL_IndepPadOwner( Pad_DeviceId( pad ) );
	if ( n >= 0 && Pad_ForPlayer( n ) && !strchr( pad->key, '@' ) ) {
		return qfalse;	// an order key ("#n") that a window had is another device now: a free pad
	}
	if ( n >= 0 && Pad_ForPlayer( n ) ) {
		// that window's pad is here already: this device (same path) is another listing of it
		pad->mirrorOf = Pad_Index( Pad_ForPlayer( n ) ) + 1;
		pad->joinDone = qtrue;
		Com_Printf( "indep: pad %s (pad %i) is another device of P%i's pad (pad %i): ignored (duplicate)\n",
			pad->key, Pad_Index( pad ), n + 1, pad->mirrorOf - 1 );
		return qfalse;
	}
	if ( n < 0 ) {
		// GUID fallback (a key without a device path changes with the device order)
		found = -1;
		count = 0;
		for ( i = 1; i < MAX_SPLITVIEW; i++ ) {
			if ( !CL_IndepSlotLive( i ) || CL_IndepPicking( i ) || Pad_ForPlayer( i ) || !padSlots[i].indepLostAt
				|| pad->attachTime < padSlots[i].indepLostAt ) {
				continue;
			}
			slotKeys = CL_IndepSlotKeys( i );
			if ( slotKeys && Pad_KeyGuidIs( slotKeys, pad->guid ) ) {
				found = i;
				count++;
			}
		}
		if ( count != 1 ) {
			return qfalse;
		}
		n = found;
		Com_Printf( "indep: pad %s (pad %i): P%i's window lost a pad of this kind -- the same pad back under a new key\n",
			pad->key, Pad_Index( pad ), n + 1 );
		CL_IndepPadAlias( n, pad->key );
	}
	Pad_Assign( pad, n );
	padSlots[n].indepLostAt = 0;
	Com_Printf( "pad %i: back -- it plays in P%i's window\n", Pad_Index( pad ), n + 1 );
	Com_Printf( "indep: pad %s -> P%i (pid %i): reconnected\n", pad->key, n + 1, CL_IndepSlotPid( n ) );
	return qtrue;
}


// a player who has no pad but should have one: lost it in game, an extra
// player added from the console, or player 1 with cl_splitP1Input pad.
// Lowest first; -1 = none.
static int Pad_HealTarget( void ) {
	int i;
	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( !CL_SplitSlotActive( i ) || Pad_ForPlayer( i ) || CL_IndepSlotLive( i ) ) {
			continue;
		}
		// player 1 is a pad player by default (cl_splitP1Input pad): any button takes it
		if ( padSlots[i].lostPad || i > 0 || Pad_P1IsPad() ) {
			return i;
		}
	}
	return -1;
}


// can extra players join now (P1 in game on our own server or a remote one, not a demo)?
static qboolean Pad_CanAddPlayers( void ) {
	return ( cla->playerNum == 0 && cls.state == CA_ACTIVE && !clc.demoplaying ) ? qtrue : qfalse;
}


// the player number the next join would get, or -1
static int Pad_NextJoinSlot( void ) {
	if ( Pad_P1IsPad() && !Pad_ForPlayer( 0 ) ) {
		return 0;
	}
	if ( CL_IndepCoordinator() ) {
		return CL_IndepFreeSlot();	// a window of its own, from the main menu too
	}
	if ( !Pad_CanAddPlayers() ) {
		return -1;
	}
	return CL_SplitFreeSlot();
}


static void Pad_Join( padDevice_t *pad ) {
	int n;

	if ( Pad_P1IsPad() && !Pad_ForPlayer( 0 ) ) {
		Pad_Assign( pad, 0 );
		Com_Printf( "pad %i: joined as P1 (keyboard/mouse still work too)\n", Pad_Index( pad ) );
		CL_ProfileP1Pad( pad->guid );
		return;
	}
	if ( CL_IndepCoordinator() ) {
		// Independent mode: the profile picker here, then a window (process) of its own
		if ( Pad_IndepReclaim( pad ) || pad->mirrorOf ) {
			return;		// R19: a pad a window plays with (or a second listing of one) never joins again
		}
		n = CL_IndepReserve( Pad_KeysFor( pad ) );
		if ( n < 0 ) {
			pad->joinDone = qtrue;
			return;
		}
		Pad_Assign( pad, n );
		Com_Printf( "pad %i: P%i picks a profile for its own window\n", Pad_Index( pad ), n + 1 );
		CL_ProfileLoad( n, NULL, qtrue );
		CL_SplitMenuJoin( n, pad->guid );
		return;
	}
	if ( !Pad_CanAddPlayers() ) {
		Com_Printf( "pad %i: join needs player 1 in a game on this machine's server\n", Pad_Index( pad ) );
		pad->joinDone = qtrue;
		return;
	}
	// the slot and its cell are taken; the player connects once a profile is
	// picked in the cell (B there cancels the join)
	n = CL_SplitAddPlayerEx( -1, qtrue );
	if ( n < 0 ) {
		pad->joinDone = qtrue;
		return;
	}
	Pad_Assign( pad, n );
	Com_Printf( "pad %i: joining as P%i (profile picker)\n", Pad_Index( pad ), n + 1 );
	CL_ProfileLoad( n, NULL, qtrue );	// a guest until something else is picked
	CL_SplitMenuJoin( n, pad->guid );
}


// a button of a pad that is nobody's yet
static void Pad_UnjoinedPress( padDevice_t *pad, int key ) {
	int target;

	if ( cla->playerNum != 0 || CL_IndepChild() ) {
		return;		// (a player's own window never joins or heals anybody: the coordinator does)
	}

	// Independent mode: a window's pad that came back (unplugged and replugged) is that window's again
	if ( Pad_IndepReclaim( pad ) ) {
		return;
	}

	// auto-heal: any button takes a player who is missing a pad
	target = Pad_HealTarget();
	if ( target >= 0 ) {
		Pad_Assign( pad, target );
		Com_Printf( "pad %i: took over P%i (it had no pad)\n", Pad_Index( pad ), target + 1 );
		if ( target == 0 ) {
			CL_ProfileP1Pad( pad->guid );
		}
		return;
	}

	if ( key != Pad_KeyFromName( cl_splitJoinButton->string ) ) {
		return;
	}
	pad->joinDone = qfalse;
	if ( cl_splitJoinHold->integer <= 0 ) {
		Pad_Join( pad );
	}
	// a hold is completed by Pad_CheckHolds
}


static void Pad_Leave( padDevice_t *pad ) {
	const int n = pad->player;

	if ( n == 0 && CL_IndepChild() ) {
		// Independent mode: this window's player leaves = the window closes, the others re-tile
		Com_Printf( "pad %i: P%i leaves (Back+Start): this window closes\n", Pad_Index( pad ), CL_IndepChildPlayer() );
		Cbuf_AddText( "quit\n" );
		return;
	}
	if ( n == 0 ) {
		// player 1 is the host slot: its pad steps out, the session goes on
		Com_Printf( "pad %i: released from P1 (P1 is the host; end the game from its menu)\n", Pad_Index( pad ) );
		Pad_Unassign( pad );
		return;
	}
	Com_Printf( "pad %i: P%i leaves (Back+Start)\n", Pad_Index( pad ), n + 1 );
	Pad_Unassign( pad );
	CL_SplitRequestDrop( n, "left (Back+Start)" );
}


static void Pad_CheckHolds( padDevice_t *pad, int now ) {
	const int joinKey = Pad_KeyFromName( cl_splitJoinButton->string );

	if ( pad->mirrorOf && pad->player < 0 ) {
		return;		// R19: a duplicate device never joins
	}
	// hold-to-join
	if ( pad->player < 0 ) {
		if ( joinKey >= 0 && pad->down[joinKey] ) {
			if ( !pad->joinDone && cl_splitJoinHold->integer > 0
				&& now - pad->downTime[joinKey] >= cl_splitJoinHold->integer ) {
				pad->joinDone = qtrue;
				Pad_Join( pad );
			}
		} else {
			pad->joinDone = qfalse;
		}
		return;
	}

	// Independent mode: a pad playing in another window leaves there
	if ( CL_IndepSlotLive( pad->player ) ) {
		return;
	}

	// hold Back+Start to leave
	if ( pad->down[PAD_BACK] && pad->down[PAD_START] ) {
		if ( !pad->leaveStart ) {
			pad->leaveStart = now ? now : 1;
		} else if ( !pad->leaveDone && now - pad->leaveStart >= cl_splitLeaveHold->integer ) {
			pad->leaveDone = qtrue;
			Pad_Leave( pad );
		}
	} else {
		pad->leaveStart = 0;
		pad->leaveDone = qfalse;
	}
}


/*
=============================================================================

MENU CONTROL (design 13.2)

While a player has a menu open (player 1: the stock ui; others: their own
menu instance) its pad drives that menu instead of the game: left stick /
d-pad = arrow keys with repeat, A = Enter, B = Escape, right stick = that
menu's cursor, RT / LT = left / right click, Start = close.  Player 1's menu
keys go through the engine's own key/mouse event queue, exactly as a
keyboard and mouse would.

=============================================================================
*/

#define PAD_REPEAT_DELAY	400		// msec before a held arrow repeats
#define PAD_REPEAT_RATE		100		// msec between repeats
#define PAD_KEYBOARD_HOLD	500		// msec holding Y in a mod menu opens the on-screen keyboard

static int Pad_MenuKeyFor( int key ) {
	switch ( key ) {
	case PAD_DPAD_UP: case PAD_LSTICK_UP: return K_UPARROW;
	case PAD_DPAD_DOWN: case PAD_LSTICK_DOWN: return K_DOWNARROW;
	case PAD_DPAD_LEFT: case PAD_LSTICK_LEFT: return K_LEFTARROW;
	case PAD_DPAD_RIGHT: case PAD_LSTICK_RIGHT: return K_RIGHTARROW;
	case PAD_A: return K_ENTER;
	case PAD_B: return K_ESCAPE;
	case PAD_RT: return K_MOUSE1;
	case PAD_LT: return K_MOUSE2;
	default: return 0;
	}
}


static qboolean Pad_MenuRepeats( int key ) {
	const int mk = Pad_MenuKeyFor( key );
	return ( mk == K_UPARROW || mk == K_DOWNARROW || mk == K_LEFTARROW || mk == K_RIGHTARROW ) ? qtrue : qfalse;
}


// the player has a menu: the engine overlay (cl_splitmenu.c) or the mod's own
static qboolean Pad_InMenu( int player ) {
	return ( CL_SplitMenuOpen( player ) || CL_SplitUIMenuOpen( player ) ) ? qtrue : qfalse;
}


static void Pad_MenuSend( int player, int menuKey, qboolean down ) {
	if ( CL_SplitMenuOpen( player ) ) {
		if ( down ) {
			CL_SplitMenuKey( player, menuKey );	// the overlay acts on presses (and repeats)
		}
	} else if ( player == 0 ) {
		Sys_QueEvent( Sys_Milliseconds(), SE_KEY, menuKey, down, 0, NULL );
	} else {
		CL_SplitUIKeyEvent( player, menuKey, down );
	}
}


// Start: close the player's menu (player 1 only in game; the main menu stays,
// Start there opens the host's splitscreen settings)
static void Pad_MenuClose( int player ) {
	if ( CL_SplitMenuOpen( player ) ) {
		CL_SplitMenuClose( player );
		return;
	}
	if ( player == 0 && cls.state == CA_DISCONNECTED ) {
		CL_SplitMenuSettings( 0 );
		return;
	}
	if ( player != 0 ) {
		CL_SplitUIClose( player );
		return;
	}
	CL_PushContext( 0 );
	if ( cls.state == CA_ACTIVE && uivm && !uivm->callLevel ) {
		VM_Call( uivm, 1, UI_SET_ACTIVE_MENU, UIMENU_NONE );
	}
	CL_PopContext();
}


// the player's menu opened: nothing it held in game keeps going
static void Pad_MenuEnter( padDevice_t *pad ) {
	if ( pad->inMenu ) {
		return;
	}
	pad->inMenu = qtrue;
	Pad_ReleaseHeld( pad->player, -1 );
	if ( pad->player > 0 ) {
		CL_SplitReleaseInput( pad->player );
	}
	pad->cursorRem[0] = pad->cursorRem[1] = 0.0f;
	pad->cursorHold = 0.0f;
}


// the menu closed: release the menu keys it still holds; a button held
// across the close does nothing in game until pressed again
static void Pad_MenuLeave( padDevice_t *pad ) {
	int k, mk;

	if ( !pad->inMenu ) {
		return;
	}
	pad->inMenu = qfalse;
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( !pad->menuDown[k] ) {
			continue;
		}
		pad->menuDown[k] = qfalse;
		mk = Pad_MenuKeyFor( k );
		if ( mk && pad->player == 0 ) {
			Pad_MenuSend( 0, mk, qfalse );
		}
	}
}


/*
==================
Pad_MenuFrame

Once per frame for a joined pad: follow its player's menu opening and
closing, repeat held arrows, move the cursor with the right stick
(cl_padCursorSpeed menu units per second at full push, squared response,
up to twice as fast after holding it far over for 0.6 s).
==================
*/
/*
==================
IN_PadCursorSpeed

Player n's right-stick cursor speed at full push (menu units per second):
cl_padCursorSpeed x the player's Cursor speed (joy_cursorSpeed, R17); the
game's own menus (their stock crosshair) x cl_padModCursorScale on top
(R14a 1.25, R17 1.4); the splitscreen overlay's dot as is.
==================
*/
float IN_PadCursorSpeed( int n, qboolean gameMenu ) {
	float speed;

	if ( (unsigned)n >= MAX_SPLITVIEW || !cl_padCursorSpeed ) {
		return 0.0f;
	}
	speed = Pad_PlayerValue( n, cl_padCursorSpeed ) * Pad_PlayerValue( n, joy_cursorSpeed );
	if ( gameMenu ) {
		speed *= cl_padModCursorScale->value;
	}
	return speed;
}


static void Pad_MenuFrame( padDevice_t *pad, int now, float dt ) {
	const int n = pad->player;
	float m, dx, dy, speed;
	int k, ix, iy;

	if ( Pad_InMenu( n ) ) {
		Pad_MenuEnter( pad );
	} else {
		Pad_MenuLeave( pad );
		return;
	}

	// hold Y in the mod's own menu: the on-screen keyboard types into it
	if ( !CL_SplitMenuOpen( n ) && pad->menuDown[PAD_Y] && pad->down[PAD_Y] && !pad->captured[PAD_Y]
		&& now - pad->downTime[PAD_Y] >= PAD_KEYBOARD_HOLD ) {
		pad->captured[PAD_Y] = qtrue;	// once per hold
		CL_SplitMenuKeyboard( n );
	}

	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( pad->menuDown[k] && pad->down[k] && Pad_MenuRepeats( k ) && !pad->captured[k] && now - pad->repeatAt[k] >= 0 ) {
			pad->repeatAt[k] = now + PAD_REPEAT_RATE;
			Pad_MenuSend( n, Pad_MenuKeyFor( k ), qtrue );
		}
	}

	m = Pad_Radial( pad->axis[PAX_RX], pad->axis[PAX_RY], Pad_PlayerValue( n, joy_deadzone ),
		Pad_PlayerValue( n, joy_deadzoneOuter ), &dx, &dy );
	if ( m <= 0.0f ) {
		pad->cursorHold = 0.0f;
		pad->cursorRem[0] = pad->cursorRem[1] = 0.0f;
		return;
	}
	if ( m > 0.9f ) {
		pad->cursorHold += dt;
	} else {
		pad->cursorHold = 0.0f;
	}
	speed = IN_PadCursorSpeed( n, !CL_SplitMenuOpen( n ) ) * m * m * ( 1.0f + MIN( pad->cursorHold / 0.6f, 1.0f ) );
	pad->cursorRem[0] += dx * speed * dt;
	pad->cursorRem[1] += dy * speed * dt;
	ix = (int)pad->cursorRem[0];
	iy = (int)pad->cursorRem[1];
	pad->cursorRem[0] -= ix;
	pad->cursorRem[1] -= iy;
	if ( !ix && !iy ) {
		return;
	}
	if ( CL_SplitMenuOpen( n ) ) {
		CL_SplitMenuMouse( n, ix, iy );
	} else if ( n == 0 ) {
		Sys_QueEvent( Sys_Milliseconds(), SE_MOUSE, ix, iy, 0, NULL );
	} else {
		CL_SplitUIMouseEvent( n, ix, iy );
	}
}


/*
==================
Pad_RouteKey

Every pad key event: unjoined pads join / heal, a player with a menu open
drives it, otherwise the player's pad binds run.
==================
*/
static void Pad_RouteKey( padDevice_t *pad, int key, qboolean down, int time ) {
	int mk, t;

	// R20: a full-screen cinematic (the intro idlogo.roq, Quake 3 and UrT): any button of any
	// pad skips it like a key press (cl_keys.c: a key < 128 is ESCAPE there), and the press and
	// its release are consumed (no join, no menu key, no bind; a held join button does not
	// complete a hold-to-join afterwards)
	if ( cls.state == CA_CINEMATIC ) {
		if ( down ) {
			pad->joinDone = qtrue;
			pad->captured[key] = qtrue;
			if ( key < PAD_LSTICK_UP && Key_GetCatcher() == 0 && !Cvar_VariableIntegerValue( "com_cameraMode" ) ) {
				Com_Printf( "pad %i: %s skips the cinematic\n", Pad_Index( pad ), padKeyNames[key] );
				CL_KeyEvent( K_ESCAPE, qtrue, time );
				CL_KeyEvent( K_ESCAPE, qfalse, time );
			}
		}
		return;
	}
	if ( !down && pad->captured[key] && !pad->menuDown[key] && !pad->sentDown[key] && !pad->cgameDown[key] ) {
		pad->captured[key] = qfalse;
		return;		// R20: the release of a press consumed above
	}

	if ( pad->mirrorOf && pad->player < 0 ) {
		return;		// R19: a second listing of another pad (duplicate device): its input is that pad's
	}
	if ( pad->player < 0 ) {
		if ( down ) {
			Pad_UnjoinedPress( pad, key );
		}
		return;
	}

	// Independent mode: once its window runs, the pad belongs to that process (only its
	// profile picker here takes its keys)
	if ( CL_IndepSlotLive( pad->player ) && !CL_SplitMenuOpen( pad->player ) && !pad->menuDown[key] ) {
		return;
	}

	if ( !down ) {
		pad->captured[key] = qfalse;
		if ( pad->cgameDown[key] ) {
			CL_SplitCgameKey( pad->player, pad->cgameDown[key], qfalse );
			pad->cgameDown[key] = 0;
		}
		if ( pad->menuDown[key] ) {
			pad->menuDown[key] = qfalse;
			mk = Pad_MenuKeyFor( key );
			if ( mk && pad->inMenu ) {
				Pad_MenuSend( pad->player, mk, qfalse );
			}
		}
		if ( pad->sentDown[key] ) {
			pad->sentDown[key] = qfalse;
			Pad_ExecBind( pad->player, key, qfalse, time );
		}
		return;
	}

	if ( Pad_InMenu( pad->player ) ) {
		Pad_MenuEnter( pad );
		pad->menuDown[key] = qtrue;		// its release belongs to the menu too
		if ( CL_SplitMenuCapturing( pad->player ) ) {
			// "press a button": the raw pad key (sticks are for moving, not binds)
			pad->captured[key] = qtrue;
			if ( key < PAD_LSTICK_UP ) {
				CL_SplitMenuCapture( pad->player, key );
			}
			return;
		}
		// the on-screen keyboard's own buttons (X, Y, L3, Start)
		if ( CL_SplitMenuPadKey( pad->player, key ) ) {
			return;
		}
		if ( key == PAD_START ) {
			Pad_MenuClose( pad->player );
			return;
		}
		// R14a: X / Y stop a refreshing server browser (the stock menu's SPACE)
		if ( ( key == PAD_X || key == PAD_Y ) && !CL_SplitMenuOpen( pad->player )
			&& CL_SplitUIBrowserRefreshing( pad->player ) ) {
			pad->captured[key] = qtrue;		// (a held Y does not open the keyboard)
			Com_Printf( "pad: P%i %s -> SPACE (stops the server list refresh)\n", pad->player + 1, padKeyNames[key] );
			Pad_MenuSend( pad->player, K_SPACE, qtrue );
			Pad_MenuSend( pad->player, K_SPACE, qfalse );
			return;
		}
		// R14a: LB / RB turn the pages of the stock player model page
		if ( ( key == PAD_LB || key == PAD_RB ) && !CL_SplitMenuOpen( pad->player ) && CL_SplitUIModelPage( pad->player ) ) {
			CL_SplitUIModelTurn( pad->player, key == PAD_LB ? -1 : 1 );
			return;
		}
		mk = Pad_MenuKeyFor( key );
		if ( mk ) {
			if ( Pad_MenuRepeats( key ) ) {
				pad->repeatAt[key] = time + PAD_REPEAT_DELAY;
			}
			Pad_MenuSend( pad->player, mk, qtrue );
		}
		return;
	}

	// player 1's console and chat line are keyboard only
	if ( pad->player == 0 && ( Key_GetCatcher() & ( KEYCATCH_CONSOLE | KEYCATCH_UI | KEYCATCH_MESSAGE ) ) ) {
		return;
	}

	// a cgame catching keys (KEYCATCH_CGAME: a mod's in-cgame menu or vote screen)
	// gets the pad's keys instead of the binds, like the keyboard's: d-pad/stick =
	// arrows, A = enter, B = escape, triggers = mouse buttons, others K_PAD0_*
	if ( CL_SplitCgameCatches( pad->player ) ) {
		mk = Pad_MenuKeyFor( key );
		if ( !mk && key <= PAD_DPAD_RIGHT ) {
			mk = K_PAD0_A + key;
		}
		if ( mk ) {
			pad->cgameDown[key] = mk;
			if ( in_padDebug->integer ) {
				Com_Printf( "pad: P%i %s -> its cgame (key %s)\n", pad->player + 1, padKeyNames[key], Key_KeynumToString( mk ) );
			}
			CL_SplitCgameKey( pad->player, mk, qtrue );
		}
		return;
	}

	// crouch / sprint toggle (R18: sprint too): a press latches the command, the next press releases it
	for ( t = 0; t < PADTOG_COUNT; t++ ) {
		if ( Pad_PlayerValue( pad->player, *padToggleCvars[t] ) && padSlots[pad->player].binds[key]
			&& !Q_stricmp( padSlots[pad->player].binds[key], padToggleCmds[t] ) ) {
			if ( padSlots[pad->player].latched[t] ) {
				Pad_ReleaseToggle( pad->player, -1, t );
			} else {
				padSlots[pad->player].latched[t] = key + 1;
				padSlots[pad->player].sprintIdle = 0;
				if ( in_padDebug->integer ) {
					Com_Printf( "pad: P%i %s toggle on (%s down)\n", pad->player + 1, padToggleWords[t], padToggleCmds[t] );
				}
				Pad_ExecBind( pad->player, key, qtrue, time );
			}
			return;
		}
	}

	pad->sentDown[key] = qtrue;
	Pad_ExecBind( pad->player, key, qtrue, time );
}


/*
=============================================================================

ANALOG MOVE / LOOK

=============================================================================
*/

// per-player value: p<N>_<cvar> if it exists, else the shared cvar
static float Pad_PlayerValue( int player, const cvar_t *base ) {
	char name[MAX_CVAR_VALUE_STRING];

	Com_sprintf( name, sizeof( name ), "p%i_%s", player + 1, base->name );
	if ( Cvar_Flags( name ) != CVAR_NONEXISTENT ) {
		return Cvar_VariableValue( name );
	}
	return base->value;
}


// radial deadzone: returns the rescaled magnitude 0..1 and the direction
static float Pad_Radial( float x, float y, float inner, float outer, float *dx, float *dy ) {
	const float mag = sqrtf( x * x + y * y );
	float m;

	*dx = *dy = 0.0f;
	if ( mag <= inner || mag <= 0.0f ) {
		return 0.0f;
	}
	if ( outer <= inner ) {
		outer = inner + 0.01f;
	}
	m = ( mag - inner ) / ( outer - inner );
	if ( m > 1.0f ) {
		m = 1.0f;
	}
	*dx = x / mag;
	*dy = y / mag;
	return m;
}


/*
==================
Pad_AimCurve

Look response on the rescaled stick magnitude m (0..1), design 14.4:
linear; standard = m^2; dynamic = S-curve (cubic ease-in-out: as fine as
standard near the center, steep mid-stick); custom = m^joy_aimExponent.
==================
*/
static float Pad_AimCurve( int n, float m ) {
	char name[MAX_CVAR_VALUE_STRING];
	char curve[32];

	Com_sprintf( name, sizeof( name ), "p%i_%s", n + 1, joy_aimCurve->name );
	if ( Cvar_Flags( name ) != CVAR_NONEXISTENT ) {
		Cvar_VariableStringBuffer( name, curve, sizeof( curve ) );
	} else {
		Q_strncpyz( curve, joy_aimCurve->string, sizeof( curve ) );
	}

	if ( !Q_stricmp( curve, "linear" ) ) {
		return m;
	}
	if ( !Q_stricmp( curve, "dynamic" ) ) {
		return ( m < 0.5f ) ? 4.0f * m * m * m : 1.0f - 4.0f * ( 1.0f - m ) * ( 1.0f - m ) * ( 1.0f - m );
	}
	if ( !Q_stricmp( curve, "custom" ) ) {
		return powf( m, Com_Clamp( 0.5f, 4.0f, Pad_PlayerValue( n, joy_aimExponent ) ) );
	}
	return m * m;	// standard
}


// the player holds a pad button bound to +zoom
static qboolean Pad_ZoomHeld( const padDevice_t *pad ) {
	const padSlot_t *s = &padSlots[ pad->player ];
	int k;

	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( pad->sentDown[k] && s->binds[k] && !Q_stricmpn( s->binds[k], "+zoom", 5 ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


// per-player aim state
typedef struct {
	float	boostMs;			// time the look stick has been near full deflection
	float	lastYaw, lastPitch;	// previous frame's rates (smoothing)
	float	dbgYaw, dbgMs;		// in_padDebug 2: degrees turned / msec since the last report
	int		dbgNext;
	int		moveDbgNext;
	qboolean	talkDbg;		// in_padDebug 2: last BUTTON_TALK state printed
} padAim_t;

static padAim_t padAim[MAX_SPLITVIEW];

#define PAD_BOOST_THRESHOLD	0.9f	// rescaled look-stick magnitude that counts as "full"


/*
==================
CL_GamepadMove

Called from CL_CreateCmd for the active context: its pad's left stick
moves, its right stick turns (degrees per second, frame-rate independent).
==================
*/
void CL_GamepadMove( usercmd_t *cmd ) {
	const int n = cla->playerNum;
	padDevice_t *pad;
	padAim_t *aim;
	float inner, outer, m, dx, dy, c, dt, ms, boost, zoom, boostTime, yawRate, pitchRate, scale;
	qboolean run, zoomed, moveInput;

	if ( !padInitialized || (unsigned)n >= MAX_SPLITVIEW ) {
		return;
	}
	aim = &padAim[n];
	pad = Pad_ForPlayer( n );
	if ( in_padDebug->integer >= 2 && ( ( cmd->buttons & BUTTON_TALK ) != 0 ) != aim->talkDbg ) {
		aim->talkDbg = ( cmd->buttons & BUTTON_TALK ) != 0;
		Com_Printf( "P%i usercmd: BUTTON_TALK %s\n", n + 1, aim->talkDbg ? "on" : "off" );
	}
	// a menu takes the sticks (Pad_MenuFrame); player 1's console mutes them
	if ( !pad || Pad_InMenu( n ) || ( n == 0 && ( Key_GetCatcher() & ( KEYCATCH_CONSOLE | KEYCATCH_UI ) ) ) ) {
		aim->boostMs = aim->lastYaw = aim->lastPitch = 0.0f;
		CL_AimAssistReset( n );
		return;
	}

	inner = Pad_PlayerValue( n, joy_deadzone );
	outer = Pad_PlayerValue( n, joy_deadzoneOuter );

	// move: magnitude is speed; a gentle push walks.  joy_alwaysRun 1: run,
	// +speed walks (like cl_run 1); 0: walk, +speed runs.  Walking means
	// walking speed, not just the quiet walk bit.
	m = Pad_Radial( pad->axis[PAX_LX], pad->axis[PAX_LY], inner, outer, &dx, &dy );
	moveInput = ( m > 0.0f ) ? qtrue : qfalse;
	if ( m > 0.0f ) {
		run = Pad_PlayerValue( n, joy_alwaysRun ) ? !cla->in.speed.active : cla->in.speed.active;
		scale = run ? 127.0f : 64.0f;
		cmd->forwardmove = ClampCharMove( cmd->forwardmove + (int)( -dy * m * scale ) );
		cmd->rightmove = ClampCharMove( cmd->rightmove + (int)( dx * m * scale ) );
		if ( !run || m < Pad_PlayerValue( n, joy_walkThreshold ) ) {
			cmd->buttons |= BUTTON_WALKING;
		} else {
			cmd->buttons &= ~BUTTON_WALKING;
		}
		if ( in_padDebug->integer >= 2 && cls.realtime >= aim->moveDbgNext ) {
			Com_Printf( "P%i move: stick %.2f,%.2f m %.3f %s -> forwardmove %i rightmove %i walk bit %i\n", n + 1,
				pad->axis[PAX_LX], pad->axis[PAX_LY], m, run ? "run" : "walk", cmd->forwardmove, cmd->rightmove,
				( cmd->buttons & BUTTON_WALKING ) ? 1 : 0 );
			aim->moveDbgNext = cls.realtime + 500;
		}
	}

	// look (design 14.4): curve, turn boost near full deflection (yaw), zoom scale
	ms = (float)cls.frametime;
	dt = ms * 0.001f;
	m = Pad_Radial( pad->axis[PAX_RX], pad->axis[PAX_RY], inner, outer, &dx, &dy );
	yawRate = pitchRate = 0.0f;
	boost = zoom = 1.0f;
	c = 0.0f;
	if ( m > 0.0f ) {
		c = Pad_AimCurve( n, m );

		if ( m >= PAD_BOOST_THRESHOLD ) {
			aim->boostMs += ms;
			boostTime = Pad_PlayerValue( n, joy_turnBoostTime );
			boost = 1.0f + ( Pad_PlayerValue( n, joy_turnBoost ) - 1.0f )
				* ( boostTime > 0.0f ? MIN( aim->boostMs / boostTime, 1.0f ) : 1.0f );
		} else {
			aim->boostMs = 0.0f;
		}

		// zoomed: the player holds +zoom, or the mod reports a reduced sensitivity
		zoomed = Pad_ZoomHeld( pad ) || ( cl.cgameSensitivity > 0.0f && cl.cgameSensitivity < 0.99f );
		if ( zoomed ) {
			zoom = Pad_PlayerValue( n, joy_zoomScale );
		}

		yawRate = Pad_PlayerValue( n, joy_yawSpeed ) * c * dx * boost * zoom;
		pitchRate = Pad_PlayerValue( n, joy_pitchSpeed ) * c * dy * zoom;
		if ( Pad_PlayerValue( n, joy_invertPitch ) ) {
			pitchRate = -pitchRate;
		}
	} else {
		aim->boostMs = 0.0f;
	}

	// optional light smoothing: average with the previous frame
	if ( Pad_PlayerValue( n, joy_aimSmooth ) ) {
		const float y = yawRate, p = pitchRate;
		yawRate = ( yawRate + aim->lastYaw ) * 0.5f;
		pitchRate = ( pitchRate + aim->lastPitch ) * 0.5f;
		aim->lastYaw = y;
		aim->lastPitch = p;
	}

	// aim assist (design 15; cl_aimassist.c): after the curve, before the angles; local games only
	CL_AimAssistApply( n, moveInput, m > 0.0f, &yawRate, &pitchRate, ms );

	cl.viewangles[YAW] -= yawRate * dt;
	cl.viewangles[PITCH] += pitchRate * dt;

	// in_padDebug 2: the turn rate actually applied, twice a second
	if ( in_padDebug->integer >= 2 && ( yawRate != 0.0f || pitchRate != 0.0f ) ) {
		const int interval = ( in_padDebug->integer >= 3 ) ? 0 : 500;
		if ( aim->dbgNext - cls.realtime > interval ) {
			aim->dbgNext = cls.realtime + interval;	// in_padDebug changed
		}
		aim->dbgYaw += yawRate * dt;
		aim->dbgMs += ms;
		if ( cls.realtime >= aim->dbgNext && aim->dbgMs > 0.0f ) {
			Com_Printf( "P%i aim: stick %.2f,%.2f m %.3f curve %.3f boost %.2f zoom %.2f -> yaw %.1f pitch %.1f deg/s (measured yaw %.1f deg/s over %.0f ms)\n",
				n + 1, pad->axis[PAX_RX], pad->axis[PAX_RY], m, c, boost, zoom, yawRate, pitchRate,
				aim->dbgYaw * 1000.0f / aim->dbgMs, aim->dbgMs );
			aim->dbgYaw = aim->dbgMs = 0.0f;
			aim->dbgNext = cls.realtime + ( in_padDebug->integer >= 3 ? 0 : 500 );
		}
	} else {
		aim->dbgYaw = aim->dbgMs = 0.0f;	// a new measurement starts with the next push
		aim->dbgNext = cls.realtime + ( in_padDebug->integer >= 3 ? 0 : 500 );
	}
}


/*
=============================================================================

JOIN HINT

=============================================================================
*/

static const char *Pad_ButtonLabel( int key, padType_t type ) {
	static const struct { int key; const char *xbox, *ps, *nin; } labels[] = {
		{ PAD_A, "A", "Cross", "B" },
		{ PAD_B, "B", "Circle", "A" },
		{ PAD_X, "X", "Square", "Y" },
		{ PAD_Y, "Y", "Triangle", "X" },
		{ PAD_START, "Start", "Options", "+" },
		{ PAD_BACK, "Back", "Share", "-" },
		{ PAD_LB, "LB", "L1", "L" },
		{ PAD_RB, "RB", "R1", "R" },
		{ PAD_LT, "LT", "L2", "ZL" },
		{ PAD_RT, "RT", "R2", "ZR" },
	};
	int i;

	if ( key < 0 ) {
		return "?";
	}
	for ( i = 0; i < (int)ARRAY_LEN( labels ); i++ ) {
		if ( labels[i].key == key ) {
			return ( type == PADTYPE_PLAYSTATION ) ? labels[i].ps : ( type == PADTYPE_NINTENDO ) ? labels[i].nin : labels[i].xbox;
		}
	}
	return padKeyNames[key] + 4;
}


/*
==================
IN_GamepadDrawHint

In game (P1 active), centered in an empty cell if the layout has one, else
in the bottom-right corner.  cl_splitJoinHint 1: "Player N: press A to
join" for an unjoined pad that produced real input in the last 10 s (idle
virtual pads -- ViGEm, receivers without a pad -- never trigger it).
cl_splitJoinHint 2: "Press A on a controller to join" for the first 10 s
of each level, nothing else.
==================
*/
#define PAD_HINT_TIME	10000
#define PAD_HINT_REMOTE	"online servers may limit extra players"

void IN_GamepadDrawHint( void ) {
	static const float bg[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
	static int levelStart;
	static qboolean wasActive;
	const int now = Sys_Milliseconds();
	char text[128];
	const char *note;
	viewRect_t r;
	padDevice_t *pad = NULL;
	int i, slot, size, w, h, x, y;
	qboolean heal;

	if ( CL_IndepChild() ) {
		return;		// Independent mode: the coordinator's window shows the hint
	}
	if ( cls.state != CA_ACTIVE && !( CL_IndepCoordinator() && cls.state == CA_DISCONNECTED ) ) {
		wasActive = qfalse;	// loading a level: mode 2 starts its 10 s again
		return;
	}
	if ( !wasActive ) {
		wasActive = qtrue;
		levelStart = cls.realtime;
	}
	if ( !padInitialized || !cl_splitJoinHint->integer || cla->playerNum != 0 || clc.demoplaying ) {
		return;
	}
	if ( CL_SplitRefusalShowing() ) {
		return;	// a remote server just refused a player: no "hold A to join" over its note
	}

	if ( cl_splitJoinHint->integer == 2 ) {
		if ( cls.realtime - levelStart > PAD_HINT_TIME || Pad_NextJoinSlot() < 0 ) {
			return;
		}
		for ( i = 0; i < MAX_PADS && !pads[i].connected; i++ )
			;
		Com_sprintf( text, sizeof( text ), "Press %s on a controller to join",
			Pad_ButtonLabel( Pad_KeyFromName( cl_splitJoinButton->string ), i < MAX_PADS ? pads[i].type : PADTYPE_XBOX ) );
	} else {
		for ( i = 0; i < MAX_PADS; i++ ) {
			if ( pads[i].connected && pads[i].player < 0 && !pads[i].mirrorOf && pads[i].lastInput
				&& now - pads[i].lastInput < PAD_HINT_TIME ) {
				pad = &pads[i];
				break;
			}
		}
		if ( !pad ) {
			return;
		}

		slot = Pad_HealTarget();
		heal = ( slot >= 0 );
		if ( !heal ) {
			slot = Pad_NextJoinSlot();
			if ( slot < 0 ) {
				return;
			}
		}
		if ( heal ) {
			Com_sprintf( text, sizeof( text ), "Player %i: press any button", slot + 1 );
		} else {
			Com_sprintf( text, sizeof( text ), "Player %i: %s %s to join", slot + 1,
				cl_splitJoinHold->integer > 0 ? "hold" : "press",
				Pad_ButtonLabel( Pad_KeyFromName( cl_splitJoinButton->string ), pad->type ) );
		}
	}

	// on another machine's server the join may be refused (its per-address cap
	// and other limits are unknown in advance): say so under the hint
	note = CL_SplitRemoteServer() ? PAD_HINT_REMOTE : NULL;

	// console font in real pixels
	size = smallchar_height;
	w = (int)strlen( text ) * smallchar_width;
	if ( note ) {
		w = MAX( w, (int)strlen( note ) * smallchar_width );
	}
	h = smallchar_height * ( note ? 2 : 1 );

	if ( CL_SplitEmptyCell( &r ) ) {
		x = r.x + ( r.w - w ) / 2;
		y = r.y + ( r.h - h ) / 2;
	} else {
		x = cls.glconfig.vidWidth - w - size;
		y = cls.glconfig.vidHeight - h - size;
	}

	re.SetColor( bg );
	re.DrawStretchPic( x - size / 2, y - size / 2, w + size, h + size, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
	SCR_DrawSmallStringExt( x, y, text, g_color_table[ ColorIndex( COLOR_WHITE ) ], qfalse, qtrue );
	if ( note ) {
		SCR_DrawSmallStringExt( x, y + smallchar_height, note, g_color_table[ ColorIndex( COLOR_YELLOW ) ], qfalse, qtrue );
	}
}


/*
==================
IN_GamepadDrawFirstHint

R20: first launch.  At the main menu (Together mode, nothing running, a pad
connected) "Start a game first, then friends press A to join", in the join
hint's style and corner, until the first game starts (cl_splitSeenHint 1,
set by IN_GamepadFrame once a game is active; never shown again).
Independent mode is left out: pads join from its main menu.
==================
*/
void IN_GamepadDrawFirstHint( void ) {
	static const float bg[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
	char text[128];
	int i, size, w, h, x, y;

	if ( !padInitialized || !cl_splitJoinHint->integer || cl_splitSeenHint->integer ) {
		return;
	}
	if ( cls.state != CA_DISCONNECTED || !( Key_GetCatcher() & KEYCATCH_UI ) || clc.demoplaying
		|| CL_IndepChild() || CL_IndepCoordinator() ) {
		return;
	}
	for ( i = 0; i < MAX_PADS && !pads[i].connected; i++ )
		;
	if ( i == MAX_PADS ) {
		return;
	}
	Com_sprintf( text, sizeof( text ), "Start a game first, then friends %s %s to join",
		cl_splitJoinHold->integer > 0 ? "hold" : "press", Pad_ButtonLabel( Pad_KeyFromName( cl_splitJoinButton->string ), pads[i].type ) );

	size = smallchar_height;
	w = (int)strlen( text ) * smallchar_width;
	h = smallchar_height;
	x = cls.glconfig.vidWidth - w - size;
	y = cls.glconfig.vidHeight - h - size;
	re.SetColor( bg );
	re.DrawStretchPic( x - size / 2, y - size / 2, w + size, h + size, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
	SCR_DrawSmallStringExt( x, y, text, g_color_table[ ColorIndex( COLOR_WHITE ) ], qfalse, qtrue );
}


/*
=============================================================================

FRAME, COMMANDS, INIT

=============================================================================
*/

/*
==================
IN_GamepadFrame

Once per frame from the platform input frame (before Com_Frame).
==================
*/
/*
==================
Pad_ApplyFov

R14a: a player with a profile (or as Guest) sees with its Field of view
(feel setting joy_fov): player 1's cg_fov (its own q3config value is kept
with its identity and comes back at quit / keyboard-mouse), the others'
p<N>_cg_fov, which their cgames read (cg_fov is per player,
CL_SplitShadowed).  Keyboard/mouse player 1 is left alone.
==================
*/
static void Pad_ApplyFov( void ) {
	const char *name;
	int n, fov;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( !CL_ProfileActive( n ) || ( n > 0 && !CL_SplitSlotActive( n ) ) ) {
			continue;
		}
		fov = (int)Pad_PlayerValue( n, joy_fov );
		if ( fov < 80 || fov > 130 ) {
			continue;
		}
		name = ( n == 0 ) ? "cg_fov" : CL_SplitShadow( n, "cg_fov", NULL )->name;
		if ( Cvar_VariableIntegerValue( name ) != fov || !Cvar_VariableString( name )[0] ) {
			Cvar_Set( name, va( "%i", fov ) );
		}
	}
}


/*
==================
Pad_MirrorFrame

R19, Independent mode, the coordinator: one physical pad listed as two
devices (two HID collections of one receiver, a remapper's virtual pad next
to the real one ...) would join twice and give a window a device it may not
even see.  Rule: two pads, at least one of them nobody's, whose every button
and stick key is the same while something is held, each held key gone down
within PAD_MIRROR_MS on both, are one pad: the one that is nobody's (of two
such, the later device) is the duplicate.  Its input is ignored here (no
join, no hint, no heal) and its key goes to the window that plays with the
other one as an alias it may open.  Undone when the two differ for
PAD_MIRROR_SPLIT ms (two people who pressed together).
==================
*/
#define PAD_MIRROR_MS		100
#define PAD_MIRROR_SPLIT	150
#define PAD_MIRROR_STABLE	1000	// R19b: matched this long (and confirmed by a press) before its key goes to a window

static qboolean Pad_SameInput( const padDevice_t *a, const padDevice_t *b ) {
	int k;

	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( a->down[k] != b->down[k] ) {
			return qfalse;
		}
	}
	return qtrue;
}


static void Pad_MirrorFrame( int now ) {
	padDevice_t *a, *b, *orig, *dup;
	int i, j, k, held;

	if ( !CL_IndepCoordinator() ) {
		return;
	}
	for ( i = 0; i < MAX_PADS; i++ ) {
		a = &pads[i];
		if ( !a->connected || !a->mirrorOf ) {
			continue;
		}
		b = &pads[a->mirrorOf - 1];
		if ( Pad_SameInput( a, b ) ) {
			a->mirrorDiff = 0;
			// R19b: a later press that went down on both together confirms it
			for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
				if ( a->down[k] && b->down[k] && a->downTime[k] > a->mirrorEdgeTime && a->downTime[k] > a->mirrorSince
					&& abs( a->downTime[k] - b->downTime[k] ) <= PAD_MIRROR_MS ) {
					a->mirrorEdgeTime = a->downTime[k];
					a->mirrorEdges++;
				}
			}
			// only a duplicate that matched for PAD_MIRROR_STABLE ms and was confirmed by another
			// press goes into the other pad's window's key list (two people never get there)
			if ( !a->mirrorAliased && a->mirrorEdges > 0 && now - a->mirrorSince >= PAD_MIRROR_STABLE
				&& b->player > 0 && CL_IndepSlotLive( b->player ) ) {
				a->mirrorAliased = qtrue;
				Com_Printf( "indep: pad %i (key %s) is confirmed as a second listing of P%i's pad (pad %i)\n", i, a->key,
					b->player + 1, a->mirrorOf - 1 );
				CL_IndepPadAlias( b->player, a->key );	// that window may open this listing too
			}
		} else if ( !a->mirrorDiff ) {
			a->mirrorDiff = now | 1;
		} else if ( now - a->mirrorDiff >= PAD_MIRROR_SPLIT ) {
			Com_Printf( "indep: pad %i (key %s) moves on its own: not a duplicate of pad %i after all "
				"(its presses meanwhile were ignored: press again to join)\n", i, a->key, a->mirrorOf - 1 );
			if ( a->mirrorAliased && b->player > 0 ) {
				CL_IndepPadUnalias( b->player, a->key );
			}
			a->mirrorOf = 0;
			a->mirrorDiff = 0;
			a->mirrorAliased = qfalse;
			a->mirrorEdges = 0;
			a->joinDone = qtrue;
		}
	}
	for ( i = 0; i < MAX_PADS; i++ ) {
		a = &pads[i];
		if ( !a->connected || a->mirrorOf ) {
			continue;
		}
		for ( j = i + 1; j < MAX_PADS; j++ ) {
			b = &pads[j];
			if ( !b->connected || b->mirrorOf || ( a->player >= 0 && b->player >= 0 ) || !Pad_SameInput( a, b ) ) {
				continue;
			}
			for ( held = 0, k = 0; k < PAD_KEY_COUNT; k++ ) {
				if ( a->down[k] ) {
					if ( abs( a->downTime[k] - b->downTime[k] ) > PAD_MIRROR_MS ) {
						break;
					}
					held++;
				}
			}
			if ( !held || k < PAD_KEY_COUNT ) {
				continue;
			}
			// the one that is nobody's; of two such, the later device (b: a higher slot)
			orig = ( b->player >= 0 ) ? b : a;
			dup = ( orig == a ) ? b : a;
			dup->mirrorOf = Pad_Index( orig ) + 1;
			dup->mirrorDiff = 0;
			dup->mirrorSince = now | 1;
			dup->mirrorEdges = 0;
			dup->mirrorEdgeTime = 0;
			dup->mirrorAliased = qfalse;
			dup->joinDone = qtrue;
			Com_Printf( "indep: pad %i (key %s) duplicates pad %i (key %s, %s): one pad listed twice -- the duplicate's input is ignored"
				" (its key goes to that pad's window once confirmed)\n",
				Pad_Index( dup ), dup->key, Pad_Index( orig ), orig->key, orig->player >= 0 ? va( "P%i", orig->player + 1 ) : "nobody's yet" );
			if ( a->mirrorOf ) {
				break;
			}
		}
	}
}


void IN_GamepadFrame( void ) {
	static int lastFrame;
	static int lastFrameTime;	// R18: sprint rest timer
	const int now = Sys_Milliseconds();
	padDevice_t *pad;
	float dt;
	int i, t;

	if ( !padInitialized ) {
		return;
	}

	// R20: the first game has started: the first-launch main-menu hint is done for good
	if ( cls.state == CA_ACTIVE && !cl_splitSeenHint->integer && !CL_IndepChild() && !clc.demoplaying ) {
		Cvar_Set( "cl_splitSeenHint", "1" );
		Com_Printf( "pad: first game started, first-launch hint done (cl_splitSeenHint 1)\n" );
	}

	dt = lastFrame ? ( now - lastFrame ) * 0.001f : 0.0f;
	dt = Com_Clamp( 0.0f, 0.1f, dt );
	lastFrame = now;

	// players dropped since last frame (dropplayer, errors, P1 left) free their pads
	for ( i = 0; i < MAX_PADS; i++ ) {
		pad = &pads[i];
		if ( pad->connected && pad->player >= 0 && !CL_SplitSlotActive( pad->player ) && !CL_IndepSlotLive( pad->player ) ) {
			Com_Printf( "pad %i: P%i is gone, pad is free to join again\n", i, pad->player + 1 );
			Pad_Unassign( pad );
		}
		if ( pad->connected ) {
			Pad_IndepReclaim( pad );
		}
	}
	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( padSlots[i].lostPad && !CL_SplitSlotActive( i ) ) {
			padSlots[i].lostPad = qfalse;
		}
		// crouch / sprint toggle switched off (menu, profile switch) while latched
		for ( t = 0; t < PADTOG_COUNT; t++ ) {
			if ( padSlots[i].latched[t] && !Pad_PlayerValue( i, *padToggleCvars[t] ) ) {
				Pad_ReleaseToggle( i, -1, t );
			}
		}
		// R18: a latched sprint ends when the player stops (left stick at rest 0.3 s),
		// as console games do; the next run needs a new click
		if ( padSlots[i].latched[PADTOG_SPRINT] ) {
			pad = Pad_ForPlayer( i );
			if ( pad && sqrtf( pad->axis[PAX_LX] * pad->axis[PAX_LX] + pad->axis[PAX_LY] * pad->axis[PAX_LY] )
					<= Pad_PlayerValue( i, joy_deadzone ) ) {
				padSlots[i].sprintIdle += now - ( lastFrameTime ? lastFrameTime : now );
				if ( padSlots[i].sprintIdle >= 300 ) {
					if ( in_padDebug->integer ) {
						Com_Printf( "pad: P%i stopped moving\n", i + 1 );
					}
					Pad_ReleaseToggle( i, -1, PADTOG_SPRINT );
				}
			} else {
				padSlots[i].sprintIdle = 0;
			}
		}
	}
	lastFrameTime = now;

	// cl_splitP1Input switched to kbm: player 1's pad steps out once its menu is closed
	pad = Pad_ForPlayer( 0 );
	if ( pad && !Pad_P1IsPad() && !Pad_InMenu( 0 ) ) {
		Com_Printf( "pad %i: released from P1 (cl_splitP1Input kbm: player 1 is keyboard/mouse)\n", Pad_Index( pad ) );
		Pad_Unassign( pad );
	}

	if ( sdl.active ) {
		SDLG_Poll();
	}
	Bus_Frame();	// R19 test device bus (in_padBus 1)
	Pad_MirrorFrame( now );	// R19: one pad listed as two devices

	for ( i = 0; i < MAX_PADS; i++ ) {
		if ( pads[i].connected ) {
			Pad_CheckHolds( &pads[i], now );
		}
		if ( pads[i].connected && pads[i].player >= 0 ) {
			Pad_MenuFrame( &pads[i], now, dt );
		}
	}

	CL_SplitMenuFrame();
	CL_ProfileFrame();
	Pad_ApplyFov();
}


static int Pad_PlayerArg( int arg ) {
	const int n = atoi( Cmd_Argv( arg ) ) - 1;
	if ( (unsigned)n >= MAX_SPLITVIEW ) {
		Com_Printf( "player number must be 1..%i\n", MAX_SPLITVIEW );
		return -1;
	}
	return n;
}


static void Pad_Bind_f( void ) {
	int n, key;

	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "usage: pbind <player 1..%i> <padkey> [command]\n", MAX_SPLITVIEW );
		return;
	}
	if ( ( n = Pad_PlayerArg( 1 ) ) < 0 ) {
		return;
	}
	key = Pad_KeyFromName( Cmd_Argv( 2 ) );
	if ( key < 0 ) {
		Com_Printf( "pbind: \"%s\" isn't a pad key (see pbindlist)\n", Cmd_Argv( 2 ) );
		return;
	}
	Pad_EnsureBinds( n );
	if ( Cmd_Argc() == 3 ) {
		Com_Printf( "P%i %s = \"%s\"\n", n + 1, padKeyNames[key], padSlots[n].binds[key] ? padSlots[n].binds[key] : "" );
		return;
	}
	Pad_ReleaseHeld( n, key );
	Pad_SetBind( n, key, Cmd_ArgsFrom( 3 ) );
	padSlots[n].custom = qtrue;
	Pad_Changed( n );
}


static void Pad_Unbind_f( void ) {
	int n, key;

	if ( Cmd_Argc() != 3 ) {
		Com_Printf( "usage: punbind <player 1..%i> <padkey|all>\n", MAX_SPLITVIEW );
		return;
	}
	if ( ( n = Pad_PlayerArg( 1 ) ) < 0 ) {
		return;
	}
	if ( !Q_stricmp( Cmd_Argv( 2 ), "defaults" ) ) {
		// back to default_pad.cfg / built-in
		IN_PadResetBinds( n );
		return;
	}
	Pad_EnsureBinds( n );
	if ( !Q_stricmp( Cmd_Argv( 2 ), "all" ) ) {
		Pad_ReleaseHeld( n, -1 );
		Pad_ClearBinds( n );
	} else {
		key = Pad_KeyFromName( Cmd_Argv( 2 ) );
		if ( key < 0 ) {
			Com_Printf( "punbind: \"%s\" isn't a pad key\n", Cmd_Argv( 2 ) );
			return;
		}
		Pad_ReleaseHeld( n, key );
		Pad_SetBind( n, key, NULL );
	}
	padSlots[n].custom = qtrue;
	Pad_Changed( n );
}


static void Pad_BindList_f( void ) {
	int n, k;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: pbindlist <player 1..%i>\npad keys:", MAX_SPLITVIEW );
		for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
			Com_Printf( " %s", padKeyNames[k] );
		}
		Com_Printf( "\n" );
		return;
	}
	if ( ( n = Pad_PlayerArg( 1 ) ) < 0 ) {
		return;
	}
	Pad_EnsureBinds( n );
	Com_Printf( "P%i pad binds (%s):\n", n + 1, CL_ProfileDescribe( n ) );
	for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
		if ( padSlots[n].binds[k] ) {
			Com_Printf( "  %-16s \"%s\"\n", padKeyNames[k], padSlots[n].binds[k] );
		}
	}
}


static void Pad_List_f( void ) {
	padDevice_t *pad;
	int i, k, found = 0;

	if ( sdl.active ) {
		Com_Printf( "SDL %i.%i.%i, %i joystick device(s) visible to SDL\n", sdl.version.major, sdl.version.minor,
			sdl.version.patch, sdl.NumJoysticks() );
		for ( i = 0; i < sdl.NumJoysticks(); i++ ) {
			char guid[40];
			sdl.JoystickGetGUIDString( sdl.JoystickGetDeviceGUID( i ), guid, sizeof( guid ) );
			Com_Printf( "  SDL %i: \"%s\" guid %s vid %04x pid %04x %s\n", i,
				sdl.JoystickNameForIndex( i ) ? sdl.JoystickNameForIndex( i ) : "?", guid,
				sdl.JoystickGetDeviceVendor( i ), sdl.JoystickGetDeviceProduct( i ),
				sdl.IsGameController( i ) ? "game controller" : "not a game controller (ignored)" );
			if ( sdl.JoystickPathForIndex && sdl.JoystickPathForIndex( i ) ) {
				Com_Printf( "         path %s\n", sdl.JoystickPathForIndex( i ) );
			}
		}
	} else {
		Com_Printf( "SDL not active (no SDL2.dll or in_gamepad 0): virtual pads only\n" );
	}

	for ( i = 0; i < MAX_PADS; i++ ) {
		pad = &pads[i];
		if ( !pad->connected ) {
			continue;
		}
		found++;
		Com_Printf( "pad %i: %s%s\"%s\" type %s guid %s player %s key %s%s\n", i, pad->isVirtual ? "virtual " : "", pad->busKey ? "bus " : "",
			pad->name, padTypeNames[pad->type], pad->guid,
			pad->player >= 0 ? va( "P%i", pad->player + 1 ) : "none", pad->key,
			pad->mirrorOf ? va( " (duplicate of pad %i: ignored)", pad->mirrorOf - 1 ) : "" );
		Com_Printf( "       axes lx %+.2f ly %+.2f rx %+.2f ry %+.2f lt %.2f rt %.2f  down:",
			pad->axis[PAX_LX], pad->axis[PAX_LY], pad->axis[PAX_RX], pad->axis[PAX_RY],
			pad->axis[PAX_LT], pad->axis[PAX_RT] );
		for ( k = 0; k < PAD_KEY_COUNT; k++ ) {
			if ( pad->down[k] ) {
				Com_Printf( " %s", padKeyNames[k] );
			}
		}
		Com_Printf( "\n" );
	}
	if ( !found ) {
		Com_Printf( "no pads connected\n" );
	}
	for ( i = 0; i < MAX_SPLITVIEW; i++ ) {
		if ( padSlots[i].lostPad ) {
			Com_Printf( "P%i: lost its pad (guid %s), waiting\n", i + 1, padSlots[i].lostGuid );
		}
	}
}


/*
==================
Pad_Inject_f

padinject <pad> connect [xbox|playstation|nintendo] [guid]
padinject <pad> disconnect
padinject <pad> button <padkey> <0|1>
padinject <pad> axis <lx|ly|rx|ry|lt|rt> <-1..1>

Virtual devices for headless tests, through the same code as SDL pads.
Needs sv_cheats or developer.
==================
*/
static void Pad_Inject( void );

static void Pad_Inject_f( void ) {
	padFromCommand = qtrue;
	Pad_Inject();
	padFromCommand = qfalse;
}

static void Pad_Inject( void ) {
	const char *what, *guidArg;
	padDevice_t *pad;
	padType_t type;
	int i, k;

	if ( !Cvar_VariableIntegerValue( "sv_cheats" ) && !( com_developer && com_developer->integer ) ) {
		Com_Printf( "padinject: needs sv_cheats or developer\n" );
		return;
	}
	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "usage: padinject <pad 0..%i> connect [xbox|playstation|nintendo] [guid] | disconnect | "
			"button <padkey> <0|1> | axis <lx|ly|rx|ry|lt|rt> <-1..1>\n", MAX_PADS - 1 );
		return;
	}
	i = atoi( Cmd_Argv( 1 ) );
	if ( i < 0 || i >= MAX_PADS ) {
		Com_Printf( "padinject: pad must be 0..%i\n", MAX_PADS - 1 );
		return;
	}
	pad = &pads[i];
	what = Cmd_Argv( 2 );

	if ( !Q_stricmp( what, "connect" ) ) {
		if ( pad->connected ) {
			Com_Printf( "padinject: pad %i is already connected\n", i );
			return;
		}
		type = PADTYPE_XBOX;
		for ( k = 0; k < (int)ARRAY_LEN( padTypeNames ); k++ ) {
			if ( !Q_stricmpn( Cmd_Argv( 3 ), padTypeNames[k], 2 ) ) {
				type = (padType_t)k;
			}
		}
		guidArg = Cmd_Argc() > 4 ? Cmd_Argv( 4 ) : va( "virtual-pad-%i", i );
		Pad_Attach( i, qtrue, guidArg, guidArg,
			va( "Virtual %s pad %i", padTypeNames[type], i ), type );
		return;
	}

	if ( !pad->connected || !pad->isVirtual ) {
		Com_Printf( "padinject: pad %i is not a connected virtual pad\n", i );
		return;
	}

	if ( !Q_stricmp( what, "disconnect" ) ) {
		Pad_Detach( pad );
	} else if ( !Q_stricmp( what, "button" ) ) {
		k = Pad_KeyFromName( Cmd_Argv( 3 ) );
		if ( k < 0 || k >= PAD_SDL_BUTTONS ) {
			Com_Printf( "padinject: \"%s\" isn't a pad button (triggers/sticks are axes)\n", Cmd_Argv( 3 ) );
			return;
		}
		Pad_SetButton( pad, k, atoi( Cmd_Argv( 4 ) ) ? qtrue : qfalse );
	} else if ( !Q_stricmp( what, "axis" ) ) {
		for ( k = 0; k < PAX_COUNT; k++ ) {
			if ( !Q_stricmp( Cmd_Argv( 3 ), padAxisNames[k] ) ) {
				break;
			}
		}
		if ( k == PAX_COUNT ) {
			Com_Printf( "padinject: axis must be lx ly rx ry lt rt\n" );
			return;
		}
		Pad_SetAxis( pad, k, atof( Cmd_Argv( 4 ) ) );
	} else {
		Com_Printf( "padinject: unknown '%s'\n", what );
	}
}


/*
==================
Pad_Menu_f

padmenu (Start): open the engine pause overlay (cl_splitmenu.c) for the
player running it, inside its cell; the mod's own in-game menu is one of its
rows.  Keyboard Esc still opens player 1's mod menu as upstream.
==================
*/
static void Pad_Menu_f( void ) {
	const int n = cla->playerNum;

	if ( clc.demoplaying || cls.state != CA_ACTIVE ) {
		return;
	}
	if ( n == 0 && ( Key_GetCatcher() & KEYCATCH_UI ) ) {
		return;
	}
	CL_SplitMenuPause( n );
}


/*
=============================================================================

PAD API FOR THE OVERLAY (cl_splitmenu.c)

=============================================================================
*/

int IN_PadNumKeys( void ) {
	return PAD_KEY_COUNT;
}


// a pad key the controls page may bind (Start/Guide open menus, sticks move/look)
qboolean IN_PadBindableKey( int key ) {
	return ( key >= 0 && key < PAD_LSTICK_UP && key != PAD_START && key != PAD_GUIDE ) ? qtrue : qfalse;
}


// a short name for a pad key as printed on player n's pad
const char *IN_PadKeyLabel( int n, int key ) {
	static const char *other[PAD_KEY_COUNT] = {
		NULL, NULL, NULL, NULL, NULL, "Guide", NULL, "L3", "R3", NULL, NULL,
		"D-pad up", "D-pad down", "D-pad left", "D-pad right",
		"Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
		NULL, NULL, "L stick up", "L stick down", "L stick left", "L stick right",
		"R stick up", "R stick down", "R stick left", "R stick right"
	};
	const padDevice_t *pad = ( (unsigned)n < MAX_SPLITVIEW ) ? Pad_ForPlayer( n ) : NULL;

	if ( key < 0 || key >= PAD_KEY_COUNT ) {
		return "?";
	}
	if ( other[key] ) {
		return other[key];
	}
	return Pad_ButtonLabel( key, pad ? pad->type : PADTYPE_XBOX );
}


const char *IN_PadGetBind( int n, int key ) {
	if ( (unsigned)n > MAX_SPLITVIEW || key < 0 || key >= PAD_KEY_COUNT ) {
		return NULL;
	}
	Pad_EnsureBinds( n );
	return padSlots[n].binds[key];
}


// the default bind set (default_pad.cfg or built-in), read once
const char *IN_PadDefaultBind( int key ) {
	return IN_PadGetBind( PAD_DEFAULTS, key );
}


void IN_PadSetBind( int n, int key, const char *cmd ) {
	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS || key < 0 || key >= PAD_KEY_COUNT ) {
		return;
	}
	Pad_EnsureBinds( n );
	Pad_ReleaseHeld( n, key );
	Pad_SetBind( n, key, cmd );
	padSlots[n].custom = qtrue;
	Pad_Changed( n );
}


// a player back to the Guest defaults; the Guest defaults back to the factory set
void IN_PadResetBinds( int n ) {
	if ( (unsigned)n > SPLIT_GUEST_DEFAULTS ) {
		return;
	}
	IN_PadCopyBinds( n, n == SPLIT_GUEST_DEFAULTS ? -1 : SPLIT_GUEST_DEFAULTS );
	Pad_Changed( n );
}


// "Join: hold A (0.7 s)" / "Leave: hold Back+Start (2 s)" for the host page
void IN_PadShortcuts( char *join, int joinSize, char *leave, int leaveSize ) {
	const padDevice_t *pad = Pad_ForPlayer( 0 );
	const padType_t type = pad ? pad->type : PADTYPE_XBOX;

	if ( cl_splitJoinHold->integer > 0 ) {
		Com_sprintf( join, joinSize, "Join: hold %s (%.1f s)", Pad_ButtonLabel( Pad_KeyFromName( cl_splitJoinButton->string ), type ),
			cl_splitJoinHold->integer * 0.001f );
	} else {
		Com_sprintf( join, joinSize, "Join: press %s", Pad_ButtonLabel( Pad_KeyFromName( cl_splitJoinButton->string ), type ) );
	}
	Com_sprintf( leave, leaveSize, "Leave: hold %s+%s (%.0f s)", Pad_ButtonLabel( PAD_BACK, type ),
		Pad_ButtonLabel( PAD_START, type ), cl_splitLeaveHold->integer * 0.001f );
}


// player n's pad type label set: 0 xbox-like, 1 playstation, 2 nintendo (menu footer)
const char *IN_PadButtonName( int n, const char *padKey ) {
	const padDevice_t *pad = ( (unsigned)n < MAX_SPLITVIEW ) ? Pad_ForPlayer( n ) : NULL;
	return Pad_ButtonLabel( Pad_KeyFromName( padKey ), pad ? pad->type : PADTYPE_XBOX );
}


/*
==================
IN_GamepadInit / IN_GamepadShutdown

Client init / shutdown (pads and their player assignment survive
vid_restart and in_restart).
==================
*/
void IN_GamepadInit( void ) {
	int i;

	Com_Memset( pads, 0, sizeof( pads ) );
	for ( i = 0; i < MAX_PADS; i++ ) {
		pads[i].player = -1;
	}
	sdlNumIgnored = 0;
	busNumIgnored = 0;
	busNum = 0;

	in_gamepad = Cvar_Get( "in_gamepad", "1", CVAR_ARCHIVE_ND | CVAR_LATCH );
	Cvar_CheckRange( in_gamepad, "0", "1", CV_INTEGER );
	Cvar_SetDescription( in_gamepad, "Gamepads through SDL2 (0 - off; virtual test pads still work)." );
	in_padDebug = Cvar_Get( "in_padDebug", "0", CVAR_TEMP );
	Cvar_SetDescription( in_padDebug, "Print every gamepad button change." );
	in_padBus = Cvar_Get( "in_padBus", "0", CVAR_TEMP );
	Cvar_SetDescription( in_padBus, "Test runs: read the shared test device bus <fs_homepath>/padbus.txt (padbus command) as if it were the system device list." );
	in_padBusOrder = Cvar_Get( "in_padBusOrder", "0", CVAR_TEMP );
	Cvar_SetDescription( in_padBusOrder, "Test runs: 1 = this process lists the bus devices in reverse order." );

	cl_splitP1Input = Cvar_Get( "cl_splitP1Input", "pad", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_splitP1Input, "Splitscreen: player 1's input (host setting).\n"
		" pad - the first pad to press a button is player 1 (keyboard/mouse still work too)\n"
		" kbm - player 1 is keyboard/mouse; the first pad to join becomes player 2" );
	cl_splitJoinButton = Cvar_Get( "cl_splitJoinButton", "PAD_A", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_splitJoinButton, "Splitscreen: pad key that joins a new player (PAD_A = bottom face button)." );
	cl_splitJoinHold = Cvar_Get( "cl_splitJoinHold", "700", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitJoinHold, "0", "5000", CV_INTEGER );
	Cvar_SetDescription( cl_splitJoinHold, "Splitscreen: milliseconds to hold the join button (0 - a press joins)." );
	cl_splitJoinHint = Cvar_Get( "cl_splitJoinHint", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitJoinHint, "0", "2", CV_INTEGER );
	Cvar_SetDescription( cl_splitJoinHint, "Splitscreen join hint.\n"
		" 0 - off\n"
		" 1 - \"Player N: press A to join\" for an unjoined pad that was used in the last 10 seconds\n"
		" 2 - \"Press A on a controller to join\" for 10 seconds after a level starts, never otherwise" );
	cl_splitSeenHint = Cvar_Get( "cl_splitSeenHint", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitSeenHint, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_splitSeenHint, "Splitscreen: 0 - the main menu shows \"Start a game first, then friends press A to join\" "
		"(Together mode, a pad connected) until the first game starts, which sets 1. Set 0 to see it again." );
	cl_padCursorSpeed = Cvar_Get( "cl_padCursorSpeed", "500", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_padCursorSpeed, "50", "3000", CV_FLOAT );
	Cvar_SetDescription( cl_padCursorSpeed, "Gamepad in menus: right-stick cursor speed in menu units (640 = screen width) "
		"per second at full push. Per player: p<N>_cl_padCursorSpeed." );
	cl_padModCursorScale = Cvar_Get( "cl_padModCursorScale", "1.4", CVAR_ARCHIVE_ND );	// R14a 1.25, R17 1.4
	Cvar_CheckRange( cl_padModCursorScale, "0.25", "4", CV_FLOAT );
	Cvar_SetDescription( cl_padModCursorScale, "Gamepad: the right-stick cursor in the mod's own menus moves this much faster "
		"than cl_padCursorSpeed (the engine overlay's dot: 1x). Both also scale with the player's joy_cursorSpeed." );
	cl_splitLeaveHold = Cvar_Get( "cl_splitLeaveHold", "2000", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_splitLeaveHold, "250", "10000", CV_INTEGER );
	Cvar_SetDescription( cl_splitLeaveHold, "Splitscreen: milliseconds to hold Back+Start to leave the game." );

	joy_deadzone = Cvar_Get( "joy_deadzone", "0.15", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_deadzone, "0", "0.9", CV_FLOAT );
	Cvar_SetDescription( joy_deadzone, "Gamepad sticks: inner radial deadzone (0..1). Per player: p<N>_joy_deadzone." );
	joy_deadzoneOuter = Cvar_Get( "joy_deadzoneOuter", "0.95", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_deadzoneOuter, "0.1", "1", CV_FLOAT );
	Cvar_SetDescription( joy_deadzoneOuter, "Gamepad sticks: deflection that counts as full (0..1)." );
	joy_yawSpeed = Cvar_Get( "joy_yawSpeed", "320", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_yawSpeed, "10", "1000", CV_FLOAT );
	Cvar_SetDescription( joy_yawSpeed, "Gamepad look: degrees per second sideways at full right-stick deflection." );
	joy_pitchSpeed = Cvar_Get( "joy_pitchSpeed", "200", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_pitchSpeed, "10", "1000", CV_FLOAT );
	Cvar_SetDescription( joy_pitchSpeed, "Gamepad look: degrees per second up/down at full right-stick deflection." );
	joy_aimCurve = Cvar_Get( "joy_aimCurve", "standard", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( joy_aimCurve, "Gamepad look response curve: linear, standard (squared: fine aim near "
		"the center), dynamic (S-curve: fine near the center, fast mid-stick), custom (joy_aimExponent). "
		"Per player: p<N>_joy_aimCurve." );
	joy_aimExponent = Cvar_Get( "joy_aimExponent", "2", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimExponent, "0.5", "4", CV_FLOAT );
	Cvar_SetDescription( joy_aimExponent, "Gamepad look: exponent of the custom aim curve (1 - linear)." );
	joy_turnBoost = Cvar_Get( "joy_turnBoost", "1.5", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_turnBoost, "1", "3", CV_FLOAT );
	Cvar_SetDescription( joy_turnBoost, "Gamepad look: sideways turn speed multiplier reached while the look stick "
		"is held near full deflection (1 - off)." );
	joy_turnBoostTime = Cvar_Get( "joy_turnBoostTime", "250", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_turnBoostTime, "0", "2000", CV_INTEGER );
	Cvar_SetDescription( joy_turnBoostTime, "Gamepad look: milliseconds over which the turn boost ramps in." );
	joy_zoomScale = Cvar_Get( "joy_zoomScale", "0.5", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_zoomScale, "0.1", "1", CV_FLOAT );
	Cvar_SetDescription( joy_zoomScale, "Gamepad look speed multiplier while zoomed (+zoom held, or the mod "
		"reports a zoomed view)." );
	joy_aimSmooth = Cvar_Get( "joy_aimSmooth", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimSmooth, "0", "1", CV_INTEGER );
	Cvar_SetDescription( joy_aimSmooth, "Gamepad look: average with the previous frame (light smoothing)." );
	joy_crouchToggle = Cvar_Get( "joy_crouchToggle", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_crouchToggle, "0", "1", CV_INTEGER );
	Cvar_SetDescription( joy_crouchToggle, "Gamepad: 1 - the crouch button (+movedown) toggles crouching; 0 - hold.  Per player and per game (p<N>_ / guest_ shadows, kept with the pad binds; default from the game's pad layout: baseq3 hold, Urban Terror toggle)." );
	joy_sprintToggle = Cvar_Get( "joy_sprintToggle", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_sprintToggle, "0", "1", CV_INTEGER );
	Cvar_SetDescription( joy_sprintToggle, "Gamepad: 1 - the sprint button (+button8, Urban Terror) toggles sprinting (ends when the left stick rests); 0 - hold.  Per player and per game like joy_crouchToggle." );
	joy_alwaysRun = Cvar_Get( "joy_alwaysRun", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_alwaysRun, "0", "1", CV_INTEGER );
	Cvar_SetDescription( joy_alwaysRun, "Gamepad: 1 - full stick runs, +speed walks; 0 - full stick walks, +speed runs." );
	joy_invertPitch = Cvar_Get( "joy_invertPitch", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_invertPitch, "0", "1", CV_INTEGER );
	Cvar_SetDescription( joy_invertPitch, "Gamepad look: invert up/down." );
	joy_triggerThreshold = Cvar_Get( "joy_triggerThreshold", "0.3", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_triggerThreshold, "0.05", "0.95", CV_FLOAT );
	Cvar_SetDescription( joy_triggerThreshold, "Gamepad: trigger travel that presses PAD_LT / PAD_RT." );
	joy_stickThreshold = Cvar_Get( "joy_stickThreshold", "0.5", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_stickThreshold, "0.1", "0.95", CV_FLOAT );
	Cvar_SetDescription( joy_stickThreshold, "Gamepad: stick deflection that presses the PAD_*STICK_* direction keys." );
	joy_walkThreshold = Cvar_Get( "joy_walkThreshold", "0.5", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_walkThreshold, "0", "1", CV_FLOAT );
	Cvar_SetDescription( joy_walkThreshold, "Gamepad: left-stick push below which the player walks (quiet) instead of runs." );
	joy_aimAssist = Cvar_Get( "joy_aimAssist", "0", CVAR_ARCHIVE_ND );	// described in cl_aimassist.c; R17: Off for Guests and new profiles
	Cvar_CheckRange( joy_aimAssist, "0", "2", CV_INTEGER );
	joy_fov = Cvar_Get( "joy_fov", "90", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_fov, "80", "130", CV_INTEGER );
	Cvar_SetDescription( joy_fov, "Gamepad players: field of view (the game's cg_fov) of a player with a profile or as Guest. "
		"Per player: p<N>_joy_fov. Keyboard/mouse player 1 keeps its own cg_fov." );
	joy_cursorSpeed = Cvar_Get( "joy_cursorSpeed", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_cursorSpeed, "0.5", "2", CV_FLOAT );
	Cvar_SetDescription( joy_cursorSpeed, "Gamepad: right-stick menu cursor speed, 0.5 .. 2 (1 = 100 %), for both the game's own "
		"menus and the splitscreen menus (Controls: Cursor speed). Per player: p<N>_joy_cursorSpeed." );

	Cmd_AddCommand( "pbind", Pad_Bind_f );
	Cmd_AddCommand( "punbind", Pad_Unbind_f );
	Cmd_AddCommand( "pbindlist", Pad_BindList_f );
	Cmd_AddCommand( "padlist", Pad_List_f );
	Cmd_AddCommand( "padinject", Pad_Inject_f );
	Cmd_AddCommand( "padbus", Pad_Bus_f );
	Cmd_AddCommand( "padmenu", Pad_Menu_f );

	padInitialized = qtrue;


	if ( in_gamepad->integer ) {
		SDLG_Init();
	} else {
		Com_Printf( "gamepad: in_gamepad 0, SDL not loaded\n" );
	}
}


void IN_GamepadShutdown( void ) {
	int i;

	if ( !padInitialized ) {
		return;
	}


	for ( i = 0; i < MAX_PADS; i++ ) {
		if ( pads[i].ctrl && sdl.active ) {
			sdl.GameControllerClose( pads[i].ctrl );
		}
	}
	Com_Memset( pads, 0, sizeof( pads ) );

	if ( sdl.active ) {
		// only what we started: an SDL build of the engine keeps its video/audio
		sdl.QuitSubSystem( SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER );
		Com_Printf( "gamepad: SDL shut down\n" );
	}
	SDLG_Unload();

	for ( i = 0; i < MAX_SPLITVIEW + 1; i++ ) {
		Pad_ClearBinds( i );
	}
	Com_Memset( padSlots, 0, sizeof( padSlots ) );

	Cmd_RemoveCommand( "pbind" );
	Cmd_RemoveCommand( "punbind" );
	Cmd_RemoveCommand( "pbindlist" );
	Cmd_RemoveCommand( "padlist" );
	Cmd_RemoveCommand( "padinject" );
	Cmd_RemoveCommand( "padbus" );
	Cmd_RemoveCommand( "padmenu" );

	padInitialized = qfalse;
}
