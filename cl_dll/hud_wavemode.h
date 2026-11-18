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
#define WAVEHUD_STATS_W 360
#define WAVEHUD_STATS_H 184
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

	float m_flStateStart; // client time the current state began (fades)

	// end stats (WaveStats message)
	int m_iStatsDiff;
	int m_iStatsWaves;
	int m_iStatsKills;
	int m_iStatsSecs;
	int m_iStatsPoints;
	int m_iStatsHigh;
	int m_iStatsNewHigh;
};