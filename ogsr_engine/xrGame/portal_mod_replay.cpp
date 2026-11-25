// portal-mod E53: the replay for filming - see portal_mod_replay.h. tools/engine_patches/patch_e53.py copies this file
// into ogsr_engine/xrGame (turning the Russian text of the hint into cp1251 escapes) and hooks it into the engine.
//
// Recording: at the end of every CLevel::OnFrame (up to `rate` times a second) the state of the world around the player
// goes into tracks, one per object / particle system / sound / light: a key is written only when something changed
// ("hold" semantics: when the change comes after a still stretch, the last still state is written first, with the
// time it was last seen). The ring keeps the last `ring_seconds`; a manual recording (Insert) keeps everything from its
// start. A take is a stretch of the ring with its times shifted to 0, saved as $app_data_root$\replays\<name>.pmr.
//
// Playing: TimerGlobal stops (the scheduler, AI, alife, live particles, bullets and alife time stand), IGame_Level skips
// Objects.Update, the physics world does not step, the live sounds pause and the renderer draws only what carries
// PORTAL_MOD_STYPE_REPLAY: "puppets" (copies of the recorded models, every bone posed from the record through an
// overwriting bone callback), particle systems re-simulated from their birth (E53b: the live ones step in the same
// quanta with the same random sequences, so the replay repeats them exactly), the recorded lights, bullets and portals.
// The camera is written into Device every frame, after everything else in the frame.
#include "stdafx.h"
#include <dinput.h>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <mutex>
#include <atomic>
#include "portal_mod_replay.h"
#include "portal_mod.h"
#include "Level.h"
#include "Actor.h"
#include "Car.h"
#include "CustomMonster.h"
#include "ai_monster_space.h"
#include "inventory_item.h"
#include "PhysicsShellHolder.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "attachment_owner.h"
#include "attachable_item.h"
#include "ParticlesObject.h"
#include "Level_Bullet_Manager.h"
#include "Weapon.h"
#include "HudItem.h"
#include "HUDManager.h"
#include "UI.h"
#include "script_engine.h"
#include "ai_space.h"
#include "xr_level_controller.h"
#include "../xr_3da/Environment.h"
#include "../xr_3da/IGame_Persistent.h"
#include "../xr_3da/CameraManager.h"
#include "../xr_3da/CameraBase.h"
#include "../xr_3da/GameFont.h"
#include "../xr_3da/IInputReceiver.h"
#include "../xr_3da/vis_common.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/ParticleCustom.h"
#include "../Include/xrRender/RenderVisual.h"

CRandom* pm_random_use(CRandom* r); // E53b: xrCore/_math.cpp - the particles' random sequence in place of the thread's

// the creature's eye, as CCustomMonster::eye_pp_s0 computes it for its vision
void CCustomMonster::PmEye(Fvector& pos, Fvector& dir)
{
    IKinematics* V = Visual() ? Visual()->dcast_PKinematics() : nullptr;
    if (!V || eye_bone < 0 || eye_bone >= V->LL_BoneCount())
    {
        pos = XFORM().c;
        pos.y += 1.6f;
        dir = XFORM().k;
        return;
    }
    Fmatrix X;
    X.mul_43(XFORM(), V->LL_GetTransform(u16(eye_bone)));
    const MonsterSpace::SBoneRotation& rotation = head_orientation();
    Fmatrix m;
    m.setHPB(-rotation.current.yaw + m_fEyeShiftYaw, -rotation.current.pitch, 0);
    pos.add(X.c, m_tEyeShift);
    dir = m.k;
}

// E53b: the muzzle's frame on the HUD (as CWeapon::UpdateFireDependencies_internal sets it up for the flame) and on the
// world model (as in third person): the recorder moves a first-person shot's flame and smoke from one to the other
bool CWeapon::PmFireFrames(Fmatrix& hud, Fmatrix& world)
{
    if (!GetHUDmode() || portal_mod::cine || !H_Parent())
        return false;
    UpdateFireDependencies_internal();
    hud = m_current_firedeps.m_FireParticlesXForm;
    hud.c = m_current_firedeps.vLastFP;
    world = XFORM();
    XFORM().transform_tiny(world.c, vLoadedFirePoint);
    return _valid(hud) && _valid(world);
}

namespace
{
using pm_replay::Tracer;

constexpr u32 F_VISIBLE = 1, F_EYE = 2;
constexpr u32 PS_STOPPED = 1, PS_XFORM = 2;
constexpr u32 L_ON = 1, L_SHADOW = 2, L_VOLUMETRIC = 4;
constexpr u8 K_OTHER = 0, K_CREATURE = 1, K_ACTOR = 2;
constexpr float TELEPORT = 8.f; // no interpolation over a jump this long (a teleport, a portal)
constexpr u32 TAKE_VERSION = 3; // 2: E53b - exact particles (seeds, the frames' ms), bullets' paths, shader time, wind;
                                // 3: E53c - every bullet's ammo (version 2 takes open too, their bullets the default)
constexpr u32 PS_Q = CParticlesObject::PM_PS_QUANTUM; // a live particle system's step, ms (E53b)

double now_sec() { return std::chrono::duration<double>(Device.GetTimerGlobal()->getElapsedTime()).count(); }

//------------------------------------------------------------------------------------------------------------------
// the record
struct BoneQ
{
    s16 q[4]; // x y z w, normalized, w >= 0
    Fvector p;
};

struct ObjKey
{
    double t;
    u32 flags;
    Fvector pos;
    float q[4]; // x y z w
    float scale;
    u64 vis[2];
    Fvector eye_pos;
    s16 eye_dir[3];
    u16 pad;
};

struct ObjTrack
{
    u16 id{}, parent{u16(-1)};
    u8 kind{};
    u16 nbones{};
    shared_str visual, name;
    xr_vector<ObjKey> keys;
    xr_vector<BoneQ> bones; // nbones per key
    // the recorder's
    double pending{-1.0};
    u32 seen{};
    bool open{};
};

struct PsKey
{
    double t;
    u32 flags;
    Fmatrix parent, rx;
    Fvector vel;
};

struct PsTrack
{
    u32 serial{};
    shared_str name;
    double born{}, end{};
    u8 looped{};
    u32 born_ms{}, seed{}; // E53b: Device.dwTimeGlobal at its Play and the seed of its random sequence
    xr_vector<PsKey> keys;
    double pending{-1.0};
    u32 seen{};
    bool open{};
};

struct SndKey
{
    double t;
    Fvector pos;
    float vol, freq, mn, mx;
};

struct SndTrack
{
    const void* id{};
    shared_str name;
    double t0{}, end{};
    u8 looped{}, b2D{};
    xr_vector<SndKey> keys;
    double pending{-1.0};
    u32 seen{};
    bool open{};
};

struct LightKey
{
    double t;
    u32 flags;
    int type;
    Fvector pos, dir, right;
    float range, cone, vsize;
    Fcolor color;
};

struct LightTrack
{
    const void* id{};
    xr_vector<LightKey> keys;
    double pending{-1.0};
    u32 seen{};
    bool open{};
};

struct Sample
{
    double t;
    float env;
    u16 weather, pad;
    u32 ms; // E53b: Device.dwTimeGlobal of the frame (the particles' quanta count in it)
    float shader_t; // E53b: the shaders' time (Device.fTimeGlobal): water, fire, flags as in the take
    Fvector wind; // E53b: CEnvironment::wind_anim (the grass's sway)
    PORTAL_MOD_DRAW pd[2];
};

// E53b: a bullet's path. The points carry the bullet's own flight time (ms of the bullet manager's steps) - the frames'
// positions (whole steps), the corners of ricochets, a portal's way in and out (the same time) and where it ended. The
// replay maps that time onto its own so that the bullet leaves the muzzle at the shot and reaches its end when the hit
// shows (the hit is committed at the next frame's start).
struct BulletPt
{
    float tau;
    Fvector pos;
    float speed;
    u32 flags; // pm_replay::BP_*
};

struct BulletTrack
{
    u32 id{};
    double born{}, end{-1.0}; // end: the frame its end was seen (-1: still flying)
    u8 tracer{}, color{};
    Fvector vis_from{}; // where it is seen to start: a first-person shot leaves the world model's muzzle, not the eye
    shared_str ammo; // E53c: the cartridge's section (empty in a version 2 take)
    xr_vector<BulletPt> pts;
    // the recorder's
    u32 seen{};
    bool open{};
    // the player's (bullets_prepare): the way flown up to every point, and the anchors (the bullet's time, the take's)
    xr_vector<float> cum;
    xr_vector<std::pair<float, double>> anch;
    shared_str model; // E53c: its model for the bullet camera ([pm_replay_bullets]; empty: none, only the glow)
    float twist{}; // E53c: the barrel's twist, m of the way per turn (0: no spin)

    float tau_end() const { return pts.empty() ? 0.f : pts.back().tau; }
    // the bullet's time at the replay's time t (< 0: not yet; > tau_end: gone)
    float tau_at(double t) const
    {
        if (anch.size() >= 2)
        {
            if (t <= anch.front().second)
                return float((t - anch.front().second) * 1000.0);
            for (size_t k = 1; k < anch.size(); ++k)
                if (t <= anch[k].second)
                {
                    const auto &a = anch[k - 1], &b = anch[k];
                    return a.first + float((t - a.second) / (b.second - a.second)) * (b.first - a.first);
                }
            return tau_end() + float(t - anch.back().second) * 1000.f;
        }
        const float te = tau_end();
        if (end > born && te > 0.f)
            return float((t - born) / (end - born)) * te;
        return float(t - born) * 1000.f;
    }
};

struct Take
{
    shared_str level;
    double t0{}, t1{};
    xr_vector<shared_str> weathers;
    xr_vector<Sample> samples;
    xr_vector<ObjTrack> objs;
    xr_vector<PsTrack> ps;
    xr_vector<SndTrack> snd;
    xr_vector<LightTrack> lights;
    xr_vector<BulletTrack> bullets;

    void clear()
    {
        level = nullptr;
        t0 = t1 = 0.0;
        weathers.clear();
        samples.clear();
        objs.clear();
        ps.clear();
        snd.clear();
        lights.clear();
        bullets.clear();
    }
    u16 weather_index(const shared_str& w)
    {
        for (u32 i = 0; i < weathers.size(); ++i)
            if (weathers[i] == w)
                return u16(i);
        weathers.push_back(w);
        return u16(weathers.size() - 1);
    }
};

// the index of the last element with .t <= t (0 when all are later)
template <typename T>
size_t key_at(const xr_vector<T>& v, double t)
{
    const auto it = std::upper_bound(v.begin(), v.end(), t, [](double x, const T& k) { return x < k.t; });
    return it == v.begin() ? 0 : size_t(it - v.begin()) - 1;
}

//------------------------------------------------------------------------------------------------------------------
// maths
s16 q16(float v)
{
    int i = iFloor(v * 32767.f + 0.5f);
    clamp(i, -32767, 32767);
    return s16(i);
}

void quat_of(const Fmatrix& m_in, float* q, float* scale)
{
    Fmatrix m = m_in;
    const float si = m.i.magnitude(), sj = m.j.magnitude(), sk = m.k.magnitude();
    if (scale)
        *scale = si;
    if (si > EPS && sj > EPS && sk > EPS)
    {
        m.i.div(si);
        m.j.div(sj);
        m.k.div(sk);
    }
    Fquaternion Q;
    Q.set(m);
    float n = _sqrt(Q.x * Q.x + Q.y * Q.y + Q.z * Q.z + Q.w * Q.w);
    if (n < EPS)
    {
        Q.x = Q.y = Q.z = 0.f;
        Q.w = n = 1.f;
    }
    const float s = (Q.w < 0.f ? -1.f : 1.f) / n;
    q[0] = Q.x * s;
    q[1] = Q.y * s;
    q[2] = Q.z * s;
    q[3] = Q.w * s;
}

void quat_lerp(const float* a, const float* b, float k, Fquaternion& out)
{
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float s = d < 0.f ? -1.f : 1.f;
    float r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = a[i] + (b[i] * s - a[i]) * k;
    float n = _sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (n < EPS)
    {
        r[0] = r[1] = r[2] = 0.f;
        r[3] = n = 1.f;
    }
    out.x = r[0] / n;
    out.y = r[1] / n;
    out.z = r[2] / n;
    out.w = r[3] / n;
}

void make_xform(Fmatrix& m, const Fquaternion& Q, const Fvector& p, float s)
{
    m.rotation(Q);
    if (!fsimilar(s, 1.f, 1e-4f))
    {
        m.i.mul(s);
        m.j.mul(s);
        m.k.mul(s);
    }
    m._14_ = m._24_ = m._34_ = 0.f;
    m._44_ = 1.f;
    m.c = p;
}

void bone_quant(const Fmatrix& m, BoneQ& b)
{
    float q[4];
    quat_of(m, q, nullptr);
    for (int i = 0; i < 4; ++i)
        b.q[i] = q16(q[i]);
    b.p = m.c;
}

void bone_lerp(const BoneQ& a, const BoneQ& b, float k, Fmatrix& m)
{
    float qa[4], qb[4];
    for (int i = 0; i < 4; ++i)
    {
        qa[i] = a.q[i] / 32767.f;
        qb[i] = b.q[i] / 32767.f;
    }
    Fquaternion Q;
    quat_lerp(qa, qb, k, Q);
    Fvector p;
    p.lerp(a.p, b.p, k);
    make_xform(m, Q, p, 1.f);
}

Fvector dir_of(float h, float p)
{
    Fvector d;
    d.setHP(h, p);
    return d;
}

//------------------------------------------------------------------------------------------------------------------
// messages on the screen (the REC line and the replay's status)
xr_string g_msg;
u32 g_msg_until{};

void message(LPCSTR text)
{
    g_msg = text;
    g_msg_until = Device.dwTimeContinual + 4000;
    Msg("[portal_mod] replay: %s", text);
}

//------------------------------------------------------------------------------------------------------------------
// the recorder
struct Recorder
{
    Take ring;
    xr_map<u16, u32> obj_of;
    xr_map<u32, u32> ps_of;
    xr_map<const void*, u32> snd_of, light_of;
    xr_map<u32, u32> bul_of; // E53b
    double last_t{-1.0}, last_trim{};
    u32 sample_no{};
    bool manual{};
    double manual_t0{};
    float ring_seconds{60.f}, radius{200.f}, rate{60.f}, max_len{600.f};
    float bullet_rate{0.25f}; // E53b: at this speed of the replay and slower every bullet is drawn
    float bullet_scale{1.f}; // E53c: the bullets' models' scale
    float bullet_models{30.f}; // E53c: a bullet's model is drawn this near the camera (m), farther only its glow
    bool cfg{};
    xr_vector<PORTAL_MOD_SOUND_SNAP> snd_tmp;
    xr_vector<PORTAL_MOD_LIGHT_SNAP> light_tmp;
    xr_vector<BoneQ> bones_tmp;

    void clear()
    {
        ring.clear();
        obj_of.clear();
        ps_of.clear();
        snd_of.clear();
        light_of.clear();
        bul_of.clear();
        last_t = -1.0;
        last_trim = 0.0;
    }
} R;

// E53b: what the bullet manager reports between two samples (its worker thread too), drained at every sample
struct BulletEvent
{
    u32 id;
    u32 born_ms; // a new bullet: Device.dwTimeGlobal (0: a point of its path)
    BulletPt pt;
    u8 tracer, color;
    Fvector vis_from;
    shared_str ammo; // E53c
};
std::mutex g_bul_lock;
xr_vector<BulletEvent> g_bul_events, g_bul_tmp;
std::atomic<u32> g_bul_next{};
std::atomic<bool> g_bul_on{}; // the recorder takes bullets (set by the recorder's frame, read by the bullet manager)

shared_str g_last; // the last take saved or opened

void load_cfg()
{
    if (R.cfg)
        return;
    R.cfg = true;
    if (!pSettings->section_exist("pm_replay"))
        return;
    auto rf = [](LPCSTR k, float& v) {
        if (pSettings->line_exist("pm_replay", k))
            v = pSettings->r_float("pm_replay", k);
    };
    rf("ring_seconds", R.ring_seconds);
    rf("radius", R.radius);
    rf("rate", R.rate);
    rf("max_seconds", R.max_len);
    rf("bullets", R.bullet_rate);
    rf("bullet_scale", R.bullet_scale);
    rf("bullet_models", R.bullet_models);
    clamp(R.bullet_scale, 0.1f, 20.f);
    clamp(R.bullet_models, 0.f, 500.f);
    clamp(R.ring_seconds, 0.f, 600.f);
    clamp(R.bullet_rate, 0.f, 4.f);
    clamp(R.radius, 10.f, 2000.f);
    clamp(R.rate, 10.f, 120.f);
    clamp(R.max_len, 10.f, 3600.f);
}

// hold semantics: a key only on a change; a still stretch before the change ends with its last state, at the time it
// was last seen
template <typename Track, typename Key, typename Same>
void record(Track& tr, const Key& k, Same same)
{
    if (!tr.keys.empty())
    {
        if (same(tr.keys.back(), k))
        {
            tr.pending = k.t;
            return;
        }
        if (tr.pending > tr.keys.back().t)
        {
            Key h = tr.keys.back();
            h.t = tr.pending;
            tr.keys.push_back(h);
        }
    }
    tr.keys.push_back(k);
    tr.pending = -1.0;
}

bool same_obj(const ObjKey& a, const ObjKey& b)
{
    if (a.flags != b.flags || a.vis[0] != b.vis[0] || a.vis[1] != b.vis[1])
        return false;
    if (!(a.flags & F_VISIBLE))
        return true;
    if (a.pos.distance_to_sqr(b.pos) > 1e-7f)
        return false;
    if (_abs(a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3]) < 0.9999995f)
        return false;
    if (!fsimilar(a.scale, b.scale, 1e-4f))
        return false;
    if (a.flags & F_EYE)
    {
        if (a.eye_pos.distance_to_sqr(b.eye_pos) > 1e-6f)
            return false;
        for (int i = 0; i < 3; ++i)
            if (_abs(int(a.eye_dir[i]) - int(b.eye_dir[i])) > 2)
                return false;
    }
    return true;
}

void record_obj(ObjTrack& tr, const ObjKey& k, const BoneQ* bones)
{
    const u32 nb = tr.nbones;
    if (!tr.keys.empty())
    {
        const ObjKey& last = tr.keys.back();
        const BoneQ* lb = nb ? &tr.bones[tr.bones.size() - nb] : nullptr;
        bool same = same_obj(last, k);
        if (same && nb && (k.flags & F_VISIBLE) && bones)
            same = 0 == memcmp(lb, bones, sizeof(BoneQ) * nb);
        if (same)
        {
            tr.pending = k.t;
            return;
        }
        if (tr.pending > last.t)
        {
            ObjKey h = last;
            h.t = tr.pending;
            tr.keys.push_back(h);
            if (nb)
            {
                const size_t ofs = tr.bones.size() - nb;
                tr.bones.resize(tr.bones.size() + nb);
                memcpy(&tr.bones[ofs + nb], &tr.bones[ofs], sizeof(BoneQ) * nb);
            }
        }
    }
    tr.keys.push_back(k);
    if (nb)
    {
        const size_t ofs = tr.bones.size();
        tr.bones.resize(ofs + nb);
        if (bones)
            memcpy(&tr.bones[ofs], bones, sizeof(BoneQ) * nb);
        else if (ofs >= nb)
            memcpy(&tr.bones[ofs], &tr.bones[ofs - nb], sizeof(BoneQ) * nb);
        else
            memset(&tr.bones[ofs], 0, sizeof(BoneQ) * nb);
    }
    tr.pending = -1.0;
}

void obj_hide(ObjTrack& tr, double t)
{
    if (tr.keys.empty() || !(tr.keys.back().flags & F_VISIBLE))
    {
        if (!tr.keys.empty())
            tr.pending = t;
        return;
    }
    ObjKey k = tr.keys.back();
    k.t = t;
    k.flags = 0;
    record_obj(tr, k, nullptr);
}

void sample_object(CObject* O, u8 kind, u16 parent, double t)
{
    if (!O || O->getDestroy())
        return;
    IRenderVisual* vis = O->Visual();
    if (!vis || vis->dcast_ParticleCustom() || !O->cNameVisual().size())
        return;
    IKinematics* K = vis->dcast_PKinematics();
    const u16 nb = K ? K->LL_BoneCount() : 0;
    const u16 id = O->ID();
    auto it = R.obj_of.find(id);
    if (it != R.obj_of.end())
    {
        ObjTrack& old = R.ring.objs[it->second];
        if (old.seen == R.sample_no)
            return; // already in this sample (an attachment that is also the item in the hands)
        // a new track for a new model, and for a new holder: a track keeps one parent, and a weapon dropped by the dead
        // (or picked up) must not take its old holder's track with it - the eye view hides what its owner holds
        if (old.visual != O->cNameVisual() || old.nbones != nb || old.parent != parent)
        {
            obj_hide(old, t);
            old.open = false;
            R.obj_of.erase(it);
            it = R.obj_of.end();
        }
    }
    u32 ti;
    if (it == R.obj_of.end())
    {
        ObjTrack tr;
        tr.id = id;
        tr.parent = parent;
        tr.kind = kind;
        tr.nbones = nb;
        tr.visual = O->cNameVisual();
        tr.name = O->cName();
        tr.open = true;
        R.ring.objs.push_back(std::move(tr));
        ti = u32(R.ring.objs.size() - 1);
        R.obj_of[id] = ti;
    }
    else
        ti = it->second;
    ObjTrack& tr = R.ring.objs[ti];
    tr.seen = R.sample_no;
    tr.parent = parent;

    if (K)
        K->CalculateBones(TRUE); // off screen too: the replay looks from anywhere
    ObjKey k{};
    k.t = t;
    k.flags = F_VISIBLE;
    k.pos = O->XFORM().c;
    quat_of(O->XFORM(), k.q, &k.scale);
    if (K)
    {
        const VisMask m = K->LL_GetBonesVisible();
        k.vis[0] = m._visimask.flags;
        k.vis[1] = m._visimask_ex.flags;
    }
    Fvector ep{}, ed{};
    bool eye = false;
    if (kind == K_ACTOR)
    {
        CActor* A = smart_cast<CActor*>(O);
        if (A && A->cam_FirstEye())
        {
            ep = A->cam_FirstEye()->vPosition;
            ed = A->cam_FirstEye()->vDirection;
            eye = true;
        }
    }
    else if (kind == K_CREATURE)
    {
        CCustomMonster* M = smart_cast<CCustomMonster*>(O);
        if (M && M->g_Alive())
        {
            M->PmEye(ep, ed);
            eye = true;
        }
    }
    if (eye && _valid(ep) && _valid(ed) && ed.square_magnitude() > EPS)
    {
        ed.normalize();
        k.flags |= F_EYE;
        k.eye_pos = ep;
        k.eye_dir[0] = q16(ed.x);
        k.eye_dir[1] = q16(ed.y);
        k.eye_dir[2] = q16(ed.z);
    }
    if (nb)
    {
        R.bones_tmp.resize(nb);
        for (u16 i = 0; i < nb; ++i)
            bone_quant(K->LL_GetTransform(i), R.bones_tmp[i]);
    }
    record_obj(tr, k, nb ? R.bones_tmp.data() : nullptr);
}

void sample_children(CObject* O, double t)
{
    const u16 pid = O->ID();
    if (CInventoryOwner* io = smart_cast<CInventoryOwner*>(O))
    {
        bool hands = true;
        if (CActor* A = smart_cast<CActor*>(O))
            hands = !A->Holder();
        else if (CEntityAlive* E = smart_cast<CEntityAlive*>(O))
            hands = !!E->g_Alive();
        if (hands)
            if (PIItem item = io->inventory().ActiveItem())
            {
                item->PmUpdateXForm(); // UpdateXForm runs from UpdateCL; this frame's pose of the hand, not the last one
                sample_object(&item->object(), K_OTHER, pid, t);
            }
    }
    if (CAttachmentOwner* ao = smart_cast<CAttachmentOwner*>(O))
        for (CAttachableItem* a : ao->attached_objects())
            if (a && a->enabled())
                sample_object(&a->item().object(), K_OTHER, pid, t);
}

bool same_ps(const PsKey& a, const PsKey& b)
{
    return a.flags == b.flags && 0 == memcmp(&a.parent, &b.parent, sizeof(Fmatrix)) && 0 == memcmp(&a.rx, &b.rx, sizeof(Fmatrix)) &&
        a.vel.similar(b.vel, 1e-4f);
}

bool same_snd(const SndKey& a, const SndKey& b)
{
    return a.pos.similar(b.pos, 1e-3f) && fsimilar(a.vol, b.vol, 1e-3f) && fsimilar(a.freq, b.freq, 1e-3f) && fsimilar(a.mn, b.mn, 1e-3f) &&
        fsimilar(a.mx, b.mx, 1e-3f);
}

bool same_light(const LightKey& a, const LightKey& b)
{
    if (a.flags != b.flags)
        return false;
    if (!(a.flags & L_ON))
        return true;
    return a.type == b.type && a.pos.similar(b.pos, 1e-3f) && a.dir.similar(b.dir, 1e-3f) && a.right.similar(b.right, 1e-3f) && fsimilar(a.range, b.range, 1e-3f) &&
        fsimilar(a.cone, b.cone, 1e-4f) && fsimilar(a.vsize, b.vsize, 1e-3f) && fsimilar(a.color.r, b.color.r, 1e-3f) && fsimilar(a.color.g, b.color.g, 1e-3f) &&
        fsimilar(a.color.b, b.color.b, 1e-3f);
}

void rebuild_maps()
{
    R.obj_of.clear();
    R.ps_of.clear();
    R.snd_of.clear();
    R.light_of.clear();
    for (u32 i = 0; i < R.ring.objs.size(); ++i)
        if (R.ring.objs[i].open)
            R.obj_of[R.ring.objs[i].id] = i;
    for (u32 i = 0; i < R.ring.ps.size(); ++i)
        if (R.ring.ps[i].open)
            R.ps_of[R.ring.ps[i].serial] = i;
    for (u32 i = 0; i < R.ring.snd.size(); ++i)
        if (R.ring.snd[i].open)
            R.snd_of[R.ring.snd[i].id] = i;
    for (u32 i = 0; i < R.ring.lights.size(); ++i)
        if (R.ring.lights[i].open)
            R.light_of[R.ring.lights[i].id] = i;
    R.bul_of.clear();
    for (u32 i = 0; i < R.ring.bullets.size(); ++i)
        if (R.ring.bullets[i].open)
            R.bul_of[R.ring.bullets[i].id] = i;
}

// drops what is older than `cut`, keeping the last key at or before it (the state at the cut)
void trim(double cut)
{
    Take& T = R.ring;
    if (cut <= T.t0)
        return;
    T.t0 = cut;
    if (!T.samples.empty())
    {
        const size_t n = key_at(T.samples, cut);
        if (n)
            T.samples.erase(T.samples.begin(), T.samples.begin() + n);
    }
    T.bullets.erase(std::remove_if(T.bullets.begin(), T.bullets.end(), [cut](const BulletTrack& tr) { return !tr.open && tr.end < cut; }),
                    T.bullets.end());
    for (ObjTrack& tr : T.objs)
    {
        if (tr.keys.empty())
            continue;
        const size_t n = key_at(tr.keys, cut);
        if (n)
        {
            tr.keys.erase(tr.keys.begin(), tr.keys.begin() + n);
            if (tr.nbones)
                tr.bones.erase(tr.bones.begin(), tr.bones.begin() + n * tr.nbones);
        }
    }
    T.objs.erase(std::remove_if(T.objs.begin(), T.objs.end(),
                                [cut](const ObjTrack& tr) {
                                    return tr.keys.empty() ||
                                        (tr.keys.size() == 1 && !(tr.keys[0].flags & F_VISIBLE) && tr.keys[0].t <= cut && tr.pending <= cut);
                                }),
                 T.objs.end());
    for (PsTrack& tr : T.ps)
    {
        const size_t n = tr.keys.empty() ? 0 : key_at(tr.keys, cut);
        if (n)
            tr.keys.erase(tr.keys.begin(), tr.keys.begin() + n);
    }
    T.ps.erase(std::remove_if(T.ps.begin(), T.ps.end(), [cut](const PsTrack& tr) { return tr.keys.empty() || (!tr.open && tr.end < cut); }), T.ps.end());
    for (SndTrack& tr : T.snd)
    {
        const size_t n = tr.keys.empty() ? 0 : key_at(tr.keys, cut);
        if (n)
            tr.keys.erase(tr.keys.begin(), tr.keys.begin() + n);
    }
    T.snd.erase(std::remove_if(T.snd.begin(), T.snd.end(), [cut](const SndTrack& tr) { return tr.keys.empty() || (!tr.open && tr.end < cut); }), T.snd.end());
    for (LightTrack& tr : T.lights)
    {
        const size_t n = tr.keys.empty() ? 0 : key_at(tr.keys, cut);
        if (n)
            tr.keys.erase(tr.keys.begin(), tr.keys.begin() + n);
    }
    T.lights.erase(std::remove_if(T.lights.begin(), T.lights.end(),
                                  [cut](const LightTrack& tr) {
                                      return tr.keys.empty() || (tr.keys.size() == 1 && !(tr.keys[0].flags & L_ON) && tr.pending <= cut);
                                  }),
                   T.lights.end());
    rebuild_maps();
}

void recorder_frame()
{
    load_cfg();
    if (!R.manual && R.ring_seconds <= 0.f)
    {
        g_bul_on = false;
        if (!R.ring.samples.empty())
            R.clear();
        return;
    }
    if (!g_pGameLevel || !g_pGameLevel->bReady)
        return;
    g_bul_on = true;
    const double t = now_sec();
    if (R.last_t >= 0.0 && t < R.last_t)
        R.clear(); // the timer went back: drop it all
    if (R.last_t >= 0.0 && t - R.last_t < 0.9 / R.rate)
        return;
    R.last_t = t;
    ++R.sample_no;
    Take& T = R.ring;
    if (T.samples.empty())
    {
        T.t0 = t;
        T.level = Level().name();
    }
    T.t1 = t;

    CActor* actor = Actor();
    const Fvector center = actor ? actor->Position() : Device.vCameraPosition;
    const float r2 = R.radius * R.radius;

    // objects: the roots, then what they hold or carry
    CObjectList& L = Level().Objects;
    const u32 n = L.o_count();
    for (u32 i = 0; i < n; ++i)
    {
        CObject* O = L.o_get_by_iterator(i);
        if (!O || O->getDestroy() || O->H_Parent())
            continue;
        u8 kind = K_OTHER;
        if (O == actor)
        {
            CHolderCustom* h = actor->Holder();
            if (h && !smart_cast<CCar*>(h))
                continue; // in a helicopter (or a mounted gun) the player is not drawn
            kind = K_ACTOR; // in first person not "visible", drawn in the replay anyway
        }
        else
        {
            if (!O->getVisible())
                continue;
            if (smart_cast<CCustomMonster*>(O))
                kind = K_CREATURE;
        }
        if (O->Position().distance_to_sqr(center) > r2)
            continue;
        sample_object(O, kind, u16(-1), t);
        sample_children(O, t);
    }
    for (ObjTrack& tr : T.objs)
        if (tr.open && tr.seen != R.sample_no)
            obj_hide(tr, t);

    // particle systems; the HUD's (a first-person shot's flame and smoke) moved from the HUD's muzzle to the world
    // model's (E53b)
    bool hud_ok = false;
    Fmatrix hud_map;
    if (actor)
        if (CWeapon* w = smart_cast<CWeapon*>(actor->inventory().ActiveItem()))
        {
            Fmatrix H, W, inv;
            if (w->PmFireFrames(H, W))
            {
                inv.invert(H);
                hud_map.mul_43(W, inv);
                hud_ok = true;
            }
        }
    for (CPS_Instance* psi : g_pGamePersistent->ps_active)
    {
        CParticlesObject* po = smart_cast<CParticlesObject*>(psi);
        if (!po || !po->pm_born || (po->pm_hud && !hud_ok) || !po->renderable.visual)
            continue; // (pm_born 0: never played)
        IParticleCustom* V = smart_cast<IParticleCustom*>(po->renderable.visual);
        if (!V)
            continue;
        Fmatrix parent = po->pm_parent, rx = po->renderable.xform;
        Fvector vel = po->pm_vel;
        if (po->pm_hud)
        {
            parent.mul_43(hud_map, po->pm_parent);
            rx.mul_43(hud_map, po->renderable.xform);
            hud_map.transform_dir(vel, po->pm_vel);
        }
        if (parent.c.distance_to_sqr(center) > r2)
            continue;
        auto it = R.ps_of.find(po->pm_serial);
        if (it != R.ps_of.end() && T.ps[it->second].born_ms != po->pm_born) // played again: a track of its own (E53b)
        {
            T.ps[it->second].open = false;
            R.ps_of.erase(it);
            it = R.ps_of.end();
        }
        u32 ti;
        if (it == R.ps_of.end())
        {
            PsTrack tr;
            tr.serial = po->pm_serial;
            tr.name = V->Name();
            const double age = po->pm_born ? double(Device.dwTimeGlobal - po->pm_born) / 1000.0 : 0.0;
            tr.born = t - (age > 0.0 ? age : 0.0);
            tr.looped = V->IsLooped() ? 1 : 0;
            tr.born_ms = po->pm_born;
            tr.seed = po->pm_seed;
            tr.open = true;
            T.ps.push_back(std::move(tr));
            ti = u32(T.ps.size() - 1);
            R.ps_of[po->pm_serial] = ti;
        }
        else
            ti = it->second;
        PsTrack& tr = T.ps[ti];
        tr.seen = R.sample_no;
        tr.end = t;
        PsKey k;
        k.t = t;
        k.flags = (V->IsDeferredStopped() ? PS_STOPPED : 0) | (po->pm_xform ? PS_XFORM : 0);
        k.parent = parent;
        k.rx = rx;
        k.vel = vel;
        record(tr, k, same_ps);
    }
    for (PsTrack& tr : T.ps)
        if (tr.open && tr.seen != R.sample_no)
            tr.open = false;

    // sounds
    R.snd_tmp.clear();
    ::Sound->portal_mod_snapshot(R.snd_tmp);
    for (const PORTAL_MOD_SOUND_SNAP& s : R.snd_tmp)
    {
        if (!s.name.size() || (!s.b2D && s.pos.distance_to_sqr(center) > r2))
            continue;
        const double t0 = t - double(s.time);
        auto it = R.snd_of.find(s.id);
        if (it != R.snd_of.end())
        {
            SndTrack& old = T.snd[it->second];
            if (old.name != s.name || (!old.looped && _abs(old.t0 - t0) > 0.3))
            {
                old.open = false;
                R.snd_of.erase(it);
                it = R.snd_of.end();
            }
        }
        u32 ti;
        if (it == R.snd_of.end())
        {
            SndTrack tr;
            tr.id = s.id;
            tr.name = s.name;
            tr.t0 = t0;
            tr.looped = s.looped ? 1 : 0;
            tr.b2D = s.b2D ? 1 : 0;
            tr.open = true;
            T.snd.push_back(std::move(tr));
            ti = u32(T.snd.size() - 1);
            R.snd_of[s.id] = ti;
        }
        else
            ti = it->second;
        SndTrack& tr = T.snd[ti];
        tr.seen = R.sample_no;
        tr.end = t;
        SndKey k{t, s.pos, s.volume, s.freq, s.min_dist, s.max_dist};
        record(tr, k, same_snd);
    }
    for (SndTrack& tr : T.snd)
        if (tr.open && tr.seen != R.sample_no)
            tr.open = false;

    // lights (not the static ones: they are in the level)
    R.light_tmp.clear();
    ::Render->portal_mod_lights(R.light_tmp, center, R.radius);
    for (const PORTAL_MOD_LIGHT_SNAP& l : R.light_tmp)
    {
        auto it = R.light_of.find(l.id);
        u32 ti;
        if (it == R.light_of.end())
        {
            LightTrack tr;
            tr.id = l.id;
            tr.open = true;
            T.lights.push_back(std::move(tr));
            ti = u32(T.lights.size() - 1);
            R.light_of[l.id] = ti;
        }
        else
            ti = it->second;
        LightTrack& tr = T.lights[ti];
        tr.seen = R.sample_no;
        LightKey k;
        k.t = t;
        k.flags = L_ON | (l.shadow ? L_SHADOW : 0) | (l.volumetric ? L_VOLUMETRIC : 0);
        k.type = l.type;
        k.pos = l.position;
        k.dir = l.direction;
        k.right = l.right;
        k.range = l.range;
        k.cone = l.cone;
        k.vsize = l.virtual_size;
        k.color = l.color;
        record(tr, k, same_light);
    }
    for (LightTrack& tr : T.lights)
        if (tr.open && tr.seen != R.sample_no && !tr.keys.empty() && (tr.keys.back().flags & L_ON))
        {
            LightKey k = tr.keys.back();
            k.t = t;
            k.flags = 0;
            record(tr, k, same_light);
        }

    // bullets (E53b): what the bullet manager reported since the last sample (new ones, corners, portals, ends - a
    // bullet that ended in the last frame's steps shows its hit in this frame), then where each one is now
    {
        std::lock_guard<std::mutex> lk(g_bul_lock);
        g_bul_tmp.swap(g_bul_events);
    }
    for (const BulletEvent& e : g_bul_tmp)
    {
        if (e.born_ms)
        {
            if (e.pt.pos.distance_to_sqr(center) > r2)
                continue;
            BulletTrack tr;
            tr.id = e.id;
            tr.born = t - double(Device.dwTimeGlobal - e.born_ms) / 1000.0;
            tr.tracer = e.tracer;
            tr.color = e.color;
            tr.vis_from = e.vis_from;
            tr.ammo = e.ammo;
            tr.open = true;
            tr.seen = R.sample_no;
            tr.pts.push_back(e.pt);
            T.bullets.push_back(std::move(tr));
            R.bul_of[e.id] = u32(T.bullets.size() - 1);
            continue;
        }
        auto it = R.bul_of.find(e.id);
        if (it == R.bul_of.end())
            continue;
        BulletTrack& tr = T.bullets[it->second];
        tr.seen = R.sample_no;
        if (!tr.pts.empty() && e.pt.tau < tr.pts.back().tau)
            continue;
        tr.pts.push_back(e.pt);
        if (e.pt.flags & pm_replay::BP_END)
        {
            tr.end = t;
            tr.open = false;
            R.bul_of.erase(it);
        }
    }
    g_bul_tmp.clear();
    for (const SBullet& b : Level().BulletManager().pm_rendered())
    {
        if (!b.pm_id)
            continue;
        auto it = R.bul_of.find(b.pm_id);
        if (it == R.bul_of.end())
            continue;
        BulletTrack& tr = T.bullets[it->second];
        tr.seen = R.sample_no;
        const float tau = float(b.pm_tau);
        if (!tr.pts.empty() && tau <= tr.pts.back().tau + 0.01f)
            continue;
        tr.pts.push_back(BulletPt{tau, b.pos, b.speed, 0});
    }
    for (BulletTrack& tr : T.bullets)
        if (tr.open && tr.seen != R.sample_no) // gone without an end (the level cleared the bullets): ends where it was
        {
            tr.open = false;
            tr.end = t;
            R.bul_of.erase(tr.id);
        }

    // the frame's own state: the time of day, the weather, the clocks, the wind, the portals
    Sample s{};
    s.t = t;
    s.env = Level().GetEnvironmentGameDayTimeSec();
    s.weather = T.weather_index(g_pGamePersistent->Environment().GetWeather());
    s.ms = Device.dwTimeGlobal;
    s.shader_t = Device.fTimeGlobal;
    s.wind = g_pGamePersistent->Environment().wind_anim;
    s.pd[0] = portal_mod_draw[0];
    s.pd[1] = portal_mod_draw[1];
    T.samples.push_back(s);

    // the ring
    if (t - R.last_trim > 1.0)
    {
        R.last_trim = t;
        double cut = R.ring_seconds > 0.f ? t - R.ring_seconds : t;
        if (R.manual)
            cut = std::min(cut, R.manual_t0);
        cut = std::max(cut, t - R.max_len);
        trim(cut);
    }
}

//------------------------------------------------------------------------------------------------------------------
// takes: a stretch of the ring with its times from 0
void extract(const Take& S, double a, double b, Take& D)
{
    D.clear();
    D.level = S.level;
    D.t0 = 0.0;
    D.t1 = b - a;
    D.weathers = S.weathers;
    if (S.samples.empty())
        return;
    for (size_t i = key_at(S.samples, a); i < S.samples.size() && S.samples[i].t <= b + 1e-6; ++i)
    {
        Sample s = S.samples[i];
        s.t = std::max(0.0, s.t - a);
        D.samples.push_back(s);
    }
    for (const ObjTrack& tr : S.objs)
    {
        if (tr.keys.empty() || tr.keys.front().t > b)
            continue;
        ObjTrack d;
        d.id = tr.id;
        d.parent = tr.parent;
        d.kind = tr.kind;
        d.nbones = tr.nbones;
        d.visual = tr.visual;
        d.name = tr.name;
        bool any_visible = false;
        for (size_t i = key_at(tr.keys, a); i < tr.keys.size() && tr.keys[i].t <= b + 1e-6; ++i)
        {
            ObjKey k = tr.keys[i];
            k.t = std::max(0.0, k.t - a);
            if (!d.keys.empty() && k.t <= d.keys.back().t && i) // the key before the start and one at it: the later one
            {
                d.keys.pop_back();
                if (d.nbones)
                    d.bones.resize(d.bones.size() - d.nbones);
            }
            d.keys.push_back(k);
            if (d.nbones)
                d.bones.insert(d.bones.end(), tr.bones.begin() + i * d.nbones, tr.bones.begin() + (i + 1) * d.nbones);
            any_visible |= !!(k.flags & F_VISIBLE);
        }
        if (!any_visible || d.keys.empty())
            continue;
        if (tr.pending > tr.keys.back().t && tr.pending <= b + 1e-6 && tr.keys.back().t >= a) // the still stretch at the end
        {
            ObjKey k = d.keys.back();
            k.t = tr.pending - a;
            d.keys.push_back(k);
            if (d.nbones)
            {
                const size_t ofs = d.bones.size() - d.nbones;
                d.bones.resize(d.bones.size() + d.nbones);
                memcpy(&d.bones[ofs + d.nbones], &d.bones[ofs], sizeof(BoneQ) * d.nbones);
            }
        }
        D.objs.push_back(std::move(d));
    }
    for (const PsTrack& tr : S.ps)
    {
        if (tr.keys.empty() || tr.born > b || (!tr.open && tr.end < a))
            continue;
        PsTrack d;
        d.serial = tr.serial;
        d.name = tr.name;
        d.born = tr.born - a;
        d.end = tr.open ? D.t1 + 1.0 : tr.end - a;
        d.looped = tr.looped;
        d.born_ms = tr.born_ms;
        d.seed = tr.seed;
        for (size_t i = key_at(tr.keys, a); i < tr.keys.size() && tr.keys[i].t <= b + 1e-6; ++i)
        {
            PsKey k = tr.keys[i];
            k.t = std::max(d.born, k.t - a);
            d.keys.push_back(k);
        }
        if (!d.keys.empty())
            D.ps.push_back(std::move(d));
    }
    for (const SndTrack& tr : S.snd)
    {
        if (tr.keys.empty() || tr.t0 > b || (!tr.open && tr.end < a))
            continue;
        SndTrack d;
        d.name = tr.name;
        d.t0 = tr.t0 - a;
        d.end = (tr.open ? b : tr.end) - a;
        d.looped = tr.looped;
        d.b2D = tr.b2D;
        for (size_t i = key_at(tr.keys, a); i < tr.keys.size() && tr.keys[i].t <= b + 1e-6; ++i)
        {
            SndKey k = tr.keys[i];
            k.t = std::max(0.0, k.t - a);
            d.keys.push_back(k);
        }
        if (!d.keys.empty())
            D.snd.push_back(std::move(d));
    }
    for (const LightTrack& tr : S.lights)
    {
        if (tr.keys.empty() || tr.keys.front().t > b)
            continue;
        LightTrack d;
        bool any = false;
        for (size_t i = key_at(tr.keys, a); i < tr.keys.size() && tr.keys[i].t <= b + 1e-6; ++i)
        {
            LightKey k = tr.keys[i];
            k.t = std::max(0.0, k.t - a);
            d.keys.push_back(k);
            any |= !!(k.flags & L_ON);
        }
        if (any)
            D.lights.push_back(std::move(d));
    }
    for (const BulletTrack& tr : S.bullets)
    {
        if (tr.pts.empty() || tr.born > b || (!tr.open && tr.end < a))
            continue;
        BulletTrack d = tr;
        d.born = tr.born - a;
        d.end = tr.open ? -1.0 : tr.end - a;
        d.open = false;
        D.bullets.push_back(std::move(d));
    }
}

template <typename T>
void w_vec(IWriter* W, const xr_vector<T>& v)
{
    W->w_u32(u32(v.size()));
    if (!v.empty())
        W->w(v.data(), v.size() * sizeof(T));
}

template <typename T>
bool r_vec(IReader* F, xr_vector<T>& v)
{
    const u32 n = F->r_u32();
    if (u64(n) * sizeof(T) > u64(F->elapsed()))
        return false;
    v.resize(n);
    if (n)
        F->r(v.data(), n * sizeof(T));
    return true;
}

void file_of(string_path& rel, LPCSTR name, LPCSTR ext) { xr_sprintf(rel, "replays\\%s%s", name, ext); }

bool save_take(const Take& T, LPCSTR name)
{
    string_path rel;
    file_of(rel, name, ".pmr");
    IWriter* W = FS.w_open("$app_data_root$", rel);
    if (!W)
        return false;
    W->w("PMRPLAY1", 8);
    W->w_u32(TAKE_VERSION);
    W->w_stringZ(T.level.size() ? T.level.c_str() : "");
    W->w(&T.t1, sizeof(double));
    W->w_u32(u32(T.weathers.size()));
    for (const shared_str& w : T.weathers)
        W->w_stringZ(w.size() ? w.c_str() : "");
    w_vec(W, T.samples);
    W->w_u32(u32(T.objs.size()));
    for (const ObjTrack& tr : T.objs)
    {
        W->w_u16(tr.id);
        W->w_u16(tr.parent);
        W->w_u8(tr.kind);
        W->w_u16(tr.nbones);
        W->w_stringZ(tr.visual.c_str());
        W->w_stringZ(tr.name.size() ? tr.name.c_str() : "");
        w_vec(W, tr.keys);
        w_vec(W, tr.bones);
    }
    W->w_u32(u32(T.ps.size()));
    for (const PsTrack& tr : T.ps)
    {
        W->w_u32(tr.serial);
        W->w_stringZ(tr.name.c_str());
        W->w(&tr.born, sizeof(double));
        W->w(&tr.end, sizeof(double));
        W->w_u8(tr.looped);
        W->w_u32(tr.born_ms);
        W->w_u32(tr.seed);
        w_vec(W, tr.keys);
    }
    W->w_u32(u32(T.snd.size()));
    for (const SndTrack& tr : T.snd)
    {
        W->w_stringZ(tr.name.c_str());
        W->w(&tr.t0, sizeof(double));
        W->w(&tr.end, sizeof(double));
        W->w_u8(tr.looped);
        W->w_u8(tr.b2D);
        w_vec(W, tr.keys);
    }
    W->w_u32(u32(T.lights.size()));
    for (const LightTrack& tr : T.lights)
        w_vec(W, tr.keys);
    W->w_u32(u32(T.bullets.size()));
    for (const BulletTrack& tr : T.bullets)
    {
        W->w_u32(tr.id);
        W->w(&tr.born, sizeof(double));
        W->w(&tr.end, sizeof(double));
        W->w_u8(tr.tracer);
        W->w_u8(tr.color);
        W->w_fvector3(tr.vis_from);
        W->w_stringZ(tr.ammo.size() ? tr.ammo.c_str() : ""); // E53c
        w_vec(W, tr.pts);
    }
    FS.w_close(W);
    return true;
}

bool load_take(LPCSTR name, Take& T)
{
    string_path rel;
    file_of(rel, name, ".pmr");
    if (!FS.exist("$app_data_root$", rel))
        return false;
    IReader* F = FS.r_open("$app_data_root$", rel);
    if (!F)
        return false;
    T.clear();
    bool ok = false;
    char magic[8];
    if (F->length() > 16)
    {
        F->r(magic, 8);
        const bool pmr = 0 == memcmp(magic, "PMRPLAY1", 8);
        const u32 ver = pmr ? F->r_u32() : 0;
        if (pmr && (ver == TAKE_VERSION || ver == 2)) // E53c: version 2 has no ammo
        {
            ok = true;
            F->r_stringZ(T.level);
            F->r(&T.t1, sizeof(double));
            const u32 nw = F->r_u32();
            for (u32 i = 0; i < nw && ok; ++i)
            {
                shared_str w;
                F->r_stringZ(w);
                T.weathers.push_back(w);
            }
            ok = ok && r_vec(F, T.samples);
            const u32 no = ok ? F->r_u32() : 0;
            for (u32 i = 0; i < no && ok; ++i)
            {
                ObjTrack tr;
                tr.id = F->r_u16();
                tr.parent = F->r_u16();
                tr.kind = F->r_u8();
                tr.nbones = F->r_u16();
                F->r_stringZ(tr.visual);
                F->r_stringZ(tr.name);
                ok = r_vec(F, tr.keys) && r_vec(F, tr.bones) && tr.bones.size() == tr.keys.size() * tr.nbones;
                T.objs.push_back(std::move(tr));
            }
            const u32 np = ok ? F->r_u32() : 0;
            for (u32 i = 0; i < np && ok; ++i)
            {
                PsTrack tr;
                tr.serial = F->r_u32();
                F->r_stringZ(tr.name);
                F->r(&tr.born, sizeof(double));
                F->r(&tr.end, sizeof(double));
                tr.looped = F->r_u8();
                tr.born_ms = F->r_u32();
                tr.seed = F->r_u32();
                ok = r_vec(F, tr.keys);
                T.ps.push_back(std::move(tr));
            }
            const u32 ns = ok ? F->r_u32() : 0;
            for (u32 i = 0; i < ns && ok; ++i)
            {
                SndTrack tr;
                F->r_stringZ(tr.name);
                F->r(&tr.t0, sizeof(double));
                F->r(&tr.end, sizeof(double));
                tr.looped = F->r_u8();
                tr.b2D = F->r_u8();
                ok = r_vec(F, tr.keys);
                T.snd.push_back(std::move(tr));
            }
            const u32 nl = ok ? F->r_u32() : 0;
            for (u32 i = 0; i < nl && ok; ++i)
            {
                LightTrack tr;
                ok = r_vec(F, tr.keys);
                T.lights.push_back(std::move(tr));
            }
            const u32 nb = ok ? F->r_u32() : 0;
            for (u32 i = 0; i < nb && ok; ++i)
            {
                BulletTrack tr;
                tr.id = F->r_u32();
                F->r(&tr.born, sizeof(double));
                F->r(&tr.end, sizeof(double));
                tr.tracer = F->r_u8();
                tr.color = F->r_u8();
                F->r_fvector3(tr.vis_from);
                if (ver >= 3)
                    F->r_stringZ(tr.ammo);
                ok = r_vec(F, tr.pts) && !tr.pts.empty();
                T.bullets.push_back(std::move(tr));
            }
        }
    }
    FS.r_close(F);
    if (!ok)
        Msg("! [portal_mod] replay: %s is not a take of this version", rel);
    return ok;
}

xr_string auto_name()
{
    const time_t now = time(nullptr);
    tm lt{};
    localtime_s(&lt, &now);
    string64 buf;
    xr_sprintf(buf, "take_%04d%02d%02d_%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

// saves [a, b] of the ring; returns the name ("" when there is nothing)
xr_string save_segment(double a, double b, LPCSTR name)
{
    if (R.ring.samples.empty() || b - a < 0.05)
        return "";
    Take T;
    extract(R.ring, a, b, T);
    if (T.samples.empty())
        return "";
    const xr_string nm = (name && *name) ? xr_string(name) : auto_name();
    if (!save_take(T, nm.c_str()))
        return "";
    g_last = nm.c_str();
    string256 m;
    xr_sprintf(m, "%s: %.1f s, %u obj, %u ps, %u snd, %u lights", nm.c_str(), float(T.t1), u32(T.objs.size()), u32(T.ps.size()), u32(T.snd.size()), u32(T.lights.size()));
    Msg("[portal_mod] replay saved %s", m);
    return nm;
}

xr_string save_ring(LPCSTR name)
{
    if (R.ring.samples.empty())
        return "";
    const double b = R.ring.samples.back().t;
    const double a = std::max(R.ring.samples.front().t, R.ring_seconds > 0.f ? b - R.ring_seconds : R.ring.samples.front().t);
    return save_segment(a, b, name);
}

void record_start()
{
    if (R.manual)
        return;
    R.manual = true;
    R.manual_t0 = now_sec();
}

xr_string record_stop()
{
    if (!R.manual)
        return "";
    R.manual = false;
    const double b = R.ring.samples.empty() ? R.manual_t0 : R.ring.samples.back().t;
    return save_segment(R.manual_t0, b, nullptr);
}

//------------------------------------------------------------------------------------------------------------------
// the player: puppets, particles
void pose_cb(CBoneInstance* B) { B->mTransform = *static_cast<const Fmatrix*>(B->callback_param()); }

class Puppet : public ISpatial, public IRenderable
{
public:
    const ObjTrack* tr;
    IKinematics* K{};
    xr_vector<Fmatrix> pose;
    bool in_space{};
    bool has_mask{};
    VisMask mask;

    explicit Puppet(const ObjTrack* t) : ISpatial(g_SpatialSpace), tr(t)
    {
        renderable.visual = ::Render->model_Create(tr->visual.c_str());
        renderable.xform.identity();
        K = renderable.visual ? renderable.visual->dcast_PKinematics() : nullptr;
        if (K && K->LL_BoneCount() == tr->nbones && tr->nbones)
        {
            pose.assign(tr->nbones, Fidentity);
            for (u16 i = 0; i < tr->nbones; ++i)
                K->LL_GetBoneInstance(i).set_callback(bctCustom, pose_cb, &pose[i], TRUE);
        }
        else
            K = nullptr;
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY;
    }
    ~Puppet() override
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
    void show()
    {
        if (!renderable.visual)
            return;
        const vis_data& v = renderable.visual->getVisData();
        Fvector P;
        renderable.xform.transform_tiny(P, v.sphere.P);
        const float s = renderable.xform.i.magnitude();
        float r = v.sphere.R * (s > EPS ? s : 1.f);
        if (!_valid(P) || !_valid(r))
            return;
        if (r < 0.5f)
            r = 0.5f;
        spatial.sphere.set(P, r);
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
        if (renderable.visual)
            ::Render->add_Visual(context_id, this, renderable.visual, renderable.xform);
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

// The particles draw their randomness from the thread's `Random` (xrParticles' drand48). E53b: a live system steps in
// quanta of PS_Q ms with a sequence of its own seeded at its Play (CParticlesObject::pm_reseed, the seed in the track);
// a replay system does the same with the same seed and quanta, its parent placed for each quantum as it was in the
// frame that ran it - the same history as the live take, and the same one again on every re-simulation.
static_assert(sizeof(CRandom) == sizeof(std::mt19937), "CRandom is just its mt19937");
void seed_random(CRandom& r, u32 seed) { reinterpret_cast<std::mt19937&>(r).seed(seed); }

class ReplayPS : public ISpatial, public IRenderable
{
public:
    IParticleCustom* V{};
    bool in_space{}, stopped{};
    CRandom rng;
    int q{}; // E53b: the quanta run (3 at its Play)
    bool rough{}; // E53b: started late, not at its birth (an old system while scrubbing back): redone when that ends

    explicit ReplayPS(LPCSTR name) : ISpatial(g_SpatialSpace)
    {
        renderable.visual = ::Render->model_CreateParticles(name);
        V = renderable.visual ? smart_cast<IParticleCustom*>(renderable.visual) : nullptr;
        renderable.xform.identity();
    }
    ~ReplayPS() override
    {
        if (in_space)
            spatial_unregister();
        if (renderable.visual)
            ::Render->model_Delete(renderable.visual);
    }
    void update_spatial()
    {
        if (!renderable.visual)
            return;
        const vis_data& v = renderable.visual->getVisData();
        if (!_valid(v.sphere))
            return;
        Fvector P;
        renderable.xform.transform_tiny(P, v.sphere.P);
        spatial.sphere.set(P, v.sphere.R);
        if (!in_space)
        {
            spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY;
            spatial_register();
            in_space = true;
        }
        else
            spatial_move();
    }
    IRenderable* dcast_Renderable() override { return this; }
    void renderable_Render(u32 context_id, IRenderable* root) override { ::Render->add_Visual(context_id, root ? root : this, renderable.visual, renderable.xform); }
};

// E53c: a bullet's model in slow motion (tools/bullet_ogf.py: +z its tip, the origin at its middle); a pool of them,
// placed every frame on the bullets near the camera
class BulletModel : public ISpatial, public IRenderable
{
public:
    shared_str name;
    float len{}; // its length, m
    bool in_space{}, used{};

    explicit BulletModel(const shared_str& nm) : ISpatial(g_SpatialSpace), name(nm)
    {
        renderable.visual = ::Render->model_Create(nm.c_str());
        renderable.xform.identity();
        if (renderable.visual)
        {
            if (IKinematics* K = renderable.visual->dcast_PKinematics())
                K->CalculateBones(TRUE);
            const vis_data& v = renderable.visual->getVisData();
            len = v.box.max.z - v.box.min.z;
        }
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY;
    }
    ~BulletModel() override
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
    void place(const Fmatrix& m, float scale)
    {
        if (!renderable.visual || !_valid(m))
            return;
        renderable.xform = m;
        spatial.sphere.set(m.c, std::max(len * scale, 0.05f));
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
        if (renderable.visual)
            ::Render->add_Visual(context_id, this, renderable.visual, renderable.xform);
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

enum CamMode
{
    CAM_FREE,
    CAM_LOOK,
    CAM_EYE,
};

struct PathKey
{
    float u, t;
    Fvector p;
    float h, pitch, roll, fov;
};

struct Player
{
    bool open{};
    Take take;
    shared_str name;
    xr_vector<Puppet*> puppets;
    xr_vector<ReplayPS*> ps;
    xr_vector<u8> ps_bad;
    xr_vector<ref_sound> snd;
    xr_vector<u8> snd_on;
    xr_vector<ref_light> lights;
    xr_vector<Tracer> tracers;
    double t{}, prev_t{};
    float speed{1.f};
    bool paused{}, jumped{true};
    double ps_time{-1e9};
    double ps_frac{};
    bool snd_running{};
    int weather{-1};
    // the camera
    CamMode mode{CAM_FREE};
    Fvector cam_p{};
    float h{}, pitch{}, roll{}, fov{67.5f}, fov0{67.5f}, fly{4.f};
    int target{-1};
    // the path
    xr_vector<PathKey> keys;
    bool path_play{};
    float path_u{};
    int slot{1};
    // input
    bool down[512]{};
    bool pressed[512]{};
    int scrub{};
    u32 scrub_since{};
    int hint{1};
    // what the replay changed, to put back
    bool cine_was{};
    PORTAL_MOD_DRAW pd_was[2]{};
    shared_str weather_was;
    bool timer_by_me{};
    bool snd_paused{};
    float shader_time{};
    double rain_acc{}; // E53b: the rain's clock, the ms fraction carried
    // E53c: the bullets' models and the bullet camera
    xr_vector<BulletModel*> bmodels;
    struct BulletCam
    {
        int idx{-1}; // the bullet followed (take.bullets), -1: off
        int phase{}; // 1: in flight, 2: after its end
        float hold{}, clock{}, rate{1.f / 512.f}, fov{24.f}, speed_was{1.f};
        Fvector d{}, p{}, look{}, last{}, hold_p{};
        bool d_ok{}, held{};
        ref_light light; // a small fill light on the bullet while the camera rides with it (the films light it too)
    } bc;
} P;

bool shift_down() { return P.down[DIK_LSHIFT] || P.down[DIK_RSHIFT]; }
bool ctrl_down() { return P.down[DIK_LCONTROL] || P.down[DIK_RCONTROL]; }

// the object's state at t: false when it is not there
bool obj_eval(const ObjTrack& tr, double t, Fmatrix* xf, VisMask* mask, Fmatrix* pose, Fvector* eye_p, Fvector* eye_d)
{
    if (tr.keys.empty() || t < tr.keys.front().t - 1e-6)
        return false;
    const size_t i = key_at(tr.keys, t);
    const ObjKey& a = tr.keys[i];
    if (!(a.flags & F_VISIBLE))
        return false;
    size_t j = i;
    float k = 0.f;
    if (i + 1 < tr.keys.size())
    {
        const ObjKey& b = tr.keys[i + 1];
        if ((b.flags & F_VISIBLE) && b.t > a.t && a.pos.distance_to_sqr(b.pos) < TELEPORT * TELEPORT)
        {
            k = float((t - a.t) / (b.t - a.t));
            clamp(k, 0.f, 1.f);
            j = i + 1;
        }
    }
    const ObjKey& b = tr.keys[j];
    if (xf)
    {
        Fquaternion Q;
        quat_lerp(a.q, b.q, k, Q);
        Fvector p;
        p.lerp(a.pos, b.pos, k);
        make_xform(*xf, Q, p, a.scale + (b.scale - a.scale) * k);
    }
    if (mask)
        mask->set(a.vis[0], a.vis[1]);
    if (pose && tr.nbones)
    {
        const BoneQ* ba = &tr.bones[i * tr.nbones];
        const BoneQ* bb = &tr.bones[j * tr.nbones];
        for (u16 n = 0; n < tr.nbones; ++n)
            bone_lerp(ba[n], bb[n], k, pose[n]);
    }
    if (eye_p || eye_d)
    {
        if (!(a.flags & F_EYE))
            return false;
        const bool lerp = (b.flags & F_EYE) != 0;
        if (eye_p)
            eye_p->lerp(a.eye_pos, lerp ? b.eye_pos : a.eye_pos, lerp ? k : 0.f);
        if (eye_d)
        {
            Fvector da{a.eye_dir[0] / 32767.f, a.eye_dir[1] / 32767.f, a.eye_dir[2] / 32767.f};
            Fvector db{b.eye_dir[0] / 32767.f, b.eye_dir[1] / 32767.f, b.eye_dir[2] / 32767.f};
            eye_d->lerp(da, lerp ? db : da, lerp ? k : 0.f);
            eye_d->normalize_safe();
        }
    }
    return true;
}

// a point to look at on the object: the chest of a creature, the middle of a thing
bool obj_aim(int ti, double t, Fvector& out)
{
    if (ti < 0 || ti >= int(P.take.objs.size()))
        return false;
    const ObjTrack& tr = P.take.objs[ti];
    Fmatrix xf;
    if (!obj_eval(tr, t, &xf, nullptr, nullptr, nullptr, nullptr))
        return false;
    Fvector ep;
    if (obj_eval(tr, t, nullptr, nullptr, nullptr, &ep, nullptr))
    {
        out.lerp(xf.c, ep, 0.75f);
        return true;
    }
    Puppet* pp = P.puppets[ti];
    if (pp && pp->renderable.visual)
    {
        xf.transform_tiny(out, pp->renderable.visual->getVisData().sphere.P);
        return true;
    }
    out = xf.c;
    return true;
}

void puppets_update(double t)
{
    // the eye view: the one we look through is not drawn, nor what he holds and wears - from his eyes his own arms
    // and sleeves are right at the lens and a hit or a fall swings them across the frame (take replay_07)
    u16 eye_id = u16(-1);
    Fvector ep;
    if (P.mode == CAM_EYE && P.target >= 0 && P.target < int(P.take.objs.size()) &&
        obj_eval(P.take.objs[P.target], t, nullptr, nullptr, nullptr, &ep, nullptr))
        eye_id = P.take.objs[P.target].id;
    for (u32 i = 0; i < P.take.objs.size(); ++i)
    {
        const ObjTrack& tr = P.take.objs[i];
        Puppet*& pp = P.puppets[i];
        Fmatrix xf;
        VisMask mask;
        if ((eye_id != u16(-1) && (tr.id == eye_id || tr.parent == eye_id)) || !obj_eval(tr, t, &xf, &mask, nullptr, nullptr, nullptr))
        {
            if (pp)
                pp->hide();
            continue;
        }
        if (!pp)
            pp = xr_new<Puppet>(&tr);
        pp->renderable.xform = xf;
        if (pp->K)
        {
            obj_eval(tr, t, nullptr, nullptr, pp->pose.data(), nullptr, nullptr);
            if (!pp->has_mask || pp->mask != mask)
            {
                VisMask m = mask;
                m.set(pp->K->LL_GetBoneRoot(), true); // the root bone stays on, or nothing is calculated
                pp->K->LL_SetBonesVisible(m);
                pp->mask = mask;
                pp->has_mask = true;
            }
            pp->K->CalculateBones_Invalidate();
            pp->K->CalculateBones(TRUE);
        }
        pp->show();
    }
}

bool ps_eval(const PsTrack& tr, double t, Fmatrix& parent, Fmatrix& rx, Fvector& vel, u32& flags)
{
    if (tr.keys.empty())
        return false;
    const size_t i = key_at(tr.keys, t);
    const PsKey& a = tr.keys[i];
    parent = a.parent;
    rx = a.rx;
    vel = a.vel;
    flags = a.flags;
    if (i + 1 < tr.keys.size() && t > a.t)
    {
        const PsKey& b = tr.keys[i + 1];
        if (b.t > a.t)
        {
            float k = float((t - a.t) / (b.t - a.t));
            clamp(k, 0.f, 1.f);
            parent.c.lerp(a.parent.c, b.parent.c, k);
            rx.c.lerp(a.rx.c, b.rx.c, k);
            vel.lerp(a.vel, b.vel, k);
        }
    }
    return true;
}

void ps_clear()
{
    for (ReplayPS*& p : P.ps)
        xr_delete(p);
}

void ps_place(u32 i, double t)
{
    ReplayPS* p = P.ps[i];
    Fmatrix parent, rx;
    Fvector vel;
    u32 fl;
    if (!ps_eval(P.take.ps[i], t, parent, rx, vel, fl))
        return;
    p->V->UpdateParent(parent, vel, (fl & PS_XFORM) ? TRUE : FALSE);
    p->renderable.xform = rx;
    if ((fl & PS_STOPPED) && !p->stopped)
    {
        p->V->Stop(TRUE);
        p->stopped = true;
    }
}

// E53b: the live clock of the take's frames (Device.dwTimeGlobal, ms) at the replay's time t
double ms_at(double t)
{
    const xr_vector<Sample>& S = P.take.samples;
    if (S.empty())
        return t * 1000.0;
    const Sample& s = S[key_at(S, t)];
    return double(s.ms) + (t - s.t) * 1000.0;
}

// the time of the first frame at or after the live ms: the frame that ran a particle quantum due then
double frame_of_ms(double ms)
{
    const xr_vector<Sample>& S = P.take.samples;
    if (S.empty())
        return ms / 1000.0;
    const auto it = std::lower_bound(S.begin(), S.end(), ms, [](const Sample& s, double m) { return double(s.ms) < m; });
    if (it == S.end())
        return S.back().t + (ms - double(S.back().ms)) / 1000.0;
    return it->t;
}

// the quanta a live system had run by the live ms: three at its Play, then one every PS_Q ms
int quanta_at(const PsTrack& tr, double ms) { return 3 + int(std::floor((ms - double(tr.born_ms)) / double(PS_Q))); }
double quantum_ms(const PsTrack& tr, int n) { return double(tr.born_ms) + double(std::max(0, n - 3)) * double(PS_Q); }

// a replay system for track i started at quantum `from` (3: at its birth, as the live one; later: an old one started
// late, with a sequence of its own) the way CParticlesObject::Play does it: placed, played, three quanta at once
bool ps_start(u32 i, int from, bool rough)
{
    const PsTrack& tr = P.take.ps[i];
    xr_delete(P.ps[i]);
    ReplayPS* p = xr_new<ReplayPS>(tr.name.c_str());
    if (!p->V)
    {
        xr_delete(p);
        P.ps_bad[i] = 1;
        return false;
    }
    P.ps[i] = p;
    p->rough = rough;
    seed_random(p->rng, from <= 3 ? tr.seed : tr.seed ^ (u32(from) * 2246822519u));
    CRandom* const was = pm_random_use(&p->rng);
    ps_place(i, frame_of_ms(quantum_ms(tr, from)));
    p->V->Play();
    for (int k = 0; k < 3; ++k)
        p->V->OnFrame(PS_Q);
    pm_random_use(was);
    p->q = from;
    return true;
}

// every system at the replay's time t: run from its birth (or, when old, from a point on a grid) quantum by quantum, its
// parent for each quantum as in the frame that ran it; drawn moved on by the time since its last quantum (slow motion)
void ps_update(double t, bool coarse)
{
    const double ms = ms_at(t);
    const int sec = int(1000 / PS_Q); // quanta in a second
    // fine: up to 20 s old from its birth, older from a 2 s grid 6 s back; scrubbing back (every frame anew): up to
    // 1.5 s from its birth, older from a 0.5 s grid 1.5 s back, redone exactly when the scrubbing ends
    const int max_birth = coarse ? sec * 3 / 2 : sec * 20;
    const int win = coarse ? sec * 3 / 2 : sec * 6, grid = coarse ? sec / 2 : sec * 2;
    for (u32 i = 0; i < P.take.ps.size(); ++i)
    {
        const PsTrack& tr = P.take.ps[i];
        if (P.ps_bad[i] || t < tr.born - 0.05 || t > tr.end || ms < double(tr.born_ms))
        {
            if (P.ps[i])
                xr_delete(P.ps[i]);
            continue;
        }
        const int target = std::max(3, quanta_at(tr, ms));
        const int age = target - 3;
        const int from = age <= max_birth ? 3 : 3 + ((age - win) / grid) * grid;
        ReplayPS* p = P.ps[i];
        if (!p || target < p->q || p->q < from - 3 || (p->rough && !coarse))
        {
            if (!ps_start(i, from, coarse && from > 3))
                continue;
            p = P.ps[i];
        }
        if (p->q < target)
        {
            CRandom* const was = pm_random_use(&p->rng);
            for (int n = p->q + 1; n <= target; ++n)
            {
                ps_place(i, frame_of_ms(quantum_ms(tr, n)));
                p->V->OnFrame(PS_Q);
            }
            pm_random_use(was);
            p->q = target;
        }
        p->V->PmSetAhead(float(ms - quantum_ms(tr, p->q)));
        p->update_spatial();
    }
}

// E53c: the way a bullet flew from the shot up to a moment of the take (the whole way after its end)
float bullet_path_at(const BulletTrack& tr, double tt)
{
    const xr_vector<BulletPt>& v = tr.pts;
    if (v.empty())
        return 0.f;
    float ta = tr.tau_at(tt);
    clamp(ta, 0.f, tr.tau_end());
    size_t j = size_t(std::upper_bound(v.begin(), v.end(), ta, [](float x, const BulletPt& b) { return x < b.tau; }) - v.begin());
    j = j ? j - 1 : 0;
    float d = tr.cum.size() == v.size() ? tr.cum[j] : 0.f;
    if (j + 1 < v.size() && v[j + 1].tau > v[j].tau)
        d += v[j].pos.distance_to(v[j + 1].pos) * (ta - v[j].tau) / (v[j + 1].tau - v[j].tau);
    return d;
}
float bullet_path_total(const BulletTrack& tr) { return tr.cum.size() == tr.pts.size() && !tr.cum.empty() ? tr.cum.back() : 0.f; }

struct BulletNow
{
    Fvector pos, dir;
    float speed, since, path; // since: the way from the muzzle or the last portal; path: from the shot
    bool from_muzzle;
};

// E53c: a bullet at the replay's time t (false: not in flight then)
bool bullet_now(const BulletTrack& tr, double t, BulletNow& o)
{
    const xr_vector<BulletPt>& v = tr.pts;
    if (v.empty() || t < tr.born)
        return false;
    const float te = tr.tau_end();
    float tau = tr.tau_at(t);
    if (tr.end > tr.born ? t > tr.end : tau > te + 40.f)
        return false;
    tau = std::min(tau, te);
    size_t i = size_t(std::upper_bound(v.begin(), v.end(), tau, [](float x, const BulletPt& b) { return x < b.tau; }) - v.begin());
    i = i ? i - 1 : 0;
    Fvector pos = v[i].pos, dir{};
    float speed = v[i].speed;
    if (i + 1 < v.size() && v[i + 1].tau > v[i].tau)
    {
        const float k = (tau - v[i].tau) / (v[i + 1].tau - v[i].tau);
        pos.lerp(v[i].pos, v[i + 1].pos, k);
        speed = v[i].speed + (v[i + 1].speed - v[i].speed) * k;
        dir.sub(v[i + 1].pos, v[i].pos);
    }
    else if (i > 0 && !(v[i].flags & pm_replay::BP_JUMP_TO))
        dir.sub(v[i].pos, v[i - 1].pos);
    else if (i + 1 < v.size())
        dir.sub(v[i + 1].pos, v[i].pos);
    if (dir.square_magnitude() < 1e-8f)
        return false;
    dir.normalize();
    // the way flown since the muzzle or the last portal (clips the tracer and the trail behind)
    float since = pos.distance_to(v[i].pos);
    bool from_muzzle = !(v[0].flags & pm_replay::BP_JUMP_TO);
    for (size_t j = i; j > 0; --j)
    {
        if (v[j].flags & pm_replay::BP_JUMP_TO)
        {
            from_muzzle = false;
            break;
        }
        since += v[j].pos.distance_to(v[j - 1].pos);
    }
    o.path = (tr.cum.size() == v.size() ? tr.cum[i] : 0.f) + pos.distance_to(v[i].pos);
    // a first-person shot starts at the eye: it is seen to leave the world model's muzzle, eased onto its path in 8 m
    if (from_muzzle && since < 8.f)
    {
        Fvector off;
        off.sub(tr.vis_from, v[0].pos);
        pos.mad(off, 1.f - since / 8.f);
    }
    o.pos = pos;
    o.dir = dir;
    o.speed = speed;
    o.since = since;
    o.from_muzzle = from_muzzle;
    return true;
}

u32 argb_scale(u32 c, float k)
{
    clamp(k, 0.f, 1.f);
    const u32 r = u32(float((c >> 16) & 0xFF) * k), g = u32(float((c >> 8) & 0xFF) * k), b = u32(float(c & 0xFF) * k);
    return (c & 0xFF000000) | (r << 16) | (g << 8) | b;
}

// E53b: the bullets at the replay's time - the tracers as the live game draws them (their streak is the way flown in a
// frame: shorter in slow motion); at `bullets` speed and slower every bullet as a hot slug with a fading trail.
// E53c: and its model (its ammo's, [pm_replay_bullets]) when it is near the camera, turning with the barrel's twist;
// the glow fades out as the camera comes near (a tracer's stays at its base, where the composition burns).
void bullets_update(double t, float rate)
{
    P.tracers.clear();
    for (BulletModel* m : P.bmodels)
        m->used = false;
    const float ar = _abs(rate);
    const bool slugs = R.bullet_rate > 0.f && ar <= R.bullet_rate + 1e-6f;
    const float blur = ar * Device.fTimeDelta; // the replay's seconds in this drawn frame
    for (size_t bi = 0; bi < P.take.bullets.size(); ++bi)
    {
        const BulletTrack& tr = P.take.bullets[bi];
        BulletNow b;
        if (!bullet_now(tr, t, b))
            continue;
        const Fvector& pos = b.pos;
        const Fvector& dir = b.dir;
        const float dist = pos.distance_to(Device.vCameraPosition);
        if (dist < 0.2f && int(bi) != P.bc.idx)
            continue;
        if (!slugs)
        {
            if (!tr.tracer)
                continue;
            Tracer x{};
            x.pos = pos;
            x.dir = dir;
            x.speed = b.speed * std::min(1.f, ar);
            x.portal_dist = b.since;
            x.color = tr.color;
            P.tracers.push_back(x);
            continue;
        }
        // the model
        float body = 0.f; // its length drawn (the glow and the trail start behind it)
        if (tr.model.size() && dist < R.bullet_models)
        {
            BulletModel* bm = nullptr;
            for (BulletModel* m : P.bmodels)
                if (!m->used && m->name == tr.model)
                {
                    bm = m;
                    break;
                }
            if (!bm && P.bmodels.size() < 64)
            {
                bm = xr_new<BulletModel>(tr.model);
                P.bmodels.push_back(bm);
            }
            if (bm && bm->renderable.visual)
            {
                bm->used = true;
                const float sc = R.bullet_scale;
                body = bm->len * sc;
                Fvector up{0.f, 1.f, 0.f}, i0, j0;
                if (_abs(dir.y) > 0.98f)
                    up.set(1.f, 0.f, 0.f);
                i0.crossproduct(up, dir).normalize();
                j0.crossproduct(dir, i0);
                // the spin: +2 pi per twist of the way about its axis (the engravings lean the same way), a phase of
                // its own
                float ang = float(tr.id) * 2.39996f;
                if (tr.twist > 1e-4f)
                    ang += PI_MUL_2 * std::fmod(b.path / tr.twist, 1.f);
                const float c = _cos(ang), sn = _sin(ang);
                Fmatrix m;
                m.identity();
                m.i.mul(i0, c).mad(j0, sn).mul(sc);
                m.j.mul(i0, -sn).mad(j0, c).mul(sc);
                m.k.mul(dir, sc);
                m.c.mad(pos, dir, -body * 0.5f);
                bm->place(m, sc);
            }
        }
        auto add = [&](const Fvector& at, float len, float width, u32 argb, bool head) {
            Tracer x{};
            x.pos = at;
            x.dir = dir;
            x.speed = b.speed;
            x.portal_dist = b.since;
            x.color = tr.color;
            x.slug = 1;
            x.head = head ? 1 : 0;
            x.length = len;
            x.width = width;
            x.argb = argb;
            P.tracers.push_back(x);
        };
        const float path_now = b.path;
        const float core = std::min(std::max(0.12f, path_now - bullet_path_at(tr, t - blur)), std::max(b.since, 0.05f));
        // near the camera the model shows: the glow fades out (k 0 within 0.4 m, whole from 3.4 m)
        float k = 1.f;
        if (body > 0.f)
        {
            k = (dist - 0.4f) / 3.f;
            clamp(k, 0.f, 1.f);
        }
        if (tr.tracer)
        {
            Fvector base; // just behind the model's base (a halo round it; at the base itself the base would hide half)
            base.mad(pos, dir, -body - (body > 0.f ? 0.004f : 0.f));
            add(base, (core * 1.3f + 0.1f) * k + 0.012f, 0.16f * k + 0.009f, 0, true); // the tracer's glow in its own colour
            if (k > 0.05f)
                add(base, core * k + 0.01f, 0.05f * k + 0.004f, 0xFFFFF6E0, true);
        }
        else if (k > 0.01f)
        {
            add(pos, (core * 1.2f + 0.08f) * k + body, 0.08f * k, argb_scale(0xFF4A2A12, k), true); // the glow
            add(pos, core * k + body, 0.03f * k, argb_scale(0xFFFFF0D0, k), true); // the hot slug
        }
        // the trail: fading segments behind it over the way of the last 7 ms of the take (~6 m in flight), not past the
        // muzzle or the last portal
        static const float seg_part[3] = {0.1f, 0.26f, 0.64f}, seg_w[3] = {0.03f, 0.045f, 0.06f};
        static const u32 seg_c[3] = {0xFF6A4424, 0xFF362618, 0xFF1A140E};
        const float head = body > 0.f ? body : core;
        const float trail = std::min(b.since, path_now - bullet_path_at(tr, t - 0.007)) - head;
        const float wk = body > 0.f ? std::max(k, 0.1f) : 1.f;
        float back = head;
        for (int s = 0; s < 3 && trail > 0.02f; ++s)
        {
            const float len = trail * seg_part[s];
            Fvector at;
            at.mad(pos, dir, -back);
            add(at, len, seg_w[s] * wk, seg_c[s], false);
            back += len;
        }
    }
    for (BulletModel* m : P.bmodels)
        if (!m->used)
            m->hide();
}

void sounds_stop()
{
    for (u32 i = 0; i < P.snd.size(); ++i)
        if (P.snd_on[i])
        {
            if (P.snd[i]._feedback())
                P.snd[i].stop();
            P.snd_on[i] = 0;
        }
    P.snd_running = false;
}

void sounds_update(double t, double prev, float rate)
{
    if (rate <= 0.f || rate > 4.f || P.jumped)
    {
        if (P.snd_running)
            sounds_stop();
        if (!(rate > 0.f && rate <= 4.f))
            return;
    }
    const bool start_all = !P.snd_running;
    P.snd_running = true;
    float k = rate;
    clamp(k, 0.2f, 2.f);
    for (u32 i = 0; i < P.take.snd.size(); ++i)
    {
        const SndTrack& tr = P.take.snd[i];
        const bool active = t >= tr.t0 && t < tr.end;
        if (!active)
        {
            if (P.snd_on[i])
            {
                if (P.snd[i]._feedback())
                    P.snd[i].stop();
                P.snd_on[i] = 0;
            }
            continue;
        }
        const size_t ki = key_at(tr.keys, t);
        const SndKey& key = tr.keys[ki];
        Fvector pos = key.pos;
        if (ki + 1 < tr.keys.size() && tr.keys[ki + 1].t > key.t && t > key.t)
        {
            float a = float((t - key.t) / (tr.keys[ki + 1].t - key.t));
            clamp(a, 0.f, 1.f);
            pos.lerp(key.pos, tr.keys[ki + 1].pos, a);
        }
        if (!P.snd_on[i])
        {
            if (!start_all && tr.t0 <= prev)
                continue; // it began before, then stopped by itself: not again
            if (!P.snd[i]._handle())
                P.snd[i].create(tr.name.c_str(), st_Effect, 0);
            if (!P.snd[i]._handle())
            {
                P.snd_on[i] = 1;
                continue;
            }
            const float len = P.snd[i].get_length_sec();
            float ofs = float(t - tr.t0);
            if (tr.looped && len > 0.05f)
                ofs = fmodf(ofs, len);
            else if (ofs >= len - 0.02f)
            {
                P.snd_on[i] = 1;
                continue;
            }
            P.snd[i].play_at_pos(nullptr, pos, (tr.looped ? sm_Looped : 0) | (tr.b2D ? sm_2D : 0));
            if (ofs > 0.02f && P.snd[i]._feedback())
            {
                float tt = ofs;
                clamp(tt, 0.f, std::max(0.f, len - 0.01f));
                P.snd[i].set_time(tt);
            }
            P.snd_on[i] = 1;
        }
        if (P.snd[i]._feedback())
        {
            if (!tr.b2D)
                P.snd[i].set_position(pos);
            P.snd[i].set_volume(key.vol);
            P.snd[i].set_frequency(key.freq * k);
            P.snd[i].set_range(key.mn, key.mx);
        }
    }
}

void lights_update(double t)
{
    for (u32 i = 0; i < P.take.lights.size(); ++i)
    {
        const LightTrack& tr = P.take.lights[i];
        bool on = !tr.keys.empty() && t >= tr.keys.front().t;
        const size_t ki = on ? key_at(tr.keys, t) : 0;
        on = on && (tr.keys[ki].flags & L_ON);
        if (!on)
        {
            if (P.lights[i])
                P.lights[i]->set_active(false);
            continue;
        }
        const LightKey& a = tr.keys[ki];
        Fvector pos = a.pos;
        if (ki + 1 < tr.keys.size() && (tr.keys[ki + 1].flags & L_ON) && tr.keys[ki + 1].t > a.t)
        {
            float k = float((t - a.t) / (tr.keys[ki + 1].t - a.t));
            clamp(k, 0.f, 1.f);
            pos.lerp(a.pos, tr.keys[ki + 1].pos, k);
        }
        if (!P.lights[i])
        {
            P.lights[i] = ::Render->light_create();
            P.lights[i]->portal_mod_replay_light = true;
        }
        IRender_Light* L = &*P.lights[i];
        L->set_type(IRender_Light::LT(a.type));
        L->set_shadow(!!(a.flags & L_SHADOW));
        L->set_volumetric(!!(a.flags & L_VOLUMETRIC));
        L->set_position(pos);
        L->set_rotation(a.dir, a.right);
        L->set_range(a.range);
        L->set_cone(a.cone);
        L->set_virtual_size(a.vsize);
        L->set_color(a.color);
        L->set_active(true);
    }
}

void env_update(double t)
{
    const xr_vector<Sample>& S = P.take.samples;
    if (S.empty())
        return;
    const size_t i = key_at(S, t);
    const Sample& s = S[i];
    if (int(s.weather) != P.weather && s.weather < P.take.weathers.size() && P.take.weathers[s.weather].size())
    {
        P.weather = s.weather;
        g_pGamePersistent->Environment().SetWeather(P.take.weathers[s.weather], true);
    }
    portal_mod_draw[0] = s.pd[0];
    portal_mod_draw[1] = s.pd[1];
    portal_mod_draw[0].params.w = portal_mod_draw[1].params.w = 0.f;
    // E53b: the shaders' time and the grass's wind as in the take: water, fire and grass follow the replay's speed
    // and stand on a pause
    const Sample& s2 = i + 1 < S.size() ? S[i + 1] : s;
    float k = 0.f;
    if (s2.t > s.t)
    {
        k = float((t - s.t) / (s2.t - s.t));
        clamp(k, 0.f, 1.f);
    }
    P.shader_time = s.shader_t + (s2.shader_t - s.shader_t) * k;
    portal_mod_replay_wind.lerp(s.wind, s2.wind, k);
}

float env_sec(double t)
{
    const xr_vector<Sample>& S = P.take.samples;
    const size_t i = key_at(S, t);
    float e = S[i].env;
    if (i + 1 < S.size() && S[i + 1].t > S[i].t && t > S[i].t && _abs(S[i + 1].env - e) < 3600.f)
    {
        float k = float((t - S[i].t) / (S[i + 1].t - S[i].t));
        clamp(k, 0.f, 1.f);
        e += (S[i + 1].env - e) * k;
    }
    return e;
}

//------------------------------------------------------------------------------------------------------------------
// the camera path: Hermite curves through the keys (tangents from the neighbours), the replay's time by a monotone
// cubic (it does not run back between two keys that go forward)
void path_unwrap()
{
    for (size_t i = 1; i < P.keys.size(); ++i)
    {
        while (P.keys[i].h - P.keys[i - 1].h > PI)
            P.keys[i].h -= PI_MUL_2;
        while (P.keys[i].h - P.keys[i - 1].h < -PI)
            P.keys[i].h += PI_MUL_2;
    }
}

float comp(const PathKey& k, int c)
{
    switch (c)
    {
    case 0: return k.p.x;
    case 1: return k.p.y;
    case 2: return k.p.z;
    case 3: return k.h;
    case 4: return k.pitch;
    case 5: return k.roll;
    case 6: return k.fov;
    default: return k.t;
    }
}

float path_eval_c(size_t i, float s, int c)
{
    const auto& K = P.keys;
    const size_t n = K.size();
    const float du = K[i + 1].u - K[i].u;
    const float f0 = comp(K[i], c), f1 = comp(K[i + 1], c);
    float m0, m1;
    if (c == 7)
    {
        // PCHIP for the time
        auto delta = [&](size_t a) { return (comp(K[a + 1], 7) - comp(K[a], 7)) / std::max(1e-4f, K[a + 1].u - K[a].u); };
        auto slope = [&](size_t a) -> float {
            if (a == 0)
                return delta(0);
            if (a == n - 1)
                return delta(n - 2);
            const float d0 = delta(a - 1), d1 = delta(a);
            if (d0 * d1 <= 0.f)
                return 0.f;
            const float h0 = K[a].u - K[a - 1].u, h1 = K[a + 1].u - K[a].u;
            const float w1 = 2.f * h1 + h0, w2 = h1 + 2.f * h0;
            return (w1 + w2) / (w1 / d0 + w2 / d1);
        };
        m0 = slope(i);
        m1 = slope(i + 1);
    }
    else
    {
        auto tangent = [&](size_t a) -> float {
            if (a == 0)
                return (comp(K[1], c) - comp(K[0], c)) / std::max(1e-4f, K[1].u - K[0].u);
            if (a == n - 1)
                return (comp(K[n - 1], c) - comp(K[n - 2], c)) / std::max(1e-4f, K[n - 1].u - K[n - 2].u);
            return (comp(K[a + 1], c) - comp(K[a - 1], c)) / std::max(1e-4f, K[a + 1].u - K[a - 1].u);
        };
        m0 = tangent(i);
        m1 = tangent(i + 1);
    }
    const float s2 = s * s, s3 = s2 * s;
    return (2.f * s3 - 3.f * s2 + 1.f) * f0 + (s3 - 2.f * s2 + s) * du * m0 + (-2.f * s3 + 3.f * s2) * f1 + (s3 - s2) * du * m1;
}

void path_eval(float u, PathKey& out)
{
    const auto& K = P.keys;
    if (K.size() == 1 || u <= K.front().u)
    {
        out = K.front();
        return;
    }
    if (u >= K.back().u)
    {
        out = K.back();
        return;
    }
    size_t i = 0;
    while (i + 2 < K.size() && K[i + 1].u <= u)
        ++i;
    const float s = (u - K[i].u) / std::max(1e-4f, K[i + 1].u - K[i].u);
    out.u = u;
    out.p.set(path_eval_c(i, s, 0), path_eval_c(i, s, 1), path_eval_c(i, s, 2));
    out.h = path_eval_c(i, s, 3);
    out.pitch = path_eval_c(i, s, 4);
    out.roll = path_eval_c(i, s, 5);
    out.fov = path_eval_c(i, s, 6);
    out.t = path_eval_c(i, s, 7);
}

bool path_file(string_path& rel, int slot)
{
    if (!P.name.size())
        return false;
    string32 ext;
    xr_sprintf(ext, ".cam%d", slot);
    file_of(rel, P.name.c_str(), ext);
    return true;
}

bool path_save(int slot)
{
    string_path rel;
    if (!path_file(rel, slot))
        return false;
    IWriter* W = FS.w_open("$app_data_root$", rel);
    if (!W)
        return false;
    W->w_string("; portal-mod E53 camera path: u t x y z heading pitch roll fov");
    for (const PathKey& k : P.keys)
    {
        string256 line;
        xr_sprintf(line, "%.4f %.4f %.4f %.4f %.4f %.6f %.6f %.6f %.3f", k.u, k.t, k.p.x, k.p.y, k.p.z, k.h, k.pitch, k.roll, k.fov);
        W->w_string(line);
    }
    FS.w_close(W);
    return true;
}

bool path_load(int slot)
{
    string_path rel;
    if (!path_file(rel, slot) || !FS.exist("$app_data_root$", rel))
        return false;
    IReader* F = FS.r_open("$app_data_root$", rel);
    if (!F)
        return false;
    P.keys.clear();
    while (!F->eof())
    {
        string512 line;
        F->r_string(line, sizeof(line));
        PathKey k{};
        if (line[0] != ';' && 9 == sscanf(line, "%f %f %f %f %f %f %f %f %f", &k.u, &k.t, &k.p.x, &k.p.y, &k.p.z, &k.h, &k.pitch, &k.roll, &k.fov))
            P.keys.push_back(k);
    }
    FS.r_close(F);
    path_unwrap();
    return true;
}

void key_add(float u, float t, const Fvector& p, float h, float pitch, float roll, float fov)
{
    PathKey k{};
    if (u < 0.f)
    {
        if (P.keys.empty())
            u = 0.f;
        else
        {
            const float dt = _abs(t - P.keys.back().t);
            u = P.keys.back().u + (dt > 1e-3f ? dt : 2.f);
        }
    }
    if (!P.keys.empty() && u < P.keys.back().u + 0.05f)
        u = P.keys.back().u + 0.05f;
    k.u = u;
    k.t = t;
    k.p = p;
    k.h = h;
    k.pitch = pitch;
    k.roll = roll;
    k.fov = fov;
    P.keys.push_back(k);
    path_unwrap();
}

//------------------------------------------------------------------------------------------------------------------
// the player's frame
void apply_camera(const Fvector& p, const Fvector& d_in, float roll, float fov)
{
    Fvector d = d_in;
    if (d.square_magnitude() < EPS)
        d.set(0.f, 0.f, 1.f);
    d.normalize();
    Fvector up{0.f, 1.f, 0.f};
    if (_abs(d.y) > 0.999f)
        up.set(0.f, 0.f, 1.f);
    Fvector r, n;
    r.crossproduct(up, d).normalize();
    n.crossproduct(d, r).normalize();
    if (_abs(roll) > EPS)
    {
        Fvector n2;
        n2.mul(n, _cos(roll)).mad(r, _sin(roll));
        n = n2.normalize();
    }
    r.crossproduct(n, d).normalize();
    clamp(fov, 1.f, 150.f);
    Device.mView.build_camera_dir(p, d, n);
    Device.vCameraPosition.set(p);
    Device.vCameraDirection.set(d);
    Device.vCameraTop.set(n);
    Device.vCameraRight.set(r);
    Device.fFOV = fov;
    Device.fASPECT = Device.fHeight_2 / Device.fWidth_2;
    float far_plane = 300.f;
    if (g_pGamePersistent && g_pGamePersistent->Environment().CurrentEnv)
        far_plane = g_pGamePersistent->Environment().CurrentEnv->far_plane;
    Device.mProject.build_projection(deg2rad(fov), Device.fASPECT, VIEWPORT_NEAR, far_plane);
    CCameraManager::ResetPP();
}

void free_from_current()
{
    Fvector d = Device.vCameraDirection;
    if (P.mode != CAM_FREE)
    {
        P.cam_p = Device.vCameraPosition;
        d.getHP(P.h, P.pitch);
    }
    P.mode = CAM_FREE;
    P.target = -1;
}

// E53c: the bullet camera. It rides with a bullet as the films do: beside the muzzle as it leaves, then just behind it
// and aside (0.29 m: the near plane is 0.2 m), slowly circling it, a narrow lens; the replay slows down by itself so
// the bullet crosses the screen at a pace one can watch - slow at the muzzle and at the hit, fast between; a metre short
// of the hit the camera stops and watches it go in; after its end it looks at the hit a while, the replay speeding up
// to 1/12 so the blood and the dust fly out, and lets go (a free camera there, paused).
void seek(double t);

float smooth01(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// the moment a bullet ended (its hit as the take shows it: the last anchor)
double bullet_end_t(const BulletTrack& tr)
{
    if (tr.anch.size() >= 2)
        return tr.anch.back().second;
    if (tr.end > tr.born)
        return tr.end;
    return tr.born + double(tr.tau_end()) / 1000.0;
}

void bcam_light(bool on, const Fvector& at);

void bcam_stop(bool natural)
{
    if (P.bc.idx < 0)
        return;
    P.bc.idx = -1;
    bcam_light(false, Device.vCameraPosition);
    P.cam_p = Device.vCameraPosition;
    Device.vCameraDirection.getHP(P.h, P.pitch);
    P.mode = CAM_FREE;
    P.target = -1;
    P.roll = 0.f;
    P.fov = P.bc.fov;
    P.speed = P.bc.speed_was;
    if (natural)
        P.paused = true;
    message(natural ? "\xEF\xF3\xEB\xFF \xE4\xEE\xEB\xE5\xF2\xE5\xEB\xE0" : "\xEA\xE0\xEC\xE5\xF0\xE0 \xE7\xE0 \xEF\xF3\xEB\xE5\xE9 \xE2\xFB\xEA\xEB\xFE\xF7\xE5\xED\xE0");
}

void bcam_start(int idx, bool from_shot)
{
    if (idx < 0 || idx >= int(P.take.bullets.size()))
        return;
    if (P.bc.idx < 0)
        P.bc.speed_was = P.speed;
    const BulletTrack& tr = P.take.bullets[idx];
    P.bc.idx = idx;
    P.bc.phase = 1;
    P.bc.hold = 0.f;
    P.bc.clock = 0.f;
    P.bc.d_ok = false;
    P.bc.held = false;
    P.bc.rate = 1.f / 1024.f;
    P.path_play = false;
    P.mode = CAM_FREE;
    P.target = -1;
    P.paused = false;
    if (from_shot)
        seek(tr.born + 0.0002);
    string128 m;
    xr_sprintf(m, "\xEA\xE0\xEC\xE5\xF0\xE0 \xE7\xE0 \xEF\xF3\xEB\xE5\xE9: %s", tr.ammo.size() ? tr.ammo.c_str() : "-");
    message(m);
}

// B: the bullet in flight nearest the middle of the screen (followed from now), else the next one shot (from its shot)
int bcam_pick(bool& from_shot)
{
    int best = -1;
    float best_c = _cos(deg2rad(30.f));
    for (u32 i = 0; i < P.take.bullets.size(); ++i)
    {
        BulletNow b;
        const BulletTrack& tr = P.take.bullets[i];
        if (!bullet_now(tr, P.t, b) || bullet_path_total(tr) - b.path < 2.f)
            continue;
        Fvector d;
        d.sub(b.pos, Device.vCameraPosition);
        const float dist = d.magnitude();
        if (dist < 0.05f)
            continue;
        d.div(dist);
        const float c = d.dotproduct(Device.vCameraDirection) - dist * 0.0002f;
        if (c > best_c)
        {
            best_c = c;
            best = int(i);
        }
    }
    from_shot = best < 0;
    if (best >= 0)
        return best;
    double first = 1e30;
    for (u32 i = 0; i < P.take.bullets.size(); ++i)
    {
        const BulletTrack& tr = P.take.bullets[i];
        if (tr.born > P.t + 1e-4 && tr.born < first && bullet_path_total(tr) > 2.f)
        {
            first = tr.born;
            best = int(i);
        }
    }
    return best;
}

void bcam_light(bool on, const Fvector& at)
{
    if (!on)
    {
        if (P.bc.light)
            P.bc.light->set_active(false);
        return;
    }
    if (!P.bc.light)
    {
        P.bc.light = ::Render->light_create();
        P.bc.light->portal_mod_replay_light = true;
    }
    IRender_Light* L = &*P.bc.light;
    L->set_type(IRender_Light::POINT);
    L->set_shadow(false);
    L->set_volumetric(false);
    L->set_position(at);
    L->set_range(0.6f);
    L->set_color(0.85f, 0.8f, 0.72f);
    L->set_active(true);
}

// the replay's speed while the camera rides (dt: the drawn frame's, for the smoothing)
float bcam_rate(float dt)
{
    const BulletTrack& tr = P.take.bullets[P.bc.idx];
    float want;
    if (P.bc.phase == 1)
    {
        const double t0 = std::max(tr.born, P.t - 0.0005), t1 = P.t + 0.0005;
        const float v = (bullet_path_at(tr, t1) - bullet_path_at(tr, t0)) / float(t1 - t0); // its speed in the take, m/s
        const float s = bullet_path_at(tr, P.t), r = bullet_path_total(tr) - s;
        // how fast it goes on the screen, m per second shown: slow by the muzzle and the hit, fast between
        const float vis = 0.9f + 15.f * smooth01(0.3f, 3.f, s) * smooth01(1.f, 9.f, r);
        want = v > 1.f ? vis / v : 1.f / 64.f;
        clamp(want, 1.f / 8192.f, 1.f / 16.f);
    }
    else // after the hit: up to x1/12 in 1.5 s, the blood and the dust fly out
    {
        const float k = smooth01(0.f, 1.5f, P.bc.hold);
        want = expf(logf(1.f / 400.f) + (logf(1.f / 12.f) - logf(1.f / 400.f)) * k);
    }
    const float a = 1.f - expf(-dt / 0.12f);
    P.bc.rate = expf(logf(P.bc.rate) + (logf(want) - logf(P.bc.rate)) * a);
    return P.bc.rate;
}

// the camera; false when it let go
bool bcam_camera(float dt)
{
    if (P.bc.idx < 0 || P.bc.idx >= int(P.take.bullets.size()))
    {
        P.bc.idx = -1;
        return false;
    }
    const BulletTrack& tr = P.take.bullets[P.bc.idx];
    BulletNow b;
    const bool flying = bullet_now(tr, P.t, b);
    P.bc.clock += dt;
    if (P.bc.phase == 1 && !flying && P.t > tr.born + 1e-4)
        P.bc.phase = 2; // it ended
    if (P.bc.phase == 1 && flying)
    {
        // the direction follows a ricochet's turn within ~0.08 s; through a portal at once
        const bool jump = P.bc.d_ok && b.pos.distance_to(P.bc.last) > 3.f;
        if (!P.bc.d_ok || jump)
            P.bc.d = b.dir;
        else
        {
            P.bc.d.lerp(P.bc.d, b.dir, 1.f - expf(-dt / 0.08f));
            if (P.bc.d.square_magnitude() < 1e-6f)
                P.bc.d = b.dir;
            P.bc.d.normalize();
        }
        P.bc.d_ok = true;
        P.bc.last = b.pos;
        const Fvector d = P.bc.d;
        Fvector up{0.f, 1.f, 0.f}, rr, n, o;
        if (_abs(d.y) > 0.98f)
            up.set(1.f, 0.f, 0.f);
        rr.crossproduct(up, d).normalize();
        n.crossproduct(d, rr);
        // aside and a little above, swinging slowly over it; riding 0.32 m behind and 0.07 m aside, looking 0.14 m ahead
        // of it: the bullet a little off the middle, its way and what it flies at in the frame
        const float th = deg2rad(25.f) + deg2rad(30.f) * _sin(P.bc.clock * 0.4f);
        o.mul(rr, _cos(th)).mad(n, _sin(th));
        const float s = b.path, r = bullet_path_total(tr) - s;
        const float st = smooth01(0.f, 1.5f, s); // 0: beside the muzzle, 1: riding behind it
        Fvector cam, look;
        cam.mad(b.pos, d, 0.05f - 0.37f * st).mad(o, 0.34f - 0.27f * st);
        look.mad(b.pos, d, 0.14f * st);
        if (r < 1.1f && s > 1.5f && !P.bc.held)
        {
            P.bc.held = true;
            P.bc.hold_p = cam;
        }
        if (P.bc.held)
        {
            cam = P.bc.hold_p;
            look = b.pos;
        }
        P.bc.p = cam;
        P.bc.look = look;
        // the fill light: between the camera and the bullet, above; not once the camera stops (it would light the hit)
        Fvector at;
        at.lerp(cam, b.pos, 0.6f).mad(n, 0.06f);
        bcam_light(!P.bc.held, at);
    }
    else if (P.bc.phase == 2)
    {
        bcam_light(false, P.bc.p);
        P.bc.hold += dt;
        P.bc.look.lerp(P.bc.look, tr.pts.back().pos, 1.f - expf(-dt / 0.3f));
        if (P.bc.hold > 3.5f)
        {
            bcam_stop(true);
            return false;
        }
    }
    else if (!P.bc.d_ok) // not shot yet (scrubbed back): beside where it starts, looking along its way
    {
        const Fvector& p0 = tr.pts.front().pos;
        Fvector d{0.f, 0.f, 1.f};
        if (tr.pts.size() > 1)
            d.sub(tr.pts[1].pos, p0);
        if (d.square_magnitude() < 1e-8f)
            d.set(0.f, 0.f, 1.f);
        d.normalize();
        P.bc.p.mad(tr.vis_from, d, 0.05f).y += 0.34f;
        P.bc.look = tr.vis_from;
    }
    Fvector dd;
    dd.sub(P.bc.look, P.bc.p);
    apply_camera(P.bc.p, dd, 0.f, P.bc.fov);
    return true;
}

void camera_frame(float dt)
{
    if (P.bc.idx >= 0 && bcam_camera(dt)) // E53c: the bullet camera
        return;
    // the flight (the free camera and the look-at one move; the eye view does not)
    if (!P.path_play && P.mode != CAM_EYE)
    {
        Fvector d = dir_of(P.h, P.pitch), r, mv{};
        r.crossproduct(Fvector().set(0.f, 1.f, 0.f), d);
        if (r.square_magnitude() < EPS)
            r.set(1.f, 0.f, 0.f);
        r.normalize();
        if (P.down[DIK_W])
            mv.add(d);
        if (P.down[DIK_S])
            mv.sub(d);
        if (P.down[DIK_D])
            mv.add(r);
        if (P.down[DIK_A])
            mv.sub(r);
        if (P.down[DIK_E])
            mv.y += 1.f;
        if (P.down[DIK_Q])
            mv.y -= 1.f;
        if (mv.square_magnitude() > EPS)
        {
            float v = P.fly * (shift_down() ? 4.f : 1.f) * (ctrl_down() ? 0.25f : 1.f);
            P.cam_p.mad(mv.normalize(), v * dt);
        }
    }
    if (P.down[DIK_Z])
        P.roll += deg2rad(30.f) * dt;
    if (P.down[DIK_C])
        P.roll -= deg2rad(30.f) * dt;

    if (P.path_play && P.keys.size() >= 2)
    {
        PathKey k;
        path_eval(P.path_u, k);
        apply_camera(k.p, dir_of(k.h, k.pitch), k.roll, k.fov);
        P.cam_p = k.p;
        P.h = k.h;
        P.pitch = k.pitch;
        P.roll = k.roll;
        P.fov = k.fov;
        return;
    }
    if (P.mode == CAM_EYE)
    {
        Fvector ep, ed;
        if (P.target >= 0 && P.target < int(P.take.objs.size()) && obj_eval(P.take.objs[P.target], P.t, nullptr, nullptr, nullptr, &ep, &ed))
        {
            apply_camera(ep, ed, P.roll, P.fov);
            return;
        }
        free_from_current(); // the character is gone (dead, out of the take)
    }
    if (P.mode == CAM_LOOK)
    {
        Fvector aim;
        if (obj_aim(P.target, P.t, aim))
        {
            Fvector d;
            d.sub(aim, P.cam_p);
            if (d.square_magnitude() > 1e-4f)
            {
                d.normalize();
                d.getHP(P.h, P.pitch);
            }
        }
    }
    clamp(P.pitch, -deg2rad(89.f), deg2rad(89.f));
    apply_camera(P.cam_p, dir_of(P.h, P.pitch), P.roll, P.fov);
}

// the track nearest the middle of the screen (eye: only those with eyes)
int pick_target(bool eye)
{
    int best = -1;
    float best_cos = _cos(deg2rad(25.f));
    for (u32 i = 0; i < P.take.objs.size(); ++i)
    {
        const ObjTrack& tr = P.take.objs[i];
        if (eye && !(tr.kind == K_CREATURE || tr.kind == K_ACTOR))
            continue;
        if (!eye && tr.parent != u16(-1))
            continue; // things in hands: the one holding them
        Fvector aim;
        if (eye)
        {
            if (!obj_eval(tr, P.t, nullptr, nullptr, nullptr, &aim, nullptr))
                continue;
        }
        else if (!obj_aim(int(i), P.t, aim))
            continue;
        Fvector d;
        d.sub(aim, Device.vCameraPosition);
        const float dist = d.magnitude();
        if (dist < 0.3f || dist > 500.f)
            continue;
        d.div(dist);
        const float c = d.dotproduct(Device.vCameraDirection) - dist * 0.00005f;
        if (c > best_cos)
        {
            best_cos = c;
            best = int(i);
        }
    }
    return best;
}

int track_of_id(int id)
{
    int found = -1;
    for (u32 i = 0; i < P.take.objs.size(); ++i)
        if (P.take.objs[i].id == u16(id))
        {
            found = int(i);
            if (obj_eval(P.take.objs[i], P.t, nullptr, nullptr, nullptr, nullptr, nullptr))
                return found; // the one there now
        }
    return found;
}

void lua_tick(float dt)
{
    try
    {
        luabind::functor<void> f;
        if (ai().script_engine().functor("portal_mod.on_replay_frame", f))
            f(dt);
    }
    catch (...)
    {
        Msg("! [portal_mod] replay: portal_mod.on_replay_frame failed");
    }
}

void seek(double t)
{
    clamp(t, 0.0, P.take.t1);
    if (_abs(t - P.t) > 1e-6)
        P.jumped = true;
    P.t = t;
}

void player_frame()
{
    if (!Device.GetTimerGlobal()->Paused())
        Device.GetTimerGlobal()->Pause(true); // the main menu lets it go when it closes
    const float dt = Device.fTimeDelta;
    P.prev_t = P.t;

    lua_tick(dt);
    if (!P.open)
        return; // closed from the script

    // the replay's time
    float rate = 0.f;
    bool coarse = false;
    if (P.path_play && P.keys.size() >= 2)
    {
        P.path_u += dt;
        if (P.path_u >= P.keys.back().u)
        {
            P.path_u = P.keys.back().u;
            P.path_play = false;
            P.paused = true;
            message("\xEF\xEE\xEB\xB8\xF2 \xEA\xEE\xED\xF7\xE8\xEB\xF1\xFF");
        }
        PathKey k;
        path_eval(P.path_u, k);
        const double nt = std::max(0.0, std::min(double(k.t), P.take.t1));
        rate = dt > EPS ? float((nt - P.t) / dt) : 0.f;
        if (_abs(nt - P.t) > 0.5)
            P.jumped = true;
        P.t = nt;
    }
    else
    {
        const bool held = P.scrub != 0 && (P.down[DIK_LEFT] || P.down[DIK_RIGHT]);
        if (held && (!P.paused || Device.dwTimeContinual - P.scrub_since > 300))
        {
            rate = float(P.scrub) * (shift_down() ? 8.f : 2.f);
            coarse = rate < 0.f;
        }
        else if (!P.paused)
            rate = P.bc.idx >= 0 ? bcam_rate(dt) : P.speed; // E53c: the bullet camera sets its own
        double nt = P.t + double(rate) * dt;
        if (nt >= P.take.t1)
        {
            nt = P.take.t1;
            if (!held)
                P.paused = true;
        }
        if (nt < 0.0)
            nt = 0.0;
        P.t = nt;
    }
    if (P.t < P.prev_t)
        coarse = true;

    env_update(P.t);
    portal_mod_replay_shader_time = P.shader_time;
    {
        // E53b: the rain on the replay's clock: it slows down with the replay, stands on a pause and scrubbing back
        const double rdt = std::max(0.0, P.t - P.prev_t);
        P.rain_acc += rdt * 1000.0;
        const u32 rms = u32(P.rain_acc);
        P.rain_acc -= double(rms);
        portal_mod_replay_rain_ms += rms;
        portal_mod_replay_rain_dms = rms;
        portal_mod_replay_rain_dt = float(rdt);
    }
    bullets_update(P.t, rate);
    puppets_update(P.t);
    ps_update(P.t, coarse);
    lights_update(P.t);
    sounds_update(P.t, P.prev_t, (P.t > P.prev_t) ? rate : 0.f);
    camera_frame(dt);
    P.jumped = false;
}

void player_close();

// E53c: a bullet's model and twist from its ammo ([pm_replay_bullets] in pm_replay.ltx: "ammo = model, twist in mm",
// the model meshes\portal_mod\pm_bullet_<model>.ogf; "none": only the glow); an ammo without a line (or a version 2
// take's bullet): `default`
void bullet_look(BulletTrack& tr)
{
    static xr_map<shared_str, std::pair<shared_str, float>> cache;
    tr.model = nullptr;
    tr.twist = 0.f;
    LPCSTR sect = "pm_replay_bullets";
    if (!pSettings->section_exist(sect))
        return;
    const shared_str key = tr.ammo.size() && pSettings->line_exist(sect, tr.ammo.c_str()) ? tr.ammo : shared_str("default");
    auto it = cache.find(key);
    if (it == cache.end())
    {
        std::pair<shared_str, float> look{};
        if (pSettings->line_exist(sect, key.c_str()))
        {
            LPCSTR v = pSettings->r_string(sect, key.c_str());
            string128 nm{};
            _GetItem(v, 0, nm);
            if (*nm && xr_strcmp(nm, "none"))
            {
                string_path f;
                xr_sprintf(f, "portal_mod\\pm_bullet_%s.ogf", nm);
                if (FS.exist("$game_meshes$", f))
                {
                    string_path m;
                    xr_sprintf(m, "portal_mod\\pm_bullet_%s", nm);
                    look.first = m;
                }
                else
                    Msg("! [portal_mod] replay: no bullet model meshes\\%s", f);
            }
            if (_GetItemCount(v) > 1)
            {
                string32 tw{};
                look.second = float(atof(_GetItem(v, 1, tw))) / 1000.f;
            }
        }
        it = cache.emplace(key, look).first;
    }
    tr.model = it->second.first;
    tr.twist = it->second.second;
}

// E53b: the bullets' timing for the replay, when a take opens.
// - The bullet's own time along its way is the way flown over its speed: the bullet manager's steps stall a bullet at
//   a hit it goes through for the rest of the step (33 ms in place), a portal's way in and out is one moment.
// - It reaches every point where a hit's particles (blood, sparks, dust) were played as they appear: they are played
//   two frames after the step that hit (the hit goes through the level's events and the server first), later than the
//   bullet's end was seen. A system born within 25 cm of a point (the end: from the shot to 80 ms after the end was
//   seen, the nearest in time) anchors that point to its birth; between the anchors the bullet's time runs evenly.
void bullets_prepare(Take& T)
{
    using namespace pm_replay;
    xr_vector<u8> used(T.ps.size(), 0);
    for (BulletTrack& tr : T.bullets)
    {
        bullet_look(tr); // E53c
        xr_vector<BulletPt>& v = tr.pts;
        tr.anch.clear();
        tr.cum.assign(v.size(), 0.f);
        if (v.empty())
            continue;
        v[0].tau = 0.f;
        for (size_t k = 1; k < v.size(); ++k)
        {
            const float d = (v[k].flags & BP_JUMP_TO) ? 0.f : v[k].pos.distance_to(v[k - 1].pos);
            float sp = 0.5f * (v[k].speed + v[k - 1].speed);
            if (!(sp > 50.f)) // (NaN too)
                sp = 50.f;
            v[k].tau = v[k - 1].tau + d / sp * 1000.f;
            tr.cum[k] = tr.cum[k - 1] + d;
        }
        if (!(tr.end > tr.born))
            continue; // still flying when the take ends: its own time from the shot
        const double raw_end = tr.end;
        double last_t = tr.born;
        tr.anch.emplace_back(0.f, tr.born);
        for (size_t k = 1; k < v.size(); ++k)
        {
            const bool is_end = k + 1 == v.size();
            if (!is_end && ((v[k].flags & (BP_JUMP_FROM | BP_JUMP_TO)) || v[k].pos.distance_to_sqr(v[k - 1].pos) < 1e-4f))
                continue; // a portal's way in or out; the same place again (stalled)
            int best = -1;
            double best_dt = 1e9;
            for (u32 j = 0; j < T.ps.size(); ++j)
            {
                const PsTrack& ps = T.ps[j];
                if (used[j] || ps.keys.empty() || ps.born <= last_t || ps.born > raw_end + 0.08)
                    continue;
                if (ps.keys.front().parent.c.distance_to_sqr(v[k].pos) > 0.25f * 0.25f)
                    continue;
                const double dt = is_end ? std::abs(ps.born - raw_end) : ps.born - last_t;
                if (dt < best_dt)
                {
                    best = int(j);
                    best_dt = dt;
                }
            }
            double at = -1.0;
            if (best >= 0)
            {
                used[best] = 1;
                at = T.ps[best].born;
            }
            else if (is_end)
                at = raw_end;
            if (at > last_t + 1e-4 && v[k].tau > tr.anch.back().first)
            {
                tr.anch.emplace_back(v[k].tau, at);
                last_t = at;
            }
        }
        if (tr.anch.back().first < v.back().tau) // the end not anchored: on at its own speed from the last anchor
            tr.anch.emplace_back(v.back().tau, last_t + double(v.back().tau - tr.anch.back().first) / 1000.0);
        tr.end = tr.anch.back().second;
    }
}

bool player_open(Take&& T, LPCSTR name)
{
    if (P.open)
        player_close();
    if (!g_pGameLevel || T.samples.empty())
        return false;
    if (T.level != Level().name())
    {
        string256 m;
        xr_sprintf(m, "%s - another level (%s)", name, T.level.c_str());
        message(m);
        return false;
    }
    if (R.manual)
        record_stop();
    P.take = std::move(T);
    bullets_prepare(P.take);
    P.name = name;
    g_last = name;
    P.puppets.assign(P.take.objs.size(), nullptr);
    P.ps.assign(P.take.ps.size(), nullptr);
    P.ps_bad.assign(P.take.ps.size(), 0);
    P.snd.clear();
    P.snd.resize(P.take.snd.size());
    P.snd_on.assign(P.take.snd.size(), 0);
    P.lights.clear();
    P.lights.resize(P.take.lights.size());
    P.tracers.clear();
    P.t = P.prev_t = 0.0;
    P.speed = 1.f;
    P.paused = false;
    P.jumped = true;
    P.ps_time = -1e9;
    P.ps_frac = 0.0;
    P.snd_running = false;
    P.weather = -1;
    P.mode = CAM_FREE;
    P.target = -1;
    P.cam_p = Device.vCameraPosition;
    Device.vCameraDirection.getHP(P.h, P.pitch);
    P.roll = 0.f;
    P.fov = P.fov0 = Device.fFOV > 1.f ? Device.fFOV : g_fov;
    P.keys.clear();
    P.path_play = false;
    P.path_u = 0.f;
    P.slot = 1;
    path_load(P.slot);
    memset(P.down, 0, sizeof(P.down));
    memset(P.pressed, 0, sizeof(P.pressed));
    P.scrub = 0;

    P.cine_was = portal_mod::cine;
    P.pd_was[0] = portal_mod_draw[0];
    P.pd_was[1] = portal_mod_draw[1];
    P.weather_was = g_pGamePersistent->Environment().GetWeather();
    P.timer_by_me = !Device.GetTimerGlobal()->Paused();
    if (P.timer_by_me)
        Device.GetTimerGlobal()->Pause(true);
    ::Sound->pause_emitters(true);
    P.snd_paused = true;
    P.shader_time = Device.fTimeGlobal;
    portal_mod_replay_shader_time = P.shader_time;
    portal_mod_replay_rain_ms = Device.dwTimeGlobal; // E53b: the live drops' times go on
    portal_mod_replay_rain_dms = 0;
    portal_mod_replay_rain_dt = 0.f;
    P.rain_acc = 0.0;
    portal_mod_replay_wind = g_pGamePersistent->Environment().wind_anim;
    g_bul_on = false; // no new bullets recorded while the world stands
    portal_mod_replay = true;
    portal_mod::cine = true; // no hands on the HUD, no first-person shadow of the player
    P.open = true;
    string256 m;
    xr_sprintf(m, "%s: %.1f s", name, float(P.take.t1));
    message(m);
    return true;
}

void player_close()
{
    if (!P.open)
        return;
    sounds_stop();
    ps_clear();
    for (Puppet*& p : P.puppets)
        xr_delete(p);
    P.puppets.clear();
    for (BulletModel*& m : P.bmodels) // E53c
        xr_delete(m);
    P.bmodels.clear();
    P.bc.idx = -1;
    P.bc.light = nullptr;
    P.ps.clear();
    P.snd.clear();
    P.snd_on.clear();
    P.lights.clear();
    P.tracers.clear();
    portal_mod_draw[0] = P.pd_was[0];
    portal_mod_draw[1] = P.pd_was[1];
    portal_mod::cine = P.cine_was;
    if (P.weather_was.size() && P.weather >= 0 && g_pGamePersistent)
        g_pGamePersistent->Environment().SetWeather(P.weather_was, true);
    portal_mod_replay = false;
    if (P.snd_paused)
        ::Sound->pause_emitters(false);
    P.snd_paused = false;
    if (P.timer_by_me)
        Device.GetTimerGlobal()->Pause(false);
    P.timer_by_me = false;
    P.open = false;
    P.path_play = false;
    P.take.clear();
    message("\xEF\xEE\xE2\xF2\xEE\xF0 \xE7\xE0\xEA\xF0\xFB\xF2");
}

bool open_named(LPCSTR name)
{
    Take T;
    if (!name || !*name)
        name = g_last.size() ? g_last.c_str() : "";
    if (!*name || !load_take(name, T))
    {
        string256 m;
        xr_sprintf(m, "no take '%s'", name);
        message(m);
        return false;
    }
    return player_open(std::move(T), name);
}

// End in the game: the manual recording, else the ring, else the last take
void open_now()
{
    xr_string nm;
    if (R.manual)
        nm = record_stop();
    else if (!R.ring.samples.empty())
        nm = save_ring(nullptr);
    open_named(nm.c_str());
}

const char* const MODE_NAMES[] = {"\xF1\xE2\xEE\xE1\xEE\xE4\xED\xE0\xFF", "\xF1\xEB\xE5\xE6\xE5\xED\xE8\xE5",
                                  "\xE3\xEB\xE0\xE7\xE0"};
} // namespace

//======================================================================================================================
namespace pm_replay
{
void on_frame()
{
    if (P.open)
        player_frame();
    else
        recorder_frame();
}

bool env_time(float& sec)
{
    if (!P.open || P.take.samples.empty())
        return false;
    sec = env_sec(P.t);
    return true;
}

void shutdown()
{
    player_close();
    R.clear();
    R.manual = false;
}

bool key_press(int dik)
{
    if (dik < 0 || dik >= 512)
        return false;
    if (!P.open)
    {
        if (dik != DIK_INSERT && dik != DIK_HOME && dik != DIK_END)
            return false;
        if (!g_pGameLevel || (HUD().GetUI() && HUD().GetUI()->MainInputReceiver()))
            return false;
        if (dik == DIK_INSERT)
        {
            if (R.manual)
            {
                const xr_string nm = record_stop();
                string256 m;
                xr_sprintf(m, "\xE4\xF3\xE1\xEB\xFC \xF1\xEE\xF5\xF0\xE0\xED\xB8\xED: %s", nm.size() ? nm.c_str() : "-");
                message(m);
            }
            else
            {
                load_cfg();
                record_start();
                message("REC");
            }
        }
        else if (dik == DIK_HOME)
        {
            load_cfg();
            const xr_string nm = save_ring(nullptr);
            string256 m;
            xr_sprintf(m, "\xE1\xF3\xF4\xE5\xF0 \xF1\xEE\xF5\xF0\xE0\xED\xB8\xED: %s", nm.size() ? nm.c_str() : "-");
            message(m);
        }
        else
            open_now();
        return true;
    }
    const EGameActions a = get_binded_action(dik);
    if (a == kCONSOLE || a == kQUIT || a == kSCREENSHOT)
        return false;
    P.down[dik] = true;
    P.pressed[dik] = true;
    const bool sh = shift_down();
    if (P.bc.idx >= 0 && (dik == DIK_LEFT || dik == DIK_RIGHT || dik == DIK_HOME || dik == DIK_P || dik == DIK_T || dik == DIK_V || dik == DIK_K))
        bcam_stop(false); // E53c: another camera or another moment
    switch (dik)
    {
    case DIK_B: // E53c: the bullet camera
        if (P.bc.idx >= 0)
            bcam_stop(false);
        else
        {
            bool from_shot = false;
            const int i = bcam_pick(from_shot);
            if (i >= 0)
                bcam_start(i, from_shot);
            else
                message("\xED\xE5\xF2 \xEF\xF3\xEB\xE8");
        }
        break;
    case DIK_END: player_close(); break;
    case DIK_SPACE:
        P.paused = !P.paused;
        if (!P.paused && P.t >= P.take.t1 - 1e-3)
            seek(0.0);
        P.path_play = false;
        break;
    case DIK_RIGHT:
    case DIK_LEFT:
        P.scrub = dik == DIK_RIGHT ? 1 : -1;
        P.scrub_since = Device.dwTimeContinual;
        P.path_play = false;
        if (P.paused)
            seek(P.t + P.scrub / 60.0);
        break;
    case DIK_UP: P.speed = std::min(4.f, P.speed * 2.f); break;
    case DIK_DOWN: P.speed = std::max(1.f / 256.f, P.speed * 0.5f); break; // E53b: down to 1/256
    case DIK_HOME:
        seek(0.0);
        P.path_play = false;
        break;
    case DIK_K:
        if (P.mode != CAM_FREE)
            free_from_current();
        key_add(-1.f, float(P.t), P.cam_p, P.h, P.pitch, P.roll, P.fov);
        path_save(P.slot);
        break;
    case DIK_DELETE:
        if (sh)
            P.keys.clear();
        else if (!P.keys.empty())
            P.keys.pop_back();
        path_save(P.slot);
        break;
    case DIK_P:
        if (P.path_play)
            P.path_play = false;
        else if (P.keys.size() >= 2)
        {
            P.path_play = true;
            P.path_u = 0.f;
            P.mode = CAM_FREE;
            P.target = -1;
            seek(P.keys.front().t);
        }
        break;
    case DIK_T:
        if (P.mode == CAM_LOOK)
            free_from_current();
        else if ((P.target = pick_target(false)) >= 0)
        {
            if (P.mode == CAM_EYE)
            {
                P.cam_p = Device.vCameraPosition;
                Device.vCameraDirection.getHP(P.h, P.pitch);
            }
            P.mode = CAM_LOOK;
        }
        break;
    case DIK_V:
        if (P.mode == CAM_EYE)
            free_from_current();
        else if ((P.target = pick_target(true)) >= 0)
            P.mode = CAM_EYE;
        break;
    case DIK_X:
        P.roll = 0.f;
        P.fov = P.fov0;
        P.bc.fov = 24.f; // E53c
        break;
    case DIK_H: P.hint = (P.hint + 1) % 3; break;
    default:
        if (dik >= DIK_1 && dik <= DIK_9)
        {
            P.slot = dik - DIK_1 + 1;
            P.path_play = false;
            if (!path_load(P.slot))
                P.keys.clear();
        }
        break;
    }
    return true;
}

bool key_release(int dik)
{
    if (!P.open || dik < 0 || dik >= 512)
        return false;
    if (!P.pressed[dik])
        return false; // held from before the replay: the game gets it, the player stops walking
    P.pressed[dik] = false;
    P.down[dik] = false;
    if ((dik == DIK_LEFT || dik == DIK_RIGHT) && !P.down[DIK_LEFT] && !P.down[DIK_RIGHT])
        P.scrub = 0;
    return true;
}

bool key_hold(int dik)
{
    if (!P.open)
        return false;
    const EGameActions a = get_binded_action(dik);
    return !(a == kCONSOLE || a == kQUIT || a == kSCREENSHOT);
}

bool mouse_move(int dx, int dy)
{
    if (!P.open)
        return false;
    if (P.path_play || P.bc.idx >= 0) // E53c: the bullet camera turns by itself
        return true;
    if (P.mode != CAM_FREE)
        free_from_current();
    const float scale = psMouseSens * psMouseSensScale / 50.f * (P.fov / (g_fov > 1.f ? g_fov : 67.5f)) * 0.75f;
    P.h -= dx * scale;
    P.pitch -= (psMouseInvert.test(1) ? -dy : dy) * scale;
    clamp(P.pitch, -deg2rad(89.f), deg2rad(89.f));
    return true;
}

bool mouse_wheel(int dir)
{
    if (!P.open)
        return false;
    if (shift_down())
    {
        P.fly *= dir > 0 ? 1.25f : 0.8f;
        clamp(P.fly, 0.25f, 200.f);
    }
    else if (P.bc.idx >= 0) // E53c
    {
        P.bc.fov *= dir > 0 ? 0.9f : 1.f / 0.9f;
        clamp(P.bc.fov, 5.f, 120.f);
    }
    else
    {
        P.fov *= dir > 0 ? 0.9f : 1.f / 0.9f;
        clamp(P.fov, 5.f, 120.f);
    }
    return true;
}

bool clean_frame() { return P.open; }

void render_ui()
{
    CGameFont* F = HUD().Font().pFontLetterica16Russian;
    if (!F)
        return;
    const float W = float(Device.dwWidth), H = float(Device.dwHeight);
    const float x = W * 0.02f, lh = F->CurrentHeight_() * 1.1f;
    F->SetAligment(CGameFont::alLeft);
    const bool msg = Device.dwTimeContinual < g_msg_until && !g_msg.empty();
    if (!P.open)
    {
        float y = H * 0.04f;
        if (R.manual)
        {
            F->SetColor(0xFFFF3030);
            F->Out(x, y, "REC %.1f", float(now_sec() - R.manual_t0));
            y += lh;
        }
        if (msg)
        {
            F->SetColor(0xFFFFD040);
            F->Out(x, y, "%s", g_msg.c_str());
        }
        return;
    }
    if (P.hint == 0)
        return;
    string512 line;
    LPCSTR mode = P.bc.idx >= 0 ? "\xEF\xF3\xEB\xFF" : (P.path_play ? "\xEF\xEE\xEB\xB8\xF2" : MODE_NAMES[P.mode]);
    string32 spd; // E53b: a slow speed as a fraction (x1/256)
    const float sp = P.bc.idx >= 0 ? P.bc.rate : P.speed; // E53c: the bullet camera's own
    if (sp < 0.999f)
        xr_sprintf(spd, "x1/%d", iFloor(1.f / sp + 0.5f));
    else
        xr_sprintf(spd, "x%.3g", sp);
    xr_sprintf(line, "\xCF\xCE\xC2\xD2\xCE\xD0 %s   %6.3f / %.2f   %s %s   \xEA\xE0\xEC\xE5\xF0\xE0: %s   FOV %.0f   %.2g \xEC/\xF1   \xEA\xEB\xFE\xF7\xE8: %u (\xF1\xEB\xEE\xF2 %d)   H - \xEF\xEE\xE4\xF1\xEA\xE0\xE7\xEA\xE0",
               P.name.c_str(), float(P.t), float(P.take.t1), spd, P.paused ? "||" : ">", mode, P.bc.idx >= 0 ? P.bc.fov : P.fov, P.fly,
               u32(P.keys.size()), P.slot);
    float y = H * 0.95f;
    if (P.hint == 2)
    {
        static const char* help[] = {
            "\xEC\xFB\xF8\xFC - \xEE\xE1\xE7\xEE\xF0, W/A/S/D - \xEB\xE5\xF2\xE5\xF2\xFC, E/Q - \xE2\xE2\xE5\xF0\xF5/\xE2\xED\xE8\xE7, Shift x4, Ctrl x1/4, \xEA\xEE\xEB\xE5\xF1\xEE - FOV, Shift+\xEA\xEE\xEB\xE5\xF1\xEE - \xF1\xEA\xEE\xF0\xEE\xF1\xF2\xFC \xEF\xEE\xEB\xB8\xF2\xE0",
            "Z/C - \xEA\xF0\xE5\xED, X - \xF1\xE1\xF0\xEE\xF1\xE8\xF2\xFC \xEA\xF0\xE5\xED \xE8 FOV, \xEF\xF0\xEE\xE1\xE5\xEB - \xEF\xE0\xF3\xE7\xE0, Home - \xE2 \xED\xE0\xF7\xE0\xEB\xEE, End - \xE2\xFB\xE9\xF2\xE8 \xE8\xE7 \xEF\xEE\xE2\xF2\xEE\xF0\xE0",
            "\xF1\xF2\xF0\xE5\xEB\xEA\xE8 \xE2\xEB\xE5\xE2\xEE/\xE2\xEF\xF0\xE0\xE2\xEE: \xE4\xE5\xF0\xE6\xE0\xF2\xFC - \xED\xE0\xE7\xE0\xE4/\xE2\xEF\xE5\xF0\xB8\xE4 x2 (Shift x8), \xED\xE0 \xEF\xE0\xF3\xE7\xE5 - \xEF\xEE \xEA\xE0\xE4\xF0\xF3; \xE2\xE2\xE5\xF0\xF5/\xE2\xED\xE8\xE7 - \xF1\xEA\xEE\xF0\xEE\xF1\xF2\xFC x2 / :2",
            "K - \xEA\xEB\xFE\xF7 \xEA\xE0\xEC\xE5\xF0\xFB, Delete - \xF3\xE1\xF0\xE0\xF2\xFC \xEF\xEE\xF1\xEB\xE5\xE4\xED\xE8\xE9 (Shift - \xE2\xF1\xE5), P - \xEF\xEE\xEB\xB8\xF2 \xEF\xEE \xEA\xEB\xFE\xF7\xE0\xEC, 1-9 - \xF1\xEB\xEE\xF2 \xEF\xF3\xF2\xE8",
            "T - \xF1\xEB\xE5\xE4\xE8\xF2\xFC \xE7\xE0 \xEE\xE1\xFA\xE5\xEA\xF2\xEE\xEC \xE2 \xF6\xE5\xED\xF2\xF0\xE5 \xEA\xE0\xE4\xF0\xE0, V - \xE3\xEB\xE0\xE7\xE0\xEC\xE8 \xEF\xE5\xF0\xF1\xEE\xED\xE0\xE6\xE0 \xE2 \xF6\xE5\xED\xF2\xF0\xE5 \xEA\xE0\xE4\xF0\xE0, H - \xEF\xEE\xE4\xF1\xEA\xE0\xE7\xEA\xE0",
            "B - \xEA\xE0\xEC\xE5\xF0\xE0 \xE7\xE0 \xEF\xF3\xEB\xE5\xE9: \xEB\xE5\xF2\xFF\xF9\xE5\xE9 \xE2 \xF6\xE5\xED\xF2\xF0\xE5 \xEA\xE0\xE4\xF0\xE0 \xE8\xEB\xE8 \xF1\xEB\xE5\xE4\xF3\xFE\xF9\xE5\xE9 \xE2\xFB\xEF\xF3\xF9\xE5\xED\xED\xEE\xE9 (\xE7\xE0\xEC\xE5\xE4\xEB\xFF\xE5\xF2 \xF1\xE0\xEC\xE0; B \xE5\xF9\xB8 \xF0\xE0\xE7 - \xE2\xFB\xE9\xF2\xE8)",
        };
        F->SetColor(0xE0FFFFFF);
        float hy = y - lh * (float(_countof(help)) + 1.5f);
        for (const char* s : help)
        {
            F->Out(x, hy, "%s", s);
            hy += lh;
        }
    }
    F->SetColor(0xFFFFFFFF);
    F->Out(x, y, "%s", line);
    if (msg)
    {
        F->SetColor(0xFFFFD040);
        F->Out(x, y - lh, "%s", g_msg.c_str());
    }
}

const xr_vector<Tracer>* tracers() { return P.open ? &P.tracers : nullptr; }

// E53b: a new bullet (CBulletManager::AddBullet, the main thread): its track's id, 0 when nothing records
u32 bullet_born(const SBullet& b, u16 sender_id, u16 weapon_id, LPCSTR ammo)
{
    if (!g_bul_on.load(std::memory_order_relaxed) || !_valid(b.pos))
        return 0;
    BulletEvent e{};
    e.id = ++g_bul_next;
    if (!e.id)
        e.id = ++g_bul_next;
    e.born_ms = Device.dwTimeGlobal ? Device.dwTimeGlobal : 1;
    e.pt = BulletPt{0.f, b.pos, b.speed, 0};
    e.tracer = b.flags.allow_tracer ? 1 : 0;
    e.color = b.m_u8ColorID;
    e.vis_from = b.pos;
    if (ammo && *ammo)
        e.ammo = ammo; // E53c
    // a first-person shot starts at the eye: it is seen to leave the world model's muzzle
    CActor* a = Actor();
    if (a && a->ID() == sender_id)
        if (CWeapon* w = smart_cast<CWeapon*>(Level().Objects.net_Find(weapon_id)))
        {
            Fmatrix H, W;
            if (w->PmFireFrames(H, W))
                e.vis_from = W.c;
        }
    std::lock_guard<std::mutex> lk(g_bul_lock);
    g_bul_events.push_back(e);
    return e.id;
}

// a point of a bullet's path (the bullet manager's worker thread too)
void bullet_point(u32 id, float tau, const Fvector& pos, float speed, u32 flags)
{
    if (!id || !_valid(pos) || !_valid(tau))
        return;
    BulletEvent e{};
    e.id = id;
    e.pt = BulletPt{tau, pos, _valid(speed) ? speed : 0.f, flags};
    std::lock_guard<std::mutex> lk(g_bul_lock);
    g_bul_events.push_back(e);
}

// Lua
LPCSTR lua_record(bool on)
{
    static xr_string s;
    load_cfg();
    if (on)
    {
        record_start();
        s.clear();
    }
    else
        s = record_stop();
    return s.c_str();
}
bool lua_recording() { return R.manual; }
void lua_ring(float seconds)
{
    load_cfg();
    R.ring_seconds = seconds < 0.f ? 0.f : seconds;
}
LPCSTR lua_save_ring(LPCSTR name)
{
    static xr_string s;
    load_cfg();
    s = save_ring(name);
    return s.c_str();
}
bool lua_open_take(LPCSTR name) { return open_named(name); }
void lua_close() { player_close(); }
bool lua_active() { return P.open; }
float lua_time() { return float(P.t); }
float lua_length() { return float(P.take.t1); }
void lua_seek(float t)
{
    if (P.open)
        seek(t);
}
void lua_speed(float k)
{
    P.speed = k;
    clamp(P.speed, 1.f / 256.f, 4.f); // E53b
}
void lua_pause(bool on) { P.paused = on; }
void lua_cam(Fvector pos, Fvector dir, float fov, float roll)
{
    P.mode = CAM_FREE;
    P.target = -1;
    P.path_play = false;
    P.cam_p = pos;
    if (dir.square_magnitude() > EPS)
        dir.normalize().getHP(P.h, P.pitch);
    if (fov > 1.f)
        P.fov = fov;
    P.roll = roll;
}
Fvector lua_cam_pos() { return Device.vCameraPosition; }
Fvector lua_cam_dir() { return Device.vCameraDirection; }
void lua_look_at(int id)
{
    if (id < 0)
    {
        free_from_current();
        return;
    }
    const int ti = track_of_id(id);
    if (ti < 0)
        return;
    if (P.mode == CAM_EYE)
    {
        P.cam_p = Device.vCameraPosition;
        Device.vCameraDirection.getHP(P.h, P.pitch);
    }
    P.target = ti;
    P.mode = CAM_LOOK;
}
void lua_eye(int id)
{
    if (id < 0)
    {
        free_from_current();
        return;
    }
    const int ti = track_of_id(id);
    if (ti < 0)
        return;
    P.target = ti;
    P.mode = CAM_EYE;
    P.path_play = false;
}
void lua_key_add(float u, float t, Fvector pos, Fvector dir, float fov, float roll)
{
    float h = 0.f, p = 0.f;
    if (dir.square_magnitude() > EPS)
        dir.normalize().getHP(h, p);
    key_add(u, t, pos, h, p, roll, fov > 1.f ? fov : P.fov);
}
void lua_key_clear() { P.keys.clear(); }
int lua_key_count() { return int(P.keys.size()); }
void lua_path_play(bool on)
{
    if (on && P.keys.size() >= 2)
    {
        P.path_play = true;
        P.path_u = 0.f;
        P.mode = CAM_FREE;
        P.target = -1;
        seek(P.keys.front().t);
    }
    else
        P.path_play = false;
}
bool lua_path_playing() { return P.path_play; }
bool lua_path_save(int slot)
{
    P.slot = slot;
    return path_save(slot);
}
bool lua_path_load(int slot)
{
    P.slot = slot;
    return path_load(slot);
}
void lua_hint(int level) { P.hint = level < 0 ? 0 : (level > 2 ? 2 : level); }
float lua_clock() { return float(Device.dwTimeContinual) / 1000.f; }
LPCSTR lua_info()
{
    static string1024 s; // E53c: longer (the bullet camera's fields)
    const Take& T = P.open ? P.take : R.ring;
    u32 bones = 0, keys = 0, vis_now = 0;
    for (const ObjTrack& tr : T.objs)
    {
        keys += u32(tr.keys.size());
        bones += tr.nbones;
    }
    if (P.open)
        for (const Puppet* p : P.puppets)
            vis_now += (p && p->in_space) ? 1 : 0;
    u32 ps_now = 0;
    if (P.open)
        for (const ReplayPS* p : P.ps)
            ps_now += p ? 1 : 0;
    u32 models_now = 0; // E53c
    for (const BulletModel* m : P.bmodels)
        models_now += m->in_space ? 1 : 0;
    const BulletTrack* bc = P.open && P.bc.idx >= 0 && P.bc.idx < int(P.take.bullets.size()) ? &P.take.bullets[P.bc.idx] : nullptr;
    xr_sprintf(s, "%s len=%.2f samples=%u objs=%u keys=%u bones=%u ps=%u snd=%u lights=%u bullets=%u puppets_now=%u ps_now=%u drawn=%u rec=%d "
                  "models_now=%u bcam=%d bcam_rate=%.6f bcam_ammo=%s bcam_model=%s",
               P.open ? "replay" : "ring", float(T.t1 - T.t0), u32(T.samples.size()), u32(T.objs.size()), keys, bones, u32(T.ps.size()), u32(T.snd.size()),
               u32(T.lights.size()), u32(T.bullets.size()), vis_now, ps_now, P.open ? u32(P.tracers.size()) : 0u, R.manual ? 1 : 0, models_now,
               bc ? P.bc.phase : 0, bc ? P.bc.rate : 0.f, bc && bc->ammo.size() ? bc->ammo.c_str() : "-", bc && bc->model.size() ? bc->model.c_str() : "-");
    return s;
}
Fvector lua_object_pos(int id)
{
    Fvector p{};
    const int ti = P.open ? track_of_id(id) : -1;
    if (ti >= 0)
    {
        Fmatrix xf;
        if (obj_eval(P.take.objs[ti], P.t, &xf, nullptr, nullptr, nullptr, nullptr))
            p = xf.c;
    }
    return p;
}
LPCSTR lua_last() { return g_last.size() ? g_last.c_str() : ""; }
// E53c: the bullet camera on the bullet whose end (its hit) was nearest t, within 0.25 s; t < 0: off. With a place: of
// the bullets that ended within 0.4 s of t, the one whose way passed nearest it (within 4 m; else the nearest in time)
float bullet_way_dist(const BulletTrack& tr, const Fvector& p)
{
    float best = flt_max;
    const xr_vector<BulletPt>& v = tr.pts;
    for (size_t j = 0; j < v.size(); ++j)
    {
        best = std::min(best, p.distance_to(v[j].pos));
        if (j + 1 >= v.size() || (v[j + 1].flags & pm_replay::BP_JUMP_TO))
            continue;
        Fvector ab, ap;
        ab.sub(v[j + 1].pos, v[j].pos);
        ap.sub(p, v[j].pos);
        const float l2 = ab.square_magnitude();
        if (l2 < 1e-8f)
            continue;
        float k = ap.dotproduct(ab) / l2;
        clamp(k, 0.f, 1.f);
        Fvector q;
        q.mad(v[j].pos, ab, k);
        best = std::min(best, p.distance_to(q));
    }
    return best;
}
int bullet_pick(float t, const Fvector* at)
{
    int best = -1, near_best = -1;
    double bd = 0.25;
    float nd = 4.f;
    for (u32 i = 0; i < P.take.bullets.size(); ++i)
    {
        const BulletTrack& tr = P.take.bullets[i];
        if (tr.pts.size() < 2 || bullet_path_total(tr) < 2.f)
            continue;
        const double d = _abs(bullet_end_t(tr) - double(t));
        if (d < bd)
        {
            bd = d;
            best = int(i);
        }
        if (at && d < 0.4)
        {
            const float w = bullet_way_dist(tr, *at);
            if (w < nd)
            {
                nd = w;
                near_best = int(i);
            }
        }
    }
    return near_best >= 0 ? near_best : best;
}
bool lua_bullet_cam(float t)
{
    if (!P.open)
        return false;
    if (t < 0.f)
    {
        bcam_stop(false);
        return true;
    }
    const int best = bullet_pick(t, nullptr);
    if (best < 0)
        return false;
    bcam_start(best, true);
    return true;
}
bool lua_bullet_cam_at(float t, Fvector pos)
{
    if (!P.open)
        return false;
    const int best = bullet_pick(t, &pos);
    if (best < 0)
        return false;
    bcam_start(best, true);
    return true;
}
int lua_bullet_cam_state() { return P.bc.idx < 0 ? 0 : P.bc.phase; }
} // namespace pm_replay
