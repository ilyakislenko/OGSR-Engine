// portal-mod E58: falling trees (tools/engine_patches/patch_e58.py) - see portal_mod_trees.h.
// A tree is an MU instance of the level (Layers/xrRenderPC_R4/r4_loader.cpp lists them: an FLOD and its parts, each part
// drawn instanced by the instance's own 3x4 matrix). Its trunk and branches are in level.cform (materials\tree_trunk,
// materials\bush). Its trunk is where the plane 0.9 m over the root cuts those (a pine's is one tall cone of 14
// triangles, so it has no corners down there; a big willow's is a closed hull of hundreds): the cut chain round the root
// gives the trunk's axis and radius. Its triangles are the pieces joined to that chain and the loose ones (branches, the
// crown's bush hull) nearest its trunk in its box. Its height is the highest trunk corner; the crown's is the FLOD's box.
// Falling: a rod turning over its root, w' = 1.5 g / L sin(th) (L between the trunk's and the crown's height), the
// matrix rotated round the horizontal axis across the fall each frame; four points along the trunk (ahead by its
// radius) and two in the crown (ahead by half its half-width: its branches hold it off a slope) sweep their arcs by rays
// against the static collision (what is left of it: the tree's own triangles point at three vertices far under the
// level, g_pm_cform_sink); the rays pass what rounds pass (crowns' bush hulls) and its own loose branches - the first
// hit stops it there (the ground, a wall, another trunk: it may hang on a neighbour).
#include "stdafx.h"
#include <algorithm>
#include "portal_mod_trees.h"
#include "portal_mod_destruct.h"
#include "portal_mod_features.h"
#include "Level.h"
#include "Actor.h"
#include "entity_alive.h"
#include "EntityCondition.h"
#include "Hit.h"
#include "PhysicsShellHolder.h"
#include "PhysicsShell.h"
#include "PHStaticGeomShell.h"
#include "PHCollideValidator.h"
#include "../xr_3da/GameMtlLib.h"

namespace portal_mod_trees
{
namespace
{
enum : u8
{
    ST_STANDING,
    ST_FALLING,
    ST_LYING
};

// a fallen trunk's collision: a static box (no body, no owner: its update must not reach for one)
class TrunkShell : public CPHStaticGeomShell
{
    void PhDataUpdate(dReal step) override
    {
        Island().Step(step);
        Island().Unmerge();
        CPHUpdateObject::Deactivate();
    }
};

struct Tree
{
    u32 rid{}; // the renderer's index (portal_mod_tree_*)
    Fmatrix xf0{}; // its own matrix
    Fvector pivot{}; // the trunk's centre at the root
    float h{}; // the crown's top above the root
    float cw{}; // the crown's half-width (half the FLOD box's mean side)
    Fbox box{}; // the FLOD's box grown by 0.3 m (its own loose branches lie in it)
    float ht{}; // the trunk's top above the root
    float r{}; // the trunk's radius a metre up (0: no trunk in the collision - it stays)
    float hp{}, mass{};
    xr_vector<u32> tris; // its triangles in level.cform
    xr_vector<CDB::TRI> saved; // as they were (put back when a save is loaded on this level)
    float dmg{};
    Fvector push{}; // the shots' and blasts' horizontal directions, weighted by their damage
    u8 state{ST_STANDING};
    bool creaked{};
    float th{}, w{}; // the angle from upright and its rate
    Fvector dir{1.f, 0.f, 0.f}; // the fall's horizontal direction
    u8 bounces{};
    u32 t_fell{};
    xr_vector<u16> hit_ids; // creatures and objects it has struck already
    TrunkShell* shell{};
};

struct Cfg
{
    bool enabled{true}, debug{}, lying_collision{true};
    float hp_k{5300.f}, hp_min{25.f}, hp_max{1200.f};
    float cut_h{2.5f}, high_k{0.15f};
    float blast_k{110.f}, blast_pow{1.5f}, blast_cover{0.f};
    float min_h{3.f}, density{800.f}, taper{0.55f};
    float w0_shot{0.08f}, w0_blast{0.9f}, bounce{0.18f};
    float crush_v{2.f}, crush_k{5e-4f}, crush_lethal{6000.f}, crush_reach{0.35f}, crush_mass{3000.f}, push_k{0.6f};
    float max_dist{300.f}; // m: trees further from the camera keep falling but skip the crush checks
    xr_vector<shared_str> snd_creak, snd_crack, snd_crash, snd_thud, fx_crash, fx_leaves;
};

struct Saved
{
    u32 rid;
    u8 state;
    float th, w, dx, dz;
};

struct State
{
    bool cfg{}, built{}, sounds{}, need_restore{};
    u32 gen{};
    shared_str level;
    xr_vector<Tree> trees;
    xr_vector<std::pair<u32, u16>> tri_tree; // sorted by triangle
    xr_vector<u16> active; // falling trees
    u16 m_trunk{GAMEMTL_NONE_IDX}, m_bush{GAMEMTL_NONE_IDX};
    xr_vector<ref_sound> creak, crack, crash, thud;
    u32 fallen{}, crushed{}, shots{};
};

struct Global
{
    u32 gen{1};
    xr_map<shared_str, xr_vector<Saved>> levels;
};

Cfg C;
State S;
Global G;

float cfg_f(LPCSTR s, LPCSTR k, float def) { return pSettings->line_exist(s, k) ? pSettings->r_float(s, k) : def; }
bool cfg_b(LPCSTR s, LPCSTR k, bool def) { return pSettings->line_exist(s, k) ? !!pSettings->r_bool(s, k) : def; }

void cfg_list(LPCSTR s, LPCSTR k, xr_vector<shared_str>& out, LPCSTR def)
{
    out.clear();
    LPCSTR v = pSettings->line_exist(s, k) ? pSettings->r_string(s, k) : def;
    if (!v || !*v)
        return;
    string256 one;
    for (int i = 0, n = _GetItemCount(v); i < n; ++i)
    {
        _GetItem(v, i, one);
        out.emplace_back(_Trim(one));
    }
}

void load_cfg()
{
    if (S.cfg)
        return;
    S.cfg = true;
    LPCSTR s = "pm_trees";
    if (!pSettings->section_exist(s))
        return;
    C.enabled = cfg_b(s, "enabled", C.enabled);
    C.debug = cfg_b(s, "debug", C.debug);
    C.lying_collision = cfg_b(s, "lying_collision", C.lying_collision);
    C.hp_k = cfg_f(s, "hp_k", C.hp_k);
    C.hp_min = cfg_f(s, "hp_min", C.hp_min);
    C.hp_max = cfg_f(s, "hp_max", C.hp_max);
    C.cut_h = cfg_f(s, "cut_h", C.cut_h);
    C.high_k = cfg_f(s, "high_k", C.high_k);
    C.blast_k = cfg_f(s, "blast_k", C.blast_k);
    C.blast_pow = cfg_f(s, "blast_pow", C.blast_pow);
    C.blast_cover = cfg_f(s, "blast_cover", C.blast_cover);
    C.min_h = cfg_f(s, "min_h", C.min_h);
    C.density = cfg_f(s, "density", C.density);
    C.taper = cfg_f(s, "taper", C.taper);
    C.w0_shot = cfg_f(s, "w0_shot", C.w0_shot);
    C.w0_blast = cfg_f(s, "w0_blast", C.w0_blast);
    C.bounce = cfg_f(s, "bounce", C.bounce);
    C.crush_v = cfg_f(s, "crush_speed", C.crush_v);
    C.crush_k = cfg_f(s, "crush_k", C.crush_k);
    C.crush_lethal = cfg_f(s, "crush_lethal", C.crush_lethal);
    C.crush_reach = cfg_f(s, "crush_reach", C.crush_reach);
    C.crush_mass = cfg_f(s, "crush_mass", C.crush_mass);
    C.push_k = cfg_f(s, "push_k", C.push_k);
    C.max_dist = cfg_f(s, "max_dist", C.max_dist);
    cfg_list(s, "snd_creak", C.snd_creak, "ambient\\x18\\x18_creak_1, ambient\\x18\\x18_creak_2");
    cfg_list(s, "snd_crack", C.snd_crack, "material\\wood\\wood_hit1_hl2, material\\wood\\wood_hit2_hl2");
    cfg_list(s, "snd_crash", C.snd_crash, "material\\wood\\wood_crash1_hl2");
    cfg_list(s, "snd_thud", C.snd_thud, "material\\large_furniture\\large_furniture_1");
    cfg_list(s, "fx_crash", C.fx_crash, "destroy_fx\\destroy_wood_00");
    cfg_list(s, "fx_leaves", C.fx_leaves, "nature\\tree_leaves_falling_01");
}

bool on() { return C.enabled && portal_mod_feat::enabled(portal_mod_feat::destruct); }

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
    mk(C.snd_creak, S.creak);
    mk(C.snd_crack, S.crack);
    mk(C.snd_crash, S.crash);
    mk(C.snd_thud, S.thud);
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

void play_fx(const xr_vector<shared_str>& names, const Fvector& pos, const Fvector& dir)
{
    if (names.empty())
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

// Rodrigues: v turned by th round the unit axis k
Fvector turn(const Fvector& v, const Fvector& k, float th)
{
    const float c = _cos(th), s = _sin(th);
    Fvector kv, out;
    kv.crossproduct(k, v);
    out.mul(v, c);
    out.mad(kv, s);
    out.mad(k, k.dotproduct(v) * (1.f - c));
    return out;
}

Fvector axis_of(const Tree& t)
{
    Fvector a;
    a.crossproduct(Fvector{0.f, 1.f, 0.f}, t.dir); // up turned round it goes towards dir
    return a.normalize_safe();
}

// the trunk's direction at angle th
Fvector up_at(const Tree& t, float th)
{
    Fvector u;
    u.set(0.f, _cos(th), 0.f);
    u.mad(t.dir, _sin(th));
    return u;
}

Fmatrix xform_at(const Tree& t, float th)
{
    const Fvector k = axis_of(t);
    Fmatrix m = t.xf0;
    m.i.set(turn(Fvector{t.xf0._11, t.xf0._12, t.xf0._13}, k, th));
    m.j.set(turn(Fvector{t.xf0._21, t.xf0._22, t.xf0._23}, k, th));
    m.k.set(turn(Fvector{t.xf0._31, t.xf0._32, t.xf0._33}, k, th));
    Fvector c;
    c.sub(t.xf0.c, t.pivot);
    m.c.add(turn(c, k, th), t.pivot);
    m._14 = m._24 = m._34 = 0.f;
    m._44 = 1.f;
    return m;
}

void drop_tris(Tree& t)
{
    if (g_pm_cform_sink == u32(-1) || !t.saved.empty())
        return;
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    t.saved.reserve(t.tris.size());
    for (u32 i : t.tris)
    {
        t.saved.push_back(T[i]);
        T[i].verts[0] = g_pm_cform_sink;
        T[i].verts[1] = g_pm_cform_sink + 1;
        T[i].verts[2] = g_pm_cform_sink + 2;
    }
}

void restore_tris(Tree& t)
{
    if (t.saved.size() != t.tris.size())
    {
        t.saved.clear();
        return;
    }
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    for (u32 k = 0; k < t.tris.size(); ++k)
        T[t.tris[k]] = t.saved[k];
    t.saved.clear();
}

void kill_shell(Tree& t)
{
    if (!t.shell)
        return;
    t.shell->Deactivate();
    xr_delete(t.shell);
}

void make_shell(Tree& t)
{
    if (!C.lying_collision || t.shell || t.r <= 0.f)
        return;
    const Fvector u = up_at(t, t.th);
    const Fvector a = axis_of(t);
    Fobb b;
    b.m_rotate.i.set(a);
    b.m_rotate.j.set(u);
    b.m_rotate.k.crossproduct(a, u);
    b.m_translate.mad(t.pivot, u, t.ht * 0.5f);
    b.m_halfsize.set(t.r, t.ht * 0.5f, t.r);
    t.shell = xr_new<TrunkShell>();
    t.shell->add_Box(b);
    t.shell->Activate(Fidentity);
    CPHCollideValidator::SetNonDynamicObject(*t.shell);
}

int tree_of_tri(u32 tri)
{
    auto it = std::lower_bound(S.tri_tree.begin(), S.tri_tree.end(), std::make_pair(tri, u16(0)));
    return it != S.tri_tree.end() && it->first == tri ? it->second : -1;
}

void apply_saved(const xr_vector<Saved>& list);

// the trunk a little over the root: cut the static model's tree_trunk triangles there by the plane y = const and chain
// the cut segments by the edges they share; the chain whose convex hull holds the root (else the nearest, 0.6 m at
// most) is its trunk: its hull's centroid the trunk's axis, sqrt(area / pi) its radius
struct Section
{
    float area{};
    Fvector2 cen{};
    xr_vector<u32> tris;
};

float hull_cross(const Fvector2& o, const Fvector2& a, const Fvector2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Andrew's monotone chain, counter-clockwise
void hull_of(xr_vector<Fvector2>& P, xr_vector<Fvector2>& H)
{
    H.clear();
    std::sort(P.begin(), P.end(), [](const Fvector2& a, const Fvector2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (P.size() < 3)
        return;
    H.resize(2 * P.size());
    size_t k = 0;
    for (size_t i = 0; i < P.size(); ++i)
    {
        while (k >= 2 && hull_cross(H[k - 2], H[k - 1], P[i]) <= 0.f)
            --k;
        H[k++] = P[i];
    }
    for (size_t i = P.size() - 1, lo = k + 1; i > 0; --i)
    {
        while (k >= lo && hull_cross(H[k - 2], H[k - 1], P[i - 1]) <= 0.f)
            --k;
        H[k++] = P[i - 1];
    }
    H.resize(k > 0 ? k - 1 : 0);
}

bool find_section(const Fvector& root, u16 m_trunk, Section& out)
{
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector* V = Level().ObjectSpace.GetStaticVerts();
    CDB::COLLIDER xrc;
    struct Seg
    {
        Fvector2 a, b;
        u32 tri;
        u64 ka, kb;
    };
    static xr_vector<Seg> segs;
    static xr_vector<u32> par;
    static xr_vector<std::pair<u64, u32>> keys;
    static xr_vector<Fvector2> P, H;
    const float h0s[4] = {0.9f, 1.3f, 0.6f, 1.7f};
    for (float h0 : h0s)
    {
        const float y = root.y + h0;
        xrc.box_query(0, Level().ObjectSpace.GetStaticModel(), Fvector{root.x, y, root.z}, Fvector{1.2f, 0.01f, 1.2f});
        segs.clear();
        for (const CDB::RESULT* rp = xrc.r_begin(); rp != xrc.r_end(); ++rp)
        {
            const CDB::TRI& tr = T[rp->id];
            if (tr.material != m_trunk)
                continue;
            Fvector2 pt[2];
            u64 kk[2];
            int n = 0;
            for (int e = 0; e < 3 && n < 2; ++e)
            {
                u32 va = tr.verts[e], vb = tr.verts[(e + 1) % 3];
                if ((V[va].y < y) == (V[vb].y < y))
                    continue;
                if (va > vb)
                    std::swap(va, vb);
                const float s = (y - V[va].y) / (V[vb].y - V[va].y);
                pt[n].set(V[va].x + s * (V[vb].x - V[va].x), V[va].z + s * (V[vb].z - V[va].z));
                kk[n] = (u64(va) << 32) | vb;
                ++n;
            }
            if (n == 2)
                segs.push_back(Seg{pt[0], pt[1], u32(rp->id), kk[0], kk[1]});
        }
        if (segs.size() < 3)
            continue;
        // segments sharing a cut edge are one chain
        par.resize(segs.size());
        for (u32 i = 0; i < par.size(); ++i)
            par[i] = i;
        auto root_of = [](u32 a) {
            while (par[a] != a)
                a = par[a] = par[par[a]];
            return a;
        };
        keys.clear();
        for (u32 i = 0; i < segs.size(); ++i)
        {
            keys.emplace_back(segs[i].ka, i);
            keys.emplace_back(segs[i].kb, i);
        }
        std::sort(keys.begin(), keys.end());
        for (u32 i = 1; i < keys.size(); ++i)
            if (keys[i].first == keys[i - 1].first)
                par[root_of(keys[i].second)] = root_of(keys[i - 1].second);
        xr_map<u32, xr_vector<u32>> groups;
        for (u32 i = 0; i < segs.size(); ++i)
            groups[root_of(i)].push_back(i);
        const Fvector2 r2{root.x, root.z};
        float best_a = flt_max, best_d = 0.6f;
        const xr_vector<u32>* best = nullptr;
        const xr_vector<u32>* best_near = nullptr;
        Fvector2 best_c{}, near_c{};
        float near_a = 0.f;
        for (const auto& [g, list] : groups)
        {
            if (list.size() < 3)
                continue;
            P.clear();
            for (u32 i : list)
            {
                P.push_back(segs[i].a);
                P.push_back(segs[i].b);
            }
            hull_of(P, H);
            if (H.size() < 3)
                continue;
            float A = 0.f, cx = 0.f, cz = 0.f;
            bool in = true;
            for (size_t i = 0; i < H.size(); ++i)
            {
                const Fvector2& a = H[i];
                const Fvector2& b = H[(i + 1) % H.size()];
                const float c = a.x * b.y - b.x * a.y;
                A += c;
                cx += (a.x + b.x) * c;
                cz += (a.y + b.y) * c;
                if (hull_cross(a, b, r2) < 0.f)
                    in = false;
            }
            if (A < 1e-5f)
                continue;
            const Fvector2 c{cx / (3.f * A), cz / (3.f * A)};
            A *= 0.5f;
            if (in)
            {
                if (A < best_a)
                    best_a = A, best = &list, best_c = c;
            }
            else
            {
                const float d = _sqrt(_sqr(c.x - r2.x) + _sqr(c.y - r2.y));
                if (d < best_d)
                    best_d = d, best_near = &list, near_c = c, near_a = A;
            }
        }
        const xr_vector<u32>* pick = best ? best : best_near;
        if (!pick)
            continue;
        out.area = best ? best_a : near_a;
        out.cen = best ? best_c : near_c;
        out.tris.clear();
        for (u32 i : *pick)
            out.tris.push_back(segs[i].tri);
        return true;
    }
    return false;
}

void build()
{
    S.built = true;
    S.gen = G.gen;
    S.level = Level().name();
    S.trees.clear();
    S.tri_tree.clear();
    S.active.clear();
    const u32 n = portal_mod_tree_count();
    const u16 m_trunk = GMLib.GetMaterialIdx("materials\\tree_trunk");
    const u16 m_bush = GMLib.GetMaterialIdx("materials\\bush");
    S.m_trunk = m_trunk;
    S.m_bush = m_bush;
    if (!n || m_trunk == GAMEMTL_NONE_IDX)
        return;
    CTimer tm;
    tm.Start();
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector* V = Level().ObjectSpace.GetStaticVerts();
    // 1. the trees with a trunk in the collision
    xr_vector<xr_vector<u32>> seeds;
    xr_vector<Fbox> boxes;
    u32 standing = 0;
    Section sec;
    for (u32 i = 0; i < n && S.trees.size() < 65000; ++i)
    {
        Fmatrix xf;
        Fbox box;
        u32 flags = 0;
        if (!portal_mod_tree_get(i, xf, box, flags) || !(flags & 1))
            continue;
        if (box.max.y - xf.c.y < C.min_h)
            continue;
        ++standing;
        if (!find_section(xf.c, m_trunk, sec))
            continue; // no trunk in the collision (a bush, a sapling, a tree bullets go through): it stays
        Tree t;
        t.rid = i;
        t.xf0 = xf;
        t.pivot.set(sec.cen.x, xf.c.y, sec.cen.y);
        t.h = box.max.y - xf.c.y;
        t.cw = 0.25f * ((box.max.x - box.min.x) + (box.max.z - box.min.z));
        t.r = clampr(_sqrt(sec.area / PI), 0.04f, 1.5f);
        box.grow(0.3f);
        t.box = box;
        S.trees.push_back(std::move(t));
        seeds.push_back(sec.tris);
        boxes.push_back(box);
    }
    // 2. its triangles: the tree_trunk / bush pieces (triangles joined by shared verts) in the boxes; the piece its
    // trunk's section cut is its own, a loose one (a branch, the crown's bush hull) goes to the nearest trunk among the
    // boxes it lies in whole
    xr_vector<std::pair<u32, u16>> cand; // (triangle, tree whose box it is in)
    {
        CDB::COLLIDER xrc;
        for (u32 i = 0; i < S.trees.size(); ++i)
        {
            Fvector c, d;
            boxes[i].get_CD(c, d);
            xrc.box_query(0, Level().ObjectSpace.GetStaticModel(), c, d);
            for (const CDB::RESULT* rp = xrc.r_begin(); rp != xrc.r_end(); ++rp)
            {
                const u16 mt = T[rp->id].material;
                if (mt == m_trunk || mt == m_bush)
                    cand.emplace_back(u32(rp->id), u16(i));
            }
        }
    }
    std::sort(cand.begin(), cand.end());
    xr_vector<u32> tris; // unique
    tris.reserve(cand.size());
    for (const auto& c : cand)
        if (tris.empty() || tris.back() != c.first)
            tris.push_back(c.first);
    auto idx_of = [&tris](u32 tri) { return u32(std::lower_bound(tris.begin(), tris.end(), tri) - tris.begin()); };
    // pieces: union-find over the triangles sharing a vertex
    xr_vector<u32> par(tris.size());
    for (u32 i = 0; i < par.size(); ++i)
        par[i] = i;
    auto find_p = [&par](u32 a) {
        while (par[a] != a)
            a = par[a] = par[par[a]];
        return a;
    };
    {
        xr_vector<std::pair<u32, u32>> vt; // (vertex, triangle index)
        vt.reserve(tris.size() * 3);
        for (u32 i = 0; i < tris.size(); ++i)
            for (u32 v : T[tris[i]].verts)
                vt.emplace_back(v, i);
        std::sort(vt.begin(), vt.end());
        for (u32 i = 1; i < vt.size(); ++i)
            if (vt[i].first == vt[i - 1].first)
            {
                const u32 a = find_p(vt[i].second), b = find_p(vt[i - 1].second);
                if (a != b)
                    par[a] = b;
            }
    }
    xr_vector<u32> piece(tris.size());
    for (u32 i = 0; i < tris.size(); ++i)
        piece[i] = find_p(i);
    // per piece: its box, its lowest point, the trees whose trunk section cut it, the trees whose boxes reach it
    struct Piece
    {
        Fbox box;
        Fvector low{0.f, flt_max, 0.f};
        xr_vector<u16> seed_of, near_of;
        int owner{-1};
    };
    xr_unordered_map<u32, Piece> pieces;
    for (u32 i = 0; i < tris.size(); ++i)
    {
        Piece& p = pieces[piece[i]];
        if (p.low.y == flt_max)
            p.box.invalidate();
        for (u32 v : T[tris[i]].verts)
        {
            p.box.modify(V[v]);
            if (V[v].y < p.low.y)
                p.low = V[v];
        }
    }
    for (u32 ti = 0; ti < seeds.size(); ++ti)
        for (u32 tri : seeds[ti])
        {
            Piece& p = pieces[piece[idx_of(tri)]];
            if (std::find(p.seed_of.begin(), p.seed_of.end(), u16(ti)) == p.seed_of.end())
                p.seed_of.push_back(u16(ti));
        }
    for (const auto& c : cand)
    {
        Piece& p = pieces[piece[idx_of(c.first)]];
        if (std::find(p.near_of.begin(), p.near_of.end(), c.second) == p.near_of.end())
            p.near_of.push_back(c.second);
    }
    u32 loose_taken = 0, loose_left = 0;
    for (auto& [id, p] : pieces)
    {
        float best = flt_max;
        const xr_vector<u16>& from = p.seed_of.empty() ? p.near_of : p.seed_of;
        for (u16 ti : from)
        {
            if (p.seed_of.empty())
            {
                const Fbox& b = boxes[ti];
                if (p.box.min.x < b.min.x || p.box.min.y < b.min.y || p.box.min.z < b.min.z || p.box.max.x > b.max.x || p.box.max.y > b.max.y ||
                    p.box.max.z > b.max.z)
                    continue;
            }
            const Tree& t = S.trees[ti];
            const float d = _sqr(t.pivot.x - p.low.x) + _sqr(t.pivot.z - p.low.z);
            if (d < best)
                best = d, p.owner = ti;
        }
        if (p.seed_of.empty())
            ++(p.owner >= 0 ? loose_taken : loose_left);
    }
    for (u32 i = 0; i < tris.size(); ++i)
    {
        const Piece& p = pieces[piece[i]];
        if (p.owner >= 0)
            S.trees[p.owner].tris.push_back(tris[i]);
    }
    // 3. the trunk's height (its highest tree_trunk corner), hit points, mass
    u32 ntris = 0;
    xr_vector<float> rs;
    for (u32 i = 0; i < S.trees.size(); ++i)
    {
        Tree& t = S.trees[i];
        std::sort(t.tris.begin(), t.tris.end());
        ntris += (u32)t.tris.size();
        float top = 0.f;
        for (u32 tri : t.tris)
        {
            S.tri_tree.emplace_back(tri, u16(i));
            const CDB::TRI& tr = T[tri];
            if (tr.material == m_trunk)
                for (u32 k : tr.verts)
                    top = _max(top, V[k].y - t.pivot.y);
        }
        t.ht = clampr(top, 1.f, t.h);
        t.hp = clampr(C.hp_k * t.r * t.r, C.hp_min, C.hp_max);
        t.mass = C.density * PI * t.r * t.r * t.ht * C.taper;
        rs.push_back(t.r);
    }
    std::sort(S.tri_tree.begin(), S.tri_tree.end());
    std::sort(rs.begin(), rs.end());
    auto q = [&rs](float f) { return rs.empty() ? 0.f : rs[_min(u32(rs.size() - 1), u32(f * rs.size()))]; };
    u32 thin = 0;
    for (float r : rs)
        thin += r < 0.25f;
    Msg("* [trees] %s: %u MU instances, %u trees %.0f m and taller, %u with a trunk in the collision (%u under r 0.25 m; r 10/50/90%%: %.2f %.2f %.2f m), "
        "%u collision triangles (%u loose pieces taken, %u left), %.0f ms",
        S.level.c_str(), n, standing, C.min_h, (u32)S.trees.size(), thin, q(0.1f), q(0.5f), q(0.9f), ntris, loose_taken, loose_left, tm.GetElapsed_sec() * 1000.f);
    auto it = G.levels.find(S.level);
    if (it != G.levels.end())
        apply_saved(it->second);
}

void start_fall(u16 id, const Fvector& dir_in, float w0, LPCSTR why)
{
    Tree& t = S.trees[id];
    if (t.state != ST_STANDING || t.r <= 0.f)
        return;
    Fvector d{dir_in.x, 0.f, dir_in.z};
    if (d.square_magnitude() < 1e-4f)
        d.set(::Random.randF(-1.f, 1.f), 0.f, ::Random.randF(-1.f, 1.f));
    t.dir = d.normalize_safe();
    t.state = ST_FALLING;
    t.th = 0.01f;
    t.w = _max(w0, 0.02f);
    t.bounces = 0;
    t.t_fell = Device.dwTimeGlobal;
    t.hit_ids.clear();
    drop_tris(t);
    portal_mod_tree_set(t.rid, xform_at(t, t.th), 1);
    S.active.push_back(id);
    ++S.fallen;
    make_sounds();
    Fvector p;
    p.mad(t.pivot, Fvector{0.f, 1.f, 0.f}, _min(1.2f, t.ht * 0.5f));
    play_snd(S.crack, p, clampr(t.mass / 300.f, 0.6f, 1.5f));
    Msg("* [trees] tree %u (r %.2f m, %.1f m, trunk %.1f m, %.0f kg) comes down by %s: %.0f of %.0f, towards %.2f %.2f, %.1f m from the camera", id,
        t.r, t.h, t.ht, t.mass, why, t.dmg, t.hp, t.dir.x, t.dir.z, t.pivot.distance_to(Device.vCameraPosition));
}

void damage(u16 id, float dmg, const Fvector& dir, float w0, LPCSTR why)
{
    Tree& t = S.trees[id];
    if (t.state != ST_STANDING || t.r <= 0.f)
        return;
    t.dmg += dmg;
    t.push.mad(Fvector{dir.x, 0.f, dir.z}.normalize_safe(), dmg);
    if (C.debug)
        Msg("[trees] tree %u: %.1f by %s, %.0f of %.0f", id, dmg, why, t.dmg, t.hp);
    if (t.dmg >= t.hp)
    {
        start_fall(id, t.push, w0, why);
        return;
    }
    if (!t.creaked && t.dmg >= 0.7f * t.hp)
    {
        t.creaked = true;
        make_sounds();
        Fvector p;
        p.mad(t.pivot, Fvector{0.f, 1.f, 0.f}, _min(2.f, t.ht * 0.5f));
        play_snd(S.creak, p, 1.f);
    }
}

// whoever stands where its trunk sweeps this frame
void crush(Tree& t, u16 id)
{
    const Fvector u = up_at(t, t.th);
    Fvector mid;
    mid.mad(t.pivot, u, t.ht * 0.5f);
    static xr_vector<CObject*> objs;
    objs.clear();
    Level().ObjectSpace.GetNearest(objs, mid, t.ht * 0.5f + 3.f + 0.5f * t.cw, nullptr);
    if (objs.empty())
        return;
    Fvector n; // the trunk's motion at th
    n.set(0.f, -_sin(t.th), 0.f);
    n.mad(t.dir, _cos(t.th));
    CActor* actor = Actor();
    for (CObject* o : objs)
    {
        if (o->getDestroy() || std::find(t.hit_ids.begin(), t.hit_ids.end(), o->ID()) != t.hit_ids.end())
            continue;
        CEntityAlive* e = smart_cast<CEntityAlive*>(o);
        CPhysicsShellHolder* ph = smart_cast<CPhysicsShellHolder*>(o);
        if (e && !e->g_Alive())
            e = nullptr;
        if (!e && !(ph && ph->PPhysicsShell()))
            continue;
        const float girth = e ? clampr(o->Radius() * 0.45f, 0.25f, 0.45f) : _min(o->Radius(), 1.f);
        const float top = e ? clampr(o->Radius() * 2.f - 0.1f, 0.5f, 1.9f) : 0.f;
        float gap = flt_max, s_at = 0.f, crown_k = 1.f;
        Fvector at{};
        for (int k = 0; k < (e ? 4 : 1); ++k)
        {
            Fvector P = o->Position();
            if (e)
                P.y += 0.3f + (top - 0.3f) * k / 3.f;
            else
                o->Center(P);
            Fvector q;
            q.sub(P, t.pivot);
            const float s = clampr(q.dotproduct(u), 0.f, t.ht);
            Fvector on;
            on.mad(t.pivot, u, s);
            const bool in_crown = s > 0.45f * t.h;
            const float g = P.distance_to(on) - t.r - girth - C.crush_reach - (in_crown ? 0.5f * t.cw : 0.f);
            if (g < gap)
                gap = g, at = P, s_at = s, crown_k = in_crown && P.distance_to(on) > t.r + girth + C.crush_reach ? 0.5f : 1.f;
        }
        if (gap > 0.f)
            continue;
        const float sp = t.w * s_at;
        if (sp < C.crush_v)
            continue;
        t.hit_ids.push_back(o->ID());
        const float m = _min(t.mass * 0.5f, C.crush_mass);
        const float E = 0.5f * m * sp * sp * crown_k;
        SHit HS;
        HS.GenHeader(GE_HIT, o->ID());
        HS.whoID = actor ? actor->ID() : o->ID();
        HS.weaponID = HS.whoID;
        HS.dir = n;
        HS.power = C.crush_k * E;
        if (e && E >= C.crush_lethal)
            HS.power = _max(HS.power, 4.f / _max(e->conditions().GetHitImmunity(ALife::eHitTypeStrike), 0.02f));
        HS.p_in_bone_space = at;
        HS.impulse = _min(600.f, C.push_k * m * sp);
        HS.hit_type = ALife::eHitTypeStrike;
        HS.boneID = 0;
        NET_Packet P;
        HS.Write_Packet(P);
        CGameObject::u_EventSend(P);
        if (!e && ph && ph->PPhysicsShell() && ph->PPhysicsShell()->isActive())
            ph->PPhysicsShell()->applyImpulse(n, _min(m, ph->PPhysicsShell()->getMass() * 4.f) * sp * C.push_k);
        ++S.crushed;
        Msg("[trees] tree %u falls on %s (id %u%s) at %.1f m/s, %.1f m along its trunk%s - %.0f J, power %.2f", id, o->cName().c_str(), o->ID(),
            e ? "" : ", an object", sp, s_at, crown_k < 1.f ? " (its branches)" : "", E, HS.power);
    }
}

// a falling tree's ray against the static collision: it passes what rounds pass (shoot factor 1 - crowns' bush hulls)
// and the tree's own loose tree_trunk pieces (branches no trunk took, in its box); the ground, walls and other trunks stop it
bool fall_ray(const Tree& t, const Fvector& a, const Fvector& d, float range, float& dist)
{
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector p = a;
    float gone = 0.f;
    for (int k = 0; k < 6 && gone < range; ++k)
    {
        collide::rq_result R;
        if (!Level().ObjectSpace.RayPick(p, d, range - gone, collide::rqtStatic, R, nullptr))
            return false;
        const CDB::TRI& tr = T[R.element];
        Fvector h;
        h.mad(p, d, R.range);
        const SGameMtl* mtl = GMLib.GetMaterialByIdx(tr.material);
        const bool pass = (mtl && mtl->fShootFactor >= 0.999f) || tr.material == S.m_bush || (tr.material == S.m_trunk && t.box.contains(h));
        if (!pass)
        {
            dist = gone + R.range;
            return true;
        }
        gone += R.range + 0.02f;
        p.mad(h, d, 0.02f);
    }
    return false;
}

// one frame of a falling tree: true when it has come to rest
bool fall_step(Tree& t, u16 id, float dt)
{
    const float L = _max(0.5f * (t.ht + t.h), 2.f);
    const float alpha = 1.5f * 9.81f / L * _sin(t.th);
    t.w += alpha * dt;
    float th1 = t.th + t.w * dt;
    if (th1 < 0.f) // a bounce back past upright: settle on the way
        th1 = 0.f;
    // where four points of the trunk and two of the crown would go: the first that meets the level stops it. The crown's
    // lead is half its half-width: its lower branches hold the trunk off the ground (they do not sink into a slope)
    const float fr[6] = {0.3f, 0.55f, 0.8f, 1.f, -0.6f, -0.85f};
    const float crown_lead = clampr(0.5f * t.cw, 0.3f, 2.5f);
    float best = 2.f;
    Fvector hit_at{};
    const Fvector u0 = up_at(t, t.th), u1 = up_at(t, th1);
    for (float f : fr)
    {
        const float len = f > 0.f ? f * t.ht : -f * t.h;
        if (f < 0.f && -f * t.h < 2.f)
            continue;
        Fvector a, b;
        a.mad(t.pivot, u0, len);
        b.mad(t.pivot, u1, len);
        Fvector d;
        d.sub(b, a);
        float l = d.magnitude();
        if (l < 1e-4f)
            continue;
        d.div(l);
        const float lead = f > 0.f ? t.r : crown_lead; // the trunk's surface ahead of its axis, the crown's branches
        float dist;
        if (fall_ray(t, a, d, l + lead, dist))
        {
            const float k = clampr((dist - lead) / l, 0.f, 1.f);
            if (k < best)
                best = k, hit_at.mad(a, d, dist);
        }
    }
    const float th_prev = t.th;
    if (best <= 1.f)
        t.th = t.th + (th1 - t.th) * best;
    else
        t.th = th1;
    if (t.th > 1.62f)
        best = 0.f, t.th = 1.62f, hit_at.mad(t.pivot, up_at(t, t.th), t.ht);
    portal_mod_tree_set(t.rid, xform_at(t, t.th), 1);
    if (t.pivot.distance_to(Device.vCameraPosition) < C.max_dist && t.w > 0.f)
        crush(t, id);
    if (best > 1.f)
        return th1 <= 0.f && t.w < 0.f ? (t.w = 0.f, true) : false;
    // it meets the ground, a wall or another tree
    const float speed = _abs(t.w) * t.ht;
    if (t.bounces == 0)
    {
        make_sounds();
        const float vol = clampr(t.mass / 250.f, 0.5f, 2.f);
        play_snd(S.crash, hit_at, vol);
        play_snd(S.thud, hit_at, vol);
        Fvector crown;
        crown.mad(t.pivot, up_at(t, t.th), _max(t.ht, 0.7f * t.h));
        play_fx(C.fx_leaves, crown, Fvector{0.f, 1.f, 0.f});
        Fvector mid;
        mid.mad(t.pivot, up_at(t, t.th), t.ht * 0.5f);
        play_fx(C.fx_crash, mid, Fvector{0.f, 1.f, 0.f});
        Msg("* [trees] tree %u lands at %.0f deg, its top at %.1f m/s, %.1f s after it began", id, rad2deg(t.th), speed,
            (Device.dwTimeGlobal - t.t_fell) / 1000.f);
    }
    if (t.bounces < 1 && t.w > 0.8f && t.th > 0.5f)
    {
        ++t.bounces;
        t.w = -C.bounce * t.w;
        return false;
    }
    (void)th_prev;
    t.w = 0.f;
    return true;
}

void lie_down(Tree& t, u16 id)
{
    t.state = ST_LYING;
    t.w = 0.f;
    make_shell(t);
    if (C.debug)
        Msg("[trees] tree %u lies at %.0f deg", id, rad2deg(t.th));
}

void restore_all()
{
    for (Tree& t : S.trees)
    {
        if (t.state == ST_STANDING && t.saved.empty())
            continue;
        kill_shell(t);
        restore_tris(t);
        portal_mod_tree_set(t.rid, t.xf0, 0);
        t.state = ST_STANDING;
        t.dmg = 0.f;
        t.push.set(0.f, 0.f, 0.f);
        t.creaked = false;
        t.th = t.w = 0.f;
    }
    S.active.clear();
}

void stash()
{
    if (!S.built || S.gen != G.gen || !S.level.size())
        return;
    xr_vector<Saved>& list = G.levels[S.level];
    list.clear();
    for (const Tree& t : S.trees)
        if (t.state != ST_STANDING)
            list.push_back(Saved{t.rid, t.state, t.th, t.w, t.dir.x, t.dir.z});
}

void apply_saved(const xr_vector<Saved>& list)
{
    for (const Saved& s : list)
    {
        auto it = std::find_if(S.trees.begin(), S.trees.end(), [&s](const Tree& t) { return t.rid == s.rid; });
        if (it == S.trees.end() || it->r <= 0.f)
            continue;
        Tree& t = *it;
        const u16 id = u16(it - S.trees.begin());
        t.dir.set(s.dx, 0.f, s.dz);
        t.dir.normalize_safe();
        t.th = s.th;
        t.w = s.w;
        t.state = s.state;
        t.dmg = t.hp;
        drop_tris(t);
        portal_mod_tree_set(t.rid, xform_at(t, t.th), 1);
        if (t.state == ST_FALLING)
            S.active.push_back(id);
        else
            make_shell(t);
    }
}
} // namespace

void bullet_hit(const Fvector& point, const Fvector& dir, u32 tri, float dmg)
{
    load_cfg();
    if (!S.built || !on() || dmg <= 0.f)
        return;
    const int id = tree_of_tri(tri);
    if (id < 0)
        return;
    Tree& t = S.trees[id];
    ++S.shots;
    const float k = point.y - t.pivot.y < C.cut_h ? 1.f : C.high_k;
    damage(u16(id), dmg * k, dir, C.w0_shot, "rounds");
}

void explosion(const Fvector& pos, float radius, float hit, float impulse)
{
    load_cfg();
    if (!S.built || !on() || radius <= 0.f)
        return;
    for (u32 i = 0; i < S.trees.size(); ++i)
    {
        Tree& t = S.trees[i];
        if (t.state != ST_STANDING || t.r <= 0.f)
            continue;
        // the nearest point of the trunk's axis
        const float s = clampr(pos.y - t.pivot.y, 0.f, t.ht);
        Fvector q{t.pivot.x, t.pivot.y + s, t.pivot.z};
        const float d = _max(q.distance_to(pos) - t.r, 0.f);
        if (d >= radius)
            continue;
        // what stands between: rays to the trunk at the blast's height and a metre over the root, stopping short of the bark
        int seen = 0;
        const Fvector pts[2] = {q, Fvector{t.pivot.x, t.pivot.y + _min(1.f, t.ht), t.pivot.z}};
        for (const Fvector& p : pts)
        {
            Fvector dv;
            dv.sub(p, pos);
            const float len = dv.magnitude() - t.r - 0.15f;
            if (len < 0.1f)
            {
                ++seen;
                continue;
            }
            dv.normalize();
            float dist;
            seen += !fall_ray(t, pos, dv, len, dist); // through crowns' bush hulls and its own loose branches, as the fall's rays
        }
        const float vis = seen ? seen / 2.f : C.blast_cover;
        if (vis <= 0.f)
            continue;
        const float fall = std::pow(1.f - d / radius, C.blast_pow);
        const float dmg = C.blast_k * hit * fall * vis;
        Fvector away;
        away.sub(t.pivot, pos);
        damage(u16(i), dmg, away, C.w0_blast * fall * clampr(impulse / 200.f, 0.5f, 2.f), "a blast");
    }
}

void update()
{
    load_cfg();
    if (!g_pGameLevel || !Level().ObjectSpace.GetStaticModel())
        return;
    if (!S.built)
    {
        if (!on() || !portal_mod_tree_count())
            return;
        build();
    }
    if (S.need_restore)
    {
        S.need_restore = false;
        restore_all();
        S.gen = G.gen;
        auto it = G.levels.find(S.level);
        if (it != G.levels.end())
            apply_saved(it->second);
    }
    if (S.active.empty())
        return;
    const float dt = clampr(Device.fTimeDelta, 0.f, 0.05f);
    if (dt <= 0.f)
        return;
    for (u32 k = 0; k < S.active.size();)
    {
        const u16 id = S.active[k];
        Tree& t = S.trees[id];
        if (t.state == ST_FALLING && fall_step(t, id, dt))
        {
            lie_down(t, id);
            S.active.erase(S.active.begin() + k);
            continue;
        }
        ++k;
    }
}

void reset()
{
    stash();
    for (Tree& t : S.trees)
        kill_shell(t);
    for (int i = 0; i < 4; ++i)
    {
        xr_vector<ref_sound>& v = i == 0 ? S.creak : i == 1 ? S.crack : i == 2 ? S.crash : S.thud;
        v.clear();
    }
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
    xr_sprintf(fn, "%s.trees", sav_path);
    u32 total = 0;
    for (const auto& [lvl, list] : G.levels)
        total += (u32)list.size();
    if (!total)
    {
        if (FS.exist(fn))
            FS.file_delete(fn);
        return;
    }
    IWriter* w = FS.w_open(fn);
    if (!w)
    {
        Msg("! [trees] cannot write %s", fn);
        return;
    }
    w->w("PMTR", 4);
    w->w_u32(1);
    w->w_u32((u32)G.levels.size());
    for (const auto& [lvl, list] : G.levels)
    {
        w->w_stringZ(lvl.c_str());
        w->w_u32((u32)list.size());
        for (const Saved& s : list)
        {
            w->w_u32(s.rid);
            w->w_u8(s.state);
            w->w_float(s.th);
            w->w_float(s.w);
            w->w_float(s.dx);
            w->w_float(s.dz);
        }
    }
    FS.w_close(w);
    Msg("* [trees] %u fallen tree(s) saved to %s", total, fn);
}

void load_game(LPCSTR sav_path)
{
    ++G.gen;
    G.levels.clear();
    if (S.built)
        S.need_restore = true; // a save loaded on this level: its trees stand up again, then the save's fall
    if (!sav_path || !*sav_path)
        return;
    string_path fn;
    xr_sprintf(fn, "%s.trees", sav_path);
    IReader* r = FS.r_open(fn);
    if (!r)
        return;
    char magic[4]{};
    if (r->length() >= 12)
        r->r(magic, 4);
    u32 total = 0;
    if (strncmp(magic, "PMTR", 4) == 0 && r->r_u32() == 1)
        for (u32 n = r->r_u32(); n && r->elapsed() > 0; --n)
        {
            shared_str lvl;
            r->r_stringZ(lvl);
            xr_vector<Saved>& list = G.levels[lvl];
            for (u32 m = r->r_u32(); m && r->elapsed() >= 21; --m)
            {
                Saved s;
                s.rid = r->r_u32();
                s.state = r->r_u8();
                s.th = r->r_float();
                s.w = r->r_float();
                s.dx = r->r_float();
                s.dz = r->r_float();
                list.push_back(s);
                ++total;
            }
        }
    FS.r_close(r);
    Msg("* [trees] %u fallen tree(s) loaded from %s", total, fn);
}

LPCSTR lua_info()
{
    static string512 buf;
    u32 can = 0, lying = 0;
    for (const Tree& t : S.trees)
        can += t.r > 0.f, lying += t.state == ST_LYING;
    xr_sprintf(buf, "trees=%u can_fall=%u falling=%u lying=%u fallen=%u crushed=%u shots=%u enabled=%d", (u32)S.trees.size(), can,
        (u32)S.active.size(), lying, S.fallen, S.crushed, S.shots, on() ? 1 : 0);
    return buf;
}

LPCSTR lua_near(const Fvector& pos, float radius)
{
    static xr_string out;
    out.clear();
    xr_vector<std::pair<float, u32>> near_t;
    for (u32 i = 0; i < S.trees.size(); ++i)
    {
        const float d = S.trees[i].pivot.distance_to_xz(pos);
        if (d <= radius)
            near_t.emplace_back(d, i);
    }
    std::sort(near_t.begin(), near_t.end());
    string256 one;
    for (u32 k = 0; k < near_t.size() && k < 64; ++k)
    {
        const Tree& t = S.trees[near_t[k].second];
        xr_sprintf(one, "%u|%.2f|%.2f|%.2f|%.1f|%.2f|%.0f|%.0f|%d|%.1f;", near_t[k].second, t.pivot.x, t.pivot.y, t.pivot.z, t.h, t.r, t.hp, t.dmg,
            (int)t.state, near_t[k].first);
        out += one;
    }
    return out.c_str();
}

bool lua_fell(int id, const Fvector& dir)
{
    load_cfg();
    if (!S.built || id < 0 || id >= (int)S.trees.size() || S.trees[id].state != ST_STANDING || S.trees[id].r <= 0.f)
        return false;
    S.trees[id].dmg = S.trees[id].hp;
    start_fall(u16(id), dir, C.w0_shot, "a script");
    return true;
}

void lua_debug(bool on_)
{
    load_cfg();
    C.debug = on_;
}
} // namespace portal_mod_trees
