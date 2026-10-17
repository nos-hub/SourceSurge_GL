#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "ss_hud_editor_v2.h"

CHudSpeedometer gHudSpeedometer;

// filled in cl_dll/view.cpp (V_CalcRefdef)
extern float g_flPlayerSpeed;

bool CHudSpeedometer::Init(void)
{
	// Enable the element so it gets updated and drawn
	m_iFlags |= HUD_ACTIVE;

	// Add it manually to the global HUD element list
	gHUD.AddHudElem(this);

	return true;
}

bool CHudSpeedometer::VidInit(void)
{
	return true;
}

bool CHudSpeedometer::Draw(float flTime)
{
	// CHECK EDITOR VISIBILITY
	if (!gHUD.m_Editor->Visible(HUD_SPEEDOMETER))
		return true;

	// GET EDITOR POSITION
	int x, y;
	gHUD.m_Editor->GetPos(HUD_SPEEDOMETER, x, y);

	char buf[64];
	sprintf(buf, "%.0f u/s", g_flPlayerSpeed);

	gEngfuncs.pfnDrawString(x, y, buf, 0, 255, 0);

	return true;
}