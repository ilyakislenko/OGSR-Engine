// portal-mod E54: destructible buildings and chips (tools/engine_patches/patch_e54.py, tools/destruct_build.py).
// A prepared building (levels\<level>\level.pmd) is cut out of the level's collision at load and stands in its place
// as pieces - their box triangles appended to the static collision model, each piece a model of its own - held by
// bonds to each other and to the ground. Bullets (by calibre) and explosions damage pieces; a destroyed piece turns
// into rubble; a structural solver (load paths to the ground, moments on horizontal bonds) breaks overloaded bonds;
// what has lost its support falls as rigid bodies (islands of pieces bonded together) which break up and damage what
// they hit. Big calibres chip real fragments off stone, concrete, brick and wood anywhere on the level.
// E56: rounds and blasts dent any face of stone, concrete, brick and wood (the face cut away by the shaders inside a
// ragged edge, a bowl behind it), the damage adding up at a spot; kept with the game like the holes.
#pragma once

struct SBullet;

namespace portal_mod_destruct
{
float ammo_damage(LPCSTR ammo_section); // [pm_destruct_ammo]: what one round does to a wall (0: nothing)
void bullet_hit(const SBullet& b, u32 tri, const Fvector& point, const Fvector& normal, u16 target_material); // main thread
void explosion(const Fvector& pos, float radius, float hit, float impulse); // CExplosive::Explode
bool hole_pass(const Fvector& p); // E54b: a point at the level inside an open hole (a bullet flies on)
void update(); // CLevel::OnFrame
void shutdown(); // CLevel::~CLevel (before the physics world goes)
// E54d: what was destroyed on each level (pieces, bonds, falling islands, holes) is kept across level changes and
// written beside the saved game as <save>.sav.destruct
void save_game(LPCSTR sav_path); // CALifeStorageManager::save, after the save is written (its full path)
void load_game(LPCSTR sav_path); // CALifeStorageManager::load (the save's full path) and new_game (nullptr: forget it all)

// Lua (level.destruct_*)
LPCSTR lua_info();
void lua_explode(const Fvector& pos, float radius, float power);
int lua_piece_at(const Fvector& pos);
Fvector lua_piece_pos(int id);
int lua_piece_state(int id);
void lua_chips(const Fvector& pos, const Fvector& normal, float damage);
float lua_hole_at(const Fvector& pos); // E54b
float lua_dent_at(const Fvector& pos); // E56: the radius of the biggest dent there (0: none)
Fvector lua_wall_probe(const Fvector& from, const Fvector& dir, float range); // E54b: distance, thickness, class
void lua_profile(bool on); // E54e: a "PROFILE" line each second - where the frame's time goes (and every frame over 40 ms)
void lua_crush_immune(int id, bool on); // E54e: falling pieces never hit this object (Kruglov)
int lua_demolish(const Fvector& pos, float radius, float y_lo, float y_hi); // E54e: the supports in a band go at once
} // namespace portal_mod_destruct

// called from xr_3da (CObjectSpace::Load) and the renderer (CRender::LoadBuffers); defined in portal_mod_destruct.cpp
bool portal_mod_destruct_cform(const Fvector* verts, size_t nv, const CDB::TRI* tris, size_t nt, xr_vector<Fvector>& out_v, xr_vector<CDB::TRI>& out_t);
void portal_mod_destruct_ib(bool fast, u32 ib, u16* data, u32 count);
// defined in Layers/xrRenderPC_R4/r4_loader.cpp: a level index buffer rewritten at the start of the next frame's render
void portal_mod_render_queue_ib(bool fast, u32 ib, const u16* data, u32 count);
