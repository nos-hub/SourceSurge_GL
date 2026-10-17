// health_on_kill.h - heal on kill + overheal system
#pragma once

class CBasePlayer;

// Call this when the player gets a kill with flPoints points.
// Grants health based on points (100 pts = 25 HP), with overheal up to 250.
void HealthOnKill_Grant(CBasePlayer* pPlayer, float flPoints);

// Call this from CBasePlayer::PostThink() to decay overheal over time.
void HealthOnKill_DecayOverheal(CBasePlayer* pPlayer);
