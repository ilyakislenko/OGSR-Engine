// portal-mod E74: a reboot instead of the player's death (tools/engine_patches/patch_e74.py; the user's T-800
// machinima, 2026-11-26). When the actor's health runs out and a charge is left, CEntity::KillEntity does not kill him:
// the reboot starts instead. The world takes him for dead - his health stays at 0, so g_Alive() is false for the AI (no
// enemy shoots at a corpse, stalkers see a body, his input and hands are off) - but nothing of the death runs (no
// GE_DIE, no ragdoll, no game over). The camera falls with him (the knees give, he topples onto his side, the impact),
// the screen dies to black, the machine's red vision comes back while scripts\pm_reboot.script types the boot sequence
// on the HUD and sends the nearest enemies to search the body; at `total` s he gets back up with `health`. One charge
// (`charges`), back after `recharge` s of play; with none left the death is the game's own. To the save command, the
// main menu and scripts (db.actor:alive()) a rebooting actor is alive: the game can be saved mid-reboot and loaded back
// into it (a loaded actor with no health is one rebooting; pm_reboot.script puts its time and charges back).
// Config: config\portal_mod\pm_reboot.ltx [pm_reboot] (read by this file and by the script). My Mods: "reboot".
#pragma once

class CEntity;
class CEntityAlive;
class CActor;
struct lua_State;

namespace portal_mod_reboot
{
bool intercept(CEntity* e, u16 who); // CEntity::KillEntity: true - the actor reboots instead (the kill is dropped)
bool active(); // a reboot is under way
bool script_alive(const CEntity* e); // the rebooting actor: alive to scripts, the save command and the main menu
bool keeps_capsule(CEntityAlive* e); // CCharacterPhysicsSupport (in_UpdateCL, in_Hit): the actor rebooting (or about to) - no ragdoll
bool spawns_down(CEntityAlive* e); // CCharacterPhysicsSupport::SpawnInitPhysics: loaded mid-reboot - his capsule, no ragdoll
void update(CActor* a); // CActor::UpdateCL, every frame
void script_register(lua_State* L); // CLevel::script_register: level.pm_reboot_*
} // namespace portal_mod_reboot
