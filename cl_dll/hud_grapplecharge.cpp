// hud_grapplecharge.cpp - grapple charge bar (client side, cl_dll/)
// Same includes, message macros and editor calls as hud_wavemode.cpp.

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ss_hud_editor_v2.h"

#include <stdio.h>
#include <math.h>

#define BAR_WIDTH 120
#define BAR_HEIGHT 8
#define STALE_TIME 0.3f // no update for this long = hide (death, weapon removed...)

DECLARE_MESSAGE(m_GrappleCharge, GrapCharge)

bool CHudGrappleCharge::Init()
{
	m_iActive = 0;
	m_iPercent = 0;
	m_iRange = 0;
	m_flLastUpdate = -100.0f;

	HOOK_MESSAGE(GrapCharge);

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return true;
}

bool CHudGrappleCharge::VidInit()
{
	m_iActive = 0;
	return true;
}

bool CHudGrappleCharge::MsgFunc_GrapCharge(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_iActive = READ_BYTE();
	m_iPercent = READ_BYTE();
	m_iRange = READ_SHORT();

	if (m_iPercent > 100)
		m_iPercent = 100;

	m_flLastUpdate = gEngfuncs.GetClientTime();
	return true;
}

bool CHudGrappleCharge::Draw(float flTime)
{
	if (!m_iActive)
		return true;

	const float now = gEngfuncs.GetClientTime();
	if (now - m_flLastUpdate > STALE_TIME)
		return true;

	// NOTE: same Visible()/GetPos() calls as hud_wavemode.cpp
	if (!gHUD.m_Editor->Visible(HUD_GRAPPLE_CHARGE))
		return true;

	int x, y;
	gHUD.m_Editor->GetPos(HUD_GRAPPLE_CHARGE, x, y);

	const float frac = m_iPercent / 100.0f;
	const bool full = (m_iPercent >= 100);

	// colour: cool blue when short, hot orange when long; full charge pulses
	int r = (int)(90.0f + 165.0f * frac);
	int g = (int)(200.0f - 90.0f * frac);
	int b = (int)(255.0f - 205.0f * frac);
	if (full)
	{
		const float pulse = 0.5f + 0.5f * sinf(now * 16.0f);
		r = (int)(255.0f);
		g = (int)(120.0f + 120.0f * pulse);
		b = (int)(40.0f + 120.0f * pulse);
	}

	// background + border
	gEngfuncs.pfnFillRGBA(x - 1, y - 1, BAR_WIDTH + 2, BAR_HEIGHT + 2, 0, 0, 0, 160);
	gEngfuncs.pfnFillRGBA(x, y, BAR_WIDTH, BAR_HEIGHT, 30, 30, 30, 160);

	// fill
	const int fillW = (int)(BAR_WIDTH * frac);
	if (fillW > 0)
		gEngfuncs.pfnFillRGBA(x, y, fillW, BAR_HEIGHT, r, g, b, 230);

	// range readout under the bar
	char buf[32];
	sprintf(buf, "%d u", m_iRange);
	gEngfuncs.pfnDrawString(x, y + BAR_HEIGHT + 4, buf, r, g, b);

	return true;
}