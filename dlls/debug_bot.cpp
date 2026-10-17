// debug_bot.cpp - spawn an inert scientist to test stuff on (put in dlls/)
//
//   spawn_debug_bot              -> spawns where your crosshair points
//   spawn_debug_bot x y z        -> spawns at those world coordinates
//   remove_debug_bots            -> removes every bot spawned this way
//
// The bot is a plain monster_generic: it has no AI behaviour, just stands there,
// takes damage, and can be pushed/pulled (FL_MONSTER, MOVETYPE_STEP).

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "player.h"

#define DEBUG_BOT_TARGETNAME "debug_bot"
#define DEBUG_BOT_MODEL      "models/scientist.mdl"
#define DEBUG_BOT_HEALTH     100.0f

void DebugBot_Spawn(CBasePlayer* pPlayer)
{
	Vector vecPos;

	if (CMD_ARGC() >= 4)
	{
		// explicit coordinates: spawn_debug_bot x y z
		vecPos = Vector(atof(CMD_ARGV(1)), atof(CMD_ARGV(2)), atof(CMD_ARGV(3)));
	}
	else
	{
		// default: where the crosshair points (pushed out of the surface by half the hull)
		UTIL_MakeVectors(pPlayer->pev->v_angle);
		const Vector vecSrc = pPlayer->GetGunPosition();

		TraceResult tr;
		UTIL_TraceLine(vecSrc, vecSrc + gpGlobals->v_forward * 4096.0f, ignore_monsters, pPlayer->edict(), &tr);
		vecPos = tr.vecEndPos + tr.vecPlaneNormal * 36.0f;
	}

	// face the player
	const Vector vecAngles(0, UTIL_VecToAngles(pPlayer->pev->origin - vecPos).y, 0);

	edict_t* pent = CREATE_NAMED_ENTITY(MAKE_STRING("monster_generic"));
	if (FNullEnt(pent))
	{
		CLIENT_PRINTF(pPlayer->edict(), print_console, "spawn_debug_bot: could not create entity\n");
		return;
	}

	entvars_t* pevBot = VARS(pent);
	pevBot->origin = vecPos;
	pevBot->angles = vecAngles;
	pevBot->model = MAKE_STRING(DEBUG_BOT_MODEL);
	pevBot->targetname = MAKE_STRING(DEBUG_BOT_TARGETNAME);
	pevBot->spawnflags |= SF_MONSTER_FALL_TO_GROUND; // let gravity drop it, don't snap/stick to floor

	DispatchSpawn(pent);

	pevBot->health = DEBUG_BOT_HEALTH;
	pevBot->max_health = DEBUG_BOT_HEALTH;

	CLIENT_PRINTF(pPlayer->edict(), print_console,
		UTIL_VarArgs("Debug bot spawned at %.0f %.0f %.0f\n", vecPos.x, vecPos.y, vecPos.z));
}

void DebugBot_RemoveAll(CBasePlayer* pPlayer)
{
	int count = 0;
	CBaseEntity* pEnt = nullptr;

	while ((pEnt = UTIL_FindEntityByTargetname(pEnt, DEBUG_BOT_TARGETNAME)) != nullptr)
	{
		UTIL_Remove(pEnt);
		count++;
	}

	CLIENT_PRINTF(pPlayer->edict(), print_console, UTIL_VarArgs("Removed %d debug bot(s)\n", count));
}
