#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ss_hud_editor_v2.h"
#include <stdio.h>
#include <stdlib.h>

DECLARE_MESSAGE(m_EdgeBug, EdgeBug)

static const float EB_FADE_TIME = 0.7f;
static const float EB_POP_TIME = 0.12f;
static const int EB_BOX_W = 220;
static const int EB_BOX_H = 78;

// fall speed tiers -> accent color
struct EbTier
{
	int minSpeed;
	int r, g, b;
	const char* name;
};
static const EbTier kEbTiers[] = {
	{1800, 255, 60, 200, "INSANE"},
	{1200, 255, 70, 50, "HUGE"},
	{800, 255, 160, 30, "BIG"},
	{500, 80, 255, 120, "GOOD"},
	{0, 220, 220, 220, ""},
};

static const EbTier& EbGetTier(int speed)
{
	for (const EbTier& t : kEbTiers)
		if (speed >= t.minSpeed)
			return t;
	return kEbTiers[4];
}

static int EbTextW(const char* s)
{
	int w = 0, h = 0;
	gEngfuncs.pfnDrawConsoleStringLen(s, &w, &h);
	return w;
}

static void EbText(int x, int y, const char* s, float r, float g, float b)
{
	gEngfuncs.pfnDrawSetTextColor(r, g, b);
	gEngfuncs.pfnDrawConsoleString(x, y, (char*)s);
}

static void EbTestCmd()
{
	int speed = gEngfuncs.Cmd_Argc() > 1 ? atoi(gEngfuncs.Cmd_Argv(1)) : 900;
	gHUD.m_EdgeBug.Test(speed);
}

void CHudEdgeBug::Test(int speed)
{
	m_iCount = 2;
	m_iDist = 640;
	m_iSpeed = speed;
	m_iFps = 250;
	m_flStart = gHUD.m_flTime;
	m_bShow = true;
}

bool CHudEdgeBug::Init()
{
	m_bShow = false;
	m_flStart = 0.0f;
	m_iFlags |= HUD_ACTIVE;
	m_pCvarTime = gEngfuncs.pfnRegisterVariable("eb_hud_time", "5", FCVAR_ARCHIVE);
	HOOK_MESSAGE(EdgeBug);
	gEngfuncs.Con_Printf("[edgebug] hooked\n");
	gEngfuncs.pfnAddCommand("eb_hud_test", EbTestCmd);
	gHUD.AddHudElem(this);
	return true;
}

bool CHudEdgeBug::VidInit() { return true; }

void CHudEdgeBug::Reset() { m_bShow = false; }


bool CHudEdgeBug::MsgFunc_EdgeBug(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_iCount = READ_BYTE();
	m_iDist = READ_SHORT();
	m_iSpeed = READ_SHORT();
	m_iFps = READ_SHORT();

	m_flStart = gHUD.m_flTime;
	m_bShow = true;
	return true;
}

bool CHudEdgeBug::Draw(float flTime)
{
	if (!gHUD.m_Editor->Visible(HUD_EDGEBUG))
		return true;
	if (gHUD.m_Editor->IsActive() || !m_bShow)
		return true;

	const float hold = m_pCvarTime ? m_pCvarTime->value : 5.0f;
	const float age = flTime - m_flStart;
	if (age > hold + EB_FADE_TIME)
	{
		m_bShow = false;
		return true;
	}

	float a = 1.0f;
	if (age > hold)
		a = 1.0f - (age - hold) / EB_FADE_TIME;
	if (a < 0.0f)
		a = 0.0f;

	// brief white flash on the title right after the hit
	const float pop = age < EB_POP_TIME ? 1.0f - age / EB_POP_TIME : 0.0f;

	int x, y;
	gHUD.m_Editor->GetPos(HUD_EDGEBUG, x, y);

	const EbTier& tier = EbGetTier(m_iSpeed);
	const float tr = tier.r / 255.0f, tg = tier.g / 255.0f, tb = tier.b / 255.0f;

	// background + accent bar
	gEngfuncs.pfnFillRGBA(x, y, EB_BOX_W, EB_BOX_H, 0, 0, 0, (int)(140 * a));
	gEngfuncs.pfnFillRGBA(x, y, 3, EB_BOX_H, tier.r, tier.g, tier.b, (int)(255 * a));

	// title
	char title[48];
	switch (m_iCount)
	{
	case 1: snprintf(title, sizeof(title), "EDGEBUG"); break;
	case 2: snprintf(title, sizeof(title), "DOUBLE EDGEBUG"); break;
	case 3: snprintf(title, sizeof(title), "TRIPLE EDGEBUG"); break;
	default: snprintf(title, sizeof(title), "%dx EDGEBUG", m_iCount); break;
	}

	// combos go gold, singles use the tier color
	float cr = m_iCount > 1 ? 1.0f : tr;
	float cg = m_iCount > 1 ? 0.85f : tg;
	float cb = m_iCount > 1 ? 0.2f : tb;
	cr += (1.0f - cr) * pop;
	cg += (1.0f - cg) * pop;
	cb += (1.0f - cb) * pop;
	EbText(x + 10, y + 6, title, cr * a, cg * a, cb * a);

	if (tier.name[0])
	{
		const int w = EbTextW(tier.name);
		EbText(x + EB_BOX_W - w - 8, y + 6, tier.name, tr * a, tg * a, tb * a);
	}

	// rows: grey label on the left, value right-aligned
	char v0[24], v1[24], v2[24];
	snprintf(v0, sizeof(v0), "%d u", m_iDist);
	snprintf(v1, sizeof(v1), "%d u/s", m_iSpeed);
	snprintf(v2, sizeof(v2), "%d", m_iFps);

	const char* labels[3] = {"DISTANCE", "FALL SPEED", "ENGINE FPS"};
	const char* vals[3] = {v0, v1, v2};

	int ry = y + 24;
	for (int i = 0; i < 3; i++)
	{
		EbText(x + 10, ry, labels[i], 0.6f * a, 0.6f * a, 0.65f * a);
		if (i == 1)
			EbText(x + EB_BOX_W - EbTextW(vals[i]) - 8, ry, vals[i], tr * a, tg * a, tb * a);
		else
			EbText(x + EB_BOX_W - EbTextW(vals[i]) - 8, ry, vals[i], a, a, a);
		ry += 14;
	}

	// fall speed bar (full at 2000 u/s)
	float frac = m_iSpeed / 2000.0f;
	if (frac > 1.0f)
		frac = 1.0f;
	const int barW = EB_BOX_W - 16;
	gEngfuncs.pfnFillRGBA(x + 8, y + EB_BOX_H - 8, barW, 3, 255, 255, 255, (int)(30 * a));
	gEngfuncs.pfnFillRGBA(x + 8, y + EB_BOX_H - 8, (int)(barW * frac), 3, tier.r, tier.g, tier.b, (int)(255 * a));

	return true;
}