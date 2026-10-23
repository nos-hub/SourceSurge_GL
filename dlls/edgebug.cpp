// edgebug.cpp - EdgeBug Stats, ported from the AMX Mod X plugin by xPaw (v2.2)
// Server side, put in dlls/.
//
// Detects successful edgebugs and prints the stats (distance, fall speed, engine FPS)
// to the player's console + HUD, to spectators watching them, and to chat for big ones.

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
#define EDGEBUG_PREFIX "[XJ]"                   // chat prefix
constexpr int   EDGEBUG_MIN_DISTANCE   = 17;    // shorter falls are ignored
constexpr int   EDGEBUG_CHAT_MIN_SPEED = 500;   // fall speed needed to announce in chat
constexpr int   EDGEBUG_BIG_DISTANCE   = 2500;  // distance that triggers the sound
constexpr bool  EDGEBUG_USE_SOUNDS     = false; // needs jumpstats/godlike.wav + jumpstats/holyshit.wav in your game folder
constexpr bool  EDGEBUG_DEBUG          = true;  // print detection details to the console (turn off once it works)

struct EbState
{
	int   edgebugs = 0;
	int   frameTime[2] = { 0, 0 }; // [0] = current cmd msec, [1] = previous
	bool  edgeBug = false;
	bool  falling = false;
	float jumpOff = 0.0f;
	float touchedVelocity = 0.0f;
};

static EbState g_Eb[33]; // indexed by entity index (1..32)

static EbState& GetState(CBasePlayer* pPlayer)
{
	int idx = pPlayer->entindex();
	if (idx < 0 || idx > 32)
		idx = 0;
	return g_Eb[idx];
}

static void ClearState(EbState& st)
{
	st.edgeBug = false;
	st.falling = false;
	st.edgebugs = 0;
}

static void DebugPrint(CBasePlayer* pPlayer, const char* msg)
{

}

static void ShowHud(CBasePlayer* pTarget, const char* text)
{
	hudtextparms_t p;
	memset(&p, 0, sizeof(p));
	p.x = -1.0f;
	p.y = 0.65f;
	p.effect = 0;
	p.r1 = 255; p.g1 = 127; p.b1 = 0;   p.a1 = 0;
	p.r2 = 255; p.g2 = 255; p.b2 = 250; p.a2 = 0;
	p.fadeinTime = 0.7f;
	p.fadeoutTime = 0.7f;
	p.holdTime = 6.0f;
	p.fxTime = 6.0f;
	p.channel = 3;
	UTIL_HudMessage(pTarget, p, text);
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

	char szMessage[256];
	snprintf(szMessage, sizeof(szMessage),
		"Successful %sEdgeBug was made!\nFall Distance: %i units\nFall Speed: %i u/s\nEngine FPS: %i",
		szTag, iDistance, iSpeed, iEngineFps);
	ShowHud(pPlayer, szMessage);

	// show the stats to dead players spectating this player
	const int iSelf = pPlayer->entindex();
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pSpec = (CBasePlayer*)UTIL_PlayerByIndex(i);
		if (!pSpec || pSpec == pPlayer || !pSpec->IsPlayer() || pSpec->IsAlive())
			continue;
		if (pSpec->pev->flags & FL_FAKECLIENT)
			continue;
		if (pSpec->pev->iuser2 == iSelf)
			ShowHud(pSpec, szMessage);
	}

	if (iSpeed < EDGEBUG_CHAT_MIN_SPEED)
		return;

	char szChat[256];
	snprintf(szChat, sizeof(szChat), "%s %s did %sEdgeBug! Fall distance is %i units with %i u/s.\n",
		EDGEBUG_PREFIX, STRING(pPlayer->pev->netname), szTag, iDistance, iSpeed);
	UTIL_ClientPrintAll(HUD_PRINTTALK, szChat);

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

	if (fabsf(vVelocity.z) <= iPossibleGain
		&& fabsf(st.touchedVelocity) > iPossibleGain
		&& flCheck < 0.00009f)
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
	else if (EDGEBUG_DEBUG)
	{
		DebugPrint(pPlayer, UTIL_VarArgs(
			"[edgebug] touch looked like one but the check failed: vz=%.5f touchedVz=%.1f possibleGain=%i check=%.6f\n",
			vVelocity.z, st.touchedVelocity, iPossibleGain, flCheck));
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

		if (EDGEBUG_DEBUG)
			DebugPrint(pPlayer, UTIL_VarArgs("[edgebug] magic touch: origin.z frac=%.5f vz=%.1f\n", flMagic, vVelocity.z));
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
