// grapple.cpp - Grappling hook weapon (server side, put in dlls/)
// Uses the 9mm handgun models for now.
//
// Charge: hold fire to charge, the shot leaves when you RELEASE the button.
//         A quick click reaches GRAPPLE_MIN_RANGE; holding for
//         GRAPPLE_CHARGE_TIME or more reaches GRAPPLE_MAX_RANGE.
//         In between the range is linear:
//             range = MIN + (MAX - MIN) * min(held, CHARGE_TIME) / CHARGE_TIME
// Hit:    a beam is drawn from the gun barrel to the hit point and a ROPE is
//         attached there. The rope has a length (the distance at the moment of
//         impact) that retracts slowly at first and then faster and faster
//         (GRAPPLE_PULL_START -> GRAPPLE_PULL_MAX, GRAPPLE_PULL_ACCEL).
//         The player is NOT dragged along a straight line: the rope only removes
//         the velocity that would make the player leave the rope's radius, and
//         guarantees a minimum closing speed. Everything else (gravity, air
//         strafing, the momentum you had) is kept, so you swing around the anchor.
//         If the hit entity is a monster or a player, BOTH are pulled toward each other.
// Cancel: press fire again while hooked, or switch weapon (momentum is kept).
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
constexpr float GRAPPLE_MIN_RANGE = 500.0f;		// units, a quick click
constexpr float GRAPPLE_MAX_RANGE = 1500.0f;	// units, fully charged
constexpr float GRAPPLE_CHARGE_TIME = 3.0f;		// seconds of holding for the max range
constexpr float GRAPPLE_CHARGE_MSG_GAP = 0.05f; // seconds between HUD updates while charging
constexpr float GRAPPLE_COOLDOWN = 1.0f;		// seconds between shots
constexpr float GRAPPLE_STOP_DIST_WALL = 30.0f;
constexpr float GRAPPLE_STOP_DIST_ENT = 72.0f;	// release when this close to a pulled entity
constexpr float GRAPPLE_MAX_PULL_TIME = 8.0f;	// safety timeout
constexpr int GRAPPLE_BEAM_WIDTH = 10;

// rope behaviour
constexpr float GRAPPLE_PULL_START = 80.0f;	
constexpr float GRAPPLE_PULL_MAX = 700.0f;		  // units/s, rope retract speed right after the hit (slow)
constexpr float GRAPPLE_PULL_ACCEL = 700.0f;	  // units/s^2, how fast the retract speed ramps up
constexpr float GRAPPLE_ROPE_STIFFNESS = 8.0f;	  // 1/s, how hard the rope corrects being stretched (0 = never corrects)
constexpr float GRAPPLE_TARGET_PULL_SHARE = 0.5f; // share of the retract speed given to a pulled monster/player (the rest is the shooter)
constexpr float GRAPPLE_STUCK_MIN_SPEED = 20.0f;  // units/s, below this (and < 25% of the retract speed) counts as stuck
constexpr float GRAPPLE_MAX_SPEED = 900.0f;		  // speed limit while hooked
constexpr float GRAPPLE_SPEED_DAMP = 8.0f;		  // 1/s, how fast excess speed gets bled off
constexpr float GRAPPLE_SLOW_RADIUS = 600.0f;	  // the pull fades out within this distance of the anchor (measured past the stop distance)
constexpr float GRAPPLE_NEAR_PULL_SCALE = 0.25f;  // pull strength right next to the anchor (1.0 = no fade)
constexpr float GRAPPLE_NEAR_MAX_SPEED = 500.0f;  // speed cap right next to the anchor (set = GRAPPLE_MAX_SPEED to disable)

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

	// rope
	float m_flRopeLength = 0; // current max distance between the player and the anchor
	float m_flPullSpeed = 0;  // current retract speed (ramps up from GRAPPLE_PULL_START)

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
		// Pressing fire while hooked = cancel (no cooldown needed to cancel)
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

	// The rope starts exactly as long as the distance at the moment of impact,
	// and retracts slowly at first (the speed ramps up in UpdatePull).
	const Vector vecAnchorNow = bPullTarget ? pHit->Center() : m_vecAnchor;
	m_flRopeLength = (vecAnchorNow - m_pPlayer->pev->origin).Length();
	m_flPullSpeed = GRAPPLE_PULL_START;

	CreateBeam(bPullTarget ? pHit : nullptr, m_vecAnchor);
	EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/xbow_hit1.wav", 1.0f, ATTN_NORM, 0, 100);
}

// Rope model (swing):
//   * The rope has a length. It only ever gets shorter: it follows the player when
//     they get closer, and it retracts by m_flPullSpeed every second, with
//     m_flPullSpeed ramping up from GRAPPLE_PULL_START to GRAPPLE_PULL_MAX.
//   * We never set the player's velocity directly. We look at how fast the player is
//     closing in on the anchor and, if that is less than the rope needs (retract
//     speed + a correction proportional to how stretched the rope is), we add the
//     missing part along the rope direction. The sideways (tangential) part of the
//     velocity is left alone, so gravity, strafing and the momentum from before
//     the shot bend the path into an arc around the anchor.
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
	const float flDt = gpGlobals->frametime;

	if (flNow - m_flPullStart > GRAPPLE_MAX_PULL_TIME)
	{
		Release();
		return;
	}


	// Stuck detection (e.g. hooked something above a low ceiling): read the speed
	// the physics left us with BEFORE we change it. The rope always asks for at
	// least m_flPullSpeed of closing speed, so a much lower speed means we hit something.
	if (flNow - m_flPullStart > 0.3f)
	{
		float flMinSpeed = m_flPullSpeed * 0.25f;
		if (flMinSpeed < GRAPPLE_STUCK_MIN_SPEED)
			flMinSpeed = GRAPPLE_STUCK_MIN_SPEED;

		if (m_pPlayer->pev->velocity.Length() < flMinSpeed)
			m_flStuckTime += flDt;
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

	// 1) the retract speed ramps up: slow at first, faster and faster
	m_flPullSpeed += GRAPPLE_PULL_ACCEL * flDt;
	if (m_flPullSpeed > GRAPPLE_PULL_MAX)
		m_flPullSpeed = GRAPPLE_PULL_MAX;

	// fade the pull out near the anchor so the swing doesn't spiral up in speed
	float flNear = (flDist - flStop) / GRAPPLE_SLOW_RADIUS;
	if (flNear < 0.0f)
		flNear = 0.0f;
	if (flNear > 1.0f)
		flNear = 1.0f;
	const float flPull = m_flPullSpeed * (GRAPPLE_NEAR_PULL_SCALE + (1.0f - GRAPPLE_NEAR_PULL_SCALE) * flNear);

	// 2) the rope only gets shorter: follow the player when they get closer, then retract
	if (flDist < m_flRopeLength)
		m_flRopeLength = flDist;

	m_flRopeLength -= flPull * flDt;
	if (m_flRopeLength < flStop)
		m_flRopeLength = flStop;

	float flStretch = flDist - m_flRopeLength; // how much the rope is stretched right now
	if (flStretch < 0.0f)
		flStretch = 0.0f;

	// 3) a pulled monster/player gets a share of the pull, straight toward the shooter
	Vector vecAnchorVel(0, 0, 0);
	if (pTarget)
	{
		vecAnchorVel = vecDir * -(flPull * GRAPPLE_TARGET_PULL_SHARE);
		pTarget->pev->velocity = vecAnchorVel;
		pTarget->pev->flags &= ~FL_ONGROUND;
	}

	// 4) rope constraint on the player: only the closing speed (along the rope) is
	//    adjusted, the sideways velocity is untouched -> the player swings.
	const float flClosing = DotProduct(m_pPlayer->pev->velocity - vecAnchorVel, vecDir);
	const float flWanted = flPull + flStretch * GRAPPLE_ROPE_STIFFNESS;

	if (flClosing < flWanted)
		m_pPlayer->pev->velocity = m_pPlayer->pev->velocity + vecDir * (flWanted - flClosing);

	m_pPlayer->pev->flags &= ~FL_ONGROUND;

		const float flMaxSpeed = GRAPPLE_NEAR_MAX_SPEED + (GRAPPLE_MAX_SPEED - GRAPPLE_NEAR_MAX_SPEED) * flNear;
	const float flSpeed = m_pPlayer->pev->velocity.Length();
	if (flSpeed > flMaxSpeed)
	{
		float flBlend = GRAPPLE_SPEED_DAMP * flDt;
		if (flBlend > 1.0f)
			flBlend = 1.0f;

		const float flNewSpeed = flSpeed - (flSpeed - flMaxSpeed) * flBlend;
		m_pPlayer->pev->velocity = m_pPlayer->pev->velocity * (flNewSpeed / flSpeed);
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
	m_flRopeLength = 0;
	m_flPullSpeed = 0;
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