// portal-mod E59: poles, fences, lamps and wires of the level break (see portal_mod_statics.h; tools/statics_build.py
// writes the data).
//
// levels\<level>\level.pms: the kinds (class: pole, fence piece, lamp, wire; material), the models, and per object its
// kind, the measure its hit points come from (a pole's cross-section over its foot, a fence piece's face, m2), mass, its
// frame (pivot: a pole's axis at its cut, the others' bottom centre; yaw), its models (a pole's top and stump, the
// others' one), its box, height and radius, its boxes for the physics (in its frame), its own triangles of level.cform,
// its render triangles (normal or fast geometry, index buffer, the offset of the triangle's first index), what is
// linked to it (a pole's lamps and wires).
//
// At load the renderer hands each index buffer over (portal_mod_destruct_ib): the three indices of every listed
// triangle are kept, so a broken object's triangles are written degenerate (a, a, a) and a standing one's put back
// (a save loaded on the level: OGSR keeps the level). Collision triangles leave by pointing at the sink vertices far
// under the level (g_pm_cform_sink, as a fallen tree's). A level collision triangle that lay on several objects (a
// fence's long quad over several of its pieces) or ran on past one into the level was cut by the tool into pieces of
// each one's (v2): at load the triangle goes (to the sink) and its pieces come in after the level's (append_cform), so
// a broken object leaves no wall of air where it stood and takes none of its neighbours'.
//
// A round on one of its collision triangles does the ammo's pm_destruct x its speed x the kind's round_k; a blast
// blast_k x its hit x (1 - d / radius)^blast_pow if a ray from it reaches the object's box (through what rounds pass).
// Past its hit points it breaks the next frame: a pole's stump stays (its model in place, no collision), its top becomes
// one rigid body of its boxes turning over the cut away from the shots (or the blast), the lamps linked to it ride on
// it, the wires linked to it snap (hidden, sparks); a fence piece and a lamp become bodies thrown by the hit. A moving
// body hits the creatures it reaches (its weight and speed, as a falling tree), knocks on landing, and lies; bodies
// stay (with the saved game, asleep where they lay).
#include "stdafx.h"
#include <algorithm>
#include <mutex> // E61: the rounds through glass come from the bullets' thread
#include "portal_mod_statics.h"
#include "portal_mod_destruct.h"
#include "portal_mod_features.h"
#include "portal_mod_trees.h" // g_pm_cform_sink
#include "PhysicsShell.h"
#include "PhysicsCommon.h"
#include "PHWorld.h"
#include "PHStaticGeomShell.h"
#include "PHCollideValidator.h"
#include "Level.h"
#include "GamePersistent.h"
#include "Actor.h"
#include "entity_alive.h"
#include "EntityCondition.h"
#include "Hit.h"
#include "../xr_3da/GameMtlLib.h"
#include "../Include/xrRender/RenderVisual.h"

bool portal_mod_ps_exists(LPCSTR name); // the renderer (E54b)
int portal_mod_sun_cascade(u32 context_id); // which sun cascade a render context draws (-1: none)
bool portal_mod_ctx_smap(u32 context_id); // a render context drawing a shadow map

namespace portal_mod_statics
{
namespace
{
enum : u8
{
    C_POLE,
    C_PANEL,
    C_LAMP,
    C_WIRE,
    C_GLASS // E61: a pane of glass: it shatters (its triangles go, no body: a burst of glass where it stood)
};
enum : u8
{
    M_WOOD,
    M_CONCRETE,
    M_METAL,
    M_GLASS,
    NMAT
};
LPCSTR const MAT_KEY[NMAT] = {"wood", "concrete", "metal", "glass"};
enum : u8
{
    ST_STANDING,
    ST_MOVING, // a body of its own (or riding on its pole's)
    ST_LYING,
    ST_GONE // a wire snapped, an object with no model
};
constexpr u32 SAVE_VERSION = 1;

struct Kind
{
    shared_str name;
    u8 cls{}, mat{};
    float hp_k{}, hp_add{}, hp_min{1.f}, hp_max{1e6f}, round_k{1.f}, blast_k{110.f};
    float push{}, w0{}; // m/s and rad/s a break gives it (a pole's top: w0 over its cut)
    float thin{}; // m: a plank fence's thickness (under 3 cm): rounds open holes in it (E54b's, no far face to measure), no dents
};

struct OBox
{
    Fvector c, h;
    Fmatrix33 R;
};

// a pole's stump's collision: a static box (no body, no owner)
class StumpShell : public CPHStaticGeomShell
{
    void PhDataUpdate(dReal step) override
    {
        Island().Step(step);
        Island().Unmerge();
        CPHUpdateObject::Deactivate();
    }
};

struct Vis : public ISpatial, public IRenderable
{
    bool in_space{};
    float r{};
    explicit Vis(LPCSTR vis) : ISpatial(g_SpatialSpace)
    {
        renderable.visual = ::Render->model_Create(vis);
        renderable.xform.identity();
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY; // drawn over a replay too (the level's own is gone)
        r = renderable.visual ? renderable.visual->getVisData().sphere.R : 0.f;
    }
    ~Vis() override
    {
        hide();
        if (renderable.visual)
            ::Render->model_Delete(renderable.visual);
    }
    void hide()
    {
        if (in_space)
        {
            spatial_unregister();
            in_space = false;
        }
    }
    void place(const Fmatrix& m)
    {
        if (!renderable.visual || !_valid(m))
            return;
        renderable.xform.set(m);
        const vis_data& v = renderable.visual->getVisData();
        Fvector P;
        m.transform_tiny(P, v.sphere.P);
        spatial.sphere.set(P, _max(v.sphere.R, 0.1f));
        if (!in_space)
        {
            spatial_register();
            in_space = true;
        }
        else
            spatial_move();
    }
    IRenderable* dcast_Renderable() override { return this; }
    void renderable_Render(u32 context_id, IRenderable* root) override
    {
        if (!renderable.visual)
            return;
        const int cas = portal_mod_sun_cascade(context_id);
        if (cas >= 1 && r < 0.4f * cas) // small things cast no shadow in the far sun cascades
            return;
        ::Render->add_Visual(context_id, this, renderable.visual, renderable.xform);
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

struct Obj
{
    u8 kind{};
    float measure{}, mass{};
    Fvector pivot{};
    float yaw{};
    int top{-1}, stump{-1};
    Fbox box{};
    float h{}, r{};
    xr_vector<OBox> boxes;
    u32 c0{}, nc{}, cut0{}, ncut{}, l0{}, nl{};
    // the game
    Fmatrix rest{};
    float hp{}, dmg{};
    Fvector push{};
    float w0{};
    u8 state{ST_STANDING};
    bool queued{}, creaked{}, cut{}, said_lies{}, tipped{};
    Fvector fall_dir{}; // horizontal, the way it was pushed
    LPCSTR why{"?"};
    int host{-1}; // a lamp riding on this pole's body
    Fmatrix rel{}; // ... its frame in the pole's
    Vis* vis{}; // the body's model (or the rider's)
    Vis* stump_vis{};
    StumpShell* stump_shell{};
    CPhysicsShell* shell{};
    Fvector v_prev{};
    float t{}; // s since it broke
    bool landed{};
    xr_vector<u16> hit_ids; // creatures it struck already
    xr_vector<std::pair<u32, CDB::TRI>> saved_tris; // the collision triangles it took out
};

struct Cfg
{
    bool enabled{true}, debug{};
    float blast_pow{1.5f}, blast_cover{0.f};
    float crush_v{2.5f}, crush_k{5e-4f}, crush_lethal{6000.f}, crush_reach{0.3f}, crush_mass{2500.f}, push_k{0.6f};
    float land_dv{3.f}, max_dist{250.f}, margin{0.02f};
    float tip_w{0.3f}, tip_angle{35.f}, tip_time{5.f}; // a broken pole's top turns over its foot at least this fast until it leans this far
    int max_fx{4};
    xr_vector<shared_str> snd_break[NMAT], snd_land[NMAT], fx_break[NMAT], fx_land[NMAT], snd_creak, fx_spark, snd_spark;
    shared_str gmtl[NMAT];
};

struct Saved
{
    u32 id;
    u8 state;
    Fmatrix xf;
};

struct AddTri // v2: a piece of a level collision triangle cut between objects
{
    u32 src; // the triangle it was cut from (its material, sector, flags)
    Fvector v[3];
};

struct State
{
    bool cfg{}, loaded{}, applied{}, sounds{}, need_restore{};
    u32 gen{};
    shared_str level;
    size_t nv{}, nt{};
    xr_vector<Kind> kinds;
    xr_vector<shared_str> models;
    xr_vector<Obj> objs;
    xr_vector<u32> ctris; // collision triangles (all objects'; v2: nt + k is the k-th of add until append_cform)
    xr_vector<u32> repl; // v2: the level's triangles cut between objects (out at load)
    xr_vector<AddTri> add; // ... their pieces
    xr_vector<u32> cut_key, cut_off; // render triangles: fast << 31 | ib, first index offset
    xr_vector<u16> cut_orig; // 3 per render triangle, as the level had them (0xFFFF: not read)
    xr_vector<u16> links;
    xr_vector<std::pair<u32, u32>> tri_obj; // sorted by triangle
    xr_map<u32, xr_vector<u32>> ib_cuts; // fast << 31 | ib -> its render triangles
    xr_vector<u32> active; // objects with a body (moving or lying)
    xr_vector<u32> queue; // broken, to go next frame
    xr_vector<ref_sound> snd_break[NMAT], snd_land[NMAT], snd_creak, snd_spark;
    u32 broken{}, shots{}, crushed{}, fx_frame{}, fx_count{};
};

struct Global
{
    u32 gen{1};
    xr_map<shared_str, std::pair<u32, xr_vector<Saved>>> levels; // the level's object count, its broken ones
};

Cfg C;
State S;
Global G;

// E61: rounds through what they pass whole (glass: shoot factor 1, no hit event of the game's), queued from the bullets'
// thread, taken at the next update
struct Pass
{
    Fvector p, d;
    u32 tri;
    float dmg;
};
std::mutex g_pass_lock;
xr_vector<Pass> g_pass;

float cfg_f(LPCSTR s, LPCSTR k, float def) { return pSettings->line_exist(s, k) ? pSettings->r_float(s, k) : def; }
bool cfg_b(LPCSTR s, LPCSTR k, bool def) { return pSettings->line_exist(s, k) ? !!pSettings->r_bool(s, k) : def; }

void cfg_list(LPCSTR s, LPCSTR k, xr_vector<shared_str>& out, LPCSTR def, bool fx)
{
    out.clear();
    LPCSTR v = pSettings->section_exist(s) && pSettings->line_exist(s, k) ? pSettings->r_string(s, k) : def;
    if (!v || !*v)
        return;
    string256 one;
    for (int i = 0, n = _GetItemCount(v); i < n; ++i)
    {
        _GetItem(v, i, one);
        LPCSTR name = _Trim(one);
        if (fx && !portal_mod_ps_exists(name))
        {
            Msg("! [statics] [%s] %s: no particle system %s", s, k, name);
            continue;
        }
        out.emplace_back(name);
    }
}

void load_cfg()
{
    if (S.cfg)
        return;
    S.cfg = true;
    LPCSTR s = "pm_statics";
    const bool has = pSettings->section_exist(s);
    if (has)
    {
        C.enabled = cfg_b(s, "enabled", C.enabled);
        C.debug = cfg_b(s, "debug", C.debug);
        C.blast_pow = cfg_f(s, "blast_pow", C.blast_pow);
        C.blast_cover = cfg_f(s, "blast_cover", C.blast_cover);
        C.crush_v = cfg_f(s, "crush_speed", C.crush_v);
        C.crush_k = cfg_f(s, "crush_k", C.crush_k);
        C.crush_lethal = cfg_f(s, "crush_lethal", C.crush_lethal);
        C.crush_reach = cfg_f(s, "crush_reach", C.crush_reach);
        C.crush_mass = cfg_f(s, "crush_mass", C.crush_mass);
        C.push_k = cfg_f(s, "push_k", C.push_k);
        C.land_dv = cfg_f(s, "land_dv", C.land_dv);
        C.max_dist = cfg_f(s, "max_dist", C.max_dist);
        C.margin = cfg_f(s, "margin", C.margin);
        C.tip_w = cfg_f(s, "tip_w", C.tip_w);
        C.tip_angle = cfg_f(s, "tip_angle", C.tip_angle);
        C.tip_time = cfg_f(s, "tip_time", C.tip_time);
        C.max_fx = (int)cfg_f(s, "max_fx", (float)C.max_fx);
    }
    LPCSTR const brk[NMAT] = {"material\\wood\\wood_hit1_hl2, material\\wood\\wood_hit2_hl2",
                              "ambient\\rnd_outdoor\\rnd_rock1, ambient\\rnd_outdoor\\rnd_rock3",
                              "material\\metal_box\\collide\\big_metal_1",
                              "material\\bottle\\collide\\bottle_break_1, material\\glass\\glass_fall01hl"};
    LPCSTR const lnd[NMAT] = {"material\\wood\\wood_crash1_hl2, material\\large_furniture\\large_furniture_1",
                              "ambient\\rnd_outdoor\\rnd_rock3, ambient\\rnd_outdoor\\rnd_rock4",
                              "material\\large_metal_trash\\collide\\large_metal_trash_1",
                              "material\\glass\\glass_fall02hl"};
    LPCSTR const fxb[NMAT] = {"destroy_fx\\destroy_wood_00", "hit_fx\\hit_stucco_home_explode, vehiclefx\\impact_concrete_03",
                              "hit_fx\\hit_metal_02, vehiclefx\\impact_metal_01", "explosions\\explosion_bottle, hit_fx\\hit_glass_01"};
    LPCSTR const fxl[NMAT] = {"destroy_fx\\destroy_wood_00", "vehiclefx\\impact_concrete_03", "vehiclefx\\impact_metal_01",
                              "hit_fx\\hit_glass_01"};
    LPCSTR const gm[NMAT] = {"objects\\large_furniture", "objects\\concrete_box", "objects\\large_metal_trash", "objects\\glass"};
    for (int m = 0; m < NMAT; ++m)
    {
        string64 k;
        xr_sprintf(k, "snd_break_%s", MAT_KEY[m]);
        cfg_list(s, k, C.snd_break[m], brk[m], false);
        xr_sprintf(k, "snd_land_%s", MAT_KEY[m]);
        cfg_list(s, k, C.snd_land[m], lnd[m], false);
        xr_sprintf(k, "fx_break_%s", MAT_KEY[m]);
        cfg_list(s, k, C.fx_break[m], fxb[m], true);
        xr_sprintf(k, "fx_land_%s", MAT_KEY[m]);
        cfg_list(s, k, C.fx_land[m], fxl[m], true);
        xr_sprintf(k, "gmtl_%s", MAT_KEY[m]);
        C.gmtl[m] = has && pSettings->line_exist(s, k) ? pSettings->r_string(s, k) : gm[m];
    }
    cfg_list(s, "snd_creak", C.snd_creak, "ambient\\x18\\x18_creak_1, ambient\\x18\\x18_creak_2", false);
    cfg_list(s, "fx_spark", C.fx_spark, "anomaly2\\electra_entrance_small", true);
    cfg_list(s, "snd_spark", C.snd_spark, "anomaly\\electra_hit1", false);
}

bool on() { return C.enabled && portal_mod_feat::enabled(portal_mod_feat::destruct); }

// a kind's numbers: [pm_statics] <key>_<kind name>, else the default by kind
void kind_cfg(Kind& k)
{
    struct D
    {
        LPCSTR name;
        float hp_k, hp_add, hp_min, hp_max, round_k, blast_k, push, w0, thin;
    };
    static const D defs[] = {
        {"wood_pole", 1700.f, 0.f, 40.f, 900.f, 1.f, 110.f, 0.4f, 0.12f, 0.f},
        {"concrete_pole", 600.f, 80.f, 80.f, 1500.f, 0.1f, 110.f, 0.3f, 0.1f, 0.f},
        {"metal_pole", 1500.f, 30.f, 30.f, 900.f, 0.6f, 110.f, 0.4f, 0.12f, 0.f},
        {"wood_fence", 10.f, 4.f, 4.f, 120.f, 1.f, 110.f, 3.f, 1.5f, 0.02f},
        {"concrete_fence", 18.f, 60.f, 60.f, 900.f, 0.05f, 110.f, 1.f, 0.4f, 0.f},
        {"mesh_fence", 6.f, 30.f, 30.f, 300.f, 0.3f, 110.f, 2.f, 0.8f, 0.f},
        {"lamp", 0.f, 2.f, 1.f, 50.f, 1.f, 110.f, 2.f, 3.f, 0.f},
        {"wire", 0.f, 1e6f, 1e6f, 1e6f, 0.f, 0.f, 0.f, 0.f, 0.f},
        {"glass", 0.f, 1.f, 1.f, 40.f, 1.f, 400.f, 0.f, 0.f, 0.f}, // E61: any round, any blast that reaches it
    };
    for (const D& d : defs)
        if (k.name == d.name)
        {
            k.hp_k = d.hp_k, k.hp_add = d.hp_add, k.hp_min = d.hp_min, k.hp_max = d.hp_max, k.round_k = d.round_k, k.blast_k = d.blast_k;
            k.push = d.push, k.w0 = d.w0, k.thin = d.thin;
        }
    LPCSTR s = "pm_statics";
    if (!pSettings->section_exist(s))
        return;
    string64 key;
    auto get = [&](LPCSTR what, float& v) {
        xr_sprintf(key, "%s_%s", what, k.name.c_str());
        v = cfg_f(s, key, v);
    };
    get("hp_k", k.hp_k);
    get("hp_add", k.hp_add);
    get("hp_min", k.hp_min);
    get("hp_max", k.hp_max);
    get("round_k", k.round_k);
    get("blast_k", k.blast_k);
    get("push", k.push);
    get("w0", k.w0);
    get("thin", k.thin);
    k.thin = clampr(k.thin, 0.f, 0.029f); // (E54b tells a plank fence's hole from a wall's by under 3 cm)
}

void make_sounds()
{
    if (S.sounds)
        return;
    S.sounds = true;
    auto mk = [](const xr_vector<shared_str>& names, xr_vector<ref_sound>& out) {
        out.resize(names.size());
        for (u32 i = 0; i < names.size(); ++i)
            out[i].create(names[i].c_str(), st_Effect, sg_SourceType);
    };
    for (int m = 0; m < NMAT; ++m)
    {
        mk(C.snd_break[m], S.snd_break[m]);
        mk(C.snd_land[m], S.snd_land[m]);
    }
    mk(C.snd_creak, S.snd_creak);
    mk(C.snd_spark, S.snd_spark);
}

void play_snd(xr_vector<ref_sound>& snd, const Fvector& pos, float vol)
{
    if (snd.empty())
        return;
    ref_sound& s = snd[::Random.randI(snd.size())];
    if (!s._handle())
        return;
    Fvector p = pos;
    float v = vol;
    s.play_no_feedback(nullptr, 0, 0.f, &p, &v);
}

bool fx_allowed()
{
    if (S.fx_frame != Device.dwFrame)
    {
        S.fx_frame = Device.dwFrame;
        S.fx_count = 0;
    }
    return (int)S.fx_count++ < C.max_fx;
}

void play_fx(const xr_vector<shared_str>& names, const Fvector& pos, const Fvector& dir)
{
    if (names.empty() || !fx_allowed())
        return;
    Fmatrix m;
    m.k.set(dir);
    if (m.k.square_magnitude() < 1e-4f)
        m.k.set(0.f, 1.f, 0.f);
    m.k.normalize();
    Fvector::generate_orthonormal_basis(m.k, m.j, m.i);
    m._14 = m._24 = m._34 = 0.f;
    m.c.set(pos);
    m._44 = 1.f;
    portal_mod_destruct::fx_at(names[::Random.randI(names.size())].c_str(), m);
}

const Kind& kind_of(const Obj& o) { return S.kinds[o.kind]; }

void rebuild_tri_obj()
{
    S.tri_obj.clear();
    for (u32 i = 0; i < S.objs.size(); ++i)
        for (u32 r = S.objs[i].c0; r < S.objs[i].c0 + S.objs[i].nc; ++r)
            S.tri_obj.emplace_back(S.ctris[r], i);
    std::sort(S.tri_obj.begin(), S.tri_obj.end());
}

int obj_of_tri(u32 tri)
{
    auto it = std::lower_bound(S.tri_obj.begin(), S.tri_obj.end(), std::make_pair(tri, u32(0)));
    return it != S.tri_obj.end() && it->first == tri ? (int)it->second : -1;
}

Fvector obj_centre(const Obj& o)
{
    Fvector c;
    o.box.getcenter(c);
    return c;
}

// ------------------------------------------------------------------------------------------------------------ the level's own
// its render triangles out (degenerate) or back, as runs of consecutive triangles in one buffer
void write_cuts(const Obj& o, bool out)
{
    static xr_vector<u16> run;
    u32 k = o.cut0;
    const u32 end = o.cut0 + o.ncut;
    while (k < end)
    {
        const u32 key = S.cut_key[k];
        const u32 off0 = S.cut_off[k];
        run.clear();
        u32 n = 0;
        while (k < end && S.cut_key[k] == key && S.cut_off[k] == off0 + 3 * n)
        {
            const u16* q = &S.cut_orig[k * 3];
            if (q[0] == 0xFFFF && q[1] == 0xFFFF && q[2] == 0xFFFF)
                break; // never read: leave it (and the rest of the run)
            if (out)
                run.push_back(q[0]), run.push_back(q[0]), run.push_back(q[0]);
            else
                run.push_back(q[0]), run.push_back(q[1]), run.push_back(q[2]);
            ++n, ++k;
        }
        if (n)
            portal_mod_render_queue_ib_part((key >> 31) != 0, key & 0x7FFFFFFF, off0, run.data(), (u32)run.size());
        else
            ++k;
    }
}

// its collision triangles leave (a pole keeps those wholly under its cut: its stump's, its footing's)
void drop_tris(Obj& o)
{
    if (g_pm_cform_sink == u32(-1) || !o.saved_tris.empty() || !o.nc)
        return;
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    const Fvector* V = Level().ObjectSpace.GetStaticVerts();
    const bool pole = kind_of(o).cls == C_POLE;
    const float keep_y = o.pivot.y + 0.03f;
    o.saved_tris.reserve(o.nc);
    for (u32 k = o.c0; k < o.c0 + o.nc; ++k)
    {
        const u32 i = S.ctris[k];
        CDB::TRI& t = T[i];
        if (pole && V[t.verts[0]].y <= keep_y && V[t.verts[1]].y <= keep_y && V[t.verts[2]].y <= keep_y)
            continue;
        o.saved_tris.emplace_back(i, t);
        t.verts[0] = g_pm_cform_sink;
        t.verts[1] = g_pm_cform_sink + 1;
        t.verts[2] = g_pm_cform_sink + 2;
    }
}

void restore_tris(Obj& o)
{
    if (!o.saved_tris.empty())
    {
        CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
        for (const auto& [i, t] : o.saved_tris)
            T[i] = t;
    }
    o.saved_tris.clear();
}

void kill_stump(Obj& o)
{
    if (!o.stump_shell)
        return;
    o.stump_shell->Deactivate();
    xr_delete(o.stump_shell);
}

// a broken pole's stump holds what falls on it: a box over its section, from its foot up to the cut
void make_stump(Obj& o)
{
    const float hs = 0.5f * (o.pivot.y - 0.06f - o.box.min.y);
    if (o.stump_shell || o.r < 0.02f || hs < 0.05f)
        return;
    Fobb b;
    b.m_rotate.i.set(o.rest.i);
    b.m_rotate.j.set(o.rest.j);
    b.m_rotate.k.set(o.rest.k);
    b.m_translate.set(o.pivot.x, o.pivot.y - 0.06f - hs, o.pivot.z);
    b.m_halfsize.set(o.r * 0.6f, hs, o.r * 0.6f); // lower and narrower than the cut: a shattered foot gives no flat seat
    o.stump_shell = xr_new<StumpShell>();
    o.stump_shell->add_Box(b);
    o.stump_shell->Activate(Fidentity);
    CPHCollideValidator::SetNonDynamicObject(*o.stump_shell);
}

void cut_out(Obj& o)
{
    if (o.cut)
        return;
    o.cut = true;
    write_cuts(o, true);
    drop_tris(o);
    // the holes and dents on what goes (a pole's stump keeps its own)
    Fbox b = o.box;
    b.grow(0.06f);
    if (kind_of(o).cls == C_POLE)
        b.min.y = _max(b.min.y, o.pivot.y - 0.03f);
    portal_mod_destruct::forget_marks(b);
}

// ------------------------------------------------------------------------------------------------------------ bodies
void kill_body(Obj& o)
{
    if (o.shell)
        destroy_physics_shell(o.shell);
    o.shell = nullptr;
    xr_delete(o.vis);
}

bool make_body(Obj& o, const Fmatrix& F, const Fvector& v, const Fvector& w, bool asleep)
{
    if (o.top < 0 || o.top >= (int)S.models.size() || o.boxes.empty())
        return false;
    o.vis = xr_new<Vis>(S.models[o.top].c_str());
    if (!o.vis->renderable.visual)
    {
        xr_delete(o.vis);
        return false;
    }
    CPhysicsElement* E = P_create_Element();
    for (const OBox& b : o.boxes)
    {
        Fobb ob;
        ob.m_rotate.set(b.R);
        ob.m_translate.set(b.c);
        ob.m_halfsize.set(_max(0.02f, b.h.x - C.margin), _max(0.02f, b.h.y - C.margin), _max(0.02f, b.h.z - C.margin));
        E->add_Box(ob);
    }
    o.shell = P_create_Shell();
    o.shell->add_Element(E);
    o.shell->setMass(_max(o.mass, 1.f));
    o.shell->SetMaterial(C.gmtl[kind_of(o).mat].c_str());
    o.shell->Activate(F, v, w, false);
    if (asleep)
        o.shell->Disable();
    o.v_prev = v;
    o.vis->place(F);
    return true;
}

void place_riders(const Obj& o, u32 self, const Fmatrix& M)
{
    for (u32 k = o.l0; k < o.l0 + o.nl; ++k)
    {
        Obj& r = S.objs[S.links[k]];
        if (r.host != (int)self || !r.vis)
            continue;
        Fmatrix X;
        X.mul_43(M, r.rel);
        r.vis->place(X);
    }
}

// ------------------------------------------------------------------------------------------------------------ breaking
void break_obj(u32 id, bool from_save, const Fmatrix* saved_xf, u8 saved_state);

void ride_or_snap(u32 pole_id, bool from_save)
{
    Obj& p = S.objs[pole_id];
    for (u32 k = p.l0; k < p.l0 + p.nl; ++k)
    {
        const u32 li = S.links[k];
        Obj& r = S.objs[li];
        if (r.state != ST_STANDING)
            continue;
        const Kind& kr = kind_of(r);
        cut_out(r);
        if (kr.cls == C_WIRE || r.top < 0)
        {
            r.state = ST_GONE;
            if (!from_save && kr.cls == C_WIRE)
            {
                Fvector at;
                r.box.getcenter(at);
                at.x = clampr(at.x, p.box.min.x, p.box.max.x), at.z = clampr(at.z, p.box.min.z, p.box.max.z);
                at.y = _min(r.box.max.y, p.box.max.y);
                play_fx(C.fx_spark, at, Fvector{0.f, 1.f, 0.f});
                make_sounds();
                play_snd(S.snd_spark, at, 1.f);
            }
            continue;
        }
        // a lamp rides on the pole's top
        r.state = ST_MOVING;
        r.host = (int)pole_id;
        Fmatrix inv;
        inv.invert(p.rest);
        r.rel.mul_43(inv, r.rest);
        r.vis = xr_new<Vis>(S.models[r.top].c_str());
        r.vis->place(r.rest);
    }
}

void break_obj(u32 id, bool from_save, const Fmatrix* saved_xf, u8 saved_state)
{
    Obj& o = S.objs[id];
    if (o.state != ST_STANDING)
        return;
    const Kind& k = kind_of(o);
    cut_out(o);
    o.t = 0.f;
    o.landed = from_save;
    o.hit_ids.clear();
    ++S.broken;
    if (k.cls == C_WIRE)
    {
        o.state = ST_GONE;
        return;
    }
    if (k.cls == C_GLASS) // E61: the pane is gone; shards burst over it and fall
    {
        o.state = ST_GONE;
        if (from_save)
            return;
        make_sounds();
        Fvector sz;
        o.box.getsize(sz);
        const float area = _max(_max(sz.x * sz.y, sz.z * sz.y), sz.x * sz.z);
        Fvector d = o.push;
        if (d.square_magnitude() < 1e-6f)
            d.set(0.f, -1.f, 0.f);
        d.normalize_safe();
        const int n = iFloor(clampr(area / 0.6f, 1.f, 4.f));
        for (int i = 0; i < n; ++i)
        {
            Fvector q{::Random.randF(o.box.min.x, o.box.max.x), ::Random.randF(o.box.min.y, o.box.max.y), ::Random.randF(o.box.min.z, o.box.max.z)};
            play_fx(C.fx_break[M_GLASS], q, d);
        }
        const Fvector c = obj_centre(o);
        play_snd(S.snd_break[M_GLASS], c, clampr(0.6f + area * 0.3f, 0.6f, 1.6f));
        Msg("* [statics] glass %u (%.2f m2) shatters by %s, %.1f m from the camera", id, area, o.why, c.distance_to(Device.vCameraPosition));
        return;
    }
    if (k.cls == C_POLE && o.stump >= 0 && o.stump < (int)S.models.size())
    {
        o.stump_vis = xr_new<Vis>(S.models[o.stump].c_str());
        o.stump_vis->place(o.rest);
    }
    if (k.cls == C_POLE)
    {
        make_stump(o);
    }
    // its push: the shots' or the blast's horizontal direction
    Fvector d{o.push.x, 0.f, o.push.z};
    if (d.square_magnitude() < 1e-6f)
        d.set(::Random.randF(-1.f, 1.f), 0.f, ::Random.randF(-1.f, 1.f));
    d.normalize_safe();
    o.fall_dir = d;
    o.tipped = false;
    Fvector axis;
    axis.crossproduct(Fvector{0.f, 1.f, 0.f}, d).normalize_safe(); // up turned round it goes towards d
    Fvector v{}, w{};
    const float w0 = _max(o.w0, k.w0);
    if (k.cls == C_POLE)
    {
        w.mul(axis, w0);
        Fvector arm{0.f, o.h * 0.45f, 0.f};
        v.crossproduct(w, arm).mad(d, k.push);
    }
    else if (k.cls == C_PANEL)
    {
        w.mul(axis, w0 * ::Random.randF(0.6f, 1.2f));
        v.mul(d, k.push * ::Random.randF(0.7f, 1.2f)).y += 0.5f;
    }
    else
    {
        w.random_dir().mul(w0);
        v.mul(d, k.push * 0.5f);
    }
    const Fmatrix F = saved_xf ? *saved_xf : o.rest;
    const bool asleep = from_save && saved_state == ST_LYING;
    if (!make_body(o, F, from_save ? Fvector{} : v, from_save ? Fvector{} : w, asleep))
    {
        o.state = ST_GONE;
        return;
    }
    o.state = asleep ? ST_LYING : ST_MOVING;
    o.v_prev = from_save ? Fvector{} : v;
    S.active.push_back(id);
    if (k.cls == C_POLE)
    {
        ride_or_snap(id, from_save);
        place_riders(o, id, F);
    }
    if (from_save)
        return;
    make_sounds();
    Fvector c = obj_centre(o);
    if (k.cls == C_POLE)
        c.set(o.pivot);
    play_snd(S.snd_break[k.mat], c, clampr(o.mass / 300.f, 0.5f, 1.5f));
    play_fx(C.fx_break[k.mat], c, Fvector{0.f, 1.f, 0.f});
    if (k.mat == M_WOOD || k.mat == M_CONCRETE)
    {
        const Fvector up{0.f, 1.f, 0.f};
        portal_mod_destruct::chips(c, up, Fvector().invert(up), k.cls == C_POLE ? 60.f : 25.f, k.mat == M_WOOD ? 2 : 0);
    }
    Msg("* [statics] %s %u (%.0f kg, %.1f m) breaks by %s: %.0f of %.0f, towards %.2f %.2f, %.1f m from the camera", k.name.c_str(), id, o.mass, o.h,
        o.why, o.dmg, o.hp, d.x, d.z, o.pivot.distance_to(Device.vCameraPosition));
}

void damage(u32 id, float dmg, const Fvector& dir, float w0, LPCSTR why)
{
    Obj& o = S.objs[id];
    if (o.state != ST_STANDING || o.queued || dmg <= 0.f)
        return;
    o.dmg += dmg;
    o.push.mad(Fvector{dir.x, 0.f, dir.z}.normalize_safe(), dmg);
    o.w0 = _max(o.w0, w0);
    o.why = why;
    if (C.debug)
        Msg("[statics] %s %u: %.1f by %s, %.0f of %.0f", kind_of(o).name.c_str(), id, dmg, why, o.dmg, o.hp);
    if (o.dmg >= o.hp)
    {
        o.queued = true; // the next frame (not inside the hit's event, maybe inside the physics step)
        S.queue.push_back(id);
        return;
    }
    if (!o.creaked && o.dmg >= 0.7f * o.hp && kind_of(o).cls == C_POLE)
    {
        o.creaked = true;
        make_sounds();
        play_snd(S.snd_creak, o.pivot, 1.f);
    }
}

// a ray against the static collision that passes what rounds pass and the object's own triangles
bool blocked(const Fvector& a, const Fvector& b, int self)
{
    Fvector d;
    d.sub(b, a);
    float range = d.magnitude();
    if (range < 0.05f)
        return false;
    d.div(range);
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector p = a;
    for (int k = 0; k < 6 && range > 0.05f; ++k)
    {
        collide::rq_result R;
        if (!Level().ObjectSpace.RayPick(p, d, range, collide::rqtStatic, R, nullptr))
            return false;
        const CDB::TRI& tr = T[R.element];
        const SGameMtl* mtl = GMLib.GetMaterialByIdx(tr.material);
        if (!((mtl && mtl->fShootFactor >= 0.999f) || obj_of_tri((u32)R.element) == self))
            return true;
        p.mad(d, R.range + 0.02f);
        range -= R.range + 0.02f;
    }
    return false;
}

// whoever a moving body reaches this frame
void crush(Obj& o, u32 id)
{
    if (!o.shell || !o.shell->isEnabled())
        return;
    Fvector v, w;
    o.shell->get_LinearVel(v);
    o.shell->get_AngularVel(w);
    Fmatrix M;
    o.shell->InterpolateGlobalTransform(&M);
    Fvector cen;
    M.transform_tiny(cen, Fvector{0.f, o.h * 0.5f, 0.f});
    const float reach = o.h * 0.6f + 2.5f;
    if (v.magnitude() + w.magnitude() * reach < C.crush_v)
        return;
    static xr_vector<CObject*> objs;
    objs.clear();
    Level().ObjectSpace.GetNearest(objs, cen, reach, nullptr);
    CActor* actor = Actor();
    for (CObject* ob : objs)
    {
        if (ob->getDestroy() || std::find(o.hit_ids.begin(), o.hit_ids.end(), ob->ID()) != o.hit_ids.end())
            continue;
        CEntityAlive* e = smart_cast<CEntityAlive*>(ob);
        if (!e || !e->g_Alive())
            continue;
        const float girth = clampr(ob->Radius() * 0.45f, 0.25f, 0.45f);
        const float top = clampr(ob->Radius() * 2.f - 0.1f, 0.5f, 1.9f);
        float gap = flt_max;
        Fvector at{};
        for (int s = 0; s < 4; ++s)
        {
            Fvector P = ob->Position();
            P.y += 0.3f + (top - 0.3f) * s / 3.f;
            for (const OBox& b : o.boxes)
            {
                // the point in the box's frame
                Fvector c;
                M.transform_tiny(c, b.c);
                Fvector q;
                q.sub(P, c);
                Fvector ax[3] = {b.R.i, b.R.j, b.R.k};
                float d2 = 0.f;
                const float hs[3] = {b.h.x, b.h.y, b.h.z};
                for (int a = 0; a < 3; ++a)
                {
                    Fvector wa;
                    M.transform_dir(wa, ax[a]);
                    const float t = _abs(q.dotproduct(wa)) - hs[a];
                    if (t > 0.f)
                        d2 += t * t;
                }
                const float g = _sqrt(d2) - girth - C.crush_reach;
                if (g < gap)
                    gap = g, at = P;
            }
        }
        if (gap > 0.f)
            continue;
        Fvector r, sp;
        r.sub(at, M.c);
        sp.crossproduct(w, r).add(v);
        const float speed = sp.magnitude();
        if (speed < C.crush_v)
            continue;
        o.hit_ids.push_back(ob->ID());
        const float m = _min(o.mass, C.crush_mass);
        const float E = 0.5f * m * speed * speed;
        SHit HS;
        HS.GenHeader(GE_HIT, ob->ID());
        HS.whoID = actor ? actor->ID() : ob->ID();
        HS.weaponID = HS.whoID;
        HS.dir = Fvector(sp).normalize_safe();
        HS.power = C.crush_k * E;
        if (E >= C.crush_lethal)
            HS.power = _max(HS.power, 4.f / _max(e->conditions().GetHitImmunity(ALife::eHitTypeStrike), 0.02f));
        HS.p_in_bone_space = at;
        HS.impulse = _min(600.f, C.push_k * m * speed);
        HS.hit_type = ALife::eHitTypeStrike;
        HS.boneID = 0;
        NET_Packet P;
        HS.Write_Packet(P);
        CGameObject::u_EventSend(P);
        ++S.crushed;
        Msg("[statics] %s %u falls on %s (id %u) at %.1f m/s - %.0f J, power %.2f", kind_of(o).name.c_str(), id, ob->cName().c_str(), ob->ID(), speed, E, HS.power);
    }
}

void update_bodies(float dt)
{
    for (u32 n = 0; n < S.active.size(); ++n)
    {
        const u32 id = S.active[n];
        Obj& o = S.objs[id];
        if (!o.shell)
            continue;
        o.t += dt;
        // a pole's top on its shattered foot: kept turning over it towards where it was pushed until it leans past
        // tip_angle (a wide heavy mast would otherwise sit balanced on its stump)
        if (kind_of(o).cls == C_POLE && !o.tipped && !o.landed)
        {
            Fmatrix M0;
            o.shell->InterpolateGlobalTransform(&M0);
            if (o.t > C.tip_time || M0.j.y < _cos(deg2rad(C.tip_angle)))
                o.tipped = true;
            else
            {
                Fvector axis, w;
                axis.crossproduct(Fvector{0.f, 1.f, 0.f}, o.fall_dir).normalize_safe();
                o.shell->get_AngularVel(w);
                const float wa = w.dotproduct(axis);
                if (wa < C.tip_w)
                {
                    if (!o.shell->isEnabled())
                        o.shell->Enable();
                    w.mad(axis, C.tip_w - wa);
                    Fvector r, v;
                    M0.transform_dir(r, Fvector{0.f, o.h * 0.45f, 0.f});
                    v.crossproduct(w, r); // round its foot, not its middle
                    o.shell->set_AngularVel(w);
                    o.shell->set_LinearVel(v);
                    o.state = ST_MOVING;
                }
            }
        }
        const bool enabled = o.shell->isEnabled();
        if (!enabled && o.state == ST_LYING)
            continue;
        Fmatrix M;
        o.shell->InterpolateGlobalTransform(&M);
        if (!_valid(M) || M.c.y < -500.f)
        {
            kill_body(o);
            o.state = ST_GONE;
            continue;
        }
        if (o.vis)
            o.vis->place(M);
        if (kind_of(o).cls == C_POLE)
            place_riders(o, id, M);
        if (!enabled)
        {
            o.state = ST_LYING;
            for (u32 k = o.l0; k < o.l0 + o.nl; ++k)
            {
                Obj& r = S.objs[S.links[k]];
                if (r.host == (int)id)
                    r.state = ST_LYING;
            }
            if (C.debug && !o.said_lies)
                Msg("[statics] %s %u lies, %.1f s after it broke", kind_of(o).name.c_str(), id, o.t);
            o.said_lies = true;
            continue;
        }
        o.state = ST_MOVING;
        Fvector vel;
        o.shell->get_LinearVel(vel);
        const float dv = Fvector().sub(vel, o.v_prev).magnitude();
        o.v_prev = vel;
        const bool near_cam = M.c.distance_to(Device.vCameraPosition) < C.max_dist;
        if (near_cam)
            crush(o, id);
        if (dv > C.land_dv && o.t > 0.1f && near_cam)
        {
            const Kind& k = kind_of(o);
            make_sounds();
            Fvector at;
            M.transform_tiny(at, Fvector{0.f, o.h * 0.5f, 0.f});
            play_snd(S.snd_land[k.mat], at, clampr(dv / 8.f, 0.3f, 1.f) * clampr(o.mass / 200.f, 0.4f, 1.5f));
            if (!o.landed)
            {
                o.landed = true;
                play_fx(C.fx_land[k.mat], at, Fvector{0.f, 1.f, 0.f});
                // the lamps on a pole's top break as it lands
                for (u32 q = o.l0; q < o.l0 + o.nl; ++q)
                {
                    Obj& r = S.objs[S.links[q]];
                    if (r.host == (int)id && r.vis)
                    {
                        const Fvector rc = r.vis->renderable.xform.c;
                        play_snd(S.snd_break[M_GLASS], rc, 0.8f);
                        play_fx(C.fx_break[M_GLASS], rc, Fvector{0.f, 1.f, 0.f});
                    }
                }
                Msg("* [statics] %s %u lands %.1f s after it broke (dv %.1f m/s)", k.name.c_str(), id, o.t, dv);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------------------ saved state
void restore_all()
{
    for (u32 i = 0; i < S.objs.size(); ++i)
    {
        Obj& o = S.objs[i];
        kill_body(o);
        xr_delete(o.stump_vis);
        kill_stump(o);
        if (o.cut)
        {
            write_cuts(o, false);
            restore_tris(o);
            o.cut = false;
        }
        o.state = ST_STANDING;
        o.dmg = 0.f;
        o.push.set(0.f, 0.f, 0.f);
        o.w0 = 0.f;
        o.queued = o.creaked = o.landed = o.said_lies = o.tipped = false;
        o.host = -1;
    }
    S.active.clear();
    S.queue.clear();
}

void stash()
{
    if (!S.loaded || S.gen != G.gen || !S.level.size())
        return;
    auto& e = G.levels[S.level];
    e.first = (u32)S.objs.size();
    e.second.clear();
    for (u32 i = 0; i < S.objs.size(); ++i)
    {
        const Obj& o = S.objs[i];
        if (o.state == ST_STANDING)
            continue;
        Saved s{i, o.state, Fidentity};
        if (o.shell)
            o.shell->InterpolateGlobalTransform(&s.xf);
        else if (o.host >= 0)
            s.state = ST_GONE + 1; // riding on its pole (put back with it)
        e.second.push_back(s);
    }
    if (e.second.empty())
        G.levels.erase(S.level);
}

void apply_saved()
{
    auto it = G.levels.find(S.level);
    if (it == G.levels.end() || it->second.first != S.objs.size())
        return;
    u32 n = 0;
    for (const Saved& s : it->second.second)
    {
        if (s.id >= S.objs.size() || S.objs[s.id].state != ST_STANDING)
            continue;
        if (s.state == ST_GONE + 1)
            continue; // a rider: its pole puts it on
        Obj& o = S.objs[s.id];
        o.dmg = o.hp;
        if (s.state == ST_GONE)
        {
            cut_out(o);
            o.state = ST_GONE;
        }
        else
            break_obj(s.id, true, &s.xf, s.state);
        ++n;
    }
    if (n)
        Msg("* [statics] %s: %u broken object(s) put back from the saved state", S.level.c_str(), n);
}
} // namespace

// ------------------------------------------------------------------------------------------------------------ load
void load_level(size_t nv, size_t nt)
{
    reset();
    load_cfg();
    string_path fn;
    if (!FS.exist(fn, fsgame::level, "level.pms"))
        return;
    IReader* F = FS.r_open(fn);
    if (!F)
        return;
    char magic[4]{};
    F->r(magic, 4);
    const u32 ver = strncmp(magic, "PMS1", 4) == 0 ? F->r_u32() : 0;
    bool ok = ver == 1 || ver == 2;
    if (ok)
    {
        F->r_stringZ(S.level);
        const u32 fnv = F->r_u32(), fnt = F->r_u32();
        if (fnv != nv || fnt != nt)
        {
            Msg("! [statics] level.pms of %s was made for another level.cform (%u/%u verts, %u/%u tris): run tools/statics_build.py",
                S.level.c_str(), fnv, (u32)nv, fnt, (u32)nt);
            ok = false;
        }
    }
    if (ok)
    {
        S.nv = nv, S.nt = nt;
        S.kinds.resize(F->r_u32());
        for (Kind& k : S.kinds)
        {
            F->r_stringZ(k.name);
            k.cls = F->r_u8();
            k.mat = F->r_u8();
            if (k.mat >= NMAT)
                k.mat = M_WOOD;
            kind_cfg(k);
        }
        S.models.resize(F->r_u32());
        for (shared_str& m : S.models)
            F->r_stringZ(m);
        S.objs.resize(F->r_u32());
        for (Obj& o : S.objs)
        {
            o.kind = F->r_u8();
            F->r_u8();
            F->r_u16();
            o.measure = F->r_float();
            o.mass = F->r_float();
            F->r_fvector3(o.pivot);
            o.yaw = F->r_float();
            o.top = F->r_s32();
            o.stump = F->r_s32();
            F->r_fvector3(o.box.min);
            F->r_fvector3(o.box.max);
            o.h = F->r_float();
            o.r = F->r_float();
            o.boxes.resize(F->r_u8());
            for (OBox& b : o.boxes)
            {
                F->r_fvector3(b.c);
                F->r_fvector3(b.h);
                F->r_fvector3(b.R.i);
                F->r_fvector3(b.R.j);
                F->r_fvector3(b.R.k);
            }
            o.c0 = F->r_u32(), o.nc = F->r_u32();
            o.cut0 = F->r_u32(), o.ncut = F->r_u32();
            o.l0 = F->r_u32(), o.nl = F->r_u32();
            if (o.kind >= S.kinds.size())
                o.kind = 0;
            o.rest.rotateY(o.yaw);
            o.rest.c.set(o.pivot);
            const Kind& k = S.kinds[o.kind];
            o.hp = clampr(k.hp_k * o.measure + k.hp_add, k.hp_min, k.hp_max);
        }
        S.ctris.resize(F->r_u32());
        if (!S.ctris.empty())
            F->r(S.ctris.data(), S.ctris.size() * 4);
        const u32 ncut = F->r_u32();
        S.cut_key.resize(ncut);
        S.cut_off.resize(ncut);
        for (u32 i = 0; i < ncut; ++i)
        {
            S.cut_key[i] = F->r_u32();
            S.cut_off[i] = F->r_u32();
        }
        S.links.resize(F->r_u32());
        if (!S.links.empty())
            F->r(S.links.data(), S.links.size() * 2);
        if (ver >= 2)
        {
            S.repl.resize(F->r_u32());
            if (!S.repl.empty())
                F->r(S.repl.data(), S.repl.size() * 4);
            S.add.resize(F->r_u32());
            for (AddTri& a : S.add)
            {
                a.src = F->r_u32();
                for (Fvector& v : a.v)
                    F->r_fvector3(v);
            }
        }
        ok = F->elapsed() >= 0;
    }
    FS.r_close(F);
    if (!ok)
    {
        reset();
        return;
    }
    S.cut_orig.assign(S.cut_key.size() * 3, 0xFFFF);
    for (u32 i = 0; i < S.cut_key.size(); ++i)
        S.ib_cuts[S.cut_key[i]].push_back(i);
    for (u32 i = 0; i < S.objs.size(); ++i)
    {
        Obj& o = S.objs[i];
        if (o.c0 > S.ctris.size())
            o.c0 = (u32)S.ctris.size();
        o.nc = std::min(o.nc, (u32)S.ctris.size() - o.c0);
        for (u32 k = o.l0; k < o.l0 + o.nl && k < S.links.size(); ++k)
            if (S.links[k] >= S.objs.size())
                S.links[k] = u16(i); // (never: a link to itself is skipped as standing nothing)
    }
    rebuild_tri_obj();
    S.loaded = true;
    u32 cls[8]{};
    for (const Obj& o : S.objs)
        ++cls[S.kinds[o.kind].cls & 7];
    Msg("* [statics] %s: %u poles, %u fence pieces, %u lamps, %u wires, %u panes of glass; %u collision (%u cut into %u) and %u render triangles of theirs",
        S.level.c_str(), cls[0], cls[1], cls[2], cls[3], cls[C_GLASS], (u32)S.ctris.size(), (u32)S.repl.size(), (u32)S.add.size(), (u32)S.cut_key.size());
}

void remap_cform(const xr_vector<u8>& cut)
{
    if (!S.loaded || cut.size() != S.nt)
        return;
    xr_vector<u32> newi(cut.size());
    u32 k = 0;
    for (size_t i = 0; i < cut.size(); ++i)
        newi[i] = cut[i] ? u32(-1) : k++;
    u32 dropped = 0;
    for (Obj& o : S.objs)
    {
        // the object's triangles the destruction took are no longer its (they are the building's pieces' now)
        u32 w = o.c0;
        for (u32 r = o.c0; r < o.c0 + o.nc; ++r)
        {
            const u32 t = S.ctris[r];
            if (t >= S.nt)
                S.ctris[w++] = t; // a piece of a cut one (append_cform numbers it)
            else if (newi[t] != u32(-1))
                S.ctris[w++] = newi[t];
            else
                ++dropped;
        }
        for (u32 r = w; r < o.c0 + o.nc; ++r)
            S.ctris[r] = S.ctris[w > o.c0 ? w - 1 : o.c0]; // (padding; nc shrinks below)
        o.nc = w - o.c0;
    }
    // the cut ones (and their pieces' sources) by their new numbers; one the destruction took is the building's now
    u32 w = 0;
    for (u32 t : S.repl)
        if (t < newi.size() && newi[t] != u32(-1))
            S.repl[w++] = newi[t];
    S.repl.resize(w);
    for (AddTri& a : S.add)
        a.src = a.src < newi.size() ? newi[a.src] : u32(-1);
    rebuild_tri_obj();
    if (dropped)
        Msg("* [statics] %u of their collision triangles belong to the destruction's building", dropped);
}

void append_cform(xr_vector<Fvector>& out_v, xr_vector<CDB::TRI>& out_t)
{
    if (!S.loaded || g_pm_cform_sink == u32(-1) || (S.repl.empty() && S.add.empty()))
        return;
    xr_vector<u32> at(S.add.size(), u32(-1));
    out_v.reserve(out_v.size() + S.add.size() * 3);
    out_t.reserve(out_t.size() + S.add.size());
    for (u32 k = 0; k < S.add.size(); ++k)
    {
        const AddTri& a = S.add[k];
        if (a.src >= out_t.size())
            continue; // its triangle went with the destruction's building
        CDB::TRI t = out_t[a.src];
        const u32 vb = (u32)out_v.size();
        out_v.push_back(a.v[0]);
        out_v.push_back(a.v[1]);
        out_v.push_back(a.v[2]);
        t.verts[0] = vb, t.verts[1] = vb + 1, t.verts[2] = vb + 2;
        at[k] = (u32)out_t.size();
        out_t.push_back(t);
    }
    for (u32 i : S.repl)
        if (i < out_t.size())
        {
            out_t[i].verts[0] = g_pm_cform_sink;
            out_t[i].verts[1] = g_pm_cform_sink + 1;
            out_t[i].verts[2] = g_pm_cform_sink + 2;
        }
    for (Obj& o : S.objs)
    {
        u32 w = o.c0;
        for (u32 r = o.c0; r < o.c0 + o.nc; ++r)
        {
            const u32 t = S.ctris[r];
            if (t < S.nt)
                S.ctris[w++] = t;
            else if (t - S.nt < at.size() && at[t - S.nt] != u32(-1))
                S.ctris[w++] = at[t - S.nt];
        }
        o.nc = w - o.c0;
    }
    rebuild_tri_obj();
    const u32 n = (u32)S.add.size(), nr = (u32)S.repl.size();
    S.add.clear();
    S.add.shrink_to_fit();
    S.repl.clear();
    S.repl.shrink_to_fit();
    Msg("* [statics] %u level collision triangles cut between them, %u pieces in", nr, n);
}

void on_ib(bool fast, u32 ib, const u16* data, u32 count)
{
    if (!S.loaded)
        return;
    auto it = S.ib_cuts.find((u32(fast) << 31) | ib);
    if (it == S.ib_cuts.end())
        return;
    for (u32 i : it->second)
    {
        const u32 off = S.cut_off[i];
        if (off + 2 < count)
        {
            S.cut_orig[i * 3] = data[off];
            S.cut_orig[i * 3 + 1] = data[off + 1];
            S.cut_orig[i * 3 + 2] = data[off + 2];
        }
    }
}

// ------------------------------------------------------------------------------------------------------------ hits
void bullet_hit(const Fvector& point, const Fvector& dir, u32 tri, float dmg)
{
    load_cfg();
    if (!S.loaded || !on() || dmg <= 0.f)
        return;
    const int id = obj_of_tri(tri);
    if (id < 0)
        return;
    ++S.shots;
    const Kind& k = kind_of(S.objs[id]);
    damage((u32)id, dmg * k.round_k, dir, 0.f, "rounds");
}

bool owns(u32 tri) { return S.loaded && obj_of_tri(tri) >= 0; } // E62

void pass_hit(const Fvector& point, const Fvector& dir, u32 tri, float dmg) // E61: any thread
{
    if (dmg <= 0.f)
        return;
    std::lock_guard<std::mutex> lk(g_pass_lock);
    if (g_pass.size() < 256)
        g_pass.push_back(Pass{point, dir, tri, dmg});
}

float thin_at(u32 tri)
{
    if (!S.loaded || !on())
        return 0.f;
    const int id = obj_of_tri(tri);
    return id >= 0 && S.objs[id].state == ST_STANDING && !S.objs[id].queued ? kind_of(S.objs[id]).thin : 0.f;
}

void explosion(const Fvector& pos, float radius, float hit, float impulse)
{
    load_cfg();
    if (!S.loaded || !on() || radius <= 0.f)
        return;
    for (u32 i = 0; i < S.objs.size(); ++i)
    {
        Obj& o = S.objs[i];
        if (o.state != ST_STANDING || o.queued)
            continue;
        const Kind& k = kind_of(o);
        if (k.blast_k <= 0.f)
            continue;
        Fvector q{clampr(pos.x, o.box.min.x, o.box.max.x), clampr(pos.y, o.box.min.y, o.box.max.y), clampr(pos.z, o.box.min.z, o.box.max.z)};
        const float d = q.distance_to(pos);
        if (d >= radius)
            continue;
        // a pole is reached at its foot or at the blast's height; the others at their nearest point or their centre
        Fvector c = obj_centre(o);
        Fvector pts[2] = {q, c};
        if (k.cls == C_POLE)
            pts[1].set(o.pivot.x, o.pivot.y + 0.5f, o.pivot.z);
        int seen = 0;
        for (const Fvector& p : pts)
            seen += !blocked(pos, p, (int)i);
        const float vis = seen ? seen / 2.f : C.blast_cover;
        if (vis <= 0.f)
            continue;
        const float fall = std::pow(1.f - d / radius, C.blast_pow);
        Fvector away;
        away.sub(c, pos);
        damage(i, k.blast_k * hit * fall * vis, away, k.w0 * (1.f + 4.f * fall) * clampr(impulse / 200.f, 0.5f, 2.f), "a blast");
    }
}

void update()
{
    load_cfg();
    static xr_vector<Pass> pass; // E61: the rounds through glass since the last update
    {
        std::lock_guard<std::mutex> lk(g_pass_lock);
        pass.swap(g_pass);
    }
    if (!S.loaded || !g_pGameLevel || !Level().ObjectSpace.GetStaticModel())
    {
        pass.clear();
        return;
    }
    if (!S.applied)
    {
        S.applied = true;
        S.gen = G.gen;
        apply_saved();
    }
    if (S.need_restore)
    {
        S.need_restore = false;
        restore_all();
        S.gen = G.gen;
        apply_saved();
    }
    for (const Pass& e : pass)
        bullet_hit(e.p, e.d, e.tri, e.dmg); // E61
    pass.clear();
    if (!S.queue.empty())
    {
        xr_vector<u32> q;
        q.swap(S.queue);
        for (u32 id : q)
        {
            S.objs[id].queued = false;
            break_obj(id, false, nullptr, 0);
        }
    }
    if (S.active.empty())
        return;
    const float dt = clampr(Device.fTimeDelta, 0.f, 0.1f);
    update_bodies(dt);
}

void reset()
{
    {
        std::lock_guard<std::mutex> lk(g_pass_lock); // E61: no round of this level's taken on the next
        g_pass.clear();
    }
    stash();
    for (Obj& o : S.objs)
    {
        if (o.shell)
            destroy_physics_shell(o.shell);
        o.shell = nullptr;
        xr_delete(o.vis);
        xr_delete(o.stump_vis);
        kill_stump(o);
    }
    for (int m = 0; m < NMAT; ++m)
    {
        S.snd_break[m].clear();
        S.snd_land[m].clear();
    }
    S.snd_creak.clear();
    S.snd_spark.clear();
    const bool cfg = S.cfg;
    S = State{};
    S.cfg = cfg;
}

void save_game(LPCSTR sav_path)
{
    if (!sav_path || !*sav_path)
        return;
    stash();
    string_path fn;
    xr_sprintf(fn, "%s.statics", sav_path);
    u32 total = 0;
    for (const auto& [lvl, e] : G.levels)
        total += (u32)e.second.size();
    if (!total)
    {
        if (FS.exist(fn))
            FS.file_delete(fn);
        return;
    }
    IWriter* w = FS.w_open(fn);
    if (!w)
    {
        Msg("! [statics] cannot write %s", fn);
        return;
    }
    w->w("PMST", 4);
    w->w_u32(SAVE_VERSION);
    w->w_u32((u32)G.levels.size());
    for (const auto& [lvl, e] : G.levels)
    {
        w->w_stringZ(lvl.c_str());
        w->w_u32(e.first);
        w->w_u32((u32)e.second.size());
        for (const Saved& s : e.second)
        {
            w->w_u32(s.id);
            w->w_u8(s.state);
            w->w_fvector3(s.xf.i);
            w->w_fvector3(s.xf.j);
            w->w_fvector3(s.xf.k);
            w->w_fvector3(s.xf.c);
        }
    }
    FS.w_close(w);
    Msg("* [statics] %u broken object(s) saved to %s", total, fn);
}

void load_game(LPCSTR sav_path)
{
    ++G.gen;
    G.levels.clear();
    if (S.loaded)
        S.need_restore = true; // a save loaded on this level: everything stands again, then the save's state
    if (!sav_path || !*sav_path)
        return;
    string_path fn;
    xr_sprintf(fn, "%s.statics", sav_path);
    IReader* r = FS.r_open(fn);
    if (!r)
        return;
    char magic[4]{};
    if (r->length() >= 12)
        r->r(magic, 4);
    u32 total = 0;
    if (strncmp(magic, "PMST", 4) == 0 && r->r_u32() == SAVE_VERSION)
        for (u32 n = r->r_u32(); n && r->elapsed() > 0; --n)
        {
            shared_str lvl;
            r->r_stringZ(lvl);
            auto& e = G.levels[lvl];
            e.first = r->r_u32();
            for (u32 m = r->r_u32(); m && r->elapsed() >= 53; --m)
            {
                Saved s;
                s.id = r->r_u32();
                s.state = r->r_u8();
                s.xf.identity();
                r->r_fvector3(s.xf.i);
                r->r_fvector3(s.xf.j);
                r->r_fvector3(s.xf.k);
                r->r_fvector3(s.xf.c);
                s.xf._14 = s.xf._24 = s.xf._34 = 0.f;
                s.xf._44 = 1.f;
                e.second.push_back(s);
                ++total;
            }
        }
    FS.r_close(r);
    Msg("* [statics] %u broken object(s) loaded from %s", total, fn);
}

// ------------------------------------------------------------------------------------------------------------ Lua
LPCSTR lua_info()
{
    static string512 buf;
    u32 standing = 0, moving = 0, lying = 0, gone = 0, cls[8]{};
    for (const Obj& o : S.objs)
    {
        ++cls[S.kinds[o.kind].cls & 7]; // E61: 4 the panes of glass
        (o.state == ST_STANDING ? standing : o.state == ST_MOVING ? moving : o.state == ST_LYING ? lying : gone)++;
    }
    xr_sprintf(buf, "level=%s objects=%u poles=%u fences=%u lamps=%u wires=%u glass=%u standing=%u moving=%u lying=%u gone=%u broken=%u shots=%u crushed=%u enabled=%d",
        S.level.size() ? S.level.c_str() : "-", (u32)S.objs.size(), cls[0], cls[1], cls[2], cls[3], cls[4], standing, moving, lying, gone, S.broken, S.shots,
        S.crushed, on() ? 1 : 0);
    return buf;
}

LPCSTR lua_near(const Fvector& pos, float radius, LPCSTR kinds)
{
    static xr_string out;
    out.clear();
    xr_vector<u8> want(S.kinds.size(), 1);
    if (kinds && *kinds)
    {
        string256 one;
        for (u32 k = 0; k < S.kinds.size(); ++k)
        {
            want[k] = 0;
            for (int n = 0, cnt = _GetItemCount(kinds); n < cnt && !want[k]; ++n)
                want[k] = strstr(S.kinds[k].name.c_str(), _Trim(_GetItem(kinds, n, one))) != nullptr;
        }
    }
    xr_vector<std::pair<float, u32>> near_o;
    for (u32 i = 0; i < S.objs.size(); ++i)
    {
        if (!want[S.objs[i].kind])
            continue;
        const Obj& o = S.objs[i];
        const float d = (kind_of(o).cls == C_POLE ? o.pivot : obj_centre(o)).distance_to(pos);
        if (d <= radius)
            near_o.emplace_back(d, i);
    }
    std::sort(near_o.begin(), near_o.end());
    string256 one;
    for (u32 k = 0; k < near_o.size() && k < 96; ++k)
    {
        const Obj& o = S.objs[near_o[k].second];
        const Fvector c = kind_of(o).cls == C_POLE ? o.pivot : obj_centre(o);
        xr_sprintf(one, "%u|%s|%.2f|%.2f|%.2f|%.1f|%.0f|%.0f|%d|%.1f;", near_o[k].second, kind_of(o).name.c_str(), c.x, c.y, c.z, o.h, o.hp, o.dmg, (int)o.state,
            near_o[k].first);
        out += one;
    }
    return out.c_str();
}

// the level's collision inside the object's first box (where it stands): rays across its thinnest side on a 6 x 4 grid
// over the inner 0.7 of the others (a fence's posts at its ends stand), from both sides; one that meets something inside
// the box - not on its faces across the rays (what a crate stands on) - counts (standing: most; broken: none, or a wall
// of air was left there)
LPCSTR lua_probe(int id)
{
    static string64 buf;
    xr_strcpy(buf, "0/0");
    if (!S.loaded || !g_pGameLevel || id < 0 || id >= (int)S.objs.size() || S.objs[id].boxes.empty())
        return buf;
    const Obj& o = S.objs[id];
    const OBox& b = o.boxes[0];
    Fvector c, ax[3];
    o.rest.transform_tiny(c, b.c);
    o.rest.transform_dir(ax[0], b.R.i);
    o.rest.transform_dir(ax[1], b.R.j);
    o.rest.transform_dir(ax[2], b.R.k);
    const float h[3] = {b.h.x, b.h.y, b.h.z};
    int n = 0, u = 0, v = 0;
    for (int i = 1; i < 3; ++i)
        if (h[i] < h[n])
            n = i;
    u = (n + 1) % 3, v = (n + 2) % 3;
    if (h[v] > h[u])
        std::swap(u, v);
    int hits = 0, rays = 0;
    for (int iu = 0; iu < 6; ++iu)
        for (int iv = 0; iv < 4; ++iv)
        {
            Fvector m = c;
            m.mad(ax[u], h[u] * 0.7f * (-1.f + 2.f * iu / 5.f));
            m.mad(ax[v], h[v] * 0.7f * (-1.f + 2.f * iv / 3.f));
            bool hit = false;
            for (float side : {1.f, -1.f})
            {
                Fvector d, e;
                d.set(ax[n]).mul(-side);
                e.mad(m, ax[n], side * (h[n] + 0.1f));
                collide::rq_result R;
                if (Level().ObjectSpace.RayPick(e, d, 2.f * (h[n] + 0.1f), collide::rqtStatic, R, nullptr) && R.element >= 0)
                {
                    Fvector q, r;
                    q.mad(e, d, R.range);
                    r.sub(q, c);
                    const float in_n = _max(h[n] - 0.04f, 0.03f);
                    hit = hit || (_abs(r.dotproduct(ax[n])) <= in_n && _abs(r.dotproduct(ax[u])) <= h[u] + 0.02f && _abs(r.dotproduct(ax[v])) <= h[v] + 0.02f);
                }
            }
            hits += hit;
            ++rays;
        }
    xr_sprintf(buf, "%d/%d", hits, rays);
    return buf;
}

bool lua_break(int id, const Fvector& dir)
{
    load_cfg();
    if (!S.loaded || id < 0 || id >= (int)S.objs.size() || S.objs[id].state != ST_STANDING)
        return false;
    Obj& o = S.objs[id];
    o.push.set(dir.x, 0.f, dir.z);
    o.dmg = o.hp;
    o.why = "a script";
    break_obj((u32)id, false, nullptr, 0);
    return true;
}

void lua_debug(bool on_)
{
    load_cfg();
    C.debug = on_;
}
} // namespace portal_mod_statics
