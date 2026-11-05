// photocamera.cpp - photo camera weapon (server side, put in dlls/)
// Uses the tripmine models as a placeholder.
//
// Fire: takes a picture of what you see (the client captures it, see photo.cpp / cl_dll/hud_photo.cpp).
//       Only one picture is kept: a new one replaces the old one.
// Spray (impulse 201): with a picture, the next spray paints THAT picture and uses it up;
//       after that (or with no picture) you get the normal spray again.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "nodes.h"
#include "player.h"
#include "gamerules.h"
#include "photo.h"

// Placeholder timings for the tripmine viewmodel (seconds). Tweak when the real camera model exists.
constexpr float CAMERA_ARM_TIME = 1.0f;	 // length of the "shot" animation (TRIPMINE_ARM1)
constexpr float CAMERA_DRAW_TIME = 0.9f; // length of the draw animation that brings the camera back

class CPhotoCamera : public CBasePlayerWeapon
{
public:
	void Spawn() override;
	void Precache() override;
	bool GetItemInfo(ItemInfo* p) override;
	bool Deploy() override;
	void PrimaryAttack() override;
	void SecondaryAttack() override {}
	void WeaponIdle() override;

private:
	bool m_bRedraw = false; // shot animation played, camera must be drawn again before idling
};

LINK_ENTITY_TO_CLASS(weapon_camera, CPhotoCamera);

void CPhotoCamera::Spawn()
{
	Precache();
	m_iId = WEAPON_CAMERA;
	SET_MODEL(ENT(pev), "models/v_tripmine.mdl"); // placeholder world model
	pev->frame = 0;
	pev->body = 3;
	pev->sequence = TRIPMINE_GROUND;
	pev->framerate = 0;
	m_iDefaultAmmo = 0;
	FallInit();
}

void CPhotoCamera::Precache()
{
	PRECACHE_MODEL("models/v_tripmine.mdl");
	PRECACHE_MODEL("models/p_tripmine.mdl");
	PRECACHE_SOUND("weapons/mine_activate.wav");
	PRECACHE_SOUND("player/sprayer.wav");
}

bool CPhotoCamera::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = nullptr;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = nullptr;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 4; // slot 4 position 3 is the snark, position 4 is free
	p->iPosition = 4;
	p->iId = m_iId = WEAPON_CAMERA;
	p->iFlags = 0;
	p->iWeight = 3;
	return true;
}

bool CPhotoCamera::Deploy()
{
	pev->body = 0; // first person body of the tripmine model
	m_bRedraw = false;
	m_flTimeWeaponIdle = gpGlobals->time + RANDOM_FLOAT(10, 15);
	return DefaultDeploy("models/v_tripmine.mdl", "models/p_tripmine.mdl", TRIPMINE_DRAW, "trip");
}

void CPhotoCamera::PrimaryAttack()
{
	if (!Photo_Shutter(m_pPlayer))
		return;

	EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_WEAPON, "weapons/mine_activate.wav", 0.8f, ATTN_NORM);
	SendWeaponAnim(TRIPMINE_ARM1);
	m_pPlayer->SetAnimation(PLAYER_ATTACK1);

	// shot animation (the "mine" leaves the hand) -> draw it again -> idle
	m_bRedraw = true;
	m_flTimeWeaponIdle = gpGlobals->time + CAMERA_ARM_TIME;

	m_flNextPrimaryAttack = gpGlobals->time + CAMERA_ARM_TIME + CAMERA_DRAW_TIME;
	m_flNextSecondaryAttack = m_flNextPrimaryAttack;
}

void CPhotoCamera::WeaponIdle()
{
	pev->body = 0;

	if (m_flTimeWeaponIdle > gpGlobals->time)
		return;

	if (m_bRedraw)
	{
		m_bRedraw = false;
		SendWeaponAnim(TRIPMINE_DRAW);
		m_flTimeWeaponIdle = gpGlobals->time + CAMERA_DRAW_TIME;
		return;
	}

	int iAnim;
	const float flRand = RANDOM_FLOAT(0, 1);
	if (flRand <= 0.25f)
	{
		iAnim = TRIPMINE_IDLE1;
		m_flTimeWeaponIdle = gpGlobals->time + 90.0f / 30.0f;
	}
	else if (flRand <= 0.75f)
	{
		iAnim = TRIPMINE_IDLE2;
		m_flTimeWeaponIdle = gpGlobals->time + 60.0f / 30.0f;
	}
	else
	{
		iAnim = TRIPMINE_FIDGET;
		m_flTimeWeaponIdle = gpGlobals->time + 100.0f / 30.0f;
	}
	SendWeaponAnim(iAnim);
}
