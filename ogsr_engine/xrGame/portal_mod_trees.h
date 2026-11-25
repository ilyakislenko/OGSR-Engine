// portal-mod E58: falling trees (tools/engine_patches/patch_e58.py).
// Every tree of the level (an MU instance whose parts have flora\ shaders and whose trunk is in the level's collision)
// takes damage from rounds on its trunk (below cut_h; the crown's branches barely) and from blasts that reach it
// ([pm_trees]: hit points by the trunk's cross-section). Past them it comes down: its collision triangles leave the
// static model, the renderer draws it by a new matrix each frame (Layers/xrRenderPC_R4/r4_loader.cpp), and it turns
// over its root like a rod under gravity, away from the shots or the blast, until its trunk or crown meets the ground,
// a building or another tree; it bounces once and lies there (a static box for its trunk). Whatever stands where it
// falls is hit by its weight and speed (a man under a big one dies); physics objects are pushed and props break.
// Fallen trees are kept across level changes and with the saved game (<save>.sav.trees).
#pragma once

namespace portal_mod_trees
{
void bullet_hit(const Fvector& point, const Fvector& dir, u32 tri, float dmg); // portal_mod_destruct::bullet_hit (a static triangle hit)
void explosion(const Fvector& pos, float radius, float hit, float impulse); // portal_mod_destruct::explosion
void update(); // portal_mod_destruct::update (main thread)
void reset(); // portal_mod_destruct::shutdown (the level goes, before the physics world)
void save_game(LPCSTR sav_path); // portal_mod_destruct::save_game
void load_game(LPCSTR sav_path); // portal_mod_destruct::load_game (nullptr: a new game)

// Lua (level.trees_*)
LPCSTR lua_info();
LPCSTR lua_near(const Fvector& pos, float radius); // "id|x|y|z|height|trunk r|hp|damage|state|distance;" nearest first
bool lua_fell(int id, const Fvector& dir); // bring it down now, falling along dir (horizontal part)
void lua_debug(bool on);
} // namespace portal_mod_trees

// defined in Layers/xrRenderPC_R4/r4_loader.cpp: the level's MU instances (an FLOD and its parts)
u32 portal_mod_tree_count();
bool portal_mod_tree_get(u32 i, Fmatrix& xf, Fbox& box, u32& flags); // flags: 1 a flora\ shader on a part, 2 a trunk shader
void portal_mod_tree_set(u32 i, const Fmatrix& xf, int state); // 0 back in place, 1 drawn at xf, 2 hidden (queued)
// defined in portal_mod_destruct.cpp: the first of three vertices far under the level (u32(-1): none)
extern u32 g_pm_cform_sink;
