// points.h - kill points + style bonus system (server side, dlls/)
// Include AFTER extdll.h / cbase.h like the other game headers.
#pragma once

class CBasePlayer;
class CBaseEntity;

// A monster/player died. If the attacker is a player, award points
// (base points x every style bonus that applies, capped).
void Points_OnKill(CBaseEntity* pVictim, entvars_t* pevAttacker);

// A player died: award the killer (multiplayer), then reset the victim to 0.
void Points_OnPlayerDeath(CBasePlayer* pVictim, entvars_t* pevAttacker);

// Set a player's points to 0, clear their style state and update their HUD
// (used by points_reset and the wave mode).
void Points_Reset(CBasePlayer* pPlayer);

// Send the current total to the client without a kill popup (spawn / load).
void Points_Sync(CBasePlayer* pPlayer);

// Per-frame tracking for the style bonuses (bhop chain, 360 spins, flicks,
// weapon switches...). Called from Wave_Think(), which already runs every frame.
void Points_Think();

// Add movement-tech points to the player's total and tell the HUD (name + points under it).
void Points_AddTech(CBasePlayer* pPlayer, int techId, int points);
