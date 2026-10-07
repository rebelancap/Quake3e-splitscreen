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
// sv_splitrules.c -- splitscreen "Server options" levers that need no mod changes
// (design doc 18): the map's entity string rewritten before the game VM reads it
// ([entities]: weapons, instagib), each client's shared playerState written after
// the game frame ([ps]: spawn weapons, infinite ammo, self-damage off, instagib
// health), god mode as a client command with cheats forced on the local server,
// quad damage pickups removed (R16, [entities]), every power-up / holdable
// removed (R17, [entities]),
// and the bot count kept.
//
// Inert unless the local client has set sv_splitRules 1 (it sets it while it runs
// a listen server; the cvars below only exist in a client process) and never on a
// dedicated server (com_dedicated).  [entities]/[ps] work only for games with
// Quake 3's playerState / entity layout (baseq3, missionpack, mods whose maps use
// baseq3 weapon_* classnames); Urban Terror (q3ut4) and unknown games get none.
//
// Layout facts verified against ioquake3 code/game/bg_public.h, g_client.c,
// g_combat.c, g_active.c, g_cmds.c (2026-10-06):
//   stats[]: STAT_HEALTH 0, STAT_HOLDABLE_ITEM 1, (missionpack: STAT_PERSISTANT_POWERUP 2,)
//            STAT_WEAPONS 2 (missionpack 3), STAT_ARMOR 3 (4), ..., STAT_MAX_HEALTH 6 (7)
//   persistant[]: PERS_SPAWN_COUNT 4, PERS_ATTACKER 6 (G_Damage: attacker->s.number,
//            ENTITYNUM_WORLD without an attacker; set on every hit)
//   weapons: WP_GAUNTLET 1 .. WP_BFG 9, WP_GRAPPLING_HOOK 10, missionpack 11..13
//   ClientSpawn: ent->flags = 0 (FL_GODMODE cleared), ent->health = STAT_HEALTH =
//            STAT_MAX_HEALTH + 25; ClientEndFrame copies ent->health to STAT_HEALTH.
//   Cmd_God_f toggles FL_GODMODE (CheatsOk: sv_cheats and alive).

#include "server.h"

#define SR_LAYOUT_NONE		0
#define SR_LAYOUT_Q3		1
#define SR_LAYOUT_MP		2	// missionpack: one more stat before STAT_WEAPONS

#define SR_PERS_SPAWN_COUNT	4
#define SR_PERS_ATTACKER	6
#define SR_WP_GAUNTLET		1
#define SR_WP_MACHINEGUN	2
#define SR_WP_RAILGUN		7
#define SR_WP_GRAPPLE		10
#define SR_AMMO_INFINITE	999
#define SR_PM_NORMAL		0
#define SR_WEAPON_READY		0

typedef struct {
	const char	*name;		// sv_splitWeapons value
	const char	*classname;
	const char	*ammo;		// its ammo box, NULL = none
	int			weapon;
	int			spawnAmmo;	// given at spawn
	qboolean	mpOnly;
} srWeapon_t;

static const srWeapon_t srWeapons[] = {
	{ "gauntlet",	"weapon_gauntlet",			NULL,				1, -1, qfalse },
	{ "machinegun",	"weapon_machinegun",		"ammo_bullets",		2, 100, qfalse },
	{ "shotgun",	"weapon_shotgun",			"ammo_shells",		3, 25, qfalse },
	{ "grenade",	"weapon_grenadelauncher",	"ammo_grenades",	4, 25, qfalse },
	{ "rocket",		"weapon_rocketlauncher",	"ammo_rockets",		5, 25, qfalse },
	{ "lightning",	"weapon_lightning",			"ammo_lightning",	6, 150, qfalse },
	{ "railgun",	"weapon_railgun",			"ammo_slugs",		7, 25, qfalse },
	{ "plasma",		"weapon_plasmagun",			"ammo_cells",		8, 100, qfalse },
	{ "bfg",		"weapon_bfg",				"ammo_bfg",			9, 25, qfalse },
	{ "nailgun",	"weapon_nailgun",			"ammo_nails",		11, 50, qtrue },
	{ "prox",		"weapon_prox_launcher",		"ammo_mines",		12, 10, qtrue },
	{ "chaingun",	"weapon_chaingun",			"ammo_belt",		13, 200, qtrue },
};
#define SR_NUM_WEAPONS	( (int)ARRAY_LEN( srWeapons ) )

// pickups instagib removes (CTF flags, Harvester cubes, holdables but the medkit stay)
static const char *srInstagibRemove[] = {
	"item_armor_", "item_health", "item_quad", "item_enviro", "item_haste", "item_invis",
	"item_regen", "item_flight", "item_scout", "item_guard", "item_doubler", "item_ammoregen",
	"holdable_medkit", "ammo_", NULL
};

// R17 "Power-ups Off": every power-up (baseq3 + missionpack) and every holdable item
static const char *srPowerups[] = {
	"item_quad", "item_haste", "item_invis", "item_regen", "item_enviro", "item_flight",
	"item_scout", "item_guard", "item_doubler", "item_ammoregen", "holdable_", NULL
};

static qboolean SR_IsPowerup( const char *c ) {
	int i;

	for ( i = 0; srPowerups[i]; i++ ) {
		if ( !Q_stricmpn( c, srPowerups[i], (int)strlen( srPowerups[i] ) ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

typedef struct {
	qboolean	seen;
	int			spawnCount;
	int			health, armor;		// after our last frame
	int			godSpawn;			// spawn count when we turned god on, -1 = off
} srClient_t;

static struct {
	// settings of this frame (from the client's sv_split* cvars)
	qboolean	active;
	qboolean	instagib;
	int			weapons;			// -2 default, -1 random, else srWeapons index
	qboolean	noQuad;				// R16: item_quad removed
	qboolean	noPowerups;			// R17: every power-up and holdable removed
	int			spawnMode;			// 0 machinegun (game default), 1 all, 2 gauntlet only
	qboolean	infiniteAmmo;
	qboolean	selfDamageOff;
	int			godMask;			// server client numbers; -1 = every human
	int			bots;				// 0 = not managed
	int			botSkill;			// 1..5, 0 = random per bot

	int			layout;				// of the running level
	char		*entities;			// rewritten entity string (Z_Malloc), NULL = the map's own
	const char	*entityStart;		// what GAME_INIT starts parsing (R16: redone for a game's own instagib)
	char		summary[256];		// what the game got
	int			seed;
	int			initTime;			// sv.time at the last game init
	int			botTime;			// sv.time of the last add/kick
	int			botsAtAdd;		// bot count when we last sent addbot, -1 = none pending
	int			frameTime;		// sv.time of the last game frame we looked at
	qboolean	cheatsForced;		// we set sv_cheats 1
	int			cheatsTime;			// sv.time it was forced (god waits a frame for the game to see it)
	qboolean	maxNoted;
	qboolean	botEnableNoted;

	// health field of the game's gentity (game-private: found by watching STAT_HEALTH)
	int			healthOfs;			// bytes, 0 = not found yet
	byte		cand[1024];			// candidate offsets / 4
	int			numCand;
	int			distinct[3];
	int			numDistinct;

	srClient_t	cl[MAX_CLIENTS];
} sr;


static void SR_ModInstagibClear( void );


static int SR_Stat( int stat ) {
	// stat: 2 = STAT_WEAPONS, 3 = STAT_ARMOR, 6 = STAT_MAX_HEALTH in baseq3 numbering
	return ( sr.layout == SR_LAYOUT_MP && stat >= 2 ) ? stat + 1 : stat;
}
#define STAT_W	SR_Stat( 2 )
#define STAT_A	SR_Stat( 3 )
#define STAT_MH	SR_Stat( 6 )

static int SR_MaxWeapon( void ) {
	return sr.layout == SR_LAYOUT_MP ? 13 : 9;
}


static int SR_WeaponByName( const char *name ) {
	int i;

	if ( !name[0] || !Q_stricmp( name, "default" ) || !Q_stricmp( name, "0" ) ) {
		return -2;
	}
	if ( !Q_stricmp( name, "random" ) ) {
		return -1;
	}
	for ( i = 0; i < SR_NUM_WEAPONS; i++ ) {
		if ( !Q_stricmp( name, srWeapons[i].name ) ) {
			return i;
		}
	}
	return -2;
}


// the client's settings (none of these cvars exist in a dedicated server)
static void SR_ReadSettings( void ) {
	const char *s;

	sr.active = ( !com_dedicated->integer && Cvar_VariableIntegerValue( "sv_splitRules" ) ) ? qtrue : qfalse;
	if ( !sr.active ) {
		sr.instagib = sr.infiniteAmmo = sr.selfDamageOff = sr.noQuad = sr.noPowerups = qfalse;
		sr.weapons = -2;
		sr.spawnMode = sr.godMask = sr.bots = sr.botSkill = 0;
		return;
	}
	// R16: never our preset in a game with its own instagib (the client says so too, a frame later)
	sr.instagib = ( Cvar_VariableIntegerValue( "sv_splitInstagib" ) && !SV_SplitModInstagib()[0] ) ? qtrue : qfalse;
	sr.weapons = SR_WeaponByName( Cvar_VariableString( "sv_splitWeapons" ) );
	s = Cvar_VariableString( "sv_splitQuad" );
	sr.noQuad = ( s[0] && !atoi( s ) ) ? qtrue : qfalse;
	s = Cvar_VariableString( "sv_splitPowerups" );
	sr.noPowerups = ( s[0] && !atoi( s ) ) ? qtrue : qfalse;
	s = Cvar_VariableString( "sv_splitSpawnWeapons" );
	sr.spawnMode = !Q_stricmp( s, "all" ) ? 1 : !Q_stricmp( s, "gauntlet" ) ? 2 : 0;
	sr.infiniteAmmo = Cvar_VariableIntegerValue( "sv_splitInfiniteAmmo" ) ? qtrue : qfalse;
	s = Cvar_VariableString( "sv_splitSelfDamage" );
	sr.selfDamageOff = ( s[0] && !atoi( s ) ) ? qtrue : qfalse;
	sr.godMask = Cvar_VariableIntegerValue( "sv_splitGod" );
	sr.bots = Cvar_VariableIntegerValue( "sv_splitBots" );
	sr.botSkill = Cvar_VariableIntegerValue( "sv_splitBotSkill" );
	if ( sr.weapons >= 0 && srWeapons[ sr.weapons ].mpOnly && sr.layout != SR_LAYOUT_MP ) {
		sr.weapons = -2;
	}
}


/*
=============================================================================

[entities]: the map's entity string, rewritten before GAME_INIT parses it

=============================================================================
*/

static qboolean SR_IsWeaponClass( const char *cls ) {
	return ( !Q_stricmpn( cls, "weapon_", 7 ) && Q_stricmp( cls, "weapon_grapplinghook" ) ) ? qtrue : qfalse;
}


// the game's layout, from the game directory and the map
static int SR_DetectLayout( const char *ents ) {
	const char *dir = FS_GetCurrentGameDir();
	int i;

	if ( !Q_stricmp( dir, "q3ut4" ) || Q_stristr( Cvar_VariableString( "fs_basegame" ), "q3ut4" ) ) {
		return SR_LAYOUT_NONE;	// Urban Terror: gear system, other playerState use
	}
	if ( !Q_stricmp( dir, "missionpack" ) ) {
		return SR_LAYOUT_MP;
	}
	if ( !Q_stricmp( dir, BASEGAME ) ) {
		return SR_LAYOUT_Q3;
	}
	// an unknown mod: only when the map uses Quake 3's weapon classnames
	for ( i = 0; ents && i < SR_NUM_WEAPONS; i++ ) {
		if ( !srWeapons[i].mpOnly && Q_stristr( ents, va( "\"%s\"", srWeapons[i].classname ) ) ) {
			return SR_LAYOUT_Q3;
		}
	}
	return SR_LAYOUT_NONE;
}


typedef struct {
	char	*buf;
	int		len, size;
} srBuf_t;

static void SR_Append( srBuf_t *b, const char *s ) {
	const int n = (int)strlen( s );
	char *nb;

	if ( b->len + n + 1 > b->size ) {
		b->size = ( b->len + n + 1 ) * 2;
		nb = Z_Malloc( b->size );
		if ( b->buf ) {
			Com_Memcpy( nb, b->buf, b->len + 1 );
			Z_Free( b->buf );
		}
		b->buf = nb;
	}
	Com_Memcpy( b->buf + b->len, s, n + 1 );
	b->len += n;
}


static int SR_RandomWeapon( void ) {
	int pool[ SR_NUM_WEAPONS ], n = 0, i;

	for ( i = 1; i < SR_NUM_WEAPONS; i++ ) {	// no gauntlet pickups
		if ( !srWeapons[i].mpOnly || sr.layout == SR_LAYOUT_MP ) {
			pool[ n++ ] = i;
		}
	}
	return pool[ (unsigned)Q_rand( &sr.seed ) % (unsigned)n ];	// Q_rand can be negative
}


#define SR_MAX_PAIRS	64

/*
==================
SR_Rewrite

A copy of 'src' with the weapon / ammo / pickup classnames changed for the
current settings, or NULL when nothing changes (or the string is not the
plain "{ key value ... }" form).
==================
*/
static char *SR_Rewrite( const char *src ) {
	static char keys[SR_MAX_PAIRS][MAX_TOKEN_CHARS / 4], vals[SR_MAX_PAIRS][MAX_TOKEN_CHARS];
	srBuf_t out = { NULL, 0, 0 };
	const char *p = src, *tok;
	int numPairs, i, cls, w, nWeap = 0, nAmmo = 0, nRemoved = 0, nEnts = 0;
	char line[ MAX_TOKEN_CHARS * 2 ];
	qboolean drop;

	SR_Append( &out, "" );
	while ( 1 ) {
		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		if ( strcmp( tok, "{" ) ) {
			goto fail;
		}
		numPairs = 0;
		cls = -1;
		while ( 1 ) {
			tok = COM_Parse( &p );
			if ( !tok[0] ) {
				goto fail;
			}
			if ( !strcmp( tok, "}" ) ) {
				break;
			}
			if ( numPairs >= SR_MAX_PAIRS ) {
				goto fail;
			}
			Q_strncpyz( keys[ numPairs ], tok, sizeof( keys[0] ) );
			tok = COM_Parse( &p );
			if ( !strcmp( tok, "}" ) ) {
				goto fail;
			}
			Q_strncpyz( vals[ numPairs ], tok, sizeof( vals[0] ) );
			if ( !Q_stricmp( keys[ numPairs ], "classname" ) ) {
				cls = numPairs;
			}
			numPairs++;
		}
		nEnts++;

		drop = qfalse;
		if ( cls >= 0 ) {
			char *c = vals[ cls ];
			if ( sr.noPowerups && SR_IsPowerup( c ) ) {
				drop = qtrue;	// R17: Power-ups off (any weapons mode, instagib too)
			} else if ( sr.instagib ) {
				for ( i = 0; srInstagibRemove[i]; i++ ) {
					if ( !Q_stricmpn( c, srInstagibRemove[i], (int)strlen( srInstagibRemove[i] ) ) ) {
						drop = qtrue;
					}
				}
				if ( SR_IsWeaponClass( c ) ) {
					Q_strncpyz( c, "weapon_railgun", sizeof( vals[0] ) );
				}
			} else if ( sr.noQuad && !Q_stricmp( c, "item_quad" ) ) {
				drop = qtrue;	// R16: Quad damage off
			} else if ( sr.weapons == -2 ) {
				;				// default weapons (only the quad changes)
			} else if ( SR_IsWeaponClass( c ) ) {
				w = ( sr.weapons == -1 ) ? SR_RandomWeapon() : sr.weapons;
				Q_strncpyz( c, srWeapons[w].classname, sizeof( vals[0] ) );
			} else if ( !Q_stricmpn( c, "ammo_", 5 ) ) {
				w = ( sr.weapons == -1 ) ? SR_RandomWeapon() : sr.weapons;
				if ( srWeapons[w].ammo ) {
					Q_strncpyz( c, srWeapons[w].ammo, sizeof( vals[0] ) );
				} else {
					drop = qtrue;	// gauntlet only: no ammo boxes
				}
			}
			if ( drop ) {
				nRemoved++;
				continue;
			}
			if ( SR_IsWeaponClass( c ) ) {
				nWeap++;
			} else if ( !Q_stricmpn( c, "ammo_", 5 ) ) {
				nAmmo++;
			}
		}

		SR_Append( &out, "{\n" );
		for ( i = 0; i < numPairs; i++ ) {
			Com_sprintf( line, sizeof( line ), "\"%s\" \"%s\"\n", keys[i], vals[i] );
			SR_Append( &out, line );
		}
		SR_Append( &out, "}\n" );
	}
	Com_sprintf( sr.summary, sizeof( sr.summary ), "%s%s: %i entities, %i weapon pickups, %i ammo, %i removed",
		sr.instagib ? "instagib" : sr.weapons == -1 ? "random weapons" : sr.weapons == -2 ? "default weapons" : va( "%s only", srWeapons[ sr.weapons ].name ),
		sr.noPowerups ? ", no power-ups" : ( sr.noQuad && !sr.instagib ) ? ", no quad" : "",
		nEnts, nWeap, nAmmo, nRemoved );
	return out.buf;

fail:
	Com_Printf( S_COLOR_YELLOW "splitrules: the map's entity string is not in the usual form; left as it is\n" );
	if ( out.buf ) {
		Z_Free( out.buf );
	}
	return NULL;
}


// classname counts of an entity string (srvdebug ents, and the line after every load)
static void SR_CountClasses( const char *ents, char *buf, int size ) {
	const char *p = ents, *tok;
	char names[32][48];
	int counts[32], num = 0, i;
	qboolean isClass = qfalse;

	buf[0] = '\0';
	while ( p ) {
		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		if ( isClass ) {
			isClass = qfalse;
			if ( SR_IsWeaponClass( tok ) || !Q_stricmpn( tok, "ammo_", 5 ) || !Q_stricmpn( tok, "item_", 5 ) || !Q_stricmpn( tok, "holdable_", 9 ) ) {
				for ( i = 0; i < num && Q_stricmp( names[i], tok ); i++ )
					;
				if ( i == num && num < 32 ) {
					Q_strncpyz( names[ num ], tok, sizeof( names[0] ) );
					counts[ num++ ] = 0;
				}
				if ( i < num ) {
					counts[i]++;
				}
			}
		} else if ( !Q_stricmp( tok, "classname" ) ) {
			isClass = qtrue;
		}
	}
	for ( i = 0; i < num; i++ ) {
		Q_strcat( buf, size, va( "%s%s x%i", i ? ", " : "", names[i], counts[i] ) );
	}
}


// the entity string the game gets for the current settings (sr.entities, or the map's own)
static const char *SR_MakeEntities( const char *src ) {
	char counts[1024];

	if ( sr.entities ) {
		Z_Free( sr.entities );
		sr.entities = NULL;
	}
	sr.summary[0] = '\0';
	if ( sr.instagib || sr.weapons != -2 || sr.noQuad || sr.noPowerups ) {
		sr.seed = Com_Milliseconds() ^ ( sv.time * 7919 ) ^ rand();
		sr.entities = SR_Rewrite( src );
		if ( sr.entities ) {
			Com_Printf( "splitrules: entities rewritten (%s)\n", sr.summary );
		}
	}
	SR_CountClasses( sr.entities ? sr.entities : src, counts, sizeof( counts ) );
	Com_DPrintf( "splitrules: the game gets %s\n", counts[0] ? counts : "no pickups" );
	return sr.entities ? sr.entities : src;
}


/*
==================
SV_SplitEntityString

SV_InitGameVM (map load and map_restart): the entity string the game will
parse.  Random weapons are re-rolled each time.
==================
*/
const char *SV_SplitEntityString( qboolean restart ) {
	const char *src = CM_EntityString();
	int i;

	if ( sr.entities ) {
		Z_Free( sr.entities );
		sr.entities = NULL;
	}
	sr.entityStart = NULL;
	if ( !restart ) {
		SR_ModInstagibClear();	// GAME_INIT registers the new game VM's cvars again
	}
	sr.summary[0] = '\0';
	sr.layout = SR_DetectLayout( src );
	sr.initTime = sv.time;
	sr.botTime = sv.time;
	sr.maxNoted = qfalse;
	sr.botsAtAdd = -1;
	sr.botEnableNoted = qfalse;
	sr.healthOfs = 0;
	sr.numCand = 0;
	sr.numDistinct = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		sr.cl[i].seen = qfalse;
		sr.cl[i].godSpawn = -1;
	}
	SR_ReadSettings();

	// cheats we forced stay on only while god mode is on (a map load's own
	// map/devmap command sets sv_cheats after this anyway)
	if ( sr.cheatsForced && ( !sr.active || !sr.godMask || !restart ) ) {
		if ( restart ) {
			Cvar_Set( "sv_cheats", "0" );
			Com_Printf( "splitrules: cheats off again (god mode is off)\n" );
		}
		sr.cheatsForced = qfalse;
	}

	if ( !sr.active ) {
		return src;
	}
	if ( sr.layout == SR_LAYOUT_NONE ) {
		Com_Printf( "splitrules: game %s: weapon/instagib/spawn/ammo/self-damage/god options do not apply\n", FS_GetCurrentGameDir() );
		return src;
	}
	sr.entityStart = SR_MakeEntities( src );
	return sr.entityStart;
}


// botlib reads the same entities as the game (BotImport_BSPEntityData)
char *SV_SplitBSPEntities( void ) {
	return sr.entities ? sr.entities : CM_EntityString();
}


/*
=============================================================================

[ps]: each client's playerState after the game frame

=============================================================================
*/

static int *SR_EntHealth( int clientNum ) {
	if ( !sr.healthOfs ) {
		return NULL;
	}
	return (int *)( (byte *)SV_GentityNum( clientNum ) + sr.healthOfs );
}


/*
==================
SR_FindHealth

The game keeps a player's real health in its private gentity part and copies
it into STAT_HEALTH at the end of every frame.  Every int offset whose value
differs from STAT_HEALTH for a live player is struck off; after three
different health values the smallest offset left is the field.
==================
*/
static void SR_FindHealth( int clientNum, const playerState_t *ps ) {
	const byte *ent;
	const int first = (int)sizeof( sharedEntity_t ) / 4;
	int last, i, h;

	if ( sr.healthOfs || !sv.gentities || sv.gentitySize <= (int)sizeof( sharedEntity_t ) ) {
		return;
	}
	last = MIN( sv.gentitySize / 4, (int)sizeof( sr.cand ) );
	ent = (const byte *)SV_GentityNum( clientNum );
	h = ps->stats[0];
	if ( sr.numCand == 0 && sr.numDistinct == 0 ) {
		for ( i = first; i < last; i++ ) {
			sr.cand[i] = 1;
		}
		sr.numCand = last - first;
	}
	for ( i = first; i < last; i++ ) {
		if ( sr.cand[i] && *(const int *)( ent + i * 4 ) != h ) {
			sr.cand[i] = 0;
			sr.numCand--;
		}
	}
	for ( i = 0; i < sr.numDistinct && sr.distinct[i] != h; i++ )
		;
	if ( i == sr.numDistinct && sr.numDistinct < 3 ) {
		sr.distinct[ sr.numDistinct++ ] = h;
	}
	if ( sr.numCand <= 0 ) {
		Com_DPrintf( "splitrules: no health field found; starting over\n" );
		sr.numCand = sr.numDistinct = 0;
		return;
	}
	if ( sr.numDistinct >= 3 ) {
		for ( i = first; i < last && !sr.cand[i]; i++ )
			;
		sr.healthOfs = i * 4;
		Com_DPrintf( "splitrules: the game's health field is at +%i (%i candidates)\n", sr.healthOfs, sr.numCand );
	}
}


static void SR_SetHealth( int clientNum, playerState_t *ps, int health ) {
	int *h = SR_EntHealth( clientNum );

	if ( h ) {
		*h = health;
		ps->stats[0] = health;
	}
}


static void SR_SpawnWeapons( int clientNum, playerState_t *ps ) {
	int bits, w, i;

	if ( sr.instagib ) {
		ps->stats[ STAT_W ] = 1 << SR_WP_RAILGUN;
		ps->ammo[ SR_WP_RAILGUN ] = SR_AMMO_INFINITE;
		w = SR_WP_RAILGUN;
	} else if ( sr.weapons >= 0 ) {
		const srWeapon_t *x = &srWeapons[ sr.weapons ];
		ps->stats[ STAT_W ] = ( 1 << SR_WP_GAUNTLET ) | ( 1 << x->weapon );
		ps->ammo[ x->weapon ] = x->spawnAmmo;
		w = x->weapon;
	} else if ( sr.spawnMode == 1 ) {
		bits = ps->stats[ STAT_W ];
		for ( i = 0; i < SR_NUM_WEAPONS; i++ ) {
			if ( srWeapons[i].weapon <= SR_MaxWeapon() ) {
				bits |= 1 << srWeapons[i].weapon;
				if ( ps->ammo[ srWeapons[i].weapon ] < srWeapons[i].spawnAmmo || srWeapons[i].spawnAmmo < 0 ) {
					ps->ammo[ srWeapons[i].weapon ] = srWeapons[i].spawnAmmo;
				}
			}
		}
		ps->stats[ STAT_W ] = bits;
		return;	// keeps the game's choice of weapon in hand
	} else if ( sr.spawnMode == 2 ) {
		ps->stats[ STAT_W ] = 1 << SR_WP_GAUNTLET;
		ps->ammo[ SR_WP_MACHINEGUN ] = 0;
		w = SR_WP_GAUNTLET;
	} else {
		return;
	}
	ps->weapon = w;
	ps->weaponstate = SR_WEAPON_READY;
	ps->weaponTime = 0;
	(void)clientNum;
}


static void SR_ClientFrame( int i, client_t *cl ) {
	playerState_t *ps = SV_GameClientNum( i );
	srClient_t *t = &sr.cl[i];
	const qboolean alive = ( ps->pm_type == SR_PM_NORMAL && ps->stats[0] > 0 ) ? qtrue : qfalse;
	const int spawn = ps->persistant[ SR_PERS_SPAWN_COUNT ];
	qboolean spawned = qfalse;
	int w, maxW;

	if ( alive ) {
		SR_FindHealth( i, ps );
	}
	if ( !t->seen || spawn != t->spawnCount ) {
		spawned = t->seen || alive;
		t->seen = qtrue;
		t->spawnCount = spawn;
		if ( spawned && alive ) {
			SR_SpawnWeapons( i, ps );
		}
		t->health = ps->stats[0];
		t->armor = ps->stats[ STAT_A ];
	}

	// self-damage off: health / armor lost to the player's own weapon come back
	// (knockback stays: rocket jumps work).  The once-a-second decay above the
	// maximum (exactly 1 point) is not damage.
	if ( sr.selfDamageOff && alive && !spawned && ps->persistant[ SR_PERS_ATTACKER ] == i ) {
		const int maxH = ps->stats[ STAT_MH ];
		const int dh = t->health - ps->stats[0];
		const int da = t->armor - ps->stats[ STAT_A ];
		if ( da > 0 && !( da == 1 && t->armor > maxH ) ) {
			ps->stats[ STAT_A ] = t->armor;
		}
		if ( dh > 0 && !( dh == 1 && t->health > maxH ) && SR_EntHealth( i ) ) {
			SR_SetHealth( i, ps, t->health );
			Com_DPrintf( "splitrules: client %i self-damage %i/%i undone\n", i, dh, MAX( da, 0 ) );
		}
	}

	// instagib: 100 health (a rail hit kills), no armor
	if ( sr.instagib && alive ) {
		if ( ps->stats[0] > 100 ) {
			SR_SetHealth( i, ps, 100 );
		}
		ps->stats[ STAT_A ] = 0;
	}

	// infinite ammo: every weapon held (not the gauntlet or the grapple)
	if ( ( sr.infiniteAmmo || sr.instagib ) && alive ) {
		maxW = SR_MaxWeapon();
		for ( w = 2; w <= maxW; w++ ) {
			if ( w != SR_WP_GRAPPLE && ( ps->stats[ STAT_W ] & ( 1 << w ) ) ) {
				ps->ammo[w] = SR_AMMO_INFINITE;
			}
		}
	}

	// god mode, a client command (needs cheats; cleared by every spawn)
	if ( cl->netchan.remoteAddress.type != NA_BOT ) {
		const qboolean want = ( sr.godMask == -1 || ( sr.godMask > 0 && i < 31 && ( sr.godMask & ( 1 << i ) ) ) ) ? qtrue : qfalse;
		const qboolean on = ( t->godSpawn >= 0 && t->godSpawn == spawn ) ? qtrue : qfalse;
		// a respawn cleared it; a dead player's god is cleared by the coming
		// respawn, so it does not hold up the cheats-off either
		if ( !on || ( !alive && !want ) ) {
			t->godSpawn = -1;
		}
		if ( alive && want != on && Cvar_VariableIntegerValue( "sv_cheats" ) && sv.time - sr.cheatsTime > 200 ) {
			SV_ExecuteClientCommand( cl, "god" );
			t->godSpawn = want ? spawn : -1;
			Com_Printf( "splitrules: god mode %s for client %i (%s)\n", want ? "on" : "off", i, cl->name );
		}
	}

	t->health = ps->stats[0];
	t->armor = ps->stats[ STAT_A ];
}


/*
=============================================================================

Bots: the count is kept (added / kicked a few at a time)

=============================================================================
*/

static char srBotNames[64][32];
static int srNumBotNames = -1;

static void SR_LoadBotNames( void ) {
	static const char *fallback[] = { "Sarge", "Visor", "Doom", "Klesk", "Anarki", "Major", "Hunter", "Slash",
		"Grunt", "Bitterman", "Orbb", "Ranger", "Razor", "Keel", "Lucy", "Mynx", "Phobos", "Sorlag", "Tankjr", "Uriel" };
	char **files = NULL;
	int numFiles = 0, f, i;
	char path[MAX_QPATH];
	void *buf;
	const char *p, *tok;

	srNumBotNames = 0;
	for ( f = -1; f < numFiles; f++ ) {
		if ( f < 0 ) {
			Q_strncpyz( path, "scripts/bots.txt", sizeof( path ) );
			files = FS_ListFiles( "scripts", ".bot", &numFiles );
		} else {
			Com_sprintf( path, sizeof( path ), "scripts/%s", files[f] );
		}
		if ( FS_ReadFile( path, &buf ) <= 0 || !buf ) {
			continue;
		}
		p = (const char *)buf;
		while ( p && srNumBotNames < (int)ARRAY_LEN( srBotNames ) ) {
			tok = COM_Parse( &p );
			if ( !tok[0] ) {
				break;
			}
			if ( Q_stricmp( tok, "name" ) ) {
				continue;
			}
			tok = COM_Parse( &p );
			for ( i = 0; i < srNumBotNames && Q_stricmp( srBotNames[i], tok ); i++ )
				;
			if ( tok[0] && i == srNumBotNames && !strchr( tok, ' ' ) ) {
				Q_strncpyz( srBotNames[ srNumBotNames++ ], tok, sizeof( srBotNames[0] ) );
			}
		}
		FS_FreeFile( buf );
	}
	if ( files ) {
		FS_FreeFileList( files );
	}
	if ( !srNumBotNames ) {
		for ( i = 0; i < (int)ARRAY_LEN( fallback ); i++ ) {
			Q_strncpyz( srBotNames[ srNumBotNames++ ], fallback[i], sizeof( srBotNames[0] ) );
		}
	}
	Com_DPrintf( "splitrules: %i bot names\n", srNumBotNames );
}


// a bot name nobody on the server uses
static const char *SR_BotName( void ) {
	char clean[MAX_NAME_LENGTH];
	int start, k, i;

	if ( srNumBotNames < 0 ) {
		SR_LoadBotNames();
	}
	start = rand() % srNumBotNames;
	for ( k = 0; k < srNumBotNames; k++ ) {
		const char *name = srBotNames[ ( start + k ) % srNumBotNames ];
		for ( i = 0; i < sv.maxclients; i++ ) {
			if ( svs.clients[i].state >= CS_CONNECTED ) {
				Q_strncpyz( clean, svs.clients[i].name, sizeof( clean ) );
				Q_CleanStr( clean );
				if ( !Q_stricmp( clean, name ) ) {
					break;
				}
			}
		}
		if ( i == sv.maxclients ) {
			return name;
		}
	}
	return srBotNames[ start ];
}


static void SR_BotFrame( void ) {
	extern int bot_enable;
	int bots = 0, humans = 0, last = -1, i;
	client_t *cl;

	if ( sr.bots <= 0 || sv.time - sr.initTime < 1500 || sv.time - sr.botTime < 400 ) {
		return;
	}
	for ( i = 0, cl = svs.clients; i < sv.maxclients; i++, cl++ ) {
		if ( cl->state < CS_CONNECTED ) {
			continue;
		}
		if ( cl->netchan.remoteAddress.type == NA_BOT ) {
			bots++;
			last = i;
		} else {
			humans++;
		}
	}
	// one addbot at a time: the next only once that bot is in (or after 5 s), so adds that
	// wait in the command buffer (behind a script's wait) never pile up
	if ( sr.botsAtAdd >= 0 ) {
		if ( bots <= sr.botsAtAdd && sv.time - sr.botTime < 5000 ) {
			return;
		}
		sr.botsAtAdd = -1;
	}
	if ( humans + sr.bots > sv_maxclients->integer && !sr.maxNoted ) {
		sr.maxNoted = qtrue;
		Cvar_Set( "sv_maxclients", va( "%i", humans + sr.bots ) );	// latched: the next map load
		Com_Printf( "splitrules: sv_maxclients %i for %i players + %i bots (at the next map load)\n",
			humans + sr.bots, humans, sr.bots );
	}
	if ( bots < sr.bots && humans + bots < sv.maxclients ) {
		if ( !bot_enable ) {
			if ( !sr.botEnableNoted ) {
				sr.botEnableNoted = qtrue;
				Cvar_Set( "bot_enable", "1" );
				Com_Printf( "splitrules: bots are off in this game (bot_enable 0): on at the next map load\n" );
			}
			return;
		}
		Cbuf_ExecuteText( EXEC_INSERT, va( "addbot %s %i\n", SR_BotName(), sr.botSkill >= 1 && sr.botSkill <= 5 ? sr.botSkill : 1 + rand() % 5 ) );	// ahead of a waiting script
		sr.botTime = sv.time;
		sr.botsAtAdd = bots;
	} else if ( bots > sr.bots && last >= 0 ) {
		Com_Printf( "splitrules: kicking %s (%i bots wanted)\n", svs.clients[last].name, sr.bots );
		SV_DropClient( &svs.clients[last], "was kicked" );
		svs.clients[last].lastPacketTime = svs.time;
		sr.botTime = sv.time;
	}
}


/*
==================
SV_SplitRulesFrame

After the game frames of a server frame (SV_Frame), before snapshots.
==================
*/
void SV_SplitRulesFrame( void ) {
	client_t *cl;
	int i;

	SR_ReadSettings();
	if ( !sr.active || sv.state != SS_GAME || !gvm ) {
		return;
	}

	// god mode needs cheats: on for this local server while it is used
	if ( sr.layout != SR_LAYOUT_NONE && sr.godMask && !Cvar_VariableIntegerValue( "sv_cheats" ) ) {
		Cvar_Set( "sv_cheats", "1" );
		sr.cheatsForced = qtrue;
		sr.cheatsTime = sv.time;
		Com_Printf( "splitrules: cheats on for god mode (this local game only)\n" );
	}

	// only right after a game frame: between them, client packets (ClientThink) change the
	// game's health before ClientEndFrame copies it into STAT_HEALTH
	if ( sr.layout != SR_LAYOUT_NONE && sv.time != sr.frameTime ) {
		sr.frameTime = sv.time;
		for ( i = 0, cl = svs.clients; i < sv.maxclients && i < MAX_CLIENTS; i++, cl++ ) {
			if ( cl->state != CS_ACTIVE || !cl->gentity ) {
				sr.cl[i].seen = qfalse;
				sr.cl[i].godSpawn = -1;
				continue;
			}
			SR_ClientFrame( i, cl );
		}
	}

	// god mode turned off mid-game: once every player's god is off again (the
	// toggle itself needs cheats), drop the cheats we forced -- never a
	// devmap game's own
	if ( sr.cheatsForced && !sr.godMask ) {
		for ( i = 0; i < sv.maxclients && i < MAX_CLIENTS; i++ ) {
			if ( sr.cl[i].godSpawn >= 0 ) {
				break;
			}
		}
		if ( i >= sv.maxclients || i >= MAX_CLIENTS ) {
			Cvar_Set( "sv_cheats", "0" );
			sr.cheatsForced = qfalse;
			Com_Printf( "splitrules: cheats off again (god mode is off)\n" );
		}
	}
	SR_BotFrame();
}


// for the client's page: the running level's layout (-1 = no level)
int SV_SplitRulesLayout( void ) {
	if ( !com_sv_running || !com_sv_running->integer || sv.state != SS_GAME ) {
		return -1;
	}
	return sr.layout;
}


qboolean SV_SplitBotsEnabled( void ) {
	extern int bot_enable;
	return bot_enable ? qtrue : qfalse;
}


/*
==================
SR_Debug_f

srvdebug ps | ents | health
==================
*/
static void SR_Debug_f( void ) {
	const char *what = Cmd_Argv( 1 );
	char buf[1024];
	playerState_t *ps;
	int i, w;

	if ( !com_sv_running->integer || sv.state != SS_GAME ) {
		Com_Printf( "srvdebug: no level running\n" );
		return;
	}
	if ( !Q_stricmp( what, "ents" ) ) {
		SR_CountClasses( SV_SplitBSPEntities(), buf, sizeof( buf ) );
		Com_Printf( "srvdebug ents: layout %i, %s; %s\n", sr.layout, sr.summary[0] ? sr.summary : "map's own entities", buf[0] ? buf : "no pickups" );
		{
			// where the first few weapon pickups are (test scripts aim the view there)
			const char *p = SV_SplitBSPEntities(), *tok;
			char cls[64] = "", org[64] = "";
			int shown = 0;
			while ( p && shown < 4 ) {
				tok = COM_Parse( &p );
				if ( !tok[0] ) {
					break;
				}
				if ( !strcmp( tok, "{" ) ) {
					cls[0] = org[0] = '\0';
				} else if ( !strcmp( tok, "}" ) ) {
					if ( SR_IsWeaponClass( cls ) && org[0] ) {
						Com_Printf( "  %s at %s\n", cls, org );
						shown++;
					}
				} else if ( !Q_stricmp( tok, "classname" ) ) {
					Q_strncpyz( cls, COM_Parse( &p ), sizeof( cls ) );
				} else if ( !Q_stricmp( tok, "origin" ) ) {
					Q_strncpyz( org, COM_Parse( &p ), sizeof( org ) );
				} else {
					COM_Parse( &p );	// a value
				}
			}
		}
		return;
	}
	if ( !Q_stricmp( what, "health" ) ) {
		Com_Printf( "srvdebug health: field %s+%i, %i candidates, %i values seen; gentity size %i, shared %i\n", sr.healthOfs ? "" : "not found ",
			sr.healthOfs, sr.numCand, sr.numDistinct, sv.gentitySize, (int)sizeof( sharedEntity_t ) );
		for ( i = 0; i < sv.maxclients; i++ ) {
			if ( svs.clients[i].state == CS_ACTIVE ) {
				const byte *ent = (const byte *)SV_GentityNum( i );
				ps = SV_GameClientNum( i );
				buf[0] = '\0';
				for ( w = (int)sizeof( sharedEntity_t ); w + 4 <= sv.gentitySize && w < 4096; w += 4 ) {
					const int v = *(const int *)( ent + w );
					if ( v == ps->stats[0] ) {
						Q_strcat( buf, sizeof( buf ), va( " +%i", w ) );
					}
				}
				Com_Printf( "  client %i STAT_HEALTH %i found at:%s\n", i, ps->stats[0], buf );
			}
		}
		return;
	}
	Com_Printf( "srvdebug ps: layout %i, instagib %i, weapons %i, no-quad %i, no-powerups %i, spawn %i, ammo %i, selfdamage-off %i, god %i, bots %i/%i, cheats %i\n",
		sr.layout, sr.instagib, sr.weapons, sr.noQuad, sr.noPowerups, sr.spawnMode, sr.infiniteAmmo, sr.selfDamageOff, sr.godMask, sr.bots, sr.botSkill,
		Cvar_VariableIntegerValue( "sv_cheats" ) );
	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( svs.clients[i].state != CS_ACTIVE ) {
			continue;
		}
		ps = SV_GameClientNum( i );
		buf[0] = '\0';
		for ( w = 1; w <= SR_MaxWeapon(); w++ ) {
			if ( ps->stats[ STAT_W ] & ( 1 << w ) ) {
				Q_strcat( buf, sizeof( buf ), va( " %i:%i", w, ps->ammo[w] ) );
			}
		}
		Com_Printf( "  %i %s%s: pm %i spawn %i score %i health %i/%i armor %i handicap %s weapon %i weapons 0x%x [%s ] attacker %i god %s speed %i origin %.0f %.0f %.0f\n",
			i, svs.clients[i].name, svs.clients[i].netchan.remoteAddress.type == NA_BOT ? " (bot)" : "",
			ps->pm_type, ps->persistant[ SR_PERS_SPAWN_COUNT ], ps->persistant[0], ps->stats[0], ps->stats[ STAT_MH ],
			ps->stats[ STAT_A ], Info_ValueForKey( svs.clients[i].userinfo, "handicap" ), ps->weapon,
			ps->stats[ STAT_W ], buf, ps->persistant[ SR_PERS_ATTACKER ],
			( sr.cl[i].godSpawn >= 0 && sr.cl[i].godSpawn == ps->persistant[ SR_PERS_SPAWN_COUNT ] ) ? "on" : "off",
			ps->speed,
			ps->origin[0], ps->origin[1], ps->origin[2] );
	}
}


void SV_SplitRulesInit( void ) {
	Cmd_AddCommand( "srvdebug", SR_Debug_f );
}


void SV_SplitRulesShutdown( void ) {
	if ( sr.entities ) {
		Z_Free( sr.entities );
		sr.entities = NULL;
	}
	sr.cheatsForced = qfalse;
}


// for the client's page: sv_cheats is on only because god mode asked for it
qboolean SV_SplitCheatsForced( void ) {
	return sr.cheatsForced;
}


/*
=============================================================================

R16: the game's own instagib cvar

Stock baseq3 / missionpack have none; mods do (Urban Terror 4.3 g_instagib,
OSP match_instagib, ...).  Every cvar the game VM registers passes here
(G_CVAR_REGISTER); the best "insta" name wins, so the Server options page can
use the mod's own mode instead of our preset.

=============================================================================
*/

static char	srModInstagib[MAX_CVAR_VALUE_STRING];
static int	srModInstagibScore;

static int SR_InstagibScore( const char *name ) {
	static const char *best[] = { "g_instagib", "instagib", "match_instagib", "g_insta", "sv_instagib", NULL };	// UrT 4.3: g_instagib, OSP: match_instagib
	static const char *skip[] = { "vote_", "match_", "ui_", "cg_", "cl_", NULL };
	int i;

	for ( i = 0; best[i]; i++ ) {
		if ( !Q_stricmp( name, best[i] ) ) {
			return 100 - i;
		}
	}
	for ( i = 0; skip[i]; i++ ) {
		if ( !Q_stricmpn( name, skip[i], (int)strlen( skip[i] ) ) ) {
			return 0;	// a vote switch, a match default, a menu value
		}
	}
	if ( Q_stristr( name, "reload" ) || Q_stristr( name, "allow" ) || Q_stristr( name, "instant" ) ) {
		return 0;
	}
	return MAX( 1, 50 - (int)strlen( name ) );	// some other g_*insta*: the shortest
}


void SV_SplitGameCvar( const char *name ) {
	int score;

	if ( !name || !Q_stristr( name, "insta" ) || strlen( name ) >= sizeof( srModInstagib ) ) {
		return;
	}
	score = SR_InstagibScore( name );
	if ( score > srModInstagibScore ) {
		srModInstagibScore = score;
		Q_strncpyz( srModInstagib, name, sizeof( srModInstagib ) );
		Com_DPrintf( "splitrules: the game has its own instagib cvar: %s\n", name );
		// G_InitGame registers its cvars before it reads the entities: our preset, chosen
		// before the game was known (first load of the game), gives way to the game's own
		// while nothing has been parsed yet
		if ( sr.instagib && sr.entityStart && sv.entityParsePoint == sr.entityStart ) {
			sr.instagib = qfalse;
			sr.entityStart = SR_MakeEntities( CM_EntityString() );
			sv.entityParsePoint = sr.entityStart;
			Com_Printf( "splitrules: the game has its own instagib (%s): our preset is off\n", name );
		}
	}
}


// a new game VM (map load, not map_restart): its cvars come again
static void SR_ModInstagibClear( void ) {
	srModInstagib[0] = '\0';
	srModInstagibScore = 0;
}


// for the client's page: the running (or last) game's instagib cvar, "" = none
const char *SV_SplitModInstagib( void ) {
	return ( srModInstagib[0] && Cvar_Flags( srModInstagib ) != CVAR_NONEXISTENT ) ? srModInstagib : "";
}


// for the client's page: the running server's client slots; the sv_maxclients
// cvar may already hold a bigger, latched value (SR_BotFrame raises it)
int SV_SplitMaxClients( void ) {
	if ( com_sv_running && com_sv_running->integer && sv.state == SS_GAME ) {
		return sv.maxclients;
	}
	return sv_maxclients ? sv_maxclients->integer : 0;
}
