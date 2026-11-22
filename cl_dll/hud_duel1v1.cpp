// ---------------------------------------------------------------------------
// Source Surge - 1v1 mode (client side)
//
//  * Free top-down camera used while you are dead and choosing where to respawn
//    (WASD pan, Space/Ctrl height, Shift fast, R recenter on the enemy).
//  * Live ring around the enemy: red disc = too close, green band = valid, outside = too far.
//  * Click inside the band to send "1v1_respawn x y z". The server validates everything.
//  * Settings menu (console: 1v1menu) that only sends commands; the server owns settings.
// ---------------------------------------------------------------------------
#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "in_buttons.h"
#include "ref_params.h"
#include "usercmd.h"
#include "triangleapi.h"
#include "const.h"
#include "hud_duel1v1.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Verify these two against what demoui.cpp / your HUD_Key_Event already receives.
#define DUEL_K_MOUSE1 241
#define DUEL_K_ESCAPE 27

// Set to 0 if your engine already draws a visible system cursor while the menu is open.
#define DUEL_DRAW_OWN_CURSOR 1

enum
{
	A_NONE = 0, A_TAB, A_DEC, A_INC, A_TOGGLE, A_WEAPON, A_PRESET, A_READY, A_STOP, A_CLOSE
};

DECLARE_MESSAGE(m_Duel1v1, DuelState)
DECLARE_MESSAGE(m_Duel1v1, DuelCfg)

extern bool g_iVisibleMouse;

// ------------------------------------------------------------------------
// INTEGRATION POINT: free / grab the mouse cursor.
// This mod has no demoui.cpp; use the same global the VGUI viewport toggles.
// ------------------------------------------------------------------------
static void Duel_CursorShow(void)
{
	g_iVisibleMouse = true;
}

static void Duel_CursorHide(void)
{
	g_iVisibleMouse = false;
}

static void Cmd_Duel1v1Menu(void)
{
	gHUD.m_Duel1v1.ToggleMenu();
}

// ------------------------------------------------------------- text utils ---
static void PutText(int x, int y, float r, float g, float b, const char *s)
{
	gEngfuncs.pfnDrawSetTextColor(r, g, b);
	gEngfuncs.pfnDrawConsoleString(x, y, (char *)s);
}

static int TextWidth(const char *s)
{
	int w = 0, h = 0;
	gEngfuncs.pfnDrawConsoleStringLen(s, &w, &h);
	return w;
}

static void PutTextCentered(int cx, int y, float r, float g, float b, const char *s)
{
	PutText(cx - TextWidth(s) / 2, y, r, g, b, s);
}

// ------------------------------------------------------------------- init ---
bool CHudDuel1v1::Init(void)
{
	HOOK_MESSAGE(DuelState);
	HOOK_MESSAGE(DuelCfg);
	gEngfuncs.pfnAddCommand("1v1menu", Cmd_Duel1v1Menu);

	for (int i = 0; i < DUEL_MAX_OPTS; i++)
		m_cfg[i] = 0;
	for (int i = 0; i < DUEL_NUM_OPTS; i++)
		m_cfg[i] = (float)atof(g_Duel1v1Opts[i].defval);
	m_weaponMask = 0;

	// unique sections in table order -> menu tabs
	m_nSec = 0;
	for (int i = 0; i < DUEL_NUM_OPTS; i++)
	{
		bool seen = false;
		for (int s = 0; s < m_nSec; s++)
			if (!strcmp(m_secName[s], g_Duel1v1Opts[i].section)) seen = true;
		if (!seen && m_nSec < 8)
			m_secName[m_nSec++] = g_Duel1v1Opts[i].section;
	}

	m_menu = false;
	m_tab = 0;
	m_cursorOn = false;
	m_cursorX = m_cursorY = 0;
	m_lastClick = -1.0f;
	m_prevMouse = 0;
	m_px = m_py = 0;
	m_pw = 500;
	m_ph = 400;
	m_nHit = 0;

	Reset();
	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return 1;
}

void CHudDuel1v1::Reset(void)
{
	m_state = DUEL_OFF;
	m_myScore = m_enScore = m_killLimit = 0;
	m_secLeft = -1;
	m_flags = 0;
	m_enemyIdx = 0;
	m_winner = 0;
	m_pickTenths = 0;
	m_mode = 0; m_hiderRel = 0; m_myTimeT = m_enTimeT = 0; m_respTenths = 0;
	for (int i = 0; i < 3; i++)
	{
		m_enemyTarget[i] = 0;
		m_enemy[i] = 0;
		m_cam[i] = 0;
		m_pick[i] = 0;
	}
	m_camYaw = 90.0f;
	m_lastMsg = -100.0f;
	m_lastCamSend = 0;
	m_wasChoosing = false;
	m_pickValid = m_pickOk = false;
}

bool CHudDuel1v1::VidInit(void)
{
	return 1;
}

// -------------------------------------------------------------- messages ---
bool CHudDuel1v1::MsgFunc_DuelState(const char *pszName, int iSize, void *pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_state     = READ_BYTE();
	m_myScore   = READ_BYTE();
	m_enScore   = READ_BYTE();
	m_killLimit = READ_BYTE();
	m_secLeft   = READ_SHORT();
	m_flags     = READ_BYTE();
	m_enemyIdx  = READ_BYTE();
	m_winner    = READ_BYTE();
	m_pickTenths = READ_SHORT();
	for (int i = 0; i < 3; i++)
		m_enemyTarget[i] = READ_LONG() / 10.0f;
	m_mode       = READ_BYTE();
	m_hiderRel   = READ_BYTE();
	m_myTimeT    = READ_SHORT();
	m_enTimeT    = READ_SHORT();
	m_respTenths = READ_SHORT();

	m_lastMsg = gEngfuncs.GetClientTime();

	const bool ch = (m_flags & 4) != 0 && m_state == DUEL_LIVE;
	if (ch && !m_wasChoosing)
		EnterChoosing();
	m_wasChoosing = ch;
	return 1;
}

bool CHudDuel1v1::MsgFunc_DuelCfg(const char *pszName, int iSize, void *pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const int n = READ_BYTE();
	for (int i = 0; i < n; i++)
	{
		const float v = READ_LONG() / 1000.0f;
		if (i < DUEL_NUM_OPTS)
			m_cfg[i] = v;
	}
	m_weaponMask = READ_LONG();
	return 1;
}

// ---------------------------------------------------------------- queries ---
bool CHudDuel1v1::IsActive(void) const
{
	return (gEngfuncs.GetClientTime() - m_lastMsg) < 1.0f && m_state != DUEL_OFF;
}

bool CHudDuel1v1::IsChoosing(void) const
{
	return IsActive() && m_state == DUEL_LIVE && (m_flags & 4) != 0;
}

float CHudDuel1v1::Cfg(const char *name) const
{
	for (int i = 0; i < DUEL_NUM_OPTS; i++)
		if (!strcmp(g_Duel1v1Opts[i].cvar, name))
			return m_cfg[i];
	return 0.0f;
}

bool CHudDuel1v1::IsHost(void) const
{
	cl_entity_t *pl = gEngfuncs.GetLocalPlayer();
	return pl && pl->index == 1; // listen server host; mirror the server check
}

bool CHudDuel1v1::OverMenu(void) const
{
	return m_menu && m_cursorX >= m_px && m_cursorX < m_px + m_pw &&
	       m_cursorY >= m_py && m_cursorY < m_py + m_ph;
}

void CHudDuel1v1::SendCmd(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	gEngfuncs.pfnServerCmd(buf);
}

void CHudDuel1v1::ToggleMenu(void)
{
	m_menu = !m_menu;
}

// ----------------------------------------------------------- camera logic ---
void CHudDuel1v1::EnterChoosing(void)
{
	for (int i = 0; i < 3; i++)
		m_enemy[i] = m_enemyTarget[i];
	m_cam[0] = m_enemy[0];
	m_cam[1] = m_enemy[1];
	m_cam[2] = m_enemy[2] + 700.0f;
	m_camYaw = 90.0f;
	m_lastCamSend = 0;
}

bool CHudDuel1v1::CalcRefdef(struct ref_params_s *pparams)
{
	if (!IsChoosing())
		return false;

	float k = pparams->frametime * 20.0f; // smooth the enemy marker
	if (k > 1.0f) k = 1.0f;
	for (int i = 0; i < 3; i++)
		m_enemy[i] += (m_enemyTarget[i] - m_enemy[i]) * k;

	pparams->vieworg[0] = m_cam[0];
	pparams->vieworg[1] = m_cam[1];
	pparams->vieworg[2] = m_cam[2];
	pparams->viewangles[0] = 90.0f; // straight down
	pparams->viewangles[1] = m_camYaw;
	pparams->viewangles[2] = 0.0f;
	pparams->nextView = 0;
	return true;
}

void CHudDuel1v1::CreateMove(float frametime, struct usercmd_s *cmd)
{
	if (!IsChoosing())
		return;

	float fwd = 0, side = 0, up = 0;
	if (cmd->buttons & IN_FORWARD)   fwd  += 1.0f;
	if (cmd->buttons & IN_BACK)      fwd  -= 1.0f;
	if (cmd->buttons & IN_MOVERIGHT) side += 1.0f;
	if (cmd->buttons & IN_MOVELEFT)  side -= 1.0f;
	if (cmd->buttons & IN_JUMP)      up   += 1.0f;
	if (cmd->buttons & IN_DUCK)      up   -= 1.0f;
	const bool fast = (cmd->buttons & IN_RUN) != 0;

	if (cmd->buttons & IN_RELOAD) // recenter on the enemy
	{
		m_cam[0] = m_enemy[0];
		m_cam[1] = m_enemy[1];
	}

	// pan in the camera's own frame so W is always "up" on screen
	float ang[3] = { 90.0f, m_camYaw, 0.0f };
	float f[3], r[3], u[3];
	gEngfuncs.pfnAngleVectors(ang, f, r, u);

	const float height = m_cam[2] - m_enemy[2];
	const float speed = (600.0f + height * 0.6f) * (fast ? 2.5f : 1.0f);
	m_cam[0] += (u[0] * fwd + r[0] * side) * speed * frametime;
	m_cam[1] += (u[1] * fwd + r[1] * side) * speed * frametime;
	m_cam[2] += up * 700.0f * (fast ? 2.0f : 1.0f) * frametime;

	// keep the camera near the action
	const float lim = Cfg("ss_1v1_respawn_max") + 512.0f;
	float dx = m_cam[0] - m_enemy[0];
	float dy = m_cam[1] - m_enemy[1];
	const float dist = sqrtf(dx * dx + dy * dy);
	if (dist > lim)
	{
		m_cam[0] = m_enemy[0] + dx / dist * lim;
		m_cam[1] = m_enemy[1] + dy / dist * lim;
	}
	if (m_cam[2] < m_enemy[2] + 128.0f)  m_cam[2] = m_enemy[2] + 128.0f;
	if (m_cam[2] > m_enemy[2] + 3000.0f) m_cam[2] = m_enemy[2] + 3000.0f;

	// let the server know where the camera is (used for entity visibility)
	const float t = gEngfuncs.GetClientTime();
	if (t - m_lastCamSend > 0.1f)
	{
		m_lastCamSend = t;
		char buf[96];
		snprintf(buf, sizeof(buf), "1v1_cam %.0f %.0f %.0f", m_cam[0], m_cam[1], m_cam[2]);
		gEngfuncs.pfnServerCmdUnreliable(buf);
	}

	// a dead player must not trigger "press any button to respawn" or other actions
	cmd->forwardmove = 0;
	cmd->sidemove = 0;
	cmd->upmove = 0;
	cmd->buttons = 0;
}

// Cursor -> ray -> intersection with the horizontal plane at the enemy's height.
// (A world trace would hit the ceiling first, so we intersect a plane instead; the
// server finds the real floor.)
void CHudDuel1v1::UpdatePick(void)
{
	m_pickValid = false;
	if (!IsChoosing() || OverMenu())
		return;

	float ang[3] = { 90.0f, m_camYaw, 0.0f };
	float f[3], r[3], u[3];
	gEngfuncs.pfnAngleVectors(ang, f, r, u);

	// Hor+ FOV: the configured fov is for 4:3, vertical extent stays fixed.
	const float fov = gHUD.m_iFOV ? (float)gHUD.m_iFOV : 90.0f;
	const float tanY = tanf(fov * 0.5f * (float)M_PI / 180.0f) * 0.75f;
	const float tanX = tanY * (float)ScreenWidth / (float)ScreenHeight;
	const float nx = (2.0f * m_cursorX / (float)ScreenWidth) - 1.0f;
	const float ny = 1.0f - (2.0f * m_cursorY / (float)ScreenHeight);

	float d[3];
	for (int i = 0; i < 3; i++)
		d[i] = f[i] + r[i] * nx * tanX + u[i] * ny * tanY;
	if (d[2] > -0.001f)
		return;

	const float t = (m_enemy[2] - m_cam[2]) / d[2];
	if (t <= 0.0f)
		return;

	m_pick[0] = m_cam[0] + d[0] * t;
	m_pick[1] = m_cam[1] + d[1] * t;
	m_pick[2] = m_enemy[2];

	const float dx = m_pick[0] - m_enemy[0];
	const float dy = m_pick[1] - m_enemy[1];
	const float dist = sqrtf(dx * dx + dy * dy);
	m_pickOk = dist >= Cfg("ss_1v1_respawn_min") && dist <= Cfg("ss_1v1_respawn_max");
	m_pickValid = true;
}

// ------------------------------------------------------------------ input ---
void CHudDuel1v1::UpdateCursor(void)
{
	const bool want = m_menu || IsChoosing();
	if (want != m_cursorOn)
	{
		m_cursorOn = want;
		if (want) Duel_CursorShow();
		else      Duel_CursorHide();
	}
	if (want)
		gEngfuncs.GetMousePosition(&m_cursorX, &m_cursorY);
}

int CHudDuel1v1::KeyEvent(int down, int keynum, const char *pszCurrentBinding)
{
	if (!m_menu && !IsChoosing())
		return 1;

	if (keynum == DUEL_K_MOUSE1)
	{
		if (down) HandleClick();
		return 0;
	}
	if (keynum == DUEL_K_ESCAPE && m_menu)
	{
		if (down) m_menu = false;
		return 0;
	}
	return 1;
}

void CHudDuel1v1::MouseEvent(int mstate)
{
	if ((m_menu || IsChoosing()) && (mstate & 1) && !(m_prevMouse & 1))
		HandleClick();
	m_prevMouse = mstate;
}

void CHudDuel1v1::HandleClick(void)
{
	const float t = gEngfuncs.GetClientTime();
	if (t - m_lastClick < 0.15f) // key event + mouse event can both fire for one click
		return;
	m_lastClick = t;

	for (int i = m_nHit - 1; i >= 0; i--)
	{
		const Hit &h = m_hit[i];
		if (m_cursorX >= h.x && m_cursorX < h.x + h.w && m_cursorY >= h.y && m_cursorY < h.y + h.h)
		{
			DoAction(h.action, h.arg);
			return;
		}
	}

	if (OverMenu())
		return;

	if (IsChoosing() && m_pickValid && m_pickOk)
		SendCmd("1v1_respawn %.1f %.1f %.1f", m_pick[0], m_pick[1], m_pick[2]);
}

void CHudDuel1v1::DoAction(int action, int arg)
{
	switch (action)
	{
	case A_TAB:
		m_tab = arg;
		break;
	case A_CLOSE:
		m_menu = false;
		break;
	case A_DEC:
	case A_INC:
	{
		const Duel1v1Opt &o = g_Duel1v1Opts[arg];
		float v = m_cfg[arg] + (action == A_INC ? 1.0f : -1.0f) * o.step;
		if (v < o.minv) v = o.minv;
		if (v > o.maxv) v = o.maxv;
		m_cfg[arg] = v; // optimistic, the server echoes the real value
		SendCmd("1v1_set %s %g", o.cvar, v);
		break;
	}
	case A_TOGGLE:
	{
		const float v = (m_cfg[arg] != 0.0f) ? 0.0f : 1.0f;
		m_cfg[arg] = v;
		SendCmd("1v1_set %s %g", g_Duel1v1Opts[arg].cvar, v);
		break;
	}
	case A_WEAPON:
	{
		const bool on = !(m_weaponMask & (1 << arg));
		m_weaponMask ^= (1 << arg);
		SendCmd("1v1_weapon %s %d", g_Duel1v1Weapons[arg].classname, on ? 1 : 0);
		break;
	}
	case A_PRESET:
		SendCmd("1v1_preset %s", g_Duel1v1Presets[arg]);
		break;
	case A_READY:
		SendCmd("1v1_ready");
		break;
	case A_STOP:
		SendCmd("1v1_stop");
		break;
	}
}

// ------------------------------------------------------------------ world ---
void CHudDuel1v1::DrawWorld(void)
{
	if (!IsChoosing())
		return;

	static HSPRITE hWhite = 0;
	if (!hWhite)
		hWhite = SPR_Load("sprites/white.spr");
	const struct model_s *spr = gEngfuncs.GetSpritePointer(hWhite);
	if (!spr)
		return;

	const float minR = Cfg("ss_1v1_respawn_min");
	const float maxR = Cfg("ss_1v1_respawn_max");
	const float cx = m_enemy[0], cy = m_enemy[1], cz = m_enemy[2] - 34.0f; // roughly floor level
	const int N = 72;
	const float step = (float)(2.0 * M_PI) / (float)N;

	gEngfuncs.pTriAPI->SpriteTexture((struct model_s *)spr, 0);
	gEngfuncs.pTriAPI->RenderMode(kRenderTransAdd);
	gEngfuncs.pTriAPI->CullFace(TRI_NONE);
	gEngfuncs.pTriAPI->Brightness(1.0f);

	// red disc: forbidden zone
	gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_FAN);
	gEngfuncs.pTriAPI->Color4f(1.0f, 0.15f, 0.1f, 0.22f);
	gEngfuncs.pTriAPI->TexCoord2f(0, 0);
	gEngfuncs.pTriAPI->Vertex3f(cx, cy, cz);
	for (int i = 0; i <= N; i++)
	{
		const float a = i * step;
		gEngfuncs.pTriAPI->TexCoord2f(0, 0);
		gEngfuncs.pTriAPI->Vertex3f(cx + cosf(a) * minR, cy + sinf(a) * minR, cz);
	}
	gEngfuncs.pTriAPI->End();

	// green band: valid zone
	gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_STRIP);
	gEngfuncs.pTriAPI->Color4f(0.2f, 1.0f, 0.3f, 0.22f);
	for (int i = 0; i <= N; i++)
	{
		const float a = i * step;
		gEngfuncs.pTriAPI->TexCoord2f(0, 0);
		gEngfuncs.pTriAPI->Vertex3f(cx + cosf(a) * maxR, cy + sinf(a) * maxR, cz);
		gEngfuncs.pTriAPI->TexCoord2f(0, 0);
		gEngfuncs.pTriAPI->Vertex3f(cx + cosf(a) * minR, cy + sinf(a) * minR, cz);
	}
	gEngfuncs.pTriAPI->End();

	// border lines
	gEngfuncs.pTriAPI->Begin(TRI_LINES);
	for (int pass = 0; pass < 2; pass++)
	{
		const float R = pass ? maxR : minR;
		if (pass) gEngfuncs.pTriAPI->Color4f(0.4f, 1.0f, 0.5f, 0.9f);
		else      gEngfuncs.pTriAPI->Color4f(1.0f, 0.4f, 0.3f, 0.9f);
		for (int i = 0; i < N; i++)
		{
			const float a0 = i * step, a1 = (i + 1) * step;
			gEngfuncs.pTriAPI->TexCoord2f(0, 0);
			gEngfuncs.pTriAPI->Vertex3f(cx + cosf(a0) * R, cy + sinf(a0) * R, cz + 1.0f);
			gEngfuncs.pTriAPI->TexCoord2f(0, 0);
			gEngfuncs.pTriAPI->Vertex3f(cx + cosf(a1) * R, cy + sinf(a1) * R, cz + 1.0f);
		}
	}
	// enemy marker (vertical line)
	gEngfuncs.pTriAPI->Color4f(1.0f, 1.0f, 1.0f, 1.0f);
	gEngfuncs.pTriAPI->TexCoord2f(0, 0);
	gEngfuncs.pTriAPI->Vertex3f(cx, cy, cz);
	gEngfuncs.pTriAPI->TexCoord2f(0, 0);
	gEngfuncs.pTriAPI->Vertex3f(cx, cy, cz + 160.0f);

	// cursor marker (diamond on the pick plane)
	if (m_pickValid)
	{
		if (m_pickOk) gEngfuncs.pTriAPI->Color4f(0.3f, 1.0f, 0.4f, 1.0f);
		else          gEngfuncs.pTriAPI->Color4f(1.0f, 0.3f, 0.2f, 1.0f);
		const float s = 24.0f;
		const float px = m_pick[0], py = m_pick[1], pz = cz + 2.0f;
		const float vx[5] = { px + s, px, px - s, px, px + s };
		const float vy[5] = { py, py + s, py, py - s, py };
		for (int i = 0; i < 4; i++)
		{
			gEngfuncs.pTriAPI->TexCoord2f(0, 0);
			gEngfuncs.pTriAPI->Vertex3f(vx[i], vy[i], pz);
			gEngfuncs.pTriAPI->TexCoord2f(0, 0);
			gEngfuncs.pTriAPI->Vertex3f(vx[i + 1], vy[i + 1], pz);
		}
	}
	gEngfuncs.pTriAPI->End();

	gEngfuncs.pTriAPI->RenderMode(kRenderNormal);
}

// -------------------------------------------------------------------- HUD ---
bool CHudDuel1v1::Draw(float flTime)
{
	m_nHit = 0;
	UpdateCursor();

	if (IsActive())
		DrawScoreboard();
	if (IsActive() && m_mode == 1 && m_respTenths > 0)
	{
		char rb[48];
		snprintf(rb, sizeof(rb), "RESPAWNING IN %.1f", m_respTenths / 10.0f);
		PutTextCentered(ScreenWidth / 2, ScreenHeight / 2, 1.0f, 0.9f, 0.4f, rb);
	}
	if (m_menu)
		DrawMenu();
	if (IsChoosing())
	{
		UpdatePick();
		DrawChoosingHud();
	}
#if DUEL_DRAW_OWN_CURSOR
	if (m_cursorOn)
		DrawCursor();
#endif
	return 1;
}

void CHudDuel1v1::DrawCursor(void)
{
	FillRGBA(m_cursorX - 8, m_cursorY - 1, 17, 3, 255, 255, 255, 200);
	FillRGBA(m_cursorX - 1, m_cursorY - 8, 3, 17, 255, 255, 255, 200);
}

void CHudDuel1v1::DrawScoreboard(void)
{
	const int cx = ScreenWidth / 2;
	char buf[128];

	if (m_mode == 1)
	{
		const int my = m_myTimeT / 10, en = m_enTimeT / 10;
		char l[64], r[64];
		snprintf(l, sizeof(l), "YOU %s %d:%02d", m_hiderRel == 1 ? "[HIDER]" : "[seeker]", my / 60, my % 60);
		snprintf(r, sizeof(r), "%d:%02d %s ENEMY", en / 60, en % 60, m_hiderRel == 2 ? "[HIDER]" : "[seeker]");
		// the hider's clock (the one that is ticking) is highlighted
		if (m_hiderRel == 1) PutText(cx - 20 - TextWidth(l), 12, 1.0f, 0.45f, 0.35f, l);
		else                 PutText(cx - 20 - TextWidth(l), 12, 0.7f, 0.7f, 0.7f, l);
		if (m_hiderRel == 2) PutText(cx + 20, 12, 1.0f, 0.45f, 0.35f, r);
		else                 PutText(cx + 20, 12, 0.7f, 0.7f, 0.7f, r);
	}
	else
	{
		snprintf(buf, sizeof(buf), "YOU  %d  -  %d  ENEMY", m_myScore, m_enScore);
		PutTextCentered(cx, 12, 1.0f, 1.0f, 1.0f, buf);
	}

	if (m_state == DUEL_LIVE || m_state == DUEL_COUNTDOWN)
	{
		if (m_mode == 1)
			snprintf(buf, sizeof(buf), "first clock to 0 wins");
		else if (m_secLeft >= 0 && m_state == DUEL_LIVE)
			snprintf(buf, sizeof(buf), "first to %d   |   %d:%02d left", m_killLimit, m_secLeft / 60, m_secLeft % 60);
		else
			snprintf(buf, sizeof(buf), "first to %d", m_killLimit);
		PutTextCentered(cx, 30, 0.7f, 0.7f, 0.7f, buf);
	}

	const int cy = ScreenHeight / 3;
	switch (m_state)
	{
	case DUEL_WAITING:
		if (m_flags & 8)
		{
			snprintf(buf, sizeof(buf), "1v1: type 1v1_ready or use 1v1menu   [you: %s | enemy: %s]",
			         (m_flags & 1) ? "READY" : "not ready", (m_flags & 2) ? "READY" : "not ready");
			PutTextCentered(cx, cy, 1.0f, 0.9f, 0.4f, buf);
		}
		else
		{
			PutTextCentered(cx, cy, 0.8f, 0.8f, 0.8f, "1v1: waiting for two players");
		}
		break;
	case DUEL_COUNTDOWN:
		snprintf(buf, sizeof(buf), "GET READY  %d", m_secLeft < 0 ? 0 : m_secLeft);
		PutTextCentered(cx, cy, 1.0f, 0.9f, 0.4f, buf);
		break;
	case DUEL_ENDED:
		if (m_winner == 1)      PutTextCentered(cx, cy, 0.4f, 1.0f, 0.4f, "YOU WIN");
		else if (m_winner == 2) PutTextCentered(cx, cy, 1.0f, 0.4f, 0.4f, "YOU LOSE");
		else                    PutTextCentered(cx, cy, 1.0f, 1.0f, 1.0f, "DRAW");
		break;
	}
}

void CHudDuel1v1::DrawChoosingHud(void)
{
	const int cx = ScreenWidth / 2;
	const int y = ScreenHeight - 110;
	char buf[96];

	PutTextCentered(cx, y, 1.0f, 1.0f, 1.0f, "CLICK INSIDE THE GREEN RING TO RESPAWN");
	snprintf(buf, sizeof(buf), "auto-pick in %.1fs", m_pickTenths / 10.0f);
	PutTextCentered(cx, y + 18, 1.0f, 0.9f, 0.4f, buf);
	PutTextCentered(cx, y + 36, 0.7f, 0.7f, 0.7f, "WASD move   Space/Ctrl height   Shift fast   R recenter on enemy");
}

void CHudDuel1v1::Button(int x, int y, int w, int h, const char *label, int action, int arg, bool enabled, bool highlight)
{
	const bool hover = m_cursorX >= x && m_cursorX < x + w && m_cursorY >= y && m_cursorY < y + h;
	const int a = enabled ? (hover ? 235 : 180) : 90;
	if (highlight) FillRGBA(x, y, w, h, 50, 120, 60, a);
	else           FillRGBA(x, y, w, h, 34, 38, 52, a);

	const float c = enabled ? 1.0f : 0.5f;
	PutText(x + (w - TextWidth(label)) / 2, y + (h - 14) / 2, c, c, c, label);

	if (enabled && m_nHit < DUEL_MAX_HITS)
	{
		Hit hit = { x, y, w, h, action, arg };
		m_hit[m_nHit++] = hit;
	}
}

void CHudDuel1v1::DrawMenu(void)
{
	m_pw = 500;
	m_ph = 400;
	m_px = 30;
	m_py = (ScreenHeight - m_ph) / 2;
	if (m_py < 10) m_py = 10;

	const int px = m_px, py = m_py, pw = m_pw, ph = m_ph;
	const bool host = IsHost();
	const bool locked = (m_state == DUEL_COUNTDOWN || m_state == DUEL_LIVE);

	FillRGBA(px, py, pw, ph, 8, 10, 16, 220);
	FillRGBA(px, py, pw, 24, 40, 90, 160, 235);
	PutText(px + 10, py + 5, 1.0f, 1.0f, 1.0f, "1v1 MODE SETTINGS");
	Button(px + pw - 28, py + 3, 22, 18, "X", A_CLOSE, 0, true, false);

	// tabs: one per section, then Weapons, then Presets
	const int ntabs = m_nSec + 2;
	const int tw = (pw - 20) / ntabs;
	for (int t = 0; t < ntabs; t++)
	{
		const char *lbl = (t < m_nSec) ? m_secName[t] : (t == m_nSec ? "Weapons" : "Presets");
		Button(px + 10 + t * tw, py + 30, tw - 4, 22, lbl, A_TAB, t, true, t == m_tab);
	}
	if (m_tab >= ntabs) m_tab = 0;

	int y = py + 66;
	const int rowH = 26;

	if (m_tab < m_nSec)
	{
		for (int i = 0; i < DUEL_NUM_OPTS; i++)
		{
			const Duel1v1Opt &o = g_Duel1v1Opts[i];
			if (strcmp(o.section, m_secName[m_tab]) != 0)
				continue;

			const bool canEdit = host && (!locked || !strcmp(o.cvar, "ss_1v1"));
			PutText(px + 16, y + 3, 0.9f, 0.9f, 0.9f, o.label);

			if (o.type == DOPT_BOOL)
			{
				Button(px + pw - 120, y, 100, 20, m_cfg[i] != 0.0f ? "ON" : "OFF", A_TOGGLE, i, canEdit, m_cfg[i] != 0.0f);
			}
			else
			{
				char v[32];
				if (o.type == DOPT_INT) snprintf(v, sizeof(v), "%d", (int)(m_cfg[i] + 0.5f));
				else                    snprintf(v, sizeof(v), "%g", m_cfg[i]);
				Button(px + pw - 200, y, 24, 20, "-", A_DEC, i, canEdit, false);
				FillRGBA(px + pw - 172, y, 100, 20, 20, 20, 26, 200);
				PutText(px + pw - 172 + (100 - TextWidth(v)) / 2, y + 3, 1.0f, 1.0f, 1.0f, v);
				Button(px + pw - 68, y, 24, 20, "+", A_INC, i, canEdit, false);
			}
			y += rowH;
		}
	}
	else if (m_tab == m_nSec) // weapons
	{
		const bool canEdit = host && !locked;
		for (int i = 0; i < DUEL_NUM_WEAPONS; i++)
		{
			const bool on = (m_weaponMask & (1 << i)) != 0;
			PutText(px + 16, y + 3, 0.9f, 0.9f, 0.9f, g_Duel1v1Weapons[i].label);
			Button(px + pw - 120, y, 100, 20, on ? "ALLOWED" : "BANNED", A_WEAPON, i, canEdit, on);
			y += rowH;
		}
	}
	else // presets
	{
		const bool canEdit = host && !locked;
		PutText(px + 16, y, 0.7f, 0.7f, 0.7f, "Loads 1v1_<name>.cfg on the server");
		y += rowH;
		for (int i = 0; i < DUEL_NUM_PRESETS; i++)
		{
			Button(px + 16, y, 200, 22, g_Duel1v1Presets[i], A_PRESET, i, canEdit, false);
			y += rowH;
		}
	}

	// footer
	const int fy = py + ph - 40;
	FillRGBA(px + 8, fy - 8, pw - 16, 1, 80, 90, 110, 200);

	if (!host)
		PutText(px + 16, fy - 26, 1.0f, 0.7f, 0.4f, "Only the host can change settings");
	else if (locked)
		PutText(px + 16, fy - 26, 1.0f, 0.7f, 0.4f, "Settings are locked during a match");
	else if (Cfg("ss_1v1") == 0.0f)
		PutText(px + 16, fy - 26, 1.0f, 0.7f, 0.4f, "1v1 mode is OFF - enable it in the Match tab");

	if (m_state == DUEL_WAITING && (m_flags & 8))
		Button(px + 16, fy, 200, 26, (m_flags & 1) ? "READY (click to cancel)" : "READY", A_READY, 0, true, (m_flags & 1) != 0);
	if (host && (m_state == DUEL_LIVE || m_state == DUEL_COUNTDOWN))
		Button(px + pw - 156, fy, 140, 26, "STOP MATCH", A_STOP, 0, true, false);
}
