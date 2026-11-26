// portal-mod E76: the T-800's machine vision, the game's side - see portal_mod_vision.h. tools/engine_patches/patch_e76.py
// copies this file into ogsr_engine/xrGame. Every Russian word on the screen comes from pm_vision.ltx (cp1251), the
// font's grids from pm_vision_font.ltx (tools/vision_font.py); the layout is the After Effects HUD's (hud_takedown.py:
// 1920 x 1080 units, scaled to the screen's height).
#include "stdafx.h"
#include <dinput.h>
#include <chrono>
#include <algorithm>
#include <random>
#include <array>
#include "portal_mod_vision.h"
#include "../xr_3da/portal_mod_vision_shared.h"
#include "portal_mod_replay.h"
#include "portal_mod_reboot.h"
#include "Level.h"
#include "Actor.h"
#include "entity_alive.h"
#include "CustomMonster.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "Weapon.h"
#include "character_info.h"
#include "string_table.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "EntityCondition.h"
#include "ActorCondition.h"
#include "CustomOutfit.h"
#include "ui/UIInventoryUtilities.h"
#include "xr_level_controller.h"
#include "HUDManager.h"
#include "UI.h"
#include "ui/UIDialogWnd.h"
#include "script_engine.h"
#include "ai_space.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xrcdb/xr_collide_defs.h"
#include "../Include/xrRender/Kinematics.h"

using namespace luabind;

namespace pm_vision
{
namespace
{
//------------------------------------------------------------------------------------------------------------------
// settings (pm_vision.ltx)
struct Font
{
    float x{}, y{}, cw{}, ch{}, adv{}, asc{}, px{};
    bool ok{};
};
enum
{
    F_HUGE,
    F_BIG,
    F_LBL,
    F_TAG,
    F_SM,
    F_COUNT
};
Font g_font[F_COUNT];
float g_tw = 1024.f, g_th = 1024.f, g_wu = 0.5f, g_wv = 0.5f; // the atlas, its white block's texel

struct Cfg
{
    int key{DIK_J};
    float range{120.f}; // m: creatures farther are not read out
    float card_range{80.f};
    float lock_radius{0.14f}; // of the screen's height round its middle
    float lock_time{0.40f}; // s
    float cps{55.f}; // the typing's characters a second
    u32 col{color_rgba(255, 244, 238, 255)}; // the HUD (the AE comp's COL)
    u32 col_hot{color_rgba(255, 70, 46, 255)}; // a high threat, the lock
    u32 col_warm{color_rgba(255, 160, 96, 255)}; // a medium threat
    u32 col_heat{color_rgba(255, 196, 72, 255)}; // the heat readings
    bool hide_hud{true}; // the game's own HUD (minimap, bars, news) off while the vision is up
    bool hide_cross{true}; // and its crosshair
    bool aim_mark{true}; // the vision's own faint aim mark in its place
} C;
xr_map<xr_string, xr_string> g_text; // [pm_vision_text]
xr_vector<std::pair<xr_string, xr_string>> g_species; // [pm_vision_species]: section substring -> name
xr_map<xr_string, xr_string> g_faction; // [pm_vision_faction]: community -> name
const void* g_cfg_level{};

LPCSTR T(LPCSTR id, LPCSTR def)
{
    auto it = g_text.find(id);
    return it != g_text.end() ? it->second.c_str() : def;
}

u32 parse_col(LPCSTR s, u32 def)
{
    int r, g, b;
    if (s && sscanf(s, "%d,%d,%d", &r, &g, &b) == 3)
        return color_rgba(clampr(r, 0, 255), clampr(g, 0, 255), clampr(b, 0, 255), 255);
    return def;
}

// an ini line's value as written: CInifile drops every space outside quotes, so a text with spaces is quoted
xr_string unquote(LPCSTR s)
{
    xr_string v = s ? s : "";
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return v;
}

void load_cfg()
{
    if (g_cfg_level == g_pGameLevel && g_font[F_SM].ok)
        return;
    g_cfg_level = g_pGameLevel;
    string_path fn;
    if (FS.exist(fn, "$game_config$", "portal_mod\\pm_vision.ltx"))
    {
        CInifile ini(fn);
        CInifile* pi = &ini;
        constexpr LPCSTR S = "pm_vision";
        if (ini.section_exist(S))
        {
            if (ini.line_exist(S, "key"))
            {
                const int k = keyname_to_dik(ini.r_string(S, "key"));
                if (k > 0)
                    C.key = k;
            }
            C.range = READ_IF_EXISTS(pi, r_float, S, "range", C.range);
            C.card_range = READ_IF_EXISTS(pi, r_float, S, "card_range", C.card_range);
            C.lock_radius = READ_IF_EXISTS(pi, r_float, S, "lock_radius", C.lock_radius);
            C.lock_time = READ_IF_EXISTS(pi, r_float, S, "lock_time", C.lock_time);
            C.cps = READ_IF_EXISTS(pi, r_float, S, "type_cps", C.cps);
            if (ini.line_exist(S, "color"))
                C.col = parse_col(ini.r_string(S, "color"), C.col);
            if (ini.line_exist(S, "color_threat"))
                C.col_hot = parse_col(ini.r_string(S, "color_threat"), C.col_hot);
            if (ini.line_exist(S, "color_warm"))
                C.col_warm = parse_col(ini.r_string(S, "color_warm"), C.col_warm);
            if (ini.line_exist(S, "color_heat"))
                C.col_heat = parse_col(ini.r_string(S, "color_heat"), C.col_heat);
            portal_mod_vision_frame& V = portal_mod_vision;
            V.heat = READ_IF_EXISTS(pi, r_float, S, "heat", V.heat);
            V.scan = READ_IF_EXISTS(pi, r_float, S, "scan_lines", V.scan);
            V.grain = READ_IF_EXISTS(pi, r_float, S, "grain", V.grain);
            V.ca = READ_IF_EXISTS(pi, r_float, S, "aberration", V.ca);
            V.vignette = READ_IF_EXISTS(pi, r_float, S, "vignette", V.vignette);
            V.glow = READ_IF_EXISTS(pi, r_float, S, "glow", V.glow);
            V.tint = READ_IF_EXISTS(pi, r_float, S, "tint", V.tint);
            V.heat_range = READ_IF_EXISTS(pi, r_float, S, "heat_range", V.heat_range);
            C.hide_hud = !!READ_IF_EXISTS(pi, r_bool, S, "hide_game_hud", C.hide_hud);
            C.hide_cross = !!READ_IF_EXISTS(pi, r_bool, S, "hide_crosshair", C.hide_cross);
            C.aim_mark = !!READ_IF_EXISTS(pi, r_bool, S, "aim_mark", C.aim_mark);
        }
        g_text.clear();
        if (ini.section_exist("pm_vision_text"))
            for (const auto& it : ini.r_section("pm_vision_text").Data)
                g_text[it.first.c_str()] = unquote(it.second.c_str());
        g_faction.clear();
        if (ini.section_exist("pm_vision_faction"))
            for (const auto& it : ini.r_section("pm_vision_faction").Data)
                g_faction[it.first.c_str()] = unquote(it.second.c_str());
        g_species.clear();
        if (ini.section_exist("pm_vision_species"))
            for (const auto& it : ini.r_section("pm_vision_species").Data)
                g_species.emplace_back(it.first.c_str(), unquote(it.second.c_str()));
        // the longest key first: "pseudodog" before "dog"
        std::sort(g_species.begin(), g_species.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    }
    else
        Msg("! E76 vision: config\\portal_mod\\pm_vision.ltx not found");
    if (FS.exist(fn, "$game_config$", "portal_mod\\pm_vision_font.ltx"))
    {
        CInifile ini(fn);
        constexpr LPCSTR S = "pm_vision_font";
        if (ini.line_exist(S, "size"))
        {
            const Fvector2 sz = ini.r_fvector2(S, "size");
            g_tw = sz.x;
            g_th = sz.y;
        }
        if (ini.line_exist(S, "white"))
        {
            const Fvector2 w = ini.r_fvector2(S, "white");
            g_wu = w.x / g_tw;
            g_wv = w.y / g_th;
        }
        static const LPCSTR names[F_COUNT] = {"huge", "big", "lbl", "tag", "sm"};
        static const float px[F_COUNT] = {44.f, 30.f, 21.f, 16.f, 15.f};
        for (int i = 0; i < F_COUNT; ++i)
        {
            Font& f = g_font[i];
            f.px = px[i];
            if (!ini.line_exist(S, names[i]))
                continue;
            float v[6]{};
            if (sscanf(ini.r_string(S, names[i]), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
            {
                f.x = v[0], f.y = v[1], f.cw = v[2], f.ch = v[3], f.adv = v[4], f.asc = v[5];
                f.ok = true;
            }
        }
    }
    else
        Msg("! E76 vision: config\\portal_mod\\pm_vision_font.ltx not found (tools/vision_font.py): no text");
    Msg("* E76 vision: config loaded, key %d, range %.0f m", C.key, C.range);
}

//------------------------------------------------------------------------------------------------------------------
// state
bool g_switch{true}; // My Mods "vision"
const void* g_switch_level{};
bool g_active{}; // the key
float g_fade{}; // 0..1
float g_glitch{}; // 0..1
float g_hit{}; // 0..1
float g_seed{};
float g_now{}; // s: the animation clock (the game's time; a replay's own time in a replay - paused, it stands)
float g_boot{-1.f}; // s since it was switched on (real time)
float g_real_prev{};
float g_actor_hp{-1.f};
float g_suit_prev{-1.f}; // the outfit's condition a frame ago (an armoured hit wears the suit, not the health)
int g_hits{}; // hits taken since it was switched on (the damage panel)
float g_dmg{}; // health lost since then, 0..1
bool g_rebooting{}; // E74's reboot under way: the vision steps aside
float g_hit_t{-100.f}; // g_now of the last hit
float g_suit0{-1.f}; // the outfit's condition when it was switched on
float g_replay_t{-1.f};
u32 g_frame{};
int g_next_num{1};
u32 g_export{}; // 0 normal, 1 the HUD alone, 2 the picture without the HUD

struct Target
{
    u16 id{};
    int num{};
    float t_in{}; // g_now when acquired
    float t_seen{}; // g_now when last seen
    float t_dead{-1.f};
    float t_data{-100.f};
    bool alive{true}, visible{}, ghost{}, hostile{}, aiming{}, monster{};
    float health{1.f}, dist{}, threat{};
    float box[4]{}; // the box round its bones, px
    float off[4]{}, vel[4]{}; // the brackets' spring: offset from the box and its speed
    float head[2]{};
    bool head_ok{}, on_screen{};
    xr_string name, faction, rank, weapon;
    u32 frame{}; // the frame it was last gathered
};
xr_map<u16, Target> g_targets;
xr_map<u16, int> g_nums; // id -> its number while the vision is on
u16 g_lock{u16(-1)}, g_lock_try{u16(-1)};
float g_lock_t{};
bool g_locked{};
u16 g_primary{u16(-1)};
float g_primary_t{};

struct LogLine
{
    xr_string s;
    float t;
};
xr_vector<LogLine> g_log;
xr_vector<xr_string> g_hex;

float S = 1.f; // px per 1080p unit
float SW = 1920.f, SH = 1080.f;

void log_add(const xr_string& s)
{
    g_log.push_back({s, g_now});
    if (g_log.size() > 6)
        g_log.erase(g_log.begin());
}

xr_string upper(LPCSTR s)
{
    xr_string r = s ? s : "";
    for (char& ch : r)
    {
        u8 c = u8(ch);
        if (c >= 'a' && c <= 'z')
            c = u8(c - 32);
        else if (c >= 0xE0)
            c = u8(c - 32);
        else if (c == 0xB8)
            c = 0xA8;
        ch = char(c);
    }
    return r;
}

xr_string fmt(LPCSTR f, ...)
{
    string512 b;
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}

float ease(float x)
{
    x = clampr(x, 0.f, 1.f);
    return 1.f - (1.f - x) * (1.f - x) * (1.f - x);
}

u32 rgba(u32 c, float a)
{
    return subst_alpha(c, u32(clampr(a, 0.f, 1.f) * 255.f + 0.5f));
}

float hash1(float n) { return _abs(std::fmod(std::sin(n * 12.9898f) * 43758.5453f, 1.f)); }

//------------------------------------------------------------------------------------------------------------------
// drawing (screen pixels; layout numbers are 1080p units times S)
void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 c)
{
    auto& v = portal_mod_vision.verts;
    v.push_back({x0, y1, u0, v1, c});
    v.push_back({x0, y0, u0, v0, c});
    v.push_back({x1, y1, u1, v1, c});
    v.push_back({x1, y0, u1, v0, c});
}

void rect(float x0, float y0, float x1, float y1, u32 c) { quad(x0, y0, x1, y1, g_wu, g_wv, g_wu, g_wv, c); }

void seg(float ax, float ay, float bx, float by, float w, u32 c)
{
    const float dx = bx - ax, dy = by - ay, l = _sqrt(dx * dx + dy * dy);
    if (l < 1e-3f)
        return;
    const float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    // the corners in quad()'s order (bottom left, top left, bottom right, top right for a line to the right): the
    // other way round they wind backwards and the pass's own cull state drops them - every slanted line, arc and
    // reticle was missing so (take vision_export_05)
    auto& v = portal_mod_vision.verts;
    v.push_back({ax + nx, ay + ny, g_wu, g_wv, c});
    v.push_back({ax - nx, ay - ny, g_wu, g_wv, c});
    v.push_back({bx + nx, by + ny, g_wu, g_wv, c});
    v.push_back({bx - nx, by - ny, g_wu, g_wv, c});
}

void hline(float x0, float x1, float y, float w, u32 c) { rect(x0, y - w * 0.5f, x1, y + w * 0.5f, c); }
void vline(float x, float y0, float y1, float w, u32 c) { rect(x - w * 0.5f, y0, x + w * 0.5f, y1, c); }

void arc(float cx, float cy, float r, float a0, float a1, float w, u32 c) // degrees, 0 = right, clockwise on the screen
{
    const int n = std::max(2, int(_abs(a1 - a0) / 6.f));
    float px = cx + r * _cos(deg2rad(a0)), py = cy + r * _sin(deg2rad(a0));
    for (int i = 1; i <= n; ++i)
    {
        const float a = deg2rad(a0 + (a1 - a0) * float(i) / float(n));
        const float qx = cx + r * _cos(a), qy = cy + r * _sin(a);
        seg(px, py, qx, qy, w, c);
        px = qx, py = qy;
    }
}

// a line of text at its top left (align 1 centre, 2 right); n < 0: all of it. Returns its width.
float text(float x, float y, LPCSTR s, int f, u32 c, int align = 0, int n = -1)
{
    const Font& F = g_font[f];
    if (!F.ok || !s)
        return 0.f;
    int len = int(xr_strlen(s));
    if (n >= 0 && n < len)
        len = n;
    const float adv = F.adv * S;
    const float w = float(len) * adv;
    if (align == 1)
        x -= w * 0.5f;
    else if (align == 2)
        x -= w;
    if (fsimilar(S, 1.f))
    {
        x = std::floor(x + 0.5f);
        y = std::floor(y + 0.5f);
    }
    for (int i = 0; i < len; ++i)
    {
        const u8 code = u8(s[i]);
        if (code <= 32)
            continue;
        const int idx = code - 32;
        const float cx = F.x + float(idx % 16) * F.cw, cy = F.y + float(idx / 16) * F.ch;
        const float px0 = x + float(i) * adv - S, py0 = y - S;
        quad(px0, py0, px0 + F.cw * S, py0 + F.ch * S, cx / g_tw, cy / g_th, (cx + F.cw) / g_tw, (cy + F.ch) / g_th, c);
    }
    return w;
}

float text_w(LPCSTR s, int f) { return g_font[f].ok && s ? float(xr_strlen(s)) * g_font[f].adv * S : 0.f; }

// typed out from t0 at C.cps, a block cursor blinking while it types; returns false before t0
bool typed(float x, float y, LPCSTR s, int f, u32 c, float t0, float cps = 0.f)
{
    if (g_now < t0)
        return false;
    const int len = int(xr_strlen(s));
    const int n = int((g_now - t0) * (cps > 0.f ? cps : C.cps));
    const float w = text(x, y, s, f, c, 0, n);
    if (n < len + 6 && int(g_now * 10.f) % 2 == 0)
    {
        const float px = g_font[f].px * S;
        rect(x + w + 3.f * S, y + 2.f * S, x + w + 3.f * S + px * 0.5f, y + px, rgba(c, (c >> 24) / 255.f * 0.9f));
    }
    return true;
}

void brackets(float x0, float y0, float x1, float y1, float L, float w, u32 c)
{
    L = std::min(L, std::min(x1 - x0, y1 - y0) * 0.45f);
    hline(x0, x0 + L, y0, w, c), vline(x0, y0, y0 + L, w, c);
    hline(x1 - L, x1, y0, w, c), vline(x1, y0, y0 + L, w, c);
    hline(x0, x0 + L, y1, w, c), vline(x0, y1 - L, y1, w, c);
    hline(x1 - L, x1, y1, w, c), vline(x1, y1 - L, y1, w, c);
}

void dashed_box(float x0, float y0, float x1, float y1, float w, u32 c)
{
    const float d = 10.f * S, g = 7.f * S;
    for (float x = x0; x < x1; x += d + g)
        hline(x, std::min(x + d, x1), y0, w, c), hline(x, std::min(x + d, x1), y1, w, c);
    for (float y = y0; y < y1; y += d + g)
        vline(x0, y, std::min(y + d, y1), w, c), vline(x1, y, std::min(y + d, y1), w, c);
}

void reticle(float x, float y, float r, float rot, u32 c) // the AE comp's lock reticle
{
    arc(x, y, r, 0.f, 360.f, 2.f * S, c);
    const float ro = r + 9.f * S;
    for (int k = 0; k < 4; ++k)
    {
        const float m = 45.f + 90.f * float(k) + rot;
        arc(x, y, ro, m - 16.f, m + 16.f, 3.f * S, c);
    }
    for (int k = 0; k < 4; ++k)
    {
        const float a = deg2rad(90.f * float(k) + rot);
        const float r0 = r - std::min(14.f * S, r * 0.35f), r1 = r + 4.f * S;
        seg(x + _cos(a) * r0, y + _sin(a) * r0, x + _cos(a) * r1, y + _sin(a) * r1, 2.f * S, c);
    }
}

bool project(const Fvector& p, float& sx, float& sy)
{
    const Fmatrix& M = Device.mFullTransform;
    const float x = p.x * M._11 + p.y * M._21 + p.z * M._31 + M._41;
    const float y = p.x * M._12 + p.y * M._22 + p.z * M._32 + M._42;
    const float w = p.x * M._14 + p.y * M._24 + p.z * M._34 + M._44;
    if (w < 0.1f)
        return false;
    sx = (x / w + 1.f) * 0.5f * SW;
    sy = (1.f - y / w) * 0.5f * SH;
    return true;
}

bool ray_clear(const Fvector& from, const Fvector& to)
{
    Fvector d;
    d.sub(to, from);
    const float dist = d.magnitude();
    if (dist < 0.4f)
        return true;
    d.div(dist);
    collide::rq_result R;
    if (!Level().ObjectSpace.RayPick(from, d, dist - 0.2f, collide::rqtStatic, R, nullptr))
        return true;
    return R.range >= dist - 0.35f;
}

//------------------------------------------------------------------------------------------------------------------
// gathering
struct Cand
{
    u16 id{};
    CObject* O{}; // the live object (data); in a replay may be null
    IKinematics* K{};
    IRenderable* R{};
    Fmatrix xf{};
    bool alive{true}, ghost{};
    float health{1.f};
};

LPCSTR species_name(CObject* O)
{
    if (!O)
        return T("unknown", "OBJECT");
    LPCSTR sect = O->cNameSect().c_str();
    for (const auto& s : g_species)
        if (strstr(sect, s.first.c_str()))
            return s.second.c_str();
    return T("mutant", "MUTANT");
}

void refresh_data(Target& g, CObject* O)
{
    if (g_now - g.t_data < 0.5f && g.name.size())
        return;
    g.t_data = g_now;
    CEntityAlive* ea = smart_cast<CEntityAlive*>(O);
    // OGSR's mutants are inventory owners too (their loot), but with no community of game_relations: asking theirs is
    // a fatal "item by index not found in section game_relations"
    CGameObject* go = smart_cast<CGameObject*>(O);
    CInventoryOwner* io = go && go->cast_base_monster() ? nullptr : smart_cast<CInventoryOwner*>(O);
    g.monster = !io;
    if (io)
    {
        g.name = upper(io->Name());
        const CHARACTER_COMMUNITY_ID cid = io->CharacterInfo().Community().id();
        auto fi = g_faction.find(cid.c_str() ? cid.c_str() : "");
        g.faction = fi != g_faction.end() ? fi->second : upper(CStringTable().translate(cid).c_str());
        g.rank = upper(CStringTable().translate(InventoryUtilities::GetRankAsText(io->Rank())).c_str());
        // in hands; else the rifle or pistol he carries (a stalker draws it only once he fights); none once dropped
        PIItem it = io->inventory().ActiveItem();
        if (it && smart_cast<CWeapon*>(it))
            g.weapon = upper(it->NameShort());
        else if (PIItem w = io->inventory().ItemFromSlot(SECOND_WEAPON_SLOT) ? io->inventory().ItemFromSlot(SECOND_WEAPON_SLOT)
                                                                               : io->inventory().ItemFromSlot(FIRST_WEAPON_SLOT))
            g.weapon = upper(w->NameShort()) + " \xB7 " + T("weapon_away", "NOT DRAWN");
        else
            g.weapon = it ? upper(it->NameShort()) : xr_string(T("no_weapon", "NONE"));
    }
    else
    {
        g.name = upper(species_name(O));
        g.faction = T("fauna", "ZONE FAUNA");
        g.rank.clear();
        g.weapon.clear();
    }
    g.hostile = ea && Actor() && ea->is_relation_enemy(Actor());
    g.aiming = false;
    if (CCustomMonster* cm = smart_cast<CCustomMonster*>(O))
        g.aiming = Actor() && cm->memory().enemy().selected() == Actor() && !pm_replay::lua_active(); // a replay's man is a
                                                                                                    // puppet: not now's aim
}

void gather(xr_vector<Cand>& out)
{
    out.clear();
    if (pm_replay::lua_active())
    {
        static xr_vector<pm_replay::VisionPuppet> pp;
        pm_replay::vision_puppets(pp);
        for (const auto& p : pp)
        {
            Cand c;
            c.id = p.id;
            c.O = Level().Objects.net_Find(p.id);
            c.K = p.K;
            c.R = p.R;
            c.xf = p.xform;
            if (p.pad & 0x8000)
            {
                c.alive = (p.pad & 0x4000) != 0;
                c.health = clampr(float(p.pad & 0x3FFF) / 100.f, 0.f, 1.f); // the recorder keeps it in hundredths
            }
            c.ghost = p.visual.size() && strstr(p.visual.c_str(), "xray");
            if (c.K)
                out.push_back(c);
        }
        return;
    }
    CActor* me = Actor();
    CObjectList& L = Level().Objects;
    const Fvector& cam = Device.vCameraPosition;
    for (u32 i = 0, n = L.o_count(); i < n; ++i)
    {
        CObject* O = L.o_get_by_iterator(i);
        if (!O || O->getDestroy() || O == me || O->H_Parent() || !O->Visual())
            continue;
        CEntityAlive* ea = smart_cast<CEntityAlive*>(O);
        if (!ea || !smart_cast<CCustomMonster*>(O))
            continue;
        // a bloodsucker gone invisible: its predator model (a distortion only, "..._xray"), or hidden by a script
        const bool ghost = O->GetTransparency() > 0.5f || strstr(O->cNameVisual().c_str(), "xray") ||
            (!O->getVisible() && strstr(O->cNameSect().c_str(), "bloodsucker"));
        if (!O->getVisible() && !ghost)
            continue;
        if (O->Position().distance_to(cam) > C.range)
            continue;
        const bool alive = !!ea->g_Alive();
        if (!alive)
        {
            auto it = g_targets.find(O->ID());
            if (it == g_targets.end() || (it->second.t_dead >= 0.f && g_now - it->second.t_dead > 1.2f))
                continue; // the dead are read out only as they die
        }
        Cand c;
        c.id = O->ID();
        c.O = O;
        c.K = O->Visual()->dcast_PKinematics();
        c.R = O;
        c.xf = O->XFORM();
        c.alive = alive;
        c.health = clampr(ea->GetfHealth(), 0.f, 1.f);
        c.ghost = ghost;
        if (c.K)
            out.push_back(c);
    }
}

// the box round its bones on the screen and the head; false when it is off the screen or behind
bool screen_box(const Cand& c, Target& g, Fvector& head_w, Fvector& chest_w, Fvector& pelvis_w)
{
    IKinematics* K = c.K;
    K->CalculateBones(TRUE);
    const u16 nb = K->LL_BoneCount();
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int n = 0;
    Fvector top{}, lo{};
    float top_y = -1e9f, lo_y = 1e9f;
    for (u16 b = 0; b < nb; ++b)
    {
        Fvector w;
        c.xf.transform_tiny(w, K->LL_GetTransform(b).c);
        if (w.y > top_y)
            top_y = w.y, top = w;
        if (w.y < lo_y)
            lo_y = w.y, lo = w;
        float sx, sy;
        if (!project(w, sx, sy))
            continue;
        x0 = std::min(x0, sx), y0 = std::min(y0, sy), x1 = std::max(x1, sx), y1 = std::max(y1, sy);
        ++n;
    }
    if (n < 2)
        return false;
    auto bone = [&](LPCSTR nm, Fvector& out) {
        const u16 id = K->LL_BoneID(nm);
        if (id == BI_NONE)
            return false;
        c.xf.transform_tiny(out, K->LL_GetTransform(id).c);
        return true;
    };
    if (!bone("bip01_head", head_w))
        head_w = top;
    if (!bone("bip01_spine2", chest_w) && !bone("bip01_spine1", chest_w) && !bone("bip01_spine", chest_w))
        chest_w.lerp(top, lo, 0.35f);
    if (!bone("bip01_pelvis", pelvis_w))
        pelvis_w.lerp(top, lo, 0.6f);
    const float h = std::max(y1 - y0, 12.f * S);
    y0 -= h * 0.11f;
    y1 += h * 0.03f;
    x0 -= h * 0.07f;
    x1 += h * 0.07f;
    if (x1 - x0 < 16.f * S)
    {
        const float m = (x0 + x1) * 0.5f;
        x0 = m - 8.f * S, x1 = m + 8.f * S;
    }
    if (x1 < -50.f || x0 > SW + 50.f || y1 < -50.f || y0 > SH + 50.f)
        return false;
    g.box[0] = x0, g.box[1] = y0, g.box[2] = x1, g.box[3] = y1;
    g.head_ok = project(head_w, g.head[0], g.head[1]);
    return true;
}

void update_targets(float dt)
{
    static xr_vector<Cand> cands;
    gather(cands);
    const Fvector& cam = Device.vCameraPosition;
    ++g_frame;
    for (const Cand& c : cands)
    {
        auto it = g_targets.find(c.id);
        const bool fresh = it == g_targets.end();
        Target tmp;
        Target& g = fresh ? tmp : it->second;
        Fvector head, chest, pelvis;
        if (!screen_box(c, g, head, chest, pelvis))
            continue;
        const bool vis = ray_clear(cam, head) || ray_clear(cam, chest) || ray_clear(cam, pelvis);
        if (!vis && (fresh || g_now - g.t_seen > 0.3f))
            continue; // not in sight (honest: no wallhack)
        if (fresh)
        {
            if (!c.alive)
                continue;
            g.id = c.id;
            g.t_in = g_now;
            auto n = g_nums.find(c.id);
            g.num = n != g_nums.end() ? n->second : (g_nums[c.id] = g_next_num++);
            // the brackets come in from a box 1.8 times as big and spring onto it
            const float cx = (g.box[0] + g.box[2]) * 0.5f, cy = (g.box[1] + g.box[3]) * 0.5f;
            const float hw = (g.box[2] - g.box[0]) * 0.9f, hh = (g.box[3] - g.box[1]) * 0.9f;
            g.off[0] = (cx - hw) - g.box[0], g.off[1] = (cy - hh) - g.box[1];
            g.off[2] = (cx + hw) - g.box[2], g.off[3] = (cy + hh) - g.box[3];
        }
        if (vis)
            g.t_seen = g_now;
        g.visible = vis;
        g.ghost = c.ghost;
        g.dist = head.distance_to(cam);
        g.health = c.health;
        g.frame = g_frame;
        if (g.alive && !c.alive)
        {
            g.t_dead = g_now;
            log_add(fmt("%s-%02d: %s", T("target_prefix", "T"), g.num, T("log_kill", "TERMINATED")));
        }
        g.alive = c.alive;
        if (c.O)
            refresh_data(g, c.O);
        else if (g.name.empty())
            g.name = T("unknown", "OBJECT");
        // threat
        float th = 0.f;
        if (g.alive)
        {
            th += g.hostile ? 0.45f : 0.f;
            th += g.aiming ? 0.30f : 0.f;
            th += (!g.monster && g.weapon.size() && g.weapon != T("no_weapon", "NONE")) ? 0.10f : (g.monster && g.hostile ? 0.08f : 0.f);
            th += 0.15f * clampr(1.f - g.dist / 80.f, 0.f, 1.f);
            if (g.ghost)
                th += 0.2f;
        }
        g.threat = clampr(th, 0.f, 1.f);
        if (fresh)
        {
            g_targets[c.id] = g;
            if (c.ghost)
                log_add(T("log_heat", "HEAT SIGNATURE: OPTICS NEGATIVE"));
            else
                log_add(fmt("%s-%02d %s: %s", T("target_prefix", "T"), g.num, T("log_found", "ACQUIRED"), g.name.c_str()));
        }
    }
    // the springs (an under-damped spring: ~12 % overshoot), the lost ones dropped
    const float k = 260.f, d = 2.f * _sqrt(k) * 0.55f;
    for (auto it = g_targets.begin(); it != g_targets.end();)
    {
        Target& g = it->second;
        if (g.frame != g_frame || (g.t_dead >= 0.f && g_now - g.t_dead > 1.2f))
        {
            it = g_targets.erase(it);
            continue;
        }
        g.on_screen = true;
        float rest = dt;
        while (rest > 1e-5f)
        {
            const float h = std::min(rest, 1.f / 120.f);
            for (int i = 0; i < 4; ++i)
            {
                g.vel[i] += (-k * g.off[i] - d * g.vel[i]) * h;
                g.off[i] += g.vel[i] * h;
            }
            rest -= h;
        }
        ++it;
    }
}

void update_lock()
{
    // the one nearest the middle of the screen, alive and in sight, within the lock radius
    const float cx = SW * 0.5f, cy = SH * 0.5f, rad = C.lock_radius * SH;
    u16 best = u16(-1);
    float bd = 1e9f;
    for (auto& [id, g] : g_targets)
    {
        if (!g.alive || !g.visible)
            continue;
        const float px = g.head_ok ? g.head[0] : (g.box[0] + g.box[2]) * 0.5f;
        const float py = g.head_ok ? g.head[1] : (g.box[1] + g.box[3]) * 0.5f;
        const bool inside = cx > g.box[0] && cx < g.box[2] && cy > g.box[1] && cy < g.box[3];
        float dd = std::min(_sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)), _sqrt(_sqr((g.box[0] + g.box[2]) * 0.5f - cx) + _sqr((g.box[1] + g.box[3]) * 0.5f - cy)));
        if (inside)
            dd *= 0.25f;
        if (dd < rad && dd < bd)
            bd = dd, best = id;
    }
    if (best != g_lock_try)
    {
        g_lock_try = best;
        g_lock_t = g_now;
        if (g_locked && best != g_lock)
            g_locked = false;
    }
    if (best == u16(-1))
    {
        g_lock = u16(-1);
        g_locked = false;
        return;
    }
    g_lock = best;
    if (!g_locked && g_now - g_lock_t >= C.lock_time)
    {
        g_locked = true;
        g_glitch = std::max(g_glitch, 0.38f);
        Target& g = g_targets[best];
        // a kick outward: the brackets jump and settle on it
        g.vel[0] -= 260.f * S, g.vel[1] -= 260.f * S, g.vel[2] += 260.f * S, g.vel[3] += 260.f * S;
        log_add(fmt("%s: %s-%02d", T("log_lock", "LOCK"), T("target_prefix", "T"), g.num));
    }
}

//------------------------------------------------------------------------------------------------------------------
// the HUD
LPCSTR threat_word(float th) { return th >= 0.72f ? T("threat_high", "HIGH") : th >= 0.45f ? T("threat_mid", "MEDIUM") : th > 0.15f ? T("threat_low", "LOW") : T("threat_none", "NONE"); }
u32 threat_col(float th) { return th >= 0.72f ? C.col_hot : th >= 0.45f ? C.col_warm : C.col; }

// where the priority target's card goes: right of its brackets, left, above or below them - the place that covers the
// fewest other targets' brackets and tags and stays on the screen; it keeps its place until another is clearly better
// (no hopping from frame to frame as men move)
int g_card_pick{-1};
float g_card_for{-1.f};

float rect_overlap(const float* a, const float* b)
{
    const float w = std::min(a[2], b[2]) - std::max(a[0], b[0]), h = std::min(a[3], b[3]) - std::max(a[1], b[1]);
    return w > 0.f && h > 0.f ? w * h : 0.f;
}

void card_place(u16 self, float x0, float y0, float x1, float y1, float cw, float ch, float& tx, float& ty)
{
    xr_vector<std::array<float, 4>> others;
    for (const auto& [id, o] : g_targets)
        if (id != self && o.on_screen && (o.t_dead < 0.f || g_now - o.t_dead < 0.9f))
        {
            const float ox0 = o.box[0] + o.off[0], oy0 = o.box[1] + o.off[1], ox1 = o.box[2] + o.off[2], oy1 = o.box[3] + o.off[3];
            const float tag = (o.ghost ? 210.f : (o.t_dead >= 0.f ? 150.f : 70.f)) * S; // its number and distance, or the stamp
            others.push_back({ox0 - 6.f * S, oy0 - 8.f * S, ox1 + 8.f * S + tag, oy1 + 14.f * S});
        }
    const float yt = y0 - 4.f * S, lo = 120.f * S, hi = SH - 160.f * S - ch;
    const float cand[6][2] = {
        {x1 + 26.f * S, yt}, {x0 - 26.f * S - cw, yt}, {x1 + 26.f * S, yt - ch - 10.f * S},
        {x0 - 26.f * S - cw, yt - ch - 10.f * S}, {x1 + 26.f * S, y1 + 24.f * S}, {x0 - 26.f * S - cw, y1 + 24.f * S}};
    float score[6], best = flt_max;
    int bi = 0;
    for (int i = 0; i < 6; ++i)
    {
        const float cx = cand[i][0], cy = clampr(cand[i][1], lo, std::max(lo, hi));
        const float r[4] = {cx - 4.f * S, cy - 4.f * S, cx + cw + 4.f * S, cy + ch + 4.f * S};
        float s = float(i) * 40.f * S * S; // the order above wins a tie
        for (const auto& o : others)
            s += rect_overlap(r, o.data());
        const float off = std::max(0.f, 70.f * S - cx) + std::max(0.f, cx + cw - (SW - 60.f * S)) + std::fabs(cy - cand[i][1]) * 0.5f;
        s += off * ch * 3.f; // off the screen (or pushed off its place) costs more than covering a tag
        score[i] = s;
        if (s < best)
            best = s, bi = i;
    }
    if (g_card_for != g_primary_t || g_card_pick < 0)
        g_card_pick = bi, g_card_for = g_primary_t;
    else if (best + 600.f * S * S < score[g_card_pick])
        g_card_pick = bi;
    tx = clampr(cand[g_card_pick][0], 70.f * S, std::max(70.f * S, SW - 60.f * S - cw));
    ty = clampr(cand[g_card_pick][1], lo, std::max(lo, hi));
}

void draw_target(const Target& g, float a)
{
    const float x0 = g.box[0] + g.off[0], y0 = g.box[1] + g.off[1], x1 = g.box[2] + g.off[2], y1 = g.box[3] + g.off[3];
    const bool primary = g.id == g_primary;
    const bool locked = g.id == g_lock;
    if (g.t_dead >= 0.f)
    {
        // the kill: the box goes solid, a cross, the stamp
        const float dt = g_now - g.t_dead, q = dt / 0.9f;
        if (q > 1.f)
            return;
        const float blink = (int(dt / 0.07f) % 2 == 0 || q > 0.4f) ? 1.f : 0.25f;
        brackets(x0, y0, x1, y1, 16.f * S, 4.f * S, rgba(C.col, a * blink * (1.f - q * 0.6f)));
        seg(x0, y0, x1, y1, 2.f * S, rgba(C.col, a * blink * (1.f - q)));
        seg(x1, y0, x0, y1, 2.f * S, rgba(C.col, a * blink * (1.f - q)));
        LPCSTR st = T("stamp_kill", "TERMINATED");
        const float sx = std::min(x1 + 14.f * S, SW - 60.f * S - text_w(st, F_LBL));
        text(sx, y0, st, F_LBL, rgba(C.col, a * blink * std::min(1.f, 2.2f * (1.f - q))));
        return;
    }
    const float age = g_now - g.t_in;
    // a fresh one pings: a diamond on its chest and a ring opening out
    if (age < 0.9f)
    {
        const float q = age / 0.9f, cx = (x0 + x1) * 0.5f, cy = y0 + (y1 - y0) * 0.35f;
        const float rr = (12.f + 64.f * ease(q)) * S;
        arc(cx, cy, rr, 0.f, 360.f, 2.f * S, rgba(C.col, a * 0.85f * (1.f - q)));
    }
    const u32 c = rgba(g.ghost ? C.col_heat : (primary ? threat_col(g.threat) : C.col), a * (primary || locked ? 0.95f : 0.62f) * (g.visible ? 1.f : 0.5f));
    const float L = std::min(16.f * S, (x1 - x0) * 0.3f);
    if (g.ghost)
        dashed_box(x0, y0, x1, y1, 2.f * S, c);
    else
        brackets(x0, y0, x1, y1, L, (primary ? 3.f : 2.f) * S, c);
    // the health bar under it
    if (!g.ghost && x1 - x0 > 14.f * S)
    {
        const float by = y1 + 6.f * S;
        rect(x0, by, x1, by + 3.f * S, rgba(C.col, a * 0.18f));
        rect(x0, by, x0 + (x1 - x0) * g.health, by + 3.f * S, rgba(g.health < 0.3f ? C.col_hot : C.col, a * 0.75f));
    }
    LPCSTR P = T("target_prefix", "T");
    if (g.ghost)
    {
        // the heat reading (the AE comp's heat label) and a scanner ring turning round it
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.4f + y1 * 0.2f - y0 * 0.2f + (y1 - y0) * 0.1f;
        const float rr = (x1 - x0) * 0.75f;
        for (int j = 0; j < 12; ++j)
        {
            const float a0 = float(j) * 30.f + std::fmod(g_now * 40.f, 360.f);
            arc(cx, cy, rr, a0, a0 + 14.f, std::max(1.f, S), rgba(C.col_heat, a * 0.75f));
        }
        const float deg = 36.4f + 4.f * (0.5f + 0.5f * std::sin(g_now * 2.5f + float(g.id)));
        text(x1 + 12.f * S, y0, fmt("%s  +%.1f%s", T("heat", "HEAT"), deg, "\xB0" "C").c_str(), F_TAG, rgba(C.col_heat, a * 0.95f));
        text(x1 + 12.f * S, y0 + 19.f * S, T("heat_source", "SOURCE: NOT VISIBLE"), F_SM, rgba(C.col_heat, a * 0.7f));
        text(x1 + 12.f * S, y0 + 37.f * S, fmt("%.0f %s", g.dist, T("m", "M")).c_str(), F_SM, rgba(C.col_heat, a * 0.6f));
        return;
    }
    if (!primary)
    {
        const float tx = x1 + 8.f * S;
        text(tx, y0 - 2.f * S, fmt("%s-%02d", P, g.num).c_str(), F_TAG, c);
        text(tx, y0 + 18.f * S, fmt("%.0f %s", g.dist, T("m", "M")).c_str(), F_SM, rgba(C.col, a * 0.5f));
        return;
    }
    // the priority target's card, typed out: number and threat, name, faction and rank, weapon, health and distance
    xr_vector<xr_string> lines;
    lines.push_back(fmt("%s-%02d  %s: %s", P, g.num, T("threat", "THREAT"), threat_word(g.threat)));
    lines.push_back(g.name);
    if (g.rank.size())
        lines.push_back(fmt("%s \xB7 %s", g.faction.c_str(), g.rank.c_str()));
    else
        lines.push_back(g.faction);
    if (!g.monster)
        lines.push_back(fmt("%s: %s", T("weapon", "WEAPON"), g.weapon.c_str()));
    lines.push_back(fmt("%s %3.0f%%   %s %.0f %s", T("health", "HEALTH"), g.health * 100.f, T("dist", "DIST"), g.dist, T("m", "M")));
    if (g.aiming) // a mutant has no weapon to train: it hunts; a man with none in hands has picked me as his enemy
        lines.push_back(g.monster ? T("hunting", "HUNTING UNIT")
                                  : (g.weapon.find(T("weapon_away", "NOT DRAWN")) == xr_string::npos && g.weapon != T("no_weapon", "NONE")
                                         ? T("aiming", "WEAPON TRAINED ON UNIT")
                                         : T("enemy_of", "UNIT MARKED AS ENEMY")));
    const float lh = 20.f * S;
    float cw = 0.f;
    for (const auto& s : lines)
        cw = std::max(cw, text_w(s.c_str(), F_SM));
    cw = std::max(cw, text_w(lines[0].c_str(), F_TAG));
    float tx, ty;
    card_place(g.id, x0, y0, x1, y1, cw, 6.f * S + lh * float(lines.size()), tx, ty);
    // the leader from the brackets' corner to the card
    const bool right = tx > x1;
    const float lx = right ? x1 : x0, ly = y0;
    const float ex = right ? tx - 6.f * S : tx + cw + 6.f * S, ey = ty + 9.f * S;
    const float lt = ease((g_now - g_primary_t) / 0.18f);
    const u32 cl = rgba(C.col, a * 0.55f);
    seg(lx, ly, lx + (ex - lx) * lt, ly + (ey - ly) * lt, 1.5f * S, cl);
    if (lt >= 1.f)
        hline(right ? ex : ex - cw * 0.2f, right ? ex + cw * 0.2f + 6.f * S : ex, ey + 12.f * S, 1.f * S, rgba(C.col, a * 0.35f));
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const float t0 = g_primary_t + 0.15f + float(i) * 0.20f;
        const int f = i == 0 ? F_TAG : F_SM;
        const u32 col = i == 0 ? rgba(threat_col(g.threat), a) : rgba(C.col, a * (i == lines.size() - 1 && g.aiming ? 0.95f : 0.82f));
        typed(tx, ty + (i == 0 ? 0.f : 6.f * S) + float(i) * lh, lines[i].c_str(), f, col, t0);
    }
}

void draw_lock(float a)
{
    if (g_lock == u16(-1))
        return;
    auto it = g_targets.find(g_lock);
    if (it == g_targets.end() || !it->second.head_ok || it->second.t_dead >= 0.f)
        return;
    const Target& g = it->second;
    const float p = ease((g_now - g_lock_t) / C.lock_time);
    const float base = clampr((g.box[2] - g.box[0]) * 0.32f, 18.f * S, 48.f * S);
    const float r = base * (2.2f - 1.2f * p);
    reticle(g.head[0], g.head[1], r, 120.f * (1.f - p) + (g_locked ? g_now * 25.f : 0.f), rgba(g_locked ? C.col_hot : C.col, a * (0.3f + 0.7f * p)));
    if (g_locked) // under the box, clear of the card at its right
        text((g.box[0] + g.box[2]) * 0.5f, g.box[3] + g.off[3] + 14.f * S, T("lock", "LOCK"), F_LBL,
             rgba(C.col_hot, a * ((int(g_now / 0.06f) % 2 == 0) ? 1.f : 0.4f)), 1);
}

void draw_chrome(float a)
{
    // the frame's corners
    const float m = 34.f * S, L = 70.f * S;
    const u32 cc = rgba(C.col, a * 0.6f);
    const float bw = 2.f * S;
    hline(m, m + L, m, bw, cc), vline(m, m, m + L, bw, cc);
    hline(SW - m - L, SW - m, m, bw, cc), vline(SW - m, m, m + L, bw, cc);
    hline(m, m + L, SH - m, bw, cc), vline(m, SH - m - L, SH - m, bw, cc);
    hline(SW - m - L, SW - m, SH - m, bw, cc), vline(SW - m, SH - m - L, SH - m, bw, cc);

    // its own aim mark while the game's crosshair is put away: four short ticks round the middle, faint (not in a
    // replay, where the middle is a camera's, nor in the export's layer)
    if (C.hide_cross && C.aim_mark && !pm_replay::lua_active() && g_export == 0)
    {
        const float cx = std::floor(SW * 0.5f) + 0.5f, cy = std::floor(SH * 0.5f) + 0.5f, g0 = 7.f * S, g1 = 15.f * S, w = 1.5f * S;
        const u32 ac = rgba(C.col, a * 0.8f);
        hline(cx - g1, cx - g0, cy, w, ac), hline(cx + g0, cx + g1, cy, w, ac);
        vline(cx, cy - g1, cy - g0, w, ac), vline(cx, cy + g0, cy + g1, w, ac);
    }

    // the mode and the status line
    int hostile = 0, total = 0;
    for (const auto& [id, g] : g_targets)
        if (g.alive && g.t_dead < 0.f)
            ++total, hostile += g.hostile ? 1 : 0;
    LPCSTR mode = g_locked ? T("mode_lock", "MODE: TARGET LOCK") : hostile ? T("mode_threat", "MODE: THREAT") : T("mode_scan", "MODE: SCAN");
    text(110.f * S, 60.f * S, mode, F_LBL, rgba(C.col, a));
    float hp = 1.f, pw = 1.f;
    if (CActor* me = Actor())
    {
        hp = clampr(me->GetfHealth(), 0.f, 1.f);
        pw = clampr(me->conditions().GetPower(), 0.f, 1.f);
    }
    (void)hp;
    text(110.f * S, 90.f * S,
         fmt("%s %d%%   %s   %s: %d   %s: %d", T("power", "POWER"), iFloor(pw * 100.f + 0.5f), T("servo", "SERVO OK"), T("hostiles", "HOSTILE"), hostile,
             T("targets", "TARGETS"), total)
             .c_str(),
         F_SM, rgba(C.col, a * 0.75f));

    // the compass tape, top centre
    Fvector dir = Device.vCameraDirection;
    float yaw = rad2deg(std::atan2(dir.x, dir.z));
    if (yaw < 0.f)
        yaw += 360.f;
    const float pitch = rad2deg(std::asin(clampr(dir.y, -1.f, 1.f)));
    const float tcx = SW * 0.5f, ty = 44.f * S, span = 40.f, ppd = 9.f * S;
    for (int dg = int(std::floor(yaw - span)) - 1; dg <= int(std::ceil(yaw + span)) + 1; ++dg)
    {
        if (dg % 5)
            continue;
        const float x = tcx + (float(dg) - yaw) * ppd;
        const float fa = a * clampr(1.f - _abs(float(dg) - yaw) / span, 0.f, 1.f);
        if (fa <= 0.f)
            continue;
        const int d360 = ((dg % 360) + 360) % 360;
        const bool big = d360 % 15 == 0;
        vline(std::floor(x) + 0.5f, ty, ty + (big ? 12.f : 6.f) * S, 1.5f * S, rgba(C.col, fa * (big ? 0.75f : 0.45f)));
        if (big)
        {
            LPCSTR lbl = d360 == 0 ? T("north", "N") : d360 == 90 ? T("east", "E") : d360 == 180 ? T("south", "S") : d360 == 270 ? T("west", "W") : nullptr;
            text(x, ty + 15.f * S, lbl ? lbl : fmt("%d", d360).c_str(), lbl ? F_TAG : F_SM, rgba(C.col, fa * (lbl ? 0.9f : 0.6f)), 1);
        }
    }
    seg(tcx - 6.f * S, ty - 9.f * S, tcx, ty - 2.f * S, 2.f * S, rgba(C.col, a * 0.9f));
    seg(tcx + 6.f * S, ty - 9.f * S, tcx, ty - 2.f * S, 2.f * S, rgba(C.col, a * 0.9f));

    // the hex stream, right (the AE comp's)
    if (g_hex.empty())
    {
        std::mt19937 r(7); // the same stream every time
        for (int i = 0; i < 400; ++i)
        {
            string16 bits{};
            for (int b = 0; b < 8; ++b)
                bits[b] = (r() & 1) ? '1' : '0';
            g_hex.push_back(fmt("%04X %04X %s", u32(r() & 0xFFFF), u32(r() & 0xFFFF), bits));
        }
    }
    const int kk = int(g_now * 15.f);
    for (int i = 0; i < 14; ++i)
        text(SW - 280.f * S, 170.f * S + float(i) * 22.f * S, g_hex[size_t(kk + i * 7) % g_hex.size()].c_str(), F_SM,
             rgba(C.col, a * (0.3f + 0.3f * float(((i + kk) % 3) == 0))));

    // the data column, left edge: bits as bars running up
    const float sy0 = 180.f * S, sy1 = SH - 240.f * S, rh = 7.f * S;
    const float scroll = g_now * 46.f * S;
    for (int col = 0; col < 3; ++col)
    {
        const float x = 44.f * S + float(col) * 7.f * S;
        for (float y = sy0 - std::fmod(scroll, rh); y < sy1; y += rh)
        {
            if (y < sy0)
                continue;
            const float row = std::floor((y + scroll) / rh);
            if (hash1(row * 3.1f + float(col) * 17.f) < 0.45f)
                continue;
            const float edge = clampr(std::min(y - sy0, sy1 - y) / (60.f * S), 0.f, 1.f);
            rect(x, y, x + 4.f * S, y + rh - 2.f * S, rgba(C.col, a * 0.22f * edge));
        }
    }

    // the telemetry, bottom right
    const Fvector& cp = Device.vCameraPosition;
    const float rx = SW - 110.f * S;
    text(rx, SH - 150.f * S, fmt("%s %03.0f   %s %+03.0f", T("azimuth", "AZ"), yaw, T("pitch", "EL"), pitch).c_str(), F_SM, rgba(C.col, a * 0.7f), 2);
    text(rx, SH - 130.f * S, fmt("X %7.1f  Y %6.1f  Z %7.1f", cp.x, cp.y, cp.z).c_str(), F_SM, rgba(C.col, a * 0.6f), 2);
    const float tt = std::max(0.f, g_now);
    text(rx, SH - 110.f * S, fmt("T+ %02d:%02d.%d", int(tt / 60.f) % 100, int(tt) % 60, int(tt * 10.f) % 10).c_str(), F_SM, rgba(C.col, a * 0.6f), 2);

    // the damage panel, bottom left (the AE comp's): shown from the first hit, for 8 s after the last one
    const float since = g_now - g_hit_t;
    if (g_hits > 0 && since < 8.f)
    {
        const float pa = a * clampr((8.f - since) / 1.f, 0.f, 1.f) * clampr(since / 0.12f + 0.3f, 0.f, 1.f);
        const float bx0 = 101.f * S, by0 = 814.f * S, bx1 = 469.f * S, by1 = 937.f * S;
        const float blink = since < 0.5f && int(since / 0.08f) % 2 ? 0.4f : 1.f;
        const u32 pc = rgba(C.col, pa * blink);
        hline(bx0, bx1, by0, 2.f * S, pc), hline(bx0, bx1, by1, 2.f * S, pc);
        vline(bx0, by0, by1, 2.f * S, pc), vline(bx1, by0, by1, 2.f * S, pc);
        text(110.f * S, 819.f * S, T("dmg_title", "EXTERNAL DAMAGE"), F_LBL, pc);
        float suit = 0.f;
        if (CActor* me = Actor())
            if (CCustomOutfit* o = me->GetOutfit())
                suit = g_suit0 >= 0.f ? std::max(0.f, g_suit0 - o->GetCondition()) : 1.f - o->GetCondition();
        // the numbers count up to their value over 0.4 s after each hit
        const float k = ease(since / 0.4f);
        text(110.f * S, 849.f * S, fmt("%s ...... %4d", T("dmg_hits", "HITS"), g_hits).c_str(), F_LBL, pc);
        text(110.f * S, 877.f * S, fmt("%s ..... %4.1f%%", T("dmg_suit", "SUIT"), suit * 100.f * k).c_str(), F_LBL, pc);
        text(110.f * S, 905.f * S, fmt("%s ..... %4.2f%%", T("dmg_endo", "ENDOSKELETON"), std::min(g_dmg, 1.f) * 100.f * k).c_str(), F_LBL, pc);
        const float tx = 481.f * S, tyy = 851.f * S;
        seg(tx, tyy + 8.f * S, tx + 7.f * S, tyy + 4.f * S, 2.f * S, rgba(C.col, pa));
        seg(tx, tyy + 8.f * S, tx + 7.f * S, tyy + 12.f * S, 2.f * S, rgba(C.col, pa));
        LPCSTR sev = g_dmg < 0.05f ? T("dmg_minor", "MINOR") : g_dmg < 0.25f ? T("dmg_moderate", "MODERATE") : T("dmg_severe", "SEVERE");
        text(tx + 16.f * S, tyy, sev, F_TAG, rgba(C.col, pa));
        text(420.f * S, 911.f * S, g_dmg < 0.05f ? T("dmg_note_minor", "PAINT SCRATCHES") : T("dmg_note", "ARMOUR BREACHED"), F_SM, rgba(C.col, pa * 0.7f));
    }

    // the event log, bottom left (above the damage panel)
    float ly = 790.f * S - float(g_log.size()) * 20.f * S;
    for (const LogLine& l : g_log)
    {
        const float age = g_now - l.t;
        const float la = a * clampr(1.f - (age - 7.f) / 2.f, 0.f, 1.f) * 0.8f;
        if (la > 0.f)
            typed(110.f * S, ly, fmt("> %s", l.s.c_str()).c_str(), F_SM, rgba(C.col, la), l.t, 70.f);
        ly += 20.f * S;
    }
}

void draw_boot(float t)
{
    if (t < 0.f || t > 2.8f)
        return;
    const float a = clampr((2.8f - t) / 0.4f, 0.f, 1.f);
    typed(110.f * S, 170.f * S, T("boot_title", "VISUAL PROCESSOR: ONLINE"), F_BIG, rgba(C.col, a), 0.15f, 40.f);
    // (typed() measures from g_now; the boot runs on its own clock: shift it)
    static const LPCSTR ids[3] = {"boot_1", "boot_2", "boot_3"};
    static const LPCSTR defs[3] = {"OPTICS ........ OK", "THERMAL ....... OK", "TARGETING ..... OK"};
    for (int i = 0; i < 3; ++i)
        typed(110.f * S, 215.f * S + float(i) * 26.f * S, T(ids[i], defs[i]), F_LBL, rgba(C.col, a * 0.85f), 0.8f + float(i) * 0.35f, 60.f);
}

void reset_targets()
{
    g_targets.clear();
    g_lock = g_lock_try = u16(-1);
    g_locked = false;
    g_primary = u16(-1);
}

void load_switch()
{
    if (g_switch_level == g_pGameLevel)
        return;
    g_switch_level = g_pGameLevel;
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
        if (xr_strcmp(_Trim(line), "vision"))
            continue;
        LPSTR v = _Trim(eq + 1);
        g_switch = !xr_strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true");
    }
    fclose(f);
    Msg("* E76 vision: %s (My Mods)", g_switch ? "on" : "off");
}

// The game's own HUD is not the robot's: while the vision is up its minimap, bars, news and crosshair are put away
// (as in the AE takes, shot with no HUD), and what was shown comes back when it goes down. A script that shows the
// indicators again meanwhile is overruled until then.
CUI* g_hud_ui{};
bool g_hud_hidden{}, g_hud_had_ind{}, g_hud_had_cross{};
void sync_game_hud()
{
    CUI* ui = g_pGameLevel ? HUD().GetUI() : nullptr;
    if (ui != g_hud_ui)
    {
        g_hud_ui = ui; // a new level or a load: nothing of ours is hidden there
        g_hud_hidden = false;
    }
    if (!ui)
        return;
    const bool want = g_active && g_switch && (C.hide_hud || C.hide_cross);
    if (want)
    {
        if (!g_hud_hidden)
        {
            g_hud_had_ind = false;
            g_hud_had_cross = false;
            g_hud_hidden = true;
        }
        if (C.hide_hud && ui->GameIndicatorsShown())
        {
            ui->HideGameIndicators();
            ui->hud_disabled_by_user = true; // the news too
            g_hud_had_ind = true;
        }
        if (C.hide_cross && ui->CrosshairShown())
        {
            ui->HideCrosshair();
            g_hud_had_cross = true;
        }
    }
    else if (g_hud_hidden)
    {
        if (g_hud_had_ind && !ui->GameIndicatorsShown())
        {
            ui->ShowGameIndicators();
            ui->hud_disabled_by_user = false;
        }
        if (g_hud_had_cross && !ui->CrosshairShown())
            ui->ShowCrosshair();
        g_hud_hidden = false;
    }
}

float real_now()
{
    return std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void set_active(bool on)
{
    if (on && !g_switch)
        on = false;
    if (on == g_active)
        return;
    g_active = on;
    if (on)
    {
        load_cfg();
        g_boot = 0.f;
        g_glitch = 1.f;
        g_now = 0.f;
        g_log.clear();
        g_nums.clear();
        g_next_num = 1;
        reset_targets();
        log_add(T("log_boot", "MACHINE VISION ONLINE"));
        g_hits = 0;
        g_dmg = 0.f;
        g_hit_t = -100.f;
        g_suit0 = -1.f;
        if (CActor* me = Actor())
            if (CCustomOutfit* o = me->GetOutfit())
                g_suit0 = o->GetCondition();
    }
    else
        g_glitch = std::max(g_glitch, 0.6f);
    Msg("* E76 vision %s", on ? "on" : "off");
}
} // namespace

void before_render()
{
    portal_mod_vision_frame& V = portal_mod_vision;
    V.verts.clear();
    V.ghosts.clear();
    load_switch();
    const float rn = real_now();
    float rdt = g_real_prev > 0.f ? rn - g_real_prev : 0.f;
    g_real_prev = rn;
    rdt = clampr(rdt, 0.f, 0.1f);
    if (!g_switch)
        g_active = false;
    // E74's reboot owns the screen while it runs (its BIOS lines, its own red vision, its diagnostics): this layer steps
    // aside and the game's HUD is left as the reboot keeps it; the held zero health and the rise's are no damage; the
    // vision boots again when he is up
    const bool rebooting = portal_mod_reboot::active();
    if (rebooting != g_rebooting)
    {
        g_rebooting = rebooting;
        if (!rebooting && g_active)
        {
            g_boot = 0.f;
            g_glitch = 1.f;
            log_add(T("log_boot", "MACHINE VISION ONLINE"));
        }
    }
    if (rebooting)
    {
        g_fade = 0.f;
        if (CActor* me = Actor())
        {
            g_actor_hp = me->GetfHealth();
            g_suit_prev = me->GetOutfit() ? me->GetOutfit()->GetCondition() : -1.f;
        }
        V.on = false;
        return;
    }
    sync_game_hud();
    // the fade: up in 0.35 s, down in 0.3 s
    g_fade = g_active ? std::min(1.f, g_fade + rdt / 0.35f) : std::max(0.f, g_fade - rdt / 0.3f);
    if (!g_switch || (!g_active && g_fade <= 0.f) || !g_pGameLevel)
    {
        V.on = false;
        return;
    }
    // the clocks
    float dt = clampr(Device.fTimeDelta, 0.f, 0.1f);
    if (pm_replay::lua_active())
    {
        const float t = pm_replay::lua_time();
        dt = g_replay_t < 0.f ? 0.f : t - g_replay_t;
        if (dt < -0.02f || dt > 0.5f)
        {
            reset_targets();
            dt = 0.f;
        }
        dt = std::max(dt, 0.f);
        g_replay_t = t;
    }
    else
        g_replay_t = -1.f;
    g_now += dt;
    if (g_boot >= 0.f)
        g_boot += rdt;
    SW = float(Device.dwWidth);
    SH = float(Device.dwHeight);
    S = SH / 1080.f;

    // a hit on the robot: a glitch and a red pulse. The T-800's armour (pm_armor.script) takes a bullet down to ~0.01 %
    // of health and wears the suit instead, so a hit is a drop of either: the health (the endoskeleton) or the suit's
    // condition
    if (CActor* me = Actor())
    {
        const float hp = me->GetfHealth();
        CCustomOutfit* suit = me->GetOutfit();
        const float sc = suit ? suit->GetCondition() : -1.f;
        // more than half the health in one frame is no round on the armour: a jump of the game's time (hunger) or the like
        const bool jump = g_actor_hp >= 0.f && hp < g_actor_hp - 0.5f;
        const bool hp_hit = g_actor_hp >= 0.f && hp < g_actor_hp - 0.002f && !jump;
        const bool suit_hit = sc >= 0.f && g_suit_prev >= 0.f && sc < g_suit_prev - 0.0004f;
        if ((hp_hit || suit_hit) && !pm_replay::lua_active())
        {
            const float drop = g_actor_hp >= 0.f && hp < g_actor_hp ? g_actor_hp - hp : 0.f;
            g_glitch = std::max(g_glitch, clampr((hp_hit ? 0.35f : 0.22f) + drop * 6.f, 0.f, 1.f));
            g_hit = hp_hit ? 1.f : std::max(g_hit, 0.45f);
            if (hp_hit)
                log_add(fmt("%s -%.1f%%", T("log_hit", "HULL DAMAGE"), drop * 100.f));
            else if (g_now - g_hit_t > 1.5f)
                log_add(T("log_armor", "ARMOUR HIT"));
            if (g_active)
            {
                ++g_hits;
                g_dmg += drop;
                g_hit_t = g_now;
            }
        }
        else if (g_active && g_actor_hp >= 0.f && hp < g_actor_hp && !jump && !pm_replay::lua_active())
            g_dmg += g_actor_hp - hp; // the armour's crumbs of a bullet, a bleed: the endoskeleton's count
        g_actor_hp = hp;
        g_suit_prev = sc;
    }
    g_glitch = std::max(0.f, g_glitch - rdt * 2.6f);
    g_hit = std::max(0.f, g_hit - rdt * 3.5f);
    if (g_boot >= 0.f && g_boot < 0.6f)
        g_glitch = std::max(g_glitch, 0.9f * (1.f - g_boot / 0.6f));
    if (g_frame % 3 == 0 || g_glitch > 0.5f)
        g_seed = float(::Random.randI(10000)) * 0.01f;

    if (g_active)
    {
        update_targets(dt);
        update_lock();
        // the priority: the locked one, else the biggest threat in sight
        u16 pr = g_lock;
        if (pr == u16(-1))
        {
            float bt = -1.f;
            for (const auto& [id, g] : g_targets)
                if (g.alive && g.visible && !g.ghost && g.t_dead < 0.f && g.dist < C.card_range && g.threat > bt)
                    bt = g.threat, pr = id;
        }
        if (pr != g_primary)
        {
            g_primary = pr;
            g_primary_t = g_now;
        }
    }

    // the HUD (crisp, over the grade; it comes on with the boot's flicker)
    float a = g_fade;
    if (g_boot >= 0.f && g_boot < 0.3f && (int(g_boot * 30.f) % 2 == 1))
        a *= 0.35f;
    load_cfg();
    if (g_active || g_fade > 0.f)
    {
        for (const auto& [id, g] : g_targets)
        {
            draw_target(g, a);
            if (g.ghost && g.alive)
                if (CObject* O = Level().Objects.net_Find(id))
                    V.ghosts.push_back(O);
        }
        if (pm_replay::lua_active())
        {
            // a replay's ghosts are its puppets
            static xr_vector<pm_replay::VisionPuppet> pp;
            pm_replay::vision_puppets(pp);
            V.ghosts.clear();
            for (const auto& p : pp)
                if (p.visual.size() && strstr(p.visual.c_str(), "xray") && g_targets.count(p.id))
                    V.ghosts.push_back(p.R);
        }
        draw_lock(a);
        draw_chrome(a);
        {
            // the boot lines run on the real clock: typed() reads g_now, so it is swapped for them
            const float keep = g_now;
            g_now = g_boot;
            draw_boot(g_boot);
            g_now = keep;
        }
    }

    V.on = true;
    V.fade = g_fade;
    V.glitch = g_glitch;
    V.seed = g_seed;
    V.time = g_now;
    V.hit = g_hit;
    V.hud_only = g_export == 1 ? 1 : 0;
    V.no_hud = g_export == 2 ? 1 : 0;
}

bool key_press(int dik)
{
    if (!g_switch || !g_pGameLevel)
        return false;
    load_cfg();
    if (dik != C.key)
        return false;
    if (Console && Console->bVisible)
        return false;
    if (!pm_replay::lua_active() && (Device.Paused() || (HUD().GetUI() && HUD().GetUI()->MainInputReceiver())))
        return false;
    set_active(!g_active);
    return true;
}

//------------------------------------------------------------------------------------------------------------------
// Lua (the bot's API)
namespace
{
void api_switch(bool on)
{
    g_switch_level = g_pGameLevel;
    if (g_switch != on)
        Msg("* E76 vision switch %s", on ? "on" : "off");
    g_switch = on;
    if (!on)
        set_active(false);
}
bool api_switched() { return g_switch; }
void api_on(bool on) { set_active(on); }
void api_toggle() { set_active(!g_active); }
bool api_active() { return g_active; }
void api_export(int mode) { g_export = u32(clampr(mode, 0, 2)); }

luabind::object api_targets()
{
    lua_State* L = ai().script_engine().lua();
    luabind::object t = luabind::newtable(L);
    int k = 1;
    for (const auto& [id, g] : g_targets)
    {
        if (g.t_dead >= 0.f)
            continue;
        luabind::object e = luabind::newtable(L);
        e["id"] = g.id;
        e["num"] = g.num;
        e["name"] = g.name.c_str();
        e["faction"] = g.faction.c_str();
        e["rank"] = g.rank.c_str();
        e["weapon"] = g.weapon.c_str();
        e["health"] = g.health;
        e["dist"] = g.dist;
        e["threat"] = g.threat;
        e["hostile"] = g.hostile;
        e["aiming"] = g.aiming;
        e["visible"] = g.visible;
        e["monster"] = g.monster;
        e["ghost"] = g.ghost;
        e["locked"] = g_locked && g_lock == id;
        e["primary"] = g_primary == id;
        e["x"] = (g.box[0] + g.box[2]) * 0.5f / std::max(1.f, SW);
        e["y"] = (g.box[1] + g.box[3]) * 0.5f / std::max(1.f, SH);
        t[k++] = e;
    }
    return t;
}

// the same as text, one target a line, "key=value" pairs split by tabs: a Lua table made here lands on the main Lua
// thread's stack (luabind pushes an object on its own state), so a script running in a coroutine gets nil from
// vision_targets() - scripts\pm_vision.script's targets() builds the table from this instead
LPCSTR api_targets_text()
{
    static xr_string out;
    out.clear();
    for (const auto& [id, g] : g_targets)
    {
        if (g.t_dead >= 0.f)
            continue;
        out += fmt("id=%u\tnum=%d\tname=%s\tfaction=%s\trank=%s\tweapon=%s\thealth=%.3f\tdist=%.2f\tthreat=%.3f\thostile=%d\taiming=%d\tvisible=%d\t"
                   "monster=%d\tghost=%d\tlocked=%d\tprimary=%d\tx=%.4f\ty=%.4f\n",
                   u32(g.id), g.num, g.name.c_str(), g.faction.c_str(), g.rank.c_str(), g.weapon.c_str(), g.health, g.dist, g.threat, g.hostile ? 1 : 0,
                   g.aiming ? 1 : 0, g.visible ? 1 : 0, g.monster ? 1 : 0, g.ghost ? 1 : 0, (g_locked && g_lock == id) ? 1 : 0, g_primary == id ? 1 : 0,
                   (g.box[0] + g.box[2]) * 0.5f / std::max(1.f, SW), (g.box[1] + g.box[3]) * 0.5f / std::max(1.f, SH));
    }
    return out.c_str();
}

LPCSTR api_info()
{
    static string512 s;
    int vis = 0, ghosts = 0;
    for (const auto& [id, g] : g_targets)
        vis += g.visible ? 1 : 0, ghosts += g.ghost ? 1 : 0;
    xr_sprintf(s, "switch=%d on=%d fade=%.2f targets=%u visible=%d ghosts=%d lock=%d locked=%d primary=%d glitch=%.2f verts=%u export=%u",
               g_switch ? 1 : 0, g_active ? 1 : 0, g_fade, u32(g_targets.size()), vis, ghosts, g_lock == u16(-1) ? -1 : int(g_lock), g_locked ? 1 : 0,
               g_primary == u16(-1) ? -1 : int(g_primary), g_glitch, u32(portal_mod_vision.verts.size()), g_export);
    return s;
}

void api_tune(LPCSTR name, float v)
{
    portal_mod_vision_frame& V = portal_mod_vision;
    if (!xr_strcmp(name, "heat"))
        V.heat = v;
    else if (!xr_strcmp(name, "scan_lines"))
        V.scan = v;
    else if (!xr_strcmp(name, "grain"))
        V.grain = v;
    else if (!xr_strcmp(name, "aberration"))
        V.ca = v;
    else if (!xr_strcmp(name, "vignette"))
        V.vignette = v;
    else if (!xr_strcmp(name, "glow"))
        V.glow = v;
    else if (!xr_strcmp(name, "tint"))
        V.tint = v;
    else if (!xr_strcmp(name, "range"))
        C.range = v;
    else if (!xr_strcmp(name, "glitch"))
        g_glitch = v;
    else
        Msg("! E76 vision_tune: unknown %s", name);
}

void api_reload()
{
    g_cfg_level = nullptr;
    load_cfg();
}
} // namespace
} // namespace pm_vision

void pm_vision_script_register(lua_State* L)
{
    using namespace pm_vision;
    module(L, "level")[def("vision_switch", &api_switch), def("vision_switched", &api_switched), def("vision_on", &api_on), def("vision_toggle", &api_toggle),
                       def("vision_active", &api_active), def("vision_targets", &api_targets), def("vision_targets_text", &api_targets_text), def("vision_info", &api_info), def("vision_tune", &api_tune),
                       def("vision_export", &api_export), def("vision_reload", &api_reload)];
}
