// health_on_kill.cpp - heal on kill + overheal (server side, put in dlls/)
//
// pev->health is the only health value. Anything above 100 is overheal.
// Damage hits pev->health directly, so it comes off overheal first, as in TF2.
// Overheal drains faster the more of it you have: 1 HP/s for a tiny bit, up to 4 HP/s at the 250 cap.
//
// Healing scales with the points of THAT kill, with diminishing returns so style
// multipliers can't refill you instantly:
//     heal = HEALTH_BASELINE_GRANT * (points / HEALTH_BASELINE_POINTS) ^ HEALTH_EXPONENT, capped per kill
//   100 pts -> 25 HP, 225 pts -> ~37 HP, 400 pts -> 50 HP, 900+ pts -> capped

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "health_on_kill.h"
#include <math.h>

// ---------------- tunables ----------------
constexpr float HEALTH_BASELINE_POINTS = 100.0f; // a kill worth this many points...
constexpr float HEALTH_BASELINE_GRANT = 25.0f;	 // ...heals this much
constexpr float HEALTH_EXPONENT = 0.5f;			 // 1.0 = linear (busted with style multipliers), 0.5 = square root
constexpr float HEALTH_MAX_PER_KILL = 60.0f;	 // hard cap on the heal from a single kill
constexpr float HEALTH_MAX_NORMAL = 100.0f;		 // normal max health
constexpr float HEALTH_MAX_OVERHEAL = 250.0f;	 // absolute cap including overheal
constexpr float OVERHEAL_DECAY_MIN = 1.0f;		 // overheal lost per second when you only have a tiny bit of overheal
constexpr float OVERHEAL_DECAY_MAX = 4.0f;		 // overheal lost per second at the full overheal cap (HEALTH_MAX_OVERHEAL)

void HealthOnKill_Grant(CBasePlayer* pPlayer, float flPoints)
{
	if (!pPlayer || !pPlayer->IsAlive() || flPoints <= 0.0f)
		return;

	float flGain = HEALTH_BASELINE_GRANT * powf(flPoints / HEALTH_BASELINE_POINTS, HEALTH_EXPONENT);
	if (flGain > HEALTH_MAX_PER_KILL)
		flGain = HEALTH_MAX_PER_KILL;

	float flNew = pPlayer->pev->health + flGain;
	if (flNew > HEALTH_MAX_OVERHEAL)
		flNew = HEALTH_MAX_OVERHEAL;

	pPlayer->pev->health = flNew;
}

void HealthOnKill_DecayOverheal(CBasePlayer* pPlayer)
{
	if (!pPlayer || !pPlayer->IsAlive())
		return;

	entvars_t* pev = pPlayer->pev;

	if (pev->health > HEALTH_MAX_NORMAL)
	{
		// how full the overheal is: 0 = just above 100 HP, 1 = at the 250 HP cap
		const float flOverheal = pev->health - HEALTH_MAX_NORMAL;
		const float flMaxOverheal = HEALTH_MAX_OVERHEAL - HEALTH_MAX_NORMAL;

		float flFrac = (flMaxOverheal > 0.0f) ? (flOverheal / flMaxOverheal) : 0.0f;
		if (flFrac > 1.0f)
			flFrac = 1.0f;

		// linear: 1 HP/s with a little overheal ... 4 HP/s with the maximum
		const float flRate = OVERHEAL_DECAY_MIN + (OVERHEAL_DECAY_MAX - OVERHEAL_DECAY_MIN) * flFrac;

		pev->health -= flRate * gpGlobals->frametime;
		if (pev->health < HEALTH_MAX_NORMAL)
			pev->health = HEALTH_MAX_NORMAL;
	}
}