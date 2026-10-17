// hud_wavemode.h - HUD for the WAVE gamemode
//   in game : "WAVE X" + enemies remaining (editor widgets)
//   lobby   : ready-up / difficulty-vote panel at the top of the screen
//   retry   : retry / leave panel after the game over stats
// Include this in hud.h just above `class CHud`, right where hud_points.h is included.
// If hud_points.h declares Init/VidInit/Draw with another return type than bool,
// copy that return type here and in the .cpp.
#pragma once

class CHudWaveMode : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

	bool MsgFunc_WaveState(const char* pszName, int iSize, void* pbuf);

	// called from HUD_Key_Event; returns true when the key was consumed
	bool KeyEvent(int down, int keynum);

private:
	void DrawCentered(int y, const char* text, int r, int g, int b);
	void DrawLobby();
	void DrawRetry();

	int m_iState; // 0 off, 1 countdown, 2 wave running, 3 lobby, 4 retry
	int m_iWave;
	int m_iRemaining;
	int m_iDifficulty;

	int m_iReady;	 // players ready (lobby) / pressed retry (retry)
	int m_iTotal;	 // connected players
	int m_iVotes[4]; // votes per difficulty
	int m_iMyReady;	 // this player pressed ready / retry
	int m_iMyVote;	 // 0-3, or 255 = no vote
	int m_iCheats;	 // lobby: sv_cheats is on, start is blocked
};