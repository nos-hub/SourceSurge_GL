// movement_tech.cpp - style points for movement tech (server side, put in dlls/)
//
//   Wallbug      +20  every 0.5 s while it lasts
//   Surf         +10  every 0.5 s while surfing
//   Rampslide    +5   every 0.5 s while sliding
//   Fast ladder  +5   every 0.5 s while climbing faster than normal
//   Wall strafe  +5   every 0.5 s while wall strafing
//   Perfect bhop +30  each
//   Edgebug      +1000 (single), +3000 (double), +5000 (triple and beyond), per edgebug
//   Jumpbug      +5000 each
//
// The first points of a continuous tech come the moment it starts, then every 0.5 s while it lasts.
// Every award goes into the SAME total as kill points (CBasePlayer::m_iPoints) through
// Points_AddTech(), and the HUD shows the tech name with the points under it.
//
// Detection is heuristic where the technique has no exact definition: see the comments on each
// detector and tune the numbers in the block below.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "points.h"
#include "movement_tech.h"
#include <math.h>

// ---------------- points ----------------
constexpr int TECH_PTS_WALLBUG = 20;   // per TECH_TICK
constexpr int TECH_PTS_SURF = 10;	   // per TECH_TICK
constexpr int TECH_PTS_RAMPSLIDE = 5;  // per TECH_TICK
constexpr int TECH_PTS_FASTLADDER = 5; // per TECH_TICK
constexpr int TECH_PTS_WALLSTRAFE = 5; // per TECH_TICK
constexpr int TECH_PTS_PERFBHOP = 30;
constexpr int TECH_PTS_JUMPBUG = 5000;
constexpr int TECH_PTS_EDGEBUG[3] = {1000, 3000, 5000}; // 1st, 2nd (double), 3rd and later (triple)

// ---------------- timing ----------------
constexpr float TECH_TICK = 0.5f;  // points are awarded every time a tech has been active this long
constexpr float TECH_GRACE = 0.2f; // a tech may flicker off this long without restarting its 0.5 s counter

// ---------------- detection numbers ----------------
constexpr float TECH_CONTACT_DIST = 2.0f;	  // how close a surface must be to count as "touching"
constexpr float TECH_PERF_MIN_SPEED = 250.0f; // horizontal speed needed for a perfect bhop to count
constexpr float TECH_JB_MIN_FALL = 100.0f;	  // jumpbug: falling at least this fast the frame before
constexpr float TECH_JB_MIN_UP = 200.0f;	  // jumpbug: vertical speed right after (a jump is about +268)
constexpr float TECH_JB_COOLDOWN = 0.5f;
constexpr float TECH_FASTLADDER_SPEED = 210.0f;		// normal ladder climb is 200 u/s; faster = ladder speeding
constexpr float TECH_WALLSTRAFE_MIN_SPEED = 300.0f; // horizontal speed
constexpr float TECH_SURF_MIN_SPEED = 250.0f;		// total speed
constexpr float TECH_RAMPSLIDE_MIN_SPEED = 300.0f;	// total speed
constexpr float TECH_SURF_NZ_MIN = 0.05f;			// ramp normal z range counted as surfable (steeper than 45.57 degrees)
constexpr float TECH_SURF_NZ_MAX = 0.70f;
constexpr float TECH_RAMP_NZ_MIN = 0.70f; // walkable but tilted ramp
constexpr float TECH_RAMP_NZ_MAX = 0.985f;
constexpr float TECH_WALL_NZ_MAX = 0.04f;		// |normal z| below this = vertical wall
constexpr int TECH_WALLBUG_FRAMES = 4;			// frames of suppressed fall acceleration before it counts as a wallbug
constexpr float TECH_WALLBUG_ACCEL_FRAC = 0.4f; // falling slower than this fraction of gravity = "held"
constexpr float TECH_RAMP_JUMP_GRACE = 0.5f;	// ignore rampslide this long after a jump press

constexpr float kPi = 3.14159265f;

// Prints to YOUR console: a status line every 2 s (proves MoveTech_PreThink is running and shows what it sees)
// and a line for every award. Set to false when everything works.
constexpr bool TECH_DEBUG = true;

struct TechState
{
	bool init = false;
	Vector prevVel = Vector(0, 0, 0);
	bool prevOnGround = false;
	bool prevDucking = false;
	float prevDt = 0.0f;
	int btn = 0;	 // buttons of the previous command
	int btnPrev = 0; // buttons of the command before that
	int groundFrames = 0;
	float lastJumpPress = -100.0f;
	bool perfPending = false;
	float nextJumpbug = -100.0f;
	int heldFrames = 0;
	float active[TECH_COUNT] = {0};
	float idle[TECH_COUNT] = {0};
	bool started[TECH_COUNT] = {false}; // the tech is currently running (first award already handled)
	float lastAward[TECH_COUNT];		// when each tech last scored
	float nextBeat = 0.0f;

	TechState()
	{
		for (int i = 0; i < TECH_COUNT; i++)
			lastAward[i] = -100.0f;
	}
};

static TechState g_Tech[33]; // indexed by entity index (1..32)

static TechState& GetState(CBasePlayer* p)
{
	int idx = p->entindex();
	if (idx < 0 || idx > 32)
		idx = 0;
	return g_Tech[idx];
}

// ---------------- surface probes ----------------
// A short hull trace from the player along dir. Returns the surface normal if something solid is within reach.
static bool TraceContact(CBasePlayer* p, const Vector& dir, Vector& normal)
{
	entvars_t* pev = p->pev;
	const int hull = (pev->flags & FL_DUCKING) ? head_hull : human_hull;

	TraceResult tr;
	UTIL_TraceHull(pev->origin, pev->origin + dir * TECH_CONTACT_DIST, ignore_monsters, hull, ENT(pev), &tr);

	if (tr.fAllSolid || tr.fStartSolid || tr.flFraction >= 1.0f)
		return false;

	normal = tr.vecPlaneNormal;
	return true;
}

// A vertical wall touching the player on any side.
static bool FindWall(CBasePlayer* p, Vector& normal)
{
	for (int k = 0; k < 8; k++)
	{
		const float a = k * (kPi / 4.0f);
		Vector n;
		if (TraceContact(p, Vector(cosf(a), sinf(a), 0.0f), n) && fabsf(n.z) < TECH_WALL_NZ_MAX)
		{
			normal = n;
			return true;
		}
	}
	return false;
}

// A tilted surface touching the player whose normal z is in [minNz, maxNz). downOnly = look below only.
static bool FindPlane(CBasePlayer* p, float minNz, float maxNz, bool downOnly, Vector& normal)
{
	Vector n;
	if (TraceContact(p, Vector(0.0f, 0.0f, -1.0f), n) && n.z >= minNz && n.z < maxNz)
	{
		normal = n;
		return true;
	}
	if (downOnly)
		return false;

	for (int k = 0; k < 8; k++)
	{
		const float a = k * (kPi / 4.0f);
		if (TraceContact(p, Vector(cosf(a), sinf(a), 0.0f), n) && n.z >= minNz && n.z < maxNz)
		{
			normal = n;
			return true;
		}
	}
	return false;
}

// ---------------- awards ----------------
static void Award(CBasePlayer* p, int tech, int points)
{
	Points_AddTech(p, tech, points);

	if (TECH_DEBUG)
		CLIENT_PRINTF(p->edict(), print_console, UTIL_VarArgs("[tech] +%d (tech id %d)\n", points, tech));
}

// Continuous techs: the FIRST award comes the moment the tech starts, then one more every TECH_TICK seconds
// while it keeps going. A short interruption (TECH_GRACE) does not restart it. To stop on/off toggling from
// scoring faster than the normal rate, a restart only scores right away if TECH_TICK has passed since the
// last award of that tech; otherwise the next award waits for that moment.
static void Tick(CBasePlayer* p, TechState& st, int tech, bool activeNow, float now, float dt, int points)
{
	if (activeNow)
	{
		if (!st.started[tech])
		{
			st.started[tech] = true;
			const float since = now - st.lastAward[tech];
			st.active[tech] = since >= TECH_TICK ? TECH_TICK : (since > 0.0f ? since : 0.0f);
		}
		else
		{
			st.active[tech] += dt;
		}

		st.idle[tech] = 0.0f;
		while (st.active[tech] >= TECH_TICK)
		{
			st.active[tech] -= TECH_TICK;
			st.lastAward[tech] = now;
			Award(p, tech, points);
		}
	}
	else
	{
		st.idle[tech] += dt;
		if (st.idle[tech] > TECH_GRACE)
		{
			st.active[tech] = 0.0f;
			st.started[tech] = false;
		}
	}
}

static void ResetState(TechState& st)
{
	st = TechState();
}

void MoveTech_OnEdgebug(CBasePlayer* p, int chain)
{
	if (!p)
		return;

	int idx = chain < 1 ? 0 : (chain > 3 ? 2 : chain - 1);
	Award(p, TECH_EDGEBUG + idx, TECH_PTS_EDGEBUG[idx]);
}

void MoveTech_PreThink(CBasePlayer* p)
{
	TechState& st = GetState(p);
	entvars_t* pev = p->pev;

	// only living players on foot, on a ladder, or in the air
	if (!p->IsAlive() || (pev->flags & FL_SPECTATOR) || pev->waterlevel >= 2 ||
		(pev->movetype != MOVETYPE_WALK && pev->movetype != MOVETYPE_FLY))
	{
		ResetState(st);
		return;
	}

	const float now = gpGlobals->time;
	float dt = gpGlobals->frametime;
	if (dt < 0.0f)
		dt = 0.0f;
	if (dt > 0.1f)
		dt = 0.1f;

	const bool onLadder = (pev->movetype == MOVETYPE_FLY);
	const bool onGround = (pev->flags & FL_ONGROUND) != 0;
	const bool ducking = (pev->flags & FL_DUCKING) != 0;
	const Vector vel = pev->velocity;
	const float hspeed = sqrtf(vel.x * vel.x + vel.y * vel.y);
	const float speed3 = vel.Length();
	const int buttons = pev->button;
	const bool jumpEdge = (p->m_afButtonPressed & IN_JUMP) != 0;

	// first frame: only take a baseline
	if (!st.init)
	{
		ResetState(st);
		st.init = true;
		st.prevVel = vel;
		st.prevOnGround = onGround;
		st.prevDucking = ducking;
		st.prevDt = dt;
		st.btn = buttons;
		st.btnPrev = buttons;
		return;
	}

	if (jumpEdge)
		st.lastJumpPress = now;

	if (onGround)
		st.groundFrames++;
	else
		st.groundFrames = 0;

	// ---------------- perfect bhop ----------------
	// The jump is pressed on the FIRST frame the player is on the ground after a hop, so no friction is applied.
	// Confirmed one frame later (the player really left the ground with a jump's vertical speed).
	if (st.perfPending)
	{
		st.perfPending = false;
		if (!onGround && vel.z >= 180.0f)
			Award(p, TECH_PERFBHOP, TECH_PTS_PERFBHOP);
	}
	if (!onLadder && onGround && st.groundFrames == 1 && !st.prevOnGround && jumpEdge && hspeed >= TECH_PERF_MIN_SPEED)
		st.perfPending = true;

	// ---------------- jumpbug ----------------
	// Starts and ends the movement frame in the air, but in between the player landed and jumped: the vertical
	// speed flips from falling to a jump's +268 without the on-ground flag ever being set. Needs jump pressed
	// on that command and the player ducked before it (jumpbug = duck, jump and unduck near the ground).
	{
		const bool jumpPressedLastCmd = (st.btn & IN_JUMP) != 0 && (st.btnPrev & IN_JUMP) == 0;
		const bool wasDucked = st.prevDucking || (st.btnPrev & IN_DUCK) != 0;

		if (!onLadder && !onGround && !st.prevOnGround &&
			st.prevVel.z <= -TECH_JB_MIN_FALL && vel.z >= TECH_JB_MIN_UP && vel.z <= 400.0f &&
			jumpPressedLastCmd && wasDucked && now >= st.nextJumpbug)
		{
			Award(p, TECH_JUMPBUG, TECH_PTS_JUMPBUG);
			st.nextJumpbug = now + TECH_JB_COOLDOWN;
		}
	}

	// ---------------- fast ladder ----------------
	// Ladder climbing is 200 u/s with one method; pointing up/down AND moving along the ladder doubles it.
	Tick(p, st, TECH_FASTLADDER, onLadder && fabsf(vel.z) > TECH_FASTLADDER_SPEED, now, dt, TECH_PTS_FASTLADDER);

	// ---------------- walls: wallbug and wall strafing ----------------
	bool haveWall = false;
	Vector wn(0, 0, 0);
	if (!onLadder && (!onGround || hspeed >= 150.0f))
		haveWall = FindWall(p, wn);

	// Wallbug (no exact definition exists for this engine, so this is a heuristic): in the air, pressed against a
	// vertical wall, and the fall is being "held": vertical speed changes far less than gravity says it should for
	// several frames in a row. Normal wall contact never does that (vertical walls have no friction).
	bool wallbugNow = false;
	if (!onGround && haveWall && st.prevDt > 0.001f)
	{
		const float g = CVAR_GET_FLOAT("sv_gravity");
		const float dvz = vel.z - st.prevVel.z;
		if (dvz > -TECH_WALLBUG_ACCEL_FRAC * g * st.prevDt)
			st.heldFrames++;
		else
			st.heldFrames = 0;
		wallbugNow = st.heldFrames >= TECH_WALLBUG_FRAMES;
	}
	else
	{
		st.heldFrames = 0;
	}
	Tick(p, st, TECH_WALLBUG, wallbugNow, now, dt, TECH_PTS_WALLBUG);

	// Wall strafing: rubbing against a wall while holding the strafe key towards it, moving along the wall,
	// faster than normal running speed.
	bool wallStrafeNow = false;
	if (haveWall && hspeed >= TECH_WALLSTRAFE_MIN_SPEED)
	{
		const float yaw = pev->v_angle.y * (kPi / 180.0f);
		const float rx = sinf(yaw);
		const float ry = -cosf(yaw);
		const float side = wn.x * rx + wn.y * ry; // wall on the right -> normal points left -> negative

		const bool wallRight = side <= -0.5f;
		const bool wallLeft = side >= 0.5f;
		const bool keyRight = (buttons & IN_MOVERIGHT) != 0;
		const bool keyLeft = (buttons & IN_MOVELEFT) != 0;

		const float along = fabsf((vel.x * wn.x + vel.y * wn.y) / hspeed); // 0 = parallel to the wall
		wallStrafeNow = ((wallRight && keyRight) || (wallLeft && keyLeft)) && along < 0.4f;
	}
	Tick(p, st, TECH_WALLSTRAFE, wallStrafeNow, now, dt, TECH_PTS_WALLSTRAFE);

	// ---------------- surf and rampslide ----------------
	bool surfNow = false;
	bool rampNow = false;
	if (!onGround && !onLadder)
	{
		Vector pn(0, 0, 0);

		// Surf: in the air, touching a ramp too steep to stand on, with real speed.
		if (speed3 >= TECH_SURF_MIN_SPEED && FindPlane(p, TECH_SURF_NZ_MIN, TECH_SURF_NZ_MAX, false, pn))
			surfNow = true;

		// Rampslide: the engine says "in the air" (moving up fast) while the player is touching a ramp it could
		// stand on: so no friction. Not counted right after a jump (that is just leaving the ground).
		if (speed3 >= TECH_RAMPSLIDE_MIN_SPEED && (now - st.lastJumpPress) > TECH_RAMP_JUMP_GRACE &&
			FindPlane(p, TECH_RAMP_NZ_MIN, TECH_RAMP_NZ_MAX, true, pn))
			rampNow = true;
	}
	Tick(p, st, TECH_SURF, surfNow, now, dt, TECH_PTS_SURF);
	Tick(p, st, TECH_RAMPSLIDE, rampNow, now, dt, TECH_PTS_RAMPSLIDE);

	// ---------------- debug status line ----------------
	if (TECH_DEBUG && now >= st.nextBeat)
	{
		st.nextBeat = now + 2.0f;
		CLIENT_PRINTF(p->edict(), print_console,
			UTIL_VarArgs("[tech] running | ground=%d ladder=%d hspeed=%.0f vz=%.0f wall=%d surf=%d ramp=%d wstrafe=%d wallbug=%d held=%d dt=%.4f\n",
				onGround ? 1 : 0, onLadder ? 1 : 0, hspeed, vel.z, haveWall ? 1 : 0, surfNow ? 1 : 0, rampNow ? 1 : 0,
				wallStrafeNow ? 1 : 0, wallbugNow ? 1 : 0, st.heldFrames, dt));
	}

	// ---------------- remember this frame ----------------
	st.prevVel = vel;
	st.prevOnGround = onGround;
	st.prevDucking = ducking;
	st.prevDt = dt;
	st.btnPrev = st.btn;
	st.btn = buttons;
}