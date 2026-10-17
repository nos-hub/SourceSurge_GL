#include "hud.h"
#include "cl_util.h"
#include "math.h"

CHudSpeedometer g_HudSpeedometer( NULL );

int CHudSpeedometer::Init()
{
    m_id = HUD_SPEEDOMETER;
    return 1;
}

int CHudSpeedometer::VidInit()
{
    return 1;
}

int CHudSpeedometer::Draw( float flTime )
{
    if( !gHUD.m_Editor->Visible(m_id) ) 
        return 1;

    int x, y;
    gHUD.m_Editor->GetPos(m_id, x, y);

    // Get player velocity
    cl_entity_t *pPlayer = gEngfuncs.GetLocalPlayer();
    if( !pPlayer ) return 1;

    float speed = sqrt( 
        pPlayer->velocity[0] * pPlayer->velocity[0] +
        pPlayer->velocity[1] * pPlayer->velocity[1]
    );

    char buf[32];
    snprintf(buf, sizeof(buf), "SPD: %.0f", speed);

    gEngfuncs.pfnDrawSetTextColor( 1.0f, 1.0f, 1.0f );
    gEngfuncs.pfnDrawConsoleString( x, y, buf );

    return 1;
}
