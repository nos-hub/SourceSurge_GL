// hud_meleecharges.h - melee knockback charges meter (client side, cl_dll/)
// Include in hud.h next to the other element headers (above class CHud).
// If your other HUD headers use int instead of bool for Init/VidInit/Draw,
// copy that return type here and in the .cpp.
#pragma once

class CHudMeleeCharges : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

	bool MsgFunc_MeleeKB(const char* pszName, int iSize, void* pbuf);

private:
	int   m_iCharges;		// 0 to m_iMax
	int   m_iMax;
	float m_flLastUpdate;	// client time of the last message, to hide a stale meter
	float m_flChangeTime;	// client time of the last change, for the flash
};
