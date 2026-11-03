#include "hud.h"
#include "cl_util.h"
#include "ss_hud_editor_v2.h"
#include "hud_speedometer.h"
#include <math.h>
#include <stdio.h>

bool CHudSpeedometer::Init()
{
	m_id = HUD_SPEEDOMETER;
	return true;
}

bool CHudSpeedometer::VidInit()
{
	return true;
}

bool CHudSpeedometer::Draw(float flTime)
{
	if (!gHUD.m_Editor->Visible(m_id))
		return true;

	int x, y;
	gHUD.m_Editor->GetPos(m_id, x, y);

	// CHud already tracks the player's velocity
	const Vector& vel = gHUD.m_vecPlayerVelocity;
	const float speed = sqrtf(vel.x * vel.x + vel.y * vel.y);

	char buf[32];
	snprintf(buf, sizeof(buf), "SPD: %.0f", speed);

	gEngfuncs.pfnDrawSetTextColor(1.0f, 1.0f, 1.0f);
	gEngfuncs.pfnDrawConsoleString(x, y, buf);

	return true;
}