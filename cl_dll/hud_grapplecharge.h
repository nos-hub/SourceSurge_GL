// hud_grapplecharge.h - grapple charge bar (client side, cl_dll/)
// Include this in hud.h next to the other element headers (above class CHud).
// If your other HUD headers use int instead of bool for Init/VidInit/Draw,
// copy that return type here and in the .cpp.
#pragma once

class CHudGrappleCharge : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

	bool MsgFunc_GrapCharge(const char* pszName, int iSize, void* pbuf);

private:
	int   m_iActive;		// 1 while the player is charging
	int   m_iPercent;		// 0-100
	int   m_iRange;			// range in units for the current charge
	float m_flLastUpdate;	// client time of the last message, to hide a stale bar
};
