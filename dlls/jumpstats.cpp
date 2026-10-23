// jumpstats.cpp - Jump stats, core port of uq_jumpstats-2025 (muppet922 / kzuq^BorJomi). Server side, put in dlls/.
//
// Jump types:  LongJump, CountJump / Double CJ / Multi CJ, BhopJump, StandUp BhopJump, WeirdJump,
//              Drop CJ / Drop DCJ / Drop MCJ, Drop BhopJump, Ladder Jump, DuckBhop
// Stats:       distance, prestrafe, max speed, strafes, sync, height, per-strafe gain/loss/time/sync
// Output:      HUD + console for the player (and dead players spectating them), personal best marker
//
// NOT ported: SQL / web / file tops, anti-script, admin and team cvars, weapon-speed tops, edge/block stats,
//             prestrafe trainer HUD, beams, sounds, menus.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "jumpstats.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "movement_tech.h"

// ---------------- tunables ----------------
constexpr float JS_MIN_DISPLAY_DISTANCE = 100.0f; // shorter jumps are not reported
constexpr float JS_BHOP_MAX_GROUND_TIME = 0.1f;	  // jump within this time of landing = bhop
constexpr float JS_CONTEXT_TIME = 0.4f;			  // how long a landing / duck press stays relevant
constexpr float JS_PLAYER_WIDTH = 32.0f;		  // added to the measured distance (hull width)
constexpr float JS_MAX_PRE = 360.0f;			  // prestrafe above this makes the jump count as failed
constexpr float JS_EXTERNAL_VEL_TOL = 2.0f;		  // velocity change outside of player movement (weapons, knockback...) above this = cheat/boost
constexpr int JS_MAX_STRAFES = 14;
constexpr bool JS_DEBUG = false; // prints why a jump was discarded

enum JsType
{
	JS_LJ = 0,
	JS_CJ,
	JS_DCJ,
	JS_MCJ,
	JS_BJ,
	JS_SBJ,
	JS_WJ,
	JS_DROPCJ,
	JS_DROPDCJ,
	JS_DROPMCJ,
	JS_DROPBJ,
	JS_LADDER,
	JS_DUCKBHOP,
	JS_COUNT
};

static const char* const g_JsNames[JS_COUNT] =
	{
		"LongJump", "CountJump", "Double CountJump", "Multi CountJump", "BhopJump", "StandUp BhopJump",
		"WeirdJump", "Drop CountJump", "Drop Double CJ", "Drop Multi CJ", "Drop BhopJump", "Ladder Jump", "DuckBhop"};

struct JsFrame
{
	Vector origin = Vector(0, 0, 0);
	Vector velocity = Vector(0, 0, 0);
	bool ducking = false;
};

struct JsStrafe
{
	float gain = 0.0f;
	float loss = 0.0f;
	int good = 0;
	int bad = 0;
	float startTime = 0.0f;
};

struct JsState
{
	// ---- options (per player) ----
	bool optHud = true;
	bool optConsole = true;
	bool optStrafes = true;
	bool optFails = false;

	// ---- external velocity detection ----
	Vector velAfterMove = Vector(0, 0, 0); // velocity right after the movement code (sampled in PostThink)
	bool haveVelSample = false;

	// ---- ground context ----
	bool wasOnGround = true;
	bool lastAirWasJump = false;
	bool lastAirWasFall = false;
	bool lastJumpDropped = false;
	bool lastLandDucking = false;
	float landTime = -100.0f;
	float groundZ = 0.0f;
	float fallStartZ = 0.0f;
	int duckCount = 0;
	float lastDuckTime = -100.0f;

	// ---- ladder ----
	bool onLadder = false;
	bool ladderKey = false;
	float ladderTopZ = 0.0f;
	float ladderSize = 0.0f;

	// ---- current jump ----
	bool inJump = false;
	JsType type = JS_LJ;
	Vector jumpOrigin = Vector(0, 0, 0);
	float jumpZ = 0.0f;
	float ladderDist = 0.0f;
	float pre = 0.0f;
	float maxSpeed = 0.0f;
	float prevSpeed = 0.0f;
	float preSpeed = 0.0f; // horizontal speed measured in PreThink (for sync)
	float maxZ = 0.0f;
	float startTime = 0.0f;
	int airFrames = 0;
	bool failed = false;
	bool preTooHigh = false;   // prestrafe above JS_MAX_PRE
	bool externalPush = false; // velocity was changed by something other than player movement
	bool haveFail = false;
	JsFrame failFrame;
	JsFrame lastAir;

	// ---- strafes ----
	int strafeNum = 0;
	JsStrafe strafe[JS_MAX_STRAFES + 1];
	int oldKeys = 0;
	float oldYaw = 0.0f;

	// ---- personal bests ----
	float best[JS_COUNT] = {0};
};

static JsState g_Js[33]; // indexed by entity index (1..32)

static JsState& GetState(CBasePlayer* pPlayer)
{
	int idx = pPlayer->entindex();
	if (idx < 0 || idx > 32)
		idx = 0;
	return g_Js[idx];
}

static inline float HDist(const Vector& a, const Vector& b)
{
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	return sqrtf(dx * dx + dy * dy);
}

static inline float NormZ(const Vector& origin, bool ducking)
{
	return ducking ? origin.z + 18.0f : origin.z;
}

static void ResetJumpData(JsState& st)
{
	st.inJump = false;
	st.airFrames = 0;
	st.failed = false;
	st.preTooHigh = false;
	st.externalPush = false;
	st.haveFail = false;
	st.strafeNum = 0;
	for (int i = 0; i <= JS_MAX_STRAFES; i++)
		st.strafe[i] = JsStrafe();
	st.oldKeys = 0;
}

static void CancelJump(JsState& st)
{
	if (st.inJump)
		ResetJumpData(st);
}

static void Debug(CBasePlayer* p, const char* msg)
{
	if (JS_DEBUG)
		CLIENT_PRINTF(p->edict(), print_console, msg);
}

// ---------------- ladder lookup ----------------
static bool FindNearbyLadder(const Vector& org, float& topZ, float& minSize)
{
	CBaseEntity* pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "func_ladder")) != nullptr)
	{
		const Vector mn = pEnt->pev->absmin;
		const Vector mx = pEnt->pev->absmax;
		if (org.x >= mn.x - 18.0f && org.x <= mx.x + 18.0f &&
			org.y >= mn.y - 18.0f && org.y <= mx.y + 18.0f &&
			org.z >= mn.z - 18.0f && org.z <= mx.z + 18.0f)
		{
			const float sx = mx.x - mn.x;
			const float sy = mx.y - mn.y;
			topZ = mx.z;
			minSize = sx <= sy ? sx : sy;
			return true;
		}
	}
	return false;
}

// ---------------- output ----------------
static void ShowHud(CBasePlayer* pTarget, const char* text, int r, int g, int b)
{
	hudtextparms_t p;
	memset(&p, 0, sizeof(p));
	p.x = -1.0f;
	p.y = 0.55f;
	p.effect = 0;
	p.r1 = (byte)r;
	p.g1 = (byte)g;
	p.b1 = (byte)b;
	p.a1 = 0;
	p.r2 = 255;
	p.g2 = 255;
	p.b2 = 250;
	p.a2 = 0;
	p.fadeinTime = 0.1f;
	p.fadeoutTime = 0.3f;
	p.holdTime = 4.0f;
	p.fxTime = 4.0f;
	p.channel = 2;
	UTIL_HudMessage(pTarget, p, text);
}

static void Announce(CBasePlayer* p, JsState& st, JsType t, float dist, bool failed, bool pb, float now)
{
	// strafe summary
	int good = 0, bad = 0;
	const int n = st.strafeNum < JS_MAX_STRAFES ? st.strafeNum : JS_MAX_STRAFES;
	for (int i = 1; i <= n; i++)
	{
		good += st.strafe[i].good;
		bad += st.strafe[i].bad;
	}
	const int sync = (good + bad) > 0 ? (good * 100) / (good + bad) : 0;
	const float height = st.maxZ - st.jumpZ;

	const char* reason = st.externalPush ? "External speed boost (weapon / knockback / push)"
										 : (st.preTooHigh ? "Prestrafe too high (max 320)" : "");
	char hudReason[96] = "";
	char lineReason[96] = "";
	if (reason[0])
	{
		snprintf(hudReason, sizeof(hudReason), "\n%s", reason);
		snprintf(lineReason, sizeof(lineReason), " | %s", reason);
	}

	char hud[256];
	snprintf(hud, sizeof(hud), "%s%s: %.2f units%s\nPre: %.1f   Max: %.1f\nStrafes: %i   Sync: %i   Height: %.1f%s",
		failed ? "FAILED " : "", g_JsNames[t], dist, pb ? "  (PB!)" : "",
		st.pre, st.maxSpeed, st.strafeNum, sync, height, hudReason);

	char line[256];
	snprintf(line, sizeof(line), "\n[JS] %s%s: %.2f units | Pre: %.1f | Max: %.1f | Strafes: %i | Sync: %i | Height: %.1f%s%s\n",
		failed ? "FAILED " : "", g_JsNames[t], dist, st.pre, st.maxSpeed, st.strafeNum, sync, height,
		pb ? " | NEW PB" : "", lineReason);

	if (st.optConsole)
	{
		CLIENT_PRINTF(p->edict(), print_console, line);

		if (st.optStrafes && n > 0)
		{
			CLIENT_PRINTF(p->edict(), print_console, "  (time and sync are in percent)\n");
			const float total = now - st.startTime;
			for (int i = 1; i <= n; i++)
			{
				const JsStrafe& s = st.strafe[i];
				const float end = (i < st.strafeNum && i < JS_MAX_STRAFES) ? st.strafe[i + 1].startTime : now;
				const float pct = total > 0.0f ? ((end - s.startTime) * 100.0f) / total : 0.0f;
				const int fr = s.good + s.bad;
				const int ssync = fr > 0 ? (s.good * 100) / fr : 0;
				char sl[160];
				snprintf(sl, sizeof(sl), "  #%-2i  gain +%-6.1f  loss -%-6.1f  time %3.0f  sync %3i\n", i, s.gain, s.loss, pct, ssync);
				CLIENT_PRINTF(p->edict(), print_console, sl);
			}
			if (st.strafeNum > JS_MAX_STRAFES)
				CLIENT_PRINTF(p->edict(), print_console, "  (only the first 14 strafes are listed)\n");
		}
	}

	if (st.optHud)
	{
		if (failed)
			ShowHud(p, hud, 255, 60, 60);
		else
			ShowHud(p, hud, 60, 255, 120);
	}

	// dead players spectating this player
	const int iSelf = p->entindex();
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pSpec = (CBasePlayer*)UTIL_PlayerByIndex(i);
		if (!pSpec || pSpec == p || !pSpec->IsPlayer() || pSpec->IsAlive())
			continue;
		if (pSpec->pev->flags & FL_FAKECLIENT)
			continue;
		if (pSpec->pev->iuser2 != iSelf)
			continue;
		if (GetState(pSpec).optHud)
			ShowHud(pSpec, hud, failed ? 255 : 60, failed ? 60 : 255, failed ? 60 : 120);
	}
}

// ---------------- jump lifecycle ----------------
static void StartJump(CBasePlayer* p, JsState& st, const JsFrame& cur, float now, float speed)
{
	const float sinceLand = now - st.landTime;
	const bool fresh = sinceLand <= JS_CONTEXT_TIME;
	const bool fallCtx = st.lastAirWasFall && fresh;
	const bool bhopCtx = st.lastAirWasJump && sinceLand <= JS_BHOP_MAX_GROUND_TIME;
	int ducks = (st.duckCount > 0 && (now - st.lastDuckTime) <= JS_CONTEXT_TIME) ? st.duckCount : 0;
	if (ducks == 0 && cur.ducking && !bhopCtx)
		ducks = 1; // already crouched when jump was pressed (held duck, then jump)

	JsType t;
	if (ducks >= 1)
	{
		if (fallCtx)
			t = ducks == 1 ? JS_DROPCJ : (ducks == 2 ? JS_DROPDCJ : JS_DROPMCJ);
		else
			t = ducks == 1 ? JS_CJ : (ducks == 2 ? JS_DCJ : JS_MCJ);
	}
	else if (bhopCtx)
	{
		if (st.lastJumpDropped)
			t = JS_DROPBJ;
		else if (cur.ducking)
			t = JS_DUCKBHOP;
		else if (st.lastLandDucking)
			t = JS_SBJ;
		else
			t = JS_BJ;
	}
	else if (fallCtx)
	{
		t = JS_WJ;
	}
	else
	{
		t = JS_LJ;
	}

	ResetJumpData(st);
	st.inJump = true;
	st.type = t;
	st.jumpOrigin = cur.origin;
	st.jumpZ = NormZ(cur.origin, cur.ducking);
	st.ladderDist = 0.0f;
	st.pre = speed;
	st.preTooHigh = speed > JS_MAX_PRE;
	st.maxSpeed = speed;
	st.prevSpeed = speed;
	st.maxZ = st.jumpZ;
	st.startTime = now;
	st.wasOnGround = false;
	st.duckCount = 0;
	st.lastAir = cur;
}

static void StartLadderJump(CBasePlayer* p, JsState& st, const JsFrame& cur, float now, float speed)
{
	ResetJumpData(st);
	st.inJump = true;
	st.type = JS_LADDER;
	st.jumpOrigin = cur.origin;
	st.jumpZ = st.ladderTopZ + 35.03125f;
	st.ladderDist = st.ladderSize + 0.03125f;
	st.pre = speed;
	st.preTooHigh = speed > JS_MAX_PRE;
	st.maxSpeed = speed;
	st.prevSpeed = speed;
	st.maxZ = NormZ(cur.origin, cur.ducking);
	st.startTime = now;
	st.wasOnGround = false;
	st.lastAirWasJump = false;
	st.lastAirWasFall = false;
	st.lastAir = cur;
}

static void FinishJump(CBasePlayer* p, JsState& st, const JsFrame& land, float now)
{
	const float g = CVAR_GET_FLOAT("sv_gravity");
	const JsType t = st.type;
	const bool ducking = land.ducking;
	const float nzL = NormZ(land.origin, ducking);
	const float hd = st.jumpZ - nzL; // > 0: landed lower than the takeoff
	bool failed = st.failed || st.preTooHigh || st.externalPush;

	// discard / fail rules (same as the original)
	if (t == JS_LADDER)
	{
		if (fabsf(st.jumpZ - nzL) > 4.0f)
			failed = true;
	}
	else
	{
		if (hd < -1.0f)
		{
			Debug(p, "[js] discarded: landed higher than the takeoff\n");
			return;
		}
		if (t == JS_SBJ)
		{
			if (hd > 1.0f)
				failed = true;
			else if (hd > 0.0f)
			{
				Debug(p, "[js] discarded: standup bhop landing height\n");
				return;
			}
		}
		else if (t == JS_DROPBJ && hd > 0.02f)
		{
			failed = true;
		}
	}

	const float width = (t == JS_LADDER) ? st.ladderDist : JS_PLAYER_WIDTH;

	float dist;
	if (failed && st.haveFail)
	{
		// distance at the moment the player dropped below the takeoff height
		const JsFrame& f = st.failFrame;
		const float zF = NormZ(f.origin, f.ducking);
		const float delta = zF - st.jumpZ;
		const float vz = f.velocity.z;
		const float disc = vz * vz + 2.0f * g * delta;
		float d = HDist(st.jumpOrigin, f.origin) + width;
		if (disc >= 0.0f && g > 0.0f)
		{
			const float tt = (vz + sqrtf(disc)) / g;
			if (tt >= 0.0f && tt <= 0.1f)
			{
				Vector pt = f.origin;
				pt.x += f.velocity.x * tt;
				pt.y += f.velocity.y * tt;
				d = HDist(st.jumpOrigin, pt) + width;
			}
		}
		dist = d;
	}
	else
	{
		// distance1: landing frame position; distance2: position interpolated to the exact landing height
		const float d1 = HDist(st.jumpOrigin, land.origin) + width;
		float d2 = d1;

		const JsFrame& a = st.lastAir;
		const float zA = NormZ(a.origin, a.ducking);
		const float delta = zA - nzL;
		const float vz = a.velocity.z;
		const float disc = vz * vz + 2.0f * g * delta;
		if (disc >= 0.0f && g > 0.0f)
		{
			const float tt = (vz + sqrtf(disc)) / g;
			if (tt >= 0.0f && tt <= 0.1f)
			{
				Vector pt = a.origin;
				pt.x += a.velocity.x * tt;
				pt.y += a.velocity.y * tt;
				d2 = HDist(st.jumpOrigin, pt) + width;
			}
		}
		dist = d1 < d2 ? d1 : d2;
	}

	if (dist < JS_MIN_DISPLAY_DISTANCE)
		return;

	if (failed && !st.optFails)
		return;

	bool pb = false;
	if (!failed && dist > st.best[t])
	{
		st.best[t] = dist;
		pb = true;
	}

	Announce(p, st, t, dist, failed, pb, now);
}

// ---------------- hooks ----------------
void JumpStats_PreThink(CBasePlayer* p)
{
	MoveTech_PreThink(p);

	JsState& st = GetState(p);
	entvars_t* pev = p->pev;
	const float now = gpGlobals->time;

	if (!p->IsAlive())
	{
		CancelJump(st);
		st.onLadder = false;
		st.ladderKey = false;
		st.lastAirWasJump = false;
		st.lastAirWasFall = false;
		return;
	}

	// ---- on a ladder: remember it, cancel any jump ----
	if (pev->movetype == MOVETYPE_FLY)
	{
		CancelJump(st);
		float top, size;
		if (FindNearbyLadder(pev->origin, top, size))
		{
			st.ladderTopZ = top;
			st.ladderSize = size;
		}
		if (pev->button & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT))
			st.ladderKey = true;
		st.onLadder = true;
		return;
	}

	// ---- anything but normal walking (noclip, water...) ----
	if (pev->movetype != MOVETYPE_WALK || pev->waterlevel >= 2)
	{
		CancelJump(st);
		st.onLadder = false;
		st.ladderKey = false;
		st.lastAirWasJump = false;
		st.lastAirWasFall = false;
		return;
	}

	const bool onGround = (pev->flags & FL_ONGROUND) != 0;
	const bool ducking = (pev->flags & FL_DUCKING) != 0;

	JsFrame cur;
	cur.origin = pev->origin;
	cur.velocity = pev->velocity;
	cur.ducking = ducking;

	const float speed = Vector(pev->velocity.x, pev->velocity.y, 0.0f).Length();
	st.preSpeed = speed;

	// Any change of the velocity between the end of the last movement (PostThink) and now was made by game code,
	// not by the player's own movement: weapons (tau cannon), knockback, grapple, slam, explosions...
	// basevelocity covers trigger_push, conveyors and moving platforms.
	bool ext = false;
	if (st.haveVelSample)
	{
		const Vector dv = pev->velocity - st.velAfterMove;
		if (dv.Length() > JS_EXTERNAL_VEL_TOL)
			ext = true;
	}
	if (pev->basevelocity.Length() > 0.0f)
		ext = true;
	if (ext && st.inJump)
		st.externalPush = true;

	// ---- just left a ladder ----
	if (st.onLadder)
	{
		st.onLadder = false;
		if (!onGround && st.ladderKey && st.ladderSize > 0.0f)
		{
			StartLadderJump(p, st, cur, now, speed);
			if (ext)
				st.externalPush = true;
		}
		st.ladderKey = false;
		st.wasOnGround = false;
		st.fallStartZ = NormZ(cur.origin, ducking); // so stepping off a ladder never counts as a drop
		if (st.inJump)
			return; // the ladder jump starts next frame's bookkeeping
	}

	// ======== on the ground ========
	if (onGround)
	{
		const float nzNow = NormZ(pev->origin, ducking);

		if (!st.wasOnGround)
		{
			if (st.inJump && st.airFrames < 3)
			{
				// the jump press never really left the ground: ignore it
				ResetJumpData(st);
			}
			else
			{
				if (st.inJump)
				{
					const float drop = st.jumpZ - nzNow;
					st.lastJumpDropped = (drop > 2.0f && drop < 30.0f);
					st.lastAirWasJump = (st.type != JS_LADDER);
					st.lastAirWasFall = false;
					FinishJump(p, st, cur, now);
					ResetJumpData(st);
				}
				else
				{
					st.lastAirWasJump = false;
					st.lastJumpDropped = false;
					st.lastAirWasFall = (st.fallStartZ - nzNow) > 1.0f;
				}
				st.landTime = now;
				st.lastLandDucking = ducking;
				st.duckCount = 0;
			}
			st.wasOnGround = true;
		}

		st.groundZ = nzNow;

		const bool jumpPressed = (p->m_afButtonPressed & IN_JUMP) != 0 && !st.inJump;

		// old duck presses expire 0.4s after duck was last pressed OR held (so the window runs from the release)
		if (st.duckCount > 0 && (now - st.lastDuckTime) > JS_CONTEXT_TIME)
			st.duckCount = 0;

		// takeoff, decided BEFORE counting this frame's duck press: duck and jump pressed in the very same frame
		// (the "+duck; +jump" bind) is a plain long jump, not a count jump
		if (jumpPressed)
		{
			StartJump(p, st, cur, now, speed);
			if (ext)
				st.externalPush = true; // boosted right before takeoff
			return;
		}

		// duck presses on the ground (count jump detection)
		if (p->m_afButtonPressed & IN_DUCK)
			st.duckCount++;
		if ((pev->button & IN_DUCK) || ducking)
			st.lastDuckTime = now;

		return;
	}

	// ======== in the air ========
	if (st.wasOnGround && !st.inJump)
	{
		// walked off a ledge without jumping
		st.wasOnGround = false;
		st.fallStartZ = st.groundZ;
	}

	if (!st.inJump)
		return;

	st.airFrames++;

	// failed: dropped below the takeoff height (same test as the original)
	if (!st.failed)
	{
		if (cur.origin.z + 18.0f - st.jumpZ < 0.0f)
			st.failed = true;
		else if (NormZ(cur.origin, ducking) >= st.jumpZ)
		{
			st.failFrame = cur;
			st.haveFail = true;
		}
	}

	const float nz = NormZ(cur.origin, ducking);
	if (nz > st.maxZ)
		st.maxZ = nz;

	// speed gained / lost, per strafe (index 0 = before the first strafe)
	const int si = st.strafeNum <= JS_MAX_STRAFES ? st.strafeNum : -1;
	if (speed > st.maxSpeed)
	{
		if (si >= 0)
			st.strafe[si].gain += speed - st.maxSpeed;
		st.maxSpeed = speed;
	}
	if (speed < st.prevSpeed && si >= 0)
		st.strafe[si].loss += st.prevSpeed - speed;
	st.prevSpeed = speed;

	st.lastAir = cur;
}

void JumpStats_PostThink(CBasePlayer* p)
{
	JsState& st = GetState(p);
	entvars_t* pev = p->pev;

	// velocity right after the movement code; anything that changes it before the next PreThink is external
	st.velAfterMove = pev->velocity;
	st.haveVelSample = true;

	// view turning
	const float yaw = pev->v_angle.y;
	float dyaw = yaw - st.oldYaw;
	if (dyaw > 180.0f)
		dyaw -= 360.0f;
	else if (dyaw < -180.0f)
		dyaw += 360.0f;
	st.oldYaw = yaw;

	if (!st.inJump)
	{
		st.oldKeys = 0;
		return;
	}

	const bool turning = dyaw != 0.0f;
	const int mask = IN_MOVELEFT | IN_MOVERIGHT | IN_FORWARD | IN_BACK;
	const int keys = pev->button & mask;

	int nkeys = 0;
	for (int k = keys; k; k &= (k - 1))
		nkeys++;

	// new strafe: a single direction key freshly pressed while turning
	if (nkeys == 1 && turning && !(st.oldKeys & keys))
	{
		st.strafeNum++;
		if (st.strafeNum <= JS_MAX_STRAFES)
			st.strafe[st.strafeNum].startTime = gpGlobals->time;
	}

	// sync: did the horizontal speed go up this frame while a movement key was held?
	if (keys)
	{
		const float after = Vector(pev->velocity.x, pev->velocity.y, 0.0f).Length();
		if (st.strafeNum <= JS_MAX_STRAFES)
		{
			if (after > st.preSpeed)
				st.strafe[st.strafeNum].good++;
			else
				st.strafe[st.strafeNum].bad++;
		}
	}

	if (nkeys > 1)
		st.oldKeys = 0;
	else if (turning)
		st.oldKeys = keys;
}

static void Toggle(CBasePlayer* p, bool& opt, const char* name)
{
	opt = !opt;
	char msg[96];
	snprintf(msg, sizeof(msg), "[JS] %s %s\n", name, opt ? "ON" : "OFF");
	CLIENT_PRINTF(p->edict(), print_console, msg);
}

bool JumpStats_Command(CBasePlayer* p, const char* pcmd)
{
	if (!p || !pcmd)
		return false;

	JsState& st = GetState(p);

	if (FStrEq(pcmd, "js_hud"))
		Toggle(p, st.optHud, "HUD stats");
	else if (FStrEq(pcmd, "js_console"))
		Toggle(p, st.optConsole, "Console stats");
	else if (FStrEq(pcmd, "js_strafes"))
		Toggle(p, st.optStrafes, "Per-strafe table");
	else if (FStrEq(pcmd, "js_fails"))
		Toggle(p, st.optFails, "Failed jump stats");
	else if (FStrEq(pcmd, "jumpstats"))
		CLIENT_PRINTF(p->edict(), print_console,
			"[JS] commands: js_hud, js_console, js_strafes (per-strafe table), js_fails (failed jumps)\n");
	else
		return false;

	return true;
}

void JumpStats_Reset(CBasePlayer* p)
{
	JsState& st = GetState(p);
	entvars_t* pev = p->pev;
	const bool ducking = (pev->flags & FL_DUCKING) != 0;

	CancelJump(st);
	st.onLadder = false;
	st.ladderKey = false;
	st.lastAirWasJump = false;
	st.lastAirWasFall = false;
	st.lastJumpDropped = false;
	st.landTime = -100.0f;
	st.duckCount = 0;
	st.haveVelSample = false;

	// the new position must not look like a fall from the old one
	st.groundZ = NormZ(pev->origin, ducking);
	st.fallStartZ = st.groundZ;
}