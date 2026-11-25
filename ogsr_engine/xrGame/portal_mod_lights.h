// portal-mod E75: lights break (tools/engine_patches/patch_e75.py).
// The lights of a level under R4 are its hanging_lamp objects (CHangingLamp): a lamp with a model of its own (its bulb
// on a light bone) or a bare light the level's designers put in a lamp of the level's geometry (no model). The level's
// own compiled lights are not drawn by OGSR's R4 (Light_DB: static lights are skipped) - only their copies that light
// dynamic objects (build_lights, STYPE_LIGHTSOURCEHEMI) are, and those go out with the lamp beside them.
// A round passing the bulb (or the bare light), or hitting the level right at its fixture, breaks it: the light goes out
// with a flash, glass and sparks fly, a brutal pop; a bare light inside one of E59's lamps (level.pms) goes out when that
// lamp is shot down or its pole falls, and a shot bare light drops E59's lamp round it. Broken lamps stay broken in saves
// (the lamp's own alife health, 0). An NPC's head torch takes a round that passes its lens: it goes out with sparks and
// the round stops in it (the NPC only flinches); kept with the torch's condition (0). The player's own torch takes enemy
// rounds in the head (by chance): the first makes it flicker, the second kills it; it is repaired by switching it on out
// of fire (8 s without a hit): 3 s of fiddling, then it is whole and on.
// Config: config\portal_mod\pm_lights.ltx ([pm_lights], read by this module itself). Switched off (My Mods, "lights"):
// nothing breaks any more, the torches work as whole ones (their damage is kept for when it is switched on again),
// broken lamps stay dark.
#pragma once

class CObject;
class CHangingLamp;
class CTorch;
struct SBullet;
struct SHit;

namespace portal_mod_lights
{
// the bullet manager (Level_Bullet_Manager.cpp)
void snapshot(); // CommitRenderSet (main thread): the bulbs and torches the bullets test this frame
float torch_clip(const SBullet* b, const Fvector& pos, const Fvector& dir, float range, int& target); // worker: the
// first lit head torch on the step: the distance to it (the step is cut there) and its index, else range and -1
void bullet_step(const SBullet* b, const Fvector& from, const Fvector& dir, float len, bool stopped_static); // worker:
// the step flown (from, dir, len); stopped_static: it ended in the level at its end
void torch_reached(const SBullet* b, int target, const Fvector& at, const Fvector& dir); // worker: the round is in it
void commit(); // CommitEvents (main thread, the frame's start): what the bullets broke, the actor's torch, E59 links
void dynamic_hit(CObject* o, u16 bone, const Fvector& point, const Fvector& dir, u16 shooter, u16 weapon); // a round in
// a creature (DynamicObjectHit): the actor's torch, an NPC's torch next to the hit

// hanging lamps (HangingLamp.cpp)
void lamp_hit(CHangingLamp* l, const SHit* h, bool was_alive); // after the game's own Hit (a light bone hit kills it)
bool lamp_turn_on_blocked(CHangingLamp* l); // a broken lamp is not turned on again (scripts' turn_on)

void shutdown(); // E75c, CLevel::~CLevel (after the bullet manager): its sounds and flashes go with the level

// torches (Torch.cpp, CTorch::Switch): what the switch really does - a broken one stays dark, the actor's starts its
// repair out of fire
bool torch_switch(CTorch* t, bool on);

// Lua (level.lights_*)
void lua_enable(bool on);
bool lua_enabled();
LPCSTR lua_info();
LPCSTR lua_near(const Fvector& pos, float radius); // "id|kind|state|x|y|z|range|dist;" nearest first; kind lamp (its
// own model) | light (bare, in a lamp of the level's); state on | off | broken
LPCSTR lua_torches(const Fvector& pos, float radius); // "npc_id|torch_id|state|x|y|z|dist;" NPCs with a torch, nearest
// first; state on | off | broken
bool lua_break(int id); // a lamp now (as if shot from above)
bool lua_torch_break(int id); // an NPC's (or the torch's own id) head torch now
Fvector lua_torch_pos(int id); // an NPC's head torch's lens (0,0,0: none)
LPCSTR lua_actor_torch(); // none | ok | flicker | dead | repair
void lua_actor_torch_damage(); // one enemy round in it
void lua_actor_torch_repair(); // whole at once
void lua_debug(bool on);
} // namespace portal_mod_lights

// defined in Layers/xrRender/Light_DB.cpp: the level's hemi lights (build_lights) within r of p on or off; how many
u32 portal_mod_hemi_set(const Fvector& p, float r, bool on);
