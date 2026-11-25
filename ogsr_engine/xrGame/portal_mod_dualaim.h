// portal-mod E70: dual wield split targeting (the T-800 series, 2026-11-26). With two guns in the hands (E26/E27/E31:
// the main slot's HUD item drawn again mirrored), the right gun shoots where the crosshair is and the left gun has a
// target of its own: scripts\pm_dual_aim.script picks it (the nearest enemy in sight that is not under the crosshair,
// or one the player marks) and hands its id here. Each frame the mirrored item - the left arm with its gun - turns at
// that target about a point between its hand and its shoulder, within the arc a real arm has, as fast as the robot's
// arm swings; a round fired from the left hand goes where that barrel points (onto the target once it is there), with
// the weapon's own spread and recoil:
//   in turn (pistols, a rifle tapped: E27)  the left hand's shots;
//   both at once (a rifle held)             a second round from the magazine at the left target with each shot.
// The My Mods switch "dual_aim" is read from $app_data_root$\mod_manager.ltx when a level is first used and set from
// Lua (level.dual_aim_switch) at start and on Apply; switched off nothing turns and every round goes to the crosshair.
#pragma once

struct attachable_hud_item;
class CWeapon;
class CActor;
class IKinematics;

namespace pm_dualaim
{
// player_hud.cpp, attachable_hud_item::calc_dual_transform(): the mirrored item turned at the left target
void turn_left(attachable_hud_item* h);
// WeaponMagazined.cpp, before a round's FireTrace (p, d: the crosshair's round). 0: as it is; 1: this round is the left
// hand's - p, d changed to the left gun's; 2: both at once - fire p2, d2 too (the left gun's round)
int shot(CWeapon* w, Fvector& p, Fvector& d, Fvector& p2, Fvector& d2);
// E70b, the third person (a scene's camera, E36/E39's two-gun pose):
// ActorAnimation.cpp, CActor::TpMirrorCallback: a mirrored left-arm bone (its turn M) turned with the arm at the target
void tp_left_turn(CActor* A, IKinematics* K, u16 bone, Fmatrix& M);
// Weapon.cpp, CWeapon::TpDualLeftXform: the left gun (in the actor's model space, off the left hand) turned with it
void tp_left_gun(const CActor* A, const Fvector& hand, Fmatrix& m2m);
} // namespace pm_dualaim

void pm_dualaim_script_register(lua_State* L); // level.dual_aim_* (level_script.cpp calls it)
