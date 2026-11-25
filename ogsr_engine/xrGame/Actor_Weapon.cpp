// Actor_Weapon.cpp:	 для работы с оружием
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "actor.h"
#include "actoreffector.h"
#include "Missile.h"
#include "inventory.h"
#include "weapon.h"
#include "map_manager.h"
#include "level.h"
#include "CharacterPhysicsSupport.h"
#include "EffectorShot.h"
#include "WeaponMagazined.h"
#include "game_base_space.h"
#include "../xr_3da/CustomHUD.h"
#include "WeaponKnife.h"
#include "WeaponBinoculars.h"
#include "portal_mod.h" // portal-mod E28
#include "../xr_3da/CameraBase.h" // portal-mod E28

constexpr float VEL_MAX = 10.f;
constexpr float VEL_A_MAX = 10.f;

#define GetWeaponParam(pWeapon, func_name, def_value) ((pWeapon) ? (pWeapon->func_name) : def_value)

//возвращает текуший разброс стрельбы (в радианах)с учетом движения
float CActor::GetWeaponAccuracy() const
{
    CWeapon* W = smart_cast<CWeapon*>(inventory().ActiveItem());

    if (m_bZoomAimingMode && W && !GetWeaponParam(W, IsRotatingToZoom(), false))
        return m_fDispAim;

    float dispersion = m_fDispBase * GetWeaponParam(W, Get_PDM_Base(), 1.0f);

    CEntity::SEntityState state;
    if (g_State(state))
    {
        // angular factor
        dispersion *= (1.f + (state.fAVelocity / VEL_A_MAX) * m_fDispVelFactor * GetWeaponParam(W, Get_PDM_Vel_F(), 1.0f));
        //		Msg("--- base=[%f] angular disp=[%f]",m_fDispBase, dispersion);
        // linear movement factor
        bool bAccelerated = isActorAccelerated(mstate_real, IsZoomAimingMode());
        if (bAccelerated)
            dispersion *= (1.f +
                           (state.fVelocity / VEL_MAX) * m_fDispVelFactor * GetWeaponParam(W, Get_PDM_Vel_F(), 1.0f) *
                               (1.f + m_fDispAccelFactor * GetWeaponParam(W, Get_PDM_Accel_F(), 1.0f)));
        else
            dispersion *= (1.f + (state.fVelocity / VEL_MAX) * m_fDispVelFactor * GetWeaponParam(W, Get_PDM_Vel_F(), 1.0f));

        if (state.bCrouch)
        {
            dispersion *= (1.f + m_fDispCrouchFactor * GetWeaponParam(W, Get_PDM_Crouch(), 1.0f));

            if (!bAccelerated)
                dispersion *= (1.f + m_fDispCrouchNoAccelFactor * GetWeaponParam(W, Get_PDM_Crouch_NA(), 1.0f));
        }
    }

    return dispersion;
}

void CActor::g_fireParams(CHudItem* pHudItem, Fvector& fire_pos, Fvector& fire_dir, const bool for_cursor)
{
    fire_dir = Cameras().Direction();
    fire_pos = Cameras().Position();
    if (portal_mod::cine && cam_Active()) // portal-mod E28: a scene camera on - the shot leaves from the actor's own eyes
    {
        fire_dir = cam_Active()->Direction();
        fire_pos = cam_Active()->Position();
        // portal-mod E40: a gun's bullet leaves its world model's muzzle, toward the point the eyes look at (the first
        // thing on the eye line from 1.5 m out - past the gun in the hands - 300 m at most): the tracer starts at the
        // barrel, the hit lands where the scene aims
        if (auto w = smart_cast<CWeapon*>(pHudItem); w && !for_cursor && !smart_cast<CWeaponKnife*>(pHudItem) &&
            !smart_cast<CMissile*>(pHudItem) && !smart_cast<CWeaponBinoculars*>(pHudItem))
        {
            float range = 300.f;
            Fvector from;
            from.mad(fire_pos, fire_dir, 1.5f);
            collide::rq_result RQ;
            if (Level().ObjectSpace.RayPick(from, fire_dir, range, collide::rqtBoth, RQ, this))
                range = RQ.range;
            Fvector aim, d2;
            aim.mad(from, fire_dir, range);
            const Fvector mz = w->get_LastFP();
            d2.sub(aim, mz);
            if (d2.magnitude() > 0.3f)
            {
                d2.normalize();
                fire_pos = mz;
                fire_dir = d2;
            }
        }
    }

    if (smart_cast<CMissile*>(pHudItem) && !for_cursor)
    {
        if (active_cam() == eacFirstEye)
        {
            Fvector offset;
            XFORM().transform_dir(offset, m_vMissileOffset);
            fire_pos.add(offset);
        }
        else
        {
            fire_dir = this->Direction();
            pHudItem->object().Center(fire_pos);
        }
    }
    else if (auto weapon = smart_cast<CWeapon*>(pHudItem);
             weapon && !smart_cast<CWeaponKnife*>(pHudItem) && !smart_cast<CMissile*>(pHudItem) && !smart_cast<CWeaponBinoculars*>(pHudItem))
    {
        if (psHUD_Flags.test(HUD_CROSSHAIR_HARD) && !(weapon->IsZoomed() && !weapon->IsRotatingToZoom()))
        {
            fire_dir = weapon->get_LastFD();
            fire_pos = weapon->get_LastShootPoint();
        }
    }
}

void CActor::g_WeaponBones(int& L, int& R1, int& R2)
{
    R1 = m_r_hand;
    R2 = m_r_finger2;
    L = m_l_finger1;
}

BOOL CActor::g_State(SEntityState& state) const
{
    state.bJump = !!(mstate_real & mcJump);
    state.bCrouch = !!(mstate_real & mcCrouch);
    state.bFall = !!(mstate_real & mcFall);
    state.bSprint = !!(mstate_real & mcSprint);
    state.fVelocity = character_physics_support()->movement()->GetVelocityActual();
    state.fAVelocity = fCurAVelocity;
    return TRUE;
}

void CActor::SetWeaponHideState(u32 State, bool bSet, bool now)
{
    if (g_Alive() && this == Level().CurrentControlEntity())
        this->inventory().SetSlotsBlocked(State, bSet, now);
}

#define ENEMY_HIT_SPOT "mp_hit_sector_location"
BOOL g_bShowHitSectors = TRUE;

void CActor::HitSector(CObject* who, CObject* weapon)
{
    if (!g_bShowHitSectors)
        return;
    if (!g_Alive())
        return;

    bool bShowHitSector = true;

    CEntityAlive* pEntityAlive = smart_cast<CEntityAlive*>(who);

    if (!pEntityAlive || this == who)
        bShowHitSector = false;

    if (weapon)
    {
        CWeapon* pWeapon = smart_cast<CWeapon*>(weapon);
        if (pWeapon)
        {
            if (pWeapon->IsSilencerAttached())
            {
                bShowHitSector = false;
                if (pWeapon->IsGrenadeLauncherAttached())
                {}
            }
        }
    }

    if (!bShowHitSector)
        return;
    Level().MapManager().AddMapLocation(ENEMY_HIT_SPOT, who->ID());
}

void CActor::on_weapon_shot_start(CWeapon* weapon)
{
    CWeaponMagazined* pWM = smart_cast<CWeaponMagazined*>(weapon);
    //*
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (!effector)
    {
        effector = (CCameraShotEffector*)Cameras().AddCamEffector(
            xr_new<CCameraShotEffector>(weapon->camMaxAngle, weapon->camRelaxSpeed, weapon->camMaxAngleHorz, weapon->camStepAngleHorz, weapon->camDispertionFrac));
    }
    R_ASSERT(effector);

    if (pWM)
    {
        if (effector->IsSingleShot())
            update_camera(effector);

        if (pWM->GetCurrentFireMode() == 1)
        {
            effector->SetSingleShoot(TRUE);
        }
        else
        {
            effector->SetSingleShoot(FALSE);
        }
    };

    effector->SetActor(this);
    effector->Shot(weapon->camDispersion + weapon->camDispersionInc * float(weapon->ShotsFired()));

    if (pWM)
    {
        if (pWM->GetCurrentFireMode() != 1)
        {
            effector->SetActive(FALSE);
            update_camera(effector);
        }
    }
}

void CActor::on_weapon_shot_stop(CWeapon* weapon)
{
    //---------------------------------------------
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (effector && effector->IsActive())
    {
        if (effector->IsSingleShot())
            update_camera(effector);
    }
    //---------------------------------------------
    Cameras().RemoveCamEffector(eCEShot);
}

void CActor::on_weapon_hide(CWeapon* weapon)
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (effector && !effector->IsActive())
        effector->Clear();
}

Fvector CActor::weapon_recoil_delta_angle()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    Fvector result = {0.f, 0.f, 0.f};

    if (effector)
        effector->GetDeltaAngle(result);

    return (result);
}

Fvector CActor::weapon_recoil_last_delta()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    Fvector result = {0.f, 0.f, 0.f};

    if (effector)
        effector->GetLastDelta(result);

    return (result);
}
