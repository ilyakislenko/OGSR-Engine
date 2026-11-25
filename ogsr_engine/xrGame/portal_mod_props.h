// portal-mod E57: breakable physics props (tools/engine_patches/patch_e57.py, tools/props_build.py).
// The game's physics props (meshes\physics\: crates, barrels, bottles, chairs, tables, cupboards, junk) take damage from
// rounds, blasts and blows ([pm_props]: hit points by material and size); past them a prop breaks - the object goes
// (its scripts told first: a loot box drops its loot) and its pieces fly as bodies of their own, cut from its own model
// by tools/props_build.py (meshes\portal_mod\props\<category>\<name>\<k>.ogf, props.bin), pushed by the hit, spinning,
// bouncing with their material's sounds; they lie a while and sink away (the oldest first past max_frags).
#pragma once

class CPhysicObject;
struct SHit;

namespace portal_mod_props
{
void hit(CPhysicObject* o, const SHit& h); // CPhysicObject::Hit (after the impulse)
void explosion(const Fvector& pos, float radius, float hit, float impulse); // portal_mod_destruct::explosion (CExplosive::Explode)
void update(); // portal_mod_destruct::update (main thread, outside the physics step)
void reset(); // portal_mod_destruct::shutdown (before the physics world goes) and load_game (a save loaded)

// Lua (level.props_*)
LPCSTR lua_info();
bool lua_break(int id); // break this object now (as from a blow from above)
void lua_debug(bool on); // a line in the log for each hit and blast on a prop
LPCSTR lua_near(const Fvector& pos, float radius); // the physics objects near: "id|section|visual|breakable or why not|hp|damage|distance|material|size;"
} // namespace portal_mod_props
