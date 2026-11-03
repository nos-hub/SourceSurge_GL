#pragma once

#include "hud.h"

class CHudSpeedometer : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

private:
	int m_id = 0;
};