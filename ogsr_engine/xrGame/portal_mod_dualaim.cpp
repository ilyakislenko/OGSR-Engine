// portal-mod E70: dual wield split targeting - see portal_mod_dualaim.h.
//
// Angles. The HUD is drawn with its own narrower field of view (hud_fov 0.45 of fov): a direction d in the HUD's camera
// space shows on the screen where the world direction (d.x / k, d.y / k, d.z) does, k = tan(hud fov / 2) / tan(fov / 2)
// (CHudItem::CorrectDirFromWorldToHud does the same through the projection matrices). So the left barrel is aimed in the
// world's angles - its aim is a world direction from the camera, moved toward the target's at the arm's speed - and the
// HUD direction the model is turned to is that one squeezed by k: on the screen the barrel points at the target, and a
// round sent from the camera along the world direction lands where the barrel points (on the target once it is there).
// The arc limits are world angles from the camera's view: out to the left, in across the body, up, down.
#include "stdafx.h"
#include <algorithm>
#include "portal_mod_dualaim.h"
#include "portal_mod.h"
#include "player_hud.h"
#include "HudItem.h"
#include "Weapon.h"
#include "Level.h"
#include "Actor.h"
#include "entity_alive.h"
#include "../Include/xrRender/Kinematics.h"

extern bool g_hud_dual, g_hud_dual_alt, g_hud_dual_swap; // player_hud.cpp (E26, E27)
extern ENGINE_API float psHUD_FOV;

using namespace luabind;

namespace pm_dualaim
{
namespace
{
bool s_on = true; // My Mods "dual_aim"
const void* s_level = nullptr; // the level the switch was read for
u16 s_target = u16(-1);
int s_point = 0; // 0 the chest, 1 the head, 2 the pelvis
// the arc (degrees, world angles from the camera's view) and the arm
float s_yaw_out = 65.f, s_yaw_in = 22.f, s_up = 40.f, s_down = 38.f;
float s_speed = 480.f; // deg/s: the fastest swing
float s_tau = 0.055f; // s: the swing's smoothing (what is left of the way goes at 1/tau a second, up to s_speed)
float s_pivot = 0.55f; // where the arm turns: 0 its hand, 1 its shoulder
float s_tp_tau() { return s_tau * 1.6f; } // the whole arm in the third person eases a little slower
// the state
Fvector s_aim{0.f, 0.f, 1.f}; // the left barrel's aim, a world direction from the camera
Fvector s_d0{0.f, 0.f, 1.f}; // the mirrored item's own barrel, the HUD camera's space (x right, y up, z forward)
u32 s_frame = u32(-1), s_seen_frame = u32(-1);
float s_err = -1.f; // deg between the aim and the target (-1: no target this frame)
float s_dist = 0.f; // m from the camera to the target
u32 s_left_rounds = 0;

void load_switch()
{
    if (s_level == g_pGameLevel)
        return;
    s_level = g_pGameLevel;
    string_path fn;
    FS.update_path(fn, "$app_data_root$", "mod_manager.ltx");
    FILE* f = nullptr;
    if (fopen_s(&f, fn, "r") || !f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '[')
            continue;
        *eq = 0;
        if (xr_strcmp(_Trim(line), "dual_aim"))
            continue;
        LPSTR v = _Trim(eq + 1);
        s_on = !xr_strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true");
    }
    fclose(f);
    Msg("* E70 dual aim: %s (My Mods)", s_on ? "on" : "off");
}

// the camera's frame
struct cam_t
{
    Fvector r, u, f, c;
    float k; // the HUD's squeeze
};

cam_t camera()
{
    cam_t c;
    c.r = Device.vCameraRight;
    c.u = Device.vCameraTop;
    c.f = Device.vCameraDirection;
    c.r.normalize_safe();
    c.u.normalize_safe();
    c.f.normalize_safe();
    c.c = Device.vCameraPosition;
    const float fov = deg2rad(Device.fFOV);
    const float hud = deg2rad(psHUD_FOV <= 1.f ? psHUD_FOV * Device.fFOV : psHUD_FOV);
    c.k = tanf(hud * 0.5f) / tanf(fov * 0.5f);
    return c;
}

Fvector to_cam(const cam_t& c, const Fvector& v) { return Fvector{v.dotproduct(c.r), v.dotproduct(c.u), v.dotproduct(c.f)}; }
Fvector to_world(const cam_t& c, const Fvector& v)
{
    Fvector w;
    w.mul(c.r, v.x).mad(c.u, v.y).mad(c.f, v.z);
    return w;
}
float yaw_of(const Fvector& v) { return atan2f(v.x, v.z); }
float pitch_of(const Fvector& v) { return atan2f(v.y, _sqrt(v.x * v.x + v.z * v.z)); }
Fvector dir_of(float yaw, float pitch) { return Fvector{_sin(yaw) * _cos(pitch), _sin(pitch), _cos(yaw) * _cos(pitch)}; }

// a camera-space direction kept within the arc
Fvector clamp_arc(const Fvector& v)
{
    const float y = std::clamp(yaw_of(v), -deg2rad(s_yaw_out), deg2rad(s_yaw_in));
    const float p = std::clamp(pitch_of(v), -deg2rad(s_down), deg2rad(s_up));
    return dir_of(y, p);
}

// the world aim's HUD direction and back (camera space)
Fvector hud_of(const Fvector& w, float k)
{
    Fvector h{w.x * k, w.y * k, _max(w.z, 0.05f)};
    return h.normalize();
}
Fvector world_of(const Fvector& h, float k)
{
    Fvector w{h.x / k, h.y / k, _max(h.z, 0.05f)};
    return w.normalize();
}

// v turned toward t by angle a (radians) at most
Fvector turn_toward(const Fvector& v, const Fvector& t, float a)
{
    const float dot = std::clamp(v.dotproduct(t), -1.f, 1.f);
    const float ang = acosf(dot);
    if (ang <= a || ang < 1e-5f)
        return t;
    Fvector axis;
    axis.crossproduct(v, t);
    if (axis.magnitude() < 1e-6f)
        return t;
    axis.normalize();
    // Rodrigues
    Fvector r, x;
    x.crossproduct(axis, v);
    r.mul(v, _cos(a)).mad(x, _sin(a)).mad(axis, axis.dotproduct(v) * (1.f - _cos(a)));
    return r.normalize();
}

CEntityAlive* target_obj()
{
    if (s_target == u16(-1) || !g_pGameLevel)
        return nullptr;
    CEntityAlive* e = smart_cast<CEntityAlive*>(Level().Objects.net_Find(s_target));
    return (e && e->g_Alive() && !e->getDestroy()) ? e : nullptr;
}

u16 find_bone(IKinematics* K, std::initializer_list<const char*> names)
{
    for (const char* n : names)
    {
        const u16 b = K->LL_BoneID(n);
        if (b != BI_NONE)
            return b;
    }
    return BI_NONE;
}

// where on o a round goes: 0 the chest, 1 the head, 2 the pelvis (the first bone its skeleton has; else its middle)
Fvector aim_point(CObject* o, int which)
{
    Fvector p;
    IKinematics* K = o->Visual() ? o->Visual()->dcast_PKinematics() : nullptr;
    u16 b = BI_NONE;
    if (K)
    {
        if (which == 1)
            b = find_bone(K, {"bip01_head", "bip01_neck", "bip01_spine2"});
        else if (which == 2)
            b = find_bone(K, {"bip01_pelvis", "bip01_spine", "bip01_spine1"});
        else
            b = find_bone(K, {"bip01_spine2", "bip01_spine1", "bip01_spine", "bip01_neck"});
    }
    if (b != BI_NONE)
        o->XFORM().transform_tiny(p, K->LL_GetTransform(b).c);
    else
        o->Center(p);
    return p;
}

// once a frame: the aim moved toward the target (or back to where the HUD holds the barrel)
void step(const cam_t& c)
{
    if (s_frame == Device.dwFrame)
        return;
    const bool fresh = s_frame == u32(-1) || Device.dwFrame - s_frame > 3; // the guns just came out: from rest
    s_frame = Device.dwFrame;
    const Fvector rest = to_world(c, world_of(s_d0, c.k));
    if (fresh)
        s_aim = rest;
    CEntityAlive* e = s_on ? target_obj() : nullptr;
    Fvector goal = rest;
    s_err = -1.f;
    float tau = 0.12f; // back to rest a little slower
    if (e)
    {
        Fvector t;
        t.sub(aim_point(e, s_point), c.c);
        s_dist = t.magnitude();
        if (s_dist > 0.3f)
        {
            t.normalize();
            goal = to_world(c, clamp_arc(to_cam(c, t)));
            tau = s_tau;
            s_err = rad2deg(acosf(std::clamp(s_aim.dotproduct(t), -1.f, 1.f)));
        }
    }
    const float dt = _min(Device.fTimeDelta, 0.1f);
    const float ang = acosf(std::clamp(s_aim.dotproduct(goal), -1.f, 1.f));
    const float a = _min(ang * (1.f - expf(-dt / tau)) + deg2rad(2.f) * dt, deg2rad(s_speed) * dt);
    s_aim = turn_toward(s_aim, goal, a);
    // never past the arc, however the camera turns
    s_aim = to_world(c, clamp_arc(to_cam(c, s_aim)));
    if (e && s_err >= 0.f)
    {
        Fvector t;
        t.sub(aim_point(e, s_point), c.c).normalize();
        s_err = rad2deg(acosf(std::clamp(s_aim.dotproduct(t), -1.f, 1.f)));
    }
}
} // namespace

void turn_left(attachable_hud_item* h)
{
    load_switch();
    if (!h || !h->m_model)
        return;
    const cam_t c = camera();
    // the mirrored item's barrel (its k axis: setup_firedeps' fire direction) as the HUD holds it
    Fvector d0 = h->m_item_transform_dual.k;
    d0.normalize_safe();
    s_d0 = to_cam(c, d0);
    s_seen_frame = Device.dwFrame;
    step(c);
    if (!s_on)
        return;
    const Fvector hc = hud_of(to_cam(c, s_aim), c.k); // where the barrel should point, the HUD's space
    if (hc.similar(s_d0, 1e-4f))
        return;
    // the turn taking s_d0 to hc: yaw back to the middle plane, pitch, yaw out (camera space)
    const float y0 = yaw_of(s_d0), p0 = pitch_of(s_d0), y1 = yaw_of(hc), p1 = pitch_of(hc);
    auto rot = [&](const Fvector& vw) {
        Fvector v = to_cam(c, vw);
        auto ry = [](Fvector a, float t) { return Fvector{a.x * _cos(t) + a.z * _sin(t), a.y, -a.x * _sin(t) + a.z * _cos(t)}; };
        auto rx = [](Fvector a, float t) { return Fvector{a.x, a.y * _cos(t) + a.z * _sin(t), -a.y * _sin(t) + a.z * _cos(t)}; };
        v = ry(rx(ry(v, -y0), p1 - p0), y1);
        return to_world(c, v);
    };
    // the arm turns about a point between its hand and its shoulder (the mirrored right arm is the left one); the model
    // drawn on the left: the live one when E27 has it there, else its copy (both at once: the same model twice)
    IKinematics* left = (h->dual_swapped() || !h->m_dual_model) ? h->m_model : h->m_dual_model;
    Fvector piv = h->m_item_transform_dual.c;
    const u16 bh = find_bone(left, {"bip01_r_hand", "bip01_r_forearm"});
    const u16 bs = find_bone(left, {"bip01_r_upperarm", "bip01_r_clavicle"});
    if (bh != BI_NONE && bs != BI_NONE)
    {
        Fvector ph, ps;
        h->m_item_transform_dual.transform_tiny(ph, left->LL_GetTransform(bh).c);
        h->m_item_transform_dual.transform_tiny(ps, left->LL_GetTransform(bs).c);
        piv.lerp(ph, ps, s_pivot);
    }
    Fmatrix& m = h->m_item_transform_dual;
    const Fvector i = rot(m.i), j = rot(m.j), k = rot(m.k);
    Fvector off;
    off.sub(m.c, piv);
    const Fvector cc = Fvector().add(piv, rot(off));
    m.i.set(i);
    m.j.set(j);
    m.k.set(k);
    m.c.set(cc);
}

// ------------------------------------------------------------------- E70b: the third person (E36/E39's two-gun pose)
// E39 poses the left arm as the right arm's mirror image across the chest's middle plane, so it points at the shared aim
// point reflected; with a left target the whole arm (from the upper arm down: its bones' turns, their places follow
// from their parents) is turned about the shoulder by the least turn taking that mirrored aim to the target - eased
// like the HUD's arm, within 75 deg of it - and the left gun (E40: the right gun's mirror off the left hand) with it.
namespace
{
const CActor* s_tp_actor = nullptr;
const IKinematics* s_tp_K = nullptr;
u16 s_tp_clav = BI_NONE, s_tp_upper = BI_NONE;
u32 s_tp_frame = u32(-1), s_tp_last = u32(-1);
bool s_tp_ok = false;
Fvector s_tp_axis{0.f, 1.f, 0.f};
float s_tp_ang = 0.f;
Fvector s_tp_dir{0.f, 0.f, 1.f}; // the arm's eased aim from the shoulder (the actor's model space)
float s_tp_err = 180.f; // deg between it and the target

Fvector rodrigues(const Fvector& v, const Fvector& axis, float a)
{
    Fvector x, r;
    x.crossproduct(axis, v);
    r.mul(v, _cos(a)).mad(x, _sin(a)).mad(axis, axis.dotproduct(v) * (1.f - _cos(a)));
    return r;
}

// once a frame, at the left upper arm's callback (its place S this frame from its parent)
void tp_compute(CActor* A, IKinematics* K, const Fvector& S)
{
    s_tp_frame = Device.dwFrame;
    s_tp_actor = A;
    s_tp_ok = false;
    s_tp_err = 180.f;
    if (!s_on || !A->m_tp_dual || A->m_tp_chest == BI_NONE)
    {
        s_tp_last = u32(-1);
        return;
    }
    Fmatrix inv;
    inv.invert(A->XFORM());
    Fvector T;
    inv.transform_tiny(T, A->m_tp_dual_target);
    const Fmatrix& CH = K->LL_GetTransform(A->m_tp_chest);
    Fvector n;
    CH.transform_dir(n, A->m_tp_chest_lat);
    if (n.square_magnitude() < EPS)
        return;
    n.normalize();
    Fvector Tm = T;
    Tm.mad(n, -2.f * Fvector().sub(T, CH.c).dotproduct(n)); // the aim point mirrored across the chest's plane
    Fvector dm;
    dm.sub(Tm, S);
    if (dm.magnitude() < 0.3f)
        return;
    dm.normalize();
    CEntityAlive* e = target_obj();
    Fvector goal = dm;
    if (e)
    {
        Fvector L, dl;
        inv.transform_tiny(L, aim_point(e, s_point));
        dl.sub(L, S);
        if (dl.magnitude() > 0.3f)
        {
            dl.normalize();
            goal = turn_toward(dm, dl, deg2rad(75.f));
        }
    }
    else if (s_tp_last == u32(-1))
        return; // no target and the arm at rest: nothing to turn
    const bool fresh = s_tp_last == u32(-1) || Device.dwFrame - s_tp_last > 3;
    s_tp_last = Device.dwFrame;
    if (fresh)
        s_tp_dir = dm;
    const float dt = _min(Device.fTimeDelta, 0.1f);
    const float ang = acosf(std::clamp(s_tp_dir.dotproduct(goal), -1.f, 1.f));
    const float a = _min(ang * (1.f - expf(-dt / (e ? s_tp_tau() : 0.12f))) + deg2rad(2.f) * dt, deg2rad(s_speed) * dt);
    s_tp_dir = turn_toward(s_tp_dir, goal, a);
    if (e)
    {
        Fvector L;
        inv.transform_tiny(L, aim_point(e, s_point));
        L.sub(S).normalize_safe();
        s_tp_err = rad2deg(acosf(std::clamp(s_tp_dir.dotproduct(L), -1.f, 1.f)));
    }
    else if (acosf(std::clamp(s_tp_dir.dotproduct(dm), -1.f, 1.f)) < deg2rad(0.5f))
    {
        s_tp_last = u32(-1); // back at rest
        return;
    }
    s_tp_axis.crossproduct(dm, s_tp_dir);
    const float sa = s_tp_axis.magnitude();
    if (sa < 1e-6f)
        return;
    s_tp_axis.div(sa);
    s_tp_ang = acosf(std::clamp(dm.dotproduct(s_tp_dir), -1.f, 1.f));
    s_tp_ok = true;
}

bool tp_live(const CActor* A) { return s_tp_ok && s_tp_actor == A && Device.dwFrame - s_tp_frame <= 2; }
} // namespace

void tp_left_turn(CActor* A, IKinematics* K, u16 bone, Fmatrix& M)
{
    if (!A || !K)
        return;
    if (s_tp_K != K)
    {
        s_tp_K = K;
        s_tp_clav = K->LL_BoneID("bip01_l_clavicle");
        s_tp_upper = K->LL_BoneID("bip01_l_upperarm");
    }
    if (bone == s_tp_upper)
        tp_compute(A, K, M.c);
    if (bone == s_tp_clav || s_tp_frame != Device.dwFrame || !tp_live(A))
        return;
    M.i.set(rodrigues(M.i, s_tp_axis, s_tp_ang));
    M.j.set(rodrigues(M.j, s_tp_axis, s_tp_ang));
    M.k.set(rodrigues(M.k, s_tp_axis, s_tp_ang));
}

void tp_left_gun(const CActor* A, const Fvector& hand, Fmatrix& m2m)
{
    if (!tp_live(A))
        return;
    m2m.i.set(rodrigues(m2m.i, s_tp_axis, s_tp_ang));
    m2m.j.set(rodrigues(m2m.j, s_tp_axis, s_tp_ang));
    m2m.k.set(rodrigues(m2m.k, s_tp_axis, s_tp_ang));
    Fvector off;
    off.sub(m2m.c, hand);
    m2m.c.add(hand, rodrigues(off, s_tp_axis, s_tp_ang));
}

// a scene's camera: the left hand's round leaves the left gun's own muzzle (E40's left gun) for the left target, once the
// arm is on it (within 6 deg); else as before (E40: the right muzzle toward the shared aim)
static int shot_tp(CWeapon* w, Fvector& p, Fvector& d, Fvector& p2, Fvector& d2)
{
    CActor* A = smart_cast<CActor*>(w->H_Parent());
    CEntityAlive* e = target_obj();
    Fmatrix m2;
    if (!A || !e || !tp_live(A) || s_tp_err > 6.f || !w->TpDualLeftXform(m2))
        return 0;
    Fvector lfp, ld;
    m2.transform_tiny(lfp, w->vLoadedFirePoint);
    ld.sub(aim_point(e, s_point), lfp);
    const float dist = ld.magnitude();
    if (dist < 0.3f)
        return 0;
    ld.div(dist);
    const bool alt = g_hud_dual_alt;
    if (alt && !g_hud_dual_swap)
        return 0;
    ++s_left_rounds;
    Msg("portal-mod E70b: left round %u (third person) at [%s] id %u, %.1f m, the arm %.1f deg off (%s)", s_left_rounds, e->cName().c_str(), e->ID(), dist,
        s_tp_err, alt ? "in turn" : "both at once");
    if (alt)
    {
        p = lfp;
        d = ld;
        return 1;
    }
    p2 = lfp;
    d2 = ld;
    return 2;
}

int shot(CWeapon* w, Fvector& p, Fvector& d, Fvector& p2, Fvector& d2)
{
    load_switch();
    if (!s_on || !g_hud_dual || !w || !w->ParentIsActor())
        return 0;
    if (portal_mod::cine)
        return shot_tp(w, p, d, p2, d2);
    if (s_seen_frame == u32(-1) || Device.dwFrame - s_seen_frame > 3) // the HUD's guns not drawn now
        return 0;
    CEntityAlive* e = target_obj();
    if (!e || s_err < 0.f)
        return 0;
    // the left barrel's aim, from where this round leaves, at the target's distance
    const cam_t c = camera();
    Fvector at, ld;
    at.mad(c.c, s_aim, s_dist);
    ld.sub(at, p);
    if (ld.magnitude() < 0.1f)
        return 0;
    ld.normalize();
    const bool alt = g_hud_dual_alt;
    if (alt && !g_hud_dual_swap)
        return 0; // in turn, this round is the right hand's: the crosshair's
    ++s_left_rounds;
    Msg("portal-mod E70: left round %u at [%s] id %u, %.1f m, the barrel %.1f deg off (%s)", s_left_rounds, e->cName().c_str(), e->ID(), s_dist,
        s_err, alt ? "in turn" : "both at once");
    if (alt)
    {
        d = ld;
        return 1;
    }
    p2 = p;
    d2 = ld;
    return 2;
}

// ------------------------------------------------------------------------------------------------------------- Lua
static void api_set(int id, int which)
{
    load_switch();
    const u16 nid = (id < 0 || id >= 0xffff) ? u16(-1) : u16(id);
    if (nid != s_target || which != s_point)
        Msg("* E70 left gun's target %d (%s)", nid == u16(-1) ? -1 : int(nid), which == 1 ? "head" : which == 2 ? "pelvis" : "chest");
    s_target = nid;
    s_point = std::clamp(which, 0, 2);
}
static void api_clear() { api_set(-1, 0); }
static int api_target() { return target_obj() ? int(s_target) : -1; }
static Fvector api_point(int id, int which)
{
    CObject* o = (g_pGameLevel && id >= 0 && id < 0xffff) ? Level().Objects.net_Find(u16(id)) : nullptr;
    return o ? aim_point(o, which) : Fvector{};
}
static float api_error() { return (s_seen_frame != u32(-1) && Device.dwFrame - s_seen_frame <= 3) ? s_err : -1.f; }
static void api_switch(bool on)
{
    load_switch();
    if (on != s_on)
        Msg("* E70 dual aim %s", on ? "on" : "off");
    s_on = on;
}
static bool api_on()
{
    load_switch();
    return s_on;
}
static void api_tune(float yaw_out, float yaw_in, float up, float down, float speed, float tau, float pivot)
{
    s_yaw_out = yaw_out;
    s_yaw_in = yaw_in;
    s_up = up;
    s_down = down;
    s_speed = _max(speed, 1.f);
    s_tau = _max(tau, 0.005f);
    s_pivot = std::clamp(pivot, 0.f, 1.5f);
    Msg("* E70 arc out %.0f in %.0f up %.0f down %.0f, %.0f deg/s, tau %.3f, pivot %.2f", s_yaw_out, s_yaw_in, s_up, s_down, s_speed, s_tau, s_pivot);
}
static LPCSTR api_info()
{
    static string512 buf;
    const cam_t c = camera();
    const Fvector a = to_cam(c, s_aim);
    xr_sprintf(buf, "on %d target %d err %.2f dist %.1f aim yaw %.1f pitch %.1f rest yaw %.1f drawn %d left_rounds %u", int(s_on), api_target(), api_error(),
               s_dist, rad2deg(yaw_of(a)), rad2deg(pitch_of(a)), rad2deg(yaw_of(world_of(s_d0, c.k))), int(api_error() >= 0.f || (s_seen_frame != u32(-1) && Device.dwFrame - s_seen_frame <= 3)),
               s_left_rounds);
    return buf;
}
} // namespace pm_dualaim

void pm_dualaim_script_register(lua_State* L)
{
    using namespace pm_dualaim;
    module(L, "level")[def("dual_aim_set", &api_set), def("dual_aim_clear", &api_clear), def("dual_aim_target", &api_target),
                       def("dual_aim_point", &api_point), def("dual_aim_error", &api_error), def("dual_aim_switch", &api_switch),
                       def("dual_aim_on", &api_on), def("dual_aim_tune", &api_tune), def("dual_aim_info", &api_info)];
}
