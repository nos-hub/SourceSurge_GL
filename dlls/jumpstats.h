// jumpstats.h - Jump stats (port of the core of uq_jumpstats-2025 by muppet922 / kzuq^BorJomi), server side, dlls/
#pragma once

class CBasePlayer;

// player.cpp, in CBasePlayer::PreThink(), right after Slam_PreThink(this); (outside any if-braces)
void JumpStats_PreThink(CBasePlayer* pPlayer);

// player.cpp, at the very top of CBasePlayer::PostThink()
void JumpStats_PostThink(CBasePlayer* pPlayer);

// client.cpp, in ClientCommand(): handles js_hud, js_console, js_strafes, js_fails, jumpstats
// returns true if the command was one of ours
bool JumpStats_Command(CBasePlayer* pPlayer, const char* pcmd);

// called after a teleport: cancels any jump in progress and clears the bhop / drop context
void JumpStats_Reset(CBasePlayer* pPlayer);