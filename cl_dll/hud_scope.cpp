// hud_scope.cpp - sniper scope overlay (client side, cl_dll/)
//
// Drawn while the sniper's scope is up (the server sends "SniperScope" 1 when right click is pressed and 0
// when it is released, on reload, weapon switch, or death). Everything outside a circle is black, with a
// thin crosshair across the lens, like an AWP scope.
//
// Initialise this module BEFORE the other HUD elements in CHud::Init() so the overlay is drawn first and the
// health/ammo/points stay visible on top of it.

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"

#include <math.h>

#define SCOPE_RADIUS 0.46f // lens radius as a fraction of the screen height
#define SCOPE_STEP 2	   // pixel rows drawn per strip (bigger = faster, rougher circle edge)

DECLARE_MESSAGE(m_Scope, SniperScope)

bool CHudScope::Init()
{
	m_bOn = false;

	HOOK_MESSAGE(SniperScope);

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return true;
}

bool CHudScope::VidInit()
{
	m_bOn = false;
	return true;
}

void CHudScope::Reset()
{
	m_bOn = false;
}

bool CHudScope::MsgFunc_SniperScope(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_bOn = READ_BYTE() != 0;
	return true;
}

bool CHudScope::Draw(float flTime)
{
	if (!m_bOn)
		return true;

	const int sw = ScreenWidth;
	const int sh = ScreenHeight;
	const int cx = sw / 2;
	const int cy = sh / 2;
	const int r = (int)(sh * SCOPE_RADIUS);

	// black everywhere except a circle: one or two strips per pixel row group
	for (int y = 0; y < sh; y += SCOPE_STEP)
	{
		const int dy = (y + SCOPE_STEP / 2) - cy;

		if (dy >= r || -dy >= r)
		{
			gEngfuncs.pfnFillRGBA(0, y, sw, SCOPE_STEP, 0, 0, 0, 255);
			continue;
		}

		const int half = (int)sqrtf((float)(r * r - dy * dy));
		const int left = cx - half;
		const int right = cx + half;

		if (left > 0)
			gEngfuncs.pfnFillRGBA(0, y, left, SCOPE_STEP, 0, 0, 0, 255);
		if (right < sw)
			gEngfuncs.pfnFillRGBA(right, y, sw - right, SCOPE_STEP, 0, 0, 0, 255);
	}

	// thin crosshair across the lens, with a small gap in the middle
	const int gap = 8;
	gEngfuncs.pfnFillRGBA(cx - r, cy - 1, r - gap, 2, 0, 0, 0, 255);		 // left
	gEngfuncs.pfnFillRGBA(cx + gap, cy - 1, r - gap, 2, 0, 0, 0, 255);		 // right
	gEngfuncs.pfnFillRGBA(cx - 1, cy - r, 2, r - gap, 0, 0, 0, 255);		 // top
	gEngfuncs.pfnFillRGBA(cx - 1, cy + gap, 2, r - gap, 0, 0, 0, 255);		 // bottom

	// small centre dot
	gEngfuncs.pfnFillRGBA(cx - 1, cy - 1, 2, 2, 255, 40, 40, 255);

	return true;
}
