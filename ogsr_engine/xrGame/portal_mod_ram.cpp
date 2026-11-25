// portal-mod E72: vehicles break what they ram (see portal_mod_ram.h; tools/engine_patches/patch_e72.py).
//
// Two ways in:
//  - The level's own breakables are static collision triangles (E54 building pieces, E58 trunks, E59 poles, fences,
//    lamps): a car that met them would stop dead on its first physics step. So every frame (CCar::UpdateCL) the car's
//    box, swept ahead along its velocity by lead_time, is tested against the static model; for each triangle in it the
//    owner is found through the modules' own Lua-side queries (statics_near, trees_near, destruct_piece_at) and its
//    hit points weighed against the car's energy into that face. Enough: it is broken now (statics_break / trees_fell /
//    a building piece's damage as from a round), before the car gets there, and the car pays the energy and a dent.
//    The freed body (a fence piece, a pole's top, a building's loose parts) is then met by the car under physics - it
//    flies as the car's mass and speed throw it. Not enough: a tree or a building piece keeps the damage (the next ram
//    adds to it), and the car crashes as it always did.
//  - Physics objects: the car's geoms get one more object contact callback (physics thread). A contact closing faster
//    than prop_min_speed is queued; the main thread gives a prop (E57) a blow by the contact's energy (reduced mass),
//    and tears a door or a gate leaf (a "door" joint) off its hinge or rail when the closing speed is over door_speed -
//    its contacts with the car are ignored from the first one until then, so the car does not stop on a hinge.
#include "stdafx.h"
#include <algorithm>
#include <mutex>
#include "portal_mod_ram.h"
#include "portal_mod_destruct.h"
#include "portal_mod_statics.h"
#include "portal_mod_trees.h"
#include "portal_mod_props.h"
#include "portal_mod_features.h" // portal-mod E55: the destruction's switch
#include "alife_space.h"
#include "hit.h"
#include "PHDestroyable.h"
#include "Car.h"
#include "PhysicObject.h"
#include "PhysicsShell.h"
#include "PhysicsShellHolder.h"
#include "PhysicsCommon.h"
#include "ExtendedGeom.h"
#include "Level.h"
#include "Level_Bullet_Manager.h"
#include "../xr_3da/GameMtlLib.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/RenderVisual.h"

namespace portal_mod_ram
{
namespace
{
enum EKind : u8
{
    K_STATICS,
    K_TREE,
    K_PIECE,
    K_PROP,
    K_DOOR,
    K_COUNT
};
LPCSTR const KIND_NAME[K_COUNT] = {"statics", "tree", "piece", "prop", "door"};

struct Cfg
{
    bool loaded{}, enabled{true}, debug{};
    float min_speed{3.f}, lead_time{0.12f}, lead_add{0.35f}, floor_clear{0.3f};
    int max_tris{96};
    float j_statics[8]{90.f, 1100.f, 600.f, 90.f, 280.f, 180.f, 40.f, 40.f}; // per hit point, statics_build's KINDS order
    float j_statics_other{400.f};
    float j_tree{420.f}, j_piece{300.f}, piece_absorb{0.35f};
    float absorb{1.f}, dent_k{1.2e-4f}, keep_s{2.f};
    float glance{0.35f}; // the least share of the speed that counts into a face struck at a slant
    // physics objects
    float prop_min_speed{2.5f}, j_prop_wood{5.f}, j_prop_metal{10.f}, j_prop_glass{1.f}, j_prop_other{6.f}, k_strike{60.f};
    float door_speed{5.f}, door_speed_heavy{7.f}, door_heavy{150.f}, door_throw{1.15f}, door_lift{1.5f};
};

struct CarRec
{
    CPhysicsShell* shell{};
    u32 frame{};
    u32 scan{}; // next door scan (ms)
    xr_vector<std::pair<u32, u32>> recent; // (kind << 24 | id, until ms)
};

struct Contact // a car's contact with a physics object (physics thread -> main thread)
{
    u16 car, other;
    Fvector pos, dir;
    float vn;
};

struct State
{
    void* level{};
    bool on{true};
    xr_map<u16, CarRec> cars;
    std::mutex mx; // guards contacts, doors, door_pass
    xr_vector<Contact> contacts;
    xr_vector<u16> doors; // physics objects near a car with a live "door" joint (callback: may pass)
    xr_vector<u16> door_pass; // doors being torn off: their contacts with cars are ignored
    xr_string events;
    u32 broken[K_COUNT]{}, tried[K_COUNT]{};
    float last_E{}, last_v0{}, last_v1{};
    xr_string last_what;
};

Cfg C;
State S;

float cfg_f(LPCSTR s, LPCSTR k, float def) { return pSettings->line_exist(s, k) ? pSettings->r_float(s, k) : def; }

void load_cfg()
{
    if (C.loaded)
        return;
    C.loaded = true;
    LPCSTR s = "pm_ram";
    if (!pSettings->section_exist(s))
        return;
    C.enabled = pSettings->line_exist(s, "enabled") ? !!pSettings->r_bool(s, "enabled") : true;
    C.debug = pSettings->line_exist(s, "debug") ? !!pSettings->r_bool(s, "debug") : false;
    C.min_speed = cfg_f(s, "min_speed", C.min_speed);
    C.lead_time = cfg_f(s, "lead_time", C.lead_time);
    C.lead_add = cfg_f(s, "lead_add", C.lead_add);
    C.floor_clear = cfg_f(s, "floor_clear", C.floor_clear);
    C.max_tris = (int)cfg_f(s, "max_tris", (float)C.max_tris);
    LPCSTR const SK[8] = {"j_wood_pole", "j_concrete_pole", "j_metal_pole", "j_wood_fence", "j_concrete_fence", "j_mesh_fence", "j_lamp", "j_wire"};
    for (int i = 0; i < 8; ++i)
        C.j_statics[i] = cfg_f(s, SK[i], C.j_statics[i]);
    C.j_statics_other = cfg_f(s, "j_statics_other", C.j_statics_other);
    C.j_tree = cfg_f(s, "j_tree", C.j_tree);
    C.j_piece = cfg_f(s, "j_piece", C.j_piece);
    C.piece_absorb = cfg_f(s, "piece_absorb", C.piece_absorb);
    C.absorb = cfg_f(s, "absorb", C.absorb);
    C.dent_k = cfg_f(s, "dent_k", C.dent_k);
    C.keep_s = cfg_f(s, "keep_s", C.keep_s);
    C.glance = cfg_f(s, "glance", C.glance);
    C.prop_min_speed = cfg_f(s, "prop_min_speed", C.prop_min_speed);
    C.j_prop_wood = cfg_f(s, "j_prop_wood", C.j_prop_wood);
    C.j_prop_metal = cfg_f(s, "j_prop_metal", C.j_prop_metal);
    C.j_prop_glass = cfg_f(s, "j_prop_glass", C.j_prop_glass);
    C.j_prop_other = cfg_f(s, "j_prop_other", C.j_prop_other);
    C.k_strike = cfg_f("pm_props", "k_strike", C.k_strike); // E57's own: damage per power of a blow
    C.door_speed = cfg_f(s, "door_speed", C.door_speed);
    C.door_speed_heavy = cfg_f(s, "door_speed_heavy", C.door_speed_heavy);
    C.door_heavy = cfg_f(s, "door_heavy", C.door_heavy);
    C.door_throw = cfg_f(s, "door_throw", C.door_throw);
    C.door_lift = cfg_f(s, "door_lift", C.door_lift);
}

// $app_data_root$\mod_manager.ltx's "ram = 0/1" (the My Mods window's file, E55's way): read when a level is made
void read_switch()
{
    S.on = true;
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
        if (xr_strcmp(_Trim(line), "ram"))
            continue;
        LPSTR v = _Trim(eq + 1);
        S.on = !xr_strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true");
    }
    fclose(f);
    Msg("* E72 My Mods: ram %d", int(S.on));
}

void level_check()
{
    if (S.level == (void*)g_pGameLevel)
        return;
    S.level = (void*)g_pGameLevel;
    S.cars.clear();
    {
        std::lock_guard<std::mutex> lk(S.mx);
        S.contacts.clear();
        S.doors.clear();
        S.door_pass.clear();
    }
    S.events.clear();
    read_switch();
}

bool is_on() { return C.enabled && S.on; }
bool destruct_on() { return portal_mod_feat::enabled(portal_mod_feat::destruct); }

void event(EKind k, u32 id, const Fvector& p, float E, float v0, float v1)
{
    ++S.broken[k];
    S.last_E = E, S.last_v0 = v0, S.last_v1 = v1;
    string256 one;
    xr_sprintf(one, "%s|%u|%.2f|%.2f|%.2f|%.1f|%.1f|%.1f;", KIND_NAME[k], id, p.x, p.y, p.z, E * 0.001f, v0, v1);
    S.last_what = one;
    if (S.events.size() < 8192)
        S.events += one;
}

bool recent(CarRec& r, EKind k, u32 id)
{
    const u32 key = (u32(k) << 24) | (id & 0xffffff), now = Device.dwTimeGlobal;
    for (auto& e : r.recent)
        if (e.first == key && e.second > now)
            return true;
    return false;
}

void remember(CarRec& r, EKind k, u32 id)
{
    const u32 key = (u32(k) << 24) | (id & 0xffffff), now = Device.dwTimeGlobal;
    r.recent.erase(std::remove_if(r.recent.begin(), r.recent.end(), [now](const std::pair<u32, u32>& e) { return e.second <= now; }), r.recent.end());
    r.recent.emplace_back(key, now + u32(C.keep_s * 1000.f));
}

// "a|b|c;d|e|f;" -> the first record whose field `fid` is `id` (or the first record at all for id < 0)
bool find_rec(LPCSTR list, int id, xr_vector<xr_string>& out)
{
    out.clear();
    if (!list || !*list)
        return false;
    LPCSTR p = list;
    while (*p)
    {
        LPCSTR e = strchr(p, ';');
        if (!e)
            e = p + xr_strlen(p);
        xr_vector<xr_string> f;
        LPCSTR q = p;
        while (q < e)
        {
            LPCSTR b = q;
            while (q < e && *q != '|')
                ++q;
            f.emplace_back(b, q);
            if (q < e)
                ++q;
        }
        if (!f.empty() && (id < 0 || atoi(f[0].c_str()) == id))
        {
            out.swap(f);
            return true;
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

// all records of a list (each split on '|')
void all_recs(LPCSTR list, xr_vector<xr_vector<xr_string>>& out)
{
    out.clear();
    if (!list)
        return;
    LPCSTR p = list;
    while (*p)
    {
        LPCSTR e = strchr(p, ';');
        if (!e)
            e = p + xr_strlen(p);
        xr_vector<xr_string> f;
        LPCSTR q = p;
        while (q < e)
        {
            LPCSTR b = q;
            while (q < e && *q != '|')
                ++q;
            f.emplace_back(b, q);
            if (q < e)
                ++q;
        }
        if (!f.empty())
            out.push_back(std::move(f));
        p = *e ? e + 1 : e;
    }
}

// a triangle against an axis-aligned box (both in the car's frame): the separating axis test (13 axes)
bool tri_box(const Fvector* v, const Fbox& b)
{
    Fvector c, h;
    b.get_CD(c, h);
    Fvector p[3];
    for (int i = 0; i < 3; ++i)
        p[i].sub(v[i], c);
    for (int a = 0; a < 3; ++a)
    {
        const float lo = _min(p[0][a], _min(p[1][a], p[2][a])), hi = _max(p[0][a], _max(p[1][a], p[2][a]));
        if (lo > h[a] || hi < -h[a])
            return false;
    }
    Fvector e[3];
    e[0].sub(p[1], p[0]);
    e[1].sub(p[2], p[1]);
    e[2].sub(p[0], p[2]);
    Fvector n;
    n.crossproduct(e[0], e[1]);
    {
        const float d = n.dotproduct(p[0]), r = h.x * _abs(n.x) + h.y * _abs(n.y) + h.z * _abs(n.z);
        if (d > r || d < -r)
            return false;
    }
    const Fvector axes[3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            Fvector a;
            a.crossproduct(axes[j], e[i]);
            if (a.square_magnitude() < 1e-10f)
                continue;
            const float d0 = a.dotproduct(p[0]), d1 = a.dotproduct(p[1]), d2 = a.dotproduct(p[2]);
            const float r = h.x * _abs(a.x) + h.y * _abs(a.y) + h.z * _abs(a.z);
            if (_min(d0, _min(d1, d2)) > r || _max(d0, _max(d1, d2)) < -r)
                return false;
        }
    return true;
}

// the point of a triangle nearest to p
Fvector closest_on_tri(const Fvector& p, const Fvector& a, const Fvector& b, const Fvector& c)
{
    Fvector ab, ac, ap;
    ab.sub(b, a), ac.sub(c, a), ap.sub(p, a);
    const float d1 = ab.dotproduct(ap), d2 = ac.dotproduct(ap);
    if (d1 <= 0.f && d2 <= 0.f)
        return a;
    Fvector bp;
    bp.sub(p, b);
    const float d3 = ab.dotproduct(bp), d4 = ac.dotproduct(bp);
    if (d3 >= 0.f && d4 <= d3)
        return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
        return Fvector().mad(a, ab, d1 / (d1 - d3));
    Fvector cp;
    cp.sub(p, c);
    const float d5 = ab.dotproduct(cp), d6 = ac.dotproduct(cp);
    if (d6 >= 0.f && d5 <= d6)
        return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
        return Fvector().mad(a, ac, d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
    {
        Fvector bc;
        bc.sub(c, b);
        return Fvector().mad(b, bc, (d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float den = 1.f / (va + vb + vc);
    return Fvector().mad(a, ab, vb * den).mad(ac, vc * den);
}

// the car pays: its speed (the shell's every element), and a dent - a strike hit on itself, which pm_car's crash rule
// (who = the car) cuts and keeps above a third of its health
float pay(CCar* car, float E, float cost, const Fvector& dir)
{
    CPhysicsShell* sh = car->PPhysicsShell();
    if (!sh || E <= 1.f)
        return 1.f;
    const float paid = _min(E * 0.95f, cost * C.absorb);
    const float f = _sqrt(_max(0.f, 1.f - paid / E));
    Fvector v;
    sh->get_LinearVel(v);
    v.mul(f);
    sh->set_LinearVel(v);
    if (C.dent_k > 0.f && paid > 100.f)
    {
        SHit HS;
        HS.GenHeader(GE_HIT, car->ID());
        HS.whoID = car->ID();
        HS.weaponID = car->ID();
        HS.dir = Fvector(dir).invert();
        HS.power = C.dent_k * paid;
        HS.p_in_bone_space.set(0.f, 0.5f, 1.5f);
        HS.impulse = 0.f;
        HS.hit_type = ALife::eHitTypeStrike;
        IKinematics* K = car->Visual() ? car->Visual()->dcast_PKinematics() : nullptr;
        HS.boneID = K ? K->LL_GetBoneRoot() : 0;
        NET_Packet P;
        HS.Write_Packet(P);
        car->u_EventSend(P);
    }
    return f;
}

// ------------------------------------------------------------------------------------------------- physics contacts
// (physics thread) a car's geom touching something: a physics object's contact closing fast enough is queued
void obj_contact(bool& do_colide, bool bo1, dContact& c, SGameMtl* /*m1*/, SGameMtl* /*m2*/)
{
    if (!do_colide)
        return;
    dGeomID gc = bo1 ? c.geom.g1 : c.geom.g2, go = bo1 ? c.geom.g2 : c.geom.g1;
    dxGeomUserData* uc = retrieveGeomUserData(gc);
    dxGeomUserData* uo = retrieveGeomUserData(go);
    if (!uc || !uo || !uc->ph_ref_object || !uo->ph_ref_object || uo->ph_ref_object == uc->ph_ref_object)
        return;
    dBodyID bc = dGeomGetBody(gc), bo = dGeomGetBody(go);
    if (!bc || !bo)
        return;
    const dReal* P = c.geom.pos;
    dVector3 vc, vo;
    dBodyGetPointVel(bc, P[0], P[1], P[2], vc);
    dBodyGetPointVel(bo, P[0], P[1], P[2], vo);
    // ODE: moving g1 along the normal parts them - from ours to the other is -n when ours is g1
    Fvector dir{(float)c.geom.normal[0], (float)c.geom.normal[1], (float)c.geom.normal[2]};
    if (bo1)
        dir.invert();
    const float vn = float(vc[0] - vo[0]) * dir.x + float(vc[1] - vo[1]) * dir.y + float(vc[2] - vo[2]) * dir.z;
    const u16 other = uo->ph_ref_object->ID();
    std::lock_guard<std::mutex> lk(S.mx);
    if (std::find(S.door_pass.begin(), S.door_pass.end(), other) != S.door_pass.end())
    {
        do_colide = false; // a leaf being torn off: the car does not stop on its hinge
        return;
    }
    if (vn < C.prop_min_speed || S.contacts.size() >= 64)
        return;
    const bool door = std::find(S.doors.begin(), S.doors.end(), other) != S.doors.end();
    if (door && vn >= C.door_speed)
    {
        // heavier leaves want more; the main thread decides by its mass - meanwhile its contacts pass
        S.door_pass.push_back(other);
        do_colide = false;
    }
    Contact ct;
    ct.car = uc->ph_ref_object->ID();
    ct.other = other;
    ct.pos.set((float)P[0], (float)P[1], (float)P[2]);
    Fvector v{float(vc[0]), float(vc[1]), float(vc[2])};
    ct.dir = v.square_magnitude() > 1e-4f ? v.normalize() : dir;
    ct.vn = vn;
    S.contacts.push_back(ct);
}

CPhysicsJoint* live_door_joint(CPhysicObject* o)
{
    CPhysicsShell* sh = o ? o->PPhysicsShell() : nullptr;
    IKinematics* K = o && o->Visual() ? o->Visual()->dcast_PKinematics() : nullptr;
    if (!sh || !K || K->LL_BoneID("door") == BI_NONE)
        return nullptr;
    CPhysicsJoint* j = sh->get_Joint("door");
    return j && j->bActive ? j : nullptr;
}

// the doors near a car (its contacts with them may pass at once)
void scan_doors(CCar* car, CarRec& r)
{
    if (Device.dwTimeGlobal < r.scan)
        return;
    r.scan = Device.dwTimeGlobal + 250;
    xr_vector<CObject*> near_o;
    Level().ObjectSpace.GetNearest(near_o, car->Position(), 25.f, car);
    xr_vector<u16> ids;
    for (CObject* ob : near_o)
        if (CPhysicObject* po = smart_cast<CPhysicObject*>(ob))
            if (live_door_joint(po))
                ids.push_back(po->ID());
    std::lock_guard<std::mutex> lk(S.mx);
    for (u16 id : ids)
        if (std::find(S.doors.begin(), S.doors.end(), id) == S.doors.end() && S.doors.size() < 256)
            S.doors.push_back(id);
}

void tear_door(CCar* car, CPhysicObject* o, const Contact& ct)
{
    CPhysicsJoint* j = live_door_joint(o);
    CPhysicsShell* sh = o->PPhysicsShell();
    const auto unpass = [&o]() {
        std::lock_guard<std::mutex> lk(S.mx);
        S.door_pass.erase(std::remove(S.door_pass.begin(), S.door_pass.end(), o->ID()), S.door_pass.end());
    };
    if (!j || !sh)
    {
        unpass();
        return;
    }
    CPhysicsElement* leaf = sh->get_Element("door");
    const float m = leaf ? leaf->getMass() : sh->getMass();
    const float need = m > C.door_heavy ? C.door_speed_heavy : C.door_speed;
    ++S.tried[K_DOOR];
    if (ct.vn < need || !leaf)
    {
        if (C.debug)
            Msg("[ram] door %s (%u, leaf %.0f kg): %.1f m/s of %.1f - holds", o->cName().c_str(), o->ID(), m, ct.vn, need);
        unpass();
        return;
    }
    Fvector v0;
    car->PPhysicsShell()->get_LinearVel(v0);
    j->Deactivate();
    sh->Enable();
    CPhysicsElement* frame = sh->get_ElementByStoreOrder(0);
    if (frame && frame != leaf)
        frame->Fix();
    Fvector lv;
    lv.mul(v0, C.door_throw);
    lv.y += C.door_lift;
    leaf->set_LinearVel(lv);
    Fvector w;
    w.random_dir().mul(::Random.randF(1.f, 3.f));
    leaf->set_AngularVel(w);
    // the car pays the leaf's momentum and a dent
    const float M = _max(car->PPhysicsShell()->getMass(), 1.f);
    const float E = 0.5f * M * v0.square_magnitude();
    const float cost = 0.5f * m * lv.square_magnitude() * 0.5f;
    const float s0 = v0.magnitude();
    const float f = pay(car, E, cost, ct.dir);
    unpass();
    {
        std::lock_guard<std::mutex> lk(S.mx);
        S.doors.erase(std::remove(S.doors.begin(), S.doors.end(), o->ID()), S.doors.end());
    }
    event(K_DOOR, o->ID(), ct.pos, cost, s0, s0 * f);
    Msg("* [ram] %s tears the door %s (%u, leaf %.0f kg) off at %.1f m/s, thrown at %.1f m/s", car->cName().c_str(), o->cName().c_str(), o->ID(), m, ct.vn,
        lv.magnitude());
}

void hit_prop(CCar* car, CPhysicObject* o, const Contact& ct)
{
    CPhysicsShell* sh = o->PPhysicsShell();
    if (!sh)
        return;
    const float M = _max(car->PPhysicsShell()->getMass(), 1.f), m = _max(sh->getMass(), 0.5f);
    const float mu = M * m / (M + m);
    const float E = 0.5f * mu * ct.vn * ct.vn;
    // its material and whether E57 breaks it: props_near's record of it
    xr_vector<xr_string> f;
    if (!find_rec(portal_mod_props::lua_near(o->Position(), 0.5f), o->ID(), f) || f.size() < 9 || f[3] != "yes")
        return;
    const xr_string& mat = f[7];
    const float j = mat == "wood" ? C.j_prop_wood : mat == "metal" ? C.j_prop_metal : mat == "glass" ? C.j_prop_glass : C.j_prop_other;
    const float dmg = E / _max(j, 0.01f);
    ++S.tried[K_PROP];
    SHit h;
    h.power = dmg / _max(C.k_strike, 0.01f);
    h.dir = ct.dir;
    h.who = car;
    h.whoID = car->ID();
    h.weaponID = car->ID();
    h.boneID = BI_NONE;
    Fmatrix inv;
    inv.invert(o->XFORM());
    inv.transform_tiny(h.p_in_bone_space, ct.pos);
    h.impulse = 0.f;
    h.hit_type = ALife::eHitTypeStrike;
    const float hp = (float)atof(f[4].c_str()), had = (float)atof(f[5].c_str());
    portal_mod_props::hit(o, h);
    if (C.debug)
        Msg("[ram] prop %s (%u, %s, %.0f kg): %.1f m/s, %.0f J -> %.0f of %.0f (had %.0f)", o->cNameVisual().c_str(), o->ID(), mat.c_str(), m, ct.vn, E, dmg, hp, had);
    if (had + dmg >= hp)
        event(K_PROP, o->ID(), ct.pos, E, ct.vn, ct.vn);
}

void do_contacts()
{
    xr_vector<Contact> q;
    {
        std::lock_guard<std::mutex> lk(S.mx);
        q.swap(S.contacts);
    }
    for (const Contact& ct : q)
    {
        CCar* car = smart_cast<CCar*>(Level().Objects.net_Find(ct.car));
        CPhysicObject* o = smart_cast<CPhysicObject*>(Level().Objects.net_Find(ct.other));
        if (!car || !o || !car->PPhysicsShell() || o->getDestroy())
        {
            std::lock_guard<std::mutex> lk(S.mx);
            S.door_pass.erase(std::remove(S.door_pass.begin(), S.door_pass.end(), ct.other), S.door_pass.end());
            continue;
        }
        auto it = S.cars.find(ct.car);
        const bool door = live_door_joint(o) != nullptr;
        if (it == S.cars.end() || recent(it->second, door ? K_DOOR : K_PROP, o->ID()))
        {
            std::lock_guard<std::mutex> lk(S.mx); // (a leaf let pass must not stay passable)
            S.door_pass.erase(std::remove(S.door_pass.begin(), S.door_pass.end(), o->ID()), S.door_pass.end());
            continue;
        }
        remember(it->second, door ? K_DOOR : K_PROP, o->ID());
        if (door)
            tear_door(car, o, ct);
        else if (destruct_on())
            hit_prop(car, o, ct);
    }
}

// ------------------------------------------------------------------------------------------------- the static sweep
struct Cand
{
    u32 tri;
    Fvector p, n; // the point nearest the car's front, the face's normal (towards the car)
    float d; // its distance ahead
};

float face_speed(const Fvector& v, float speed, const Fvector& n, bool round)
{
    const float vn = _abs(v.dotproduct(n));
    return round ? _max(vn, 0.8f * speed) : _max(vn, C.glance * speed);
}

void sweep(CCar* car, CarRec& r)
{
    CPhysicsShell* sh = car->PPhysicsShell();
    CDB::MODEL* M = Level().ObjectSpace.GetStaticModel();
    if (!sh || !M || !car->Visual())
        return;
    Fvector v;
    sh->get_LinearVel(v);
    const float speed = v.magnitude();
    if (!_valid(speed) || speed < C.min_speed)
        return;
    const Fmatrix& X = car->XFORM();
    Fmatrix inv;
    inv.invert(X);
    Fvector dl;
    inv.transform_dir(dl, v);
    dl.div(speed);
    Fbox b = car->Visual()->getVisData().box;
    const float hgt = b.max.y - b.min.y;
    b.min.y += _min(C.floor_clear, hgt * 0.4f);
    const float lead = speed * C.lead_time + C.lead_add;
    for (int a = 0; a < 3; ++a)
    {
        if (a == 1)
            continue;
        if (dl[a] > 0.f)
            b.max[a] += dl[a] * lead;
        else
            b.min[a] += dl[a] * lead;
    }
    if (dl.y > 0.f)
        b.max.y += dl.y * lead;
    // its world box
    Fbox wb;
    wb.invalidate();
    for (int k = 0; k < 8; ++k)
    {
        Fvector p{(k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z}, w;
        X.transform_tiny(w, p);
        wb.modify(w);
    }
    Fvector wc, wd;
    wb.get_CD(wc, wd);
    CDB::COLLIDER xrc;
    xrc.box_query(0, M, wc, wd);
    if (!xrc.r_count())
        return;
    const CDB::TRI* T = M->get_tris();
    const Fvector* V = M->get_verts();
    const Fbox& vb = car->Visual()->getVisData().box;
    Fvector front_l{(vb.min.x + vb.max.x) * 0.5f, (vb.min.y + vb.max.y) * 0.5f, (vb.min.z + vb.max.z) * 0.5f};
    front_l.mad(dl, 0.5f * _max(vb.max.z - vb.min.z, vb.max.x - vb.min.x));
    Fvector front;
    X.transform_tiny(front, front_l);
    Fvector dw = Fvector(v).div(speed);
    xr_vector<Cand> cand;
    int n = 0;
    for (const CDB::RESULT* rp = xrc.r_begin(); rp != xrc.r_end() && n < C.max_tris; ++rp)
    {
        const CDB::TRI& t = T[rp->id];
        Fvector w[3] = {V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]}, l[3];
        if (w[0].y < -5000.f) // E54's sink: a triangle that left the level
            continue;
        for (int k = 0; k < 3; ++k)
            inv.transform_tiny(l[k], w[k]);
        if (!tri_box(l, b))
            continue;
        ++n;
        Cand c;
        c.tri = u32(rp->id);
        c.p = closest_on_tri(front, w[0], w[1], w[2]);
        c.n.mknormal(w[0], w[1], w[2]);
        if (c.n.dotproduct(dw) > 0.f)
            c.n.invert();
        c.d = Fvector().sub(c.p, front).dotproduct(dw);
        cand.push_back(c);
    }
    if (cand.empty())
        return;
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    const float mass = _max(sh->getMass(), 50.f);
    float E = 0.5f * mass * speed * speed; // what the car has left for the things ahead this frame
    const bool dbg = C.debug;
    const u16 m_trunk = GMLib.GetMaterialIdx("materials\\tree_trunk");
    // the poles, fences, lamps and the trees round the swept box: one query each a frame
    struct Near
    {
        int id;
        xr_string kind;
        Fvector at;
        float hp, dmg, r;
    };
    xr_vector<Near> stat, trees;
    xr_vector<xr_vector<xr_string>> recs;
    const float reach = wd.magnitude();
    all_recs(portal_mod_statics::lua_near(wc, reach + 3.4f, ""), recs);
    for (const auto& rc : recs)
        if (rc.size() >= 10 && atoi(rc[8].c_str()) == 0 && rc[1] != "wire")
            stat.push_back({atoi(rc[0].c_str()), rc[1], Fvector{(float)atof(rc[2].c_str()), (float)atof(rc[3].c_str()), (float)atof(rc[4].c_str())},
                (float)atof(rc[6].c_str()), (float)atof(rc[7].c_str()), 0.f});
    all_recs(portal_mod_trees::lua_near(wc, _sqrt(wd.x * wd.x + wd.z * wd.z) + 1.6f), recs);
    for (const auto& rc : recs)
        if (rc.size() >= 9 && atoi(rc[8].c_str()) == 0)
            trees.push_back({atoi(rc[0].c_str()), "tree", Fvector{(float)atof(rc[1].c_str()), (float)atof(rc[2].c_str()), (float)atof(rc[3].c_str())},
                (float)atof(rc[6].c_str()), (float)atof(rc[7].c_str()), (float)atof(rc[5].c_str())});
    static LPCSTR const KN[8] = {"wood_pole", "concrete_pole", "metal_pole", "wood_fence", "concrete_fence", "mesh_fence", "lamp", "wire"};
    for (const Cand& c : cand)
    {
        if (E < 200.f)
            break;
        if (_abs(c.n.y) > 0.75f) // a floor, a slab's top, the road: driven on, never rammed
            continue;
        const CDB::TRI& t = T[c.tri];
        const float sp_now = _sqrt(2.f * E / mass);
        const Fvector vnow = Fvector(v).mul(sp_now / speed);
        // 1. a destructible building's piece (E54: the construction site, the ramming walls)
        const int pc = portal_mod_destruct::lua_piece_at(c.p);
        if (pc >= 0 && portal_mod_destruct::lua_piece_state(pc) == 0 && portal_mod_destruct::lua_piece_pos(pc).distance_to(c.p) < 2.2f)
        {
            if (recent(r, K_PIECE, (u32)pc))
                continue;
            remember(r, K_PIECE, (u32)pc);
            ++S.tried[K_PIECE];
            const float vf = face_speed(vnow, sp_now, c.n, false);
            const float Ef = _min(E, 0.5f * mass * vf * vf);
            SBullet bl;
            bl.dir = dw;
            bl.speed = bl.max_speed = 1.f;
            bl.pm_destruct = Ef / _max(C.j_piece, 1.f);
            portal_mod_destruct::bullet_hit(bl, c.tri, c.p, c.n, u16(t.material));
            const bool gone = portal_mod_destruct::lua_piece_state(pc) != 0;
            if (dbg)
                Msg("[ram] piece %d: %.1f kJ into it (%.0f of damage) -> %s", pc, Ef * 0.001f, bl.pm_destruct, gone ? "broken" : "holds");
            if (gone)
            {
                const float cost = Ef * C.piece_absorb;
                const float s0 = sp_now;
                const float k = pay(car, E, cost, dw);
                E *= k * k;
                event(K_PIECE, (u32)pc, c.p, cost, s0, s0 * k);
            }
            continue;
        }
        // 2. a tree's trunk (E58): the nearest standing one
        if (t.material == m_trunk)
        {
            const Near* best = nullptr;
            float bd = 1.6f;
            for (const Near& q : trees)
                if (q.at.distance_to_xz(c.p) < bd)
                    bd = q.at.distance_to_xz(c.p), best = &q;
            if (!best || recent(r, K_TREE, (u32)best->id))
                continue;
            remember(r, K_TREE, (u32)best->id);
            ++S.tried[K_TREE];
            const float vf = face_speed(vnow, sp_now, c.n, true);
            const float Ef = _min(E, 0.5f * mass * vf * vf);
            const float need = _max(0.f, best->hp - best->dmg) * C.j_tree;
            if (dbg)
                Msg("[ram] tree %d (r %.2f m, %.0f of %.0f hp): %.1f kJ of %.1f needed", best->id, best->r, best->dmg, best->hp, Ef * 0.001f, need * 0.001f);
            if (Ef >= need)
            {
                portal_mod_trees::lua_fell(best->id, dw);
                const float s0 = sp_now;
                const float k = pay(car, E, need, dw);
                E *= k * k;
                event(K_TREE, (u32)best->id, c.p, need, s0, s0 * k);
                Msg("* [ram] %s fells tree %d (r %.2f m) at %.1f m/s: %.1f kJ", car->cName().c_str(), best->id, best->r, s0, need * 0.001f);
            }
            else
                portal_mod_trees::bullet_hit(c.p, dw, c.tri, Ef / _max(C.j_tree, 1.f)); // it keeps the damage
            continue;
        }
        // 3. a pole, a fence piece, a lamp (E59): the nearest standing one whose kind fits the face's material
        const SGameMtl* gm = GMLib.GetMaterialByIdx(u16(t.material));
        LPCSTR mname = gm ? gm->m_Name.c_str() : "";
        const bool m_wood = strstr(mname, "wood") || strstr(mname, "tree");
        const bool m_conc = strstr(mname, "concrete") || strstr(mname, "stone") || strstr(mname, "bricks");
        const bool m_metal = strstr(mname, "metal") || strstr(mname, "setka") || strstr(mname, "mtl") || strstr(mname, "iron");
        const bool m_glass = !!strstr(mname, "glass");
        const Near* best = nullptr;
        float bd = flt_max;
        for (const Near& q : stat)
        {
            LPCSTR kn = q.kind.c_str();
            const bool pole = strstr(kn, "pole") != nullptr;
            bool fits = false;
            if (strstr(kn, "wood"))
                fits = m_wood;
            else if (strstr(kn, "concrete"))
                fits = m_conc;
            else if (strstr(kn, "metal") || strstr(kn, "mesh"))
                fits = m_metal;
            else if (q.kind == "lamp")
                fits = m_glass || m_metal;
            if (!fits)
                continue;
            const float d = pole ? q.at.distance_to_xz(c.p) : q.at.distance_to(c.p);
            if (d < (pole ? 1.2f : 3.4f) && d < bd)
                bd = d, best = &q;
        }
        if (!best || recent(r, K_STATICS, (u32)best->id))
            continue;
        remember(r, K_STATICS, (u32)best->id);
        ++S.tried[K_STATICS];
        const bool pole = strstr(best->kind.c_str(), "pole") != nullptr;
        float j = C.j_statics_other;
        for (int k = 0; k < 8; ++k)
            if (best->kind == KN[k])
                j = C.j_statics[k];
        const float vf = face_speed(vnow, sp_now, c.n, pole);
        const float Ef = _min(E, 0.5f * mass * vf * vf);
        const float need = _max(0.f, best->hp - best->dmg) * j;
        if (dbg)
            Msg("[ram] %s %d (%s, %.0f of %.0f hp, %.1f m off): %.1f kJ of %.1f needed", best->kind.c_str(), best->id, mname, best->dmg, best->hp, bd, Ef * 0.001f,
                need * 0.001f);
        if (Ef >= need && portal_mod_statics::lua_break(best->id, dw))
        {
            const float s0 = sp_now;
            const float k = pay(car, E, need, dw);
            E *= k * k;
            event(K_STATICS, (u32)best->id, c.p, need, s0, s0 * k);
            Msg("* [ram] %s breaks %s %d at %.1f m/s: %.1f kJ, on at %.1f m/s", car->cName().c_str(), best->kind.c_str(), best->id, s0, need * 0.001f, s0 * k);
        }
    }
}
} // namespace

// ------------------------------------------------------------------------------------------------------------ public
void car_update(CCar* car)
{
    load_cfg();
    if (!car || !g_pGameLevel || car->getDestroy())
        return;
    level_check();
    CarRec& r = S.cars[car->ID()];
    if (r.frame == Device.dwFrame)
        return;
    r.frame = Device.dwFrame;
    CPhysicsShell* sh = car->PPhysicsShell();
    if (!sh || !sh->isActive())
        return;
    if (r.shell != sh)
    {
        r.shell = sh;
        sh->add_ObjectContactCallback(&obj_contact);
        if (C.debug)
            Msg("[ram] %s (%u): watching its contacts (%.0f kg)", car->cName().c_str(), car->ID(), sh->getMass());
    }
    if (!is_on())
    {
        std::lock_guard<std::mutex> lk(S.mx);
        S.contacts.clear();
        S.door_pass.clear();
        return;
    }
    scan_doors(car, r);
    do_contacts();
    if (destruct_on())
        sweep(car, r);
}

bool feature_set(LPCSTR name, bool on)
{
    if (!name || xr_strcmp(name, "ram"))
        return false;
    load_cfg();
    if (S.level != (void*)g_pGameLevel)
        level_check();
    if (S.on != on)
        Msg("* E72 ram %s", on ? "on" : "off");
    S.on = on;
    return true;
}

bool enabled()
{
    load_cfg();
    return is_on();
}

LPCSTR lua_info()
{
    load_cfg();
    static string1024 buf;
    xr_sprintf(buf, "on %d (cfg %d), cars %u; broken: statics %u/%u, trees %u/%u, pieces %u/%u, props %u/%u, doors %u/%u; last %s (%.1f kJ, %.1f -> %.1f m/s)",
        int(S.on), int(C.enabled), (u32)S.cars.size(), S.broken[K_STATICS], S.tried[K_STATICS], S.broken[K_TREE], S.tried[K_TREE], S.broken[K_PIECE], S.tried[K_PIECE],
        S.broken[K_PROP], S.tried[K_PROP], S.broken[K_DOOR], S.tried[K_DOOR], S.last_what.empty() ? "-" : S.last_what.c_str(), S.last_E * 0.001f, S.last_v0, S.last_v1);
    return buf;
}

LPCSTR lua_events()
{
    static xr_string out;
    out.swap(S.events);
    S.events.clear();
    return out.c_str();
}

void lua_debug(bool on)
{
    load_cfg();
    C.debug = on;
}
} // namespace portal_mod_ram
