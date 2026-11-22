// ---------------------------------------------------------------------------
// Source Surge - 1v1 mode (server side), v2
//
//  ss_1v1_mode 0  DUEL: continuous duel, first to ss_1v1_killlimit. A dead player picks the
//                 respawn spot on a free top-down camera (client side), the server validates it.
//  ss_1v1_mode 1  HIDE & SEEK: each player has a clock (ss_1v1_hns_time). Only the HIDER's clock
//                 ticks. When the seeker kills the hider the roles swap (the killer becomes the
//                 hider, his own clock starts ticking). The killed player respawns automatically
//                 where he died after ss_1v1_hns_respawn seconds. First clock to 0 wins.
//
//  A sound (ss_1v1_spawnsound, volume ss_1v1_spawnsound_vol) is played at every spawn position.
//
//  v2 also contains every fix from WEAPON_FIX_2 / CRASH_FIX / CRASH_FIX_2:
//   * banned weapons are blocked at creation (GiveNamedItem) and at pickup (AddPlayerItem)
//   * everything is allowed while the mod's own Spawn() runs (g_inSpawn), then RemoveAllItems wipes it
//   * EnforceWeapons() runs once per respawn / match start, never periodically
//   * entity guard turns the mode off before the engine runs out of edicts
//
// MERGE NOTE: if you changed v1 of this file to make it compile, diff first and keep those fixes.
// ---------------------------------------------------------------------------
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "duel1v1_shared.h"
#include "duel1v1.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

// ------------------------------------------------------------------ state ---
static int gmsgDuelState = 0;
static int gmsgDuelCfg   = 0;

static int    g_state = DUEL_OFF;
static int    g_duelist[2] = { 0, 0 };   // edict indices of the two duelists (0 = none)
static int    g_winner = 0;              // edict index, 0 = draw / none
static bool   g_ready[33];
static int    g_score[33];
static bool   g_choosing[33];            // duel mode: dead and picking a spot
static float  g_pickStart[33];
static float  g_godUntil[33];
static Vector g_camOrg[33];
static float  g_camTime[33];
static bool   g_camValid[33];
static float  g_stateEnd   = 0;
static float  g_matchStart = 0;
static float  g_nextState  = 0;
static float  g_nextCfg    = 0;
static float  g_lastThink  = 0;
static bool   g_cfgDirty   = true;
static bool   g_inSpawn    = false;      // true while the mod's own Spawn() gives its default items

// hide & seek
static float  g_hnsTime[33];             // seconds left on each player's hider clock
static int    g_hider = 0;               // edict index of the current hider
static float  g_respawnAt[33];           // > 0 while waiting for the automatic respawn
static Vector g_deathPos[33];
static bool   g_applyRole[33];           // re-apply the role loadout next frame (after a role swap)

// spawn sound (name captured at precache time, only a precached sound may be played)
static char   g_sndName[96] = "";

static cvar_t g_cvars[DUEL_MAX_OPTS];
static cvar_t g_cvWeapons = { "ss_1v1_weapons",    DUEL_DEFAULT_WEAPONS,    FCVAR_SERVER };
static cvar_t g_cvSound   = { "ss_1v1_spawnsound", DUEL_DEFAULT_SPAWNSOUND, FCVAR_SERVER };
static cvar_t g_cvDebug   = { "ss_1v1_debug",      "0" };
static cvar_t g_cvHiderWeapons = { "ss_1v1_hns_hider_weapons", "weapon_camera", FCVAR_SERVER };

// ---------------------------------------------------------------- helpers ---
static inline float CF(const char *name) { return CVAR_GET_FLOAT(name); }

static bool HnsMode(void)
{
	return CF("ss_1v1_mode") >= 1.0f;
}

static CBasePlayer *PlayerByIdx(int idx)
{
	if (idx < 1 || idx > gpGlobals->maxClients || idx > 32)
		return NULL;
	CBaseEntity *pEnt = UTIL_PlayerByIndex(idx);
	if (!pEnt || !pEnt->IsPlayer())
		return NULL;
	if (!STRING(pEnt->pev->netname)[0])
		return NULL;
	return (CBasePlayer *)pEnt;
}

static bool IsDuelist(int idx)
{
	return idx > 0 && (idx == g_duelist[0] || idx == g_duelist[1]);
}

static int EnemyOf(int idx)
{
	if (idx == g_duelist[0]) return g_duelist[1];
	if (idx == g_duelist[1]) return g_duelist[0];
	return 0;
}

static bool IsPicking(int idx)
{
	return g_state == DUEL_LIVE && idx > 0 && idx <= 32 && g_choosing[idx];
}

// The weapon list that applies to this duelist right now.
static const char *RoleList(int idx)
{
	if (HnsMode() && idx == g_hider)
		return CVAR_GET_STRING("ss_1v1_hns_hider_weapons");
	return CVAR_GET_STRING("ss_1v1_weapons");
}

static const Duel1v1Opt *FindOpt(const char *name)
{
	for (int i = 0; i < DUEL_NUM_OPTS; i++)
		if (!strcmp(g_Duel1v1Opts[i].cvar, name))
			return &g_Duel1v1Opts[i];
	return NULL;
}

static bool ListHasIn(const char *list, const char *classname)
{
	const char *p = list;
	const size_t n = strlen(classname);
	while (*p)
	{
		while (*p == ',' || *p == ' ') p++;
		const char *e = p;
		while (*e && *e != ',' && *e != ' ') e++;
		if ((size_t)(e - p) == n && !strncmp(p, classname, n))
			return true;
		p = e;
	}
	return false;
}

// Is classname in the comma separated ss_1v1_weapons list?
static bool ListHas(const char *classname)
{
	return ListHasIn(CVAR_GET_STRING("ss_1v1_weapons"), classname);
}

static void SetWeaponAllowed(const char *classname, bool on)
{
	char out[256];
	out[0] = 0;
	for (int i = 0; i < DUEL_NUM_WEAPONS; i++)
	{
		const char *c = g_Duel1v1Weapons[i].classname;
		const bool allow = !strcmp(c, classname) ? on : ListHas(c);
		if (!allow) continue;
		if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
		strncat(out, c, sizeof(out) - strlen(out) - 1);
	}
	CVAR_SET_STRING("ss_1v1_weapons", out);
}

static void FixRespawnBounds(void)
{
	const float mn = CF("ss_1v1_respawn_min");
	if (CF("ss_1v1_respawn_max") < mn + 64.0f)
		CVAR_SET_FLOAT("ss_1v1_respawn_max", mn + 64.0f);
}

// ------------------------------------------------------------- public api ---
bool Duel1v1_Active(void)
{
	return CF("ss_1v1") != 0;
}

// True while a dead duelist must NOT be able to respawn on its own:
// duel mode (picking a spot) or hide & seek (waiting for the automatic respawn).
// Use this in the respawn gate (PlayerDeathThink / FPlayerCanRespawn).
bool Duel1v1_IsChoosing(CBasePlayer *p)
{
	if (!p || !Duel1v1_Active() || g_state != DUEL_LIVE)
		return false;
	const int idx = ENTINDEX(p->edict());
	if (idx < 1 || idx > 32)
		return false;
	return g_choosing[idx] || g_respawnAt[idx] > 0.0f;
}

bool Duel1v1_WeaponAllowed(const char *classname)
{
	if (g_inSpawn || !Duel1v1_Active())
		return true;
	if (strncmp(classname, "weapon_", 7) != 0)
		return true; // ammo_*, item_* etc. are not our business
	return ListHas(classname);
}

// Role aware version: use this one in the GiveNamedItem / AddPlayerItem hooks (see player.cpp below).
bool Duel1v1_PlayerMayHave(CBasePlayer *p, const char *classname)
{
	if (g_inSpawn || !Duel1v1_Active())
		return true;
	if (strncmp(classname, "weapon_", 7) != 0)
		return true;
	if (!p)
		return ListHas(classname);
	const int idx = ENTINDEX(p->edict());
	if (!IsDuelist(idx))
		return ListHas(classname);
	return ListHasIn(RoleList(idx), classname);
}

bool Duel1v1_GetPVSOrigin(edict_t *pClient, Vector &outOrigin)
{
	if (!pClient || g_state != DUEL_LIVE)
		return false;
	const int idx = ENTINDEX(pClient);
	if (idx < 1 || idx > 32 || !g_choosing[idx])
		return false;
	CBasePlayer *pe = PlayerByIdx(EnemyOf(idx));
	if (!pe)
		return false;

	// Default: what the enemy can see (always sensible, the ring is centered on them).
	outOrigin = pe->pev->origin;

	// If the free camera is inside playable space, use it so entities around it are networked.
	// When it floats above the map (solid/void) POINT_CONTENTS is not EMPTY and we keep the enemy PVS.
	if (g_camValid[idx] && gpGlobals->time - g_camTime[idx] < 0.5f &&
	    POINT_CONTENTS(g_camOrg[idx]) == CONTENTS_EMPTY)
	{
		outOrigin = g_camOrg[idx];
	}
	return true;
}

void Duel1v1_Precache(void)
{
	g_sndName[0] = 0;
	const char *s = CVAR_GET_STRING("ss_1v1_spawnsound");
	if (s && s[0] && strlen(s) < sizeof(g_sndName))
	{
		strcpy(g_sndName, s);
		PRECACHE_SOUND((char *)g_sndName);
	}
}

// ----------------------------------------------------------- spawn logic ---
static void GiveLoadout(CBasePlayer *p)
{
	p->RemoveAllItems(false); // keep the suit

	const char *list = RoleList(ENTINDEX(p->edict()));
	const char *first = NULL;
	for (int i = 0; i < DUEL_NUM_WEAPONS; i++)
	{
		const char *c = g_Duel1v1Weapons[i].classname;
		if (!ListHasIn(list, c)) continue;
		p->GiveNamedItem(c);
		if (!first) first = c;
	}

	if (CF("ss_1v1_fullammo") != 0)
	{
		for (int i = 0; i < MAX_ITEM_TYPES; i++)
		{
			for (CBasePlayerItem *it = p->m_rgpPlayerItems[i]; it; it = it->m_pNext)
			{
				ItemInfo II;
				memset(&II, 0, sizeof(II));
				if (!it->GetItemInfo(&II))
					continue;
				if (II.pszAmmo1 && II.iMaxAmmo1 > 0)
					p->GiveAmmo(II.iMaxAmmo1, II.pszAmmo1, II.iMaxAmmo1);
				if (II.pszAmmo2 && II.iMaxAmmo2 > 0)
					p->GiveAmmo(II.iMaxAmmo2, II.pszAmmo2, II.iMaxAmmo2);
			}
		}
	}

	if (first)
		p->SelectItem(first);
}

// Remove every weapon that is not on the whitelist. Called ONCE per respawn / match start,
// never periodically (deleting weapons repeatedly can trigger the mod's own re-give code).
static void EnforceWeapons(CBasePlayer *p)
{
	if (!p || !p->IsAlive() || g_inSpawn)
		return;

	const char *list = RoleList(ENTINDEX(p->edict()));
	bool removedActive = false;
	for (int i = 0; i < MAX_ITEM_TYPES; i++)
	{
		CBasePlayerItem *it = p->m_rgpPlayerItems[i];
		while (it)
		{
			CBasePlayerItem *next = it->m_pNext; // grab before DestroyItem unlinks it
			if (!ListHasIn(list, STRING(it->pev->classname)))
			{
				if (it == p->m_pActiveItem)
					removedActive = true;
				it->DestroyItem();
			}
			it = next;
		}
	}

	if (removedActive || !p->m_pActiveItem)
	{
		for (int i = 0; i < DUEL_NUM_WEAPONS; i++)
		{
			const char *c = g_Duel1v1Weapons[i].classname;
			if (ListHasIn(list, c) && p->HasNamedPlayerItem(c))
			{
				p->SelectItem(c);
				break;
			}
		}
	}
}

static void BeginGod(CBasePlayer *p)
{
	const float t = CF("ss_1v1_godtime");
	if (t <= 0.0f)
		return;
	const int idx = ENTINDEX(p->edict());
	g_godUntil[idx] = gpGlobals->time + t;
	p->pev->takedamage = DAMAGE_NO;
	p->pev->renderfx   = kRenderFxGlowShell;
	p->pev->renderamt  = 12;
	p->pev->rendercolor = Vector(90, 160, 255);
}

static void EndGod(CBasePlayer *p)
{
	const int idx = ENTINDEX(p->edict());
	g_godUntil[idx] = 0;
	if (p->IsAlive())
		p->pev->takedamage = DAMAGE_AIM;
	p->pev->renderfx  = kRenderFxNone;
	p->pev->renderamt = 255;
	p->pev->rendercolor = Vector(0, 0, 0);
}

// Sound at the position where the player just spawned (3D, everybody nearby hears it).
static void PlaySpawnSound(CBasePlayer *p)
{
	if (!g_sndName[0])
		return;
	const float vol = CF("ss_1v1_spawnsound_vol");
	if (vol <= 0.0f)
		return;
	EMIT_SOUND_DYN(p->edict(), CHAN_AUTO, g_sndName, vol > 1.0f ? 1.0f : vol, ATTN_NORM, 0, PITCH_NORM);
}

// Respawn a duelist. at == NULL uses a normal map spawn point.
static void DoRespawn(CBasePlayer *p, const Vector *at)
{
	const int idx = ENTINDEX(p->edict());
	CBasePlayer *pe = PlayerByIdx(EnemyOf(idx));

	if (g_cvDebug.value > 0)
		ALERT(at_console, "[1v1] DoRespawn idx %d: entities before %d\n", idx, NUMBER_OF_ENTITIES());

	g_inSpawn = true;      // the mod's PlayerSpawn may give default items, let it (we wipe them next)
	p->Spawn();
	g_inSpawn = false;

	p->pev->renderfx  = kRenderFxNone;
	p->pev->takedamage = DAMAGE_AIM;
	GiveLoadout(p);

	if (at)
	{
		UTIL_SetOrigin(p->pev, *at);
		p->pev->velocity = Vector(0, 0, 0);
		if (pe)
		{
			Vector d = pe->pev->origin - *at;
			Vector a = UTIL_VecToAngles(d);
			a.x = 0;
			a.z = 0;
			p->pev->angles  = a;
			p->pev->v_angle = a;
			p->pev->fixangle = true;
		}
	}
	EnforceWeapons(p);
	PlaySpawnSound(p);

	g_choosing[idx]  = false;
	g_respawnAt[idx] = 0;
	g_godUntil[idx]  = 0;

	if (g_cvDebug.value > 0)
		ALERT(at_console, "[1v1] DoRespawn idx %d: entities after %d\n", idx, NUMBER_OF_ENTITIES());
}

static bool InHazardTrigger(const Vector &p)
{
	static const char *kClasses[] = { "trigger_hurt", "trigger_teleport", "trigger_changelevel", NULL };
	for (int i = 0; kClasses[i]; i++)
	{
		edict_t *pent = FIND_ENTITY_BY_CLASSNAME(NULL, kClasses[i]);
		while (!FNullEnt(pent))
		{
			const Vector &mn = pent->v.absmin;
			const Vector &mx = pent->v.absmax;
			if (p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y &&
			    p.z + 36.0f >= mn.z && p.z - 36.0f <= mx.z)
				return true;
			pent = FIND_ENTITY_BY_CLASSNAME(pent, kClasses[i]);
		}
	}
	return false;
}

// Find a free standing position at (want.x, want.y): hull trace down from refZ + 72 to refZ - zrange,
// then check the hull fits (players included), no lava/slime and no hazard trigger.
static bool StandableAt(const Vector &want, float refZ, float zrange, Vector &out)
{
	TraceResult tr;
	UTIL_TraceHull(Vector(want.x, want.y, refZ + 72.0f), Vector(want.x, want.y, refZ - zrange),
	               ignore_monsters, human_hull, NULL, &tr);
	if (tr.fStartSolid || tr.fAllSolid || tr.flFraction >= 1.0f)
		return false;

	out = tr.vecEndPos;
	out.z += 1.0f;

	UTIL_TraceHull(out, out, dont_ignore_monsters, human_hull, NULL, &tr);
	if (tr.fStartSolid || tr.fAllSolid)
		return false;

	const int c = POINT_CONTENTS(out);
	if (c == CONTENTS_LAVA || c == CONTENTS_SLIME)
		return false;
	return !InHazardTrigger(out);
}

// Validate a wanted spawn position (duel mode). The client only SUGGESTS x/y; the server decides z.
static bool CheckSpot(int idx, const Vector &want, Vector &out, const char **why)
{
	CBasePlayer *pe = PlayerByIdx(EnemyOf(idx));
	if (!pe) { *why = "No opponent"; return false; }

	const Vector eo = pe->pev->origin;
	const float dx = want.x - eo.x;
	const float dy = want.y - eo.y;
	const float d  = (float)sqrt(dx * dx + dy * dy);

	if (d < CF("ss_1v1_respawn_min")) { *why = "Too close to the enemy"; return false; }
	if (d > CF("ss_1v1_respawn_max")) { *why = "Too far from the enemy";  return false; }

	if (!StandableAt(want, eo.z, CF("ss_1v1_respawn_zrange"), out))
	{
		*why = "Can't respawn there";
		return false;
	}

	if (CF("ss_1v1_los") == 0)
	{
		TraceResult tr;
		UTIL_TraceLine(eo + pe->pev->view_ofs, out + Vector(0, 0, 17), ignore_monsters, pe->edict(), &tr);
		if (tr.flFraction >= 1.0f) { *why = "Pick a spot out of the enemy's sight"; return false; }
	}
	return true;
}

// Nearest safe standing spot around pos (used by the hide & seek auto respawn at the death location).
static bool SafeSpotNear(const Vector &pos, Vector &out)
{
	if (StandableAt(pos, pos.z, 96.0f, out))
		return true;
	static const float kR[] = { 48.0f, 96.0f, 160.0f, 256.0f };
	for (int r = 0; r < 4; r++)
	{
		for (int i = 0; i < 8; i++)
		{
			const float a = i * 0.7853982f;
			const Vector w(pos.x + (float)cos(a) * kR[r], pos.y + (float)sin(a) * kR[r], pos.z);
			if (StandableAt(w, pos.z, 96.0f, out))
				return true;
		}
	}
	return false;
}

// Random valid spot in the ring (used on pick timeout). Falls back to a map spawn.
static void AutoPick(CBasePlayer *p)
{
	const int idx = ENTINDEX(p->edict());
	CBasePlayer *pe = PlayerByIdx(EnemyOf(idx));
	if (pe)
	{
		const float minR = CF("ss_1v1_respawn_min");
		const float maxR = CF("ss_1v1_respawn_max");
		for (int i = 0; i < 64; i++)
		{
			const float a = RANDOM_FLOAT(0.0f, 6.2831853f);
			const float r = RANDOM_FLOAT(minR, maxR);
			const Vector want(pe->pev->origin.x + (float)cos(a) * r,
			                  pe->pev->origin.y + (float)sin(a) * r,
			                  pe->pev->origin.z);
			Vector out;
			const char *why = "";
			if (CheckSpot(idx, want, out, &why))
			{
				DoRespawn(p, &out);
				BeginGod(p);
				return;
			}
		}
	}
	DoRespawn(p, NULL);
	BeginGod(p);
}

// Hide & seek: automatic respawn at (or next to) the place where the player died.
static void HnsRespawn(CBasePlayer *p)
{
	const int idx = ENTINDEX(p->edict());
	Vector out;
	if (SafeSpotNear(g_deathPos[idx], out))
		DoRespawn(p, &out);
	else
		DoRespawn(p, NULL);
	BeginGod(p);
}

// ------------------------------------------------------------ match flow ---
static void ClearMatchData(void)
{
	for (int i = 0; i < 33; i++)
	{
		g_ready[i] = false;
		g_score[i] = 0;
		g_choosing[i] = false;
		g_godUntil[i] = 0;
		g_camValid[i] = false;
		g_camTime[i]  = 0;
		g_hnsTime[i]  = 0;
		g_respawnAt[i] = 0;
		g_applyRole[i] = false;
	}
	g_winner = 0;
	g_hider = 0;
}

static void ReleaseDuelists(void)
{
	for (int k = 0; k < 2; k++)
	{
		CBasePlayer *p = PlayerByIdx(g_duelist[k]);
		if (!p) continue;
		p->EnableControl(true);
		EndGod(p);
	}
}

static void ResetAll(void)
{
	ReleaseDuelists();
	ClearMatchData();
	g_duelist[0] = g_duelist[1] = 0;
	g_state = DUEL_OFF;
}

static void AbortToWaiting(void)
{
	ReleaseDuelists();
	for (int i = 0; i < 33; i++)
	{
		g_ready[i] = false;
		g_choosing[i] = false;
		g_respawnAt[i] = 0;
		g_applyRole[i] = false;
	}
	g_state = DUEL_WAITING;
}

static void PickDuelists(void)
{
	int n = 0, ids[2] = { 0, 0 };
	for (int i = 1; i <= gpGlobals->maxClients && i <= 32; i++)
	{
		if (PlayerByIdx(i))
		{
			if (n < 2) ids[n++] = i;
		}
		else
		{
			g_ready[i] = false;
		}
	}
	g_duelist[0] = ids[0];
	g_duelist[1] = ids[1];
}

static void StartCountdown(void)
{
	g_state = DUEL_COUNTDOWN;
	g_winner = 0;
	g_stateEnd = gpGlobals->time + CF("ss_1v1_countdown");

	if (HnsMode())
		g_hider = RANDOM_LONG(0, 1) ? g_duelist[0] : g_duelist[1];

	for (int k = 0; k < 2; k++)
	{
		const int idx = g_duelist[k];
		g_score[idx] = 0;
		g_ready[idx] = false;
		g_respawnAt[idx] = 0;
		g_hnsTime[idx] = CF("ss_1v1_hns_time");
		CBasePlayer *p = PlayerByIdx(idx);
		if (!p) continue;
		DoRespawn(p, NULL);
		p->EnableControl(false);
	}
}

static void StartLive(void)
{
	g_state = DUEL_LIVE;
	g_matchStart = gpGlobals->time;
	g_lastThink = gpGlobals->time;
	for (int k = 0; k < 2; k++)
	{
		const int idx = g_duelist[k];
		CBasePlayer *p = PlayerByIdx(idx);
		if (!p) continue;
		p->EnableControl(true);
		EnforceWeapons(p);
		BeginGod(p);
		if (HnsMode())
			ClientPrint(p->pev, HUD_PRINTCENTER, idx == g_hider ? "You are the HIDER - survive!" : "You are the SEEKER - find and kill the hider");
	}
}

// forcedWinner > 0: that edict index wins (hide & seek clock reached 0). Otherwise decided by score / clocks.
static void EndMatch(int forcedWinner)
{
	const int a = g_duelist[0], b = g_duelist[1];

	if (forcedWinner > 0)
		g_winner = forcedWinner;
	else if (HnsMode())
		g_winner = g_hnsTime[a] < g_hnsTime[b] ? a : (g_hnsTime[b] < g_hnsTime[a] ? b : 0);
	else
		g_winner = g_score[a] > g_score[b] ? a : (g_score[b] > g_score[a] ? b : 0);

	g_state = DUEL_ENDED;
	g_stateEnd = gpGlobals->time + 8.0f;
	g_choosing[a] = g_choosing[b] = false;
	g_respawnAt[a] = g_respawnAt[b] = 0;

	CBasePlayer *pw = PlayerByIdx(g_winner);
	if (!pw)
		UTIL_ClientPrintAll(HUD_PRINTTALK, UTIL_VarArgs("[1v1] Draw\n"));
	else if (HnsMode())
		UTIL_ClientPrintAll(HUD_PRINTTALK, UTIL_VarArgs("[1v1] %s wins - the hider clock ran out\n", STRING(pw->pev->netname)));
	else
		UTIL_ClientPrintAll(HUD_PRINTTALK, UTIL_VarArgs("[1v1] %s wins %d - %d\n", STRING(pw->pev->netname), g_score[g_winner], g_score[EnemyOf(g_winner)]));
}

void Duel1v1_PlayerKilled(CBasePlayer *victim, entvars_t *pevAttacker)
{
	if (!victim || !Duel1v1_Active() || g_state != DUEL_LIVE)
		return;
	const int v = ENTINDEX(victim->edict());
	if (!IsDuelist(v))
		return;
	const int e = EnemyOf(v);

	g_godUntil[v] = 0;
	g_camValid[v] = false;

	if (HnsMode())
	{
		// Auto respawn where the player died, after the cooldown. No spot picking in this mode.
		g_deathPos[v]  = victim->pev->origin;
		g_respawnAt[v] = gpGlobals->time + CF("ss_1v1_hns_respawn");
		if (g_respawnAt[v] <= gpGlobals->time)
			g_respawnAt[v] = gpGlobals->time + 0.05f;

		// Only a kill by the other player counts: the seeker killing the hider swaps the roles.
		CBaseEntity *pAtt = pevAttacker ? CBaseEntity::Instance(pevAttacker) : NULL;
		const bool byEnemy = pAtt && pAtt->IsPlayer() && ENTINDEX(pAtt->edict()) == e;
		if (v == g_hider && byEnemy)
		{
			g_hider = e;
			g_applyRole[e] = true; // strip the new hider's knife next frame (not inside the damage call stack)
			CBasePlayer *pk = PlayerByIdx(e);
			if (pk) ClientPrint(pk->pev, HUD_PRINTCENTER, "You are now the HIDER");
			ClientPrint(victim->pev, HUD_PRINTCENTER, "You are now the SEEKER");
		}
		return;
	}

	// Duel: the opponent scores on any death (kill, suicide, world). If you want suicides to
	// cost a point instead, change it here using pevAttacker.
	if (e > 0 && g_score[e] < 255)
		g_score[e]++;

	g_choosing[v]  = true;
	g_pickStart[v] = gpGlobals->time;
}

// ------------------------------------------------------------- messaging ---
void Duel1v1_LinkUserMessages(void)
{
	gmsgDuelState = REG_USER_MSG("DuelState", -1);
	gmsgDuelCfg   = REG_USER_MSG("DuelCfg", -1);
}

static void SendCfg(void)
{
	if (!gmsgDuelCfg) return;
	int mask = 0;
	for (int i = 0; i < DUEL_NUM_WEAPONS; i++)
		if (ListHas(g_Duel1v1Weapons[i].classname))
			mask |= (1 << i);

	MESSAGE_BEGIN(MSG_ALL, gmsgDuelCfg);
		WRITE_BYTE(DUEL_NUM_OPTS);
		for (int i = 0; i < DUEL_NUM_OPTS; i++)
			WRITE_LONG((int)(CF(g_Duel1v1Opts[i].cvar) * 1000.0f));
		WRITE_LONG(mask);
	MESSAGE_END();
}

static void SendState(CBasePlayer *p, float now)
{
	const int idx = ENTINDEX(p->edict());
	const bool duelist = IsDuelist(idx);
	const int  e = duelist ? EnemyOf(idx) : 0;
	CBasePlayer *pe = PlayerByIdx(e);
	const bool hns = HnsMode();

	int secLeft = -1;
	if (g_state == DUEL_COUNTDOWN)
	{
		secLeft = (int)ceil(g_stateEnd - now);
		if (secLeft < 0) secLeft = 0;
	}
	else if (g_state == DUEL_LIVE && CF("ss_1v1_timelimit") > 0)
	{
		secLeft = (int)ceil(g_matchStart + CF("ss_1v1_timelimit") * 60.0f - now);
		if (secLeft < 0) secLeft = 0;
	}

	int flags = 0;
	if (g_ready[idx])               flags |= 1;
	if (e && g_ready[e])            flags |= 2;
	if (g_choosing[idx])            flags |= 4;   // duel mode only: client switches to the top-down camera
	if (duelist)                    flags |= 8;

	int pickTenths = 0;
	if (g_choosing[idx])
	{
		pickTenths = (int)((CF("ss_1v1_pick_timeout") - (now - g_pickStart[idx])) * 10.0f);
		if (pickTenths < 0) pickTenths = 0;
	}

	// 0 = none, 1 = you won, 2 = enemy won, 3 = draw
	int winnerRel = 0;
	if (g_state == DUEL_ENDED)
		winnerRel = (g_winner == 0) ? 3 : (g_winner == idx ? 1 : 2);

	// hide & seek extras
	int hiderRel = 0, myT = 0, enT = 0, respTenths = 0;
	if (hns && duelist)
	{
		hiderRel = (g_hider == idx) ? 1 : 2;
		myT = (int)(g_hnsTime[idx] * 10.0f);
		enT = e ? (int)(g_hnsTime[e] * 10.0f) : 0;
		if (g_respawnAt[idx] > 0.0f)
		{
			respTenths = (int)((g_respawnAt[idx] - now) * 10.0f);
			if (respTenths < 0) respTenths = 0;
		}
	}

	Vector eo = pe ? pe->pev->origin : Vector(0, 0, 0);

	MESSAGE_BEGIN(MSG_ONE_UNRELIABLE, gmsgDuelState, NULL, p->pev);
		WRITE_BYTE(g_state);
		WRITE_BYTE(duelist ? g_score[idx] : g_score[g_duelist[0]]);
		WRITE_BYTE(duelist ? (e ? g_score[e] : 0) : g_score[g_duelist[1]]);
		WRITE_BYTE((int)CF("ss_1v1_killlimit"));
		WRITE_SHORT(secLeft);
		WRITE_BYTE(flags);
		WRITE_BYTE(e);
		WRITE_BYTE(winnerRel);
		WRITE_SHORT(pickTenths);
		WRITE_LONG((int)(eo.x * 10.0f));
		WRITE_LONG((int)(eo.y * 10.0f));
		WRITE_LONG((int)(eo.z * 10.0f));
		// ---- v2 fields (client must read them in this order, after the three coordinates) ----
		WRITE_BYTE(hns ? 1 : 0);
		WRITE_BYTE(hiderRel);
		WRITE_SHORT(myT);
		WRITE_SHORT(enT);
		WRITE_SHORT(respTenths);
	MESSAGE_END();
}

// ----------------------------------------------------------------- think ---
void Duel1v1_Init(void)
{
	for (int i = 0; i < DUEL_NUM_OPTS; i++)
	{
		if (CVAR_GET_POINTER(g_Duel1v1Opts[i].cvar))
			continue; // already exists (your own mechanic cvars), do not re-register
		g_cvars[i].name   = (char *)g_Duel1v1Opts[i].cvar;
		g_cvars[i].string = (char *)g_Duel1v1Opts[i].defval;
		g_cvars[i].flags  = FCVAR_SERVER;
		g_cvars[i].value  = 0;
		g_cvars[i].next   = NULL;
		CVAR_REGISTER(&g_cvars[i]);
	}
	CVAR_REGISTER(&g_cvWeapons);
	CVAR_REGISTER(&g_cvSound);
	CVAR_REGISTER(&g_cvDebug);
	CVAR_REGISTER(&g_cvHiderWeapons);
	ClearMatchData();
}

void Duel1v1_Think(void)
{
	const float now = gpGlobals->time;

	if (g_cfgDirty || now >= g_nextCfg)
	{
		SendCfg();
		g_cfgDirty = false;
		g_nextCfg = now + 2.0f;
	}

	if (!Duel1v1_Active())
	{
		if (g_state != DUEL_OFF)
			ResetAll();
		return;
	}

	// Safety net: leave 1v1 mode before the engine runs out of edicts.
	const int ents = NUMBER_OF_ENTITIES();
	if (g_cvDebug.value > 0)
	{
		static float s_nextLog = 0;
		if (g_state != DUEL_WAITING && now >= s_nextLog)
		{
			s_nextLog = now + 0.5f;
			ALERT(at_console, "[1v1] state %d, entities %d/%d\n", g_state, ents, gpGlobals->maxEntities);
		}
	}
	if (ents > gpGlobals->maxEntities - 100)
	{
		ALERT(at_console, "[1v1] entity count critical (%d), turning 1v1 mode off\n", ents);
		CVAR_SET_FLOAT("ss_1v1", 0);
		return;
	}

	if (g_state == DUEL_OFF)
	{
		ClearMatchData();
		g_state = DUEL_WAITING;
	}

	float dt = now - g_lastThink;
	g_lastThink = now;
	if (dt < 0.0f) dt = 0.0f;
	if (dt > 0.25f) dt = 0.25f;

	if (g_state == DUEL_WAITING)
		PickDuelists();
	else if (g_state != DUEL_ENDED && (!PlayerByIdx(g_duelist[0]) || !PlayerByIdx(g_duelist[1])))
		AbortToWaiting(); // someone left mid match

	switch (g_state)
	{
	case DUEL_WAITING:
		if (g_duelist[0] && g_duelist[1] && g_ready[g_duelist[0]] && g_ready[g_duelist[1]])
			StartCountdown();
		break;

	case DUEL_COUNTDOWN:
		if (now >= g_stateEnd)
			StartLive();
		break;

	case DUEL_LIVE:
	{
		const int a = g_duelist[0], b = g_duelist[1];
		const float tl = CF("ss_1v1_timelimit");

		if (HnsMode())
		{
			// Only the hider's clock ticks, and only while the hider is alive.
			CBasePlayer *h = PlayerByIdx(g_hider);
			if (h && h->IsAlive())
			{
				g_hnsTime[g_hider] -= dt;
				if (g_hnsTime[g_hider] <= 0.0f)
				{
					g_hnsTime[g_hider] = 0.0f;
					EndMatch(g_hider);
					break;
				}
			}

			// automatic respawns at the death location
			for (int k = 0; k < 2; k++)
			{
				const int idx = g_duelist[k];
				CBasePlayer *p = PlayerByIdx(idx);
				if (p && g_respawnAt[idx] > 0.0f && now >= g_respawnAt[idx])
					HnsRespawn(p);
			}
		}
		else
		{
			const int kl = (int)CF("ss_1v1_killlimit");
			if (g_score[a] >= kl || g_score[b] >= kl)
			{
				EndMatch(0);
				break;
			}

			for (int k = 0; k < 2; k++)
			{
				const int idx = g_duelist[k];
				CBasePlayer *p = PlayerByIdx(idx);
				if (!p) continue;
				if (g_choosing[idx] && now - g_pickStart[idx] >= CF("ss_1v1_pick_timeout"))
					AutoPick(p);
			}
		}

		if (tl > 0 && now >= g_matchStart + tl * 60.0f)
			EndMatch(0);
		break;
	}

	case DUEL_ENDED:
		if (now >= g_stateEnd)
			AbortToWaiting();
		break;
	}

	// role swap: apply the new loadout (new hider loses the melee weapon and selects the camera)
	if (g_state == DUEL_LIVE)
	{
		for (int k = 0; k < 2; k++)
		{
			const int idx = g_duelist[k];
			CBasePlayer *p = PlayerByIdx(idx);
			if (p && g_applyRole[idx])
			{
				g_applyRole[idx] = false;
				EnforceWeapons(p);
			}
		}
	}

	// godmode expiry / end on fire
	if (g_state == DUEL_LIVE)
	{
		for (int k = 0; k < 2; k++)
		{
			const int idx = g_duelist[k];
			CBasePlayer *p = PlayerByIdx(idx);
			if (!p || g_godUntil[idx] <= 0) continue;
			const bool fired = CF("ss_1v1_god_endonfire") != 0 && (p->pev->button & (IN_ATTACK | IN_ATTACK2));
			if (now >= g_godUntil[idx] || fired)
				EndGod(p);
		}
	}

	// state to every client (fast while someone is choosing so the ring follows the enemy)
	if (now >= g_nextState)
	{
		const bool anyChoosing = g_choosing[g_duelist[0]] || g_choosing[g_duelist[1]];
		g_nextState = now + (anyChoosing ? 0.05f : 0.1f);
		for (int i = 1; i <= gpGlobals->maxClients && i <= 32; i++)
		{
			CBasePlayer *p = PlayerByIdx(i);
			if (p) SendState(p, now);
		}
	}
}

// --------------------------------------------------------- client commands ---
bool Duel1v1_ClientCommand(CBasePlayer *p, const char *cmd)
{
	if (strncmp(cmd, "1v1_", 4) != 0)
		return false;

	const int idx  = ENTINDEX(p->edict());
	const bool host = (idx == 1); // listen server host. On a dedicated server swap this for your own admin check.
	const int argc = CMD_ARGC();
	const float now = gpGlobals->time;
	const bool locked = (g_state == DUEL_COUNTDOWN || g_state == DUEL_LIVE);

	if (!strcmp(cmd, "1v1_ready"))
	{
		if (!Duel1v1_Active() || g_state != DUEL_WAITING || !IsDuelist(idx))
			return true;
		g_ready[idx] = !g_ready[idx];
		return true;
	}

	if (!strcmp(cmd, "1v1_stop"))
	{
		if (host && Duel1v1_Active() && g_state != DUEL_WAITING)
			AbortToWaiting();
		return true;
	}

	if (!strcmp(cmd, "1v1_set"))
	{
		if (!host) { ClientPrint(p->pev, HUD_PRINTTALK, "[1v1] Only the host can change settings\n"); return true; }
		if (argc < 3) return true;
		const Duel1v1Opt *o = FindOpt(CMD_ARGV(1));
		if (!o) return true;
		if (locked && strcmp(o->cvar, "ss_1v1")) { ClientPrint(p->pev, HUD_PRINTTALK, "[1v1] Settings are locked during a match\n"); return true; }

		float v = (float)atof(CMD_ARGV(2));
		if (v < o->minv) v = o->minv;
		if (v > o->maxv) v = o->maxv;
		if (o->type == DOPT_BOOL) v = (v != 0.0f) ? 1.0f : 0.0f;
		if (o->type == DOPT_INT)  v = (float)floor(v + 0.5f);
		CVAR_SET_FLOAT(o->cvar, v);
		FixRespawnBounds();
		g_cfgDirty = true;
		return true;
	}

	if (!strcmp(cmd, "1v1_weapon"))
	{
		if (!host || locked || argc < 3) return true;
		bool known = false;
		for (int i = 0; i < DUEL_NUM_WEAPONS; i++)
			if (!strcmp(g_Duel1v1Weapons[i].classname, CMD_ARGV(1))) known = true;
		if (!known) return true;
		SetWeaponAllowed(CMD_ARGV(1), atoi(CMD_ARGV(2)) != 0);
		g_cfgDirty = true;
		return true;
	}

	if (!strcmp(cmd, "1v1_preset"))
	{
		if (!host || locked || argc < 2) return true;
		const char *name = CMD_ARGV(1);
		char safe[32];
		int n = 0;
		for (; name[n] && n < 31; n++)
		{
			const char c = name[n];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
				return true;
			safe[n] = c;
		}
		safe[n] = 0;
		if (!n) return true;
		SERVER_COMMAND(UTIL_VarArgs("exec 1v1_%s.cfg\n", safe));
		g_cfgDirty = true;
		return true;
	}

	if (!strcmp(cmd, "1v1_cam"))
	{
		if (argc < 4 || !IsPicking(idx)) return true;
		g_camOrg[idx]  = Vector((float)atof(CMD_ARGV(1)), (float)atof(CMD_ARGV(2)), (float)atof(CMD_ARGV(3)));
		g_camTime[idx] = now;
		g_camValid[idx] = true;
		return true;
	}

	if (!strcmp(cmd, "1v1_respawn"))
	{
		if (argc < 3 || !IsPicking(idx)) return true;
		const Vector want((float)atof(CMD_ARGV(1)), (float)atof(CMD_ARGV(2)), 0.0f);
		Vector out;
		const char *why = "";
		if (!CheckSpot(idx, want, out, &why))
		{
			ClientPrint(p->pev, HUD_PRINTCENTER, why);
			return true;
		}
		DoRespawn(p, &out);
		BeginGod(p);
		return true;
	}

	return true; // unknown 1v1_ command, swallow it
}
