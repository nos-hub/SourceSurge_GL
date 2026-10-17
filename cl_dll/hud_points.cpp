// hud_points.cpp - points total + kill popup HUD element (client side, cl_dll/)

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ss_hud_editor_v2.h"

#define POPUP_TIME 2.5f // seconds the kill popup stays on screen
#define POPUP_FADE 0.5f // fade-out time at the end of it

#define SS_SCALE 6		   // pixels per cell of the big SS letters
#define SS_GAP 4		   // pixels between the two letters
#define SS_BLINK_TIME 0.6f // last part of the popup: SS blinks out

// Tag names, indexed by tag id. MUST match the STY_* enum order in dlls/points.cpp.
static const char* g_StyleNames[] =
	{
		"",
		"AIR", "BHOP", "SPEED", "CRANE", "GAP", "FALL", "HEIGHT", "FLICK",
		"CHAIN", "CLUTCH", "360",
		"MELEE", "BONK",
		"PISTOL", "TAP", "EMPTY", "QUICKDRAW",
		"HANDCANNON", "HIP",
		"SPRAY", "GRENADE", "FROM-HIP",
		"PUMP", "POINTBLANK", "AIRSHOT", "DOUBLE",
		"BOLT", "PIN", "SNIPE",
		"ROCKET", "AIR-ROCKET", "RJ", "STACK",
		"GAUSS", "THROUGH", "RAIL",
		"BEAM", "MELT", "OVERHEAT",
		"NEST", "SWARM",
		"FRAG", "SATCHEL", "TRAP", "SNARK", "CHAOS"};
#define STYLE_NAME_COUNT ((int)(sizeof(g_StyleNames) / sizeof(g_StyleNames[0])))

// rank names and colors, indexed by rank (0 = none)
static const char* g_RankNames[] = {"", "CLEAN", "NICE", "SICK", "INSANE", "SS"};
static const int g_RankColors[6][3] =
	{
		{255, 255, 255},
		{120, 255, 120}, // CLEAN
		{80, 220, 255},	 // NICE
		{255, 200, 60},	 // SICK
		{255, 110, 40},	 // INSANE
		{255, 60, 220}	 // SS
};

DECLARE_MESSAGE(m_Points, Points)

bool CHudPoints::Init()
{
	HOOK_MESSAGE(Points);

	m_iTotal = 0;
	m_iGained = 0;
	m_iMult100 = 100;
	m_iTagCount = 0;
	m_iTags[0] = m_iTags[1] = m_iTags[2] = 0;
	m_iRank = 0;
	m_flPopupStart = -100.0f;

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);

	return true;
}

bool CHudPoints::VidInit()
{
	return true;
}

bool CHudPoints::MsgFunc_Points(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);

	m_iTotal = READ_LONG();
	m_iGained = READ_LONG();
	m_iMult100 = READ_SHORT();
	m_iTagCount = READ_BYTE();
	m_iTags[0] = READ_BYTE();
	m_iTags[1] = READ_BYTE();
	m_iTags[2] = READ_BYTE();
	m_iRank = READ_BYTE();

	if (m_iTagCount > 3)
		m_iTagCount = 3;
	if (m_iRank > 5)
		m_iRank = 5;

	// gained == 0 is a plain total update (reset, spawn, load): no popup
	if (m_iGained > 0)
		m_flPopupStart = gEngfuncs.GetClientTime();
	else
		m_flPopupStart = -100.0f;

	m_iFlags |= HUD_ACTIVE;
	return true;
}

// 5x7 bitmap of the letter S
static const char* g_BigS[7] =
	{
		".###.",
		"#...#",
		"#....",
		".###.",
		"....#",
		"#...#",
		".###."};

// Draws "SS" as big pixel blocks (text can't be scaled in GoldSrc).
// Colour cycles pink <-> red-purple, the letters shake slightly, and in the
// last SS_BLINK_TIME seconds they blink faster and fade out.
static void DrawBigSS(int x, int y, float age)
{
	const float blinkStart = POPUP_TIME - SS_BLINK_TIME;
	int alpha = 255;
	if (age > blinkStart)
	{
		const float k = (age - blinkStart) / SS_BLINK_TIME; // 0 -> 1
		const bool on = (((int)(age * 14.0f)) & 1) == 0;
		alpha = on ? (int)(255.0f * (1.0f - k)) : 0;
	}
	if (alpha <= 0)
		return;

	// colour: bright pink (255,70,190) <-> red-purple (170,0,110)
	const float t = 0.5f + 0.5f * sinf(age * 7.0f);
	const int r = (int)(170.0f + (255.0f - 170.0f) * t);
	const int g = (int)(0.0f + 70.0f * t);
	const int b = (int)(110.0f + (190.0f - 110.0f) * t);

	// shake: small offset that changes every frame
	const int sx = (int)(sinf(age * 53.0f) * 2.0f);
	const int sy = (int)(cosf(age * 61.0f) * 1.5f);

	const int x0 = x + sx;
	const int y0 = y + sy;

	// dark shadow first so it reads on bright maps
	for (int pass = 0; pass < 2; pass++)
	{
		const int ox = (pass == 0) ? 2 : 0;
		const int oy = (pass == 0) ? 2 : 0;
		const int pr = (pass == 0) ? 0 : r;
		const int pg = (pass == 0) ? 0 : g;
		const int pb = (pass == 0) ? 0 : b;
		const int pa = (pass == 0) ? alpha / 2 : alpha;

		for (int li = 0; li < 2; li++)
		{
			const int lx = x0 + ox + li * (5 * SS_SCALE + SS_GAP);
			for (int row = 0; row < 7; row++)
			{
				int col = 0;
				while (col < 5)
				{
					if (g_BigS[row][col] != '#')
					{
						col++;
						continue;
					}
					const int start = col;
					while (col < 5 && g_BigS[row][col] == '#')
						col++;

					gEngfuncs.pfnFillRGBA(
						lx + start * SS_SCALE,
						y0 + oy + row * SS_SCALE,
						(col - start) * SS_SCALE,
						SS_SCALE,
						pr, pg, pb, pa);
				}
			}
		}
	}
}

bool CHudPoints::Draw(float flTime)
{
	int x, y;
	char buf[128];

	// ---- total points ----
	if (gHUD.m_Editor->Visible(HUD_POINTS_TOTAL))
	{
		gHUD.m_Editor->GetPos(HUD_POINTS_TOTAL, x, y);
		snprintf(buf, sizeof(buf), "%d pts", m_iTotal);
		gEngfuncs.pfnDrawString(x, y, buf, 255, 220, 80);
	}

	// ---- kill popup: points gained, multiplier, style tags, rank ----
	const float age = gEngfuncs.GetClientTime() - m_flPopupStart;

	if (age >= 0.0f && age < POPUP_TIME && gHUD.m_Editor->Visible(HUD_POINTS_POPUP))
	{
		float fade = 1.0f;
		if (age > POPUP_TIME - POPUP_FADE)
			fade = (POPUP_TIME - age) / POPUP_FADE;

		gHUD.m_Editor->GetPos(HUD_POINTS_POPUP, x, y);

		// line 1: points gained + total multiplier
		snprintf(buf, sizeof(buf), "+%d   x%.2f", m_iGained, m_iMult100 / 100.0f);
		gEngfuncs.pfnDrawString(x, y, buf, (int)(255 * fade), (int)(255 * fade), (int)(80 * fade));

		int line = 1;

		// line 2: up to 3 style tags
		if (m_iTagCount > 0)
		{
			buf[0] = 0;
			for (int i = 0; i < m_iTagCount; i++)
			{
				const int id = m_iTags[i];
				if (id <= 0 || id >= STYLE_NAME_COUNT)
					continue;
				if (buf[0])
					strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
				strncat(buf, g_StyleNames[id], sizeof(buf) - strlen(buf) - 1);
			}
			if (buf[0])
			{
				gEngfuncs.pfnDrawString(x, y + 16 * line, buf, (int)(120 * fade), (int)(220 * fade), (int)(255 * fade));
				line++;
			}
		}

		// line 3: style rank (SS is drawn big, the others as text)
		if (m_iRank == 5)
		{
			DrawBigSS(x, y + 16 * line + 4, age);
		}
		else if (m_iRank > 0)
		{
			gEngfuncs.pfnDrawString(x, y + 16 * line, (char*)g_RankNames[m_iRank],
				(int)(g_RankColors[m_iRank][0] * fade),
				(int)(g_RankColors[m_iRank][1] * fade),
				(int)(g_RankColors[m_iRank][2] * fade));
		}
	}

	return true;
}