#include "stdafx.h"
#include "actor.h"
#include "inventory.h"
#include "xrserver_objects_alife_monsters.h"
#include "xrServer.h"
#include "CameraFirstEye.h"

#include "ActorEffector.h"

#include "PHWorld.h"
#include "actorcameracollision.h"
#include "level.h"
#include "alife_registry_wrappers.h"
#include "..\Include/xrRender/Kinematics.h"
#include "..\Include/xrRender/KinematicsAnimated.h"
#include "client_spawn_manager.h"
#include "CharacterPhysicsSupport.h"
#include "Grenade.h"
#include "WeaponMagazined.h"
#include "CustomOutfit.h"

#include "actor_anim_defs.h"

#include "map_manager.h"
#include "HUDManager.h"
#include "ui/UIArtefactPanel.h"
#include "GameTaskManager.h"
#include "holder_custom.h"
#include "actor_memory.h"
#include "actor_statistic_mgr.h"
#include "alife_simulator_header.h"
#include "actorcondition.h"
#include "Car.h"
#include "player_hud.h"
#include "UIGameSP.h"
#include "ui/UIPDAWnd.h"
#include "ui/UIEncyclopediaWnd.h"
#include "ui/UIDiaryWnd.h"

#include "debug_renderer.h"

CActor* g_actor = NULL;

CActor* Actor()
{
    // KRodin: Эта функция теперь вызывается из многих новых каллбеков,
    // и вполне может быть вызвана, когда актора ещё нет.
    // Т.ч. вылетать не будем в этом случае.
    // VERIFY		(g_actor);
    return g_actor;
};

BOOL CActor::net_Spawn(CSE_Abstract* DC)
{
    m_holder_id = ALife::_OBJECT_ID(-1);
    m_feel_touch_characters = 0;
    m_snd_noise = 0.0f;
    m_sndShockEffector = NULL;
    /*	m_followers			= NULL;*/
    if (m_pPhysicsShell)
    {
        m_pPhysicsShell->Deactivate();
        xr_delete(m_pPhysicsShell);
    };
    // force actor to be local on server client
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeCreatureActor* E = smart_cast<CSE_ALifeCreatureActor*>(e);

    {
        E->s_flags.set(M_SPAWN_OBJECT_LOCAL, TRUE);
    }

    if (TRUE == E->s_flags.test(M_SPAWN_OBJECT_LOCAL) && TRUE == E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER))
        g_actor = this;

    VERIFY(m_pActorEffector == NULL);
    m_pActorEffector = xr_new<CActorCameraManager>();

    // motions
    m_bAnimTorsoPlayed = false;
    m_current_legs_blend = 0;
    m_current_jump_blend = 0;
    m_current_legs.invalidate();
    m_current_torso.invalidate();
    m_current_head.invalidate();
    //-------------------------------------
    // инициализация реестров, используемых актером
    encyclopedia_registry->registry().init(ID());
    game_news_registry->registry().init(ID());

    {
        auto& news = game_news_registry->registry().objects();
        if (news.size() > NewsToShow())
        {
            size_t s = news.size();
            news.erase(news.begin(), news.begin() + (news.size() - NewsToShow()));
            Msg("[%s]: purge %u news items, %u left", __FUNCTION__, s - news.size(), news.size());
        }
    }

    if (HUD().GetUI())
    {
        CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(HUD().GetUI()->UIGame());
        if (pGameSP)
            pGameSP->PdaMenu->UIEncyclopediaWnd->FillEncyclopedia();
        pGameSP->PdaMenu->UIDiaryWnd->FillNews();
    }

    if (!CInventoryOwner::net_Spawn(DC))
        return FALSE;
    if (!inherited::net_Spawn(DC))
        return FALSE;

    CSE_ALifeTraderAbstract* pTA = smart_cast<CSE_ALifeTraderAbstract*>(e);
    set_money(pTA->m_dwMoney, false);

    m_pPhysics_support->in_NetSpawn(e);

    character_physics_support()->movement()->ActivateBox(m_loaded_ph_box_id);
    if (E->m_holderID != u16(-1))
    {
        character_physics_support()->movement()->DestroyCharacter();
    }
    if (m_bOutBorder)
        character_physics_support()->movement()->setOutBorder();
    r_torso_tgt_roll = 0;

    r_model_yaw = E->o_torso.yaw;
    r_torso.yaw = E->o_torso.yaw;
    r_torso.pitch = E->o_torso.pitch;
    r_torso.roll = 0.0f; // E->o_Angle.z;

    unaffected_r_torso.yaw = r_torso.yaw;
    unaffected_r_torso.pitch = r_torso.pitch;
    unaffected_r_torso.roll = r_torso.roll;

    // KRodin: это, мне кажется, лишнее.
    // if( psActorFlags.test(AF_PSP) )
    //	cam_Set(eacLookAt);
    // else
    cam_Set(eacFirstEye);

    cam_Active()->Set(-E->o_torso.yaw, E->o_torso.pitch, 0); // E->o_Angle.z);

    // *** movement state - respawn
    mstate_wishful = 0;
    if (m_loaded_ph_box_id == 1 || m_loaded_ph_box_id == 3)
        mstate_real = mcCrouch;
    else if (m_loaded_ph_box_id == 2 || m_loaded_ph_box_id == 4)
        mstate_real = mcCrouch | mcAccel;
    else
        mstate_real = 0;
    mstate_old = mstate_real;
    m_bJumpKeyPressed = FALSE;

    NET_SavedAccel.set(0, 0, 0);

    setEnabled(E->s_flags.is(M_SPAWN_OBJECT_LOCAL));

    Engine.Sheduler.Register(this, TRUE);

    hit_slowmo = 0.f;

    OnChangeVisual();

    processing_activate();

#ifdef DEBUG
    LastPosS.clear();
    LastPosH.clear();
    LastPosL.clear();
#endif

    SetDefaultVisualOutfit(cNameVisual());

    smart_cast<IKinematics*>(Visual())->CalculateBones();

    //--------------------------------------------------------------
    inventory().SetPrevActiveSlot(NO_ACTIVE_SLOT);

    //-------------------------------------
    if (!g_Alive())
    {
        mstate_wishful &= ~mcAnyMove;
        mstate_real &= ~mcAnyMove;
        IKinematicsAnimated* K = smart_cast<IKinematicsAnimated*>(Visual());
        K->PlayCycle("death_init");

        //остановить звук тяжелого дыхания
        m_HeavyBreathSnd.stop();
    }

    auto callback = fastdelegate::MakeDelegate(this, &CActor::on_requested_spawn);
    m_holder_id = E->m_holderID;
    if (E->m_holderID != ALife::_OBJECT_ID(-1))
        Level().client_spawn_manager().add(E->m_holderID, ID(), callback);
    // F
    //-------------------------------------------------------------
    m_iLastHitterID = u16(-1);
    m_iLastHittingWeaponID = u16(-1);
    m_s16LastHittedElement = -1;
    m_bWasHitted = false;

    Level().MapManager().AddMapLocation("actor_location", ID());
    Level().MapManager().AddMapLocation("actor_location_p", ID());

    m_game_task_manager = xr_new<CGameTaskManager>();
    GameTaskManager().initialize(ID());

    m_statistic_manager = xr_new<CActorStatisticMgr>();

    spatial.type |= STYPE_REACTTOSOUND;

    g_player_hud->load_default();

    Level().debug_renderer().ClearDebugContainer();

    return TRUE;
}

void CActor::net_Destroy()
{
    inherited::net_Destroy();

    if (m_holder_id != ALife::_OBJECT_ID(-1))
        Level().client_spawn_manager().remove(m_holder_id, ID());

    delete_data(m_game_task_manager);
    delete_data(m_statistic_manager);

    Level().MapManager().RemoveMapLocationByObjectID(ID());

#pragma todo("Dima to MadMax : do not comment inventory owner net_Destroy!!!")
    CInventoryOwner::net_Destroy();
    cam_UnsetLadder();
    character_physics_support()->movement()->DestroyCharacter();
    if (m_pPhysicsShell)
    {
        m_pPhysicsShell->Deactivate();
        xr_delete(m_pPhysicsShell);
    }
    m_pPhysics_support->in_NetDestroy();

    xr_delete(m_sndShockEffector);
    xr_delete(m_pActorEffector);
    pCamBobbing = NULL;

#ifdef DEBUG
    LastPosS.clear();
    LastPosH.clear();
    LastPosL.clear();
#endif

    processing_deactivate();
    m_holder = NULL;
    m_holderID = u16(-1);

    SetDefaultVisualOutfit(NULL);

    if (g_actor == this)
        g_actor = NULL;

    Engine.Sheduler.Unregister(this);

    if (actor_camera_shell && actor_camera_shell->get_ElementByStoreOrder(0)->PhysicsRefObject() == this)
        destroy_physics_shell(actor_camera_shell);
}

void CActor::net_Relcase(CObject* O)
{
    VERIFY(O);

    CGameObject* GO = smart_cast<CGameObject*>(O);
    if (GO && m_pObjectWeLookingAt == GO)
    {
        m_pObjectWeLookingAt = NULL;
    }
    CHolderCustom* HC = smart_cast<CHolderCustom*>(GO);
    if (HC && HC == m_pVehicleWeLookingAt)
    {
        m_pVehicleWeLookingAt = NULL;
    }
    if (HC && HC == m_holder)
    {
        m_holder->detach_Actor();
        m_holder = NULL;
    }

    if (m_pending_car == O)
    {
        m_pending_car = {};
        m_pending_car_frames = 0;
    }

    inherited::net_Relcase(O);

    memory().remove_links(O);
    m_pPhysics_support->in_NetRelcase(O);
    conditions().net_Relcase(O);
}

BOOL CActor::net_Relevant() // relevant for export to server
{
    return getSVU() | getLocal();
}

void CActor::SetCallbacks()
{
    IKinematics* V = smart_cast<IKinematics*>(Visual());
    VERIFY(V);
    u16 spine0_bone = V->LL_BoneID("bip01_spine");
    u16 spine1_bone = V->LL_BoneID("bip01_spine1");
    u16 shoulder_bone = V->LL_BoneID("bip01_spine2");
    u16 head_bone = V->LL_BoneID("bip01_head");
    V->LL_GetBoneInstance(u16(spine0_bone)).set_callback(bctCustom, Spin0Callback, this);
    V->LL_GetBoneInstance(u16(spine1_bone)).set_callback(bctCustom, Spin1Callback, this);
    V->LL_GetBoneInstance(u16(shoulder_bone)).set_callback(bctCustom, ShoulderCallback, this);
    V->LL_GetBoneInstance(u16(head_bone)).set_callback(bctCustom, HeadCallback, this);
    // portal-mod E34: the right arm, aimed by the script (off while m_arm_aim_k is 0)
    const u16 ua = V->LL_BoneID("bip01_r_upperarm"), fa = V->LL_BoneID("bip01_r_forearm"), hb = V->LL_BoneID("bip01_r_hand");
    if (ua != BI_NONE && fa != BI_NONE && hb != BI_NONE)
    {
        m_arm_axis[0].set(V->LL_GetData(fa).bind_transform.c);
        m_arm_axis[1].set(V->LL_GetData(hb).bind_transform.c);
        m_arm_axis[0].normalize_safe();
        m_arm_axis[1].normalize_safe();
        V->LL_GetBoneInstance(ua).set_callback(bctCustom, ArmAimUpperCallback, this);
        V->LL_GetBoneInstance(fa).set_callback(bctCustom, ArmAimForeCallback, this);
    }
    // portal-mod E38: the chest's forward in each spine bone's own frame - the bind pose's, square to the shoulders
    // (right upper arm - left upper arm, crossed with up), turned into the bone's frame by its model-to-bone matrix
    {
        const u16 lu = V->LL_BoneID("bip01_l_upperarm"), ru = V->LL_BoneID("bip01_r_upperarm");
        const u16 sb[4] = {spine0_bone, spine1_bone, shoulder_bone, head_bone};
        if (lu != BI_NONE && ru != BI_NONE)
        {
            Fmatrix bl, br;
            bl.invert(V->LL_GetData(lu).m2b_transform);
            br.invert(V->LL_GetData(ru).m2b_transform);
            Fvector side, up_, f;
            side.sub(br.c, bl.c);
            up_.set(0.f, 1.f, 0.f);
            f.crossproduct(side, up_);
            f.y = 0.f;
            f.normalize_safe();
            // which way is the front: where the feet point, foot -> toe (take 23: +z taken for it turned the chest and
            // the head round to the back - the skeleton's bind pose faces -z)
            const u16 ft = V->LL_BoneID("bip01_l_foot"), te = V->LL_BoneID("bip01_l_toe0");
            Fvector toe_dir;
            toe_dir.set(0.f, 0.f, -1.f);
            if (ft != BI_NONE && te != BI_NONE)
            {
                Fmatrix bf, bt;
                bf.invert(V->LL_GetData(ft).m2b_transform);
                bt.invert(V->LL_GetData(te).m2b_transform);
                toe_dir.sub(bt.c, bf.c);
                Msg("portal-mod E38: foot -> toe (bind) [%.2f %.2f %.2f]", toe_dir.x, toe_dir.y, toe_dir.z);
            }
            if (f.x * toe_dir.x + f.z * toe_dir.z < 0.f)
                f.invert();
            for (int i = 0; i < 4; ++i)
                if (sb[i] != BI_NONE)
                    V->LL_GetData(sb[i]).m2b_transform.transform_dir(m_spine_fwd[i], f);
            Msg("portal-mod E38: chest forward (bind) [%.2f %.2f %.2f]", f.x, f.y, f.z);
        }
    }
    // portal-mod E36: the left arm for the two-gun pose; both hands for the second gun's place
    const u16 lua = V->LL_BoneID("bip01_l_upperarm"), lfa = V->LL_BoneID("bip01_l_forearm"), lhb = V->LL_BoneID("bip01_l_hand");
    m_tp_dual_hand[0] = hb;
    m_tp_dual_hand[1] = lhb;
    if (lua != BI_NONE && lfa != BI_NONE && lhb != BI_NONE)
    {
        m_larm_axis[0].set(V->LL_GetData(lfa).bind_transform.c);
        m_larm_axis[1].set(V->LL_GetData(lhb).bind_transform.c);
        m_larm_axis[0].normalize_safe();
        m_larm_axis[1].normalize_safe();
        V->LL_GetBoneInstance(lua).set_callback(bctCustom, LArmAimUpperCallback, this);
        V->LL_GetBoneInstance(lfa).set_callback(bctCustom, LArmAimForeCallback, this);
    }
    // portal-mod E39: the left arm's bones with a right twin mirror it while the two-gun pose is on (over E36's aim)
    m_tp_mirror_n = 0;
    m_tp_chest = V->LL_BoneID("bip01_spine2");
    if (m_tp_chest != BI_NONE)
    {
        V->LL_GetData(m_tp_chest).m2b_transform.transform_dir(m_tp_chest_lat, Fvector().set(1.f, 0.f, 0.f));
        m_tp_chest_lat.normalize_safe();
        Fmatrix H0;
        H0.identity();
        H0.i.x = -1.f;
        float worst = 1.f;
        for (u16 id = 0; id < V->LL_BoneCount() && m_tp_mirror_n < 32; ++id)
        {
            const char* nm = V->LL_BoneName(id);
            if (!nm || strncmp(nm, "bip01_l_", 8) != 0)
                continue;
            if (!strstr(nm, "clavicle") && !strstr(nm, "upperarm") && !strstr(nm, "forearm") && !strstr(nm, "hand") && !strstr(nm, "finger"))
                continue;
            string128 rn;
            xr_sprintf(rn, "bip01_r_%s", nm + 8);
            const u16 r = V->LL_BoneID(rn);
            if (r == BI_NONE)
                continue;
            Fmatrix bl, q0, q;
            bl.invert(V->LL_GetData(id).m2b_transform);
            q0.mul_43(V->LL_GetData(r).m2b_transform, H0);
            q.mul_43(q0, bl);
            for (Fvector* c : {&q.i, &q.j, &q.k}) // snapped to the nearest signed axis
            {
                const float ax = _abs(c->x), ay = _abs(c->y), az = _abs(c->z);
                worst = _min(worst, _max(ax, _max(ay, az)));
                if (ax >= ay && ax >= az)
                    c->set(c->x > 0.f ? 1.f : -1.f, 0.f, 0.f);
                else if (ay >= az)
                    c->set(0.f, c->y > 0.f ? 1.f : -1.f, 0.f);
                else
                    c->set(0.f, 0.f, c->z > 0.f ? 1.f : -1.f);
            }
            q.c.set(0.f, 0.f, 0.f);
            SPortalMirror& m = m_tp_mirror[m_tp_mirror_n++];
            m.actor = this;
            m.l = id;
            m.r = r;
            m.q = q;
            V->LL_GetBoneInstance(id).set_callback(bctCustom, TpMirrorCallback, &m);
            Msg("portal-mod E39: %s <- %s q [%+.0f %+.0f %+.0f] [%+.0f %+.0f %+.0f] [%+.0f %+.0f %+.0f]", nm, rn, q.i.x, q.i.y, q.i.z,
                q.j.x, q.j.y, q.j.z, q.k.x, q.k.y, q.k.z);
        }
        Fmatrix hl, hr;
        if (lhb != BI_NONE && hb != BI_NONE)
        {
            hl.invert(V->LL_GetData(lhb).m2b_transform);
            hr.invert(V->LL_GetData(hb).m2b_transform);
            Msg("portal-mod E39: %u mirrored bones, worst axis fit %.3f; bind hands L [%.3f %.3f %.3f] R [%.3f %.3f %.3f], chest axis [%.2f %.2f %.2f]",
                m_tp_mirror_n, worst, hl.c.x, hl.c.y, hl.c.z, hr.c.x, hr.c.y, hr.c.z, m_tp_chest_lat.x, m_tp_chest_lat.y, m_tp_chest_lat.z);
        }
    }
    portal_grip_setup(V); // portal-mod E43
}
void CActor::ResetCallbacks()
{
    IKinematics* V = smart_cast<IKinematics*>(Visual());
    VERIFY(V);
    u16 spine0_bone = V->LL_BoneID("bip01_spine");
    u16 spine1_bone = V->LL_BoneID("bip01_spine1");
    u16 shoulder_bone = V->LL_BoneID("bip01_spine2");
    u16 head_bone = V->LL_BoneID("bip01_head");
    V->LL_GetBoneInstance(u16(spine0_bone)).reset_callback();
    V->LL_GetBoneInstance(u16(spine1_bone)).reset_callback();
    V->LL_GetBoneInstance(u16(shoulder_bone)).reset_callback();
    V->LL_GetBoneInstance(u16(head_bone)).reset_callback();
    for (const char* b : {"bip01_r_upperarm", "bip01_r_forearm", "bip01_l_upperarm", "bip01_l_forearm"}) // portal-mod E34, E36
        if (const u16 id = V->LL_BoneID(b); id != BI_NONE)
            V->LL_GetBoneInstance(id).reset_callback();
    for (u16 i = 0; i < m_tp_mirror_n; ++i) // portal-mod E39
        if (m_tp_mirror[i].l < V->LL_BoneCount())
            V->LL_GetBoneInstance(m_tp_mirror[i].l).reset_callback();
    m_tp_mirror_n = 0;
    for (u16 i = 0; i < m_grip_cb_n; ++i) // portal-mod E43
        if (m_grip_cb[i].bone < V->LL_BoneCount())
            V->LL_GetBoneInstance(m_grip_cb[i].bone).reset_callback();
    m_grip_cb_n = 0;
}

// portal-mod E43: the grip's bones: the arms' lengths, the hands' knuckle frames, the right hand's and fingers' callbacks
// (the left ones are E39's mirror bones - its callback hands them over)
void CActor::portal_grip_setup(IKinematics* V)
{
    m_grip_cb_n = 0;
    for (int s = 0; s < 2; ++s)
    {
        const char* sd = s == 0 ? "r" : "l";
        string64 nm;
        auto id = [&](const char* b) {
            xr_sprintf(nm, "bip01_%s_%s", sd, b);
            return V->LL_BoneID(nm);
        };
        m_grip_bone[s][0] = id("upperarm");
        m_grip_bone[s][1] = id("forearm");
        m_grip_bone[s][2] = id("hand");
        const char* fn[6] = {"finger1", "finger11", "finger12", "finger2", "finger21", "finger22"};
        for (int k = 0; k < 6; ++k)
            m_grip_fing_bone[s][k] = id(fn[k]);
        m_grip_len[s][0] = m_grip_len[s][1] = 0.f;
        m_grip_hand_ax[s].set(0.f, 0.f, 0.f);
        m_grip_fore_ok[s] = false;
        const u16 ua = m_grip_bone[s][0], fa = m_grip_bone[s][1], hb = m_grip_bone[s][2];
        if (ua == BI_NONE || fa == BI_NONE || hb == BI_NONE)
            continue;
        m_grip_len[s][0] = V->LL_GetData(fa).bind_transform.c.magnitude();
        m_grip_len[s][1] = V->LL_GetData(hb).bind_transform.c.magnitude();
        const u16 f1 = m_grip_fing_bone[s][0], f2 = m_grip_fing_bone[s][3];
        if (f1 != BI_NONE && f2 != BI_NONE && V->LL_GetData(f1).GetParentID() == hb && V->LL_GetData(f2).GetParentID() == hb)
        {
            const Fvector& c1 = V->LL_GetData(f1).bind_transform.c;
            const Fvector& c2 = V->LL_GetData(f2).bind_transform.c;
            Fvector ax, lat, n;
            ax.add(c1, c2).mul(0.5f);
            const float knuckles = ax.magnitude();
            ax.normalize_safe();
            lat.sub(c2, c1);
            n.crossproduct(ax, lat);
            n.normalize_safe();
            m_grip_hand_ax[s] = ax;
            m_grip_hand_n[s] = n;
            Msg("portal-mod E43: %s arm %.3f + %.3f m, wrist -> knuckles %.3f m, finger1 %.3f + %.3f, knuckle axis [%.2f %.2f %.2f] normal [%.2f %.2f %.2f]",
                sd, m_grip_len[s][0], m_grip_len[s][1], knuckles,
                m_grip_fing_bone[s][1] != BI_NONE ? V->LL_GetData(m_grip_fing_bone[s][1]).bind_transform.c.magnitude() : 0.f,
                m_grip_fing_bone[s][2] != BI_NONE ? V->LL_GetData(m_grip_fing_bone[s][2]).bind_transform.c.magnitude() : 0.f,
                ax.x, ax.y, ax.z, n.x, n.y, n.z);
        }
        else
            Msg("! portal-mod E43: %s hand: finger1/finger2 not on the hand - no hand turn", sd);
        if (s == 0) // the right hand and fingers (no other callback on them)
        {
            auto add = [&](u16 b, u8 kind) {
                if (b == BI_NONE || m_grip_cb_n >= 8)
                    return;
                SPortalGrip& g = m_grip_cb[m_grip_cb_n++];
                g.actor = this;
                g.bone = b;
                g.side = 0;
                g.kind = kind;
                V->LL_GetBoneInstance(b).set_callback(bctCustom, GripBoneCallback, &g);
            };
            add(hb, 0);
            for (int k = 0; k < 6; ++k)
                add(m_grip_fing_bone[0][k], 1);
        }
    }
}

void CActor::OnChangeVisual()
{
    ///	inherited::OnChangeVisual();
    {
        CPhysicsShell* tmp_shell = PPhysicsShell();
        PPhysicsShell() = NULL;
        inherited::OnChangeVisual();
        PPhysicsShell() = tmp_shell;
        tmp_shell = NULL;
    }

    IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(Visual());
    if (V)
    {
        SetCallbacks();
        m_anims->Create(V);
        m_vehicle_anims->Create(V);
        CDamageManager::reload(*cNameSect(), "damage", pSettings);
        //-------------------------------------------------------------------------------
        m_head = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_head");
        m_r_hand = smart_cast<IKinematics*>(Visual())->LL_BoneID(pSettings->r_string(*cNameSect(), "weapon_bone0"));
        m_l_finger1 = smart_cast<IKinematics*>(Visual())->LL_BoneID(pSettings->r_string(*cNameSect(), "weapon_bone1"));
        m_r_finger2 = smart_cast<IKinematics*>(Visual())->LL_BoneID(pSettings->r_string(*cNameSect(), "weapon_bone2"));
        //-------------------------------------------------------------------------------
        m_neck = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_neck");
        m_l_clavicle = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_l_clavicle");
        m_r_clavicle = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_r_clavicle");
        m_spine2 = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_spine2");
        m_spine1 = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_spine1");
        m_spine = smart_cast<IKinematics*>(Visual())->LL_BoneID("bip01_spine");
        //-------------------------------------------------------------------------------
        reattach_items();
        //-------------------------------------------------------------------------------
        m_pPhysics_support->in_ChangeVisual();
        //-------------------------------------------------------------------------------
        SetCallbacks();
        //-------------------------------------------------------------------------------
        m_current_head.invalidate();
        m_current_legs.invalidate();
        m_current_torso.invalidate();
        m_current_legs_blend = NULL;
        m_current_torso_blend = NULL;
        m_current_jump_blend = NULL;

        CStepManager::reload(*cNameSect());
    }
};

void CActor::ChangeVisual(shared_str NewVisual)
{
    if (!NewVisual.size())
        return;
    if (cNameVisual().size())
    {
        if (cNameVisual() == NewVisual)
            return;
    }

    cNameVisual_set(NewVisual);

    g_SetAnimation(mstate_real);
    Visual()->dcast_PKinematics()->CalculateBones_Invalidate();
    Visual()->dcast_PKinematics()->CalculateBones();

    CCar* car;
    if ((Holder()) && (car = smart_cast<CCar*>(Holder())))
    {
        IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(Visual());
        R_ASSERT(V);
        u16 anim_type = car->DriverAnimationType();
        SVehicleAnimCollection& anims = m_vehicle_anims->m_vehicles_type_collections[anim_type];
        V->PlayCycle(anims.idles[0], FALSE);

        ResetCallbacks();
        u16 head_bone = V->dcast_PKinematics()->LL_BoneID("bip01_head");
        V->dcast_PKinematics()->LL_GetBoneInstance(u16(head_bone)).set_callback(bctPhysics, VehicleHeadCallback, this);

        character_physics_support()->movement()->DestroyCharacter();
    }
};

void CActor::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    CInventoryOwner::save(output_packet);
    output_packet.w_u8(u8(m_bOutBorder));
    output_packet.w_u8(u8(character_physics_support()->movement()->BoxID()));
}

void CActor::load(IReader& input_packet)
{
    inherited::load(input_packet);
    CInventoryOwner::load(input_packet);
    m_bOutBorder = !!(input_packet.r_u8());
    if (ai().get_alife()->header().version() > 5)
        m_loaded_ph_box_id = input_packet.r_u8();
}

#ifdef DEBUG

extern Flags32 dbg_net_Draw_Flags;

void dbg_draw_piramid(Fvector pos, Fvector dir, float size, float xdir, u32 color)
{
    Fvector p0, p1, p2, p3, p4;
    p0.set(size, size, 0.0f);
    p1.set(-size, size, 0.0f);
    p2.set(-size, -size, 0.0f);
    p3.set(size, -size, 0.0f);
    p4.set(0, 0, size * 4);

    bool Double = false;
    Fmatrix t;
    t.identity();
    if (_valid(dir) && dir.square_magnitude() > 0.01f)
    {
        t.k.normalize(dir);
        Fvector::generate_orthonormal_basis(t.k, t.j, t.i);
    }
    else
    {
        t.rotateY(xdir);
        Double = true;
    }
    t.c.set(pos);

    //	Level().debug_renderer().draw_line(t, p0, p1, color);
    //	Level().debug_renderer().draw_line(t, p1, p2, color);
    //	Level().debug_renderer().draw_line(t, p2, p3, color);
    //	Level().debug_renderer().draw_line(t, p3, p0, color);

    //	Level().debug_renderer().draw_line(t, p0, p4, color);
    //	Level().debug_renderer().draw_line(t, p1, p4, color);
    //	Level().debug_renderer().draw_line(t, p2, p4, color);
    //	Level().debug_renderer().draw_line(t, p3, p4, color);

    if (!Double)
    {
        RCache.dbg_DrawTRI(t, p0, p1, p4, color);
        RCache.dbg_DrawTRI(t, p1, p2, p4, color);
        RCache.dbg_DrawTRI(t, p2, p3, p4, color);
        RCache.dbg_DrawTRI(t, p3, p0, p4, color);
    }
    else
    {
        //		Fmatrix scale;
        //		scale.scale(0.8f, 0.8f, 0.8f);
        //		t.mulA_44(scale);
        //		t.c.set(pos);

        Level().debug_renderer().draw_line(t, p0, p1, color);
        Level().debug_renderer().draw_line(t, p1, p2, color);
        Level().debug_renderer().draw_line(t, p2, p3, color);
        Level().debug_renderer().draw_line(t, p3, p0, color);

        Level().debug_renderer().draw_line(t, p0, p4, color);
        Level().debug_renderer().draw_line(t, p1, p4, color);
        Level().debug_renderer().draw_line(t, p2, p4, color);
        Level().debug_renderer().draw_line(t, p3, p4, color);
    };
};
#endif

void CActor::net_Save(NET_Packet& P)
{
#ifdef DEBUG
    u32 pos;
    Msg("Actor net_Save");

    pos = P.w_tell();
    inherited::net_Save(P);
    Msg("inherited::net_Save() : %d", P.w_tell() - pos);

    pos = P.w_tell();
    m_pPhysics_support->in_NetSave(P);
    P.w_u16(m_holderID);
    Msg("m_pPhysics_support->in_NetSave() : %d", P.w_tell() - pos);
#else
    inherited::net_Save(P);
    m_pPhysics_support->in_NetSave(P);
    P.w_u16(m_holderID);
#endif
}

BOOL CActor::net_SaveRelevant() { return TRUE; }

void CActor::SetHitInfo(CObject* who, CObject* weapon, s16 element, Fvector Pos, Fvector Dir)
{
    m_iLastHitterID = (who != NULL) ? who->ID() : u16(-1);
    m_iLastHittingWeaponID = (weapon != NULL) ? weapon->ID() : u16(-1);
    m_s16LastHittedElement = element;
    m_fLastHealth = GetfHealth();
    m_bWasHitted = true;
    m_vLastHitDir = Dir;
    m_vLastHitPos = Pos;
};

bool CActor::InventoryAllowSprint()
{
    PIItem pActiveItem = inventory().ActiveItem();
    if (pActiveItem && !pActiveItem->IsSprintAllowed())
    {
        return false;
    }

    auto wpn = smart_cast<const CWeapon*>(pActiveItem);
    if (wpn)
    {
        if (psActorFlags.test(AF_LOCK_RELOAD) && wpn->GetState() == CWeapon::eReload)
        {
            return false;
        }
    }

    const PIItem pOutfitItem = inventory().ItemFromSlot(OUTFIT_SLOT);
    if (pOutfitItem && !pOutfitItem->IsSprintAllowed())
    {
        return false;
    }
    return true;
};

void CActor::On_B_NotCurrentEntity() { inventory().Items_SetCurrentEntityHud(false); }

void CActor::net_Export(CSE_Abstract* E)
{
    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(E);
    creature->fHealth = GetfHealth();
    creature->timestamp = Level().timeServer();
    creature->flags = 0;
    creature->o_Position = Position();
    creature->o_model = angle_normalize(r_model_yaw);
    creature->o_torso.yaw = angle_normalize(unaffected_r_torso.yaw);
    creature->o_torso.pitch = angle_normalize(unaffected_r_torso.pitch);
    creature->o_torso.roll = angle_normalize(unaffected_r_torso.roll);
    creature->s_team = u8(g_Team());
    creature->s_squad = u8(g_Squad());
    creature->s_group = u8(g_Group());

    CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(E);
    actor->mstate = (u16)(mstate_real & 0x0000ffff);
    actor->accel = NET_SavedAccel;
    actor->velocity = character_physics_support()->movement()->GetVelocity();
    actor->fRadiation = g_Radiation();
    actor->weapon = u8(inventory().GetActiveSlot());
    /////////////////////////////////////////////////
    actor->m_u16NumItems = 0;
};
