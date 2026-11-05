// photo.h - photo camera state (server side, dlls/)
#pragma once

#include "extdll.h"

class CBasePlayer;

void Photo_ResetAll();						   // map start
void Photo_PlayerDisconnect(int iPlayerIndex); // forget a player's photo
bool Photo_Shutter(CBasePlayer* pPlayer);	   // weapon_camera fired: ask the client for a picture
bool Photo_HasPhoto(CBasePlayer* pPlayer);
bool Photo_TrySpray(CBasePlayer* pPlayer, const TraceResult& tr); // true = the photo was sprayed (and used up)
bool Photo_ClientCommand(edict_t* pEntity, const char* pcmd);	  // handles "ss_photo ..."
