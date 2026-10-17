// edgebug.h - EdgeBug Stats (port of the AMX Mod X plugin by xPaw, v2.2) - server side, dlls/
#pragma once

class CBasePlayer;
class CBaseEntity;
struct edict_s;
struct usercmd_s;

// client.cpp, top of CmdStart(): records the player's frame time (msec)
void Edgebug_CmdStart(const edict_s* player, const usercmd_s* cmd);

// player.cpp, first line of CBasePlayer::PreThink()
void Edgebug_PreThink(CBasePlayer* pPlayer);

// player.cpp, CBasePlayer::Touch()
void Edgebug_Touch(CBasePlayer* pPlayer);

// triggers.cpp, top of CBaseTrigger::TeleportTouch() (optional): resets the player's state
void Edgebug_Reset(CBaseEntity* pEntity);
