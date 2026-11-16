#pragma once

// Hitsounds + hitmarkers (client).
// Fed by the "HitMark" user message from dlls/hitfeedback.cpp.
// Custom sounds: drop .wav files in <gamedir>/sound/hitsounds/

class CHudHitFeedback : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

	bool MsgFunc_HitMark(const char* pszName, int iSize, void* pbuf);

	void UserCmd_HitSoundReload();
	void UserCmd_HitSoundList();
	void UserCmd_HitTest();

private:
	enum { SLOT_HIT = 0, SLOT_HEAD = 1, SLOT_KILL = 2, NUM_SLOTS = 3 };

	struct soundslot_t
	{
		char name[64];   // resolved name (no folder, no .wav)
		bool ok;         // file exists
	};

	void OnHit(bool kill, bool head);
	void PlayHitSound(bool kill, bool head);
	void ResolveSlot(int slot, bool force);
	void ReloadSounds();
	bool SoundExists(const char* name);
	static void ParseColor(cvar_t* cv, int& r, int& g, int& b, int defR, int defG, int defB);

	// hitsound cvars
	cvar_t* m_pSoundEnable;
	cvar_t* m_pSoundVolume;
	cvar_t* m_pSoundCvar[NUM_SLOTS]; // hs_hit / hs_head / hs_kill

	// hitmarker cvars
	cvar_t* m_pMarkerEnable;
	cvar_t* m_pMarkerSize;
	cvar_t* m_pMarkerGap;
	cvar_t* m_pMarkerThick;
	cvar_t* m_pMarkerTime;
	cvar_t* m_pColorHit;
	cvar_t* m_pColorHead;
	cvar_t* m_pColorKill;

	soundslot_t m_Slots[NUM_SLOTS];

	float m_flHitTime; // gHUD.m_flTime of the last hit, 0 = nothing to draw
	int   m_iHitType;  // SLOT_HIT / SLOT_HEAD / SLOT_KILL
};
