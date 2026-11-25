#include "stdafx.h"
#include "helicopter.h"
#include "xrserver_objects_alife.h"
#include "PhysicsShell.h"
#include "level.h"
#include "ai_sounds.h"
#include "clsid_game.h"
#include "..\Include/xrRender/Kinematics.h"
#include "..\Include/xrRender/KinematicsAnimated.h"
#include "script_callback_ex.h"
#include "game_object_space.h"
#include "script_game_object.h"
#include "../xr_3da/LightAnimLibrary.h"
#include "HudManager.h"
#include "physicscommon.h"
#include "actor.h" // portal-mod E44
#include "ActorEffector.h"
#include "CameraFirstEye.h"
#include "../xr_3da/CameraManager.h"
#include "xr_level_controller.h"
#include "xrMessages.h"
#include "inventory_item.h"
#include "CustomRocket.h"
#include "../xr_3da/GameMtlLib.h" // portal-mod E50: passable materials are no obstacle

// 50fps fixed
float STEP = 0.02f;

CHelicopter::CHelicopter()
{
    m_pParticle = NULL;
    m_light_render = NULL;
    m_lanim = NULL;

    ISpatial* self = smart_cast<ISpatial*>(this);
    if (self)
        self->spatial.type |= STYPE_VISIBLEFORAI;

    m_movement.parent = this;
    m_body.parent = this;
}

CHelicopter::~CHelicopter()
{
    HUD_SOUND::DestroySound(m_sndShot);
    HUD_SOUND::DestroySound(m_sndShotRocket);
    xr_delete(m_pilot_cam); // portal-mod E44
}

void CHelicopter::setState(CHelicopter::EHeliState s) { m_curState = s; }

void CHelicopter::init()
{
    m_cur_rot.set(0.0f, 0.0f);
    m_tgt_rot.set(0.0f, 0.0f);
    m_bind_rot.set(0.0f, 0.0f);

    m_allow_fire = FALSE;
    m_use_rocket_on_attack = TRUE;
    m_use_mgun_on_attack = TRUE;
    m_syncronize_rocket = TRUE;
    m_min_rocket_dist = 20.0f;
    m_max_rocket_dist = 200.0f;
    m_time_between_rocket_attack = 0;
    m_last_rocket_attack = Device.dwTimeGlobal;

    SetfHealth(1.0f);
}

DLL_Pure* CHelicopter::_construct()
{
    CEntity::_construct();
    init();
    return this;
}

void CHelicopter::reinit()
{
    inherited::reinit();
    m_movement.reinit();
    m_body.reinit();
    m_enemy.reinit();
};

void CHelicopter::Load(LPCSTR section)
{
    inherited::Load(section);
    m_movement.Load(section);
    m_body.Load(section);
    m_enemy.Load(section);

    m_death_ang_vel = pSettings->r_fvector3(section, "death_angular_vel");
    m_death_lin_vel_k = pSettings->r_float(section, "death_lin_vel_koeff");

    CHitImmunity::LoadImmunities(pSettings->r_string(section, "immunities_sect"), pSettings);

    // weapons
    CShootingObject::Load(section);
    HUD_SOUND::LoadSound(section, "snd_shoot", m_sndShot, SOUND_TYPE_WEAPON_SHOOTING);
    HUD_SOUND::LoadSound(section, "snd_shoot_rocket", m_sndShotRocket, SOUND_TYPE_WEAPON_SHOOTING);
    CRocketLauncher::Load(section);

    UseFireTrail(m_enemy.bUseFireTrail); // temp force reloar disp params

    m_sAmmoType = pSettings->r_string(section, "ammo_class");
    m_CurrentAmmo.Load(*m_sAmmoType, 0);

    m_sRocketSection = pSettings->r_string(section, "rocket_class");

    m_use_rocket_on_attack = !!pSettings->r_bool(section, "use_rocket");
    m_use_mgun_on_attack = !!pSettings->r_bool(section, "use_mgun");
    m_min_rocket_dist = pSettings->r_float(section, "min_rocket_attack_dist");
    m_max_rocket_dist = pSettings->r_float(section, "max_rocket_attack_dist");
    m_min_mgun_dist = pSettings->r_float(section, "min_mgun_attack_dist");
    m_max_mgun_dist = pSettings->r_float(section, "max_mgun_attack_dist");
    m_time_between_rocket_attack = pSettings->r_u32(section, "time_between_rocket_attack");
    m_syncronize_rocket = !!pSettings->r_bool(section, "syncronize_rocket");
    m_barrel_dir_tolerance = pSettings->r_float(section, "barrel_dir_tolerance");

    // lighting & sounds
    m_smoke_particle = pSettings->r_string(section, "smoke_particle");

    m_light_range = pSettings->r_float(section, "light_range");
    m_light_brightness = pSettings->r_float(section, "light_brightness");

    m_light_color = pSettings->r_fcolor(section, "light_color");
    m_light_color.a = 1.f;
    m_light_color.mul_rgb(m_light_brightness);
    LPCSTR lanim = pSettings->r_string(section, "light_color_animmator");
    m_lanim = LALib.FindItem(lanim);
}

void CHelicopter::reload(LPCSTR section) { inherited::reload(section); }

void CollisionCallbackAlife(bool& do_colide, bool bo1, dContact& c, SGameMtl* material_1, SGameMtl* material_2) { do_colide = false; }

void ContactCallbackAlife(CDB::TRI* T, dContactGeom* c) {}
BOOL CHelicopter::net_Spawn(CSE_Abstract* DC)
{
    SetfHealth(100.0f);
    setState(CHelicopter::eAlive);
    m_flame_started = false;
    m_light_started = false;
    m_exploded = false;
    m_ready_explode = false;
    m_dead = false;

    if (!inherited::net_Spawn(DC))
        return (FALSE);

    CPHSkeleton::Spawn((CSE_Abstract*)(DC));
    for (u32 i = 0; i < 4; ++i)
        CRocketLauncher::SpawnRocket(*m_sRocketSection, smart_cast<CGameObject*>(this));

    // assigning m_animator here
    CSE_Abstract* abstract = (CSE_Abstract*)(DC);
    CSE_ALifeHelicopter* heli = smart_cast<CSE_ALifeHelicopter*>(abstract);
    VERIFY(heli);

    R_ASSERT(Visual() && smart_cast<IKinematics*>(Visual()));
    IKinematics* K = smart_cast<IKinematics*>(Visual());
    CInifile* pUserData = K->LL_UserData();

    m_rotate_x_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "wpn_rotate_x_bone"));
    m_rotate_y_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "wpn_rotate_y_bone"));
    m_fire_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "wpn_fire_bone"));
    m_death_bones_to_hide = pUserData->r_string("on_death_mode", "scale_bone");
    m_left_rocket_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "left_rocket_bone"));
    m_right_rocket_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "right_rocket_bone"));

    m_smoke_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "smoke_bone"));
    m_light_bone = K->LL_BoneID(pUserData->r_string("helicopter_definition", "light_bone"));

    CExplosive::Load(pUserData, "explosion");
    CExplosive::SetInitiator(ID());

    LPCSTR s = pUserData->r_string("helicopter_definition", "hit_section");

    if (pUserData->section_exist(s))
    {
        int lc = pUserData->line_count(s);
        LPCSTR name{};
        LPCSTR value{};
        s16 boneID;
        for (int i = 0; i < lc; ++i)
        {
            pUserData->r_line(s, i, &name, &value);
            boneID = K->LL_BoneID(name);
            m_hitBones.insert(std::make_pair(boneID, (float)atof(value)));
        }
    }

    CBoneInstance& biX = smart_cast<IKinematics*>(Visual())->LL_GetBoneInstance(m_rotate_x_bone);
    biX.set_callback(bctCustom, BoneMGunCallbackX, this);
    CBoneInstance& biY = smart_cast<IKinematics*>(Visual())->LL_GetBoneInstance(m_rotate_y_bone);
    biY.set_callback(bctCustom, BoneMGunCallbackY, this);
    CBoneData& bdX = K->LL_GetData(m_rotate_x_bone);
    VERIFY(bdX.IK_data.type == jtJoint);
    m_lim_x_rot.set(bdX.IK_data.limits[0].limit.x, bdX.IK_data.limits[0].limit.y);
    CBoneData& bdY = K->LL_GetData(m_rotate_y_bone);
    VERIFY(bdY.IK_data.type == jtJoint);
    m_lim_y_rot.set(bdY.IK_data.limits[1].limit.x, bdY.IK_data.limits[1].limit.y);

    xr_vector<Fmatrix> matrices;
    K->LL_GetBindTransform(matrices);
    m_i_bind_x_xform.invert(matrices[m_rotate_x_bone]);
    m_i_bind_y_xform.invert(matrices[m_rotate_y_bone]);
    m_bind_rot.x = matrices[m_rotate_x_bone].k.getP();
    m_bind_rot.y = matrices[m_rotate_y_bone].k.getH();
    m_bind_x.set(matrices[m_rotate_x_bone].c);
    m_bind_y.set(matrices[m_rotate_y_bone].c);

    IKinematicsAnimated* A = smart_cast<IKinematicsAnimated*>(Visual());
    if (A)
    {
        // portal-mod E50: a script-spawned helicopter (the call radio) has "$editor" for its cycle - "idle" then
        MotionID m = A->ID_Cycle_Safe(heli->startup_animation);
        if (!m.valid())
            m = A->ID_Cycle_Safe(shared_str("idle"));
        if (m.valid())
            A->PlayCycle(m);
        K->CalculateBones(TRUE);
    }

    {
        // portal-mod E50: ... and no engine sound - the section's engine_sound or the vanilla one
        shared_str snd = heli->engine_sound;
        if (!snd.size())
            snd = READ_IF_EXISTS(pSettings, r_string, cNameSect(), "engine_sound", "alexmx\\helicopter");
        string_path fn;
        if (snd.size() && FS.exist(fn, "$game_sounds$", *snd, ".ogg"))
        {
            m_engineSound.create(*snd, st_Effect, sg_SourceType);
            m_engineSound.play_at_pos(0, XFORM().c, sm_Looped);
        }
        else
            Msg("~ portal-mod E50: [%s] has no engine sound [%s]", cName().c_str(), snd.c_str());
    }

    if (m_bLightShotEnabled)
        CShootingObject::Light_Create();

    setVisible(TRUE);
    setEnabled(TRUE);

    m_stepRemains = 0.0f;

    // lighting
    m_light_render = ::Render->light_create();
    m_light_render->set_shadow(false);
    m_light_render->set_type(IRender_Light::POINT);
    m_light_render->set_range(m_light_range);
    m_light_render->set_color(m_light_color);
    m_light_render->set_moveable(true);

    if (g_Alive())
        processing_activate();
    TurnEngineSound(false);
    if (pUserData->section_exist("destroyed"))
        CPHDestroyable::Load(pUserData, "destroyed");
#ifdef DEBUG
    Device.seqRender.Add(this, REG_PRIORITY_LOW);
#endif

    renderable.visual->ignore_optimization = true;

    return TRUE;
}

void CHelicopter::net_Destroy()
{
    inherited::net_Destroy();
    CExplosive::net_Destroy();
    CShootingObject::Light_Destroy();
    CShootingObject::StopFlameParticles();
    CPHSkeleton::RespawnInit();
    CPHDestroyable::RespawnInit();
    m_engineSound.stop();
    m_brokenSound.stop();
    CParticlesObject::Destroy(m_pParticle);
    m_light_render.destroy();
    m_movement.net_Destroy();
#ifdef DEBUG
    Device.seqRender.Remove(this);
#endif
}

void CHelicopter::SpawnInitPhysics(CSE_Abstract* D)
{
    PPhysicsShell() = P_build_Shell(this, false);
    if (g_Alive())
    {
        PPhysicsShell()->EnabledCallbacks(FALSE);
        PPhysicsShell()->set_ObjectContactCallback(CollisionCallbackAlife);
        PPhysicsShell()->set_ContactCallback(ContactCallbackAlife);
        PPhysicsShell()->Disable();
    }
}

void CHelicopter::net_Save(NET_Packet& P)
{
    inherited::net_Save(P);
    CPHSkeleton::SaveNetState(P);
}

float GetCurrAcc(float V0, float V1, float dist, float a0, float a1);

void CHelicopter::MoveStep()
{
    Fvector dir, pathDir;
    float desired_H = m_movement.currPathH;
    float desired_P;
    if (m_movement.type != eMovNone)
    {
        float dist = m_movement.currP.distance_to(m_movement.desiredPoint);

        dir.sub(m_movement.desiredPoint, m_movement.currP);
        dir.normalize_safe();
        pathDir = dir;
        dir.getHP(desired_H, desired_P);
        float speed_ = _min(m_movement.GetSpeedInDestPoint(), GetMaxVelocity());

        static float ang = pSettings->r_float(cNameSect(), "magic_angle");
        if (m_movement.curLinearSpeed > GetMaxVelocity() || angle_difference(m_movement.currPathH, desired_H) > ang)
            m_movement.curLinearAcc = -m_movement.LinearAcc_bk;
        else
            m_movement.curLinearAcc = GetCurrAcc(m_movement.curLinearSpeed, speed_, dist * 0.95f, m_movement.LinearAcc_fw, -m_movement.LinearAcc_bk);

        angle_lerp(m_movement.currPathH, desired_H, m_movement.GetAngSpeedHeading(m_movement.curLinearSpeed), STEP);
        angle_lerp(m_movement.currPathP, desired_P, m_movement.GetAngSpeedPitch(m_movement.curLinearSpeed), STEP);

        dir.setHP(m_movement.currPathH, m_movement.currPathP);

        float vp = m_movement.curLinearSpeed * STEP + (m_movement.curLinearAcc * STEP * STEP) / 2.0f;
        m_movement.currP.mad(dir, vp);
        m_movement.curLinearSpeed += m_movement.curLinearAcc * STEP;
        // static bool aaa = false;
        // if (aaa)
        //	Msg("1-m_movement.curLinearSpeed = [%f]", m_movement.curLinearSpeed);
        clamp(m_movement.curLinearSpeed, 0.0f, 1000.0f);
        // if (aaa)
        //	Msg("2-m_movement.curLinearSpeed = [%f]", m_movement.curLinearSpeed);
    }
    else
    { // go stopping
        if (!fis_zero(m_movement.curLinearSpeed))
        {
            m_movement.curLinearAcc = -m_movement.LinearAcc_bk;

            float vp = m_movement.curLinearSpeed * STEP + (m_movement.curLinearAcc * STEP * STEP) / 2.0f;
            dir.setHP(m_movement.currPathH, m_movement.currPathP);
            dir.normalize_safe();
            m_movement.currP.mad(dir, vp);
            m_movement.curLinearSpeed += m_movement.curLinearAcc * STEP;
            clamp(m_movement.curLinearSpeed, 0.0f, 1000.0f);
            //			clamp(m_movement.curLinearSpeed,0.0f,m_movement.maxLinearSpeed);
        }
        else
        {
            m_movement.curLinearAcc = 0.0f;
            m_movement.curLinearSpeed = 0.0f;
        }
    };

    if (m_body.b_looking_at_point)
    {
        Fvector desired_dir;
        desired_dir.sub(m_body.looking_point, m_movement.currP).normalize_safe();

        float center_desired_H, tmp_P;
        desired_dir.getHP(center_desired_H, tmp_P);
        angle_lerp(m_body.currBodyHPB.x, center_desired_H, m_movement.GetAngSpeedHeading(m_movement.curLinearSpeed), STEP);
    }
    else
    {
        angle_lerp(m_body.currBodyHPB.x, m_movement.currPathH, m_movement.GetAngSpeedHeading(m_movement.curLinearSpeed), STEP);
    }

    float needBodyP = -m_body.model_pitch_k * m_movement.curLinearSpeed;
    if (m_movement.curLinearAcc < 0)
        needBodyP *= -1;
    angle_lerp(m_body.currBodyHPB.y, needBodyP, m_body.model_angSpeedPitch, STEP);

    float sign;
    Fvector cp;
    cp.crossproduct(pathDir, dir);
    (cp.y > 0.0) ? sign = 1.0f : sign = -1.0f;
    float ang_diff = angle_difference(m_movement.currPathH, desired_H);

    float needBodyB = -ang_diff * sign * m_body.model_bank_k * m_movement.curLinearSpeed;
    angle_lerp(m_body.currBodyHPB.z, needBodyB, m_body.model_angSpeedBank, STEP);

    XFORM().setHPB(m_body.currBodyHPB.x, m_body.currBodyHPB.y, m_body.currBodyHPB.z);

    XFORM().translate_over(m_movement.currP);
}

void CHelicopter::UpdateCL()
{
    inherited::UpdateCL();
    CExplosive::UpdateCL();
    if (PPhysicsShell() && (state() == CHelicopter::eDead))
    {
        PPhysicsShell()->InterpolateGlobalTransform(&XFORM());

        IKinematics* K = smart_cast<IKinematics*>(Visual());
        K->CalculateBones();
        // smoke
        UpdateHeliParticles();

        if (m_brokenSound._feedback())
            m_brokenSound.set_position(XFORM().c);

        return;
    }
    else
        PPhysicsShell()->SetTransform(XFORM());

    if (m_pilot_on || m_pilot_ghost) // portal-mod E44: a player flies it - the AI's movement is off (E50: or nobody, falling)
        PilotStep(Device.fTimeDelta);
    else if (!m_pilot_parked) // portal-mod E50: left on the ground, the AI does not move it
    {
        m_movement.Update();

        m_stepRemains += Device.fTimeDelta;
        while (m_stepRemains > STEP)
        {
            MoveStep();
            m_stepRemains -= STEP;
        }
    }

#ifdef DEBUG
    if (bDebug)
    {
        CGameFont* F = UI()->Font()->pFontDI;
        F->SetAligment(CGameFont::alCenter);
        //		F->SetSizeI			(0.02f);
        F->OutSetI(0.f, -0.8f);
        F->SetColor(0xffffffff);
        F->OutNext("Heli: speed=%4.4f acc=%4.4f dist=%4.4f", m_movement.curLinearSpeed, m_movement.curLinearAcc, m_movement.GetDistanceToDestPosition());
    }
#endif

    if (m_engineSound._feedback())
        m_engineSound.set_position(XFORM().c);

    if (m_pilot_on) // portal-mod E44: the player's guns aim where the view looks
        PilotWeapons();
    else if (!m_pilot_parked && !m_pilot_ghost) // portal-mod E50: left or falling: nobody shoots
    {
        m_enemy.Update();
        // weapon
        UpdateWeapons();
    }
    UpdateHeliParticles();

    IKinematics* K = smart_cast<IKinematics*>(Visual());
    K->CalculateBones();

    // portal-mod E44: the pilot in the seat; the view follows this frame's move
    if (m_pilot_on && Owner())
    {
        Fmatrix M;
        M.setHPB(m_body.currBodyHPB.x, 0.f, 0.f);
        M.c = PilotSeat();
        Owner()->XFORM().set(M);
        if (OwnerActor() && OwnerActor()->IsMyCamera())
        {
            cam_Update(Device.fTimeDelta, m_pilot_fov);
            OwnerActor()->Cameras().UpdateFromCamera(Camera());
            OwnerActor()->Cameras().ApplyDevice();
        }
    }
}

void CHelicopter::shedule_Update(u32 time_delta)
{
    if (!getEnabled())
        return;

    inherited::shedule_Update(time_delta);
    if (CPHDestroyable::Destroyed())
        CPHDestroyable::SheduleUpdate(time_delta);
    else
        CPHSkeleton::Update(time_delta);

    if (state() != CHelicopter::eDead)
    {
        for (u32 i = getRocketCount(); i < 4; ++i)
            CRocketLauncher::SpawnRocket(*m_sRocketSection, this);
    }
    if (m_ready_explode)
        ExplodeHelicopter();
}

void CHelicopter::goPatrolByPatrolPath(LPCSTR path_name, int start_idx) { m_movement.goPatrolByPatrolPath(path_name, start_idx); }

void CHelicopter::goByRoundPath(Fvector center, float radius, bool clockwise) { m_movement.goByRoundPath(center, radius, clockwise); }

void CHelicopter::LookAtPoint(Fvector point, bool do_it) { m_body.LookAtPoint(point, do_it); }

void CHelicopter::save(NET_Packet& output_packet)
{
    m_movement.save(output_packet);
    m_body.save(output_packet);
    m_enemy.save(output_packet);
    output_packet.w_vec3(XFORM().c);
    output_packet.w_float(m_barrel_dir_tolerance);
    save_data(m_use_rocket_on_attack, output_packet);
    save_data(m_use_mgun_on_attack, output_packet);
    save_data(m_min_rocket_dist, output_packet);
    save_data(m_max_rocket_dist, output_packet);
    save_data(m_min_mgun_dist, output_packet);
    save_data(m_max_mgun_dist, output_packet);
    save_data(m_time_between_rocket_attack, output_packet);
    save_data(m_syncronize_rocket, output_packet);
}

void CHelicopter::load(IReader& input_packet)
{
    m_movement.load(input_packet);
    m_body.load(input_packet);
    m_enemy.load(input_packet);
    input_packet.r_fvector3(XFORM().c);
    m_barrel_dir_tolerance = input_packet.r_float();
    UseFireTrail(m_enemy.bUseFireTrail); // force reloar disp params

    load_data(m_use_rocket_on_attack, input_packet);
    load_data(m_use_mgun_on_attack, input_packet);
    load_data(m_min_rocket_dist, input_packet);
    load_data(m_max_rocket_dist, input_packet);
    load_data(m_min_mgun_dist, input_packet);
    load_data(m_max_mgun_dist, input_packet);
    load_data(m_time_between_rocket_attack, input_packet);
    load_data(m_syncronize_rocket, input_packet);
}
void CHelicopter::net_Relcase(CObject* O)
{
    CExplosive::net_Relcase(O);
    inherited::net_Relcase(O);
}

// ---------------------------------------------------------------------------------------------------------------------
// portal-mod E44: a player flies the helicopter (CHolderCustom). F at the hull gets in (CActor::use_Helicopter), F gets
// out beside the cockpit. WASD: forward/back, the roll (sideways); jump/crouch: up/down; the accelerate key: faster; the
// mouse turns the view, the heading follows it; fire: the nose gun, zoom: rockets - both at the point the view looks at;
// camera keys 1 the cockpit, 2/3 behind.

// the rays skip the helicopter, its pilot, loose items, rockets and grenades
static BOOL portal_heli_ray_test(const collide::ray_defs&, CObject* O, LPVOID data)
{
    CHelicopter* H = static_cast<CHelicopter*>(data);
    if (O == static_cast<CObject*>(H) || O == static_cast<CObject*>(H->Owner()))
        return FALSE;
    if (smart_cast<CInventoryItem*>(O) || smart_cast<CCustomRocket*>(O))
        return FALSE;
    return TRUE;
}

// portal-mod E50: the realistic flight's numbers
static constexpr float PORTAL_G = 9.81f;
static constexpr float PORTAL_TMAX = 1.7f * PORTAL_G; // the rotor's thrust at full collective (per kg)
static constexpr float PORTAL_HOVER = PORTAL_G / PORTAL_TMAX;

// portal-mod E50: which way the engine's pitch and bank go (+P lifts the nose, +B tips the thrust right), found once
static float s_portal_pitch_sign = 0.f, s_portal_bank_sign = 0.f;
static void portal_heli_signs()
{
    if (s_portal_pitch_sign != 0.f)
        return;
    Fmatrix M;
    M.setHPB(0.f, 0.3f, 0.f);
    s_portal_pitch_sign = M.k.y > 0.f ? 1.f : -1.f;
    M.setHPB(0.f, 0.f, 0.3f);
    s_portal_bank_sign = M.j.x > 0.f ? 1.f : -1.f; // heading 0: the right is +x
    Msg("* E50 heli signs: pitch %+.0f, bank %+.0f", s_portal_pitch_sign, s_portal_bank_sign);
}

float CHelicopter::PilotRay(const Fvector& from, const Fvector& dir, float range)
{
    collide::rq_results R;
    collide::ray_defs RD(from, dir, range, CDB::OPT_CULL, collide::rqtBoth);
    float best = -1.f;
    if (Level().ObjectSpace.RayQuery(R, RD, nullptr, this, portal_heli_ray_test, this))
    {
        // portal-mod E50: what a body passes through (bushes and the like: flPassable) is no obstacle - the nearest of the rest
        for (const collide::rq_result* r = R.r_begin(); r != R.r_end(); ++r)
        {
            if (!r->O)
            {
                const CDB::TRI* T = Level().ObjectSpace.GetStaticTris() + r->element;
                if (GMLib.GetMaterialByIdx(T->material)->Flags.is(SGameMtl::flPassable))
                    continue;
            }
            if (best < 0.f || r->range < best)
                best = r->range;
        }
    }
    return best;
}

static int portal_pilot_key_index(int cmd)
{
    switch (cmd)
    {
    case kFWD: return 0;
    case kBACK: return 1;
    case kL_STRAFE: return 2;
    case kR_STRAFE: return 3;
    case kJUMP: return 4;
    case kCROUCH: return 5;
    case kACCEL:
    case kSPRINT_TOGGLE: return 6;
    case kWPN_FIRE: return 7;
    case kWPN_ZOOM: return 8;
    case kWPN_RELOAD: return 9; // portal-mod E50: hover hold
    }
    return -1;
}

bool CHelicopter::pilot_key(u32 bit) const
{
    if (m_pilot_keys & bit)
        return true;
    if (!(m_pilot_hold_keys & bit))
        return false;
    for (int i = 0; i < 10; ++i)
        if (bit == (1u << i))
            return Device.dwTimeGlobal - m_pilot_hold_time[i] < 150;
    return false;
}

void CHelicopter::pilot_set_key(int cmd, bool on, bool hold)
{
    const int i = portal_pilot_key_index(cmd);
    if (i < 0)
        return;
    const u32 bit = 1u << i;
    if (hold)
    {
        m_pilot_hold_keys |= bit;
        m_pilot_hold_time[i] = Device.dwTimeGlobal;
    }
    else if (on)
        m_pilot_keys |= bit;
    else
    {
        m_pilot_keys &= ~bit;
        m_pilot_hold_keys &= ~bit;
    }
}

void CHelicopter::OnKeyboardPress(int cmd)
{
    if (cmd == kCAM_1 || cmd == kCAM_2 || cmd == kCAM_3)
    {
        // portal-mod E47: F1 switches the cockpit <-> behind (take heli_06: F2 never brought it back), F2/F3 behind
        m_pilot_view = cmd == kCAM_1 ? 1 - m_pilot_view : 0;
        Msg("* E44 heli %s: view %s (cam key %d)", cName().c_str(), m_pilot_view ? "the cockpit" : "behind", int(cmd - kCAM_1) + 1);
    }
    else if (cmd == kWPN_FUNC) // portal-mod E50: V - the flight mode
        portal_set_pilot_mode(1 - m_pilot_mode);
    else
        pilot_set_key(cmd, true, false);
}

void CHelicopter::OnKeyboardRelease(int cmd) { pilot_set_key(cmd, false, false); }

void CHelicopter::OnKeyboardHold(int cmd) { pilot_set_key(cmd, true, true); }

void CHelicopter::OnMouseMove(int dx, int dy)
{
    const float scale = psMouseSens * psMouseSensScale / 50.f;
    if (m_pilot_mode == 1) // portal-mod E50: the mouse is the cyclic (drawn back: the nose up; to the right: bank right)
    {
        portal_heli_signs();
        const float k = scale * 0.6f;
        m_pilot_tb += float(dx) * k * s_portal_bank_sign;
        m_pilot_tp += (psMouseInvert.test(1) ? -1.f : 1.f) * float(dy) * k * s_portal_pitch_sign;
        return;
    }
    m_pilot_cam_h = angle_normalize_signed(m_pilot_cam_h - float(dx) * scale);
    m_pilot_cam_p -= (psMouseInvert.test(1) ? -1.f : 1.f) * float(dy) * scale * 3.f / 4.f;
    clamp(m_pilot_cam_p, -1.3f, 0.9f);
}

bool CHelicopter::Use(const Fvector& pos, const Fvector& dir, const Fvector& foot_pos) { return !Owner() && state() != CHelicopter::eDead && g_Alive() && !m_exploded; }

float CHelicopter::portal_pilot_gear() { return Visual() ? -Visual()->getVisData().box.min.y : 0.f; }

Fvector CHelicopter::PilotSeat()
{
    // portal-mod E50: once it is dead its bones follow the physics shell's elements and the seat's bone read near the
    // level's origin (the pilot thrown out 70 m off the wreck) - the seat as last read alive, in the hull's frame
    if (m_pilot_seat_bone == u16(-1))
        m_pilot_seat_local.set(0.f, 0.f, m_pilot_box.max.z * 0.5f);
    else if (state() != CHelicopter::eDead)
        m_pilot_seat_local = smart_cast<IKinematics*>(Visual())->LL_GetTransform(m_pilot_seat_bone).c;
    Fvector p;
    XFORM().transform_tiny(p, m_pilot_seat_local);
    return p;
}

bool CHelicopter::attach_Actor(CGameObject* actor)
{
    if (state() == CHelicopter::eDead || !g_Alive() || m_exploded)
        return false;
    CHolderCustom::attach_Actor(actor);
    if (!m_pilot_cam)
        m_pilot_cam = xr_new<CCameraFirstEye>(this, 0);

    IKinematics* K = smart_cast<IKinematics*>(Visual());
    LPCSTR seat = READ_IF_EXISTS(pSettings, r_string, *cNameSect(), "pilot_seat_bone", "pilot2");
    m_pilot_seat_bone = K->LL_BoneID(seat);
    const Fbox& vb = Visual()->getVisData().box;
    m_pilot_box.set(vb);
    m_pilot_rotor_r = READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "pilot_rotor_radius", 8.6f); // portal-mod E50: the Mi-24's 17.3 m disc (the visual's box follows the blades: 8.4..10.4)
    clamp(m_pilot_box.min.x, -3.2f, -1.f);
    clamp(m_pilot_box.max.x, 1.f, 3.2f);

    m_pilot_on = true;
    m_pilot_eject = false;
    m_pilot_keys = m_pilot_hold_keys = 0;
    if (!m_pilot_ghost) // portal-mod E50: back at the controls of a falling one: its own velocity
        m_pilot_vel.setHP(m_movement.currPathH, m_movement.currPathP).mul(m_movement.curLinearSpeed);
    m_pilot_turn = 0.f;
    m_pilot_vfwd_prev = 0.f;
    m_pilot_afwd = 0.f;
    m_pilot_cam_h = m_body.currBodyHPB.x;
    m_pilot_cam_p = -0.2f;
    m_pilot_cam_d = 0.f;
    m_pilot_view = 0;
    m_pilot_shots = m_pilot_rockets = 0;
    m_pilot_ground = false;
    MGunFireEnd();
    // portal-mod E50: the realistic flight from the hull's attitude; on the ground the collective down, in the air the
    // hover's; the ammunition loaded the first time
    portal_heli_signs();
    m_pilot_ghost = false;
    m_pilot_parked = false;
    m_pilot_tp = angle_normalize_signed(m_body.currBodyHPB.y); // (the AI leaves its angles past 2 pi at times)
    m_pilot_tb = angle_normalize_signed(m_body.currBodyHPB.z);
    m_pilot_yaw = 0.f;
    m_pilot_agl = PilotGroundGap();
    m_pilot_coll = m_pilot_agl < 0.6f ? 0.f : PORTAL_HOVER;
    m_pilot_chase_h = m_body.currBodyHPB.x;
    if (m_pilot_hp_full <= 0.f)
        m_pilot_hp_full = _max(1.f, GetfHealth());
    if (m_pilot_ammo < 0)
        m_pilot_ammo = 1470;
    if (m_pilot_rkt < 0)
        m_pilot_rkt = 128;
    fireDispersionBase = deg2rad(pSettings->r_float(*cNameSect(), "fire_dispersion_null"));
    TurnEngineSound(true);
    Msg("* E44 heli %s piloted at (%.2f, %.2f, %.2f): box (%.2f, %.2f, %.2f)-(%.2f, %.2f, %.2f), rotor %.1f m, seat bone %s %d", cName().c_str(), XFORM().c.x, XFORM().c.y,
        XFORM().c.z, vb.min.x, vb.min.y, vb.min.z, vb.max.x, vb.max.y, vb.max.z, m_pilot_rotor_r, seat, int(m_pilot_seat_bone));
    return true;
}

void CHelicopter::detach_Actor()
{
    if (!Owner())
        return;
    CHolderCustom::detach_Actor();
    m_pilot_on = false;
    // portal-mod E50: left in the air it flies on with nobody at the controls (its collective winding down: it falls);
    // left on the ground it is parked - the engine off, the AI does not move it
    const bool air = !m_pilot_ground && !m_pilot_eject && state() != CHelicopter::eDead;
    m_pilot_ghost = air;
    m_pilot_parked = !air && state() != CHelicopter::eDead;
    if (air)
    {
        if (m_pilot_mode == 0)
        {
            m_pilot_tp = angle_normalize_signed(m_body.currBodyHPB.y); // (the AI leaves its angles past 2 pi at times)
            m_pilot_tb = angle_normalize_signed(m_body.currBodyHPB.z);
        }
        m_pilot_coll = m_pilot_mode == 0 ? PORTAL_HOVER : _min(m_pilot_coll, PORTAL_HOVER);
        Msg("* E50 heli %s: left in the air (%.1f m up) - nobody at the controls", cName().c_str(), m_pilot_agl);
    }
    else if (m_pilot_parked)
        TurnEngineSound(false);
    MGunFireEnd();
    UseFireTrail(m_enemy.bUseFireTrail); // the AI's dispersion back
    // the AI takes over where the player left it: still, on the spot
    m_movement.currP = XFORM().c;
    m_movement.currPathH = m_body.currBodyHPB.x;
    m_movement.currPathP = 0.f;
    m_movement.curLinearSpeed = 0.f;
    m_movement.curLinearAcc = 0.f;
    Fvector p = XFORM().c;
    m_movement.SetDestPosition(&p);
    m_stepRemains = 0.f;
    Msg("* E44 heli %s left at (%.2f, %.2f, %.2f) after %u rounds, %u rockets", cName().c_str(), p.x, p.y, p.z, m_pilot_shots, m_pilot_rockets);
}

void CHelicopter::PilotEject()
{
    if (!OwnerActor() || m_pilot_eject)
        return;
    m_pilot_eject = true;
    NET_Packet P;
    u_EventGen(P, GEG_PLAYER_DETACH_HOLDER, OwnerActor()->ID());
    P.w_u32(ID());
    u_EventSend(P);
    Msg("* E44 heli %s: the pilot thrown out", cName().c_str());
}

// beside the cockpit on the side that is free (left, then right), on the ground when it is near; else on top
Fvector CHelicopter::ExitPosition()
{
    Fmatrix M;
    M.setHPB(m_body.currBodyHPB.x, 0.f, 0.f);
    M.c.set(XFORM().c);
    const Fvector seat = PilotSeat();
    for (float side : {-1.f, 1.f})
    {
        Fvector d;
        d.set(M.i).mul(side);
        const float out = 2.6f;
        if (PilotRay(seat, d, out + 0.4f) >= 0.f)
            continue;
        Fvector p;
        p.mad(seat, d, out);
        Fvector from = p;
        from.y += 0.5f;
        const float g = PilotRay(from, Fvector{0.f, -1.f, 0.f}, 4.f);
        if (g >= 0.f)
            p.y = from.y - g + 0.05f;
        return p;
    }
    Fvector p;
    M.transform_tiny(p, Fvector{0.f, m_pilot_box.max.y + 0.3f, 0.f});
    return p;
}

// the move cut where the fuselage's rays meet the world, per world axis (it slides along what stops it)
void CHelicopter::PilotCollide(Fvector& m)
{
    const Fbox& b = m_pilot_box;
    m_pilot_rotor_cut = 0.f; // portal-mod E50
    m_pilot_rotor_up = false;
    Fmatrix M;
    M.setHPB(m_body.currBodyHPB.x, 0.f, 0.f);
    M.c.set(XFORM().c);
    const float ybot = b.min.y + 0.5f, ylo = b.min.y + 0.6f, ymid = (b.min.y + b.max.y) * 0.5f;
    const float zf = b.max.z - 0.3f, zb = b.min.z + 0.3f;
    const float margin = 0.6f;

    // down from 0.5 m above the wheels' level: the ground comes no nearer than that 0.5 m (sunk in: up out of it)
    {
        const Fvector pts[] = {{0.f, ybot, 0.f}, {0.f, ybot, zf * 0.6f}, {0.f, ybot, zb * 0.6f}, {b.min.x * 0.8f, ybot, 0.f}, {b.max.x * 0.8f, ybot, 0.f}};
        const float reach = 0.5f + _max(0.f, -m.y) + 0.3f;
        float gap = flt_max;
        for (const Fvector& lp : pts)
        {
            Fvector p;
            M.transform_tiny(p, lp);
            const float r = PilotRay(p, Fvector{0.f, -1.f, 0.f}, reach);
            if (r >= 0.f)
                gap = _min(gap, r - 0.5f);
        }
        m_pilot_ground = false;
        if (gap < flt_max)
        {
            if (m.y < -gap)
                m.y = -gap;
            if (gap + m.y < 0.05f)
                m_pilot_ground = true;
        }
    }
    // up from the middle to the rotor's height
    if (m.y > 0.f)
    {
        const float top = b.max.y - ymid;
        const Fvector pts[] = {{0.f, ymid, 0.f}, {0.f, ymid, zf * 0.5f}, {0.f, ymid, zb * 0.5f}};
        for (const Fvector& lp : pts)
        {
            Fvector p;
            M.transform_tiny(p, lp);
            const float r = PilotRay(p, Fvector{0.f, 1.f, 0.f}, top + m.y + 0.3f);
            if (r >= 0.f)
            {
                const float y0 = m.y;
                m.y = _max(0.f, _min(m.y, r - top - 0.3f));
                if (m.y < y0 - 1e-3f)
                    m_pilot_rotor_up = true; // portal-mod E50: something over the hull, in the rotor's height
            }
        }
    }
    M.c.y += m.y;
    // sideways, world x then z: the nose, the tail, the middle, low and at mid height, the wing tips; the rotor's disc
    // from its hub
    const Fvector pts[] = {{0.f, ymid, zf}, {0.f, ymid, zb}, {0.f, ymid, 0.f}, {0.f, ylo, zf * 0.8f}, {0.f, ylo, zb * 0.6f}, {b.min.x + 0.2f, ylo, 0.f}, {b.max.x - 0.2f, ylo, 0.f}};
    for (int ax = 0; ax < 3; ax += 2)
    {
        float& c = ax == 0 ? m.x : m.z;
        if (_abs(c) < 1e-4f)
            continue;
        Fvector d{0.f, 0.f, 0.f};
        (ax == 0 ? d.x : d.z) = c > 0.f ? 1.f : -1.f;
        float allow = _abs(c);
        for (const Fvector& lp : pts)
        {
            Fvector p;
            M.transform_tiny(p, lp);
            const float r = PilotRay(p, d, allow + margin);
            if (r >= 0.f)
                allow = _max(0.f, _min(allow, r - margin));
        }
        if (m_pilot_rotor_r > 1.f)
        {
            Fvector p;
            M.transform_tiny(p, Fvector{0.f, b.max.y - 0.3f, 0.f});
            const float r = PilotRay(p, d, allow + m_pilot_rotor_r);
            if (r >= 0.f)
            {
                const float a0 = allow;
                allow = _max(0.f, _min(allow, r - m_pilot_rotor_r));
                if (allow < a0 - 1e-4f) // portal-mod E50: the rotor's disc met something, at the speed it went that way
                    m_pilot_rotor_cut = _sqrt(m_pilot_rotor_cut * m_pilot_rotor_cut + c * c);
            }
        }
        c = c > 0.f ? allow : -allow;
        (ax == 0 ? M.c.x : M.c.z) += c;
    }
}

// the player's flight: a hover that answers the keys - the velocity eases to what they ask in the heading's frame, the
// heading follows the view's, the nose dips with the speed and the acceleration, it banks into strafes and turns
void CHelicopter::PilotStep(float dt)
{
    clamp(dt, 0.f, 0.05f);
    if (dt <= 0.f)
        return;
    if (OwnerActor() && !OwnerActor()->g_Alive()) // portal-mod E47: a dead pilot holds no keys (it hangs there)
        m_pilot_keys = m_pilot_hold_keys = 0;
    if (m_pilot_mode == 1 || !m_pilot_on) // portal-mod E50: the realistic flight (and nobody at the controls)
    {
        PilotStepReal(dt);
        return;
    }
    float& H = m_body.currBodyHPB.x;
    float& P = m_body.currBodyHPB.y;
    float& B = m_body.currBodyHPB.z;
    P = angle_normalize_signed(P); // portal-mod E50: the AI leaves its angles past 2 pi at times
    B = angle_normalize_signed(B);

    const float in_f = (pilot_key(ePilotFwd) ? 1.f : 0.f) - (pilot_key(ePilotBack) ? 1.f : 0.f);
    const float in_s = (pilot_key(ePilotRight) ? 1.f : 0.f) - (pilot_key(ePilotLeft) ? 1.f : 0.f);
    const float in_u = (pilot_key(ePilotUp) ? 1.f : 0.f) - (pilot_key(ePilotDown) ? 1.f : 0.f);
    const bool boost = pilot_key(ePilotBoost);
    const bool parked = m_pilot_ground && in_u <= 0.f; // on its wheels: no turning, no sliding

    // the heading turns toward the view's (up to 63 deg/s), with a little inertia
    const float want_turn = parked ? 0.f : clampr(angle_difference_signed(m_pilot_cam_h, H) * 2.2f, -1.1f, 1.1f);
    m_pilot_turn += clampr(want_turn - m_pilot_turn, -3.f * dt, 3.f * dt);
    H = angle_normalize_signed(H + m_pilot_turn * dt);
    Fvector f, r;
    f.set(-_sin(H), 0.f, _cos(H));
    r.set(_cos(H), 0.f, _sin(H));

    // the velocity eases to what the keys ask
    Fvector want{0.f, 0.f, 0.f};
    if (!parked)
    {
        want.mad(f, in_f * (in_f > 0.f ? (boost ? 50.f : 32.f) : 12.f));
        want.mad(r, in_s * 14.f);
    }
    Fvector vh{m_pilot_vel.x, 0.f, m_pilot_vel.z};
    Fvector dv;
    dv.sub(want, vh);
    const float acc = parked ? 15.f : (in_f != 0.f || in_s != 0.f) ? (boost ? 12.f : 9.f) : 7.f;
    const float dvm = dv.magnitude();
    if (dvm > acc * dt)
        dv.mul(acc * dt / dvm);
    vh.add(dv);
    m_pilot_vel.x = vh.x;
    m_pilot_vel.z = vh.z;
    m_pilot_agl = PilotGroundGap(); // portal-mod E50: down it eases onto the ground (~1 m/s near it): no hard landing
    const float wy = in_u > 0.f ? in_u * 9.f : in_u * _min(11.f, 1.f + _max(0.f, m_pilot_agl) * 0.6f);
    m_pilot_vel.y += clampr(wy - m_pilot_vel.y, -8.f * dt, 8.f * dt);

    // the move, cut by the rays; what they take off it the velocity loses
    Fvector m;
    m.mul(m_pilot_vel, dt);
    const Fvector m0 = m;
    PilotCollide(m);
    PilotImpacts(m0, m, dt); // portal-mod E50: what the rays stopped is an impact
    if (state() == CHelicopter::eDead)
        return;
    if (_abs(m.x) < _abs(m0.x) - 1e-4f)
        m_pilot_vel.x = 0.f;
    if (_abs(m.z) < _abs(m0.z) - 1e-4f)
        m_pilot_vel.z = 0.f;
    if (m.y > m0.y + 1e-4f || m.y < m0.y - 1e-4f)
        m_pilot_vel.y = 0.f;

    // the attitude: the nose down with the speed and the acceleration, banked into strafes and turns
    const float vf = m_pilot_vel.dotproduct(f), vs = m_pilot_vel.dotproduct(r);
    m_pilot_afwd += ((vf - m_pilot_vfwd_prev) / dt - m_pilot_afwd) * _min(1.f, dt * 4.f);
    m_pilot_vfwd_prev = vf;
    float wantP = -(vf * 0.008f + m_pilot_afwd * 0.035f);
    float wantB = vs * 0.03f - m_pilot_turn * (0.15f + _abs(vf) * 0.012f);
    if (m_pilot_ground)
        wantP = wantB = 0.f;
    clamp(wantP, -0.42f, 0.3f);
    clamp(wantB, -0.55f, 0.55f);
    P += (wantP - P) * _min(1.f, dt * 2.5f);
    B += (wantB - B) * _min(1.f, dt * 2.5f);

    Fvector pos;
    pos.add(XFORM().c, m);
    XFORM().setHPB(H, P, B);
    XFORM().translate_over(pos);
    // the AI's state kept with it (scripts read it; the AI goes on from here)
    m_movement.currP = pos;
    m_movement.currPathH = H;
    m_movement.currPathP = 0.f;
    m_movement.curLinearSpeed = m_pilot_vel.magnitude();

    if (Device.dwTimeGlobal >= m_pilot_log_time)
    {
        m_pilot_log_time = Device.dwTimeGlobal + 1000;
        Msg("* E44 heli at (%.1f, %.1f, %.1f) v (%.1f, %.1f, %.1f) %.1f m/s, heading %.0f, pitch %.0f, bank %.0f, %s, keys %03x, view %.0f/%.0f, aim (%.1f, %.1f, %.1f), %u rounds, %u rockets",
            pos.x, pos.y, pos.z, m_pilot_vel.x, m_pilot_vel.y, m_pilot_vel.z, m_pilot_vel.magnitude(), rad2deg(H), rad2deg(P), rad2deg(B), m_pilot_ground ? "on the ground" : "flying",
            m_pilot_keys | m_pilot_hold_keys, rad2deg(m_pilot_cam_h), rad2deg(m_pilot_cam_p), m_pilot_aim.x, m_pilot_aim.y, m_pilot_aim.z, m_pilot_shots, m_pilot_rockets);
    }
}

// the turret and the rockets aim at the point the view looks at
void CHelicopter::PilotWeapons()
{
    UpdateMGunDir();
    angle_lerp(m_cur_rot.x, m_tgt_rot.x, PI_MUL_2, Device.fTimeDelta);
    angle_lerp(m_cur_rot.y, m_tgt_rot.y, PI_MUL_2, Device.fTimeDelta);
    if (!m_allow_fire) // out of the turret's reach or still turning: the rounds go where the barrel points
        m_fire_dir.set(m_pilot_barrel_dir);
    if (pilot_key(ePilotFire) && m_pilot_ammo != 0) // portal-mod E50: while the rounds last
    {
        if (!IsWorking())
            MGunFireStart();
    }
    else if (IsWorking())
        MGunFireEnd();
    MGunUpdateFire();
    if (pilot_key(ePilotRocket) && m_pilot_rkt != 0 && Device.dwTimeGlobal - m_last_rocket_attack > 400) // portal-mod E50: while rockets last
    {
        startRocket(m_last_launched_rocket == 1 ? 2 : 1);
        m_last_rocket_attack = Device.dwTimeGlobal;
    }
}

// the view: the world heading/pitch from the mouse; behind - over the helicopter and back along the view, pulled in by
// what stands behind; the cockpit - the pilot's eyes. The guns' aim: the view's ray from abreast of the helicopter on
void CHelicopter::cam_Update(float dt, float fov)
{
    if (!m_pilot_cam)
        return;
    if (m_pilot_mode == 1) // portal-mod E50: the realistic flight's view
    {
        cam_UpdateReal(dt, fov);
        return;
    }
    Fmatrix V;
    V.setHPB(m_pilot_cam_h, m_pilot_cam_p, 0.f);
    const Fvector dir = V.k;
    Fvector P;
    if (m_pilot_view == 1)
    {
        P = PilotSeat();
        P.mad(XFORM().j, 0.75f);
    }
    else
    {
        Fvector pivot = XFORM().c;
        pivot.y += _max(2.f, m_pilot_box.max.y + 1.5f);
        const float want = 1.2f * _max(m_pilot_box.max.z - m_pilot_box.min.z, 8.f);
        Fvector back;
        back.invert(dir);
        const float r = PilotRay(pivot, back, want + 0.5f);
        const float d = r >= 0.f ? _max(1.f, r - 0.5f) : want;
        if (Device.dwFrame != m_pilot_cam_frame)
        {
            m_pilot_cam_frame = Device.dwFrame;
            if (m_pilot_cam_d <= 0.f || d < m_pilot_cam_d)
                m_pilot_cam_d = d; // in at once (never through a wall), out slowly
            else
                m_pilot_cam_d += (d - m_pilot_cam_d) * _min(1.f, dt * 2.f);
        }
        P.mad(pivot, back, _min(m_pilot_cam_d, d));
    }
    m_pilot_cam->vPosition.set(P);
    m_pilot_cam->vDirection.set(dir);
    m_pilot_cam->vNormal.set(V.j);
    m_pilot_cam->yaw = -m_pilot_cam_h;
    m_pilot_cam->pitch = -m_pilot_cam_p;
    m_pilot_cam->roll = 0.f;
    m_pilot_cam->f_fov = fov;

    Fvector to_c;
    to_c.sub(XFORM().c, P);
    Fvector A;
    A.mad(P, dir, _max(0.f, to_c.dotproduct(dir)));
    const float ar = PilotRay(A, dir, 800.f);
    m_pilot_aim.mad(A, dir, ar >= 0.f ? ar : 800.f);

    Level().Cameras().UpdateFromCamera(m_pilot_cam);
}

// ---------------------------------------------------------------------------------------------------------------------
// portal-mod E50: the realistic flight, crashes, ammunition, parked / falling with nobody at the controls

float CHelicopter::portal_pilot_pitch()
{
    portal_heli_signs();
    return rad2deg(m_body.currBodyHPB.y * s_portal_pitch_sign);
}

float CHelicopter::portal_pilot_bank()
{
    portal_heli_signs();
    return rad2deg(m_body.currBodyHPB.z * s_portal_bank_sign);
}

float CHelicopter::portal_pilot_stick_pitch()
{
    portal_heli_signs();
    return rad2deg(m_pilot_tp * s_portal_pitch_sign);
}

float CHelicopter::portal_pilot_stick_bank()
{
    portal_heli_signs();
    return rad2deg(m_pilot_tb * s_portal_bank_sign);
}

// (out of line, logged once a second: a script read inf from the inline ones in some runs while the flight's own log was right)
Fvector CHelicopter::portal_pilot_velocity() const { return m_pilot_vel; }

Fvector CHelicopter::portal_pilot_aim() const { return m_pilot_aim; }

float CHelicopter::portal_pilot_hull() const
{
    const float full = m_pilot_hp_full > 0.f ? m_pilot_hp_full : _max(1.f, GetfHealth());
    return clampr(GetfHealth() / full, 0.f, 1.f);
}

void CHelicopter::portal_set_pilot_ammo(int rounds, int rockets)
{
    m_pilot_ammo = rounds;
    m_pilot_rkt = rockets;
}

void CHelicopter::portal_set_pilot_mode(int mode)
{
    mode = mode ? 1 : 0;
    if (mode == m_pilot_mode)
        return;
    m_pilot_mode = mode;
    if (mode == 1)
    {
        m_pilot_tp = angle_normalize_signed(m_body.currBodyHPB.y); // (the AI leaves its angles past 2 pi at times)
        m_pilot_tb = angle_normalize_signed(m_body.currBodyHPB.z);
        m_pilot_yaw = 0.f;
        m_pilot_coll = m_pilot_ground ? 0.f : PORTAL_HOVER;
        m_pilot_chase_h = m_body.currBodyHPB.x;
    }
    else
    {
        m_pilot_cam_h = m_body.currBodyHPB.x;
        m_pilot_cam_p = -0.2f;
        m_pilot_turn = 0.f;
    }
    Msg("* E50 heli %s: flight mode %s", cName().c_str(), mode ? "realistic" : "simple");
}

void CHelicopter::portal_set_pilot_parked(bool on)
{
    if (m_pilot_on || state() == CHelicopter::eDead)
        return;
    m_pilot_ghost = false;
    m_pilot_parked = on;
    if (on)
    {
        MGunFireEnd();
        TurnEngineSound(false);
    }
    else
    {
        TurnEngineSound(true);
        m_movement.currP = XFORM().c;
        m_movement.currPathH = m_body.currBodyHPB.x;
        m_movement.currPathP = 0.f;
        m_movement.curLinearSpeed = 0.f;
        m_movement.curLinearAcc = 0.f;
        Fvector p = XFORM().c;
        m_movement.SetDestPosition(&p);
        m_stepRemains = 0.f;
    }
    Msg("* E50 heli %s: %s", cName().c_str(), on ? "parked (engine off, no AI)" : "given back to the AI");
}

// metres from the wheels down to what is under them (400: nothing within reach)
float CHelicopter::PilotGroundGap()
{
    Fvector p;
    XFORM().transform_tiny(p, Fvector{0.f, m_pilot_box.min.y + 0.5f, 0.f});
    const float r = PilotRay(p, Fvector{0.f, -1.f, 0.f}, 400.f);
    return r >= 0.f ? r - 0.5f : 400.f;
}

void CHelicopter::PilotDamage(float dmg, LPCSTR why)
{
    if (state() == CHelicopter::eDead)
        return;
    const float full = m_pilot_hp_full > 0.f ? m_pilot_hp_full : _max(1.f, GetfHealth());
    const float h0 = GetfHealth();
    SetfHealth(_max(0.f, h0 - dmg * full));
    m_pilot_crash_time = Device.dwTimeGlobal;
    Msg("* E50 heli %s: %s - damage %.2f of its health, %.2f -> %.2f (of %.2f)", cName().c_str(), why, dmg, h0, GetfHealth(), full);
    if (GetfHealth() <= 0.005f * full)
        DieHelicopter(); // the pilot thrown out (E44), it burns and falls
}

// what PilotCollide stopped this step, as speeds: the hull into something, the wheels into the ground, the rotor's disc
void CHelicopter::PilotImpacts(const Fvector& m0, const Fvector& m, float dt)
{
    if (dt <= 0.f || state() == CHelicopter::eDead)
        return;
    const float lx = _abs(m.x) < _abs(m0.x) - 1e-4f ? m_pilot_vel.x : 0.f;
    const float lz = _abs(m.z) < _abs(m0.z) - 1e-4f ? m_pilot_vel.z : 0.f;
    const float ly = (m.y > m0.y + 1e-4f || m.y < m0.y - 1e-4f) ? m_pilot_vel.y : 0.f;
    const float hull = _sqrt(lx * lx + lz * lz);
    const float down = ly < 0.f ? -ly : 0.f;
    const float rise = ly > 0.f ? ly : 0.f;
    const float rotor = m_pilot_rotor_cut / dt;
    string256 why;
    if (rotor > 4.f || (m_pilot_rotor_up && rise > 2.f))
    {
        xr_sprintf(why, "the rotor struck (%.1f m/s%s)", rotor, m_pilot_rotor_up ? ", from above" : "");
        PilotDamage(10.f, why);
        return;
    }
    float dmg = 0.f;
    if (rotor > 1.5f)
        dmg += 0.12f;
    if (hull > 7.f)
        dmg += (hull - 7.f) * (hull - 7.f) * 0.003f;
    if (down > 4.f)
        dmg += (down - 4.f) * (down - 4.f) * 0.02f;
    if (dmg > 0.f)
    {
        xr_sprintf(why, "an impact: the hull %.1f m/s, down %.1f m/s, the rotor %.1f m/s", hull, down, rotor);
        PilotDamage(dmg, why);
    }
}

void CHelicopter::PilotStepReal(float dt)
{
    portal_heli_signs();
    const bool ghost = !m_pilot_on;
    float& H = m_body.currBodyHPB.x;
    float& P = m_body.currBodyHPB.y;
    float& B = m_body.currBodyHPB.z;
    P = angle_normalize_signed(P); // the AI leaves its angles past 2 pi at times (a bank of 359 deg made a 60 deg stick)
    B = angle_normalize_signed(B);
    m_pilot_agl = PilotGroundGap();

    float in_c = 0.f, in_y = 0.f, in_p = 0.f;
    bool boost = false, assist = false;
    if (!ghost)
    {
        in_c = (pilot_key(ePilotUp) ? 1.f : 0.f) - (pilot_key(ePilotDown) ? 1.f : 0.f);
        in_y = (pilot_key(ePilotRight) ? 1.f : 0.f) - (pilot_key(ePilotLeft) ? 1.f : 0.f);
        in_p = (pilot_key(ePilotBack) ? 1.f : 0.f) - (pilot_key(ePilotFwd) ? 1.f : 0.f); // S lifts the nose, W dips it
        boost = pilot_key(ePilotBoost);
        assist = pilot_key(ePilotAssist);
    }
    Fvector f, r;
    f.set(-_sin(H), 0.f, _cos(H));
    r.set(_cos(H), 0.f, _sin(H));
    const float vf = m_pilot_vel.dotproduct(f), vs = m_pilot_vel.dotproduct(r), vy = m_pilot_vel.y;
    const float vh = _sqrt(vf * vf + vs * vs);

    // the collective (it stays where it is left); nobody at the controls: the engine winds down
    if (ghost)
        m_pilot_coll = _max(0.f, m_pilot_coll - 0.15f * dt);
    else
        m_pilot_coll += in_c * (boost ? 0.9f : 0.4f) * dt;
    // the cyclic from the keys
    m_pilot_tp += in_p * 0.7f * dt * s_portal_pitch_sign;
    if (assist)
    {
        // hover hold: tilted against the level drift, the collective against the climb (jump / crouch: 3 m/s up / down,
        // slower near the ground)
        const float wp = clampr(vf * 0.06f, -0.35f, 0.35f) * s_portal_pitch_sign;
        const float wb = clampr(-vs * 0.06f, -0.35f, 0.35f) * s_portal_bank_sign;
        m_pilot_tp += (wp - m_pilot_tp) * _min(1.f, dt * 2.5f);
        m_pilot_tb += (wb - m_pilot_tb) * _min(1.f, dt * 2.5f);
        const float want_vy = in_c * (in_c < 0.f ? _min(3.f, 0.6f + _max(0.f, m_pilot_agl) * 0.4f) : 3.f);
        const float tilt = _max(0.5f, _cos(P) * _cos(B));
        const float wc = PORTAL_HOVER / tilt - (vy - want_vy) * 0.08f;
        m_pilot_coll += clampr(wc - m_pilot_coll, -0.8f * dt, 0.8f * dt);
    }
    clamp(m_pilot_coll, 0.f, 1.f);
    {
        float up = m_pilot_tp * s_portal_pitch_sign;
        clamp(up, -0.75f, 0.5f); // 43 deg nose down, 29 up
        m_pilot_tp = up * s_portal_pitch_sign;
        clamp(m_pilot_tb, -1.05f, 1.05f); // 60 deg either side
    }
    // on its wheels under the hover's collective: it sits level, the stick centres, it does not turn
    const bool sitting = m_pilot_ground && m_pilot_coll < PORTAL_HOVER * 0.9f;
    if (sitting)
    {
        m_pilot_tp += (0.f - m_pilot_tp) * _min(1.f, dt * 3.f);
        m_pilot_tb += (0.f - m_pilot_tb) * _min(1.f, dt * 3.f);
    }
    // the hull follows the cyclic (57 deg/s in pitch, 80 in bank)
    P += clampr((sitting ? 0.f : m_pilot_tp) - P, -1.0f * dt, 1.0f * dt);
    B += clampr((sitting ? 0.f : m_pilot_tb) - B, -1.4f * dt, 1.4f * dt);

    // the heading: the pedals (weaker with speed), at speed the bank's coordinated turn; the tail into the wind
    const float s = clampr((vh - 8.f) / 14.f, 0.f, 1.f);
    float want = -in_y * (0.9f - 0.55f * s);
    if (vh > 8.f)
        want -= PORTAL_G * tanf(clampr(B * s_portal_bank_sign, -1.2f, 1.2f)) / _max(vh, 10.f) * s;
    if (sitting)
        want = 0.f;
    m_pilot_yaw += clampr(want - m_pilot_yaw, -2.f * dt, 2.f * dt);
    H = angle_normalize_signed(H + m_pilot_yaw * dt);
    if (vh > 15.f && !sitting)
    {
        const float hv = atan2f(-m_pilot_vel.x, m_pilot_vel.z);
        const float d = angle_difference_signed(hv, H);
        if (_abs(d) < 1.6f)
            H = angle_normalize_signed(H + d * 0.3f * s * dt);
    }

    // the forces: the thrust along the hull's up (+ translational lift), gravity, drag in the hull's level frame
    Fmatrix M;
    M.setHPB(H, P, B);
    const float T = m_pilot_coll * PORTAL_TMAX * (1.f + 0.12f * clampr((vh - 8.f) / 12.f, 0.f, 1.f));
    Fvector a;
    a.mul(M.j, T);
    a.y -= PORTAL_G;
    f.set(-_sin(H), 0.f, _cos(H));
    r.set(_cos(H), 0.f, _sin(H));
    const float vf2 = m_pilot_vel.dotproduct(f), vs2 = m_pilot_vel.dotproduct(r);
    a.mad(f, -(0.04f * vf2 + 0.0006f * vf2 * _abs(vf2)));
    a.mad(r, -(0.25f * vs2 + 0.01f * vs2 * _abs(vs2)));
    a.y -= 0.35f * vy + 0.015f * vy * _abs(vy);
    m_pilot_vel.mad(a, dt);
    if (m_pilot_ground) // the wheels: it rolls a little, it does not slide
    {
        const float k = _max(0.f, 1.f - (sitting ? 6.f : 1.5f) * dt);
        m_pilot_vel.x *= k;
        m_pilot_vel.z *= k;
    }

    // the move, cut by the rays; what they stop is an impact
    Fvector m;
    m.mul(m_pilot_vel, dt);
    const Fvector m0 = m;
    PilotCollide(m);
    PilotImpacts(m0, m, dt);
    if (state() == CHelicopter::eDead)
        return;
    if (_abs(m.x) < _abs(m0.x) - 1e-4f)
        m_pilot_vel.x = 0.f;
    if (_abs(m.z) < _abs(m0.z) - 1e-4f)
        m_pilot_vel.z = 0.f;
    if (m.y > m0.y + 1e-4f || m.y < m0.y - 1e-4f)
        m_pilot_vel.y = 0.f;

    Fvector pos;
    pos.add(XFORM().c, m);
    XFORM().setHPB(H, P, B);
    XFORM().translate_over(pos);
    m_movement.currP = pos;
    m_movement.currPathH = H;
    m_movement.currPathP = 0.f;
    m_movement.curLinearSpeed = m_pilot_vel.magnitude();

    if (ghost && m_pilot_ground && m_pilot_vel.magnitude() < 0.5f)
    {
        m_pilot_ghost = false;
        m_pilot_parked = true;
        TurnEngineSound(false);
        Msg("* E50 heli %s: down with nobody at the controls at (%.1f, %.1f, %.1f), health %.2f - parked", cName().c_str(), pos.x, pos.y, pos.z, GetfHealth());
    }
    if (Device.dwTimeGlobal >= m_pilot_log_time)
    {
        m_pilot_log_time = Device.dwTimeGlobal + 1000;
        Msg("* E50 heli %s%s at (%.1f, %.1f, %.1f) v (%.1f, %.1f, %.1f) %.1f m/s, agl %.1f, heading %.0f, pitch %.0f, bank %.0f (stick %.0f/%.0f), collective %.2f, yaw %.0f deg/s, %s%s, keys %03x, %d rounds, %d rockets, hull %.2f",
            cName().c_str(), ghost ? " (nobody at the controls)" : "", pos.x, pos.y, pos.z, m_pilot_vel.x, m_pilot_vel.y, m_pilot_vel.z, m_pilot_vel.magnitude(), m_pilot_agl,
            rad2deg(H), rad2deg(P * s_portal_pitch_sign), rad2deg(B * s_portal_bank_sign), rad2deg(m_pilot_tp * s_portal_pitch_sign), rad2deg(m_pilot_tb * s_portal_bank_sign),
            m_pilot_coll, rad2deg(m_pilot_yaw), m_pilot_ground ? "on the ground" : "flying", assist ? ", hover hold" : "", m_pilot_keys | m_pilot_hold_keys, m_pilot_ammo,
            m_pilot_rkt, portal_pilot_hull());
    }
}

// the realistic flight's view: the cockpit is the hull's (it rolls and pitches with it); behind, the camera follows the
// heading, a little down, level. The guns' aim: along the hull from its nose (the boresight)
void CHelicopter::cam_UpdateReal(float dt, float fov)
{
    Fmatrix M;
    M.setHPB(m_body.currBodyHPB.x, m_body.currBodyHPB.y, m_body.currBodyHPB.z);
    Fvector dir, up, P;
    if (m_pilot_view == 1)
    {
        dir = M.k;
        up = M.j;
        P = PilotSeat();
        P.mad(M.j, 0.75f);
    }
    else
    {
        if (Device.dwFrame != m_pilot_chase_frame)
        {
            m_pilot_chase_frame = Device.dwFrame;
            m_pilot_chase_h = angle_normalize_signed(m_pilot_chase_h + angle_difference_signed(m_body.currBodyHPB.x, m_pilot_chase_h) * _min(1.f, dt * 2.5f));
        }
        Fmatrix V;
        V.setHPB(m_pilot_chase_h, -0.2f, 0.f);
        dir = V.k;
        up = V.j;
        Fvector pivot = XFORM().c;
        pivot.y += _max(2.f, m_pilot_box.max.y + 1.5f);
        const float want = 1.2f * _max(m_pilot_box.max.z - m_pilot_box.min.z, 8.f);
        Fvector back;
        back.invert(dir);
        const float rr = PilotRay(pivot, back, want + 0.5f);
        const float d = rr >= 0.f ? _max(1.f, rr - 0.5f) : want;
        if (Device.dwFrame != m_pilot_cam_frame)
        {
            m_pilot_cam_frame = Device.dwFrame;
            if (m_pilot_cam_d <= 0.f || d < m_pilot_cam_d)
                m_pilot_cam_d = d;
            else
                m_pilot_cam_d += (d - m_pilot_cam_d) * _min(1.f, dt * 2.f);
        }
        P.mad(pivot, back, _min(m_pilot_cam_d, d));
    }
    dir.getHP(m_pilot_cam_h, m_pilot_cam_p);
    m_pilot_cam->vPosition.set(P);
    m_pilot_cam->vDirection.set(dir);
    m_pilot_cam->vNormal.set(up);
    m_pilot_cam->yaw = -m_pilot_cam_h;
    m_pilot_cam->pitch = -m_pilot_cam_p;
    m_pilot_cam->roll = 0.f;
    m_pilot_cam->f_fov = fov;

    Fvector A;
    A.mad(XFORM().c, M.k, m_pilot_box.max.z + 1.f);
    const float ar = PilotRay(A, M.k, 1500.f);
    m_pilot_aim.mad(A, M.k, ar >= 0.f ? ar : 1500.f);

    Level().Cameras().UpdateFromCamera(m_pilot_cam);
}

void CHelicopter::UpdateEx(float fov)
{
    m_pilot_fov = fov;
    if (m_pilot_on && OwnerActor() && OwnerActor()->IsMyCamera())
    {
        cam_Update(Device.fTimeDelta, fov);
        OwnerActor()->Cameras().UpdateFromCamera(Camera());
        OwnerActor()->Cameras().ApplyDevice();
    }
}
