// melee_kb.h - melee wall/ground knockback with charges (server side, dlls/)
// Include it at the TOP of the .cpp file (never inside a function), AFTER cbase.h and player.h,
// like the other game headers. It deliberately has no forward declarations: a forward
// declaration inside a function would create a different, local class with the same name.
#pragma once

// Called by the crowbar when a swing CONNECTS, after its damage has been applied.
//   vecPlaneNormal : tr.vecPlaneNormal. Not used any more, kept so the call does not change.
//   pHitEntity     : the entity that was hit (CBaseEntity::Instance(tr.pHit)), may be the world
// World hit  -> throws the player BEHIND them (opposite to the view direction), costs 1 charge.
// Enemy hit  -> a normal melee attack: no knockback, no charge.
// With 0 charges nothing is pushed (the swing still damages as usual).
void MeleeKB_OnMeleeHit(CBasePlayer* pPlayer, const Vector& vecPlaneNormal, CBaseEntity* pHitEntity);

// Called by the points system when a player gets a kill with the melee weapon:
// restores 1 charge (never above the maximum).
void MeleeKB_OnMeleeKill(CBasePlayer* pPlayer);

// Every frame (called from Wave_Think): gives full charges on spawn, keeps the HUD in sync.
void MeleeKB_Think();

void MeleeKB_ToggleInfinite(CBasePlayer* pPlayer);