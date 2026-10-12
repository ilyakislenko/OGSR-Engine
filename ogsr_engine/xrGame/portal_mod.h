// portal-mod: Portal 2 portals, shared math for the game side (bullets, physics bodies, the camera through a portal).
// The portals themselves are xr_3da/Render.h portal_mod_draw[2], filled from Lua (level.portal_set).
#pragma once

#include "../xr_3da/Render.h"

namespace portal_mod
{
struct Frame
{
    Fvector c; // centre (the drawn quad, a centimetre in front of the surface)
    Fvector r, u, n; // right, up, surface normal (unit)
    float hw{}, hh{}; // half width, half height
};

inline bool get_frame(int i, Frame& f)
{
    const PORTAL_MOD_DRAW& p = portal_mod_draw[i];
    if (!p.on)
        return false;
    f.hw = p.xform.i.magnitude();
    f.hh = p.xform.j.magnitude();
    if (f.hw < EPS_L || f.hh < EPS_L)
        return false;
    f.r.div(p.xform.i, f.hw);
    f.u.div(p.xform.j, f.hh);
    f.n.set(p.xform.k);
    f.c.set(p.xform.c);
    return true;
}

inline bool linked() { return portal_mod_draw[0].on && portal_mod_draw[1].on && portal_mod_draw[0].params.y > 0.5f; }

// A direction into A comes out of B: a half turn about the portal's up axis (local x -> -x, z -> -z).
inline Fvector map_dir(const Frame& A, const Frame& B, const Fvector& v)
{
    Fvector o;
    o.mul(B.r, -v.dotproduct(A.r)).mad(B.u, v.dotproduct(A.u)).mad(B.n, -v.dotproduct(A.n));
    return o;
}

inline Fvector map_point(const Frame& A, const Frame& B, const Fvector& p)
{
    Fvector d;
    d.sub(p, A.c);
    Fvector o = map_dir(A, B, d);
    o.add(B.c);
    return o;
}

// Orientation through the portals: rows i, j, k of a rotation mapped like directions.
inline void map_rotation(const Frame& A, const Frame& B, Fmatrix& m)
{
    const Fvector i = map_dir(A, B, m.i), j = map_dir(A, B, m.j), k = map_dir(A, B, m.k);
    m.i.set(i);
    m.j.set(j);
    m.k.set(k);
}

// Does the segment from p0 along unit dir, len metres long, enter A's oval from its front side?
// t = distance along the segment to the portal plane. `grow` widens the oval (body radius etc.).
inline bool crosses(const Frame& A, const Fvector& p0, const Fvector& dir, float len, float& t, float grow = 0.f)
{
    const float d0 = Fvector().sub(p0, A.c).dotproduct(A.n);
    const float dn = dir.dotproduct(A.n);
    if (d0 < -0.05f || dn >= -EPS_S)
        return false; // behind the portal, or not moving towards it
    t = _max(0.f, d0) / -dn;
    if (t > len)
        return false;
    Fvector hit;
    hit.mad(p0, dir, t).sub(A.c);
    const float x = hit.dotproduct(A.r) / (A.hw + grow), y = hit.dotproduct(A.u) / (A.hh + grow);
    return x * x + y * y <= 1.f;
}
} // namespace portal_mod
