// portal-mod E71: NPC wounds by body part. Every round that hits a stalker is put down to the part it hit (by the
// bone's name: head, torso, left / right arm with the hand, left / right leg) as the health it took; damage stays as the
// game makes it, only the reactions are new:
//   - a leg that has taken limp_damage: the stalker limps - SoC's own "dmg_norm_" locomotion (a whole limping set in
//     stalker_animation.omf, loaded by the engine as body state 2 and never used) on standing walks and runs, slower;
//   - legs that have taken down_damage (or one round of down_single): he goes down (scripts\pm_wounds.script sits him
//     wounded where he is, SoC's own wounded scheme); a "crawl" flag (level.pm_wound_set) plays walk_fwd_crawl_0 -
//     SoC's unused hunched creep - as a global animation at crawl_speed, for a script that wants it;
//   - an arm hit of arm_damage: the script drops his weapon;
//   - a new limp, fall or arm hit plays the game's own critical-hit animation for that part when it can (standing, in
//     combat, a weapon in hand: CAI_Stalker::critical_wound_external_conditions_suitable).
// The script reads each hit and each new reaction from a queue (level.pm_wound_events), decides surrender itself and
// tells the engine (level.pm_wound_set). My Mods switch "npc_wounds": read from $app_data_root$\mod_manager.ltx when a
// level is first used, set by level.pm_wounds_switch(on); off, nothing is counted, nobody limps.
#pragma once

#include "../Include/xrRender/KinematicsAnimated.h"

class CAI_Stalker;
class CStalkerAnimationData;
struct SHit;
struct lua_State;

namespace portal_mod_wounds
{
// CAI_Stalker::Hit after inherited::Hit: the critical wound type (stalker_animation_names.h, 4..9) to start now, or -1
int on_hit(CAI_Stalker* npc, const SHit& hit, float health_before);
bool limping(const CAI_Stalker* npc); // the animation's body state: dmg_norm_ instead of norm_
float speed(const CAI_Stalker* npc, float v); // a movement speed, slowed for a limp or a crawl
MotionID global_anim(CAI_Stalker* npc, bool moving); // the crawl (invalid: none)
void fill_dmg(CStalkerAnimationData* data); // dmg_norm_ sets the OMF lacks: norm_'s (no empty vector is ever selected)
} // namespace portal_mod_wounds

void pm_wounds_script_register(lua_State* L); // level.pm_wound_*
