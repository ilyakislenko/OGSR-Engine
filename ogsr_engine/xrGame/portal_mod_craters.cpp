// portal-mod E60: craters in the ground (see portal_mod_craters.h).
//
// The pool: at load (portal_mod_destruct_cform's last step) pool_tris triangles and three vertices for each are appended
// to the static model, the triangles on the sink vertices far under the level (g_pm_cform_sink). A crater of radius r,
// depth D at c: the ground's triangles (by material, facing up) in the square of half side Rb + 0.12 round it (Rb =
// rim_k x r: where its bowl meets the ground) leave (to the sink), the parts of them outside the square come back as
// pool triangles (clipped by the square's four sides), and a grid over the square takes its place: each point on the
// ground as it was there (the triangles it lay in) less D (1 - (rho / Rb)^2) inside Rb; a skirt hangs from the grid's
// rim under the ground (the old ground's creases between the grid's points would leave a hairline). The model's tree is
// refitted (CDB::MODEL::refit, E60: the pool's nodes fitted again, the level's own only ever grow, so a triangle sunk and
// brought back later - a fallen tree's - is still found) and the bodies round it woken. What was changed is kept (the
// first original of each triangle), so a save loaded on the level puts the ground back and digs the save's craters again.
// E62: a breach - a blast at a wall (portal_mod_destruct) - cuts a square out of the wall's faces the same way: the
// triangles parallel to the wall within its thickness go to the sink, their parts outside the square come back from the
// pool; no bowl (the hole is open, its sides not closed: the walker is wider than the wall is thick). Its look is the destruction's (portal_mod_destruct: an E56 dent of
// fresh earth, the ground cut away inside a ragged edge and a bowl drawn under it, its profile the same as here).
#include "stdafx.h"
#include <algorithm>
#include "portal_mod_craters.h"
#include "portal_mod_features.h"
#include "portal_mod_trees.h" // g_pm_cform_sink
#include "portal_mod_statics.h" // E62: a pole's, fence's, pane's triangles are left to them
#include "Level.h"
#include "PhysicsShellHolder.h"
#include "PhysicsShell.h"
#include "../xr_3da/GameMtlLib.h"

namespace portal_mod_craters
{
namespace
{
constexpr u32 SAVE_VERSION = 2; // E62: 2 - each a kind (a crater, a breach with its wall's normal)

struct Cfg
{
    bool enabled{true}, loaded{};
    float k_r{0.42f}, r_min{0.25f}, r_max{1.8f}, depth_k{0.4f}, reach{1.6f}, rim_k{1.05f}, steep{0.6f};
    float pit_r_min{0.16f}, pit_grow{1.25f}, pit_gap{0.3f}, skirt{0.25f}, max_depth{1.6f};
    int pool{49152}, max_craters{160};
};

struct Crater
{
    Fvector c;
    float r, depth;
    u8 kind{0}; // E62: 0 a crater or a pit; 1 a breach (c the middle of the wall, r the cut's half side, depth the wall's thickness)
    Fvector n{}; // ... the wall's normal (level)
};

struct State
{
    bool loaded{}, applied{}, need_restore{};
    u32 gen{};
    shared_str level;
    u32 v0{}, t0{}, cap{}, used{}; // the pool: its first vertex, first triangle, triangles, used so far
    xr_vector<std::pair<u32, CDB::TRI>> undo; // the level's own triangles changed: each one's original, once
    xr_vector<u8> touched; // per triangle of the level (below t0): in undo already
    xr_vector<Crater> list; // dug on this level, in order
    xr_vector<Crater> pits; // pits waiting (one a pit_gap s)
    u32 pit_next{}, dug{}, full{}, cuts{}; // E62: cuts, the breaches
    float ms{};
};

struct Global
{
    u32 gen{1};
    xr_map<shared_str, xr_vector<Crater>> levels;
};

Cfg C;
State S;
Global G;
xr_vector<u8> g_ground; // per material: 1 soil (earth, grass, dirt, sand, gravel), 0 not

float cfg_f(LPCSTR s, LPCSTR k, float def) { return pSettings->line_exist(s, k) ? pSettings->r_float(s, k) : def; }

void load_cfg()
{
    if (C.loaded)
        return;
    C.loaded = true;
    LPCSTR s = "pm_craters";
    if (!pSettings->section_exist(s))
        return;
    C.enabled = pSettings->line_exist(s, "enabled") ? !!pSettings->r_bool(s, "enabled") : C.enabled;
    C.k_r = cfg_f(s, "radius_k", C.k_r);
    C.r_min = cfg_f(s, "radius_min", C.r_min);
    C.r_max = cfg_f(s, "radius_max", C.r_max);
    C.depth_k = cfg_f(s, "depth_k", C.depth_k);
    C.reach = cfg_f(s, "reach", C.reach);
    C.rim_k = cfg_f(s, "rim_k", C.rim_k);
    C.steep = cfg_f(s, "steep", C.steep);
    C.pit_r_min = cfg_f(s, "pit_radius_min", C.pit_r_min);
    C.pit_grow = cfg_f(s, "pit_grow", C.pit_grow);
    C.pit_gap = cfg_f(s, "pit_gap", C.pit_gap);
    C.skirt = cfg_f(s, "skirt", C.skirt);
    C.max_depth = cfg_f(s, "max_depth", C.max_depth);
    C.pool = (int)cfg_f(s, "pool_tris", (float)C.pool);
    C.max_craters = (int)cfg_f(s, "max_craters", (float)C.max_craters);
}

bool on() { return C.enabled && portal_mod_feat::enabled(portal_mod_feat::destruct); }

u8 ground_class(u16 mtl)
{
    if (g_ground.empty())
    {
        g_ground.assign(GMLib.CountMaterial(), 0);
        u16 i = 0;
        for (auto it = GMLib.FirstMaterial(); it != GMLib.LastMaterial(); ++it, ++i)
        {
            const xr_string n = (*it)->m_Name.c_str();
            if (n.find("death") != xr_string::npos)
                continue;
            if (n.find("earth") != xr_string::npos || n.find("grass") != xr_string::npos || n.find("dirt") != xr_string::npos ||
                n.find("sand") != xr_string::npos || n.find("gravel") != xr_string::npos)
                g_ground[i] = 1;
        }
    }
    return mtl < g_ground.size() ? g_ground[mtl] : 0;
}

bool sunk(const CDB::TRI& t) { return t.verts[0] == g_pm_cform_sink; }

float profile(float rho, float Rb) { return rho < Rb ? 1.f - (rho / Rb) * (rho / Rb) : 0.f; }

// the part of a convex polygon (vertical planes: a . (x, z) <= b kept)
void clip_xz(xr_vector<Fvector>& poly, float ax, float az, float b)
{
    static xr_vector<Fvector> out;
    out.clear();
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i)
    {
        const Fvector& p = poly[i];
        const Fvector& q = poly[(i + 1) % n];
        const float sp = ax * p.x + az * p.z - b, sq = ax * q.x + az * q.z - b;
        if (sp <= 0.f)
            out.push_back(p);
        if ((sp < 0.f && sq > 0.f) || (sp > 0.f && sq < 0.f))
        {
            Fvector m;
            m.lerp(p, q, sp / (sp - sq));
            out.push_back(m);
        }
    }
    poly.swap(out);
}

// E62: the part of a convex polygon where a . p <= b
void clip3(xr_vector<Fvector>& poly, const Fvector& a, float b)
{
    static xr_vector<Fvector> out;
    out.clear();
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i)
    {
        const Fvector& p = poly[i];
        const Fvector& q = poly[(i + 1) % n];
        const float sp = a.dotproduct(p) - b, sq = a.dotproduct(q) - b;
        if (sp <= 0.f)
            out.push_back(p);
        if ((sp < 0.f && sq > 0.f) || (sp > 0.f && sq < 0.f))
        {
            Fvector m;
            m.lerp(p, q, sp / (sp - sq));
            out.push_back(m);
        }
    }
    poly.swap(out);
}

void keep_undo(u32 i, const CDB::TRI& t)
{
    if (i >= S.t0 || i >= S.touched.size() || S.touched[i])
        return;
    S.touched[i] = 1;
    S.undo.emplace_back(i, t);
}

// the bodies round it wake (they lay on the ground that went)
void wake(const Fvector& c, float R)
{
    xr_vector<CObject*> near_;
    Level().ObjectSpace.GetNearest(near_, c, R, nullptr);
    for (CObject* o : near_)
        if (auto* h = smart_cast<CPhysicsShellHolder*>(o))
            if (CPhysicsShell* sh = h->PPhysicsShell())
                if (sh->isActive() && !sh->isEnabled())
                    sh->Enable();
}

// dig a crater into the collision: false when the pool has no room (or there is no ground there)
bool dig(const Crater& k)
{
    if (!S.loaded || g_pm_cform_sink == u32(-1) || !g_pGameLevel)
        return false;
    CDB::MODEL* M = Level().ObjectSpace.GetStaticModel();
    if (!M || M->get_tris_count() < S.t0 + S.cap)
        return false;
    CTimer tm;
    tm.Start();
    CDB::TRI* T = M->get_tris();
    Fvector* V = M->get_verts();
    const float Rb = k.r * C.rim_k, Hs = Rb + 0.12f;
    const int n = clampr(iCeil(2.f * Hs / clampr(Rb / 5.f, 0.08f, 0.3f)), 4, 24);
    const float g = 2.f * Hs / float(n);
    const float x0 = k.c.x - Hs, x1 = k.c.x + Hs, z0 = k.c.z - Hs, z1 = k.c.z + Hs;
    // the ground in the square (as far up or down as a slope a crater digs into rises over it)
    const float Hy = 0.5f + Hs;
    static CDB::COLLIDER xrc;
    xrc.box_query(0, M, k.c, Fvector{Hs, Hy, Hs});
    static xr_vector<u32> ground, skirts;
    ground.clear();
    skirts.clear();
    float up_sign = 0.f;
    for (const CDB::RESULT* rp = xrc.r_begin(); rp != xrc.r_end(); ++rp)
    {
        const u32 i = (u32)rp->id;
        const CDB::TRI& t = T[i];
        if (sunk(t))
            continue;
        Fvector nn;
        nn.mknormal_non_normalized(V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]);
        const float len = nn.magnitude();
        if (len < 1e-9f)
            continue;
        const float ny = nn.y / len;
        const bool pool = i >= S.t0 && i < S.t0 + S.cap;
        if (pool && _abs(ny) < 0.02f)
        {
            skirts.push_back(i); // an earlier crater's skirt
            continue;
        }
        if (!ground_class(u16(t.material)) || _abs(ny) < (pool ? 0.02f : 0.35f))
            continue;
        ground.push_back(i);
        if (up_sign == 0.f)
            up_sign = ny > 0.f ? 1.f : -1.f;
    }
    if (ground.empty())
        return false;
    // the ground's height at the grid's points (the triangle each lies in, the one nearest the crater's height)
    static xr_vector<float> H;
    static xr_vector<u32> Ht;
    static xr_vector<u8> got;
    H.assign((n + 1) * (n + 1), k.c.y);
    Ht.assign((n + 1) * (n + 1), ground[0]);
    got.assign((n + 1) * (n + 1), 0);
    for (int a = 0; a <= n; ++a)
        for (int b = 0; b <= n; ++b)
        {
            const float x = x0 + a * g, z = z0 + b * g;
            float best = flt_max;
            for (u32 i : ground)
            {
                const CDB::TRI& t = T[i];
                const Fvector& A = V[t.verts[0]];
                const Fvector& B = V[t.verts[1]];
                const Fvector& Cc = V[t.verts[2]];
                const float d = (B.z - Cc.z) * (A.x - Cc.x) + (Cc.x - B.x) * (A.z - Cc.z);
                if (_abs(d) < 1e-9f)
                    continue;
                const float l1 = ((B.z - Cc.z) * (x - Cc.x) + (Cc.x - B.x) * (z - Cc.z)) / d;
                const float l2 = ((Cc.z - A.z) * (x - Cc.x) + (A.x - Cc.x) * (z - Cc.z)) / d;
                const float l3 = 1.f - l1 - l2;
                if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f)
                    continue;
                const float y = l1 * A.y + l2 * B.y + l3 * Cc.y;
                if (_abs(y - k.c.y) < best && _abs(y - k.c.y) <= Hy)
                {
                    best = _abs(y - k.c.y);
                    H[a * (n + 1) + b] = y;
                    Ht[a * (n + 1) + b] = i;
                    got[a * (n + 1) + b] = 1;
                }
            }
        }
    // a point over no ground (a wall's foot, a slab): the ground's height next to it (else the crater's)
    for (int pass = 0; pass < n; ++pass)
    {
        bool left = false;
        for (int a = 0; a <= n; ++a)
            for (int b = 0; b <= n; ++b)
            {
                const int q = a * (n + 1) + b;
                if (got[q])
                    continue;
                float sum = 0.f;
                int cnt = 0;
                const int nb[4][2] = {{a - 1, b}, {a + 1, b}, {a, b - 1}, {a, b + 1}};
                for (const auto& e : nb)
                    if (e[0] >= 0 && e[0] <= n && e[1] >= 0 && e[1] <= n && got[e[0] * (n + 1) + e[1]] == 1)
                    {
                        sum += H[e[0] * (n + 1) + e[1]];
                        Ht[q] = Ht[e[0] * (n + 1) + e[1]];
                        ++cnt;
                    }
                if (cnt)
                    H[q] = sum / float(cnt), got[q] = 2;
                else
                    left = true;
            }
        for (u8& g2 : got)
            g2 = g2 ? 1 : 0;
        if (!left)
            break;
    }
    // room in the pool: the ground's parts outside the square (4 a triangle at most), the grid, the skirt both ways
    const u32 need = (u32)ground.size() * 4 * 3 + u32(2 * n * n) + u32(16 * n);
    if (S.used + need > S.cap)
    {
        ++S.full;
        return false;
    }
    auto put = [&](const Fvector& a, const Fvector& b, const Fvector& c, const CDB::TRI& like, bool face_up) {
        if (S.used >= S.cap)
            return;
        Fvector nn;
        nn.mknormal_non_normalized(a, b, c);
        if (nn.square_magnitude() < 1e-12f)
            return;
        const u32 ti = S.t0 + S.used, vi = S.v0 + 3 * S.used;
        ++S.used;
        V[vi] = a;
        // the level's winding: its ground faces up_sign
        if (face_up && (nn.y > 0.f) != (up_sign > 0.f))
            V[vi + 1] = c, V[vi + 2] = b;
        else
            V[vi + 1] = b, V[vi + 2] = c;
        CDB::TRI t = like;
        t.verts[0] = vi, t.verts[1] = vi + 1, t.verts[2] = vi + 2;
        T[ti] = t;
    };
    // the ground in the square goes; its parts outside come back
    static xr_vector<Fvector> poly, piece;
    for (u32 i : ground)
    {
        const CDB::TRI t = T[i];
        Fvector P[3] = {V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]};
        keep_undo(i, t);
        T[i].verts[0] = g_pm_cform_sink, T[i].verts[1] = g_pm_cform_sink + 1, T[i].verts[2] = g_pm_cform_sink + 2;
        bool inside = true;
        for (const Fvector& p : P)
            inside = inside && p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1;
        if (inside)
            continue;
        // outside the square: x < x0 | x > x1 | (x0..x1, z < z0) | (x0..x1, z > z1)
        const float planes[4][3][3] = {
            {{1.f, 0.f, x0}, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}},
            {{-1.f, 0.f, -x1}, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}},
            {{-1.f, 0.f, -x0}, {1.f, 0.f, x1}, {0.f, 1.f, z0}},
            {{-1.f, 0.f, -x0}, {1.f, 0.f, x1}, {0.f, -1.f, -z1}},
        };
        for (const auto& reg : planes)
        {
            piece.assign(P, P + 3);
            for (const auto& pl : reg)
                if (pl[0] != 0.f || pl[1] != 0.f)
                    clip_xz(piece, pl[0], pl[1], pl[2]);
            for (size_t m = 1; m + 1 < piece.size(); ++m)
                put(piece[0], piece[m], piece[m + 1], t, false); // (the clipped parts keep the triangle's winding)
        }
    }
    for (u32 i : skirts)
        T[i].verts[0] = g_pm_cform_sink, T[i].verts[1] = g_pm_cform_sink + 1, T[i].verts[2] = g_pm_cform_sink + 2;
    // the bowl
    auto pt = [&](int a, int b) {
        const float x = x0 + a * g, z = z0 + b * g;
        const float rho = _sqrt((x - k.c.x) * (x - k.c.x) + (z - k.c.z) * (z - k.c.z));
        return Fvector{x, H[a * (n + 1) + b] - k.depth * profile(rho, Rb), z};
    };
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < n; ++b)
        {
            const CDB::TRI& like = T[Ht[a * (n + 1) + b]];
            CDB::TRI t = like;
            t.suppress_wm = 0;
            const Fvector p00 = pt(a, b), p10 = pt(a + 1, b), p11 = pt(a + 1, b + 1), p01 = pt(a, b + 1);
            put(p00, p10, p11, t, true);
            put(p00, p11, p01, t, true);
        }
    // the skirt under the grid's rim, both ways
    auto skirt = [&](const Fvector& p, const Fvector& q, const CDB::TRI& like) {
        const Fvector pd{p.x, p.y - C.skirt, p.z}, qd{q.x, q.y - C.skirt, q.z};
        put(p, q, qd, like, false);
        put(p, qd, pd, like, false);
        put(p, qd, q, like, false);
        put(p, pd, qd, like, false);
    };
    for (int s = 0; s < n; ++s)
    {
        skirt(pt(s, 0), pt(s + 1, 0), T[Ht[s * (n + 1)]]);
        skirt(pt(s, n), pt(s + 1, n), T[Ht[s * (n + 1) + n]]);
        skirt(pt(0, s), pt(0, s + 1), T[Ht[s]]);
        skirt(pt(n, s), pt(n, s + 1), T[Ht[n * (n + 1) + s]]);
    }
    M->refit(S.t0, g_pm_cform_sink); // portal-mod E60 (xrCDB)
    wake(k.c, Hs + 1.5f);
    ++S.dug;
    S.ms = tm.GetElapsed_sec() * 1000.f;
    return true;
}

// E62: a breach - the wall's faces (parallel to it, within its thickness) cut out in the square of half side k.r round
// k.c in the wall's plane; their parts outside come back from the pool
bool cut(const Crater& k)
{
    if (!S.loaded || g_pm_cform_sink == u32(-1) || !g_pGameLevel)
        return false;
    CDB::MODEL* M = Level().ObjectSpace.GetStaticModel();
    if (!M || M->get_tris_count() < S.t0 + S.cap)
        return false;
    CTimer tm;
    tm.Start();
    CDB::TRI* T = M->get_tris();
    Fvector* V = M->get_verts();
    Fvector n{k.n.x, 0.f, k.n.z};
    if (n.square_magnitude() < 1e-6f)
        return false;
    n.normalize();
    const Fvector u{-n.z, 0.f, n.x}, v{0.f, 1.f, 0.f};
    const float sq = k.r, half = k.depth * 0.5f + 0.08f;
    static CDB::COLLIDER xrc;
    xrc.box_query(0, M, k.c, Fvector{sq + half, sq + 0.05f, sq + half});
    static xr_vector<u32> wall;
    wall.clear();
    for (const CDB::RESULT* rp = xrc.r_begin(); rp != xrc.r_end(); ++rp)
    {
        const u32 i = (u32)rp->id;
        const CDB::TRI& t = T[i];
        if (sunk(t) || (i < S.t0 && portal_mod_statics::owns(i)))
            continue;
        const Fvector P[3] = {V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]};
        Fvector tn;
        tn.mknormal_non_normalized(P[0], P[1], P[2]);
        const float len = tn.magnitude();
        if (len < 1e-9f || _abs(tn.dotproduct(n)) < 0.5f * len)
            continue; // not a face of the wall (a floor, the wall's end)
        float u0 = flt_max, u1 = -flt_max, v0 = flt_max, v1 = -flt_max;
        bool slab = true;
        for (const Fvector& q : P)
        {
            Fvector d;
            d.sub(q, k.c);
            slab = slab && _abs(d.dotproduct(n)) <= half;
            const float du = d.dotproduct(u), dv = d.dotproduct(v);
            u0 = _min(u0, du), u1 = _max(u1, du), v0 = _min(v0, dv), v1 = _max(v1, dv);
        }
        if (!slab || u1 < -sq || u0 > sq || v1 < -sq || v0 > sq)
            continue;
        wall.push_back(i);
    }
    if (wall.empty())
        return false;
    if (S.used + (u32)wall.size() * 4 * 3 > S.cap)
    {
        ++S.full;
        return false;
    }
    auto put = [&](const Fvector& a, const Fvector& b, const Fvector& c, const CDB::TRI& like) {
        if (S.used >= S.cap)
            return;
        Fvector nn;
        nn.mknormal_non_normalized(a, b, c);
        if (nn.square_magnitude() < 1e-12f)
            return;
        const u32 ti = S.t0 + S.used, vi = S.v0 + 3 * S.used;
        ++S.used;
        V[vi] = a, V[vi + 1] = b, V[vi + 2] = c;
        CDB::TRI t = like;
        t.verts[0] = vi, t.verts[1] = vi + 1, t.verts[2] = vi + 2;
        T[ti] = t;
    };
    const float cu = u.dotproduct(k.c), cv = v.dotproduct(k.c);
    const Fvector mu{-u.x, -u.y, -u.z}, mv{-v.x, -v.y, -v.z};
    // outside the square: u < -sq | u > sq | (-sq..sq, v < -sq) | (-sq..sq, v > sq)
    struct Pl
    {
        Fvector a;
        float b;
    };
    const Pl regs[4][3] = {
        {{u, cu - sq}, {u, 1e9f}, {u, 1e9f}},
        {{mu, -(cu + sq)}, {u, 1e9f}, {u, 1e9f}},
        {{mu, -(cu - sq)}, {u, cu + sq}, {v, cv - sq}},
        {{mu, -(cu - sq)}, {u, cu + sq}, {mv, -(cv + sq)}},
    };
    static xr_vector<Fvector> piece;
    for (u32 i : wall)
    {
        const CDB::TRI t = T[i];
        const Fvector P[3] = {V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]};
        keep_undo(i, t);
        T[i].verts[0] = g_pm_cform_sink, T[i].verts[1] = g_pm_cform_sink + 1, T[i].verts[2] = g_pm_cform_sink + 2;
        bool inside = true;
        for (const Fvector& q : P)
        {
            Fvector d;
            d.sub(q, k.c);
            inside = inside && _abs(d.dotproduct(u)) <= sq && _abs(d.dotproduct(v)) <= sq;
        }
        if (inside)
            continue;
        for (const auto& reg : regs)
        {
            piece.assign(P, P + 3);
            for (const Pl& pl : reg)
                if (pl.b < 1e8f)
                    clip3(piece, pl.a, pl.b);
            for (size_t m = 1; m + 1 < piece.size(); ++m)
                put(piece[0], piece[m], piece[m + 1], t); // (the triangle's winding kept)
        }
    }
    M->refit(S.t0, g_pm_cform_sink); // portal-mod E60 (xrCDB)
    wake(k.c, sq + 1.5f);
    ++S.dug;
    ++S.cuts;
    S.ms = tm.GetElapsed_sec() * 1000.f;
    return true;
}

bool make(const Crater& k) { return k.kind == 1 ? cut(k) : dig(k); } // E62

// everything dug on this level undone: the level's triangles back, the pool empty
void undo_all()
{
    if (!g_pGameLevel || (S.undo.empty() && !S.used))
        return;
    CDB::MODEL* M = Level().ObjectSpace.GetStaticModel();
    if (!M)
        return;
    CDB::TRI* T = M->get_tris();
    for (auto it = S.undo.rbegin(); it != S.undo.rend(); ++it)
        T[it->first] = it->second;
    for (u32 k = 0; k < S.used; ++k)
    {
        CDB::TRI& t = T[S.t0 + k];
        t.verts[0] = g_pm_cform_sink, t.verts[1] = g_pm_cform_sink + 1, t.verts[2] = g_pm_cform_sink + 2;
    }
    S.undo.clear();
    std::fill(S.touched.begin(), S.touched.end(), u8(0));
    S.used = 0;
    S.list.clear();
    M->refit(S.t0, g_pm_cform_sink);
}

void stash()
{
    if (!S.loaded || S.gen != G.gen || !S.level.size())
        return;
    if (S.list.empty())
        G.levels.erase(S.level);
    else
        G.levels[S.level] = S.list;
}

void apply_saved()
{
    auto it = G.levels.find(S.level);
    if (it == G.levels.end())
        return;
    u32 n = 0;
    for (const Crater& k : it->second)
        if (make(k)) // E62: a crater or a breach
        {
            S.list.push_back(k);
            ++n;
        }
    if (n)
        Msg("* [craters] %s: %u crater(s) dug again from the saved state", S.level.c_str(), n);
}
} // namespace

bool is_ground(u16 mtl) { return ground_class(mtl) != 0; }
bool is_soil(u16 mtl) { return ground_class(mtl) == 1; }
float depth_at(const Fvector& pos) { return lua_depth_at(pos); }

void append_pool(xr_vector<Fvector>& out_v, xr_vector<CDB::TRI>& out_t)
{
    reset();
    load_cfg();
    if (!C.enabled || g_pm_cform_sink == u32(-1) || C.pool <= 0 || out_t.empty())
        return;
    S.v0 = (u32)out_v.size();
    S.t0 = (u32)out_t.size();
    S.cap = (u32)C.pool;
    out_v.resize(out_v.size() + S.cap * 3, Fvector{0.f, -9000.f, 0.f});
    CDB::TRI t = out_t[0];
    t.verts[0] = g_pm_cform_sink, t.verts[1] = g_pm_cform_sink + 1, t.verts[2] = g_pm_cform_sink + 2;
    out_t.resize(out_t.size() + S.cap, t);
    S.touched.assign(S.t0, 0);
    S.loaded = true;
    Msg("* [craters] a pool of %u collision triangles for craters", S.cap);
}

bool blast(const Fvector& pos, float radius, float hit, Fvector& c, Fvector& n, float& r, float& depth)
{
    load_cfg();
    if (!S.loaded || !on() || hit <= 0.f || !g_pGameLevel)
        return false;
    collide::rq_result R;
    const Fvector down{0.f, -1.f, 0.f};
    if (!Level().ObjectSpace.RayPick(pos, down, C.reach, collide::rqtStatic, R, nullptr) || R.element < 0)
        return false;
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    const Fvector* V = Level().ObjectSpace.GetStaticVerts();
    const CDB::TRI& t = T[R.element];
    if (!is_ground(u16(t.material)))
        return false;
    n.mknormal(V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]);
    if (n.y < 0.f)
        n.invert();
    if (n.y < C.steep)
        return false;
    const float h0 = R.range;
    r = _min(C.r_max, C.k_r * _sqrt(hit) * _sqrt(_max(0.f, 1.f - h0 / C.reach)));
    if (r < C.r_min)
        return false;
    c.mad(pos, down, h0);
    // a wall, a slab, a trunk round it: the crater stops short of it (its bowl would be drawn into its foot)
    for (int a = 0; a < 8; ++a)
    {
        const float an = float(a) * PI_DIV_4;
        const Fvector u{_cos(an), 0.f, _sin(an)};
        const Fvector s0{c.x, c.y + 0.15f + 0.1f * r, c.z};
        collide::rq_result W;
        if (!Level().ObjectSpace.RayPick(s0, u, r * 1.4f, collide::rqtStatic, W, nullptr) || W.element < 0)
            continue;
        const CDB::TRI& w = T[W.element];
        Fvector wn;
        wn.mknormal(V[w.verts[0]], V[w.verts[1]], V[w.verts[2]]);
        if (is_ground(u16(w.material)) || _abs(wn.y) > 0.6f || GMLib.GetMaterialByIdx(u16(w.material))->Flags.test(SGameMtl::flPassable))
            continue; // the ground rising, a road, a bush
        r = _min(r, W.range / 1.4f);
    }
    if (r < C.r_min)
        return false;
    depth = r * C.depth_k;
    // ground dug before goes no deeper than max_depth
    const float had = lua_depth_at(c);
    const float dig_depth = _min(depth, C.max_depth - had);
    if ((int)S.list.size() >= C.max_craters || dig_depth < 0.02f)
        return true; // drawn, not dug
    const Crater k{c, r, dig_depth};
    if (dig(k))
    {
        S.list.push_back(k);
        Msg("* [craters] a blast (%.1f) %.2f m over %s dug a crater %.2f m wide, %.2f m deep at [%.2f, %.2f, %.2f] (%.1f ms; pool %u of %u)", hit, h0,
            GMLib.GetMaterialByIdx(u16(t.material))->m_Name.c_str(), 2.f * r, k.depth, VPUSH(c), S.ms, S.used, S.cap);
    }
    else
        Msg("! [craters] a blast at [%.2f, %.2f, %.2f]: the crater is drawn but not dug (pool %u of %u full)", VPUSH(c), S.used, S.cap);
    return true;
}

// E62: a breach through a wall: its faces cut out in the square of half side half_side round c (the middle of the wall)
bool breach(const Fvector& c, const Fvector& n, float half_side, float thick)
{
    load_cfg();
    if (!S.loaded || !on() || half_side <= 0.f || (int)S.list.size() >= C.max_craters)
        return false;
    Crater k{c, half_side, thick};
    k.kind = 1;
    k.n = n;
    if (!cut(k))
        return false;
    S.list.push_back(k);
    return true;
}

void pit(const Fvector& c, float r, float depth)
{
    load_cfg();
    if (!S.loaded || !on() || r < C.pit_r_min)
        return;
    for (Crater& p : S.pits)
        if (p.c.distance_to(c) < 0.05f)
        {
            p.r = _max(p.r, r), p.depth = _max(p.depth, depth);
            return;
        }
    S.pits.push_back({c, r, depth});
}

void update()
{
    load_cfg();
    if (!S.loaded || !g_pGameLevel || !Level().ObjectSpace.GetStaticModel())
        return;
    if (!S.applied)
    {
        S.applied = true;
        S.gen = G.gen;
        S.level = Level().name();
        apply_saved();
    }
    if (S.need_restore)
    {
        S.need_restore = false;
        undo_all();
        S.gen = G.gen;
        apply_saved();
    }
    if (!S.pits.empty() && Device.dwTimeGlobal >= S.pit_next && (int)S.list.size() < C.max_craters)
    {
        // a pit grows by the round: its collision is dug again once it is pit_grow wider than its last dig there
        const Crater p = S.pits.front();
        S.pits.erase(S.pits.begin());
        float last = 0.f;
        for (const Crater& k : S.list)
            if (k.kind == 0 && k.c.distance_to(p.c) < _max(k.r, p.r) * 0.5f) // E62: not a breach
                last = _max(last, k.r);
        if (p.r >= last * C.pit_grow)
        {
            // only what it is deeper than the digs there already
            float had = 0.f;
            for (const Crater& k : S.list)
                if (k.kind == 0) // E62
                    had += k.depth * profile(Fvector2{k.c.x - p.c.x, k.c.z - p.c.z}.magnitude(), k.r * C.rim_k);
            const Crater k{p.c, p.r, _min(_max(0.f, p.depth - had), C.max_depth - had)};
            if (k.depth > 0.01f && dig(k))
                S.list.push_back(k);
        }
        S.pit_next = Device.dwTimeGlobal + u32(C.pit_gap * 1000.f);
    }
}

void reset()
{
    stash();
    S = State{}; // (the material table stays)
}

void save_game(LPCSTR sav_path)
{
    if (!sav_path || !*sav_path)
        return;
    stash();
    string_path fn;
    xr_sprintf(fn, "%s.craters", sav_path);
    u32 total = 0;
    for (const auto& [lvl, l] : G.levels)
        total += (u32)l.size();
    if (!total)
    {
        if (FS.exist(fn))
            FS.file_delete(fn);
        return;
    }
    IWriter* w = FS.w_open(fn);
    if (!w)
    {
        Msg("! [craters] cannot write %s", fn);
        return;
    }
    w->w("PMCR", 4);
    w->w_u32(SAVE_VERSION);
    w->w_u32((u32)G.levels.size());
    for (const auto& [lvl, l] : G.levels)
    {
        w->w_stringZ(lvl.c_str());
        w->w_u32((u32)l.size());
        for (const Crater& k : l)
        {
            w->w_u8(k.kind); // E62 (v2)
            w->w_fvector3(k.c);
            w->w_float(k.r);
            w->w_float(k.depth);
            if (k.kind == 1)
                w->w_fvector3(k.n);
        }
    }
    FS.w_close(w);
    Msg("* [craters] %u crater(s) saved to %s", total, fn);
}

void load_game(LPCSTR sav_path)
{
    ++G.gen;
    G.levels.clear();
    if (S.loaded)
        S.need_restore = true; // a save loaded on this level: the ground back, then the save's craters
    if (!sav_path || !*sav_path)
        return;
    string_path fn;
    xr_sprintf(fn, "%s.craters", sav_path);
    IReader* r = FS.r_open(fn);
    if (!r)
        return;
    char magic[4]{};
    if (r->length() >= 12)
        r->r(magic, 4);
    u32 total = 0;
    const u32 ver = strncmp(magic, "PMCR", 4) == 0 ? r->r_u32() : 0;
    if (ver == 1 || ver == 2) // E62: 2 - each one's kind
        for (u32 n = r->r_u32(); n && r->elapsed() > 0; --n)
        {
            shared_str lvl;
            r->r_stringZ(lvl);
            auto& l = G.levels[lvl];
            for (u32 m = r->r_u32(); m && r->elapsed() >= 20; --m)
            {
                Crater k;
                k.kind = ver >= 2 ? r->r_u8() : u8(0);
                r->r_fvector3(k.c);
                k.r = r->r_float();
                k.depth = r->r_float();
                if (k.kind == 1)
                    r->r_fvector3(k.n);
                l.push_back(k);
                ++total;
            }
        }
    FS.r_close(r);
    Msg("* [craters] %u crater(s) loaded from %s", total, fn);
}

LPCSTR lua_info()
{
    static string256 buf;
    xr_sprintf(buf, "level=%s craters=%u dug=%u breaches=%u pool=%u/%u full=%u last_ms=%.1f enabled=%d", S.level.size() ? S.level.c_str() : "-",
        (u32)S.list.size(), S.dug, S.cuts, S.used, S.cap, S.full, S.ms, on() ? 1 : 0); // E62: breaches
    return buf;
}

float lua_depth_at(const Fvector& pos)
{
    float d = 0.f;
    for (const Crater& k : S.list)
        if (k.kind == 0) // E62
            d += k.depth * profile(Fvector2{k.c.x - pos.x, k.c.z - pos.z}.magnitude(), k.r * C.rim_k);
    return d;
}

float lua_ground(const Fvector& pos)
{
    if (!g_pGameLevel)
        return -1.f;
    collide::rq_result R;
    const Fvector from{pos.x, pos.y + 1.f, pos.z};
    if (!Level().ObjectSpace.RayPick(from, Fvector{0.f, -1.f, 0.f}, 5.f, collide::rqtStatic, R, nullptr) || R.element < 0)
        return -1.f;
    const CDB::TRI& t = Level().ObjectSpace.GetStaticTris()[R.element];
    const Fvector* V = Level().ObjectSpace.GetStaticVerts();
    if (!is_ground(u16(t.material)))
        return -1.f;
    Fvector n;
    n.mknormal(V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]);
    return _abs(n.y);
}

bool lua_make(const Fvector& pos, float r)
{
    load_cfg();
    if (!S.loaded || !g_pGameLevel || r <= 0.f)
        return false;
    collide::rq_result R;
    const Fvector from{pos.x, pos.y + 1.f, pos.z};
    if (!Level().ObjectSpace.RayPick(from, Fvector{0.f, -1.f, 0.f}, 4.f, collide::rqtStatic, R, nullptr) || R.element < 0)
        return false;
    const Crater k{Fvector{pos.x, from.y - R.range, pos.z}, r, r * C.depth_k};
    if (!dig(k))
        return false;
    S.list.push_back(k);
    Msg("* [craters] dug by a script: %.2f m wide, %.2f m deep at [%.2f, %.2f, %.2f] (%.1f ms; pool %u of %u)", 2.f * r, k.depth, VPUSH(k.c), S.ms, S.used,
        S.cap);
    return true;
}
} // namespace portal_mod_craters
