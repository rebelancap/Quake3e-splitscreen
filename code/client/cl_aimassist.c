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
// cl_aimassist.c -- subtle pad aim assist, local games only (design doc 15)
//
// Shapes nothing but a pad player's own look-stick rates inside CL_GamepadMove
// (the player's own context is active there), from that player's own
// snapshot: no snap, no magnetism, nothing sent to a server except the
// visible name marker in the userinfo.
//
// Hard gate (code, not a cvar): this process hosts the game (com_sv_running)
// and player 1 reaches it over loopback.  On any other server every value is
// exactly zero / one and the Controls row is greyed.
//
// Two components, both per player (p<N>_joy_aimAssist 0 Off / 1 Low /
// 2 Standard) and both only inside a target's "bubble" with line of sight:
//  - slowdown: look-stick speed scaled down towards the bubble center;
//  - rotational: while the player gives move or look input, a fraction of
//    the target's angular velocity around the view (its motion and the
//    player's own) is added; never with both sticks idle, never after a flick.

#include "client.h"

// per-game entity conventions (keyed by game dir; unknown games use baseq3's)
typedef struct {
	const char	*game;
	int			playerType;		// entityState_t.eType of a player
	int			deadFlag;		// eFlags bit of a dead player
	const char	*teamKey;		// CS_PLAYERS info key holding the team
	int			spectator;		// team value of spectators
	float		chestZ;			// chest point above the entity origin
	float		bodyLow, bodyHigh;	// the body axis the bubble is measured from (above the origin)
	const char	*noAssistGametypes;	// g_gametype values without foes ("9 " = UrT jump), space separated
	const char	*marker;		// name suffix for cl_aimAssistMarker "auto", level Low
	const char	*markerStd;		// the same, level Standard
} aaGame_t;

static const aaGame_t aaGames[] = {
	{ "baseq3",			1, 0x00000001, "t", 3, 12.0f, -16.0f, 28.0f, "", " ^3+", " ^2+" },
	{ "missionpack",	1, 0x00000001, "t", 3, 12.0f, -16.0f, 28.0f, "", " ^3+", " ^2+" },
	{ "q3ut4",			1, 0x00000001, "t", 3, 10.0f, -16.0f, 26.0f, "9", "+", "+" },	// UrT drops spaces and colours from names
};

#define AA_CS_PLAYERS		544		// CS_SOUNDS + MAX_SOUNDS in baseq3, missionpack and UrT
#define AA_MAX_CANDIDATES	8
#define AA_PM_NORMAL		0
#define AA_MAX_OMEGA		720.0f	// deg/s: faster angular motion is a teleport / respawn, not running
#define AA_CORE				0.4f	// share of the bubble radius at full slowdown (R14a)

typedef struct {
	float		fade;			// 0..1, ramps over joy_aimFade ms
	float		falloff;		// last slowdown falloff (kept while fading out)
	int			target;			// entity number tracked, -1 = none
	float		lastAng[2];		// world yaw/pitch to the target last frame
	float		omega[2];		// filtered angular velocity of the target around the eye, deg/s
	int			flickUntil;		// cls.realtime until which rotation is off
	int			markerOn;		// last marker state, 0 off / level (player 1: userinfo resend on change)
	char		state[64];		// last effective state logged ("Low (Guest)", "off: not a local game" ...)
	int64_t		usTotal;		// cl_aimAssistDebug: cost
	int			usCalls;
} aaPlayer_t;

static aaPlayer_t	aa[MAX_SPLITVIEW];

static cvar_t	*cl_aimAssistAllow;
static cvar_t	*cl_aimAssistMarker;
static cvar_t	*cl_aimAssistDebug;
static cvar_t	*joy_aimAssist;		// the shared (Guest default) strength; players: p<N>_joy_aimAssist
static cvar_t	*joy_aimRange;
static cvar_t	*joy_aimBubble;
static cvar_t	*joy_aimBubbleMax;
static cvar_t	*joy_aimSlowLow;
static cvar_t	*joy_aimSlowStandard;
static cvar_t	*joy_aimRotLow;
static cvar_t	*joy_aimRotStandard;
static cvar_t	*joy_aimRotCap;
static cvar_t	*joy_aimFade;
static cvar_t	*joy_aimFlick;
static cvar_t	*joy_aimFlickTime;


static const aaGame_t *AA_Game( void ) {
	const char *game = FS_GetCurrentGameDir();
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( aaGames ); i++ ) {
		if ( !Q_stricmp( game, aaGames[i].game ) ) {
			return &aaGames[i];
		}
	}
	return &aaGames[0];
}


/*
==================
CL_AimAssistLocal

The hard gate: this process runs the server and player 1 is on it over
loopback (a demo, or any other server, is never local).
==================
*/
qboolean CL_AimAssistLocal( void ) {
	const clientContext_t *p1 = clx[ 0 ];

	if ( !com_sv_running || !com_sv_running->integer || !p1 ) {
		return qfalse;
	}
	if ( p1->clConn.serverAddress.type != NA_LOOPBACK || p1->clConn.demoplaying ) {
		return qfalse;
	}
	return CL_SplitRemoteServer() ? qfalse : qtrue;
}


// player n's chosen strength 0..2 (whatever the gate says)
int CL_AimAssistLevel( int n ) {
	char name[32];

	if ( !joy_aimAssist ) {
		return 0;
	}
	Com_sprintf( name, sizeof( name ), "p%i_%s", n + 1, joy_aimAssist->name );
	if ( Cvar_Flags( name ) != CVAR_NONEXISTENT ) {
		return (int)Com_Clamp( 0, 2, (float)Cvar_VariableIntegerValue( name ) );
	}
	return (int)Com_Clamp( 0, 2, (float)joy_aimAssist->integer );
}


/*
==================
CL_AimAssistOn

Player n's assist is in effect: local game, host allows it, the player
chose Low/Standard and plays with a pad.
==================
*/
qboolean CL_AimAssistOn( int n ) {
	if ( (unsigned)n >= MAX_SPLITVIEW || !cl_aimAssistAllow || !cl_aimAssistAllow->integer ) {
		return qfalse;
	}
	if ( CL_AimAssistLevel( n ) <= 0 || !IN_PadGuid( n ) ) {
		return qfalse;
	}
	return CL_AimAssistLocal();
}


/*
=============================================================================

TARGETS

=============================================================================
*/

// a configstring of the active context
static const char *AA_ConfigString( int index ) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		return "";
	}
	return cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
}


// team value of client c ('t' key), -1 unknown
static int AA_Team( const aaGame_t *g, int c ) {
	const char *s;

	if ( c < 0 || c >= MAX_CLIENTS ) {
		return -1;
	}
	s = AA_ConfigString( AA_CS_PLAYERS + c );
	if ( !s[0] ) {
		return -1;
	}
	s = Info_ValueForKey( s, g->teamKey );
	return s[0] ? atoi( s ) : 0;
}


// the gametype has no foes (UrT jump)
static qboolean AA_NoFoes( const aaGame_t *g ) {
	char gt[16];
	const char *p, *s;
	int len;

	if ( !g->noAssistGametypes[0] ) {
		return qfalse;
	}
	Q_strncpyz( gt, Info_ValueForKey( AA_ConfigString( CS_SERVERINFO ), "g_gametype" ), sizeof( gt ) );
	len = (int)strlen( gt );
	if ( !len ) {
		return qfalse;
	}
	for ( p = g->noAssistGametypes; *p; ) {
		while ( *p == ' ' ) p++;
		s = p;
		while ( *p && *p != ' ' ) p++;
		if ( p - s == len && !Q_strncmp( s, gt, len ) ) {
			return qtrue;
		}
	}
	return qfalse;
}


// position of a trajectory at time t (msec), the engine-side part of BG_EvaluateTrajectory
static void AA_Evaluate( const trajectory_t *tr, int t, vec3_t out ) {
	float dt;

	switch ( tr->trType ) {
	case TR_LINEAR:
	case TR_LINEAR_STOP:
		if ( tr->trType == TR_LINEAR_STOP && tr->trDuration > 0 && t > tr->trTime + tr->trDuration ) {
			t = tr->trTime + tr->trDuration;
		}
		dt = ( t - tr->trTime ) * 0.001f;
		if ( dt < 0.0f ) {
			dt = 0.0f;
		}
		VectorMA( tr->trBase, dt, tr->trDelta, out );
		break;
	default:	// TR_STATIONARY, TR_INTERPOLATE (players): the snapshot's position
		VectorCopy( tr->trBase, out );
		break;
	}
}


// the snapshot before the latest one whose entities are still in the ring, or NULL
static const clSnapshot_t *AA_PrevSnap( void ) {
	const clSnapshot_t *s;
	int k, num;

	for ( k = 1; k <= 3; k++ ) {
		num = cl.snap.messageNum - k;
		s = &cl.snapshots[ num & PACKET_MASK ];
		if ( s->valid && s->messageNum == num && s->serverTime < cl.snap.serverTime
			&& cl.parseEntitiesNum - s->parseEntitiesNum <= MAX_PARSE_ENTITIES - MAX_SNAPSHOT_ENTITIES ) {
			return s;
		}
	}
	return NULL;
}


static const entityState_t *AA_FindEntity( const clSnapshot_t *s, int number ) {
	const entityState_t *es;
	int i;

	if ( !s ) {
		return NULL;
	}
	for ( i = 0; i < s->numEntities; i++ ) {
		es = &cl.parseEntities[ ( s->parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 ) ];
		if ( es->number == number ) {
			return es;
		}
		if ( es->number > number ) {
			break;	// sorted by number
		}
	}
	return NULL;
}


/*
Position at cl.serverTime (what the cgame shows): interpolated between the
previous and the latest snapshot, or extrapolated (at most 100 ms) past the
latest with the entity's velocity.  p0/p1/v1 are the two snapshot
positions and the latest velocity.
*/
static void AA_Lerp( const clSnapshot_t *s0, const vec3_t p0, qboolean have0, const vec3_t p1, const vec3_t v1, vec3_t out ) {
	const int t = cl.serverTime;
	float f;

	if ( t <= cl.snap.serverTime && have0 && s0 && cl.snap.serverTime > s0->serverTime
		&& DistanceSquared( p0, p1 ) < 256.0f * 256.0f ) {
		f = (float)( t - s0->serverTime ) / (float)( cl.snap.serverTime - s0->serverTime );
		f = Com_Clamp( 0.0f, 1.0f, f );
		out[0] = p0[0] + ( p1[0] - p0[0] ) * f;
		out[1] = p0[1] + ( p1[1] - p0[1] ) * f;
		out[2] = p0[2] + ( p1[2] - p0[2] ) * f;
		return;
	}
	f = Com_Clamp( -0.1f, 0.1f, ( t - cl.snap.serverTime ) * 0.001f );
	VectorMA( p1, f, v1, out );
}


typedef struct {
	int		num;
	vec3_t	chest;
	float	dist;
	float	ang;		// angle between the view and the chest point, degrees
	float	bubble;		// bubble radius, degrees
	float	ratio;		// ang / bubble
} aaCand_t;


static float AA_AngleTo( const vec3_t view, const vec3_t dir ) {
	float d = DotProduct( view, dir );
	return RAD2DEG( acosf( Com_Clamp( -1.0f, 1.0f, d ) ) );
}


/*
==================
AA_FindTarget

The best target of the active context: inside its bubble, nearest to the
bubble center, with line of sight.  qfalse = none.
==================
*/
static qboolean AA_FindTarget( const aaGame_t *g, const vec3_t eye, const vec3_t forward, aaCand_t *best, int *traces ) {
	const clSnapshot_t *s0 = AA_PrevSnap();
	const entityState_t *es, *es0;
	aaCand_t cands[AA_MAX_CANDIDATES], c;
	int numCands = 0, i, j, myTeam, team;
	vec3_t p0, p1, v1, pos, dir, aim, aimDir;
	float range, bubbleUnits, bubbleMax, horiz, fxy, z;
	trace_t tr;

	*traces = 0;
	VectorClear( p0 );
	myTeam = AA_Team( g, clc.clientNum );
	if ( myTeam == g->spectator || AA_NoFoes( g ) ) {
		return qfalse;
	}
	range = joy_aimRange->value;
	bubbleUnits = joy_aimBubble->value;
	bubbleMax = joy_aimBubbleMax->value;

	for ( i = 0; i < cl.snap.numEntities; i++ ) {
		es = &cl.parseEntities[ ( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 ) ];
		if ( es->number >= MAX_CLIENTS ) {
			break;	// sorted: the rest are not players (corpses, items, missiles)
		}
		if ( es->number == clc.clientNum || es->eType != g->playerType || ( es->eFlags & g->deadFlag ) ) {
			continue;
		}
		team = AA_Team( g, es->number );
		if ( team < 0 || team == g->spectator ) {
			continue;
		}
		if ( myTeam > 0 && team == myTeam ) {
			continue;	// teammate (team 0 = free-for-all: everyone is a foe)
		}

		AA_Evaluate( &es->pos, cl.snap.serverTime, p1 );
		VectorCopy( es->pos.trDelta, v1 );
		if ( es->pos.trType == TR_STATIONARY ) {
			VectorClear( v1 );
		}
		es0 = AA_FindEntity( s0, es->number );
		if ( es0 ) {
			AA_Evaluate( &es0->pos, s0->serverTime, p0 );
		}
		AA_Lerp( s0, p0, es0 != NULL, p1, v1, pos );
		VectorCopy( pos, aim );
		pos[2] += g->chestZ;

		VectorSubtract( pos, eye, dir );
		c.dist = VectorNormalize( dir );
		if ( c.dist > range || c.dist < 1.0f ) {
			continue;
		}
		c.bubble = MIN( RAD2DEG( atan2f( bubbleUnits, c.dist ) ), bubbleMax );

		// angle from the view to the nearest point of the body's vertical axis
		// (the view ray's height at the target's distance, clamped feet..head)
		horiz = sqrtf( ( aim[0] - eye[0] ) * ( aim[0] - eye[0] ) + ( aim[1] - eye[1] ) * ( aim[1] - eye[1] ) );
		fxy = sqrtf( forward[0] * forward[0] + forward[1] * forward[1] );
		z = ( fxy > 0.01f ) ? eye[2] + forward[2] / fxy * horiz : pos[2];
		aim[2] = Com_Clamp( aim[2] + g->bodyLow, aim[2] + g->bodyHigh, z );
		VectorSubtract( aim, eye, aimDir );
		VectorNormalize( aimDir );
		c.ang = AA_AngleTo( forward, aimDir );
		if ( cl_aimAssistDebug->integer >= 3 ) {
			Com_Printf( "AA   cand %i dist %.0f ang %.2f bubble %.2f pos %.0f %.0f %.0f eye %.0f %.0f %.0f\n", es->number, c.dist, c.ang,
				c.bubble, pos[0], pos[1], pos[2], eye[0], eye[1], eye[2] );
		}
		if ( c.ang >= c.bubble ) {
			continue;	// outside its bubble: contributes nothing
		}
		c.num = es->number;
		c.ratio = c.ang / c.bubble;
		VectorCopy( pos, c.chest );

		// keep the nearest AA_MAX_CANDIDATES by bubble position
		for ( j = numCands; j > 0 && cands[j-1].ratio > c.ratio; j-- ) {
			if ( j < AA_MAX_CANDIDATES ) {
				cands[j] = cands[j-1];
			}
		}
		if ( j < AA_MAX_CANDIDATES ) {
			cands[j] = c;
			if ( numCands < AA_MAX_CANDIDATES ) {
				numCands++;
			}
		}
	}

	// line of sight, best first: at most one trace per candidate
	for ( i = 0; i < numCands; i++ ) {
		CM_BoxTrace( &tr, eye, cands[i].chest, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse );
		( *traces )++;
		if ( tr.fraction >= 1.0f && !tr.startsolid ) {
			*best = cands[i];
			return qtrue;
		}
	}
	return qfalse;
}


static float AA_Approach( float v, float goal, float step ) {
	if ( v < goal ) {
		return MIN( v + step, goal );
	}
	return MAX( v - step, goal );
}


void CL_AimAssistReset( int n ) {
	if ( (unsigned)n < MAX_SPLITVIEW ) {
		aa[n].fade = aa[n].falloff = 0.0f;
		aa[n].target = -1;
		aa[n].omega[0] = aa[n].omega[1] = 0.0f;
	}
}


/*
==================
CL_AimAssistApply

From CL_GamepadMove, active context = player n, after the aim curve / boost /
zoom / smoothing: *yawRate / *pitchRate are the look stick's rates in deg/s
(CL_GamepadMove's signs: viewangles[YAW] -= yawRate * dt, viewangles[PITCH]
+= pitchRate * dt).  moveInput / lookInput: that stick is outside its
deadzone.  Returns with the rates untouched unless the assist is in effect.
==================
*/
void CL_AimAssistApply( int n, qboolean moveInput, qboolean lookInput, float *yawRate, float *pitchRate, float msec ) {
	aaPlayer_t *p;
	const aaGame_t *g;
	const playerState_t *ps;
	const clSnapshot_t *s0;
	aaCand_t t;
	vec3_t eye, eye0, viewAng, forward, dir, ang;
	float dt, stickYaw, stickPitch, slowMin, rotFrac, slow, edge, k, rot[2], raw[2], cap, len, x;
	int level, traces = 0, i;
	qboolean found = qfalse, rotOn = qfalse, observe;
	int64_t us0 = 0;

	if ( (unsigned)n >= MAX_SPLITVIEW ) {
		return;
	}
	p = &aa[n];
	Com_Memset( &t, 0, sizeof( t ) );
	stickYaw = *yawRate;
	stickPitch = *pitchRate;
	slow = 1.0f;
	rot[0] = rot[1] = 0.0f;

	// cl_aimAssistDebug with level Off in a local game: targets are found and logged, nothing applied
	observe = ( !CL_AimAssistOn( n ) && cl_aimAssistDebug->integer > 0 && CL_AimAssistLevel( n ) == 0 && cl_aimAssistAllow->integer
		&& IN_PadGuid( n ) && CL_AimAssistLocal() ) ? qtrue : qfalse;
	if ( ( !observe && !CL_AimAssistOn( n ) ) || cls.state != CA_ACTIVE || !cl.snap.valid || CM_NumInlineModels() <= 0 ) {
		CL_AimAssistReset( n );
		if ( cl_aimAssistDebug->integer > 0 && ( lookInput || moveInput || cl_aimAssistDebug->integer >= 2 ) ) {
			Com_Printf( "AA P%i off (%s, level %i): slow %.3f rot %.2f %.2f stick %.2f -> yaw %.2f deg/s\n", n + 1,
				CL_AimAssistLocal() ? ( cl_aimAssistAllow->integer ? ( IN_PadGuid( n ) ? "level 0" : "no pad" ) : "host forbids" ) : "not a local game",
				CL_AimAssistLevel( n ), slow, rot[0], rot[1], -stickYaw, -*yawRate );
		}
		return;
	}
	if ( cl_aimAssistDebug->integer ) {
		us0 = Sys_Microseconds();
	}

	dt = msec * 0.001f;
	g = AA_Game();
	ps = &cl.snap.ps;
	level = CL_AimAssistLevel( n );
	slowMin = ( level >= 2 ) ? joy_aimSlowStandard->value : ( level == 1 ) ? joy_aimSlowLow->value : 1.0f;
	rotFrac = ( level >= 2 ) ? joy_aimRotStandard->value : ( level == 1 ) ? joy_aimRotLow->value : 0.0f;

	// own eye at cl.serverTime; the view = the angles sent + the server's delta
	if ( ps->pm_type != AA_PM_NORMAL || ps->clientNum != clc.clientNum ) {
		CL_AimAssistReset( n );	// dead, spectating, following someone, intermission
	} else {
		s0 = AA_PrevSnap();
		VectorClear( eye0 );
		if ( s0 ) {
			VectorCopy( s0->ps.origin, eye0 );
		}
		AA_Lerp( s0, eye0, s0 != NULL, ps->origin, ps->velocity, eye );
		eye[2] += ps->viewheight;
		for ( i = 0; i < 3; i++ ) {
			viewAng[i] = cl.viewangles[i] + SHORT2ANGLE( ps->delta_angles[i] );
		}
		AngleVectors( viewAng, forward, NULL, NULL );

		found = AA_FindTarget( g, eye, forward, &t, &traces );
		p->fade = AA_Approach( p->fade, found ? 1.0f : 0.0f, joy_aimFade->value > 0.0f ? msec / joy_aimFade->value : 1.0f );

		if ( found ) {
			// slowdown: slowMin over the bubble's core (inner AA_CORE of its radius),
			// smoothstep back to 1 at the edge (R14a: was smoothstep from the very center,
			// which left only a sliver of the bubble at full strength)
			x = Com_Clamp( 0.0f, 1.0f, ( t.ratio - AA_CORE ) / ( 1.0f - AA_CORE ) );
			p->falloff = 1.0f - x * x * ( 3.0f - 2.0f * x );

			// the target's angular velocity around the eye (world angles, so the
			// player's own turning is not in it), low-passed over ~50 ms
			VectorSubtract( t.chest, eye, dir );
			vectoangles( dir, ang );
			raw[0] = ( dt > 0.0f ) ? AngleDelta( ang[YAW], p->lastAng[0] ) / dt : 0.0f;
			raw[1] = ( dt > 0.0f ) ? AngleDelta( ang[PITCH], p->lastAng[1] ) / dt : 0.0f;
			if ( p->target == t.num && dt > 0.0f && fabsf( raw[0] ) < AA_MAX_OMEGA && fabsf( raw[1] ) < AA_MAX_OMEGA ) {
				k = MIN( 1.0f, dt / 0.05f );
				p->omega[0] += ( raw[0] - p->omega[0] ) * k;
				p->omega[1] += ( raw[1] - p->omega[1] ) * k;
			} else {
				// new target, or a jump no running player makes (teleport, respawn)
				p->omega[0] = p->omega[1] = 0.0f;	// new target: measure from here
			}
			p->target = t.num;
			p->lastAng[0] = ang[YAW];
			p->lastAng[1] = ang[PITCH];
		} else {
			p->target = -1;
			p->omega[0] = p->omega[1] = 0.0f;
		}
		// a fast flick switches the assist off for a moment: sweeping past a
		// target is never slowed, nothing pulls after it
		if ( sqrtf( stickYaw * stickYaw + stickPitch * stickPitch ) > joy_aimFlick->value ) {
			p->flickUntil = cls.realtime + joy_aimFlickTime->integer;
		}
		if ( cls.realtime >= p->flickUntil ) {
			slow = 1.0f - ( 1.0f - slowMin ) * p->fade * p->falloff;
		}

		// rotational: inside the bubble, with input, not right after a flick
		if ( found && ( moveInput || lookInput ) && cls.realtime >= p->flickUntil ) {
			edge = Com_Clamp( 0.0f, 1.0f, ( 1.0f - t.ratio ) / 0.3f );
			rot[0] = rotFrac * p->fade * edge * p->omega[0];
			rot[1] = rotFrac * p->fade * edge * p->omega[1];
			cap = joy_aimRotCap->value;
			len = sqrtf( rot[0] * rot[0] + rot[1] * rot[1] );
			if ( len > cap && len > 0.0f ) {
				rot[0] *= cap / len;
				rot[1] *= cap / len;
			}
			rotOn = qtrue;
		}
	}

	*yawRate = stickYaw * slow - rot[0];		// viewangles[YAW] -= yawRate * dt
	*pitchRate = stickPitch * slow + rot[1];	// viewangles[PITCH] += pitchRate * dt

	if ( cl_aimAssistDebug->integer ) {
		p->usTotal += Sys_Microseconds() - us0;
		p->usCalls++;
		if ( cl_aimAssistDebug->integer > 0 && ( lookInput || moveInput || found || cl_aimAssistDebug->integer >= 2 ) ) {
			Com_Printf( "AA P%i lvl %i t %i tgt %i dist %.0f ang %.2f bub %.2f los %i fade %.2f slow %.3f omega %.1f %.1f rot %.2f %.2f%s traces %i stick %.2f -> yaw %.2f deg/s\n",
				n + 1, level, cl.serverTime, found ? t.num : -1, found ? t.dist : 0.0f, found ? t.ang : 0.0f, found ? t.bubble : 0.0f,
				found ? 1 : 0, p->fade, slow, p->omega[0], p->omega[1], rot[0], rot[1],
				rotOn ? "" : cls.realtime < p->flickUntil ? " (flick)" : !found ? " (no target)" : " (no input)", traces, -stickYaw, -*yawRate );
		}
	}
}


/*
=============================================================================

MARKER (name suffix through the userinfo) AND CELL GLYPH

=============================================================================
*/

// player n's marker text ("" = none): the game's per-level marker for "auto"
static const char *AA_MarkerText( int n ) {
	static char buf[32];
	const char *s = cl_aimAssistMarker->string;
	int i, j;

	if ( !Q_stricmp( s, "auto" ) ) {
		s = ( CL_AimAssistLevel( n ) >= 2 ) ? AA_Game()->markerStd : AA_Game()->marker;
	} else if ( !Q_stricmp( s, "0" ) ) {
		return "";
	}
	// nothing that breaks an info string
	for ( i = j = 0; s[i] && j < (int)sizeof( buf ) - 1; i++ ) {
		if ( s[i] != '\\' && s[i] != '"' && s[i] != ';' ) {
			buf[j++] = s[i];
		}
	}
	buf[j] = '\0';
	return buf;
}


/*
==================
CL_AimAssistUserinfo

Player n's outgoing userinfo: the name gets the marker while its assist is
on.  The name cvar itself is never changed (profiles, q3config and the mod's
menus keep the clean name).
==================
*/
const char *CL_AimAssistUserinfo( int n, const char *info ) {
	static char out[MAX_INFO_STRING];
	char name[MAX_INFO_VALUE];
	const char *marker;

	if ( !CL_AimAssistOn( n ) ) {
		return info;
	}
	marker = AA_MarkerText( n );
	if ( !marker[0] ) {
		return info;
	}
	Q_strncpyz( out, info, sizeof( out ) );
	Q_strncpyz( name, Info_ValueForKey( info, "name" ), sizeof( name ) );
	Q_strcat( name, sizeof( name ), marker );
	Info_SetValueForKey( out, "name", name );
	return out;
}


// player n's effective assist state as one line ("" = not playing a level now)
static void AA_StateText( int n, char *buf, int size ) {
	const int level = CL_AimAssistLevel( n );
	const char *src = CL_ProfileIsGuest( n ) ? "Guest" : CL_ProfileName( n )[0] ? va( "profile \"%s\"", CL_ProfileName( n ) ) : "shared joy_aimAssist";

	buf[0] = '\0';
	if ( cls.state != CA_ACTIVE || !CL_SplitSlotActive( n ) ) {
		return;
	}
	if ( !IN_PadGuid( n ) ) {
		Q_strncpyz( buf, "off: no pad (keyboard/mouse)", size );
	} else if ( !CL_AimAssistLocal() ) {
		Q_strncpyz( buf, "off: not a local game (only games this PC hosts)", size );
	} else if ( !cl_aimAssistAllow->integer ) {
		Q_strncpyz( buf, "off: the host does not allow it (cl_aimAssistAllow 0)", size );
	} else if ( level <= 0 ) {
		Com_sprintf( buf, size, "off (%s)", src );
	} else {
		Com_sprintf( buf, size, "%s (%s)", level >= 2 ? "Standard" : "Low", src );
	}
}


/*
==================
CL_AimAssistFrame

Once per frame: a line in the console/log whenever a player's effective
assist changes ("AA P1: Low (Guest)", "AA P2: off: not a local game ...");
player 1's marker change makes its userinfo go out again (the extras'
userinfo is polled and compared already).
==================
*/
void CL_AimAssistFrame( void ) {
	char state[sizeof( aa[0].state )];
	int n, on;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		AA_StateText( n, state, sizeof( state ) );
		if ( strcmp( state, aa[n].state ) ) {
			Q_strncpyz( aa[n].state, state, sizeof( aa[n].state ) );
			if ( state[0] ) {
				Com_Printf( "AA P%i: %s\n", CL_IndepChildPlayer() > 0 ? CL_IndepChildPlayer() : n + 1, state );
			}
		}
		on = ( CL_AimAssistOn( n ) && AA_MarkerText( n )[0] ) ? CL_AimAssistLevel( n ) : 0;
		if ( on != aa[n].markerOn ) {
			aa[n].markerOn = on;
			if ( cl_aimAssistDebug->integer > 0 ) {
				Com_Printf( "AA P%i marker %s\n", n + 1, on ? AA_MarkerText( n ) : "off" );
			}
			if ( n == 0 ) {
				cvar_modifiedFlags |= CVAR_USERINFO;
			}
		}
	}
}


/*
==================
CL_AimAssistDraw

A small '+' in the top right corner of the cell of every player whose
assist is on (screen pass, after the cgames): yellow = Low, green = Standard.
==================
*/
void CL_AimAssistDraw( void ) {
	static const vec4_t bg = { 0.0f, 0.0f, 0.0f, 0.5f };
	viewRect_t r;
	int n, x, y;

	if ( cls.state != CA_ACTIVE ) {
		return;
	}
	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( !CL_SplitSlotActive( n ) || CL_SplitMenuOpen( n ) || !CL_AimAssistOn( n ) ) {
			continue;
		}
		CL_SplitViewRect( n, &r );
		if ( r.w <= 0 || r.h <= 0 ) {
			continue;
		}
		x = r.x + r.w - smallchar_width * 3;
		y = r.y + smallchar_height;
		re.SetColor( bg );
		re.DrawStretchPic( x - smallchar_width / 2, y - smallchar_height / 4, smallchar_width * 2, smallchar_height * 3 / 2,
			0, 0, 0, 0, cls.whiteShader );
		re.SetColor( NULL );
		SCR_DrawSmallStringExt( x, y, "+", g_color_table[ ColorIndex( CL_AimAssistLevel( n ) >= 2 ? COLOR_GREEN : COLOR_YELLOW ) ], qtrue, qtrue );
	}
}


static void AA_Stats_f( void ) {
	int n;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		if ( aa[n].usCalls ) {
			Com_Printf( "AA P%i: %i calls, %.2f us per call\n", n + 1, aa[n].usCalls, (double)aa[n].usTotal / aa[n].usCalls );
		}
		aa[n].usCalls = 0;
		aa[n].usTotal = 0;
	}
}


/*
==================
AA_TestPlace_f

aimassist_testplace <player 2-8> <distance> <view offset deg> [wall]
(test scripts; developer or sv_cheats): from player 1's position, the
direction with the longest clear run; player 1 is turned to face it plus
the offset, player <p> is put <distance> units along it, facing player 1
('setviewpos', a cheat of the game).  'wall': the other player goes behind
the nearest wall instead (no line of sight), player 1 faces it.
==================
*/
// along yaw from o: the first open spot with a floor past the first wall, hidden
// from the eye at o + viewheight; *spotTr = its floor trace, *free = wall distance
static qboolean AA_BehindWall( const vec3_t o, int viewheight, float yaw, const vec3_t mins, const vec3_t maxs,
	trace_t *spotTr, float *free ) {
	vec3_t dir, end, spot, down, eye, chest;
	trace_t tr, los;
	int k;

	dir[0] = cosf( DEG2RAD( yaw ) );
	dir[1] = sinf( DEG2RAD( yaw ) );
	dir[2] = 0.0f;
	VectorMA( o, 2000.0f, dir, end );
	CM_BoxTrace( &tr, o, end, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
	*free = tr.fraction * 2000.0f;
	if ( tr.fraction >= 1.0f || *free < 24.0f ) {
		return qfalse;
	}
	VectorCopy( o, eye );
	eye[2] += viewheight;
	for ( k = 0; k < 40; k++ ) {
		VectorMA( o, *free + 48.0f + k * 16.0f, dir, spot );
		CM_BoxTrace( &tr, spot, spot, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
		if ( tr.startsolid || tr.allsolid ) {
			continue;
		}
		VectorCopy( spot, down );
		down[2] -= 256.0f;
		CM_BoxTrace( &tr, spot, down, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
		if ( tr.fraction >= 1.0f || tr.startsolid ) {
			continue;
		}
		VectorCopy( tr.endpos, chest );
		chest[2] += AA_Game()->chestZ;
		CM_BoxTrace( &los, eye, chest, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse );
		if ( los.fraction < 1.0f ) {
			*spotTr = tr;
			return qtrue;
		}
		return qfalse;	// the first open spot is visible: a low obstacle, not a wall
	}
	return qfalse;
}


static void AA_TestPlace_f( void ) {
	static const vec3_t mins = { -15, -15, -24 }, maxs = { 15, 15, 32 };
	const playerState_t *ps;
	vec3_t o, dir, end, spot, down;
	trace_t tr, wallTr;
	float yaw, bestYaw = 0.0f, bestFree = -1.0f, dist, offset, free;
	qboolean wall;
	int p, k, side;

	if ( !Cvar_VariableIntegerValue( "sv_cheats" ) && !( com_developer && com_developer->integer ) ) {
		Com_Printf( "aimassist_testplace: needs sv_cheats or developer\n" );
		return;
	}
	if ( Cmd_Argc() < 4 ) {
		Com_Printf( "usage: aimassist_testplace <player 2-8> <distance> <view offset deg> [wall]\n" );
		return;
	}
	p = atoi( Cmd_Argv( 1 ) ) - 1;
	dist = atof( Cmd_Argv( 2 ) );
	offset = atof( Cmd_Argv( 3 ) );
	wall = !Q_stricmp( Cmd_Argv( 4 ), "wall" );
	if ( p < 1 || p >= MAX_SPLITVIEW || !clx[ 0 ] || !clx[ 0 ]->clActive.snap.valid || CM_NumInlineModels() <= 0 ) {
		Com_Printf( "aimassist_testplace: needs player 1 in a game and a player 2-8\n" );
		return;
	}
	ps = &clx[ 0 ]->clActive.snap.ps;
	VectorCopy( ps->origin, o );
	Com_Memset( &wallTr, 0, sizeof( wallTr ) );

	for ( yaw = 0.0f; yaw < 360.0f; yaw += 10.0f ) {
		if ( wall ) {
			break;	// below
		}
		dir[0] = cosf( DEG2RAD( yaw ) );
		dir[1] = sinf( DEG2RAD( yaw ) );
		dir[2] = 0.0f;
		VectorMA( o, 2000.0f, dir, end );
		CM_BoxTrace( &tr, o, end, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
		free = tr.fraction * 2000.0f;
		if ( !wall ) {
			// open: room for the distance and a floor under the spot (no ledges, no void)
			if ( free < dist + 40.0f ) {
				continue;
			}
			VectorMA( o, dist, dir, spot );
			VectorCopy( spot, down );
			down[2] -= 48.0f;
			CM_BoxTrace( &tr, spot, down, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
			if ( tr.fraction >= 1.0f || tr.startsolid ) {
				continue;
			}
			// and room for player 1 to strafe 120 units to its left (r12-aim D), on a floor
			for ( side = 1; side <= 1; side += 2 ) {
				vec3_t perp;
				perp[0] = -dir[1] * side;
				perp[1] = dir[0] * side;
				perp[2] = 0.0f;
				VectorMA( o, 120.0f, perp, end );
				CM_BoxTrace( &tr, o, end, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
				if ( tr.fraction < 1.0f ) {
					break;
				}
				VectorCopy( end, down );
				down[2] -= 48.0f;
				CM_BoxTrace( &tr, end, down, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
				if ( tr.fraction >= 1.0f || tr.startsolid ) {
					break;
				}
			}
			if ( side <= 1 ) {
				continue;
			}
		}
		if ( free > bestFree ) {
			bestFree = free;
			bestYaw = yaw;
		}
	}
	dir[0] = cosf( DEG2RAD( bestYaw ) );
	dir[1] = sinf( DEG2RAD( bestYaw ) );
	dir[2] = 0.0f;

	if ( wall ) {
		// the direction with the nearest wall that has an open spot with a floor
		// behind it, hidden from player 1's eye
		for ( k = 0; k < 36; k++ ) {
			if ( AA_BehindWall( o, ps->viewheight, k * 10.0f, mins, maxs, &tr, &free ) && ( bestFree < 0.0f || free < bestFree ) ) {
				bestFree = free;
				bestYaw = k * 10.0f;
				wallTr = tr;
			}
		}
		if ( bestFree < 0.0f ) {
			Com_Printf( "aimassist_testplace: no open spot hidden behind a wall\n" );
			return;
		}
		tr = wallTr;
		offset = 0.0f;
	} else {
		if ( bestFree < 0.0f ) {
			Com_Printf( "aimassist_testplace: no direction with %.0f clear units and a floor\n", dist );
			return;
		}
		VectorMA( o, dist, dir, spot );
		VectorCopy( spot, down );
		down[2] -= 256.0f;
		CM_BoxTrace( &tr, spot, down, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
	}
	Com_Printf( "aimassist_testplace: P1 at %.0f %.0f %.0f facing %.0f; P%i at %.0f %.0f %.0f (%s, yaw %.0f, %.0f units clear)\n",
		o[0], o[1], o[2], bestYaw + offset, p + 1, tr.endpos[0], tr.endpos[1], tr.endpos[2],
		wall ? "behind a wall" : "in the open", bestYaw, bestFree );
	// player 1 only turns (its own view angles: a teleport would make it slide);
	// the other one is teleported facing away (it slides a little further off),
	// now (a queued command can end up behind other script text)
	clx[ 0 ]->clActive.viewangles[YAW] = AngleNormalize360( bestYaw + offset - SHORT2ANGLE( ps->delta_angles[YAW] ) );
	clx[ 0 ]->clActive.viewangles[PITCH] = -SHORT2ANGLE( ps->delta_angles[PITCH] );
	Cmd_ExecuteString( va( "p%i setviewpos %.1f %.1f %.1f %.1f", p + 1, tr.endpos[0], tr.endpos[1], tr.endpos[2], bestYaw ) );
}


// aimassist_ents: the player entities of the active player's snapshot (convention checks per game)
static void AA_Ents_f( void ) {
	const aaGame_t *g = AA_Game();
	const entityState_t *es;
	int i;

	if ( !cl.snap.valid ) {
		Com_Printf( "aimassist_ents: no snapshot\n" );
		return;
	}
	Com_Printf( "AA ents P%i (client %i, team %i, pm_type %i, game %s, g_gametype %s): %i entities\n", cla->playerNum + 1,
		clc.clientNum, AA_Team( g, clc.clientNum ), cl.snap.ps.pm_type, g->game,
		Info_ValueForKey( AA_ConfigString( CS_SERVERINFO ), "g_gametype" ), cl.snap.numEntities );
	for ( i = 0; i < cl.snap.numEntities; i++ ) {
		es = &cl.parseEntities[ ( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 ) ];
		if ( es->number >= MAX_CLIENTS && es->eType != g->playerType ) {
			continue;
		}
		Com_Printf( "  ent %i eType %i eFlags 0x%x clientNum %i team %i trType %i origin %.0f %.0f %.0f\n", es->number, es->eType,
			es->eFlags, es->clientNum, AA_Team( g, es->number ), es->pos.trType, es->pos.trBase[0], es->pos.trBase[1], es->pos.trBase[2] );
	}
}


void CL_AimAssistInit( void ) {
	int n;

	for ( n = 0; n < MAX_SPLITVIEW; n++ ) {
		CL_AimAssistReset( n );
		aa[n].markerOn = 0;
		aa[n].state[0] = '\0';
	}

	cl_aimAssistAllow = Cvar_Get( "cl_aimAssistAllow", "1", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( cl_aimAssistAllow, "0", "1", CV_INTEGER );
	Cvar_SetDescription( cl_aimAssistAllow, "Splitscreen host setting: 0 - no pad aim assist for anybody this session. "
		"Aim assist only ever works in games this machine hosts." );
	cl_aimAssistMarker = Cvar_Get( "cl_aimAssistMarker", "auto", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_aimAssistMarker, "Name suffix of players using aim assist (auto - \" ^3+\" Low, \" ^2+\" Standard, UrT \"+\"; 0 - none). "
		"Everyone sees it on the scoreboard." );
	cl_aimAssistDebug = Cvar_Get( "cl_aimAssistDebug", "0", CVAR_TEMP );
	Cvar_SetDescription( cl_aimAssistDebug, "Print the aim assist of every pad player each frame (1 - with input or a target, 2 - always, 3 - plus candidates; -1 - only time it, see aimassist_stats)." );

	// the per-player strength is a pad feel setting (in_gamepad.c makes p<N>_ / guest_ copies)
	joy_aimAssist = Cvar_Get( "joy_aimAssist", "0", CVAR_ARCHIVE_ND );	// R17: Off (Guests, new profiles); was Low
	Cvar_CheckRange( joy_aimAssist, "0", "2", CV_INTEGER );
	Cvar_SetDescription( joy_aimAssist, "Gamepad aim assist in local games: 0 - off (default), 1 - low, 2 - standard. Per player: p<N>_joy_aimAssist." );

	joy_aimRange = Cvar_Get( "joy_aimRange", "1500", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimRange, "100", "8192", CV_FLOAT );
	Cvar_SetDescription( joy_aimRange, "Aim assist: farthest target, game units." );
	joy_aimBubble = Cvar_Get( "joy_aimBubble", "72", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimBubble, "5", "200", CV_FLOAT );
	Cvar_SetDescription( joy_aimBubble, "Aim assist: bubble radius around a target's chest, game units (its angle shrinks with distance)." );
	joy_aimBubbleMax = Cvar_Get( "joy_aimBubbleMax", "12", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimBubbleMax, "1", "30", CV_FLOAT );
	Cvar_SetDescription( joy_aimBubbleMax, "Aim assist: largest bubble radius, degrees (close targets)." );
	joy_aimSlowLow = Cvar_Get( "joy_aimSlowLow", "0.64", CVAR_ARCHIVE_ND );	// R17: 0.7 -> 0.64 (a quarter of the way to Standard)
	Cvar_CheckRange( joy_aimSlowLow, "0.3", "1", CV_FLOAT );
	Cvar_SetDescription( joy_aimSlowLow, "Aim assist Low: look speed multiplier over the bubble's core (inner 40%)." );
	joy_aimSlowStandard = Cvar_Get( "joy_aimSlowStandard", "0.45", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimSlowStandard, "0.3", "1", CV_FLOAT );
	Cvar_SetDescription( joy_aimSlowStandard, "Aim assist Standard: look speed multiplier over the bubble's core (inner 40%)." );
	joy_aimRotLow = Cvar_Get( "joy_aimRotLow", "0.375", CVAR_ARCHIVE_ND );	// R17: 0.3 -> 0.375
	Cvar_CheckRange( joy_aimRotLow, "0", "0.8", CV_FLOAT );
	Cvar_SetDescription( joy_aimRotLow, "Aim assist Low: fraction of a target's angular velocity added while you move or look." );
	joy_aimRotStandard = Cvar_Get( "joy_aimRotStandard", "0.6", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimRotStandard, "0", "0.8", CV_FLOAT );
	Cvar_SetDescription( joy_aimRotStandard, "Aim assist Standard: fraction of a target's angular velocity added while you move or look." );
	joy_aimRotCap = Cvar_Get( "joy_aimRotCap", "120", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimRotCap, "0", "180", CV_FLOAT );
	Cvar_SetDescription( joy_aimRotCap, "Aim assist: most the rotational assist turns, degrees per second." );
	joy_aimFade = Cvar_Get( "joy_aimFade", "100", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimFade, "0", "1000", CV_FLOAT );
	Cvar_SetDescription( joy_aimFade, "Aim assist: milliseconds to fade in on a target / out after losing it." );
	joy_aimFlick = Cvar_Get( "joy_aimFlick", "300", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimFlick, "50", "2000", CV_FLOAT );
	Cvar_SetDescription( joy_aimFlick, "Aim assist: look-stick turn rate (deg/s) that counts as a flick (rotation pauses)." );
	joy_aimFlickTime = Cvar_Get( "joy_aimFlickTime", "150", CVAR_ARCHIVE_ND );
	Cvar_CheckRange( joy_aimFlickTime, "0", "1000", CV_INTEGER );
	Cvar_SetDescription( joy_aimFlickTime, "Aim assist: milliseconds rotation stays off after a flick." );

	Cmd_AddCommand( "aimassist_stats", AA_Stats_f );
	Cmd_AddCommand( "aimassist_ents", AA_Ents_f );
	Cmd_AddCommand( "aimassist_testplace", AA_TestPlace_f );
}


void CL_AimAssistShutdown( void ) {
	Cmd_RemoveCommand( "aimassist_stats" );
	Cmd_RemoveCommand( "aimassist_ents" );
	Cmd_RemoveCommand( "aimassist_testplace" );
}
