// melee_kb.cpp - melee wall/ground knockback with charges (server side, put in dlls/)
//
// RULES
//   * A player has up to MELEE_MAX_CHARGES (4) charges. They start full on spawn.
//   * World hit (any solid: wall, floor, ceiling, breakables...) with >= 1 charge:
//       the player's velocity gets  -viewDirection * MELEE_KB_WORLD  ADDED to it, so you are
//       always thrown BEHIND you, opposite to where you look (it stacks with your current
//       speed), and 1 charge is spent. The surface normal is NOT used.
//         looking at a wall   -> pushed straight back
//         looking down at the floor -> pushed up (and back if you look down at an angle)
//         looking up at the ceiling -> pushed down
//   * Enemy hit (a monster or a player): a normal melee attack. NO knockback on either side
//     and NO charge is spent, whether or not the hit kills.
//   * A melee KILL restores 1 charge, clamped to the maximum. At 4/4 nothing is added.
//     Charges are NOT restored by landing; only by melee kills (and by respawning).
//   * 0 charges: the swing still damages as usual, but world hits push nothing.
//
// Velocity is changed on the server (authoritative). The crowbar calls MeleeKB_OnMeleeHit from
// its server-side hit code, and the points system calls MeleeKB_OnMeleeKill.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "UserMessages.h"
#include "melee_kb.h"

// ---------------- tunables ----------------
constexpr int MELEE_MAX_CHARGES = 4;
constexpr float MELEE_KB_WORLD = 450.0f;	// u/s added opposite to the view direction (a jump is ~268)
constexpr float MELEE_HUD_HEARTBEAT = 1.0f; // seconds between HUD refreshes even without a change
constexpr int MELEE_MAX_PLAYERS = 32;
constexpr float MELEE_RECHARGE_TIME = 1.5f; // seconds to passively regain 1 charge

struct melee_state_t
{
	bool init; // charges are valid (set when the player is first seen alive)
	bool infinite; // cheat: charges are never spent (needs sv_cheats 1)
	int charges;
	int sentCharges; // last value sent to the HUD
	float nextSend;
	float nextRecharge; // when the next passive charge is due
};

static melee_state_t g_Melee[MELEE_MAX_PLAYERS + 1]; // slot 0 = scratch
static float g_flMeleeLastTime = 0.0f;				 // detects a new level (the clock restarts)

static int MeleeSlot(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return 0;
	const int idx = ENTINDEX(pPlayer->edict());
	if (idx < 1 || idx > MELEE_MAX_PLAYERS)
		return 0;
	return idx;
}

static void MeleeSend(CBasePlayer* pPlayer, melee_state_t& s)
{
	s.sentCharges = s.charges;
	s.nextSend = gpGlobals->time + MELEE_HUD_HEARTBEAT;

	if (gmsgMeleeCharges == 0 || !pPlayer)
		return;

	MESSAGE_BEGIN(MSG_ONE, gmsgMeleeCharges, NULL, pPlayer->pev);
	WRITE_BYTE(s.charges);
	WRITE_BYTE(MELEE_MAX_CHARGES);
	MESSAGE_END();
}

static void MeleeEnsureInit(CBasePlayer* pPlayer, melee_state_t& s)
{
	if (s.init)
		return;
	s.init = true;
	s.charges = MELEE_MAX_CHARGES;
	MeleeSend(pPlayer, s);
}

static bool MeleeInfiniteActive(const melee_state_t& s)
{
	return s.infinite && CVAR_GET_FLOAT("sv_cheats") != 0;
}

// ---------------------------------------------------------------------------

void MeleeKB_Think()
{
	
	const float now = gpGlobals->time;

	// the game clock went backwards: a new level, start everybody fresh
	if (now < g_flMeleeLastTime)
	{
		for (int i = 0; i <= MELEE_MAX_PLAYERS; i++)
			g_Melee[i].init = false;
	}
	g_flMeleeLastTime = now;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MELEE_MAX_PLAYERS; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		melee_state_t& s = g_Melee[i];

		if (!pEnt || !pEnt->IsPlayer() || (pEnt->pev->flags & FL_CLIENT) == 0 || !pEnt->IsAlive())
		{
			s.init = false; // dead / gone: full charges again at the next spawn
			continue;
		}

		CBasePlayer* pPlayer = static_cast<CBasePlayer*>(pEnt);
		MeleeEnsureInit(pPlayer, s);

				if (MeleeInfiniteActive(s))
			s.charges = MELEE_MAX_CHARGES;

		// passive recharge: +1 charge every MELEE_RECHARGE_TIME while below the maximum
		if (s.charges >= MELEE_MAX_CHARGES)
		{
			// full: keep the timer parked so the first recharge after a spend takes the full delay
			s.nextRecharge = now + MELEE_RECHARGE_TIME;
		}
		else if (now >= s.nextRecharge)
		{
			s.charges++;
			s.nextRecharge = now + MELEE_RECHARGE_TIME;
		}

		if (s.charges != s.sentCharges || now >= s.nextSend || s.nextSend > now + MELEE_HUD_HEARTBEAT + 1.0f)
			MeleeSend(pPlayer, s);
	}
}

void MeleeKB_OnMeleeKill(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return;

	melee_state_t& s = g_Melee[MeleeSlot(pPlayer)];
	MeleeEnsureInit(pPlayer, s);

	if (s.charges < MELEE_MAX_CHARGES)
	{
		s.charges++;
		MeleeSend(pPlayer, s);
	}
	// already 4/4: nothing is added
}

// vecPlaneNormal is no longer used (the push follows the view direction, not the surface);
// the parameter stays so the call in crowbar.cpp does not have to change.
void MeleeKB_OnMeleeHit(CBasePlayer* pPlayer, const Vector& /*vecPlaneNormal*/, CBaseEntity* pHit)
{
	if (!pPlayer || !pPlayer->IsAlive())
		return;

	// an enemy (monster or player, alive or not) is a normal melee attack:
	// no knockback and no charge spent
	if (pHit && (pHit->pev->flags & (FL_MONSTER | FL_CLIENT)) != 0)
		return;

	melee_state_t& s = g_Melee[MeleeSlot(pPlayer)];
	MeleeEnsureInit(pPlayer, s);

	if (s.charges <= 0)
		return; // no charges: no knockback

	// ---- world hit: throw the player BEHIND them, ADDED to the current velocity ----
	// the view direction, pitch included: looking down at the floor pushes you up
	UTIL_MakeVectors(pPlayer->pev->v_angle);
	const Vector back = Vector(0, 0, 0) - gpGlobals->v_forward;

	pPlayer->pev->velocity = pPlayer->pev->velocity + back * MELEE_KB_WORLD;
	pPlayer->pev->flags &= ~FL_ONGROUND; // lets an upward push lift you off the floor

	if (!MeleeInfiniteActive(s))
	{
		s.charges--;
	}
	MeleeSend(pPlayer, s);
}

void MeleeKB_ToggleInfinite(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return;

	if (CVAR_GET_FLOAT("sv_cheats") == 0)
	{
		ClientPrint(pPlayer->pev, HUD_PRINTCONSOLE, "melee_infinite requires sv_cheats 1\n");
		return;
	}

	melee_state_t& s = g_Melee[MeleeSlot(pPlayer)];
	MeleeEnsureInit(pPlayer, s);

	s.infinite = !s.infinite;
	if (s.infinite)
	{
		s.charges = MELEE_MAX_CHARGES;
		MeleeSend(pPlayer, s);
	}
	ClientPrint(pPlayer->pev, HUD_PRINTCONSOLE, s.infinite ? "Infinite melee charges ON\n" : "Infinite melee charges OFF\n");
}