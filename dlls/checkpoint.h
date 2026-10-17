// checkpoint.h - addcheckpoint / teleport commands (cheats only), server side, dlls/
#pragma once

class CBasePlayer;

// client.cpp, in ClientCommand(): handles "addcheckpoint" and "teleport"
// returns true if the command was one of ours (even when it was ignored because sv_cheats is 0)
bool Checkpoint_Command(CBasePlayer* pPlayer, const char* pcmd);
