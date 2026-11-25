// portal-mod E54: destructible buildings and chips (see portal_mod_destruct.h; tools/destruct_build.py writes the data).
//
// levels\<level>\level.pmd (tools/destruct_build.py): the building's frame, which level collision triangles and which
// render triangles are its own, the pieces (model, material, mass, boxes, their collision triangles), the bonds
// between pieces (shared area, vertical or horizontal) and the ground anchors.
//
// At load the building's own collision triangles leave the static model and the pieces' triangles come in at its end
// (a table maps them back to pieces), so bullets, rays, physics and AI see the pieces. The level keeps drawing its own
// (lightmapped) building until the first piece is destroyed; then the building's render triangles are cut out of the
// level's index buffers (CRender::LoadBuffers kept a cut copy) and every piece is drawn as its model.
//
// A piece takes damage from bullets ([pm_destruct_ammo]: per round, by calibre), explosions (by distance) and impacts
// of falling pieces; at 0 it turns to rubble (chips flying, dust, a sound). The solver after every change: pieces that
// have no path to the ground over unbroken bonds are released; the rest carry loads down their shortest paths (to the
// ground first), split over the bonds below them by bond area; horizontal transfers carry the moment of what hangs on
// them; a bond breaks when its load or moment passes its capacity - the larger of the material's strength and twice
// what it carried when the building stood whole (so the intact building stands as built). Released pieces fall as
// islands: the pieces still bonded together, one rigid body (an element with the pieces' boxes). A hard landing damages
// what it lands on, breaks some of the island's own bonds and splits it.
//
// Chips: fragments flying off the hit spot (concrete, brick, wood models of four sizes), bouncing on the level's
// collision (rays), lying a while, then sinking away. Not physics bodies: cheap enough for hundreds.
#include "stdafx.h"
#include <algorithm>
#include <queue>
#include "portal_mod_destruct.h"
#include "portal_mod_features.h" // portal-mod E55: My Mods' switches
#include "Level.h"
#include "Level_Bullet_Manager.h"
#include "ParticlesObject.h"
#include "PhysicsShell.h"
#include "PhysicsCommon.h"
#include "ExtendedGeom.h"
#include "PHWorld.h"
#include "../xr_3da/GameMtlLib.h"
#include "GamePersistent.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../xr_3da/Stats.h"
#include "Actor.h"
#include "entity_alive.h"
#include "EntityCondition.h"
#include "Hit.h"

// portal-mod E54b: defined in the renderer (Layers/xrRender/Blender_Recorder_StandartBinding.cpp, xrRenderPC_R4/r4_loader.cpp)
extern Fvector4 portal_mod_holes[32]; // the holes the shaders cut (world space centre, radius; 0 - none)
extern bool portal_mod_holes_near; // a hole near the camera: the occluders are off
bool portal_mod_ps_exists(LPCSTR name);
// portal-mod E54e: the profiler's sources (xr_3da/Device.cpp, xrRenderPC_R4/r4_loader.cpp)
extern float g_pm_frame_ms[4]; // the last frame: the game's update, the render, the second thread's tasks, the wait for them
extern BOOL g_pm_stat_gather; // the engine's stat timers on without the stats on screen
void portal_mod_render_stats(u32* out); // main calls, verts, models' calls, level calls, 3 sun cascades' calls
int portal_mod_sun_cascade(u32 context_id); // which sun cascade a render context draws (-1: none)
// portal-mod E56: dents (Layers/xrRender/Blender_Recorder_StandartBinding.cpp, xrRenderPC_R4/r4_loader.cpp)
extern Fvector4 portal_mod_dents[3][48]; // world space, the nearest first: centre & radius, normal & seed, tangent & band
extern u32 portal_mod_dents_count; // how many of them the shaders cut
extern Fvector4 portal_mod_dents_groups[6]; // world space: a sphere round each group of 8 slots (spatially close dents)
extern u32 portal_mod_dents_groups_count;
bool portal_mod_ctx_smap(u32 context_id); // a render context drawing a shadow map
extern bool g_r_dsgraph_dup_ok; // the render graph takes a visual once a pass but while this is up (r__dsgraph_build.cpp)

namespace portal_mod_destruct
{
namespace
{
enum : u8
{
    ST_INTACT,
    ST_FALLING,
    ST_GONE
};
constexpr u32 ISLAND_MAGIC = 0x50444953;
constexpr int NMAT = 3; // concrete, brick, wood (tools/destruct_build.py MATERIALS order)
LPCSTR const MAT_KEY[NMAT] = {"concrete", "brick", "wood"};
LPCSTR const CHIP_SIZE[4] = {"s", "m", "l", "x"};
const float CHIP_R[4] = {0.025f, 0.06f, 0.16f, 0.3f}; // the chip models' radius about their centre
constexpr int DENT_VARS = 6; // E56: a dent's bowl models per material (tools/destruct_build.py write_dents): 0-3 a round's, 4-5 a blast's
constexpr int DENT_DRAW_MAX = 48; // E56: the shaders' arrays (pm_dents_a/b/c)
constexpr float DENT_RIM = 1.32f, DENT_DEPTH = 0.45f; // E56: the bowl's rim and depth in its model, x the dent's radius

struct Cfg
{
    bool enabled{true};
    float hp_m3[NMAT]{300.f, 180.f, 90.f}; // a piece's hit points per m3 of it
    float k_expl{300.f}, expl_pow{2.f}, expl_push{6.f}; // explosion: damage = hit * k_expl * (1 - d/R)^pow
    float sc{1.5e5f}, ss{6e4f}, sm{3e4f}, safety{2.f}; // N/m2: compression, shear, bending (x area^1.5)
    // E54d lateral stability: every piece leans with `sway` of its weight sideways (a building's imperfections - a column
    // out of plumb, wind) and a blast pushes the pieces near it sideways, blast_lat N per m2 of a piece's side at its
    // centre (falling off like the damage). A side bond braces a piece along its direction; what is not braced goes down
    // the stack, and a stack tips over its contact below once the moment passes the weight above times half the
    // contact's depth (blocks on blocks) plus the contact's tensile strength st (N/m2, which also holds what hangs)
    float sway{0.04f}, st{1.5e4f}, blast_lat{2e4f};
    float tip_w{0.7f}, release_w{0.25f}; // rad/s: a toppled stack is set going over its edge; anything let go rocks a bit
    int max_moving{60}; // E54d: bodies moving at once past which a small one breaking up on landing turns to rubble
    int crumble_pieces{4}; // E54e: ... a part of so many pieces at most
    float crumble_vol{2.f}; // E54e: ... and under so many m3 (6 RPG rockets brought it down as 338 bodies, 277 moving: 100+ ms frames)
    // E54e: past max_release bodies moving at once, the parts let go together are glued into bodies of up to glue_pieces
    // pieces that split on their first landing (one rocket let 240 parts go in one frame: physics 10 ms a step at 100 steps
    // a second - more steps a frame the longer the frame, 100-146 ms frames for two seconds)
    int max_release{100}, glue_pieces{40};
    // E54e: an overloaded bond breaks only within cascade_r m across of a piece destroyed (or on a piece worn under 60%):
    // the loads go down the shortest paths only, a real frame shares them round - one rocket at a corner of the ground
    // floor took the whole building down bond by bond, overloads of x100-x1000 far from it (0: anywhere)
    float cascade_r{2.5f};
    // E54e: what holds a piece that comes to hang from the one above it (its support under it gone), N per m2 of the
    // contact; one that hung standing holds `safety` times that (column stubs hung under a floor in a row, 1.5 m up)
    float sh{0.f};
    float imp_k{0.006f}, imp_self_k{0.002f}, imp_dv{3.f}, break_dv{4.f}; // impacts: damage per J; dv (m/s) that counts
    float margin{0.02f}; // the boxes of a falling piece are this much smaller than its voxels
    float chip_k{1.f}; // the number of chips per hit
    float chip_life{14.f}, rubble_life{40.f}; // s lying before they sink
    int max_chips{600};
    // E56: a chip smaller than chip_shadow_min (m, its model's sphere) casts no shadow; one farther than chip_draw_dist
    // (m; the big ones twice that) is not drawn
    float chip_shadow_min{0.1f}, chip_draw_dist{45.f};
    int max_fx{6}; // particle systems started per frame at most
    // E54b holes: a round of at least hole_min_dmg into a wall no thicker than hole_max_thick (m) adds up with the
    // rounds before it at the spot; past hole_need (per m of the wall's thickness, by material) a hole opens, its
    // radius on the wall's faces hole_r0 + hole_r_k * (damage past the need), up to hole_r_max
    float hole_min_dmg{9.f}, hole_max_thick{0.45f};
    float hole_need[NMAT]{260.f, 160.f, 70.f};
    float hole_r0{0.06f}, hole_r_k{0.0028f}, hole_r_max{0.33f};
    int max_holes{64};
    // E56 dents: a round of at least dent_min_dmg into stone, concrete, brick or wood anywhere (any thickness, one face
    // or two) adds up at the spot - with the rounds before it within the dent's radius + dent_merge on the same face;
    // past dent_need it is a dent: the face cut away inside a ragged dark edge, a bowl behind it. Its radius dent_r0 x
    // dent_scale (by material) x sqrt(damage / need), up to dent_r_max (a blast's dent_r_max_blast), as small as the face
    // round it holds; as deep as dent_depth x the radius, at most 0.6 of the wall's thickness. max_dents kept a level (the
    // oldest goes), the dent_draw nearest the camera within dent_draw_dist m drawn, casting no shadows
    bool dents{true};
    float dent_min_dmg{2.f}, dent_need{5.f}, dent_merge{0.1f};
    float dent_r0{0.05f}, dent_scale[NMAT]{1.f, 1.15f, 1.3f}, dent_r_max{0.32f}, dent_r_max_blast{0.75f}, dent_depth{0.45f};
    float dent_band{0.5f}, dent_band_blast{1.2f}; // round its edge, x its radius: a round's light dust, a blast's soot
    int max_dents{400}, dent_draw{48};
    float dent_draw_dist{70.f};
    // E56: a blast dents the faces round it on any level - blast_dent_rays rays out to blast_r x blast_dent_reach (at most
    // blast_dent_range m); a face hit gets a dent at its point nearest the blast, damage blast x blast_dent_k x
    // (1 - d / range)^2, and the nearest blast_dent_chips of them throw chunks
    int blast_dent_rays{26}, blast_dent_chips{6};
    float blast_dent_reach{0.5f}, blast_dent_range{5.f}, blast_dent_k{300.f};
    xr_vector<shared_str> fx_destroy[NMAT], fx_impact, fx_heavy_hit, snd_destroy, snd_impact;
    shared_str snd_collapse;
    // E54e crush: a falling body moving faster than crush_v (m/s, at the spot) that reaches a living creature hits it -
    // power crush_k per J of 0.5 m v^2 (m: the body's mass, at most crush_mass), of crush_hit_type, impulse crush_impulse
    // per kg m/s (at most 600); the actor too if crush_actor; blamed on the actor (who brought it down) if crush_blame_actor
    bool crush{true}, crush_actor{true}, crush_blame_actor{true};
    float crush_v{2.5f}, crush_k{5e-4f}, crush_mass{4000.f}, crush_impulse{0.05f};
    float crush_lethal{6000.f}; // J: past this it kills through any immunity, outfit or rank
    float crush_reach{0.35f}; // m: such a body reaches this much further (what flies off it as it crashes down)
    float crush_pin_mass{800.f}; // kg: a body this heavy reaching a creature counts as moving at crush_v at least (it
                                 // comes to rest on him: the character's own capsule stops it before it is fast)
    bool crush_debug{}; // a line a second for each moving body with a creature near: how far its boxes are from him
    bool solve_debug{}; // E54e: a line for each pass of the solver that broke a bond - which way they broke, what is let go
    // E54e: a body of more than merge_boxes boxes collides as its boxes merged on a grid (merge_cell m at least, 40
    // cells across at most); 0 - never
    int merge_boxes{24};
    float merge_cell{0.3f};
    // E54e: the pieces' sun shadows - a piece smaller than shadow_min_r (m, its bounding sphere) casts no shadow in the
    // second sun cascade, smaller than twice that none in the third (where it is a pixel or two); 0 - all cast
    float shadow_min_r{0.6f};
    // E54e: level.destruct_demolish - of what stood on the charges, a bond between storeys breaks with demolish_break_v,
    // any other with demolish_break_h (one rigid frame of 600 pieces landed whole on its stubs); a body of more than
    // big_body pieces lands as hard at half impact_dv and break_dv (it cannot take a landing without cracking)
    float demolish_break_v{0.8f}, demolish_break_h{0.45f};
    int big_body{100};
    ALife::EHitType crush_type{ALife::eHitTypeStrike};
    xr_vector<shared_str> crush_immune; // sections never hit (level.destruct_crush_immune(id, true) for one object)
    xr_vector<shared_str> fx_demolish; // E54e: level.destruct_demolish - the charges going off
    shared_str snd_demolish;
};
Cfg C;

struct Box
{
    Fvector c, h; // piece-local (the building's axes), half sizes
};

struct PieceVis : public ISpatial, public IRenderable
{
    bool in_space{};
    explicit PieceVis(LPCSTR vis) : ISpatial(g_SpatialSpace)
    {
        renderable.visual = ::Render->model_Create(vis);
        renderable.pROS_Allowed = 2; // E54c: no light traced for it, the open sky and the sun (LightTrack.cpp)
        renderable.xform.identity();
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY; // drawn over a replay too (the level's own is gone)
    }
    ~PieceVis() override
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
    void place(const Fmatrix& m, bool standing = false)
    {
        if (!renderable.visual || !_valid(m))
            return;
        renderable.xform.set(m);
        if (standing) // E54b: the shaders cut holes only in the level and in standing pieces, known by this scale
        {
            renderable.xform.i.mul(1.0001f);
            renderable.xform.j.mul(1.0001f);
            renderable.xform.k.mul(1.0001f);
        }
        const vis_data& v = renderable.visual->getVisData();
        Fvector P;
        m.transform_tiny(P, v.sphere.P);
        spatial.sphere.set(P, _max(v.sphere.R, 0.3f));
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
        if (C.shadow_min_r > 0.f) // E54e: a small piece casts no shadow in the far sun cascades
        {
            const int cas = portal_mod_sun_cascade(context_id);
            if (cas >= 1 && spatial.sphere.R < C.shadow_min_r * cas)
                return;
        }
        ::Render->add_Visual(context_id, this, renderable.visual, renderable.xform);
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

// one model drawn at every chip that uses it
struct ChipModel : public ISpatial, public IRenderable
{
    xr_vector<Fmatrix> xf;
    bool in_space{};
    explicit ChipModel(LPCSTR vis) : ISpatial(g_SpatialSpace)
    {
        renderable.visual = ::Render->model_Create(vis);
        renderable.xform.identity();
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY;
    }
    ~ChipModel() override
    {
        if (in_space)
            spatial_unregister();
        if (renderable.visual)
            ::Render->model_Delete(renderable.visual);
    }
    void commit()
    {
        if (xf.empty() || !renderable.visual)
        {
            if (in_space)
            {
                spatial_unregister();
                in_space = false;
            }
            return;
        }
        Fbox b;
        b.invalidate();
        for (const Fmatrix& m : xf)
            b.modify(m.c);
        b.grow(0.5f);
        Fvector c;
        b.getcenter(c);
        renderable.xform.identity();
        renderable.xform.c = c;
        spatial.sphere.set(c, b.getradius());
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
        const float R = renderable.visual->getVisData().sphere.R;
        if (R < C.chip_shadow_min && portal_mod_ctx_smap(context_id)) // E56: a small chip's shadow is a dot
            return;
        if (C.shadow_min_r > 0.f) // E54e: chips cast no shadow in the far sun cascades
        {
            const int cas = portal_mod_sun_cascade(context_id);
            if (cas >= 1 && R < C.shadow_min_r * cas)
                return;
        }
        const float reach = C.chip_draw_dist * (R >= C.chip_shadow_min ? 2.f : 1.f); // (`far` is a Windows macro)
        const float far2 = reach * reach;
        const Fvector& cam = Device.vCameraPosition;
        g_r_dsgraph_dup_ok = true; // E56: one model at many places this pass (else only the first chip of a model is drawn)
        for (Fmatrix& m : xf)
            if (m.c.distance_to_sqr(cam) < far2)
                ::Render->add_Visual(context_id, this, renderable.visual, m);
        g_r_dsgraph_dup_ok = false;
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

// E56: a dent's bowl behind the face cut away - one of the shared models (S.dent_models own them), drawn at the dent with
// its own light (traced from out in front of the face); no shadows: it is inside the wall
struct DentVis : public ISpatial, public IRenderable
{
    bool in_space{};
    u32 dent{}; // the id of the dent it shows (0: none)
    DentVis() : ISpatial(g_SpatialSpace)
    {
        renderable.visual = nullptr;
        renderable.pROS_Allowed = 3; // its light traced from its model's centre, out in front of the wall (LightTrack.cpp, E56)
        renderable.xform.identity();
        spatial.type = STYPE_RENDERABLE | PORTAL_MOD_STYPE_REPLAY;
    }
    ~DentVis() override
    {
        hide();
        renderable.visual = nullptr; // not its own
    }
    void hide()
    {
        if (in_space)
        {
            spatial_unregister();
            in_space = false;
        }
        dent = 0;
    }
    // the sphere for culling and the sector: out in front of the face (the room it is seen from), over the whole bowl
    void place(IRenderVisual* v, const Fmatrix& m, const Fvector& front, float R)
    {
        renderable.visual = v;
        renderable.xform.set(m);
        spatial.sphere.set(front, R);
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
        if (!renderable.visual || portal_mod_ctx_smap(context_id))
            return;
        g_r_dsgraph_dup_ok = true; // the bowls share their models: another dent's may have gone this pass already
        ::Render->add_Visual(context_id, this, renderable.visual, renderable.xform);
        g_r_dsgraph_dup_ok = false;
    }
    BOOL renderable_ShadowReceive() override { return TRUE; }
};

struct Piece
{
    shared_str vis;
    u8 mat{};
    bool anchored{};
    float mass{}, vol{}, anchor_area{};
    Fvector pivot{}; // world, intact
    Fbox aabb{};
    Fvector lo{}, hi{}; // E54d: its extents in the building's axes (x, z along S.rot's i, k)
    xr_vector<Box> boxes;
    u32 tri0{}, ntri{};
    float hp{}, hp_max{}, hp_solved{};
    u8 state{ST_INTACT};
    int island{-1};
    Fvector local{}; // its pivot in its island's frame
    xr_vector<u32> bonds;
    PieceVis* pv{};
};

struct Bond
{
    u16 a{}, b{};
    bool vertical{};
    float area{}, area_v{};
    Fvector pt{};
    bool broken{};
    float cap_f{}, cap_s{}, cap_m{}, f{}, m{};
    // E54d, a vertical bond: its contact's depth along the building's x and z, the tensile part of what holds the stack
    // above from tipping (cap_t) and what holds a piece hanging under it (cap_n); the moment's direction (where the
    // stack would go); hanging - the piece the load comes from is under it; toppled - broke tipping over, when
    float dx{}, dz{}, cap_t{}, cap_n{};
    Fvector tip{};
    bool hang{}, toppled{};
    u32 t_broke{};
    bool zone{}; // E54e: within cascade_r (across) of a piece destroyed - it may break overloaded
};

struct Island
{
    u32 magic{ISLAND_MAGIC};
    CPhysicsShell* shell{};
    xr_vector<u16> pieces;
    float mass{};
    Fvector v_prev{};
    bool has_prev{}, dead{};
    xr_vector<u16> touched; // intact pieces its boxes touched since last frame
    bool touched_static{};
    Fmatrix xf{};
    float radius{}; // E54e: its boxes' reach from its origin
    bool glued{}; // E54e: parts let go together glued into one body (over max_release) - it splits on its first landing
};

struct Chip
{
    Fvector p, v, axis;
    float ang, w, r, life, t;
    u16 model;
    bool rest;
};

// E54b: a spot on a thin wall taking rounds; open once their damage passes the need - a sphere across the wall
struct Hole
{
    Fvector c{}; // the middle of the wall's thickness under the first hit
    Fvector dir{}; // the first round's direction
    float thick{}, need{}, dmg{}, r{}; // r: the sphere's radius once open (0: still closed)
    int piece{-1}; // on a building's standing piece (the hole goes with the piece)
    u8 mat{};
    u32 time{};
};

// E56: a spot on any face of stone, concrete, brick or wood taking rounds (or a blast); a dent once their damage passes
// dent_need - the shaders cut the face away inside its ragged edge, a bowl is drawn behind it
struct Dent
{
    Fvector c{}, n{}, t{}; // the centre on the face, the face's normal (towards where the hits came from), a tangent
    float r{}, depth{}, dmg{}, thick{}, seed{}; // r 0: not yet a dent; thick: the wall behind it (1: 1 m or more)
    float cap{-1.f}; // the radius the face round it holds (-1: not found short yet), and checked: up to which it was seen to
    float checked{};
    int piece{-1}; // on a building's standing piece (the dent goes with it)
    u8 mat{}, var{};
    bool blast{};
    u32 time{}, id{};
};

struct IbCopy
{
    bool fast;
    u32 ib;
    xr_vector<u16> data;
};

struct State
{
    bool loaded{}, swapped{}, dirty{}, models{};
    shared_str name;
    Fmatrix rot{}; // the building's axes
    xr_vector<u32> cut_cform;
    xr_map<u64, xr_vector<u32>> ib_cuts;
    xr_vector<IbCopy> ib_copies;
    xr_vector<shared_str> mtl;
    xr_vector<Piece> pieces;
    xr_vector<Bond> bonds;
    xr_vector<Fvector> app_v;
    xr_vector<u32> app_t;
    u32 tri_base{}, tri_count{}, sink{};
    xr_vector<u16> tri_piece;
    xr_vector<Island*> islands;
    xr_vector<Chip> chips;
    xr_vector<ChipModel*> chip_models; // [mat][size][variant]
    xr_vector<u8> mtl_class; // GMLib index -> chip class (255: none)
    xr_vector<struct Hole> holes; // E54b
    u32 holes_open{};
    xr_vector<Dent> dents; // E56
    xr_vector<DentVis*> dent_vis; // E56: the bowls drawn (DENT_DRAW_MAX at most)
    xr_vector<IRenderVisual*> dent_models; // E56: [mat][variant]
    bool dent_models_made{};
    u32 dent_ids{}, dents_drawn{};
    u32 fx_frame{}, fx_count{};
    u32 solves{}, broken{}, toppled{};
    u32 restacked{}; // E54d: sideways bonds between pieces one above the other, counted as stacking
    u32 vbase{}; // the pieces' first vertex in the static collision model (app_t is kept: a load puts their triangles back)
    Fvector blast_pos{};
    float blast_r{}, blast_push{};
    u32 blast_frame{u32(-1)};
    xr_vector<Fvector> blast_h; // E54d: the last blast's sideways push on each piece (N), counted in its frame
    float base_y{}; // the ground under the building (the lowest anchored piece's bottom)
    shared_str level; // E54d: the level, once its saved state is put back (the first frame)
    bool restored{};
    u32 gen{}; // the saved states' generation it came from (a later load or a new game does not take its state back)
    xr_vector<ref_sound> snd_destroy, snd_impact;
    ref_sound snd_collapse;
    u32 collapse_snd_time{};
    xr_vector<std::pair<u16, u32>> crush_next; // E54e: a creature hit by a falling body, and when it can be hit again
    u32 crushed{}, crush_kills{};
    ref_sound snd_demolish;
};
State S;
xr_vector<u16> g_crush_immune; // E54e: objects never hit by falling pieces (Lua; kept across level loads, not saved)

// E54d: each level's destruction as it was left (a blob per level name; the level played now is written in when it
// goes and when the game is saved), and the generation - a load or a new game starts the next one
constexpr u32 SAVE_VERSION = 1;
struct Saved
{
    u32 gen{1};
    xr_map<shared_str, xr_vector<u8>> levels;
};
Saved G;

u64 ib_key(bool fast, u32 ib) { return (u64(fast) << 32) | ib; }

float cfg_f(LPCSTR sect, LPCSTR k, float def) { return pSettings->line_exist(sect, k) ? pSettings->r_float(sect, k) : def; }

void cfg_list(LPCSTR sect, LPCSTR k, xr_vector<shared_str>& out, LPCSTR def)
{
    out.clear();
    LPCSTR v = pSettings->line_exist(sect, k) ? pSettings->r_string(sect, k) : def;
    if (!v || !*v)
        return; // E54e: an empty value (crush_immune =)
    string256 one;
    for (int i = 0, n = _GetItemCount(v); i < n; ++i)
        out.emplace_back(_Trim(_GetItem(v, i, one)));
}

void cfg_fx(LPCSTR sect, LPCSTR k, xr_vector<shared_str>& out, LPCSTR def)
{
    cfg_list(sect, k, out, def);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [k](const shared_str& n) {
                                 const bool ok = portal_mod_ps_exists(n.c_str());
                                 if (!ok)
                                     Msg("! [destruct] [pm_destruct] %s: no particle effect or group '%s' - skipped", k, n.c_str());
                                 return !ok;
                             }),
              out.end());
}

void load_cfg()
{
    LPCSTR s = "pm_destruct";
    C = Cfg{};
    if (!pSettings->section_exist(s))
        return;
    C.enabled = !pSettings->line_exist(s, "enabled") || pSettings->r_bool(s, "enabled");
    for (int m = 0; m < NMAT; ++m)
        C.hp_m3[m] = cfg_f(s, (xr_string("hp_m3_") + MAT_KEY[m]).c_str(), C.hp_m3[m]);
    C.k_expl = cfg_f(s, "explosion_k", C.k_expl);
    C.expl_pow = cfg_f(s, "explosion_pow", C.expl_pow);
    C.expl_push = cfg_f(s, "explosion_push", C.expl_push);
    C.sc = cfg_f(s, "strength_compression", C.sc);
    C.ss = cfg_f(s, "strength_shear", C.ss);
    C.sm = cfg_f(s, "strength_bending", C.sm);
    C.safety = cfg_f(s, "safety", C.safety);
    C.sway = cfg_f(s, "sway", C.sway);
    C.st = cfg_f(s, "strength_tension", C.st);
    C.sh = cfg_f(s, "strength_hanging", C.sh);
    C.blast_lat = cfg_f(s, "explosion_lateral", C.blast_lat);
    C.tip_w = cfg_f(s, "topple_spin", C.tip_w);
    C.release_w = cfg_f(s, "release_spin", C.release_w);
    C.max_moving = (int)cfg_f(s, "max_moving_bodies", (float)C.max_moving);
    C.crumble_pieces = (int)cfg_f(s, "crumble_pieces", (float)C.crumble_pieces);
    C.crumble_vol = cfg_f(s, "crumble_volume", C.crumble_vol);
    C.max_release = (int)cfg_f(s, "max_released_bodies", (float)C.max_release);
    C.glue_pieces = (int)cfg_f(s, "glue_max_pieces", (float)C.glue_pieces);
    C.cascade_r = cfg_f(s, "cascade_radius", C.cascade_r);
    C.imp_k = cfg_f(s, "impact_k", C.imp_k);
    C.imp_self_k = cfg_f(s, "impact_self_k", C.imp_self_k);
    C.imp_dv = cfg_f(s, "impact_dv", C.imp_dv);
    C.break_dv = cfg_f(s, "break_dv", C.break_dv);
    C.margin = cfg_f(s, "box_margin", C.margin);
    C.chip_k = cfg_f(s, "chips_k", C.chip_k);
    C.chip_life = cfg_f(s, "chip_life", C.chip_life);
    C.rubble_life = cfg_f(s, "rubble_life", C.rubble_life);
    C.max_chips = (int)cfg_f(s, "max_chips", (float)C.max_chips);
    C.chip_shadow_min = cfg_f(s, "chip_shadow_min", C.chip_shadow_min);
    C.chip_draw_dist = cfg_f(s, "chip_draw_dist", C.chip_draw_dist);
    C.max_fx = (int)cfg_f(s, "max_fx_per_frame", (float)C.max_fx);
    C.hole_min_dmg = cfg_f(s, "hole_min_damage", C.hole_min_dmg);
    C.hole_max_thick = cfg_f(s, "hole_max_thickness", C.hole_max_thick);
    for (int m = 0; m < NMAT; ++m)
        C.hole_need[m] = cfg_f(s, (xr_string("hole_need_") + MAT_KEY[m]).c_str(), C.hole_need[m]);
    C.hole_r0 = cfg_f(s, "hole_radius_min", C.hole_r0);
    C.hole_r_k = cfg_f(s, "hole_radius_k", C.hole_r_k);
    C.hole_r_max = cfg_f(s, "hole_radius_max", C.hole_r_max);
    C.max_holes = (int)cfg_f(s, "max_holes", (float)C.max_holes);
    C.dents = !pSettings->line_exist(s, "dents") || pSettings->r_bool(s, "dents"); // E56
    C.dent_min_dmg = cfg_f(s, "dent_min_damage", C.dent_min_dmg);
    C.dent_need = _max(0.1f, cfg_f(s, "dent_need", C.dent_need));
    C.dent_merge = cfg_f(s, "dent_merge", C.dent_merge);
    C.dent_r0 = cfg_f(s, "dent_radius_min", C.dent_r0);
    for (int m = 0; m < NMAT; ++m)
        C.dent_scale[m] = cfg_f(s, (xr_string("dent_scale_") + MAT_KEY[m]).c_str(), C.dent_scale[m]);
    C.dent_r_max = cfg_f(s, "dent_radius_max", C.dent_r_max);
    C.dent_r_max_blast = cfg_f(s, "dent_radius_max_blast", C.dent_r_max_blast);
    C.dent_depth = cfg_f(s, "dent_depth", C.dent_depth);
    C.dent_band = cfg_f(s, "dent_band", C.dent_band);
    C.dent_band_blast = cfg_f(s, "dent_band_blast", C.dent_band_blast);
    C.max_dents = (int)cfg_f(s, "max_dents", (float)C.max_dents);
    C.dent_draw = clampr((int)cfg_f(s, "dent_draw", (float)C.dent_draw), 0, DENT_DRAW_MAX);
    C.dent_draw_dist = cfg_f(s, "dent_draw_dist", C.dent_draw_dist);
    C.blast_dent_rays = (int)cfg_f(s, "blast_dent_rays", (float)C.blast_dent_rays);
    C.blast_dent_chips = (int)cfg_f(s, "blast_dent_chips", (float)C.blast_dent_chips);
    C.blast_dent_reach = cfg_f(s, "blast_dent_reach", C.blast_dent_reach);
    C.blast_dent_range = cfg_f(s, "blast_dent_range", C.blast_dent_range);
    C.blast_dent_k = cfg_f(s, "blast_dent_k", C.blast_dent_k);
    cfg_fx(s, "fx_destroy_concrete", C.fx_destroy[0], "vehiclefx\\impact_concrete_03");
    cfg_fx(s, "fx_destroy_brick", C.fx_destroy[1], "explosions\\expl_wall_00");
    cfg_fx(s, "fx_destroy_wood", C.fx_destroy[2], "destroy_fx\\destroy_wood_00");
    cfg_fx(s, "fx_impact", C.fx_impact, "vehiclefx\\impact_concrete_02");
    cfg_fx(s, "fx_heavy_hit", C.fx_heavy_hit, "vehiclefx\\impact_concrete_01");
    cfg_list(s, "snd_destroy", C.snd_destroy, "ambient\\rnd_outdoor\\rnd_rock1");
    cfg_list(s, "snd_impact", C.snd_impact, "ambient\\rnd_outdoor\\rnd_rock2");
    C.snd_collapse = pSettings->line_exist(s, "snd_collapse") ? pSettings->r_string(s, "snd_collapse") : "ambient\\earthquake";
    auto cfg_b = [s](LPCSTR k, bool def) { return pSettings->line_exist(s, k) ? !!pSettings->r_bool(s, k) : def; };
    C.crush = cfg_b("crush", C.crush);
    C.crush_actor = cfg_b("crush_actor", C.crush_actor);
    C.crush_blame_actor = cfg_b("crush_blame_actor", C.crush_blame_actor);
    C.crush_v = cfg_f(s, "crush_speed", C.crush_v);
    C.crush_k = cfg_f(s, "crush_k", C.crush_k);
    C.crush_mass = cfg_f(s, "crush_mass", C.crush_mass);
    C.crush_impulse = cfg_f(s, "crush_impulse", C.crush_impulse);
    C.crush_lethal = cfg_f(s, "crush_lethal", C.crush_lethal);
    C.crush_pin_mass = cfg_f(s, "crush_pin_mass", C.crush_pin_mass);
    C.crush_reach = cfg_f(s, "crush_reach", C.crush_reach);
    C.crush_debug = cfg_b("crush_debug", C.crush_debug);
    C.solve_debug = cfg_b("solve_debug", C.solve_debug);
    C.merge_boxes = (int)cfg_f(s, "body_merge_boxes", (float)C.merge_boxes);
    C.merge_cell = cfg_f(s, "body_merge_cell", C.merge_cell);
    C.shadow_min_r = cfg_f(s, "shadow_min_radius", C.shadow_min_r);
    C.demolish_break_v = cfg_f(s, "demolish_break_v", C.demolish_break_v);
    C.demolish_break_h = cfg_f(s, "demolish_break_h", C.demolish_break_h);
    C.big_body = (int)cfg_f(s, "big_body", (float)C.big_body);
    if (pSettings->line_exist(s, "crush_hit_type"))
        C.crush_type = ALife::g_tfString2HitType(pSettings->r_string(s, "crush_hit_type"));
    cfg_list(s, "crush_immune", C.crush_immune, "");
    cfg_fx(s, "fx_demolish", C.fx_demolish, "explosions\\expl_car");
    C.snd_demolish = pSettings->line_exist(s, "snd_demolish") ? pSettings->r_string(s, "snd_demolish") : "weapons\\heli_explosion";
}

// ------------------------------------------------------------------------------------------------------------- data
bool read_pmd(IReader& F, size_t nv, size_t nt)
{
    char magic[4];
    F.r(magic, 4);
    if (strncmp(magic, "PMD1", 4) != 0 || F.r_u32() != 1)
        return false;
    shared_str level;
    F.r_stringZ(level);
    F.r_stringZ(S.name);
    const u32 cnv = F.r_u32(), cnt = F.r_u32();
    if (cnv != nv || cnt != nt)
    {
        Msg("! [destruct] %s: level.pmd was made for another level.cform (%u/%u verts, %u/%u tris) - not used", S.name.c_str(), cnv, (u32)nv, cnt, (u32)nt);
        return false;
    }
    const float yaw = F.r_float();
    Fvector origin;
    F.r_fvector3(origin);
    F.r_float(); // voxel size
    S.rot.identity();
    S.rot.i.set(_cos(yaw), 0.f, _sin(yaw));
    S.rot.j.set(0.f, 1.f, 0.f);
    S.rot.k.set(-_sin(yaw), 0.f, _cos(yaw));
    S.cut_cform.resize(F.r_u32());
    F.r(S.cut_cform.data(), S.cut_cform.size() * 4);
    for (u32 n = F.r_u32(); n; --n)
    {
        const bool fast = F.r_u8() != 0;
        const u32 ib = F.r_u32();
        xr_vector<u32>& v = S.ib_cuts[ib_key(fast, ib)];
        v.resize(F.r_u32());
        F.r(v.data(), v.size() * 4);
    }
    S.mtl.resize(F.r_u32());
    for (shared_str& m : S.mtl)
        F.r_stringZ(m);
    S.pieces.resize(F.r_u32());
    for (Piece& p : S.pieces)
    {
        F.r_stringZ(p.vis);
        p.mat = F.r_u8();
        p.anchored = F.r_u8() != 0;
        F.r_u16();
        p.mass = F.r_float();
        p.vol = F.r_float();
        F.r_fvector3(p.pivot);
        F.r_fvector3(p.aabb.min);
        F.r_fvector3(p.aabb.max);
        p.boxes.resize(F.r_u16());
        for (Box& b : p.boxes)
        {
            F.r_fvector3(b.c);
            F.r_fvector3(b.h);
        }
        p.tri0 = F.r_u32();
        p.ntri = F.r_u32();
        p.anchor_area = F.r_float();
        if (p.mat >= NMAT)
            p.mat = 0;
        p.hp = p.hp_max = p.hp_solved = _max(5.f, p.vol * C.hp_m3[p.mat]);
        const Fvector bp{p.pivot.dotproduct(S.rot.i), p.pivot.y, p.pivot.dotproduct(S.rot.k)};
        p.lo = p.hi = bp;
        for (const Box& b : p.boxes)
            for (int a = 0; a < 3; ++a)
            {
                p.lo[a] = _min(p.lo[a], bp[a] + b.c[a] - b.h[a]);
                p.hi[a] = _max(p.hi[a], bp[a] + b.c[a] + b.h[a]);
            }
    }
    S.bonds.resize(F.r_u32());
    for (u32 i = 0; i < S.bonds.size(); ++i)
    {
        Bond& b = S.bonds[i];
        b.a = F.r_u16();
        b.b = F.r_u16();
        b.vertical = F.r_u8() == 0;
        F.r_u8();
        F.r_u16();
        b.area = F.r_float();
        b.area_v = F.r_float();
        F.r_fvector3(b.pt);
        if (b.a < S.pieces.size() && b.b < S.pieces.size())
        {
            S.pieces[b.a].bonds.push_back(i);
            S.pieces[b.b].bonds.push_back(i);
            // E54d: the contact's footprint - where the two pieces' extents overlap, shrunk to its area keeping the shape
            const Piece& pa = S.pieces[b.a];
            const Piece& pb = S.pieces[b.b];
            float ox = _max(0.05f, _min(pa.hi.x, pb.hi.x) - _max(pa.lo.x, pb.lo.x));
            float oz = _max(0.05f, _min(pa.hi.z, pb.hi.z) - _max(pa.lo.z, pb.lo.z));
            const float A = b.area_v > 0.f ? b.area_v : b.area;
            if (ox * oz > A && A > 0.f)
            {
                const float k = _sqrt(A / (ox * oz));
                ox *= k;
                oz *= k;
            }
            b.dx = ox;
            b.dz = oz;
            // two pieces one above the other stack, whatever their rough contact's voxel faces say (the file calls a bond
            // vertical by its faces: a column's noisy cuts came out sideways, braced it and were never checked for tipping)
            const float dy = _abs(pb.pivot.y - pa.pivot.y), dh = _sqrt(_sqr(pb.pivot.x - pa.pivot.x) + _sqr(pb.pivot.z - pa.pivot.z));
            if (!b.vertical && dy > 1.5f * dh)
            {
                b.vertical = true;
                ++S.restacked;
            }
        }
        else
            b.broken = true;
    }
    S.app_v.resize(F.r_u32());
    F.r(S.app_v.data(), S.app_v.size() * sizeof(Fvector));
    S.app_t.resize(F.r_u32() * 3);
    F.r(S.app_t.data(), S.app_t.size() * 4);
    return !F.eof() || F.elapsed() == 0;
}

int piece_of_tri(u32 tri)
{
    if (!S.loaded || tri < S.tri_base || tri >= S.tri_base + S.tri_count)
        return -1;
    return S.tri_piece[tri - S.tri_base];
}

// a piece leaves the collision: its triangles point at the three sink vertices far below the level
void drop_tris(const Piece& p)
{
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    for (u32 k = 0; k < p.ntri; ++k)
    {
        CDB::TRI& t = T[S.tri_base + p.tri0 + k];
        t.verts[0] = S.sink;
        t.verts[1] = S.sink + 1;
        t.verts[2] = S.sink + 2;
    }
}

Fmatrix rest_xform(const Piece& p)
{
    Fmatrix m = S.rot;
    m.c = p.pivot;
    return m;
}

void make_models()
{
    if (S.models)
        return;
    S.models = true;
    for (Piece& p : S.pieces)
        p.pv = xr_new<PieceVis>(p.vis.c_str());
    for (int m = 0; m < NMAT; ++m)
        for (int s = 0; s < 4; ++s)
            for (int v = 0; v < 4; ++v)
            {
                string_path n;
                xr_sprintf(n, "portal_mod\\destruct\\chips\\%s_%s%d", MAT_KEY[m], CHIP_SIZE[s], v);
                S.chip_models.push_back(xr_new<ChipModel>(n));
            }
    auto mk = [](const xr_vector<shared_str>& names, xr_vector<ref_sound>& out) {
        out.resize(names.size());
        for (u32 i = 0; i < names.size(); ++i)
            out[i].create(names[i].c_str(), st_Effect, sg_SourceType);
    };
    mk(C.snd_destroy, S.snd_destroy);
    mk(C.snd_impact, S.snd_impact);
    if (C.snd_collapse.size())
        S.snd_collapse.create(C.snd_collapse.c_str(), st_Effect, sg_SourceType);
    if (C.snd_demolish.size())
        S.snd_demolish.create(C.snd_demolish.c_str(), st_Effect, sg_SourceType); // E54e
}

// the first destroyed piece: the level stops drawing its own building, the pieces are drawn instead
void swap_to_pieces()
{
    if (S.swapped || !S.loaded)
        return;
    S.swapped = true;
    make_models();
    for (const IbCopy& c : S.ib_copies)
        portal_mod_render_queue_ib(c.fast, c.ib, c.data.data(), (u32)c.data.size());
    S.ib_copies.clear();
    S.ib_copies.shrink_to_fit();
    for (Piece& p : S.pieces)
        if (p.state == ST_INTACT && p.pv)
            p.pv->place(rest_xform(p), true);
    Msg("* [destruct] %s: the level's own building is cut out, %u pieces drawn", S.name.c_str(), (u32)S.pieces.size());
}

// ------------------------------------------------------------------------------------------------------------- fx
bool fx_allowed()
{
    if (S.fx_frame != Device.dwFrame)
    {
        S.fx_frame = Device.dwFrame;
        S.fx_count = 0;
    }
    return S.fx_count++ < (u32)C.max_fx;
}

void play_fx(const xr_vector<shared_str>& names, const Fvector& pos, const Fvector& dir)
{
    if (names.empty() || !fx_allowed())
        return;
    if (pos.distance_to_sqr(Device.vCameraPosition) > 250.f * 250.f)
        return;
    CParticlesObject* ps = CParticlesObject::Create(names[::Random.randI(names.size())].c_str(), TRUE);
    Fmatrix m;
    m.identity();
    Fvector d = dir;
    if (d.square_magnitude() < EPS)
        d.set(0.f, 1.f, 0.f);
    d.normalize();
    m.k.set(d);
    Fvector::generate_orthonormal_basis(m.k, m.j, m.i);
    m.c.set(pos);
    ps->UpdateParent(m, {});
    GamePersistent().ps_needtoplay.push_back(ps);
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

u8 chip_class(u16 mtl_idx)
{
    if (S.mtl_class.empty())
    {
        S.mtl_class.assign(GMLib.CountMaterial(), 255);
        u16 i = 0;
        for (auto it = GMLib.FirstMaterial(); it != GMLib.LastMaterial(); ++it, ++i)
        {
            xr_string n = (*it)->m_Name.c_str();
            if (n.find("brick") != xr_string::npos)
                S.mtl_class[i] = 1;
            else if (n.find("wood") != xr_string::npos)
                S.mtl_class[i] = 2;
            else if (n.find("concrete") != xr_string::npos || n.find("stone") != xr_string::npos || n.find("asphalt") != xr_string::npos ||
                     n.find("tile") != xr_string::npos)
                S.mtl_class[i] = 0;
        }
    }
    return mtl_idx < S.mtl_class.size() ? S.mtl_class[mtl_idx] : 255;
}

void add_chip(u8 mat, int size, const Fvector& p, const Fvector& v, float life)
{
    make_models();
    if ((int)S.chips.size() >= C.max_chips)
    {
        // the oldest lying one goes first, else the oldest
        u32 best = 0;
        float bt = -1.f;
        for (u32 i = 0; i < S.chips.size(); ++i)
        {
            const float age = S.chips[i].rest ? S.chips[i].t + 1000.f : S.chips[i].t;
            if (age > bt)
                bt = age, best = i;
        }
        S.chips[best] = S.chips.back();
        S.chips.pop_back();
    }
    Chip c;
    c.p = p;
    c.v = v;
    c.axis.random_dir();
    c.ang = ::Random.randF(0.f, PI_MUL_2);
    c.w = ::Random.randF(4.f, 18.f) / (1.f + size);
    c.r = CHIP_R[size];
    c.life = life * ::Random.randF(0.7f, 1.3f);
    c.t = 0.f;
    c.model = u16((mat * 4 + size) * 4 + ::Random.randI(4));
    c.rest = false;
    S.chips.push_back(c);
}

// fragments off a hit spot: count and size by the round's damage
void chips_at(const Fvector& point, const Fvector& normal, const Fvector& dir, float dmg, u8 mat)
{
    if (mat >= NMAT || dmg < 1.f)
        return;
    Fvector n = normal;
    if (n.square_magnitude() < EPS)
        n.invert(dir);
    n.normalize_safe();
    // E56: more and bigger - a pistol's round (1) two crumbs, an assault rifle's (3-5) four or five with a fragment or
    // two, from 9 a big one, from 25 two, from 60 (a blast's) a chunk
    const int count = iFloor(_min(18.f, (2.f + dmg * 0.7f) * C.chip_k));
    for (int i = 0; i < count; ++i)
    {
        int size = 0;
        if (dmg >= 60.f)
            size = i < 1 ? 3 : (i < 3 ? 2 : (i < 7 ? 1 : 0));
        else if (dmg >= 25.f)
            size = i < 2 ? 2 : (i < 6 ? 1 : 0);
        else if (dmg >= 9.f)
            size = i < 1 ? 2 : (i < 4 ? 1 : 0);
        else if (dmg >= 3.f)
            size = i < (dmg >= 5.f ? 2 : 1) ? 1 : 0;
        Fvector d;
        d.random_dir(n, deg2rad(70.f));
        Fvector v;
        v.mul(d, ::Random.randF(2.f, 7.f) * (size == 3 ? 0.35f : size == 2 ? 0.5f : 1.f));
        Fvector p;
        p.mad(point, n, CHIP_R[size] + 0.02f);
        add_chip(mat, size, p, v, C.chip_life * (size >= 1 ? 1.6f : 1.f));
    }
    if (dmg >= 3.f) // E56: from an assault rifle's round on (was 9)
        play_fx(C.fx_heavy_hit, point, n);
}

// ------------------------------------------------------------------------------------------------------------- holes (E54b)
bool hole_contains(const Fvector& p, float k)
{
    for (const Hole& h : S.holes)
        if (h.r > 0.f && h.c.distance_to_sqr(p) < h.r * h.r * k * k)
            return true;
    return false;
}

// a round of `dmg` into a wall at `point` along `dir`: the wall's thickness behind it, the damage added up at the spot
void hole_hit(const Fvector& point, const Fvector& dir, float dmg, u8 mat, int pc)
{
    if (mat >= NMAT || dmg < C.hole_min_dmg)
        return;
    // the far face: a ray on from just inside the wall (the static model has both sides of its faces)
    collide::rq_result R;
    Fvector p0;
    p0.mad(point, dir, 0.012f);
    if (!Level().ObjectSpace.RayPick(p0, dir, C.hole_max_thick, collide::rqtStatic, R, nullptr) || R.element < 0)
        return; // thicker than hole_max_thick (or one-sided): chips only
    const float thick = R.range + 0.012f;
    if (thick < 0.03f)
        return;
    Fvector c;
    c.mad(point, dir, thick * 0.5f);
    Hole* h = nullptr;
    for (Hole& o : S.holes)
        if (o.c.distance_to(c) < _max(o.r, 0.14f) + 0.04f && o.piece == pc)
        {
            h = &o;
            break;
        }
    if (!h)
    {
        if ((int)S.holes.size() >= C.max_holes)
        {
            u32 oldest = 0;
            for (u32 i = 1; i < S.holes.size(); ++i)
                if (S.holes[i].time < S.holes[oldest].time)
                    oldest = i;
            S.holes.erase(S.holes.begin() + oldest);
        }
        Hole n;
        n.c = c;
        n.dir = dir;
        n.thick = thick;
        n.need = C.hole_need[mat] * thick;
        n.piece = pc;
        n.mat = mat;
        S.holes.push_back(n);
        h = &S.holes.back();
    }
    h->time = Device.dwTimeGlobal;
    h->dmg += dmg;
    if (h->dmg < h->need)
        return;
    const bool opened = h->r <= 0.f;
    const float rf = _min(C.hole_r_max, C.hole_r0 + C.hole_r_k * (h->dmg - h->need));
    const float half = h->thick * 0.5f + 0.03f;
    h->r = _sqrt(rf * rf + half * half);
    if (opened)
    {
        ++S.holes_open;
        const Hole hh = *h; // E56: the dents inside it go (it is through the wall there)
        S.dents.erase(std::remove_if(S.dents.begin(), S.dents.end(), [&hh](const Dent& d) { return d.piece == hh.piece && d.c.distance_to(hh.c) < hh.r; }),
                      S.dents.end());
        // the far side bursts out
        Fvector exit;
        exit.mad(h->c, h->dir, half);
        chips_at(exit, h->dir, h->dir, _max(dmg, 12.f), mat);
        Msg("* [destruct] a hole through a %.2f m wall (%s) at [%.2f, %.2f, %.2f], radius %.2f m on its faces", h->thick, MAT_KEY[mat], VPUSH(h->c), rf);
    }
}

// the holes for the shaders: the 32 open ones nearest the camera; a piece's hole goes when the piece leaves
void update_holes()
{
    S.holes.erase(std::remove_if(S.holes.begin(), S.holes.end(),
                                 [](const Hole& h) { return h.piece >= 0 && (h.piece >= (int)S.pieces.size() || S.pieces[h.piece].state != ST_INTACT); }),
                  S.holes.end());
    using HD = std::pair<float, u32>;
    xr_vector<HD> near_;
    const Fvector& cam = Device.vCameraPosition;
    for (u32 i = 0; i < S.holes.size(); ++i)
        if (S.holes[i].r > 0.f)
            near_.push_back({S.holes[i].c.distance_to_sqr(cam), i});
    std::sort(near_.begin(), near_.end());
    portal_mod_holes_near = !near_.empty() && near_[0].first < 90.f * 90.f;
    for (u32 k = 0; k < 32; ++k)
    {
        if (k < near_.size())
        {
            const Hole& h = S.holes[near_[k].second];
            portal_mod_holes[k].set(h.c.x, h.c.y, h.c.z, h.r);
        }
        else
            portal_mod_holes[k].set(0.f, 0.f, 0.f, 0.f);
    }
}

// ------------------------------------------------------------------------------------------------------------- dents (E56)
void make_dent_models()
{
    if (S.dent_models_made)
        return;
    S.dent_models_made = true;
    for (int m = 0; m < NMAT; ++m)
        for (int v = 0; v < DENT_VARS; ++v)
        {
            string_path n;
            xr_sprintf(n, "portal_mod\\destruct\\dents\\%s_%d", MAT_KEY[m], v);
            IRenderVisual* vis = ::Render->model_Create(n);
            if (!vis)
                Msg("! [destruct] no dent model %s: such dents are not drawn", n);
            S.dent_models.push_back(vis);
        }
}

// the bowl reaching out to rho all round is hidden behind the face or what stands on it: 8 ways out along the face - a
// ray 2 cm before it meets something first (a floor, a wall across: an inside corner, the bowl goes on behind it), or 5 cm
// out from the face at rho a ray finds the same face behind (at an outside edge it does not: the bowl would show)
bool dent_fits(const Fvector& c, const Fvector& n, const Fvector& t, float rho)
{
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector* V = Level().ObjectSpace.GetStaticVerts();
    Fvector b, back;
    b.crossproduct(n, t);
    back.invert(n);
    collide::rq_result R;
    for (int k = 0; k < 8; ++k)
    {
        const float a = float(k) * PI_DIV_4;
        Fvector u, s0;
        u.set(0.f, 0.f, 0.f).mad(t, _cos(a)).mad(b, _sin(a));
        s0.mad(c, n, 0.02f);
        if (Level().ObjectSpace.RayPick(s0, u, rho, collide::rqtStatic, R, nullptr) && R.element >= 0)
            continue; // an inside corner that way
        Fvector q;
        q.mad(c, u, rho).mad(n, 0.05f);
        if (!Level().ObjectSpace.RayPick(q, back, 0.1f, collide::rqtStatic, R, nullptr) || R.element < 0 || _abs(R.range - 0.05f) > 0.02f)
            return false;
        const CDB::TRI& tr = T[R.element];
        Fvector fn;
        fn.mknormal(V[tr.verts[0]], V[tr.verts[1]], V[tr.verts[2]]);
        if (_abs(fn.dotproduct(n)) < 0.9f)
            return false;
    }
    return true;
}

// the radius it may have, up to `want`: as big as the face round it holds (at a corner or an edge it stays smaller)
float dent_fit(Dent& d, float want)
{
    if (d.cap >= 0.f)
        return _min(want, d.cap);
    if (want <= d.checked)
        return want;
    for (float r = want; r >= 0.025f; r *= 0.8f)
        if (dent_fits(d.c, d.n, d.t, r * DENT_RIM))
        {
            if (r < want)
                d.cap = r;
            else
                d.checked = want;
            return r;
        }
    d.cap = 0.f;
    return 0.f;
}

void dent_grow(Dent& d)
{
    if (d.dmg < C.dent_need)
        return;
    const float want = _min(d.blast ? C.dent_r_max_blast : C.dent_r_max, C.dent_r0 * C.dent_scale[d.mat] * _sqrt(d.dmg / C.dent_need));
    if (want <= d.r + 0.002f)
        return;
    const float r = dent_fit(d, want);
    if (r <= d.r)
        return;
    d.r = r;
    d.depth = _max(0.012f, _min(C.dent_depth * r, 0.6f * d.thick));
}

// a round (or a blast) of `dmg` on a face at `point` coming along `dir`: added up at the spot, a dent past dent_need
void dent_hit(const Fvector& point, const Fvector& normal, const Fvector& dir, float dmg, u8 mat, int pc, bool blast)
{
    if (!C.dents || mat >= NMAT || dmg < (blast ? C.dent_need : C.dent_min_dmg) || !g_pGameLevel)
        return;
    if (pc >= 0 && (pc >= (int)S.pieces.size() || S.pieces[pc].state != ST_INTACT))
        return;
    if (hole_contains(point, 1.f))
        return; // an open hole there: through the wall
    Fvector n = normal;
    if (n.square_magnitude() < EPS)
        n.invert(dir);
    n.normalize_safe();
    if (n.dotproduct(dir) > 0.f)
        n.invert(); // the face it came at
    Dent* d = nullptr;
    float best = flt_max;
    for (Dent& o : S.dents)
    {
        if (o.piece != pc || o.n.dotproduct(n) < 0.7f)
            continue;
        Fvector v;
        v.sub(point, o.c);
        const float h = v.dotproduct(o.n);
        if (_abs(h) > 0.05f)
            continue;
        const float rr = _sqrt(_max(v.square_magnitude() - h * h, 0.f));
        const float reach = _max(o.r, 0.04f) * (blast ? DENT_RIM : 1.f) + C.dent_merge;
        if (rr < reach && rr < best)
        {
            best = rr;
            d = &o;
        }
    }
    if (!d)
    {
        if ((int)S.dents.size() >= _max(C.max_dents, 1))
        {
            u32 oldest = 0;
            for (u32 i = 1; i < S.dents.size(); ++i)
                if (S.dents[i].time < S.dents[oldest].time)
                    oldest = i;
            S.dents.erase(S.dents.begin() + oldest);
        }
        Dent nd;
        nd.c = point;
        nd.n = n;
        Fvector up;
        Fvector::generate_orthonormal_basis_normalized(nd.n, up, nd.t);
        nd.seed = ::Random.randF(0.f, PI_MUL_2);
        nd.mat = mat;
        nd.piece = pc;
        nd.blast = blast;
        nd.var = u8(blast ? 4 + ::Random.randI(2) : ::Random.randI(4));
        nd.id = ++S.dent_ids;
        // the wall behind it: the bowl goes no deeper than 0.6 of it (a ray on from just inside; a face alone - 1 m)
        collide::rq_result R;
        Fvector p0, back;
        back.invert(n);
        p0.mad(point, back, 0.012f);
        nd.thick = Level().ObjectSpace.RayPick(p0, back, 1.f, collide::rqtStatic, R, nullptr) && R.element >= 0 ? R.range + 0.012f : 1.f;
        S.dents.push_back(nd);
        d = &S.dents.back();
    }
    d->time = Device.dwTimeGlobal;
    d->dmg += dmg;
    if (blast && !d->blast)
    {
        d->blast = true;
        d->var = u8(4 + ::Random.randI(2));
    }
    dent_grow(*d);
}

// a blast on any level: the faces round it get a dent each at their point nearest it, the nearest few throw chunks
void blast_dents(const Fvector& pos, float radius, float hit)
{
    if (!C.dents || C.blast_dent_rays <= 0 || hit <= 0.f || !g_pGameLevel)
        return;
    const float range = _min(radius * C.blast_dent_reach, C.blast_dent_range);
    if (range < 0.2f)
        return;
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector* V = Level().ObjectSpace.GetStaticVerts();
    struct Spot
    {
        Fvector p, n;
        float d;
        u8 mat;
        int pc;
    };
    xr_vector<Spot> spots;
    collide::rq_result R;
    const int N = C.blast_dent_rays;
    for (int i = 0; i < N; ++i)
    {
        // evenly over the sphere (a Fibonacci spiral)
        const float y = 1.f - 2.f * (float(i) + 0.5f) / float(N);
        const float rr = _sqrt(_max(0.f, 1.f - y * y));
        const float a = float(i) * 2.39996323f;
        const Fvector dir{rr * _cos(a), y, rr * _sin(a)};
        if (!Level().ObjectSpace.RayPick(pos, dir, range, collide::rqtStatic, R, nullptr) || R.element < 0)
            continue;
        const CDB::TRI& tr = T[R.element];
        const int pc = piece_of_tri((u32)R.element);
        const u8 mat = pc >= 0 ? S.pieces[pc].mat : chip_class(tr.material);
        if (mat >= NMAT || (pc >= 0 && S.pieces[pc].state != ST_INTACT))
            continue;
        Fvector n;
        n.mknormal(V[tr.verts[0]], V[tr.verts[1]], V[tr.verts[2]]);
        if (n.dotproduct(dir) > 0.f)
            n.invert();
        Fvector hp;
        hp.mad(pos, dir, R.range);
        // the face's point nearest the blast, if the same face is there
        Fvector q = hp;
        const float dist = Fvector().sub(pos, hp).dotproduct(n);
        if (dist > 0.01f && dist < R.range - 0.01f)
        {
            Fvector back, foot;
            back.invert(n);
            foot.mad(pos, back, dist);
            collide::rq_result R2;
            if (Level().ObjectSpace.RayPick(pos, back, dist + 0.05f, collide::rqtStatic, R2, nullptr) && R2.element >= 0 && _abs(R2.range - dist) < 0.03f)
                q = foot;
        }
        const float d = pos.distance_to(q);
        bool dup = false;
        for (const Spot& o : spots)
            if (o.n.dotproduct(n) > 0.9f && o.p.distance_to(q) < 0.3f)
            {
                dup = true;
                break;
            }
        if (!dup)
            spots.push_back({q, n, d, mat, pc});
    }
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.d < b.d; });
    u32 made = 0;
    for (u32 k = 0; k < spots.size(); ++k)
    {
        const Spot& o = spots[k];
        const float f = 1.f - o.d / range;
        if (f <= 0.f)
            continue;
        const float dmg = hit * C.blast_dent_k * f * f;
        Fvector dir;
        dir.invert(o.n);
        const u32 before = (u32)S.dents.size();
        dent_hit(o.p, o.n, dir, dmg, o.mat, o.pc, true);
        made += (u32)S.dents.size() > before;
        if ((int)k < C.blast_dent_chips)
            chips_at(o.p, o.n, dir, _min(80.f, dmg * 0.12f), o.mat);
    }
    if (!spots.empty())
        Msg("* [destruct] a blast (%.1f, %.0f m) at [%.2f, %.2f, %.2f]: %u faces round it, %u dents made, the nearest %.2f m off", hit, radius, VPUSH(pos),
            (u32)spots.size(), made, spots[0].d);
}

// the dents for the shaders and their bowls: the dent_draw nearest the camera within dent_draw_dist, the nearest first
void update_dents()
{
    if (!S.dents.empty())
        S.dents.erase(std::remove_if(S.dents.begin(), S.dents.end(),
                                     [](const Dent& d) { return d.piece >= 0 && (d.piece >= (int)S.pieces.size() || S.pieces[d.piece].state != ST_INTACT); }),
                      S.dents.end());
    u32 k = 0, ng = 0;
    if (!S.dents.empty() && C.dent_draw > 0)
    {
        make_dent_models();
        using DD = std::pair<float, u32>;
        static xr_vector<DD> near_;
        near_.clear();
        const Fvector& cam = Device.vCameraPosition;
        const float far2 = C.dent_draw_dist * C.dent_draw_dist;
        for (u32 i = 0; i < S.dents.size(); ++i)
        {
            const Dent& d = S.dents[i];
            const u32 mi = u32(d.mat) * DENT_VARS + d.var;
            if (d.r <= 0.f || mi >= S.dent_models.size() || !S.dent_models[mi])
                continue;
            const float d2 = d.c.distance_to_sqr(cam);
            if (d2 < far2)
                near_.push_back({d2, i});
        }
        std::sort(near_.begin(), near_.end());
        k = _min((u32)near_.size(), (u32)C.dent_draw);
        if (S.dent_vis.size() < k)
            while (S.dent_vis.size() < (u32)C.dent_draw)
                S.dent_vis.push_back(xr_new<DentVis>());
        // a bowl keeps its dent while it is drawn (its light settled there); the new ones take the bowls let go
        auto drawn = [k](u32 id) {
            for (u32 i = 0; i < k; ++i)
                if (S.dents[near_[i].second].id == id)
                    return true;
            return false;
        };
        for (DentVis* v : S.dent_vis)
            if (v->dent && !drawn(v->dent))
                v->hide();
        for (u32 i = 0; i < k; ++i)
        {
            const Dent& d = S.dents[near_[i].second];
            DentVis* vis = nullptr;
            for (DentVis* v : S.dent_vis)
                if (v->dent == d.id)
                {
                    vis = v;
                    break;
                }
            if (!vis)
                for (DentVis* v : S.dent_vis)
                    if (!v->dent)
                    {
                        vis = v;
                        break;
                    }
            if (!vis)
                continue;
            Fvector b;
            b.crossproduct(d.n, d.t);
            Fmatrix m;
            m.identity();
            m.i.mul(d.t, d.r);
            m.j.mul(b, d.r * 1.0008f); // the shaders know a bowl by this (pm_dent_clip: darker the deeper)
            m.k.mul(d.n, d.depth / DENT_DEPTH);
            m.c.mad(d.c, d.n, -0.005f);
            Fvector front;
            front.mad(d.c, d.n, 0.1f);
            vis->place(S.dent_models[u32(d.mat) * DENT_VARS + d.var], m, front, d.r * 1.4f + 0.1f);
            vis->dent = d.id;
        }
        // to the shaders in groups of 8 slots: the nearest dent not placed yet and the 7 nearest to it, a sphere round them
        // (a pixel outside it skips them: 35 dents looped one by one over every pixel cost 4.5 ms a frame)
        bool taken[DENT_DRAW_MAX]{};
        for (u32 s = 0; s < k && ng < 6; ++s)
        {
            if (taken[s])
                continue;
            u32 mem[8];
            u32 nm = 0;
            mem[nm++] = s;
            taken[s] = true;
            const Fvector& cs = S.dents[near_[s].second].c;
            while (nm < 8)
            {
                int best = -1;
                float bd = flt_max;
                for (u32 q = 0; q < k; ++q)
                    if (!taken[q])
                    {
                        const float dq = S.dents[near_[q].second].c.distance_to_sqr(cs);
                        if (dq < bd)
                            bd = dq, best = (int)q;
                    }
                if (best < 0)
                    break;
                mem[nm++] = (u32)best;
                taken[best] = true;
            }
            Fvector gc{0.f, 0.f, 0.f};
            for (u32 j = 0; j < nm; ++j)
                gc.add(S.dents[near_[mem[j]].second].c);
            gc.div(float(nm));
            float gr = 0.f;
            for (u32 j = 0; j < nm; ++j)
            {
                const Dent& d = S.dents[near_[mem[j]].second];
                const float band = d.blast ? C.dent_band_blast : C.dent_band;
                // the shader's reach in the face, the face's own depth tolerance out of it
                gr = _max(gr, d.c.distance_to(gc) + d.r * (DENT_RIM + band) + 0.01f + 0.03f + 0.25f * d.r + 0.02f);
                const u32 i = ng * 8 + j;
                portal_mod_dents[0][i].set(d.c.x, d.c.y, d.c.z, d.r);
                portal_mod_dents[1][i].set(d.n.x, d.n.y, d.n.z, d.seed);
                portal_mod_dents[2][i].set(d.t.x, d.t.y, d.t.z, d.blast ? -C.dent_band_blast : C.dent_band); // < 0: soot
            }
            for (u32 j = nm; j < 8; ++j)
                portal_mod_dents[0][ng * 8 + j].set(0.f, 0.f, 0.f, 0.f); // the group's end
            portal_mod_dents_groups[ng].set(gc.x, gc.y, gc.z, gr);
            ++ng;
        }
    }
    else
        for (DentVis* v : S.dent_vis)
            if (v->dent)
                v->hide();
    if (ng < 6)
        portal_mod_dents_groups[ng].set(0.f, 0.f, 0.f, 0.f);
    portal_mod_dents_groups_count = ng;
    portal_mod_dents_count = ng * 8; // every slot of the groups (the empty ones zero)
    if (ng * 8 < DENT_DRAW_MAX)
        portal_mod_dents[0][ng * 8].set(0.f, 0.f, 0.f, 0.f);
    S.dents_drawn = k;
}

void update_chips(float dt)
{
    if (S.chips.empty() && S.chip_models.empty())
        return;
    collide::rq_result R;
    CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
    Fvector* V = Level().ObjectSpace.GetStaticVerts();
    for (u32 i = 0; i < S.chips.size();)
    {
        Chip& c = S.chips[i];
        c.t += dt;
        if (c.rest)
        {
            if (c.t > c.life)
                c.p.y -= 0.12f * dt; // sinks into the ground
            if (c.t > c.life + 4.f)
            {
                c = S.chips.back();
                S.chips.pop_back();
                continue;
            }
            ++i;
            continue;
        }
        c.v.y -= 9.81f * dt;
        Fvector d;
        d.mul(c.v, dt);
        const float len = d.magnitude();
        if (len > 1e-5f)
        {
            Fvector dir;
            dir.div(d, len);
            if (Level().ObjectSpace.RayPick(c.p, dir, len + c.r, collide::rqtStatic, R, nullptr) && R.element >= 0 &&
                !hole_contains(Fvector().mad(c.p, dir, R.range), 0.95f))
            {
                const CDB::TRI& t = T[R.element];
                Fvector n;
                n.mknormal(V[t.verts[0]], V[t.verts[1]], V[t.verts[2]]);
                if (n.dotproduct(dir) > 0.f)
                    n.invert();
                c.p.mad(dir, _max(0.f, R.range - c.r));
                Fvector vn;
                vn.mul(n, c.v.dotproduct(n));
                Fvector vt;
                vt.sub(c.v, vn);
                c.v.mul(vt, 0.55f);
                c.v.mad(vn, -0.3f);
                c.w *= 0.6f;
                if (c.v.magnitude() < 0.6f && n.y > 0.5f)
                {
                    c.rest = true;
                    c.t = 0.f;
                    c.v.set(0.f, 0.f, 0.f);
                }
            }
            else
                c.p.add(d);
        }
        c.ang += c.w * dt;
        if (c.p.y < -300.f || c.t > 60.f)
        {
            c = S.chips.back();
            S.chips.pop_back();
            continue;
        }
        ++i;
    }
    for (ChipModel* m : S.chip_models)
        m->xf.clear();
    for (const Chip& c : S.chips)
    {
        if (c.model >= S.chip_models.size())
            continue;
        Fmatrix m;
        m.rotation(c.axis, c.ang);
        m.c = c.p;
        S.chip_models[c.model]->xf.push_back(m);
    }
    for (ChipModel* m : S.chip_models)
        m->commit();
}

// rubble of a destroyed piece: chunks from inside its boxes
void rubble(const Piece& p, const Fmatrix& xf, const Fvector& push_dir, float push, const Fvector& v0)
{
    const int nbig = clampr(iFloor(1.f + p.vol * 6.f), 1, 10);
    const int nsmall = clampr(iFloor(3.f + p.vol * 10.f), 3, 14);
    for (int i = 0; i < nbig + nsmall; ++i)
    {
        const Box& b = p.boxes.empty() ? Box{{0, 0, 0}, {0.2f, 0.2f, 0.2f}} : p.boxes[::Random.randI(p.boxes.size())];
        Fvector l;
        l.set(b.c.x + ::Random.randF(-b.h.x, b.h.x), b.c.y + ::Random.randF(-b.h.y, b.h.y), b.c.z + ::Random.randF(-b.h.z, b.h.z));
        Fvector w;
        xf.transform_tiny(w, l);
        Fvector v;
        v.random_dir();
        v.mul(::Random.randF(0.5f, 2.5f));
        v.y = _abs(v.y) + 0.5f;
        v.mad(push_dir, push * ::Random.randF(0.3f, 1.f));
        v.add(v0);
        const int size = i < nbig ? (p.vol > 0.25f && ::Random.randI(3) == 0 ? 3 : 2) : ::Random.randI(2);
        add_chip(p.mat, size, w, v, size >= 2 ? C.rubble_life : C.chip_life);
    }
}

// ------------------------------------------------------------------------------------------------------------- pieces
Fmatrix piece_xform(const Piece& p)
{
    if (p.state == ST_FALLING && p.island >= 0 && p.island < (int)S.islands.size() && S.islands[p.island])
    {
        const Fmatrix& M = S.islands[p.island]->xf;
        Fmatrix m = M;
        M.transform_tiny(m.c, p.local);
        return m;
    }
    return rest_xform(p);
}

// E54e: the bonds within cascade_r across of a standing piece destroyed may break overloaded from now on
void mark_zone(const Piece& p)
{
    const float r2 = C.cascade_r * C.cascade_r;
    for (Bond& b : S.bonds)
    {
        if (b.zone || b.broken)
            continue;
        const float dx = _max(0.f, _max(p.aabb.min.x - b.pt.x, b.pt.x - p.aabb.max.x));
        const float dz = _max(0.f, _max(p.aabb.min.z - b.pt.z, b.pt.z - p.aabb.max.z));
        b.zone = dx * dx + dz * dz <= r2;
    }
}

void destroy_piece(u16 pc, const Fvector& dir, float push)
{
    Piece& p = S.pieces[pc];
    if (p.state == ST_GONE)
        return;
    if (p.state == ST_INTACT && C.cascade_r > 0.f)
        mark_zone(p);
    const Fmatrix xf = piece_xform(p);
    Fvector v0{};
    if (p.state == ST_INTACT)
        drop_tris(p);
    else if (p.island >= 0 && S.islands[p.island] && S.islands[p.island]->shell)
        S.islands[p.island]->shell->get_LinearVel(v0);
    p.state = ST_GONE;
    if (p.pv)
        p.pv->hide();
    swap_to_pieces();
    rubble(p, xf, dir, push, v0);
    Fvector up{0.f, 1.f, 0.f};
    play_fx(C.fx_destroy[p.mat], xf.c, up);
    if (fx_allowed())
        play_snd(S.snd_destroy, xf.c, _min(1.f, 0.4f + p.vol));
    for (u32 bi : p.bonds)
        S.bonds[bi].broken = true;
    S.dirty = true;
}

void damage_piece(u16 pc, float dmg, const Fvector& dir, float push)
{
    Piece& p = S.pieces[pc];
    if (p.state != ST_INTACT || dmg <= 0.f)
        return;
    p.hp -= dmg;
    if (p.hp <= 0.f)
        destroy_piece(pc, dir, push);
    else if (p.hp_solved - p.hp > 0.15f * p.hp_max)
        S.dirty = true;
}

void contact_cb(CDB::TRI* T, dContactGeom* c)
{
    Island* I = nullptr;
    for (dGeomID g : {c->g1, c->g2})
    {
        if (!g)
            continue;
        dxGeomUserData* ud = retrieveGeomUserData(g);
        if (ud && ud->callback_data && static_cast<Island*>(ud->callback_data)->magic == ISLAND_MAGIC)
        {
            I = static_cast<Island*>(ud->callback_data);
            break;
        }
    }
    if (!I)
        return;
    I->touched_static = true;
    const int pc = piece_of_tri(u32(T - Level().ObjectSpace.GetStaticTris()));
    if (pc >= 0 && S.pieces[pc].state == ST_INTACT && std::find(I->touched.begin(), I->touched.end(), u16(pc)) == I->touched.end() && I->touched.size() < 16)
        I->touched.push_back(u16(pc));
}

int new_island_slot()
{
    for (u32 i = 0; i < S.islands.size(); ++i)
        if (!S.islands[i])
            return (int)i;
    S.islands.push_back(nullptr);
    return (int)S.islands.size() - 1;
}

// E54e: a big body's collision - its pieces' boxes (all square to the building's axes, in the body's frame) marked on a
// coarse grid and the marked cells merged into as few boxes as cover them, each cut back to the pieces' own boxes inside
// it. A body of 600 pieces is 1500 boxes against the level and the bodies beside it: 10-30 ms of collision a frame
void merge_boxes(const xr_vector<Fobb>& in, xr_vector<Fobb>& out)
{
    Fvector lo{flt_max, flt_max, flt_max}, hi{-flt_max, -flt_max, -flt_max};
    for (const Fobb& b : in)
    {
        lo.min(Fvector().sub(b.m_translate, b.m_halfsize));
        hi.max(Fvector().add(b.m_translate, b.m_halfsize));
    }
    Fvector ext;
    ext.sub(hi, lo);
    const float g = _max(C.merge_cell, _max(ext.x, _max(ext.y, ext.z)) / 40.f);
    const int n[3] = {_max(1, iCeil(ext.x / g)), _max(1, iCeil(ext.y / g)), _max(1, iCeil(ext.z / g))};
    auto at = [&n](int x, int y, int z) { return (size_t(z) * n[1] + y) * n[0] + x; };
    xr_vector<u8> cell(size_t(n[0]) * n[1] * n[2], 0); // 1 marked, 2 taken
    // the cells a box covers along an axis: those it covers by 30% or more; a box thinner than a cell - the one its middle is in
    auto range = [g](float a0, float a1, float l0, int nn, int& i0, int& i1) {
        const float a = (a0 - l0) / g, b = (a1 - l0) / g;
        if (b - a < 1.f)
        {
            i0 = i1 = clampr(iFloor((a + b) * 0.5f), 0, nn - 1);
            return;
        }
        i0 = clampr(iFloor(a + 0.3f), 0, nn - 1);
        i1 = clampr(iCeil(b - 0.3f) - 1, i0, nn - 1);
    };
    for (const Fobb& b : in)
    {
        int r[3][2];
        for (int a = 0; a < 3; ++a)
            range(b.m_translate[a] - b.m_halfsize[a], b.m_translate[a] + b.m_halfsize[a], lo[a], n[a], r[a][0], r[a][1]);
        for (int z = r[2][0]; z <= r[2][1]; ++z)
            for (int y = r[1][0]; y <= r[1][1]; ++y)
                for (int x = r[0][0]; x <= r[0][1]; ++x)
                    cell[at(x, y, z)] = 1;
    }
    auto free_row = [&](int x0, int x1, int y, int z) {
        for (int x = x0; x <= x1; ++x)
            if (cell[at(x, y, z)] != 1)
                return false;
        return true;
    };
    for (int z = 0; z < n[2]; ++z)
        for (int y = 0; y < n[1]; ++y)
            for (int x = 0; x < n[0]; ++x)
            {
                if (cell[at(x, y, z)] != 1)
                    continue;
                int x1 = x, z1 = z, y1 = y;
                while (x1 + 1 < n[0] && cell[at(x1 + 1, y, z)] == 1)
                    ++x1;
                while (z1 + 1 < n[2] && free_row(x, x1, y, z1 + 1))
                    ++z1;
                for (bool grow = true; grow && y1 + 1 < n[1];)
                {
                    for (int zz = z; zz <= z1 && grow; ++zz)
                        grow = free_row(x, x1, y1 + 1, zz);
                    if (grow)
                        ++y1;
                }
                for (int zz = z; zz <= z1; ++zz)
                    for (int yy = y; yy <= y1; ++yy)
                        for (int xx = x; xx <= x1; ++xx)
                            cell[at(xx, yy, zz)] = 2;
                // the cells' box, cut back to what the pieces' own boxes fill of it
                Fvector c0{lo.x + x * g, lo.y + y * g, lo.z + z * g}, c1{lo.x + (x1 + 1) * g, lo.y + (y1 + 1) * g, lo.z + (z1 + 1) * g};
                Fvector t0{flt_max, flt_max, flt_max}, t1{-flt_max, -flt_max, -flt_max};
                for (const Fobb& b : in)
                {
                    Fvector b0, b1;
                    b0.sub(b.m_translate, b.m_halfsize).max(c0);
                    b1.add(b.m_translate, b.m_halfsize).min(c1);
                    if (b0.x < b1.x && b0.y < b1.y && b0.z < b1.z)
                    {
                        t0.min(b0);
                        t1.max(b1);
                    }
                }
                if (t0.x > t1.x)
                    continue;
                Fobb o;
                o.m_rotate.identity();
                o.m_translate.add(t0, t1).mul(0.5f);
                o.m_halfsize.sub(t1, t0).mul(0.5f);
                out.push_back(o);
            }
}

// pieces (already out of the collision, their `local` in the frame F) fall as one body
void make_island(const xr_vector<u16>& pcs, const Fmatrix& F, const Fvector& v, const Fvector& w)
{
    if (pcs.empty())
        return;
    const int slot = new_island_slot();
    Island* I = xr_new<Island>();
    S.islands[slot] = I;
    I->pieces = pcs;
    CPhysicsElement* E = P_create_Element();
    static xr_vector<Fobb> raw, merged;
    raw.clear();
    merged.clear();
    for (u16 pc : pcs)
    {
        Piece& p = S.pieces[pc];
        p.state = ST_FALLING;
        p.island = slot;
        I->mass += p.mass;
        for (const Box& b : p.boxes)
        {
            Fobb o;
            o.m_rotate.identity();
            o.m_translate.add(p.local, b.c);
            o.m_halfsize = b.h;
            raw.push_back(o);
            I->radius = _max(I->radius, o.m_translate.magnitude() + b.h.magnitude()); // E54e
        }
    }
    if (C.merge_boxes > 0 && (int)raw.size() > C.merge_boxes) // E54e
        merge_boxes(raw, merged);
    for (Fobb& o : merged.empty() ? raw : merged)
    {
        o.m_halfsize.set(_max(0.03f, o.m_halfsize.x - C.margin), _max(0.03f, o.m_halfsize.y - C.margin), _max(0.03f, o.m_halfsize.z - C.margin));
        E->add_Box(o);
    }
    if (!merged.empty() && raw.size() > 200)
        Msg("[destruct] body of %u pieces: %u boxes merged into %u", (u32)pcs.size(), (u32)raw.size(), (u32)merged.size());
    I->shell = P_create_Shell();
    I->shell->add_Element(E);
    I->shell->setMass(_max(I->mass, 1.f));
    I->shell->SetMaterial("objects\\concrete_box");
    I->shell->set_ContactCallback(&contact_cb);
    I->shell->set_CallbackData(I);
    I->shell->Activate(F, v, w, false);
    I->xf = F;
    I->v_prev = v;
    I->has_prev = true;
    for (u16 pc : pcs)
        if (S.pieces[pc].pv && S.swapped)
            S.pieces[pc].pv->place(piece_xform(S.pieces[pc]));
}

// intact pieces let go: out of the collision, into an island in the building's frame (velocity from a blast nearby)
void release(const xr_vector<u16>& pcs)
{
    swap_to_pieces();
    Fvector c{};
    float m = 0.f;
    for (u16 pc : pcs)
    {
        c.mad(S.pieces[pc].pivot, S.pieces[pc].mass);
        m += S.pieces[pc].mass;
    }
    c.div(_max(m, 1.f));
    Fmatrix F = S.rot;
    F.c = c;
    Fmatrix Fi;
    Fi.invert(F);
    for (u16 pc : pcs)
    {
        Piece& p = S.pieces[pc];
        drop_tris(p);
        Fi.transform_tiny(p.local, p.pivot);
    }
    Fvector v{}, w{};
    // E54d: nothing let go sits perfectly balanced on what is left under it - a little rocking about a level axis; a
    // stack that broke tipping over is set going over its edge, the way its moment pushed it (what tips it only if its
    // middle is high over a narrow contact: a squat piece rocks back)
    const Fvector up{0.f, 1.f, 0.f};
    Fvector ax{::Random.randF(-1.f, 1.f), 0.f, ::Random.randF(-1.f, 1.f)};
    ax.normalize_safe();
    w.mul(ax, C.release_w * ::Random.randF(0.5f, 1.f));
    Fvector tip{};
    for (u16 pc : pcs)
        for (u32 bi : S.pieces[pc].bonds)
        {
            const Bond& b = S.bonds[bi];
            if (b.broken && b.toppled && Device.dwTimeGlobal - b.t_broke < 3000)
                tip.mad(b.tip, b.m);
        }
    if (tip.magnitude() > 1.f)
    {
        tip.y = 0.f;
        tip.normalize_safe();
        Fvector a;
        a.crossproduct(up, tip);
        w.mad(a, C.tip_w);
    }
    if (S.blast_frame == Device.dwFrame)
    {
        Fvector d;
        d.sub(c, S.blast_pos);
        const float dist = d.magnitude();
        if (dist < S.blast_r * 1.5f)
        {
            d.normalize_safe();
            v.mul(d, S.blast_push * (1.f - dist / (S.blast_r * 1.5f)) / _max(1.f, _sqrt(m / 1000.f)));
            Fvector r;
            r.random_dir();
            w.mad(r, ::Random.randF(0.2f, 1.2f));
        }
    }
    make_island(pcs, F, v, w);
}

void kill_island(int slot)
{
    Island* I = S.islands[slot];
    if (!I)
        return;
    if (I->shell)
        destroy_physics_shell(I->shell);
    for (u16 pc : I->pieces)
    {
        Piece& p = S.pieces[pc];
        if (p.island == slot)
        {
            p.island = -1;
            if (p.state == ST_FALLING)
                p.state = ST_GONE;
            if (p.pv)
                p.pv->hide();
        }
    }
    xr_delete(I);
    S.islands[slot] = nullptr;
}

// E54d: a falling piece breaks up where it is (xf) into rubble moving with it
void crumble(u16 pc, const Fmatrix& xf, const Fvector& v)
{
    Piece& p = S.pieces[pc];
    p.state = ST_GONE;
    p.island = -1;
    if (p.pv)
        p.pv->hide();
    const Fvector up{0.f, 1.f, 0.f};
    rubble(p, xf, up, 0.5f, v);
    play_fx(C.fx_destroy[p.mat], xf.c, up);
    for (u32 bi : p.bonds)
        S.bonds[bi].broken = true;
}

// an island after a hard landing: its pieces still bonded together go on as islands of their own
void split_island(int slot)
{
    Island* I = S.islands[slot];
    Fmatrix M = I->xf;
    Fvector v{}, w{};
    I->shell->get_LinearVel(v);
    I->shell->get_AngularVel(w);
    xr_vector<u16> left;
    for (u16 pc : I->pieces)
        if (S.pieces[pc].state == ST_FALLING && S.pieces[pc].island == slot)
            left.push_back(pc);
    destroy_physics_shell(I->shell);
    xr_delete(I);
    S.islands[slot] = nullptr;
    // components over the unbroken bonds inside
    xr_map<u16, int> comp;
    for (u16 pc : left)
        comp[pc] = -1;
    int nc = 0;
    for (u16 start : left)
    {
        if (comp[start] >= 0)
            continue;
        xr_vector<u16> q{start};
        comp[start] = nc;
        for (u32 qi = 0; qi < q.size(); ++qi)
            for (u32 bi : S.pieces[q[qi]].bonds)
            {
                const Bond& b = S.bonds[bi];
                if (b.broken)
                    continue;
                const u16 o = b.a == q[qi] ? b.b : b.a;
                auto it = comp.find(o);
                if (it != comp.end() && it->second < 0)
                {
                    it->second = nc;
                    q.push_back(o);
                }
            }
        ++nc;
    }
    // E54d: a whole building coming down had 100+ bodies moving at once and frames of 100 ms (take destruct_15) - past
    // max_moving a small part (two pieces, under 0.8 m3) breaks up into rubble on landing instead
    u32 moving = 0;
    for (const Island* J : S.islands)
        moving += J && J->shell && J->shell->isEnabled();
    for (int k = 0; k < nc; ++k)
    {
        xr_vector<u16> pcs;
        Fvector lc{};
        float m = 0.f, vol = 0.f;
        for (u16 pc : left)
            if (comp[pc] == k)
            {
                pcs.push_back(pc);
                lc.mad(S.pieces[pc].local, S.pieces[pc].mass);
                m += S.pieces[pc].mass;
                vol += S.pieces[pc].vol;
            }
        if ((int)moving >= C.max_moving && (int)pcs.size() <= C.crumble_pieces && vol < C.crumble_vol) // E54e: was 2 pieces, 0.8 m3
        {
            for (u16 pc : pcs)
            {
                Fmatrix xf = M;
                M.transform_tiny(xf.c, S.pieces[pc].local);
                crumble(pc, xf, v);
            }
            S.dirty = true;
            continue;
        }
        ++moving;
        lc.div(_max(m, 1.f));
        Fmatrix F = M;
        M.transform_tiny(F.c, lc);
        for (u16 pc : pcs)
            S.pieces[pc].local.sub(lc);
        Fvector r;
        r.sub(F.c, M.c);
        Fvector vk;
        vk.crossproduct(w, r);
        vk.add(v);
        make_island(pcs, F, vk, w);
    }
}

void impact(int slot, float dv)
{
    Island* I = S.islands[slot];
    const float E = 0.5f * I->mass * dv * dv;
    Fvector up{0.f, 1.f, 0.f};
    Fvector down{0.f, -1.f, 0.f};
    if (!I->touched.empty())
    {
        const float each = C.imp_k * E / I->touched.size();
        for (u16 pc : I->touched)
            damage_piece(pc, each, down, 1.f);
    }
    play_fx(C.fx_impact, I->xf.c, up);
    if (E > 2.0e4f && fx_allowed())
        play_snd(S.snd_impact, I->xf.c, _min(1.f, E / 2.0e5f));
    if (E > 3.0e5f && S.snd_collapse._handle() && Device.dwTimeGlobal > S.collapse_snd_time)
    {
        S.collapse_snd_time = Device.dwTimeGlobal + 6000;
        Fvector p = I->xf.c;
        float vol = 1.f;
        S.snd_collapse.play_no_feedback(nullptr, 0, 0.f, &p, &vol);
    }
    // its own pieces: damage, and the bonds inside break with a chance that grows with the hit
    bool changed = false;
    for (u16 pc : I->pieces)
    {
        Piece& p = S.pieces[pc];
        if (p.state != ST_FALLING || p.island != slot)
            continue;
        p.hp -= C.imp_self_k * 0.5f * p.mass * dv * dv;
        if (p.hp <= 0.f)
        {
            destroy_piece(pc, up, 1.f); // (does not touch the islands)
            changed = true;
        }
    }
    const float break_dv = (int)I->pieces.size() > C.big_body ? C.break_dv * 0.5f : C.break_dv; // E54e
    if (dv > break_dv)
    {
        const float chance = clampr((dv - break_dv) / 5.f + 0.25f, 0.f, 1.f);
        for (u16 pc : I->pieces)
            for (u32 bi : S.pieces[pc].bonds)
            {
                Bond& b = S.bonds[bi];
                if (!b.broken && b.a == pc && ::Random.randF() < chance)
                {
                    b.broken = true;
                    changed = true;
                }
            }
    }
    if (I->glued) // E54e
        changed = true;
    I->touched.clear();
    I->touched_static = false;
    if (changed)
        split_island(slot);
}

// E54e: a falling body reaching a living creature hits it - its boxes (in its own frame) against three points up the
// creature's middle, grown by the creature's girth; what counts is the speed of the body at that point
bool crush_immune(CObject* o)
{
    if (std::find(g_crush_immune.begin(), g_crush_immune.end(), o->ID()) != g_crush_immune.end())
        return true;
    for (const shared_str& sec : C.crush_immune)
        if (sec == o->cNameSect())
            return true;
    return false;
}

void crush(int slot)
{
    Island* I = S.islands[slot];
    if (!C.crush || !I->shell || !I->shell->isEnabled())
        return;
    Fvector v, w;
    I->shell->get_LinearVel(v);
    I->shell->get_AngularVel(w);
    const bool heavy = I->mass >= C.crush_pin_mass;
    if (v.magnitude() + w.magnitude() * I->radius < (heavy ? 0.3f : C.crush_v))
        return;
    static xr_vector<CObject*> near_objs;
    near_objs.clear();
    Level().ObjectSpace.GetNearest(near_objs, I->xf.c, I->radius + 2.5f, nullptr);
    if (near_objs.empty())
        return;
    Fmatrix Mi;
    Mi.invert(I->xf);
    CActor* actor = Actor();
    static u32 debug_t{};
    const bool debug = C.crush_debug && Device.dwTimeGlobal >= debug_t;
    float dbg_gap = flt_max, dbg_h = 0.f;
    CObject* dbg_o = nullptr;
    for (CObject* o : near_objs)
    {
        CEntityAlive* e = smart_cast<CEntityAlive*>(o);
        if (!e || !e->g_Alive() || o->getDestroy() || crush_immune(o))
            continue;
        if (o == actor && !C.crush_actor)
            continue;
        auto cd = std::find_if(S.crush_next.begin(), S.crush_next.end(), [o](const std::pair<u16, u32>& c) { return c.first == o->ID(); });
        if (cd != S.crush_next.end() && Device.dwTimeGlobal < cd->second)
            continue;
        // a standing creature from 0.3 m up to the top of his head (his bounding sphere's diameter, a man 1.9 m), each
        // point grown by his girth: the boxes reach him a little before his capsule would stop them
        const float girth = clampr(o->Radius() * 0.45f, 0.25f, 0.45f) + (heavy ? C.crush_reach : 0.f);
        const float top = clampr(o->Radius() * 2.f - 0.1f, 0.5f, 1.9f);
        float gap = flt_max;
        Fvector at{};
        for (int k = 0; k < 4; ++k)
        {
            Fvector P = o->Position();
            P.y += 0.3f + (top - 0.3f) * k / 3.f;
            Fvector l;
            Mi.transform_tiny(l, P);
            if (l.magnitude() > I->radius + girth + 2.f)
                continue;
            for (u16 pc : I->pieces)
            {
                const Piece& p = S.pieces[pc];
                if (p.state != ST_FALLING || p.island != slot)
                    continue;
                for (const Box& b : p.boxes)
                {
                    const Fvector c = Fvector().add(p.local, b.c);
                    const float g = _max(_abs(l.x - c.x) - b.h.x, _max(_abs(l.y - c.y) - b.h.y, _abs(l.z - c.z) - b.h.z)) - girth;
                    if (g < gap)
                        gap = g, at = P;
                }
            }
        }
        if (debug && gap < dbg_gap)
            dbg_gap = gap, dbg_o = o, dbg_h = at.y - o->Position().y;
        if (gap > 0.f)
            continue;
        Fvector vp;
        vp.crossproduct(w, Fvector().sub(at, I->xf.c));
        vp.add(v);
        float sp = vp.magnitude();
        if (heavy)
            sp = _max(sp, C.crush_v); // it comes to rest on him
        if (sp < C.crush_v)
            continue;
        const float m = _min(I->mass, C.crush_mass);
        const float E = 0.5f * m * sp * sp;
        Fvector dir = vp;
        if (dir.square_magnitude() < 0.01f)
            dir.set(0.f, -1.f, 0.f);
        dir.normalize_safe();
        SHit HS;
        HS.GenHeader(GE_HIT, o->ID());
        HS.whoID = C.crush_blame_actor && actor ? actor->ID() : o->ID();
        HS.weaponID = HS.whoID;
        HS.dir = dir;
        HS.power = C.crush_k * E;
        if (E >= C.crush_lethal) // what would kill a man outright kills whatever his outfit, rank or kind
            HS.power = _max(HS.power, 4.f / _max(e->conditions().GetHitImmunity(C.crush_type), 0.02f));
        HS.p_in_bone_space = at;
        HS.impulse = _min(600.f, C.crush_impulse * m * sp);
        HS.hit_type = C.crush_type;
        HS.boneID = 0;
        NET_Packet P;
        HS.Write_Packet(P);
        CGameObject::u_EventSend(P);
        ++S.crushed;
        S.crush_kills += HS.power >= e->GetfHealth();
        Msg("[destruct] crush: %s (id %u, health %.2f) hit by a body of %.0f kg at %.1f m/s (%.1f m up him) - %.0f J, power %.2f", o->cName().c_str(),
            o->ID(), e->GetfHealth(), I->mass, sp, at.y - o->Position().y, E, HS.power);
        if (cd != S.crush_next.end())
            cd->second = Device.dwTimeGlobal + 300;
        else
            S.crush_next.emplace_back(o->ID(), Device.dwTimeGlobal + 300);
    }
    if (debug && dbg_o)
    {
        debug_t = Device.dwTimeGlobal + 250;
        Msg("[destruct] crush check: body %d (%.0f kg, %u pieces, r %.1f, v %.1f) - %s %.2f m off (at %.1f m up him)", slot, I->mass,
            (u32)I->pieces.size(), I->radius, v.magnitude(), dbg_o->cName().c_str(), dbg_gap, dbg_h);
    }
}

void update_islands()
{
    for (u32 slot = 0; slot < S.islands.size(); ++slot)
    {
        Island* I = S.islands[slot];
        if (!I)
            continue;
        bool alive = false;
        for (u16 pc : I->pieces)
            if (S.pieces[pc].state == ST_FALLING && S.pieces[pc].island == (int)slot)
            {
                alive = true;
                break;
            }
        if (!alive || !I->shell)
        {
            kill_island(slot);
            continue;
        }
        Fmatrix M;
        I->shell->InterpolateGlobalTransform(&M);
        if (!_valid(M) || M.c.y < -300.f)
        {
            kill_island(slot);
            continue;
        }
        I->xf = M;
        for (u16 pc : I->pieces)
        {
            Piece& p = S.pieces[pc];
            if (p.state == ST_FALLING && p.island == (int)slot && p.pv)
                p.pv->place(piece_xform(p));
        }
        if (portal_mod_feat::enabled(portal_mod_feat::destruct)) // portal-mod E55: switched off - what still falls lands, crushing no one, breaking on nothing
            crush((int)slot); // E54e
        Fvector v;
        I->shell->get_LinearVel(v);
        const float dv = I->has_prev ? Fvector().sub(v, I->v_prev).magnitude() : 0.f;
        I->v_prev = v;
        I->has_prev = true;
        if (portal_mod_feat::enabled(portal_mod_feat::destruct) && dv > ((int)I->pieces.size() > C.big_body ? C.imp_dv * 0.5f : C.imp_dv) && I->touched_static) // E54e: a big body half; portal-mod E55: switched off, no break-up
        {
            impact((int)slot, dv);
            continue; // the island was split (a new one, maybe in this slot)
        }
        I->touched.clear();
        I->touched_static = false;
    }
}

// ------------------------------------------------------------------------------------------------------------- solver
// dist: shortest path to the ground over unbroken bonds between intact pieces (vertical steps count little)
void distances(xr_vector<float>& dist)
{
    const u32 n = (u32)S.pieces.size();
    dist.assign(n, flt_max);
    using QE = std::pair<float, u16>;
    std::priority_queue<QE, xr_vector<QE>, std::greater<QE>> q;
    for (u32 i = 0; i < n; ++i)
        if (S.pieces[i].state == ST_INTACT && S.pieces[i].anchored)
        {
            dist[i] = 0.f;
            q.push({0.f, u16(i)});
        }
    while (!q.empty())
    {
        const auto [d, i] = q.top();
        q.pop();
        if (d > dist[i])
            continue;
        for (u32 bi : S.pieces[i].bonds)
        {
            const Bond& b = S.bonds[bi];
            if (b.broken)
                continue;
            const u16 j = b.a == i ? b.b : b.a;
            if (S.pieces[j].state != ST_INTACT)
                continue;
            const Fvector& a = S.pieces[i].pivot;
            const Fvector& c = S.pieces[j].pivot;
            const float step = b.vertical ? 0.1f + 0.1f * _abs(a.y - c.y) : 0.2f + _sqrt(_sqr(a.x - c.x) + _sqr(a.z - c.z));
            if (d + step < dist[j])
            {
                dist[j] = d + step;
                q.push({dist[j], j});
            }
        }
    }
}

// loads down the paths: bond forces and moments (N, N*m)
void loads(const xr_vector<float>& dist)
{
    const u32 n = (u32)S.pieces.size();
    xr_vector<u16> order;
    for (u32 i = 0; i < n; ++i)
        if (S.pieces[i].state == ST_INTACT && dist[i] < flt_max)
            order.push_back(u16(i));
    std::sort(order.begin(), order.end(), [&](u16 a, u16 b) { return dist[a] > dist[b]; });
    xr_vector<float> L(n, 0.f), Mo(n, 0.f);
    // E54d, sideways: the blast's push (horizontal force, and its sum of force x height - the moment about any height y
    // is Q - y H) and the lean, worst case along each of the building's axes on its own (hx, qx; hz, qz)
    const bool blast = S.blast_frame == Device.dwFrame && S.blast_h.size() == n;
    xr_vector<Fvector> Hb(n, Fvector{}), Qb(n, Fvector{});
    xr_vector<float> hx(n, 0.f), qx(n, 0.f), hz(n, 0.f), qz(n, 0.f);
    for (Bond& b : S.bonds)
    {
        b.f = b.m = 0.f;
        b.hang = false;
    }
    const float g = 9.81f;
    for (u16 i : order)
    {
        const Piece& p = S.pieces[i];
        L[i] += p.mass * g;
        if (blast)
        {
            Hb[i].add(S.blast_h[i]);
            Qb[i].mad(S.blast_h[i], p.pivot.y);
        }
        const float lean = C.sway * p.mass * g;
        hx[i] += lean;
        qx[i] += lean * p.pivot.y;
        hz[i] += lean;
        qz[i] += lean * p.pivot.y;
        // braced: a side bond to a standing piece takes what pushes along it
        for (u32 bi : p.bonds)
        {
            const Bond& b = S.bonds[bi];
            const u16 j = b.a == i ? b.b : b.a;
            if (b.broken || b.vertical || S.pieces[j].state != ST_INTACT || dist[j] == flt_max)
                continue;
            Fvector e;
            e.sub(S.pieces[j].pivot, p.pivot);
            const float dy = _abs(e.y);
            e.y = 0.f;
            if (e.magnitude() < _max(dy, 1e-3f))
                continue; // more above or under it than beside: no brace
            e.normalize();
            Hb[i].mad(e, -Hb[i].dotproduct(e));
            Qb[i].mad(e, -Qb[i].dotproduct(e));
            const float ex = 1.f - _sqr(e.dotproduct(S.rot.i)), ez = 1.f - _sqr(e.dotproduct(S.rot.k));
            hx[i] *= ex;
            qx[i] *= ex;
            hz[i] *= ez;
            qz[i] *= ez;
        }
        float wsum = 0.f, vsum = 0.f; // vsum: the contacts under it (what pushes sideways goes down those)
        for (u32 bi : p.bonds)
        {
            const Bond& b = S.bonds[bi];
            const u16 j = b.a == i ? b.b : b.a;
            if (!b.broken && S.pieces[j].state == ST_INTACT && dist[j] < dist[i] - 1e-4f)
            {
                wsum += b.area * (b.vertical ? 3.f : 1.f);
                vsum += b.vertical && S.pieces[j].pivot.y <= p.pivot.y ? b.area : 0.f;
            }
        }
        if (wsum <= 0.f)
            continue; // anchored: into the ground
        for (u32 bi : p.bonds)
        {
            Bond& b = S.bonds[bi];
            const u16 j = b.a == i ? b.b : b.a;
            if (b.broken || S.pieces[j].state != ST_INTACT || !(dist[j] < dist[i] - 1e-4f))
                continue;
            const float share = b.area * (b.vertical ? 3.f : 1.f) / wsum;
            const float F = L[i] * share;
            b.f += F;
            L[j] += F;
            if (!b.vertical)
            {
                const float lever = _sqrt(_sqr(p.pivot.x - b.pt.x) + _sqr(p.pivot.z - b.pt.z));
                const float M = (Mo[i] + L[i] * lever) * share;
                b.m += M;
                Mo[j] += M;
                continue;
            }
            if (S.pieces[j].pivot.y > p.pivot.y)
            {
                b.hang = true; // under it: held by the bond's tension, nothing to tip
                continue;
            }
            // the stack above tips over this contact: its moment here, the push and the lean along each axis added worst
            const float y = b.pt.y, sv = b.area / _max(vsum, 1e-6f);
            Fvector M;
            M.mad(Qb[i], Hb[i], -y).mul(sv);
            float mx = M.dotproduct(S.rot.i), mz = M.dotproduct(S.rot.k);
            const float lx = _max(0.f, (qx[i] - y * hx[i]) * sv), lz = _max(0.f, (qz[i] - y * hz[i]) * sv);
            mx += mx < 0.f ? -lx : lx;
            mz += mz < 0.f ? -lz : lz;
            b.tip.set(0.f, 0.f, 0.f);
            b.tip.mad(S.rot.i, mx).mad(S.rot.k, mz);
            b.m = b.tip.magnitude();
            b.tip.normalize_safe();
            Hb[j].mad(Hb[i], sv);
            Qb[j].mad(Qb[i], sv);
            hx[j] += hx[i] * sv;
            qx[j] += qx[i] * sv;
            hz[j] += hz[i] * sv;
            qz[j] += qz[i] * sv;
        }
    }
}

// E54d: how deep a vertical bond's contact is the way its moment pushes (the edge the stack above tips over is half that away)
float tip_depth(const Bond& b) { return _abs(b.tip.dotproduct(S.rot.i)) * b.dx + _abs(b.tip.dotproduct(S.rot.k)) * b.dz; }

void calibrate()
{
    xr_vector<float> dist;
    distances(dist);
    u32 unreached = 0; // stood in the level as built (on something the tool did not see): held where they are
    for (u32 i = 0; i < S.pieces.size(); ++i)
        if (dist[i] == flt_max)
        {
            S.pieces[i].anchored = true;
            ++unreached;
        }
    if (unreached)
        distances(dist);
    loads(dist);
    u32 boosted = 0, tip_boosted = 0;
    for (Bond& b : S.bonds)
    {
        b.cap_f = _max(C.sc * b.area, C.safety * b.f);
        b.cap_s = _max(C.ss * b.area, C.safety * b.f);
        b.cap_m = _max(C.sm * b.area * _sqrt(b.area), C.safety * b.m);
        boosted += (C.sc * b.area < C.safety * b.f) || (!b.vertical && C.sm * b.area * _sqrt(b.area) < C.safety * b.m);
        // E54d: a vertical bond holds what tipped or hung from it standing, `safety` times over too
        b.cap_t = 0.f;
        b.cap_n = C.sh * (b.area_v > 0.f ? b.area_v : b.area); // E54e: strength_hanging (was strength_tension x area_v: 0 for a restacked contact)
        if (!b.vertical)
            continue;
        if (b.hang)
            b.cap_n = _max(b.cap_n, C.safety * b.f);
        else
        {
            const float d = tip_depth(b);
            b.cap_t = _max(0.f, C.safety * b.m - (b.f * d * 0.5f + C.st * b.area_v * d / 6.f));
            tip_boosted += b.cap_t > 0.f;
        }
    }
    S.base_y = flt_max;
    for (const Piece& p : S.pieces)
        if (p.anchored)
            S.base_y = _min(S.base_y, p.aabb.min.y);
    if (S.base_y == flt_max)
        S.base_y = 0.f;
    Msg("* [destruct] %s: %u pieces, %u bonds (%u above the material's strength when whole, %u stacks leaning past their weight and tension, "
        "%u sideways ones stacking), %u pieces with no path to the ground held in place",
        S.name.c_str(), (u32)S.pieces.size(), (u32)S.bonds.size(), boosted, tip_boosted, S.restacked, unreached);
}

// unreachable intact pieces -> islands over their unbroken bonds. E54e: past max_release bodies moving at once the parts
// let go together are glued, smallest first, to the smallest part next to them (any bond between them, broken or not)
// into bodies of up to glue_pieces pieces; such a body splits back into its parts on its first landing
void release_loose(const xr_vector<float>& dist)
{
    const u32 n = (u32)S.pieces.size();
    xr_vector<int> comp(n, -1);
    xr_vector<xr_vector<u16>> comps;
    for (u32 s = 0; s < n; ++s)
    {
        if (S.pieces[s].state != ST_INTACT || dist[s] < flt_max || comp[s] >= 0)
            continue;
        const int ci = (int)comps.size();
        comps.emplace_back();
        xr_vector<u16>& q = comps.back();
        q.push_back(u16(s));
        comp[s] = ci;
        for (u32 qi = 0; qi < q.size(); ++qi)
            for (u32 bi : S.pieces[q[qi]].bonds)
            {
                const Bond& b = S.bonds[bi];
                const u16 o = b.a == q[qi] ? b.b : b.a;
                if (!b.broken && S.pieces[o].state == ST_INTACT && comp[o] < 0)
                {
                    comp[o] = ci;
                    q.push_back(o);
                }
            }
    }
    const u32 nc = (u32)comps.size();
    if (!nc)
        return;
    u32 moving = 0;
    for (const Island* J : S.islands)
        moving += J && J->shell && J->shell->isEnabled();
    xr_vector<int> grp(nc);
    xr_vector<u32> gsize(nc);
    for (u32 i = 0; i < nc; ++i)
    {
        grp[i] = (int)i;
        gsize[i] = (u32)comps[i].size();
    }
    auto root = [&](int i) {
        while (grp[i] != i)
            i = grp[i] = grp[grp[i]];
        return i;
    };
    u32 groups = nc;
    if (C.max_release > 0 && nc > 1 && moving + nc > (u32)C.max_release)
    {
        const u32 want = moving + 1 < (u32)C.max_release ? (u32)C.max_release - moving : 1;
        xr_vector<u32> order(nc);
        for (u32 i = 0; i < nc; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return comps[a].size() < comps[b].size(); });
        for (int pass = 0; pass < 6 && groups > want; ++pass)
        {
            bool any = false;
            for (u32 ci : order)
            {
                if (groups <= want)
                    break;
                const int r = root((int)ci);
                int best = -1;
                u32 bs = u32(-1);
                for (u16 pc : comps[ci])
                    for (u32 bi : S.pieces[pc].bonds)
                    {
                        const Bond& b = S.bonds[bi];
                        const u16 o = b.a == pc ? b.b : b.a;
                        if (comp[o] < 0)
                            continue;
                        const int ro = root(comp[o]);
                        if (ro == r || gsize[r] + gsize[ro] > (u32)C.glue_pieces)
                            continue;
                        if (gsize[ro] < bs)
                            bs = gsize[ro], best = ro;
                    }
                if (best < 0)
                    continue;
                grp[r] = best;
                gsize[best] += gsize[r];
                --groups;
                any = true;
            }
            if (!any)
                break;
        }
    }
    xr_vector<int> gi(nc, -1);
    xr_vector<xr_vector<u16>> rel;
    xr_vector<u8> multi;
    for (u32 i = 0; i < nc; ++i)
    {
        const int r = root((int)i);
        if (gi[r] < 0)
        {
            gi[r] = (int)rel.size();
            rel.emplace_back();
            multi.push_back(0);
        }
        xr_vector<u16>& g = rel[gi[r]];
        if (!g.empty())
            multi[gi[r]] = 1;
        g.insert(g.end(), comps[i].begin(), comps[i].end());
    }
    u32 glued = 0;
    for (u32 k = 0; k < rel.size(); ++k)
    {
        release(rel[k]);
        const int slot = S.pieces[rel[k][0]].island;
        if (multi[k] && slot >= 0 && S.islands[slot])
        {
            S.islands[slot]->glued = true;
            ++glued;
        }
    }
    if (rel.size() < nc)
        Msg("[destruct] %u parts let go at once (%u bodies moving): %u bodies, %u of them glued", nc, moving, (u32)rel.size(), glued);
}

// one pass: release what has no path to the ground, break the overloaded bonds (the next frame goes on if any broke)
void solve()
{
    S.dirty = false;
    ++S.solves;
    xr_vector<float> dist;
    distances(dist);
    u32 standing0 = 0;
    if (C.solve_debug)
        for (const Piece& p : S.pieces)
            standing0 += p.state == ST_INTACT;
    release_loose(dist);
    loads(dist);
    u32 broke = 0, toppled = 0;
    u32 why[5] = {}; // E54e solve_debug: shear, bending, hanging, crushed, tipped
    float worst[5] = {};
    for (Bond& b : S.bonds)
    {
        if (b.broken || S.pieces[b.a].state != ST_INTACT || S.pieces[b.b].state != ST_INTACT)
            continue;
        const Piece& pa = S.pieces[b.a];
        const Piece& pb = S.pieces[b.b];
        const float h = _min(pa.hp / pa.hp_max, pb.hp / pb.hp_max);
        if (C.cascade_r > 0.f && !b.zone && h >= 0.6f)
            continue; // E54e: far from any damage the frame holds
        const float k = 0.35f + 0.65f * clampr(h, 0.f, 1.f);
        float u;
        bool tips = false;
        int cause;
        if (!b.vertical)
        {
            const float us = b.f / (b.cap_s * k), um = b.m / (b.cap_m * k);
            u = _max(us, um);
            cause = us >= um ? 0 : 1;
        }
        else if (b.hang)
        {
            u = b.f / (b.cap_n * k);
            cause = 2;
        }
        else
        {
            u = b.f / (b.cap_f * k);
            cause = 3;
            const float d = tip_depth(b);
            const float ut = b.m / ((b.f * d * 0.5f + C.st * b.area_v * d / 6.f + b.cap_t) * k + 1.f);
            if (ut > u)
            {
                u = ut;
                tips = true;
                cause = 4;
            }
        }
        if (u > 1.f)
        {
            b.broken = true;
            b.toppled = tips;
            b.t_broke = Device.dwTimeGlobal;
            ++broke;
            toppled += tips;
            ++why[cause];
            worst[cause] = _max(worst[cause], u);
        }
    }
    for (Piece& p : S.pieces)
        p.hp_solved = p.hp;
    S.broken += broke;
    S.toppled += toppled;
    if (broke)
    {
        S.dirty = true;
        if (S.blast_frame == Device.dwFrame)
        {
            distances(dist); // what the blast broke loose goes now, thrown by it
            release_loose(dist);
        }
    }
    if (C.solve_debug && broke)
    {
        u32 standing = 0;
        for (const Piece& p : S.pieces)
            standing += p.state == ST_INTACT;
        Msg("[destruct] solve %u: %u let go, %u standing; %u bonds broke - shear %u (x%.1f), bending %u (x%.1f), hanging %u (x%.1f), "
            "crushed %u (x%.1f), tipped %u (x%.1f)",
            S.solves, standing0 - standing, standing, broke, why[0], worst[0], why[1], worst[1], why[2], worst[2], why[3], worst[3], why[4], worst[4]);
    }
}

void reset()
{
    for (u32 i = 0; i < S.islands.size(); ++i)
        if (S.islands[i])
        {
            if (S.islands[i]->shell)
                destroy_physics_shell(S.islands[i]->shell);
            xr_delete(S.islands[i]);
        }
    for (Piece& p : S.pieces)
        xr_delete(p.pv);
    for (ChipModel*& m : S.chip_models)
        xr_delete(m);
    for (DentVis*& v : S.dent_vis) // E56: the bowls first (they draw the shared models)
        xr_delete(v);
    for (IRenderVisual*& m : S.dent_models)
        if (m)
            ::Render->model_Delete(m);
    S = State{};
    for (Fvector4& h : portal_mod_holes)
        h.set(0.f, 0.f, 0.f, 0.f);
    portal_mod_holes_near = false;
    portal_mod_dents_count = 0; // E56
    portal_mod_dents_groups_count = 0;
    portal_mod_dents[0][0].set(0.f, 0.f, 0.f, 0.f);
}

// ------------------------------------------------------------------------------------------------------------- E54d: saved state
// a saved game loaded on the level played now: OGSR keeps the level (only its objects load again), so the building is
// made whole here before the save's state goes on it - its triangles back, every piece standing (drawn as pieces if the
// level's own building was already cut out), the bodies, holes and chips gone
void rebuild()
{
    for (u32 i = 0; i < S.islands.size(); ++i)
        if (S.islands[i])
        {
            if (S.islands[i]->shell)
                destroy_physics_shell(S.islands[i]->shell);
            xr_delete(S.islands[i]);
        }
    S.islands.clear();
    if (S.loaded && S.app_t.size() == S.tri_count * 3)
    {
        CDB::TRI* T = Level().ObjectSpace.GetStaticTris();
        for (u32 k = 0; k < S.tri_count; ++k)
            for (int v = 0; v < 3; ++v)
                T[S.tri_base + k].verts[v] = S.vbase + S.app_t[k * 3 + v];
    }
    const u32 n = (u32)S.pieces.size();
    for (Piece& p : S.pieces)
    {
        p.state = ST_INTACT;
        p.hp = p.hp_solved = p.hp_max;
        p.island = -1;
        if (p.pv)
        {
            if (S.swapped)
                p.pv->place(rest_xform(p), true);
            else
                p.pv->hide();
        }
    }
    for (Bond& b : S.bonds)
    {
        b.broken = b.a >= n || b.b >= n;
        b.toppled = false;
        b.zone = false; // E54e
    }
    S.holes.clear();
    S.holes_open = 0;
    S.dents.clear(); // E56 (their bowls hide in the next update)
    S.chips.clear();
    S.blast_h.clear();
    S.blast_frame = u32(-1);
    S.dirty = false;
}

// the level played now into G (nothing destroyed: its entry goes)
void stash()
{
    if (!S.restored || !S.level.size() || S.gen != G.gen)
        return;
    bool any = S.swapped || !S.holes.empty() || !S.dents.empty(); // E56: dents too
    for (const Piece& p : S.pieces)
        any = any || p.state != ST_INTACT || p.hp < p.hp_max;
    if (!any)
    {
        G.levels.erase(S.level);
        return;
    }
    CMemoryWriter w;
    w.w_u32(SAVE_VERSION);
    w.w_stringZ(S.name.c_str() ? S.name.c_str() : "");
    w.w_u32((u32)S.pieces.size());
    w.w_u32((u32)S.bonds.size());
    w.w_u8(S.swapped ? 1 : 0);
    for (const Piece& p : S.pieces)
    {
        w.w_u8(p.state);
        w.w_float(p.hp);
    }
    for (const Bond& b : S.bonds)
        w.w_u8((b.broken ? 1 : 0) | (b.zone ? 2 : 0)); // E54e: the zone in bit 1
    xr_vector<std::pair<const Island*, xr_vector<u16>>> isl;
    for (u32 slot = 0; slot < S.islands.size(); ++slot)
    {
        const Island* I = S.islands[slot];
        if (!I || !I->shell)
            continue;
        xr_vector<u16> pcs;
        for (u16 pc : I->pieces)
            if (S.pieces[pc].state == ST_FALLING && S.pieces[pc].island == (int)slot)
                pcs.push_back(pc);
        if (!pcs.empty())
            isl.emplace_back(I, std::move(pcs));
    }
    w.w_u32((u32)isl.size());
    for (const auto& [I, pcs] : isl)
    {
        w.w(&I->xf, sizeof(Fmatrix));
        w.w_u32((u32)pcs.size());
        for (u16 pc : pcs)
        {
            w.w_u16(pc);
            w.w_fvector3(S.pieces[pc].local);
        }
    }
    w.w_u32((u32)S.holes.size());
    for (const Hole& h : S.holes)
    {
        w.w_fvector3(h.c);
        w.w_fvector3(h.dir);
        w.w_float(h.thick);
        w.w_float(h.need);
        w.w_float(h.dmg);
        w.w_float(h.r);
        w.w_s32(h.piece);
        w.w_u8(h.mat);
    }
    w.w_u32((u32)S.dents.size()); // E56: after the holes (a save before it ends there)
    for (const Dent& d : S.dents)
    {
        w.w_fvector3(d.c);
        w.w_fvector3(d.n);
        w.w_fvector3(d.t);
        w.w_float(d.r);
        w.w_float(d.depth);
        w.w_float(d.dmg);
        w.w_float(d.thick);
        w.w_float(d.seed);
        w.w_float(d.cap);
        w.w_s32(d.piece);
        w.w_u8(d.mat);
        w.w_u8(d.var);
        w.w_u8(d.blast ? 1 : 0);
    }
    xr_vector<u8>& blob = G.levels[S.level];
    blob.assign(w.pointer(), w.pointer() + w.size());
}

// the first frame on a level: its state from G put back - pieces gone, bonds broken, islands where they lay, holes
void restore()
{
    S.restored = true;
    S.level = Level().name();
    S.gen = G.gen;
    auto it = G.levels.find(S.level);
    if (it == G.levels.end() || it->second.size() < 8)
        return;
    IReader r(it->second.data(), (int)it->second.size());
    if (r.r_u32() != SAVE_VERSION)
        return;
    shared_str name;
    r.r_stringZ(name);
    const u32 np = r.r_u32(), nb = r.r_u32();
    const bool swapped = r.r_u8() != 0;
    const bool building = S.loaded && name == S.name && np == S.pieces.size() && nb == S.bonds.size();
    u32 gone = 0, falling = 0, holes = 0;
    if (!building)
    {
        if (np || nb)
            Msg("! [destruct] %s: the saved state was for another building ('%s', %u pieces) - the building stands whole", S.level.c_str(), name.c_str(), np);
        r.advance(np * 5 + nb);
    }
    else
    {
        for (Piece& p : S.pieces)
        {
            const u8 st = r.r_u8();
            p.hp = p.hp_solved = r.r_float();
            if (st != ST_INTACT)
            {
                drop_tris(p);
                p.state = ST_GONE; // an island's own become falling again below
            }
        }
        for (Bond& b : S.bonds)
        {
            const u8 v = r.r_u8();
            b.broken = (v & 1) != 0;
            b.zone = (v & 2) != 0; // E54e
        }
        if (swapped)
            swap_to_pieces();
    }
    for (u32 n = r.r_u32(); n; --n)
    {
        Fmatrix xf;
        r.r(&xf, sizeof(Fmatrix));
        xr_vector<u16> pcs;
        for (u32 k = r.r_u32(); k; --k)
        {
            const u16 pc = r.r_u16();
            Fvector l;
            r.r_fvector3(l);
            if (building && pc < S.pieces.size() && S.pieces[pc].state == ST_GONE)
            {
                S.pieces[pc].local = l;
                pcs.push_back(pc);
            }
        }
        if (building && !pcs.empty() && _valid(xf))
        {
            make_island(pcs, xf, Fvector{}, Fvector{});
            falling += (u32)pcs.size();
        }
    }
    for (u32 n = r.r_u32(); n; --n)
    {
        Hole h;
        r.r_fvector3(h.c);
        r.r_fvector3(h.dir);
        h.thick = r.r_float();
        h.need = r.r_float();
        h.dmg = r.r_float();
        h.r = r.r_float();
        h.piece = r.r_s32();
        h.mat = r.r_u8();
        h.time = Device.dwTimeGlobal;
        if (h.piece < 0 || (building && h.piece < (int)S.pieces.size() && S.pieces[h.piece].state == ST_INTACT))
        {
            S.holes.push_back(h);
            holes += 1;
            S.holes_open += h.r > 0.f;
        }
    }
    u32 dents = 0;
    if (r.elapsed() >= 4) // E56
        for (u32 n = r.r_u32(); n && r.elapsed() >= 67; --n)
        {
            Dent d;
            r.r_fvector3(d.c);
            r.r_fvector3(d.n);
            r.r_fvector3(d.t);
            d.r = r.r_float();
            d.depth = r.r_float();
            d.dmg = r.r_float();
            d.thick = r.r_float();
            d.seed = r.r_float();
            d.cap = r.r_float();
            d.piece = r.r_s32();
            d.mat = r.r_u8();
            d.var = r.r_u8();
            d.blast = r.r_u8() != 0;
            d.time = Device.dwTimeGlobal;
            d.id = ++S.dent_ids;
            d.checked = d.r;
            if (d.mat < NMAT && d.var < DENT_VARS && (d.piece < 0 || (building && d.piece < (int)S.pieces.size() && S.pieces[d.piece].state == ST_INTACT)))
            {
                S.dents.push_back(d);
                ++dents;
            }
        }
    if (building)
        for (const Piece& p : S.pieces)
            gone += p.state == ST_GONE;
    S.dirty = building; // the solver looks once more at what stands
    Msg("* [destruct] %s: the saved destruction put back - %u pieces gone, %u lying as %u bodies, %u holes, %u dents", S.level.c_str(), gone, falling,
        (u32)std::count_if(S.islands.begin(), S.islands.end(), [](const Island* I) { return I != nullptr; }), holes, dents);
}
} // namespace

// ------------------------------------------------------------------------------------------------------------- API
float ammo_damage(LPCSTR ammo)
{
    LPCSTR s = "pm_destruct_ammo";
    if (!ammo || !*ammo || !pSettings->section_exist(s))
        return 0.f;
    if (pSettings->line_exist(s, ammo))
        return pSettings->r_float(s, ammo);
    return pSettings->line_exist(s, "default") ? pSettings->r_float(s, "default") : 0.f;
}

void bullet_hit(const SBullet& b, u32 tri, const Fvector& point, const Fvector& normal, u16 target_material)
{
    if (!C.enabled || !portal_mod_feat::enabled(portal_mod_feat::destruct) || b.pm_destruct <= 0.f) // portal-mod E55: My Mods
        return;
    const float sp = b.max_speed > 1.f ? clampr(b.speed / b.max_speed, 0.2f, 1.f) : 1.f;
    const float dmg = b.pm_destruct * sp;
    const int pc = piece_of_tri(tri);
    const u8 mat = pc >= 0 ? S.pieces[pc].mat : chip_class(target_material);
    chips_at(point, normal, b.dir, dmg, mat);
    if (pc < 0 || S.pieces[pc].state == ST_INTACT)
    {
        hole_hit(point, b.dir, dmg, mat, pc);
        dent_hit(point, normal, b.dir, dmg, mat, pc, false); // E56: any face, any thickness
    }
    if (pc >= 0)
        damage_piece(u16(pc), dmg, b.dir, 1.5f);
}

bool hole_pass(const Fvector& p) { return !S.holes.empty() && hole_contains(p, 0.97f); }

void explosion(const Fvector& pos, float radius, float hit, float impulse)
{
    if (!C.enabled || !portal_mod_feat::enabled(portal_mod_feat::destruct) || radius <= 0.f) // portal-mod E55
        return;
    blast_dents(pos, radius, hit); // E56: the faces round it, on any level
    if (!S.loaded)
        return;
    const float R = radius;
    const float D0 = hit * C.k_expl;
    S.blast_pos = pos;
    S.blast_r = R;
    S.blast_push = C.expl_push * clampr(impulse / 100.f, 0.5f, 3.f);
    S.blast_frame = Device.dwFrame;
    S.blast_h.assign(S.pieces.size(), Fvector{});
    for (u32 i = 0; i < S.pieces.size(); ++i)
    {
        Piece& p = S.pieces[i];
        if (p.state != ST_INTACT)
            continue;
        Fvector q;
        q.set(clampr(pos.x, p.aabb.min.x, p.aabb.max.x), clampr(pos.y, p.aabb.min.y, p.aabb.max.y), clampr(pos.z, p.aabb.min.z, p.aabb.max.z));
        const float d = q.distance_to(pos);
        if (d >= R)
            continue;
        Fvector dir;
        dir.sub(p.pivot, pos).normalize_safe();
        const float fall = std::pow(1.f - d / R, C.expl_pow);
        // E54d: the blast pushes it sideways too - on its side facing the blast (~ its volume^2/3), for the solver below
        Fvector side{dir.x, 0.f, dir.z};
        if (side.magnitude() > 0.1f)
        {
            side.normalize();
            S.blast_h[i].mul(side, C.blast_lat * std::pow(p.vol, 2.f / 3.f) * fall * S.blast_push / _max(C.expl_push, 0.01f));
            S.dirty = true;
        }
        damage_piece(u16(i), D0 * fall, dir, S.blast_push * (1.f - d / R));
    }
    // falling pieces near it are thrown
    for (Island* I : S.islands)
        if (I && I->shell)
        {
            Fvector d;
            d.sub(I->xf.c, pos);
            const float dist = d.magnitude();
            if (dist < R * 1.5f)
            {
                d.normalize_safe();
                I->shell->applyImpulse(d, I->mass * S.blast_push * (1.f - dist / (R * 1.5f)));
            }
        }
    if (S.dirty)
        solve();
}

static void update_body()
{
    if (portal_mod_replay)
        return; // a replay plays: the world stands
    if (g_pGameLevel && (!S.restored || S.gen != G.gen))
    {
        if (S.restored)
            rebuild(); // E54d: a save loaded on this level (it stays loaded)
        if (S.loaded && !S.models)
            make_models();
        restore(); // E54d
    }
    update_holes();
    update_dents(); // E56
    if (!S.loaded)
    {
        if (S.chips.empty())
            return;
    }
    else if (!S.models)
        make_models(); // the frame after the level loaded: the pieces' and chips' models
    const float dt = _min(Device.fTimeDelta, 0.05f);
    if (S.loaded)
    {
        update_islands();
        if (S.dirty && portal_mod_feat::enabled(portal_mod_feat::destruct)) // portal-mod E55: switched off - the solver stands, nothing more gives way
            solve();
    }
    update_chips(dt);
}

// portal-mod E54e: the profiler (level.destruct_profile) - the last frame's parts each frame, a "PROFILE" line each
// second with the mean / worst of each, and one for every frame over 40 ms
namespace prof
{
enum
{
    FRAME, MOVE, RENDER, SECOND, WAIT, MOD, SCHED, UCL, PHYS, COLL, CORE, CALC, DUMP, ANIM, CALLS, MODELS, SUN, NP
};
LPCSTR const NAME[NP] = {"frame", "move", "render", "second", "wait", "destruct", "sched", "ucl", "phys", "coll", "core",
                         "calc", "dump", "anim", "calls", "models", "sun"};
bool on{}, primed{};
u32 n{}, t0{}, slow{};
double sum[NP]{}, prev[8]{};
float mx[NP]{}, worst[NP]{}, upd{};

void window()
{
    n = 0;
    t0 = Device.dwTimeGlobal;
    std::fill(sum, sum + NP, 0.0);
    std::fill(mx, mx + NP, 0.f);
    std::fill(worst, worst + NP, 0.f);
}

void bodies(u32& awake, u32& all)
{
    awake = all = 0;
    for (const Island* I : S.islands)
        if (I)
        {
            ++all;
            awake += I->shell && I->shell->isEnabled();
        }
}

void frame()
{
    if (!on)
        return;
    using ms = std::chrono::duration<double, std::milli>;
    CStats* st = Device.Statistic;
    const CStatTimer* tm[8] = {&st->Sheduler, &st->UpdateClient, &st->Physics, &st->ph_collision, &st->ph_core, &st->RenderCALC, &st->RenderDUMP, &st->Animation};
    float f[NP]{};
    f[FRAME] = Device.fTimeDeltaReal * 1000.f; // all of these: the frame before this one
    for (int i = 0; i < 4; ++i)
        f[MOVE + i] = g_pm_frame_ms[i];
    f[MOD] = upd;
    for (int i = 0; i < 8; ++i)
    {
        const double c = ms(tm[i]->accum).count();
        f[SCHED + i] = float(c >= prev[i] ? c - prev[i] : c);
        prev[i] = c;
    }
    u32 rs[7]{};
    portal_mod_render_stats(rs);
    f[CALLS] = (float)rs[0];
    f[MODELS] = (float)rs[2];
    f[SUN] = float(rs[4] + rs[5] + rs[6]);
    if (!primed)
    {
        primed = true;
        window();
        return;
    }
    ++n;
    for (int i = 0; i < NP; ++i)
    {
        sum[i] += f[i];
        mx[i] = _max(mx[i], f[i]);
    }
    if (f[FRAME] > worst[FRAME])
        std::copy(f, f + NP, worst);
    u32 awake, all;
    if (f[FRAME] > 40.f && slow < 60)
    {
        ++slow;
        bodies(awake, all);
        Msg("PROFILE slow %.0f ms: move %.1f render %.1f second %.1f wait %.1f destruct %.1f | sched %.1f ucl %.1f phys %.1f (coll %.1f core %.1f) "
            "calc %.1f dump %.1f anim %.1f | calls %.0f models %.0f sun %.0f | bodies %u/%u chips %u",
            f[FRAME], f[MOVE], f[RENDER], f[SECOND], f[WAIT], f[MOD], f[SCHED], f[UCL], f[PHYS], f[COLL], f[CORE], f[CALC], f[DUMP], f[ANIM],
            f[CALLS], f[MODELS], f[SUN], awake, all, (u32)S.chips.size());
    }
    if (Device.dwTimeGlobal - t0 < 1000 || !n)
        return;
    string1024 line{};
    int len = xr_sprintf(line, "PROFILE %u frames, mean/max:", n);
    for (int i = 0; i < NP && len > 0 && len < 900; ++i)
        len += xr_sprintf(line + len, sizeof(line) - len, i < CALLS ? " %s %.1f/%.0f" : " %s %.0f/%.0f", NAME[i], sum[i] / n, mx[i]);
    bodies(awake, all);
    Msg("%s | worst: move %.0f render %.0f second %.0f wait %.0f | bodies %u/%u chips %u", line, worst[MOVE], worst[RENDER], worst[SECOND],
        worst[WAIT], awake, all, (u32)S.chips.size());
    window();
}
} // namespace prof

void update()
{
    prof::frame(); // E54e
    if (!prof::on)
    {
        update_body();
        return;
    }
    CTimer t;
    t.Start();
    update_body();
    prof::upd = t.GetElapsed_sec() * 1000.f;
}

void lua_profile(bool on)
{
    prof::on = on;
    prof::primed = false;
    prof::slow = 0;
    g_pm_stat_gather = on ? TRUE : FALSE;
    Msg("PROFILE %s", on ? "on" : "off");
}

void shutdown()
{
    stash(); // E54d: the level's destruction kept for when the player comes back (or the game is saved elsewhere)
    reset();
}

void save_game(LPCSTR sav_path)
{
    if (!sav_path || !*sav_path)
        return;
    stash();
    string_path fn;
    xr_sprintf(fn, "%s.destruct", sav_path);
    IWriter* w = FS.w_open(fn);
    if (!w)
    {
        Msg("! [destruct] cannot write %s", fn);
        return;
    }
    w->w("PMDS", 4);
    w->w_u32(SAVE_VERSION);
    w->w_u32((u32)G.levels.size());
    for (const auto& [lvl, blob] : G.levels)
    {
        w->w_stringZ(lvl.c_str());
        w->w_u32((u32)blob.size());
        if (!blob.empty())
            w->w(blob.data(), blob.size());
    }
    FS.w_close(w);
    Msg("* [destruct] destruction saved for %u level(s) to %s", (u32)G.levels.size(), fn);
}

void load_game(LPCSTR sav_path)
{
    ++G.gen;
    G.levels.clear();
    if (!sav_path || !*sav_path)
        return;
    string_path fn;
    xr_sprintf(fn, "%s.destruct", sav_path);
    IReader* r = FS.r_open(fn);
    if (!r)
        return; // saved before E54d, or nothing to keep
    char magic[4]{};
    if (r->length() >= 12)
        r->r(magic, 4);
    if (strncmp(magic, "PMDS", 4) == 0 && r->r_u32() == SAVE_VERSION)
        for (u32 n = r->r_u32(); n && r->elapsed() > 0; --n)
        {
            shared_str lvl;
            r->r_stringZ(lvl);
            const u32 sz = r->r_u32();
            if ((u32)r->elapsed() < sz)
                break;
            xr_vector<u8>& blob = G.levels[lvl];
            blob.resize(sz);
            if (sz)
                r->r(blob.data(), sz);
        }
    FS.r_close(r);
    Msg("* [destruct] destruction loaded for %u level(s) from %s", (u32)G.levels.size(), fn);
}

LPCSTR lua_info()
{
    static string512 buf;
    u32 intact = 0, falling = 0, gone = 0, islands = 0, awake = 0, tall = 0;
    for (const Piece& p : S.pieces)
    {
        (p.state == ST_INTACT ? intact : p.state == ST_FALLING ? falling : gone)++;
        tall += p.state == ST_INTACT && p.aabb.min.y > S.base_y + 2.5f; // E54d: still standing above head height
    }
    for (const Island* I : S.islands)
        if (I)
        {
            ++islands;
            awake += I->shell && I->shell->isEnabled();
        }
    u32 broken = 0;
    for (const Bond& b : S.bonds)
        broken += b.broken;
    u32 dents = 0; // E56: open ones
    for (const Dent& d : S.dents)
        dents += d.r > 0.f;
    xr_sprintf(buf, "loaded=%d name=%s pieces=%u intact=%u falling=%u gone=%u islands=%u awake=%u bonds=%u broken=%u swapped=%d chips=%u solves=%u holes=%u open=%u tall=%u toppled=%u crushed=%u kills=%u dents=%u spots=%u drawn=%u",
               S.loaded ? 1 : 0, S.name.c_str() ? S.name.c_str() : "-", (u32)S.pieces.size(), intact, falling, gone, islands, awake, (u32)S.bonds.size(), broken,
               S.swapped ? 1 : 0, (u32)S.chips.size(), S.solves, (u32)S.holes.size(), S.holes_open, tall, S.toppled, S.crushed, S.crush_kills, dents,
               (u32)S.dents.size(), S.dents_drawn);
    return buf;
}

void lua_explode(const Fvector& pos, float radius, float power)
{
    Fvector up{0.f, 1.f, 0.f};
    play_fx(C.fx_destroy[0], pos, up);
    explosion(pos, radius, power, 100.f);
}

int lua_piece_at(const Fvector& pos)
{
    int best = -1;
    float bd = flt_max;
    for (u32 i = 0; i < S.pieces.size(); ++i)
    {
        const Piece& p = S.pieces[i];
        if (p.state == ST_GONE)
            continue;
        Fbox b = p.aabb;
        b.grow(0.05f);
        const float d = b.contains(pos) ? 0.f : p.pivot.distance_to(pos) + 10.f;
        if (d < bd)
            bd = d, best = (int)i;
    }
    return bd < 13.f ? best : -1;
}

Fvector lua_piece_pos(int id)
{
    if (id < 0 || id >= (int)S.pieces.size())
        return Fvector{};
    return piece_xform(S.pieces[id]).c;
}

int lua_piece_state(int id) { return id < 0 || id >= (int)S.pieces.size() ? -1 : S.pieces[id].state; }

void lua_chips(const Fvector& pos, const Fvector& normal, float damage) { if (portal_mod_feat::enabled(portal_mod_feat::destruct)) /* portal-mod E55 */ chips_at(pos, normal, Fvector().invert(normal), damage, 0); }

// what a bullet from `from` along `dir` meets first on the level: x - its distance (-1: nothing within range), y - the
// wall's thickness there as a hole would measure it (-1: thicker, or bullets fly through its material), z - its chip
// class (0 concrete/stone, 1 brick, 2 wood; -1: none), +10 on a building's piece
Fvector lua_wall_probe(const Fvector& from, const Fvector& dir_in, float range)
{
    Fvector out{-1.f, -1.f, -1.f};
    Fvector dir = dir_in;
    dir.normalize_safe();
    collide::rq_result R;
    if (!Level().ObjectSpace.RayPick(from, dir, range, collide::rqtStatic, R, nullptr) || R.element < 0)
        return out;
    out.x = R.range;
    const CDB::TRI& t = Level().ObjectSpace.GetStaticTris()[R.element];
    const SGameMtl* m = GMLib.GetMaterialByIdx(t.material);
    if (!m || fsimilar(m->fShootFactor, 1.0f, EPS))
        return out;
    const int pc = piece_of_tri(u32(R.element));
    const u8 cls = pc >= 0 ? S.pieces[pc].mat : chip_class(t.material);
    if (cls >= NMAT)
        return out;
    out.z = float(cls) + (pc >= 0 ? 10.f : 0.f);
    Fvector p0;
    p0.mad(from, dir, R.range + 0.012f);
    collide::rq_result R2;
    if (Level().ObjectSpace.RayPick(p0, dir, C.hole_max_thick, collide::rqtStatic, R2, nullptr) && R2.element >= 0)
        out.y = R2.range + 0.012f;
    return out;
}

// E54e: the objects falling pieces never hit
void lua_crush_immune(int id, bool on)
{
    if (id < 0 || id >= 0xffff)
        return;
    auto it = std::find(g_crush_immune.begin(), g_crush_immune.end(), u16(id));
    if (on && it == g_crush_immune.end())
        g_crush_immune.push_back(u16(id));
    else if (!on && it != g_crush_immune.end())
        g_crush_immune.erase(it);
    Msg("[destruct] crush immune %d: %s", id, on ? "on" : "off");
}

// E54e: the charges go off - every standing piece reaching into the band y_lo..y_hi m over the ground under the building
// (within `radius` m of pos across, 0: the whole building) turns to rubble at once; what stood on them comes down
int lua_demolish(const Fvector& pos, float radius, float y_lo, float y_hi)
{
    if (!S.loaded || !C.enabled || !portal_mod_feat::enabled(portal_mod_feat::destruct)) // portal-mod E55
        return 0;
    const float a = S.base_y + y_lo, b = S.base_y + y_hi;
    xr_vector<u16> pcs;
    for (u32 i = 0; i < S.pieces.size(); ++i)
    {
        const Piece& p = S.pieces[i];
        if (p.state != ST_INTACT || p.aabb.max.y < a || p.aabb.min.y > b)
            continue;
        if (radius > 0.f)
        {
            const float dx = _max(0.f, _max(p.aabb.min.x - pos.x, pos.x - p.aabb.max.x));
            const float dz = _max(0.f, _max(p.aabb.min.z - pos.z, pos.z - p.aabb.max.z));
            if (dx * dx + dz * dz > radius * radius)
                continue;
        }
        pcs.push_back(u16(i));
    }
    if (pcs.empty())
        return 0;
    // the bangs: up to 8 spread over the charges (each the farthest from those already going off)
    Fvector c0;
    S.pieces[pcs[0]].aabb.getcenter(c0);
    xr_vector<Fvector> bangs{c0};
    while (bangs.size() < 8 && bangs.size() < pcs.size())
    {
        float bd = -1.f;
        Fvector best{};
        for (u16 pc : pcs)
        {
            Fvector c;
            S.pieces[pc].aabb.getcenter(c);
            float d = flt_max;
            for (const Fvector& q : bangs)
                d = _min(d, c.distance_to_sqr(q));
            if (d > bd)
                bd = d, best = c;
        }
        if (bd < 4.f)
            break;
        bangs.push_back(best);
    }
    const Fvector up{0.f, 1.f, 0.f};
    for (const Fvector& q : bangs)
    {
        if (!C.fx_demolish.empty())
        {
            CParticlesObject* ps = CParticlesObject::Create(C.fx_demolish[::Random.randI(C.fx_demolish.size())].c_str(), TRUE);
            Fmatrix m;
            m.identity();
            m.c = q;
            ps->UpdateParent(m, {});
            GamePersistent().ps_needtoplay.push_back(ps);
        }
        if (S.snd_demolish._handle())
        {
            Fvector sp = q;
            float vol = 1.f;
            S.snd_demolish.play_no_feedback(nullptr, 0, ::Random.randF(0.f, 0.25f), &sp, &vol);
        }
    }
    // what stood on them comes down in parts: the bonds between the pieces over the band (in the radius) break by chance
    auto over = [&](const Piece& p) {
        if (p.state != ST_INTACT || p.aabb.min.y < a - 0.05f)
            return false;
        if (radius <= 0.f)
            return true;
        const float dx = _max(0.f, _max(p.aabb.min.x - pos.x, pos.x - p.aabb.max.x));
        const float dz = _max(0.f, _max(p.aabb.min.z - pos.z, pos.z - p.aabb.max.z));
        return dx * dx + dz * dz <= radius * radius;
    };
    u32 cut = 0, unanchored = 0;
    for (Piece& p : S.pieces) // the building may lean on a bank or a platform over the charges: nothing over them holds
        if (p.anchored && over(p))
        {
            p.anchored = false;
            ++unanchored;
        }
    for (Bond& bd : S.bonds)
        if (!bd.broken && over(S.pieces[bd.a]) && over(S.pieces[bd.b]) && ::Random.randF() < (bd.vertical ? C.demolish_break_v : C.demolish_break_h))
        {
            bd.broken = true;
            ++cut;
        }
    for (u16 pc : pcs)
        destroy_piece(pc, up, 0.5f);
    Msg("[destruct] demolish: %u pieces in the band %.1f..%.1f m (radius %.1f), %u charges, %u bonds over it cut, %u anchors over it let go",
        (u32)pcs.size(), y_lo, y_hi, radius, (u32)bangs.size(), cut, unanchored);
    if (S.dirty)
        solve();
    u32 low = 0, high = 0, held = 0;
    for (const Piece& p : S.pieces)
        if (p.state == ST_INTACT)
        {
            (p.aabb.max.y < a ? low : high)++;
            held += p.anchored;
        }
    Msg("[destruct] demolish: standing after it %u under the band, %u reaching over it (%u anchored)", low, high, held);
    return (int)pcs.size();
}

// E56: the radius of the biggest dent reaching `pos` (within its rim of its centre; 0: none)
float lua_dent_at(const Fvector& pos)
{
    float best = 0.f;
    for (const Dent& d : S.dents)
        if (d.r > best && d.c.distance_to(pos) < d.r * DENT_RIM + 0.03f)
            best = d.r;
    return best;
}

float lua_hole_at(const Fvector& pos)
{
    for (const Hole& h : S.holes)
        if (h.r > 0.f && h.c.distance_to(pos) < h.r)
            return h.r;
    return 0.f;
}
} // namespace portal_mod_destruct

// ------------------------------------------------------------------------------------------------------------- load hooks
bool portal_mod_destruct_cform(const Fvector* verts, size_t nv, const CDB::TRI* tris, size_t nt, xr_vector<Fvector>& out_v, xr_vector<CDB::TRI>& out_t)
{
    using namespace portal_mod_destruct;
    reset();
    load_cfg();
    if (!C.enabled)
        return false;
    string_path fn;
    if (!FS.exist(fn, fsgame::level, "level.pmd"))
        return false;
    IReader* F = FS.r_open(fn);
    if (!F)
        return false;
    const bool ok = read_pmd(*F, nv, nt);
    FS.r_close(F);
    if (!ok || S.pieces.empty())
    {
        reset();
        return false;
    }
    xr_vector<u8> cut(nt, 0);
    xr_map<u16, u32> sectors;
    // level.cform's triangles sit unaligned in the file (after a 36-byte header) while CDB::TRI is 16-byte aligned:
    // copied as bytes, never as a TRI (the compiler's aligned SSE load faults)
    auto tri_at = [tris](size_t i) {
        CDB::TRI t;
        CopyMemory(&t, reinterpret_cast<const u8*>(tris) + i * sizeof(CDB::TRI), sizeof(CDB::TRI));
        return t;
    };
    for (u32 i : S.cut_cform)
        if (i < nt)
        {
            cut[i] = 1;
            ++sectors[u16(tri_at(i).sector)];
        }
    u16 sector = 0;
    u32 best = 0;
    for (const auto& [s, n] : sectors)
        if (n > best)
            best = n, sector = s;
    out_v.assign(verts, verts + nv);
    out_t.clear();
    out_t.reserve(nt + S.app_t.size() / 3);
    out_t.resize(nt + S.app_t.size() / 3);
    size_t kept = 0;
    for (size_t i = 0; i < nt; ++i)
        if (!cut[i])
            CopyMemory(&out_t[kept++], reinterpret_cast<const u8*>(tris) + i * sizeof(CDB::TRI), sizeof(CDB::TRI));
    out_t.resize(kept);
    S.tri_base = (u32)out_t.size();
    const u32 vbase = (u32)out_v.size();
    S.vbase = vbase;
    out_v.insert(out_v.end(), S.app_v.begin(), S.app_v.end());
    xr_vector<u32> mtl_id(S.mtl.size());
    for (u32 m = 0; m < S.mtl.size(); ++m)
        mtl_id[m] = GMLib.GetMaterialID(S.mtl[m].c_str());
    S.tri_count = (u32)(S.app_t.size() / 3);
    S.tri_piece.assign(S.tri_count, 0);
    for (u32 pc = 0; pc < S.pieces.size(); ++pc)
    {
        const Piece& p = S.pieces[pc];
        for (u32 k = p.tri0; k < p.tri0 + p.ntri && k < S.tri_count; ++k)
            S.tri_piece[k] = u16(pc);
    }
    for (u32 k = 0; k < S.tri_count; ++k)
    {
        CDB::TRI t;
        t.dummy = 0;
        t.verts[0] = vbase + S.app_t[k * 3];
        t.verts[1] = vbase + S.app_t[k * 3 + 1];
        t.verts[2] = vbase + S.app_t[k * 3 + 2];
        const u8 m = S.pieces[S.tri_piece[k]].mat;
        t.material = m < mtl_id.size() ? mtl_id[m] : 0;
        t.suppress_shadows = 1;
        t.suppress_wm = 1; // no bullet marks: they would stay in the air when the piece goes
        t.sector = sector;
        out_t.push_back(t);
    }
    S.sink = (u32)out_v.size();
    out_v.push_back(Fvector{0.f, -9000.f, 0.f});
    out_v.push_back(Fvector{0.02f, -9000.f, 0.f});
    out_v.push_back(Fvector{0.f, -9000.f, 0.02f});
    S.app_v.clear();
    S.app_v.shrink_to_fit();
    S.loaded = true; // (app_t stays: E54d's rebuild puts the pieces' triangles back when a save is loaded on this level)
    calibrate();
    Msg("* [destruct] %s: %u level collision triangles out, %u piece triangles in", S.name.c_str(), (u32)S.cut_cform.size(), S.tri_count);
    return true;
}

void portal_mod_destruct_ib(bool fast, u32 ib, u16* data, u32 count)
{
    using namespace portal_mod_destruct;
    if (!S.loaded)
        return;
    auto it = S.ib_cuts.find(ib_key(fast, ib));
    if (it == S.ib_cuts.end())
        return;
    IbCopy c{fast, ib, xr_vector<u16>(data, data + count)};
    for (u32 off : it->second)
        if (off + 2 < count)
            c.data[off + 1] = c.data[off + 2] = c.data[off];
    S.ib_copies.push_back(std::move(c));
}
