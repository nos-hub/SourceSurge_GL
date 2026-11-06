/***
 *
 *	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
 *
 *	This product contains software technology licensed from Id
 *	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
 *	All Rights Reserved.
 *
 *   Use, distribution, and modification of this source code and/or resulting
 *   object code is restricted to non-commercial enhancements to products from
 *   Valve LLC.  All other use, distribution, or modification is prohibited
 *   without written permission from Valve LLC.
 *
 ****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"

#ifndef CLIENT_DLL
#include "melee_kb.h"
#endif

#define KNIFE_IDLE_TIME 20.0f
#define KNIFE_SLASH_DAMAGE 25
#define KNIFE_HEAVY_DAMAGE 50
#define KNIFE_SLASH_DELAY 0.4f
#define KNIFE_HEAVY_DELAY 1.1f
#define KNIFE_HEAVY_WINDUP 0.3f

#define CROWBAR_BODYHIT_VOLUME 128
#define CROWBAR_WALLHIT_VOLUME 512
#define CROWBAR_RANGE 50

LINK_ENTITY_TO_CLASS(weapon_crowbar, CCrowbar);

void CCrowbar::Spawn()
{
	Precache();
	m_iId = WEAPON_CROWBAR;
	m_bHeavySwing = false;
	SET_MODEL(ENT(pev), "models/w_knife.mdl");
	m_iClip = -1;

	FallInit(); // get ready to fall down.
}


void CCrowbar::Precache()
{
	PRECACHE_MODEL("models/v_knife.mdl");
	PRECACHE_MODEL("models/w_knife.mdl");
	PRECACHE_MODEL("models/p_knife.mdl");
	PRECACHE_SOUND("weapons/cbar_hit1.wav");
	PRECACHE_SOUND("weapons/cbar_hit2.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod1.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod2.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod3.wav");
	PRECACHE_SOUND("weapons/cbar_miss1.wav");

	m_usCrowbar = PRECACHE_EVENT(1, "events/crowbar.sc");
}

bool CCrowbar::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;
	p->iPosition = 0;
	p->iId = WEAPON_CROWBAR;
	p->iWeight = CROWBAR_WEIGHT;
	return true;
}



bool CCrowbar::Deploy()
{
	m_bHeavySwing = false;
	// "crowbar" = third-person player animation set, keep it so the player model still animates
	return DefaultDeploy("models/v_knife.mdl", "models/p_knife.mdl", CROWBAR_DRAW, "crowbar");
}

void CCrowbar::Holster()
{
	// cancel any pending wind-up / swing / smack
	SetThink(NULL);

	// the CS knife has no holster animation
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5;
}

void CCrowbar::WeaponIdle()
{
	// never cut a swing / wind-up / recovery
	if (m_flNextPrimaryAttack > UTIL_WeaponTimeBase() || m_flNextSecondaryAttack > UTIL_WeaponTimeBase())
		return;

	if (m_flTimeWeaponIdle > UTIL_WeaponTimeBase())
		return;

	SendWeaponAnim(CROWBAR_IDLE);
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + KNIFE_IDLE_TIME;
}

void FindHullIntersection(const Vector& vecSrc, TraceResult& tr, const Vector& mins, const Vector& maxs, edict_t* pEntity)
{
	int i, j, k;
	float distance;
	const Vector* minmaxs[2] = {&mins, &maxs};
	TraceResult tmpTrace;
	Vector vecHullEnd = tr.vecEndPos;
	Vector vecEnd;

	distance = 1e6f;

	vecHullEnd = vecSrc + ((vecHullEnd - vecSrc) * 2);
	UTIL_TraceLine(vecSrc, vecHullEnd, dont_ignore_monsters, pEntity, &tmpTrace);
	if (tmpTrace.flFraction < 1.0)
	{
		tr = tmpTrace;
		return;
	}

	for (i = 0; i < 2; i++)
	{
		for (j = 0; j < 2; j++)
		{
			for (k = 0; k < 2; k++)
			{
				vecEnd.x = vecHullEnd.x + minmaxs[i]->x;
				vecEnd.y = vecHullEnd.y + minmaxs[j]->y;
				vecEnd.z = vecHullEnd.z + minmaxs[k]->z;

				UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, pEntity, &tmpTrace);
				if (tmpTrace.flFraction < 1.0)
				{
					float thisDistance = (tmpTrace.vecEndPos - vecSrc).Length();
					if (thisDistance < distance)
					{
						tr = tmpTrace;
						distance = thisDistance;
					}
				}
			}
		}
	}
}


// Shared line + hull trace used by Swing() and the backstab pre-check
void CCrowbar::TraceMelee(TraceResult& tr, Vector& vecSrc, Vector& vecEnd)
{
	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	vecSrc = m_pPlayer->GetGunPosition();
	vecEnd = vecSrc + gpGlobals->v_forward * CROWBAR_RANGE;

	UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, ENT(m_pPlayer->pev), &tr);

#ifndef CLIENT_DLL
	if (tr.flFraction >= 1.0)
	{
		UTIL_TraceHull(vecSrc, vecEnd, dont_ignore_monsters, head_hull, ENT(m_pPlayer->pev), &tr);
		if (tr.flFraction < 1.0)
		{
			// Calculate the point of intersection of the line (or hull) and the object we hit
			// This is and approximation of the "best" intersection
			CBaseEntity* pHit = CBaseEntity::Instance(tr.pHit);
			if (!pHit || pHit->IsBSPModel())
				FindHullIntersection(vecSrc, tr, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX, m_pPlayer->edict());
			vecEnd = tr.vecEndPos; // This is the point on the actual surface (the hull could have hit space)
		}
	}
#endif
}


// TF2-style backstab: uses the victim's VIEW angles (not their position),
// which is what makes trickstabs work.
bool CCrowbar::CanBackstab(CBaseEntity* pVictim)
{
#ifndef CLIENT_DLL
	if (!pVictim || !pVictim->IsAlive())
		return false;
	if (!pVictim->IsPlayer() && !(pVictim->pev->flags & FL_MONSTER))
		return false;

	// victim's view angles (players), body angles (NPCs)
	Vector vAng = pVictim->IsPlayer() ? pVictim->pev->v_angle : pVictim->pev->angles;
	Vector vVictimFwd, vOwnerFwd;
	g_engfuncs.pfnAngleVectors(vAng, vVictimFwd, NULL, NULL);
	g_engfuncs.pfnAngleVectors(m_pPlayer->pev->v_angle, vOwnerFwd, NULL, NULL);

	Vector vToVictim = pVictim->Center() - m_pPlayer->Center();
	vVictimFwd.z = vOwnerFwd.z = vToVictim.z = 0;
	if (vToVictim.Length() < 1)
		return false;
	vVictimFwd = vVictimFwd.Normalize();
	vOwnerFwd = vOwnerFwd.Normalize();
	vToVictim = vToVictim.Normalize();

	return DotProduct(vVictimFwd, vToVictim) > 0.0f		 // victim looking away from you
		   && DotProduct(vOwnerFwd, vToVictim) > 0.5f	 // you're facing them
		   && DotProduct(vOwnerFwd, vVictimFwd) > -0.3f; // views roughly aligned
#else
	return false;
#endif
}


void CCrowbar::PrimaryAttack()
{
	m_bHeavySwing = false;
	if (!Swing(1))
	{
		SetThink(&CCrowbar::SwingAgain);
		pev->nextthink = gpGlobals->time + 0.1;
	}
}

void CCrowbar::SecondaryAttack()
{
	m_bHeavySwing = true;

	// The stab animation + swing sound start RIGHT AT CLICK, on both client and server,
	// so client prediction sees them (the event is FEV_NOTHOST, owner never gets it from the server).
	// The stab anim has its own wind-up built in. iparam1 = 1 -> heavy.
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usCrowbar,
		0.0, g_vecZero, g_vecZero, 0, 0, 1, 0, 0, 0);
	m_pPlayer->SetAnimation(PLAYER_ATTACK1);

	// lock both buttons for wind-up + recovery
	m_flNextPrimaryAttack = m_flNextSecondaryAttack =
		GetNextAttackDelay(KNIFE_HEAVY_WINDUP + KNIFE_HEAVY_DELAY);

#ifndef CLIENT_DLL
	// backstab skips the wind-up: damage lands instantly
	TraceResult trCheck;
	Vector vecSrcCheck, vecEndCheck;
	TraceMelee(trCheck, vecSrcCheck, vecEndCheck);
	if (trCheck.flFraction < 1.0 && CanBackstab(CBaseEntity::Instance(trCheck.pHit)))
	{
		HeavyStrike();
		return;
	}

	// otherwise the damage lands after the wind-up (server only)
	SetThink(&CCrowbar::HeavyStrike);
	pev->nextthink = gpGlobals->time + KNIFE_HEAVY_WINDUP;
#endif
}

void CCrowbar::HeavyStrike()
{
	if (!m_pPlayer || !m_pPlayer->IsAlive())
		return;

	m_bHeavySwing = true;
	Swing(1);
}


void CCrowbar::Smack()
{
	DecalGunshot(&m_trHit, BULLET_PLAYER_CROWBAR);
}


void CCrowbar::SwingAgain()
{
	Swing(false);
}


bool CCrowbar::Swing(bool fFirst)
{
	bool fDidHit = false;

	TraceResult tr;
	Vector vecSrc, vecEnd;

	TraceMelee(tr, vecSrc, vecEnd);

	const float flDelay = KNIFE_SLASH_DELAY;

	// Light attack plays its event here. The heavy one already played it at click time
	// in SecondaryAttack(), and its lockout is already set there too.
	const bool fLight = !m_bHeavySwing;

	if (fFirst && fLight)
	{
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usCrowbar,
			0.0, g_vecZero, g_vecZero, 0, 0, 0, 0, 0, 0);
	}


	if (tr.flFraction >= 1.0)
	{
		if (fFirst && fLight)
		{
			// miss: same delay as a hit so you can't alternate buttons to bypass it
			m_flNextPrimaryAttack = m_flNextSecondaryAttack = GetNextAttackDelay(flDelay);

			// player "shoot" animation
			m_pPlayer->SetAnimation(PLAYER_ATTACK1);
		}
	}
	else
	{
		if (fLight)
		{
			switch ((m_iSwing++) % 2)
			{
			case 0:
				SendWeaponAnim(CROWBAR_SLASH1);
				break;
			case 1:
				SendWeaponAnim(CROWBAR_SLASH2);
				break;
			}

			// player "shoot" animation
			m_pPlayer->SetAnimation(PLAYER_ATTACK1);
		}

#ifndef CLIENT_DLL

		// hit
		fDidHit = true;
		CBaseEntity* pEntity = CBaseEntity::Instance(tr.pHit);

		ClearMultiDamage();

		float flDamage = m_bHeavySwing ? KNIFE_HEAVY_DAMAGE : KNIFE_SLASH_DAMAGE;
		if (CanBackstab(pEntity))
			flDamage = 9999; // instakill, armor can't save it

		pEntity->TraceAttack(m_pPlayer->pev, flDamage, gpGlobals->v_forward, &tr, DMG_CLUB);
		ApplyMultiDamage(m_pPlayer->pev, m_pPlayer->pev);
		MeleeKB_OnMeleeHit(m_pPlayer, tr.vecPlaneNormal, pEntity);

#endif

		// don't let SwingAgain overwrite the recovery time (heavy lockout was set at click)
		if (fFirst && fLight)
		{
			m_flNextPrimaryAttack = m_flNextSecondaryAttack = GetNextAttackDelay(flDelay);
		}

#ifndef CLIENT_DLL
		// play thwack, smack, or dong sound
		float flVol = 1.0;
		bool fHitWorld = true;

		if (pEntity)
		{
			if (pEntity->Classify() != CLASS_NONE && pEntity->Classify() != CLASS_MACHINE)
			{
				// play thwack or smack sound
				switch (RANDOM_LONG(0, 2))
				{
				case 0:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hitbod1.wav", 1, ATTN_NORM);
					break;
				case 1:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hitbod2.wav", 1, ATTN_NORM);
					break;
				case 2:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hitbod3.wav", 1, ATTN_NORM);
					break;
				}
				m_pPlayer->m_iWeaponVolume = CROWBAR_BODYHIT_VOLUME;
				if (!pEntity->IsAlive())
					return true;
				else
					flVol = 0.1;

				fHitWorld = false;
			}
		}

		// play texture hit sound
		// UNDONE: Calculate the correct point of intersection when we hit with the hull instead of the line

		if (fHitWorld)
		{
			float fvolbar = TEXTURETYPE_PlaySound(&tr, vecSrc, vecSrc + (vecEnd - vecSrc) * 2, BULLET_PLAYER_CROWBAR);

			if (g_pGameRules->IsMultiplayer())
			{
				// override the volume here, cause we don't play texture sounds in multiplayer,
				// and fvolbar is going to be 0 from the above call.

				fvolbar = 1;
			}

			// also play crowbar strike
			switch (RANDOM_LONG(0, 1))
			{
			case 0:
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hit1.wav", fvolbar, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
				break;
			case 1:
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hit2.wav", fvolbar, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
				break;
			}

			// delay the decal a bit
			m_trHit = tr;
		}

		m_pPlayer->m_iWeaponVolume = flVol * CROWBAR_WALLHIT_VOLUME;
#endif
		SetThink(&CCrowbar::Smack);
		pev->nextthink = gpGlobals->time + 0.2;
	}
	return fDidHit;
}