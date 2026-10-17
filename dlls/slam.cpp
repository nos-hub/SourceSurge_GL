// slam.cpp - air slam mechanic (server side, put in dlls/)
//
// Trigger:  double-tap duck while airborne for at least SLAM_MIN_AIR_TIME seconds.
//           A single duck press does nothing.
// Slam:     horizontal speed is halved once, then the player is forced to fall at
//           (at least) SLAM_DOWN_SPEED until they touch the ground.
// Landing:  shockwave centred on the player, pushes every movable entity within
//           SLAM_RADIUS away (horizontal + vertical). Fall damage is halved.
//
// Everything runs from the single Slam_PreThink() hook.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "slam.h"

#ifndef PLAYER_MAX_SAFE_FALL_SPEED
#define PLAYER_MAX_SAFE_FALL_SPEED 580
#endif

// ---------------- tunables ----------------
constexpr float SLAM_MIN_AIR_TIME       = 0.5f;   // seconds in the air before a slam is allowed
constexpr float SLAM_DOWN_SPEED         = 650.0f; // u/s downward while slamming
constexpr float SLAM_HORIZONTAL_FACTOR  = 0.5f;   // horizontal speed multiplier at slam start
constexpr float SLAM_RADIUS             = 80.0f; // 100 each side of the player = 200 wide
constexpr float SLAM_KNOCKBACK_H        = 600.0f; // horizontal push at the centre
constexpr float SLAM_KNOCKBACK_V        = 600.0f; // vertical push at the centre
constexpr float SLAM_EDGE_FALLOFF       = 0.5f;   // push is scaled to (1 - this) at the edge of the radius
constexpr float SLAM_FALL_DAMAGE_FACTOR = 0.3f;   // applied to the part of the fall speed above the safe speed

// print what the slam is doing to the console (set to false once everything works)
constexpr bool  SLAM_DEBUG = false;

// ---------------- sounds (swap freely) ----------------
#define SLAM_SND_START "weapons/gauss2.wav"      // played once when the slam starts
#define SLAM_SND_FALL  "weapons/rocket1.wav"     // loops while slamming, stopped on landing
#define SLAM_SND_LAND  "weapons/mortarhit.wav"   // landing boom
#define SLAM_SND_THUD  "common/bodydrop3.wav"    // landing thud layered under the boom

struct SlamState
{
	bool wantSlam = false;		   // set by the "slam" command, consumed in PreThink
	bool  airborne = false;        // currently counting air time
	float airStart = 0.0f;         // time the player left the ground
	float lastDuckPress = -100.0f;
	bool  active = false;          // slam in progress
	float rawFall = 0.0f;          // last fall velocity before / after the damage reduction (debug)
	float adjFall = 0.0f;
};

static SlamState g_Slam[33]; // indexed by entity index (1..32)

static SlamState& GetState(CBasePlayer* pPlayer)
{
	int idx = pPlayer->entindex();
	if (idx < 0 || idx > 32)
		idx = 0;
	return g_Slam[idx];
}

static void DebugPrint(CBasePlayer* pPlayer, const char* msg)
{
	if (SLAM_DEBUG)
		CLIENT_PRINTF(pPlayer->edict(), print_console, msg);
}

static bool IsMovable(CBaseEntity* pEnt)
{
	switch (pEnt->pev->movetype)
	{
	case MOVETYPE_WALK:
	case MOVETYPE_STEP:
	case MOVETYPE_FLY:
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_PUSHSTEP:
		return true;
	default:
		return false;
	}
}

static int DoShockwave(CBasePlayer* pPlayer)
{
	const Vector vecOrigin = pPlayer->pev->origin;
	int iPushed = 0;

	CBaseEntity* pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityInSphere(pEnt, vecOrigin, SLAM_RADIUS)) != nullptr)
	{
		if (pEnt == pPlayer || !IsMovable(pEnt))
			continue;

		Vector vecDir = pEnt->Center() - vecOrigin;
		float flDist = vecDir.Length();
		vecDir.z = 0;

		// directly above/below the player: pick a random horizontal direction
		if (vecDir.Length() < 1.0f)
		{
			vecDir = Vector(RANDOM_FLOAT(-1, 1), RANDOM_FLOAT(-1, 1), 0);
			if (vecDir.Length() < 0.01f)
				vecDir = Vector(1, 0, 0);
		}
		vecDir = vecDir.Normalize();

		float flFrac = flDist / SLAM_RADIUS;
		if (flFrac > 1.0f)
			flFrac = 1.0f;
		const float flScale = 1.0f - SLAM_EDGE_FALLOFF * flFrac;

		// entities standing on the ground are "glued" to it by the engine: unglue and lift a hair
		if (pEnt->pev->flags & FL_ONGROUND)
		{
			pEnt->pev->flags &= ~FL_ONGROUND;
			UTIL_SetOrigin(pEnt->pev, pEnt->pev->origin + Vector(0, 0, 1));
		}

		pEnt->pev->velocity = pEnt->pev->velocity
			+ vecDir * (SLAM_KNOCKBACK_H * flScale)
			+ Vector(0, 0, SLAM_KNOCKBACK_V * flScale);

		iPushed++;
	}

	return iPushed;
}

static void StartSlam(CBasePlayer* pPlayer, SlamState& st)
{
	entvars_t* pev = pPlayer->pev;

	st.active = true;
	pev->velocity.x *= SLAM_HORIZONTAL_FACTOR;
	pev->velocity.y *= SLAM_HORIZONTAL_FACTOR;

	EMIT_SOUND_DYN(ENT(pev), CHAN_ITEM, SLAM_SND_START, 1.0f, ATTN_NORM, 0, 140);
	EMIT_SOUND_DYN(ENT(pev), CHAN_BODY, SLAM_SND_FALL, 0.7f, ATTN_NORM, 0, 100);

	DebugPrint(pPlayer, "[slam] started\n");
}

static void StopFallSound(CBasePlayer* pPlayer)
{
	STOP_SOUND(ENT(pPlayer->pev), CHAN_BODY, SLAM_SND_FALL);
}

static void Land(CBasePlayer* pPlayer, SlamState& st)
{
	StopFallSound(pPlayer);

	EMIT_SOUND_DYN(ENT(pPlayer->pev), CHAN_WEAPON, SLAM_SND_LAND, 1.0f, ATTN_NORM, 0, 95);
	EMIT_SOUND_DYN(ENT(pPlayer->pev), CHAN_ITEM, SLAM_SND_THUD, 1.0f, ATTN_NORM, 0, 80);

	const int iPushed = DoShockwave(pPlayer);

	if (SLAM_DEBUG)
	{
		CLIENT_PRINTF(pPlayer->edict(), print_console,
			UTIL_VarArgs("[slam] landed: fall speed %.0f -> %.0f (damage uses the second), pushed %d entities\n",
				st.rawFall, st.adjFall, iPushed));
	}
}

void Slam_Precache()
{
	PRECACHE_SOUND(SLAM_SND_START);
	PRECACHE_SOUND(SLAM_SND_FALL);
	PRECACHE_SOUND(SLAM_SND_LAND);
	PRECACHE_SOUND(SLAM_SND_THUD);
}

void Slam_Request(CBasePlayer* pPlayer)
{
	GetState(pPlayer).wantSlam = true;
}

void Slam_PreThink(CBasePlayer* pPlayer)
{
	SlamState& st = GetState(pPlayer);
	entvars_t* pev = pPlayer->pev;
	const float now = gpGlobals->time;

	// only normal walking-type movement outside of water can slam
	const bool bCanSlam = pPlayer->IsAlive() && pev->movetype == MOVETYPE_WALK && pev->waterlevel == 0;
	if (!bCanSlam)
	{
		if (st.active)
		{
			StopFallSound(pPlayer);
		}
		st = SlamState();
		return;
	}

	// ---- on the ground ----
	if (pev->flags & FL_ONGROUND)
	{
		if (st.active)
			Land(pPlayer, st); // the previous frame ended with the slam touching the ground

		st.airborne = false;
		st.active = false;
		st.wantSlam = false; // a key pressed on the ground must not slam the next jump
		return;
	}

	// ---- airborne ----
	if (!st.airborne)
	{
		st.airborne = true;
		st.airStart = now;
	}

	// slam requested by the bound key
	if (st.wantSlam)
	{
		st.wantSlam = false;
		if (!st.active)
		{
			if ((now - st.airStart) >= SLAM_MIN_AIR_TIME)
				StartSlam(pPlayer, st);
			else
				DebugPrint(pPlayer, UTIL_VarArgs("[slam] ignored: only %.2fs in the air\n", now - st.airStart));
		}
	}

	if (st.active)
	{
		// never fall slower than SLAM_DOWN_SPEED
		if (pev->velocity.z > -SLAM_DOWN_SPEED)
			pev->velocity.z = -SLAM_DOWN_SPEED;

		// Halve the fall damage: the engine computes it from m_flFallVelocity (set earlier this frame
		// from the velocity), as (m_flFallVelocity - safe speed) * constant. Shrinking the part above the
		// safe speed shrinks the damage by the same factor.
		st.rawFall = pPlayer->m_flFallVelocity;
		if (pPlayer->m_flFallVelocity > PLAYER_MAX_SAFE_FALL_SPEED)
		{
			pPlayer->m_flFallVelocity = PLAYER_MAX_SAFE_FALL_SPEED +
										(pPlayer->m_flFallVelocity - PLAYER_MAX_SAFE_FALL_SPEED) * SLAM_FALL_DAMAGE_FACTOR;
		}
		st.adjFall = pPlayer->m_flFallVelocity;
	}
}

// ---- legacy hooks, intentionally empty ----
void Slam_PostThink(CBasePlayer* pPlayer)
{
}

float Slam_ModifyFallDamage(CBasePlayer* pPlayer, float flDamage)
{
	return flDamage; // the reduction is now done in Slam_PreThink; do not halve twice
}
