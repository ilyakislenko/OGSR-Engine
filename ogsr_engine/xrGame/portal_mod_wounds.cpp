// portal-mod E71: NPC wounds by body part (see portal_mod_wounds.h). The engine counts and shows; the script
// (scripts\pm_wounds.script) decides the falls, the dropped weapon, surrender and the calls for help.
#include "stdafx.h"
#include "portal_mod_wounds.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_animation_data.h"
#include "stalker_animation_names.h"
#include "stalker_movement_manager.h"
#include "EntityCondition.h"
#include "Level.h"
#include "Hit.h"
#include "memory_manager.h"
#include "hit_memory_manager.h"
#include "memory_space.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "../Include/xrRender/Kinematics.h"

using namespace luabind;

namespace portal_mod_wounds
{
namespace
{
enum EPart
{
    p_head,
    p_torso,
    p_arm_l,
    p_arm_r,
    p_leg_l,
    p_leg_r,
    p_count
};
const char* const PART_NAME[p_count] = {"head", "torso", "arm_l", "arm_r", "leg_l", "leg_r"};

enum EEvent
{
    ev_hit,
    ev_limp,
    ev_down,
    ev_arm
};
const char* const EV_NAME[] = {"hit", "limp", "down", "arm"};

struct Wound
{
    const CAI_Stalker* obj{}; // the object these wounds belong to (an id is used again by later objects)
    float dmg[p_count]{}; // health taken by each part
    int legs{}; // 0, 1 limps, 2 down
    int arms{}; // 0, 1 an arm hit (the weapon dropped)
    bool surr{}; // gave up (set by the script)
    bool crawl{}; // the hunched creep (set by a script: level.pm_wound_set(id, "crawl", 1))
    u32 t_arm{};
    int last_part{-1};
    u16 last_who{u16(-1)};
    float last_dmg{};
    u32 last_t{};
    bool anims{};
    MotionID m_crawl, m_crawl_stop;
};

struct Event
{
    u16 id;
    u8 kind;
    s8 part;
    u16 who;
    float dmg, health;
};

xr_map<u16, Wound> s_w;
u8 s_fast[65536]{}; // 1: the id has an entry in s_w (the animation and movement queries skip the map otherwise)
xr_vector<Event> s_ev;

bool s_on = true; // My Mods "npc_wounds"
const void* s_level = nullptr; // the level the switch was read for
bool s_debug = false;
// config\portal_mod\pm_wounds.ltx [pm_wounds], pushed by the script (level.pm_wound_tune)
float s_limp_damage = 0.05f; // health one leg has taken: he limps
float s_down_damage = 0.32f; // both legs together: he goes down
float s_down_single = 0.25f; // or one round to a leg this hard
float s_down_both = 0.12f; // or each leg this much
float s_arm_damage = 0.015f; // one round to an arm this hard: the weapon drops (an AK round: 0.04 in the forearm)
u32 s_arm_cooldown = 15000; // ms before an arm hit drops a weapon again
float s_limp_walk = 0.62f, s_limp_run = 0.55f; // a limp's speed: walking, running
float s_crawl_speed = 0.45f; // m/s

void load_switch()
{
    if (s_level == g_pGameLevel)
        return;
    s_level = g_pGameLevel;
    s_w.clear();
    s_ev.clear();
    ZeroMemory(s_fast, sizeof(s_fast));
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
        if (xr_strcmp(_Trim(line), "npc_wounds"))
            continue;
        LPSTR v = _Trim(eq + 1);
        s_on = !xr_strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true");
    }
    fclose(f);
    Msg("* E71 NPC wounds: %s (My Mods)", s_on ? "on" : "off");
}

int part_of(CAI_Stalker* npc, u16 bone)
{
    IKinematics* k = smart_cast<IKinematics*>(npc->Visual());
    if (!k || bone == BI_NONE || bone >= k->LL_BoneCount())
        return p_torso;
    LPCSTR name = k->LL_BoneName(bone);
    if (!name)
        return p_torso;
    string64 low;
    xr_strcpy(low, name);
    _strlwr(low);
    const bool l = !!strstr(low, "_l_"), r = !!strstr(low, "_r_");
    if (l || r)
    {
        if (strstr(low, "thigh") || strstr(low, "calf") || strstr(low, "foot") || strstr(low, "toe"))
            return l ? p_leg_l : p_leg_r;
        if (strstr(low, "clavicle") || strstr(low, "upperarm") || strstr(low, "forearm") || strstr(low, "hand") || strstr(low, "finger"))
            return l ? p_arm_l : p_arm_r;
    }
    if (strstr(low, "head") || strstr(low, "neck") || strstr(low, "eye") || strstr(low, "jaw"))
        return p_head;
    return p_torso;
}

Wound& get(CAI_Stalker* npc)
{
    Wound& w = s_w[npc->ID()];
    if (w.obj != npc)
    {
        w = Wound{};
        w.obj = npc;
    }
    s_fast[npc->ID()] = 1;
    return w;
}

Wound* find(const CAI_Stalker* npc)
{
    if (!s_fast[npc->ID()])
        return nullptr;
    auto it = s_w.find(npc->ID());
    if (it == s_w.end() || it->second.obj != npc)
        return nullptr;
    return &it->second;
}

void forget(u16 id)
{
    s_w.erase(id);
    s_fast[id] = 0;
}

void push(u16 id, EEvent kind, int part, u16 who, float dmg, float health)
{
    if (s_ev.size() < 512)
        s_ev.push_back(Event{id, u8(kind), s8(part), who, dmg, health});
}

int critical_type(int part)
{
    switch (part)
    {
    case p_arm_l: return critical_wound_type_hand_left;
    case p_arm_r: return critical_wound_type_hand_right;
    case p_leg_l: return critical_wound_type_leg_left;
    case p_leg_r: return critical_wound_type_leg_right;
    }
    return -1;
}

CAI_Stalker* stalker(u16 id)
{
    if (!g_pGameLevel)
        return nullptr;
    return smart_cast<CAI_Stalker*>(Level().Objects.net_Find(id));
}
} // namespace

int on_hit(CAI_Stalker* npc, const SHit& hit, float health_before)
{
    load_switch();
    if (!s_on || hit.hit_type != ALife::eHitTypeFireWound)
        return -1;
    if (!npc->g_Alive())
    {
        forget(npc->ID());
        return -1;
    }
    const float lost = npc->conditions().GetHealthLost();
    if (lost <= 0.f)
        return -1;
    const float health = health_before - lost;
    const int part = part_of(npc, hit.boneID);
    const u16 who = hit.who ? hit.who->ID() : u16(-1);
    Wound& w = get(npc);
    w.dmg[part] += lost;
    w.last_part = part;
    w.last_who = who;
    w.last_dmg = lost;
    w.last_t = Device.dwTimeGlobal;
    push(npc->ID(), ev_hit, part, who, lost, health);

    int react = -1;
    if (part == p_leg_l || part == p_leg_r)
    {
        const float both = w.dmg[p_leg_l] + w.dmg[p_leg_r];
        if (w.legs < 2 && (both >= s_down_damage || lost >= s_down_single || (w.dmg[p_leg_l] >= s_down_both && w.dmg[p_leg_r] >= s_down_both)))
        {
            w.legs = 2;
            push(npc->ID(), ev_down, part, who, lost, health);
            react = part;
        }
        else if (w.legs < 1 && w.dmg[part] >= s_limp_damage)
        {
            w.legs = 1;
            push(npc->ID(), ev_limp, part, who, lost, health);
            react = part;
        }
    }
    else if ((part == p_arm_l || part == p_arm_r) && lost >= s_arm_damage && (!w.t_arm || Device.dwTimeGlobal - w.t_arm > s_arm_cooldown))
    {
        w.arms = 1;
        w.t_arm = Device.dwTimeGlobal;
        push(npc->ID(), ev_arm, part, who, lost, health);
        react = part;
    }
    if (s_debug)
        Msg("* E71 %s [%d] hit %s %.3f (health %.2f -> %.2f) legs %d arms %d%s", npc->cName().c_str(), npc->ID(), PART_NAME[part], lost, health_before,
            health, w.legs, w.arms, react >= 0 ? " - reacts" : "");
    if (react < 0 || w.surr || w.crawl)
        return -1;
    return critical_type(react);
}

bool limping(const CAI_Stalker* npc)
{
    if (!s_on)
        return false;
    const Wound* w = find(npc);
    return w && w->legs >= 1 && !w->crawl && !w->surr;
}

float speed(const CAI_Stalker* npc, float v)
{
    if (!s_on || v <= 0.f)
        return v;
    const Wound* w = find(npc);
    if (!w)
        return v;
    if (w->crawl)
        return _min(v, s_crawl_speed);
    if (w->legs >= 1 && !w->surr)
        return v * (const_cast<CAI_Stalker*>(npc)->movement().movement_type() == MonsterSpace::eMovementTypeRun ? s_limp_run : s_limp_walk);
    return v;
}

MotionID global_anim(CAI_Stalker* npc, bool moving)
{
    if (!s_on)
        return MotionID();
    Wound* w = find(npc);
    if (!w || !w->crawl)
        return MotionID();
    if (!w->anims)
    {
        w->anims = true;
        if (IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(npc->Visual()))
        {
            w->m_crawl = ka->ID_Cycle_Safe("walk_fwd_crawl_0");
            w->m_crawl_stop = ka->ID_Cycle_Safe("walk_fwd_crawl_stop");
        }
    }
    if (!moving && w->m_crawl_stop)
        return w->m_crawl_stop;
    return w->m_crawl;
}

void fill_dmg(CStalkerAnimationData* data)
{
    auto& A = data->m_part_animations.A;
    if (A.size() < 3)
        return;
    CStalkerAnimationState& d = A[2]; // dmg_norm_
    const CStalkerAnimationState& s = A[MonsterSpace::eBodyStateStand]; // norm_
    u32 filled = 0, own = 0;
    auto vec = [&](ANIM_VECTOR& dv, const ANIM_VECTOR& sv) {
        if (dv.size() < sv.size())
        {
            dv = sv;
            ++filled;
        }
        else if (!dv.empty())
            ++own;
    };
    for (u32 i = 0; i < d.m_global.A.size() && i < s.m_global.A.size(); ++i)
        vec(d.m_global.A[i].A, s.m_global.A[i].A);
    for (u32 i = 0; i < d.m_torso.A.size() && i < s.m_torso.A.size(); ++i)
        for (u32 j = 0; j < d.m_torso.A[i].A.size() && j < s.m_torso.A[i].A.size(); ++j)
            vec(d.m_torso.A[i].A[j].A, s.m_torso.A[i].A[j].A);
    for (u32 i = 0; i < d.m_movement.A.size() && i < s.m_movement.A.size(); ++i)
        for (u32 j = 0; j < d.m_movement.A[i].A.size() && j < s.m_movement.A[i].A.size(); ++j)
            vec(d.m_movement.A[i].A[j].A, s.m_movement.A[i].A[j].A);
    if (d.m_in_place && s.m_in_place)
        for (u32 i = 0; i < d.m_in_place->A.size() && i < s.m_in_place->A.size(); ++i)
            if (!d.m_in_place->A[i])
            {
                d.m_in_place->A[i] = s.m_in_place->A[i];
                ++filled;
            }
    Msg("* E71 the limp's animations (dmg_norm_): %u sets of its own, %u taken from norm_", own, filled);
}

// ---------------------------------------------------------------------------------------------------------- Lua
namespace
{
string1024 s_buf;

// "legs=1 arms=0 surr=0 crawl=0 head=0.000 torso=0.120 arm_l=0.000 arm_r=0.000 leg_l=0.180 leg_r=0.000 last=leg_l
// who=0 last_dmg=0.180 t=12.3" (t: seconds since the last hit), "" when the stalker has no wounds
LPCSTR lua_info(u16 id)
{
    s_buf[0] = 0;
    CAI_Stalker* npc = stalker(id);
    const Wound* w = npc ? find(npc) : nullptr;
    if (!w)
        return s_buf;
    xr_sprintf(s_buf, "legs=%d arms=%d surr=%d crawl=%d head=%.3f torso=%.3f arm_l=%.3f arm_r=%.3f leg_l=%.3f leg_r=%.3f last=%s who=%d last_dmg=%.3f t=%.1f",
               w->legs, w->arms, int(w->surr), int(w->crawl), w->dmg[p_head], w->dmg[p_torso], w->dmg[p_arm_l], w->dmg[p_arm_r], w->dmg[p_leg_l], w->dmg[p_leg_r],
               w->last_part >= 0 ? PART_NAME[w->last_part] : "none", w->last_who == u16(-1) ? -1 : int(w->last_who), w->last_dmg,
               float(Device.dwTimeGlobal - w->last_t) / 1000.f);
    return s_buf;
}

// every hit and reaction since the last call, "id kind part who dmg health;..." (who -1: none); clears the queue and
// forgets the stalkers that are gone or dead
xr_string s_events;
LPCSTR lua_events()
{
    load_switch();
    s_events.clear();
    string128 one;
    for (const Event& e : s_ev)
    {
        xr_sprintf(one, "%d %s %s %d %.3f %.3f;", int(e.id), EV_NAME[e.kind], e.part >= 0 ? PART_NAME[e.part] : "none", e.who == u16(-1) ? -1 : int(e.who), e.dmg,
                   e.health);
        s_events += one;
    }
    s_ev.clear();
    for (auto it = s_w.begin(); it != s_w.end();)
    {
        CAI_Stalker* npc = stalker(it->first);
        if (!npc || npc != it->second.obj || !npc->g_Alive())
        {
            s_fast[it->first] = 0;
            it = s_w.erase(it);
        }
        else
            ++it;
    }
    return s_events.c_str();
}

// what the script decides: "surrender" 0/1, "crawl" 0/1, "legs" 0..2, "arms" 0/1, "clear" (healed: every wound gone)
void lua_set(u16 id, LPCSTR key, float v)
{
    load_switch();
    CAI_Stalker* npc = stalker(id);
    if (!npc || !key)
        return;
    if (!xr_strcmp(key, "clear"))
    {
        forget(id);
        return;
    }
    Wound& w = get(npc);
    if (!xr_strcmp(key, "surrender"))
        w.surr = v > 0.5f;
    else if (!xr_strcmp(key, "crawl"))
        w.crawl = v > 0.5f;
    else if (!xr_strcmp(key, "legs"))
        w.legs = iFloor(v + 0.5f);
    else if (!xr_strcmp(key, "arms"))
        w.arms = iFloor(v + 0.5f);
    if (s_debug)
        Msg("* E71 %s [%d] %s = %.2f", npc->cName().c_str(), id, key, v);
}

// a comrade hears the call: the shooter goes into his memory as one who hit him (no damage, no script callback) and
// his enemy selection takes it from there
bool lua_alert(u16 id, u16 who_id)
{
    CAI_Stalker* npc = stalker(id);
    CEntityAlive* who = g_pGameLevel ? smart_cast<CEntityAlive*>(Level().Objects.net_Find(who_id)) : nullptr;
    if (!npc || !who || !npc->g_Alive() || !who->g_Alive() || npc->ID() == who->ID())
        return false;
    MemorySpace::CHitObject h;
    h.fill(who, npc, npc->agent_manager().member().mask(npc));
    Fvector d;
    d.sub(npc->Position(), who->Position());
    if (d.square_magnitude() < EPS_S)
        d.set(0.f, 0.f, 1.f);
    h.m_direction = d.normalize();
    h.m_bone_index = 0;
    h.m_amount = 0.1f;
    npc->memory().hit().add(h);
    return true;
}

void lua_switch(bool on)
{
    load_switch();
    if (s_on != on)
        Msg("* E71 NPC wounds switched %s", on ? "on" : "off");
    s_on = on;
}

bool lua_on()
{
    load_switch();
    return s_on;
}

void lua_tune(LPCSTR key, float v)
{
    if (!key)
        return;
    struct
    {
        const char* name;
        float* f;
    } const keys[] = {{"limp_damage", &s_limp_damage}, {"down_damage", &s_down_damage}, {"down_single", &s_down_single}, {"down_both", &s_down_both},
                      {"arm_damage", &s_arm_damage},   {"limp_walk", &s_limp_walk},     {"limp_run", &s_limp_run},       {"crawl_speed", &s_crawl_speed}};
    for (const auto& k : keys)
        if (!xr_strcmp(key, k.name))
        {
            *k.f = v;
            return;
        }
    if (!xr_strcmp(key, "arm_cooldown"))
        s_arm_cooldown = u32(_max(0.f, v) * 1000.f);
    else if (!xr_strcmp(key, "debug"))
        s_debug = v > 0.5f;
}
} // namespace
} // namespace portal_mod_wounds

void pm_wounds_script_register(lua_State* L)
{
    using namespace portal_mod_wounds;
    module(L, "level")[def("pm_wound_info", &lua_info), def("pm_wound_events", &lua_events), def("pm_wound_set", &lua_set), def("pm_wound_alert", &lua_alert),
                       def("pm_wounds_switch", &lua_switch), def("pm_wounds_on", &lua_on), def("pm_wound_tune", &lua_tune)];
}
