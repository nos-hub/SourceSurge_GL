// grapple.cpp - Grappling hook weapon (server side, put in dlls/)
// Uses the 9mm handgun models for now.
//
// Charge: hold fire to charge, the shot leaves when you RELEASE the button.
//         A quick click reaches GRAPPLE_MIN_RANGE (300 units); holding for
//         GRAPPLE_CHARGE_TIME (3.5 s) or more reaches GRAPPLE_MAX_RANGE (1500).
//         In between the range is linear:
//             range = MIN + (MAX - MIN) * min(held, CHARGE_TIME) / CHARGE_TIME
// Hit:    a beam is drawn from the gun barrel to the hit point and the player is
//         pulled toward it at GRAPPLE_PULL_SPEED.
//         If the hit entity is a monster or a player, BOTH are pulled toward each other.
// Cancel: press fire again while pulling, or switch weapon (momentum is kept).
//         Switching weapon while charging cancels the charge without firing.
// Shots are limited by GRAPPLE_COOLDOWN (measured from the moment of release).
//
// The charge is sent to the client HUD with the "GrapCharge" user message
// (cl_dll/hud_grapplecharge.cpp).

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "nodes.h"
#include "player.h"
#include "gamerules.h"
#include "UserMessages.h"

// ---------------- tunables ----------------
constexpr float GRAPPLE_MIN_RANGE = 300.0f;		// units, a quick click
constexpr float GRAPPLE_MAX_RANGE = 1500.0f;	// units, fully charged
constexpr float GRAPPLE_CHARGE_TIME = 3.5f;		// seconds of holding for the max range
constexpr float GRAPPLE_CHARGE_MSG_GAP = 0.05f; // seconds between HUD updates while charging
constexpr float GRAPPLE_PULL_SPEED = 680.0f;	// units/s (applied to the player, and to the target if it is pulled too)
constexpr float GRAPPLE_COOLDOWN = 1.5f;		// seconds between shots
constexpr float GRAPPLE_STOP_DIST_WALL = 48.0f; // release when this close to a wall anchor
constexpr float GRAPPLE_STOP_DIST_ENT = 72.0f;	// release when this close to a pulled entity
constexpr float GRAPPLE_MAX_PULL_TIME = 6.0f;	// safety timeout
constexpr int GRAPPLE_BEAM_WIDTH = 10;

#define GRAPPLE_BEAM_SPRITE "sprites/laserbeam.spr"

class CGrapple : public CBasePlayerWeapon
{
public:
	void Spawn() override;
	void Precache() override;
	bool GetItemInfo(ItemInfo* p) override;
	bool Deploy() override;
	void Holster() override;
	void PrimaryAttack() override;
	void ItemPostFrame() override;
	void WeaponIdle() override;

private:
	enum GrappleState
	{
		GRAPPLE_IDLE = 0,
		GRAPPLE_PULLING
	};

	float ChargeFraction() const;
	static float RangeForCharge(float flFraction);
	void SendCharge(bool bActive, float flFraction, float flRange);
	void CancelCharge();
	void Fire(float flRange);

	void UpdatePull();
	void Release();
	void CreateBeam(CBaseEntity* pTarget, const Vector& vecAnchor);
	void DestroyBeam();
	CBeam* GetBeam() { return static_cast<CBeam*>(static_cast<CBaseEntity*>(m_hBeam)); }

	int m_state = GRAPPLE_IDLE;
	float m_flNextShotTime = 0;
	float m_flPullStart = 0;
	float m_flStuckTime = 0;
	Vector m_vecAnchor; // fixed anchor (walls etc.)
	EHANDLE m_hTarget;	// set only when the target is pulled too (monster/player)
	bool m_bPullTarget = false;
	EHANDLE m_hBeam;

	// charge
	bool m_bCharging = false;
	bool m_bChargeMaxed = false;
	float m_flChargeStart = 0;
	float m_flNextChargeMsg = 0;
};

LINK_ENTITY_TO_CLASS(weapon_grapple, CGrapple);

void CGrapple::Spawn()
{
	Precache();
	m_iId = WEAPON_GRAPPLE;
	SET_MODEL(ENT(pev), "models/w_9mmhandgun.mdl"); // world model = 9mm
	m_iDefaultAmmo = 0;
	FallInit();
}

void CGrapple::Precache()
{
	PRECACHE_MODEL("models/v_grapple.mdl");	   // your custom viewmodel
	PRECACHE_MODEL("models/w_9mmhandgun.mdl"); // 9mm world
	PRECACHE_MODEL("models/p_9mmhandgun.mdl"); // 9mm third person

	PRECACHE_MODEL(GRAPPLE_BEAM_SPRITE);

	PRECACHE_SOUND("weapons/xbow_fire1.wav");
	PRECACHE_SOUND("weapons/xbow_hit1.wav");
	PRECACHE_SOUND("weapons/357_cock1.wav");
}


bool CGrapple::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = nullptr;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = nullptr;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0; // same slot as the crowbar
	p->iPosition = 1;
	p->iId = m_iId = WEAPON_GRAPPLE;
	p->iFlags = 0;
	p->iWeight = 5;
	return true;
}

bool CGrapple::Deploy()
{
	m_state = GRAPPLE_IDLE;
	m_bCharging = false;
	m_flTimeWeaponIdle = gpGlobals->time + RANDOM_FLOAT(10, 15);
	return DefaultDeploy("models/v_grapple.mdl", "models/p_9mmhandgun.mdl", GLOCK_DRAW, "onehanded");
}

void CGrapple::Holster()
{
	// Remove the string and the pulling force. We never touch the velocity here,
	// so the player and the target keep the momentum they already have.
	CancelCharge();
	Release();
	CBasePlayerWeapon::Holster();
}

// ---------------- charge ----------------

float CGrapple::ChargeFraction() const
{
	float flHeld = gpGlobals->time - m_flChargeStart;
	if (flHeld < 0.0f)
		flHeld = 0.0f;
	if (flHeld > GRAPPLE_CHARGE_TIME)
		flHeld = GRAPPLE_CHARGE_TIME;
	return flHeld / GRAPPLE_CHARGE_TIME;
}

float CGrapple::RangeForCharge(float flFraction)
{
	return GRAPPLE_MIN_RANGE + (GRAPPLE_MAX_RANGE - GRAPPLE_MIN_RANGE) * flFraction;
}

void CGrapple::SendCharge(bool bActive, float flFraction, float flRange)
{
	if (gmsgGrappleCharge == 0 || !m_pPlayer)
		return;

	MESSAGE_BEGIN(MSG_ONE, gmsgGrappleCharge, nullptr, m_pPlayer->pev);
	WRITE_BYTE(bActive ? 1 : 0);
	WRITE_BYTE((int)(flFraction * 100.0f + 0.5f));
	WRITE_SHORT((int)(flRange + 0.5f));
	MESSAGE_END();
}

void CGrapple::CancelCharge()
{
	if (!m_bCharging)
		return;

	m_bCharging = false;
	m_bChargeMaxed = false;
	SendCharge(false, 0.0f, 0.0f);
}

void CGrapple::ItemPostFrame()
{
	if (m_state == GRAPPLE_PULLING)
	{
		// Pressing fire while pulling = cancel (no cooldown needed to cancel)
		if (m_pPlayer->m_afButtonPressed & IN_ATTACK)
		{
			Release();
			return;
		}

		UpdatePull();
	}
	else if (m_bCharging)
	{
		const bool bHeld = (m_pPlayer->pev->button & IN_ATTACK) != 0;
		const float flCharge = ChargeFraction();

		if (!bHeld)
		{
			// released: the shot leaves now, with the range of the charge so far
			m_bCharging = false;
			m_bChargeMaxed = false;
			SendCharge(false, 0.0f, 0.0f);
			Fire(RangeForCharge(flCharge));
		}
		else
		{
			// little click once when the charge is full
			if (!m_bChargeMaxed && flCharge >= 1.0f)
			{
				m_bChargeMaxed = true;
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/357_cock1.wav", 0.6f, ATTN_NORM, 0, 130);
			}

			if (gpGlobals->time >= m_flNextChargeMsg)
			{
				m_flNextChargeMsg = gpGlobals->time + GRAPPLE_CHARGE_MSG_GAP;
				SendCharge(true, flCharge, RangeForCharge(flCharge));
			}
		}
	}

	CBasePlayerWeapon::ItemPostFrame();
}

// Called every frame by ItemPostFrame while fire is held. It only STARTS a charge;
// the shot itself leaves in ItemPostFrame when the button is released.
void CGrapple::PrimaryAttack()
{
	if (m_state != GRAPPLE_IDLE || m_bCharging)
		return;

	// a charge has to start with a fresh click, and respect the cooldown
	if (!(m_pPlayer->m_afButtonPressed & IN_ATTACK))
		return;

	if (gpGlobals->time < m_flNextShotTime)
		return;

	m_bCharging = true;
	m_bChargeMaxed = false;
	m_flChargeStart = gpGlobals->time;
	m_flNextChargeMsg = 0;

	SendCharge(true, 0.0f, GRAPPLE_MIN_RANGE);
}

void CGrapple::Fire(float flRange)
{
	m_flNextShotTime = gpGlobals->time + GRAPPLE_COOLDOWN;

	m_pPlayer->SetAnimation(PLAYER_ATTACK1);
	SendWeaponAnim(GLOCK_SHOOT);
	EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_WEAPON, "weapons/xbow_fire1.wav", 1.0f, ATTN_NORM, 0, 100);

	// ---- hitscan along the crosshair, up to the charged range ----
	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	const Vector vecSrc = m_pPlayer->GetGunPosition();
	const Vector vecEnd = vecSrc + gpGlobals->v_forward * flRange;

	TraceResult tr;
	UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, m_pPlayer->edict(), &tr);

	// nothing hit, or we hit the skybox -> miss
	const bool bSky = (tr.flFraction < 1.0f) &&
					  (UTIL_PointContents(tr.vecEndPos + gpGlobals->v_forward * 4.0f) == CONTENTS_SKY);

	if (tr.flFraction >= 1.0f || bSky)
	{
		EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/357_cock1.wav", 0.8f, ATTN_NORM, 0, 100);
		return;
	}

	CBaseEntity* pHit = CBaseEntity::Instance(tr.pHit);

	// Monsters and players are pulled toward us as well
	const bool bPullTarget = pHit &&
							 (pHit->pev->flags & (FL_MONSTER | FL_CLIENT)) &&
							 pHit->pev->deadflag == DEAD_NO;

	m_vecAnchor = tr.vecEndPos;
	m_hTarget = bPullTarget ? pHit : nullptr;
	m_bPullTarget = bPullTarget;
	m_state = GRAPPLE_PULLING;
	m_flPullStart = gpGlobals->time;
	m_flStuckTime = 0;

	CreateBeam(bPullTarget ? pHit : nullptr, m_vecAnchor);
	EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/xbow_hit1.wav", 1.0f, ATTN_NORM, 0, 100);
}

void CGrapple::UpdatePull()
{
	CBaseEntity* pTarget = m_hTarget; // nullptr when anchored to the world

	// The pulled target vanished or died -> let go
	if (m_bPullTarget && (pTarget == nullptr || pTarget->pev->deadflag != DEAD_NO))
	{
		Release();
		return;
	}

	const float flNow = gpGlobals->time;

	if (flNow - m_flPullStart > GRAPPLE_MAX_PULL_TIME)
	{
		Release();
		return;
	}

	// Stuck detection (e.g. hooked something above a low ceiling): read the speed
	// the physics left us with BEFORE we overwrite it.
	if (flNow - m_flPullStart > 0.3f)
	{
		if (m_pPlayer->pev->velocity.Length() < GRAPPLE_PULL_SPEED * 0.25f)
			m_flStuckTime += gpGlobals->frametime;
		else
			m_flStuckTime = 0;

		if (m_flStuckTime > 0.35f)
		{
			Release();
			return;
		}
	}

	const Vector vecAnchor = pTarget ? pTarget->Center() : m_vecAnchor;
	const Vector vecToAnchor = vecAnchor - m_pPlayer->pev->origin;
	const float flDist = vecToAnchor.Length();
	const float flStop = pTarget ? GRAPPLE_STOP_DIST_ENT : GRAPPLE_STOP_DIST_WALL;

	if (flDist <= flStop)
	{
		Release(); // arrived, momentum is kept
		return;
	}

	const Vector vecDir = vecToAnchor * (1.0f / flDist);

	// pull the player
	m_pPlayer->pev->velocity = vecDir * GRAPPLE_PULL_SPEED;
	m_pPlayer->pev->flags &= ~FL_ONGROUND;

	// pull the target toward the player
	if (pTarget)
	{
		pTarget->pev->velocity = vecDir * -GRAPPLE_PULL_SPEED;
		pTarget->pev->flags &= ~FL_ONGROUND;
	}

	// keep the beam's bounding box / PVS up to date while everything moves
	if (CBeam* pBeam = GetBeam())
		pBeam->RelinkBeam();
}

void CGrapple::Release()
{
	m_state = GRAPPLE_IDLE;
	m_hTarget = nullptr;
	m_bPullTarget = false;
	m_flStuckTime = 0;
	DestroyBeam();
	// velocities are intentionally NOT touched -> everybody keeps their momentum
}

void CGrapple::CreateBeam(CBaseEntity* pTarget, const Vector& vecAnchor)
{
	DestroyBeam();

	CBeam* pBeam = CBeam::BeamCreate(GRAPPLE_BEAM_SPRITE, GRAPPLE_BEAM_WIDTH);
	if (!pBeam)
		return;

	if (pTarget)
	{
		// player's gun -> target entity (follows the target)
		pBeam->EntsInit(m_pPlayer->entindex(), pTarget->entindex());
		pBeam->SetStartAttachment(1);
	}
	else
	{
		// fixed point -> player's gun
		pBeam->PointEntInit(vecAnchor, m_pPlayer->entindex());
		pBeam->SetEndAttachment(1);
	}

	pBeam->SetColor(220, 220, 255);
	pBeam->SetBrightness(255);
	pBeam->SetScrollRate(0);
	pBeam->pev->spawnflags |= SF_BEAM_TEMPORARY; // removed on level change
	pBeam->RelinkBeam();

	m_hBeam = pBeam;
}

void CGrapple::DestroyBeam()
{
	if (CBeam* pBeam = GetBeam())
		UTIL_Remove(pBeam);

	m_hBeam = nullptr;
}

void CGrapple::WeaponIdle()
{
	if (m_state != GRAPPLE_IDLE || m_bCharging)
		return;

	if (m_flTimeWeaponIdle > gpGlobals->time)
		return;

	SendWeaponAnim(GLOCK_IDLE1);
	m_flTimeWeaponIdle = gpGlobals->time + RANDOM_FLOAT(10, 15);
}