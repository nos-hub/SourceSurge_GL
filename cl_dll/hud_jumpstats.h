// hud_jumpstats.h - client side HUD for the server side jumpstats (dlls/jumpstats.cpp, GCLJS port)
//
// Put this file in cl_dll/ and #include it from hud.h right BEFORE "class CHud" (CHudBase must already be declared).
// Then add this member to class CHud, next to m_MeleeCharges:
//
//     CHudJumpStats m_JumpStats;

#pragma once

#define JS_HUD_MAX_FRAMES  200 // must match MAX_JUMP_FRAMES in dlls/jumpstats.cpp
#define JS_HUD_MAX_STRAFES 20  // must match MAX_STRAFES in dlls/jumpstats.cpp

class CHudJumpStats : public CHudBase
{
public:
	bool Init();
	bool VidInit();
	bool Draw(float flTime);

	// "JumpStat" user message, see the protocol comment in dlls/jumpstats.cpp
	bool MsgFunc_JumpStat(const char* pszName, int iSize, void* pbuf);

private:
	struct Info
	{
		int type;
		int flags;
		int strafes;
		int dir;
		float distance, xjDistance, blockDist, edge, landEdge, veer, airpath;
		float prespeed, maxspeed;
		float sync, loss, potency, jumpoffAngle, height, airtimeFrac;
		int airtime, overlap, deadair, fwdRelease;
	};

	struct StrafeRow
	{
		float sync, gain, loss, max;
		int air, overlap, deadair;
		int avgEff, peakEff;
	};

	void DrawInfo(int x, int y);
	void DrawGraph(int x, int y);
	void DrawStrafes(int x, int y);
	void DrawSpeed(int x, int y);

	Info m_info;
	StrafeRow m_strafes[JS_HUD_MAX_STRAFES];
	int m_strafeCount;
	unsigned char m_graph[JS_HUD_MAX_FRAMES];
	int m_graphLen;

	bool m_bHaveInfo;
	float m_flJumpTime;	// client time of the last INFO message
	bool m_bPrespeed;	// show "(prespeed)" under the speed until we land
	float m_flPrespeed;

	int m_iCharW;
	int m_iLineH;

	cvar_t* m_pCvarHold;
	cvar_t* m_pCvarFrames;
	cvar_t* m_pCvarVertical;
};
