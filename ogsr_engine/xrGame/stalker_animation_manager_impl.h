////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager_impl.h
//	Created 	: 16.11.2003
//  Modified 	: 16.11.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "CharacterPhysicsSupport.h"
#include "portal_mod_wounds.h" // portal-mod E71: the limp (dmg_norm_)

// portal-mod E71: a limping stalker standing walks, runs and stands on SoC's own dmg_norm_ set (state 2)
IC CStalkerAnimationManager::EBodyState CStalkerAnimationManager::body_state() const
{
    const EBodyState state = object().movement().body_state();
    return ((state == MonsterSpace::eBodyStateStand && portal_mod_wounds::limping(&object())) ? EBodyState(2) : state);
}

IC bool CStalkerAnimationManager::standing() const
{
    CAI_Stalker& obj = object();
    CStalkerMovementManager& movement = obj.movement();

    if (movement.speed(obj.character_physics_support()->movement()) < EPS_L)
        return (true);

    if (eMovementTypeStand == movement.movement_type())
        return (true);

    return (false);
}

IC void CStalkerAnimationManager::fill_object_info()
{
    CInventoryItem* item = object().inventory().ActiveItem();
    VERIFY(item);
    m_weapon = smart_cast<CWeapon*>(item);
    m_missile = smart_cast<CMissile*>(item);
}

IC bool CStalkerAnimationManager::strapped() const
{
    VERIFY(m_weapon);
    return (object().CObjectHandler::weapon_strapped(m_weapon));
}

IC u32 CStalkerAnimationManager::object_slot() const
{
    if (m_weapon)
        return (m_weapon->animation_slot());

    if (m_missile)
        return (m_missile->animation_slot());

    return (0);
}
