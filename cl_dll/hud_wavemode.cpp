// hud_wavemode.cpp - HUD for the WAVE gamemode
// Mirrors hud_points.cpp: same includes, same message macros, same editor calls.

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ss_hud_editor_v2.h"

#include <stdio.h>
#include <string.h>

// engine key codes (common/keydefs.h): F1 = 135, so F4 = 138 and F5 = 139
#ifndef K_F4
#define K_F4 138
#endif
#ifndef K_F5
#define K_F5 139
#endif

DECLARE_MESSAGE(m_WaveMode, WaveState)

bool CHudWaveMode::Init()
{
	m_iState = 0;
	m_iWave = 0;
	m_iRemaining = 0;
	m_iDifficulty = 0;
	m_iReady = 0;
	m_iTotal = 0;
	m_iVotes[0] = m_iVotes[1] = m_iVotes[2] = m_iVotes[3] = 0;
	m_iMyReady = 0;
	m_iMyVote = 255;
	m_iCheats = 0;

	HOOK_MESSAGE(WaveState);

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return true;
}

bool CHudWaveMode::VidInit()
{
	// new level: the server will resend the state if a session is running
	m_iState = 0;
	return true;
}

bool CHudWaveMode::MsgFunc_WaveState(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_iState = READ_BYTE();
	m_iWave = READ_SHORT();
	m_iRemaining = READ_SHORT();
	m_iDifficulty = READ_BYTE();
	m_iReady = READ_BYTE();
	m_iTotal = READ_BYTE();
	m_iVotes[0] = READ_BYTE();
	m_iVotes[1] = READ_BYTE();
	m_iVotes[2] = READ_BYTE();
	m_iVotes[3] = READ_BYTE();
	m_iMyReady = READ_BYTE();
	m_iMyVote = READ_BYTE();
	m_iCheats = READ_BYTE();
	return true;
}

// ---------------------------------------------------------------------------
// keys: F4 ready / retry, F5 leave, 7/8/9/0 vote
// ---------------------------------------------------------------------------

bool CHudWaveMode::KeyEvent(int down, int keynum)
{
	if (m_iState != 3 && m_iState != 4)
		return false;

	char cmd[48];
	cmd[0] = 0;
	bool clientCmd = false;

	if (keynum == K_F4)
	{
		strcpy(cmd, m_iState == 3 ? "ss_wave_ready" : "ss_wave_retry");
	}
	else if (keynum == K_F5 && m_iState == 4)
	{
		strcpy(cmd, "disconnect");
		clientCmd = true;
	}
	else if (m_iState == 3 && (keynum == '7' || keynum == '8' || keynum == '9' || keynum == '0'))
	{
		int vote = 0;
		if (keynum == '7')
			vote = 0; // easy
		else if (keynum == '8')
			vote = 1; // normal
		else if (keynum == '9')
			vote = 2; // hard
		else
			vote = 3; // 0 = extreme
		sprintf(cmd, "ss_wave_vote %d", vote);
	}

	if (cmd[0] == 0)
		return false;

	// act on the press only, but swallow the release too
	if (down)
	{
		if (clientCmd)
			gEngfuncs.pfnClientCmd(cmd);
		else
			gEngfuncs.pfnServerCmd(cmd);
	}
	return true;
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void CHudWaveMode::DrawCentered(int y, const char* text, int r, int g, int b)
{
	int w = 0, h = 0;
	gEngfuncs.pfnDrawConsoleStringLen(text, &w, &h);
	gEngfuncs.pfnDrawSetTextColor(r / 255.0f, g / 255.0f, b / 255.0f);
	gEngfuncs.pfnDrawConsoleString((ScreenWidth - w) / 2, y, (char*)text);
}

void CHudWaveMode::DrawLobby()
{
	static const char* names[4] = {"Easy", "Normal", "Hard", "Extreme"};

	const int panelW = 480;
	const int panelH = m_iCheats ? 118 : 96;
	const int x = (ScreenWidth - panelW) / 2;
	const int y = 24;

	gEngfuncs.pfnFillRGBA(x, y, panelW, panelH, 0, 0, 0, 140);

	char buf[200];

	DrawCentered(y + 8, "WAVE MODE - READY UP", 255, 190, 0);

	if (m_iMyReady)
		sprintf(buf, "You are READY   (%d/%d ready)", m_iReady, m_iTotal);
	else
		sprintf(buf, "Press F4 to ready up   (%d/%d ready)", m_iReady, m_iTotal);
	DrawCentered(y + 28, buf, m_iMyReady ? 80 : 255, 255, m_iMyReady ? 80 : 255);

	sprintf(buf, "VOTE:  [7] Easy %d   [8] Normal %d   [9] Hard %d   [0] Extreme %d",
		m_iVotes[0], m_iVotes[1], m_iVotes[2], m_iVotes[3]);
	DrawCentered(y + 50, buf, 255, 255, 255);

	if (m_iMyVote < 4)
		sprintf(buf, "Your vote: %s", names[m_iMyVote]);
	else
		sprintf(buf, "Your vote: none (normal if nobody votes)");
	DrawCentered(y + 70, buf, 180, 180, 180);

	if (m_iCheats)
		DrawCentered(y + 94, "sv_cheats set to 1 set it to zero to start !", 255, 60, 60);
}

void CHudWaveMode::DrawRetry()
{
	const int panelW = 480;
	const int panelH = 76;
	const int x = (ScreenWidth - panelW) / 2;
	const int y = 24;

	gEngfuncs.pfnFillRGBA(x, y, panelW, panelH, 0, 0, 0, 140);

	char buf[200];

	DrawCentered(y + 8, "GAME OVER - RETRY?", 255, 190, 0);

	if (m_iMyReady)
		sprintf(buf, "Waiting for the others...   (%d/%d want to retry)", m_iReady, m_iTotal);
	else
		sprintf(buf, "F4 = retry   (%d/%d)        F5 = leave", m_iReady, m_iTotal);
	DrawCentered(y + 30, buf, m_iMyReady ? 80 : 255, 255, m_iMyReady ? 80 : 255);

	DrawCentered(y + 52, "F5 = leave the game", 180, 180, 180);
}

bool CHudWaveMode::Draw(float flTime)
{
	if (m_iState == 0)
		return true;

	if (m_iState == 3)
	{
		DrawLobby();
		return true;
	}
	if (m_iState == 4)
	{
		DrawRetry();
		return true;
	}

	char buf[64];
	int x, y;

	// NOTE: use the exact Visible()/GetPos() calls from hud_points.cpp
	// if their signatures differ from these.
	if (gHUD.m_Editor->Visible(HUD_WAVE_COUNTER))
	{
		gHUD.m_Editor->GetPos(HUD_WAVE_COUNTER, x, y);
		sprintf(buf, "WAVE %d", m_iWave);
		gHUD.DrawHudString(x, y, ScreenWidth, buf, 255, 160, 0);
	}

	if (gHUD.m_Editor->Visible(HUD_WAVE_ENEMIES))
	{
		gHUD.m_Editor->GetPos(HUD_WAVE_ENEMIES, x, y);
		if (m_iState == 1)
			sprintf(buf, "GET READY");
		else
			sprintf(buf, "ENEMIES: %d", m_iRemaining);
		gHUD.DrawHudString(x, y, ScreenWidth, buf, 255, 255, 255);
	}

	return true;
}