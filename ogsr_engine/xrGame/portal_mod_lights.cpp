// portal-mod E75: lights break - see portal_mod_lights.h.
//
// Threads: the bullets fly on the second thread (CBulletManager::UpdateWorkload, mtBullets). snapshot() lists the bulbs
// and the lit head torches on the main thread at CommitRenderSet, before that thread starts; torch_clip / bullet_step /
// torch_reached only read that list and append to `pend`, which commit() takes at the next frame's start
// (CommitEvents), once the second thread is done - the bullet manager's own events work the same way.
#include "stdafx.h"
#include "portal_mod_lights.h"
#include "portal_mod_statics.h" // E59: the level's lamps (level.pms) the bare lights sit in
#include "Level.h"
#include "Level_Bullet_Manager.h"
#include "HangingLamp.h"
#include "Torch.h"
#include "Actor.h"
#include "Inventory.h"
#include "entity_alive.h"
#include "Hit.h"
#include "ParticlesObject.h"
#include "xrServer_Objects_ALife.h"
#include "../Include/xrRender/Kinematics.h"
#include <algorithm>

using namespace luabind;

// CHangingLamp keeps its light, its bone and its health private: it names this struct its friend (portal-mod E75)
struct portal_mod_lamp_access
{
    static float& health(CHangingLamp* l) { return l->fHealth; }
    static u16 bone(const CHangingLamp* l) { return l->light_bone; }
    static IRender_Light* light(const CHangingLamp* l) { return l->light_render._get(); }
    static bool& lit(CHangingLamp* l) { return l->lights_turned_on; }
};
using LA = portal_mod_lamp_access;

namespace
{
// CTorch's guide bone (the lens) is protected: read through a derived class (nothing of this type is ever made)
struct pm_torch_access : public CTorch
{
    static u16 guide(const CTorch* t) { return t->*(&pm_torch_access::guid_bone); }
};

struct Cfg
{
    float lamp_r = 0.16f, light_r = 0.22f, static_r = 0.40f, torch_r = 0.075f, torch_hit_r = 0.12f;
    float torch_hit_power = 0.04f, torch_hit_impulse = 60.f;
    float e59_link = 1.2f, hemi_r = 1.5f, test_dist = 250.f;
    float p_head = 0.55f, p_neck = 0.35f, p_spine2 = 0.08f;
    float repair_quiet = 8.f, repair_time = 3.f;
    float flash_range = 7.f, flash_ms = 70.f, torch_flash_range = 2.5f;
    Fcolor flash_color{1.7f, 1.6f, 2.0f, 1.f};
    float die_ms = 380.f;
    float vol_glass = 1.f, vol_break = 1.f, vol_pop = 1.f, vol_sparks = 0.8f, vol_flicker = 0.35f, vol_click = 0.7f, vol_repair = 0.8f;
    float vol_torch = 0.8f;
    xr_vector<shared_str> fx_glass, fx_spark, fx_torch;
    xr_vector<shared_str> snd_glass, snd_break, snd_pop, snd_sparks, snd_flicker, snd_click, snd_repair_start, snd_repair_end;
    bool debug = false;
};
Cfg C;

struct Lamp
{
    u16 id{};
    bool model{}; // its own visual (a bulb on a light bone)
    bool broken{};
    bool hemi_off{}; // its hemi lights are off
    bool e59_tried{};
    int e59 = -1; // the E59 lamp (level.pms) it sits in
    Fvector e59_c{};
    u32 die_at{}; // flickering out until then (its E59 lamp went)
    Fvector pos{}; // its bulb, last seen
    float range{};
    Fvector dir{0.f, -1.f, 0.f};
};

struct Target
{
    Fvector p;
    float r;
    u16 id; // a lamp: the lamp's id; a torch: the torch's
    u16 owner; // a torch: the NPC wearing it
};

enum : u8
{
    P_LAMP,
    P_TORCH
};
struct Pend
{
    u8 type;
    u16 id, owner, shooter, weapon;
    Fvector at, dir;
    bool stat; // a lamp: the round hit the level at its fixture (else it passed the bulb)
};

struct Flash
{
    ref_light L;
    u32 t0, dur;
    Fcolor c;
};

struct State
{
    bool on = true;
    const void* level = nullptr;
    shared_str level_name; // a level loaded at the same address: its name tells (E75b)
    xr_vector<Lamp> lamps;
    xr_map<u16, u32> index;
    xr_vector<u16> torches; // CTorch ids worn by NPCs
    u32 next_scan = 0, next_poll = 0, level_t0 = 0;
    xr_vector<Target> t_lamps, t_torches; // the worker's view (snapshot)
    xr_vector<Pend> pend; // the worker's finds
    xr_vector<Flash> flashes;
    bool cfg_loaded = false, sounds = false;
    xr_vector<ref_sound> s_glass, s_break, s_pop, s_sparks, s_flicker, s_click, s_repair_start, s_repair_end;
    u32 broken_n = 0, torches_n = 0, hemi_n = 0;
    // the actor's torch
    u32 last_hit = 0; // when an enemy round last hit the player (ms)
    bool repairing = false;
    u32 repair_t0 = 0, repair_end = 0, repair_hit = 0;
    int repair_step = 0;
    bool flick_on = true, flick_forced = false;
    int burst = 0;
    u32 flick_next = 0;
};
State S;

u32 now() { return Device.dwTimeGlobal; }

void names(CInifile& ini, LPCSTR key, xr_vector<shared_str>& out)
{
    if (!ini.line_exist("pm_lights", key))
        return;
    out.clear();
    LPCSTR v = ini.r_string("pm_lights", key);
    string256 one;
    for (int i = 0, n = _GetItemCount(v); i < n; ++i)
        out.emplace_back(_Trim(_GetItem(v, i, one)));
}

void load_cfg()
{
    if (S.cfg_loaded)
        return;
    S.cfg_loaded = true;
    // defaults, overridden by config\portal_mod\pm_lights.ltx
    auto set = [](xr_vector<shared_str>& v, std::initializer_list<LPCSTR> l) {
        v.clear();
        for (LPCSTR s : l)
            v.emplace_back(s);
    };
    set(C.fx_glass, {"hit_fx\\hit_glass_01", "explosions\\explosion_bottle"});
    set(C.fx_spark, {"anomaly2\\electra_entrance_small"});
    set(C.fx_torch, {"anomaly2\\electra_entrance_small"});
    set(C.snd_glass, {"material\\bullet\\collide\\glass01hl", "material\\bullet\\collide\\glass02hl", "material\\bullet\\collide\\glass03hl"});
    set(C.snd_break, {"material\\bottle\\collide\\bottle_break_1", "material\\glass\\glass_fall01hl", "material\\glass\\glass_fall02hl"});
    set(C.snd_pop, {"anomaly\\electra_hit1", "anomaly\\electra_hit"});
    set(C.snd_sparks, {"ambient\\sparks1"});
    set(C.snd_flicker, {"anomaly\\electra_hit"});
    set(C.snd_click, {"ambient\\switch_1", "ambient\\switch_2"});
    set(C.snd_repair_start, {"interface\\inv_detach_addon"});
    set(C.snd_repair_end, {"interface\\inv_attach_addon"});

    string_path fn;
    FS.update_path(fn, "$game_config$", "portal_mod\\pm_lights.ltx");
    if (!FS.exist(fn))
    {
        Msg("~ [lights] no %s: defaults", fn);
        return;
    }
    CInifile ini(fn);
    if (!ini.section_exist("pm_lights"))
        return;
    auto f = [&](LPCSTR key, float& v) {
        if (ini.line_exist("pm_lights", key))
            v = ini.r_float("pm_lights", key);
    };
    f("lamp_radius", C.lamp_r);
    f("light_radius", C.light_r);
    f("static_radius", C.static_r);
    f("torch_radius", C.torch_r);
    f("torch_hit_radius", C.torch_hit_r);
    f("torch_hit_power", C.torch_hit_power);
    f("torch_hit_impulse", C.torch_hit_impulse);
    f("e59_link", C.e59_link);
    f("hemi_radius", C.hemi_r);
    f("test_dist", C.test_dist);
    f("actor_p_head", C.p_head);
    f("actor_p_neck", C.p_neck);
    f("actor_p_spine2", C.p_spine2);
    f("repair_quiet", C.repair_quiet);
    f("repair_time", C.repair_time);
    f("flash_range", C.flash_range);
    f("flash_ms", C.flash_ms);
    f("torch_flash_range", C.torch_flash_range);
    f("die_ms", C.die_ms);
    f("vol_glass", C.vol_glass);
    f("vol_break", C.vol_break);
    f("vol_pop", C.vol_pop);
    f("vol_sparks", C.vol_sparks);
    f("vol_flicker", C.vol_flicker);
    f("vol_click", C.vol_click);
    f("vol_repair", C.vol_repair);
    f("vol_torch", C.vol_torch);
    if (ini.line_exist("pm_lights", "flash_color"))
    {
        const Fvector c = ini.r_fvector3("pm_lights", "flash_color");
        C.flash_color.set(c.x, c.y, c.z, 1.f);
    }
    names(ini, "fx_glass", C.fx_glass);
    names(ini, "fx_spark", C.fx_spark);
    names(ini, "fx_torch", C.fx_torch);
    names(ini, "snd_glass", C.snd_glass);
    names(ini, "snd_break", C.snd_break);
    names(ini, "snd_pop", C.snd_pop);
    names(ini, "snd_sparks", C.snd_sparks);
    names(ini, "snd_flicker", C.snd_flicker);
    names(ini, "snd_click", C.snd_click);
    names(ini, "snd_repair_start", C.snd_repair_start);
    names(ini, "snd_repair_end", C.snd_repair_end);
    if (ini.line_exist("pm_lights", "debug"))
        C.debug = !!ini.r_bool("pm_lights", "debug");
}

void make_sounds()
{
    if (S.sounds)
        return;
    S.sounds = true;
    auto mk = [](const xr_vector<shared_str>& n, xr_vector<ref_sound>& out) {
        out.resize(n.size());
        for (u32 i = 0; i < n.size(); ++i)
            out[i].create(n[i].c_str(), st_Effect, sg_SourceType);
    };
    mk(C.snd_glass, S.s_glass);
    mk(C.snd_break, S.s_break);
    mk(C.snd_pop, S.s_pop);
    mk(C.snd_sparks, S.s_sparks);
    mk(C.snd_flicker, S.s_flicker);
    mk(C.snd_click, S.s_click);
    mk(C.snd_repair_start, S.s_repair_start);
    mk(C.snd_repair_end, S.s_repair_end);
}

void play(xr_vector<ref_sound>& snd, const Fvector& pos, float vol)
{
    make_sounds();
    if (snd.empty() || vol <= 0.f)
        return;
    ref_sound& s = snd[::Random.randI(snd.size())];
    if (!s._handle())
        return;
    Fvector p = pos;
    float v = vol;
    s.play_no_feedback(nullptr, 0, 0.f, &p, &v);
}

void fx(const xr_vector<shared_str>& n, const Fvector& pos, const Fvector& dir)
{
    if (n.empty() || pos.distance_to_sqr(Device.vCameraPosition) > 200.f * 200.f)
        return;
    Fmatrix m;
    m.identity();
    Fvector d = dir;
    if (d.square_magnitude() < EPS)
        d.set(0.f, 1.f, 0.f);
    d.normalize();
    m.k.set(d);
    Fvector::generate_orthonormal_basis(m.k, m.j, m.i);
    m.c.set(pos);
    CParticlesObject* ps = CParticlesObject::Create(n[::Random.randI(n.size())].c_str(), TRUE);
    ps->UpdateParent(m, Fvector().set(0.f, 0.f, 0.f));
    ps->Play(false);
}

void flash(const Fvector& pos, float range, float ms, float k = 1.f)
{
    Flash f;
    f.L = ::Render->light_create();
    f.L->set_type(IRender_Light::POINT);
    f.L->set_shadow(false);
    f.L->set_range(range);
    f.c = C.flash_color;
    f.c.mul_rgb(k);
    f.L->set_color(f.c);
    f.L->set_position(pos);
    f.L->set_active(true);
    f.t0 = now();
    f.dur = (u32)_max(ms, 10.f);
    S.flashes.push_back(f);
}

void update_flashes()
{
    for (u32 i = 0; i < S.flashes.size();)
    {
        Flash& f = S.flashes[i];
        const float t = float(now() - f.t0) / float(f.dur);
        if (t >= 1.f)
        {
            f.L->set_active(false);
            S.flashes[i] = S.flashes.back();
            S.flashes.pop_back();
            continue;
        }
        Fcolor c = f.c;
        c.mul_rgb(1.f - t * t);
        f.L->set_color(c);
        ++i;
    }
}

CHangingLamp* lamp_obj(u16 id) { return smart_cast<CHangingLamp*>(Level().Objects.net_Find(id)); }

Fvector bulb(CHangingLamp* l)
{
    const u16 b = LA::bone(l);
    IKinematics* K = l->Visual() ? smart_cast<IKinematics*>(l->Visual()) : nullptr;
    if (K && b != BI_NONE)
    {
        Fmatrix m;
        m.mul_43(l->XFORM(), K->LL_GetTransform(b));
        return m.c;
    }
    return l->XFORM().c;
}

Fvector lens(const CTorch* t)
{
    IKinematics* K = t->Visual() ? smart_cast<IKinematics*>(const_cast<CTorch*>(t)->Visual()) : nullptr;
    const u16 b = pm_torch_access::guide(t);
    if (K && b != BI_NONE && b < K->LL_BoneCount())
    {
        Fmatrix m;
        m.mul_43(t->XFORM(), K->LL_GetTransform(b));
        return m.c;
    }
    return t->XFORM().c;
}

bool torch_dead(const CTorch* t) { return t->GetCondition() < 0.005f; }
bool torch_flicker(const CTorch* t)
{
    const float c = t->GetCondition();
    return c > 0.29f && c < 0.31f;
}

CTorch* actor_torch()
{
    CActor* a = Actor();
    return a ? smart_cast<CTorch*>(a->inventory().ItemFromSlot(TORCH_SLOT)) : nullptr;
}

Lamp* find(u16 id)
{
    auto it = S.index.find(id);
    return it == S.index.end() ? nullptr : &S.lamps[it->second];
}

void hemi(Lamp& e, bool off)
{
    if (e.hemi_off == off)
        return;
    e.hemi_off = off;
    const u32 n = portal_mod_hemi_set(e.pos, C.hemi_r, !off);
    if (off)
        S.hemi_n += n;
    if (C.debug && n)
        Msg("[lights] lamp %u: %u hemi light(s) %s", e.id, n, off ? "off" : "on");
}

// the E59 lamp round a bare light: its id and centre (state 0 standing)
int e59_state(const Lamp& e)
{
    if (e.e59 < 0)
        return -1;
    LPCSTR s = portal_mod_statics::lua_near(e.e59_c, 0.05f, "lamp");
    if (!s || !*s)
        return -1;
    u32 id = 0;
    char kind[64];
    float x, y, z, h, hp, dmg, d;
    int st = 0;
    if (sscanf(s, "%u|%63[^|]|%f|%f|%f|%f|%f|%f|%d|%f;", &id, kind, &x, &y, &z, &h, &hp, &dmg, &st, &d) == 10 && (int)id == e.e59)
        return st;
    return -1;
}

void link_e59(Lamp& e)
{
    e.e59_tried = true;
    if (e.model)
        return;
    LPCSTR s = portal_mod_statics::lua_near(e.pos, C.e59_link, "lamp");
    if (!s || !*s)
        return;
    u32 id = 0;
    char kind[64];
    float x, y, z, h, hp, dmg, d;
    int st = 0;
    if (sscanf(s, "%u|%63[^|]|%f|%f|%f|%f|%f|%f|%d|%f;", &id, kind, &x, &y, &z, &h, &hp, &dmg, &st, &d) == 10)
    {
        e.e59 = (int)id;
        e.e59_c.set(x, y, z);
        if (C.debug)
            Msg("[lights] light %u linked to E59 %s %u (%.2f m, state %d)", e.id, kind, id, d, st);
    }
}

// the lamp goes out for good: `l` dark, its alife health 0 (kept in saves), its hemi lights off; how it went: the fx
void finish(Lamp& e, CHangingLamp* l, const Fvector& dir, bool glass, LPCSTR why)
{
    if (e.broken)
        return;
    const bool dying = e.die_at != 0;
    e.broken = true;
    e.die_at = 0;
    ++S.broken_n;
    if (l)
    {
        e.pos = bulb(l);
        LA::health(l) = 0.f;
        if (dying) // flickered with lights_turned_on: as it was before
            LA::lit(l) = true;
        if (LA::lit(l)) // not twice (the game's own hit has turned it off already: TurnOff counts its processing down)
            l->TurnOff();
        else if (IKinematics* K = l->Visual() ? smart_cast<IKinematics*>(l->Visual()) : nullptr) // switched off by a script: its bulb goes
            if (LA::bone(l) != BI_NONE)
                K->LL_SetBoneVisible(LA::bone(l), FALSE, TRUE);
        if (auto se = smart_cast<CSE_ALifeObjectHangingLamp*>(l->alife_object()))
            se->m_health = 0.f;
    }
    hemi(e, true);
    Fvector up{0.f, 1.f, 0.f}, back;
    back.set(dir).invert();
    flash(e.pos, C.flash_range, C.flash_ms);
    if (glass)
    {
        fx(C.fx_glass, e.pos, back.square_magnitude() > EPS ? back : up);
        play(S.s_glass, e.pos, C.vol_glass);
        play(S.s_break, e.pos, C.vol_break);
    }
    fx(C.fx_spark, e.pos, Fvector().set(0.f, -1.f, 0.f));
    play(S.s_pop, e.pos, C.vol_pop);
    play(S.s_sparks, e.pos, C.vol_sparks);
    Msg("[lights] %s %u broken (%s) at (%.2f, %.2f, %.2f)", e.model ? "lamp" : "light", e.id, why, e.pos.x, e.pos.y, e.pos.z);
}

void break_lamp(Lamp& e, const Fvector& dir, LPCSTR why)
{
    if (e.broken)
        return;
    CHangingLamp* l = lamp_obj(e.id);
    finish(e, l, dir, true, why);
    // a bare light shot inside one of E59's lamps: that lamp drops in its burst of glass too
    if (!e.model && e.e59 >= 0 && e59_state(e) == 0)
        portal_mod_statics::lua_break(e.e59, dir);
}

void rescan()
{
    S.next_scan = now() + 1000;
    S.torches.clear();
    for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
    {
        CObject* o = Level().Objects.o_get_by_iterator(i);
        if (!o)
            continue;
        if (CHangingLamp* l = smart_cast<CHangingLamp*>(o))
        {
            Lamp* e = find(l->ID());
            if (!e)
            {
                Lamp ne;
                ne.id = l->ID();
                ne.model = l->Visual() != nullptr;
                S.index[ne.id] = S.lamps.size();
                S.lamps.push_back(ne);
                e = &S.lamps.back();
            }
            if (l->getDestroy())
                continue;
            e->pos = bulb(l);
            if (IRender_Light* L = LA::light(l))
                e->range = L->get_range();
            const bool dead = LA::health(l) <= 0.f;
            if (dead && !e->broken) // loaded broken (alife health 0), or the game's own hit killed it
            {
                e->broken = true;
                e->die_at = 0;
            }
            else if (!dead && e->broken) // a save loaded from before it broke
                e->broken = false;
            if (!e->e59_tried)
                link_e59(*e);
            hemi(*e, e->broken);
            if (e->broken) // (E75b) a save loaded on the same level brings the renderer's hemi lights back on
                S.hemi_n += portal_mod_hemi_set(e->pos, C.hemi_r, false);
            continue;
        }
        if (CTorch* t = smart_cast<CTorch*>(o))
        {
            CObject* p = t->H_Parent();
            if (p && p != Actor() && smart_cast<CEntityAlive*>(p))
                S.torches.push_back(t->ID());
        }
    }
}

// My Mods' "lights" row (E55's way): $app_data_root$\mod_manager.ltx, "lights = 0" off, read from the disk when a level is
// made (the window writes it with Lua's io); the script sets it again at the level's start and on Apply
void load_switch()
{
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
        if (xr_strcmp(_Trim(line), "lights"))
            continue;
        LPSTR v = _Trim(eq + 1);
        S.on = !xr_strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true");
    }
    fclose(f);
    Msg("* E75 breakable lights: %s (My Mods)", S.on ? "on" : "off");
}

void reset_level()
{
    S.level = g_pGameLevel;
    S.level_name = Level().name();
    load_switch();
    S.lamps.clear();
    S.index.clear();
    S.torches.clear();
    S.t_lamps.clear();
    S.t_torches.clear();
    S.pend.clear();
    for (Flash& f : S.flashes)
        f.L->set_active(false);
    S.flashes.clear();
    S.next_scan = S.next_poll = 0;
    S.level_t0 = now();
    S.broken_n = S.torches_n = S.hemi_n = 0;
    S.repairing = false;
    S.flick_forced = false;
}

bool ready()
{
    if (!g_pGameLevel)
        return false;
    load_cfg();
    if (S.level != g_pGameLevel || S.level_name != Level().name())
        reset_level();
    return true;
}

// NPC's torch: dark for good (condition 0, kept in saves), sparks; the round stops in it - the NPC flinches
void break_torch(CTorch* t, u16 shooter, u16 weapon, const Fvector& at, const Fvector& dir, bool stopped_round)
{
    if (!t || torch_dead(t))
        return;
    const Fvector p = lens(t);
    t->Switch(false);
    t->SetCondition(0.f);
    ++S.torches_n;
    Fvector back;
    back.set(dir).invert();
    fx(C.fx_torch, p, back);
    flash(p, C.torch_flash_range, 45.f, 0.6f);
    play(S.s_glass, p, C.vol_torch);
    play(S.s_pop, p, C.vol_torch);
    CObject* owner = t->H_Parent();
    Msg("[lights] head torch %u of %s (id %u) broken at (%.2f, %.2f, %.2f)", t->ID(), owner ? owner->cName().c_str() : "-", owner ? owner->ID() : 0, p.x,
        p.y, p.z);
    CEntityAlive* npc = smart_cast<CEntityAlive*>(owner);
    if (!stopped_round || !npc || !npc->g_Alive())
        return;
    IKinematics* K = npc->Visual() ? smart_cast<IKinematics*>(npc->Visual()) : nullptr;
    u16 head = K ? K->LL_BoneID("bip01_head") : BI_NONE;
    SHit HS;
    HS.GenHeader(GE_HIT, npc->ID());
    HS.whoID = shooter != u16(-1) ? shooter : npc->ID();
    HS.weaponID = weapon != u16(-1) ? weapon : HS.whoID;
    HS.dir = dir;
    HS.power = C.torch_hit_power;
    HS.p_in_bone_space.set(0.f, 0.f, 0.f);
    HS.impulse = C.torch_hit_impulse;
    HS.hit_type = ALife::eHitTypeStrike;
    HS.boneID = head != BI_NONE ? head : 0;
    NET_Packet P;
    HS.Write_Packet(P);
    CGameObject::u_EventSend(P);
}

// the player's torch takes one more round: whole -> flickering -> dead
void actor_torch_damage(CTorch* t, LPCSTR why)
{
    if (!t || torch_dead(t))
        return;
    const Fvector at = Device.vCameraPosition;
    if (torch_flicker(t))
    {
        t->SetCondition(0.f);
        if (t->torch_active())
            t->Switch(false);
        S.flick_forced = false;
        play(S.s_pop, at, C.vol_pop * 0.7f);
        play(S.s_glass, at, C.vol_glass * 0.6f);
        Msg("[lights] the player's torch is dead (%s)", why);
    }
    else
    {
        t->SetCondition(0.3f);
        S.burst = 6;
        S.flick_next = 0;
        play(S.s_pop, at, C.vol_flicker * 1.5f);
        Msg("[lights] the player's torch flickers (%s)", why);
    }
    S.repairing = false;
}

void actor_torch_update()
{
    CTorch* t = actor_torch();
    const u32 tn = now();
    if (S.repairing)
    {
        if (!t || !Actor() || !Actor()->g_Alive())
        {
            S.repairing = false;
            return;
        }
        const Fvector at = Device.vCameraPosition;
        if (S.last_hit > S.repair_t0) // shot at meanwhile: given up
        {
            S.repairing = false;
            play(S.s_click, at, C.vol_click);
            Msg("[lights] the torch's repair broken off: under fire");
            return;
        }
        const float k = float(tn - S.repair_t0) / _max(1.f, float(S.repair_end - S.repair_t0));
        if (S.repair_step == 0 && k > 0.35f)
        {
            S.repair_step = 1;
            play(S.s_click, at, C.vol_click);
        }
        if (S.repair_step == 1 && k > 0.65f)
        {
            S.repair_step = 2;
            play(S.s_click, at, C.vol_click);
        }
        if (tn >= S.repair_end)
        {
            S.repairing = false;
            t->SetCondition(1.f);
            play(S.s_repair_end, at, C.vol_repair);
            t->Switch(true);
            Msg("[lights] the player's torch repaired");
        }
        return;
    }
    if (!t || !S.on || !t->torch_active() || !torch_flicker(t))
    {
        if (S.flick_forced && t && t->torch_active()) // whole again (or switched off in My Mods): lit as it should be
        {
            if (IRender_Light* L = t->GetLight(0))
                L->set_active(true);
            if (IRender_Light* L = t->GetLight(1))
                L->set_active(true);
        }
        S.flick_forced = false;
        return;
    }
    // a loose contact: mostly on, bursts of dropouts, now and then a long one
    if (tn >= S.flick_next)
    {
        if (S.burst > 0)
        {
            S.flick_on = !S.flick_on;
            --S.burst;
            S.flick_next = tn + (u32)::Random.randI(30, 120);
        }
        else if (!S.flick_on)
        {
            S.flick_on = true;
            S.flick_next = tn + (u32)::Random.randI(500, 2800);
        }
        else if (::Random.randF(0.f, 1.f) < 0.25f)
        {
            S.flick_on = false;
            S.flick_next = tn + (u32)::Random.randI(300, 1100);
            play(S.s_flicker, Device.vCameraPosition, C.vol_flicker);
        }
        else
        {
            S.burst = 2 * ::Random.randI(1, 4);
            S.flick_on = false;
            S.flick_next = tn + (u32)::Random.randI(30, 90);
            play(S.s_flicker, Device.vCameraPosition, C.vol_flicker);
        }
    }
    if (IRender_Light* L = t->GetLight(0))
        L->set_active(S.flick_on);
    if (IRender_Light* L = t->GetLight(1))
        L->set_active(S.flick_on);
    S.flick_forced = true;
}

float ray_sphere(const Fvector& o, const Fvector& d, float len, const Fvector& c, float r)
{
    // the entry distance along o + d t, t in [0, len], or -1
    Fvector oc;
    oc.sub(o, c);
    const float b = oc.dotproduct(d);
    const float cc = oc.square_magnitude() - r * r;
    if (cc <= 0.f)
        return 0.f;
    const float disc = b * b - cc;
    if (disc < 0.f)
        return -1.f;
    const float t = -b - _sqrt(disc);
    return (t >= 0.f && t <= len) ? t : -1.f;
}
} // namespace

namespace portal_mod_lights
{
void snapshot()
{
    S.t_lamps.clear();
    S.t_torches.clear();
    if (!ready() || !S.on)
        return;
    const Fvector& cam = Device.vCameraPosition;
    const float d2 = C.test_dist * C.test_dist;
    for (Lamp& e : S.lamps)
    {
        if (e.broken || e.die_at)
            continue;
        CHangingLamp* l = lamp_obj(e.id);
        if (!l || l->getDestroy() || LA::health(l) <= 0.f)
            continue;
        e.pos = bulb(l);
        if (e.pos.distance_to_sqr(cam) > d2)
            continue;
        S.t_lamps.push_back(Target{e.pos, e.model ? C.lamp_r : C.light_r, e.id, 0});
    }
    for (u16 id : S.torches)
    {
        CTorch* t = smart_cast<CTorch*>(Level().Objects.net_Find(id));
        if (!t || !t->torch_active() || torch_dead(t))
            continue;
        CObject* p = t->H_Parent();
        CEntityAlive* npc = smart_cast<CEntityAlive*>(p);
        if (!npc || !npc->g_Alive())
            continue;
        const Fvector c = lens(t);
        if (c.distance_to_sqr(cam) > d2)
            continue;
        S.t_torches.push_back(Target{c, C.torch_r, t->ID(), p->ID()});
    }
}

float torch_clip(const SBullet* b, const Fvector& pos, const Fvector& dir, float range, int& target)
{
    target = -1;
    float best = range;
    for (u32 i = 0; i < S.t_torches.size(); ++i)
    {
        const Target& t = S.t_torches[i];
        if (t.owner == b->parent_id)
            continue; // his own round
        const float d = ray_sphere(pos, dir, best, t.p, t.r);
        if (d >= 0.f && d < best)
        {
            best = d;
            target = (int)i;
        }
    }
    return best;
}

void torch_reached(const SBullet* b, int target, const Fvector& at, const Fvector& dir)
{
    if (target < 0 || target >= (int)S.t_torches.size())
        return;
    const Target& t = S.t_torches[target];
    S.pend.push_back(Pend{P_TORCH, t.id, t.owner, b->parent_id, b->weapon_id, at, dir, false});
}

void bullet_step(const SBullet* b, const Fvector& from, const Fvector& dir, float len, bool stopped_static)
{
    if (S.t_lamps.empty())
        return;
    Fvector end;
    end.mad(from, dir, len);
    for (const Target& t : S.t_lamps)
    {
        Fvector v;
        v.sub(t.p, from);
        const float along = v.dotproduct(dir);
        if (along < -t.r || along > len + C.static_r)
            continue;
        const float tt = _min(_max(along, 0.f), len);
        Fvector q;
        q.mad(from, dir, tt);
        if (q.distance_to_sqr(t.p) <= t.r * t.r)
        {
            S.pend.push_back(Pend{P_LAMP, t.id, 0, b->parent_id, b->weapon_id, q, dir, false});
            continue;
        }
        if (stopped_static && end.distance_to_sqr(t.p) <= C.static_r * C.static_r)
            S.pend.push_back(Pend{P_LAMP, t.id, 0, b->parent_id, b->weapon_id, end, dir, true});
    }
}

void commit()
{
    if (!ready())
        return;
    xr_vector<Pend> pend;
    pend.swap(S.pend);
    if (S.on)
        for (const Pend& p : pend)
        {
            if (p.type == P_LAMP)
            {
                if (Lamp* e = find(p.id))
                {
                    if (C.debug)
                        Msg("[lights] round %s lamp %u (%.2f m from its bulb)", p.stat ? "at the fixture of" : "through the bulb of", p.id,
                            p.at.distance_to(e->pos));
                    break_lamp(*e, p.dir, p.stat ? "a round at its fixture" : "a round through the bulb");
                }
            }
            else
                break_torch(smart_cast<CTorch*>(Level().Objects.net_Find(p.id)), p.shooter, p.weapon, p.at, p.dir, true);
        }
    const u32 tn = now();
    if (tn >= S.next_scan)
        rescan();
    if (S.on && tn >= S.next_poll) // E59: a bare light's lamp shot down, or its pole fell: it flickers out
    {
        S.next_poll = tn + 250;
        for (Lamp& e : S.lamps)
            if (!e.broken && !e.die_at && e.e59 >= 0)
            {
                const int st = e59_state(e);
                if (st > 0)
                {
                    e.die_at = tn + (u32)C.die_ms;
                    play(S.s_sparks, e.pos, C.vol_sparks);
                    if (C.debug)
                        Msg("[lights] light %u: its E59 lamp %d went (state %d)", e.id, e.e59, st);
                }
            }
    }
    for (Lamp& e : S.lamps) // flickering out
        if (e.die_at && !e.broken)
        {
            CHangingLamp* l = lamp_obj(e.id);
            if (!l || tn >= e.die_at)
            {
                finish(e, l, Fvector().set(0.f, -1.f, 0.f), false, "its lamp went");
                continue;
            }
            LA::lit(l) = ::Random.randF(0.f, 1.f) < 0.45f;
        }
    update_flashes();
    actor_torch_update();
}

void dynamic_hit(CObject* o, u16 bone, const Fvector& point, const Fvector& dir, u16 shooter, u16 weapon)
{
    if (!o || !ready())
        return;
    CActor* a = Actor();
    if (o == a)
    {
        if (shooter == a->ID())
            return;
        S.last_hit = now();
        CTorch* t = actor_torch();
        if (!S.on || !t || torch_dead(t))
            return;
        IKinematics* K = o->Visual() ? smart_cast<IKinematics*>(o->Visual()) : nullptr;
        LPCSTR bn = K && bone < K->LL_BoneCount() ? K->LL_BoneName(bone) : "";
        float p = 0.f;
        if (strstr(bn, "head"))
            p = C.p_head;
        else if (strstr(bn, "neck"))
            p = C.p_neck;
        else if (strstr(bn, "spine2"))
            p = C.p_spine2;
        if (C.debug)
            Msg("[lights] an enemy round in the player's %s (torch chance %.2f)", bn, p);
        if (p > 0.f && ::Random.randF(0.f, 1.f) < p)
            actor_torch_damage(t, bn);
        return;
    }
    if (!S.on)
        return;
    // a round in an NPC right at his lit torch (it went through the head first): the torch goes too
    for (u16 id : S.torches)
    {
        CTorch* t = smart_cast<CTorch*>(Level().Objects.net_Find(id));
        if (!t || t->H_Parent() != o || !t->torch_active() || torch_dead(t))
            continue;
        if (lens(t).distance_to(point) <= C.torch_hit_r)
            break_torch(t, shooter, weapon, point, dir, false);
    }
}

void lamp_hit(CHangingLamp* l, const SHit* h, bool was_alive)
{
    if (!ready() || !S.on || !was_alive || LA::health(l) > 0.f)
        return;
    Lamp* e = find(l->ID());
    if (!e)
    {
        Lamp ne;
        ne.id = l->ID();
        ne.model = l->Visual() != nullptr;
        S.index[ne.id] = S.lamps.size();
        S.lamps.push_back(ne);
        e = &S.lamps.back();
    }
    if (e->broken)
        return;
    finish(*e, l, h ? h->dir : Fvector().set(0.f, -1.f, 0.f), true, "hit");
}

bool lamp_turn_on_blocked(CHangingLamp* l) { return S.on && LA::health(l) <= 0.f; }

// E75c: the sounds and the flashes' lights go with the level (made again at the next break) - a static's destructor
// runs after the sound engine and the renderer are gone at exit (c0000005 in ~State, reported by the E73 session)
void shutdown()
{
    for (Flash& f : S.flashes)
        f.L->set_active(false);
    S.flashes.clear();
    for (xr_vector<ref_sound>* v : {&S.s_glass, &S.s_break, &S.s_pop, &S.s_sparks, &S.s_flicker, &S.s_click, &S.s_repair_start, &S.s_repair_end})
    {
        for (ref_sound& s : *v)
            s.destroy();
        v->clear();
    }
    S.sounds = false;
    S.lamps.clear();
    S.index.clear();
    S.torches.clear();
    S.t_lamps.clear();
    S.t_torches.clear();
    S.pend.clear();
    S.level = nullptr; // the next level (or the save loaded) starts afresh
    S.level_name = nullptr;
}

bool torch_switch(CTorch* t, bool on)
{
    if (!on || !S.on || !t)
        return on;
    CObject* p = t->H_Parent();
    CActor* a = Actor();
    const bool dead = torch_dead(t), flick = torch_flicker(t);
    if (!a || p != a)
        return !dead; // an NPC's (or a lying) broken torch stays dark
    if (S.repairing)
        return false;
    if (!dead && !flick)
        return true;
    // the player's damaged torch switched on: out of fire it is repaired, under fire it comes on as it is
    const bool settled = ready() && now() - S.level_t0 > 3000 && a->g_Alive() && a->inventory().ItemFromSlot(TORCH_SLOT) == t;
    if (settled && float(now() - S.last_hit) >= C.repair_quiet * 1000.f)
    {
        S.repairing = true;
        S.repair_t0 = now();
        S.repair_end = S.repair_t0 + (u32)(C.repair_time * 1000.f);
        S.repair_step = 0;
        play(S.s_repair_start, Device.vCameraPosition, C.vol_repair);
        Msg("[lights] repairing the player's torch (%.1f s)", C.repair_time);
        return false;
    }
    if (dead)
    {
        if (settled)
            play(S.s_click, Device.vCameraPosition, C.vol_click);
        return false;
    }
    return true;
}

// Lua --------------------------------------------------------------------------------------------------------------
void lua_enable(bool on)
{
    if (S.on == on)
        return;
    S.on = on;
    Msg("[lights] %s", on ? "on" : "off");
    if (!on)
    {
        S.repairing = false;
        // the torches work as whole ones: an NPC's broken one may light again when he switches it
    }
}

bool lua_enabled() { return S.on; }

LPCSTR lua_info()
{
    static string512 buf;
    if (!ready())
        return "";
    u32 lamps = 0, lights = 0, broken = 0, linked = 0, lit = 0;
    for (const Lamp& e : S.lamps)
    {
        (e.model ? lamps : lights)++;
        broken += e.broken;
        linked += e.e59 >= 0;
        if (!e.broken)
            if (CHangingLamp* l = lamp_obj(e.id))
                if (IRender_Light* L = LA::light(l))
                    lit += L->get_active();
    }
    u32 tor_on = 0;
    for (u16 id : S.torches)
        if (CTorch* t = smart_cast<CTorch*>(Level().Objects.net_Find(id)))
            tor_on += t->torch_active();
    xr_sprintf(buf, "on=%d lamps=%u lights=%u lit=%u broken=%u linked=%u torches=%u torches_on=%u torches_broken=%u hemi_off=%u actor_torch=%s",
        int(S.on), lamps, lights, lit, broken, linked, (u32)S.torches.size(), tor_on, S.torches_n, S.hemi_n, lua_actor_torch());
    return buf;
}

LPCSTR lua_near(const Fvector& pos, float radius)
{
    static xr_string out;
    out.clear();
    if (!ready())
        return out.c_str();
    if (now() >= S.next_scan)
        rescan();
    xr_vector<std::pair<float, u32>> nr;
    for (u32 i = 0; i < S.lamps.size(); ++i)
    {
        const float d = S.lamps[i].pos.distance_to(pos);
        if (d <= radius)
            nr.emplace_back(d, i);
    }
    std::sort(nr.begin(), nr.end());
    string256 one;
    for (u32 k = 0; k < nr.size() && k < 128; ++k)
    {
        const Lamp& e = S.lamps[nr[k].second];
        LPCSTR st = "broken";
        if (!e.broken)
        {
            CHangingLamp* l = lamp_obj(e.id);
            IRender_Light* L = l ? LA::light(l) : nullptr;
            st = e.die_at ? "dying" : (L && L->get_active()) ? "on" : "off";
        }
        xr_sprintf(one, "%u|%s|%s|%.2f|%.2f|%.2f|%.1f|%.1f;", e.id, e.model ? "lamp" : "light", st, e.pos.x, e.pos.y, e.pos.z, e.range, nr[k].first);
        out += one;
    }
    return out.c_str();
}

LPCSTR lua_torches(const Fvector& pos, float radius)
{
    static xr_string out;
    out.clear();
    if (!ready())
        return out.c_str();
    if (now() >= S.next_scan)
        rescan();
    xr_vector<std::pair<float, u16>> nr;
    for (u16 id : S.torches)
        if (CTorch* t = smart_cast<CTorch*>(Level().Objects.net_Find(id)))
            if (CObject* p = t->H_Parent())
            {
                const float d = p->Position().distance_to(pos);
                if (d <= radius)
                    nr.emplace_back(d, id);
            }
    std::sort(nr.begin(), nr.end());
    string256 one;
    for (auto& [d, id] : nr)
    {
        CTorch* t = smart_cast<CTorch*>(Level().Objects.net_Find(id));
        const Fvector c = lens(t);
        xr_sprintf(one, "%u|%u|%s|%.2f|%.2f|%.2f|%.1f;", t->H_Parent()->ID(), id, torch_dead(t) ? "broken" : t->torch_active() ? "on" : "off", c.x, c.y, c.z, d);
        out += one;
    }
    return out.c_str();
}

bool lua_break(int id)
{
    if (!ready())
        return false;
    if (now() >= S.next_scan)
        rescan();
    Lamp* e = find((u16)id);
    if (!e || e->broken)
        return false;
    break_lamp(*e, Fvector().set(0.f, -1.f, 0.f), "script");
    return true;
}

static CTorch* torch_of(int id)
{
    CObject* o = Level().Objects.net_Find((u16)id);
    if (CTorch* t = smart_cast<CTorch*>(o))
        return t;
    for (u16 tid : S.torches)
        if (CTorch* t = smart_cast<CTorch*>(Level().Objects.net_Find(tid)))
            if (t->H_Parent() && t->H_Parent()->ID() == (u16)id)
                return t;
    return nullptr;
}

bool lua_torch_break(int id)
{
    if (!ready())
        return false;
    if (now() >= S.next_scan)
        rescan();
    CTorch* t = torch_of(id);
    if (!t || torch_dead(t))
        return false;
    break_torch(t, u16(-1), u16(-1), lens(t), Fvector().set(0.f, 0.f, 1.f), false);
    return true;
}

Fvector lua_torch_pos(int id)
{
    if (!ready())
        return Fvector().set(0.f, 0.f, 0.f);
    if (now() >= S.next_scan)
        rescan();
    CTorch* t = torch_of(id);
    return t ? lens(t) : Fvector().set(0.f, 0.f, 0.f);
}

LPCSTR lua_actor_torch()
{
    CTorch* t = g_pGameLevel ? actor_torch() : nullptr;
    if (!t)
        return "none";
    if (S.repairing)
        return "repair";
    return torch_dead(t) ? "dead" : torch_flicker(t) ? "flicker" : "ok";
}

void lua_actor_torch_damage()
{
    if (!ready())
        return;
    S.last_hit = now(); // as an enemy round would: under fire now
    actor_torch_damage(actor_torch(), "script");
}

void lua_actor_torch_repair()
{
    if (!ready())
        return;
    S.repairing = false;
    if (CTorch* t = actor_torch())
        t->SetCondition(1.f);
}

void lua_debug(bool on)
{
    load_cfg();
    C.debug = on;
}
} // namespace portal_mod_lights

void pm_lights_script_register(lua_State* L)
{
    using namespace portal_mod_lights;
    module(L, "level")[def("lights_enable", &lua_enable), def("lights_enabled", &lua_enabled), def("lights_info", &lua_info),
                       def("lights_near", &lua_near), def("lights_torches", &lua_torches), def("lights_break", &lua_break),
                       def("lights_torch_break", &lua_torch_break), def("lights_torch_pos", &lua_torch_pos),
                       def("lights_actor_torch", &lua_actor_torch), def("lights_actor_torch_damage", &lua_actor_torch_damage),
                       def("lights_actor_torch_repair", &lua_actor_torch_repair), def("lights_debug", &lua_debug)];
}
