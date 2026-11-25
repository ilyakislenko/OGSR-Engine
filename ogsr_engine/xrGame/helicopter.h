#pragma once

#include "shootingobject.h"
#include "weaponammo.h"
#include "rocketlauncher.h"
#include "entity.h"
#include "phskeleton.h"
#include "hit_immunity.h"
#include "script_export_space.h"
#include "memory_manager.h"
#include "HudSound.h"

#include "patrol_path.h"
#include "PHDestroyable.h"
#include "Explosive.h"
#include "holder_custom.h" // portal-mod E44

class CScriptGameObject;
class CLAItem;
class CHelicopterMovManager;
class CHelicopter;

enum EHeliHuntState
{
    eEnemyNone,
    eEnemyPoint,
    eEnemyEntity
};
struct SHeliEnemy
{
    EHeliHuntState type;
    Fvector destEnemyPos;
    u32 destEnemyID;
    float fire_trail_length_curr;
    float fire_trail_length_des;
    bool bUseFireTrail;
    float fStartFireTime;
    void reinit();
    void Update();
    void save(NET_Packet& output_packet);
    void load(IReader& input_packet);
    void Load(LPCSTR section);
};

enum EHeliBodyState
{
    eBodyByPath,
    eBodyToPoint
};
struct SHeliBodyState
{
    CHelicopter* parent;
    EHeliBodyState type;
    // settings, constants
    float model_pitch_k;
    float model_bank_k;
    float model_angSpeedBank;
    float model_angSpeedPitch;

    // runtime params
    Fvector currBodyHPB;

    bool b_looking_at_point;
    Fvector looking_point;
    void reinit();
    void LookAtPoint(Fvector point, bool do_it);

    void save(NET_Packet& output_packet);
    void load(IReader& input_packet);
    void Load(LPCSTR section);
};

enum EHeilMovementState
{
    eMovNone,
    eMovToPoint,
    eMovPatrolPath,
    eMovRoundPath,
    eMovLanding,
    eMovTakeOff
};
struct SHeliMovementState
{
    struct STmpPt
    {
        Fvector point;
        float dir_h;
        STmpPt(const Fvector& p, const float h) : point(p), dir_h(h){};
    };
    ~SHeliMovementState();
    CHelicopter* parent;
    EHeilMovementState type;
    // specified path
    const CPatrolPath* currPatrolPath;
    const CPatrolPath::CVertex* currPatrolVertex;

    int patrol_begin_idx;
    shared_str patrol_path_name;
    bool need_to_del_path;
    float safe_altitude_add;
    float maxLinearSpeed;
    float LinearAcc_fw;
    float LinearAcc_bk;
    float isAdnAcc;

protected:
    float HeadingSpK{}, HeadingSpB{};
    float PitchSpK{}, PitchSpB{}, AngSP{}, AngSH{};
    float speedInDestPoint{};
    void SetPointFlags(u32 idx, u32 new_flags);

public:
    float min_altitude;
    // runtime values
    Fvector desiredPoint;

    float curLinearSpeed;
    float curLinearAcc;

    Fvector currP;
    float currPathH;
    float currPathP;

    Fvector round_center;
    float round_radius;
    bool round_reverse;

    float onPointRangeDist;
    float GetSpeedInDestPoint();
    void SetSpeedInDestPoint(float);
    float GetAngSpeedPitch(float speed);
    float GetAngSpeedHeading(float speed);

    float GetSafeAltitude();
    void reinit();
    void Update();
    void UpdateMovToPoint();
    void UpdatePatrolPath();
    bool AlreadyOnPoint();
    void goByRoundPath(Fvector center, float radius, bool clockwise);
    float GetDistanceToDestPosition();
    void getPathAltitude(Fvector& point, float base_altitude);
    void SetDestPosition(Fvector* pos);
    void goPatrolByPatrolPath(LPCSTR path_name, int start_idx);
    void CreateRoundPoints(Fvector center, float radius, float start_h, float end_h, xr_vector<STmpPt>& round_points);
    void save(NET_Packet& output_packet);
    void load(IReader& input_packet);
    void Load(LPCSTR section);
    void net_Destroy();
};

class CHelicopter : public CEntity,
                    public CShootingObject,
                    public CRocketLauncher,
                    public CPHSkeleton,
                    public CPHDestroyable,
                    public CHitImmunity,
                    public CExplosive,
                    public CHolderCustom // portal-mod E44: a player flies it
#ifdef DEBUG
    ,
                    public pureRender
#endif

{
    typedef CEntity inherited;

public:
    enum EHeliState
    {
        eAlive = u32(0),
        eDead,
        eForce = u32(-1)
    };

    // heli weapons
    bool m_use_rocket_on_attack;
    bool m_use_mgun_on_attack;
    float m_min_rocket_dist, m_max_rocket_dist;
    float m_min_mgun_dist, m_max_mgun_dist;
    u32 m_time_between_rocket_attack;
    bool m_syncronize_rocket;
    float m_barrel_dir_tolerance;
    HUD_SOUND m_sndShot;
    HUD_SOUND m_sndShotRocket;

    Fvector m_fire_dir, m_fire_pos;

    u16 m_left_rocket_bone, m_right_rocket_bone, m_fire_bone, m_rotate_x_bone, m_rotate_y_bone;

    Fmatrix m_fire_bone_xform;
    Fmatrix m_i_bind_x_xform, m_i_bind_y_xform;
    Fvector2 m_lim_x_rot, m_lim_y_rot;
    Fvector2 m_tgt_rot;
    Fvector2 m_cur_rot;
    Fvector2 m_bind_rot;
    Fvector m_bind_x, m_bind_y;
    bool m_allow_fire;
    u16 m_last_launched_rocket;
    u32 m_last_rocket_attack;

    shared_str m_sAmmoType, m_sRocketSection;
    CCartridge m_CurrentAmmo;
    float delta_t;
    float flag_by_fire;
    Fmatrix m_left_rocket_bone_xform, m_right_rocket_bone_xform;

    static void BoneMGunCallbackX(CBoneInstance* B);
    static void BoneMGunCallbackY(CBoneInstance* B);
    void startRocket(u16 idx);

    // CShootingObject
    virtual const Fmatrix& ParticlesXFORM() const { return m_fire_bone_xform; };

    virtual const Fvector& CurrentFirePoint() { return m_fire_pos; };

    void MGunFireStart();
    void MGunFireEnd();
    void MGunUpdateFire();
    virtual void OnShot();

    void UpdateMGunDir();
    void UpdateWeapons();

    bool m_flame_started;
    bool m_light_started;
    bool m_ready_explode;
    bool m_exploded;
    bool m_dead;

protected:
    SHeliEnemy m_enemy;
    SHeliBodyState m_body;
    SHeliMovementState m_movement;

    // on death...
    Fvector m_death_ang_vel;
    float m_death_lin_vel_k;
    shared_str m_death_bones_to_hide;

    //////////////////////////////////////////////////

    // sound, light, particles...
    ref_sound m_engineSound;
    ref_sound m_brokenSound;
    //	ref_sound						m_explodeSound;
    ref_light m_light_render;
    CLAItem* m_lanim;
    u16 m_light_bone, m_smoke_bone;
    float m_light_range, m_light_brightness;
    Fcolor m_light_color;
    shared_str m_smoke_particle;
    CParticlesObject* m_pParticle;
    Fmatrix m_particleXFORM;

    void StartFlame();
    void UpdateHeliParticles();
    void DieHelicopter();
    void TurnLighting(bool bOn);
    void TurnEngineSound(bool bOn);
    // explosive
    virtual void OnAfterExplosion(){};
    virtual void GetRayExplosionSourcePos(Fvector& pos) { random_point_in_object_box(pos, this); }
    virtual void ActivateExplosionBox(const Fvector& size, Fvector& in_out_pos){};
    // general
    EHeliState m_curState;

    xr_map<s16, float> m_hitBones;
    typedef xr_map<s16, float>::iterator bonesIt;
    float m_stepRemains;

    void UpdateState();

public:
    void ExplodeHelicopter();

    CHelicopter();
    virtual ~CHelicopter();

    CHelicopter::EHeliState state() { return m_curState; };
    int state_script() { return m_curState; };

    void setState(CHelicopter::EHeliState s);
    void setState_script(u32 s) { setState((CHelicopter::EHeliState)s); };

    void init();
    virtual void reinit();

    virtual void Load(LPCSTR section);
    virtual void reload(LPCSTR section);

    virtual BOOL net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Export(CSE_Abstract* E){};
    virtual void net_Relcase(CObject* O);
    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);

    virtual void SpawnInitPhysics(CSE_Abstract* D);
    virtual CPhysicsShellHolder* PPhysicsShellHolder() { return PhysicsShellHolder(); }
    virtual void net_Save(NET_Packet& P);
    virtual BOOL net_SaveRelevant() { return (inherited::net_SaveRelevant() && BOOL(PPhysicsShell() != NULL)) || m_exploded; };

    virtual void renderable_Render(u32 context_id, IRenderable* root) override { inherited::renderable_Render(context_id, root); };

    virtual void OnEvent(NET_Packet& P, u16 type);
    virtual void UpdateCL();
    virtual void shedule_Update(u32 time_delta);
    void MoveStep();

    virtual void Hit(SHit* pHDS);
    virtual void PHHit(SHit& H);
    // CEntity
    virtual void HitSignal(float P, Fvector& local_dir, CObject* who, s16 element) { ; }

    virtual const Fmatrix& get_ParticlesXFORM();
    virtual const Fvector& get_CurrentFirePoint();
    virtual bool IsHudModeNow() { return false; };

    virtual CGameObject* cast_game_object() { return this; }
    virtual CExplosive* cast_explosive() { return this; }
    virtual CPHSkeleton* PHSkeleton() { return this; }

public:
    // for scripting
    bool isVisible(CScriptGameObject* O);
    bool isObjectVisible(CObject* O);
    bool isOnAttack() { return m_enemy.type != eEnemyNone; }

    void goPatrolByPatrolPath(LPCSTR path_name, int start_idx);
    void goByRoundPath(Fvector center, float radius, bool clockwise);
    void LookAtPoint(Fvector point, bool do_it);
    void SetDestPosition(Fvector* pos);
    float GetDistanceToDestPosition();

    void SetSpeedInDestPoint(float sp);
    float GetSpeedInDestPoint(float sp);

    void SetOnPointRangeDist(float d);
    float GetOnPointRangeDist();

    float GetCurrVelocity();
    float GetMaxVelocity();
    void SetMaxVelocity(float v);
    //////////////////////Start By JoHnY///////////////////////
    void SetLinearAcc(float LAcc_fw, float LAcc_bw);
    //////////////////////End By JoHnY/////////////////////////
    Fvector GetCurrVelocityVec();
    void SetBarrelDirTolerance(float val) { m_barrel_dir_tolerance = val; };
    void SetEnemy(CScriptGameObject* e);
    void SetEnemy(Fvector* pos);
    void UnSetEnemy();
    void SetFireTrailLength(float val);
    bool UseFireTrail();
    void UseFireTrail(bool val);

    float GetRealAltitude();

    int GetMovementState();
    int GetHuntState();
    int GetBodyState();

    virtual DLL_Pure* _construct();
    float GetSafeAltitude() { return m_movement.GetSafeAltitude(); };
    float GetHeliHealth() const { return inherited::GetfHealth(); }
    float SetHeliHealth(float value) { return inherited::SetfHealth(value); }

    virtual float GetHotness() override { return 0.f; }

    // portal-mod E44: a player flies it (F in, F out): WASD forward/back and the roll, jump/crouch up/down, the mouse
    // turns the view (the heading follows it), fire the nose gun, zoom rockets; while flown the AI's movement is off,
    // the helicopter moves by its XFORM and rays keep it out of the ground and the walls
    virtual CHolderCustom* cast_holder_custom() override { return this; }
    virtual void OnMouseMove(int x, int y) override;
    virtual void OnKeyboardPress(int dik) override;
    virtual void OnKeyboardRelease(int dik) override;
    virtual void OnKeyboardHold(int dik) override;
    virtual CInventory* GetInventory() override { return nullptr; }
    virtual void cam_Update(float dt, float fov = 90.0f) override;
    virtual void UpdateEx(float fov) override;
    virtual bool Use(const Fvector& pos, const Fvector& dir, const Fvector& foot_pos) override;
    virtual bool attach_Actor(CGameObject* actor) override;
    virtual void detach_Actor() override;
    virtual bool allowWeapon() const override { return false; }
    virtual bool HUDView() const override { return true; }
    virtual Fvector ExitPosition() override;
    virtual Fvector ExitVelocity() override { return m_pilot_vel; }
    virtual CCameraBase* Camera() override { return m_pilot_cam; }
    bool portal_piloted() const { return m_pilot_on; }
    float portal_pilot_gear();
    void PilotEject();
    // portal-mod E50: the realistic flight, crashes, ammunition, parked / falling with nobody at the controls
    int portal_pilot_mode() const { return m_pilot_mode; }
    void portal_set_pilot_mode(int mode);
    int portal_pilot_rounds() const { return m_pilot_ammo; }
    int portal_pilot_rockets() const { return m_pilot_rkt; }
    void portal_set_pilot_ammo(int rounds, int rockets);
    bool portal_pilot_on_ground() const { return m_pilot_ground; }
    float portal_pilot_agl() const { return m_pilot_agl; }
    float portal_pilot_collective() const { return m_pilot_coll; }
    float portal_pilot_pitch();
    float portal_pilot_bank();
    float portal_pilot_heading() const { return m_body.currBodyHPB.x; }
    float portal_pilot_stick_pitch();
    float portal_pilot_stick_bank();
    Fvector portal_pilot_velocity() const;
    Fvector portal_pilot_aim() const;
    float portal_pilot_vel(int i) const { return i == 0 ? m_pilot_vel.x : i == 1 ? m_pilot_vel.y : m_pilot_vel.z; } // by component
    float portal_pilot_aim_at(int i) const { return i == 0 ? m_pilot_aim.x : i == 1 ? m_pilot_aim.y : m_pilot_aim.z; }
    int portal_pilot_view() const { return m_pilot_view; }
    bool portal_pilot_assist() const { return m_pilot_on && pilot_key(ePilotAssist); }
    float portal_pilot_hull() const;
    u32 portal_pilot_crash_age() const { return m_pilot_crash_time ? Device.dwTimeGlobal - m_pilot_crash_time : u32(-1); }
    bool portal_pilot_parked() const { return m_pilot_parked; }
    void portal_set_pilot_parked(bool on);

private:
    enum
    {
        ePilotFwd = 1 << 0,
        ePilotBack = 1 << 1,
        ePilotLeft = 1 << 2,
        ePilotRight = 1 << 3,
        ePilotUp = 1 << 4,
        ePilotDown = 1 << 5,
        ePilotBoost = 1 << 6,
        ePilotFire = 1 << 7,
        ePilotRocket = 1 << 8,
        ePilotAssist = 1 << 9, // portal-mod E50: R held, hover hold
    };
    CCameraBase* m_pilot_cam{};
    bool m_pilot_on{};
    u32 m_pilot_keys{};      // pressed (press / release)
    u32 m_pilot_hold_keys{}; // held (hold events, each good for 150 ms)
    u32 m_pilot_hold_time[10]{};
    Fvector m_pilot_vel{};
    float m_pilot_turn{}; // the heading's rate, rad/s
    float m_pilot_vfwd_prev{};
    float m_pilot_afwd{}; // the forward acceleration, smoothed (the nose's pitch)
    float m_pilot_cam_h{}, m_pilot_cam_p{}; // the view's world heading and pitch (Fvector::setHP)
    float m_pilot_cam_d{}; // the chase camera's distance now
    float m_pilot_fov{90.f};
    u32 m_pilot_cam_frame{};
    int m_pilot_view{}; // 0 behind, 1 the cockpit
    Fvector m_pilot_aim{};
    Fvector m_pilot_barrel_dir{};
    bool m_pilot_ground{};
    bool m_pilot_eject{};
    u32 m_pilot_log_time{};
    u32 m_pilot_shots{}, m_pilot_rockets{};
    Fbox m_pilot_box{}; // the fuselage (local), for the rays
    float m_pilot_rotor_r{};
    u16 m_pilot_seat_bone{u16(-1)};
    int m_pilot_mode{1};               // portal-mod E50: 1 the realistic flight, 0 E44's simple one
    float m_pilot_coll{};              // the collective, 0..1
    float m_pilot_tp{}, m_pilot_tb{};  // the cyclic: the pitch and the bank it holds the hull at (rad, the engine's)
    float m_pilot_yaw{};               // the heading's rate (rad/s)
    float m_pilot_agl{};               // metres from the wheels to the ground under them
    float m_pilot_chase_h{};           // the camera behind: its heading, easing after the hull's
    u32 m_pilot_chase_frame{};
    int m_pilot_ammo{-1}, m_pilot_rkt{-1}; // rounds, rockets (-1: not loaded yet)
    float m_pilot_hp_full{};           // the health it had when first flown: crash damage is a fraction of it
    bool m_pilot_ghost{};              // left in the air: it flies on with nobody at the controls (and falls)
    bool m_pilot_parked{};             // left on the ground: the engine off, the AI does not move it
    float m_pilot_rotor_cut{};         // this step: the move toward what the rotor's disc met (m)
    bool m_pilot_rotor_up{};           // this step: something over the hull in the rotor's height stopped a climb
    u32 m_pilot_crash_time{};
    Fvector m_pilot_seat_local{};      // the seat in the hull's frame as last read alive (dead, the bones follow the physics)
    bool pilot_key(u32 bit) const;
    void pilot_set_key(int cmd, bool on, bool hold);
    void PilotStep(float dt);
    void PilotCollide(Fvector& m);
    void PilotStepReal(float dt);                                  // portal-mod E50
    void PilotImpacts(const Fvector& m0, const Fvector& m, float dt); // portal-mod E50
    void PilotDamage(float dmg, LPCSTR why);                        // portal-mod E50
    float PilotGroundGap();                                         // portal-mod E50
    void cam_UpdateReal(float dt, float fov);                       // portal-mod E50
    void PilotWeapons();
    Fvector PilotSeat();

public:
    float PilotRay(const Fvector& from, const Fvector& dir, float range);

#ifdef DEBUG
public:
    virtual void OnRender();
#endif

    DECLARE_SCRIPT_REGISTER_FUNCTION
};
add_to_type_list(CHelicopter)
#undef script_type_list
#define script_type_list save_type_list(CHelicopter)