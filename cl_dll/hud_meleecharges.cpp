// hud_meleecharges.cpp - melee knockback charges meter (client side, cl_dll/)
// Same includes, message macros and editor calls as hud_grapplecharge.cpp.

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ss_hud_editor_v2.h"

#include <stdio.h>
#include <math.h>

#define PIP_W       24
#define PIP_H       10
#define PIP_GAP     5
#define STALE_TIME  3.0f	// the server refreshes about every 1 s; no news for this long = hide (dead, level change)
#define FLASH_TIME  0.25f

DECLARE_MESSAGE(m_MeleeCharges, MeleeKB)

bool CHudMeleeCharges::Init()
{
	m_iCharges = 0;
	m_iMax = 4;
	m_flLastUpdate = -100.0f;
	m_flChangeTime = -100.0f;

	HOOK_MESSAGE(MeleeKB);

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return true;
}

bool CHudMeleeCharges::VidInit()
{
	m_flLastUpdate = -100.0f;	// hidden until the server sends the state of the new level
	return true;
}

bool CHudMeleeCharges::MsgFunc_MeleeKB(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const int charges = READ_BYTE();
	const int maxCharges = READ_BYTE();

	if (maxCharges > 0 && maxCharges <= 8)
		m_iMax = maxCharges;
	if (charges != m_iCharges)
		m_flChangeTime = gEngfuncs.GetClientTime();
	m_iCharges = charges;
	if (m_iCharges > m_iMax)
		m_iCharges = m_iMax;

	m_flLastUpdate = gEngfuncs.GetClientTime();
	return true;
}

bool CHudMeleeCharges::Draw(float flTime)
{
	const float now = gEngfuncs.GetClientTime();
	if (now - m_flLastUpdate > STALE_TIME)
		return true;

	// NOTE: same Visible()/GetPos() calls as hud_grapplecharge.cpp (m_Editor is a pointer)
	if (!gHUD.m_Editor->Visible(HUD_MELEE_CHARGES))
		return true;

	int x, y;
	gHUD.m_Editor->GetPos(HUD_MELEE_CHARGES, x, y);

	const bool flashing = (now - m_flChangeTime) < FLASH_TIME;

	// colour of a filled pip: orange, red and pulsing when only 1 is left
	int r = 255, g = 150, b = 40;
	if (m_iCharges == 1)
	{
		const float pulse = 0.5f + 0.5f * sinf(now * 10.0f);
		r = 255;
		g = (int)(40.0f + 50.0f * pulse);
		b = (int)(30.0f + 30.0f * pulse);
	}
	if (flashing)
	{
		r = 255; g = 255; b = 255;
	}

	// label
	gEngfuncs.pfnDrawString(x, y - 14, "MELEE", 200, 200, 200);

	// the pips
	for (int i = 0; i < m_iMax; i++)
	{
		const int px = x + i * (PIP_W + PIP_GAP);

		gEngfuncs.pfnFillRGBA(px - 1, y - 1, PIP_W + 2, PIP_H + 2, 0, 0, 0, 170);	// border
		if (i < m_iCharges)
			gEngfuncs.pfnFillRGBA(px, y, PIP_W, PIP_H, r, g, b, 235);				// charge left
		else
			gEngfuncs.pfnFillRGBA(px, y, PIP_W, PIP_H, 40, 40, 40, 170);			// empty
	}

	// the number
	char buf[16];
	sprintf(buf, "%d/%d", m_iCharges, m_iMax);
	gEngfuncs.pfnDrawString(x + m_iMax * (PIP_W + PIP_GAP) + 4, y - 1, buf, r, g, b);

	return true;
}
