// hud_points.h - points total + kill popup HUD element (client side, cl_dll/)
// Include this from hud.h in the same place as your other element headers
// (before class CHud, after class CHudBase).
#pragma once

class CHudPoints : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;

	bool MsgFunc_Points(const char* pszName, int iSize, void* pbuf);
	bool MsgFunc_TechPts(const char* pszName, int iSize, void* pbuf);

private:
	int m_iTotal;
	int m_iGained;
	int m_iMult100;
	int m_iTagCount; // style tags shown (0-3)
	int m_iTags[3];	 // tag ids, same list as STY_* in dlls/points.cpp
	int m_iRank;	 // 0 none, 1 CLEAN .. 5 SS
	float m_flPopupStart;

	// movement tech indicator (name + points under it)
	int m_iTechId;		 // tech id, same list as TECH_* in dlls/movement_tech.h
	int m_iTechSum;		 // points shown (adds up while the same tech keeps scoring)
	float m_flTechStart; // when the indicator was last refreshed
};