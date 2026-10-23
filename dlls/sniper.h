// sniper.h - sniper rifle shot info for the style points system (server side, dlls/)
#pragma once

// Filled by weapon_sniper while one shot is being applied, read by points.cpp for each kill of that shot.
struct SniperShot
{
	bool  active;   // true only while the shot's damage is being applied
	bool  scoped;   // the player was scoped when firing
	float scopeAge; // seconds since the scope went up (for QUICKSCOPE)
	int   kills;    // kills already made by THIS shot (so the next one is a collateral)
};

const SniperShot& Sniper_GetShot();

// points.cpp calls this after scoring a sniper kill
void Sniper_NoteKill();
