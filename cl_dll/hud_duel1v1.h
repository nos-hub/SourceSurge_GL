#ifndef HUD_DUEL1V1_H
#define HUD_DUEL1V1_H

// NOTE: include this header from hud.h at the same place your other HUD element
// headers are included, i.e. AFTER class CHudBase is declared and BEFORE class CHud.
// This header intentionally does not include hud.h itself.

#include "duel1v1_shared.h"

struct ref_params_s;
struct usercmd_s;

#define DUEL_MAX_HITS 96

class CHudDuel1v1 : public CHudBase
{
public:
	bool Init(void);
	bool VidInit(void);
	bool Draw(float flTime);
	void Reset(void);

	// user messages
	bool MsgFunc_DuelState(const char *pszName, int iSize, void *pbuf);
	bool MsgFunc_DuelCfg(const char *pszName, int iSize, void *pbuf);

	// engine entry points (hooked from view.cpp / input.cpp / tri.cpp)
	bool CalcRefdef(struct ref_params_s *pparams);          // true = we set the view, skip normal one
	void CreateMove(float frametime, struct usercmd_s *cmd);
	void DrawWorld(void);                                   // ring + markers (HUD_DrawTransparentTriangles)
	int  KeyEvent(int down, int keynum, const char *pszCurrentBinding); // 0 = consumed
	void MouseEvent(int mstate);

	void ToggleMenu(void);
	bool IsActive(void) const;
	bool IsChoosing(void) const;

private:
	// helpers
	float Cfg(const char *name) const;
	bool  IsHost(void) const;
	bool  OverMenu(void) const;
	void  EnterChoosing(void);
	void  UpdateCursor(void);
	void  UpdatePick(void);
	void  HandleClick(void);
	void  DoAction(int action, int arg);
	void  SendCmd(const char *fmt, ...);
	void  Button(int x, int y, int w, int h, const char *label, int action, int arg, bool enabled, bool highlight);
	void  DrawMenu(void);
	void  DrawScoreboard(void);
	void  DrawChoosingHud(void);
	void  DrawCursor(void);

	// server state (DuelState message)
	int   m_state;
	int   m_myScore, m_enScore, m_killLimit;
	int   m_secLeft;      // -1 = none
	int   m_flags;        // 1 my ready, 2 enemy ready, 4 choosing, 8 duelist
	int   m_enemyIdx;
	int   m_winner;       // 0 none, 1 you, 2 enemy, 3 draw
	int   m_pickTenths;
	int   m_mode;         // 0 duel, 1 hide & seek
	int   m_hiderRel;     // 0 none, 1 you are the hider, 2 the enemy is the hider
	int   m_myTimeT;      // my clock in tenths of a second
	int   m_enTimeT;      // enemy clock in tenths of a second
	int   m_respTenths;   // my automatic respawn countdown in tenths (hide & seek)
	float m_enemyTarget[3];
	float m_enemy[3];     // smoothed
	float m_lastMsg;
	bool  m_wasChoosing;

	// settings (DuelCfg message)
	float m_cfg[DUEL_MAX_OPTS];
	int   m_weaponMask;

	// free camera
	float m_cam[3];
	float m_camYaw;
	float m_lastCamSend;

	// picking
	float m_pick[3];
	bool  m_pickValid, m_pickOk;

	// menu
	bool  m_menu;
	int   m_tab;
	int   m_nSec;
	const char *m_secName[8];
	int   m_px, m_py, m_pw, m_ph;
	int   m_cursorX, m_cursorY;
	bool  m_cursorOn;
	float m_lastClick;
	int   m_prevMouse;

	struct Hit { int x, y, w, h, action, arg; };
	Hit   m_hit[DUEL_MAX_HITS];
	int   m_nHit;
};

#endif // HUD_DUEL1V1_H
