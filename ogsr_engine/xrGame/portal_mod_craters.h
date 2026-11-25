// portal-mod E60: craters in the ground (tools/engine_patches/patch_e60.py). A blast over earth, grass, dirt, sand,
// or gravel digs a crater into the level's own collision - the ground's triangles round it are cut out and a
// bowl of new ones takes their place (from a pool appended to the static model at load, its tree refitted), so whoever
// stands there drops into it and what lies there rolls in - and the destruction's E56 dents draw it (the ground cut
// away inside a ragged edge, a bowl of fresh earth under it). Rounds into the ground dig a pit that grows with them;
// past pit_r_min its collision is dug too. Kept across level changes and with the saved game (<save>.sav.craters).
// E62: breaches - a square cut out of a wall's faces where a blast opened it - ride on the same pool.
#pragma once

namespace CDB
{
class TRI;
}

namespace portal_mod_craters
{
void append_pool(xr_vector<Fvector>& out_v, xr_vector<CDB::TRI>& out_t); // portal_mod_destruct_cform, its very last step
bool is_ground(u16 mtl); // earth, grass, dirt, sand, gravel: a crater digs into it
bool is_soil(u16 mtl); // (the same now: asphalt keeps E56's concrete dents, no crater)
float depth_at(const Fvector& pos); // how deep the craters dug under pos are, all of them (0: none)
// a blast at pos: the crater it digs into the ground under it (its centre on the ground, the ground's normal, radius,
// depth at the middle); false: none (no ground within reach, a slope too steep, too weak)
bool blast(const Fvector& pos, float radius, float hit, Fvector& c, Fvector& n, float& r, float& depth);
void pit(const Fvector& c, float r, float depth); // a pit rounds dug: its collision too (queued, one a 0.3 s)
// E62: a breach through a wall (portal_mod_destruct's blast at a wall): the wall's faces cut out in the square of half side
// half_side round c (the middle of the wall's thickness), n the wall's normal; kept and saved with the craters
bool breach(const Fvector& c, const Fvector& n, float half_side, float thick);
void update(); // portal_mod_destruct::update (main thread)
void reset(); // portal_mod_destruct::shutdown
void save_game(LPCSTR sav_path);
void load_game(LPCSTR sav_path);

// Lua (level.craters_*)
LPCSTR lua_info();
float lua_depth_at(const Fvector& pos); // how far the ground's collision under pos is below where it was (0: not dug)
bool lua_make(const Fvector& pos, float r); // dig one there now (the lab)
float lua_ground(const Fvector& pos); // the ground under pos (4 m down): its normal's y if a crater digs into it, else -1
} // namespace portal_mod_craters
