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
// cl_splitprofile.c -- player profiles, Guest, Guest defaults (design 12.3, 13.1, 14.3)
//
// Settings follow the person, not the player number.  A profile is a name
// (also the player's in-game name) plus
//   <homepath>/profiles/<key>.cfg         name and controls feel, all mods
//   <homepath>/<game>/profiles/<key>.cfg  pad binds and player settings
//                                         (userinfo: model, colours, ...)
// <key> is the sanitised name in lower case with '_' for spaces, so two
// names differing only in case are the same profile.  Files are plain
// "keyword value" lines; the reader ignores what it does not know and keeps
// the defaults for what is missing or invalid.
//
// A player slot plays as a named profile (changes are saved, debounced),
// as a Guest ("Player N" with default player settings, P1 too, and an
// in-memory copy of the Guest defaults; changes vanish when
// the player leaves) or -- player 1 with cl_splitP1Input kbm, or before a
// pad claimed player 1 -- as nothing: plain q3config, exactly as upstream.
//
// The Guest defaults (profiles/_guest.cfg, both levels) are edited by the
// host on its Splitscreen page; they affect guests who join afterwards.
// profiles/_padlast.cfg remembers the last profile used with each pad
// (by GUID) for the join-time picker's pre-highlight.
//
// Nothing here writes a file unless a profile is created or changed, the
// Guest defaults are edited, or a pad joins with a named profile.

#include "client.h"

#define PROF_DIR			"profiles"
#define PROF_GUEST			"_guest"
#define PROF_PADLAST		"_padlast"
#define PROF_P1KEEP			"_p1q3config"	// player 1's own identity while it plays as a profile (crash safety)
#define PROF_MAX			64
#define PROF_KEY_LEN		( PROFILE_NAME_LEN + 1 )
#define PROF_MAX_FILE		( 64 * 1024 )
#define PROF_LINE_MAX		512
#define PROF_SAVE_DELAY		1000	// msec: changes are written at most this long after the first
#define PROF_INFO_POLL		500		// msec between userinfo comparisons of named profiles
#define PROF_MAX_PADS		32

typedef struct {
	char	key[PROF_KEY_LEN];
	char	name[PROF_KEY_LEN];
} profEntry_t;

typedef struct {
	qboolean	active;			// a profile or a guest is loaded
	qboolean	guest;
	char		key[PROF_KEY_LEN];
	char		name[PROF_KEY_LEN];
	qboolean	dirty;
	int			dirtyTime;
	int			infoCheck;
	char		info[MAX_INFO_STRING];	// the saved player settings, to notice mod-menu changes
	char		gameName[MAX_CVAR_VALUE_STRING];	// in-game name last seen (mod-menu renames)
} profSlot_t;

static profEntry_t	profList[PROF_MAX];
static int			profCount;
static profSlot_t	profSlots[MAX_SPLITVIEW];
static qboolean		profInitialized;

static qboolean		guestDirty;
static int			guestDirtyTime;

static struct {
	char	guid[40];
	char	key[PROF_KEY_LEN];
} padLast[PROF_MAX_PADS];
static int			numPadLast;

// player 1's own (q3config) settings, kept while P1 plays as a named profile
static qboolean		p1Saved;
static char			profGameDir[MAX_QPATH];	// the mod the per-mod profile files were read for
static char			p1Info[MAX_INFO_STRING];

static cvar_t		*cl_splitP1Input;


/*
=============================================================================

NAMES AND FILES

=============================================================================
*/

/*
==================
CL_ProfileSanitize

Letters, digits, space, '_' and '-'; colour codes dropped; no leading or
trailing separators, single spaces; at most PROFILE_NAME_LEN characters.
qfalse if nothing is left or the name is reserved ("guest").
==================
*/
static qboolean Prof_DeviceName( const char *s ) {
	static const char *dev[] = { "con", "nul", "aux", "prn", "com", "lpt", "conin$", "conout$", NULL };
	int i, len;

	for ( i = 0; dev[i]; i++ ) {
		len = (int)strlen( dev[i] );
		if ( Q_stricmpn( s, dev[i], len ) ) {
			continue;
		}
		if ( i == 4 || i == 5 ) {	// COM1-9, LPT1-9
			if ( s[len] < '1' || s[len] > '9' ) {
				continue;
			}
			len++;
		}
		// the device name alone (no '.' passes sanitising, so no "nul.x"; a space becomes '_' in the key)
		if ( s[len] == '\0' ) {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean CL_ProfileSanitize( const char *in, char *out, int size ) {
	int len = 0, max = MIN( size - 1, PROFILE_NAME_LEN );
	char c;

	out[0] = '\0';
	if ( !in ) {
		return qfalse;
	}
	for ( ; *in && len < max; in++ ) {
		if ( Q_IsColorString( in ) ) {
			in++;
			continue;
		}
		c = *in;
		if ( c == '\t' ) {
			c = ' ';
		}
		if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' )
			|| c == ' ' || c == '_' || c == '-' ) ) {
			continue;
		}
		if ( len == 0 && ( c == ' ' || c == '_' || c == '-' ) ) {
			continue;	// '_' starts the engine's own files
		}
		if ( c == ' ' && out[len - 1] == ' ' ) {
			continue;
		}
		out[len++] = c;
	}
	while ( len > 0 && ( out[len - 1] == ' ' || out[len - 1] == '_' || out[len - 1] == '-' ) ) {
		len--;
	}
	out[len] = '\0';
	if ( !len || !Q_stricmp( out, "guest" ) ) {
		return qfalse;
	}
	// Windows device names would open the device, not a file ("nul.cfg")
	if ( Prof_DeviceName( out ) ) {
		if ( len >= max ) {
			return qfalse;
		}
		out[len++] = '_';
		out[len] = '\0';
	}
	return qtrue;
}


// file key of a sanitised name: lower case, '_' for spaces
static void Prof_Key( const char *name, char *key ) {
	int i;

	for ( i = 0; name[i] && i < PROF_KEY_LEN - 1; i++ ) {
		key[i] = ( name[i] == ' ' ) ? '_' : tolower( (unsigned char)name[i] );
	}
	key[i] = '\0';
}


static const char *Prof_HomePath( void ) {
	return Cvar_VariableString( "fs_homepath" );
}


// <homepath>/profiles/<key>.cfg or <homepath>/<game>/profiles/<key>.cfg
static const char *Prof_OSPath( const char *key, qboolean perMod ) {
	if ( perMod ) {
		return FS_BuildOSPath( Prof_HomePath(), FS_GetCurrentGameDir(), va( PROF_DIR "/%s.cfg", key ) );
	}
	return FS_BuildOSPath( Prof_HomePath(), PROF_DIR, va( "%s.cfg", key ) );
}


// a whole homepath file (never the base path), NUL-terminated; NULL if none
static char *Prof_ReadFile( const char *ospath ) {
	FILE *f;
	char *buf;
	long len;

	f = Sys_FOpen( ospath, "rb" );
	if ( !f ) {
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	len = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( len < 0 ) {
		fclose( f );
		return NULL;
	}
	if ( len > PROF_MAX_FILE ) {
		Com_Printf( S_COLOR_YELLOW "profile: %s is larger than %i KB; only the start is read\n", ospath, PROF_MAX_FILE / 1024 );
		len = PROF_MAX_FILE;
	}
	buf = Z_Malloc( len + 1 );
	len = (long)fread( buf, 1, len, f );
	buf[ len > 0 ? len : 0 ] = '\0';
	fclose( f );
	return buf;
}


static qboolean Prof_Exists( const char *ospath ) {
	FILE *f = Sys_FOpen( ospath, "rb" );

	if ( !f ) {
		return qfalse;
	}
	fclose( f );
	return qtrue;
}


static fileHandle_t Prof_OpenWrite( const char *key, qboolean perMod ) {
	const char *qpath = va( PROF_DIR "/%s.cfg", key );
	fileHandle_t f = perMod ? FS_FOpenFileWrite( qpath ) : FS_SV_FOpenFileWrite( qpath );

	if ( f == FS_INVALID_HANDLE ) {
		Com_Printf( S_COLOR_YELLOW "profile: couldn't write %s%s\n", perMod ? va( "%s/", FS_GetCurrentGameDir() ) : "", qpath );
	}
	return f;
}


/*
==================
Prof_Lines

Calls func for every non-empty, non-comment line of text, already split
into its first word and the rest.  Overlong lines are skipped.  Returns the
number of bad lines (overlong, or refused by func).
==================
*/
typedef qboolean ( *profLineFunc_t )( int n, const char *word, const char *rest );

static int Prof_Lines( int n, const char *text, profLineFunc_t func ) {
	char line[PROF_LINE_MAX];
	char word[64];
	const char *s, *e, *p;
	int len, bad = 0, i;

	for ( s = text; *s; s = *e ? e + 1 : e ) {
		for ( e = s; *e && *e != '\n'; e++ )
			;
		len = (int)( e - s );
		if ( len >= PROF_LINE_MAX ) {
			bad++;
			continue;
		}
		Com_Memcpy( line, s, len );
		line[len] = '\0';
		for ( i = 0; i < len; i++ ) {
			if ( line[i] == '\r' || line[i] == '\t' || (unsigned char)line[i] < ' ' ) {
				line[i] = ' ';
			}
		}
		p = line;
		while ( *p == ' ' ) {
			p++;
		}
		if ( !*p || ( p[0] == '/' && p[1] == '/' ) ) {
			continue;
		}
		for ( i = 0; *p && *p != ' ' && i < (int)sizeof( word ) - 1; p++ ) {
			word[i++] = *p;
		}
		word[i] = '\0';
		while ( *p == ' ' ) {
			p++;
		}
		if ( !func( n, word, p ) ) {
			bad++;
		}
	}
	return bad;
}


// a quoted (or bare) value: the rest of the line without the outer quotes
static void Prof_Value( const char *rest, char *out, int size ) {
	int len;

	Q_strncpyz( out, rest, size );
	len = (int)strlen( out );
	while ( len > 0 && out[len - 1] == ' ' ) {
		out[--len] = '\0';
	}
	if ( len >= 2 && out[0] == '"' && out[len - 1] == '"' ) {
		out[len - 1] = '\0';
		memmove( out, out + 1, len - 1 );
	}
}


// a first token and a second (quoted) one: 'model "doom"', 'pad "guid" "key"'
static qboolean Prof_TwoTokens( const char *rest, char *a, int aSize, char *b, int bSize ) {
	const char *p = rest;

	Q_strncpyz( a, COM_ParseExt( &p, qfalse ), aSize );
	Q_strncpyz( b, COM_ParseExt( &p, qfalse ), bSize );
	return a[0] ? qtrue : qfalse;
}


/*
=============================================================================

PLAYER SETTINGS (USERINFO) OF A SLOT

=============================================================================
*/

// a userinfo key a profile carries (not the name, guid or connection keys)
static qboolean Prof_InfoKey( const char *key ) {
	if ( !key[0] || !Q_stricmp( key, "name" ) || !Q_stricmp( key, "cl_guid" ) ) {
		return qfalse;
	}
	return CL_SplitShadowed( key, CVAR_USERINFO );
}


// the slot's player settings that differ from their defaults, as an info string
static void Prof_SlotInfo( int n, char *out ) {
	char key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	const char *s, *def;

	out[0] = '\0';
	s = CL_SplitBuildUserinfo( n, NULL );
	while ( *s ) {
		s = Info_NextPair( s, key, value );
		if ( !key[0] ) {
			break;
		}
		if ( !Prof_InfoKey( key ) ) {
			continue;
		}
		def = Cvar_DefaultString( key );
		if ( def && !strcmp( def, value ) ) {
			continue;
		}
		Info_SetValueForKey( out, key, value );
	}
}


// one player setting of slot n (player 1: the real cvar, if the mod has it)
static void Prof_SetInfo( int n, const char *key, const char *value ) {
	if ( n == 0 ) {
		if ( Cvar_Flags( key ) & CVAR_USERINFO ) {
			Cvar_Set( key, value );
		}
		return;
	}
	Cvar_Set( CL_SplitShadow( n, key, value )->name, value );
}


// every player setting of slot n back to its default; name = 'name'
static void Prof_ResetInfo( int n, const char *name ) {
	char key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	const char *s, *def;

	s = Cvar_InfoString( CVAR_USERINFO, NULL );
	while ( *s ) {
		s = Info_NextPair( s, key, value );
		if ( !key[0] ) {
			break;
		}
		if ( !Prof_InfoKey( key ) ) {
			continue;
		}
		def = Cvar_DefaultString( key );
		Prof_SetInfo( n, key, def ? def : "" );
	}
	if ( n == 0 ) {
		Cvar_Set( "name", name );
	} else {
		Cvar_Set( CL_SplitShadow( n, "name", NULL )->name, name );
	}
}


// a Guest's in-game name: "Player N" (an Independent-mode window's slot 0 is its own player number)
static const char *Prof_GuestName( int n ) {
	if ( n == 0 && CL_IndepChildPlayer() > 0 ) {
		return va( "Player %i", CL_IndepChildPlayer() );
	}
	return va( "Player %i", n + 1 );
}


// player 1's q3config identity, kept while it plays as a named profile or a Guest
static void Prof_SaveP1( void ) {
	char key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	const char *s;
	fileHandle_t f;

	if ( p1Saved ) {
		return;
	}
	p1Info[0] = '\0';
	s = Cvar_InfoString( CVAR_USERINFO, NULL );
	while ( *s ) {
		s = Info_NextPair( s, key, value );
		if ( !key[0] ) {
			break;
		}
		if ( Prof_InfoKey( key ) || !Q_stricmp( key, "name" ) ) {
			Info_SetValueForKey( p1Info, key, value );
		}
	}
	// R14a: and its field of view (a pad player's cg_fov is its profile's, in_gamepad.c Pad_ApplyFov)
	if ( Cvar_VariableString( "cg_fov" )[0] ) {
		Info_SetValueForKey( p1Info, "cg_fov", Cvar_VariableString( "cg_fov" ) );
	}
	p1Saved = qtrue;

	if ( CL_IndepChild() ) {
		return;		// Independent mode, a player's window: never writes q3config, nothing to protect
	}

	// the engine writes q3config whenever an archived cvar changes, i.e. with the
	// profile's name in it: keep P1's own values on disk too, so a crash can't lose them
	f = Prof_OpenWrite( PROF_P1KEEP, qfalse );
	if ( f != FS_INVALID_HANDLE ) {
		FS_Printf( f, "// player 1's q3config identity while it plays as a profile; restored at the next start if the game did not quit cleanly\n" );
		for ( s = p1Info; *s; ) {
			s = Info_NextPair( s, key, value );
			if ( !key[0] ) {
				break;
			}
			FS_Printf( f, "%s \"%s\"\n", key, value );
		}
		FS_FCloseFile( f );
	}
}


static qboolean Prof_P1KeepLine( int n, const char *word, const char *rest ) {
	char value[PROF_LINE_MAX];

	if ( !Prof_InfoKey( word ) && Q_stricmp( word, "name" ) && Q_stricmp( word, "cg_fov" ) ) {
		return qfalse;
	}
	Prof_Value( rest, value, sizeof( value ) );
	if ( strchr( value, '"' ) || strchr( value, ';' ) || strchr( value, '\\' ) ) {
		return qfalse;
	}
	Cvar_Set( word, value );
	return qtrue;
}


static void Prof_RestoreP1( void ) {
	char key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	const char *s = p1Info;

	if ( !p1Saved ) {
		return;
	}
	while ( *s ) {
		s = Info_NextPair( s, key, value );
		if ( !key[0] ) {
			break;
		}
		Cvar_Set( key, value );
	}
	p1Saved = qfalse;
	if ( !CL_IndepChild() ) {
		FS_Remove( Prof_OSPath( PROF_P1KEEP, qfalse ) );
	}
}


/*
=============================================================================

LOADING

=============================================================================
*/

static int		profLoadBinds;		// bind lines taken from the file being read
static int		profLoadToggles;	// R19: 'toggle' lines in it (written by R18 or later)
static char		profLoadName[PROF_KEY_LEN];

// shared file: name + feel
static qboolean Prof_FeelLine( int n, const char *word, const char *rest ) {
	char value[PROF_LINE_MAX];

	Prof_Value( rest, value, sizeof( value ) );
	if ( !Q_stricmp( word, "name" ) ) {
		CL_ProfileSanitize( value, profLoadName, sizeof( profLoadName ) );
		return qtrue;
	}
	if ( IN_PadFeelCvar( n, word ) ) {
		return IN_PadFeelSet( n, word, value );
	}
	return qfalse;	// unknown
}


// per-mod file: binds + player settings
static qboolean Prof_ModLine( int n, const char *word, const char *rest ) {
	char a[64], b[PROF_LINE_MAX];

	if ( !Q_stricmp( word, "bind" ) ) {
		const char *p = rest;
		Q_strncpyz( a, COM_ParseExt( &p, qfalse ), sizeof( a ) );
		while ( *p == ' ' ) {
			p++;
		}
		Prof_Value( p, b, sizeof( b ) );
		if ( !b[0] || strchr( b, '\n' ) ) {
			return qfalse;
		}
		if ( !profLoadBinds ) {
			IN_PadClearBinds( n );	// the file has a bind table: it replaces the defaults
		}
		if ( !IN_PadSetBindByName( n, a, b ) ) {
			return qfalse;
		}
		profLoadBinds++;
		return qtrue;
	}
	// R18: crouch / sprint hold-or-toggle, per game (after the bind table, which resets them)
	if ( !Q_stricmp( word, "toggle" ) ) {
		profLoadToggles++;
		if ( !Prof_TwoTokens( rest, a, sizeof( a ), b, sizeof( b ) ) ) {
			return qfalse;
		}
		return IN_PadSetToggleByName( n, a, b );
	}
	if ( !Q_stricmp( word, "userinfo" ) && n < MAX_SPLITVIEW ) {
		if ( !Prof_TwoTokens( rest, a, sizeof( a ), b, sizeof( b ) ) || !b[0] || !Prof_InfoKey( a )
			|| strpbrk( a, "\\;\"" ) || strpbrk( b, "\\;\"" ) ) {
			return qfalse;
		}
		Prof_SetInfo( n, a, b );
		return qtrue;
	}
	return qfalse;
}


/*
==================
Prof_ReadInto

Read profile 'key' into slot n (or the Guest defaults): the shared file's
feel over the Guest defaults (already in place), the per-mod file's binds
(else the Guest defaults' binds) and player settings.  A bad line or value
is skipped with one warning per file; nothing in a file can crash or
abort the load.
==================
*/
static void Prof_ReadInto( int n, const char *key, qboolean warnMissing ) {
	char *text;
	int bad;

	profLoadName[0] = '\0';
	text = Prof_ReadFile( Prof_OSPath( key, qfalse ) );
	if ( text ) {
		bad = Prof_Lines( n, text, Prof_FeelLine );
		if ( bad ) {
			Com_Printf( S_COLOR_YELLOW "profile: %s/%s.cfg: %i bad or unknown line(s) ignored (defaults kept)\n", PROF_DIR, key, bad );
		}
		Z_Free( text );
	} else if ( warnMissing ) {
		Com_Printf( S_COLOR_YELLOW "profile: %s/%s.cfg is missing; using the Guest defaults\n", PROF_DIR, key );
	}

	profLoadBinds = 0;
	profLoadToggles = 0;
	text = Prof_ReadFile( Prof_OSPath( key, qtrue ) );
	if ( text ) {
		bad = Prof_Lines( n, text, Prof_ModLine );
		if ( bad ) {
			Com_Printf( S_COLOR_YELLOW "profile: %s/%s/%s.cfg: %i bad or unknown line(s) ignored (defaults kept)\n",
				FS_GetCurrentGameDir(), PROF_DIR, key, bad );
		}
		Z_Free( text );
	}
	if ( !profLoadBinds && n != SPLIT_GUEST_DEFAULTS ) {
		IN_PadCopyBinds( n, SPLIT_GUEST_DEFAULTS );	// first use in this mod
	} else if ( profLoadBinds && !profLoadToggles ) {
		// R18: an untouched earlier built-in layout follows the built-in one (R19: only a file
		// from before R18 -- one with 'toggle' lines was saved by R18 or later, so its table,
		// even the R11 one, and its toggles are the player's own choice)
		const char *old = IN_PadUpgradeBinds( n );
		if ( old ) {
			Com_Printf( "profile: %s/%s/%s.cfg had the %s built-in %s pad layout; now the current one\n",
				FS_GetCurrentGameDir(), PROF_DIR, key, old, FS_GetCurrentGameDir() );
		}
	}
}


/*
=============================================================================

SAVING

=============================================================================
*/

static void Prof_WriteFeel( fileHandle_t f, int n ) {
	cvar_t *v;
	int i;

	for ( i = 0; i < IN_PadNumFeel(); i++ ) {
		v = IN_PadFeelCvar( n, IN_PadFeelName( i ) );
		if ( v ) {
			FS_Printf( f, "%s \"%s\"\n", IN_PadFeelName( i ), v->string );
		}
	}
}


// slot n's current settings as profile 'key' / 'name'
static qboolean Prof_Write( int n, const char *key, const char *name ) {
	char info[MAX_INFO_STRING], k[BIG_INFO_KEY], v[BIG_INFO_VALUE];
	const char *s;
	fileHandle_t f;

	f = Prof_OpenWrite( key, qfalse );
	if ( f == FS_INVALID_HANDLE ) {
		return qfalse;
	}
	FS_Printf( f, "// Quake3e-splitscreen player profile: name and controls (all mods). Written by the engine.\n" );
	FS_Printf( f, "name \"%s\"\n", name );
	Prof_WriteFeel( f, n );
	FS_FCloseFile( f );

	f = Prof_OpenWrite( key, qtrue );
	if ( f == FS_INVALID_HANDLE ) {
		return qfalse;
	}
	FS_Printf( f, "// Quake3e-splitscreen player profile \"%s\": %s pad buttons and player settings. Written by the engine.\n",
		name, FS_GetCurrentGameDir() );
	IN_PadWriteBinds( n, f );
	Prof_SlotInfo( n, info );
	for ( s = info; *s; ) {
		s = Info_NextPair( s, k, v );
		if ( !k[0] ) {
			break;
		}
		FS_Printf( f, "userinfo %s \"%s\"\n", k, v );
	}
	FS_FCloseFile( f );
	return qtrue;
}


static void Prof_SaveSlot( int n ) {
	profSlot_t *p = &profSlots[n];

	p->dirty = qfalse;
	if ( !p->active || p->guest ) {
		return;
	}
	Prof_SlotInfo( n, p->info );
	if ( Prof_Write( n, p->key, p->name ) ) {
		Com_Printf( "profile: P%i's settings saved to \"%s\"\n", n + 1, p->name );
	}
}


static void Prof_SaveGuest( void ) {
	fileHandle_t f;

	guestDirty = qfalse;
	if ( CL_IndepChild() ) {
		return;		// Independent mode: only player 1's window writes the Guest defaults
	}
	f = Prof_OpenWrite( PROF_GUEST, qfalse );
	if ( f == FS_INVALID_HANDLE ) {
		return;
	}
	FS_Printf( f, "// Quake3e-splitscreen Guest defaults: controls every Guest starts with (all mods). Written by the engine.\n" );
	Prof_WriteFeel( f, SPLIT_GUEST_DEFAULTS );
	FS_FCloseFile( f );

	f = Prof_OpenWrite( PROF_GUEST, qtrue );
	if ( f == FS_INVALID_HANDLE ) {
		return;
	}
	FS_Printf( f, "// Quake3e-splitscreen Guest defaults: %s pad buttons every Guest starts with. Written by the engine.\n",
		FS_GetCurrentGameDir() );
	IN_PadWriteBinds( SPLIT_GUEST_DEFAULTS, f );
	FS_FCloseFile( f );
	Com_Printf( "profile: Guest defaults saved\n" );
}


/*
=============================================================================

PAD -> LAST PROFILE

=============================================================================
*/

static void Prof_SavePadLast( void ) {
	fileHandle_t f;
	int i;

	if ( CL_IndepChild() ) {
		return;		// Independent mode: player 1's window is the only writer of _padlast.cfg
	}
	f = Prof_OpenWrite( PROF_PADLAST, qfalse );
	if ( f == FS_INVALID_HANDLE ) {
		return;
	}
	FS_Printf( f, "// Quake3e-splitscreen: the profile last used with each pad (by GUID). Written by the engine.\n" );
	for ( i = 0; i < numPadLast; i++ ) {
		FS_Printf( f, "pad \"%s\" \"%s\"\n", padLast[i].guid, padLast[i].key );
	}
	FS_FCloseFile( f );
}


static qboolean Prof_PadLastLine( int n, const char *word, const char *rest ) {
	char guid[64], key[64], name[PROF_KEY_LEN];

	if ( Q_stricmp( word, "pad" ) || !Prof_TwoTokens( rest, guid, sizeof( guid ), key, sizeof( key ) )
		|| strlen( guid ) >= sizeof( padLast[0].guid ) || !CL_ProfileSanitize( key, name, sizeof( name ) )
		|| numPadLast >= PROF_MAX_PADS ) {
		return qfalse;
	}
	Q_strncpyz( padLast[numPadLast].guid, guid, sizeof( padLast[0].guid ) );
	Prof_Key( name, padLast[numPadLast].key );
	numPadLast++;
	return qtrue;
}


// remember (key) or forget (NULL) the profile of a pad; written when it changed
static void Prof_SetPadLast( const char *guid, const char *key ) {
	int i;

	if ( !guid || !guid[0] ) {
		return;
	}
	for ( i = 0; i < numPadLast && Q_stricmp( padLast[i].guid, guid ); i++ )
		;
	if ( !key ) {
		if ( i == numPadLast ) {
			return;
		}
		padLast[i] = padLast[ --numPadLast ];
	} else {
		if ( i < numPadLast && !Q_stricmp( padLast[i].key, key ) ) {
			return;
		}
		if ( i == numPadLast ) {
			if ( numPadLast == PROF_MAX_PADS ) {
				memmove( padLast, padLast + 1, sizeof( padLast[0] ) * ( PROF_MAX_PADS - 1 ) );
				i = --numPadLast;
			}
			numPadLast++;
			Q_strncpyz( padLast[i].guid, guid, sizeof( padLast[i].guid ) );
		}
		Q_strncpyz( padLast[i].key, key, sizeof( padLast[i].key ) );
	}
	Prof_SavePadLast();
}


// every pad entry naming 'from' now names 'to' (NULL: forgotten)
static void Prof_RenamePadLast( const char *from, const char *to ) {
	qboolean changed = qfalse;
	int i;

	for ( i = 0; i < numPadLast; ) {
		if ( !Q_stricmp( padLast[i].key, from ) ) {
			changed = qtrue;
			if ( to ) {
				Q_strncpyz( padLast[i].key, to, sizeof( padLast[i].key ) );
			} else {
				padLast[i] = padLast[ --numPadLast ];
				continue;
			}
		}
		i++;
	}
	if ( changed ) {
		Prof_SavePadLast();
	}
}


/*
=============================================================================

THE PROFILE LIST

=============================================================================
*/

static int QDECL Prof_Compare( const void *a, const void *b ) {
	return Q_stricmp( ( (const profEntry_t *)a )->name, ( (const profEntry_t *)b )->name );
}


static qboolean Prof_NameLine( int n, const char *word, const char *rest ) {
	char value[PROF_LINE_MAX];

	if ( !Q_stricmp( word, "name" ) ) {
		Prof_Value( rest, value, sizeof( value ) );
		CL_ProfileSanitize( value, profLoadName, sizeof( profLoadName ) );
	}
	return qtrue;
}


void CL_ProfileRefresh( void ) {
	char **files, name[PROF_KEY_LEN], key[PROF_KEY_LEN], base[MAX_OSPATH], *text;
	int numFiles, i, len;

	profCount = 0;
	files = Sys_ListFiles( FS_BuildOSPath( Prof_HomePath(), PROF_DIR, NULL ), ".cfg", NULL, &numFiles, qfalse );
	for ( i = 0; files && i < numFiles && profCount < PROF_MAX; i++ ) {
		Q_strncpyz( base, files[i], sizeof( base ) );
		len = (int)strlen( base );
		if ( len > 4 ) {
			base[len - 4] = '\0';
		}
		if ( base[0] == '_' ) {
			continue;	// _guest, _padlast
		}
		// only files the engine could have written (a hand-made "Odd Name!.cfg" is not a profile)
		if ( !CL_ProfileSanitize( base, name, sizeof( name ) ) ) {
			continue;
		}
		Prof_Key( name, key );
		if ( strcmp( key, base ) ) {
			if ( !profInitialized ) {	// once, at start-up
				Com_Printf( S_COLOR_YELLOW "profile: %s/%s ignored: not a profile file name (expected %s.cfg)\n", PROF_DIR, files[i], key );
			}
			continue;
		}
		Q_strncpyz( profList[profCount].key, key, sizeof( profList[0].key ) );
		profLoadName[0] = '\0';
		text = Prof_ReadFile( Prof_OSPath( key, qfalse ) );
		if ( text ) {
			Prof_Lines( 0, text, Prof_NameLine );
			Z_Free( text );
		}
		if ( profLoadName[0] ) {
			char nameKey[PROF_KEY_LEN];
			Prof_Key( profLoadName, nameKey );
			if ( strcmp( nameKey, key ) ) {
				// the file name is the profile (load / delete go by it): a hand-edited name line that
				// maps to another key is ignored
				if ( !profInitialized ) {
					Com_Printf( S_COLOR_YELLOW "profile: %s/%s: name \"%s\" does not match the file name; listed as \"%s\"\n",
						PROF_DIR, files[i], profLoadName, name );
				}
				profLoadName[0] = '\0';
			}
		}
		Q_strncpyz( profList[profCount].name, profLoadName[0] ? profLoadName : name, sizeof( profList[0].name ) );
		profCount++;
	}
	if ( files ) {
		Sys_FreeFileList( files );
	}
	qsort( profList, profCount, sizeof( profList[0] ), Prof_Compare );
}


int CL_ProfileCount( void ) {
	return profCount;
}


const char *CL_ProfileListName( int i ) {
	return ( i >= 0 && i < profCount ) ? profList[i].name : "";
}


static int Prof_UserOfKey( const char *key ) {
	int n;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( profSlots[n].active && !profSlots[n].guest && !Q_stricmp( profSlots[n].key, key ) ) {
			return n;
		}
	}
	return CL_IndepProfileUser( key );	// Independent mode: played in another window
}


// Independent mode: profile keys across processes
const char *CL_ProfileSlotKey( int n ) {
	if ( !CL_ProfileActive( n ) ) {
		return "";
	}
	return profSlots[n].guest ? "guest" : profSlots[n].key;
}


void CL_ProfileKeyFor( const char *name, char *key, int size ) {
	char clean[PROF_KEY_LEN], k[PROF_KEY_LEN];

	if ( !name || !CL_ProfileSanitize( name, clean, sizeof( clean ) ) ) {
		Q_strncpyz( key, "", size );
		return;
	}
	Prof_Key( clean, k );
	Q_strncpyz( key, k, size );
}


int CL_ProfileListUser( int i ) {
	return ( i >= 0 && i < profCount ) ? Prof_UserOfKey( profList[i].key ) : -1;
}


static int Prof_IndexOfKey( const char *key ) {
	int i;

	for ( i = 0; i < profCount; i++ ) {
		if ( !Q_stricmp( profList[i].key, key ) ) {
			return i;
		}
	}
	return -1;
}


int CL_ProfileFind( const char *name ) {
	char clean[PROF_KEY_LEN], key[PROF_KEY_LEN];

	if ( !CL_ProfileSanitize( name, clean, sizeof( clean ) ) ) {
		return -1;
	}
	Prof_Key( clean, key );
	return Prof_IndexOfKey( key );
}


int CL_ProfilePadLast( const char *guid ) {
	int i;

	if ( !guid ) {
		return -1;
	}
	for ( i = 0; i < numPadLast; i++ ) {
		if ( !Q_stricmp( padLast[i].guid, guid ) ) {
			return Prof_IndexOfKey( padLast[i].key );
		}
	}
	return -1;
}


/*
=============================================================================

SLOTS

=============================================================================
*/

qboolean CL_ProfileActive( int n ) {
	return ( (unsigned)n < MAX_SPLITVIEW && profSlots[n].active ) ? qtrue : qfalse;
}


qboolean CL_ProfileIsGuest( int n ) {
	return ( CL_ProfileActive( n ) && profSlots[n].guest ) ? qtrue : qfalse;
}


const char *CL_ProfileName( int n ) {
	if ( !CL_ProfileActive( n ) ) {
		return "";
	}
	return profSlots[n].guest ? "Guest" : profSlots[n].name;
}


const char *CL_ProfileDescribe( int n ) {
	if ( n == SPLIT_GUEST_DEFAULTS ) {
		return "Guest defaults";
	}
	if ( !CL_ProfileActive( n ) ) {
		return n == 0 ? "none (q3config)" : "none";
	}
	return profSlots[n].guest ? "Guest" : va( "\"%s\"", profSlots[n].name );
}


// a slot stops using its profile (left, or player 1 went keyboard/mouse)
static void Prof_Release( int n ) {
	profSlot_t *p = &profSlots[n];

	if ( !p->active ) {
		return;
	}
	if ( p->dirty ) {
		Prof_SaveSlot( n );
	}
	if ( n == 0 ) {
		Prof_RestoreP1();
	}
	Com_Memset( p, 0, sizeof( *p ) );
}


/*
==================
CL_ProfileLoad

Slot n plays as profile 'name' (NULL / "guest": a Guest).  joining: the
slot is new (nothing to release; quiet).  Otherwise this is a live switch:
held buttons are released, binds and feel swapped, and the new name and
player settings reach the server through the normal userinfo update.
qfalse (nothing changed) if the profile does not exist or another player
is using it.
==================
*/
qboolean CL_ProfileLoad( int n, const char *name, qboolean joining ) {
	profSlot_t *p;
	const char *guid;
	int i = -1, user;

	if ( (unsigned)n >= MAX_SPLITVIEW || !profInitialized ) {
		return qfalse;
	}
	p = &profSlots[n];

	if ( name && name[0] && Q_stricmp( name, "guest" ) ) {
		i = CL_ProfileFind( name );
		if ( i < 0 ) {
			CL_ProfileRefresh();
			i = CL_ProfileFind( name );
		}
		if ( i < 0 ) {
			Com_Printf( "profile: no profile \"%s\" (profile_list)\n", name );
			return qfalse;
		}
		user = Prof_UserOfKey( profList[i].key );
		if ( user >= 0 && user != n ) {
			Com_Printf( "profile: \"%s\" is in use by P%i\n", profList[i].name, user + 1 );
			return qfalse;
		}
	}

	// the previous profile keeps what was changed
	if ( p->active && p->dirty ) {
		Prof_SaveSlot( n );
	}
	IN_PadReleasePlayer( n );

	// everything starts from the Guest defaults
	IN_PadCopyFeel( n, SPLIT_GUEST_DEFAULTS );

	if ( i < 0 ) {
		IN_PadCopyBinds( n, SPLIT_GUEST_DEFAULTS );
		if ( n == 0 ) {
			// R14a (maintainer): a Guest is "Player N" with default player settings for
			// player 1 too; q3config's own identity is only for keyboard/mouse (no
			// profile) and is put back at quit / kbm / the next start after a crash
			Prof_SaveP1();
		}
		Prof_ResetInfo( n, Prof_GuestName( n ) );
		Com_Memset( p, 0, sizeof( *p ) );
		p->active = qtrue;
		p->guest = qtrue;
	} else {
		if ( n == 0 ) {
			Prof_SaveP1();
		}
		Prof_ResetInfo( n, profList[i].name );
		Prof_ReadInto( n, profList[i].key, qtrue );
		Com_Memset( p, 0, sizeof( *p ) );
		p->active = qtrue;
		Q_strncpyz( p->key, profList[i].key, sizeof( p->key ) );
		Q_strncpyz( p->name, profList[i].name, sizeof( p->name ) );
		Prof_SlotInfo( n, p->info );
	}
	p->infoCheck = cls.realtime + PROF_INFO_POLL;

	// the pad remembers its last named profile (a guest visit does not erase it)
	guid = IN_PadGuid( n );
	if ( guid && !p->guest ) {
		Prof_SetPadLast( guid, p->key );
	}
	Com_Printf( "profile: P%i %s %s\n", n + 1, joining ? "joins as" : "is now", CL_ProfileDescribe( n ) );
	return qtrue;
}


// a pad became player 1 (cl_splitP1Input pad): its last profile, else Guest
void CL_ProfileP1Pad( const char *guid ) {
	int i;

	if ( profSlots[0].active ) {
		if ( guid && !profSlots[0].guest ) {
			Prof_SetPadLast( guid, profSlots[0].key );
		}
		return;
	}
	i = CL_ProfilePadLast( guid );
	if ( i >= 0 && CL_ProfileListUser( i ) < 0 && CL_ProfileLoad( 0, profList[i].name, qtrue ) ) {
		return;
	}
	CL_ProfileLoad( 0, NULL, qtrue );
}


void CL_ProfileChanged( int n ) {
	if ( n == SPLIT_GUEST_DEFAULTS ) {
		if ( !guestDirty ) {
			guestDirty = qtrue;
			guestDirtyTime = cls.realtime;
		}
		return;
	}
	if ( (unsigned)n >= MAX_SPLITVIEW || !profSlots[n].active || profSlots[n].guest ) {
		return;	// a guest's changes are not kept
	}
	if ( !profSlots[n].dirty ) {
		profSlots[n].dirty = qtrue;
		profSlots[n].dirtyTime = cls.realtime;
	}
}


/*
==================
CL_ProfileSaveAs

A new profile 'name' from slot n's current settings (a Guest keeping its
settings; "New profile" at the picker, where n holds the Guest defaults);
slot n then plays as it.
==================
*/
qboolean CL_ProfileSaveAs( int n, const char *name, char *err, int errSize ) {
	char clean[PROF_KEY_LEN], key[PROF_KEY_LEN];
	profSlot_t *p;

	if ( (unsigned)n >= MAX_SPLITVIEW || !profSlots[n].active ) {
		Q_strncpyz( err, "No profile in use", errSize );
		return qfalse;
	}
	if ( !CL_ProfileSanitize( name, clean, sizeof( clean ) ) ) {
		Q_strncpyz( err, "Type a name (letters, digits, - _)", errSize );
		return qfalse;
	}
	Prof_Key( clean, key );
	CL_ProfileRefresh();
	if ( Prof_IndexOfKey( key ) >= 0 ) {
		Com_sprintf( err, errSize, "\"%s\" exists: pick another name", profList[ Prof_IndexOfKey( key ) ].name );
		return qfalse;
	}
	p = &profSlots[n];
	if ( p->dirty ) {
		Prof_SaveSlot( n );	// the old one keeps its own changes
	}

	// write first: a failed write (read-only homepath) changes nothing in game
	if ( !Prof_Write( n, key, clean ) ) {
		Q_strncpyz( err, "Could not write the profile", errSize );
		return qfalse;
	}
	if ( n == 0 ) {
		Prof_SaveP1();
		Cvar_Set( "name", clean );
	} else {
		Cvar_Set( CL_SplitShadow( n, "name", NULL )->name, clean );
	}
	p->guest = qfalse;
	p->dirty = qfalse;
	Q_strncpyz( p->key, key, sizeof( p->key ) );
	Q_strncpyz( p->name, clean, sizeof( p->name ) );
	Prof_SlotInfo( n, p->info );
	CL_ProfileRefresh();
	Prof_SetPadLast( IN_PadGuid( n ), key );
	Com_Printf( "profile: P%i created profile \"%s\"%s\n", n + 1, clean, Q_stricmp( clean, name ) ? va( " (typed \"%s\")", name ) : "" );
	return qtrue;
}


// remove both files of a profile key
static void Prof_RemoveFiles( const char *key ) {
	FS_Remove( Prof_OSPath( key, qfalse ) );
	FS_HomeRemove( va( PROF_DIR "/%s.cfg", key ) );
}


static qboolean Prof_Rename( int n, const char *name, qboolean setName, char *err, int errSize );

qboolean CL_ProfileRename( int n, const char *name, char *err, int errSize ) {
	return Prof_Rename( n, name, qtrue, err, errSize );
}


// setName: the in-game name becomes the clean profile name (qfalse: it was
// changed in the mod's own menu and stays as typed, colours and all)
static qboolean Prof_Rename( int n, const char *name, qboolean setName, char *err, int errSize ) {
	char clean[PROF_KEY_LEN], key[PROF_KEY_LEN], oldKey[PROF_KEY_LEN];
	profSlot_t *p;

	if ( (unsigned)n >= MAX_SPLITVIEW || !profSlots[n].active || profSlots[n].guest ) {
		Q_strncpyz( err, "A Guest has no profile to rename", errSize );
		return qfalse;
	}
	if ( !CL_ProfileSanitize( name, clean, sizeof( clean ) ) ) {
		Q_strncpyz( err, "Type a name (letters, digits, - _)", errSize );
		return qfalse;
	}
	p = &profSlots[n];
	Prof_Key( clean, key );
	CL_ProfileRefresh();
	if ( Q_stricmp( key, p->key ) && Prof_IndexOfKey( key ) >= 0 ) {
		Com_sprintf( err, errSize, "\"%s\" exists: pick another name", profList[ Prof_IndexOfKey( key ) ].name );
		return qfalse;
	}
	Q_strncpyz( oldKey, p->key, sizeof( oldKey ) );
	if ( !Prof_Write( n, key, clean ) ) {
		Q_strncpyz( err, "Could not write the profile", errSize );
		return qfalse;
	}
	if ( !setName ) {
		// nothing to set
	} else if ( n == 0 ) {
		Cvar_Set( "name", clean );
	} else {
		Cvar_Set( CL_SplitShadow( n, "name", NULL )->name, clean );
	}
	if ( Q_stricmp( oldKey, key ) ) {
		Prof_RemoveFiles( oldKey );
		Prof_RenamePadLast( oldKey, key );
	}
	Com_Printf( "profile: P%i renamed \"%s\" to \"%s\"\n", n + 1, p->name, clean );
	Q_strncpyz( p->key, key, sizeof( p->key ) );
	Q_strncpyz( p->name, clean, sizeof( p->name ) );
	p->dirty = qfalse;
	Prof_SlotInfo( n, p->info );
	CL_ProfileRefresh();
	return qtrue;
}


qboolean CL_ProfileDelete( int i, int bySlot, char *err, int errSize ) {
	char key[PROF_KEY_LEN], name[PROF_KEY_LEN];
	int user;

	if ( i < 0 || i >= profCount ) {
		Q_strncpyz( err, "No such profile", errSize );
		return qfalse;
	}
	user = Prof_UserOfKey( profList[i].key );
	if ( user >= 0 && user != bySlot ) {
		Com_sprintf( err, errSize, "\"%s\" is in use by player %i", profList[i].name, user + 1 );
		return qfalse;
	}
	Q_strncpyz( key, profList[i].key, sizeof( key ) );
	Q_strncpyz( name, profList[i].name, sizeof( name ) );
	if ( user >= 0 ) {
		profSlots[user].dirty = qfalse;
		CL_ProfileLoad( user, NULL, qfalse );	// the player goes on as a Guest
	}
	Prof_RemoveFiles( key );
	Prof_RenamePadLast( key, NULL );
	CL_ProfileRefresh();
	Com_Printf( "profile: deleted \"%s\"\n", name );
	return qtrue;
}


/*
=============================================================================

FRAME, INIT, COMMANDS

=============================================================================
*/

/*
==================
Prof_CheckGameName

A named player's in-game name changed in the mod's own menu: the profile
takes it over (renamed, files and all) if it sanitises to a free profile
name; the in-game name stays as typed.
==================
*/
static void Prof_CheckGameName( int n ) {
	profSlot_t *p = &profSlots[n];
	char gameName[MAX_CVAR_VALUE_STRING], clean[PROF_KEY_LEN], err[96];

	if ( n == 0 ) {
		Cvar_VariableStringBuffer( "name", gameName, sizeof( gameName ) );
	} else {
		Q_strncpyz( gameName, CL_SplitShadow( n, "name", NULL )->string, sizeof( gameName ) );
	}
	if ( !strcmp( gameName, p->gameName ) ) {
		return;
	}
	Q_strncpyz( p->gameName, gameName, sizeof( p->gameName ) );
	if ( !CL_ProfileSanitize( gameName, clean, sizeof( clean ) ) || !strcmp( clean, p->name ) ) {
		return;
	}
	if ( !Prof_Rename( n, clean, qfalse, err, sizeof( err ) ) ) {
		Com_Printf( "profile: P%i's new in-game name \"%s^7\" is not taken over by profile \"%s\": %s\n", n + 1, gameName, p->name, err );
	}
}


/*
==================
Prof_CheckGameDir

The mod changed (game_restart / fs_game from a server): pad binds, player
settings and the Guest defaults' binds are per mod -- re-read them.
==================
*/
static void Prof_LoadGuestDefaults( void );

static void Prof_CheckGameDir( void ) {
	profSlot_t *p;
	int n;

	if ( !Q_stricmp( profGameDir, FS_GetCurrentGameDir() ) ) {
		return;
	}
	Com_Printf( "profile: mod changed (%s -> %s): re-reading profiles\n", profGameDir, FS_GetCurrentGameDir() );
	Q_strncpyz( profGameDir, FS_GetCurrentGameDir(), sizeof( profGameDir ) );
	guestDirty = qfalse;		// unsaved Guest-default changes belonged to the old mod
	CL_ProfileRefresh();
	Prof_LoadGuestDefaults();
	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		p = &profSlots[n];
		if ( !p->active ) {
			continue;
		}
		IN_PadReleasePlayer( n );
		// a game restart reset every cvar (Cvar_Restart) and ran the new mod's q3config
		IN_PadCopyFeel( n, SPLIT_GUEST_DEFAULTS );
		if ( p->guest ) {
			IN_PadCopyBinds( n, SPLIT_GUEST_DEFAULTS );
			if ( n == 0 ) {
				p1Saved = qfalse;	// player 1's own identity is now the new mod's q3config one
				Prof_SaveP1();
				Prof_ResetInfo( 0, Prof_GuestName( 0 ) );
			}
			continue;
		}
		p->dirty = qfalse;		// (saved within PROF_SAVE_DELAY of the change; the rest belonged to the old mod)
		if ( n == 0 ) {
			p1Saved = qfalse;	// player 1's own identity is now the new mod's q3config one
			Prof_SaveP1();
		}
		Prof_ResetInfo( n, p->name );
		Prof_ReadInto( n, p->key, qfalse );
		Prof_SlotInfo( n, p->info );
		Com_Printf( "profile: P%i's %s binds and player settings loaded for %s\n", n + 1, p->name, profGameDir );
	}
}


void CL_ProfileFrame( void ) {
	profSlot_t *p;
	char info[MAX_INFO_STRING];
	int n;

	if ( !profInitialized ) {
		return;
	}

	Prof_CheckGameDir();

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		p = &profSlots[n];
		if ( !p->active ) {
			continue;
		}
		// left the game / player 1 became the keyboard/mouse player
		if ( ( n > 0 && !CL_SplitSlotActive( n ) && !CL_IndepPicking( n ) )
			|| ( n == 0 && !Q_stricmp( cl_splitP1Input->string, "kbm" ) && !IN_PadGuid( 0 ) ) ) {
			Prof_Release( n );
			continue;
		}
		if ( p->guest ) {
			continue;
		}
		// player settings changed in the mod's menu (model, colours ...)
		if ( cls.realtime >= p->infoCheck ) {
			p->infoCheck = cls.realtime + PROF_INFO_POLL;
			Prof_SlotInfo( n, info );
			if ( strcmp( info, p->info ) ) {
				Q_strncpyz( p->info, info, sizeof( p->info ) );
				CL_ProfileChanged( n );
			}
			Prof_CheckGameName( n );
		}
		if ( p->dirty && cls.realtime - p->dirtyTime >= PROF_SAVE_DELAY ) {
			Prof_SaveSlot( n );
		}
	}

	if ( guestDirty && cls.realtime - guestDirtyTime >= PROF_SAVE_DELAY ) {
		Prof_SaveGuest();
	}
}


static void Prof_LoadGuestDefaults( void ) {
	char *text;
	const char *legacy;

	IN_PadCopyFeel( SPLIT_GUEST_DEFAULTS, -1 );
	IN_PadCopyBinds( SPLIT_GUEST_DEFAULTS, -1 );
	Prof_ReadInto( SPLIT_GUEST_DEFAULTS, PROF_GUEST, qfalse );

	// R7's interim per-player-number store: player 1's settings become the
	// Guest defaults if nobody has profiles or Guest defaults yet
	legacy = FS_BuildOSPath( Prof_HomePath(), FS_GetCurrentGameDir(), "splitpads.cfg" );
	text = ( Sys_SplitLaunchFlags() & SPLIT_FLAG_CHILD ) ? NULL : Prof_ReadFile( legacy );	// (not in a child window)
	if ( !text ) {
		return;
	}
	if ( profCount == 0 && !Prof_Exists( Prof_OSPath( PROF_GUEST, qfalse ) ) ) {
		if ( IN_PadMigrateLegacy( text ) ) {
			Prof_SaveGuest();
			Com_Printf( "profile: splitpads.cfg (R7) imported: player 1's controls and buttons are now the Guest defaults; file removed\n" );
		} else {
			Com_Printf( "profile: splitpads.cfg (R7) had nothing for player 1; file removed\n" );
		}
		FS_HomeRemove( "splitpads.cfg" );
	} else {
		Com_Printf( "profile: splitpads.cfg (R7) ignored: profiles or Guest defaults already exist (delete it by hand)\n" );
	}
	Z_Free( text );
}


static int Prof_SlotArg( int arg ) {
	const int n = atoi( Cmd_Argv( arg ) ) - 1;

	if ( (unsigned)n >= MAX_SPLITVIEW ) {
		Com_Printf( "player number must be 1..%i\n", MAX_SPLITVIEW );
		return -1;
	}
	if ( n == 0 && !Q_stricmp( cl_splitP1Input->string, "kbm" ) ) {
		Com_Printf( "player 1 is the keyboard/mouse player (cl_splitP1Input kbm): it uses q3config, no profile\n" );
		return -1;
	}
	if ( n > 0 && !CL_SplitSlotActive( n ) ) {
		Com_Printf( "no local player %i\n", n + 1 );
		return -1;
	}
	return n;
}


static void Prof_List_f( void ) {
	int i, n;

	CL_ProfileRefresh();
	Com_Printf( "%i profile(s) in %s:\n", profCount, FS_BuildOSPath( Prof_HomePath(), PROF_DIR, NULL ) );
	for ( i = 0; i < profCount; i++ ) {
		n = CL_ProfileListUser( i );
		Com_Printf( "  %-20s  file %s.cfg%s\n", profList[i].name, profList[i].key, n >= 0 ? va( "  (in use by P%i)", n + 1 ) : "" );
	}
	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( profSlots[n].active ) {
			Com_Printf( "P%i: %s%s\n", n + 1, CL_ProfileDescribe( n ), profSlots[n].dirty ? " (unsaved changes)" : "" );
		}
	}
}


static void Prof_Load_f( void ) {
	int n;

	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "usage: profile_load <player> <name|guest>\n" );
		return;
	}
	if ( ( n = Prof_SlotArg( 1 ) ) < 0 ) {
		return;
	}
	CL_ProfileLoad( n, Cmd_ArgsFrom( 2 ), qfalse );
}


static void Prof_Save_f( void ) {
	char err[128];
	int n;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: profile_save <player> [new name]\n" );
		return;
	}
	if ( ( n = Prof_SlotArg( 1 ) ) < 0 ) {
		return;
	}
	if ( Cmd_Argc() > 2 ) {
		if ( !CL_ProfileSaveAs( n, Cmd_ArgsFrom( 2 ), err, sizeof( err ) ) ) {
			Com_Printf( "profile_save: %s\n", err );
		}
		return;
	}
	if ( !profSlots[n].active || profSlots[n].guest ) {
		Com_Printf( "profile_save: P%i is a Guest; give a name to keep its settings\n", n + 1 );
		return;
	}
	Prof_SaveSlot( n );
}


static void Prof_Rename_f( void ) {
	char err[128];
	int n;

	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "usage: profile_rename <player> <new name>\n" );
		return;
	}
	if ( ( n = Prof_SlotArg( 1 ) ) >= 0 && !CL_ProfileRename( n, Cmd_ArgsFrom( 2 ), err, sizeof( err ) ) ) {
		Com_Printf( "profile_rename: %s\n", err );
	}
}


static void Prof_Delete_f( void ) {
	char err[128];
	int i;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: profile_delete <name>\n" );
		return;
	}
	CL_ProfileRefresh();
	i = CL_ProfileFind( Cmd_ArgsFrom( 1 ) );
	if ( !CL_ProfileDelete( i, -1, err, sizeof( err ) ) ) {
		Com_Printf( "profile_delete: %s\n", err );
	}
}


void CL_ProfileInit( void ) {
	char *text;

	Com_Memset( profSlots, 0, sizeof( profSlots ) );
	numPadLast = 0;
	guestDirty = qfalse;
	p1Saved = qfalse;
	Q_strncpyz( profGameDir, FS_GetCurrentGameDir(), sizeof( profGameDir ) );
	cl_splitP1Input = Cvar_Get( "cl_splitP1Input", "pad", CVAR_ARCHIVE_ND );

	// the last session ended while player 1 played as a profile (crash, killed):
	// q3config may hold that profile's name/model -- put player 1's own back
	// (Independent mode: player 1's window's file, never a child's business)
	text = ( Sys_SplitLaunchFlags() & SPLIT_FLAG_CHILD ) ? NULL : Prof_ReadFile( Prof_OSPath( PROF_P1KEEP, qfalse ) );
	if ( text ) {
		Prof_Lines( 0, text, Prof_P1KeepLine );
		Z_Free( text );
		FS_Remove( Prof_OSPath( PROF_P1KEEP, qfalse ) );
		Com_Printf( "profile: player 1's own name/model restored (the last session did not end cleanly)\n" );
	}

	CL_ProfileRefresh();
	Prof_LoadGuestDefaults();
	text = Prof_ReadFile( Prof_OSPath( PROF_PADLAST, qfalse ) );
	if ( text ) {
		if ( Prof_Lines( 0, text, Prof_PadLastLine ) ) {
			Com_Printf( S_COLOR_YELLOW "profile: %s/%s.cfg: bad line(s) ignored\n", PROF_DIR, PROF_PADLAST );
		}
		Z_Free( text );
	}
	if ( profCount ) {
		Com_Printf( "profile: %i profile(s) found\n", profCount );
	}

	Cmd_AddCommand( "profile_list", Prof_List_f );
	Cmd_AddCommand( "profile_load", Prof_Load_f );
	Cmd_AddCommand( "profile_save", Prof_Save_f );
	Cmd_AddCommand( "profile_rename", Prof_Rename_f );
	Cmd_AddCommand( "profile_delete", Prof_Delete_f );
	profInitialized = qtrue;
}


void CL_ProfileShutdown( void ) {
	int n;

	if ( !profInitialized ) {
		return;
	}
	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( profSlots[n].active && !profSlots[n].guest ) {
			char info[MAX_INFO_STRING];
			Prof_SlotInfo( n, info );
			if ( profSlots[n].dirty || strcmp( info, profSlots[n].info ) ) {
				Prof_SaveSlot( n );
			}
		}
	}
	if ( guestDirty ) {
		Prof_SaveGuest();
	}
	if ( p1Saved ) {
		Prof_RestoreP1();
		Com_WriteConfiguration();	// q3config keeps player 1's own name and model
	}
	Com_Memset( profSlots, 0, sizeof( profSlots ) );

	Cmd_RemoveCommand( "profile_list" );
	Cmd_RemoveCommand( "profile_load" );
	Cmd_RemoveCommand( "profile_save" );
	Cmd_RemoveCommand( "profile_rename" );
	Cmd_RemoveCommand( "profile_delete" );
	profInitialized = qfalse;
}
