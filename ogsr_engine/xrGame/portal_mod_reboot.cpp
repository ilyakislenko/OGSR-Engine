// portal-mod E74: a reboot instead of the player's death (see portal_mod_reboot.h).
//
// The timeline (s from the death; config\portal_mod\pm_reboot.ltx [pm_reboot], defaults):
//   0 .. fall (1.15)        the fall: the knees give (the eye drops to 1.25 m, the head nods), then he topples onto his
//                           side over the knees, faster and faster, the view rolling with the body (roll 80 deg), the
//                           head comes down on the ground (the eye 0.42 m over it), bounces once, shakes. Red flash, noise,
//                           the picture tearing (duality), two drop-outs: the power failing.
//   fall .. dark (1.9)      the screen dies: a last flash at the impact, the picture collapses to black in flickers.
//   dark .. vision (3.6)    black (the HUD types the BIOS lines over it).
//   vision .. total - rise  the machine's vision comes back red and monochrome, in steps (the optics calibrating),
//                           noisy at first; the view lies still - the enemies walk into it.
//   total - rise .. total   he gets up like a machine (2.4 s): a servo jolt, up onto the knees in two pushes with
//                           pauses, then up to standing; the red fades out over the last 1.2 s. At `total` (12) the
//                           health is `health` (0.5), the wounds closed, the hands' slot back.
// The script may hold the rise while a looter is still on his way (level.pm_reboot_extend, up to total_max), and adds
// small effects at its beats (level.pm_reboot_fx: 1 a servo twitch, 2 an optics zoom pulse, 3 a picture glitch).
// While it runs the health is held at 0, the body (the character capsule) is held where it fell, the first-person
// camera's yaw and pitch are held at the pose it will stand up into (the mouse does nothing).
#include "stdafx.h"
#include "portal_mod_reboot.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "ActorCondition.h"
#include "Inventory.h"
#include "Level.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "xr_level_controller.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"
#include "../xr_3da/CameraBase.h"
#include "../xr_3da/CameraManager.h"
#include "../xr_3da/Effector.h"
#include "../xr_3da/EffectorPP.h"
#include "../xrcdb/xr_collide_defs.h"

using namespace luabind;

namespace portal_mod_reboot
{
namespace
{
const ECamEffectorType CAM_TYPE = (ECamEffectorType)7474;
const EEffectorPPType PP_TYPE = (EEffectorPPType)7474;

struct Cfg
{
    int charges = 1; // reboots in reserve
    float recharge = 300.f; // s of play for a spent one to come back
    float health = 0.5f; // after the reboot
    float total = 12.f, total_max = 16.f;
    float fall = 1.15f, dark = 1.9f, vision = 3.6f, rise = 2.4f;
    float eye_ground = 0.42f, roll = 80.f, pitch_end = 3.f, yaw_max = 70.f;
    Fvector vis_color{0.62f, 0.13f, 0.10f}; // the machine vision: luminance x this x 2 (the post-process's colour base)
    float vis_add = 0.02f, vis_noise = 0.1f; // a red floor; the noise once settled
    bool loaded = false;
} C;

enum EPhase
{
    phIdle,
    phFall,
    phDown,
    phBoot,
    phRise
};

struct Pulse
{
    int kind;
    float t0, k;
};

struct State
{
    bool on = true, on_read = false; // My Mods
    int charges = -1; // -1: from the config at first use
    float recharge_left = 0.f;
    bool active = false, started = false;
    bool posed = false; // the body others see lies (lie_down)
    float t = 0.f, total = 12.f;
    u16 killer = u16(-1);
    u32 gen = 0;
    CActor* actor = nullptr; // whose reboot (a loaded save makes a new actor)
    // the fall, fixed at the start
    Fvector pos{}, fwd{}, side{}; // the body's place, the heading he lies looking along, the side he falls to
    float gy0 = 0.f; // the ground under the body
    float e0 = 1.6f; // the eye over the ground at the start
    float h0 = 0.f, p0 = 0.f, h_end = 0.f; // the view at the start; the heading he lies along (and stands up into)
    float sgn = 1.f; // +1 to the right, -1 to the left
    float phi_end = 1.4f; // rad: the body lies over (= the view's roll)
    float rh = 0.8f; // m from the knees to the eye
    float y_end = 0.f; // the eye lying (world y)
    u32 slot0 = NO_ACTIVE_SLOT; // the hands' slot before
    xr_vector<Pulse> pulses;
    // a save loaded mid-reboot: the actor spawns with no health and goes on rebooting (no charge spent)
    bool fresh = true; // no living frame of this actor seen since it spawned (level.pm_reboot_spawned, the binder)
    CActor* alive_actor = nullptr; // the actor last seen alive
    bool resume_set = false; // the script's record of the reboot in the save (it may come before the engine's resume)
    float resume_t = 0.f;
    int resume_killer = -1;
    u32 resume_slot = NO_ACTIVE_SLOT;
} S;

const float KNEE = 0.45f; // m: the knees over the ground (the arc's pivot before it sinks)
const float PIV_LOW = 0.15f; // the pivot lying

float lerpf(float a, float b, float k) { return a + (b - a) * k; }
float sat(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
float ease(float x)
{
    x = sat(x);
    return x * x * (3.f - 2.f * x);
}
float seg(float t, float a, float b) { return b > a ? sat((t - a) / (b - a)) : (t >= b ? 1.f : 0.f); }
float wrap_pi(float a)
{
    while (a > PI)
        a -= PI_MUL_2;
    while (a < -PI)
        a += PI_MUL_2;
    return a;
}
// a stable random 0..1 for a time bucket (flickers that hold for a few frames)
float hash01(float x, u32 salt)
{
    u32 n = u32(iFloor(x)) * 747796405u + salt * 2891336453u + 12345u;
    n ^= n >> 15;
    n *= 0x2c1b3c6du;
    n ^= n >> 12;
    n *= 0x297a2d39u;
    n ^= n >> 15;
    return float(n & 0xffffu) / 65535.f;
}

void load_cfg(bool force = false)
{
    if (C.loaded && !force)
        return;
    C = Cfg{};
    C.loaded = true;
    string_path fn;
    if (!FS.exist(fn, "$game_config$", "portal_mod\\pm_reboot.ltx"))
    {
        Msg("! E74 reboot: no config\\portal_mod\\pm_reboot.ltx, the defaults");
        return;
    }
    CInifile ini(fn, TRUE, TRUE, FALSE);
    LPCSTR s = "pm_reboot";
    if (!ini.section_exist(s))
        return;
    auto f = [&](LPCSTR k, float d) { return ini.line_exist(s, k) ? ini.r_float(s, k) : d; };
    C.charges = iFloor(f("charges", float(C.charges)) + 0.5f);
    C.recharge = f("recharge", C.recharge);
    C.health = f("health", C.health);
    C.total = f("total", C.total);
    C.total_max = f("total_max", C.total_max);
    C.fall = f("fall", C.fall);
    C.dark = f("dark", C.dark);
    C.vision = f("vision", C.vision);
    C.rise = f("rise", C.rise);
    C.eye_ground = f("eye_ground", C.eye_ground);
    C.roll = f("roll", C.roll);
    C.pitch_end = f("pitch_end", C.pitch_end);
    C.yaw_max = f("yaw_max", C.yaw_max);
    if (ini.line_exist(s, "vision_color"))
        C.vis_color = ini.r_fvector3(s, "vision_color");
    C.vis_add = f("vision_add", C.vis_add);
    C.vis_noise = f("vision_noise", C.vis_noise);
    // sane order
    clamp(C.charges, 0, 99);
    clamp(C.fall, 0.4f, 4.f);
    C.dark = _max(C.dark, C.fall + 0.1f);
    C.vision = _max(C.vision, C.dark);
    clamp(C.rise, 1.f, 6.f);
    C.total = _max(C.total, C.vision + C.rise + 0.5f);
    C.total_max = _max(C.total_max, C.total);
    clamp(C.health, 0.05f, 1.f);
    clamp(C.roll, 20.f, 90.f);
    Msg("* E74 reboot: charges %d, recharge %.0f s, health %.2f, %.1f s (up to %.1f)", C.charges, C.recharge, C.health, C.total, C.total_max);
}

// $app_data_root$\mod_manager.ltx (My Mods): "reboot = 1"; no file or no line: on
void read_switch()
{
    S.on_read = true;
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
        if (xr_strcmp(_Trim(line), "reboot"))
            continue;
        LPSTR v = _Trim(eq + 1);
        S.on = !xr_strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true");
    }
    fclose(f);
}

void ensure()
{
    load_cfg();
    if (!S.on_read)
        read_switch();
    if (S.charges < 0)
        S.charges = C.charges;
}

EPhase phase()
{
    if (!S.active)
        return phIdle;
    if (S.t < C.fall)
        return phFall;
    if (S.t < C.vision)
        return phDown;
    if (S.t < S.total - C.rise)
        return phBoot;
    return phRise;
}

// the ground under p (static and objects other than the actor), within 2.5 m down from 0.6 m over it; else def
float ground(const Fvector& p, CObject* ignore, float def)
{
    collide::rq_result R;
    const Fvector start{p.x, p.y + 0.6f, p.z};
    if (Level().ObjectSpace.RayPick(start, Fvector{0.f, -1.f, 0.f}, 3.1f, collide::rqtBoth, R, ignore))
        return start.y - R.range;
    return def;
}

// free distance from the body at height h along dir (up to 2 m)
float clear(const Fvector& p, float h, const Fvector& dir, CObject* ignore)
{
    collide::rq_result R;
    const Fvector start{p.x, p.y + h, p.z};
    if (Level().ObjectSpace.RayPick(start, dir, 2.f, collide::rqtBoth, R, ignore))
        return R.range;
    return 2.f;
}

void pulses_prune()
{
    for (u32 i = 0; i < S.pulses.size();)
    {
        if (S.t - S.pulses[i].t0 > 1.5f)
            S.pulses.erase(S.pulses.begin() + i);
        else
            ++i;
    }
}

//----------------------------------------------------------------------------------------------------------------------
// the camera: the body's pose by the timeline, absolute (the actor's own camera is only the end it stands up into)
class CRebootCam : public CEffectorCam
{
    u32 m_gen;

public:
    CRebootCam(u32 gen) : CEffectorCam(CAM_TYPE, 1000.f), m_gen(gen) { SetHudAffect(false); }
    BOOL Valid() override { return S.active && S.started && S.gen == m_gen; }
    BOOL ProcessCam(SCamEffectorInfo& info) override
    {
        if (!Valid())
            return FALSE;
        pose(info);
        return TRUE;
    }

    // the eye on its arc over the knees: the pivot over the body's place (a stumble forward), the angle phi to the side
    static Fvector arc(float phi, float piv_y, float yoff)
    {
        Fvector e = S.pos;
        e.mad(S.fwd, 0.08f);
        e.y = S.gy0 + piv_y + S.rh * _cos(phi) + yoff;
        e.mad(S.side, S.rh * _sin(phi));
        return e;
    }

    static void pose(SCamEffectorInfo& info)
    {
        const float t = S.t;
        const float F = C.fall, fb = 0.28f * F, ft = 0.83f * F;
        const float p_b = clampr(S.p0 - deg2rad(10.f), -0.6f, 0.35f);
        const float h_b = lerpf(S.h0, S.h_end, 0.2f);
        const float y_fix = S.y_end - (S.gy0 + PIV_LOW + S.rh * _cos(S.phi_end)); // lands on the ground at its side
        const float pitch_end = deg2rad(C.pitch_end);
        float h = S.h0, p = S.p0, b = 0.f, fov_add = 0.f;
        Fvector eye = info.p;
        const float rise0 = S.total - C.rise;

        if (t < fb) // the knees give
        {
            const float u = ease(t / fb);
            Fvector knee = S.pos;
            knee.mad(S.fwd, 0.08f * u);
            eye.set(knee.x, S.gy0 + lerpf(S.e0, KNEE + S.rh, u * u), knee.z);
            p = lerpf(S.p0, p_b, u);
            b = S.sgn * deg2rad(4.f) * u;
            h = lerpf(S.h0, h_b, u);
        }
        else if (t < ft) // the topple: faster and faster, over the knees sinking to the ground
        {
            const float u = (t - fb) / (ft - fb);
            const float phi = lerpf(deg2rad(4.f), S.phi_end, powf(u, 2.1f));
            eye = arc(phi, lerpf(KNEE, PIV_LOW, u * u), y_fix * u * u);
            b = S.sgn * phi;
            p = lerpf(p_b, pitch_end, ease(u));
            h = lerpf(h_b, S.h_end, ease(u));
        }
        else if (t < rise0) // the impact, then lying still
        {
            const float u = t - ft;
            eye = arc(S.phi_end, PIV_LOW, y_fix);
            if (u < 0.32f)
                eye.y += 0.05f * _sin(PI * u / 0.32f) * (1.f - u / 0.32f);
            const float dec = expf(-9.f * u);
            b = S.sgn * (S.phi_end + deg2rad(5.f) * dec * _sin(u * 31.f));
            p = pitch_end + deg2rad(1.6f) * dec * (hash01(t / 0.03f, 1) - 0.5f);
            h = S.h_end + deg2rad(1.6f) * dec * (hash01(t / 0.03f, 2) - 0.5f);
            eye.x += 0.012f * dec * (hash01(t / 0.03f, 3) - 0.5f);
            eye.z += 0.012f * dec * (hash01(t / 0.03f, 4) - 0.5f);
        }
        else // getting up like a machine (the times below for a 2.4 s rise)
        {
            const float r = (t - rise0) * 2.4f / C.rise;
            const float a1 = S.phi_end - deg2rad(8.f), a2 = deg2rad(45.f), a3 = deg2rad(12.f);
            float phi, piv, lift = 0.f;
            if (r < 0.25f) // a servo jolt
            {
                const float u = ease(r / 0.25f);
                phi = lerpf(S.phi_end, a1, u);
                piv = PIV_LOW;
                lift = 0.05f * u;
            }
            else if (r < 0.45f) // holds
            {
                phi = a1;
                piv = PIV_LOW;
                lift = 0.05f;
            }
            else if (r < 0.85f) // the first push
            {
                const float u = ease((r - 0.45f) / 0.4f);
                phi = lerpf(a1, a2, u);
                piv = lerpf(PIV_LOW, 0.3f, u);
                lift = 0.05f * (1.f - u);
            }
            else if (r < 0.95f)
            {
                phi = a2;
                piv = 0.3f;
            }
            else if (r < 1.3f) // up onto the knees
            {
                const float u = ease((r - 0.95f) / 0.35f);
                phi = lerpf(a2, a3, u);
                piv = lerpf(0.3f, KNEE, u);
            }
            else
            {
                phi = a3;
                piv = KNEE;
            }
            const float yk = S.phi_end - a3 > 0.01f ? sat((phi - a3) / (S.phi_end - a3)) : 0.f;
            Fvector kneel = arc(phi, piv, y_fix * yk + lift);
            float bb = S.sgn * phi, pp = r < 1.3f ? lerpf(pitch_end, deg2rad(-14.f), sat((r - 0.45f) / 0.85f)) : deg2rad(-14.f);
            // standing up: from the knees to the actor's own eye, a small overshoot, then exactly it
            float k = 0.f, over = 0.f;
            if (r >= 1.55f)
            {
                const float u = sat((r - 1.55f) / 0.65f);
                k = ease(u);
                over = 0.025f * _sin(PI * u) * u * u; // rises a little past his height and settles
            }
            eye.lerp(kneel, info.p, k);
            eye.y += over;
            b = lerpf(bb, 0.f, k);
            p = lerpf(pp, 0.f, k);
            h = S.h_end;
        }

        // the script's beats
        for (const Pulse& q : S.pulses)
        {
            const float d = t - q.t0;
            if (d < 0.f)
                continue;
            if (q.kind == 1 && d < 0.4f) // a servo twitch
            {
                const float dec = expf(-11.f * d);
                b += S.sgn * deg2rad(2.5f * q.k) * _sin(d * 42.f) * dec;
                eye.y += 0.012f * q.k * _sin(d * 35.f) * dec;
            }
            else if (q.kind == 2 && d < 0.6f) // an optics zoom pulse (q.k deg)
                fov_add -= q.k * (d < 0.12f ? d / 0.12f : (d < 0.27f ? 1.f : 1.f - (d - 0.27f) / 0.33f));
        }

        Fmatrix M;
        M.setHPB(h, p, b);
        info.p.set(eye);
        info.d.set(M.k);
        info.n.set(M.j);
        info.r.set(M.i);
        info.fFov = _max(5.f, info.fFov + fov_add);
    }
};

//----------------------------------------------------------------------------------------------------------------------
// the picture: red flash and tearing in the fall, black, then the machine's red monochrome vision coming back in steps
class CRebootPP : public CEffectorPP
{
    u32 m_gen;

public:
    CRebootPP(u32 gen) : CEffectorPP(PP_TYPE, 1000.f, true), m_gen(gen) { bOverlap = false; }
    BOOL Valid() override { return S.active && S.started && S.gen == m_gen; }
    BOOL Process(SPPInfo& pp) override
    {
        if (!Valid())
            return FALSE;
        pp.blur = 0.f;
        pp.gray = 0.f;
        pp.duality.set(0.f, 0.f);
        pp.noise.set(0.f, 1.f, 30.f);
        pp.color_base.set(.5f, .5f, .5f);
        pp.color_gray.set(.333f, .333f, .333f);
        pp.color_add.set(0.f, 0.f, 0.f);
        pp.cm_influence = 0.f;
        pp.cm_interpolate = 0.f;

        const float t = S.t, F = C.fall;
        if (t < F) // the fall: red flash, the picture tearing, the power failing
        {
            const float u = t / F;
            const float flash = t < 0.12f ? 1.f - t / 0.12f : 0.f;
            pp.color_add.set(0.32f * flash, 0.02f * flash, 0.f);
            pp.gray = 0.25f + 0.35f * u;
            float k = 1.f - 0.25f * u;
            if ((u > 0.52f && u < 0.57f) || (u > 0.78f && u < 0.82f))
                k *= 0.12f;
            pp.color_base.set(0.56f * k, 0.40f * k, 0.38f * k);
            pp.noise.set(0.15f + 0.4f * u, 0.5f, 24.f);
            if (hash01(t / 0.07f, 11) > 0.5f)
                pp.duality.set((hash01(t / 0.07f, 12) - 0.5f) * 0.035f, (hash01(t / 0.07f, 13) - 0.5f) * 0.015f);
        }
        else if (t < C.dark) // the screen dies
        {
            const float u = (t - F) / (C.dark - F);
            float k = (1.f - u) * (1.f - u) * (1.f - u);
            if (hash01(t / 0.05f, 21) > 0.7f)
                k *= 0.25f;
            const float flash = t - F < 0.06f ? 1.f : 0.f;
            pp.color_base.set(0.55f * k, 0.38f * k, 0.36f * k);
            pp.color_add.set(0.25f * flash, 0.08f * flash, 0.05f * flash);
            pp.gray = 0.6f;
            pp.noise.set(0.5f, 0.5f, 30.f);
        }
        else if (t < C.vision) // black
        {
            pp.color_base.set(0.f, 0.f, 0.f);
        }
        else // the machine's vision, red and monochrome, in steps; fading back to colour over the last 1.2 s
        {
            const float v = t - C.vision;
            static const float STEP_T[] = {0.25f, 1.0f, 1.8f, 2.6f};
            static const float STEP_K[] = {0.32f, 0.58f, 0.82f, 1.f};
            float k = 0.f;
            for (int i = 0; i < 4; ++i)
                if (v >= STEP_T[i])
                    k = lerpf(i ? STEP_K[i - 1] : 0.f, STEP_K[i], sat((v - STEP_T[i]) / 0.08f));
            if (v < 3.f && hash01(t / 0.06f, 31) > 0.8f)
                k *= 0.35f;
            const float w = seg(t, S.total - 1.2f, S.total);
            const float noise = v < 3.f ? lerpf(0.55f, C.vis_noise + 0.02f, v / 3.f) : C.vis_noise;
            const Fvector& vc = C.vis_color;
            pp.gray = lerpf(1.f, 0.f, w);
            pp.color_base.set(lerpf(vc.x * k, .5f, w), lerpf(vc.y * k, .5f, w), lerpf(vc.z * k, .5f, w));
            pp.color_add.set(lerpf(C.vis_add * k, 0.f, w), 0.f, 0.f);
            pp.noise.set(lerpf(noise, 0.f, w), 0.55f, 30.f);
        }

        // the script's glitches
        for (const Pulse& q : S.pulses)
        {
            const float d = t - q.t0;
            if (q.kind != 3 || d < 0.f || d > 0.3f * _max(0.3f, q.k))
                continue;
            pp.duality.set((hash01(t / 0.04f, 41) - 0.5f) * 0.05f * q.k, (hash01(t / 0.04f, 42) - 0.5f) * 0.02f * q.k);
            pp.noise.intensity = _max(pp.noise.intensity, 0.6f);
            if (hash01(t / 0.05f, 43) > 0.6f)
            {
                pp.color_base.r *= 0.3f;
                pp.color_base.g *= 0.3f;
                pp.color_base.b *= 0.3f;
            }
        }
        return TRUE;
    }
};

void forget()
{
    // (the effectors belong to the old actor's camera manager, gone with it: nothing to remove)
    S.active = false;
    S.started = false;
    S.actor = nullptr;
    S.pulses.clear();
}

void begin(CActor* a, u16 who)
{
    S.active = true;
    S.started = false;
    S.posed = false;
    S.t = 0.f;
    S.total = C.total;
    S.killer = who;
    S.gen++;
    S.actor = a;
    S.slot0 = NO_ACTIVE_SLOT;
    S.pulses.clear();
    a->SetfHealth(0.f);
    CObject* k = who != u16(-1) ? Level().Objects.net_Find(who) : nullptr;
    Msg("* E74 reboot: the player down (killer %s), %d charge(s) left, %.0f s to the next", k ? k->cName().c_str() : "none", S.charges, S.recharge_left);
}

// the first frame: the view, the side with room to fall, the ground, the hands down, the effectors
void start(CActor* a)
{
    S.started = true;
    const Fvector eye = Device.vCameraPosition;
    Fvector d = Device.vCameraDirection;
    d.getHP(S.h0, S.p0);
    S.pos = a->Position();
    S.gy0 = ground(S.pos, a, S.pos.y);
    if (S.pos.y - S.gy0 > 1.2f)
        S.gy0 = S.pos.y;
    S.e0 = _max(0.6f, eye.y - S.gy0);

    // he lies looking toward his killer, within yaw_max of where he looked
    S.h_end = S.h0;
    CObject* k = S.killer != u16(-1) && S.killer != a->ID() ? Level().Objects.net_Find(S.killer) : nullptr;
    if (k)
    {
        Fvector to;
        to.sub(k->Position(), S.pos);
        to.y = 0.f;
        if (to.magnitude() > 0.5f)
        {
            to.normalize();
            float hk, pk;
            to.getHP(hk, pk);
            float dh = wrap_pi(hk - S.h0);
            clamp(dh, -deg2rad(C.yaw_max), deg2rad(C.yaw_max));
            S.h_end = S.h0 + dh;
        }
    }
    S.fwd.setHP(S.h_end, 0.f);
    const Fvector right{_cos(S.h_end), 0.f, _sin(S.h_end)};

    // the side with room (the right one when both have)
    float best = -1.f;
    S.sgn = 1.f;
    for (float sg : {1.f, -1.f})
    {
        const Fvector L{right.x * sg, 0.f, right.z * sg};
        const float c = _min(clear(S.pos, 0.5f, L, a), clear(S.pos, 1.0f, L, a));
        if (c > best + 0.05f)
        {
            best = c;
            S.sgn = sg;
        }
    }
    S.side.set(right.x * S.sgn, 0.f, right.z * S.sgn);
    S.rh = 0.8f;
    S.phi_end = deg2rad(C.roll);
    const float lat = best - 0.3f;
    if (lat < S.rh * _sin(S.phi_end)) // against a wall: he leans on it
        S.phi_end = asinf(clampr(lat / S.rh, 0.3f, 1.f));
    Fvector hp = S.pos;
    hp.mad(S.side, S.rh * _sin(S.phi_end));
    const float gy = ground(hp, a, S.gy0);
    S.y_end = clampr(gy + C.eye_ground, S.gy0 - 0.6f, S.gy0 + 0.9f);
    if (S.phi_end < deg2rad(70.f))
        S.y_end = _max(S.y_end, S.gy0 + PIV_LOW + S.rh * _cos(S.phi_end));

    // the trigger let go, the hands down (they come back as he stands)
    a->inventory().Action(kWPN_FIRE, CMD_STOP);
    a->inventory().Action(kWPN_ZOOM, CMD_STOP);
    const u32 cur = a->inventory().GetActiveSlot();
    if (cur != NO_ACTIVE_SLOT) // (a resumed reboot has its hands down already and its slot from the save)
    {
        S.slot0 = cur;
        a->inventory().Activate(NO_ACTIVE_SLOT, eGeneral, true, true);
    }
    a->set_lock_corpse(true); // no monster drags or eats the machine

    a->Cameras().RemoveCamEffector(CAM_TYPE);
    a->Cameras().RemovePPEffector(PP_TYPE);
    a->Cameras().AddCamEffector(xr_new<CRebootCam>(S.gen));
    a->Cameras().AddPPEffector(xr_new<CRebootPP>(S.gen));
    Msg("* E74 reboot: falls to the %s (room %.2f m, lies at %.0f deg), looks along %.0f deg (was %.0f), eye %.2f -> %.2f m", S.sgn > 0 ? "right" : "left", best,
        rad2deg(S.phi_end), rad2deg(S.h_end), rad2deg(S.h0), S.e0, S.y_end - S.gy0);
}

// the capsule stays where he fell: with no input the character keeps its last push, and with no health it slides down
// a slope (5.7 m in reboot_09's second reboot, no looter near) - put back over the spot each frame, its sideways speed
// gone; it may still fall (the height is the physics')
void hold_body(CActor* a, float dt)
{
    CPHMovementControl* mc = a->character_physics_support() ? a->character_physics_support()->movement() : nullptr;
    if (!mc || !mc->CharacterExist() || dt <= 0.f)
        return;
    Fvector acc{0.f, 0.f, 0.f};
    mc->Calculate(acc, Device.vCameraDirection, 0.f, 0.f, dt, false);
    Fvector p, v;
    mc->GetPosition(p);
    mc->GetCharacterVelocity(v);
    if (_sqr(p.x - S.pos.x) + _sqr(p.z - S.pos.z) > 0.0004f || _abs(v.x) + _abs(v.z) > 0.02f)
    {
        p.x = S.pos.x;
        p.z = S.pos.z;
        mc->SetPosition(p);
        mc->SetVelocity(0.f, _min(v.y, 0.f), 0.f);
    }
    mc->GetPosition(a->Position());
    mc->bSleep = FALSE;
}

// the body others see (a looter, the drone, a scene's camera): the actor's own death motion, mp_stalker.omf's
// norm_death_0 (stop at end), played once a few frames in - on the first frame with no health CActor::g_SetAnimation
// plays norm_dead_stop_0, a standing pose; with the capsule kept (keeps_capsule) no ragdoll lays him down. A resumed
// reboot lies at the motion's end at once. At the rise g_SetAnimation's living branch stands the model back up.
void lie_down(CActor* a)
{
    S.posed = true;
    IKinematicsAnimated* ka = a->Visual() ? smart_cast<IKinematicsAnimated*>(a->Visual()) : nullptr;
    if (!ka)
        return;
    const MotionID m = ka->ID_Cycle_Safe("norm_death_0");
    if (!m.valid())
    {
        Msg("! E74 reboot: no norm_death_0 among the actor's motions - his body stands while he reboots");
        return;
    }
    CBlend* B = ka->PlayCycle(m);
    if (B && S.t > 1.f)
        B->timeCurrent = _max(0.f, B->timeTotal - 0.05f);
}

void finish(CActor* a)
{
    a->SetfHealth(C.health);
    a->conditions().ClearWounds();
    a->set_lock_corpse(false);
    if (S.slot0 == NO_ACTIVE_SLOT) // a resumed reboot with no slot on record: the rifle, else the pistol
        S.slot0 = a->inventory().ItemFromSlot(SECOND_WEAPON_SLOT) ? u32(SECOND_WEAPON_SLOT) : (a->inventory().ItemFromSlot(FIRST_WEAPON_SLOT) ? u32(FIRST_WEAPON_SLOT) : u32(NO_ACTIVE_SLOT));
    if (S.slot0 != NO_ACTIVE_SLOT && a->inventory().ItemFromSlot(S.slot0))
        a->inventory().Activate(S.slot0, eGeneral, true);
    if (CPHMovementControl* mc = a->character_physics_support() ? a->character_physics_support()->movement() : nullptr)
        if (mc->CharacterExist())
            mc->EnableCharacter();
    a->cam_FirstEye()->yaw = -S.h_end;
    a->cam_FirstEye()->pitch = 0.f;
    S.active = false;
    S.started = false;
    S.pulses.clear();
    Msg("* E74 reboot: the player up after %.1f s, health %.2f, %d charge(s), %.0f s to the next", S.t, C.health, S.charges, S.recharge_left);
}

// an actor with no health that was never alive since it spawned: a save made mid-reboot, loaded (a save cannot be made
// dead otherwise). AlreadyDie() is no test: CEntity::net_Spawn sets the death time of whatever spawns dead (the first
// reboot:save take lay dead after the load)
bool spawned_down(CActor* a) { return a->GetfHealth() <= 0.f && (S.fresh || S.alive_actor != a); }

void resume(CActor* a, u16 who)
{
    begin(a, S.resume_set && S.resume_killer >= 0 ? u16(S.resume_killer) : who);
    S.t = clampr(S.resume_set ? S.resume_t : C.dark, C.dark, _max(C.dark, S.total - C.rise - 0.3f)); // black, then the red vision
    S.slot0 = S.resume_set ? S.resume_slot : NO_ACTIVE_SLOT;
    S.fresh = false;
    S.resume_set = false;
    // not dead: spawned dead, its death time was set - left so, CEntityAlive would never call KillEntity at his next
    // death (no reboot, no death either). (Spawned dead he is no member of his team's seniority group: nothing the
    // actor needs - CEntity::Die unregisters only a member.)
    a->m_level_death_time = 0;
    a->m_game_death_time = 0;
    Msg("* E74 reboot: a save made mid-reboot - goes on at %.1f s", S.t);
}
} // namespace

bool intercept(CEntity* e, u16 who)
{
    CActor* a = smart_cast<CActor*>(e);
    if (!a || a != Actor())
        return false;
    if (S.active && S.actor == a)
        return true;
    ensure();
    if (spawned_down(a)) // loaded mid-reboot: it goes on whatever the switch and the charges
    {
        resume(a, who);
        return true;
    }
    if (!S.on || S.charges <= 0 || a->Holder())
    {
        Msg("* E74 reboot: no reboot (%s) - the death is real", !S.on ? "switched off in My Mods" : (S.charges <= 0 ? "no charge left" : "in a vehicle"));
        return false;
    }
    S.charges--;
    if (S.recharge_left <= 0.f)
        S.recharge_left = C.recharge;
    begin(a, who);
    return true;
}

bool active() { return S.active && S.actor && S.actor == Actor(); }

bool script_alive(const CEntity* e) { return S.active && e && e == S.actor && S.actor == Actor(); }

// The engine turns an entity with no health into a ragdoll and destroys its capsule (CCharacterPhysicsSupport::
// in_UpdateCL every frame; a killing hit: in_Hit -> KillHit). Stood up after the reboot the actor had no capsule: stuck
// where he fell, his position the capsule's stale one, the camera over it at a crouch's height - the ragdoll slid 1.6 m
// down the slope while he lay, and a second reboot measured his eye from a position 0.6 m over the ground (takes
// reboot_dual_01/02). The rebooting actor keeps his capsule (hold_body holds it) and lies in his own dead pose
// (CActor::g_SetAnimation plays norm_dead_stop_0 while g_Alive() is false).
bool keeps_capsule(CEntityAlive* e)
{
    CActor* a = smart_cast<CActor*>(e);
    if (!a || a != Actor() || a->GetfHealth() > 0.f)
        return false;
    if (S.active && S.actor == a)
        return true;
    ensure();
    if (spawned_down(a))
        return true; // loaded mid-reboot: update() resumes it this frame
    // no health and no KillEntity yet (CEntityAlive::shedule_Update calls it after this frame's UpdateCL): it will reboot
    return S.on && S.charges > 0 && !a->Holder();
}

// an actor spawning with no health is a save made mid-reboot (no dead actor is ever saved otherwise)
bool spawns_down(CEntityAlive* e) { return e && e->GetfHealth() <= 0.f && smart_cast<CActor*>(e); }

void update(CActor* a)
{
    if (!a || a != Actor())
        return;
    ensure();
    if (S.actor && S.actor != a)
        forget(); // a save loaded: another actor
    const float dt = Device.fTimeDelta;
    if (!S.active && spawned_down(a))
        resume(a, u16(-1));
    if (!S.active)
    {
        if (a->GetfHealth() > 0.f)
        {
            S.fresh = false;
            S.alive_actor = a;
        }
        if (S.charges < C.charges)
        {
            S.recharge_left -= dt;
            if (S.recharge_left <= 0.f)
            {
                S.charges++;
                S.recharge_left = S.charges < C.charges ? C.recharge : 0.f;
                Msg("* E74 reboot: a charge back (%d)", S.charges);
            }
        }
        else
            S.recharge_left = 0.f;
        return;
    }
    if (!S.started)
        start(a);
    else
        S.t += dt;
    if (a->GetfHealth() > 0.f)
        a->SetfHealth(0.f); // dead to the world until he stands up
    hold_body(a, dt);
    if (!S.posed && S.t >= 0.1f)
        lie_down(a);
    a->cam_FirstEye()->yaw = -S.h_end; // the pose he stands up into (the mouse does nothing)
    a->cam_FirstEye()->pitch = 0.f;
    pulses_prune();
    if (S.t >= S.total)
        finish(a);
}

//----------------------------------------------------------------------------------------------------------------------
// Lua: level.pm_reboot_*
namespace
{
LPCSTR lua_info()
{
    ensure();
    static string512 buf;
    float hp = Actor() ? Actor()->GetfHealth() : 0.f;
    xr_sprintf(buf,
               "on=%d active=%d phase=%d t=%.3f total=%.3f fall=%.3f dark=%.3f vision=%.3f rise=%.3f total_max=%.3f charges=%d max=%d recharge=%.1f "
               "killer=%d gen=%u yaw=%.4f side=%d health=%.3f slot=%d",
               int(S.on), int(active()), int(phase()), S.active ? S.t : -1.f, S.total, C.fall, C.dark, C.vision, C.rise, C.total_max, S.charges, C.charges,
               S.recharge_left, S.killer == u16(-1) ? -1 : int(S.killer), S.gen, S.h_end, int(S.sgn), hp, S.slot0 == NO_ACTIVE_SLOT ? -1 : int(S.slot0));
    return buf;
}
bool lua_active() { return active(); }
int lua_phase() { return int(phase()); }
float lua_time() { return S.active ? S.t : -1.f; }
int lua_charges()
{
    ensure();
    return S.charges;
}
void lua_enable(bool on)
{
    S.on = on;
    S.on_read = true;
}
bool lua_enabled()
{
    ensure();
    return S.on;
}
void lua_set_charges(int n, float recharge_left)
{
    ensure();
    S.charges = clampr(n, 0, 99);
    S.recharge_left = _max(0.f, recharge_left);
    if (S.charges >= C.charges)
        S.recharge_left = 0.f;
    else if (S.recharge_left <= 0.f)
        S.recharge_left = C.recharge;
}
float lua_extend(float sec)
{
    if (S.active && phase() == phBoot && sec > 0.f)
        S.total = _min(C.total_max, S.total + sec);
    return S.total;
}
void lua_fx(int kind, float k)
{
    if (S.active && kind >= 1 && kind <= 3)
        S.pulses.push_back(Pulse{kind, S.t, k});
}
// kills the player now as a hit from `killer` would (-1: himself); with force a charge is put in if none is left. Never
// a real death: false (and nothing happens) when the switch is off, no charge, in a vehicle or already down.
bool lua_trigger(int killer, bool force)
{
    CActor* a = Actor();
    if (!a || !a->g_Alive() || S.active)
        return false;
    ensure();
    if (force && S.charges <= 0)
        S.charges = 1;
    if (!S.on || S.charges <= 0 || a->Holder())
        return false;
    a->SetfHealth(0.f);
    a->KillEntity(killer >= 0 ? u16(killer) : a->ID());
    return S.active;
}
// a save loaded: the script's record of the charges and of a reboot under way (its time, its killer, the hands' slot);
// the engine may have resumed the reboot already, or does it on the actor's first frame and takes these then
void lua_restore(int charges, float recharge_left, bool act, float t, int killer, int slot)
{
    ensure();
    S.charges = clampr(charges, 0, 99);
    S.recharge_left = _max(0.f, recharge_left);
    if (act && S.active)
    {
        S.t = clampr(t, C.dark, _max(C.dark, S.total - C.rise - 0.3f));
        if (killer >= 0)
            S.killer = u16(killer);
        if (slot >= 0)
            S.slot0 = u32(slot);
    }
    else if (act)
    {
        S.resume_set = true;
        S.resume_t = t;
        S.resume_killer = killer;
        S.resume_slot = slot >= 0 ? u32(slot) : NO_ACTIVE_SLOT;
    }
    Msg("* E74 reboot: restored - %d charge(s), %.0f s to the next%s", S.charges, S.recharge_left, act ? ", a reboot under way" : "");
}
// the actor's binder spawned (a new game, a loaded save): nothing of the old actor's reboot is kept
void lua_spawned()
{
    forget();
    S.fresh = true;
    S.alive_actor = nullptr;
    S.resume_set = false;
}
void lua_reload() { load_cfg(true); }
} // namespace

void script_register(lua_State* L)
{
    module(L, "level")[def("pm_reboot_info", &lua_info), def("pm_reboot_active", &lua_active), def("pm_reboot_phase", &lua_phase),
                       def("pm_reboot_time", &lua_time), def("pm_reboot_charges", &lua_charges), def("pm_reboot_enable", &lua_enable),
                       def("pm_reboot_enabled", &lua_enabled), def("pm_reboot_set_charges", &lua_set_charges), def("pm_reboot_extend", &lua_extend),
                       def("pm_reboot_fx", &lua_fx), def("pm_reboot_trigger", &lua_trigger), def("pm_reboot_restore", &lua_restore),
                       def("pm_reboot_spawned", &lua_spawned), def("pm_reboot_reload", &lua_reload)];
}
} // namespace portal_mod_reboot
