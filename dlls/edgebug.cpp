// edgebug.cpp - EdgeBug Stats, ported from the AMX Mod X plugin by xPaw (v2.2)
// Server side, put in dlls/.
//
// Detects successful edgebugs and prints the stats (distance, fall speed, engine FPS)
// to the player's console, sends them to the custom client HUD (player + spectators),
// and to chat for big ones (only for players who enabled it with eb_chat).
// Also drives the EdgeBug movement assist: its strength scales with each player's fps.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "usercmd.h"
#include "edgebug.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "movement_tech.h"

// ---------------- tunables ----------------
#define EDGEBUG_PREFIX "[XJ]"				// chat prefix
constexpr int EDGEBUG_MIN_DISTANCE = 17;	// shorter falls are ignored
constexpr int EDGEBUG_CHAT_MIN_SPEED = 500; // fall speed needed to announce in chat
constexpr int EDGEBUG_BIG_DISTANCE = 2500;	// distance that triggers the sound
constexpr bool EDGEBUG_USE_SOUNDS = false;	// needs jumpstats/godlike.wav + jumpstats/holyshit.wav in your game folder

constexpr int EDGEBUG_ASSIST_MIN_DISTANCE = 0; // assisted EBs shorter than this don't count (0 = always count, 17 = same as natural EBs)

// assist strength scaling with fps: (fps, units) points, linear in between, clamped outside
constexpr int EDGEBUG_ASSIST_FPS_LOW = 30;	  // at or below this fps -> EDGEBUG_ASSIST_VAL_LOW
constexpr int EDGEBUG_ASSIST_FPS_MID = 200;	  // at this fps -> EDGEBUG_ASSIST_VAL_MID
constexpr int EDGEBUG_ASSIST_FPS_HIGH = 1000; // at or above this fps -> EDGEBUG_ASSIST_VAL_HIGH
constexpr int EDGEBUG_ASSIST_VAL_LOW = 2;
constexpr int EDGEBUG_ASSIST_VAL_MID = 10;
constexpr int EDGEBUG_ASSIST_VAL_HIGH = 20;
constexpr float EDGEBUG_ASSIST_SAMPLE_TIME = 5.0f; // seconds between fps checks
constexpr int EDGEBUG_ASSIST_FORCE_MAX = 64;	   // highest value eb_assist_force accepts
constexpr bool EDGEBUG_ASSIST_LOG = true;		   // print to the server console when the automatic value changes

extern float g_EbAssistFall[33]; // set by the pm_shared EdgeBug assist

int gmsgEdgeBug = 0;

// EdgeBug assist master switch: 0 = off, anything else = on (strength comes from fps, or eb_assist_force)
cvar_t sv_eb_assist = {"sv_eb_assist", "1", FCVAR_SERVER};
static bool g_bEbCvarRegistered = false;

struct EbState
{
	int edgebugs = 0;
	int frameTime[2] = {0, 0}; // [0] = current cmd msec, [1] = previous
	bool edgeBug = false;
	bool falling = false;
	float jumpOff = 0.0f;
	float touchedVelocity = 0.0f;
	bool chat = false; // per-player: show EdgeBug announcements in chat (eb_chat)

	// assist strength (per player, because fps is per player)
	int assistAuto = EDGEBUG_ASSIST_VAL_MID; // latest value computed from fps
	int assistForce = -1;					 // eb_assist_force value, -1 = not forced
	int assistSent = -1;					 // last value pushed to physinfo "eba"
	float nextSample = 0.0f;				 // next time we read the fps
};

static EbState g_Eb[33]; // indexed by entity index (1..32)

static EbState& GetState(CBasePlayer* pPlayer)
{
	int idx = pPlayer->entindex();
	if (idx < 0 || idx > 32)
		idx = 0;
	return g_Eb[idx];
}

// does not touch st.chat or the assist fields on purpose, those survive landings and respawns
static void ClearState(EbState& st)
{
	st.edgeBug = false;
	st.falling = false;
	st.edgebugs = 0;
}

// fps -> assist units: 30 fps = 2, 200 fps = 10, 1000 fps = 20, linear in between
static int Edgebug_AssistForFps(int fps)
{
	if (fps <= EDGEBUG_ASSIST_FPS_LOW)
		return EDGEBUG_ASSIST_VAL_LOW;
	if (fps >= EDGEBUG_ASSIST_FPS_HIGH)
		return EDGEBUG_ASSIST_VAL_HIGH;

	float v;
	if (fps <= EDGEBUG_ASSIST_FPS_MID)
	{
		const float t = (float)(fps - EDGEBUG_ASSIST_FPS_LOW) / (float)(EDGEBUG_ASSIST_FPS_MID - EDGEBUG_ASSIST_FPS_LOW);
		v = EDGEBUG_ASSIST_VAL_LOW + t * (EDGEBUG_ASSIST_VAL_MID - EDGEBUG_ASSIST_VAL_LOW);
	}
	else
	{
		const float t = (float)(fps - EDGEBUG_ASSIST_FPS_MID) / (float)(EDGEBUG_ASSIST_FPS_HIGH - EDGEBUG_ASSIST_FPS_MID);
		v = EDGEBUG_ASSIST_VAL_MID + t * (EDGEBUG_ASSIST_VAL_HIGH - EDGEBUG_ASSIST_VAL_MID);
	}

	return (int)(v + 0.5f);
}

// works out what this player's assist should be right now and pushes it to pm_shared when it changed
static void Edgebug_UpdateAssist(CBasePlayer* pPlayer, EbState& st)
{
	int target;

	if ((int)sv_eb_assist.value <= 0)
		target = 0; // master switch off
	else if (st.assistForce >= 0 && CVAR_GET_FLOAT("sv_cheats") != 0)
		target = st.assistForce; // forced (cheats only)
	else
		target = st.assistAuto;

	if (target == st.assistSent)
		return;

	st.assistSent = target;
	g_engfuncs.pfnSetPhysicsKeyValue(pPlayer->edict(), "eba", UTIL_dtos1(target));
}

void Edgebug_RegisterMessages()
{
	if (!g_bEbCvarRegistered)
	{
		CVAR_REGISTER(&sv_eb_assist);
		g_bEbCvarRegistered = true;
	}

	if (gmsgEdgeBug)
		return;

	gmsgEdgeBug = REG_USER_MSG("EdgeBug", -1);
	ALERT(at_console, "[edgebug] registered EdgeBug as msg %d\n", gmsgEdgeBug);
}

// called every frame from StartFrame: re-reads each player's fps every few seconds
// and keeps their assist strength up to date
void Edgebug_Think()
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = (CBasePlayer*)UTIL_PlayerByIndex(i);
		if (!pPlayer || !pPlayer->IsPlayer())
			continue;
		if (pPlayer->pev->flags & FL_FAKECLIENT)
			continue;

		EbState& st = g_Eb[i];

		const bool due = gpGlobals->time >= st.nextSample ||
						 st.nextSample > gpGlobals->time + EDGEBUG_ASSIST_SAMPLE_TIME; // time went backwards (map change)

		if (due && st.frameTime[0] > 0)
		{
			const int fps = (int)floorf(1000.0f / (float)st.frameTime[0] + 0.5f);
			const int value = Edgebug_AssistForFps(fps);

			if (EDGEBUG_ASSIST_LOG && value != st.assistAuto)
				ALERT(at_console, "[edgebug] assist auto: %d fps -> %d units\n", fps, value);

			st.assistAuto = value;
			st.nextSample = gpGlobals->time + EDGEBUG_ASSIST_SAMPLE_TIME;
		}

		Edgebug_UpdateAssist(pPlayer, st);
	}
}

void Edgebug_ClientPutInServer(CBasePlayer* pPlayer)
{
	// fresh player in this slot: back to defaults (chat off, assist automatic)
	EbState& st = GetState(pPlayer);
	ClearState(st);
	st.chat = false;
	st.assistAuto = EDGEBUG_ASSIST_VAL_MID; // until the first fps sample comes in
	st.assistForce = -1;
	st.assistSent = -1;
	st.nextSample = 0.0f;

	// new players need the assist key right away
	Edgebug_UpdateAssist(pPlayer, st);
}

// handles "eb_chat" and "eb_assist_force" from the player's console
bool Edgebug_Command(CBasePlayer* pPlayer, const char* pcmd)
{
	if (FStrEq(pcmd, "eb_chat"))
	{
		EbState& st = GetState(pPlayer);

		if (CMD_ARGC() > 1)
			st.chat = atoi(CMD_ARGV(1)) != 0;
		else
			st.chat = !st.chat;

		CLIENT_PRINTF(pPlayer->edict(), print_console,
			st.chat ? "[edgebug] chat announcements ON\n" : "[edgebug] chat announcements OFF\n");
		return true;
	}

	if (FStrEq(pcmd, "eb_assist_force"))
	{
		// cheats only: without sv_cheats the command is silently discarded
		if (CVAR_GET_FLOAT("sv_cheats") == 0)
			return true;

		EbState& st = GetState(pPlayer);

		if (CMD_ARGC() < 2)
		{
			char szInfo[128];
			if (st.assistForce >= 0)
				snprintf(szInfo, sizeof(szInfo), "[edgebug] assist forced to %d (eb_assist_force off to go back to automatic)\n", st.assistForce);
			else
				snprintf(szInfo, sizeof(szInfo), "[edgebug] assist automatic, currently %d\n", st.assistAuto);
			CLIENT_PRINTF(pPlayer->edict(), print_console, szInfo);
			return true;
		}

		const char* arg = CMD_ARGV(1);

		if (FStrEq(arg, "off") || FStrEq(arg, "-1"))
		{
			st.assistForce = -1;
			CLIENT_PRINTF(pPlayer->edict(), print_console, "[edgebug] assist back to automatic (fps based)\n");
		}
		else if (arg[0] >= '0' && arg[0] <= '9')
		{
			int v = atoi(arg);
			if (v > EDGEBUG_ASSIST_FORCE_MAX)
				v = EDGEBUG_ASSIST_FORCE_MAX;

			st.assistForce = v;

			char szInfo[96];
			snprintf(szInfo, sizeof(szInfo), "[edgebug] assist forced to %d\n", v);
			CLIENT_PRINTF(pPlayer->edict(), print_console, szInfo);
		}
		else
		{
			CLIENT_PRINTF(pPlayer->edict(), print_console, "usage: eb_assist_force <0-64> | off\n");
		}

		Edgebug_UpdateAssist(pPlayer, st);
		return true;
	}

	return false;
}

static void SendHud(CBasePlayer* pTarget, int count, int dist, int speed, int fps)
{
	if (!gmsgEdgeBug)
		return;

	MESSAGE_BEGIN(MSG_ONE, gmsgEdgeBug, NULL, pTarget->edict());
	WRITE_BYTE(count > 255 ? 255 : count);
	WRITE_SHORT(dist > 32767 ? 32767 : dist);
	WRITE_SHORT(speed > 32767 ? 32767 : speed);
	WRITE_SHORT(fps > 32767 ? 32767 : fps);
	MESSAGE_END();
}

static void PrintMessage(CBasePlayer* pPlayer, EbState& st, int iDistance, int iSpeed, int iEngineFps)
{
	st.edgebugs++;
	MoveTech_OnEdgebug(pPlayer, st.edgebugs);

	char szTag[16] = "";
	switch (st.edgebugs)
	{
	case 1: break;
	case 2: snprintf(szTag, sizeof(szTag), "Double "); break;
	case 3: snprintf(szTag, sizeof(szTag), "Triple "); break;
	default: snprintf(szTag, sizeof(szTag), "%ix ", st.edgebugs); break;
	}

	char szLine[256];
	snprintf(szLine, sizeof(szLine),
		"\nSuccessful %sEdgeBug was made! Fall Distance: %i units. Fall Speed: %i u/s. Engine FPS: %i\n",
		szTag, iDistance, iSpeed, iEngineFps);
	CLIENT_PRINTF(pPlayer->edict(), print_console, szLine);

	// custom HUD: the player and anyone spectating them
	SendHud(pPlayer, st.edgebugs, iDistance, iSpeed, iEngineFps);

	const int iSelf = pPlayer->entindex();
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pSpec = (CBasePlayer*)UTIL_PlayerByIndex(i);
		if (!pSpec || pSpec == pPlayer || !pSpec->IsPlayer() || pSpec->IsAlive())
			continue;
		if (pSpec->pev->flags & FL_FAKECLIENT)
			continue;
		if (pSpec->pev->iuser2 == iSelf)
			SendHud(pSpec, st.edgebugs, iDistance, iSpeed, iEngineFps);
	}

	if (iSpeed < EDGEBUG_CHAT_MIN_SPEED)
		return;

	// chat: only players who turned it on with eb_chat
	char szChat[256];
	snprintf(szChat, sizeof(szChat), "%s %s did %sEdgeBug! Fall distance is %i units with %i u/s.\n",
		EDGEBUG_PREFIX, STRING(pPlayer->pev->netname), szTag, iDistance, iSpeed);

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (!g_Eb[i].chat)
			continue;

		CBasePlayer* pTo = (CBasePlayer*)UTIL_PlayerByIndex(i);
		if (!pTo || !pTo->IsPlayer())
			continue;
		if (pTo->pev->flags & FL_FAKECLIENT)
			continue;

		ClientPrint(pTo->pev, HUD_PRINTTALK, szChat);
	}

	if (EDGEBUG_USE_SOUNDS && iDistance >= EDGEBUG_BIG_DISTANCE)
	{
		const char* snd = st.edgebugs > 1 ? "jumpstats/holyshit.wav" : "jumpstats/godlike.wav";
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
			if (pEnt && pEnt->IsPlayer())
				CLIENT_COMMAND(pEnt->edict(), "spk \"%s\"\n", snd);
		}
	}
}

void Edgebug_CmdStart(const edict_s* player, const usercmd_s* cmd)
{
	const int idx = ENTINDEX(const_cast<edict_t*>(player));
	if (idx < 1 || idx > 32)
		return;

	EbState& st = g_Eb[idx];
	st.frameTime[1] = st.frameTime[0];
	st.frameTime[0] = cmd->msec;
}

void Edgebug_PreThink(CBasePlayer* pPlayer)
{
	if (!pPlayer->IsAlive())
		return;

	EbState& st = GetState(pPlayer);
	entvars_t* pev = pPlayer->pev;

	// did the movement assist convert a landing into an EdgeBug last frame?
	float flAssistFall = 0.0f;
	const int iIdx = pPlayer->entindex();
	if (iIdx >= 1 && iIdx <= 32)
	{
		flAssistFall = g_EbAssistFall[iIdx];
		g_EbAssistFall[iIdx] = 0.0f;
	}

	if (pev->waterlevel >= 2)
	{
		ClearState(st);
		return;
	}

	const int iFlags = pev->flags;

	if (!st.falling && !(iFlags & FL_ONGROUND))
	{
		st.falling = true;
		st.jumpOff = pev->absmin.z + 1.0f;
	}

	if (!st.falling)
		return;

	if (iFlags & FL_ONGROUND)
	{
		ClearState(st);
		return;
	}

	// assisted EdgeBug: always counts, skips the physics checks below
	if (flAssistFall > 0.0f && st.frameTime[0] > 0)
	{
		st.edgeBug = false;

		const int iAssistFps = (int)floorf(1.0f / (st.frameTime[0] * 0.001f) + 0.5f);
		const float flAssistOriginZ = pev->absmin.z + 1.0f;

		int iAssistDist = (int)floorf(st.jumpOff - flAssistOriginZ);
		if (iAssistDist < 0)
			iAssistDist = 0;

		if (iAssistDist < EDGEBUG_ASSIST_MIN_DISTANCE || iAssistFps <= 0)
		{
			ClearState(st);
			return;
		}

		PrintMessage(pPlayer, st, iAssistDist, (int)floorf(flAssistFall + 0.5f), iAssistFps);

		st.jumpOff = flAssistOriginZ;
		return;
	}

	if (!st.edgeBug)
		return;

	st.edgeBug = false;

	if (st.frameTime[0] <= 0)
		return; // no usercmd seen yet, can't compute fps

	const Vector vVelocity = pev->velocity;

	const int iEngineFps = (int)floorf(1.0f / (st.frameTime[0] * 0.001f) + 0.5f);
	if (iEngineFps <= 0)
		return;
	const int iPossibleGain = 2000 / iEngineFps;

	const float flGravity = pev->gravity;
	const float flSvGravity = CVAR_GET_FLOAT("sv_gravity");

	const float flCheck = fabsf(vVelocity.z + flSvGravity * flGravity * 0.001f * 0.5f * st.frameTime[1]);

	if (fabsf(vVelocity.z) <= iPossibleGain && fabsf(st.touchedVelocity) > iPossibleGain && flCheck < 0.00009f)
	{
		const float flFallVelocity = pev->flFallVelocity;
		const float flOriginZ = pev->absmin.z + 1.0f;

		const int iDistance = (int)floorf(st.jumpOff - flOriginZ);

		if (iDistance < EDGEBUG_MIN_DISTANCE)
		{
			ClearState(st);
			return;
		}

		PrintMessage(pPlayer, st, iDistance, (int)floorf(flFallVelocity + 0.5f), iEngineFps);

		st.jumpOff = flOriginZ;
	}
}

void Edgebug_Touch(CBasePlayer* pPlayer)
{
	EbState& st = GetState(pPlayer);
	if (!st.falling)
		return;

	const Vector vVelocity = pPlayer->pev->velocity;
	if (vVelocity.z >= 0.0f)
		return;

	const Vector vOrigin = pPlayer->pev->origin;
	const float flMagic = fabsf(vOrigin.z - truncf(vOrigin.z));

	if (flMagic == 0.03125f || flMagic == 0.96875f) // Lt.Rat is watching you!
	{
		st.edgeBug = true;
		st.touchedVelocity = vVelocity.z;
	}
}

void Edgebug_Reset(CBaseEntity* pEntity)
{
	if (!pEntity || !pEntity->IsPlayer())
		return;

	int idx = pEntity->entindex();
	if (idx < 1 || idx > 32)
		return;

	ClearState(g_Eb[idx]);
}