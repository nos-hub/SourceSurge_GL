// hud_jumpstats.cpp - client side HUD for the server side jumpstats (cl_dll/)
//
// The server (dlls/jumpstats.cpp) sends the raw jump data in the "JumpStat" user message, this file formats and
// draws it. Every block is its own widget in the HUD editor (hud_edit): drag it, hide it with right click.
//   HUD_JS_INFO     jump type, distance, prestrafe, OL/DA, airpath, strafes, loss, potency...
//   HUD_JS_GRAPH    strafe key / mouse / duck graph (one coloured cell per frame)
//   HUD_JS_STRAFES  per strafe table (sync, gain, loss, max, air, OL/DA, efficiency)
//   HUD_JS_SPEED    speed and the prestrafe in brackets while in the air (the normal speedometer is separate)
//
// Client cvars (all archived):
//   js_hud_time         seconds the jump info stays on screen after a jump         (default 3)
//   js_hud_graph_frames how many of the last frames the graph shows                (default 65)
//   js_hud_vertical     1 = one stat per line in the jump info block               (default 0)
//
// Based on the HUD of GCLJS by GameChaos (https://codeberg.org/GameChaos/gcljs).

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ss_hud_editor_v2.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

DECLARE_MESSAGE(m_JumpStats, JumpStat)

// message types, same as JsMsgType in dlls/jumpstats.cpp
enum
{
	JSMSG_INFO = 0,
	JSMSG_STRAFES = 1,
	JSMSG_GRAPH = 2,
	JSMSG_PRESPEED = 3,
	JSMSG_CLEAR = 4
};

// info flags, same as JsInfoFlags in dlls/jumpstats.cpp
enum
{
	JSINFO_FAILED = 1,
	JSINFO_BLOCK = 2,
	JSINFO_EDGE = 4,
	JSINFO_LANDEDGE = 8
};

// MUST be in the same order as JumpType in dlls/jumpstats.cpp
static const char* const g_jsTypeNames[] = {
	"NONE", "DD", "GS", "LJ", "CJ", "DCJ", "MCJ", "SCJ", "WJ", "LDJ", "BH", "DBH", "SBJ", "SLJ"};
#define JS_TYPE_COUNT ((int)(sizeof(g_jsTypeNames) / sizeof(g_jsTypeNames[0])))

// StrafeType, same order as dlls/jumpstats.cpp
enum
{
	ST_OVERLAP = 0,
	ST_NONE,
	ST_LEFT,
	ST_OVERLAP_LEFT,
	ST_NONE_LEFT,
	ST_RIGHT,
	ST_OVERLAP_RIGHT,
	ST_NONE_RIGHT
};

#define GRAPH_CELL_W 3
#define GRAPH_CELL_H 4
#define GRAPH_STEP_X (GRAPH_CELL_W + 1)
#define GRAPH_STEP_Y (GRAPH_CELL_H + 1)
#define GRAPH_ROWS 5

static int JsDrawText(int x, int y, int r, int g, int b, const char* text)
{
	gEngfuncs.pfnDrawSetTextColor(r / 255.0f, g / 255.0f, b / 255.0f);
	return gEngfuncs.pfnDrawConsoleString(x, y, (char*)text);
}

// Current horizontal speed of the local player. Same source as hud_speedometer.cpp.
static float JsGetLocalSpeed()
{
	const Vector& vel = gHUD.m_vecPlayerVelocity;
	return sqrtf(vel.x * vel.x + vel.y * vel.y);
}

bool CHudJumpStats::Init()
{
	memset(&m_info, 0, sizeof(m_info));
	memset(m_strafes, 0, sizeof(m_strafes));
	memset(m_graph, 0, sizeof(m_graph));
	m_strafeCount = 0;
	m_graphLen = 0;
	m_bHaveInfo = false;
	m_flJumpTime = -100.0f;
	m_bPrespeed = false;
	m_flPrespeed = 0.0f;
	m_iCharW = 8;
	m_iLineH = 14;

	m_pCvarHold = CVAR_CREATE("js_hud_time", "3", FCVAR_ARCHIVE);
	m_pCvarFrames = CVAR_CREATE("js_hud_graph_frames", "65", FCVAR_ARCHIVE);
	m_pCvarVertical = CVAR_CREATE("js_hud_vertical", "0", FCVAR_ARCHIVE);

	HOOK_MESSAGE(JumpStat);

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return true;
}

bool CHudJumpStats::VidInit()
{
	// the new level has no jump yet
	m_bHaveInfo = false;
	m_flJumpTime = -100.0f;
	m_bPrespeed = false;
	m_strafeCount = 0;
	m_graphLen = 0;

	int w = 0, h = 0;
	gEngfuncs.pfnDrawConsoleStringLen("0123456789", &w, &h);
	m_iCharW = w > 0 ? w / 10 : 8;
	m_iLineH = (h > 0 ? h : 12) + 2;
	return true;
}

bool CHudJumpStats::MsgFunc_JumpStat(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const int type = READ_BYTE();

	switch (type)
	{
	case JSMSG_INFO:
	{
		m_info.type = READ_BYTE();
		m_info.flags = READ_BYTE();
		m_info.strafes = READ_BYTE();
		m_info.dir = READ_BYTE();
		m_info.distance = READ_LONG() / 10000.0f;
		m_info.xjDistance = READ_LONG() / 10000.0f;
		m_info.blockDist = READ_LONG() / 1000.0f;
		m_info.edge = READ_LONG() / 10000.0f;
		m_info.landEdge = READ_LONG() / 10000.0f;
		m_info.veer = READ_LONG() / 10000.0f;
		m_info.airpath = READ_LONG() / 10000.0f;
		m_info.prespeed = READ_LONG() / 100.0f;
		m_info.maxspeed = READ_LONG() / 100.0f;
		m_info.sync = READ_SHORT() / 100.0f;
		m_info.loss = READ_SHORT() / 100.0f;
		m_info.potency = READ_SHORT() / 100.0f;
		m_info.jumpoffAngle = READ_SHORT() / 100.0f;
		m_info.height = READ_SHORT() / 100.0f;
		m_info.airtimeFrac = READ_SHORT() / 100.0f;
		m_info.airtime = READ_SHORT();
		m_info.overlap = READ_SHORT();
		m_info.deadair = READ_SHORT();
		m_info.fwdRelease = READ_SHORT();

		// the strafe table and the graph follow in their own messages
		m_strafeCount = 0;
		m_graphLen = 0;
		m_bHaveInfo = true;
		m_flJumpTime = gEngfuncs.GetClientTime();
		break;
	}

	case JSMSG_STRAFES:
	{
		const int start = READ_BYTE();
		const int count = READ_BYTE();
		const int total = READ_BYTE();
		for (int i = 0; i < count; i++)
		{
			StrafeRow row;
			row.sync = READ_SHORT() / 10.0f;
			row.gain = READ_SHORT() / 10.0f;
			row.loss = READ_SHORT() / 10.0f;
			row.max = READ_SHORT() / 10.0f;
			row.air = READ_BYTE();
			row.overlap = READ_BYTE();
			row.deadair = READ_BYTE();
			row.avgEff = READ_SHORT();
			row.peakEff = READ_SHORT();
			if (start + i < JS_HUD_MAX_STRAFES)
				m_strafes[start + i] = row;
		}
		m_strafeCount = total < JS_HUD_MAX_STRAFES ? total : JS_HUD_MAX_STRAFES;
		break;
	}

	case JSMSG_GRAPH:
	{
		const int start = READ_BYTE();
		const int count = READ_BYTE();
		const int total = READ_BYTE();
		for (int i = 0; i < count; i++)
		{
			const int b = READ_BYTE();
			if (start + i < JS_HUD_MAX_FRAMES)
				m_graph[start + i] = (unsigned char)b;
		}
		m_graphLen = total < JS_HUD_MAX_FRAMES ? total : JS_HUD_MAX_FRAMES;
		break;
	}

	case JSMSG_PRESPEED:
	{
		m_bPrespeed = READ_BYTE() != 0;
		m_flPrespeed = READ_LONG() / 100.0f;
		break;
	}

	case JSMSG_CLEAR:
		m_bHaveInfo = false;
		m_flJumpTime = -100.0f;
		m_bPrespeed = false;
		break;

	default:
		break;
	}

	return true;
}

bool CHudJumpStats::Draw(float flTime)
{
	// the editor draws a box for every widget while it's open
	if (gHUD.m_Editor->IsActive())
		return true;

	const float now = gEngfuncs.GetClientTime();
	const float hold = m_pCvarHold ? m_pCvarHold->value : 3.0f;
	const bool showStats = m_bHaveInfo && (now - m_flJumpTime) < hold;

	int x, y;
	if (showStats)
	{
		if (gHUD.m_Editor->Visible(HUD_JS_INFO))
		{
			gHUD.m_Editor->GetPos(HUD_JS_INFO, x, y);
			DrawInfo(x, y);
		}
		if (gHUD.m_Editor->Visible(HUD_JS_GRAPH))
		{
			gHUD.m_Editor->GetPos(HUD_JS_GRAPH, x, y);
			DrawGraph(x, y);
		}
		if (gHUD.m_Editor->Visible(HUD_JS_STRAFES))
		{
			gHUD.m_Editor->GetPos(HUD_JS_STRAFES, x, y);
			DrawStrafes(x, y);
		}
	}

	if (gHUD.m_Editor->Visible(HUD_JS_SPEED))
	{
		gHUD.m_Editor->GetPos(HUD_JS_SPEED, x, y);
		DrawSpeed(x, y);
	}

	return true;
}

void CHudJumpStats::DrawInfo(int x, int y)
{
	const Info& in = m_info;
	const bool failed = (in.flags & JSINFO_FAILED) != 0;
	const bool hasBlock = (in.flags & JSINFO_BLOCK) != 0;
	const char* name = (in.type >= 0 && in.type < JS_TYPE_COUNT) ? g_jsTypeNames[in.type] : "?";

	const int tr = failed ? 255 : 60;
	const int tg = failed ? 60 : 255;
	const int tb = failed ? 60 : 120;

	char line[256];
	snprintf(line, sizeof(line), "%s%s: %.5f (XJ: %.5f)", failed ? "FAILED " : "", name, in.distance, in.xjDistance);
	JsDrawText(x, y, tr, tg, tb, line);
	y += m_iLineH;

	if (m_pCvarVertical && m_pCvarVertical->value != 0.0f)
	{
		// one stat per line
		if (hasBlock)
		{
			snprintf(line, sizeof(line), "Block: %.3f", in.blockDist);
			JsDrawText(x, y, 255, 255, 255, line);
			y += m_iLineH;
		}
		snprintf(line, sizeof(line), "Pre: %.2f", in.prespeed);
		JsDrawText(x, y, 255, 255, 255, line);
		y += m_iLineH;
		snprintf(line, sizeof(line), "OL/DA: %i/%i", in.overlap, in.deadair);
		JsDrawText(x, y, 255, 255, 255, line);
		y += m_iLineH;
		snprintf(line, sizeof(line), "Jumpoff Angle: %.2f", in.jumpoffAngle);
		JsDrawText(x, y, 255, 255, 255, line);
		y += m_iLineH;
		snprintf(line, sizeof(line), "Airpath: %.4f", in.airpath);
		JsDrawText(x, y, 255, 255, 255, line);
		y += m_iLineH;
		snprintf(line, sizeof(line), "Strafes: %i", in.strafes);
		JsDrawText(x, y, 255, 255, 255, line);
		y += m_iLineH;
		snprintf(line, sizeof(line), "Loss: %.2f", in.loss);
		JsDrawText(x, y, 255, 255, 255, line);
		y += m_iLineH;
		snprintf(line, sizeof(line), "Potency: %.2f", in.potency);
		JsDrawText(x, y, 255, 255, 255, line);
		return;
	}

	if (hasBlock)
		snprintf(line, sizeof(line), "[Block: %.3f | Pre: %.2f | OL/DA: %i/%i | Jumpoff Angle: %.2f]",
			in.blockDist, in.prespeed, in.overlap, in.deadair, in.jumpoffAngle);
	else
		snprintf(line, sizeof(line), "[Pre: %.2f | OL/DA: %i/%i | Jumpoff Angle: %.2f]",
			in.prespeed, in.overlap, in.deadair, in.jumpoffAngle);
	JsDrawText(x, y, 255, 255, 255, line);
	y += m_iLineH;

	snprintf(line, sizeof(line), "[Airpath: %.4f | Strafes: %i | Loss: %.2f | Potency: %.2f]",
		in.airpath, in.strafes, in.loss, in.potency);
	JsDrawText(x, y, 255, 255, 255, line);
}

void CHudJumpStats::DrawGraph(int x, int y)
{
	if (m_graphLen <= 0)
		return;

	int frames = m_pCvarFrames ? (int)m_pCvarFrames->value : 65;
	if (frames < 16)
		frames = 16;
	if (frames > JS_HUD_MAX_FRAMES)
		frames = JS_HUD_MAX_FRAMES;

	const int n = m_graphLen < frames ? m_graphLen : frames;
	const int first = m_graphLen - n;

	// rows: 0 = left key, 1 = turning left, 2 = right key, 3 = turning right, 4 = duck
	for (int i = 0; i < n; i++)
	{
		const int b = m_graph[first + i];
		const int type = b & 7;
		const int mouse = (b >> 3) & 3;
		const int duck = (b >> 5) & 1;
		const int cx = x + i * GRAPH_STEP_X;

		// faint background so the empty frames are visible
		for (int row = 0; row < GRAPH_ROWS; row++)
			gEngfuncs.pfnFillRGBA(cx, y + row * GRAPH_STEP_Y, GRAPH_CELL_W, GRAPH_CELL_H, 255, 255, 255, 22);

		const int ky0 = y + 0 * GRAPH_STEP_Y;
		const int ky2 = y + 2 * GRAPH_STEP_Y;

		switch (type)
		{
		case ST_LEFT:
			gEngfuncs.pfnFillRGBA(cx, ky0, GRAPH_CELL_W, GRAPH_CELL_H, 255, 60, 60, 235);
			break;
		case ST_RIGHT:
			gEngfuncs.pfnFillRGBA(cx, ky2, GRAPH_CELL_W, GRAPH_CELL_H, 0, 220, 255, 235);
			break;
		case ST_OVERLAP:
		case ST_OVERLAP_LEFT:
		case ST_OVERLAP_RIGHT:
			// both keys held
			gEngfuncs.pfnFillRGBA(cx, ky0, GRAPH_CELL_W, GRAPH_CELL_H, 255, 220, 0, 235);
			gEngfuncs.pfnFillRGBA(cx, ky2, GRAPH_CELL_W, GRAPH_CELL_H, 255, 220, 0, 235);
			break;
		case ST_NONE_LEFT:
			// no key, but still moving left
			gEngfuncs.pfnFillRGBA(cx, ky0, GRAPH_CELL_W, GRAPH_CELL_H, 255, 60, 60, 70);
			break;
		case ST_NONE_RIGHT:
			gEngfuncs.pfnFillRGBA(cx, ky2, GRAPH_CELL_W, GRAPH_CELL_H, 0, 220, 255, 70);
			break;
		default:
			break;
		}

		if (mouse == 1)
			gEngfuncs.pfnFillRGBA(cx, y + 1 * GRAPH_STEP_Y, GRAPH_CELL_W, GRAPH_CELL_H, 80, 255, 120, 235);
		else if (mouse == 2)
			gEngfuncs.pfnFillRGBA(cx, y + 3 * GRAPH_STEP_Y, GRAPH_CELL_W, GRAPH_CELL_H, 80, 255, 120, 235);

		if (duck)
			gEngfuncs.pfnFillRGBA(cx, y + 4 * GRAPH_STEP_Y, GRAPH_CELL_W, GRAPH_CELL_H, 255, 60, 255, 235);
	}
}

void CHudJumpStats::DrawStrafes(int x, int y)
{
	if (m_strafeCount <= 0)
		return;

	// column starts, in characters
	static const int col[] = {0, 3, 9, 15, 22, 28, 33, 40};
	static const char* const head[] = {"#", "Sync", "Gain", "Loss", "Max", "Air", "OL/DA", "Eff (Max)"};

	for (int c = 0; c < 8; c++)
		JsDrawText(x + col[c] * m_iCharW, y, 0, 255, 255, head[c]);
	y += m_iLineH;

	for (int i = 0; i < m_strafeCount; i++)
	{
		const StrafeRow& r = m_strafes[i];
		char cell[32];

		snprintf(cell, sizeof(cell), "%i", i + 1);
		JsDrawText(x + col[0] * m_iCharW, y, 255, 255, 255, cell);
		snprintf(cell, sizeof(cell), "%.0f%%", r.sync);
		JsDrawText(x + col[1] * m_iCharW, y, 255, 255, 255, cell);
		snprintf(cell, sizeof(cell), "%.0f", r.gain);
		JsDrawText(x + col[2] * m_iCharW, y, 80, 255, 120, cell);
		snprintf(cell, sizeof(cell), "%.1f", r.loss);
		JsDrawText(x + col[3] * m_iCharW, y, 255, 80, 80, cell);
		snprintf(cell, sizeof(cell), "%.0f", r.max);
		JsDrawText(x + col[4] * m_iCharW, y, 255, 255, 255, cell);
		snprintf(cell, sizeof(cell), "%i", r.air);
		JsDrawText(x + col[5] * m_iCharW, y, 255, 255, 255, cell);
		snprintf(cell, sizeof(cell), "%i/%i", r.overlap, r.deadair);
		JsDrawText(x + col[6] * m_iCharW, y, 255, 255, 255, cell);
		snprintf(cell, sizeof(cell), "%i%% (%i%%)", r.avgEff, r.peakEff);
		JsDrawText(x + col[7] * m_iCharW, y, 255, 255, 255, cell);

		y += m_iLineH;
	}
}

void CHudJumpStats::DrawSpeed(int x, int y)
{
	// both lines are centred on the middle of the widget box (80 px wide in the editor registration)
	const int cx = x + 40;

	char buf[32];
	int w = 0, h = 0;

	snprintf(buf, sizeof(buf), "%.2f", JsGetLocalSpeed());
	gEngfuncs.pfnDrawConsoleStringLen(buf, &w, &h);
	JsDrawText(cx - w / 2, y, 255, 255, 255, buf);

	if (m_bPrespeed)
	{
		snprintf(buf, sizeof(buf), "(%.2f)", m_flPrespeed);
		gEngfuncs.pfnDrawConsoleStringLen(buf, &w, &h);
		JsDrawText(cx - w / 2, y + m_iLineH, 255, 255, 255, buf);
	}
}