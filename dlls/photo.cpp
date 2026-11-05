// photo.cpp - photo camera state, upload relay and photo spray (server side, dlls/)
//
// Flow:
//  1. weapon_camera fires -> Photo_Shutter() gives the player a new photo id and sends "PhotoCap" to THAT client.
//  2. The client captures the screen, shrinks it and uploads it with "ss_photo <id> <chunk> <text>" commands.
//     Each valid chunk is stored here and relayed to every client with "PhotoDat".
//  3. Impulse 201 (spray) -> Photo_TrySpray(). If the player has a photo it is sent as "PhotoSpr"
//     (position, normal, yaw) and USED UP; the next spray is the normal one again.
//
// One photo per player at a time: taking a new picture replaces the old one.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "UserMessages.h"
#include "shake.h"
#include "photo.h"
#include "photo_shared.h"

#include <stdlib.h>
#include <string.h>

constexpr float PHOTO_UPLOAD_TIMEOUT = 4.0f; // seconds the server waits for the upload before dropping the photo

// Camera flash on other players (flashbang style)
constexpr float FLASH_RANGE = 1200.0f;		 // units; further away nobody is hit
constexpr float FLASH_FULL_RANGE = 300.0f;	 // inside this distance there's no distance falloff
constexpr float FLASH_AIM_MIN_DOT = 0.75f;	 // camera must point at the victim (cos of ~41 degrees)
constexpr float FLASH_FACE_MIN_DOT = 0.25f;	 // victim must look roughly towards the camera (cos of ~75 degrees)
constexpr float FLASH_FACE_FULL_DOT = 0.90f; // looking right at the camera = full effect
constexpr float FLASH_HOLD_MAX = 0.1f;		 // seconds of full white at full effect
constexpr float FLASH_FADE_MIN = 0.4f;		 // fade-out time at the weakest effect
constexpr float FLASH_FADE_MAX = 2.5f;		 // fade-out time at full effect

namespace
{
struct PhotoSlot
{
	bool hasPhoto = false;
	bool complete = false;
	short id = 0;	  // id of the current photo (0 = none yet)
	short lastId = 0; // last id handed out
	int nRecv = 0;
	float shotTime = 0;
	bool got[PHOTO_CHUNKS] = {};
	unsigned char data[PHOTO_BYTES] = {};
};

PhotoSlot g_Slots[33]; // indexed by entity index (1..32)

PhotoSlot* SlotFor(int idx)
{
	if (idx < 1 || idx > gpGlobals->maxClients || idx >= 33)
		return nullptr;
	return &g_Slots[idx];
}

PhotoSlot* SlotFor(CBasePlayer* p)
{
	return p ? SlotFor(ENTINDEX(p->edict())) : nullptr;
}
} // namespace

void Photo_ResetAll()
{
	for (auto& s : g_Slots)
		s = PhotoSlot();
}

void Photo_PlayerDisconnect(int iPlayerIndex)
{
	if (PhotoSlot* s = SlotFor(iPlayerIndex))
	{
		const short keep = s->lastId;
		*s = PhotoSlot();
		s->lastId = keep;
	}
}

bool Photo_HasPhoto(CBasePlayer* pPlayer)
{
	PhotoSlot* s = SlotFor(pPlayer);
	return s && s->hasPhoto;
}

// Everybody who is in front of the camera and looking at it gets flashed.
// The more directly they look at the camera (and the closer they are), the stronger it is:
// turning away weakens it, turning fully away avoids it.
static void FlashPlayersInFront(CBasePlayer* pShooter)
{
	UTIL_MakeVectors(pShooter->pev->v_angle);
	const Vector camPos = pShooter->EyePosition();
	const Vector camDir = gpGlobals->v_forward;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pVictim = static_cast<CBasePlayer*>(UTIL_PlayerByIndex(i));
		if (!pVictim || pVictim == pShooter || !pVictim->IsAlive() || !pVictim->IsNetClient())
			continue;
		if (pVictim->pev->flags & FL_SPECTATOR)
			continue;

		const Vector victimPos = pVictim->EyePosition();
		Vector toVictim = victimPos - camPos;
		const float dist = toVictim.Length();
		if (dist < 1.0f || dist > FLASH_RANGE)
			continue;
		toVictim = toVictim * (1.0f / dist);

		// the camera has to point at the victim
		if (DotProduct(camDir, toVictim) < FLASH_AIM_MIN_DOT)
			continue;

		// the victim has to look towards the camera
		UTIL_MakeVectors(pVictim->pev->v_angle);
		const float facing = DotProduct(gpGlobals->v_forward, toVictim * -1.0f);
		if (facing < FLASH_FACE_MIN_DOT)
			continue;

		// nothing in between
		TraceResult tr;
		UTIL_TraceLine(camPos, victimPos, ignore_monsters, ENT(pShooter->pev), &tr);
		if (tr.flFraction < 1.0f && tr.pHit != pVictim->edict())
			continue;

		float strength = (facing - FLASH_FACE_MIN_DOT) / (FLASH_FACE_FULL_DOT - FLASH_FACE_MIN_DOT);
		strength = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);

		if (dist > FLASH_FULL_RANGE)
			strength *= 1.0f - (dist - FLASH_FULL_RANGE) / (FLASH_RANGE - FLASH_FULL_RANGE);
		if (strength <= 0.02f)
			continue;

		const float hold = FLASH_HOLD_MAX * strength;
		const float fade = FLASH_FADE_MIN + (FLASH_FADE_MAX - FLASH_FADE_MIN) * strength;
		UTIL_ScreenFade(pVictim, Vector(255, 255, 255), fade, hold, (int)(255.0f * strength), FFADE_IN);
	}
}

bool Photo_Shutter(CBasePlayer* pPlayer)
{
	PhotoSlot* s = SlotFor(pPlayer);
	if (!s || gmsgPhotoCap == 0)
		return false;

	short id = s->lastId + 1;
	if (id <= 0)
		id = 1;
	s->lastId = id;

	// a new picture replaces the old one
	s->id = id;
	s->hasPhoto = true;
	s->complete = false;
	s->nRecv = 0;
	s->shotTime = gpGlobals->time;
	memset(s->got, 0, sizeof(s->got));

	// taking a picture resets the spray cooldown: new photo = you can spray right away
	pPlayer->m_flNextDecalTime = 0;

	MESSAGE_BEGIN(MSG_ONE, gmsgPhotoCap, nullptr, pPlayer->pev);
	WRITE_SHORT(id);
	MESSAGE_END();

	FlashPlayersInFront(pPlayer);
	return true;
}

// "ss_photo <id> <chunk> <text>" or "ss_photo <id> fail"
bool Photo_ClientCommand(edict_t* pEntity, const char* pcmd)
{
	if (!FStrEq(pcmd, "ss_photo"))
		return false;

	PhotoSlot* s = SlotFor(ENTINDEX(pEntity));
	if (!s || !s->hasPhoto || s->complete || CMD_ARGC() < 3)
		return true;

	if (atoi(CMD_ARGV(1)) != s->id)
		return true; // stale upload of an older picture

	const char* what = CMD_ARGV(2);
	if (FStrEq(what, "fail"))
	{
		s->hasPhoto = false; // client couldn't take it: default spray comes back
		return true;
	}

	if (CMD_ARGC() < 4)
		return true;

	const int chunk = atoi(what);
	if (chunk < 0 || chunk >= PHOTO_CHUNKS || s->got[chunk])
		return true;

	if (!PhotoDecodeChunk(CMD_ARGV(3), s->data + chunk * PHOTO_CHUNK_BYTES))
		return true;

	s->got[chunk] = true;
	if (++s->nRecv >= PHOTO_CHUNKS)
		s->complete = true;

	// relay to everybody (the owner's client ignores its own)
	if (gmsgPhotoDat != 0)
	{
		MESSAGE_BEGIN(MSG_ALL, gmsgPhotoDat, nullptr);
		WRITE_BYTE(ENTINDEX(pEntity));
		WRITE_SHORT(s->id);
		WRITE_BYTE(chunk);
		const unsigned char* src = s->data + chunk * PHOTO_CHUNK_BYTES;
		for (int i = 0; i < PHOTO_CHUNK_BYTES; i++)
			WRITE_BYTE(src[i]);
		MESSAGE_END();
	}
	return true;
}

bool Photo_TrySpray(CBasePlayer* pPlayer, const TraceResult& tr)
{
	PhotoSlot* s = SlotFor(pPlayer);
	if (!s || !s->hasPhoto || gmsgPhotoSpr == 0)
		return false;

	// upload never finished: forget the photo, normal spray
	if (!s->complete && gpGlobals->time > s->shotTime + PHOTO_UPLOAD_TIMEOUT)
	{
		s->hasPhoto = false;
		return false;
	}

	s->hasPhoto = false; // used up; the data stays so clients still waiting can finish

	const int idx = ENTINDEX(pPlayer->edict());
	int yaw = (int)(pPlayer->pev->v_angle.y * (256.0f / 360.0f)) & 255;

	MESSAGE_BEGIN(MSG_ALL, gmsgPhotoSpr, nullptr);
	WRITE_BYTE(idx);
	WRITE_SHORT(s->id);
	WRITE_LONG((int)(tr.vecEndPos.x * 16.0f));
	WRITE_LONG((int)(tr.vecEndPos.y * 16.0f));
	WRITE_LONG((int)(tr.vecEndPos.z * 16.0f));
	WRITE_CHAR((int)(tr.vecPlaneNormal.x * 127.0f));
	WRITE_CHAR((int)(tr.vecPlaneNormal.y * 127.0f));
	WRITE_CHAR((int)(tr.vecPlaneNormal.z * 127.0f));
	WRITE_BYTE(yaw);
	MESSAGE_END();

	EMIT_SOUND(ENT(pPlayer->pev), CHAN_VOICE, "player/sprayer.wav", 1, ATTN_NORM);
	return true;
}
