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

// portal-mod E10: a character's contact with static geometry at p (contact normal nrm) inside a linked portal:
// anything at or behind the portal's surface (up to 2.5 m: the wall, frames and bumps in it, whatever is behind it)
// but the ground at its foot, or, in front of it, the surface itself (a sphere's contact point can sit up to its
// radius out, facing along the portal's normal).
// Such contacts are dropped, so the portal is a hole you can walk into (and fall into, on a floor); a floor in front
// of a wall portal still holds you up. depth: the contact's penetration. feet_y (a character's feet): only what is right under them (12 cm) counts as ground,
// not the top or underside of a fence rail or a sill at shin, knee or chest height.
inline bool hole(const float* p, const float* nrm, float feet_y = FLT_MAX, float depth = 0.f)
{
    if (!linked())
        return false;
    for (int i = 0; i < 2; ++i)
    {
        Frame A;
        if (!get_frame(i, A))
            continue;
        Fvector q;
        q.set(p[0] - A.c.x, p[1] - A.c.y, p[2] - A.c.z);
        const float h = q.dotproduct(A.n); // the surface is at -0.012 (the drawn quad floats 1.2 cm in front)
        if (h > 0.6f || h < -2.5f)
            continue;
        const float nu = _abs(nrm[0] * A.u.x + nrm[1] * A.u.y + nrm[2] * A.u.z);
        if (h > 0.3f)
        {
            if (_abs(nrm[0] * A.n.x + nrm[1] * A.n.y + nrm[2] * A.n.z) < 0.5f)
                continue; // further out only the surface itself (a sphere's contact point sits at its centre)
        }
        else if (h > 0.05f)
        {
            if (nu > 0.7f && p[1] < feet_y + 0.12f)
                continue; // the ground in front holds you up (floor/ceiling portals: whatever stands along them)
        }
        else if (_abs(A.n.y) < 0.7f && h > -0.6f && nu > 0.993f && p[1] < feet_y + 0.05f)
            continue; // flat ground level with the feet at and just behind a wall portal: you walk in on it (with nothing
                      // under the leading foot the character controller backs off as from a ledge). Not a fence's bottom
                      // rail or the boards' lower edge: their contacts are higher or slanted (a board's edge leans
                      // 10-12 degrees, the ground under the feet hardly at all). No depth test: a foot that sank a
                      // little would lose the ground behind the plane and sink further.
        // at or behind the surface anything goes: the wall, frames and bumps in it, steps and floors behind it
        // the hole is the portal's rectangle, as in Portal 2 (the oval is only drawn): an oval's narrow bottom would
        // catch the feet of someone walking into a portal that stands on the floor. A wall portal's hole reaches 0.2 m
        // below it, so a portal a few centimetres above the floor can be walked into like a low step.
        const float y = q.dotproduct(A.u);
        if (_abs(q.dotproduct(A.r)) < A.hw && y < A.hh && y > -A.hh - (_abs(A.n.y) < 0.7f ? 0.2f : 0.f))
            return true;
    }
    return false;
}


// portal-mod E11: is a point (a body's centre) in a linked portal's hole zone: over its rectangle (0.2 m more at the
// bottom of a wall portal), from 1 m in front of it to 2 m behind it?
inline bool in_hole(const float* p)
{
    if (!linked())
        return false;
    for (int i = 0; i < 2; ++i)
    {
        Frame A;
        if (!get_frame(i, A))
            continue;
        Fvector q;
        q.set(p[0] - A.c.x, p[1] - A.c.y, p[2] - A.c.z);
        const float h = q.dotproduct(A.n), y = q.dotproduct(A.u);
        if (h < 1.f && h > -2.f && _abs(q.dotproduct(A.r)) < A.hw && y < A.hh && y > -A.hh - (_abs(A.n.y) < 0.7f ? 0.2f : 0.f))
            return true;
    }
    return false;
}

// portal-mod E13: where a contact really is. The tri-list collider reports a long triangle's far vertex as the contact
// point (a 20 m fence board: 20 m away); then the body's centre projected onto the contact plane is used instead.
inline void contact_point(const float* pos, const float* nrm, const float* body, float* out)
{
    const float dx = pos[0] - body[0], dy = pos[1] - body[1], dz = pos[2] - body[2];
    if (dx * dx + dy * dy + dz * dz <= 1.5f * 1.5f)
    {
        out[0] = pos[0];
        out[1] = pos[1];
        out[2] = pos[2];
        return;
    }
    const float d = dx * nrm[0] + dy * nrm[1] + dz * nrm[2];
    out[0] = body[0] + nrm[0] * d;
    out[1] = body[1] + nrm[1] * d;
    out[2] = body[2] + nrm[2] * d;
}

// portal-mod E10: roll added to the first-person camera (radians), set from Lua (level.portal_view_roll)
inline float view_roll = 0.f;
} // namespace portal_mod
