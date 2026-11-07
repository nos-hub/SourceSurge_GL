// jumpstats.h - C++ port of GCLJS 2.0.0dev (GameChaos's Longjumps), server side, put in dlls/
// Original: https://codeberg.org/GameChaos/gcljs  (AMX Mod X plugin by GameChaos)
// Port keeps the original jump detection, validation, stats, HUD, beams and commands.
// NOT ported: SQL, sounds, menus (replaced by console commands), weapon-speed stats (CS 1.6 only).
#pragma once

class CBasePlayer;
struct usercmd_s;
struct playermove_s;
struct physent_s;

// ---------------------------------------------------------------------------------------------
// game.cpp, GameDLLInit(): registers cvars and execs gcljs.cfg
void JumpStats_Init();

// player.cpp, LinkUserMessages(): registers the "JumpStat" message that feeds the client HUD (hud_jumpstats.cpp)
void JumpStats_LinkUserMessages();

// client.cpp, ClientPrecache(): precaches the beam sprite
void JumpStats_Precache();

// client.cpp, ClientPutInServer(): resets per player state, loads the default options
void JumpStats_ClientPutInServer(CBasePlayer* pPlayer);

// client.cpp, CmdStart(): same place as your other CmdStart hooks. pPlayer may be null (it's ignored)
void JumpStats_CmdStart(CBasePlayer* pPlayer, const struct usercmd_s* cmd);

// player.cpp, at the very top of CBasePlayer::PreThink()
void JumpStats_PreThink(CBasePlayer* pPlayer);

// player.cpp, at the very top of CBasePlayer::PostThink()
void JumpStats_PostThink(CBasePlayer* pPlayer);

// client.cpp, ClientCommand(): call this FIRST (before the "say" handling) and return if it returns true.
// handles console commands and "say /command" chat commands, see jumpstats.cpp for the list
bool JumpStats_Command(CBasePlayer* pPlayer, const char* pcmd);

// call after a teleport: cancels any jump in progress (and clears the hud / beam if gcljs_clear_hud_and_beam_on_tp is on)
void JumpStats_Reset(CBasePlayer* pPlayer);

// ---------------------------------------------------------------------------------------------
// pm_shared hooks (server build only). These replace the Orpheu hooks of the plugin.
// pm_shared may be a C file, so these are extern "C".
#if !defined(CLIENT_DLL)
#ifdef __cplusplus
extern "C" {
#endif

// PM_Move(): right after `pmove = ppmove;`
void JumpStats_PM_Move(struct playermove_s* ppmove);

// wrap PM_Jump(): call Pre before the original body and Post after it
void JumpStats_PM_JumpPre(struct playermove_s* ppmove);
void JumpStats_PM_JumpPost(struct playermove_s* ppmove);

// PM_LadderMove(): at the top, after the MOVETYPE_NOCLIP early return
void JumpStats_PM_LadderMove(struct playermove_s* ppmove, struct physent_s* pLadder);

#ifdef __cplusplus
}
#endif
#endif