//----------------------------------------------------
// file: PSObject.cpp
//----------------------------------------------------
#include "stdafx.h"

#include "ParticlesObject.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../Include/xrRender/ParticleCustom.h"
#include "..\xr_3da\render.h"
#include "..\xr_3da\IGame_Persistent.h"

static std::atomic<u32> s_pm_ps_serial{}; // portal-mod E53

CRandom* pm_random_use(CRandom* r); // portal-mod E53b: xrCore/_math.cpp
static_assert(sizeof(CRandom) == sizeof(std::mt19937), "CRandom is just its mt19937");

// portal-mod E53b: a seed for every Play from the system's serial and how many times it was played (the replay's recorder
// keeps it with the track)
void CParticlesObject::pm_reseed()
{
    pm_seed = pm_serial * 2654435761u + (++pm_plays) * 40503u + 17u;
    reinterpret_cast<std::mt19937&>(pm_rng).seed(pm_seed);
}

CParticlesObject::CParticlesObject(LPCSTR p_name, BOOL bAutoRemove, bool destroy_on_game_load) : inherited(destroy_on_game_load)
{
    Init(p_name, IRender_Sector::INVALID_SECTOR_ID, bAutoRemove);
}

void CParticlesObject::Init(LPCSTR p_name, IRender_Sector::sector_id_t sector_id, BOOL bAutoRemove)
{
    m_bLooped = false;
    m_bAutoRemove = bAutoRemove;

    // create visual
    renderable.visual = Render->model_CreateParticles(p_name);
    VERIFY(renderable.visual);
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    const float time_limit = V->GetTimeLimit();

    if (time_limit > 0.f)
    {
        m_iLifeTime = iFloor(time_limit * 1000.f);
    }
    else
    {
        if (bAutoRemove)
        {
            R_ASSERT3(!m_bAutoRemove, "Can't set auto-remove flag for looped particle system.", p_name);
        }
        else
        {
            m_iLifeTime = 0;
            m_bLooped = true;
        }
    }

    // spatial
    spatial.type = 0;
    spatial.sector_id = sector_id;

    // sheduled
    shedule.t_min = 20;
    shedule.t_max = 50;
    shedule_register();

    dwLastTime = Device.dwTimeGlobal;
    pm_serial = ++s_pm_ps_serial; // portal-mod E53
}

//----------------------------------------------------
CParticlesObject::~CParticlesObject()
{
    //	we do not need this since CPS_Instance does it
    //	shedule_unregister(true);
}

void CParticlesObject::UpdateSpatial()
{
    // spatial	(+ workaround occasional bug inside particle-system)
    if (_valid(renderable.visual->getVisData().sphere))
    {
        Fvector P;
        float R;
        renderable.xform.transform_tiny(P, renderable.visual->getVisData().sphere.P);
        R = renderable.visual->getVisData().sphere.R;
        if (0 == spatial.type)
        {
            // First 'valid' update - register
            spatial.type = STYPE_RENDERABLE;
            spatial.sphere.set(P, R);
            spatial_register();
        }
        else
        {
            BOOL bMove = FALSE;
            if (!P.similar(spatial.sphere.P, EPS_L * 10.f))
                bMove = TRUE;
            if (!fsimilar(R, spatial.sphere.R, 0.15f))
                bMove = TRUE;
            if (bMove)
            {
                spatial.sphere.set(P, R);
                spatial_move();
            }
        }
    }
}

const shared_str CParticlesObject::Name()
{
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    return (V) ? V->Name() : "";
}

//----------------------------------------------------
void CParticlesObject::Play(bool hudMode)
{
    dwLastTime = Device.dwTimeGlobal - 3 * PM_PS_QUANTUM; // portal-mod E53b: three quanta at once (was 33 ms)
    pm_acc = 0;
    market = Device.dwFrame - 1;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->SetHudMode(hudMode);
    pm_hud = hudMode; // portal-mod E53
    pm_born = Device.dwTimeGlobal;
    pm_reseed(); // portal-mod E53b
    {
        CRandom* const was = pm_random_use(&pm_rng);
        V->Play();
        pm_random_use(was);
    }

    DoWork();
}

void CParticlesObject::play_at_pos(const Fvector& pos, BOOL xform)
{
    dwLastTime = Device.dwTimeGlobal - 3 * PM_PS_QUANTUM; // portal-mod E53b
    pm_acc = 0;
    market = Device.dwFrame - 1;

    Fmatrix m;
    m.translate(pos);

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->UpdateParent(m, {}, xform);
    pm_parent = m; // portal-mod E53
    pm_vel.set(0.f, 0.f, 0.f);
    pm_xform = xform;
    pm_born = Device.dwTimeGlobal;
    pm_reseed(); // portal-mod E53b
    {
        CRandom* const was = pm_random_use(&pm_rng);
        V->Play();
        pm_random_use(was);
    }

    DoWork();
}

void CParticlesObject::Stop(BOOL bDefferedStop)
{
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->Stop(bDefferedStop);
}

void CParticlesObject::shedule_Update(u32 _dt)
{
    inherited::shedule_Update(_dt);

    if (market < Device.dwFrame - (30 + Random.randI(60))) // если не рендерили на прошлом кадре - ГГ не видит
    {
        DoWork();
    }
}

void CParticlesObject::DoWork()
{
    // Update
    if (m_bDead)
        return;

    if (market != Device.dwFrame)
    {
        market = Device.dwFrame;

        ZoneScoped;

        const u32 dt = Device.dwTimeGlobal - dwLastTime;
        if (dt)
        {
            IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
            VERIFY(V);
            // portal-mod E53b: whole quanta with the system's own random sequence; a long gap (not drawn for a while, a
            // hitch) goes in one call as before (the effect runs three steps of it and drops the rest)
            pm_acc += dt;
            const u32 pm_n = pm_acc / PM_PS_QUANTUM;
            pm_acc %= PM_PS_QUANTUM;
            if (pm_n)
            {
                CRandom* const was = pm_random_use(&pm_rng);
                if (pm_n <= 32)
                    for (u32 i = 0; i < pm_n; ++i)
                        V->OnFrame(PM_PS_QUANTUM);
                else
                    V->OnFrame(pm_n * PM_PS_QUANTUM);
                pm_random_use(was);
            }

            dwLastTime = Device.dwTimeGlobal;
        }

        UpdateSpatial();
    }
}

void CParticlesObject::PerformFrame()
{
    DoWork();
}

void CParticlesObject::SetXFORM(const Fmatrix& m)
{
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->UpdateParent(m, {}, TRUE);
    pm_parent = m; // portal-mod E53
    pm_vel.set(0.f, 0.f, 0.f);
    pm_xform = TRUE;
    renderable.xform.set(m);
    UpdateSpatial();
}

void CParticlesObject::UpdateParent(const Fmatrix& m, const Fvector& vel)
{
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->UpdateParent(m, vel, FALSE);
    pm_parent = m; // portal-mod E53
    pm_vel = vel;
    pm_xform = FALSE;
    UpdateSpatial();
}

Fvector& CParticlesObject::Position() { return renderable.visual->getVisData().sphere.P; }

float CParticlesObject::shedule_Scale() { return Device.vCameraPosition.distance_to(Position()) / 200.f; }

void CParticlesObject::renderable_Render(u32 context_id, IRenderable* root)
{
    R_ASSERT(renderable.visual);

    ::Render->add_Visual(context_id, root, renderable.visual, renderable.xform);
}

bool CParticlesObject::IsAutoRemove()
{
    if (m_bAutoRemove)
        return true;
    else
        return false;
}
void CParticlesObject::SetAutoRemove(bool auto_remove)
{
    VERIFY(!IsLooped());
    m_bAutoRemove = auto_remove;
}

//играются ли партиклы, отличается от PSI_Alive, тем что после
//остановки Stop партиклы могут еще доигрывать анимацию IsPlaying = true
bool CParticlesObject::IsPlaying()
{
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    return !!V->IsPlaying();
}
