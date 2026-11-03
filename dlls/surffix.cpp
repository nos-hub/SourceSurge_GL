// surffix.cpp - Surf Fix, port of the AMX Mod X plugin "Surf Fix" v1.9 by Lopol2010 (server side, put in dlls/)
//
// The surf bug: while sliding or surfing on a slope the player sometimes loses ALL horizontal speed for a frame
// and stops dead. This fixes it:
//   1. CmdStart  - remember the velocity the player had before the movement of this command.
//   2. CmdEnd    - if the player had horizontal speed before, has none after, and is touching a slope of any angle,
//                  put the speed back and lift the player 0.01 units off the surface.
//
// Details kept from the original:
//   - the player's base velocity (trigger_push) is subtracted before the "no speed" test, because a push trigger
//     changes velocity even when the player does not visibly move;
//   - the lift is skipped when the spot above would be inside a wall (so the player is never stuck in a ceiling);
//   - the slope test uses a hull trace only 0.01 units below the player: any larger distance makes the fix give
//     the player the speed he had before a teleport onto a slope.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "surffix.h"
#include <math.h>


static float g_flNextSurfFixHeartbeat[33];
// How high to lift the player after the bug. More than 1 would be noticeable and break surfing.
constexpr float SURFFIX_UNSTUCK_HEIGHT = 0.01f;

// How far below the player to look for a slope. Do not raise it (see above).
constexpr float SURFFIX_TRACE_HEIGHT = 0.01f;

// Prints "[surffix] fixed" to the player's console each time the fix fires (set to false once it works).
constexpr bool SURFFIX_DEBUG = false;

static Vector g_StartVelocity[33]; // velocity at the start of the command, per player (entity index)

static inline bool IsVectorZero2D(const Vector& v)
{
	return v.x == 0.0f && v.y == 0.0f;
}

// A living player for this edict, or NULL

static CBasePlayer* GetLivingPlayer(const edict_t* ent)
{
	if (!ent)
		return NULL;

	CBaseEntity* pEntity = CBaseEntity::Instance(const_cast<edict_t*>(ent));
	if (!pEntity || !pEntity->IsPlayer() || !pEntity->IsAlive())
		return NULL;

	const int idx = pEntity->entindex();
	if (idx < 1 || idx > 32)
		return NULL;

	return static_cast<CBasePlayer*>(pEntity);
}

// The player's hull at this position fits (not buried in a wall).
static bool IsHullVacant(const Vector& origin, int hull, edict_t* ent)
{
	TraceResult tr;
	TRACE_HULL(origin, origin, dont_ignore_monsters, hull, ent, &tr);

	return !(tr.fStartSolid && tr.fAllSolid);
}

// Touching a slope of ANY angle (surface normal neither flat floor nor vertical wall).
static bool IsPlayerOnSlope(CBasePlayer* pPlayer)
{
	entvars_t* pev = pPlayer->pev;

	const Vector origin = pev->origin;
	Vector end = origin;
	end.z -= SURFFIX_TRACE_HEIGHT;

	const int hull = (pev->flags & FL_DUCKING) ? head_hull : human_hull;

	TraceResult tr;
	TRACE_HULL(origin, end, ignore_monsters | missile, hull, ENT(pev), &tr);

	if (tr.flFraction >= 1.0f)
		return false; // nothing below: no plane

	const float nz = tr.vecPlaneNormal.z;
	return nz != 0.0f && nz != 1.0f;
}

void SurfFix_CmdStart(const edict_t* player)
{
	CBasePlayer* pPlayer = GetLivingPlayer(player);
	if (!pPlayer)
		return;

	g_StartVelocity[pPlayer->entindex()] = pPlayer->pev->velocity;
}

void SurfFix_CmdEnd(const edict_t* player)
{
	CBasePlayer* pPlayer = GetLivingPlayer(player);
	if (!pPlayer)
		return;

	const int idx = pPlayer->entindex();
	entvars_t* pev = pPlayer->pev;
	const Vector startVelocity = g_StartVelocity[idx];

	// Heartbeat every 2 seconds
	if (SURFFIX_DEBUG)
	{
		const float now = gpGlobals->time;
		if (now >= g_flNextSurfFixHeartbeat[idx])
		{
			g_flNextSurfFixHeartbeat[idx] = now + 2.0f;
			
		}
	}

	// cheap test first: a player who had no horizontal speed at the start cannot have "lost" it
	if (IsVectorZero2D(startVelocity))
		return;

	Vector velocity = pev->velocity;

	// subtract base velocity (trigger_push) from the velocity: we ignore pushes
	if (!IsVectorZero2D(velocity))
	{
		const Vector baseVelocity = pev->basevelocity;
		if (!IsVectorZero2D(baseVelocity))
		{
			velocity.x = fabsf(velocity.x) - fabsf(baseVelocity.x);
			velocity.y = fabsf(velocity.y) - fabsf(baseVelocity.y);
		}
	}

	// speed was lost completely: maybe the surf bug
	if (IsVectorZero2D(velocity) && IsPlayerOnSlope(pPlayer))
	{
		const int hull = (pev->flags & FL_DUCKING) ? head_hull : human_hull;

		Vector origin = pev->origin;
		origin.z += SURFFIX_UNSTUCK_HEIGHT;

		// do not teleport into a wall / ceiling and get stuck
		if (IsHullVacant(origin, hull, ENT(pev)))
		{
			pev->origin = origin;
			pev->velocity = startVelocity;

			if (SURFFIX_DEBUG)
				CLIENT_PRINTF(pPlayer->edict(), print_console, "[surffix] have fun surfing!\n");
		}
	}
}