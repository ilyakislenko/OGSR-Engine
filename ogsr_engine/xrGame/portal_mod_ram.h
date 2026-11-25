// portal-mod E72: vehicles break what they ram (tools/engine_patches/patch_e72.py).
// A moving car (the Niva, the cars, the junk bike) looks a little ahead of itself every frame: the level's collision
// triangles its box will sweep within lead_time. Those of a breakable thing - a pole, a fence piece or a lamp (E59), a
// tree's trunk (E58), a piece of a destructible building (E54: the construction site, the ramming walls) - are weighed
// against the car's energy along the face (0.5 m v^2): what it can break is broken just before the car gets there, so
// the car goes through and the freed bodies meet it and fly; what it cannot break takes the damage and stops it as before.
// Breaking costs the car its energy (it slows: v' = sqrt(v^2 - 2 E / m)) and a dent (a strike hit on itself, which
// pm_car.script's crash rule caps). Physics props (crates, barrels, E57) the car strikes take a blow by the energy of
// the contact; a door or a gate leaf (a "door" joint) struck hard enough is torn off its hinge or its rail and thrown.
// Tuned in [pm_ram] (config\portal_mod\pm_ram.ltx); the My Mods switch "ram" (level.pm_feature_set("ram", on)).
#pragma once

class CCar;

namespace portal_mod_ram
{
void car_update(CCar* car); // CCar::UpdateCL and CCar::UpdateEx (main thread, every frame; once a frame per car)
bool feature_set(LPCSTR name, bool on); // level.pm_feature_set: "ram" is this one's (true: handled)
bool enabled(); // the switch (and [pm_ram] enabled)

// Lua (level.ram_*)
LPCSTR lua_info(); // counters: what was broken, by kind, the last ram
LPCSTR lua_events(); // what happened since the last call: "kind|id|x|y|z|energy kJ|speed before|speed after;" (kind:
// statics, tree, piece, prop, door) - the scripts make a rammed door stay broken (pm_ram.script)
void lua_debug(bool on); // a log line for each thing weighed
} // namespace portal_mod_ram
