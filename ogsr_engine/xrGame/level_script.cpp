////////////////////////////////////////////////////////////////////////////
//	Module 		: level_script.cpp
//	Created 	: 28.06.2004
//  Modified 	: 28.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Level script export
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "level.h"
#include "actor.h"
#include "script_game_object.h"
#include "patrol_path_storage.h"
#include "xrServer.h"
#include "client_spawn_manager.h"
#include "..\xr_3da\IGame_Persistent.h"
#include "game_cl_base.h"
#include "ui/UIDialogWnd.h"
#include "date_time.h"
#include "ai_space.h"
#include "level_graph.h"
#include "PHCommander.h"
#include "PHScriptCall.h"
#include "HUDManager.h"
#include "script_engine.h"
#include "game_cl_single.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "map_manager.h"
#include "map_location.h"
#include "portal_mod.h" // portal-mod E10
#include "phworld.h"
#include "../xrcdb/xr_collide_defs.h"
#include "script_rq_result.h"
#include "monster_community.h"
#include "GamePersistent.h"
#include "EffectorBobbing.h"
#include "LevelDebugScript.h"
#include "CharacterPhysicsSupport.h" // portal-mod
#include "PHMovementControl.h"
#include "PHSimpleCharacter.h" // portal-mod E17
#include "CustomMonster.h" // portal-mod E24
#include "PhysicsShellHolder.h" // portal-mod E41
#include "PhysicsShell.h"
#include "PHObject.h" // portal-mod E42
#include "movement_manager.h"
#include "detail_path_manager.h"

// portal-mod: Lua side of the Portal 2 portals and the actor's velocity (momentum through portals)
static void portal_set(int idx, Fvector pos, Fvector normal, Fvector up, float w, float h, float color, bool linked, float open)
{
    if (idx < 0 || idx > 1)
        return;
    Fvector n = normal, u = up, r;
    n.normalize_safe();
    u.normalize_safe();
    r.crossproduct(u, n).normalize_safe();
    PORTAL_MOD_DRAW& p = portal_mod_draw[idx];
    p.xform.identity();
    p.xform.i.mul(r, w * 0.5f);
    p.xform.j.mul(u, h * 0.5f);
    p.xform.k.set(n);
    p.xform.c.mad(pos, n, 0.012f); // a centimetre off the wall: no z-fighting
    p.params.set(color, linked ? 1.f : 0.f, open, 0.f);
    p.on = true;
}

static void portal_view_roll(float roll) { portal_mod::view_roll = roll; } // portal-mod E10

static void portal_clear(int idx)
{
    if (idx >= 0 && idx <= 1)
        portal_mod_draw[idx].on = false;
}

// Normal of the static triangle a ray hits (facing the ray), or (0,0,0) if it hits nothing static.
static Fvector ray_query_normal(Fvector start, Fvector dir, float range)
{
    Fvector res{};
    collide::rq_result RQ;
    CObject* ignore = Level().CurrentEntity();
    if (Level().ObjectSpace.RayPick(start, dir, range, collide::rqtStatic, RQ, ignore) && !RQ.O)
    {
        const CDB::TRI& t = Level().ObjectSpace.GetStaticTris()[RQ.element];
        const Fvector* v = Level().ObjectSpace.GetStaticVerts();
        Fvector e1, e2;
        e1.sub(v[t.verts[1]], v[t.verts[0]]);
        e2.sub(v[t.verts[2]], v[t.verts[0]]);
        res.crossproduct(e1, e2).normalize_safe();
        if (res.dotproduct(dir) > 0.f)
            res.invert();
    }
    return res;
}

static Fvector actor_velocity()
{
    Fvector v{};
    if (CActor* a = Actor())
        v.set(a->character_physics_support()->movement()->GetVelocity());
    return v;
}

static void portal_set_actor_position(Fvector p) // portal-mod E12
{
    if (CActor* a = Actor())
        a->portal_mod_place(p);
}

static void actor_arm_aim(Fvector p, float k) // portal-mod E34: the third-person body's right arm held out at p
{
    if (CActor* a = Actor())
        a->portal_mod_arm_aim(p, k);
}

static void actor_tp_dual(bool on, Fvector p) // portal-mod E36: two guns in the third person, both arms out at p
{
    if (CActor* a = Actor())
        a->portal_mod_tp_dual(on, p);
}

// portal-mod E43: a two-handed grip in the third person: the wrists on r and l (world), the elbows bent out by elbow_out
static void actor_tp_grip(bool on, Fvector r, Fvector l, float elbow_out)
{
    if (CActor* a = Actor())
        a->portal_mod_tp_grip(on, r, l, elbow_out);
}

// portal-mod E43: the grip's hands: knuckle axes (a zero vector: along the forearm), knuckle normals, finger curls (rad)
static void actor_tp_grip_hands(Fvector rf, Fvector rn, Fvector lf, Fvector ln, float rc, float lc)
{
    if (CActor* a = Actor())
        a->portal_mod_tp_grip_hands(rf, rn, lf, ln, rc, lc);
}

static void portal_fly_actor(Fvector v) // portal-mod E17: out of a portal at speed (flies, see CPHSimpleCharacter)
{
    if (CActor* a = Actor())
        if (auto* ch = dynamic_cast<CPHSimpleCharacter*>(a->character_physics_support()->movement()->PHCharacter()))
            ch->portal_mod_fly(v);
}

// portal-mod E24: an NPC or a mutant put at pos moving at vel, as out of a portal (E8)
static void portal_move_npc(u16 id, Fvector pos, Fvector vel)
{
    CCustomMonster* m = smart_cast<CCustomMonster*>(Level().Objects.net_Find(id));
    if (!m || !m->g_Alive() || m->H_Parent() || !m->character_physics_support())
        return;
    CPHMovementControl* mc = m->character_physics_support()->movement();
    if (!mc || !mc->CharacterExist())
        return;
    Fmatrix F = m->XFORM();
    F.c = pos;
    m->XFORM().set(F);
    mc->EnableCharacter();
    mc->SetPosition(pos);
    mc->SetVelocity(vel);
    m->movement().detail().make_inactual();
    if (m->animation_movement_controlled())
        m->destroy_anim_mov_ctrl();
}

// portal-mod E41: a physics object (a scene's prop) held at pos, its +z along dir and +y toward up: the shell's bodies
// put there with the interpolation restarted, the velocities zeroed; collide false lets the actor through it
static void portal_pose_object(u16 id, Fvector pos, Fvector dir, Fvector up, bool collide)
{
    auto* o = smart_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(id));
    if (!o || o->H_Parent())
        return;
    CPhysicsShell* sh = o->PPhysicsShell();
    if (!sh || !sh->isActive())
        return;
    Fvector k = dir, i, j;
    if (k.square_magnitude() < EPS)
        return;
    k.normalize();
    i.crossproduct(up, k);
    if (i.square_magnitude() < EPS)
        i.crossproduct(Fvector().set(0.f, 0.f, 1.f), k);
    if (i.square_magnitude() < EPS)
        i.crossproduct(Fvector().set(1.f, 0.f, 0.f), k);
    i.normalize();
    j.crossproduct(k, i);
    Fmatrix m;
    m.set(i, j, k, pos);
    // portal-mod E42: out of the spatial alone, the prop's own collision pass still found the actor and its fixed body
    // shoved it - the collide-class bits (CPHCollideValidator's private cbNCClassDynamic 1 << 3, cbNCClassCharacter
    // 1 << 5) make it ignore characters and dynamic bodies too
    constexpr CLClassBits ignore_bits = (1u << 3) | (1u << 5);
    if (auto* pho = dynamic_cast<CPHObject*>(sh))
        pho->collide_class_bits().set(ignore_bits, !collide);
    if (collide)
        sh->EnableCollision();
    else
        sh->DisableCollision();
    sh->SetGlTransformDynamic(m);
    sh->set_LinearVel(Fvector().set(0.f, 0.f, 0.f));
    sh->set_AngularVel(Fvector().set(0.f, 0.f, 0.f));
    o->XFORM().set(m);
}

static void set_actor_velocity(Fvector v)
{
    if (CActor* a = Actor())
        a->character_physics_support()->movement()->SetVelocity(v);
}

using namespace luabind;

LPCSTR command_line() { return (Core.Params); }

#ifdef DEBUG
void check_object(CScriptGameObject* object)
{
    try
    {
        Msg("check_object %s", object->Name());
    }
    catch (...)
    {
        object = object;
    }
}

CScriptGameObject* tpfGetActor()
{
    static bool first_time = true;
    if (first_time)
        ai().script_engine().script_log(eLuaMessageTypeError, "Do not use level.actor function!");
    first_time = false;

    CActor* l_tpActor = smart_cast<CActor*>(Level().CurrentEntity());
    if (l_tpActor)
        return (smart_cast<CGameObject*>(l_tpActor)->lua_game_object());
    else
        return (0);
}

CScriptGameObject* get_object_by_name(LPCSTR caObjectName)
{
    static bool first_time = true;
    if (first_time)
        ai().script_engine().script_log(eLuaMessageTypeError, "Do not use level.object function!");
    first_time = false;

    CGameObject* l_tpGameObject = smart_cast<CGameObject*>(Level().Objects.FindObjectByName(caObjectName));
    if (l_tpGameObject)
        return (l_tpGameObject->lua_game_object());
    else
        return (0);
}
#endif

CScriptGameObject* get_object_by_id(u32 id)
{
    CGameObject* pGameObject = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    if (!pGameObject || pGameObject->getDestroy())
        return nullptr;

    return pGameObject->lua_game_object();
}

LPCSTR get_weather() { return (*g_pGamePersistent->Environment().GetWeather()); }

LPCSTR get_weather_prev() { return (*g_pGamePersistent->Environment().GetPrevWeather()); }

u32 get_weather_last_shift() { return g_pGamePersistent->Environment().GetWeatherLastShift(); }

extern bool s_ScriptWeather;

void set_weather(LPCSTR weather_name, bool forced)
{
    if (s_ScriptWeather)
        return;

    // KRodin: ТЧ погоду всегда надо обновлять форсировано, иначе она почему-то не всегда корректно обновляется. А для ЗП погоды так делать нельзя - будут очень резкие переходы!
    if (!g_pGamePersistent->Environment().USED_COP_WEATHER)
        forced = true;

    g_pGamePersistent->Environment().SetWeather(weather_name, forced);
}

void set_weather_next(LPCSTR weather_name)
{
    if (s_ScriptWeather)
        return;

    g_pGamePersistent->Environment().SetWeatherNext(weather_name);
}

bool set_weather_fx(LPCSTR weather_name)
{
    if (s_ScriptWeather)
        return false;

    return g_pGamePersistent->Environment().SetWeatherFX(weather_name);
}

bool start_weather_fx_from_time(LPCSTR weather_name, float time)
{
    if (s_ScriptWeather)
        return false;

    return g_pGamePersistent->Environment().SetWeatherFXFromTime(weather_name, time);
}

bool is_wfx_playing() { return (g_pGamePersistent->Environment().IsWeatherFXPlaying()); }

float get_wfx_time() { return (g_pGamePersistent->Environment().wfx_time); }

void stop_weather_fx() { g_pGamePersistent->Environment().StopWeatherFX(); }

void set_time_factor(float time_factor)
{
    Level().Server->game->SetGameTimeFactor(time_factor);
    GamePersistent().Environment().SetGameTime(Level().GetEnvironmentGameDayTimeSec(), Level().game->GetEnvironmentGameTimeFactor());
}

float get_time_factor() { return (Level().GetGameTimeFactor()); }

void set_game_difficulty(ESingleGameDifficulty dif)
{
    g_SingleGameDifficulty = dif;
    game_cl_Single* game = smart_cast<game_cl_Single*>(Level().game);
    VERIFY(game);
    game->OnDifficultyChanged();
}
ESingleGameDifficulty get_game_difficulty() { return g_SingleGameDifficulty; }

u32 get_time_days()
{
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, milisecs = 0;
    split_time((g_pGameLevel && Level().game) ? Level().GetGameTime() : ai().alife().time_manager().game_time(), year, month, day, hours, mins, secs, milisecs);
    return day;
}

u32 get_time_hours()
{
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, milisecs = 0;
    split_time((g_pGameLevel && Level().game) ? Level().GetGameTime() : ai().alife().time_manager().game_time(), year, month, day, hours, mins, secs, milisecs);
    return hours;
}

u32 get_time_minutes()
{
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, milisecs = 0;
    split_time((g_pGameLevel && Level().game) ? Level().GetGameTime() : ai().alife().time_manager().game_time(), year, month, day, hours, mins, secs, milisecs);
    return mins;
}

float high_cover_in_direction(u32 level_vertex_id, const Fvector& direction)
{
    float y, p;
    direction.getHP(y, p);
    return (ai().level_graph().high_cover_in_direction(y, level_vertex_id));
}

float low_cover_in_direction(u32 level_vertex_id, const Fvector& direction)
{
    float y, p;
    direction.getHP(y, p);
    return (ai().level_graph().low_cover_in_direction(y, level_vertex_id));
}

float cover_in_direction(u32 level_vertex_id, const Fvector& direction)
{
    float y, p;
    direction.getHP(y, p);
    const float high_cover_value = ai().level_graph().high_cover_in_direction(y, level_vertex_id);
    const float low_cover_value = ai().level_graph().low_cover_in_direction(y, level_vertex_id);
    return std::min(high_cover_value, low_cover_value);
}

float rain_factor() { return g_pGamePersistent->Environment().CurrentEnv->rain_density; }

float rain_hemi()
{
    CObject* E = g_pGameLevel->CurrentViewEntity();
    if (E && E->renderable_ROS())
    {
        const float* hemi_cube = E->renderable_ROS()->get_luminocity_hemi_cube();
        float hemi_val = _max(hemi_cube[0], hemi_cube[1]);
        hemi_val = _max(hemi_val, hemi_cube[2]);
        hemi_val = _max(hemi_val, hemi_cube[3]);
        hemi_val = _max(hemi_val, hemi_cube[5]);
        return hemi_val;
    }
    return 0.f;
}

u32 vertex_in_direction(u32 level_vertex_id, Fvector direction, float max_distance)
{
    direction.normalize_safe();
    direction.mul(max_distance);
    Fvector start_position = ai().level_graph().vertex_position(level_vertex_id);
    Fvector finish_position = Fvector(start_position).add(direction);
    u32 result = u32(-1);
    ai().level_graph().farthest_vertex_in_direction(level_vertex_id, start_position, finish_position, result, 0);
    return (ai().level_graph().valid_vertex_id(result) ? result : level_vertex_id);
}

Fvector vertex_position(u32 level_vertex_id) { return (ai().level_graph().vertex_position(level_vertex_id)); }

void map_add_object_spot(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().AddMapLocation(spot_type, id);
    if (text && xr_strlen(text))
        ml->SetHint(text);
}

void map_add_object_spot_ser(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().AddMapLocation(spot_type, id);
    if (text && xr_strlen(text))
        ml->SetHint(text);

    ml->SetSerializable(true);
}

#include "graph_engine.h"

u16 map_add_user_spot(u8 level_id, Fvector position, LPCSTR spot_type, LPCSTR text)
{
    shared_str level_name = ai().game_graph().header().level((GameGraph::_LEVEL_ID)level_id).name();

    CMapLocation* ml = Level().MapManager().AddUserLocation(spot_type, level_name, position);
    if (xr_strlen(text))
        ml->SetHint(text);

    return ml->ObjectID();
}

void map_change_spot_hint(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetHint(text);
}

void map_change_spot_ser(u16 id, LPCSTR spot_type, BOOL v)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetSerializable(!!v);
}

void prefetch_many_sounds( LPCSTR prefix ) {
  Level().PrefetchManySoundsLater( prefix );
}


void map_remove_object_spot(u16 id, LPCSTR spot_type) { Level().MapManager().RemoveMapLocation(spot_type, id); }

u16 map_has_object_spot(u16 id, LPCSTR spot_type) { return Level().MapManager().HasMapLocation(spot_type, id); }

bool patrol_path_exists(LPCSTR patrol_path) { return (!!ai().patrol_paths().path(patrol_path, true)); }

LPCSTR get_name() { return (*Level().name()); }

void prefetch_sound(LPCSTR name) { Level().PrefetchSound(name); }

CClientSpawnManager& get_client_spawn_manager() { return (Level().client_spawn_manager()); }

void start_stop_menu(CUIDialogWnd* pDialog, bool bDoHideIndicators) { HUD().GetUI()->StartStopMenu(pDialog, bDoHideIndicators); }

void add_dialog_to_render(CUIDialogWnd* pDialog) { HUD().GetUI()->AddDialogToRender(pDialog); }

void remove_dialog_to_render(CUIDialogWnd* pDialog) { HUD().GetUI()->RemoveDialogToRender(pDialog); }

CUIDialogWnd* main_input_receiver() { return HUD().GetUI()->MainInputReceiver(); }

#include "UIGameCustom.h"
#include "ui/UIInventoryWnd.h"
#include "ui/UITradeWnd.h"
#include "ui/UITalkWnd.h"
#include "ui/UICarBodyWnd.h"
#include "UIGameSP.h"
#include "HUDManager.h"
#include "HUDTarget.h"
#include "InventoryBox.h"

CUIWindow* GetInventoryWindow()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    return (CUIWindow*)pGameSP->InventoryMenu;
}
CUIWindow* GetTradeWindow()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    return (CUIWindow*)pGameSP->TalkMenu->GetTradeWnd();
}
CUIWindow* GetTalkWindow()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    return (CUIWindow*)pGameSP->TalkMenu;
}
CScriptGameObject* GetSecondTalker()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    CUITalkWnd* wnd = pGameSP->TalkMenu;
    if (wnd == nullptr)
        return nullptr;
    return smart_cast<CGameObject*>(wnd->GetSecondTalker())->lua_game_object();
}
CUIWindow* GetPdaWindow()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    return (CUIWindow*)pGameSP->PdaMenu;
}
CUIWindow* GetCarBodyWindow()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    return (CUIWindow*)pGameSP->UICarBodyMenu;
}
CScriptGameObject* GetCarBodyTarget()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    CUICarBodyWnd* wnd = pGameSP->UICarBodyMenu;
    if (wnd == nullptr)
        return nullptr;
    if (wnd->m_pOthersObject != nullptr)
        return smart_cast<CGameObject*>(wnd->m_pOthersObject)->lua_game_object();
    if (wnd->m_pInventoryBox != nullptr)
        return (wnd->m_pInventoryBox->object().lua_game_object());
    return nullptr;
}

CUIWindow* GetUIChangeLevelWnd()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!pGameSP)
        return nullptr;
    return (CUIWindow*)pGameSP->UIChangeLevelWnd;
}

script_rq_result PerformRayQuery(Fvector start, Fvector dir, float range, collide::rq_target tgt, CScriptGameObject* ignore)
{
    collide::rq_result RQ;
    script_rq_result res;
    CObject* obj = nullptr;
    if (ignore)
        obj = smart_cast<CObject*>(&(ignore->object()));
    if (Level().ObjectSpace.RayPick(start, dir, range, tgt, RQ, obj))
    {
        res.set_result(RQ);
    }
    return res;
}

/*void DisableActorCallbacks(u32 _fl)
{
    psCallbackFlags.set(_fl, true);
}*/

float GetTargetDist() { return ((CHUDManager*)g_hud)->GetTarget()->GetDist(); }

script_rq_result GetCurrentRayQuery()
{
    collide::rq_result& RQ = HUD().GetCurrentRayQuery();
    script_rq_result res;
    res.set_result(RQ);
    return res;
}

CScriptGameObject* GetTargetObj()
{
    CObject* obj = ((CHUDManager*)g_hud)->GetTarget()->GetObj();
    if (!obj)
        return nullptr;
    return smart_cast<CGameObject*>(obj)->lua_game_object();
}

void hide_indicators()
{
    HUD().GetUI()->UIGame()->HideShownDialogs();

    HUD().GetUI()->HideGameIndicators();
    HUD().GetUI()->HideCrosshair();
}

void show_indicators()
{
    HUD().GetUI()->ShowGameIndicators();
    HUD().GetUI()->ShowCrosshair();
}

bool game_indicators_shown() { return HUD().GetUI()->GameIndicatorsShown(); }

Flags32 get_hud_flags() { return psHUD_Flags; }

bool is_level_present() { return (!!g_pGameLevel); }

bool is_removing_objects_script() { return Level().is_removing_objects(); }

CPHCall* add_call(const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    luabind::functor<bool> _condition = condition;
    luabind::functor<void> _action = action;
    CPHScriptCondition* c = xr_new<CPHScriptCondition>(_condition);
    CPHScriptAction* a = xr_new<CPHScriptAction>(_action);
    return Level().ph_commander_scripts().add_call(c, a);
}

void remove_call(const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    CPHScriptCondition c(condition);
    CPHScriptAction a(action);
    Level().ph_commander_scripts().remove_call(&c, &a);
}

CPHCall* add_call(const luabind::object& lua_object, LPCSTR condition, LPCSTR action)
{
    //	try{
    //		CPHScriptObjectCondition	*c=xr_new<CPHScriptObjectCondition>(lua_object,condition);
    //		CPHScriptObjectAction		*a=xr_new<CPHScriptObjectAction>(lua_object,action);
    luabind::functor<bool> _condition = object_cast<luabind::functor<bool>>(lua_object[condition]);
    luabind::functor<void> _action = object_cast<luabind::functor<void>>(lua_object[action]);
    CPHScriptObjectConditionN* c = xr_new<CPHScriptObjectConditionN>(lua_object, _condition);
    CPHScriptObjectActionN* a = xr_new<CPHScriptObjectActionN>(lua_object, _action);
    return Level().ph_commander_scripts().add_call_unique(c, c, a, a);
    //	}
    //	catch(...)
    //	{
    //		Msg("add_call excepted!!");
    //	}
}

void remove_call(const luabind::object& lua_object, LPCSTR condition, LPCSTR action)
{
    CPHScriptObjectCondition c(lua_object, condition);
    CPHScriptObjectAction a(lua_object, action);
    Level().ph_commander_scripts().remove_call(&c, &a);
}

CPHCall* add_call(const luabind::object& lua_object, const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    CPHScriptObjectConditionN* c = xr_new<CPHScriptObjectConditionN>(lua_object, condition);
    CPHScriptObjectActionN* a = xr_new<CPHScriptObjectActionN>(lua_object, action);
    return Level().ph_commander_scripts().add_call(c, a);
}

void remove_call(const luabind::object& lua_object, const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    CPHScriptObjectConditionN c(lua_object, condition);
    CPHScriptObjectActionN a(lua_object, action);
    Level().ph_commander_scripts().remove_call(&c, &a);
}

void remove_calls_for_object(const luabind::object& lua_object)
{
    CPHSriptReqObjComparer c(lua_object);
    Level().ph_commander_scripts().remove_calls(&c);
}

CPHWorld* physics_world() { return ph_world; }
CEnvironment* environment() { return &g_pGamePersistent->Environment(); }

extern bool g_bDisableAllInput;

void disable_input()
{
    g_bDisableAllInput = true;
    if (Actor())
        Actor()->PickupModeOff();
}
void enable_input()
{
    g_bDisableAllInput = false;

    Fvector2 pos = GetUICursor()->GetCursorPosition();
    GetUICursor()->SetUICursorPosition(pos);
}

bool g_block_all_except_movement{};
bool g_actor_allow_ladder{true};
bool g_actor_allow_pda{true};

void block_all_except_movement(bool b) { g_block_all_except_movement = b; }

bool only_movement_allowed() { return g_block_all_except_movement; }

void set_actor_allow_ladder(bool b) { g_actor_allow_ladder = b; }

bool actor_ladder_allowed() { return g_actor_allow_ladder; }

void set_actor_allow_pda(bool b) { g_actor_allow_pda = b; }

bool actor_pda_allowed() { return g_actor_allow_pda; }

void spawn_phantom(const Fvector& position) { Level().spawn_item("m_phantom", position, u32(-1), u16(-1), false); }

Fbox get_bounding_volume() { return Level().ObjectSpace.GetBoundingVolume(); }

void iterate_sounds(LPCSTR prefix, u32 max_count, const CScriptCallbackEx<void>& callback)
{
    for (int j = 0, N = _GetItemCount(prefix); j < N; ++j)
    {
        string_path fn, s;
        LPSTR S = (LPSTR)&s;
        _GetItem(prefix, j, S);
        if (FS.exist(fn, "$game_sounds$", S, ".ogg"))
            callback(prefix);

        for (u32 i = 0; i < max_count; ++i)
        {
            string_path name;
            sprintf_s(name, "%s%d", S, i);
            if (FS.exist(fn, "$game_sounds$", name, ".ogg"))
                callback(name);
        }
    }
}

void iterate_sounds1(LPCSTR prefix, u32 max_count, const luabind::functor<void>& functor)
{
    CScriptCallbackEx<void> temp;
    temp.set(functor);
    iterate_sounds(prefix, max_count, temp);
}

void iterate_sounds2(LPCSTR prefix, u32 max_count, const luabind::object& object, const luabind::functor<void>& functor)
{
    CScriptCallbackEx<void> temp;
    temp.set(functor, object);
    iterate_sounds(prefix, max_count, temp);
}

#include "actoreffector.h"
float add_cam_effector(LPCSTR fn, int id, bool cyclic, LPCSTR cb_func)
{
    CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>(cb_func);
    e->SetType((ECamEffectorType)id);
    e->SetCyclic(cyclic);
    e->Start(fn);
    Actor()->Cameras().AddCamEffector(e);
    return e->GetAnimatorLength();
}

float add_cam_effector2(LPCSTR fn, int id, bool cyclic, LPCSTR cb_func)
{
    CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>(cb_func);
    e->m_bAbsolutePositioning = true;
    e->SetType((ECamEffectorType)id);
    e->SetCyclic(cyclic);
    e->Start(fn);
    Actor()->Cameras().AddCamEffector(e);
    return e->GetAnimatorLength();
}

void remove_cam_effector(int id) { Actor()->Cameras().RemoveCamEffector((ECamEffectorType)id); }

// portal-mod E21: a scene's camera, set from a script every frame; absolute and applied last (AbsolutePositioning
// puts it at the front of the list, processed last); it drops out by itself once level.cine_cam_off() is called
static Fvector pm_cine_p{}, pm_cine_d{0.f, 0.f, 1.f};
static float pm_cine_fov{};
float get_snd_volume() { return psSoundVFactor; }

void set_snd_volume(float v)
{
    psSoundVFactor = v;
    clamp(psSoundVFactor, 0.0f, 1.0f);
}
#include "actor_statistic_mgr.h"
void add_actor_points(LPCSTR sect, LPCSTR detail_key, int cnt, int pts) { return Actor()->StatisticMgr().AddPoints(sect, detail_key, cnt, pts); }

void add_actor_points_str(LPCSTR sect, LPCSTR detail_key, LPCSTR str_value) { return Actor()->StatisticMgr().AddPoints(sect, detail_key, str_value); }

int get_actor_points(LPCSTR sect) { return Actor()->StatisticMgr().GetSectionPoints(sect); }

void remove_actor_points(LPCSTR sect, LPCSTR detail_key) { Actor()->StatisticMgr().RemovePoints(sect, detail_key); }
extern int get_actor_ranking();
extern void add_human_to_top_list(u16 id);
extern void remove_human_from_top_list(u16 id);

#include "ActorEffector.h"
void add_complex_effector(LPCSTR section, int id) { AddEffector(Actor(), id, section); }

void remove_complex_effector(int id) { RemoveEffector(Actor(), id); }

#include "postprocessanimator.h"
void add_pp_effector(LPCSTR fn, int id, bool cyclic)
{
    CPostprocessAnimator* pp = xr_new<CPostprocessAnimator>(id, cyclic);
    pp->Load(fn);
    Actor()->Cameras().AddPPEffector(pp);
}

void remove_pp_effector(int id)
{
    CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(Actor()->Cameras().GetPPEffector((EEffectorPPType)id));

    if (pp)
        pp->Stop(1.0f);
}

void set_pp_effector_factor(int id, float f, float f_sp)
{
    CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(Actor()->Cameras().GetPPEffector((EEffectorPPType)id));

    if (pp)
        pp->SetDesiredFactor(f, f_sp);
}

void set_pp_effector_factor2(int id, float f)
{
    CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(Actor()->Cameras().GetPPEffector((EEffectorPPType)id));

    if (pp)
        pp->SetCurrentFactor(f);
}

#include "relation_registry.h"

int g_community_goodwill(LPCSTR _community, int _entity_id)
{
    CHARACTER_COMMUNITY c;
    c.set(_community);

    return RELATION_REGISTRY().GetCommunityGoodwill(c.index(), u16(_entity_id));
}

void g_set_community_goodwill(LPCSTR _community, int _entity_id, int val)
{
    CHARACTER_COMMUNITY c;
    c.set(_community);
    RELATION_REGISTRY().SetCommunityGoodwill(c.index(), u16(_entity_id), val);
}

void g_change_community_goodwill(LPCSTR _community, int _entity_id, int val)
{
    CHARACTER_COMMUNITY c;
    c.set(_community);
    RELATION_REGISTRY().ChangeCommunityGoodwill(c.index(), u16(_entity_id), val);
}
int g_get_personal_goodwill(int _who_id, int _to_whom_id) { return RELATION_REGISTRY().GetGoodwill(u16(_who_id), u16(_to_whom_id)); }
void g_set_personal_goodwill(int _who_id, int _to_whom_id, int _amount) { RELATION_REGISTRY().SetGoodwill(u16(_who_id), u16(_to_whom_id), _amount); }
void g_change_personal_goodwill(int _who_id, int _to_whom_id, int _amount)
{
    CHARACTER_GOODWILL gw = RELATION_REGISTRY().GetGoodwill(u16(_who_id), u16(_to_whom_id));
    RELATION_REGISTRY().SetGoodwill(u16(_who_id), u16(_to_whom_id), gw + _amount);
}
void g_clear_personal_goodwill(int _who_id, int _to_whom_id) { RELATION_REGISTRY().ClearGoodwill(u16(_who_id), u16(_to_whom_id)); }
void g_clear_personal_relations(int _who_id) { RELATION_REGISTRY().ClearRelations(u16(_who_id)); }
void set_ignore_game_state_update() { Game().m_need_to_update = false; }

static void g_set_artefact_position(const u32 i, const float x, const float y, const float z)
{
    if (fis_zero(x) && fis_zero(y) && fis_zero(z))
        shader_exports.set_artefact_position(i, Fvector2{});
    else
    {
        Fvector pos{x, y, z};
        Device.mView.transform_tiny(pos);
        shader_exports.set_artefact_position(i, Fvector2{pos.x, pos.z});
    }
}

static void g_set_anomaly_position(const u32 i, const float x, const float y, const float z)
{
    if (fis_zero(x) && fis_zero(y) && fis_zero(z))
        shader_exports.set_anomaly_position(i, Fvector2{});
    else
    {
        Fvector pos{x, y, z};
        Device.mView.transform_tiny(pos);
        shader_exports.set_anomaly_position(i, Fvector2{pos.x, pos.z});
    }
}

static void g_set_detector_params(const int _one, const int _two) { shader_exports.set_detector_params(Ivector2{_one, _two}); }

#include "game_sv_single.h"
void AdvanceGameTime(u32 _ms)
{
    auto game = smart_cast<game_sv_Single*>(Level().Server->game);
    game->alife().time_manager().advance_game_time(_ms);

    Level().game->SetGameTimeFactor(ai().get_alife() ? ai().alife().time().game_time() : Level().GetGameTime(), Level().game->GetGameTimeFactor());
    GamePersistent().Environment().SetGameTime(Level().GetEnvironmentGameDayTimeSec(), Level().game->GetEnvironmentGameTimeFactor());
}

//
void send_event_key_press(int dik) //Нажатие клавиши
{
    Level().IR_OnKeyboardPress(dik);
}
void send_event_key_release(int dik) //Отпускание клавиши
{
    Level().IR_OnKeyboardRelease(dik);
}
void send_event_key_hold(int dik) //Удержание клавиши.
{
    Level().IR_OnKeyboardHold(dik);
}
void send_event_mouse_wheel(int vol) //Вращение колеса мыши
{
    Level().IR_OnMouseWheel(vol);
}
//

// Real Wolf 07.07.2014
u32 vertex_id(const Fvector& vec) { return ai().level_graph().vertex_id(vec); }

u32 vertex_id(u32 node, const Fvector& vec) { return ai().level_graph().vertex_id(node, vec); }

u32 nearest_vertex_id(const Fvector& vec) { return ai().level_graph().nearest_vertex_id(vec); }

void update_inventory_window() { HUD().GetUI()->UIGame()->ReInitShownUI(); }

void update_inventory_weight()
{
    CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (pGameSP)
        pGameSP->InventoryMenu->UpdateWeight();
}

void change_level(GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, Fvector pos, Fvector dir)
{
    NET_Packet p;
    p.w_begin(M_CHANGE_LEVEL);
    p.w(&game_vertex_id, sizeof(game_vertex_id));
    p.w(&level_vertex_id, sizeof(level_vertex_id));
    p.w_vec3(pos);
    p.w_vec3(dir);
    Level().Send(p, net_flags(TRUE));
}

void set_cam_inert(float v)
{
    psCamInert = v;
    clamp(psCamInert, 0.0f, 1.0f);
}

void set_monster_relation(LPCSTR from, LPCSTR to, int rel) { MONSTER_COMMUNITY::set_relation((MONSTER_COMMUNITY_ID)from, (MONSTER_COMMUNITY_ID)to, rel); }

void patrol_path_add(LPCSTR patrol_path, CPatrolPath* path) { ai().patrol_paths_raw().add_path(shared_str(patrol_path), path); }

void patrol_path_remove(LPCSTR patrol_path) { ai().patrol_paths_raw().remove_path(shared_str(patrol_path)); }

bool valid_vertex_id(u32 level_vertex_id) { return ai().level_graph().valid_vertex_id(level_vertex_id); }

u32 vertex_count() { return ai().level_graph().header().vertex_count(); }

void disable_vertex(u32 vertex_id) { ai().level_graph().set_mask(vertex_id); }
void enable_vertex(u32 vertex_id) { ai().level_graph().clear_mask(vertex_id); }

bool is_accessible_vertex_id(u32 level_vertex_id) { return ai().level_graph().is_accessible(level_vertex_id); }

IC Fvector construct_position(u32 level_vertex_id, float x, float z) { return Fvector().set(x, ai().level_graph().vertex_plane_y(level_vertex_id, x, z), z); }

IC bool CSpaceRestrictionBase_inside(Fsphere m_sphere, const Fvector& position, const float& radius)
{
    Fsphere sphere;
    sphere.P = position;
    sphere.R = radius;
    return sphere.intersect(m_sphere);
}

bool CSpaceRestrictionBase_inside(Fsphere m_sphere, u32 level_vertex_id, bool partially_inside, float radius = EPS_L)
{
    const Fvector& position = ai().level_graph().vertex_position(level_vertex_id);
    float offset = ai().level_graph().header().cell_size() * .5f - EPS_L;
    if (partially_inside)
        return (CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x + offset, position.z + offset), radius) ||
                CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x + offset, position.z - offset), radius) ||
                CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x - offset, position.z + offset), radius) ||
                CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x - offset, position.z - offset), radius) ||
                CSpaceRestrictionBase_inside(m_sphere, Fvector().set(position.x, position.y, position.z), radius));
    else
        return (CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x + offset, position.z + offset), radius) &&
                CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x + offset, position.z - offset), radius) &&
                CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x - offset, position.z + offset), radius) &&
                CSpaceRestrictionBase_inside(m_sphere, construct_position(level_vertex_id, position.x - offset, position.z - offset), radius) &&
                CSpaceRestrictionBase_inside(m_sphere, Fvector().set(position.x, position.y, position.z), radius));
}

void iterate_vertices_inside(Fvector P, float R, bool partially_inside, const luabind::functor<void>& funct)
{
    Fvector start, dest;
    start.sub(Fvector().set(0, 0, 0), Fvector().set(R, 0.f, R));
    dest.add(Fvector().set(0, 0, 0), Fvector().set(R, 0.f, R));
    start.add(P);
    dest.add(P);
    Fsphere m_sphere;
    m_sphere.P = P;
    m_sphere.R = R;
    ai().level_graph().iterate_vertices(start, dest, [&](const auto& vertex) {
        if (CSpaceRestrictionBase_inside(m_sphere, ai().level_graph().vertex_id(&vertex), partially_inside))
            funct(ai().level_graph().vertex_id(&vertex));
    });
}

void iterate_vertices_border(Fvector P, float R, const luabind::functor<void>& funct)
{
    Fvector start, dest;
    start.sub(Fvector().set(0, 0, 0), Fvector().set(R, 0.f, R));
    dest.add(Fvector().set(0, 0, 0), Fvector().set(R, 0.f, R));
    start.add(P);
    dest.add(P);
    Fsphere m_sphere;
    m_sphere.P = P;
    m_sphere.R = R;
    ai().level_graph().iterate_vertices(start, dest, [&](const auto& vertex) {
        if (CSpaceRestrictionBase_inside(m_sphere, ai().level_graph().vertex_id(&vertex), true) &&
            !CSpaceRestrictionBase_inside(m_sphere, ai().level_graph().vertex_id(&vertex), false))
            funct(ai().level_graph().vertex_id(&vertex));
    });
}

int get_character_community_team(LPCSTR comm)
{
    CHARACTER_COMMUNITY community;
    community.set(comm);
    return community.team();
}

#include "../xr_3da/fdemorecord.h"

void demo_record_start()
{ 
    string_path fn{};
    g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoRecord>(fn));
}

void demo_record_stop() 
{
    g_pGameLevel->Cameras().RemoveCamEffector(cefDemo);
}

Fvector demo_record_get_position()
{ 
    CDemoRecord* demo = (CDemoRecord*)g_pGameLevel->Cameras().GetCamEffector(cefDemo); 
    return demo->m_Position;
}

void demo_record_set_position(Fvector p)
{
    CDemoRecord* demo = (CDemoRecord*)g_pGameLevel->Cameras().GetCamEffector(cefDemo);
    demo->m_Position = p;
}

Fvector demo_record_get_HPB()
{
    CDemoRecord* demo = (CDemoRecord*)g_pGameLevel->Cameras().GetCamEffector(cefDemo);
    return demo->m_HPB;
}

void demo_record_set_HPB(Fvector p)
{
    CDemoRecord* demo = (CDemoRecord*)g_pGameLevel->Cameras().GetCamEffector(cefDemo);
    demo->m_HPB = p;
}

void demo_record_set_direct_input(bool f)
{
    CDemoRecord* demo = (CDemoRecord*)g_pGameLevel->Cameras().GetCamEffector(cefDemo);
    demo->m_b_redirect_input_to_level = f;
}

CEffectorBobbing* get_effector_bobbing() { return Actor()->GetEffectorBobbing(); }

void iterate_nearest(const Fvector& pos, float radius, luabind::functor<bool> functor)
{
    xr_vector<CObject*> m_nearest;
    Level().ObjectSpace.GetNearest(m_nearest, pos, radius, NULL);

    if (!m_nearest.size())
        return;

    xr_vector<CObject*>::iterator it = m_nearest.begin();
    xr_vector<CObject*>::iterator it_e = m_nearest.end();
    for (; it != it_e; it++)
    {
        CGameObject* obj = smart_cast<CGameObject*>(*it);
        if (!obj)
            continue;
        if (functor(obj->lua_game_object()))
            break;
    }
}

float is_ray_intersect_sphere(Fvector pos, Fvector dir, Fvector C, float R)
{
    Fsphere sphere;
    sphere.P = C;
    sphere.R = R;
    dir.normalize_safe();
    float dist;
    return sphere.intersect_ray(pos, dir, dist) == Fsphere::rpNone ? -1.0f : dist;
}

DBG_ScriptObject* get_object(u16 id)
{
    xr_map<u16, DBG_ScriptObject*>::iterator it = Level().getScriptRenderQueue()->find(id);
    if (it == Level().getScriptRenderQueue()->end())
        return nullptr;

    return it->second;
}

void remove_object(u16 id)
{
    DBG_ScriptObject* dbg_obj = get_object(id);
    if (!dbg_obj)
        return;

    xr_delete(dbg_obj);
    Level().getScriptRenderQueue()->erase(id);
}

DBG_ScriptObject* add_object(u16 id, DebugRenderType type)
{
    remove_object(id);
    DBG_ScriptObject* dbg_obj{};

    switch (type)
    {
    case eDBGSphere: dbg_obj = xr_new<DBG_ScriptSphere>(); break;
    case eDBGBox: dbg_obj = xr_new<DBG_ScriptBox>(); break;
    case eDBGLine: dbg_obj = xr_new<DBG_ScriptLine>(); break;
    default: R_ASSERT(false, "Wrong debug object type used!");
    }

    R_ASSERT(dbg_obj);
    Level().getScriptRenderQueue()->emplace(mk_pair(id, dbg_obj));

    return dbg_obj;
}


static void shader_set_custom_param_vector(const char* key, const Fvector4 v) { shader_exports.set_custom_params(key, v); }

static Fvector4 shader_get_custom_param_vector(const char* key) { return shader_exports.get_custom_params(key); }

static void shader_set_custom_param(const char* key, float x, float y, float z, float w) { shader_exports.set_custom_params(key, Fvector4{x, y, z, w}); }

static void shader_get_custom_param(const char* key, float& x, float& y, float& z, float& w)
{
    const auto& v = shader_exports.get_custom_params(key);
    x = v.x;
    y = v.y;
    z = v.z;
    w = v.w;
}


// stalker-bot B1 ---------------------------------------------------------------------------------------------
#include "level_changer.h"
#include "location_manager.h"
#include "ai_object_location.h"
#include "game_graph.h"
#include <queue>

u32 g_bot_move_flags = 0;
u32 g_bot_move_until = 0;
extern xr_vector<CLevelChanger*> g_lchangers;

struct BotAvoid
{
    float x, z, r2;
};

static bool bot_in_avoid(const xr_vector<BotAvoid>& av, float x, float z)
{
    for (const auto& a : av)
        if (_sqr(x - a.x) + _sqr(z - a.z) < a.r2)
            return true;
    return false;
}

static bool bot_segment_clear(const xr_vector<BotAvoid>& av, const Fvector& a, const Fvector& b)
{
    const float dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
    for (const auto& c : av)
    {
        float t = len2 > EPS_L ? ((c.x - a.x) * dx + (c.z - a.z) * dz) / len2 : 0.f;
        clamp(t, 0.f, 1.f);
        if (_sqr(a.x + dx * t - c.x) + _sqr(a.z + dz * t - c.z) < c.r2)
            return false;
    }
    return true;
}

// A* over the level graph (the graph NPCs walk on) that keeps out of the avoid circles (anomalies).
static bool bot_astar(const CLevelGraph& lg, u32 from, u32 to, const xr_vector<BotAvoid>& av, xr_vector<u32>& path)
{
    const u32 n = lg.header().vertex_count();
    static xr_vector<float> g;
    static xr_vector<u32> parent, seen, closed;
    static u32 stamp = 0;
    if (g.size() != n)
    {
        g.assign(n, 0.f);
        parent.assign(n, u32(-1));
        seen.assign(n, 0);
        closed.assign(n, 0);
        stamp = 0;
    }
    if (++stamp == 0)
    {
        std::fill(seen.begin(), seen.end(), 0);
        std::fill(closed.begin(), closed.end(), 0);
        stamp = 1;
    }
    const Fvector goal = lg.vertex_position(to);
    using Node = std::pair<float, u32>;
    std::priority_queue<Node, xr_vector<Node>, std::greater<Node>> open;
    seen[from] = stamp;
    g[from] = 0.f;
    parent[from] = u32(-1);
    open.push({lg.vertex_position(from).distance_to(goal), from});
    while (!open.empty())
    {
        const u32 v = open.top().second;
        open.pop();
        if (closed[v] == stamp)
            continue;
        closed[v] = stamp;
        if (v == to)
        {
            for (u32 x = to; x != u32(-1); x = parent[x])
                path.push_back(x);
            std::reverse(path.begin(), path.end());
            return true;
        }
        const Fvector pv = lg.vertex_position(v);
        CLevelGraph::const_iterator i, e;
        lg.begin(v, i, e);
        for (; i != e; ++i)
        {
            const u32 w = lg.value(v, i);
            if (!lg.valid_vertex_id(w) || closed[w] == stamp)
                continue;
            const Fvector pw = lg.vertex_position(w);
            // stalker-bot B3: inside an avoid circle a step costs 40 times its length (around if possible, out by
            // the shortest way, in only for a goal inside)
            const float ng = g[v] + pv.distance_to(pw) * (bot_in_avoid(av, pw.x, pw.z) ? 40.f : 1.f);
            if (seen[w] != stamp || ng < g[w])
            {
                seen[w] = stamp;
                g[w] = ng;
                parent[w] = v;
                open.push({ng + pw.distance_to(goal), w});
            }
        }
    }
    return false;
}

// The route, string-pulled: from each corner the farthest vertex of the path reachable in a straight line on the
// graph that misses the avoid circles (at most 60 vertices, ~40 m, so the walker re-aims now and then).
// avoid: {x1, z1, r1, x2, z2, r2, ...}; a circle around the start or the goal is ignored (it must stay reachable).
static luabind::object bot_path(u32 from, u32 to, luabind::object const& avoid)
{
    auto t = luabind::newtable(ai().script_engine().lua());
    const CLevelGraph& lg = ai().level_graph();
    if (!lg.valid_vertex_id(from) || !lg.valid_vertex_id(to))
        return t;
    const Fvector ps = lg.vertex_position(from), pg = lg.vertex_position(to);
    xr_vector<float> flat;
    if (avoid.type() == LUA_TTABLE)
        for (auto it = avoid.begin(); it != avoid.end(); ++it)
            flat.push_back(luabind::object_cast<float>(*it));
    xr_vector<BotAvoid> av;
    for (u32 k = 0; k + 2 < flat.size(); k += 3)
    {
        av.push_back(BotAvoid{flat[k], flat[k + 1], _sqr(flat[k + 2])}); // stalker-bot B3: no circle dropped
    }
    xr_vector<u32> path;
    if (!bot_astar(lg, from, to, av, path) || path.empty())
        return t;
    int k = 1;
    t[k++] = path[0];
    u32 i = 0;
    while (i + 1 < path.size())
    {
        u32 j = i + 1;
        while (j + 1 < path.size() && j - i < 60 && lg.check_vertex_in_direction(path[i], lg.vertex_position(path[i]), path[j + 1]) &&
               bot_segment_clear(av, lg.vertex_position(path[i]), lg.vertex_position(path[j + 1])))
            ++j;
        t[k++] = path[j];
        i = j;
    }
    return t;
}

// The level changer on the game-graph route to dest_gv, as map_location.cpp picks it for the PDA arrow.
// stalker-bot B9: a dead man's known info portions to the actor, as the inventory window of his body passes them
#include "alife_registry_wrappers.h"
// stalker-bot B12: the actor's movement keys this frame (wished) and what it does (real: mcClimb on a ladder...)
static u32 bot_actor_input()
{
    return Actor() ? Actor()->bot_wished_moves() : 0;
}
static u32 bot_actor_state()
{
    return Actor() ? Actor()->MovingState() : 0;
}

static void bot_take_body_info(u16 id)
{
    if (!Actor())
        return;
    CInfoPortionWrapper* reg = xr_new<CInfoPortionWrapper>();
    reg->registry().init(id);
    KNOWN_INFO_VECTOR& known = reg->registry().objects();
    for (auto it = known.begin(); it != known.end(); ++it)
    {
        NET_Packet P;
        CGameObject::u_EventGen(P, GE_INFO_TRANSFER, Actor()->ID());
        P.w_u16(0);
        P.w_stringZ((*it).info_id);
        P.w_u8(1);
        CGameObject::u_EventSend(P);
    }
    known.clear();
    xr_delete(reg);
}

// stalker-bot B8: every level changer online on this level and the game vertex it leads to
static luabind::object bot_exits()
{
    auto t = luabind::newtable(ai().script_engine().lua());
    int k = 1;
    for (CLevelChanger* lc : g_lchangers)
    {
        auto e = luabind::newtable(ai().script_engine().lua());
        e["id"] = lc->ID();
        e["dest_gv"] = u32(lc->bot_dest_gv());
        t[k++] = e;
    }
    return t;
}

static luabind::object bot_exit_to(u32 dest_gv)
{
    auto t = luabind::newtable(ai().script_engine().lua());
    CActor* a = Actor();
    if (!a || !ai().get_game_graph() || dest_gv >= ai().game_graph().header().vertex_count())
        return t;
    xr_vector<u32> path;
    GraphEngineSpace::CGameVertexParams params(a->locations().vertex_types(), flt_max);
    if (!ai().graph_engine().search(ai().game_graph(), a->ai_location().game_vertex_id(), GameGraph::_GRAPH_ID(dest_gv), &path, params))
        return t;
    for (auto it = path.rbegin(); it != path.rend(); ++it)
        for (CLevelChanger* lc : g_lchangers)
            if (*it == lc->ai_location().game_vertex_id())
            {
                t["id"] = lc->ID();
                t["dest_gv"] = u32(lc->bot_dest_gv());
                return t;
            }
    return t;
}

// The bot heads for a level changer: for the next second walking into one changes the level without asking.
u32 g_bot_lc_until = 0;
static void bot_level_change_auto() { g_bot_lc_until = Device.dwTimeGlobal + 1000; }

static luabind::object bot_talk()
{
    lua_State* L = ai().script_engine().lua();
    auto t = luabind::newtable(L);
    CUIGameSP* g = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!g || !g->TalkMenu || !g->TalkMenu->IsShown())
        return t;
    CUITalkWnd* w = g->TalkMenu;
    t["mode"] = w->BotTopicMode() ? "topic" : "phrase";
    t["dialog"] = w->BotDialogId().c_str();
    t["pending"] = w->BotPending();
    auto opts = luabind::newtable(L);
    int k = 1;
    for (const auto& q : w->m_bot_questions)
    {
        auto o = luabind::newtable(L);
        o["id"] = q.first.c_str();
        o["text"] = q.second.c_str();
        opts[k++] = o;
    }
    t["options"] = opts;
    return t;
}

static bool bot_talk_select(LPCSTR id)
{
    CUIGameSP* g = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
    if (!g || !g->TalkMenu || !g->TalkMenu->IsShown())
        return false;
    return g->TalkMenu->BotAsk(id);
}

static void bot_move(int fwd, int strafe, bool sprint, bool jump, bool crouch)
{
    u32 f = 0;
    if (fwd > 0)
        f |= mcFwd;
    else if (fwd < 0)
        f |= mcBack;
    if (strafe > 0)
        f |= mcRStrafe;
    else if (strafe < 0)
        f |= mcLStrafe;
    if (sprint)
        f |= mcSprint;
    if (jump)
        f |= mcJump;
    if (crouch)
        f |= mcCrouch;
    g_bot_move_flags = f;
    g_bot_move_until = Device.dwTimeGlobal + 300;
}
// portal-mod E26: dual wield (player_hud.cpp)
extern bool g_hud_dual;
static void hud_dual(bool on) { g_hud_dual = on; }
static bool hud_dual_on() { return g_hud_dual; }
// portal-mod E27: alternate fire (player_hud.cpp)
extern bool g_hud_dual_alt, g_hud_dual_swap;
static void hud_dual_alt(bool on) { g_hud_dual_alt = on; }
static bool hud_dual_swapped() { return g_hud_dual_swap; }
// stalker-bot B2: all the actor's tasks (scripts only see the active one)
#include "GameTaskManager.h"
#include "GameTask.h"
static luabind::object bot_tasks()
{
    lua_State* L = ai().script_engine().lua();
    auto t = luabind::newtable(L);
    CActor* a = Actor();
    if (!a)
        return t;
    int k = 1;
    for (SGameTaskKey& key : a->GameTaskManager().GameTasks())
    {
        CGameTask* gt = key.game_task;
        if (!gt || gt->m_Objectives.empty())
            continue;
        auto o = luabind::newtable(L);
        o["id"] = gt->m_ID.c_str();
        o["title"] = gt->m_Title.c_str();
        o["prio"] = gt->m_priority;
        o["state"] = int(gt->Objective(0).TaskState());
        o["received"] = double(gt->m_ReceiveTime);
        auto objs = luabind::newtable(L);
        int j = 1;
        for (u32 i = 1; i < gt->m_Objectives.size(); ++i)
        {
            SGameTaskObjective& ob = gt->m_Objectives[i];
            auto x = luabind::newtable(L);
            x["idx"] = i;
            x["desc"] = ob.description.c_str();
            x["state"] = int(ob.TaskState());
            x["target"] = ob.object_id;
            objs[j++] = x;
        }
        o["objectives"] = objs;
        t[k++] = o;
    }
    return t;
}

// stalker-bot B4: trading inside an open talk, as the trade window does it
#include "Inventory.h"
#include "Trade.h"
#include "trade_parameters.h"

// the partner's side of the deal (its CTrade: its prices, its money), when we are talking to it
static CTrade* bot_their_trade(u16 partner_id)
{
    CActor* a = Actor();
    if (!a || !a->IsTalking() || !a->GetTrade() || !a->GetTrade()->GetPartner())
        return nullptr;
    CGameObject* p = smart_cast<CGameObject*>(a->GetTrade()->GetPartner());
    if (!p || p->ID() != partner_id)
        return nullptr;
    return a->GetTrade()->GetPartnerTrade();
}

static PIItem bot_item(u16 id)
{
    CObject* o = Level().Objects.net_Find(id);
    return o ? smart_cast<PIItem>(o) : nullptr;
}

static u32 bot_trade_price(u16 partner_id, u16 item_id, bool actor_buys)
{
    CTrade* t = bot_their_trade(partner_id);
    PIItem it = bot_item(item_id);
    if (!t || !it || it->m_flags.test(CInventoryItem::FIAlwaysUntradable) || !it->CanTrade())
        return 0;
    CInventoryOwner* trader = t->pThis.inv_owner;
    CInventoryOwner* me = t->pPartner.inv_owner;
    CGameObject* owner = smart_cast<CGameObject*>(actor_buys ? trader : me);
    if (!owner || it->object().H_Parent() != owner)
        return 0;
    if (!actor_buys)
    {
        const shared_str& sect = it->object().cNameSect();
        if (!trader->trade_parameters().enabled(CTradeParameters::action_buy(0), sect))
            return 0;
        if (it->GetCondition() < trader->trade_parameters().factors(CTradeParameters::action_buy(0), sect).min_condition())
            return 0;
        if (trader->inventory().CalcTotalWeight() + it->Weight() > trader->inventory().GetMaxWeight())
            return 0;
    }
    return t->GetItemPrice(it, !actor_buys); // the trader's side: it buys what we sell
}

static bool bot_trade(u16 partner_id, u16 item_id, bool actor_buys)
{
    CTrade* t = bot_their_trade(partner_id);
    PIItem it = bot_item(item_id);
    const u32 price = bot_trade_price(partner_id, item_id, actor_buys);
    if (!t || !it || price == 0)
        return false;
    CInventoryOwner* trader = t->pThis.inv_owner;
    CInventoryOwner* me = t->pPartner.inv_owner;
    if (actor_buys ? me->get_money() < price : (!trader->InfinitiveMoney() && trader->get_money() < price))
        return false;
    t->OnPerformTrade(actor_buys ? price : 0, actor_buys ? 0 : price);
    t->TransferItem(it, !actor_buys);
    return true;
}

// stalker-bot B5: a stash, emptied the way its window does it
void move_item(u16 from_id, u16 to_id, u16 what_id); // ui/UICarBodyWnd.cpp
static bool bot_take_from_box(u16 box_id, u16 item_id)
{
    CActor* a = Actor();
    CObject* bo = Level().Objects.net_Find(box_id);
    CObject* io = Level().Objects.net_Find(item_id);
    if (!a || !bo || !io || io->H_Parent() != bo)
        return false;
    IInventoryBox* box = smart_cast<IInventoryBox*>(bo);
    if (!box || !box->IsOpened() || a->Position().distance_to(bo->Position()) > 3.f)
        return false;
    move_item(box_id, a->ID(), item_id);
    return true;
}

// stalker-bot B1 end --------------------------------------------------------------------------------------------

void CLevel::script_register(lua_State* L)
{
    module(L)[class_<CEnvDescriptor>("CEnvDescriptor")
                  .def_readwrite("fog_density", &CEnvDescriptor::fog_density)
                  .def_readwrite("fog_distance", &CEnvDescriptor::fog_distance)
                  .def_readwrite("far_plane", &CEnvDescriptor::far_plane)
                  .def_readwrite("sun_dir", &CEnvDescriptor::sun_dir)
                  .def_readwrite("wind_velocity", &CEnvDescriptor::wind_velocity)
                  .def_readwrite("wind_direction", &CEnvDescriptor::wind_direction)
                  .def_readwrite("m_fTreeAmplitudeIntensity", &CEnvDescriptor::m_fTreeAmplitudeIntensity)
                  .def_readwrite("m_fSunShaftsIntensity", &CEnvDescriptor::m_fSunShaftsIntensity)
                  .property("m_identifier", [](CEnvDescriptor* self) { return self->m_identifier.c_str(); })
                  .def("set_env_ambient", &CEnvDescriptor::setEnvAmbient),
              class_<CEnvironment>("CEnvironment")
                  .def("getCurrentWeather", [](CEnvironment* self, const size_t idx) { R_ASSERT(idx < 2); return self->Current[idx]; }),

              class_<CPHCall>("CPHCall").def("set_pause", &CPHCall::setPause),

              class_<CEffectorBobbing>("CEffectorBobbing")
                  .def_readwrite("run_amplitude", &CEffectorBobbing::m_fAmplitudeRun)
                  .def_readwrite("walk_amplitude", &CEffectorBobbing::m_fAmplitudeWalk)
                  .def_readwrite("limp_amplitude", &CEffectorBobbing::m_fAmplitudeLimp)
                  .def_readwrite("run_speed", &CEffectorBobbing::m_fSpeedRun)
                  .def_readwrite("walk_speed", &CEffectorBobbing::m_fSpeedWalk)
                  .def_readwrite("limp_speed", &CEffectorBobbing::m_fSpeedLimp),

        class_<DBG_ScriptObject>("DBG_ScriptObject")
                  .enum_("dbg_type")[
                      value("line", (int)DebugRenderType::eDBGLine), 
                      value("sphere", (int)DebugRenderType::eDBGSphere), 
                      value("box", (int)DebugRenderType::eDBGBox)
                  ]
                  .def("cast_dbg_sphere", &DBG_ScriptObject::cast_dbg_sphere)
                  .def("cast_dbg_box", &DBG_ScriptObject::cast_dbg_box)
                  .def("cast_dbg_line", &DBG_ScriptObject::cast_dbg_line)
                  .def_readwrite("color", &DBG_ScriptObject::m_color)
                  .def_readwrite("hud", &DBG_ScriptObject::m_hud)
                  .def_readwrite("visible", &DBG_ScriptObject::m_visible),

              class_<DBG_ScriptSphere, DBG_ScriptObject>("DBG_ScriptSphere")
                .def_readwrite("matrix", &DBG_ScriptSphere::m_mat),
              class_<DBG_ScriptBox, DBG_ScriptObject>("DBG_ScriptBox")
                .def_readwrite("matrix", &DBG_ScriptBox::m_mat)
                .def_readwrite("size", &DBG_ScriptBox::m_size),
              class_<DBG_ScriptLine, DBG_ScriptObject>("DBG_ScriptLine")
                .def_readwrite("point_a", &DBG_ScriptLine::m_point_a)
                .def_readwrite("point_b", &DBG_ScriptLine::m_point_b)];

        module(L, "debug_render")[
            def("add_object", add_object), 
            def("remove_object", remove_object), 
            def("get_object", get_object)
        ];

        module(L, "level")[
            // obsolete\deprecated
            def("object_by_id", &get_object_by_id), def("is_removing_objects", &is_removing_objects_script),
#ifdef DEBUG
            def("debug_object", &get_object_by_name), def("debug_actor", &tpfGetActor), def("check_object", &check_object),
#endif

            def("get_weather", &get_weather), def("get_weather_prev", &get_weather_prev), def("get_weather_last_shift", &get_weather_last_shift), def("set_weather", &set_weather),
            def("set_weather_next", &set_weather_next), def("set_weather_fx", &set_weather_fx), def("start_weather_fx_from_time", &start_weather_fx_from_time),
            def("is_wfx_playing", &is_wfx_playing), def("get_wfx_time", &get_wfx_time), def("stop_weather_fx", &stop_weather_fx), def("environment", &environment),

            def("set_time_factor", &set_time_factor), def("get_time_factor", &get_time_factor),

            def("set_game_difficulty", &set_game_difficulty), def("get_game_difficulty", &get_game_difficulty),

            def("get_time_days", &get_time_days), def("get_time_hours", &get_time_hours), def("get_time_minutes", &get_time_minutes),

            def("high_cover_in_direction", &high_cover_in_direction), def("low_cover_in_direction", &low_cover_in_direction), def("cover_in_direction", &cover_in_direction),
            def("vertex_in_direction", &vertex_in_direction), def("rain_factor", &rain_factor), def("rain_hemi", rain_hemi),
            def("rain_wetness", [] { return g_pGamePersistent->Environment().wetness_factor; }),
            def("set_rain_wetness", [](float val) {
                clamp(val, 0.f, 1.f);
                g_pGamePersistent->Environment().wetness_factor = val;
            }),
            def("patrol_path_exists", &patrol_path_exists), def("vertex_position", &vertex_position), def("name", &get_name), def("prefetch_sound", &prefetch_sound),

            def("prefetch_sound", prefetch_sound),
            def("prefetch_many_sounds", prefetch_many_sounds ),

            def("client_spawn_manager", &get_client_spawn_manager),

            def("map_add_object_spot_ser", &map_add_object_spot_ser), def("map_add_object_spot", &map_add_object_spot), def("map_remove_object_spot", &map_remove_object_spot),
            def("map_has_object_spot", &map_has_object_spot), def("map_change_spot_hint", &map_change_spot_hint), def("map_change_spot_ser", &map_change_spot_ser),
            def("map_add_user_spot", &map_add_user_spot),

            def("start_stop_menu", &start_stop_menu), def("add_dialog_to_render", &add_dialog_to_render), def("remove_dialog_to_render", &remove_dialog_to_render),
            def("main_input_receiver", &main_input_receiver), def("hide_indicators", &hide_indicators), def("show_indicators", &show_indicators),
            def("game_indicators_shown", &game_indicators_shown), def("get_hud_flags", &get_hud_flags),
            def("add_call", ((CPHCall * (*)(const luabind::functor<bool>&, const luabind::functor<void>&)) & add_call)),
            def("add_call", ((CPHCall * (*)(const luabind::object&, const luabind::functor<bool>&, const luabind::functor<void>&)) & add_call)),
            def("add_call", ((CPHCall * (*)(const luabind::object&, LPCSTR, LPCSTR)) & add_call)),
            def("remove_call", ((void (*)(const luabind::functor<bool>&, const luabind::functor<void>&)) & remove_call)),
            def("remove_call", ((void (*)(const luabind::object&, const luabind::functor<bool>&, const luabind::functor<void>&)) & remove_call)),
            def("remove_call", ((void (*)(const luabind::object&, LPCSTR, LPCSTR)) & remove_call)), def("remove_calls_for_object", remove_calls_for_object),

            def("present", is_level_present),

            def("disable_input", disable_input), def("enable_input", enable_input), 

            def("only_allow_movekeys", block_all_except_movement), def("only_movekeys_allowed", only_movement_allowed),

            def("set_actor_allow_ladder", set_actor_allow_ladder), def("actor_ladder_allowed", actor_ladder_allowed),
            def("set_actor_allow_pda", set_actor_allow_pda), def("actor_pda_allowed", actor_pda_allowed),

            def("spawn_phantom", spawn_phantom),

            def("get_bounding_volume", &get_bounding_volume),

            def("iterate_sounds", &iterate_sounds1), def("iterate_sounds", &iterate_sounds2), def("physics_world", &physics_world), def("get_snd_volume", &get_snd_volume),
            def("set_snd_volume", &set_snd_volume), def("add_cam_effector", &add_cam_effector), def("add_cam_effector2", &add_cam_effector2),
            def("remove_cam_effector", &remove_cam_effector), def("add_pp_effector", &add_pp_effector), def("set_pp_effector_factor", &set_pp_effector_factor),
            def("set_pp_effector_factor", &set_pp_effector_factor2), def("remove_pp_effector", &remove_pp_effector),

            def("demo_record_start", &demo_record_start), def("demo_record_stop", &demo_record_stop),
            def("demo_record_get_position", &demo_record_get_position), def("demo_record_set_position", &demo_record_set_position),
            def("demo_record_get_HPB", &demo_record_get_HPB), def("demo_record_set_HPB", &demo_record_set_HPB),
            def("demo_record_set_direct_input", &demo_record_set_direct_input),

            def("add_complex_effector", &add_complex_effector), def("remove_complex_effector", &remove_complex_effector),

            def("game_id", &GameID), def("set_ignore_game_state_update", &set_ignore_game_state_update),

            def("get_inventory_wnd", &GetInventoryWindow), def("get_talk_wnd", &GetTalkWindow), def("get_trade_wnd", &GetTradeWindow), def("get_pda_wnd", &GetPdaWindow),
            def("get_car_body_wnd", &GetCarBodyWindow), def("get_second_talker", &GetSecondTalker), def("get_car_body_target", &GetCarBodyTarget),
            def("get_change_level_wnd", &GetUIChangeLevelWnd),

            def("ray_query", &PerformRayQuery),

            // Real Wolf 07.07.2014
            def("vertex_id", ((u32(*)(const Fvector&)) & vertex_id)), def("vertex_id", ((u32(*)(u32, const Fvector&)) & vertex_id)), def("nearest_vertex_id", &nearest_vertex_id),

            def("advance_game_time", &AdvanceGameTime),

            def("portal_set", &portal_set), def("portal_clear", &portal_clear), def("portal_view_roll", &portal_view_roll), def("ray_query_normal", &ray_query_normal), // portal-mod
            def("actor_velocity", &actor_velocity), def("portal_set_actor_position", &portal_set_actor_position), def("portal_fly_actor", &portal_fly_actor), def("portal_move_npc", &portal_move_npc), // portal-mod E24
            def("hud_dual", &hud_dual), def("hud_dual_on", &hud_dual_on), // portal-mod E26
            def("hud_dual_alt", &hud_dual_alt), def("hud_dual_swapped", &hud_dual_swapped), // portal-mod E27
            def("cine_cam", &cine_cam), def("cine_cam_off", &cine_cam_off), def("cine_cam_on", &cine_cam_on), // portal-mod E21
            def("actor_arm_aim", &actor_arm_aim), // portal-mod E34
            def("actor_tp_dual", &actor_tp_dual), // portal-mod E36
            def("portal_pose_object", &portal_pose_object), // portal-mod E41
            def("actor_tp_grip", &actor_tp_grip), def("actor_tp_grip_hands", &actor_tp_grip_hands), // portal-mod E43
            def("bot_path", &bot_path), def("bot_exit_to", &bot_exit_to), def("bot_exits", &bot_exits), /* stalker-bot B8 */ def("bot_take_body_info", &bot_take_body_info), /* stalker-bot B9 */ def("bot_actor_input", &bot_actor_input), def("bot_actor_state", &bot_actor_state), /* stalker-bot B12 */ def("bot_level_change_auto", &bot_level_change_auto), // stalker-bot B1
            def("bot_talk", &bot_talk), def("bot_talk_select", &bot_talk_select), def("bot_move", &bot_move), def("bot_tasks", &bot_tasks), // stalker-bot B2
            def("bot_trade_price", &bot_trade_price), def("bot_trade", &bot_trade), // stalker-bot B4
            def("bot_take_from_box", &bot_take_from_box), // stalker-bot B5
            def("set_actor_velocity", &set_actor_velocity),

            def("get_target_dist", &GetTargetDist), def("get_target_obj", &GetTargetObj), def("get_current_ray_query", &GetCurrentRayQuery),
            //
            def("send_event_key_press", &send_event_key_press), def("send_event_key_release", &send_event_key_release), def("send_event_key_hold", &send_event_key_hold),
            def("send_event_mouse_wheel", &send_event_mouse_wheel),

            def("iterate_nearest", &iterate_nearest),

            def("change_level", &change_level), def("set_cam_inert", &set_cam_inert), def("set_monster_relation", &set_monster_relation), def("patrol_path_add", &patrol_path_add),
            def("patrol_path_remove", &patrol_path_remove), def("valid_vertex_id", &valid_vertex_id), def("vertex_count", &vertex_count), def("disable_vertex", &disable_vertex),
            def("enable_vertex", &enable_vertex), def("is_accessible_vertex_id", &is_accessible_vertex_id), def("iterate_vertices_inside", &iterate_vertices_inside),
            def("iterate_vertices_border", &iterate_vertices_border), def("get_character_community_team", &get_character_community_team),

            def("get_effector_bobbing", &get_effector_bobbing), def("is_ray_intersect_sphere", &is_ray_intersect_sphere),

            def("block_action", [](EGameActions action) { Level().block_action(action); }),
            def("unblock_action", [](EGameActions action) { Level().unblock_action(action); })
    ],

        module(L, "actor_stats")[def("add_points", &add_actor_points), def("add_points_str", &add_actor_points_str), def("get_points", &get_actor_points),
                                 def("remove_points", &remove_actor_points), def("add_to_ranking", &add_human_to_top_list), def("remove_from_ranking", &remove_human_from_top_list),
                                 def("get_actor_ranking", &get_actor_ranking)];

    module(L)[def("command_line", &command_line)];

    module(L, "relation_registry")[def("community_goodwill", &g_community_goodwill), def("set_community_goodwill", &g_set_community_goodwill),
                                   def("change_community_goodwill", &g_change_community_goodwill), def("get_personal_goodwill", &g_get_personal_goodwill),
                                   def("set_personal_goodwill", &g_set_personal_goodwill), def("change_personal_goodwill", &g_change_personal_goodwill),
                                   def("clear_personal_goodwill", &g_clear_personal_goodwill), def("clear_personal_relations", &g_clear_personal_relations)];
    //установка параметров для шейдеров из скриптов
    module(L)[def("set_artefact_slot", &g_set_artefact_position), def("set_anomaly_slot", &g_set_anomaly_position), def("set_detector_mode", &g_set_detector_params),
              def("set_pda_params", [](const Fvector& p) { shader_exports.set_pda_params(p); }), def("update_inventory_window", &update_inventory_window),

           def("set_dof_params", [](const float& p1, const float& p2, const float& p3, const float& p4) { shader_exports.set_dof_params(p1, p2, p3, p4); }),

              def("update_inventory_weight", &update_inventory_weight),

              class_<enum_exporter<collide::rq_target>>("rq_target")
                  .enum_("rq_target")[value("rqtNone", int(collide::rqtNone)), value("rqtObject", int(collide::rqtObject)), value("rqtStatic", int(collide::rqtStatic)),
                                      value("rqtShape", int(collide::rqtShape)), value("rqtObstacle", int(collide::rqtObstacle)), value("rqtBoth", int(collide::rqtBoth)),
                                      value("rqtDyn", int(collide::rqtDyn))],

           def("shader_set_custom_param_vector", &shader_set_custom_param_vector),
           def("shader_get_custom_param_vector", &shader_get_custom_param_vector),
           def("shader_set_custom_param", &shader_set_custom_param),
           def("shader_get_custom_param", &shader_get_custom_param, pure_out_value<2>() + pure_out_value<3>() + pure_out_value<4>() + pure_out_value<5>())
   ];
}
