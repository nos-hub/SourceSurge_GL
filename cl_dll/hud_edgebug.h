#pragma once

class CHudEdgeBug : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override;
	void Reset() override;
	void Test(int speed);
	bool MsgFunc_EdgeBug(const char* pszName, int iSize, void* pbuf);

private:
	bool  m_bShow = false;
	float m_flStart = 0.0f;
	int   m_iCount = 0, m_iDist = 0, m_iSpeed = 0, m_iFps = 0;
	cvar_t* m_pCvarTime = nullptr;
};