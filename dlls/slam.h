// slam.h - air slam mechanic (server side, dlls/)
#pragma once

class CBasePlayer;

// Does EVERYTHING now (trigger, fall, landing shockwave, fall damage, sounds).
// Call from CBasePlayer::PreThink(), right AFTER the block that does
//     m_flFallVelocity = -pev->velocity.z;
void Slam_PreThink(CBasePlayer* pPlayer);

// Call from ClientPrecache() in client.cpp
void Slam_Precache();

// Kept only so the old hook calls still compile. They do nothing now and can be deleted
// from player.cpp whenever you want.
void Slam_PostThink(CBasePlayer* pPlayer);
float Slam_ModifyFallDamage(CBasePlayer* pPlayer, float flDamage);
void Slam_Request(CBasePlayer* pPlayer);