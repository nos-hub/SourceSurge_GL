// jumpstats.cpp - C++ port of GCLJS 2.0.0dev (GameChaos's Longjumps). Server side, put in dlls/.
//
// Original plugin: https://codeberg.org/GameChaos/gcljs (AMX Mod X, by GameChaos). Keep the credit
// and check the repo's LICENSE before distributing this.
//
// Jump types:  LJ, CJ / DCJ / MCJ / SCJ, WJ, LDJ (ladder), BH, DBH (duck bhop), SBJ, SLJ
//              (DD and GS are tracked internally for CJ detection but never printed)
// Stats:       distance, XJ distance, block distance, edge, land edge, veer, jumpoff angle, airpath,
//              forward release, sync, prestrafe, max speed, stamina, height, loss, potency,
//              overlap / dead air, per strafe table (sync, gain, loss, max, air, OL/DA, efficiency)
// Output:      chat, console, jump beam and veer beam, plus the "JumpStat" user message that feeds the client
//              HUD (cl_dll/hud_jumpstats.cpp: jump info, strafe graph, strafe table, speed panel).
//              Everything goes to the player and to dead players spectating them.
// Failstats:   printed with a "FAILED" prefix.
//
// NOT ported:  SQL, sounds / failstat sounds, option menus (console commands instead), weapon speed stats
//              (that was CS 1.6 only), AMXX config autogeneration (gcljs.cfg is written by gcljs_savedefaults).
//
// Chat commands ("say /xxx") and console commands:
//   /jumpbeam      gcljs_jumpbeam      toggle the jump beam
//   /veerbeam      gcljs_veerbeam      toggle the veer beam
//   /gcljs         gcljs_options       list your options. "gcljs_options <option> [value]" changes one
//   /gcljsversion  gcljs_version       print the version
//   gcljs_defaults [<option> <value>]  show / set the server defaults (listen server host only)
//   gcljs_savedefaults                 write the defaults to gcljs.cfg (listen server host only)

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "pm_defs.h"
#include "usercmd.h"
#include "jumpstats.h"
#include "movement_tech.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits>

#ifdef _WIN32
#include <direct.h>
#define JS_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define JS_MKDIR(p) mkdir(p, 0755)
#endif

// ------------------------------------------------------------------------------------------------
// build options
// ------------------------------------------------------------------------------------------------
#define JS_VERSION "2.0.0dev (C++ port)"

// 1: chat uses the CS style colour bytes (\x01 \x03 \x04). 0: colour codes are stripped.
// only turn this on if your client's SayText handler understands them.
#define JS_CHAT_COLORS 0

// 1: pev->fuser2 holds a stamina value that should be displayed. 0: never print stamina.
#define JS_USE_STAMINA 0

// 1: jumps that reach tier 5 are broadcast to everyone in chat (like the plugin does)
#define JS_BROADCAST_TIER5 1

// 1: print why jumps get discarded to the player's console
#define JS_DEBUG 0

#if JS_DEBUG
#define JS_DEBUG_PRINT(client, ...) PrintConsole((client), __VA_ARGS__)
#else
#define JS_DEBUG_PRINT(client, ...) ((void)0)
#endif

// ------------------------------------------------------------------------------------------------
// constants
// ------------------------------------------------------------------------------------------------
static const int GC_MAX_PLAYERS = 33;
static const int MAX_STRAFES = 20;
static const float MAX_EDGE = 32.0f;
static const int MAX_JUMP_FRAMES = 200; // for frame based arrays
static const int MAX_BHOP_FRAMES = 8;
static const int MAX_GSTRAFE_FRAMES = 8;
static const int HUD_GRAPH_MAX_CHARS = 128;
static const float GC_DUCK_HEIGHT_CHANGE = 18.0f;
// roughly the max speed you can go up a ladder is 400u/s aka 4 units per frame at 100fps.
static const float GC_OFFSET_TOLERANCE_LDJ = 4.0f;
static const float GC_OFFSET_TOLERANCE_BJ = 1.5f;

static const char* const CHAT_PREFIX = "!w[!gGC!w]";

static const char* const GC_REPLAY_DIR = "gcljs_replays"; // relative to the game directory
static const char* const GC_REPLAY_EXT = "gcljrp";
static const char* const GC_REPLAY_IDENT = "GCLJ";
static const int GC_REPLAY_MAGIC = 0x6c115;
static const int GC_REPLAY_VERSION = 1;

static const float GC_FLOAT_INFINITY = std::numeric_limits<float>::infinity();
static const float GC_FLOAT_NEGATIVE_INFINITY = -std::numeric_limits<float>::infinity();

static const char* const BEAM_SPRITE = "sprites/zbeam2.spr";

// ------------------------------------------------------------------------------------------------
// types
// ------------------------------------------------------------------------------------------------
struct Vec3
{
	float x, y, z;
	Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
	Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
	float& operator[](int i) { return (&x)[i]; }
	float operator[](int i) const { return (&x)[i]; }
	Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
	Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
	Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

static inline Vec3 ToVec3(const Vector& v) { return Vec3(v.x, v.y, v.z); }
static inline Vec3 ToVec3(const float* v) { return Vec3(v[0], v[1], v[2]); }
static inline Vector ToVector(const Vec3& v) { return Vector(v.x, v.y, v.z); }

enum JumpType
{
	// unprintable jumptypes. only for tracking
	JUMPTYPE_NONE,
	JUMPTYPE_DD, // double duck with FOG > 8
	JUMPTYPE_GS, // groundstrafe: double duck with FOG <= 8

	// printable jumptypes
	JUMPTYPE_LJ,
	JUMPTYPE_CJ,
	JUMPTYPE_DCJ,
	JUMPTYPE_MCJ,
	JUMPTYPE_SCJ,
	JUMPTYPE_WJ,
	JUMPTYPE_LDJ,
	JUMPTYPE_BH,
	JUMPTYPE_DBH,
	JUMPTYPE_SBJ,
	JUMPTYPE_SLJ,
	JUMPTYPE_COUNT
};

enum JumpTier
{
	JUMPTIER_0,
	JUMPTIER_1,
	JUMPTIER_2,
	JUMPTIER_3,
	JUMPTIER_4,
	JUMPTIER_5,
	JUMPTIER_MAX_DIST,
	JUMPTIER_COUNT
};

enum JumpDir
{
	JUMPDIR_FORWARDS,
	JUMPDIR_BACKWARDS,
	JUMPDIR_LEFT,
	JUMPDIR_RIGHT,
	JUMPDIR_COUNT
};

enum StrafeType
{
	STRAFETYPE_OVERLAP,
	STRAFETYPE_NONE,
	STRAFETYPE_LEFT,
	STRAFETYPE_OVERLAP_LEFT,
	STRAFETYPE_NONE_LEFT,
	STRAFETYPE_RIGHT,
	STRAFETYPE_OVERLAP_RIGHT,
	STRAFETYPE_NONE_RIGHT,
	STRAFETYPE_COUNT
};

enum JumpBeamColour
{
	JUMPBEAM_NEUTRAL,
	JUMPBEAM_LOSS,
	JUMPBEAM_GAIN,
	JUMPBEAM_DUCK
};

enum OptionType
{
	OPT_ENABLE_PLUGIN,
	OPT_SHOW_JUMP_BEAM,
	OPT_SHOW_VEER_BEAM,
	OPT_CLEAR_HUD_BEAM_ON_TP,
	OPT_COUNT
};

struct OptionDef
{
	const char* name; // cvar name (also the per player option name)
	const char* defaultValue;
	const char* description;
	float min;
	float max;
	bool isFloat;
};

// HUD visibility / position / layout live in the client HUD editor now (hud_edit), not here.
static const OptionDef g_optionDefs[OPT_COUNT] = {
	{"gcljs_enable", "1", "Enable plugin", 0, 1, false},
	{"gcljs_show_jump_beam", "0", "Show jump beam", 0, 1, false},
	{"gcljs_show_veer_beam", "0", "Show veer beam", 0, 1, false},
	{"gcljs_clear_hud_and_beam_on_tp", "0", "Clear HUD & beam on tp", 0, 1, false},
};

struct JsFrame
{
	Vec3 origin; // feet position
	Vec3 velocity;
	Vec3 angles;   // v_angle
	Vec3 wishmove; // forwardmove, sidemove, upmove of the usercmd
	float stamina;
	int buttons;
	int flags;
	int movetype;
	JsFrame() : stamina(0.0f), buttons(0), flags(0), movetype(0) {}
};
static_assert(sizeof(JsFrame) == 64, "JsFrame is written to replay files as 16 cells");

struct JsPlayerData
{
	int usercmdCount;
	int timeMsec;
	int frametimeMsec;

	int framesOnGround;
	int framesInAir;
	Vec3 angleSpeed;
	Vec3 lastAngleSpeed;
	Vec3 lastGroundPos; // last position where the player left the ground.
	bool lastGroundPosWalkedOff;
	bool landedDucked;

	bool speedShowPrespeed;
	float prespeedStamina;

	Vec3 jumpGround;
	Vec3 jumpPos;
	Vec3 jumpAngles;
	Vec3 jumpVelocity;
	Vec3 landPos;

	int fwdReleaseFrame;
	int jumpFrame;
	bool trackingJump;
	bool failedJump;

	int ladderEntity;
	Vec3 ladderMins;
	Vec3 ladderMaxs;

	// jump data
	JumpType jumpType;
	JumpType lastJumpType;
	JumpDir jumpDir;
	float jumpDistance;
	float jumpXJDistance;
	float jumpPrespeed;
	float jumpMaxspeed;
	float jumpLoss;
	float jumpVeer;
	float jumpAirpath;
	float jumpSync;
	float jumpPotency;
	float jumpEdge;
	float jumpEdges[8]; // four 2d vectors
	float jumpLandEdge;
	float jumpBlockDist;
	float jumpHeight;
	float jumpJumpoffAngle;
	float jumpAirtimeFrac;
	int jumpStartMsec;
	int jumpAirtime;
	int jumpFwdRelease;
	int jumpOverlap;
	int jumpDeadair;

	// strafes
	int strafeCount;
	float strafeSync[MAX_STRAFES];
	float strafeGain[MAX_STRAFES];
	float strafeLoss[MAX_STRAFES];
	float strafeMax[MAX_STRAFES];
	int strafeFrame[MAX_STRAFES];
	int strafeAirtime[MAX_STRAFES];
	int strafeOverlap[MAX_STRAFES];
	int strafeDeadair[MAX_STRAFES];
	float strafeAvgGain[MAX_STRAFES];
	float strafeAvgEfficiency[MAX_STRAFES];
	float strafePeakEfficiency[MAX_STRAFES];

	StrafeType strafeGraph[MAX_JUMP_FRAMES];
	float mouseGraph[MAX_JUMP_FRAMES];
	int duckGraph[MAX_JUMP_FRAMES];
};

struct JsHudBeamData
{
	int frames;
	float beamX[MAX_JUMP_FRAMES];
	float beamY[MAX_JUMP_FRAMES];
	JumpBeamColour beamColour[MAX_JUMP_FRAMES];
	Vec3 veerStart;
	Vec3 veerEnd;
	int timestampMsec;
};

struct JsCvar
{
	cvar_t cv;
	char name[56];
	char value[24];
};

// ------------------------------------------------------------------------------------------------
// globals
// ------------------------------------------------------------------------------------------------
static const char* const g_szJumpTypes[JUMPTYPE_COUNT] = {
	"NONE", "DD", "GS", "LJ", "CJ", "DCJ", "MCJ", "SCJ", "WJ", "LDJ", "BH", "DBH", "SBJ", "SLJ"};

static const char g_szStrafeTypeChar[STRAFETYPE_COUNT] = {
	'0', // STRAFETYPE_OVERLAP
	'~', // STRAFETYPE_NONE
	'<', // STRAFETYPE_LEFT
	'1', // STRAFETYPE_OVERLAP_LEFT
	'2', // STRAFETYPE_NONE_LEFT
	'>', // STRAFETYPE_RIGHT
	'1', // STRAFETYPE_OVERLAP_RIGHT
	'2', // STRAFETYPE_NONE_RIGHT
};

static const bool g_jumpTypePrintable[JUMPTYPE_COUNT] = {
	false, false, false, // NONE, DD, GS
	true, true, true, true, true, true, true, true, true, true, true};

static const char* const g_jumpDirString[JUMPDIR_COUNT] = {"Forwards", "Backwards", "Sideways", "Sideways"};
static const int g_jumpDirForwardButton[JUMPDIR_COUNT] = {IN_FORWARD, IN_BACK, IN_MOVELEFT, IN_MOVERIGHT};
static const int g_jumpDirLeftButton[JUMPDIR_COUNT] = {IN_MOVELEFT, IN_MOVERIGHT, IN_BACK, IN_FORWARD};
static const int g_jumpDirRightButton[JUMPDIR_COUNT] = {IN_MOVERIGHT, IN_MOVELEFT, IN_FORWARD, IN_BACK};

static JsPlayerData g_pd[GC_MAX_PLAYERS];
static JsHudBeamData g_hbd[GC_MAX_PLAYERS];
static float g_options[GC_MAX_PLAYERS][OPT_COUNT];

// circular buffer of frames.
static JsFrame g_replay[GC_MAX_PLAYERS][MAX_JUMP_FRAMES];
static int g_replayIndex[GC_MAX_PLAYERS];
static int g_replayCount[GC_MAX_PLAYERS];

static int g_beamSprite = 0;
static bool g_initialised = false;

static JsCvar g_optionCvars[OPT_COUNT];
static JsCvar g_tierCvars[JUMPTYPE_COUNT][JUMPTIER_COUNT];
static bool g_tierCvarValid[JUMPTYPE_COUNT];
static JsCvar g_cvarSaveReplays;

// pm_shared state (Orpheu hooks in the plugin)
static playermove_s* g_pmove = NULL;
static int g_preJumpGround = -1;

struct TierDefaults
{
	JumpType type;
	const char* abbr;
	float d[JUMPTIER_COUNT];
};

// tier 0..5 are min distances, the last one is the max distance. MCJ shares the DCJ cvars.
static const TierDefaults g_tierDefaults[] = {
	{JUMPTYPE_LJ, "lj", {210, 245, 250, 253, 255, 257, 270}},
	{JUMPTYPE_CJ, "cj", {220, 253, 257, 263, 267, 270, 280}},
	{JUMPTYPE_DCJ, "dcj", {220, 255, 265, 268, 272, 275, 285}},
	{JUMPTYPE_SCJ, "scj", {220, 253, 257, 263, 267, 270, 280}},
	{JUMPTYPE_WJ, "wj", {220, 255, 265, 268, 272, 275, 285}},
	{JUMPTYPE_LDJ, "ldj", {110, 160, 170, 175, 185, 190, 220}},
	{JUMPTYPE_BH, "bh", {200, 230, 235, 240, 243, 246, 260}},
	{JUMPTYPE_DBH, "dbh", {150, 210, 215, 220, 225, 228, 260}}, // NOTE: idk what good distance for duckbhop are
	{JUMPTYPE_SBJ, "sbj", {200, 230, 235, 240, 245, 248, 260}},
	{JUMPTYPE_SLJ, "slj", {210, 247, 252, 255, 257, 259, 280}},
};

// ------------------------------------------------------------------------------------------------
// small helpers
// ------------------------------------------------------------------------------------------------
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline float fmaxf_(float a, float b) { return a > b ? a : b; }
static inline float fminf_(float a, float b) { return a < b ? a : b; }
static inline int iround(float f) { return (int)floorf(f + 0.5f); }

static int js_stricmp(const char* a, const char* b)
{
	for (; *a && *b; a++, b++)
	{
		const int ca = tolower((unsigned char)*a);
		const int cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca - cb;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static inline float LengthXY(const Vec3& v) { return sqrtf(v.x * v.x + v.y * v.y); }
static inline float DistanceXY(const Vec3& a, const Vec3& b) { return LengthXY(a - b); }
static inline int FSign(float f) { return f > 0.0f ? 1 : (f < 0.0f ? -1 : 0); }
static inline bool IsFloatInRange(float n, float mn, float mx) { return n >= mn && n <= mx; }

static inline float NormaliseYaw(float yaw)
{
	if (yaw <= -180.0f)
		yaw += 360.0f;
	if (yaw > 180.0f)
		yaw -= 360.0f;
	return yaw;
}

static inline bool IsOverlapping(int buttons, JumpDir dir)
{
	if (dir == JUMPDIR_FORWARDS || dir == JUMPDIR_BACKWARDS)
		return (buttons & IN_MOVERIGHT) && (buttons & IN_MOVELEFT);
	return (buttons & IN_FORWARD) && (buttons & IN_BACK);
}

static inline bool IsDeadAirtime(int buttons, JumpDir dir)
{
	if (dir == JUMPDIR_FORWARDS || dir == JUMPDIR_BACKWARDS)
		return !(buttons & IN_MOVERIGHT) && !(buttons & IN_MOVELEFT);
	return !(buttons & IN_FORWARD) && !(buttons & IN_BACK);
}

static bool IsNormalAxial(const Vec3& n)
{
	int mx = fabsf(n.x) > fabsf(n.y) ? 0 : 1;
	mx = fabsf(n[mx]) > fabsf(n.z) ? mx : 2;
	return fabsf(n[(mx + 1) % 3]) <= 0.00001f && fabsf(n[(mx + 2) % 3]) <= 0.00001f;
}

// traces a hull between 2 positions. true if it hit an axis aligned plane.
static bool TraceBlock(const Vec3& pos1, const Vec3& pos2, Vec3& result)
{
	TraceResult tr;
	UTIL_TraceHull(ToVector(pos1), ToVector(pos2), ignore_monsters, head_hull, NULL, &tr);
	result = ToVec3(tr.vecEndPos);
	return tr.flFraction != 1.0f && tr.flFraction != 0.0f && IsNormalAxial(ToVec3(tr.vecPlaneNormal));
}

static float CalculateGain(float wishdirAng, float wishspeed, float accel, float initialSpeed)
{
	const float wishdirX = cosf(wishdirAng);
	const float wishdirY = sinf(wishdirAng);

	float wishspd = wishspeed;
	if (wishspd > 30.0f)
		wishspd = 30.0f;

	float velX = initialSpeed;
	float velY = 0.0f;

	const float currentspeed = velX * wishdirX + velY * wishdirY;
	const float addspeed = wishspd - currentspeed;
	if (addspeed <= 0.0f)
		return 0.0f;

	float accelspeed = accel * wishspeed * gpGlobals->frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	velX += accelspeed * wishdirX;
	velY += accelspeed * wishdirY;
	return sqrtf(velX * velX + velY * velY) - initialSpeed;
}

// returns distance fraction "travelled"
static float GetRealLandingOrigin(float landGroundZ, const Vec3& origin, const Vec3& velocity, Vec3& result)
{
	if ((origin.z - landGroundZ) == 0.0f)
	{
		result = origin;
		return 0.0f;
	}

	// this is like this because it works
	const float frametime = gpGlobals->frametime;
	const float verticalDistance = origin.z - (origin.z + velocity.z * frametime);
	const float fraction = (origin.z - landGroundZ) / verticalDistance;

	result = origin + velocity * (frametime * fraction);
	return fraction;
}

static inline float GetPlayerGravity(CBasePlayer* p)
{
	// the engine treats 0 as 1.0
	return p->pev->gravity == 0.0f ? 1.0f : p->pev->gravity;
}

// normal ground speed of the player, used for strafe efficiency / potency
static float GetPlayerMaxspeed(CBasePlayer* p)
{
	float ms = p->pev->maxspeed;
	if (ms <= 0.0f)
		ms = CVAR_GET_FLOAT("sv_maxspeed");
	return ms;
}

static void Append(char* buf, size_t size, const char* fmt, ...)
{
	const size_t len = strlen(buf);
	if (len + 1 >= size)
		return;
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf + len, size - len, fmt, args);
	va_end(args);
	buf[size - 1] = 0;
}

// ------------------------------------------------------------------------------------------------
// cvars and options
// ------------------------------------------------------------------------------------------------
static void RegisterJsCvar(JsCvar& c, const char* name, const char* value)
{
	memset(&c, 0, sizeof(c));
	snprintf(c.name, sizeof(c.name), "%s", name);
	snprintf(c.value, sizeof(c.value), "%s", value);
	c.cv.name = c.name;
	c.cv.string = c.value;
	c.cv.flags = 0;
	c.cv.value = 0.0f;
	c.cv.next = NULL;
	CVAR_REGISTER(&c.cv);
}

static float GetTierDist(JumpType type, JumpTier tier)
{
	if (type < 0 || type >= JUMPTYPE_COUNT || !g_tierCvarValid[type])
		return 0.0f;
	return CVAR_GET_FLOAT(g_tierCvars[type][tier].name);
}

static float GetDefaultOption(int type)
{
	return CVAR_GET_FLOAT(g_optionDefs[type].name);
}

static inline float GetOptionFloat(int client, int type) { return g_options[client][type]; }
static inline int GetOptionInt(int client, int type) { return (int)g_options[client][type]; }

static void LoadDefaultOptions(int client)
{
	for (int i = 0; i < OPT_COUNT; i++)
		g_options[client][i] = GetDefaultOption(i);
}

void JumpStats_Init()
{
	if (g_initialised)
		return;
	g_initialised = true;

	for (int i = 0; i < OPT_COUNT; i++)
		RegisterJsCvar(g_optionCvars[i], g_optionDefs[i].name, g_optionDefs[i].defaultValue);

	for (size_t i = 0; i < sizeof(g_tierDefaults) / sizeof(g_tierDefaults[0]); i++)
	{
		const TierDefaults& t = g_tierDefaults[i];
		g_tierCvarValid[t.type] = true;
		for (int tier = 0; tier < JUMPTIER_COUNT; tier++)
		{
			char name[56];
			char value[24];
			if (tier == JUMPTIER_MAX_DIST)
				snprintf(name, sizeof(name), "gcljs_%s_max_dist", t.abbr);
			else
				snprintf(name, sizeof(name), "gcljs_%s_min_dist_tier%i", t.abbr, tier);
			snprintf(value, sizeof(value), "%.1f", t.d[tier]);
			RegisterJsCvar(g_tierCvars[t.type][tier], name, value);
		}
	}

	// MCJ uses the same distances as DCJ
	g_tierCvarValid[JUMPTYPE_MCJ] = true;
	for (int tier = 0; tier < JUMPTIER_COUNT; tier++)
		g_tierCvars[JUMPTYPE_MCJ][tier] = g_tierCvars[JUMPTYPE_DCJ][tier];

	RegisterJsCvar(g_cvarSaveReplays, "gcljs_save_replays", "0"); // TODO of the original: this is only temporary!

	SERVER_COMMAND("exec gcljs.cfg\n");
}

void JumpStats_Precache()
{
	JumpStats_Init();
	g_beamSprite = PRECACHE_MODEL((char*)BEAM_SPRITE);
}

// ------------------------------------------------------------------------------------------------
// replay ring buffer
// ------------------------------------------------------------------------------------------------
static int ReplayIndex(int client, int relativeIndex = 0)
{
	const int rewind = imin(imin(relativeIndex, g_replayCount[client]), MAX_JUMP_FRAMES - 1);
	int index = g_replayIndex[client] - rewind;
	if (index < 0)
		index += MAX_JUMP_FRAMES;
	return index;
}

static JsFrame GetReplayFrame(int client, int relativeIndex = 0)
{
	return g_replay[client][ReplayIndex(client, relativeIndex)];
}

// ------------------------------------------------------------------------------------------------
// output
// ------------------------------------------------------------------------------------------------
// cromchat style "!x" colour codes -> CS colour bytes, or stripped
static void ConvertColours(const char* src, char* dst, size_t size, bool keepColours)
{
	size_t o = 0;
	for (const char* s = src; *s && o + 2 < size; s++)
	{
		if (*s == '!' && s[1] && strchr("rbwgtnp", s[1]))
		{
			if (keepColours)
			{
				switch (s[1])
				{
				case 'g': dst[o++] = '\x04'; break;
				case 't':
				case 'r':
				case 'b': dst[o++] = '\x03'; break;
				case 'w':
				case 'n': dst[o++] = '\x01'; break;
				default: break;
				}
			}
			s++;
			continue;
		}
		dst[o++] = *s;
	}
	dst[o] = 0;
}

static void PrintConsole(CBasePlayer* p, const char* fmt, ...)
{
	char text[1536];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	// send line by line, the engine doesn't like very long messages
	const char* s = text;
	while (*s)
	{
		char chunk[200];
		size_t n = 0;
		while (s[n] && n < sizeof(chunk) - 2)
		{
			n++;
			if (s[n - 1] == '\n')
				break;
		}
		memcpy(chunk, s, n);
		chunk[n] = 0;
		CLIENT_PRINTF(p->edict(), print_console, chunk);
		s += n;
	}
}

static void PrintChat(CBasePlayer* p, const char* text)
{
	char buf[192];
	ConvertColours(text, buf, sizeof(buf), JS_CHAT_COLORS != 0);
	UTIL_SayText(buf, p);
}

// calls fn(target) for the player and for every dead player spectating him
template <typename F>
static void ForPlayerAndSpectators(CBasePlayer* p, F fn)
{
	fn(p);

	const int self = p->entindex();
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pSpec = (CBasePlayer*)UTIL_PlayerByIndex(i);
		if (!pSpec || pSpec == p || !pSpec->IsPlayer() || pSpec->IsAlive())
			continue;
		if (pSpec->pev->flags & FL_FAKECLIENT)
			continue;
		if (pSpec->pev->iuser2 != self)
			continue;
		fn(pSpec);
	}
}

static void ClientAndSpecsPrintChat(CBasePlayer* p, const char* text)
{
	ForPlayerAndSpectators(p, [&](CBasePlayer* t)
		{ PrintChat(t, text); });
}

static void ClientAndSpecsPrintConsole(CBasePlayer* p, const char* fmt, ...)
{
	char text[1536];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	ForPlayerAndSpectators(p, [&](CBasePlayer* t)
		{ PrintConsole(t, "%s", text); });
}

// TE_BEAMPOINTS. Sent unreliable: the plugin sends up to ~100 of these every 0.2 s
// and reliable messages could overflow the client's reliable channel.
static void SendBeamPoints(CBasePlayer* p, const Vec3& a, const Vec3& b, int r, int g, int bl, int brightness, int life)
{
	MESSAGE_BEGIN(MSG_ONE_UNRELIABLE, SVC_TEMPENTITY, NULL, p->edict());
	WRITE_BYTE(TE_BEAMPOINTS);
	WRITE_COORD(a.x);
	WRITE_COORD(a.y);
	WRITE_COORD(a.z);
	WRITE_COORD(b.x);
	WRITE_COORD(b.y);
	WRITE_COORD(b.z);
	WRITE_SHORT(g_beamSprite);
	WRITE_BYTE(0);	  // starting frame
	WRITE_BYTE(0);	  // frame rate in 0.1's
	WRITE_BYTE(life); // life in 0.1's
	WRITE_BYTE(10);	  // line width in 0.1's
	WRITE_BYTE(0);	  // noise amplitude in 0.01's
	WRITE_BYTE(r);
	WRITE_BYTE(g);
	WRITE_BYTE(bl);
	WRITE_BYTE(brightness);
	WRITE_BYTE(0); // scroll speed in 0.1's
	MESSAGE_END();
}

// ------------------------------------------------------------------------------------------------
// client HUD message "JumpStat". The client (cl_dll/hud_jumpstats.cpp) formats and draws everything, so the
// layout is handled by your HUD editor. Messages are variable length (max 192 bytes), so big things are chunked.
//   INFO     byte 0, byte type, byte flags, byte strafes, byte dir, 7x long (distances), 2x long (speeds),
//            6x short (sync, loss, potency, jumpoff, height, airtime frac), 4x short (airtime, OL, DA, fwd)
//   STRAFES  byte 1, byte start, byte count, byte total, then per strafe 4x short, 3x byte, 2x short
//   GRAPH    byte 2, byte start, byte count, byte total, then one byte per frame: type | mouse << 3 | duck << 5
//   PRESPEED byte 3, byte show, long prespeed * 100
//   CLEAR    byte 4
// ------------------------------------------------------------------------------------------------
enum JsMsgType
{
	JSMSG_INFO = 0,
	JSMSG_STRAFES = 1,
	JSMSG_GRAPH = 2,
	JSMSG_PRESPEED = 3,
	JSMSG_CLEAR = 4
};

enum JsInfoFlags
{
	JSINFO_FAILED = 1,
	JSINFO_BLOCK = 2,
	JSINFO_EDGE = 4,
	JSINFO_LANDEDGE = 8
};

static int gmsgJumpStat = 0;

void JumpStats_LinkUserMessages()
{
	gmsgJumpStat = REG_USER_MSG("JumpStat", -1);
}

static int ToFixed(float v, float scale)
{
	const float f = v * scale;
	if (f > 2147483000.0f)
		return 2147483000;
	if (f < -2147483000.0f)
		return -2147483000;
	return (int)floorf(f + 0.5f);
}

static int ToShort(float v, float scale)
{
	const float f = floorf(v * scale + 0.5f);
	if (f > 32767.0f)
		return 32767;
	if (f < -32768.0f)
		return -32768;
	return (int)f;
}

static inline int ToByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void SendJumpStatInfo(CBasePlayer* p, const JsPlayerData& pd, int flags)
{
	if (!gmsgJumpStat)
		return;

	ForPlayerAndSpectators(p, [&](CBasePlayer* t)
		{
		MESSAGE_BEGIN(MSG_ONE, gmsgJumpStat, NULL, t->edict());
		WRITE_BYTE(JSMSG_INFO);
		WRITE_BYTE((int)pd.jumpType);
		WRITE_BYTE(flags);
		WRITE_BYTE(ToByte(pd.strafeCount + 1));
		WRITE_BYTE((int)pd.jumpDir);
		WRITE_LONG(ToFixed(pd.jumpDistance, 10000.0f));
		WRITE_LONG(ToFixed(pd.jumpXJDistance, 10000.0f));
		WRITE_LONG(ToFixed(pd.jumpBlockDist, 1000.0f));
		WRITE_LONG(ToFixed(pd.jumpEdge, 10000.0f));
		WRITE_LONG(ToFixed(pd.jumpLandEdge, 10000.0f));
		WRITE_LONG(ToFixed(pd.jumpVeer, 10000.0f));
		WRITE_LONG(ToFixed(pd.jumpAirpath, 10000.0f));
		WRITE_LONG(ToFixed(pd.jumpPrespeed, 100.0f));
		WRITE_LONG(ToFixed(pd.jumpMaxspeed, 100.0f));
		WRITE_SHORT(ToShort(pd.jumpSync, 100.0f));
		WRITE_SHORT(ToShort(pd.jumpLoss, 100.0f));
		WRITE_SHORT(ToShort(pd.jumpPotency, 100.0f));
		WRITE_SHORT(ToShort(pd.jumpJumpoffAngle, 100.0f));
		WRITE_SHORT(ToShort(pd.jumpHeight, 100.0f));
		WRITE_SHORT(ToShort(pd.jumpAirtimeFrac, 100.0f));
		WRITE_SHORT(pd.jumpAirtime);
		WRITE_SHORT(pd.jumpOverlap);
		WRITE_SHORT(pd.jumpDeadair);
		WRITE_SHORT(pd.jumpFwdRelease);
		MESSAGE_END(); });
}

static void SendJumpStatStrafes(CBasePlayer* p, const JsPlayerData& pd)
{
	if (!gmsgJumpStat)
		return;

	const int total = imin(pd.strafeCount + 1, MAX_STRAFES);
	const int perMsg = 10;
	for (int start = 0; start < total; start += perMsg)
	{
		const int count = imin(perMsg, total - start);
		ForPlayerAndSpectators(p, [&](CBasePlayer* t)
			{
			MESSAGE_BEGIN(MSG_ONE, gmsgJumpStat, NULL, t->edict());
			WRITE_BYTE(JSMSG_STRAFES);
			WRITE_BYTE(start);
			WRITE_BYTE(count);
			WRITE_BYTE(total);
			for (int i = start; i < start + count; i++)
			{
				const float peak = pd.strafePeakEfficiency[i] == GC_FLOAT_NEGATIVE_INFINITY ? 0.0f : pd.strafePeakEfficiency[i];
				WRITE_SHORT(ToShort(pd.strafeSync[i], 10.0f));
				WRITE_SHORT(ToShort(pd.strafeGain[i], 10.0f));
				WRITE_SHORT(ToShort(pd.strafeLoss[i], 10.0f));
				WRITE_SHORT(ToShort(pd.strafeMax[i], 10.0f));
				WRITE_BYTE(ToByte(pd.strafeAirtime[i]));
				WRITE_BYTE(ToByte(pd.strafeOverlap[i]));
				WRITE_BYTE(ToByte(pd.strafeDeadair[i]));
				WRITE_SHORT(ToShort(pd.strafeAvgEfficiency[i], 1.0f));
				WRITE_SHORT(ToShort(peak, 1.0f));
			}
			MESSAGE_END(); });
	}
}

static void SendJumpStatGraph(CBasePlayer* p, const JsPlayerData& pd)
{
	if (!gmsgJumpStat)
		return;

	const int total = imin(pd.jumpAirtime, MAX_JUMP_FRAMES);
	const int perMsg = 120;
	for (int start = 0; start < total; start += perMsg)
	{
		const int count = imin(perMsg, total - start);
		ForPlayerAndSpectators(p, [&](CBasePlayer* t)
			{
			MESSAGE_BEGIN(MSG_ONE, gmsgJumpStat, NULL, t->edict());
			WRITE_BYTE(JSMSG_GRAPH);
			WRITE_BYTE(start);
			WRITE_BYTE(count);
			WRITE_BYTE(total);
			for (int i = start; i < start + count; i++)
			{
				// mouse: 0 = none, 1 = yaw increased (turning left), 2 = yaw decreased (turning right)
				const int mouse = pd.mouseGraph[i] == 0.0f ? 0 : (pd.mouseGraph[i] > 0.0f ? 1 : 2);
				const int duck = pd.duckGraph[i] ? 1 : 0;
				WRITE_BYTE((int)pd.strafeGraph[i] | (mouse << 3) | (duck << 5));
			}
			MESSAGE_END(); });
	}
}

static void SendJumpStatPrespeed(CBasePlayer* p, bool show, float prespeed)
{
	if (!gmsgJumpStat || !p)
		return;

	ForPlayerAndSpectators(p, [&](CBasePlayer* t)
		{
		MESSAGE_BEGIN(MSG_ONE, gmsgJumpStat, NULL, t->edict());
		WRITE_BYTE(JSMSG_PRESPEED);
		WRITE_BYTE(show ? 1 : 0);
		WRITE_LONG(ToFixed(prespeed, 100.0f));
		MESSAGE_END(); });
}

static void SendJumpStatClear(CBasePlayer* p)
{
	if (!gmsgJumpStat || !p)
		return;

	ForPlayerAndSpectators(p, [&](CBasePlayer* t)
		{
		MESSAGE_BEGIN(MSG_ONE, gmsgJumpStat, NULL, t->edict());
		WRITE_BYTE(JSMSG_CLEAR);
		MESSAGE_END(); });
}

// the player data structs live in one array, so the index is the entity index
static inline CBasePlayer* PlayerOf(const JsPlayerData& pd)
{
	return (CBasePlayer*)UTIL_PlayerByIndex((int)(&pd - g_pd));
}

// ------------------------------------------------------------------------------------------------
// jump data
// ------------------------------------------------------------------------------------------------
static void PDResetJumpData(JsPlayerData& pd)
{
	// NOTE: only resets things that need to be reset
	pd.jumpPos = Vec3();
	pd.landPos = Vec3();
	pd.trackingJump = false;
	pd.failedJump = false;

	// NOTE: don't reset jumpType or lastJumpType
	pd.jumpMaxspeed = 0.0f;
	pd.jumpLoss = 0.0f;
	pd.jumpSync = 0.0f;
	pd.jumpPotency = 0.0f;
	pd.jumpEdge = 0.0f;
	pd.jumpBlockDist = 0.0f;
	pd.jumpHeight = 0.0f;
	pd.jumpAirtime = 0;
	pd.jumpOverlap = 0;
	pd.jumpDeadair = 0;
	pd.jumpAirpath = 0.0f;

	pd.strafeCount = 0;
	for (int i = 0; i < MAX_STRAFES; i++)
	{
		pd.strafeSync[i] = 0.0f;
		pd.strafeGain[i] = 0.0f;
		pd.strafeLoss[i] = 0.0f;
		pd.strafeMax[i] = 0.0f;
		pd.strafeFrame[i] = 0;
		pd.strafeAirtime[i] = 0;
		pd.strafeOverlap[i] = 0;
		pd.strafeDeadair[i] = 0;
		pd.strafeAvgGain[i] = 0.0f;
		pd.strafeAvgEfficiency[i] = 0.0f;
		pd.strafePeakEfficiency[i] = GC_FLOAT_NEGATIVE_INFINITY;
	}
}

static bool IsWishspeedMovingLeft(float forwardspeed, float sidespeed, JumpDir dir)
{
	if (dir == JUMPDIR_FORWARDS)
		return sidespeed < 0.0f;
	if (dir == JUMPDIR_BACKWARDS)
		return sidespeed > 0.0f;
	if (dir == JUMPDIR_LEFT)
		return forwardspeed < 0.0f;
	return forwardspeed > 0.0f; // JUMPDIR_RIGHT
}

static bool IsWishspeedMovingRight(float forwardspeed, float sidespeed, JumpDir dir)
{
	if (dir == JUMPDIR_FORWARDS)
		return sidespeed > 0.0f;
	if (dir == JUMPDIR_BACKWARDS)
		return sidespeed < 0.0f;
	if (dir == JUMPDIR_LEFT)
		return forwardspeed > 0.0f;
	return forwardspeed < 0.0f; // JUMPDIR_RIGHT
}

static bool IsNewStrafe(const JsPlayerData& pd)
{
	const int lastSpeed = FSign(pd.lastAngleSpeed.y);
	const int speed = FSign(pd.angleSpeed.y);
	return ((speed > 0 && lastSpeed <= 0) || (speed < 0 && lastSpeed >= 0)) && pd.jumpAirtime != 1;
}

static void OnPlayerJumped(JsPlayerData& pd, JumpType jumpType, const JsFrame& fd, const JsFrame& lastFd)
{
	pd.lastJumpType = pd.jumpType;
	PDResetJumpData(pd);
	pd.jumpType = jumpType;

	pd.trackingJump = g_jumpTypePrintable[jumpType];

	pd.prespeedStamina = fd.stamina;

	// start from clean graphs so a short jump never shows the tail of the previous one
	memset(pd.strafeGraph, 0, sizeof(pd.strafeGraph));
	memset(pd.mouseGraph, 0, sizeof(pd.mouseGraph));
	memset(pd.duckGraph, 0, sizeof(pd.duckGraph));
	for (int i = 0; i < MAX_JUMP_FRAMES; i++)
		pd.strafeGraph[i] = STRAFETYPE_NONE;

	// jump direction
	if (LengthXY(fd.velocity) > 0.0f)
	{
		const float velDir = atan2f(fd.velocity.y, fd.velocity.x) * (180.0f / 3.14159265358979f);
		const float dir = NormaliseYaw(fd.angles.y - velDir);

		pd.jumpDir = JUMPDIR_FORWARDS;
		if (IsFloatInRange(dir, 45.0f, 135.0f))
			pd.jumpDir = JUMPDIR_RIGHT;
		if (IsFloatInRange(dir, -135.0f, -45.0f))
			pd.jumpDir = JUMPDIR_LEFT;
		else if (dir > 135.0f || dir < -135.0f)
			pd.jumpDir = JUMPDIR_BACKWARDS;
	}

	pd.jumpStartMsec = pd.timeMsec;
	if (jumpType != JUMPTYPE_LDJ)
	{
		pd.jumpFrame = pd.usercmdCount;
		pd.jumpPos = fd.origin;
		pd.jumpAngles = fd.angles;
		pd.jumpVelocity = fd.velocity;
		pd.jumpPrespeed = LengthXY(fd.velocity);
	}
	else
	{
		pd.jumpFrame = pd.usercmdCount - 1;
		pd.jumpPos = lastFd.origin;
		pd.jumpAngles = lastFd.angles;
		pd.jumpVelocity = lastFd.velocity;
		pd.jumpStartMsec -= pd.frametimeMsec * 2;
		pd.jumpPrespeed = LengthXY(lastFd.velocity);
	}

	pd.jumpGround = pd.jumpPos;

	if (jumpType == JUMPTYPE_LDJ)
	{
		// fix jump ground
		if (pd.jumpGround.z > pd.ladderMaxs.z)
			pd.jumpGround.z = pd.ladderMaxs.z;
	}

	{
		Vec3 basePos = pd.jumpGround;
		// move origin to the bottom of HULL_HEAD and 2 units down, so we can touch the side of the lj blocks
		basePos.z += GC_DUCK_HEIGHT_CHANGE - 2.0f;

		for (int i = 0; i < 8; i += 2)
		{
			// +x, +y, -x, -y
			const int blockAxis = (i / 2) % 2;
			const int blockDir = 1 - (i / 4) * 2;
			Vec3 endPos = basePos;
			Vec3 startPos = basePos;
			startPos[blockAxis] += (float)blockDir * MAX_EDGE;

			Vec3 jumpEdge;
			bool gotEdge = false;
			if (pd.jumpType == JUMPTYPE_LDJ)
			{
				endPos[blockAxis] -= (float)blockDir * 1.0f;
				gotEdge = TraceBlock(endPos, startPos, jumpEdge);
			}
			else
			{
				gotEdge = TraceBlock(startPos, endPos, jumpEdge);
			}

			if (gotEdge)
			{
				pd.jumpEdges[i] = jumpEdge.x;
				pd.jumpEdges[i + 1] = jumpEdge.y;
			}
			else
			{
				pd.jumpEdges[i] = GC_FLOAT_INFINITY;
				pd.jumpEdges[i + 1] = GC_FLOAT_INFINITY;
			}
		}
	}

	// show the prespeed under the speed until we land
	pd.speedShowPrespeed = true;
	SendJumpStatPrespeed(PlayerOf(pd), true, pd.jumpPrespeed);
}

static void FinishTrackingJump(JsPlayerData& pd, const JsFrame& lastFd)
{
	// finish up stats:
	const float xAxisVeer = fabsf(pd.landPos.x - pd.jumpPos.x);
	const float yAxisVeer = fabsf(pd.landPos.y - pd.jumpPos.y);
	pd.jumpVeer = fminf_(xAxisVeer, yAxisVeer);

	pd.jumpFwdRelease = pd.fwdReleaseFrame - pd.jumpFrame;
	pd.jumpSync = pd.jumpAirtime > 0 ? pd.jumpSync / (float)pd.jumpAirtime * 100.0f : 0.0f;
	pd.jumpPotency = pd.jumpAirtime > 0 ? pd.jumpPotency / (float)pd.jumpAirtime : 0.0f;

	for (int strafe = 0; strafe < pd.strafeCount + 1 && strafe < MAX_STRAFES; strafe++)
	{
		const float air = (float)pd.strafeAirtime[strafe];
		if (air != 0.0f)
		{
			pd.strafeAvgGain[strafe] = pd.strafeGain[strafe] / air;
			pd.strafeAvgEfficiency[strafe] /= air;
			pd.strafeSync[strafe] = pd.strafeSync[strafe] / air * 100.0f;
		}
		else
		{
			pd.strafeAvgGain[strafe] = 0.0f;
			pd.strafeAvgEfficiency[strafe] = 0.0f;
			pd.strafeSync[strafe] = 0.0f;
		}
	}

	// airpath!
	{
		const Vec3 delta = pd.landPos - lastFd.origin;
		pd.jumpAirpath += LengthXY(delta);
		const float denom = pd.jumpType != JUMPTYPE_LDJ ? pd.jumpDistance - 32.0f : pd.jumpDistance;
		pd.jumpAirpath = denom != 0.0f ? pd.jumpAirpath / denom : 0.0f;
	}

	// Calculate block distance and jumpoff edge
	{
		const int blockAxis = fabsf(pd.landPos.y - pd.jumpPos.y) > fabsf(pd.landPos.x - pd.jumpPos.x) ? 1 : 0;
		const int blockDir = FSign(pd.jumpPos[blockAxis] - pd.landPos[blockAxis]);

		Vec3 endPos = pd.landPos;
		endPos.z += GC_DUCK_HEIGHT_CHANGE - 2.0f;

		Vec3 startPos = endPos;
		startPos[blockAxis] -= (pd.landPos[blockAxis] - pd.jumpPos[blockAxis]) / 2.0f;

		// extend land origin, so if we fail within MAX_EDGE units of the block we can still get the block distance.
		endPos[blockAxis] -= (float)blockDir * MAX_EDGE;

		pd.jumpLandEdge = MAX_EDGE;
		pd.jumpEdge = -1.0f;
		pd.jumpBlockDist = -1.0f;
		Vec3 landEdge;
		if (TraceBlock(startPos, endPos, landEdge))
		{
			// 0.03125 is to make blocks accurate. the measured distance with engine trace is usually
			// integer block distance - 0.0625 (0.03125*2), for example 255-0.0625, but the player isn't
			// able to land a <255 lj on most 255 lj blocks. the real physical distance of most blocks is their
			// integer distance as measured in hammer.
			pd.jumpLandEdge = (landEdge[blockAxis] - pd.landPos[blockAxis]) * (float)blockDir;
			pd.jumpLandEdge -= 0.03125f;
		}

		const int edgeInd = ((blockDir + 1) + blockAxis) * 2 + blockAxis;
		const float jumpEdge = (edgeInd >= 0 && edgeInd < 8) ? pd.jumpEdges[edgeInd] : GC_FLOAT_INFINITY;

		if (jumpEdge != GC_FLOAT_INFINITY)
		{
			pd.jumpEdge = (pd.jumpPos[blockAxis] - jumpEdge) * (float)blockDir;

			if (pd.jumpLandEdge != MAX_EDGE)
			{
				pd.jumpBlockDist = fabsf(landEdge[blockAxis] - jumpEdge);
				if (pd.jumpType != JUMPTYPE_LDJ)
					pd.jumpBlockDist += 32.0625f;
				// round block distance to 1/2048, as that is nearly the precision limit of goldsrc's map size,
				// add 1/2048, to avoid block distances such as 251.899 being shown to the player.
				pd.jumpBlockDist = (float)iround(pd.jumpBlockDist * 2048.0f) / 2048.0f + (1.0f / 2048.0f);
			}

			if (pd.jumpType != JUMPTYPE_LDJ)
				pd.jumpEdge -= 0.03125f;
			else
				pd.jumpEdge += 0.03125f; // ladder edge faces the other way!
		}
	}

	// jumpoff angle!
	{
		Vec3 airpathDir = pd.landPos - pd.jumpPos;
		const float len = sqrtf(airpathDir.x * airpathDir.x + airpathDir.y * airpathDir.y + airpathDir.z * airpathDir.z);
		if (len > 0.0f)
			airpathDir = airpathDir * (1.0f / len);

		float airpathYaw = NormaliseYaw(atan2f(airpathDir.y, airpathDir.x) * (180.0f / 3.14159265358979f));

		// Fix bugs with -180 to 180 transitions
		if (fabsf(airpathYaw - pd.jumpAngles.y) > 180.0f)
			airpathYaw += 360.0f;

		pd.jumpJumpoffAngle = NormaliseYaw(airpathYaw - pd.jumpAngles.y);
	}
}

// ------------------------------------------------------------------------------------------------
// printing
// ------------------------------------------------------------------------------------------------
static void PrintStats(CBasePlayer* p, JsPlayerData& pd, JsHudBeamData& hbd, int client)
{
	hbd.frames = imin(pd.jumpAirtime, MAX_JUMP_FRAMES - 1);
	for (int i = 0; i < hbd.frames; i++)
	{
		const JsFrame lastFd = GetReplayFrame(client, hbd.frames - i);
		const JsFrame fd = GetReplayFrame(client, hbd.frames - 1 - i);
		hbd.beamX[i] = fd.origin.x;
		hbd.beamY[i] = fd.origin.y;
		hbd.beamColour[i] = JUMPBEAM_NEUTRAL;

		const float lastSpeed = LengthXY(lastFd.velocity);
		const float speed = LengthXY(fd.velocity);
		if (speed > lastSpeed)
			hbd.beamColour[i] = JUMPBEAM_GAIN;
		else if (speed < lastSpeed)
			hbd.beamColour[i] = JUMPBEAM_LOSS;

		if (fd.flags & FL_DUCKING)
			hbd.beamColour[i] = JUMPBEAM_DUCK;
	}

	// make sure the last frame in the jump beam has the correct position!
	if (pd.jumpAirtime < MAX_JUMP_FRAMES)
	{
		hbd.beamX[hbd.frames] = pd.landPos.x;
		hbd.beamY[hbd.frames] = pd.landPos.y;
	}

	hbd.veerStart = pd.jumpPos;
	hbd.veerEnd = pd.landPos;

	char fwdRelease[32] = "";
	if (pd.jumpFwdRelease == 0)
		snprintf(fwdRelease, sizeof(fwdRelease), "Fwd: !g0");
	else if (abs(pd.jumpFwdRelease) > 16)
		snprintf(fwdRelease, sizeof(fwdRelease), "Fwd: !nNo");
	else if (pd.jumpFwdRelease > 0)
		snprintf(fwdRelease, sizeof(fwdRelease), "Fwd: !w+%i", pd.jumpFwdRelease);
	else
		snprintf(fwdRelease, sizeof(fwdRelease), "Fwd: !w%i", pd.jumpFwdRelease);

	char edge[32] = "";
	char chatEdge[32] = "";
	bool hasEdge = false;
	if (pd.jumpEdge >= 0.0f && pd.jumpEdge < MAX_EDGE)
	{
		snprintf(edge, sizeof(edge), "Edge: %.4f", pd.jumpEdge);
		snprintf(chatEdge, sizeof(chatEdge), "Edge: !n%.2f", pd.jumpEdge);
		hasEdge = true;
	}

	char block[32] = "";
	bool hasBlock = false;
	if (IsFloatInRange(pd.jumpBlockDist, GetTierDist(pd.jumpType, JUMPTIER_0), GetTierDist(pd.jumpType, JUMPTIER_MAX_DIST)))
	{
		snprintf(block, sizeof(block), "Block: %.3f", pd.jumpBlockDist);
		hasBlock = true;
	}

	char landEdge[32] = "";
	bool hasLandEdge = false;
	if (fabsf(pd.jumpLandEdge) < MAX_EDGE)
	{
		snprintf(landEdge, sizeof(landEdge), "Land Edge: %.4f", pd.jumpLandEdge);
		hasLandEdge = true;
	}

	char stamina[32] = "";
	bool hasStamina = false;
#if JS_USE_STAMINA
	if (pd.prespeedStamina != 0.0f)
	{
		snprintf(stamina, sizeof(stamina), "Stamina: %.1f", pd.prespeedStamina);
		hasStamina = true;
	}
#endif

	// ---- chat
	char chatStats[256];
	snprintf(chatStats, sizeof(chatStats), "%s !w%s%s: !n%.4f !w[%s%sVeer: !n%.2f!w | %s!w | Sync: !n%.1f!w | Max: !n%.1f!w]",
		CHAT_PREFIX,
		pd.failedJump ? "FAILED " : "",
		g_szJumpTypes[pd.jumpType],
		pd.jumpDistance,
		chatEdge,
		hasEdge ? " !w| " : "",
		pd.jumpVeer,
		fwdRelease,
		pd.jumpSync,
		pd.jumpMaxspeed);
	ClientAndSpecsPrintChat(p, chatStats);

	// Print the highest tier jumps to everyone else
#if JS_BROADCAST_TIER5
	if (pd.jumpDistance >= GetTierDist(pd.jumpType, JUMPTIER_5))
	{
		char name[24];
		snprintf(name, sizeof(name), "%s", STRING(p->pev->netname));

		char chatStatsAll[256];
		snprintf(chatStatsAll, sizeof(chatStatsAll), "%s !g%s!w jumped !w%s%s: !n%.4f !w[%s%sVeer: !n%.2f!w | %s!w | Sync: !n%.1f!w | Max: !n%.1f!w] !n%s!w",
			CHAT_PREFIX,
			name,
			pd.failedJump ? "FAILED " : "",
			g_szJumpTypes[pd.jumpType],
			pd.jumpDistance,
			chatEdge,
			hasEdge ? " !w| " : "",
			pd.jumpVeer,
			fwdRelease,
			pd.jumpSync,
			pd.jumpMaxspeed,
			pd.failedJump ? "" : block);

		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer* pOther = (CBasePlayer*)UTIL_PlayerByIndex(i);
			if (!pOther || pOther == p || !pOther->IsPlayer() || (pOther->pev->flags & FL_FAKECLIENT))
				continue;
			PrintChat(pOther, chatStatsAll);
		}
	}
#endif

	// ---- console
	char consoleStats[1024];
	snprintf(consoleStats, sizeof(consoleStats), "\n[GC] %s%s: %.5f (XJ: %.5f) [%s%s%s%sVeer: %.4f | %s | Sync: %.2f | Max: %.3f]\n",
		pd.failedJump ? "FAILED " : "",
		g_szJumpTypes[pd.jumpType],
		pd.jumpDistance,
		pd.jumpXJDistance,
		block,
		hasBlock ? " | " : "",
		edge,
		hasEdge ? " | " : "",
		pd.jumpVeer,
		fwdRelease,
		pd.jumpSync,
		pd.jumpMaxspeed);

	Append(consoleStats, sizeof(consoleStats), "[%s%sPre: %.4f | OL/DA: %i/%i | Joff Angle: %.3f | Airpath: %.4f | Strafes: %i | Air: %i (%.2f)]\n",
		landEdge,
		hasLandEdge ? " | " : "",
		pd.jumpPrespeed,
		pd.jumpOverlap,
		pd.jumpDeadair,
		pd.jumpJumpoffAngle,
		pd.jumpAirpath,
		pd.strafeCount + 1,
		pd.jumpAirtime,
		pd.jumpAirtimeFrac);

	{
		char stripped[1024];
		ConvertColours(consoleStats, stripped, sizeof(stripped), false);
		ClientAndSpecsPrintConsole(p, "%s", stripped);
	}

	snprintf(consoleStats, sizeof(consoleStats), "[Jump Direction: %s | Height: %.4f%s%s | Loss: %.2f | Potency: %.2f]\n",
		g_jumpDirString[pd.jumpDir],
		pd.jumpHeight,
		hasStamina ? " | " : "",
		stamina,
		pd.jumpLoss,
		pd.jumpPotency);
	ClientAndSpecsPrintConsole(p, "%s", consoleStats);

	ClientAndSpecsPrintConsole(p, "\n #.  Sync   Gain   Loss  Max   Air  OL/DA AvgGain Distance  AvgEff%% (MaxEff%%)\n");
	for (int strafe = 0; strafe <= pd.strafeCount && strafe < MAX_STRAFES; strafe++)
	{
		JsFrame nextStrafeFd;
		// NOTE: intentionally get 1 frame before this strafe's start!
		const JsFrame strafeFd = GetReplayFrame(client, pd.jumpAirtime - pd.strafeFrame[strafe]);
		if (strafe < pd.strafeCount && strafe + 1 < MAX_STRAFES)
			nextStrafeFd = GetReplayFrame(client, pd.jumpAirtime - pd.strafeFrame[strafe + 1]);
		else
			nextStrafeFd = GetReplayFrame(client, 0);

		Vec3 strafeOffset = strafeFd.origin - nextStrafeFd.origin;
		strafeOffset.z = 0.0f;
		Vec3 jumpNormal = pd.jumpPos - pd.landPos;
		jumpNormal.z = 0.0f;
		const float jnLen = sqrtf(jumpNormal.x * jumpNormal.x + jumpNormal.y * jumpNormal.y);
		if (jnLen > 0.0f)
			jumpNormal = jumpNormal * (1.0f / jnLen);

		const float dot = jumpNormal.x * strafeOffset.x + jumpNormal.y * strafeOffset.y + jumpNormal.z * strafeOffset.z;
		const float strafeDistance = pd.strafeAirtime[strafe] ? dot / (float)pd.strafeAirtime[strafe] : 0.0f;

		const float peak = pd.strafePeakEfficiency[strafe] == GC_FLOAT_NEGATIVE_INFINITY ? 0.0f : pd.strafePeakEfficiency[strafe];

		ClientAndSpecsPrintConsole(p, "%2i. %5.1f%% %6.2f %6.2f  %5.1f %3i %2i/%-2i    %3.2f      %-5.2f   %3i%% (%i%%)\n",
			strafe + 1,
			pd.strafeSync[strafe],
			pd.strafeGain[strafe],
			pd.strafeLoss[strafe],
			pd.strafeMax[strafe],
			pd.strafeAirtime[strafe],
			pd.strafeOverlap[strafe],
			pd.strafeDeadair[strafe],
			pd.strafeAvgGain[strafe],
			strafeDistance,
			iround(pd.strafeAvgEfficiency[strafe]),
			iround(peak));
	}

	// ---- hud text
	char moveLeftGraph[HUD_GRAPH_MAX_CHARS] = "";
	char moveRightGraph[HUD_GRAPH_MAX_CHARS] = "";
	char mouseGraphLeft[HUD_GRAPH_MAX_CHARS] = "";
	char mouseGraphRight[HUD_GRAPH_MAX_CHARS] = "";
	char duckGraph[HUD_GRAPH_MAX_CHARS] = "";

	const int graphLen = imin(pd.jumpAirtime, imin(MAX_JUMP_FRAMES, HUD_GRAPH_MAX_CHARS - 1));
	for (int i = 0; i < graphLen; i++)
	{
		StrafeType strafeTypeLeft = pd.strafeGraph[i];
		StrafeType strafeTypeRight = pd.strafeGraph[i];

		if (strafeTypeLeft == STRAFETYPE_RIGHT || strafeTypeLeft == STRAFETYPE_NONE_RIGHT || strafeTypeLeft == STRAFETYPE_OVERLAP_RIGHT)
			strafeTypeLeft = STRAFETYPE_NONE;
		if (strafeTypeRight == STRAFETYPE_LEFT || strafeTypeRight == STRAFETYPE_NONE_LEFT || strafeTypeRight == STRAFETYPE_OVERLAP_LEFT)
			strafeTypeRight = STRAFETYPE_NONE;

		Append(moveLeftGraph, sizeof(moveLeftGraph), "%c", g_szStrafeTypeChar[strafeTypeLeft]);
		Append(moveRightGraph, sizeof(moveRightGraph), "%c", g_szStrafeTypeChar[strafeTypeRight]);

		if (pd.mouseGraph[i] == 0.0f)
		{
			Append(mouseGraphLeft, sizeof(mouseGraphLeft), "%c", g_szStrafeTypeChar[STRAFETYPE_NONE]);
			Append(mouseGraphRight, sizeof(mouseGraphRight), "%c", g_szStrafeTypeChar[STRAFETYPE_NONE]);
		}
		else if (pd.mouseGraph[i] < 0.0f)
		{
			Append(mouseGraphLeft, sizeof(mouseGraphLeft), "%c", g_szStrafeTypeChar[STRAFETYPE_NONE]);
			Append(mouseGraphRight, sizeof(mouseGraphRight), "$");
		}
		else
		{
			Append(mouseGraphLeft, sizeof(mouseGraphLeft), "$");
			Append(mouseGraphRight, sizeof(mouseGraphRight), "%c", g_szStrafeTypeChar[STRAFETYPE_NONE]);
		}

		Append(duckGraph, sizeof(duckGraph), "%c", pd.duckGraph[i] ? 'C' : g_szStrafeTypeChar[STRAFETYPE_NONE]);
	}

	// ---- client HUD
	{
		int flags = 0;
		if (pd.failedJump)
			flags |= JSINFO_FAILED;
		if (hasBlock)
			flags |= JSINFO_BLOCK;
		if (hasEdge)
			flags |= JSINFO_EDGE;
		if (hasLandEdge)
			flags |= JSINFO_LANDEDGE;
		SendJumpStatInfo(p, pd, flags);
		SendJumpStatStrafes(p, pd);
		SendJumpStatGraph(p, pd);
	}

	hbd.timestampMsec = pd.timeMsec;

	// the console graphs use '$' for pressed keys / ducking
	for (char* c = moveLeftGraph; *c; c++)
		if (*c == g_szStrafeTypeChar[STRAFETYPE_LEFT])
			*c = '$';
	for (char* c = moveRightGraph; *c; c++)
		if (*c == g_szStrafeTypeChar[STRAFETYPE_RIGHT])
			*c = '$';
	for (char* c = duckGraph; *c; c++)
		if (*c == 'C')
			*c = '$';

	ClientAndSpecsPrintConsole(p, "\nStrafe keys:\nL: %s\nR: %s\n", moveLeftGraph, moveRightGraph);
	ClientAndSpecsPrintConsole(p, "Mouse movement:\nL: %s\nR: %s\n", mouseGraphLeft, mouseGraphRight);
	ClientAndSpecsPrintConsole(p, "Duck:\nD: %s\n\n", duckGraph);
}

static void SaveReplay(CBasePlayer* p, int client, const JsPlayerData& pd)
{
	char gameDir[128];
	GET_GAME_DIR(gameDir);

	char dir[300];
	snprintf(dir, sizeof(dir), "%s/%s", gameDir, GC_REPLAY_DIR);
	JS_MKDIR(dir);

	char steamid[64];
	const char* auth = GETPLAYERAUTHID(p->edict());
	snprintf(steamid, sizeof(steamid), "%s", auth ? auth : "unknown");
	for (char* c = steamid; *c; c++)
	{
		if (*c == ':')
			*c = '_';
		else if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_'))
			*c = '-';
	}
	if (strncmp(steamid, "STEAM_0", 7) == 0)
		steamid[6] = '1';

	char outDir[400];
	snprintf(outDir, sizeof(outDir), "%s/%s", dir, steamid);
	JS_MKDIR(outDir);

	char timeStr[64];
	time_t now = time(NULL);
	strftime(timeStr, sizeof(timeStr), "%Y-%d-%m_%H.%M.%S", localtime(&now));

	char path[600];
	snprintf(path, sizeof(path), "%s/%s_%s_%s_%.3f_%.4f.%s",
		outDir, timeStr, steamid, g_szJumpTypes[pd.jumpType],
		(pd.jumpBlockDist < 400.0f && pd.jumpBlockDist > 0.0f) ? pd.jumpBlockDist : 0.0f,
		pd.jumpDistance, GC_REPLAY_EXT);

	FILE* f = fopen(path, "wb");
	if (!f)
		return;

	// same layout as the plugin: every value is a 32 bit cell
	auto wi = [&](int v)
	{ fwrite(&v, sizeof(v), 1, f); };
	auto wf = [&](float v)
	{ fwrite(&v, sizeof(v), 1, f); };
	auto wv = [&](const Vec3& v)
	{ wf(v.x); wf(v.y); wf(v.z); };

	fwrite(GC_REPLAY_IDENT, 1, strlen(GC_REPLAY_IDENT), f);
	wi(GC_REPLAY_MAGIC);
	wi(GC_REPLAY_VERSION);

	wv(pd.lastGroundPos);
	wi(pd.lastGroundPosWalkedOff);
	wi(pd.landedDucked);
	wi(-1); // FOG (removed)
	wf(pd.prespeedStamina);
	wv(pd.jumpPos);
	wv(pd.jumpAngles);
	wv(pd.jumpVelocity);
	wv(pd.landPos);
	wv(pd.landPos); // written twice by the plugin as well
	wi(pd.fwdReleaseFrame);
	wi(pd.jumpFrame);
	wi(pd.jumpType);
	wi(pd.lastJumpType);
	wi(pd.jumpDir);
	wf(pd.jumpDistance);
	wf(pd.jumpXJDistance);
	wf(pd.jumpPrespeed);
	wf(pd.jumpMaxspeed);
	wf(0.0f); // weapon speed (CS only, always 0 here)
	wf(pd.jumpLoss);
	wf(pd.jumpVeer);
	wf(pd.jumpAirpath);
	wf(pd.jumpSync);
	wf(pd.jumpPotency);
	wf(pd.jumpEdge);
	wf(pd.jumpLandEdge);
	wf(pd.jumpBlockDist);
	wf(pd.jumpHeight);
	wf(pd.jumpJumpoffAngle);
	wi(pd.jumpStartMsec);
	wi(pd.jumpAirtime);
	wi(pd.jumpFwdRelease);
	wi(pd.jumpOverlap);
	wi(pd.jumpDeadair);
	wi(-1); // weapon id (CS only)
	wi(pd.strafeCount);

	// TODO of the original: compression
	wi(g_replayCount[client]);
	for (int i = g_replayCount[client] - 1; i >= 0; i--)
	{
		const JsFrame fd = GetReplayFrame(client, i);
		fwrite(&fd, sizeof(fd), 1, f);
	}
	fclose(f);
}

// ------------------------------------------------------------------------------------------------
// jump tracking
// ------------------------------------------------------------------------------------------------
static void OnPlayerFailstat(CBasePlayer* p, int client, JsPlayerData& pd)
{
	// get frame just before "landing"
	// TODO of the original: airtime on noduck failstats is still too high (same as ducked)
	JsFrame fd;
	int frameBeforeLand = 0;
	for (; frameBeforeLand < g_replayCount[client]; frameBeforeLand++)
	{
		fd = GetReplayFrame(client, frameBeforeLand);
		if (fd.origin.z > pd.jumpGround.z)
			break;
	}
	const JsFrame lastFd = GetReplayFrame(client, frameBeforeLand + 1);

	pd.failedJump = true;

	const float gravity = CVAR_GET_FLOAT("sv_gravity") * GetPlayerGravity(p);
	const float frametime = (float)pd.frametimeMsec / 1000.0f;
	Vec3 fixedVelocity = fd.velocity;
	if (fd.flags & FL_ONGROUND)
	{
		// fix zero velocity when on ground
		fixedVelocity.z = lastFd.velocity.z - gravity * frametime;
	}
	fixedVelocity.z -= gravity * 0.5f * frametime;

	const float fraction = GetRealLandingOrigin(pd.jumpGround.z, fd.origin, fixedVelocity, pd.landPos);

	pd.jumpDistance = DistanceXY(pd.jumpPos, pd.landPos);
	pd.jumpXJDistance = fmaxf_(fabsf(pd.jumpPos.x - pd.landPos.x), fabsf(pd.jumpPos.y - pd.landPos.y));
	pd.jumpAirtimeFrac = (float)(pd.jumpAirtime - frameBeforeLand) + fraction;
	if (pd.jumpType != JUMPTYPE_LDJ)
	{
		pd.jumpDistance += 32.0f;
		pd.jumpXJDistance += 32.0f;
	}

	FinishTrackingJump(pd, lastFd);
	PrintStats(p, pd, g_hbd[client], client);
	PDResetJumpData(pd);
}

static void OnPlayerLanded(CBasePlayer* p, int client, JsPlayerData& pd, const JsFrame& fd, const JsFrame& lastFd)
{
	if (pd.speedShowPrespeed)
	{
		pd.speedShowPrespeed = false;
		SendJumpStatPrespeed(p, false, pd.jumpPrespeed);
	}
	pd.landedDucked = (fd.flags & FL_DUCKING) != 0;

	if (!pd.trackingJump || pd.jumpType == JUMPTYPE_NONE || !g_jumpTypePrintable[pd.jumpType])
	{
		JS_DEBUG_PRINT(p, "invalid jump: tracking %i type %s\n", pd.trackingJump, g_szJumpTypes[pd.jumpType]);
		PDResetJumpData(pd);
		return;
	}

	// NOTE: HACK of the original!!
	// TODO of the original: properly find the correct start height of ladderjumps with an engine trace!
	float offsetToleranceUp = 0.0001f;
	float offsetToleranceDown = 0.02f;
	if (pd.jumpType == JUMPTYPE_LDJ)
	{
		offsetToleranceUp = GC_OFFSET_TOLERANCE_LDJ;
		offsetToleranceDown = GC_OFFSET_TOLERANCE_LDJ;
	}
	else if (pd.jumpType == JUMPTYPE_BH || pd.jumpType == JUMPTYPE_DBH || pd.jumpType == JUMPTYPE_SBJ)
	{
		// don't let players bhop to lower ground!
		offsetToleranceDown = GC_OFFSET_TOLERANCE_BJ;
	}

	// second check for failstats. other one is in TrackJump()
	if (!IsFloatInRange(pd.jumpGround.z, fd.origin.z - offsetToleranceDown, fd.origin.z + offsetToleranceUp))
	{
		JS_DEBUG_PRINT(p, "jump invalid cos offset in z: jumpZ %f z %f\n", pd.jumpGround.z, fd.origin.z);
		return;
	}

	// get land position
	Vec3 landOrigin;
	Vec3 fixedVelocity;
	Vec3 airOrigin;

	// fix incorrect landing distance
	Vec3 lastPosition = lastFd.origin;
	const bool lastDucking = (lastFd.flags & FL_DUCKING) != 0;
	const bool ducking = (fd.flags & FL_DUCKING) != 0;
	if (!lastDucking && ducking)
		lastPosition.z += GC_DUCK_HEIGHT_CHANGE;
	else if (lastDucking && !ducking)
		lastPosition.z -= GC_DUCK_HEIGHT_CHANGE;

	const float gravity = CVAR_GET_FLOAT("sv_gravity") * GetPlayerGravity(p);
	const float frametime = (float)pd.frametimeMsec / 1000.0f;
	const float currentZSpeed = lastFd.velocity.z - gravity * 0.5f * frametime;
	const int isBugged = fd.origin.z > (lastFd.origin.z + currentZSpeed * frametime) ? 1 : 0;
	if (isBugged)
	{
		fixedVelocity = fd.velocity;
		// NOTE: The 0.5 here removes half the gravity in a tick, because in pmove code half the gravity is applied
		// before movement calculation and the other half after it's finished.
		// We're trying to fix a bug that happens in the middle of movement code.
		fixedVelocity.z = lastFd.velocity.z - gravity * 0.5f * frametime;
		airOrigin = lastPosition;
	}
	else
	{
		// NOTE: calculate current frame's z velocity
		Vec3 tempVel = fd.velocity;
		tempVel.z = currentZSpeed;
		// NOTE: calculate velocity after the current frame.
		fixedVelocity = tempVel;
		fixedVelocity.z -= gravity * frametime;
		// NOTE: calculate position where the player was before they were snapped to the ground
		airOrigin = lastPosition + tempVel * frametime;
	}

	// Check if jump height simulated from the jump pos matches up with actual jump offset.
	{
		const float jumpImpulse = pd.jumpVelocity.z;
		float jumpDuration = (float)(pd.timeMsec - pd.jumpStartMsec) / 1000.0f;
		// NOTE(GameChaos): Pretty hacky, haven't dove deep into the maths, this is just a rough estimate really.
		if (pd.jumpType == JUMPTYPE_LDJ)
			jumpDuration -= frametime; // ladderjumps are weird
		else if (!isBugged)
			jumpDuration += frametime;

		float expectedOffset = jumpImpulse * jumpDuration + 0.5f * -gravity * (jumpDuration * jumpDuration);
		if (fd.flags & FL_DUCKING)
			expectedOffset += GC_DUCK_HEIGHT_CHANGE;

		const float realOffset = airOrigin.z - pd.jumpPos.z;
		const float tolerance = lastFd.velocity.z * frametime - 1.0f;
		if (expectedOffset - realOffset < tolerance)
		{
			JS_DEBUG_PRINT(p, "INVALID JUMP: expected offset %f, real offset %f\n", expectedOffset, realOffset);
			PDResetJumpData(pd);
			return;
		}
	}

	const float landingZ = fd.origin.z;
	const float fraction = GetRealLandingOrigin(landingZ, airOrigin, fixedVelocity, landOrigin);
	pd.landPos = landOrigin;
	pd.jumpAirtimeFrac = (float)(pd.jumpAirtime - isBugged) + fraction;

	pd.jumpDistance = DistanceXY(pd.jumpPos, pd.landPos);
	if (isBugged)
		pd.jumpXJDistance = fmaxf_(fabsf(pd.jumpPos.x - pd.landPos.x), fabsf(pd.jumpPos.y - pd.landPos.y));
	else
		pd.jumpXJDistance = fmaxf_(fabsf(pd.jumpPos.x - fd.origin.x), fabsf(pd.jumpPos.y - fd.origin.y));

	if (pd.jumpType != JUMPTYPE_LDJ)
	{
		pd.jumpDistance += 32.0f;
		pd.jumpXJDistance += 32.0f;
	}

	const float minDistance = GetTierDist(pd.jumpType, JUMPTIER_0);
	const float maxDistance = GetTierDist(pd.jumpType, JUMPTIER_MAX_DIST);
	if (IsFloatInRange(pd.jumpDistance, minDistance, maxDistance))
	{
		FinishTrackingJump(pd, lastFd);
		PrintStats(p, pd, g_hbd[client], client);

		if (CVAR_GET_FLOAT(g_cvarSaveReplays.name) != 0.0f)
			SaveReplay(p, client, pd);
	}
	else
	{
		JS_DEBUG_PRINT(p, "Jump distance not in range (%f-%f): %f\n", minDistance, maxDistance, pd.jumpDistance);
	}
	PDResetJumpData(pd);
}

static void TrackJump(CBasePlayer* p, int client, JsPlayerData& pd, const JsFrame& fd, const JsFrame& lastFd)
{
	if (!pd.trackingJump)
		return;

	const int frameIndex = pd.jumpAirtime++;

	// Jump validation
	const float frametime = (float)pd.frametimeMsec / 1000.0f;
	const float svGravity = CVAR_GET_FLOAT("sv_gravity");
	const float fallAcceleration = fd.velocity.z - lastFd.velocity.z;
	const float expectedFallAccel = -(svGravity * frametime);
	bool isValidFallAccel = IsFloatInRange(fallAcceleration - expectedFallAccel, -0.0001f, 0.0001f);
	if (pd.framesInAir < 2)
	{
		// last frame was on ground
		isValidFallAccel = true;
	}

	// crusty teleport detection
	{
		const Vec3 posDelta = fd.origin - lastFd.origin;
		const float moveLength = sqrtf(posDelta.x * posDelta.x + posDelta.y * posDelta.y + posDelta.z * posDelta.z);
		// maxvelocity * ~vectorlen(1, 1, 1)
		const float maxMoveDistance = CVAR_GET_FLOAT("sv_maxvelocity") * 1.74f;
		if (moveLength > maxMoveDistance * frametime)
		{
			JS_DEBUG_PRINT(p, "jump invalidated: movelength %f frametime %f\n", moveLength, frametime);
			PDResetJumpData(pd);
			if (GetOptionInt(client, OPT_CLEAR_HUD_BEAM_ON_TP) != 0)
			{
				g_hbd[client].timestampMsec = -100000;
				SendJumpStatClear(p);
			}
			pd.trackingJump = false;
		}
	}

	const float playerGravity = GetPlayerGravity(p);
	if (pd.jumpType == JUMPTYPE_NONE)
		pd.trackingJump = false;
	else if (!g_jumpTypePrintable[pd.jumpType])
		pd.trackingJump = false;
	else if (playerGravity != 1.0f)
		pd.trackingJump = false;
	else if (!isValidFallAccel)
		pd.trackingJump = false;
	else if (fd.movetype != MOVETYPE_WALK && fd.movetype != MOVETYPE_FLY)
		pd.trackingJump = false;
	else if (pd.jumpAirtime < 10 && pd.framesOnGround)
		pd.trackingJump = false;

	if (!pd.trackingJump)
	{
		PDResetJumpData(pd);
		return;
	}

	// make sure the jump z velocity is broadly correct. difference isn't abs'd intentionally.
	if (fd.velocity.z - pd.jumpVelocity.z > svGravity * frametime + 2.0f)
	{
		// add back roughly the gravity of the last frame. (accurate if frametime doesn't fluctuate)
		pd.jumpVelocity.z = fd.velocity.z + svGravity * frametime;
	}

	const float speed = LengthXY(fd.velocity);
	if (speed > pd.jumpMaxspeed)
		pd.jumpMaxspeed = speed;

	const float lastSpeed = LengthXY(lastFd.velocity);
	if (speed > lastSpeed)
		pd.jumpSync++;
	else if (speed < lastSpeed)
		pd.jumpLoss += lastSpeed - speed;

	const float height = fd.origin.z - pd.jumpPos.z;
	if (height > pd.jumpHeight)
		pd.jumpHeight = height;

	if (IsOverlapping(fd.buttons, pd.jumpDir))
		pd.jumpOverlap++;

	if (IsDeadAirtime(fd.buttons, pd.jumpDir))
		pd.jumpDeadair++;

	// strafestats!
	if (pd.strafeCount + 1 < MAX_STRAFES)
	{
		if (IsNewStrafe(pd))
		{
			pd.strafeCount++;
			pd.strafeFrame[pd.strafeCount] = frameIndex;
		}

		const int strafe = pd.strafeCount;

		if (pd.jumpAirtime == 1)
			pd.strafeFrame[strafe] = 0;

		pd.strafeAirtime[strafe]++;

		if (speed > lastSpeed)
		{
			pd.strafeSync[strafe] += 1.0f;
			pd.strafeGain[strafe] += speed - lastSpeed;
		}
		else if (speed < lastSpeed)
		{
			pd.strafeLoss[strafe] += lastSpeed - speed;
		}

		if (speed > pd.strafeMax[strafe])
			pd.strafeMax[strafe] = speed;

		if (IsOverlapping(fd.buttons, pd.jumpDir))
			pd.strafeOverlap[strafe]++;

		if (IsDeadAirtime(fd.buttons, pd.jumpDir))
			pd.strafeDeadair[strafe]++;

		// efficiency & potency!
		{
			const float maxWishspeed = 30.0f;
			const float airaccelerate = CVAR_GET_FLOAT("sv_airaccelerate");
			float maxspeed = GetPlayerMaxspeed(p);
			if (fd.flags & FL_DUCKING)
				maxspeed *= 0.333f;

			const float yawdiff = fabsf(pd.angleSpeed.y);
			float perfectYawDiff = yawdiff;
			float perfectYaw = 0.0f;
			{
				float accelspeed = airaccelerate * maxspeed * frametime;
				if (accelspeed > maxWishspeed)
					accelspeed = maxWishspeed;
				const float addspeed = maxWishspeed - accelspeed;
				if (lastSpeed >= maxWishspeed)
				{
					const float s = accelspeed / lastSpeed;
					const float c = addspeed / lastSpeed;
					perfectYawDiff = asinf(s > 1.0f ? 1.0f : s) * (180.0f / 3.14159265358979f);
					perfectYaw = acosf(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c));
				}
				else
				{
					perfectYawDiff = 0.0f;
				}
			}
			float efficiency = 100.0f;
			if (perfectYawDiff != 0.0f)
				efficiency = (yawdiff - perfectYawDiff) / perfectYawDiff * 100.0f + 100.0f;

			float potency = 0.0f;
			const float optimalGain = CalculateGain(perfectYaw, maxspeed, airaccelerate, lastSpeed);
			const float actualGain = fmaxf_(0.0f, speed - lastSpeed);
			if (optimalGain > 0.0f)
				potency = (actualGain / optimalGain) * 100.0f;

			pd.strafeAvgEfficiency[strafe] += efficiency;
			if (efficiency > pd.strafePeakEfficiency[strafe])
				pd.strafePeakEfficiency[strafe] = efficiency;
			pd.jumpPotency += potency;
		}
	}

	if (frameIndex < MAX_JUMP_FRAMES)
	{
		// strafe type and mouse graph
		StrafeType strafeType = STRAFETYPE_NONE;

		const bool moveLeft = (fd.buttons & g_jumpDirLeftButton[pd.jumpDir]) != 0;
		const bool moveRight = (fd.buttons & g_jumpDirRightButton[pd.jumpDir]) != 0;

		const bool velLeft = IsWishspeedMovingLeft(fd.wishmove.x, fd.wishmove.y, pd.jumpDir);
		const bool velRight = IsWishspeedMovingRight(fd.wishmove.x, fd.wishmove.y, pd.jumpDir);
		const bool velIsZero = !velLeft && !velRight;

		if (moveLeft && !moveRight && velLeft)
			strafeType = STRAFETYPE_LEFT;
		else if (moveRight && !moveLeft && velRight)
			strafeType = STRAFETYPE_RIGHT;
		// (the original has a third, unreachable "moveRight && !moveLeft && velRight -> LEFT" branch here)
		else if (moveRight && moveLeft && velIsZero)
			strafeType = STRAFETYPE_OVERLAP;
		else if (moveRight && moveLeft && velLeft)
			strafeType = STRAFETYPE_OVERLAP_LEFT;
		else if (moveRight && moveLeft && velRight)
			strafeType = STRAFETYPE_OVERLAP_RIGHT;
		else if (!moveRight && !moveLeft && velIsZero)
			strafeType = STRAFETYPE_NONE;
		else if (!moveRight && !moveLeft && velLeft)
			strafeType = STRAFETYPE_NONE_LEFT;
		else if (!moveRight && !moveLeft && velRight)
			strafeType = STRAFETYPE_NONE_RIGHT;

		pd.strafeGraph[frameIndex] = strafeType;
		float yawDiff = fd.angles.y - lastFd.angles.y;
		if (yawDiff > 180.0f)
			yawDiff -= 360.0f;
		if (yawDiff < -180.0f)
			yawDiff += 360.0f;
		pd.mouseGraph[imax(frameIndex - 1, 0)] = yawDiff;
		pd.duckGraph[frameIndex] = (fd.flags & FL_DUCKING) ? 1 : 0;
	}

	// check for failstat after jump tracking is done
	Vec3 duckedPos = fd.origin;
	if (!(fd.flags & FL_DUCKING))
		duckedPos.z += GC_DUCK_HEIGHT_CHANGE;

	// failstat if there's absolutely no way we can land on level ground.
	float offsetToleranceDown = 0.0001f;
	if (pd.jumpType == JUMPTYPE_LDJ)
		offsetToleranceDown = GC_OFFSET_TOLERANCE_LDJ;
	else if (pd.jumpType == JUMPTYPE_BH || pd.jumpType == JUMPTYPE_DBH || pd.jumpType == JUMPTYPE_SBJ)
		offsetToleranceDown = GC_OFFSET_TOLERANCE_BJ; // don't let players bhop to lower ground!

	if (pd.jumpGround.z > duckedPos.z + offsetToleranceDown)
	{
		JS_DEBUG_PRINT(p, "jump invalid cos z is too low: jumpGroundZ %f duckedPosZ %f\n", pd.jumpGround.z, duckedPos.z);
		OnPlayerFailstat(p, client, pd);
		return;
	}

	// airpath.
	// NOTE: Track airpath after failstat has been saved, so we don't track the last frame of failstats.
	// That should happen inside of FinishTrackingJump, because we need the real landing position.
	if (!pd.framesOnGround)
	{
		const Vec3 delta = fd.origin - lastFd.origin;
		pd.jumpAirpath += LengthXY(delta);
	}
}

// ------------------------------------------------------------------------------------------------
// pm_shared hooks
// ------------------------------------------------------------------------------------------------
extern "C" void JumpStats_PM_Move(struct playermove_s* ppmove)
{
	g_pmove = ppmove;
	const int client = ppmove->player_index + 1;
	if (client >= 1 && client < GC_MAX_PLAYERS)
		g_pd[client].ladderEntity = 0;
}

extern "C" void JumpStats_PM_JumpPre(struct playermove_s* ppmove)
{
	g_preJumpGround = ppmove->onground;
}

extern "C" void JumpStats_PM_JumpPost(struct playermove_s* ppmove)
{
	g_pmove = ppmove;

	const int client = ppmove->player_index + 1;
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	const int onground = ppmove->onground;
	if (!(g_preJumpGround != -1 && onground == -1))
		return;

	if (!GetOptionInt(client, OPT_ENABLE_PLUGIN))
		return;

	JsPlayerData& pd = g_pd[client];
	const int buttons = ppmove->cmd.buttons;

	// get previous frame, current frame doesn't have proper data yet.
	JsFrame fd = GetReplayFrame(client, 1);

	const bool sbj = ((fd.flags & FL_DUCKING) && !(buttons & IN_DUCK)) || (pd.landedDucked && !(buttons & IN_DUCK));
	const bool duckbhop = (fd.flags & FL_DUCKING) && (buttons & IN_DUCK);
	const float groundOffset = fd.origin.z - pd.lastGroundPos.z;
	JumpType jumpType = JUMPTYPE_NONE;
	if (pd.framesOnGround <= MAX_BHOP_FRAMES)
	{
		// NOTE: the -2.0 is an arbitrary value. it was chosen cos bhop blocks go slightly down when bhopping on them.
		if (pd.lastGroundPosWalkedOff && groundOffset <= -2.0f)
		{
			jumpType = JUMPTYPE_WJ;
		}
		else
		{
			// TODO of the original: afterjump stats
			if (pd.jumpType == JUMPTYPE_DD)
			{
				jumpType = sbj ? JUMPTYPE_SCJ : JUMPTYPE_CJ;
			}
			else if (pd.jumpType == JUMPTYPE_GS)
			{
				if (pd.lastJumpType == JUMPTYPE_GS)
					jumpType = JUMPTYPE_MCJ;
				else if (pd.lastJumpType == JUMPTYPE_DD)
					jumpType = JUMPTYPE_DCJ;
			}
			else if (pd.jumpType == JUMPTYPE_LDJ)
			{
				// prevent jumps from ladders counting as bhops.
				jumpType = JUMPTYPE_WJ;
			}
			else
			{
				if (sbj)
				{
					if (pd.lastGroundPosWalkedOff && groundOffset == 0.0f)
						jumpType = JUMPTYPE_SLJ;
					else
						jumpType = JUMPTYPE_SBJ;
				}
				else if (duckbhop)
				{
					jumpType = JUMPTYPE_DBH;
				}
				else
				{
					jumpType = JUMPTYPE_BH;
				}
			}
		}
	}
	else
	{
		jumpType = JUMPTYPE_LJ;
	}

	if (jumpType != JUMPTYPE_NONE)
	{
		const int lastLastFrame = ReplayIndex(client, 2);
		// set jump velocity so that PM_PreventMegaBunnyJumping gets accounted for
		fd.velocity = ToVec3(ppmove->velocity);
		OnPlayerJumped(pd, jumpType, fd, g_replay[client][lastLastFrame]);
	}

	pd.lastGroundPos = fd.origin;
	pd.lastGroundPosWalkedOff = false;
}

extern "C" void JumpStats_PM_LadderMove(struct playermove_s* ppmove, struct physent_s* pLadder)
{
	if (!pLadder || !ppmove)
		return;

	const int client = ppmove->player_index + 1;
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	float mins[3] = {0, 0, 0};
	float maxs[3] = {0, 0, 0};
	if (ppmove->PM_GetModelBounds && pLadder->model)
	{
		ppmove->PM_GetModelBounds(pLadder->model, mins, maxs);
		g_pd[client].ladderMins = ToVec3(mins);
		g_pd[client].ladderMaxs = ToVec3(maxs);
	}

	// NOTE: the plugin also traced the ladder to get its normal, but that value was never used.
	g_pd[client].ladderEntity = pLadder->info;
}

// ------------------------------------------------------------------------------------------------
// player hooks
// ------------------------------------------------------------------------------------------------
void JumpStats_ClientPutInServer(CBasePlayer* p)
{
	if (!p)
		return;
	const int client = p->entindex();
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	JumpStats_Init();

	g_pd[client] = JsPlayerData(); // value-initialised: everything zero
	g_hbd[client] = JsHudBeamData();
	g_hbd[client].timestampMsec = -100000;
	g_replayIndex[client] = 0;
	g_replayCount[client] = 0;

	g_pd[client].jumpType = JUMPTYPE_NONE;
	g_pd[client].lastJumpType = JUMPTYPE_NONE;
	PDResetJumpData(g_pd[client]);
	LoadDefaultOptions(client);
}

void JumpStats_CmdStart(CBasePlayer* p, const struct usercmd_s* cmd)
{
	if (!p || !cmd)
		return;
	const int client = p->entindex();
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	JsPlayerData& pd = g_pd[client];
	pd.usercmdCount++;
	pd.frametimeMsec = cmd->msec;
	pd.timeMsec += pd.frametimeMsec;

	if (!p->IsAlive())
		return;

	if (!GetOptionInt(client, OPT_ENABLE_PLUGIN))
		return;

	// NOTE of the original: actual frame data gets updated in postthink, so functions that get called between this
	// and postthink can very easily have invalid data if the programmer isn't careful.
	g_replayIndex[client]++;
	g_replayCount[client]++;

	g_replayCount[client] = imin(g_replayCount[client], MAX_JUMP_FRAMES);
	if (g_replayIndex[client] >= MAX_JUMP_FRAMES)
		g_replayIndex[client] = 0;

	JsFrame& frame = g_replay[client][ReplayIndex(client)];
	frame.wishmove = Vec3(cmd->forwardmove, cmd->sidemove, cmd->upmove);
}

void JumpStats_PreThink(CBasePlayer* p)
{
	// NOTE: this call was already in the old jumpstats.cpp, keep it here so MoveTech still runs.
	MoveTech_PreThink(p);

	const int client = p->entindex();
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	if (!p->IsAlive())
		return;

	if (!GetOptionInt(client, OPT_ENABLE_PLUGIN))
		return;

	JsPlayerData& pd = g_pd[client];
	entvars_t* pev = p->pev;

	const int buttons = pev->button;
	const int oldButtons = pev->oldbuttons;

	// NOTE(GameChaos): tracking new frame has already started, get previous frame and the one before that
	const JsFrame fd = GetReplayFrame(client, 1);
	const JsFrame lastFd = GetReplayFrame(client, 2);

	if (!(buttons & IN_DUCK) && (oldButtons & IN_DUCK) && !(fd.flags & FL_DUCKING))
	{
		Vec3 startpos = fd.origin;
		startpos.z += 36.0f + GC_DUCK_HEIGHT_CHANGE;
		Vec3 endpos = startpos;
		endpos.z += GC_DUCK_HEIGHT_CHANGE;

		TraceResult tr;
		UTIL_TraceHull(ToVector(startpos), ToVector(endpos), ignore_monsters, head_hull, NULL, &tr);

		if (tr.flFraction == 1.0f)
		{
			// double ducked!
			pd.lastGroundPos = lastFd.origin;
			pd.lastGroundPosWalkedOff = false;
			if (pd.framesOnGround > MAX_GSTRAFE_FRAMES)
				OnPlayerJumped(pd, JUMPTYPE_DD, fd, lastFd);
			else
				OnPlayerJumped(pd, JUMPTYPE_GS, fd, lastFd);
		}
	}
}

void JumpStats_PostThink(CBasePlayer* p)
{
	const int client = p->entindex();
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	if (!p->IsAlive())
	{
		// reset when player isn't alive to avoid discontinuity
		g_replayCount[client] = 0;
		g_replayIndex[client] = 0;
		if (g_pd[client].speedShowPrespeed)
		{
			g_pd[client].speedShowPrespeed = false;
			SendJumpStatPrespeed(p, false, 0.0f);
		}
		return;
	}

	if (!GetOptionInt(client, OPT_ENABLE_PLUGIN))
		return;

	JsPlayerData& pd = g_pd[client];
	entvars_t* pev = p->pev;

	// framedata
	{
		JsFrame& frame = g_replay[client][ReplayIndex(client)];
		frame.origin = ToVec3(pev->origin);
		frame.origin.z -= (pev->flags & FL_DUCKING) ? 18.0f : 36.0f; // feet position
		frame.velocity = ToVec3(pev->velocity);
		frame.angles = ToVec3(pev->v_angle);
#if JS_USE_STAMINA
		frame.stamina = pev->fuser2;
#else
		frame.stamina = 0.0f;
#endif
		frame.buttons = pev->button;
		frame.flags = pev->flags;
		frame.movetype = pev->movetype;
	}

	// last button and velocity etc might act weird if there aren't enough frames recorded.
	if (g_replayCount[client] < 3)
		return;

	const JsFrame fd = GetReplayFrame(client, 0);
	const JsFrame lastFd = GetReplayFrame(client, 1);
	const JsFrame lastLastFd = GetReplayFrame(client, 2);

	for (int i = 0; i < 3; i++)
	{
		pd.angleSpeed[i] = NormaliseYaw(fd.angles[i] - lastFd.angles[i]);
		pd.lastAngleSpeed[i] = NormaliseYaw(lastFd.angles[i] - lastLastFd.angles[i]);
	}

	if (pev->flags & FL_ONGROUND)
	{
		pd.framesInAir = 0;
		pd.framesOnGround++;
	}
	else if (fd.movetype != MOVETYPE_FLY)
	{
		pd.framesInAir++;
		pd.framesOnGround = 0;
	}

	// LJ stuff
	if (!GetOptionInt(client, OPT_ENABLE_PLUGIN))
		return;

	if (pd.framesInAir == 1)
	{
		if (pd.lastGroundPos.x != lastFd.origin.x || pd.lastGroundPos.y != lastFd.origin.y || pd.lastGroundPos.z != lastFd.origin.z)
		{
			pd.lastGroundPos = lastFd.origin;
			pd.lastGroundPosWalkedOff = true;
		}
	}

	const bool forwardReleased = (lastFd.buttons & g_jumpDirForwardButton[pd.jumpDir]) && !(fd.buttons & g_jumpDirForwardButton[pd.jumpDir]);
	if (forwardReleased)
		pd.fwdReleaseFrame = pd.usercmdCount;

	if (fd.movetype == MOVETYPE_WALK && lastFd.movetype == MOVETYPE_FLY && !pd.trackingJump)
		OnPlayerJumped(pd, JUMPTYPE_LDJ, fd, lastFd);

	TrackJump(p, client, pd, fd, lastFd);

	if (pd.framesOnGround == 1)
		OnPlayerLanded(p, client, pd, fd, lastFd);

	// ---- beams
	JsHudBeamData& hbd = g_hbd[client];
	const int modulo = pd.usercmdCount % 20; // beams are refreshed every 20 usercmds
	const float elapsed = (float)(pd.timeMsec - hbd.timestampMsec) / 1000.0f;
	if (elapsed < 10.0f && modulo == 0)
	{
		const int life = 2;
		if (GetOptionInt(client, OPT_SHOW_VEER_BEAM))
		{
			Vec3 jumpPos(floorf(hbd.veerStart.x + 0.5f), floorf(hbd.veerStart.y + 0.5f), floorf(hbd.veerStart.z + 0.5f));
			Vec3 landPos(floorf(hbd.veerEnd.x + 0.5f), floorf(hbd.veerEnd.y + 0.5f), floorf(hbd.veerEnd.z + 0.5f));
			Vec3 beamEnd(landPos.x, jumpPos.y, landPos.z);

			const int alpha = 127;
			SendBeamPoints(p, jumpPos, landPos, 255, 255, 255, alpha, life);
			// x axis
			SendBeamPoints(p, jumpPos, beamEnd, 255, 0, 0, alpha, life);
			// y axis
			SendBeamPoints(p, landPos, beamEnd, 0, 255, 0, alpha, life);
		}

		if (GetOptionInt(client, OPT_SHOW_JUMP_BEAM))
		{
			Vec3 beamPos(floorf(hbd.veerStart.x + 0.5f), floorf(hbd.veerStart.y + 0.5f), ceilf(hbd.veerStart.z));
			Vec3 lastBeamPos;
			// only draw every 2nd frame of data. this makes the beam smoother and reduces flicker.
			for (int i = 1; i < hbd.frames; i += 2)
			{
				// make sure to draw the last point as well!
				if (i + 2 == hbd.frames)
					i = hbd.frames - 1;

				lastBeamPos = beamPos;
				beamPos.x = floorf(hbd.beamX[i] + 0.5f);
				beamPos.y = floorf(hbd.beamY[i] + 0.5f);

				int r = 255, g = 255, b = 0;
				if (hbd.beamColour[i] == JUMPBEAM_LOSS)
				{
					r = 255;
					g = 0;
					b = 0;
				}
				else if (hbd.beamColour[i] == JUMPBEAM_GAIN)
				{
					r = 0;
					g = 255;
					b = 0;
				}
				else if (hbd.beamColour[i] == JUMPBEAM_DUCK)
				{
					r = 255;
					g = 0;
					b = 255;
				}

				SendBeamPoints(p, lastBeamPos, beamPos, r, g, b, 255, life);
			}
		}
	}
}

void JumpStats_Reset(CBasePlayer* p)
{
	if (!p)
		return;
	const int client = p->entindex();
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return;

	JsPlayerData& pd = g_pd[client];
	PDResetJumpData(pd);
	pd.landedDucked = false;
	pd.lastGroundPosWalkedOff = false;
	// the discontinuity in the frame buffer would look like a huge movement
	g_replayCount[client] = 0;
	g_replayIndex[client] = 0;

	if (pd.speedShowPrespeed)
	{
		pd.speedShowPrespeed = false;
		SendJumpStatPrespeed(p, false, 0.0f);
	}

	if (GetOptionInt(client, OPT_CLEAR_HUD_BEAM_ON_TP))
	{
		g_hbd[client].timestampMsec = -100000;
		SendJumpStatClear(p);
	}
}

// ------------------------------------------------------------------------------------------------
// commands
// ------------------------------------------------------------------------------------------------
enum CommandId
{
	CMD_NONE,
	CMD_JUMPBEAM,
	CMD_VEERBEAM,
	CMD_OPTIONS,
	CMD_VERSION,
	CMD_SAVEDEFAULTS,
	CMD_DEFAULTS
};

struct CommandDef
{
	const char* chat; // "say /<chat>"
	const char* console;
	CommandId id;
};

static const CommandDef g_commands[] = {
	{"jumpbeam", "gcljs_jumpbeam", CMD_JUMPBEAM},
	{"veerbeam", "gcljs_veerbeam", CMD_VEERBEAM},
	{"gcljs", "gcljs_options", CMD_OPTIONS},
	{"gcljsversion", "gcljs_version", CMD_VERSION},
	{"gcljs_savedefaults", "gcljs_savedefaults", CMD_SAVEDEFAULTS},
	{"gcljs_defaults", "gcljs_defaults", CMD_DEFAULTS},
};

static int FindOption(const char* name)
{
	if (!name || !name[0])
		return -1;
	for (int i = 0; i < OPT_COUNT; i++)
	{
		const char* full = g_optionDefs[i].name;
		if (!js_stricmp(name, full) || !js_stricmp(name, full + 6)) // 6 = strlen("gcljs_")
			return i;
	}
	return -1;
}

static void PrintOptionLine(CBasePlayer* p, int i, float value)
{
	const OptionDef& d = g_optionDefs[i];
	if (d.isFloat)
	{
		if (value == -1.0f)
			PrintConsole(p, "  %-32s Centre   (%s) [%.2f..%.2f]\n", d.name, d.description, d.min, d.max);
		else
			PrintConsole(p, "  %-32s %-8.2f (%s) [%.2f..%.2f]\n", d.name, value, d.description, d.min, d.max);
	}
	else
	{
		PrintConsole(p, "  %-32s %-8s (%s)\n", d.name, value != 0.0f ? "Yes" : "No", d.description);
	}
}

static float ClampOption(int i, float v)
{
	if (v < g_optionDefs[i].min)
		v = g_optionDefs[i].min;
	if (v > g_optionDefs[i].max)
		v = g_optionDefs[i].max;
	if (!g_optionDefs[i].isFloat)
		v = v != 0.0f ? 1.0f : 0.0f;
	return v;
}

static void SaveDefaults()
{
	char gameDir[128];
	GET_GAME_DIR(gameDir);
	char path[300];
	snprintf(path, sizeof(path), "%s/gcljs.cfg", gameDir);

	FILE* f = fopen(path, "w");
	if (!f)
		return;

	fprintf(f, "// gcljs defaults, written by gcljs_savedefaults\n");
	for (int i = 0; i < OPT_COUNT; i++)
		fprintf(f, "%s \"%s\"\n", g_optionDefs[i].name, CVAR_GET_STRING(g_optionDefs[i].name));
	for (size_t i = 0; i < sizeof(g_tierDefaults) / sizeof(g_tierDefaults[0]); i++)
	{
		for (int tier = 0; tier < JUMPTIER_COUNT; tier++)
		{
			const char* name = g_tierCvars[g_tierDefaults[i].type][tier].name;
			fprintf(f, "%s \"%s\"\n", name, CVAR_GET_STRING(name));
		}
	}
	fprintf(f, "%s \"%s\"\n", g_cvarSaveReplays.name, CVAR_GET_STRING(g_cvarSaveReplays.name));
	fclose(f);
}

static bool IsHost(CBasePlayer* p)
{
	return !IS_DEDICATED_SERVER() && p->entindex() == 1;
}

static void HandleOptions(CBasePlayer* p, int client, int argc, char argv[][64])
{
	if (argc < 2)
	{
		PrintConsole(p, "\n[GC] GCLJS player options. Use: gcljs_options <option> [value]\n");
		for (int i = 0; i < OPT_COUNT; i++)
			PrintOptionLine(p, i, g_options[client][i]);
		PrintConsole(p, "\n");
		return;
	}

	const int opt = FindOption(argv[1]);
	if (opt < 0)
	{
		PrintConsole(p, "[GC] Unknown option \"%s\". Type gcljs_options to see all of them.\n", argv[1]);
		return;
	}

	float value;
	if (argc >= 3)
	{
		value = ClampOption(opt, (float)atof(argv[2]));
	}
	else if (!g_optionDefs[opt].isFloat)
	{
		value = g_options[client][opt] != 0.0f ? 0.0f : 1.0f; // toggle
	}
	else
	{
		// same as the menu: step by 0.05, wrap around, skip the negative range (except -1 = centre)
		value = g_options[client][opt] + 0.05f;
		if (value < g_optionDefs[opt].min || value > g_optionDefs[opt].max)
			value = g_optionDefs[opt].min;
		else if (g_optionDefs[opt].min == -1.0f && value < 0.0f)
			value = 0.0f;
	}
	g_options[client][opt] = value;
	PrintOptionLine(p, opt, value);
}

static void HandleDefaults(CBasePlayer* p, int argc, char argv[][64])
{
	if (!IsHost(p))
	{
		PrintConsole(p, "[GC] Only the listen server host can change the defaults. Use the server console otherwise.\n");
		return;
	}

	if (argc < 3)
	{
		PrintConsole(p, "\n[GC] GCLJS client defaults. Use: gcljs_defaults <option> <value>\n");
		for (int i = 0; i < OPT_COUNT; i++)
			PrintOptionLine(p, i, GetDefaultOption(i));
		PrintConsole(p, "\n");
		return;
	}

	const int opt = FindOption(argv[1]);
	if (opt < 0)
	{
		PrintConsole(p, "[GC] Unknown option \"%s\".\n", argv[1]);
		return;
	}

	char value[32];
	snprintf(value, sizeof(value), "%g", ClampOption(opt, (float)atof(argv[2])));
	CVAR_SET_STRING(g_optionDefs[opt].name, value);
	PrintOptionLine(p, opt, GetDefaultOption(opt));
}

bool JumpStats_Command(CBasePlayer* p, const char* pcmd)
{
	if (!p || !pcmd)
		return false;

	const int client = p->entindex();
	if (client < 1 || client >= GC_MAX_PLAYERS)
		return false;

	// split into tokens: either "say /cmd a b" or "cmd a b"
	char argv[6][64];
	int argc = 0;
	for (int i = 0; i < 6; i++)
		argv[i][0] = 0;

	CommandId id = CMD_NONE;
	if (FStrEq(pcmd, "say") || FStrEq(pcmd, "say_team"))
	{
		const char* text = CMD_ARGV(1);
		if (!text || text[0] != '/')
			return false;

		char line[256];
		snprintf(line, sizeof(line), "%s", text + 1);
		for (char* tok = strtok(line, " \t"); tok && argc < 6; tok = strtok(NULL, " \t"))
			snprintf(argv[argc++], sizeof(argv[0]), "%s", tok);

		for (size_t i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++)
		{
			if (argc > 0 && !js_stricmp(argv[0], g_commands[i].chat))
			{
				id = g_commands[i].id;
				break;
			}
		}
	}
	else
	{
		for (size_t i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++)
		{
			if (!js_stricmp(pcmd, g_commands[i].console))
			{
				id = g_commands[i].id;
				break;
			}
		}
		if (id == CMD_NONE)
			return false;

		snprintf(argv[argc++], sizeof(argv[0]), "%s", pcmd);
		for (int i = 1; i < CMD_ARGC() && argc < 6; i++)
			snprintf(argv[argc++], sizeof(argv[0]), "%s", CMD_ARGV(i));
	}

	if (id == CMD_NONE)
		return false;

	switch (id)
	{
	case CMD_JUMPBEAM:
		g_options[client][OPT_SHOW_JUMP_BEAM] = GetOptionInt(client, OPT_SHOW_JUMP_BEAM) ? 0.0f : 1.0f;
		break;
	case CMD_VEERBEAM:
		g_options[client][OPT_SHOW_VEER_BEAM] = GetOptionInt(client, OPT_SHOW_VEER_BEAM) ? 0.0f : 1.0f;
		break;
	case CMD_OPTIONS:
		HandleOptions(p, client, argc, argv);
		break;
	case CMD_VERSION:
	{
		char msg[128];
		snprintf(msg, sizeof(msg), "%s GCLJS version %s.", CHAT_PREFIX, JS_VERSION);
		PrintChat(p, msg);
		break;
	}
	case CMD_SAVEDEFAULTS:
		if (IsHost(p))
		{
			SaveDefaults();
			char msg[64];
			snprintf(msg, sizeof(msg), "%s Saved all cvars!", CHAT_PREFIX);
			PrintChat(p, msg);
		}
		else
		{
			PrintConsole(p, "[GC] Only the listen server host can save the defaults.\n");
		}
		break;
	case CMD_DEFAULTS:
		HandleDefaults(p, argc, argv);
		break;
	default:
		return false;
	}

	return true;
}