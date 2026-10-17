// checkpoint.cpp - addcheckpoint / teleport (server side, put in dlls/)
//
// addcheckpoint : saves your position, view angles and duck state
// teleport      : goes back to the last checkpoint, removes all velocity and restores the duck state
//
// Both commands only work with sv_cheats 1, otherwise they silently do nothing.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "checkpoint.h"
#include "edgebug.h"
#include "jumpstats.h"
#include <string.h>

constexpr bool CP_RESTORE_ANGLES = true; // also restore the view angles

struct Checkpoint
{
	bool   valid = false;
	Vector origin = Vector(0, 0, 0);
	Vector angles = Vector(0, 0, 0);
	bool   ducked = false;
	char   map[64] = "";
};

static Checkpoint g_Cp[33]; // indexed by entity index (1..32)

static Checkpoint& GetCp(CBasePlayer* pPlayer)
{
	int idx = pPlayer->entindex();
	if (idx < 0 || idx > 32)
		idx = 0;
	return g_Cp[idx];
}

static void AddCheckpoint(CBasePlayer* p)
{
	Checkpoint& cp = GetCp(p);

	cp.valid = true;
	cp.origin = p->pev->origin;
	cp.angles = p->pev->v_angle;
	cp.ducked = (p->pev->flags & FL_DUCKING) != 0;
	strncpy(cp.map, STRING(gpGlobals->mapname), sizeof(cp.map) - 1);
	cp.map[sizeof(cp.map) - 1] = 0;

	CLIENT_PRINTF(p->edict(), print_center, "Checkpoint added");
}

static void TeleportToCheckpoint(CBasePlayer* p)
{
	Checkpoint& cp = GetCp(p);

	// a checkpoint from another map is useless
	if (!cp.valid || strcmp(cp.map, STRING(gpGlobals->mapname)) != 0)
	{
		CLIENT_PRINTF(p->edict(), print_center, "No checkpoint set");
		return;
	}

	entvars_t* pev = p->pev;

	// Duck state first: the saved origin belongs to a ducked hull (18 units lower) or a standing one
	if (cp.ducked)
	{
		pev->flags |= FL_DUCKING;
		p->m_afPhysicsFlags |= PFLAG_DUCKING;
		pev->view_ofs = VEC_DUCK_VIEW;
		UTIL_SetSize(pev, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX);
	}
	else
	{
		pev->flags &= ~FL_DUCKING;
		p->m_afPhysicsFlags &= ~PFLAG_DUCKING;
		pev->view_ofs = VEC_VIEW;
		UTIL_SetSize(pev, VEC_HULL_MIN, VEC_HULL_MAX);
	}
	pev->bInDuck = 0;
	pev->flDuckTime = 0;

	UTIL_SetOrigin(pev, cp.origin);

	// remove all velocity and any pending fall damage
	pev->velocity = g_vecZero;
	pev->basevelocity = g_vecZero;
	pev->punchangle = g_vecZero;
	pev->flFallVelocity = 0;
	p->m_flFallVelocity = 0;

	if (CP_RESTORE_ANGLES)
	{
		pev->angles = cp.angles;
		pev->v_angle = cp.angles;
		pev->fixangle = 1;
	}

	// the teleport must not leave half-finished stats behind
	Edgebug_Reset(p);
	JumpStats_Reset(p);
}

bool Checkpoint_Command(CBasePlayer* p, const char* pcmd)
{
	if (!p || !pcmd)
		return false;

	const bool bAdd = FStrEq(pcmd, "addcheckpoint");
	const bool bTele = FStrEq(pcmd, "teleport");
	if (!bAdd && !bTele)
		return false;

	// no cheats (or dead): do nothing at all
	if (CVAR_GET_FLOAT("sv_cheats") == 0.0f || !p->IsAlive())
		return true;

	if (bAdd)
		AddCheckpoint(p);
	else
		TeleportToCheckpoint(p);

	return true;
}
