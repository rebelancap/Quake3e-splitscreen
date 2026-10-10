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
// cl_splitsrv.c -- state behind player 1's "Server options" page (design doc 18)
//
// The page (cl_splitmenu.c) edits:
//  - native cvars directly (timelimit, fraglimit, capturelimit, g_friendlyFire,
//    g_gravity, s_volume): they persist as the game/engine keeps them, so a
//    mod's own start-server menu is never overridden;
//  - our archived cl_splitSrv* settings for the engine levers, which this file
//    copies every frame into the sv_split* cvars the local server reads
//    (server/sv_splitrules.c; a dedicated server never has them).  Instagib,
//    Weapons, Quad damage and Power-ups (R17) are held back while the page is open: they apply
//    when it is left, with one restart; Player speed / Weapon respawn (R16) are
//    written into the game's g_speed / g_weaponrespawn every frame (live), and so are
//    Respawn time / Wave respawns (R24: UrT g_respawndelay + g_redwave / g_bluewave,
//    g_waverespawns; baseq3-style games g_forcerespawn) and the R25 round rows Round time /
//    Max rounds / Sudden death / Spawn protection (UrT g_roundtime, g_maxrounds,
//    g_suddendeath, g_respawnprotection); Swap roles and Warmup (UrT g_swaproles,
//    g_warmup; baseq3 g_doWarmup + g_warmup) are read at map start: held back while the
//    page is open like Quad damage, then one restart;
//  - a pending map and game type (cl_splitSrvMap / cl_splitSrvGametype, temp),
//    loaded when the page is left.
// Saved sets: <homepath>/<game>/serversettings/<key>.cfg, `cvar "value"` lines.

#include "client.h"

#define SRV_DIR		"serversettings"
#define SRV_MAX_SETS	64
#define SRV_MAX_MAPS	512

static cvar_t *srvInstagib, *srvWeapons, *srvSpawn, *srvAmmo, *srvBots, *srvBotSkill;
static cvar_t *srvSelfDamage, *srvGod, *srvHealth, *srvSet, *srvMap, *srvGametype;
static cvar_t *srvQuad, *srvSpeed, *srvWeapRespawn, *srvModInsta, *srvPowerups;
static cvar_t *srvRespawnTime, *srvWave;
static cvar_t *srvRoundTime, *srvMaxRounds, *srvSudden, *srvSwap, *srvProtect, *srvWarmup;	// R25

// R24: a game cvar the page holds at a value; a value we did not write is the game's own
// (its base), given back once when the row goes to Default
typedef struct {
	const char	*name;
	char		base[32];
	char		applied[32];	// what we set it to, "" = untouched
} srvHold_t;

enum { SRV_H_RESPAWNDELAY, SRV_H_REDWAVE, SRV_H_BLUEWAVE, SRV_H_FORCERESPAWN, SRV_H_WAVES,
	SRV_H_ROUNDTIME, SRV_H_MAXROUNDS, SRV_H_SUDDEN, SRV_H_SWAP, SRV_H_PROTECT, SRV_H_WARMUP, SRV_H_DOWARMUP, SRV_H_COUNT };
static srvHold_t srvHolds[ SRV_H_COUNT ] = {
	{ "g_respawndelay" }, { "g_redwave" }, { "g_bluewave" }, { "g_forcerespawn" }, { "g_waverespawns" },
	{ "g_roundtime" }, { "g_maxrounds" }, { "g_suddendeath" }, { "g_swaproles" }, { "g_respawnprotection" },
	{ "g_warmup" }, { "g_doWarmup" }
};

static struct {
	qboolean	editing;			// the page is open: instagib / weapons / quad wait
	char		instagib[8];		// what the server has (sv_split*)
	char		weapons[32];
	char		quad[8];
	char		powerups[8];		// R17
	char		swap[8];			// R25: Swap roles / Warmup as the server has them
	char		warmup[8];
	char		speedBase[32];		// g_speed as the game had it (R16 Player speed)
	char		speedApplied[32];	// what we set it to, "" = untouched
	int			respawnApplied;		// g_weaponrespawn we set, 0 = untouched
	char		openMap[MAX_QPATH];	// at the page's opening
	int			openGametype;
	int			lastBots;
	int			lastSkill;
	char		lastHealth[8];

	char		sets[SRV_MAX_SETS][PROFILE_NAME_LEN + 1];
	int			numSets;

	char		**maps;				// FS_ListFiles
	int			numMaps;
	int			mapOrder[SRV_MAX_MAPS];	// current map first
	char		shotMap[MAX_QPATH];
	qhandle_t	shot;
} srv;

// the settings a set keeps (native ones included); map and game type are pending values
static const char *srvSetCvars[] = {
	"cl_splitSrvGametype", "cl_splitSrvMap", "timelimit", "fraglimit", "capturelimit",
	"cl_splitSrvInstagib", "cl_splitSrvWeapons", "cl_splitSrvSpawnWeapons", "cl_splitSrvInfiniteAmmo",
	"cl_splitSrvBots", "cl_splitSrvBotSkill", "g_friendlyFire", "cl_splitSrvSelfDamage",
	"cl_splitSrvGod", "cl_splitSrvHealth", "g_gravity", "cl_splitSrvQuad", "cl_splitSrvSpeed",
	"cl_splitSrvWeaponRespawn", "cl_splitSrvModInstagib", "cl_splitSrvPowerups",
	"cl_splitSrvRespawnTime", "cl_splitSrvWaveRespawns",
	"cl_splitSrvRoundTime", "cl_splitSrvMaxRounds", "cl_splitSrvSuddenDeath", "cl_splitSrvSwapRoles",
	"cl_splitSrvSpawnProtection", "cl_splitSrvWarmup", NULL
};


/*
=============================================================================

STATE

=============================================================================
*/

// player 1 plays on this process's own server (not a demo, not another machine's)
qboolean CL_SplitSrvLocalGame( void ) {
	return CL_AimAssistLocal();
}


// 0: Urban Terror / unknown game (no [entities]/[ps] rows), 1 baseq3 layout, 2 missionpack
int CL_SplitSrvLayout( void ) {
	const int l = SV_SplitRulesLayout();
	const char *dir = FS_GetCurrentGameDir();

	if ( l >= 0 && CL_SplitSrvLocalGame() ) {
		return l;
	}
	if ( CL_SplitSrvUrT() ) {
		return 0;
	}
	return !Q_stricmp( dir, "missionpack" ) ? 2 : 1;
}


qboolean CL_SplitSrvUrT( void ) {
	return ( !Q_stricmp( FS_GetCurrentGameDir(), "q3ut4" ) || Q_stristr( Cvar_VariableString( "fs_basegame" ), "q3ut4" ) ) ? qtrue : qfalse;
}


// the CTF game type number of this game (capture limit row)
qboolean CL_SplitSrvCTF( void ) {
	const int gt = srvGametype ? srvGametype->integer : 0;
	return CL_SplitSrvUrT() ? ( gt == 7 ) : ( gt == 4 || gt == 5 );
}


// R16: the game's own instagib cvar (UrT g_instagib, OSP match_instagib, ...), "" = none: the Instagib
// row then uses it instead of our preset
const char *CL_SplitSrvModInstagib( void ) {
	return SV_SplitModInstagib();
}


// instagib is on for the running game (our preset on the server, or the game's own)
qboolean CL_SplitSrvInstagibOn( void ) {
	const char *mod = CL_SplitSrvModInstagib();

	if ( !CL_SplitSrvLocalGame() ) {
		return qfalse;
	}
	if ( mod[0] ) {
		return Cvar_VariableIntegerValue( mod ) ? qtrue : qfalse;
	}
	return ( atoi( srv.instagib ) && CL_SplitSrvLayout() != 0 ) ? qtrue : qfalse;
}


// the page's Instagib row differs from the game's own instagib cvar
static qboolean SRV_ModInstaChanged( void ) {
	const char *mod = CL_SplitSrvModInstagib();

	return ( mod[0] && CL_SplitSrvLocalGame() && !srvModInsta->integer != !Cvar_VariableIntegerValue( mod ) ) ? qtrue : qfalse;
}


// the server-side copies; instagib / weapons only while the page is closed
static void SRV_Sync( void ) {
	int mask = 0, i;
	const char *god = srvGod->string;
	const char *mod = CL_SplitSrvModInstagib();
	const qboolean modOn = ( mod[0] && Cvar_VariableIntegerValue( mod ) ) ? qtrue : qfalse;

	Cvar_Set( "sv_splitRules", "1" );
	if ( !srv.editing ) {
		Q_strncpyz( srv.instagib, srvInstagib->string, sizeof( srv.instagib ) );
		Q_strncpyz( srv.weapons, srvWeapons->string, sizeof( srv.weapons ) );
		Q_strncpyz( srv.quad, srvQuad->string, sizeof( srv.quad ) );
		Q_strncpyz( srv.powerups, srvPowerups->string, sizeof( srv.powerups ) );
		Q_strncpyz( srv.swap, srvSwap->string, sizeof( srv.swap ) );
		Q_strncpyz( srv.warmup, srvWarmup->string, sizeof( srv.warmup ) );
	}
	// a game with its own instagib (R16): not our preset; while it is on, no weapon levers
	Cvar_Set( "sv_splitInstagib", CL_SplitSrvModInstagib()[0] ? "0" : srv.instagib );
	Cvar_Set( "sv_splitWeapons", modOn ? "default" : srv.weapons );
	Cvar_Set( "sv_splitQuad", srv.quad );
	Cvar_Set( "sv_splitPowerups", srv.powerups );
	Cvar_Set( "sv_splitSpawnWeapons", modOn ? "mg" : srvSpawn->string );
	Cvar_Set( "sv_splitInfiniteAmmo", modOn ? "0" : srvAmmo->string );
	Cvar_Set( "sv_splitSelfDamage", srvSelfDamage->string );
	Cvar_Set( "sv_splitBots", srvBots->string );
	Cvar_Set( "sv_splitBotSkill", srvBotSkill->string );

	// god mode: the server's client numbers of the chosen local players
	if ( !Q_stricmp( god, "all" ) ) {
		mask = -1;
	} else if ( atoi( god ) >= 1 && atoi( god ) <= MAX_SPLITVIEW ) {
		i = atoi( god ) - 1;
		if ( clx[i] && ( i == 0 ? cls.state >= CA_PRIMED : CL_SplitSlotActive( i ) )
			&& clx[i]->clConn.clientNum >= 0 && clx[i]->clConn.clientNum < 31 ) {
			mask = 1 << clx[i]->clConn.clientNum;
		}
	}
	if ( !CL_SplitSrvLocalGame() || CL_SplitSrvLayout() == 0 ) {
		mask = 0;
	}
	Cvar_Set( "sv_splitGod", va( "%i", mask ) );
}


/*
==================
SRV_Speed

Player speed (R16): g_speed scaled from the game's own value.  A value we did
not write (the game's default, a config, a mod menu) is the new base; 100 %
puts the base back.
==================
*/
static void SRV_Speed( qboolean local ) {
	const int pct = srvSpeed->integer;
	char cur[32], want[32];

	if ( !local || CL_SplitSrvUrT() || Cvar_Flags( "g_speed" ) == CVAR_NONEXISTENT ) {	// UrT ignores g_speed
		return;
	}
	Q_strncpyz( cur, Cvar_VariableString( "g_speed" ), sizeof( cur ) );
	if ( !srv.speedApplied[0] || strcmp( cur, srv.speedApplied ) ) {
		Q_strncpyz( srv.speedBase, cur, sizeof( srv.speedBase ) );
		srv.speedApplied[0] = '\0';
	}
	if ( pct == 100 || pct < 10 ) {
		if ( srv.speedApplied[0] ) {
			Cvar_Set( "g_speed", srv.speedBase );
			Com_Printf( "Server options: player speed 100%% (g_speed %s)\n", srv.speedBase );
			srv.speedApplied[0] = '\0';
		}
		return;
	}
	Com_sprintf( want, sizeof( want ), "%i", (int)( atof( srv.speedBase ) * pct / 100.0f + 0.5f ) );
	if ( strcmp( want, cur ) ) {
		Cvar_Set( "g_speed", want );
		Com_Printf( "Server options: player speed %i%% (g_speed %s of %s)\n", pct, want, srv.speedBase );
	}
	Q_strncpyz( srv.speedApplied, want, sizeof( srv.speedApplied ) );
}


/*
==================
SRV_WeaponRespawn

Weapon respawn (R16): g_weaponrespawn (and the team games' g_weaponTeamRespawn)
while not Default; back to the game's defaults once when set to Default.
==================
*/
static void SRV_WeaponRespawn( qboolean local ) {
	const int s = srvWeapRespawn->integer;
	const qboolean team = ( Cvar_Flags( "g_weaponTeamRespawn" ) != CVAR_NONEXISTENT ) ? qtrue : qfalse;

	if ( !local || Cvar_Flags( "g_weaponrespawn" ) == CVAR_NONEXISTENT ) {
		return;
	}
	if ( s > 0 ) {
		if ( Cvar_VariableIntegerValue( "g_weaponrespawn" ) != s || ( team && Cvar_VariableIntegerValue( "g_weaponTeamRespawn" ) != s ) ) {
			Cvar_Set( "g_weaponrespawn", va( "%i", s ) );
			if ( team ) {
				Cvar_Set( "g_weaponTeamRespawn", va( "%i", s ) );
			}
			Com_Printf( "Server options: weapons respawn after %i s\n", s );
		}
		srv.respawnApplied = s;
	} else if ( srv.respawnApplied ) {
		Cvar_Reset( "g_weaponrespawn" );
		if ( team ) {
			Cvar_Reset( "g_weaponTeamRespawn" );
		}
		Com_Printf( "Server options: weapon respawn back to the game's default (%s s)\n", Cvar_VariableString( "g_weaponrespawn" ) );
		srv.respawnApplied = 0;
	}
}


// R24: hold a game cvar at want (NULL = give the game's own value back, once); qtrue = written
static qboolean SRV_Hold( srvHold_t *h, const char *want ) {
	char cur[32];

	if ( Cvar_Flags( h->name ) == CVAR_NONEXISTENT ) {
		return qfalse;
	}
	Q_strncpyz( cur, Cvar_VariableString( h->name ), sizeof( cur ) );
	if ( !h->applied[0] || strcmp( cur, h->applied ) ) {
		// a value we did not write (the game's default, a config, a vote): the new base
		Q_strncpyz( h->base, cur, sizeof( h->base ) );
		h->applied[0] = '\0';
	}
	if ( !want ) {
		if ( h->applied[0] ) {
			h->applied[0] = '\0';
			if ( strcmp( cur, h->base ) ) {
				Cvar_Set( h->name, h->base );
				return qtrue;
			}
		}
		return qfalse;
	}
	Q_strncpyz( h->applied, want, sizeof( h->applied ) );
	if ( strcmp( cur, want ) ) {
		Cvar_Set( h->name, want );
		return qtrue;
	}
	return qfalse;
}


/*
==================
SRV_RespawnTime

Respawn time / Wave respawns (R24), live.  Urban Terror: g_respawndelay (seconds
dead before the respawn) and, so that wave respawns do not defeat it, the team
wave period g_redwave / g_bluewave; g_waverespawns on / off.  A game without
g_respawndelay (baseq3, missionpack, OSP): g_forcerespawn, the seconds after
which a dead player respawns without pressing fire (fire still respawns after
the game's own 1.7 s).
==================
*/
static void SRV_RespawnTime( qboolean local ) {
	const int s = srvRespawnTime->integer;
	const int wave = srvWave->integer;
	char want[16];
	const char *w = NULL;
	qboolean changed = qfalse;

	if ( !local ) {
		return;
	}
	if ( s > 0 ) {
		Com_sprintf( want, sizeof( want ), "%i", s );
		w = want;
	}
	if ( Cvar_Flags( srvHolds[ SRV_H_RESPAWNDELAY ].name ) != CVAR_NONEXISTENT ) {
		changed |= SRV_Hold( &srvHolds[ SRV_H_RESPAWNDELAY ], w );
		changed |= SRV_Hold( &srvHolds[ SRV_H_REDWAVE ], w );
		changed |= SRV_Hold( &srvHolds[ SRV_H_BLUEWAVE ], w );
	} else {
		changed |= SRV_Hold( &srvHolds[ SRV_H_FORCERESPAWN ], w );
	}
	if ( changed ) {
		if ( w ) {
			Com_Printf( "Server options: respawn time %i s\n", s );
		} else {
			Com_Printf( "Server options: respawn time back to the game's own\n" );
		}
	}
	if ( SRV_Hold( &srvHolds[ SRV_H_WAVES ], wave < 0 ? NULL : wave ? "1" : "0" ) ) {
		Com_Printf( "Server options: wave respawns %s\n", wave < 0 ? "back to the game's own" : wave ? "on" : "off" );
	}
}


// R25: SRV_Hold and a console line when it wrote something
static void SRV_HoldNote( int h, const char *want, const char *what ) {
	if ( SRV_Hold( &srvHolds[h], want ) ) {
		Com_Printf( "Server options: %s %s (%s)\n", what, want ? want : "back to the game's own", srvHolds[h].name );
	}
}


// R25: -1 = Default (the game's own), else the integer as a string
static const char *SRV_IntWant( int v, char *buf, int size ) {
	if ( v < 0 ) {
		return NULL;
	}
	Com_sprintf( buf, size, "%i", v );
	return buf;
}


// R25: Swap roles / Warmup differ from what the server has (they need a restart)
static qboolean SRV_SwapChanged( void ) {
	return ( Cvar_Flags( "g_swaproles" ) != CVAR_NONEXISTENT && atoi( srvSwap->string ) != atoi( srv.swap ) ) ? qtrue : qfalse;
}

static qboolean SRV_WarmupChanged( void ) {
	return ( Cvar_Flags( "g_warmup" ) != CVAR_NONEXISTENT && atoi( srvWarmup->string ) != atoi( srv.warmup ) ) ? qtrue : qfalse;
}


/*
==================
SRV_RoundRules

The R25 round rows, native cvars held like Respawn time (a value we did not
write is the game's own; Default gives it back once).  Live (the game reads them
at the next round / spawn / match end): Round time (UrT g_roundtime, minutes,
fractions work, not below 1: UrT's game init resets that to 3), Max rounds
(g_maxrounds, 0 = unlimited, drawn rounds count), Sudden death (g_suddendeath),
Spawn protection (g_respawnprotection, seconds).  Read at map
start, so from srv.swap / srv.warmup (held while the page is open, applied with
the restart when it is left): Swap roles (UrT g_swaproles, a latched cvar) and
Warmup (UrT g_warmup; games with g_doWarmup but not UrT: g_doWarmup 0 / 1 plus
g_warmup seconds).
==================
*/
static void SRV_RoundRules( qboolean local ) {
	char want[16];
	const int warm = atoi( srv.warmup );

	if ( !local ) {
		return;
	}
	if ( srvRoundTime->value > 0.0f ) {
		Com_sprintf( want, sizeof( want ), "%g", srvRoundTime->value );
		SRV_HoldNote( SRV_H_ROUNDTIME, want, "round time" );
	} else {
		SRV_HoldNote( SRV_H_ROUNDTIME, NULL, "round time" );
	}
	SRV_HoldNote( SRV_H_MAXROUNDS, SRV_IntWant( srvMaxRounds->integer, want, sizeof( want ) ), "max rounds" );
	SRV_HoldNote( SRV_H_SUDDEN, SRV_IntWant( srvSudden->integer, want, sizeof( want ) ), "sudden death" );
	SRV_HoldNote( SRV_H_PROTECT, SRV_IntWant( srvProtect->integer, want, sizeof( want ) ), "spawn protection" );
	SRV_HoldNote( SRV_H_SWAP, SRV_IntWant( atoi( srv.swap ), want, sizeof( want ) ), "swap roles" );
	if ( CL_SplitSrvUrT() || Cvar_Flags( "g_doWarmup" ) == CVAR_NONEXISTENT ) {
		// UrT: g_warmup alone (UrT's g_doWarmup is not its switch)
		SRV_HoldNote( SRV_H_WARMUP, SRV_IntWant( warm, want, sizeof( want ) ), "warmup" );
	} else {
		// baseq3-style: g_doWarmup switches it; g_warmup is the length (Off keeps the game's own length)
		SRV_HoldNote( SRV_H_DOWARMUP, warm < 0 ? NULL : warm ? "1" : "0", "warmup" );
		SRV_HoldNote( SRV_H_WARMUP, warm > 0 ? SRV_IntWant( warm, want, sizeof( want ) ) : NULL, "warmup length" );
	}
}


/*
==================
CL_SplitSrvFrame

Once per frame: the server-side copies; bot count/difficulty changes that
need bots kicked; player health changes resend userinfo; player speed and
weapon respawn (native cvars, live).
==================
*/
void CL_SplitSrvFrame( void ) {
	const qboolean local = CL_SplitSrvLocalGame();

	if ( !srvInstagib ) {
		return;
	}
	SRV_Sync();

	if ( srvBots->integer != srv.lastBots || srvBotSkill->integer != srv.lastSkill ) {
		// Off after bots: they go; another difficulty: they come back with it
		if ( local && srv.lastBots > 0 && ( srvBots->integer == 0 || srvBotSkill->integer != srv.lastSkill ) ) {
			Com_Printf( "Server options: %s\n", srvBots->integer ? "bots re-added with the new difficulty" : "bots removed" );
			Cbuf_ExecuteText( EXEC_INSERT, "kickbots\n" );	// ahead of a waiting script
		}
		srv.lastBots = srvBots->integer;
		srv.lastSkill = srvBotSkill->integer;
	}
	if ( strcmp( va( "%s/%i", srvHealth->string, CL_SplitSrvInstagibOn() ), srv.lastHealth ) ) {
		Q_strncpyz( srv.lastHealth, va( "%s/%i", srvHealth->string, CL_SplitSrvInstagibOn() ), sizeof( srv.lastHealth ) );
		cvar_modifiedFlags |= CVAR_USERINFO;	// player 1; the others' userinfo is polled
	}
	SRV_Speed( local );
	SRV_WeaponRespawn( local );
	SRV_RespawnTime( local );
	SRV_RoundRules( local );
}


/*
==================
CL_SplitSrvUserinfo

Player health (a handicap for every local player, local games only) over
whatever the profiles say.
==================
*/
const char *CL_SplitSrvUserinfo( int n, const char *info ) {
	static char out[MAX_SPLITVIEW][MAX_INFO_STRING];
	const int h = srvHealth ? srvHealth->integer : 0;

	if ( h < 5 || h > 95 || (unsigned)n >= MAX_SPLITVIEW || !CL_SplitSrvLocalGame() || CL_SplitSrvInstagibOn() ) {
		return info;
	}
	Q_strncpyz( out[n], info, sizeof( out[n] ) );
	Info_SetValueForKey( out[n], "handicap", va( "%i", h ) );
	return out[n];
}


/*
=============================================================================

THE PAGE: open / leave

=============================================================================
*/

void CL_SplitSrvOpen( void ) {
	const char *map = Cvar_VariableString( "mapname" );

	srv.editing = qtrue;
	Q_strncpyz( srv.openMap, CL_SplitSrvLocalGame() ? map : "", sizeof( srv.openMap ) );
	srv.openGametype = Cvar_VariableIntegerValue( "g_gametype" );
	Cvar_Set( srvMap->name, srv.openMap );
	Cvar_Set( srvGametype->name, va( "%i", srv.openGametype ) );
	// a game with its own instagib: the row shows (and edits) that cvar's state
	Cvar_Set( srvModInsta->name, ( CL_SplitSrvModInstagib()[0] && Cvar_VariableIntegerValue( CL_SplitSrvModInstagib() ) ) ? "1" : "0" );
	CL_SplitSrvRefreshMaps();
}


// what leaving the page will do ("" = nothing restarts)
const char *CL_SplitSrvPending( void ) {
	static char buf[96];
	const qboolean local = CL_SplitSrvLocalGame();
	const int need = CL_SplitPlayersInGame() + srvBots->integer;

	if ( !local ) {
		return "";
	}
	if ( srvMap->string[0] && Q_stricmp( srvMap->string, srv.openMap ) ) {
		Com_sprintf( buf, sizeof( buf ), "loads %s", srvMap->string );
		return buf;
	}
	if ( srvGametype->integer != srv.openGametype ) {
		return "reloads the map (game type)";
	}
	if ( srvBots->integer > 0 && !SV_SplitBotsEnabled() ) {
		return "reloads the map (bots)";
	}
	if ( SRV_ModInstaChanged() ) {
		return "restarts the map (instagib)";
	}
	if ( Q_stricmp( srvInstagib->string, srv.instagib ) || Q_stricmp( srvWeapons->string, srv.weapons ) || Q_stricmp( srvQuad->string, srv.quad ) || Q_stricmp( srvPowerups->string, srv.powerups ) ) {
		return "restarts the map";
	}
	if ( SRV_WarmupChanged() ) {
		return "restarts the map (warmup)";
	}
	if ( SRV_SwapChanged() ) {
		return "restarts the map (swap roles)";
	}
	if ( srvBots->integer > 0 && need > SV_SplitMaxClients() ) {
		return "restarts the map (room for the bots)";
	}
	return "";
}


/*
==================
CL_SplitSrvLeave

Leaving the page applies everything: at most one map load or map_restart.
==================
*/
void CL_SplitSrvLeave( void ) {
	const qboolean local = CL_SplitSrvLocalGame();
	const int need = CL_SplitPlayersInGame() + srvBots->integer;
	const char *map = srvMap->string;
	const char *mod = CL_SplitSrvModInstagib();
	const qboolean modChanged = SRV_ModInstaChanged();
	qboolean load, restart;

	if ( !srv.editing ) {
		return;
	}
	load = ( local && map[0] && ( Q_stricmp( map, srv.openMap ) || srvGametype->integer != srv.openGametype
		|| ( srvBots->integer > 0 && !SV_SplitBotsEnabled() ) ) ) ? qtrue : qfalse;
	restart = ( local && !load && ( Q_stricmp( srvInstagib->string, srv.instagib ) || Q_stricmp( srvWeapons->string, srv.weapons ) || Q_stricmp( srvQuad->string, srv.quad ) || Q_stricmp( srvPowerups->string, srv.powerups ) || modChanged
		|| SRV_WarmupChanged() || SRV_SwapChanged() || ( srvBots->integer > 0 && need > SV_SplitMaxClients() ) ) ) ? qtrue : qfalse;
	srv.editing = qfalse;
	if ( modChanged ) {
		// the game's own instagib (latched in some games: the restart / load below reads it)
		Cvar_Set( mod, srvModInsta->integer ? "1" : "0" );
		Com_Printf( "Server options: %s %s (the game's own instagib)\n", mod, srvModInsta->integer ? "1" : "0" );
	}
	SRV_Sync();		// instagib / weapons go to the server now
	SRV_RoundRules( local );	// R25: swap roles / warmup into the game's cvars before the restart reads them

	if ( local && srvBots->integer > 0 && need > Cvar_VariableIntegerValue( "sv_maxclients" ) ) {
		Cvar_Set( "sv_maxclients", va( "%i", need ) );	// latched: the restart below
	}
	if ( local && srvGametype->integer != Cvar_VariableIntegerValue( "g_gametype" ) ) {
		Cvar_Set( "g_gametype", srvGametype->string );		// latched: the map load below
	}
	if ( load ) {
		// keep a devmap a devmap (cheats we did not force)
		const char *cmd = ( Cvar_VariableIntegerValue( "sv_cheats" ) && !SV_SplitCheatsForced() ) ? "devmap" : "map";
		Com_Printf( "Server options: applied; %s %s (game type %i)\n", cmd, map, srvGametype->integer );
		Cbuf_ExecuteText( EXEC_INSERT, va( "%s %s\n", cmd, map ) );	// ahead of a waiting script
	} else if ( restart ) {
		Com_Printf( "Server options: applied; map_restart\n" );
		Cbuf_ExecuteText( EXEC_INSERT, "map_restart 0\n" );	// ahead of a waiting script
	} else {
		Com_Printf( "Server options: applied; no restart needed\n" );
	}
}


void CL_SplitSrvRestartRound( void ) {
	if ( CL_SplitSrvLocalGame() ) {
		Com_Printf( "Server options: restart round\n" );
		Cbuf_ExecuteText( EXEC_INSERT, "map_restart 0\n" );	// ahead of a waiting script
	}
}


void CL_SplitSrvReset( void ) {
	static const char *natives[] = { "timelimit", "fraglimit", "capturelimit", "g_friendlyFire", "g_gravity", NULL };
	cvar_t *ours[] = { srvInstagib, srvWeapons, srvSpawn, srvAmmo, srvBots, srvBotSkill, srvSelfDamage, srvGod, srvHealth,
		srvQuad, srvSpeed, srvWeapRespawn, srvPowerups, srvRespawnTime, srvWave,
		srvRoundTime, srvMaxRounds, srvSudden, srvSwap, srvProtect, srvWarmup };
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( ours ); i++ ) {
		Cvar_Reset( ours[i]->name );
	}
	for ( i = 0; natives[i]; i++ ) {
		if ( Cvar_Flags( natives[i] ) != CVAR_NONEXISTENT ) {
			Cvar_Reset( natives[i] );
		}
	}
	// R16: everything else too -- game type (free for all: both games number it 0; it loads
	// with the map when the page is left) and the volume (100 %); only the map stays
	Cvar_Set( srvGametype->name, "0" );
	Cvar_Set( srvModInsta->name, "0" );
	Cvar_Set( "s_volume", "1" );
	Cvar_Set( srvSet->name, "" );
	Com_Printf( "Server options: reset to defaults (the map is kept)\n" );
}


/*
=============================================================================

MAPS

=============================================================================
*/

static int QDECL SRV_MapCompare( const void *a, const void *b ) {
	return Q_stricmp( *(const char *const *)a, *(const char *const *)b );
}


void CL_SplitSrvRefreshMaps( void ) {
	const char *cur = srv.openMap;
	int i, n = 0, len;

	if ( srv.maps ) {
		FS_FreeFileList( srv.maps );
		srv.maps = NULL;
	}
	srv.maps = FS_ListFiles( "maps", ".bsp", &srv.numMaps );
	srv.numMaps = MIN( srv.numMaps, SRV_MAX_MAPS );
	for ( i = 0; i < srv.numMaps; i++ ) {
		len = (int)strlen( srv.maps[i] );
		if ( len > 4 && !Q_stricmp( srv.maps[i] + len - 4, ".bsp" ) ) {
			srv.maps[i][len - 4] = '\0';
		}
	}
	if ( srv.maps && srv.numMaps > 1 ) {
		qsort( srv.maps, srv.numMaps, sizeof( srv.maps[0] ), SRV_MapCompare );	// alphabetical
	}
	for ( i = 0; i < srv.numMaps; i++ ) {
		if ( cur[0] && !Q_stricmp( srv.maps[i], cur ) ) {
			srv.mapOrder[ n++ ] = i;
		}
	}
	for ( i = 0; i < srv.numMaps; i++ ) {
		if ( !cur[0] || Q_stricmp( srv.maps[i], cur ) ) {
			srv.mapOrder[ n++ ] = i;
		}
	}
	srv.numMaps = n;
}


int CL_SplitSrvNumMaps( void ) {
	return srv.numMaps;
}


// i-th map in page order (the current one first)
const char *CL_SplitSrvMapName( int i ) {
	if ( i < 0 || i >= srv.numMaps || !srv.maps ) {
		return "";
	}
	return srv.maps[ srv.mapOrder[i] ];
}


const char *CL_SplitSrvCurrentMap( void ) {
	return srv.openMap;
}


// the map's levelshot, 0 = none
qhandle_t CL_SplitSrvLevelshot( const char *map ) {
	static const char *ext[] = { "tga", "jpg", "png", NULL };
	int i;

	if ( !Q_stricmp( map, srv.shotMap ) ) {
		return srv.shot;
	}
	Q_strncpyz( srv.shotMap, map, sizeof( srv.shotMap ) );
	srv.shot = 0;
	for ( i = 0; ext[i]; i++ ) {
		if ( FS_FOpenFileRead( va( "levelshots/%s.%s", map, ext[i] ), NULL, qfalse ) > 0 ) {
			srv.shot = re.RegisterShaderNoMip( va( "levelshots/%s", map ) );
			break;
		}
	}
	return srv.shot;
}


/*
=============================================================================

SAVED SETS

=============================================================================
*/

static void SRV_Key( const char *name, char *key, int size ) {
	int i;

	Q_strncpyz( key, name, size );
	for ( i = 0; key[i]; i++ ) {
		key[i] = ( key[i] == ' ' ) ? '_' : ( key[i] >= 'A' && key[i] <= 'Z' ) ? key[i] + 32 : key[i];
	}
}


void CL_SplitSrvRefreshSets( void ) {
	char **files, base[MAX_OSPATH], name[PROFILE_NAME_LEN + 1], key[PROFILE_NAME_LEN + 1];
	int numFiles, i, len;

	srv.numSets = 0;
	files = Sys_ListFiles( FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(), SRV_DIR ), ".cfg", NULL, &numFiles, qfalse );
	for ( i = 0; files && i < numFiles && srv.numSets < SRV_MAX_SETS; i++ ) {
		Q_strncpyz( base, files[i], sizeof( base ) );
		len = (int)strlen( base );
		if ( len > 4 ) {
			base[len - 4] = '\0';
		}
		if ( !CL_ProfileSanitize( base, name, sizeof( name ) ) ) {
			continue;
		}
		SRV_Key( name, key, sizeof( key ) );
		if ( strcmp( key, base ) ) {
			continue;	// not a name the page writes
		}
		// the display name is in the file; the key will do as a fallback
		Q_strncpyz( srv.sets[ srv.numSets++ ], base, sizeof( srv.sets[0] ) );
	}
	if ( files ) {
		Sys_FreeFileList( files );
	}
}


int CL_SplitSrvNumSets( void ) {
	return srv.numSets;
}


const char *CL_SplitSrvSetName( int i ) {
	return ( i >= 0 && i < srv.numSets ) ? srv.sets[i] : "";
}


const char *CL_SplitSrvActiveSet( void ) {
	return srvSet ? srvSet->string : "";
}


qboolean CL_SplitSrvSaveSet( const char *text, char *err, int errSize ) {
	char name[PROFILE_NAME_LEN + 1], key[PROFILE_NAME_LEN + 1];
	fileHandle_t f;
	int i;

	if ( !CL_ProfileSanitize( text, name, sizeof( name ) ) ) {
		Q_strncpyz( err, "Type a name (letters, digits, - _)", errSize );
		return qfalse;
	}
	SRV_Key( name, key, sizeof( key ) );
	f = FS_FOpenFileWrite( va( SRV_DIR "/%s.cfg", key ) );
	if ( f == FS_INVALID_HANDLE ) {
		Q_strncpyz( err, "Could not write the file", errSize );
		return qfalse;
	}
	FS_Printf( f, "// splitscreen server settings \"%s\" (Server options page)\n", name );
	for ( i = 0; srvSetCvars[i]; i++ ) {
		if ( Cvar_Flags( srvSetCvars[i] ) != CVAR_NONEXISTENT ) {
			FS_Printf( f, "%s \"%s\"\n", srvSetCvars[i], Cvar_VariableString( srvSetCvars[i] ) );
		}
	}
	FS_FCloseFile( f );
	Cvar_Set( srvSet->name, key );
	Com_Printf( "Server options: saved %s/%s.cfg\n", SRV_DIR, key );
	return qtrue;
}


qboolean CL_SplitSrvLoadSet( int idx ) {
	const char *key = CL_SplitSrvSetName( idx );
	char *text, *p;
	const char *tok;
	char name[64];
	int i, count = 0;

	if ( !key[0] || FS_ReadFile( va( SRV_DIR "/%s.cfg", key ), (void **)&text ) <= 0 || !text ) {
		return qfalse;
	}
	p = text;
	while ( 1 ) {
		tok = COM_ParseExt( (const char **)&p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		Q_strncpyz( name, tok, sizeof( name ) );
		tok = COM_ParseExt( (const char **)&p, qfalse );
		for ( i = 0; srvSetCvars[i] && Q_stricmp( srvSetCvars[i], name ); i++ )
			;
		if ( !srvSetCvars[i] ) {
			Com_Printf( S_COLOR_YELLOW "Server options: %s.cfg: unknown setting %s skipped\n", key, name );
			continue;
		}
		if ( !Q_stricmp( name, "cl_splitSrvMap" ) && ( !tok[0] || !CL_SplitSrvLocalGame() ) ) {
			continue;	// a map only means something in a game
		}
		Cvar_Set( srvSetCvars[i], tok );
		count++;
	}
	FS_FreeFile( text );
	Cvar_Set( srvSet->name, key );
	Com_Printf( "Server options: loaded %s (%i settings)\n", key, count );
	return qtrue;
}


qboolean CL_SplitSrvDeleteSet( int idx ) {
	const char *key = CL_SplitSrvSetName( idx );

	if ( !key[0] ) {
		return qfalse;
	}
	FS_HomeRemove( va( SRV_DIR "/%s.cfg", key ) );
	Com_Printf( "Server options: deleted %s/%s.cfg\n", SRV_DIR, key );
	if ( !Q_stricmp( srvSet->string, key ) ) {
		Cvar_Set( srvSet->name, "" );
	}
	CL_SplitSrvRefreshSets();
	return qtrue;
}


/*
=============================================================================

INIT

=============================================================================
*/

void CL_SplitSrvInit( void ) {
	Com_Memset( &srv, 0, sizeof( srv ) );
	srvInstagib = Cvar_Get( "cl_splitSrvInstagib", "0", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvInstagib, "Server options (local games): instagib, railguns only, no pickups, 100 health. Applies with a restart." );
	srvWeapons = Cvar_Get( "cl_splitSrvWeapons", "default", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvWeapons, "Server options: map weapons: default, random, or one weapon (gauntlet machinegun shotgun grenade rocket lightning railgun plasma bfg). Applies with a restart." );
	srvSpawn = Cvar_Get( "cl_splitSrvSpawnWeapons", "mg", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvSpawn, "Server options: weapons at spawn: mg (game default), all, gauntlet." );
	srvAmmo = Cvar_Get( "cl_splitSrvInfiniteAmmo", "0", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvAmmo, "Server options: infinite ammo." );
	srvBots = Cvar_Get( "cl_splitSrvBots", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvBots, "0", "20", CV_INTEGER );
	Cvar_SetDescription( srvBots, "Server options: bots kept on the local server (0 = not managed)." );
	srvBotSkill = Cvar_Get( "cl_splitSrvBotSkill", "2", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvBotSkill, "0", "5", CV_INTEGER );
	Cvar_SetDescription( srvBotSkill, "Server options: bot difficulty 1..5, 0 = random per bot." );
	srvSelfDamage = Cvar_Get( "cl_splitSrvSelfDamage", "1", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvSelfDamage, "Server options: 0 = a player's own weapon does not hurt them (knockback stays)." );
	srvGod = Cvar_Get( "cl_splitSrvGod", "0", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvGod, "Server options: god mode for 'all' local players or player 1..8 (turns cheats on for the local server)." );
	srvHealth = Cvar_Get( "cl_splitSrvHealth", "0", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvHealth, "Server options: every local player's handicap 5..95 (0 = each player's own)." );
	srvSet = Cvar_Get( "cl_splitSrvSet", "", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( srvSet, "Server options: the saved set last selected or saved." );
	srvQuad = Cvar_Get( "cl_splitSrvQuad", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvQuad, "0", "1", CV_INTEGER );
	Cvar_SetDescription( srvQuad, "Server options: 0 = no quad damage pickups on the map (item_quad removed). Applies with a restart." );
	srvPowerups = Cvar_Get( "cl_splitSrvPowerups", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvPowerups, "0", "1", CV_INTEGER );
	Cvar_SetDescription( srvPowerups, "Server options: 0 = no power-ups or holdable items on the map (quad, haste, invisibility, "
		"regeneration, battle suit, flight, Team Arena's scout / guard / doubler / ammo regen, every holdable_*). Applies with a restart." );
	srvSpeed = Cvar_Get( "cl_splitSrvSpeed", "100", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvSpeed, "50", "200", CV_INTEGER );
	Cvar_SetDescription( srvSpeed, "Server options: player speed in % of the game's own g_speed (50..200)." );
	srvWeapRespawn = Cvar_Get( "cl_splitSrvWeaponRespawn", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvWeapRespawn, "0", "30", CV_INTEGER );
	Cvar_SetDescription( srvWeapRespawn, "Server options: weapon pickups respawn after 1..30 s (g_weaponrespawn); 0 = the game's default." );
	srvRespawnTime = Cvar_Get( "cl_splitSrvRespawnTime", "3", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvRespawnTime, "0", "15", CV_INTEGER );
	Cvar_SetDescription( srvRespawnTime, "Server options: seconds before a dead player respawns, 1..15 (Urban Terror: g_respawndelay "
		"and the team wave period g_redwave / g_bluewave; Quake 3: g_forcerespawn); 0 = the game's own." );
	srvWave = Cvar_Get( "cl_splitSrvWaveRespawns", "0", CVAR_ARCHIVE_ND );	// off: Respawn time means what it says
	Cvar_CheckRange( srvWave, "-1", "1", CV_INTEGER );
	Cvar_SetDescription( srvWave, "Server options (Urban Terror): wave respawns, 1 on, 0 off (g_waverespawns, our default off so Respawn time is exact); -1 = the game's own." );
	// R25: round rows, Default (the game's own) unless set
	srvRoundTime = Cvar_Get( "cl_splitSrvRoundTime", "0", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvRoundTime, "0", "10", CV_FLOAT );
	Cvar_SetDescription( srvRoundTime, "Server options (Urban Terror): minutes per round 1..10 by 0.5 (g_roundtime; round game types; UrT resets values below 1 to 3 at map start); 0 = the game's own." );
	srvMaxRounds = Cvar_Get( "cl_splitSrvMaxRounds", "-1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvMaxRounds, "-1", "30", CV_INTEGER );
	Cvar_SetDescription( srvMaxRounds, "Server options (Urban Terror): rounds per match 1..30, 0 unlimited (g_maxrounds); -1 = the game's own." );
	srvSudden = Cvar_Get( "cl_splitSrvSuddenDeath", "-1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvSudden, "-1", "1", CV_INTEGER );
	Cvar_SetDescription( srvSudden, "Server options (Urban Terror): 1 = a match tied at the end gets one more round (g_suddendeath), 0 off; -1 = the game's own." );
	srvSwap = Cvar_Get( "cl_splitSrvSwapRoles", "-1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvSwap, "-1", "1", CV_INTEGER );
	Cvar_SetDescription( srvSwap, "Server options (Urban Terror): 1 = teams swap sides (g_swaproles), 0 off; "
		"-1 = the game's own. Applies with a restart." );
	srvProtect = Cvar_Get( "cl_splitSrvSpawnProtection", "-1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvProtect, "-1", "10", CV_INTEGER );
	Cvar_SetDescription( srvProtect, "Server options (Urban Terror): seconds a fresh spawn cannot be hurt, 0..10 (g_respawnprotection); -1 = the game's own." );
	srvWarmup = Cvar_Get( "cl_splitSrvWarmup", "-1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( srvWarmup, "-1", "60", CV_INTEGER );
	Cvar_SetDescription( srvWarmup, "Server options: seconds of warmup before a match, 0 = off (Urban Terror g_warmup; Quake 3 g_doWarmup + g_warmup); "
		"-1 = the game's own. Applies with a restart." );
	srvMap = Cvar_Get( "cl_splitSrvMap", "", CVAR_TEMP );
	srvGametype = Cvar_Get( "cl_splitSrvGametype", "0", CVAR_TEMP );
	srvModInsta = Cvar_Get( "cl_splitSrvModInstagib", "0", CVAR_TEMP );
	Cvar_SetDescription( srvModInsta, "Server options: the page's Instagib row in a game with its own instagib cvar (set into it when the page is left)." );
	srv.lastBots = srvBots->integer;
	srv.lastSkill = srvBotSkill->integer;
	Q_strncpyz( srv.lastHealth, va( "%s/%i", srvHealth->string, CL_SplitSrvInstagibOn() ), sizeof( srv.lastHealth ) );
	SRV_Sync();
}


void CL_SplitSrvShutdown( void ) {
	if ( srv.maps ) {
		FS_FreeFileList( srv.maps );
		srv.maps = NULL;
	}
	srv.editing = qfalse;
}
