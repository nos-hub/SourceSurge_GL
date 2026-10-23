// sniper.cpp - Sniper rifle (weapon_sniper), server side, put in dlls/
//
// Built from the crossbow: same models, animations and pickup code.
//
//   Magazine 5, reserve 25 ("sniper" ammo), reload 2.5 s.
//   Damage 100 per target. The bullet goes THROUGH living targets (monsters and players) and stops at
//   solid scenery, so one shot can kill several enemies (collateral).
//   No crosshair (see sprites/weapon_sniper.txt). HOLD right click to scope: the FOV zooms in and the
//   client draws the scope overlay (cl_dll/hud_scope.cpp) for exactly as long as the button is held.
//   Firing without the scope is allowed but inaccurate (SNIPER_NOSCOPE_SPREAD).
//
// Style points: the shot info is exposed through sniper.h so points.cpp can award NOSCOPE, QUICKSCOPE,
// COLLAT, LONGRANGE and CLOSERANGE.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "nodes.h"
#include "player.h"
#include "gamerules.h"
#include "decals.h"
#include "UserMessages.h"
#include "sniper.h"

// ---------------- tunables ----------------
constexpr int   SNIPER_MAX_CLIP = 5;
constexpr int   SNIPER_MAX_CARRY = 25;
constexpr int   SNIPER_DEFAULT_GIVE = 5;			// ammo in a freshly picked-up weapon
constexpr int   SNIPER_AMMO_PICKUP = 5;				// ammo_sniper box
constexpr float SNIPER_DAMAGE = 100.0f;
constexpr float SNIPER_RELOAD_TIME = 2.5f;			// seconds
constexpr float SNIPER_FIRE_DELAY = 1.2f;			// bolt cycle between shots
constexpr int   SNIPER_ZOOM_FOV = 20;				// scoped field of view (default is 90)
constexpr float SNIPER_RANGE = 8192.0f;
constexpr int   SNIPER_MAX_PIERCE = 16;				// living targets one bullet can go through
constexpr float SNIPER_NOSCOPE_SPREAD = 0.025f;		// aim cone when firing unscoped (about 1.4 degrees)
constexpr float SNIPER_RECOIL = 5.0f;				// view kick (degrees, upward)

#define SNIPER_VMODEL "models/v_crossbow.mdl"
#define SNIPER_PMODEL "models/p_crossbow.mdl"
#define SNIPER_WMODEL "models/w_crossbow.mdl"
#define SNIPER_AMMO_MODEL "models/w_crossbow_clip.mdl"
#define SNIPER_SND_FIRE "weapons/357_shot1.wav"
#define SNIPER_SND_RELOAD "weapons/xbow_reload1.wav"
#define SNIPER_BEAM_SPRITE "sprites/laserbeam.spr"

// same animation numbering as the crossbow model
enum sniper_e
{
	SNIPER_IDLE1 = 0,	// full
	SNIPER_IDLE2,		// empty
	SNIPER_FIDGET1,
	SNIPER_FIDGET2,
	SNIPER_FIRE1,		// full
	SNIPER_FIRE2,
	SNIPER_FIRE3,		// empty
	SNIPER_RELOAD,
	SNIPER_DRAW1,		// full
	SNIPER_DRAW2,		// empty
	SNIPER_HOLSTER1,	// full
	SNIPER_HOLSTER2		// empty
};

static SniperShot g_SniperShot = {false, false, 0.0f, 0};
static int g_iSniperBeam = 0;

const SniperShot& Sniper_GetShot()
{
	return g_SniperShot;
}

void Sniper_NoteKill()
{
	if (g_SniperShot.active)
		g_SniperShot.kills++;
}

class CSniper : public CBasePlayerWeapon
{
public:
	void Spawn() override;
	void Precache() override;
	bool GetItemInfo(ItemInfo* p) override;
	bool Deploy() override;
	void Holster() override;
	void PrimaryAttack() override;
	void SecondaryAttack() override {} // the scope is handled in ItemPostFrame (hold to scope)
	void Reload() override;
	void WeaponIdle() override;
	void ItemPostFrame() override;

private:
	void SetScope(bool on);
	void SendScopeMessage(bool on);
	void Fire();

	bool m_bScoped = false;
	float m_flScopeStart = 0.0f;
};

LINK_ENTITY_TO_CLASS(weapon_sniper, CSniper);

void CSniper::Spawn()
{
	Precache();
	m_iId = WEAPON_SNIPER;
	SET_MODEL(ENT(pev), SNIPER_WMODEL);

	m_iDefaultAmmo = SNIPER_DEFAULT_GIVE;

	FallInit(); // get ready to fall down.
}

void CSniper::Precache()
{
	PRECACHE_MODEL(SNIPER_WMODEL);
	PRECACHE_MODEL(SNIPER_VMODEL);
	PRECACHE_MODEL(SNIPER_PMODEL);

	PRECACHE_SOUND(SNIPER_SND_FIRE);
	PRECACHE_SOUND(SNIPER_SND_RELOAD);

	g_iSniperBeam = PRECACHE_MODEL(SNIPER_BEAM_SPRITE);
}

bool CSniper::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = "sniper";
	p->iMaxAmmo1 = SNIPER_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = SNIPER_MAX_CLIP;
	p->iSlot = 2;
	p->iPosition = 4;
	p->iId = WEAPON_SNIPER;
	p->iFlags = 0;
	p->iWeight = 11;
	return true;
}

void CSniper::SendScopeMessage(bool on)
{
	if (gmsgSniperScope == 0)
		return;

	MESSAGE_BEGIN(MSG_ONE, gmsgSniperScope, NULL, m_pPlayer->pev);
	WRITE_BYTE(on ? 1 : 0);
	MESSAGE_END();
}

// Scope up / down: zoom the field of view, hide the viewmodel while scoped, and tell the client to draw
// (or stop drawing) the overlay.
void CSniper::SetScope(bool on)
{
	if (on == m_bScoped)
		return;

	m_bScoped = on;
	m_flScopeStart = gpGlobals->time;

	m_pPlayer->m_iFOV = on ? SNIPER_ZOOM_FOV : 0; // 0 = default fov
	m_pPlayer->pev->viewmodel = on ? 0 : MAKE_STRING(SNIPER_VMODEL);

	SendScopeMessage(on);
}

bool CSniper::Deploy()
{
	m_bScoped = false;
	m_pPlayer->m_iFOV = 0;
	SendScopeMessage(false);

	return DefaultDeploy(SNIPER_VMODEL, SNIPER_PMODEL, m_iClip != 0 ? SNIPER_DRAW1 : SNIPER_DRAW2, "bow");
}

void CSniper::Holster()
{
	m_fInReload = false; // cancel any reload in progress.

	SetScope(false);

	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5;
	SendWeaponAnim(m_iClip != 0 ? SNIPER_HOLSTER1 : SNIPER_HOLSTER2);
}

// Holding right click keeps the scope up; letting go (or reloading) drops it.
void CSniper::ItemPostFrame()
{
	const bool wantScope = (m_pPlayer->pev->button & IN_ATTACK2) != 0 && !m_fInReload && m_pPlayer->IsAlive();
	SetScope(wantScope);

	CBasePlayerWeapon::ItemPostFrame();
}

void CSniper::PrimaryAttack()
{
	if (m_iClip <= 0)
	{
		PlayEmptySound();
		m_flNextPrimaryAttack = GetNextAttackDelay(0.3f);
		return;
	}

	Fire();
}

void CSniper::Fire()
{
	const float now = gpGlobals->time;

	m_iClip--;

	m_pPlayer->m_iWeaponVolume = LOUD_GUN_VOLUME;
	m_pPlayer->m_iWeaponFlash = BRIGHT_GUN_FLASH;
	m_pPlayer->SetAnimation(PLAYER_ATTACK1);
	SendWeaponAnim(m_iClip != 0 ? SNIPER_FIRE1 : SNIPER_FIRE3);
	EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_WEAPON, SNIPER_SND_FIRE, 1.0f, ATTN_NORM, 0, 100);

	Vector anglesAim = m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle;
	UTIL_MakeVectors(anglesAim);

	const Vector vecSrc = m_pPlayer->GetGunPosition() - gpGlobals->v_up * 2;
	const Vector vecRight = gpGlobals->v_right;
	const Vector vecUp = gpGlobals->v_up;
	Vector vecDir = gpGlobals->v_forward;

	// unscoped shots are inaccurate; scoped shots are exact
	if (!m_bScoped)
	{
		vecDir = vecDir + vecRight * (RANDOM_FLOAT(-1.0f, 1.0f) * SNIPER_NOSCOPE_SPREAD) + vecUp * (RANDOM_FLOAT(-1.0f, 1.0f) * SNIPER_NOSCOPE_SPREAD);
		vecDir = vecDir.Normalize();
	}

	// what points.cpp needs to know about this shot
	g_SniperShot.active = true;
	g_SniperShot.scoped = m_bScoped;
	g_SniperShot.scopeAge = m_bScoped ? (now - m_flScopeStart) : 0.0f;
	g_SniperShot.kills = 0;

	// The bullet: trace, hurt whatever it hits, and keep going through living targets.
	Vector vecStart = vecSrc;
	const Vector vecEnd = vecSrc + vecDir * SNIPER_RANGE;
	Vector vecBeamEnd = vecEnd;
	edict_t* pIgnore = m_pPlayer->edict();

	for (int i = 0; i < SNIPER_MAX_PIERCE; i++)
	{
		TraceResult tr;
		UTIL_TraceLine(vecStart, vecEnd, dont_ignore_monsters, pIgnore, &tr);

		if (tr.flFraction >= 1.0f)
			break;

		vecBeamEnd = tr.vecEndPos;

		CBaseEntity* pEntity = FNullEnt(tr.pHit) ? NULL : CBaseEntity::Instance(tr.pHit);

		if (pEntity && pEntity->pev->takedamage != DAMAGE_NO)
		{
			ClearMultiDamage();
			pEntity->TraceAttack(m_pPlayer->pev, SNIPER_DAMAGE, vecDir, &tr, DMG_BULLET | DMG_NEVERGIB);
			ApplyMultiDamage(m_pPlayer->pev, m_pPlayer->pev);

			const bool bLiving = pEntity->IsPlayer() || (pEntity->pev->flags & FL_MONSTER) != 0;
			if (bLiving)
			{
				// through it and on to the next one
				vecStart = tr.vecEndPos + vecDir * 2.0f;
				pIgnore = tr.pHit;
				continue;
			}
		}
		else
		{
			// solid scenery: the bullet stops here
			UTIL_DecalTrace(&tr, DECAL_GUNSHOT1 + RANDOM_LONG(0, 4));
		}

		break;
	}

	g_SniperShot.active = false;

	// tracer line from the muzzle to where the bullet ended
	if (g_iSniperBeam != 0)
	{
		const Vector vecMuzzle = vecSrc + vecRight * 4 - vecUp * 3;

		MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, vecMuzzle);
		WRITE_BYTE(TE_BEAMPOINTS);
		WRITE_COORD(vecMuzzle.x);
		WRITE_COORD(vecMuzzle.y);
		WRITE_COORD(vecMuzzle.z);
		WRITE_COORD(vecBeamEnd.x);
		WRITE_COORD(vecBeamEnd.y);
		WRITE_COORD(vecBeamEnd.z);
		WRITE_SHORT(g_iSniperBeam);
		WRITE_BYTE(0);	 // start frame
		WRITE_BYTE(0);	 // framerate
		WRITE_BYTE(2);	 // life (0.1 s units)
		WRITE_BYTE(3);	 // width (0.1 units)
		WRITE_BYTE(0);	 // noise
		WRITE_BYTE(255); // r
		WRITE_BYTE(240); // g
		WRITE_BYTE(200); // b
		WRITE_BYTE(200); // brightness
		WRITE_BYTE(0);	 // scroll speed
		MESSAGE_END();
	}

	// view kick
	m_pPlayer->pev->punchangle.x -= SNIPER_RECOIL;

	if (m_iClip == 0 && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0)
		// HEV suit - indicate out of ammo condition
		m_pPlayer->SetSuitUpdate("!HEV_AMO0", false, 0);

	m_flNextPrimaryAttack = GetNextAttackDelay(SNIPER_FIRE_DELAY);
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + (m_iClip != 0 ? 5.0f : 1.0f);
}

void CSniper::Reload()
{
	if (m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 || m_iClip >= SNIPER_MAX_CLIP)
		return;

	SetScope(false);

	// The crossbow reload animation is longer than 2.5 s: it is cut when the reload ends.
	if (DefaultReload(SNIPER_MAX_CLIP, SNIPER_RELOAD, SNIPER_RELOAD_TIME))
	{
		EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, SNIPER_SND_RELOAD, RANDOM_FLOAT(0.95, 1.0), ATTN_NORM, 0, 93 + RANDOM_LONG(0, 0xF));
	}
}

void CSniper::WeaponIdle()
{
	ResetEmptySound();

	if (m_flTimeWeaponIdle < UTIL_WeaponTimeBase())
	{
		SendWeaponAnim(m_iClip != 0 ? SNIPER_IDLE1 : SNIPER_IDLE2);
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + RANDOM_FLOAT(10, 15);
	}
}

// ---------------- ammo box ----------------
class CSniperAmmo : public CBasePlayerAmmo
{
	void Spawn() override
	{
		Precache();
		SET_MODEL(ENT(pev), SNIPER_AMMO_MODEL);
		CBasePlayerAmmo::Spawn();
	}

	void Precache() override
	{
		PRECACHE_MODEL(SNIPER_AMMO_MODEL);
		PRECACHE_SOUND("items/9mmclip1.wav");
	}

	bool AddAmmo(CBaseEntity* pOther) override
	{
		if (pOther->GiveAmmo(SNIPER_AMMO_PICKUP, "sniper", SNIPER_MAX_CARRY) != -1)
		{
			EMIT_SOUND(ENT(pev), CHAN_ITEM, "items/9mmclip1.wav", 1, ATTN_NORM);
			return true;
		}
		return false;
	}
};

LINK_ENTITY_TO_CLASS(ammo_sniper, CSniperAmmo);
