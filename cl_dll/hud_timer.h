#ifndef HUD_TIMER_H
#define HUD_TIMER_H

#include "hud.h"

class CHudTimer : public CHudBase
{
public:
    bool Init(void) override;
    bool VidInit(void) override;
    bool Draw(float flTime) override;
    void Reset() { m_startTime = gEngfuncs.GetAbsoluteTime(); }

private:
    int m_id;
    float m_startTime;
};

extern CHudTimer g_HudTimer;

#endif
