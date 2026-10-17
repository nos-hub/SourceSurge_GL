#include "hud.h"
#include "cl_util.h"
#include "ss_hud_editor_v2.h"

CHudTimer g_HudTimer;

bool CHudTimer::Init()
{
    m_id = HUD_TIMER;
    m_startTime = gEngfuncs.GetAbsoluteTime();
    return true;
}

bool CHudTimer::VidInit()
{
    return true;
}

bool CHudTimer::Draw( float flTime )
{
    if( !gHUD.m_Editor->Visible(m_id) ) 
        return true;

    int x, y;
    gHUD.m_Editor->GetPos(m_id, x, y);

    float elapsed = gEngfuncs.GetAbsoluteTime() - m_startTime;
    int mins = (int)elapsed / 60;
    int secs = (int)elapsed % 60;
    int millis = (int)(( elapsed - (int)elapsed ) * 100);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d.%02d", mins, secs, millis);

    gEngfuncs.pfnDrawSetTextColor( 1.0f, 1.0f, 0.0f );
    gEngfuncs.pfnDrawConsoleString( x, y, buf );

    return true;
}
