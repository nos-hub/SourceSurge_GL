// movement_tech.h - style points for movement tech (server side, dlls/)
#pragma once

class CBasePlayer;

// Tech ids sent to the client (message "TechPts"). The client has the SAME list in
// hud_points.cpp (g_TechNames): keep the order identical on both sides.
enum
{
	TECH_NONE = 0,
	TECH_WALLBUG,
	TECH_SURF,
	TECH_EDGEBUG,  // single edgebug
	TECH_EDGEBUG2, // double
	TECH_EDGEBUG3, // triple and beyond
	TECH_RAMPSLIDE,
	TECH_PERFBHOP,
	TECH_FASTLADDER,
	TECH_WALLSTRAFE,
	TECH_JUMPBUG,
	TECH_COUNT
};

// player.cpp, CBasePlayer::PreThink(), right after JumpStats_PreThink(this);
void MoveTech_PreThink(CBasePlayer* pPlayer);

// edgebug.cpp, PrintMessage(), right after st.edgebugs++;  (chain = 1, 2, 3...)
void MoveTech_OnEdgebug(CBasePlayer* pPlayer, int chain);
