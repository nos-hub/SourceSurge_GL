// hitfeedback.cpp - server side of hitsounds / hitmarkers.
// Called from CBaseMonster::TakeDamage (combat.cpp) right after the health is subtracted.
// Sends the "HitMark" user message (3 bytes) to the attacker only:
//   short damage, byte flags (bit0 = kill, bit1 = headshot)

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"

#ifndef HITGROUP_HEAD
#define HITGROUP_HEAD 1
#endif

int gmsgHitMark = 0;

// Called from CBasePlayer::Precache(), right after JumpStats_LinkUserMessages().
// Safe to call on every spawn: it only registers once.
void HitFeedback_LinkUserMessages()
{
	if (gmsgHitMark)
		return;

	gmsgHitMark = REG_USER_MSG("HitMark", 3);
}

void HitFeedback_Send(entvars_t* pevAttacker, CBaseEntity* pVictim, float flDamage, int bitsDamageType)
{
	if (!pevAttacker || !pVictim || !pVictim->pev)
		return;

	// no feedback for self damage (rocket jumps, falls, ...)
	if (pevAttacker == pVictim->pev)
		return;

	// shooting a corpse
	if (pVictim->pev->deadflag != DEAD_NO)
		return;

	CBaseEntity* pAttacker = CBaseEntity::Instance(pevAttacker);
	if (!pAttacker || !pAttacker->IsPlayer())
		return;

	// only living things (players, monsters, bots). Breakables etc. get nothing.
	CBaseMonster* pMonster = pVictim->MyMonsterPointer();
	if (!pMonster)
		return;

	int flags = 0;

	if (pVictim->pev->health <= 0)
		flags |= 1; // kill

	// m_LastHitGroup is set in TraceAttack; ignore it for splash damage (it can be stale)
	if (!(bitsDamageType & DMG_BLAST) && pMonster->m_LastHitGroup == HITGROUP_HEAD)
		flags |= 2; // headshot

	int dmg = (int)flDamage;
	if (dmg < 0)
		dmg = 0;
	if (dmg > 32767)
		dmg = 32767;

	MESSAGE_BEGIN(MSG_ONE, gmsgHitMark, NULL, pAttacker->pev);
	WRITE_SHORT(dmg);
	WRITE_BYTE(flags);
	MESSAGE_END();
}