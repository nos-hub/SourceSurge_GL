#ifndef HUD_SPEEDOMETER_H
#define HUD_SPEEDOMETER_H

#include "hud.h"

class CHudSpeedometer : public CHudBase
{
public:
	bool Init(void) override;
	bool VidInit(void) override;
	bool Draw(float flTime) override;
};

extern CHudSpeedometer gHudSpeedometer;

#endif