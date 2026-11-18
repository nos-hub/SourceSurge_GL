// hud_wavemode.h - HUD for the WAVE gamemode
//   in game : "WAVE X" + enemies remaining (editor widgets)
//   lobby   : ready-up / difficulty-vote panel          (widget WaveLobby)
//   stats   : end stats after the game over             (widget WaveStats)
//   retry   : retry / leave panel after the stats       (widget WaveRetry)
// Include this in hud.h just above `class CHud`, right where hud_points.h is included.
#pragma once

// panel sizes, shared with the editor registration in hud.cpp
#define WAVEHUD_LOBBY_W 480
#define WAVEHUD_LOBBY_H 152
#define WAVEHUD_STATS_W 420
#define WAVEHUD_STATS_H 392 // maximum: the panel shrinks when there is no scoreboard
#define WAVEHUD_RETRY_W 480
#define WAVEHUD_RETRY_H 84

class CHudWaveMode : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

	bool MsgFunc_WaveState(const char* pszName, int iSize, void* pbuf);
	bool MsgFunc_WaveStats(const char* pszName, int iSize, void* pbuf);

	// called from HUD_Key_Event; returns true when the key was consumed
	bool KeyEvent(int down, int keynum);

private:
	void DrawLobby(int x, int y, float a);
	void DrawRetry(int x, int y, float a);
	void DrawStats(int x, int y, float a, float since);

	void DrawPanel(int x, int y, int w, int h, int r, int g, int b, float a);
	void DrawBar(int x, int y, int w, int h, float frac, int r, int g, int b, float a);
	void DrawLeft(int x, int y, const char* text, int r, int g, int b, float a);
	void DrawCentered(int x, int w, int y, const char* text, int r, int g, int b, float a);
	void DrawRight(int rx, int y, const char* text, int r, int g, int b, float a);

	int m_iState; // 0 off, 1 countdown, 2 wave running, 3 lobby, 4 retry, 5 end stats
	int m_iWave;
	int m_iRemaining;
	int m_iDifficulty;

	int m_iReady;	 // players ready (lobby) / pressed retry (retry)
	int m_iTotal;	 // connected players
	int m_iVotes[4]; // votes per difficulty
	int m_iMyReady;	 // this player pressed ready / retry
	int m_iMyVote;	 // 0-3, or 255 = no vote
	int m_iCheats;	 // lobby: sv_cheats is on, start is blocked
	int m_iRunCheated; // sv_cheats was enabled during the run: no high score

	float m_flStateStart; // client time the current state began (fades)

	// end stats (WaveStats message)
	int m_iStatsDiff;
	int m_iStatsWaves;
	int m_iStatsKills;
	int m_iStatsSecs;
	int m_iStatsPoints;
	int m_iStatsHigh;
	int m_iStatsNewHigh;
	int m_iStatsCheated;

	int m_iStatsFastWave, m_iStatsFastSecs; // 0 secs = none
	int m_iStatsSlowWave, m_iStatsSlowSecs;
	int m_iStatsBestKill;
	char m_szStatsBestKillName[32];
	char m_szStatsFirstDown[32];
	char m_szStatsLastStanding[32];

	// scoreboard rows (best points first), filled by the WaveStats row messages
	enum { STATS_MAX_ROWS = 8 };
	int m_iStatsRows;
	int m_iRowIndex[STATS_MAX_ROWS]; // entity index (own row is highlighted)
	char m_szRowName[STATS_MAX_ROWS][32];
	int m_iRowPoints[STATS_MAX_ROWS];
	int m_iRowKills[STATS_MAX_ROWS];
	int m_iRowDeaths[STATS_MAX_ROWS];
	int m_iRowFlags[STATS_MAX_ROWS]; // 1 = top player, 2 = most kills
	int m_iStatsExpectedRows;
};