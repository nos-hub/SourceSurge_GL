// wave.h - WAVE gamemode (server side)
#ifndef WAVE_H
#define WAVE_H

struct entvars_s;
class CBasePlayer;
class CBaseEntity;

enum
{
	WAVE_DIFF_EASY = 0,
	WAVE_DIFF_NORMAL,
	WAVE_DIFF_HARD,
	WAVE_DIFF_EXTREME,
	WAVE_DIFF_COUNT
};

// call once from GameDLLInit(): registers cvar ss_wave_auto and the commands
// ss_wave_start / ss_wave_stop
void Wave_Init();

// call from ClientPrecache(): precaches the grunt so no late precache happens
void Wave_Precache();

// call every frame from StartFrame()
void Wave_Think();

// true once the first wave has begun (mobs spawning) until the session ends.
// Use it to block respawns: it is false during the lobby and the countdown
// before wave 1, where players may still respawn.
bool Wave_IsActive();

// true while respawning must be refused (cvar ss_wave_respawn_lock, default 1): from the
// first wave until the retry menu shows. Free in the lobby, in the countdown before wave 1
// and while the retry menu is up. Used by FPlayerCanRespawn() in solo and multiplayer rules.
bool Wave_RespawnBlocked();

// damage multiplier for hits dealt by wave enemies (1.0 for everything else).
// call at the top of CBasePlayer::TakeDamage
float Wave_DamageScale(struct entvars_s* pevAttacker);

// called by the points system after a player's kill is scored: wave grunts may
// drop grenades / rockets, with a chance that scales with the points of that kill
void Wave_OnPlayerKill(CBaseEntity* pVictim, int pointsGained, CBasePlayer* pKiller);

// client -> server, call from ClientCommand()
void Wave_ClientReady(CBasePlayer* pPlayer);				// "ss_wave_ready"
void Wave_ClientVote(CBasePlayer* pPlayer, int difficulty); // "ss_wave_vote <0-3>"
void Wave_ClientRetry(CBasePlayer* pPlayer);				// "ss_wave_retry"

#endif // WAVE_H