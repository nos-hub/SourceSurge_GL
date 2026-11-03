// surffix.h - Surf Fix (server side, dlls/)
#pragma once

struct edict_s;
typedef struct edict_s edict_t;

// client.cpp, CmdStart(): remembers the player's velocity before the movement of this command
void SurfFix_CmdStart(const edict_t* player);

// client.cpp, CmdEnd(): detects the surf bug after the movement and fixes it
void SurfFix_CmdEnd(const edict_t* player);
