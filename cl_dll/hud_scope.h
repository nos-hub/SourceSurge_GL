// hud_scope.h - sniper scope overlay (client side, cl_dll/)
// Include this in hud.h next to the other element headers (above class CHud).
#pragma once

class CHudScope : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;
	void Reset() override;

	bool MsgFunc_SniperScope(const char* pszName, int iSize, void* pbuf);

private:
	bool m_bOn; // the server says the scope is up (right click held)
};
