// points.cpp - kill points + style bonus system (server side, put in dlls/)
//
// Every kill is worth POINTS_BASE. Every style tag that applies to the kill
// multiplies the total (tags stack). There is no cap.
// The HUD shows the 3 strongest tags and a rank (CLEAN..SS) from the final mult.
// Dying resets the player's points to 0.
//
// All numbers come from sourcesurge_style_bonuses.txt: tune them in the
// "tunables" block below.
//
// Not implemented yet (they need hooks inside weapon code): WHIFF-BONK, ONE-TAP,
// COOK, CHARGED, EXPLOSIVE, NO-CHARGE, LJ, WALL, and CLUTCH "last alive".

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "weapons.h"
#include "player.h"
#include "points.h"
#include "wave.h"
#include "melee_kb.h"
#include "UserMessages.h"
#include "health_on_kill.h"
#include "sniper.h"

#include <math.h>
#include <string.h>

// set while a monster/player is being killed (see combat.cpp); tells us what
// actually did the damage: a rocket, a grenade, a bolt, the player's own gun...
extern entvars_t* g_pevLastInflictor;

// ---------------- tunables ----------------
constexpr int POINTS_BASE = 100;

// rank thresholds on the final multiplier (no cap: anything at or above RANK_SS is SS)
constexpr float RANK_CLEAN = 1.3f;
constexpr float RANK_NICE = 1.6f;
constexpr float RANK_SICK = 2.2f;
constexpr float RANK_INSANE = 3.0f;
constexpr float RANK_SS = 4.0f;

// core movement
constexpr float STYLE_AIR_MULT = 1.2f;
constexpr float STYLE_BHOP_MULT = 1.25f;	// kill during a bhop chain of 2+ hops
constexpr float STYLE_SPEED_MULT_1 = 1.15f; // horizontal speed >= 300
constexpr float STYLE_SPEED_MULT_2 = 1.3f;	// >= 400
constexpr float STYLE_SPEED_MULT_3 = 1.5f;	// >= 500
constexpr float STYLE_CRANE_MULT = 1.3f;	// air + duck
constexpr float STYLE_GAP_MULT = 1.4f;		// airborne over a pit / no floor
constexpr float STYLE_FALL_MULT = 1.2f;		// falling fast
constexpr float STYLE_HEIGHT_MULT = 1.2f;	// height advantage over the victim
constexpr float STYLE_FLICK_MULT = 1.15f;
constexpr float STYLE_CHAIN_MULT = 1.1f;  // per extra kill in the chain, cumulative
constexpr float STYLE_CLUTCH_MULT = 1.5f; // low HP
constexpr float STYLE_SPIN_MULT = 2.0f;	  // 360 then a kill

constexpr float STYLE_BHOP_WINDOW = 0.15f;	 // max time on the ground between two hops
constexpr float STYLE_BHOP_CHAIN_GAP = 1.0f; // max time between two jumps in a chain
constexpr float STYLE_GAP_DIST = 384.0f;	 // floor further than this below = pit
constexpr float STYLE_FALL_SPEED = -400.0f;	 // vertical velocity for FALL
constexpr float STYLE_HEIGHT_DIST = 128.0f;
constexpr float STYLE_FLICK_SPEED = 900.0f; // degrees/second
constexpr float STYLE_FLICK_WINDOW = 0.25f; // kill within this long after a flick
constexpr float STYLE_CHAIN_WINDOW = 2.0f;
constexpr float STYLE_CLUTCH_HP = 25.0f;
constexpr float STYLE_SPIN_DEGREES = 360.0f;
constexpr float STYLE_SPIN_LEAK = 200.0f; // deg/s that fade out, so a 360 has to be quick
constexpr float STYLE_SPIN_WINDOW = 2.0f; // the kill must come within this long after the 360

// weapons
constexpr float STYLE_QUICKDRAW_TIME = 1.5f;
constexpr float STYLE_TAP_IDLE = 0.5f;	  // button released this long before the shot
constexpr float STYLE_HIP_RANGE = 256.0f; // .357 "close" range
constexpr float STYLE_POINTBLANK = 64.0f;
constexpr float STYLE_SNIPE_RANGE = 1024.0f;
constexpr float STYLE_RJ_RADIUS = 150.0f; // own rocket this close = rocket jump kill
constexpr float STYLE_MELT_HOLD = 1.5f;	  // egon beam held this long
constexpr int STYLE_OVERHEAT_AMMO = 15;	  // egon ammo at or below this
constexpr int STYLE_SWARM_COUNT = 3;	  // hornets / snarks alive for SWARM / CHAOS

// sniper
constexpr float STYLE_NOSCOPE_MULT = 1.5f;	  // fired without the scope up
constexpr float STYLE_QUICKSCOPE_MULT = 1.5f; // fired within STYLE_QUICKSCOPE_TIME of scoping in
constexpr float STYLE_COLLAT_MULT = 1.8f;	  // 2nd and later kill of the SAME shot
constexpr float STYLE_LONGRANGE_MULT = 1.6f;
constexpr float STYLE_CLOSERANGE_MULT = 1.4f;
constexpr float STYLE_QUICKSCOPE_TIME = 0.4f; // seconds
constexpr float STYLE_SNIPER_LONG = 2048.0f;  // units
constexpr float STYLE_SNIPER_CLOSE = 256.0f;  // units



// ---------------- tag ids (the client has the same list, keep the order!) ----------------
enum
{
	STY_NONE = 0,
	STY_AIR,
	STY_BHOP,
	STY_SPEED,
	STY_CRANE,
	STY_GAP,
	STY_FALL,
	STY_HEIGHT,
	STY_FLICK,
	STY_CHAIN,
	STY_CLUTCH,
	STY_SPIN360,
	STY_MELEE,
	STY_BONK,
	STY_PISTOL,
	STY_TAP,
	STY_EMPTY,
	STY_QUICKDRAW,
	STY_HANDCANNON,
	STY_HIP,
	STY_SPRAY,
	STY_GRENADE,
	STY_FROMHIP,
	STY_PUMP,
	STY_POINTBLANK,
	STY_AIRSHOT,
	STY_DOUBLE,
	STY_BOLT,
	STY_PIN,
	STY_SNIPE,
	STY_ROCKET,
	STY_AIRROCKET,
	STY_RJ,
	STY_STACK,
	STY_GAUSS,
	STY_THROUGH,
	STY_RAIL,
	STY_BEAM,
	STY_MELT,
	STY_OVERHEAT,
	STY_NEST,
	STY_SWARM,
	STY_FRAG,
	STY_SATCHEL,
	STY_TRAP,
	STY_SNARK,
	STY_CHAOS,	
	STY_NOSCOPE,
	STY_COLLAT,
	STY_LONGRANGE,
	STY_CLOSERANGE,
	STY_QUICKSCOPE,
	STY_COUNT
};

// ---------------- per-player style state ----------------
constexpr int STYLE_MAX_PLAYERS = 32;

struct style_state_t
{
	bool init;

	// bhop
	bool prevOnGround;
	float groundSince;
	float lastGroundSeen;
	float lastJumpTime;
	int hops;

	// aim
	float lastYaw, lastPitch;
	float spin;
	float spinUntil;
	float lastFlickTime;

	// weapon
	int lastWeaponId;
	float switchTime;

	// buttons
	bool prevAttack;
	float attackHeldStart;
	float lastAttackDownTime;
	float lastPressTime;
	bool tapShot;

	// rocket jump
	Vector prevVel;
	float rjUntil;

	// kills
	float lastKillTime;
	int chain;
	entvars_t* lastKillInflictor;
	int lastKillWid;
};

static style_state_t g_Style[STYLE_MAX_PLAYERS + 1]; // slot 0 = scratch

static void StyleReset(style_state_t& s)
{
	s.init = false;
	s.prevOnGround = false;
	s.groundSince = -100.0f;
	s.lastGroundSeen = -100.0f;
	s.lastJumpTime = -100.0f;
	s.hops = 0;
	s.lastYaw = 0.0f;
	s.lastPitch = 0.0f;
	s.spin = 0.0f;
	s.spinUntil = -100.0f;
	s.lastFlickTime = -100.0f;
	s.lastWeaponId = 0;
	s.switchTime = -100.0f;
	s.prevAttack = false;
	s.attackHeldStart = -100.0f;
	s.lastAttackDownTime = -100.0f;
	s.lastPressTime = -100.0f;
	s.tapShot = false;
	s.prevVel = Vector(0, 0, 0);
	s.rjUntil = -100.0f;
	s.lastKillTime = -100.0f;
	s.chain = 0;
	s.lastKillInflictor = NULL;
	s.lastKillWid = 0;
}

static int StyleSlot(CBasePlayer* pPlayer)
{
	int idx = ENTINDEX(pPlayer->edict());
	if (idx < 1 || idx > STYLE_MAX_PLAYERS)
		return 0;
	return idx;
}

static float AngleDelta(float a, float b)
{
	float d = a - b;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}

// ---------------- tag collection ----------------
constexpr int STYLE_MAX_TAGS = 24;

struct style_tags_t
{
	int count;
	int id[STYLE_MAX_TAGS];
	float mult[STYLE_MAX_TAGS];
	float total;
};

static void AddTag(style_tags_t& t, int id, float mult)
{
	if (t.count < STYLE_MAX_TAGS)
	{
		t.id[t.count] = id;
		t.mult[t.count] = mult;
		t.count++;
	}
	t.total *= mult;
}

// the 3 strongest tags, strongest first
static int PickTopTags(const style_tags_t& t, int* outIds)
{
	bool used[STYLE_MAX_TAGS];
	for (int i = 0; i < STYLE_MAX_TAGS; i++)
		used[i] = false;

	int n = 0;
	for (int k = 0; k < 3; k++)
	{
		int best = -1;
		for (int i = 0; i < t.count; i++)
		{
			if (!used[i] && (best < 0 || t.mult[i] > t.mult[best]))
				best = i;
		}
		if (best < 0)
			break;
		used[best] = true;
		outIds[n++] = t.id[best];
	}
	return n;
}

// 0 none, 1 CLEAN, 2 NICE, 3 SICK, 4 INSANE, 5 SS
static int RankFromMult(float mult)
{
	if (mult >= RANK_SS)
		return 5;
	if (mult >= RANK_INSANE)
		return 4;
	if (mult >= RANK_SICK)
		return 3;
	if (mult >= RANK_NICE)
		return 2;
	if (mult >= RANK_CLEAN)
		return 1;
	return 0;
}

// counts live entities of a class that belong to this player (hornets, snarks)
static int CountOwned(const char* classname, entvars_t* pevOwner)
{
	int n = 0;
	CBaseEntity* p = NULL;
	while ((p = UTIL_FindEntityByClassname(p, classname)) != NULL)
	{
		if (p->pev->owner == ENT(pevOwner))
			n++;
	}
	return n;
}

// ---------------- networking ----------------
// Message layout (client reads it in hud_points.cpp):
//   long  total points
//   long  points gained by this kill (0 = no popup, just a total update)
//   short multiplier * 100 (e.g. 144 = x1.44)
//   byte  number of tags shown (0-3)
//   byte  tag id x3 (unused ones are 0)
//   byte  rank (0 none .. 5 SS)
static void SendPoints(CBasePlayer* pPlayer, int gained, float mult, const int* tags, int tagCount, int rank)
{
	if (!pPlayer || gmsgPoints == 0)
		return;

	MESSAGE_BEGIN(MSG_ONE, gmsgPoints, NULL, pPlayer->pev);
	WRITE_LONG(pPlayer->m_iPoints);
	WRITE_LONG(gained);
	WRITE_SHORT((mult > 320.0f) ? 32000 : (int)(mult * 100.0f + 0.5f));
	WRITE_BYTE(tagCount);
	for (int k = 0; k < 3; k++)
		WRITE_BYTE((tags && k < tagCount) ? tags[k] : 0);
	WRITE_BYTE(rank);
	MESSAGE_END();
}

// ---------------- per-frame tracking ----------------
void Points_Think()
{
	const float dt = gpGlobals->frametime;
	if (dt <= 0.0f)
		return;
	const float now = gpGlobals->time;

	for (int i = 1; i <= gpGlobals->maxClients && i <= STYLE_MAX_PLAYERS; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		style_state_t& s = g_Style[i];

		if (!pEnt || !pEnt->IsPlayer() || !pEnt->IsAlive() || (pEnt->pev->flags & FL_SPECTATOR) != 0)
		{
			s.init = false;
			continue;
		}

		CBasePlayer* p = static_cast<CBasePlayer*>(pEnt);
		entvars_t* pevPl = p->pev;

		const bool onGround = (pevPl->flags & FL_ONGROUND) != 0;
		const int wid = p->m_pActiveItem ? p->m_pActiveItem->m_iId : 0;
		const bool atk = (pevPl->button & IN_ATTACK) != 0;

		// first frame after spawn / reset: just take a baseline
		if (!s.init)
		{
			StyleReset(s);
			s.prevOnGround = onGround;
			s.lastYaw = pevPl->v_angle.y;
			s.lastPitch = pevPl->v_angle.x;
			s.lastWeaponId = wid;
			s.prevAttack = atk;
			s.prevVel = pevPl->velocity;
			s.init = true;
			continue;
		}

		const Vector dv = pevPl->velocity - s.prevVel;

		// ---- bhop: jump events chained with very short ground contact ----
		if (onGround)
		{
			if (!s.prevOnGround)
				s.groundSince = now;
			s.lastGroundSeen = now;
		}

		if (dv.z > 200.0f && pevPl->velocity.z > 200.0f && now - s.lastGroundSeen < 0.1f)
		{
			const float groundTime = onGround ? (now - s.groundSince) : 0.0f;
			if (now - s.lastJumpTime < STYLE_BHOP_CHAIN_GAP && groundTime <= STYLE_BHOP_WINDOW)
				s.hops++;
			else
				s.hops = 1;
			s.lastJumpTime = now;
		}
		if (now - s.lastJumpTime > STYLE_BHOP_CHAIN_GAP + 0.2f)
			s.hops = 0;

		s.prevOnGround = onGround;

		// ---- aim: 360 spins and flicks ----
		const float dy = AngleDelta(pevPl->v_angle.y, s.lastYaw);
		const float dp = AngleDelta(pevPl->v_angle.x, s.lastPitch);
		s.lastYaw = pevPl->v_angle.y;
		s.lastPitch = pevPl->v_angle.x;

		s.spin += dy;
		const float leak = STYLE_SPIN_LEAK * dt;
		if (s.spin > 0.0f)
		{
			s.spin -= leak;
			if (s.spin < 0.0f)
				s.spin = 0.0f;
		}
		else if (s.spin < 0.0f)
		{
			s.spin += leak;
			if (s.spin > 0.0f)
				s.spin = 0.0f;
		}
		if (fabsf(s.spin) >= STYLE_SPIN_DEGREES)
		{
			s.spinUntil = now + STYLE_SPIN_WINDOW;
			s.spin = 0.0f;
		}

		if ((fabsf(dy) + fabsf(dp)) / dt > STYLE_FLICK_SPEED)
			s.lastFlickTime = now;

		// ---- weapon switch ----
		if (wid != s.lastWeaponId)
		{
			s.lastWeaponId = wid;
			s.switchTime = now;
		}

		// ---- attack button: tap vs held ----
		if (atk && !s.prevAttack)
		{
			s.tapShot = (now - s.lastAttackDownTime) > STYLE_TAP_IDLE;
			s.attackHeldStart = now;
			s.lastPressTime = now;
		}
		if (atk)
			s.lastAttackDownTime = now;
		s.prevAttack = atk;

		// ---- rocket jump: big velocity kick while holding the RPG ----
		if (wid == WEAPON_RPG && (dv.z > 320.0f || dv.Length2D() > 350.0f))
			s.rjUntil = now + 2.5f;

		s.prevVel = pevPl->velocity;
	}
}

// ---------------- the kill ----------------
void Points_OnKill(CBaseEntity* pVictim, entvars_t* pevAttacker)
{
	if (!pevAttacker || !pVictim)
		return;

	CBaseEntity* pAttacker = CBaseEntity::Instance(pevAttacker);
	if (!pAttacker || !pAttacker->IsPlayer() || pAttacker == pVictim)
		return;

	CBasePlayer* pPlayer = static_cast<CBasePlayer*>(pAttacker);
	entvars_t* pevPl = pPlayer->pev;

	style_state_t& s = g_Style[StyleSlot(pPlayer)];
	const float now = gpGlobals->time;

	// what did the damage, and with which weapon?
	entvars_t* pevInfl = g_pevLastInflictor;
	const char* infl = pevInfl ? STRING(pevInfl->classname) : "";

	int wid = 0;
	CBasePlayerWeapon* pWep = NULL;
	if (pPlayer->m_pActiveItem)
	{
		wid = pPlayer->m_pActiveItem->m_iId;
		pWep = static_cast<CBasePlayerWeapon*>(pPlayer->m_pActiveItem);
	}

	const Vector vicCenter = pVictim->Center();
	const Vector plCenter = pPlayer->Center();
	const float dist = (vicCenter - plCenter).Length();

	// airborne: off the ground, not swimming, not on a ladder / noclip
	const bool airborne =
		(pevPl->flags & FL_ONGROUND) == 0 &&
		pevPl->movetype == MOVETYPE_WALK &&
		pevPl->waterlevel < 2;

	// kill chain
	if (now - s.lastKillTime <= STYLE_CHAIN_WINDOW)
		s.chain++;
	else
		s.chain = 1;

	style_tags_t tags;
	tags.count = 0;
	tags.total = 1.0f;

	// ================= core movement =================
	if (airborne)
		AddTag(tags, STY_AIR, STYLE_AIR_MULT);

	if (s.hops >= 2 && now - s.lastJumpTime < STYLE_BHOP_CHAIN_GAP)
		AddTag(tags, STY_BHOP, STYLE_BHOP_MULT);

	const float speed = pevPl->velocity.Length2D();
	if (speed >= 500.0f)
		AddTag(tags, STY_SPEED, STYLE_SPEED_MULT_3);
	else if (speed >= 400.0f)
		AddTag(tags, STY_SPEED, STYLE_SPEED_MULT_2);
	else if (speed >= 300.0f)
		AddTag(tags, STY_SPEED, STYLE_SPEED_MULT_1);

	if (airborne && (pevPl->flags & FL_DUCKING) != 0)
		AddTag(tags, STY_CRANE, STYLE_CRANE_MULT);

	if (airborne)
	{
		TraceResult tr;
		UTIL_TraceLine(pevPl->origin, pevPl->origin - Vector(0, 0, 2048), ignore_monsters, ENT(pevPl), &tr);
		if (tr.flFraction >= 1.0f || tr.flFraction * 2048.0f > STYLE_GAP_DIST)
			AddTag(tags, STY_GAP, STYLE_GAP_MULT);

		if (pevPl->velocity.z <= STYLE_FALL_SPEED)
			AddTag(tags, STY_FALL, STYLE_FALL_MULT);
	}

	if (plCenter.z - vicCenter.z >= STYLE_HEIGHT_DIST)
		AddTag(tags, STY_HEIGHT, STYLE_HEIGHT_MULT);

	if (now - s.lastFlickTime < STYLE_FLICK_WINDOW)
		AddTag(tags, STY_FLICK, STYLE_FLICK_MULT);

	if (s.chain >= 2)
		AddTag(tags, STY_CHAIN, powf(STYLE_CHAIN_MULT, (float)(s.chain - 1)));

	if (pevPl->health > 0.0f && pevPl->health < STYLE_CLUTCH_HP)
		AddTag(tags, STY_CLUTCH, STYLE_CLUTCH_MULT);

	if (now < s.spinUntil)
	{
		AddTag(tags, STY_SPIN360, STYLE_SPIN_MULT);
		s.spinUntil = -100.0f; // one 360 = one bonus
	}

	// ================= weapons =================
	const bool sameVolley = (now - s.lastKillTime < 0.05f);

	if (FStrEq(infl, "rpg_rocket"))
	{
		AddTag(tags, STY_ROCKET, 1.3f);
		if (airborne)
			AddTag(tags, STY_AIRROCKET, 1.5f);

		CBaseEntity* pRocket = CBaseEntity::Instance(pevInfl);
		const float dRocket = pRocket ? (pevPl->origin - pRocket->pev->origin).Length() : 99999.0f;
		if (now < s.rjUntil || dRocket < STYLE_RJ_RADIUS)
			AddTag(tags, STY_RJ, 2.0f);

		if (sameVolley && s.lastKillInflictor == pevInfl)
			AddTag(tags, STY_STACK, 1.8f);
	}
	else if (FStrEq(infl, "crossbow_bolt"))
	{
		AddTag(tags, STY_BOLT, 1.3f);

		if (dist >= STYLE_SNIPE_RANGE)
			AddTag(tags, STY_SNIPE, 1.6f);

		// pinned: the bolt flies on into a wall right behind the victim
		Vector dir = pevInfl->velocity;
		if (dir.Length() > 1.0f)
		{
			dir = dir.Normalize();
			TraceResult tr;
			UTIL_TraceLine(vicCenter, vicCenter + dir * 56.0f, ignore_monsters, ENT(pVictim->pev), &tr);
			if (tr.flFraction < 1.0f)
				AddTag(tags, STY_PIN, 1.5f);
		}
	}
	else if (FStrEq(infl, "hornet"))
	{
		AddTag(tags, STY_NEST, 1.3f);
		if (CountOwned("hornet", pevPl) >= STYLE_SWARM_COUNT)
			AddTag(tags, STY_SWARM, 1.5f);
	}
	else if (FStrEq(infl, "monster_snark"))
	{
		if (CountOwned("monster_snark", pevPl) >= STYLE_SWARM_COUNT)
			AddTag(tags, STY_CHAOS, 2.5f);
		else
			AddTag(tags, STY_SNARK, 2.0f);
	}
	else if (FStrEq(infl, "monster_satchel"))
	{
		AddTag(tags, STY_SATCHEL, 1.5f);
	}
	else if (FStrEq(infl, "monster_tripmine"))
	{
		AddTag(tags, STY_TRAP, 1.6f);
	}
	else if (FStrEq(infl, "grenade"))
	{
		// MP5 launcher and hand grenades are the same entity: tell them apart by the weapon in hand
		if (wid == WEAPON_MP5)
			AddTag(tags, STY_GRENADE, 1.6f);
		else
			AddTag(tags, STY_FRAG, 1.4f);
	}
	else
	{
		const bool attackHeld = s.prevAttack;
		const float heldFor = attackHeld ? (now - s.attackHeldStart) : 0.0f;

		switch (wid)
		{
		case WEAPON_CROWBAR:
			if (airborne)
				AddTag(tags, STY_BONK, 1.8f);
			else
				AddTag(tags, STY_MELEE, 1.5f);
			MeleeKB_OnMeleeKill(pPlayer); // a melee kill restores 1 knockback charge
			break;

		case WEAPON_GLOCK:
			AddTag(tags, STY_PISTOL, 1.1f);
			if (s.tapShot && now - s.lastPressTime < 0.3f)
				AddTag(tags, STY_TAP, 1.2f);
			if (pWep && pWep->m_iClip == 0)
				AddTag(tags, STY_EMPTY, 1.4f);
			if (now - s.switchTime < STYLE_QUICKDRAW_TIME)
				AddTag(tags, STY_QUICKDRAW, 1.15f);
			break;

		case WEAPON_PYTHON:
			AddTag(tags, STY_HANDCANNON, 1.25f);
			if ((pPlayer->m_iFOV == 0 || pPlayer->m_iFOV >= 80) && dist <= STYLE_HIP_RANGE)
				AddTag(tags, STY_HIP, 1.2f);
			break;

		case WEAPON_MP5:
			AddTag(tags, STY_SPRAY, 1.05f);
			if (airborne && heldFor >= 0.25f)
				AddTag(tags, STY_FROMHIP, 1.3f);
			break;

		case WEAPON_SHOTGUN:
			AddTag(tags, STY_PUMP, 1.15f);
			if (dist <= STYLE_POINTBLANK)
				AddTag(tags, STY_POINTBLANK, 1.4f);
			if ((pVictim->pev->flags & (FL_ONGROUND | FL_FLY | FL_SWIM)) == 0)
				AddTag(tags, STY_AIRSHOT, 1.5f);
			if ((pevPl->button & IN_ATTACK2) != 0)
				AddTag(tags, STY_DOUBLE, 1.35f);
			break;

		case WEAPON_GAUSS:
			AddTag(tags, STY_GAUSS, 1.2f);
			if (sameVolley && s.lastKillWid == WEAPON_GAUSS)
				AddTag(tags, STY_THROUGH, 1.7f);
			if (airborne)
				AddTag(tags, STY_RAIL, 1.6f);
			break;

		case WEAPON_EGON:
			AddTag(tags, STY_BEAM, 1.2f);
			if (attackHeld && heldFor >= STYLE_MELT_HOLD)
				AddTag(tags, STY_MELT, 1.35f);
			if (pWep && pWep->m_iPrimaryAmmoType >= 0 &&
				pPlayer->m_rgAmmo[pWep->m_iPrimaryAmmoType] <= STYLE_OVERHEAT_AMMO)
				AddTag(tags, STY_OVERHEAT, 1.4f);
			break;

		case WEAPON_SNIPER:
		{
			const SniperShot& shot = Sniper_GetShot();

			if (shot.active)
			{
				if (!shot.scoped)
					AddTag(tags, STY_NOSCOPE, STYLE_NOSCOPE_MULT);
				else if (shot.scopeAge <= STYLE_QUICKSCOPE_TIME)
					AddTag(tags, STY_QUICKSCOPE, STYLE_QUICKSCOPE_MULT);

				if (shot.kills >= 1)
					AddTag(tags, STY_COLLAT, STYLE_COLLAT_MULT);
			}

			if (dist >= STYLE_SNIPER_LONG)
				AddTag(tags, STY_LONGRANGE, STYLE_LONGRANGE_MULT);
			else if (dist <= STYLE_SNIPER_CLOSE)
				AddTag(tags, STY_CLOSERANGE, STYLE_CLOSERANGE_MULT);

			Sniper_NoteKill(); // so the next kill of this same shot counts as a collateral
			break;
		}

		default:
			break;
		}
	}

	// ================= result =================
	const float mult = tags.total;

	int shown[3];
	const int shownCount = PickTopTags(tags, shown);
	const int rank = RankFromMult(mult);

	const int gained = (int)(POINTS_BASE * mult + 0.5f);
	pPlayer->m_iPoints += gained;
	HealthOnKill_Grant(pPlayer, (float)gained);

	SendPoints(pPlayer, gained, mult, shown, shownCount, rank);

	// wave grunts may drop rockets / grenades; more points = better odds
	Wave_OnPlayerKill(pVictim, gained);

	s.lastKillTime = now;
	s.lastKillInflictor = pevInfl;
	s.lastKillWid = wid;
}

void Points_OnPlayerDeath(CBasePlayer* pVictim, entvars_t* pevAttacker)
{
	// in multiplayer another player may have killed us
	Points_OnKill(pVictim, pevAttacker);

	Points_Reset(pVictim);
}

void Points_Reset(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return;

	pPlayer->m_iPoints = 0;
	StyleReset(g_Style[StyleSlot(pPlayer)]);
	SendPoints(pPlayer, 0, 0.0f, NULL, 0, 0);
}

void Points_Sync(CBasePlayer* pPlayer)
{
	SendPoints(pPlayer, 0, 0.0f, NULL, 0, 0);
}

// Movement tech points go into the SAME total as kill points. Message layout (client: MsgFunc_TechPts):
//   byte  tech id (TECH_* in movement_tech.h)
//   short points just earned
//   long  new total
void Points_AddTech(CBasePlayer* pPlayer, int techId, int points)
{
	if (!pPlayer || points <= 0)
		return;

	pPlayer->m_iPoints += points;

	if (gmsgTechPts == 0)
		return;

	MESSAGE_BEGIN(MSG_ONE, gmsgTechPts, NULL, pPlayer->pev);
	WRITE_BYTE(techId);
	WRITE_SHORT(points);
	WRITE_LONG(pPlayer->m_iPoints);
	MESSAGE_END();
}
