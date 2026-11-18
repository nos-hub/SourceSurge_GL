// hud_wavemode.cpp - HUD for the WAVE gamemode (lobby, wave counters, end stats, retry)
// Lobby / stats / retry are editor widgets (HUD_WAVE_LOBBY / HUD_WAVE_STATS / HUD_WAVE_RETRY).

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ss_hud_editor_v2.h"

#include <stdio.h>
#include <string.h>

#ifndef K_F4
#define K_F4 138
#endif
#ifndef K_F5
#define K_F5 139
#endif

#define WAVEHUD_FADE_IN 0.30f
#define WAVEHUD_FADE_OUT 1.00f
#define WAVEHUD_STATS_TIME 9.5f // keep equal to WAVE_STATS_TIME in wave.cpp
#define WAVEHUD_COUNT_UP 1.2f	// stats numbers count up over this long

static const int g_DiffCol[4][3] = {{90, 220, 90}, {240, 220, 80}, {255, 150, 40}, {255, 70, 60}};
static const char* g_DiffName[4] = {"Easy", "Normal", "Hard", "Extreme"};

DECLARE_MESSAGE(m_WaveMode, WaveState)
DECLARE_MESSAGE(m_WaveMode, WaveStats)

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
	m_flStateStart = 0;

	m_iStatsDiff = 1;
	m_iStatsWaves = m_iStatsKills = m_iStatsSecs = 0;
	m_iStatsPoints = m_iStatsHigh = m_iStatsNewHigh = 0;

	HOOK_MESSAGE(WaveState);
	HOOK_MESSAGE(WaveStats);

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
	const int newState = READ_BYTE();
	if (newState != m_iState)
		m_flStateStart = gHUD.m_flTime;
	m_iState = newState;

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

bool CHudWaveMode::MsgFunc_WaveStats(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_iStatsDiff = READ_BYTE();
	if (m_iStatsDiff < 0 || m_iStatsDiff > 3)
		m_iStatsDiff = 1;
	m_iStatsWaves = READ_SHORT();
	m_iStatsKills = READ_SHORT();
	m_iStatsSecs = READ_SHORT();
	m_iStatsPoints = READ_LONG();
	m_iStatsHigh = READ_LONG();
	m_iStatsNewHigh = READ_BYTE();
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
// drawing helpers (colors are scaled by the fade alpha; text has no alpha channel)
// ---------------------------------------------------------------------------

void CHudWaveMode::DrawPanel(int x, int y, int w, int h, int r, int g, int b, float a)
{
	gEngfuncs.pfnFillRGBA(x, y, w, h, 0, 0, 0, (int)(150 * a));
	gEngfuncs.pfnFillRGBA(x, y, w, 2, r, g, b, (int)(255 * a));		   // accent line
	gEngfuncs.pfnFillRGBA(x, y + h - 1, w, 1, r, g, b, (int)(90 * a)); // border
	gEngfuncs.pfnFillRGBA(x, y, 1, h, r, g, b, (int)(90 * a));
	gEngfuncs.pfnFillRGBA(x + w - 1, y, 1, h, r, g, b, (int)(90 * a));
}

void CHudWaveMode::DrawBar(int x, int y, int w, int h, float frac, int r, int g, int b, float a)
{
	if (frac < 0.0f)
		frac = 0.0f;
	if (frac > 1.0f)
		frac = 1.0f;
	gEngfuncs.pfnFillRGBA(x, y, w, h, 255, 255, 255, (int)(30 * a));
	const int fw = (int)(w * frac);
	if (fw > 0)
		gEngfuncs.pfnFillRGBA(x, y, fw, h, r, g, b, (int)(220 * a));
}

void CHudWaveMode::DrawLeft(int x, int y, const char* text, int r, int g, int b, float a)
{
	gEngfuncs.pfnDrawSetTextColor(r * a / 255.0f, g * a / 255.0f, b * a / 255.0f);
	gEngfuncs.pfnDrawConsoleString(x, y, (char*)text);
}

void CHudWaveMode::DrawCentered(int x, int w, int y, const char* text, int r, int g, int b, float a)
{
	int tw = 0, th = 0;
	gEngfuncs.pfnDrawConsoleStringLen(text, &tw, &th);
	DrawLeft(x + (w - tw) / 2, y, text, r, g, b, a);
}

void CHudWaveMode::DrawRight(int rx, int y, const char* text, int r, int g, int b, float a)
{
	int tw = 0, th = 0;
	gEngfuncs.pfnDrawConsoleStringLen(text, &tw, &th);
	DrawLeft(rx - tw, y, text, r, g, b, a);
}

// ---------------------------------------------------------------------------
// the three panels
// ---------------------------------------------------------------------------

void CHudWaveMode::DrawLobby(int x, int y, float a)
{
	static const char keys[4] = {'7', '8', '9', '0'};
	const int w = WAVEHUD_LOBBY_W;
	char buf[160];

	DrawPanel(x, y, w, WAVEHUD_LOBBY_H, 255, 190, 0, a);
	DrawCentered(x, w, y + 10, "WAVE MODE - READY UP", 255, 190, 0, a);

	if (m_iMyReady)
		sprintf(buf, "You are READY   (%d/%d ready)", m_iReady, m_iTotal);
	else
		sprintf(buf, "Press F4 to ready up   (%d/%d ready)", m_iReady, m_iTotal);
	DrawCentered(x, w, y + 30, buf, m_iMyReady ? 80 : 255, 255, m_iMyReady ? 80 : 255, a);

	DrawBar(x + 20, y + 48, w - 40, 6, m_iTotal > 0 ? (float)m_iReady / m_iTotal : 0.0f, 80, 220, 80, a);

	// difficulty votes, one row each
	for (int i = 0; i < 4; i++)
	{
		const int ry = y + 66 + i * 16;
		const int* c = g_DiffCol[i];

		if (m_iMyVote == i)
			gEngfuncs.pfnFillRGBA(x + 12, ry - 2, w - 24, 15, c[0], c[1], c[2], (int)(40 * a));

		sprintf(buf, "[%c] %s", keys[i], g_DiffName[i]);
		DrawLeft(x + 20, ry, buf, c[0], c[1], c[2], a);

		DrawBar(x + 130, ry + 3, 270, 8, m_iTotal > 0 ? (float)m_iVotes[i] / m_iTotal : 0.0f, c[0], c[1], c[2], a);

		sprintf(buf, "%d", m_iVotes[i]);
		DrawRight(x + w - 20, ry, buf, 255, 255, 255, a);
	}

	if (m_iCheats)
		DrawCentered(x, w, y + 134, "sv_cheats is 1 - set it to 0 to start!", 255, 60, 60, a);
	else if (m_iMyVote >= 4)
		DrawCentered(x, w, y + 134, "No vote = Normal", 160, 160, 160, a);
}

void CHudWaveMode::DrawRetry(int x, int y, float a)
{
	const int w = WAVEHUD_RETRY_W;
	char buf[160];

	DrawPanel(x, y, w, WAVEHUD_RETRY_H, 255, 190, 0, a);
	DrawCentered(x, w, y + 10, "GAME OVER - RETRY?", 255, 190, 0, a);

	if (m_iMyReady)
		sprintf(buf, "Waiting for the others...   (%d/%d want to retry)", m_iReady, m_iTotal);
	else
		sprintf(buf, "%d/%d want to retry", m_iReady, m_iTotal);
	DrawCentered(x, w, y + 30, buf, m_iMyReady ? 80 : 255, 255, m_iMyReady ? 80 : 255, a);

	DrawBar(x + 20, y + 48, w - 40, 6, m_iTotal > 0 ? (float)m_iReady / m_iTotal : 0.0f, 80, 220, 80, a);

	DrawCentered(x, w, y + 62, "[F4] Retry          [F5] Leave", 200, 200, 200, a);
}

void CHudWaveMode::DrawStats(int x, int y, float a, float since)
{
	const int w = WAVEHUD_STATS_W;
	const int* c = g_DiffCol[m_iStatsDiff];
	char buf[64];

	// numbers count up at the start
	float k = since / WAVEHUD_COUNT_UP;
	if (k > 1.0f)
		k = 1.0f;
	if (k < 0.0f)
		k = 0.0f;

	DrawPanel(x, y, w, WAVEHUD_STATS_H, 255, 190, 0, a);
	DrawCentered(x, w, y + 10, "WAVE MODE OVER", 255, 190, 0, a);
	DrawCentered(x, w, y + 28, g_DiffName[m_iStatsDiff], c[0], c[1], c[2], a);

	const int lx = x + 24, rx = x + w - 24;
	int ry = y + 54;
	const int step = 18;

	DrawLeft(lx, ry, "Waves survived", 200, 200, 200, a);
	sprintf(buf, "%d", (int)(m_iStatsWaves * k));
	DrawRight(rx, ry, buf, 255, 255, 255, a);
	ry += step;

	DrawLeft(lx, ry, "Enemies killed", 200, 200, 200, a);
	sprintf(buf, "%d", (int)(m_iStatsKills * k));
	DrawRight(rx, ry, buf, 255, 255, 255, a);
	ry += step;

	DrawLeft(lx, ry, "Time survived", 200, 200, 200, a);
	sprintf(buf, "%d:%02d", m_iStatsSecs / 60, m_iStatsSecs % 60);
	DrawRight(rx, ry, buf, 255, 255, 255, a);
	ry += step;

	DrawLeft(lx, ry, "Points", 200, 200, 200, a);
	sprintf(buf, "%d", (int)(m_iStatsPoints * k));
	DrawRight(rx, ry, buf, 255, 190, 0, a);
	ry += step;

	DrawLeft(lx, ry, "High score", 200, 200, 200, a);
	sprintf(buf, "%d", m_iStatsHigh);
	DrawRight(rx, ry, buf, 255, 255, 255, a);

	if (m_iStatsNewHigh)
		DrawCentered(x, w, y + 152, "NEW HIGH SCORE!", 255, 220, 60, a);
}

bool CHudWaveMode::Draw(float flTime)
{
	if (m_iState == 0)
		return true;

	// while the layout editor is open it draws the boxes itself
	if (gHUD.m_Editor->IsActive())
		return true;

	int x, y;

	// fade in on every state change, fade out at the end of the stats
	const float since = flTime - m_flStateStart;
	float a = since / WAVEHUD_FADE_IN;
	if (m_iState == 5)
	{
		const float left = (WAVEHUD_STATS_TIME - since) / WAVEHUD_FADE_OUT;
		if (left < a)
			a = left;
	}
	if (a > 1.0f)
		a = 1.0f;
	if (a < 0.0f)
		a = 0.0f;

	if (m_iState == 3)
	{
		if (gHUD.m_Editor->Visible(HUD_WAVE_LOBBY))
		{
			gHUD.m_Editor->GetPos(HUD_WAVE_LOBBY, x, y);
			DrawLobby(x, y, a);
		}
		return true;
	}
	if (m_iState == 4)
	{
		if (gHUD.m_Editor->Visible(HUD_WAVE_RETRY))
		{
			gHUD.m_Editor->GetPos(HUD_WAVE_RETRY, x, y);
			DrawRetry(x, y, a);
		}
		return true;
	}
	if (m_iState == 5)
	{
		if (gHUD.m_Editor->Visible(HUD_WAVE_STATS))
		{
			gHUD.m_Editor->GetPos(HUD_WAVE_STATS, x, y);
			DrawStats(x, y, a, since);
		}
		return true;
	}

	// 1 = prep, 2 = active
	char buf[64];

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