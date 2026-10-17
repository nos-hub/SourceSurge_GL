// wave.h - WAVE gamemode (server side)
#ifndef WAVE_H
#define WAVE_H

struct entvars_s;
class CBasePlayer;

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

// damage multiplier for hits dealt by wave enemies (1.0 for everything else).
// call at the top of CBasePlayer::TakeDamage
float Wave_DamageScale(struct entvars_s* pevAttacker);

// client -> server, call from ClientCommand()
void Wave_ClientReady(CBasePlayer* pPlayer);				// "ss_wave_ready"
void Wave_ClientVote(CBasePlayer* pPlayer, int difficulty); // "ss_wave_vote <0-3>"
void Wave_ClientRetry(CBasePlayer* pPlayer);				// "ss_wave_retry"

#endif // WAVE_H