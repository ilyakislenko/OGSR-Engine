// portal-mod E57: breakable physics props (see portal_mod_props.h; tools/props_build.py writes the data).
//
// meshes\portal_mod\props\props.bin: for each prop model it cut (its visual, "physics\box\box_1a"), its game material,
// material class (wood, metal, glass, plastic, cloth, stone), mass and size, and its pieces - each one's centre in the
// prop's frame, its box (axes, half sizes), mass and surface area; the pieces' models are <category>\<name>\<k>.ogf,
// framed at the piece's centre in the prop's axes.
//
// A CPhysicObject drawn with such a model takes damage from every hit (CPhysicObject::Hit): the hit's power times
// [pm_props] k_<type>; it breaks once the damage adds up to hp_<material> per m of its size (at least hp_min). Breaking
// is queued to the next frame: the object's scripts get its death (a loot box spawns its items), the object hides and is
// destroyed (released from the game for good), and each piece becomes a body of its own - one box, its mass, the prop's
// game material - moving with the prop and pushed away from the hit (more the nearer to it), spinning. They clatter on
// landing (a sound by material when one's speed jumps), lie frag_life seconds, then sink into the ground and go; past
// max_frags the oldest lying go first. Wood and stone throw splinters where rounds hit (portal_mod_destruct's chips).
// A blast throws a prop it does not break; such a prop, while it flies, breaks on a hard landing (imp_dv_<material>).
//
// Not taken: objects with scripted logic (doors, buttons, story things - but loot boxes are), a story id, models with
// the game's own broken parts (CDestroyablePhysicsObject that can destroy), and the models props_build left whole.
#include "stdafx.h"
#include <algorithm>
#include "portal_mod_props.h"
#include "portal_mod_destruct.h"
#include "portal_mod_features.h" // portal-mod E55: My Mods' switches
#include "PhysicObject.h"
#include "PHCollisionDamageReceiver.h"
#include "PHDestroyable.h"
#include "hit_immunity.h"
#include "damage_manager.h"
#include "DestroyablePhysicsObject.h"
#include "PhysicsShell.h"
#include "PhysicsCommon.h"
#include "PHWorld.h"
#include "Level.h"
#include "ParticlesObject.h"
#include "GamePersistent.h"
#include "Hit.h"
#include "game_object_space.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "../xr_3da/GameMtlLib.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/RenderVisual.h"

bool portal_mod_ps_exists(LPCSTR name); // xrGame/portal_mod_destruct.cpp's renderer side (E54)
int portal_mod_sun_cascade(u32 context_id); // which sun cascade a render context draws (-1: none)
bool portal_mod_ctx_smap(u32 context_id); // a render context drawing a shadow map

namespace portal_mod_props
{
namespace
{
enum : u8
{
    M_WOOD,
    M_METAL,
    M_GLASS,
    M_PLASTIC,
    M_CLOTH,
    M_STONE,
    NMAT
};
LPCSTR const MAT_KEY[NMAT] = {"wood", "metal", "glass", "plastic", "cloth", "stone"}; // tools/props_build.py MATS
LPCSTR const MAT_GMTL[NMAT] = {"objects\\small_box", "objects\\small_metal_trash", "objects\\glass", "objects\\small_box",
                               "objects\\clothes", "objects\\concrete_box"}; // a prop with no usable game material of its own

struct Cfg
{
    bool enabled{true};
    float hp[NMAT]{65.f, 250.f, 12.f, 35.f, 160.f, 300.f}; // per m of the prop's size (its box's diagonal)
    float hp_min{6.f}, size_min{0.25f}, size_max{3.f};
    float k_fire{100.f}, k_blast{0.f}, k_strike{60.f}, k_burn{0.f}; // damage per unit of a hit's power, by its type
    float fire[NMAT]{1.f, 0.1f, 1.f, 1.f, 0.3f, 0.3f}; // ... a round's, times this by material (a barrel takes holes, not apart)
    // a blast (CExplosive::Explode, through portal_mod_destruct::explosion): blast_k x its hit x (1 - d / its radius)^blast_pow
    // on every prop in reach (round tables and crates as a blast wave goes; the game's own blast hits are k_blast)
    float blast_k{100.f}, blast_pow{1.5f};
    float blast_cover{0.f}; // the share a prop with the level between it and the blast at all three points still takes
    float keep_s{600.f}; // s a prop keeps its damage without another hit
    float push_k{1.f}, push_min{1.2f}, push_max{14.f}; // m/s: the pieces' push, the hit's impulse over the prop's mass
    float spin{8.f}; // rad/s at most, a small piece's (a heavy one less)
    float imp_dv[NMAT]{9.f, 30.f, 3.5f, 12.f, 40.f, 14.f}; // m/s: a thrown prop breaks landing this hard
    float imp_k{20.f}; // ... its damage per kg (m/s)^2 past that
    float watch_s{8.f}; // s a hit prop's landings are watched
    bool debug{}; // a line for each hit on a prop (level.props_debug)
    float frag_life{240.f}, frag_sink{2.5f}; // s lying, s sinking
    float frag_mass_min{0.25f}, frag_half_min{0.015f}, frag_margin{0.003f};
    int max_frags{260};
    float clatter_dv{2.5f}; // m/s: a piece's speed jumping by this plays its material's knock
    int max_clatter{3}; // knocks a frame at most
    float chips_k{0.12f}; // splinters off wood and stone: a round's damage times this, in portal_mod_destruct's units
    int max_fx{4}; // particle systems a frame at most
    xr_vector<shared_str> fx[NMAT], snd[NMAT], knock[NMAT];
};
Cfg C;

struct PieceDef
{
    Fvector c, h;
    Fmatrix33 R;
    float mass, area;
};

struct PropDef
{
    shared_str name, gmtl, dir; // dir: its pieces' folder under meshes\portal_mod\props (tools/props_build.py piece_dir)
    u8 mat{};
    float mass{}, size{};
    u32 first{}, count{};
};

struct Defs
{
    bool tried{}, ok{};
    xr_map<shared_str, u32> index;
    xr_vector<PropDef> props;
    xr_vector<PieceDef> pieces;
};
Defs D;

// a piece flying or lying: its body and its model
struct Frag : public ISpatial, public IRenderable
{
    CPhysicsShell* shell{};
    bool in_space{}, resting{};
    u8 mat{};
    float t{}, life{}, sink{}, r{};
    Fvector v_prev{};
    Fmatrix xf{};
    explicit Frag(LPCSTR vis) : ISpatial(g_SpatialSpace)
    {
        renderable.visual = ::Render->model_Create(vis);
        renderable.xform.identity();
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY;
        r = renderable.visual ? renderable.visual->getVisData().sphere.R : 0.f;
    }
    ~Frag() override
    {
        if (in_space)
            spatial_unregister();
        if (renderable.visual)
            ::Render->model_Delete(renderable.visual);
        if (shell)
            destroy_physics_shell(shell);
    }
    void place(const Fmatrix& m)
    {
        if (!renderable.visual || !_valid(m))
            return;
        xf = m;
        renderable.xform.set(m);
        const vis_data& v = renderable.visual->getVisData();
        Fvector P;
        m.transform_tiny(P, v.sphere.P);
        spatial.sphere.set(P, _max(v.sphere.R, 0.05f));
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
        if (!renderable.visual)
            return;
        if (r < 0.08f && portal_mod_ctx_smap(context_id)) // a splinter's shadow is a dot
            return;
        const int cas = portal_mod_sun_cascade(context_id);
        if (cas >= 1 && r < 0.3f * cas) // nor in the far sun cascades
            return;
        ::Render->add_Visual(context_id, this, renderable.visual, renderable.xform);
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

// a prop taking hits: its damage so far, the last hit; queued once it breaks
struct Track
{
    CObject* obj{};
    u32 def{};
    float dmg{}, hp{};
    u32 last{}; // Device.dwTimeGlobal of the last hit
    Fvector point{}, dir{0.f, -1.f, 0.f}, v_prev{};
    float impulse{};
    u16 who{u16(-1)};
    bool queued{}, has_prev{};
    LPCSTR cause{"?"}; // what dealt the blow that broke it (the log line)
    int type{-1};      // its hit type
};

struct State
{
    bool cfg{};
    xr_map<u16, Track> tracks;
    xr_vector<Frag*> frags;
    xr_vector<ref_sound> snd[NMAT], knock[NMAT];
    bool sounds{};
    u32 fx_frame{}, fx_count{}, knock_count{};
    u32 broken{}, hits{}, frags_made{};
};
State S;

float cfg_f(LPCSTR s, LPCSTR k, float def) { return pSettings->line_exist(s, k) ? pSettings->r_float(s, k) : def; }

void cfg_list(LPCSTR s, LPCSTR k, xr_vector<shared_str>& out, LPCSTR def, bool fx)
{
    out.clear();
    LPCSTR v = pSettings->line_exist(s, k) ? pSettings->r_string(s, k) : def;
    if (!v || !*v)
        return;
    string256 one;
    for (int i = 0, n = _GetItemCount(v); i < n; ++i)
    {
        _GetItem(v, i, one);
        LPCSTR name = _Trim(one);
        if (fx && !portal_mod_ps_exists(name))
        {
            Msg("! [props] [pm_props] %s: no particle effect or group '%s' - skipped", k, name);
            continue;
        }
        out.emplace_back(name);
    }
}

void load_cfg()
{
    if (S.cfg)
        return;
    S.cfg = true;
    LPCSTR s = "pm_props";
    if (!pSettings->section_exist(s))
        return;
    C.enabled = pSettings->line_exist(s, "enabled") ? !!pSettings->r_bool(s, "enabled") : true;
    C.debug = pSettings->line_exist(s, "debug") && !!pSettings->r_bool(s, "debug");
    string64 k;
    static LPCSTR const FX_DEF[NMAT] = {"destroy_fx\\destroy_wood_00", "hit_fx\\hit_metal_02", "explosions\\explosion_bottle",
                                        "destroy_fx\\destroy_small_box_00", "destroy_fx\\destroy_small_box_00", "hit_fx\\hit_stucco_home_explode"};
    static LPCSTR const SND_DEF[NMAT] = {"material\\wood\\wood_crash1_hl2", "material\\metal_box\\collide\\big_metal_1",
                                         "material\\bottle\\collide\\bottle_break_1", "material\\large_furniture\\large_furniture_2",
                                         "material\\human\\collide\\clothes1", "ambient\\rnd_outdoor\\rnd_rock1"};
    static LPCSTR const KNOCK_DEF[NMAT] = {"material\\wood\\wood_hit1_hl2", "material\\barrel\\collide\\hithard01hl", "material\\glass\\glass_fall01hl",
                                           "material\\barrel\\collide\\barrel_1", "material\\human\\collide\\clothes2", "ambient\\rnd_outdoor\\rnd_rock2"};
    for (int m = 0; m < NMAT; ++m)
    {
        xr_sprintf(k, "hp_%s", MAT_KEY[m]);
        C.hp[m] = cfg_f(s, k, C.hp[m]);
        xr_sprintf(k, "fire_%s", MAT_KEY[m]);
        C.fire[m] = cfg_f(s, k, C.fire[m]);
        xr_sprintf(k, "imp_dv_%s", MAT_KEY[m]);
        C.imp_dv[m] = cfg_f(s, k, C.imp_dv[m]);
        xr_sprintf(k, "fx_%s", MAT_KEY[m]);
        cfg_list(s, k, C.fx[m], FX_DEF[m], true);
        xr_sprintf(k, "snd_%s", MAT_KEY[m]);
        cfg_list(s, k, C.snd[m], SND_DEF[m], false);
        xr_sprintf(k, "knock_%s", MAT_KEY[m]);
        cfg_list(s, k, C.knock[m], KNOCK_DEF[m], false);
    }
    C.hp_min = cfg_f(s, "hp_min", C.hp_min);
    C.size_min = cfg_f(s, "size_min", C.size_min);
    C.size_max = cfg_f(s, "size_max", C.size_max);
    C.k_fire = cfg_f(s, "k_fire", C.k_fire);
    C.k_blast = cfg_f(s, "k_blast", C.k_blast);
    C.k_strike = cfg_f(s, "k_strike", C.k_strike);
    C.k_burn = cfg_f(s, "k_burn", C.k_burn);
    C.push_k = cfg_f(s, "push_k", C.push_k);
    C.push_min = cfg_f(s, "push_min", C.push_min);
    C.push_max = cfg_f(s, "push_max", C.push_max);
    C.spin = cfg_f(s, "spin", C.spin);
    C.imp_k = cfg_f(s, "imp_k", C.imp_k);
    C.watch_s = cfg_f(s, "watch_s", C.watch_s);
    C.blast_k = cfg_f(s, "blast_k", C.blast_k);
    C.blast_pow = cfg_f(s, "blast_pow", C.blast_pow);
    C.blast_cover = cfg_f(s, "blast_cover", C.blast_cover);
    C.keep_s = cfg_f(s, "keep_s", C.keep_s);
    C.frag_life = cfg_f(s, "frag_life", C.frag_life);
    C.frag_sink = cfg_f(s, "frag_sink", C.frag_sink);
    C.frag_mass_min = cfg_f(s, "frag_mass_min", C.frag_mass_min);
    C.frag_half_min = cfg_f(s, "frag_half_min", C.frag_half_min);
    C.max_frags = (int)cfg_f(s, "max_frags", (float)C.max_frags);
    C.clatter_dv = cfg_f(s, "clatter_dv", C.clatter_dv);
    C.max_clatter = (int)cfg_f(s, "max_clatter", (float)C.max_clatter);
    C.chips_k = cfg_f(s, "chips_k", C.chips_k);
    C.max_fx = (int)cfg_f(s, "max_fx", (float)C.max_fx);
}

void make_sounds()
{
    if (S.sounds)
        return;
    S.sounds = true;
    for (int m = 0; m < NMAT; ++m)
    {
        S.snd[m].resize(C.snd[m].size());
        for (u32 i = 0; i < C.snd[m].size(); ++i)
            S.snd[m][i].create(C.snd[m][i].c_str(), st_Effect, sg_SourceType);
        S.knock[m].resize(C.knock[m].size());
        for (u32 i = 0; i < C.knock[m].size(); ++i)
            S.knock[m][i].create(C.knock[m][i].c_str(), st_Effect, sg_SourceType);
    }
}

void load_defs()
{
    if (D.tried)
        return;
    D.tried = true;
    string_path fn;
    if (!FS.exist(fn, "$game_meshes$", "portal_mod\\props\\props.bin"))
    {
        Msg("! [props] meshes\\portal_mod\\props\\props.bin is missing (tools/props_build.py): no prop breaks");
        return;
    }
    IReader* r = FS.r_open(fn);
    if (!r)
        return;
    char magic[4]{};
    if (r->length() >= 12)
        r->r(magic, 4);
    const u32 ver = strncmp(magic, "PMPR", 4) == 0 ? r->r_u32() : 0;
    if (ver != 2)
    {
        Msg("! [props] %s: not a props.bin of version 2 - no prop breaks", fn);
        FS.r_close(r);
        return;
    }
    const u32 n = r->r_u32();
    D.props.reserve(n);
    for (u32 i = 0; i < n && r->elapsed() > 0; ++i)
    {
        PropDef p;
        xr_string name, gmtl;
        r->r_stringZ(name);
        r->r_stringZ(gmtl);
        p.name = name.c_str();
        p.gmtl = gmtl.c_str();
        // physics\\box\\box_1a -> box\\box_1a, objects\\dynamics\\box\\x -> dynamics\\box\\x
        if (!strncmp(name.c_str(), "physics\\", 8))
            p.dir = name.c_str() + 8;
        else if (!strncmp(name.c_str(), "objects\\dynamics\\", 17))
            p.dir = (xr_string("dynamics\\") + (name.c_str() + 17)).c_str();
        else
            p.dir = name.c_str();
        p.mat = r->r_u8();
        p.count = r->r_u8();
        p.mass = r->r_float();
        p.size = r->r_float();
        p.first = (u32)D.pieces.size();
        if (p.mat >= NMAT || (u32)r->elapsed() < p.count * 17 * 4)
            break;
        for (u32 k = 0; k < p.count; ++k)
        {
            PieceDef pc;
            r->r_fvector3(pc.c);
            r->r_fvector3(pc.R.i);
            r->r_fvector3(pc.R.j);
            r->r_fvector3(pc.R.k);
            r->r_fvector3(pc.h);
            pc.mass = r->r_float();
            pc.area = r->r_float();
            D.pieces.push_back(pc);
        }
        if (GMLib.GetMaterialIdx(p.gmtl.c_str()) == GAMEMTL_NONE_IDX)
            p.gmtl = MAT_GMTL[p.mat];
        D.index[p.name] = (u32)D.props.size();
        D.props.push_back(p);
    }
    FS.r_close(r);
    D.ok = !D.props.empty();
    Msg("* [props] %u breakable prop models, %u pieces", (u32)D.props.size(), (u32)D.pieces.size());
}

int def_of(CObject* o)
{
    load_defs();
    if (!D.ok || !o || !o->cNameVisual().size())
        return -1;
    string_path low;
    xr_strcpy(low, o->cNameVisual().c_str());
    xr_strlwr(low);
    const size_t n = xr_strlen(low);
    if (n > 4 && !xr_strcmp(low + n - 4, ".ogf"))
        low[n - 4] = 0;
    const auto it = D.index.find(shared_str(low));
    return it == D.index.end() ? -1 : (int)it->second;
}

// why an object of a cut model does not break (nullptr: it does)
LPCSTR not_breakable(CPhysicObject* o)
{
    if (o->getDestroy() || !o->getEnabled())
        return "going"; // broken (disabled at once, destroyed a few frames later)
    if (o->story_id() != INVALID_STORY_ID)
        return "story";
    CInifile* ini = o->spawn_ini();
    if (ini && ini->section_exist("logic") && !ini->section_exist("drop_box"))
        return "logic";
    if (!o->PPhysicsShell())
        return "no shell";
    CDestroyablePhysicsObject* d = smart_cast<CDestroyablePhysicsObject*>(o);
    if (d && d->CanDestroy())
        return "own parts";
    return nullptr;
}

float prop_hp(const PropDef& p) { return _max(C.hp_min, C.hp[p.mat] * clampr(p.size, C.size_min, C.size_max)); }

bool fx_allowed()
{
    if (S.fx_frame != Device.dwFrame)
    {
        S.fx_frame = Device.dwFrame;
        S.fx_count = 0;
        S.knock_count = 0;
    }
    return S.fx_count++ < (u32)C.max_fx;
}

void play_fx(const xr_vector<shared_str>& names, const Fvector& pos, const Fvector& dir)
{
    if (names.empty() || !fx_allowed() || pos.distance_to_sqr(Device.vCameraPosition) > 200.f * 200.f)
        return;
    LPCSTR name = names[::Random.randI(names.size())].c_str();
    Fmatrix m;
    m.identity();
    Fvector d = dir;
    if (d.square_magnitude() < EPS)
        d.set(0.f, 1.f, 0.f);
    d.normalize();
    m.k.set(d);
    Fvector::generate_orthonormal_basis(m.k, m.j, m.i);
    m.c.set(pos);
    portal_mod_destruct::fx_at(name, m); // E57: a looped system too
}

void play_snd(xr_vector<ref_sound>& snd, const Fvector& pos, float vol)
{
    if (snd.empty())
        return;
    ref_sound& s = snd[::Random.randI(snd.size())];
    if (!s._handle())
        return;
    Fvector p = pos;
    float v = vol;
    s.play_no_feedback(nullptr, 0, 0.f, &p, &v);
}

void kill_frag(u32 i)
{
    xr_delete(S.frags[i]);
    S.frags[i] = S.frags.back();
    S.frags.pop_back();
}

// the oldest lying pieces start sinking until no more than max_frags - n are left
void make_room(int n)
{
    int over = (int)S.frags.size() + n - C.max_frags;
    if (over <= 0)
        return;
    xr_vector<std::pair<float, u32>> age;
    for (u32 i = 0; i < S.frags.size(); ++i)
        if (S.frags[i]->sink <= 0.f)
            age.emplace_back(S.frags[i]->t + (S.frags[i]->resting ? 1e4f : 0.f), i);
    std::sort(age.begin(), age.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& a : age)
    {
        if (over-- <= 0)
            break;
        S.frags[a.second]->life = 0.f; // sinks from the next frame
    }
}

void spawn_piece(const PropDef& p, u32 k, const Fmatrix& X, const Fvector& v, const Fvector& w, const Fvector& point, const Fvector& dir, float push)
{
    const PieceDef& pc = D.pieces[p.first + k];
    string_path vis;
    xr_sprintf(vis, "portal_mod\\props\\%s\\%u", p.dir.c_str(), k);
    Frag* f = xr_new<Frag>(vis);
    if (!f->renderable.visual)
    {
        xr_delete(f);
        return;
    }
    Fmatrix F = X;
    X.transform_tiny(F.c, pc.c);
    // its velocity: the prop's at that point, pushed away from the hit (along it, more the nearer), and a spin
    Fvector r;
    r.sub(F.c, X.c);
    Fvector vk;
    vk.crossproduct(w, r).add(v);
    Fvector away;
    away.sub(F.c, point);
    const float d = away.magnitude();
    if (d > EPS_L)
        away.div(d);
    else
        away.set(dir);
    Fvector pd;
    pd.mad(away, dir, 0.8f).normalize_safe();
    const float near_k = 0.35f + 0.65f * expf(-d / _max(0.15f, p.size * 0.35f));
    vk.mad(pd, push * near_k * ::Random.randF(0.7f, 1.2f));
    Fvector wk;
    wk.random_dir();
    wk.mul(C.spin * ::Random.randF(0.3f, 1.f) / (1.f + pc.mass * 0.2f)).add(w);
    // its body: one box
    CPhysicsElement* E = P_create_Element();
    Fobb o;
    o.m_rotate.set(pc.R);
    o.m_translate.set(0.f, 0.f, 0.f);
    o.m_halfsize.set(_max(C.frag_half_min, pc.h.x - C.frag_margin), _max(C.frag_half_min, pc.h.y - C.frag_margin),
                     _max(C.frag_half_min, pc.h.z - C.frag_margin));
    E->add_Box(o);
    f->shell = P_create_Shell();
    f->shell->add_Element(E);
    f->shell->setMass(_max(pc.mass, C.frag_mass_min));
    f->shell->SetMaterial(p.gmtl.c_str());
    f->shell->Activate(F, vk, wk, false);
    f->mat = p.mat;
    f->life = C.frag_life * ::Random.randF(0.8f, 1.2f);
    f->v_prev = vk;
    f->place(F);
    S.frags.push_back(f);
    ++S.frags_made;
}

void break_prop(CPhysicObject* o, Track& t)
{
    const PropDef& p = D.props[t.def];
    CPhysicsShell* sh = o->PPhysicsShell();
    const Fmatrix X = o->XFORM();
    Fvector v{}, w{};
    if (sh && sh->isActive())
    {
        sh->get_LinearVel(v);
        sh->get_AngularVel(w);
    }
    // its scripts first: a loot box drops its items (bind_physic_object.script's death callback)
    CObject* who = t.who != u16(-1) ? Level().Objects.net_Find(t.who) : nullptr;
    CGameObject* gwho = smart_cast<CGameObject*>(who);
    Msg("* [props] %s (%u) breaks: %.0f of %.0f by %s (type %d) from %s at %.1f m", o->cNameVisual().c_str(), o->ID(), t.dmg, t.hp, t.cause, t.type,
        who ? who->cName().c_str() : "-", o->Position().distance_to(Device.vCameraPosition));
    o->callback(GameObject::eDeath)(o->lua_game_object(), gwho ? gwho->lua_game_object() : nullptr);
    if (sh)
    {
        sh->Disable();
        sh->DisableCollision();
    }
    o->setVisible(FALSE);
    o->setEnabled(FALSE);
    o->DestroyObject();
    // the pieces
    const float push = clampr(t.impulse * C.push_k / _max(p.mass, 1.f), C.push_min, C.push_max);
    make_room((int)p.count);
    for (u32 k = 0; k < p.count; ++k)
        spawn_piece(p, k, X, v, w, t.point, t.dir, push);
    Fvector c;
    X.transform_tiny(c, o->Visual() ? o->Visual()->getVisData().sphere.P : Fvector{});
    Fvector up{0.f, 1.f, 0.f};
    play_fx(C.fx[p.mat], c, up);
    make_sounds();
    play_snd(S.snd[p.mat], c, _min(1.f, 0.5f + p.size * 0.4f));
    ++S.broken;
}

float hit_k(ALife::EHitType type)
{
    switch (type)
    {
    case ALife::eHitTypeFireWound: return C.k_fire;
    case ALife::eHitTypeExplosion: return C.k_blast;
    case ALife::eHitTypeStrike:
    case ALife::eHitTypeWound:
    case ALife::eHitTypeWound_2:
    case ALife::eHitTypePhysicStrike: return C.k_strike;
    case ALife::eHitTypeBurn: return C.k_burn;
    default: return 0.f;
    }
}

void damage(CPhysicObject* o, Track& t, float dmg, LPCSTR cause, int type = -1)
{
    t.dmg += dmg;
    t.cause = cause;
    t.type = type;
    if (t.dmg >= t.hp && !t.queued)
        t.queued = true; // broken next frame (not inside the hit's event, maybe inside the physics step)
}

void update_frags(float dt)
{
    for (u32 i = 0; i < S.frags.size();)
    {
        Frag* f = S.frags[i];
        f->t += dt;
        if (f->sink > 0.f || (f->life <= 0.f) || (f->resting && f->t > f->life))
        {
            if (f->shell) // sinking: no body, straight down
                destroy_physics_shell(f->shell);
            f->sink += dt;
            if (f->sink >= C.frag_sink)
            {
                kill_frag(i);
                continue;
            }
            Fmatrix m = f->xf;
            m.c.y -= dt * (f->r * 2.f + 0.1f) / C.frag_sink;
            f->place(m);
            ++i;
            continue;
        }
        if (!f->shell)
        {
            ++i;
            continue;
        }
        Fmatrix M;
        f->shell->InterpolateGlobalTransform(&M);
        if (!_valid(M) || M.c.y < -300.f)
        {
            kill_frag(i);
            continue;
        }
        const bool enabled = f->shell->isEnabled();
        if (enabled || !f->resting)
            f->place(M);
        f->resting = !enabled;
        if (enabled)
        {
            Fvector vel;
            f->shell->get_LinearVel(vel);
            const float dv = Fvector().sub(vel, f->v_prev).magnitude();
            f->v_prev = vel;
            if (dv > C.clatter_dv && f->t > 0.05f)
            {
                fx_allowed(); // resets the frame's counts
                if ((int)S.knock_count++ < C.max_clatter && M.c.distance_to_sqr(Device.vCameraPosition) < 60.f * 60.f)
                {
                    make_sounds();
                    play_snd(S.knock[f->mat], M.c, clampr(dv / 10.f, 0.15f, 0.8f) * clampr(f->r * 3.f, 0.3f, 1.f));
                }
            }
        }
        ++i;
    }
}

// queued breaks, and the landings of props a hit has thrown
void update_tracks()
{
    const u32 now = Device.dwTimeGlobal;
    for (auto it = S.tracks.begin(); it != S.tracks.end();)
    {
        Track& t = it->second;
        CObject* o = Level().Objects.net_Find(it->first);
        CPhysicObject* po = o == t.obj ? smart_cast<CPhysicObject*>(o) : nullptr;
        if (!po || po->getDestroy())
        {
            it = S.tracks.erase(it);
            continue;
        }
        if (t.queued)
        {
            if (!not_breakable(po))
                break_prop(po, t);
            it = S.tracks.erase(it);
            continue;
        }
        if (now - t.last > u32(C.keep_s * 1000.f))
        {
            it = S.tracks.erase(it);
            continue;
        }
        CPhysicsShell* sh = now - t.last < u32(C.watch_s * 1000.f) ? po->PPhysicsShell() : nullptr;
        if (sh && sh->isActive() && sh->isEnabled())
        {
            Fvector v;
            sh->get_LinearVel(v);
            const PropDef& p = D.props[t.def];
            if (t.has_prev)
            {
                const float dv = Fvector().sub(v, t.v_prev).magnitude();
                if (dv > C.imp_dv[p.mat])
                {
                    Fvector d = t.v_prev;
                    d.normalize_safe();
                    t.dir = d;
                    t.point = po->Position();
                    t.impulse = p.mass * dv * 0.5f;
                    damage(po, t, C.imp_k * p.mass * (dv - C.imp_dv[p.mat]) * (dv - C.imp_dv[p.mat]) + 1e-3f, "a hard landing");
                }
            }
            t.v_prev = v;
            t.has_prev = true;
        }
        else
            t.has_prev = false;
        ++it;
    }
}

// ------------------------------------------------------------------------------------------------------------- API
// the prop's track (its damage so far), made anew for an object not seen before under this id
Track& track_of(CPhysicObject* o, int def)
{
    Track& t = S.tracks[o->ID()];
    if (t.obj != o)
    {
        t = Track{};
        t.obj = o;
        t.def = (u32)def;
        t.hp = prop_hp(D.props[def]);
    }
    return t;
}
} // namespace

void hit(CPhysicObject* o, const SHit& h)
{
    load_cfg();
    if (!C.enabled || !portal_mod_feat::enabled(portal_mod_feat::destruct) || !o) // portal-mod E55: My Mods
        return;
    // a physic_destroyable_object's own collision damage (CPHCollisionDamageReceiver: whoID is the object itself) - one
    // spawned touching the level hits itself on load (a box 250 m off broke): its landings are the watch's (update_tracks)
    if (h.who == o)
        return;
    const int def = def_of(o);
    if (def < 0 || not_breakable(o))
        return;
    const PropDef& p = D.props[def];
    float dmg = h.power * hit_k(h.type());
    if (h.type() == ALife::eHitTypeFireWound)
        dmg *= C.fire[p.mat];
    if (C.debug)
        Msg("[props] hit %s (%u, %s): type %d power %.3f impulse %.1f -> %.1f", o->cNameVisual().c_str(), o->ID(), MAT_KEY[p.mat], (int)h.type(), h.power,
            h.impulse, dmg);
    if (dmg <= 0.f)
        return;
    ++S.hits;
    Track& t = track_of(o, def);
    if (t.queued)
        return;
    // where it was hit, in the world
    Fvector pt = h.p_in_bone_space;
    IKinematics* K = o->Visual() ? o->Visual()->dcast_PKinematics() : nullptr;
    if (K && h.boneID != BI_NONE && h.boneID < K->LL_BoneCount())
        K->LL_GetTransform(h.boneID).transform_tiny(pt);
    o->XFORM().transform_tiny(pt);
    t.point = pt;
    t.dir = h.dir;
    t.dir.normalize_safe();
    t.impulse = _max(t.impulse * 0.5f, h.impulse);
    t.who = h.who ? h.who->ID() : u16(-1);
    t.last = Device.dwTimeGlobal;
    if (h.type() == ALife::eHitTypeFireWound && (p.mat == M_WOOD || p.mat == M_STONE))
        portal_mod_destruct::chips(pt, Fvector().invert(t.dir), t.dir, h.power * C.k_fire * C.chips_k, p.mat == M_WOOD ? 2 : 0);
    damage(o, t, dmg, "a hit", (int)h.type());
}

void explosion(const Fvector& pos, float radius, float hit_power, float impulse)
{
    load_cfg();
    if (!C.enabled || !portal_mod_feat::enabled(portal_mod_feat::destruct) || radius <= 0.f || C.blast_k <= 0.f || !g_pGameLevel)
        return;
    load_defs();
    if (!D.ok)
        return;
    u32 n = 0;
    for (u32 i = 0, cnt = Level().Objects.o_count(); i < cnt; ++i)
    {
        CPhysicObject* o = smart_cast<CPhysicObject*>(Level().Objects.o_get_by_iterator(i));
        if (!o)
            continue;
        Fvector c;
        o->Center(c);
        const float d = c.distance_to(pos);
        if (d >= radius)
            continue;
        const int def = def_of(o);
        if (def < 0 || not_breakable(o))
            continue;
        // the level in between (a floor slab, a wall): rays from the blast to the prop's middle, top and foot - the share
        // that sees it (a blast on the floor above went through the slab and broke the room under it)
        Fvector pts[3] = {c, c, c};
        if (o->Visual())
        {
            const Fbox& b = o->Visual()->getVisData().box;
            Fvector top{(b.min.x + b.max.x) * 0.5f, b.max.y - 0.05f, (b.min.z + b.max.z) * 0.5f}, foot = top;
            foot.y = b.min.y + 0.1f;
            o->XFORM().transform_tiny(pts[1], top);
            o->XFORM().transform_tiny(pts[2], foot);
        }
        int seen = 0;
        for (const Fvector& q : pts)
        {
            Fvector dir;
            dir.sub(q, pos);
            const float len = dir.magnitude();
            if (len < 0.1f)
            {
                ++seen;
                continue;
            }
            dir.div(len);
            seen += !Level().ObjectSpace.RayTest(pos, dir, len - 0.05f, collide::rqtStatic, nullptr, nullptr);
        }
        const float vis = seen ? seen / 3.f : C.blast_cover;
        const float dmg = C.blast_k * hit_power * powf(1.f - d / radius, C.blast_pow) * vis;
        if (dmg <= 0.f)
            continue;
        Track& t = track_of(o, def);
        if (t.queued)
            continue;
        t.point = pos;
        t.dir.sub(c, pos).normalize_safe();
        t.impulse = _max(t.impulse, impulse * (1.f - d / radius));
        t.last = Device.dwTimeGlobal;
        t.who = u16(-1);
        damage(o, t, dmg, "a blast");
        ++n;
        if (C.debug)
            Msg("[props] blast %.1f m off %s (%u), seen %d of 3: %.1f of %.0f", d, o->cNameVisual().c_str(), o->ID(), seen, t.dmg, t.hp);
    }
    if (C.debug)
        Msg("[props] blast at %.1f,%.1f,%.1f r %.1f hit %.2f impulse %.0f: %u props", pos.x, pos.y, pos.z, radius, hit_power, impulse, n);
}

void lua_debug(bool on) { C.debug = on; }

void update()
{
    if (portal_mod_replay || !g_pGameLevel)
        return; // a replay plays: the world stands
    if (S.tracks.empty() && S.frags.empty())
        return;
    load_cfg();
    const float dt = _min(Device.fTimeDelta, 0.05f);
    update_tracks();
    update_frags(dt);
}

void reset()
{
    for (Frag*& f : S.frags)
        xr_delete(f);
    S.frags.clear();
    S.tracks.clear();
    // the sounds go with the level (made again at the next break): a static's destructor runs after the sound engine is gone
    for (int m = 0; m < NMAT; ++m)
    {
        S.snd[m].clear();
        S.knock[m].clear();
    }
    S.sounds = false;
}

LPCSTR lua_info()
{
    load_defs();
    static string256 buf;
    u32 awake = 0, lying = 0;
    for (const Frag* f : S.frags)
    {
        awake += f->shell && f->shell->isEnabled();
        lying += f->resting;
    }
    xr_sprintf(buf, "models=%u tracked=%u broken=%u hits=%u frags=%u awake=%u lying=%u made=%u", (u32)D.props.size(), (u32)S.tracks.size(), S.broken, S.hits,
               (u32)S.frags.size(), awake, lying, S.frags_made);
    return buf;
}

bool lua_break(int id)
{
    load_cfg();
    CPhysicObject* o = smart_cast<CPhysicObject*>(Level().Objects.net_Find(u16(id)));
    if (!o)
        return false;
    const int def = def_of(o);
    if (def < 0 || not_breakable(o))
        return false;
    Track& t = track_of(o, def);
    t.point = o->Position();
    t.point.y += D.props[def].size * 0.5f;
    t.dir.set(0.f, -1.f, 0.f);
    t.last = Device.dwTimeGlobal;
    t.queued = true;
    return true;
}

LPCSTR lua_near(const Fvector& pos, float radius)
{
    load_cfg();
    static xr_string out;
    out.clear();
    string512 one;
    for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
    {
        CObject* o = Level().Objects.o_get_by_iterator(i);
        CPhysicObject* po = smart_cast<CPhysicObject*>(o);
        if (!po || po->Position().distance_to(pos) > radius)
            continue;
        const int def = def_of(po);
        LPCSTR why = def < 0 ? "not cut" : not_breakable(po);
        const auto it = S.tracks.find(po->ID());
        xr_sprintf(one, "%u|%s|%s|%s|%.0f|%.0f|%.1f|%s|%.2f;", po->ID(), po->cNameSect().c_str(), po->cNameVisual().c_str(), why ? why : "yes",
                   def >= 0 ? prop_hp(D.props[def]) : 0.f, it != S.tracks.end() ? it->second.dmg : 0.f, po->Position().distance_to(pos),
                   def >= 0 ? MAT_KEY[D.props[def].mat] : "-", def >= 0 ? D.props[def].size : 0.f);
        out += one;
    }
    return out.c_str();
}
} // namespace portal_mod_props
