// wave.cpp - WAVE gamemode (server side)
//
// Flow:
//   LOBBY  (ready up with F4, vote difficulty with 7/8/9/0; starts when every
//           connected player is ready, winning vote = difficulty)
//   PREP   (countdown) <-> ACTIVE (grunts out) ... repeated per wave
//   dead players become spectators; when nobody is alive -> game over
//   STATS  (end stats on screen) -> RETRY (F4 retry / F5 leave on the client)
//   retry  = reload the map and restart directly with the same difficulty
//
// Start paths:
//   ss_wave_start              open the ready-up lobby
//   ss_wave_start <difficulty> skip the lobby (testing)
//   cvar ss_wave_auto 1        open the lobby automatically on every map load
//
// Formulas (wave numbers start at 1):
//   enemy count  = base(difficulty) + (wave - 1) / 2        -> +1 every 2 waves
//   tier         = (wave - 1) / 5                           -> steps up every 5 waves
//   enemy HP     = grunt HP * hpMul(difficulty)  * 1.20 ^ tier
//   enemy damage = normal   * dmgMul(difficulty) * 1.15 ^ tier   (capped at x4)
//
// Enemies are stock monster_human_grunt entities. Kills are detected by
// polling the tracked entities, so no hook in CBaseMonster::Killed is needed.
// Points come from the existing points system (points.h).

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "schedule.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "UserMessages.h"
#include "points.h"
#include "wave.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAVE_MAX_ALIVE 24		 // max grunts alive at once (extra ones queue up)
#define WAVE_START_DELAY 3.0f	 // seconds before wave 1
#define WAVE_PAUSE 5.0f			 // seconds between waves
#define WAVE_ALERT_INTERVAL 4.0f // how often idle grunts are pointed at a player
#define WAVE_NET_INTERVAL 1.0f	 // heartbeat for the client HUD
#define WAVE_STATS_HOLD 8.0f	 // seconds the end stats stay on screen
#define WAVE_STATS_TIME 9.5f	 // stats + fade, then the retry menu appears

#define WAVE_TIER_LENGTH 5
#define WAVE_HP_PER_TIER 1.20f
#define WAVE_DMG_PER_TIER 1.15f
#define WAVE_DMG_SCALE_MAX 4.0f

// grunts spawn on a ring around a living player
#define WAVE_SPAWN_MIN_DIST 300.0f // units from the player
#define WAVE_SPAWN_MAX_DIST 500.0f
#define WAVE_SPAWN_MAX_DROP 192.0f // how far down to look for a floor
#define WAVE_SPAWN_MAX_DZ 128.0f   // max height difference to the player
#define WAVE_SPAWN_ATTEMPTS 40	   // random tries before using the map spawns

#define WAVE_MAX_PLAYERS 32

// session state. 0-4 are also what the client HUD understands.
enum
{
	WAVE_NET_OFF = 0,
	WAVE_NET_PREP,	 // countdown before a wave
	WAVE_NET_ACTIVE, // enemies out
	WAVE_NET_LOBBY,	 // ready up + difficulty vote
	WAVE_NET_RETRY,	 // retry / leave menu
	WAVE_NET_STATS	 // server only: end stats showing (sent to clients as OFF)
};

struct wave_diff_t
{
	const char* name;
	int baseCount; // grunts in wave 1
	float hpMul;
	float dmgMul;
	float spawnInterval; // seconds between two grunt spawns
};

static const wave_diff_t g_WaveDiff[WAVE_DIFF_COUNT] =
	{
		{"easy", 3, 0.75f, 0.75f, 1.5f},
		{"normal", 4, 1.00f, 1.00f, 1.0f},
		{"hard", 5, 1.25f, 1.25f, 0.7f},
		{"extreme", 6, 1.50f, 1.50f, 0.4f},
};

struct wave_state_t
{
	int state;
	int difficulty;
	int wave; // current (or upcoming, during prep) wave number
	int wavesCleared;
	int kills;
	int toSpawn; // grunts of this wave still to spawn
	int peakPoints;

	float startTime;
	float nextWaveTime; // also used as the "retry menu appears at" time in STATS
	float nextSpawnTime;
	float nextAlertTime;
	float nextNetTime;

	float hpScale;
	float dmgScale;

	int sentState;
	int sentWave;
	int sentRemaining;
	int sentReady;
	int sentTotal;
	int sentCheats;

	// true once the first wave has begun (mobs spawning): from then on there
	// are no respawns, dead players spectate and the gameplay cvars are forced
	bool locked;

	// lobby / retry, indexed by player entity index (1..32)
	bool readyFlag[WAVE_MAX_PLAYERS + 1];
	int voteFor[WAVE_MAX_PLAYERS + 1]; // -1 = no vote

	bool used[WAVE_MAX_ALIVE];
	EHANDLE hEnemy[WAVE_MAX_ALIVE];
};

static wave_state_t g_Wave;
static int g_WaveHighScore[WAVE_DIFF_COUNT];
static bool g_WaveHighLoaded = false;

// cvars we override while a session runs (<0 = not overridden)
static float g_WavePrevAllowMonsters = -1.0f;
static float g_WavePrevAllowSpec = -1.0f;

static float g_WaveLastTime = 0.0f; // detects a new level (time restarts)
static bool g_WaveAutoArmed = true; // may auto-open the lobby on this level
static int g_WavePendingDiff = -1;	// >=0: start this difficulty after a retry reload

static cvar_t g_cvWaveAuto = {"ss_wave_auto", "0", FCVAR_SERVER};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static bool Wave_StrEqNoCase(const char* a, const char* b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return false;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

// a connected human (alive, dead or spectating)
static bool Wave_IsConnected(CBaseEntity* pEnt)
{
	if (!pEnt || !pEnt->IsPlayer())
		return false;
	return (pEnt->pev->flags & FL_CLIENT) != 0;
}

// a human who is still in the fight
static bool Wave_PlayerAlive(CBaseEntity* pEnt)
{
	if (!Wave_IsConnected(pEnt))
		return false;
	if ((pEnt->pev->flags & FL_SPECTATOR) != 0)
		return false;
	// SOLID_NOT also filters out observers and disconnected players
	return pEnt->IsAlive() && pEnt->pev->solid != SOLID_NOT;
}

static bool Wave_AnyPlayerAlive()
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (Wave_PlayerAlive(UTIL_PlayerByIndex(i)))
			return true;
	}
	return false;
}

static CBaseEntity* Wave_NearestPlayer(const Vector& origin)
{
	CBaseEntity* pBest = NULL;
	float flBest = 999999.0f;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		if (!Wave_PlayerAlive(pEnt))
			continue;
		float d = (pEnt->pev->origin - origin).Length();
		if (d < flBest)
		{
			flBest = d;
			pBest = pEnt;
		}
	}
	return pBest;
}

// points are reset to 0 when the player dies, so remember the best value seen
static void Wave_SamplePoints()
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		if (!pEnt || !pEnt->IsPlayer())
			continue;
		CBasePlayer* pPlayer = (CBasePlayer*)pEnt;
		if (pPlayer->m_iPoints > g_Wave.peakPoints)
			g_Wave.peakPoints = pPlayer->m_iPoints;
	}
}

static int Wave_EnemyCount(int wave)
{
	return g_WaveDiff[g_Wave.difficulty].baseCount + (wave - 1) / 2;
}

static int Wave_AliveCount()
{
	int n = 0;
	for (int i = 0; i < WAVE_MAX_ALIVE; i++)
	{
		if (g_Wave.used[i])
			n++;
	}
	return n;
}

static bool Wave_IsEnemyDead(CBaseEntity* pEnt)
{
	if (!pEnt)
		return true;
	if (pEnt->pev->deadflag != DEAD_NO)
		return true;
	CBaseMonster* pMon = pEnt->MyMonsterPointer();
	if (pMon && pMon->HasMemory(bits_MEMORY_KILLED))
		return true;
	return false;
}

static int Wave_SlotOf(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return 0;
	int idx = ENTINDEX(pPlayer->edict());
	if (idx < 1 || idx > WAVE_MAX_PLAYERS)
		return 0;
	return idx;
}

// ---------------------------------------------------------------------------
// cvars forced on while a session runs
// ---------------------------------------------------------------------------

static void Wave_ApplyCvars()
{
	// multiplayer gamerules delete monsters unless mp_allowmonsters is 1
	if (g_WavePrevAllowMonsters < 0.0f)
		g_WavePrevAllowMonsters = CVAR_GET_FLOAT("mp_allowmonsters");
	CVAR_SET_FLOAT("mp_allowmonsters", 1);

	// dead players become spectators
	if (g_WavePrevAllowSpec < 0.0f)
		g_WavePrevAllowSpec = CVAR_GET_FLOAT("allow_spectators");
	CVAR_SET_FLOAT("allow_spectators", 1);
}

static void Wave_RestoreCvars()
{
	if (g_WavePrevAllowMonsters >= 0.0f)
	{
		CVAR_SET_FLOAT("mp_allowmonsters", g_WavePrevAllowMonsters);
		g_WavePrevAllowMonsters = -1.0f;
	}
	if (g_WavePrevAllowSpec >= 0.0f)
	{
		CVAR_SET_FLOAT("allow_spectators", g_WavePrevAllowSpec);
		g_WavePrevAllowSpec = -1.0f;
	}
}

// ---------------------------------------------------------------------------
// high score (one value per difficulty, <gamedir>/wave_highscore.txt)
// ---------------------------------------------------------------------------

static void Wave_HighScorePath(char* out)
{
	char dir[256];
	dir[0] = 0;
	GET_GAME_DIR(dir);
	sprintf(out, "%s/wave_highscore.txt", dir);
}

static void Wave_LoadHighScores()
{
	if (g_WaveHighLoaded)
		return;
	g_WaveHighLoaded = true;

	for (int i = 0; i < WAVE_DIFF_COUNT; i++)
		g_WaveHighScore[i] = 0;

	char path[300];
	Wave_HighScorePath(path);
	FILE* f = fopen(path, "r");
	if (!f)
		return;
	for (int i = 0; i < WAVE_DIFF_COUNT; i++)
	{
		int v = 0;
		if (fscanf(f, "%d", &v) == 1)
			g_WaveHighScore[i] = v;
	}
	fclose(f);
}

static void Wave_SaveHighScores()
{
	char path[300];
	Wave_HighScorePath(path);
	FILE* f = fopen(path, "w");
	if (!f)
		return;
	for (int i = 0; i < WAVE_DIFF_COUNT; i++)
		fprintf(f, "%d\n", g_WaveHighScore[i]);
	fclose(f);
}

// ---------------------------------------------------------------------------
// lobby / retry bookkeeping
// ---------------------------------------------------------------------------

static void Wave_ResetFlags()
{
	for (int i = 0; i <= WAVE_MAX_PLAYERS; i++)
	{
		g_Wave.readyFlag[i] = false;
		g_Wave.voteFor[i] = -1;
	}
}

// counts connected players, how many are ready, and the difficulty votes
static void Wave_CountReady(int& total, int& ready, int* votes)
{
	total = 0;
	ready = 0;
	if (votes)
	{
		for (int k = 0; k < WAVE_DIFF_COUNT; k++)
			votes[k] = 0;
	}

	for (int i = 1; i <= gpGlobals->maxClients && i <= WAVE_MAX_PLAYERS; i++)
	{
		if (!Wave_IsConnected(UTIL_PlayerByIndex(i)))
			continue;
		total++;
		if (g_Wave.readyFlag[i])
			ready++;
		if (votes && g_Wave.voteFor[i] >= 0 && g_Wave.voteFor[i] < WAVE_DIFF_COUNT)
			votes[g_Wave.voteFor[i]]++;
	}
}

// most votes wins, ties go to the easier difficulty, no votes = normal
static int Wave_TallyVotes()
{
	int total, ready, votes[WAVE_DIFF_COUNT];
	Wave_CountReady(total, ready, votes);

	int best = WAVE_DIFF_NORMAL;
	int bestVotes = 0;
	for (int i = 0; i < WAVE_DIFF_COUNT; i++)
	{
		if (votes[i] > bestVotes)
		{
			bestVotes = votes[i];
			best = i;
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// networking: one message per connected player (carries that player's flags)
// ---------------------------------------------------------------------------

static void Wave_Send(bool force)
{
	if (gmsgWaveState == 0)
		return;

	int remaining = 0;
	if (g_Wave.state == WAVE_NET_PREP || g_Wave.state == WAVE_NET_ACTIVE)
		remaining = g_Wave.toSpawn + Wave_AliveCount();

	const int netState = (g_Wave.state == WAVE_NET_STATS) ? WAVE_NET_OFF : g_Wave.state;

	// lobby only: tell the HUD when sv_cheats blocks the start
	const int cheats = (g_Wave.state == WAVE_NET_LOBBY && CVAR_GET_FLOAT("sv_cheats") > 0) ? 1 : 0;

	int total = 0, ready = 0, votes[WAVE_DIFF_COUNT];
	Wave_CountReady(total, ready, votes);

	if (!force && g_Wave.state == g_Wave.sentState && g_Wave.wave == g_Wave.sentWave &&
		remaining == g_Wave.sentRemaining && ready == g_Wave.sentReady &&
		total == g_Wave.sentTotal && cheats == g_Wave.sentCheats &&
		gpGlobals->time < g_Wave.nextNetTime)
		return;

	g_Wave.sentState = g_Wave.state;
	g_Wave.sentWave = g_Wave.wave;
	g_Wave.sentRemaining = remaining;
	g_Wave.sentReady = ready;
	g_Wave.sentTotal = total;
	g_Wave.sentCheats = cheats;
	g_Wave.nextNetTime = gpGlobals->time + WAVE_NET_INTERVAL;

	for (int i = 1; i <= gpGlobals->maxClients && i <= WAVE_MAX_PLAYERS; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		if (!Wave_IsConnected(pEnt))
			continue;

		MESSAGE_BEGIN(MSG_ONE, gmsgWaveState, NULL, pEnt->pev);
		WRITE_BYTE(netState);
		WRITE_SHORT(g_Wave.wave);
		WRITE_SHORT(remaining);
		WRITE_BYTE(g_Wave.difficulty);
		WRITE_BYTE(ready);
		WRITE_BYTE(total);
		WRITE_BYTE(votes[0]);
		WRITE_BYTE(votes[1]);
		WRITE_BYTE(votes[2]);
		WRITE_BYTE(votes[3]);
		WRITE_BYTE(g_Wave.readyFlag[i] ? 1 : 0);
		WRITE_BYTE(g_Wave.voteFor[i] < 0 ? 255 : g_Wave.voteFor[i]);
		WRITE_BYTE(cheats);
		MESSAGE_END();
	}
}

// ---------------------------------------------------------------------------
// enemies
// ---------------------------------------------------------------------------

static void Wave_ClearEnemies(bool removeAlive)
{
	for (int i = 0; i < WAVE_MAX_ALIVE; i++)
	{
		if (g_Wave.used[i] && removeAlive)
		{
			CBaseEntity* pEnt = g_Wave.hEnemy[i];
			if (pEnt)
				UTIL_Remove(pEnt);
		}
		g_Wave.used[i] = false;
		g_Wave.hEnemy[i] = NULL;
	}
}

// frees slots of dead/removed grunts, counts kills, returns grunts still alive
static int Wave_ReapEnemies()
{
	int alive = 0;
	for (int i = 0; i < WAVE_MAX_ALIVE; i++)
	{
		if (!g_Wave.used[i])
			continue;

		CBaseEntity* pEnt = g_Wave.hEnemy[i];
		if (Wave_IsEnemyDead(pEnt))
		{
			g_Wave.kills++;
			g_Wave.used[i] = false;
			g_Wave.hEnemy[i] = NULL;
		}
		else
		{
			alive++;
		}
	}
	return alive;
}

static int Wave_CollectSpawns(CBaseEntity** out, int max)
{
	static const char* names[] = {"info_wave_spawn", "info_player_deathmatch", "info_player_start"};

	for (int n = 0; n < 3; n++)
	{
		int count = 0;
		CBaseEntity* pSpot = NULL;
		while (count < max && (pSpot = UTIL_FindEntityByClassname(pSpot, names[n])) != NULL)
			out[count++] = pSpot;
		if (count > 0)
			return count;
	}
	return 0;
}

// fallback: map spawn points (info_wave_spawn / deathmatch / player start)
static bool Wave_SpawnEnemyAtMapSpot(int slot)
{
	CBaseEntity* spawns[64];
	int n = Wave_CollectSpawns(spawns, 64);
	if (n == 0)
		return false;

	int start = RANDOM_LONG(0, n - 1);
	for (int k = 0; k < n; k++)
	{
		CBaseEntity* pSpot = spawns[(start + k) % n];
		Vector origin = pSpot->pev->origin + Vector(0, 0, 1);

		// skip spots that are blocked (player, another grunt, geometry)
		TraceResult tr;
		UTIL_TraceHull(origin, origin, dont_ignore_monsters, human_hull, NULL, &tr);
		if (tr.fStartSolid != 0 || tr.fAllSolid != 0)
			continue;

		CBaseEntity* pEnt = CBaseEntity::Create("monster_human_grunt", origin, pSpot->pev->angles, NULL);
		if (!pEnt)
			return false;

		// HP scaling (Spawn() already set pev->health from the skill cvar)
		float hp = pEnt->pev->health * g_Wave.hpScale;
		if (hp < 1.0f)
			hp = 1.0f;
		pEnt->pev->health = hp;
		pEnt->pev->max_health = hp;

		g_Wave.hEnemy[slot] = pEnt;
		g_Wave.used[slot] = true;
		return true;
	}
	return false;
}

// Finds a safe floor position 300-500 units from a random living player.
// Rejects: walls between player and spot, no floor (void), different floor
// level, water/slime/lava, and spots where a grunt would be stuck or overlap
// another monster/player.
static bool Wave_FindSpotNearPlayer(Vector& outOrigin, Vector& outAngles)
{
	CBaseEntity* players[WAVE_MAX_PLAYERS];
	int np = 0;
	for (int i = 1; i <= gpGlobals->maxClients && np < WAVE_MAX_PLAYERS; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		if (Wave_PlayerAlive(pEnt))
			players[np++] = pEnt;
	}
	if (np == 0)
		return false;

	for (int attempt = 0; attempt < WAVE_SPAWN_ATTEMPTS; attempt++)
	{
		CBaseEntity* pPlayer = players[RANDOM_LONG(0, np - 1)];
		const Vector center = pPlayer->pev->origin;

		const float yaw = RANDOM_FLOAT(0.0f, 360.0f) * (3.14159265f / 180.0f);
		const float dist = RANDOM_FLOAT(WAVE_SPAWN_MIN_DIST, WAVE_SPAWN_MAX_DIST);
		const Vector cand = center + Vector(cosf(yaw) * dist, sinf(yaw) * dist, 0);

		TraceResult tr;

		// 1) straight path from the player must be clear: no walls in between
		UTIL_TraceHull(center, cand, ignore_monsters, human_hull, ENT(pPlayer->pev), &tr);
		if (tr.fStartSolid != 0 || tr.flFraction < 1.0f)
			continue;

		// 2) drop to the floor; no floor means void / under the map
		const Vector top = cand + Vector(0, 0, 48);
		const Vector bottom = cand - Vector(0, 0, WAVE_SPAWN_MAX_DROP);
		UTIL_TraceHull(top, bottom, ignore_monsters, human_hull, NULL, &tr);
		if (tr.fStartSolid != 0 || tr.fAllSolid != 0 || tr.flFraction >= 1.0f)
			continue;

		const Vector spot = tr.vecEndPos + Vector(0, 0, 1);

		// 3) roughly the player's level (no spawning on a far-away ledge)
		if (fabsf(spot.z - center.z) > WAVE_SPAWN_MAX_DZ)
			continue;

		// 4) open air only: no water, slime, lava or solid at feet/head
		if (UTIL_PointContents(spot) != CONTENTS_EMPTY)
			continue;
		if (UTIL_PointContents(spot + Vector(0, 0, 36)) != CONTENTS_EMPTY)
			continue;

		// 5) final fit check, also rejects overlap with monsters/players
		UTIL_TraceHull(spot, spot, dont_ignore_monsters, human_hull, NULL, &tr);
		if (tr.fStartSolid != 0 || tr.fAllSolid != 0)
			continue;

		outOrigin = spot;
		outAngles = UTIL_VecToAngles(center - spot);
		outAngles.x = 0;
		outAngles.z = 0;
		return true;
	}
	return false;
}

static bool Wave_CreateGrunt(const Vector& origin, const Vector& angles, int slot)
{
	CBaseEntity* pEnt = CBaseEntity::Create("monster_human_grunt", origin, angles, NULL);
	if (!pEnt)
		return false;

	float hp = pEnt->pev->health * g_Wave.hpScale;
	if (hp < 1.0f)
		hp = 1.0f;
	pEnt->pev->health = hp;
	pEnt->pev->max_health = hp;

	g_Wave.hEnemy[slot] = pEnt;
	g_Wave.used[slot] = true;
	return true;
}

static bool Wave_SpawnEnemy(int slot)
{
	Vector origin, angles;
	if (Wave_FindSpotNearPlayer(origin, angles))
		return Wave_CreateGrunt(origin, angles, slot);

	// no safe spot near the player this try: use the map spawn points
	return Wave_SpawnEnemyAtMapSpot(slot);
}

// point idle grunts at the nearest player so a wave cannot stall
static void Wave_AlertEnemies()
{
	for (int i = 0; i < WAVE_MAX_ALIVE; i++)
	{
		if (!g_Wave.used[i])
			continue;

		CBaseEntity* pEnt = g_Wave.hEnemy[i];
		if (!pEnt)
			continue;
		CBaseMonster* pMon = pEnt->MyMonsterPointer();
		if (!pMon)
			continue;

		CBaseEntity* pCurrent = pMon->m_hEnemy;
		if (pCurrent && pCurrent->IsAlive())
			continue;

		CBaseEntity* pTarget = Wave_NearestPlayer(pMon->pev->origin);
		if (!pTarget)
			continue;

		pMon->m_hEnemy = pTarget;
		pMon->m_vecEnemyLKP = pTarget->pev->origin;
		pMon->SetConditions(bits_COND_NEW_ENEMY);
	}
}

float Wave_DamageScale(entvars_t* pevAttacker)
{
	if (g_Wave.state == WAVE_NET_OFF || !pevAttacker)
		return 1.0f;

	for (int i = 0; i < WAVE_MAX_ALIVE; i++)
	{
		if (!g_Wave.used[i])
			continue;
		CBaseEntity* pEnt = g_Wave.hEnemy[i];
		if (pEnt && pEnt->pev == pevAttacker)
			return g_Wave.dmgScale;
	}
	return 1.0f;
}

// ---------------------------------------------------------------------------
// session flow
// ---------------------------------------------------------------------------

static void Wave_BeginWave()
{
	const wave_diff_t& d = g_WaveDiff[g_Wave.difficulty];
	int tier = (g_Wave.wave - 1) / WAVE_TIER_LENGTH;

	g_Wave.hpScale = d.hpMul * powf(WAVE_HP_PER_TIER, (float)tier);
	g_Wave.dmgScale = d.dmgMul * powf(WAVE_DMG_PER_TIER, (float)tier);
	if (g_Wave.dmgScale > WAVE_DMG_SCALE_MAX)
		g_Wave.dmgScale = WAVE_DMG_SCALE_MAX;

	g_Wave.toSpawn = Wave_EnemyCount(g_Wave.wave);
	g_Wave.nextSpawnTime = gpGlobals->time;
	g_Wave.nextAlertTime = gpGlobals->time + WAVE_ALERT_INTERVAL;
	g_Wave.state = WAVE_NET_ACTIVE;

	// mobs are about to spawn: from now on no respawns, dead players spectate,
	// and mp_allowmonsters / allow_spectators are forced (restored on stop)
	g_Wave.locked = true;
	Wave_ApplyCvars();

	char msg[64];
	sprintf(msg, "WAVE %d", g_Wave.wave);
	UTIL_ClientPrintAll(HUD_PRINTCENTER, msg);
}

static void Wave_Cleared()
{
	g_Wave.wavesCleared++;
	g_Wave.wave++;
	g_Wave.state = WAVE_NET_PREP;
	g_Wave.nextWaveTime = gpGlobals->time + WAVE_PAUSE;

	UTIL_ClientPrintAll(HUD_PRINTCENTER, "WAVE CLEARED");
}

// ends the session completely (state OFF, cvars restored, HUD hidden)
static void Wave_Stop(bool removeEnemies)
{
	Wave_RestoreCvars();
	Wave_ClearEnemies(removeEnemies);
	g_Wave.state = WAVE_NET_OFF;
	g_Wave.toSpawn = 0;
	g_Wave.locked = false;
	Wave_Send(true);
}

// dead players become spectators instead of respawning (multiplayer only;
// in singleplayer death still reloads the map)
static void Wave_SpectateDead()
{
	if (!g_pGameRules || !g_pGameRules->IsMultiplayer())
		return;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		if (!Wave_IsConnected(pEnt))
			continue;

		// wait until the death animation is over
		const int d = pEnt->pev->deadflag;
		if (d != DEAD_DEAD && d != DEAD_RESPAWNABLE)
			continue;

		CBasePlayer* pPlayer = (CBasePlayer*)pEnt;
		if ((pPlayer->m_afPhysicsFlags & PFLAG_OBSERVER) != 0)
			continue;

		pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->angles);
	}
}

static void Wave_GameOver()
{
	Wave_LoadHighScores();
	Wave_SamplePoints();

	const int diff = g_Wave.difficulty;
	const int secs = (int)(gpGlobals->time - g_Wave.startTime);

	bool newHigh = false;
	if (g_Wave.peakPoints > g_WaveHighScore[diff])
	{
		g_WaveHighScore[diff] = g_Wave.peakPoints;
		Wave_SaveHighScores();
		newHigh = true;
	}

	char msg[512];
	sprintf(msg,
		"WAVE MODE OVER (%s)\n\n"
		"Waves survived: %d\n"
		"Enemies killed: %d\n"
		"Time survived: %d:%02d\n"
		"Points: %d\n"
		"High score: %d%s\n",
		g_WaveDiff[diff].name,
		g_Wave.wavesCleared,
		g_Wave.kills,
		secs / 60, secs % 60,
		g_Wave.peakPoints,
		g_WaveHighScore[diff], newHigh ? "  (NEW!)" : "");

	// on-screen
	hudtextparms_t tp;
	memset(&tp, 0, sizeof(tp));
	tp.channel = 3;
	tp.x = -1;
	tp.y = 0.25f;
	tp.effect = 0;
	tp.r1 = 255;
	tp.g1 = 190;
	tp.b1 = 0;
	tp.a1 = 255;
	tp.r2 = 255;
	tp.g2 = 190;
	tp.b2 = 0;
	tp.a2 = 255;
	tp.fadeinTime = 0.3f;
	tp.fadeoutTime = 1.0f;
	tp.holdTime = WAVE_STATS_HOLD;
	tp.fxTime = 0;
	UTIL_HudMessageAll(tp, msg);

	// console (clients), plus the server console on dedicated servers
	UTIL_ClientPrintAll(HUD_PRINTCONSOLE, msg);
	if (IS_DEDICATED_SERVER())
		ALERT(at_console, "%s", msg);

	// stats are showing; the retry menu opens once they have faded
	Wave_ClearEnemies(true);
	g_Wave.toSpawn = 0;
	g_Wave.state = WAVE_NET_STATS;
	g_Wave.nextWaveTime = gpGlobals->time + WAVE_STATS_TIME;
	Wave_Send(true);
}

// begins wave 1 (after a short countdown) with the given difficulty
static void Wave_Start(int difficulty)
{
	Wave_LoadHighScores();
	Wave_ClearEnemies(true);
	g_Wave.locked = false; // cvars + respawn lock come when the first wave begins

	g_WaveAutoArmed = false;

	g_Wave.difficulty = difficulty;
	g_Wave.wave = 1;
	g_Wave.wavesCleared = 0;
	g_Wave.kills = 0;
	g_Wave.toSpawn = 0;
	g_Wave.peakPoints = 0;
	g_Wave.hpScale = 1.0f;
	g_Wave.dmgScale = 1.0f;
	g_Wave.startTime = gpGlobals->time;
	g_Wave.nextWaveTime = gpGlobals->time + WAVE_START_DELAY;
	g_Wave.nextNetTime = 0;
	g_Wave.sentState = -1;
	g_Wave.state = WAVE_NET_PREP;
	Wave_ResetFlags();

	// fresh points for the session
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
		if (pEnt && pEnt->IsPlayer())
			Points_Reset((CBasePlayer*)pEnt);
	}

	Wave_Send(true);

	char msg[96];
	sprintf(msg, "Wave mode started (%s). First wave in %d seconds.\n",
		g_WaveDiff[difficulty].name, (int)WAVE_START_DELAY);
	UTIL_ClientPrintAll(HUD_PRINTCONSOLE, msg);
}

// opens the ready-up + vote lobby
static void Wave_OpenLobby()
{
	Wave_ClearEnemies(true);
	Wave_ResetFlags();

	g_WaveAutoArmed = false;

	g_Wave.difficulty = WAVE_DIFF_NORMAL;
	g_Wave.wave = 0;
	g_Wave.wavesCleared = 0;
	g_Wave.kills = 0;
	g_Wave.toSpawn = 0;
	g_Wave.peakPoints = 0;
	g_Wave.hpScale = 1.0f;
	g_Wave.dmgScale = 1.0f;
	g_Wave.nextNetTime = 0;
	g_Wave.sentState = -1;
	g_Wave.state = WAVE_NET_LOBBY;
	g_Wave.locked = false;

	Wave_Send(true);
	UTIL_ClientPrintAll(HUD_PRINTCONSOLE,
		"Wave mode lobby: F4 = ready, 7/8/9/0 = vote easy/normal/hard/extreme\n");
}

// everyone pressed retry: reload the map, then restart with the same difficulty
static void Wave_DoRetry()
{
	g_WavePendingDiff = g_Wave.difficulty;
	Wave_Stop(false);

	if (g_pGameRules && g_pGameRules->IsMultiplayer())
	{
		char cmd[96];
		sprintf(cmd, "changelevel %s\n", STRING(gpGlobals->mapname));
		SERVER_COMMAND(cmd);
	}
	else
	{
		SERVER_COMMAND("reload\n");
	}
}

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------

bool Wave_IsActive()
{
	// not true during the lobby or the countdown before wave 1: players can
	// still respawn there. Locks when the first wave begins.
	return g_Wave.locked;
}

void Wave_Think()
{
	// style-bonus tracking (bhop chain, 360 spins, flicks...) runs every frame
	Points_Think();

	const float t = gpGlobals->time;

	// time went backwards: a new level was loaded
	if (t < g_WaveLastTime)
	{
		g_WaveAutoArmed = true;
		if (g_Wave.state != WAVE_NET_OFF)
			Wave_Stop(false);
	}
	g_WaveLastTime = t;

	// ---- no session: maybe start one automatically ----
	if (g_Wave.state == WAVE_NET_OFF)
	{
		const bool retryPending = g_WavePendingDiff >= 0;
		const bool autoStart = g_WaveAutoArmed && g_cvWaveAuto.value > 0;

		if ((retryPending || autoStart) && Wave_AnyPlayerAlive())
		{
			if (retryPending)
			{
				int diff = g_WavePendingDiff;
				g_WavePendingDiff = -1;
				Wave_Start(diff);
			}
			else
			{
				Wave_OpenLobby();
			}
		}
		return;
	}

	int total = 0, ready = 0;

	// ---- lobby ----
	if (g_Wave.state == WAVE_NET_LOBBY)
	{
		Wave_CountReady(total, ready, NULL);
		if (total > 0 && ready == total)
		{
			// everyone is ready: wait here until sv_cheats is 0
			if (CVAR_GET_FLOAT("sv_cheats") <= 0)
			{
				Wave_Start(Wave_TallyVotes());
				return;
			}
		}
		Wave_Send(false);
		return;
	}

	// ---- end stats showing ----
	if (g_Wave.state == WAVE_NET_STATS)
	{
		Wave_SpectateDead();
		if (t >= g_Wave.nextWaveTime)
		{
			Wave_ResetFlags();
			g_Wave.state = WAVE_NET_RETRY;
			Wave_Send(true);
		}
		else
		{
			Wave_Send(false);
		}
		return;
	}

	// ---- retry menu ----
	if (g_Wave.state == WAVE_NET_RETRY)
	{
		Wave_SpectateDead();
		Wave_CountReady(total, ready, NULL);
		if (total > 0 && ready == total)
		{
			Wave_DoRetry();
			return;
		}
		Wave_Send(false);
		return;
	}

	// ---- PREP / ACTIVE ----
	Wave_SamplePoints();
	if (g_Wave.locked)
		Wave_SpectateDead();

	// before the first wave nobody can lose: players may still be respawning
	if (g_Wave.locked && !Wave_AnyPlayerAlive())
	{
		Wave_GameOver();
		return;
	}

	int alive = Wave_ReapEnemies();

	if (g_Wave.state == WAVE_NET_PREP)
	{
		if (t >= g_Wave.nextWaveTime)
		{
			// wave 1: wait until someone is actually alive (they may be respawning)
			if (!g_Wave.locked && !Wave_AnyPlayerAlive())
				g_Wave.nextWaveTime = t + 1.0f;
			else
				Wave_BeginWave();
		}
	}
	else
	{
		if (g_Wave.toSpawn > 0)
		{
			if (alive < WAVE_MAX_ALIVE && t >= g_Wave.nextSpawnTime)
			{
				int slot = -1;
				for (int i = 0; i < WAVE_MAX_ALIVE; i++)
				{
					if (!g_Wave.used[i])
					{
						slot = i;
						break;
					}
				}

				if (slot >= 0 && Wave_SpawnEnemy(slot))
				{
					g_Wave.toSpawn--;
					g_Wave.nextSpawnTime = t + g_WaveDiff[g_Wave.difficulty].spawnInterval;
				}
				else
				{
					g_Wave.nextSpawnTime = t + 0.25f; // blocked spawns, retry soon
				}
			}
		}
		else if (alive == 0)
		{
			Wave_Cleared();
		}

		if (t >= g_Wave.nextAlertTime)
		{
			Wave_AlertEnemies();
			g_Wave.nextAlertTime = t + WAVE_ALERT_INTERVAL;
		}
	}

	Wave_Send(false);
}

void Wave_Precache()
{
	UTIL_PrecacheOther("monster_human_grunt");
}

// ---------------------------------------------------------------------------
// client commands (from ClientCommand in client.cpp)
// ---------------------------------------------------------------------------

void Wave_ClientReady(CBasePlayer* pPlayer)
{
	if (g_Wave.state != WAVE_NET_LOBBY)
		return;
	int idx = Wave_SlotOf(pPlayer);
	if (idx == 0)
		return;

	g_Wave.readyFlag[idx] = true;
	Wave_Send(true);
}

void Wave_ClientVote(CBasePlayer* pPlayer, int difficulty)
{
	if (g_Wave.state != WAVE_NET_LOBBY)
		return;
	if (difficulty < 0 || difficulty >= WAVE_DIFF_COUNT)
		return;
	int idx = Wave_SlotOf(pPlayer);
	if (idx == 0)
		return;

	g_Wave.voteFor[idx] = difficulty;
	Wave_Send(true);
}

void Wave_ClientRetry(CBasePlayer* pPlayer)
{
	if (g_Wave.state != WAVE_NET_RETRY)
		return;
	int idx = Wave_SlotOf(pPlayer);
	if (idx == 0)
		return;

	g_Wave.readyFlag[idx] = true;
	Wave_Send(true);
}

// ---------------------------------------------------------------------------
// console commands
// ---------------------------------------------------------------------------

static void Wave_PrintUsage()
{
	ALERT(at_console, "usage: ss_wave_start [easy|normal|hard|extreme]  (no argument = ready-up lobby)\n");
}

static void Wave_StartCmd()
{
	if (!Wave_AnyPlayerAlive())
	{
		ALERT(at_console, "ss_wave_start: no living player\n");
		return;
	}

	// no argument: open the ready-up / vote lobby
	if (CMD_ARGC() < 2)
	{
		Wave_OpenLobby();
		return;
	}

	const char* arg = CMD_ARGV(1);
	int diff = -1;
	for (int i = 0; i < WAVE_DIFF_COUNT; i++)
	{
		if (Wave_StrEqNoCase(arg, g_WaveDiff[i].name))
		{
			diff = i;
			break;
		}
	}

	if (diff < 0)
	{
		Wave_PrintUsage();
		return;
	}

	Wave_Start(diff);
}

static void Wave_StopCmd()
{
	if (g_Wave.state == WAVE_NET_OFF)
	{
		ALERT(at_console, "wave mode is not running\n");
		return;
	}
	g_WavePendingDiff = -1;
	g_WaveAutoArmed = false;
	Wave_Stop(true);
	ALERT(at_console, "wave mode stopped\n");
}

void Wave_Init()
{
	CVAR_REGISTER(&g_cvWaveAuto);

	// char arrays: works whether the engine header wants char* or const char*
	static char cmdStart[] = "ss_wave_start";
	static char cmdStartAlias[] = "ss_wavemode_start";
	static char cmdStop[] = "ss_wave_stop";

	g_engfuncs.pfnAddServerCommand(cmdStart, Wave_StartCmd);
	g_engfuncs.pfnAddServerCommand(cmdStartAlias, Wave_StartCmd);
	g_engfuncs.pfnAddServerCommand(cmdStop, Wave_StopCmd);

	Wave_ClearEnemies(false);
	Wave_ResetFlags();
	g_Wave.state = WAVE_NET_OFF;
	g_Wave.sentState = -1;
}