// hud_hitfeedback.cpp - hitsounds + hitmarkers (client).
//
// Sounds: <gamedir>/sound/hitsounds/<name>.wav
//   hit.wav       normal hit      (cvar hs_hit,  default "hit")
//   headshot.wav  headshot        (cvar hs_head, default "headshot")
//   kill.wav      kill            (cvar hs_kill, default "kill")
// Priority on a hit: kill -> headshot -> hit. A missing file just falls through
// to the next one, and if nothing exists there is simply no sound.
// Use 16-bit PCM mono .wav files (stereo / compressed wavs may not play).

#include <filesystem>
#include <string>

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include <stdio.h>
#include <string.h>

DECLARE_MESSAGE(m_HitFeedback, HitMark)
DECLARE_COMMAND(m_HitFeedback, HitSoundReload)
DECLARE_COMMAND(m_HitFeedback, HitSoundList)
DECLARE_COMMAND(m_HitFeedback, HitTest)

// ---------------------------------------------------------------------------

// Turns a cvar value into a safe sound name: no path tricks, no ".wav" suffix.
static void CleanSoundName(const char* in, char* out, int outSize)
{
	out[0] = 0;
	if (!in || !in[0])
		return;
	if (strstr(in, "..") || strchr(in, '\\') || strchr(in, ':'))
		return;

	strncpy(out, in, outSize - 1);
	out[outSize - 1] = 0;

	int len = (int)strlen(out);
	if (len > 4 && out[len - 4] == '.' && (out[len - 3] | 32) == 'w' && (out[len - 2] | 32) == 'a' && (out[len - 1] | 32) == 'v')
		out[len - 4] = 0;
}

static bool IsWavFile(const char* file)
{
	int len = (int)strlen(file);
	return len > 4 && file[len - 4] == '.' && (file[len - 3] | 32) == 'w' && (file[len - 2] | 32) == 'a' && (file[len - 1] | 32) == 'v';
}

static void PrintWavName(const char* file)
{
	char name[128];
	strncpy(name, file, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	name[strlen(name) - 4] = 0; // strip .wav
	gEngfuncs.Con_Printf("  %s\n", name);
}

// ---------------------------------------------------------------------------

bool CHudHitFeedback::Init()
{
	m_iFlags |= HUD_ACTIVE;
	m_flHitTime = 0;
	m_iHitType = SLOT_HIT;
	memset(m_Slots, 0, sizeof(m_Slots));

	HOOK_MESSAGE(HitMark);
	HOOK_COMMAND("hitsound_reload", HitSoundReload);
	HOOK_COMMAND("hitsound_list", HitSoundList);
	HOOK_COMMAND("hit_test", HitTest);

	m_pSoundEnable = CVAR_CREATE("hs_enable", "1", FCVAR_ARCHIVE);
	m_pSoundVolume = CVAR_CREATE("hs_volume", "0.8", FCVAR_ARCHIVE);
	m_pSoundCvar[SLOT_HIT] = CVAR_CREATE("hs_hit", "hit", FCVAR_ARCHIVE);
	m_pSoundCvar[SLOT_HEAD] = CVAR_CREATE("hs_head", "headshot", FCVAR_ARCHIVE);
	m_pSoundCvar[SLOT_KILL] = CVAR_CREATE("hs_kill", "kill", FCVAR_ARCHIVE);

	m_pMarkerEnable = CVAR_CREATE("hm_enable", "1", FCVAR_ARCHIVE);
	m_pMarkerSize = CVAR_CREATE("hm_size", "8", FCVAR_ARCHIVE);
	m_pMarkerGap = CVAR_CREATE("hm_gap", "4", FCVAR_ARCHIVE);
	m_pMarkerThick = CVAR_CREATE("hm_thickness", "2", FCVAR_ARCHIVE);
	m_pMarkerTime = CVAR_CREATE("hm_time", "0.25", FCVAR_ARCHIVE);
	m_pColorHit = CVAR_CREATE("hm_color", "255 255 255", FCVAR_ARCHIVE);
	m_pColorHead = CVAR_CREATE("hm_color_head", "255 200 0", FCVAR_ARCHIVE);
	m_pColorKill = CVAR_CREATE("hm_color_kill", "255 40 40", FCVAR_ARCHIVE);

	gHUD.AddHudElem(this);
	return true;
}

bool CHudHitFeedback::VidInit()
{
	m_flHitTime = 0;
	ReloadSounds();
	return true;
}

// ---------------------------------------------------------------------------
// sounds

bool CHudHitFeedback::SoundExists(const char* name)
{
	char path[160];
	snprintf(path, sizeof(path), "sound/hitsounds/%s.wav", name);

	byte* f = gEngfuncs.COM_LoadFile(path, 5, NULL);
	if (!f)
		return false;

	gEngfuncs.COM_FreeFile(f);
	return true;
}

void CHudHitFeedback::ResolveSlot(int slot, bool force)
{
	char clean[64];
	CleanSoundName(m_pSoundCvar[slot]->string, clean, sizeof(clean));

	if (!force && strcmp(clean, m_Slots[slot].name) == 0)
		return;

	strcpy(m_Slots[slot].name, clean);
	m_Slots[slot].ok = clean[0] && SoundExists(clean);
}

void CHudHitFeedback::ReloadSounds()
{
	for (int i = 0; i < NUM_SLOTS; i++)
		ResolveSlot(i, true);
}

void CHudHitFeedback::PlayHitSound(bool kill, bool head)
{
	if (m_pSoundEnable->value == 0)
		return;

	// picks up cvar changes (hs_hit "mysound") without needing a reload
	for (int i = 0; i < NUM_SLOTS; i++)
		ResolveSlot(i, false);

	int slot = -1;
	if (kill && m_Slots[SLOT_KILL].ok)
		slot = SLOT_KILL;
	else if (head && m_Slots[SLOT_HEAD].ok)
		slot = SLOT_HEAD;
	else if (m_Slots[SLOT_HIT].ok)
		slot = SLOT_HIT;

	if (slot < 0)
		return;

	float vol = m_pSoundVolume->value;
	if (vol < 0)
		vol = 0;
	if (vol > 1)
		vol = 1;

	char path[128];
	snprintf(path, sizeof(path), "hitsounds/%s.wav", m_Slots[slot].name);
	gEngfuncs.pfnPlaySoundByName(path, vol);
}

// ---------------------------------------------------------------------------
// events

void CHudHitFeedback::OnHit(bool kill, bool head)
{
	m_flHitTime = gHUD.m_flTime;
	m_iHitType = kill ? SLOT_KILL : (head ? SLOT_HEAD : SLOT_HIT);
	PlayHitSound(kill, head);
}

bool CHudHitFeedback::MsgFunc_HitMark(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	int damage = READ_SHORT(); // not used yet (handy for damage numbers later)
	int flags = READ_BYTE();
	(void)damage;

	OnHit((flags & 1) != 0, (flags & 2) != 0);
	return true;
}

// ---------------------------------------------------------------------------
// drawing

void CHudHitFeedback::ParseColor(cvar_t* cv, int& r, int& g, int& b, int defR, int defG, int defB)
{
	r = defR;
	g = defG;
	b = defB;

	int rr, gg, bb;
	if (cv && sscanf(cv->string, "%d %d %d", &rr, &gg, &bb) == 3)
	{
		r = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
		g = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
		b = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
	}
}

bool CHudHitFeedback::Draw(float flTime)
{
	if (m_flHitTime <= 0 || m_pMarkerEnable->value == 0)
		return true;

	float dur = m_pMarkerTime->value;
	if (dur < 0.05f)
		dur = 0.05f;

	float t = (flTime - m_flHitTime) / dur;
	if (t < 0 || t >= 1.0f)
	{
		m_flHitTime = 0; // expired (or the clock was reset by a map change)
		return true;
	}

	int r, g, b;
	if (m_iHitType == SLOT_KILL)
		ParseColor(m_pColorKill, r, g, b, 255, 40, 40);
	else if (m_iHitType == SLOT_HEAD)
		ParseColor(m_pColorHead, r, g, b, 255, 200, 0);
	else
		ParseColor(m_pColorHit, r, g, b, 255, 255, 255);

	int size = (int)m_pMarkerSize->value;
	int gap = (int)m_pMarkerGap->value;
	int thick = (int)m_pMarkerThick->value;
	if (size < 1)
		size = 1;
	if (size > 64)
		size = 64;
	if (gap < 0)
		gap = 0;
	if (thick < 1)
		thick = 1;
	if (thick > 8)
		thick = 8;

	if (m_iHitType == SLOT_KILL)
		size = size * 3 / 2; // kills get a bigger marker

	gap += (int)(t * 3.0f); // small "pop" outwards while it fades
	int alpha = (int)(255.0f * (1.0f - t));

	int cx = ScreenWidth / 2;
	int cy = ScreenHeight / 2;

	// FillRGBA only draws rectangles, so each diagonal arm is a row of small squares
	for (int i = 0; i < size; i++)
	{
		int d = gap + i;
		int h = thick / 2;

		gEngfuncs.pfnFillRGBA(cx - d - h, cy - d - h, thick, thick, r, g, b, alpha);
		gEngfuncs.pfnFillRGBA(cx + d - h, cy - d - h, thick, thick, r, g, b, alpha);
		gEngfuncs.pfnFillRGBA(cx - d - h, cy + d - h, thick, thick, r, g, b, alpha);
		gEngfuncs.pfnFillRGBA(cx + d - h, cy + d - h, thick, thick, r, g, b, alpha);
	}

	return true;
}

// ---------------------------------------------------------------------------
// commands

void CHudHitFeedback::UserCmd_HitSoundReload()
{
	ReloadSounds();

	gEngfuncs.Con_Printf("hitsounds: hit=%s%s head=%s%s kill=%s%s\n",
		m_Slots[SLOT_HIT].name, m_Slots[SLOT_HIT].ok ? "" : " (missing)",
		m_Slots[SLOT_HEAD].name, m_Slots[SLOT_HEAD].ok ? "" : " (missing)",
		m_Slots[SLOT_KILL].name, m_Slots[SLOT_KILL].ok ? "" : " (missing)");
}

void CHudHitFeedback::UserCmd_HitSoundList()
{
	char dir[256];
	snprintf(dir, sizeof(dir), "%s/sound/hitsounds", gEngfuncs.pfnGetGameDirectory());
	gEngfuncs.Con_Printf("Hitsounds in %s (use the names with hs_hit / hs_head / hs_kill):\n", dir);

	int count = 0;

	std::error_code ec;
	std::filesystem::directory_iterator it(dir, ec), end;
	for (; !ec && it != end; it.increment(ec))
	{
		std::string file = it->path().filename().string();
		if (IsWavFile(file.c_str()))
		{
			PrintWavName(file.c_str());
			count++;
		}
	}

	if (count == 0)
		gEngfuncs.Con_Printf("  (no .wav files found, create the folder and drop some in)\n");
}

// hit_test [hit|head|kill]  -> fakes a hit so you can preview sound + marker
void CHudHitFeedback::UserCmd_HitTest()
{
	const char* arg = gEngfuncs.Cmd_Argv(1);
	bool kill = arg && !strcmp(arg, "kill");
	bool head = arg && !strcmp(arg, "head");
	OnHit(kill, head);
}