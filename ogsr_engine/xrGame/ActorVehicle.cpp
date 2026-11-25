#include "stdafx.h"

#include "actor.h"
#include "../xr_3da/camerabase.h"

#include "ActorEffector.h"
#include "holder_custom.h"
#ifdef DEBUG
#include "PHDebug.h"
#endif
#include "alife_space.h"
#include "hit.h"
#include "PHDestroyable.h"
#include "Car.h"
#include "..\Include/xrRender/Kinematics.h"
#include "..\Include/xrRender/KinematicsAnimated.h"
#include "PHShellSplitter.h"

#include "actor_anim_defs.h"
#include "game_object_space.h"
#include "characterphysicssupport.h"
#include "inventory.h"

#include "script_callback_ex.h"
#include "script_game_object.h"

void CActor::attach_Vehicle(CHolderCustom* vehicle)
{
    if (!vehicle)
        return;

    if (m_holder)
        return;
    PickupModeOff();
    m_holder = vehicle;

    IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(Visual());
    R_ASSERT(V);

    if (!m_holder->attach_Actor(this))
    {
        m_holder = NULL;
        return;
    }
    // temp play animation
    CCar* car = smart_cast<CCar*>(m_holder);
    u16 anim_type = car->DriverAnimationType();
    SVehicleAnimCollection& anims = m_vehicle_anims->m_vehicles_type_collections[anim_type];
    V->PlayCycle(anims.idles[0], FALSE);

    ResetCallbacks();
    u16 head_bone = V->dcast_PKinematics()->LL_BoneID("bip01_head");
    V->dcast_PKinematics()->LL_GetBoneInstance(u16(head_bone)).set_callback(bctPhysics, VehicleHeadCallback, this);

    character_physics_support()->movement()->DestroyCharacter();
    mstate_wishful = 0;
    m_holderID = car->ID();

    SetWeaponHideState(INV_STATE_CAR, true);

    CStepManager::on_animation_start(MotionID(), 0);

    this->callback(GameObject::eAttachVehicle)(car->lua_game_object());
}

void CActor::detach_Vehicle()
{
    if (!m_holder)
        return;
    CCar* car = smart_cast<CCar*>(m_holder);
    if (!car)
        return;
    CPHShellSplitterHolder* sh = car->PPhysicsShell()->SplitterHolder();
    if (sh)
        sh->Deactivate();
    if (!character_physics_support()->movement()->ActivateBoxDynamic(0))
    {
        if (sh)
            sh->Activate();
        return;
    }
    if (sh)
        sh->Activate();
    m_holder->detach_Actor(); //

    character_physics_support()->movement()->SetPosition(m_holder->ExitPosition());
    character_physics_support()->movement()->SetVelocity(m_holder->ExitVelocity());

    r_model_yaw = -m_holder->Camera()->yaw;
    r_torso.yaw = r_model_yaw;
    r_model_yaw_dest = r_model_yaw;
    m_holder = NULL;
    SetCallbacks();
    IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(Visual());
    R_ASSERT(V);
    V->PlayCycle(m_anims->m_normal.legs_idle);
    V->PlayCycle(m_anims->m_normal.m_torso_idle);
    m_holderID = u16(-1);

    //.	SetWeaponHideState(whs_CAR, FALSE);
    SetWeaponHideState(INV_STATE_CAR, false);

    this->callback(GameObject::eDetachVehicle)(car->lua_game_object());
}

bool CActor::use_Vehicle(CHolderCustom* object)
{
    //	CHolderCustom* vehicle=smart_cast<CHolderCustom*>(object);
    CHolderCustom* vehicle = object;
    Fvector center;
    Center(center);
    if (m_holder)
    {
        if (!vehicle && m_holder->Use(Device.vCameraPosition, Device.vCameraDirection, center))
            detach_Vehicle();
        else
        {
            if (m_holder == vehicle)
                if (m_holder->Use(Device.vCameraPosition, Device.vCameraDirection, center))
                    detach_Vehicle();
        }
        return true;
    }
    else
    {
        if (vehicle)
        {
            if (vehicle->Use(Device.vCameraPosition, Device.vCameraDirection, center))
            {
                if (pCamBobbing)
                {
                    Cameras().RemoveCamEffector(eCEBobbing);
                    pCamBobbing = NULL;
                }

                attach_Vehicle(vehicle);
            }

            else if (auto car = smart_cast<CCar*>(vehicle))
            {
                this->callback(GameObject::eUseVehicle)(car->lua_game_object());
            }

            return true;
        }
        return false;
    }
}

void CActor::on_requested_spawn(CObject* object)
{
    CCar* car = smart_cast<CCar*>(object);
    //attach_Vehicle(car);

    // insead of immediate attach, we set a pending car to be attached in Actor::UpdateCl after 10 frames

    m_pending_car = car;
    m_pending_car_frames = 10;
}

// portal-mod E44: into and out of a helicopter the player flies: seated, the character's physics body is taken away (the
// helicopter carries the player's XFORM) and the weapon put away; out, the body comes back beside the cockpit with the
// helicopter's velocity, looking the way the view looked
bool CActor::use_Helicopter(CHolderCustom* object)
{
    if (m_holder)
    {
        if (object && m_holder != object)
            return true;
        CGameObject* GO = smart_cast<CGameObject*>(m_holder);
        const Fvector pos = m_holder->ExitPosition();
        const Fvector vel = m_holder->ExitVelocity();
        const float yaw = m_holder->Camera() ? m_holder->Camera()->yaw : -r_model_yaw;
        m_holder->detach_Actor();
        m_holder = NULL;
        XFORM().setHPB(-yaw, 0.f, 0.f);
        XFORM().c.set(pos);
        character_physics_support()->movement()->CreateCharacter();
        character_physics_support()->movement()->SetPosition(pos);
        character_physics_support()->movement()->SetVelocity(vel);
        r_model_yaw = -yaw;
        r_torso.yaw = r_model_yaw;
        r_model_yaw_dest = r_model_yaw;
        cam_Active()->yaw = yaw;
        cam_Active()->pitch = 0.f;
        SetWeaponHideState(INV_STATE_CAR, false);
        Msg("* E44 the player out of %s at (%.2f, %.2f, %.2f), %.1f m/s", GO->cName().c_str(), pos.x, pos.y, pos.z, vel.magnitude());
        this->callback(GameObject::eDetachVehicle)(GO->lua_game_object());
        return true;
    }
    if (!object)
        return false;
    Fvector center;
    Center(center);
    if (!object->Use(Device.vCameraPosition, Device.vCameraDirection, center) || !object->attach_Actor(this))
        return false;
    character_physics_support()->movement()->DestroyCharacter();
    PickupModeOff();
    mstate_wishful = 0;
    m_holder = object;
    if (pCamBobbing)
    {
        Cameras().RemoveCamEffector(eCEBobbing);
        pCamBobbing = NULL;
    }
    SetWeaponHideState(INV_STATE_CAR, true);
    this->callback(GameObject::eAttachVehicle)(smart_cast<CGameObject*>(object)->lua_game_object());
    return true;
}