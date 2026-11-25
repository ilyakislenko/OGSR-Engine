// portal-mod E59: poles, fences, lamps and wires of the level break (tools/engine_patches/patch_e59.py,
// tools/statics_build.py).
// levels\<level>\level.pms lists them: each one's models (a pole's top and stump, cut over its foot; a fence piece, a
// lamp), its boxes and mass, its own level collision triangles and the render triangles to cut out of the level's index
// buffers. Rounds and blasts add up damage by its kind ([pm_statics]: hit points by a pole's cross-section, a fence
// piece's face); past them its triangles leave the level (drawn and collision) and it goes on as a body: a pole's top
// turns over its stump away from the shots or the blast (the lamps on it ride with it, the wires off it snap), a fence
// piece is thrown over, a lamp drops in a burst of glass. What a falling body reaches is hit by its weight and speed.
// Kept across level changes and with the saved game (<save>.sav.statics).
// E61: panes of glass (glas\* textures) shatter under a round or a blast: gone, a burst of glass, no body.
#pragma once

namespace CDB
{
class TRI;
}

namespace portal_mod_statics
{
void load_level(size_t nv, size_t nt); // portal_mod_destruct_cform (the level's collision is being read)
void remap_cform(const xr_vector<u8>& cut); // ... when the destruction cut triangles out of it (their indices shift)
void append_cform(xr_vector<Fvector>& out_v, xr_vector<CDB::TRI>& out_t); // ... its last step (after the sink): the
// triangles cut between objects go, their pieces come in
void on_ib(bool fast, u32 ib, const u16* data, u32 count); // portal_mod_destruct_ib (the renderer reads the level's IBs)
void bullet_hit(const Fvector& point, const Fvector& dir, u32 tri, float dmg); // portal_mod_destruct::bullet_hit
float thin_at(u32 tri); // ... a standing plank fence piece's triangle: its thickness (E54b holes in it), else 0
bool owns(u32 tri); // E62: a level collision triangle of a pole's, fence piece's, lamp's, pane's (a breach leaves it alone)
void pass_hit(const Fvector& point, const Fvector& dir, u32 tri, float dmg); // E61: a round through a triangle it passes
// whole (glass): queued (the bullets' thread), taken as a bullet_hit at the next update
void explosion(const Fvector& pos, float radius, float hit, float impulse); // portal_mod_destruct::explosion
void update(); // portal_mod_destruct::update (main thread)
void reset(); // portal_mod_destruct::shutdown (the level goes, before the physics world)
void save_game(LPCSTR sav_path); // portal_mod_destruct::save_game
void load_game(LPCSTR sav_path); // portal_mod_destruct::load_game (nullptr: a new game)

// Lua (level.statics_*)
LPCSTR lua_info();
LPCSTR lua_near(const Fvector& pos, float radius, LPCSTR kinds); // "id|kind|x|y|z|height|hp|damage|state|distance;" nearest
// first (a pole: its axis at the cut; the others: their box's centre); kinds: "" all, else a comma list of parts of kind names
bool lua_break(int id, const Fvector& dir); // break it now, falling along dir
LPCSTR lua_probe(int id); // "hits/rays": the level's collision met inside its first box (broken: should be 0/24)
void lua_debug(bool on);
} // namespace portal_mod_statics

// defined in Layers/xrRenderPC_R4/r4_loader.cpp: a range of a level index buffer rewritten at the next frame's render
void portal_mod_render_queue_ib_part(bool fast, u32 ib, u32 off, const u16* data, u32 count);
