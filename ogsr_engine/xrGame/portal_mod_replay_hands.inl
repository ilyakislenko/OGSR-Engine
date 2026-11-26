// portal-mod E53d: the first-person view of a take - the recorder's capture and the player's hands. tools/engine_patches/
// patch_e53d.py copies this file into ogsr_engine/xrGame; portal_mod_replay.cpp includes it in its unnamed namespace right
// after the Player (P). The data: portal_mod_replay_hands_data.inl.
//
// Recording: CHUDManager::Render_MAIN (the frame's render, right after its CLevel::OnFrame) hands the capture the camera
// as drawn, then every model player_hud draws (player_hud.cpp: pm_hud_add) with its pose read right after the renderer
// calculated it; the frame is stamped with the time of the recorder's sample taken in the same frame (the moment of the
// frame's objects) and moved into the ring at the next sample.
// Playing: the hands are posed from the record (bones lerped between two frames, slow motion smooth) and drawn by
// Render_MAIN on the camera there is, in the camera's own frame - on the take's camera (F) exactly where they were.

//------------------------------------------------------------------------------------------------------------------
// the camera's frame: x right, y up, z forward (as apply_camera builds it), at the origin
void hands_basis(Fmatrix& m, const Fvector& d_in, const Fvector& n_in)
{
    Fvector d = d_in;
    d.normalize_safe();
    Fvector r;
    r.crossproduct(n_in, d);
    if (r.square_magnitude() < 1e-10f)
    {
        Fvector up{0.f, 1.f, 0.f};
        if (_abs(d.y) > 0.99f)
            up.set(0.f, 0.f, 1.f);
        r.crossproduct(up, d);
    }
    r.normalize_safe();
    Fvector n;
    n.crossproduct(d, r).normalize_safe();
    m.identity();
    m.i = r;
    m.j = n;
    m.k = d;
}

float hands_det(const Fmatrix& m)
{
    Fvector jk;
    jk.crossproduct(m.j, m.k);
    return m.i.dotproduct(jk);
}

// between two placements of a model; a mirror image (E26's left gun) stays one
void hands_xf_lerp(const Fmatrix& a, const Fmatrix& b, float k, Fmatrix& out)
{
    const bool ma = hands_det(a) < 0.f, mb = hands_det(b) < 0.f;
    if (ma != mb)
    {
        out = k < 0.5f ? a : b;
        return;
    }
    Fmatrix A = a, B = b;
    if (ma)
    {
        A.i.invert();
        B.i.invert();
    }
    float qa[4], qb[4], sa = 1.f, sb = 1.f;
    quat_of(A, qa, &sa);
    quat_of(B, qb, &sb);
    Fquaternion Q;
    quat_lerp(qa, qb, k, Q);
    Fvector p;
    p.lerp(a.c, b.c, k);
    make_xform(out, Q, p, sa + (sb - sa) * k);
    if (ma)
        out.i.invert();
}

//------------------------------------------------------------------------------------------------------------------
// the recorder's side
struct HandsCapture
{
    std::mutex lock;
    u32 rec_frame{u32(-1)}; // Device.dwFrame of the recorder's last sample
    double rec_t{}; // ... and its time
    u32 done_frame{u32(-1)}; // the frame last taken (Render_MAIN can come twice in a frame: a second viewport)
    bool on{};
    HudFrame cur;
    xr_vector<std::pair<const void*, u16>> inst_of; // the frame's models: their kinematics -> inst
    xr_vector<HudFrame> done; // taken, for the next sample
    // this run's slots (the models seen: a kinematics' address with its file) and items; the ring copies them
    xr_vector<HudSlot> slots;
    xr_vector<const void*> slot_ptr;
    xr_vector<shared_str> items;
} C;

u16 hands_item_index(const shared_str& s)
{
    for (u32 i = 0; i < C.items.size(); ++i)
        if (C.items[i] == s)
            return u16(i);
    C.items.push_back(s);
    return u16(C.items.size() - 1);
}

void hands_cap_begin()
{
    std::lock_guard<std::mutex> lk(C.lock);
    C.on = false;
    if (P.open || portal_mod_replay || C.rec_frame != Device.dwFrame || C.done_frame == Device.dwFrame)
        return; // no sample in this frame (the recorder off, or between its samples), or taken already
    C.on = true;
    C.done_frame = Device.dwFrame;
    C.inst_of.clear();
    HudFrame& f = C.cur;
    f = HudFrame{};
    f.t = C.rec_t;
    f.cam_p = Device.vCameraPosition;
    f.cam_d = Device.vCameraDirection;
    f.cam_n = Device.vCameraTop;
    f.fov = Device.fFOV;
    f.hud_fov = psHUD_FOV <= 1.f ? psHUD_FOV * Device.fFOV : psHUD_FOV;
    CActor* A = Actor();
    if (A && g_pGameLevel && g_pGameLevel->CurrentViewEntity() == A && A->active_cam() == eacFirstEye && !portal_mod::cine && !A->Holder())
        f.flags |= HF_FP;
    if (A)
        if (PIItem it = A->inventory().ActiveItem())
        {
            xr_string s = it->object().cNameSect().c_str();
            s += "|";
            if (CHudItem* hi = smart_cast<CHudItem*>(it))
                s += hi->HudSection().size() ? hi->HudSection().c_str() : "";
            f.item = hands_item_index(shared_str(s.c_str()));
        }
}

void hands_cap_draw(IRenderVisual* v, const Fmatrix& m)
{
    if (!C.on || !v)
        return;
    std::lock_guard<std::mutex> lk(C.lock);
    if (!C.on)
        return;
    HudFrame& f = C.cur;
    IKinematics* K = v->dcast_PKinematics();
    const void* key = K ? static_cast<const void*>(K) : static_cast<const void*>(v);
    u16 inst = u16(-1);
    for (const auto& p : C.inst_of)
        if (p.first == key)
        {
            inst = p.second; // drawn again in this frame (E26: the same gun mirrored)
            break;
        }
    if (inst == u16(-1))
    {
        const shared_str name = v->getDebugName();
        const u16 nb = K ? K->LL_BoneCount() : 0;
        u16 slot = u16(-1);
        for (u32 s = 0; s < C.slots.size(); ++s)
            if (C.slot_ptr[s] == key && C.slots[s].visual == name && C.slots[s].nbones == nb)
            {
                slot = u16(s);
                break;
            }
        if (slot == u16(-1))
        {
            for (const void*& p : C.slot_ptr)
                if (p == key)
                    p = nullptr; // that model is gone: another one at its address
            C.slots.push_back(HudSlot{name, nb});
            C.slot_ptr.push_back(key);
            slot = u16(C.slots.size() - 1);
        }
        HudInst n{};
        n.slot = slot;
        n.nbones = nb;
        n.bone0 = u32(f.bones.size());
        if (K)
        {
            const VisMask vm = K->LL_GetBonesVisible();
            n.vis[0] = vm._visimask.flags;
            n.vis[1] = vm._visimask_ex.flags;
            f.bones.resize(f.bones.size() + nb);
            for (u16 b = 0; b < nb; ++b)
            {
                const Fmatrix& bm = K->LL_GetTransform(b);
                bone_quant(bm, f.bones[n.bone0 + b]);
                const float sc = (bm.i.magnitude() + bm.j.magnitude() + bm.k.magnitude()) / 3.f;
                if (_valid(sc) && _abs(sc - 1.f) > 1e-3f)
                    f.scl.push_back(HudScale{n.bone0 + b, sc}); // E26's folded arm
            }
        }
        f.inst.push_back(n);
        inst = u16(f.inst.size() - 1);
        C.inst_of.emplace_back(key, inst);
    }
    // where: in the camera's own frame (the HUD's space is the world's axes with the eye at the origin)
    Fmatrix R, Rt;
    hands_basis(R, f.cam_d, f.cam_n);
    Rt.transpose(R);
    Rt._14_ = Rt._24_ = Rt._34_ = 0.f;
    Rt._41 = Rt._42 = Rt._43 = 0.f;
    Rt._44_ = 1.f;
    HudDraw d{};
    d.inst = inst;
    d.xf.mul_43(Rt, m);
    if (_valid(d.xf))
        f.draws.push_back(d);
}

void hands_cap_end()
{
    std::lock_guard<std::mutex> lk(C.lock);
    if (!C.on)
        return;
    C.on = false;
    if (!C.cur.draws.empty())
        C.cur.flags |= HF_HUD;
    C.done.push_back(std::move(C.cur));
    if (C.done.size() > 32)
        C.done.erase(C.done.begin()); // nobody takes them
}

// recorder_frame, a sample: the frames drawn since go into the ring, and this frame is to be taken at t
void hands_rec_sample(Take& T, double t)
{
    std::lock_guard<std::mutex> lk(C.lock);
    for (HudFrame& f : C.done)
        if (T.hands.frames.empty() || f.t > T.hands.frames.back().t)
            T.hands.frames.push_back(std::move(f));
    C.done.clear();
    if (T.hands.slots.size() != C.slots.size())
        T.hands.slots = C.slots;
    if (T.hands.items.size() != C.items.size())
        T.hands.items = C.items;
    C.rec_frame = Device.dwFrame;
    C.rec_t = t;
}

// recorder_frame: E53b's map of the HUD's particles onto the world model (to take them back to the HUD)
void hands_rec_map(Take& T, double t, const Fmatrix& m)
{
    if (!T.hands.maps.empty() && 0 == memcmp(&T.hands.maps.back().m, &m, sizeof(Fmatrix)))
        return;
    T.hands.maps.push_back(HudMap{t, m});
}

//------------------------------------------------------------------------------------------------------------------
// the player's side
class HandsRoot : public IRenderable
{
public:
    void renderable_Render(u32, IRenderable*) override {}
    float GetHotness() override { return 0.15f; } // E76's thermal: the robot himself barely warm, as his puppet
};

struct HandsModel
{
    IRenderVisual* V{};
    IKinematics* K{};
    xr_vector<Fmatrix> pose;
    VisMask mask;
    bool has_mask{};
    u32 drawn_frame{u32(-1)};
};

struct HandsPlay
{
    int user{-1}; // G / level.replay_hands: -1 the camera's own (on for the take's camera), 0 off, 1 on
    int last_mode{-1}; // the camera they were set for: another camera, the camera's own again
    bool drawn{};
    // the take's camera at the replay's time
    bool cam_ok{}, fp{};
    Fvector cp{}, cd{}, cn{};
    float fov{67.5f}, hud_fov{};
    u16 item{u16(-1)};
    int actor_track{-1};
    struct Draw
    {
        u16 slot;
        Fmatrix local, world;
    };
    xr_vector<Draw> draws;
    xr_vector<HandsModel*> models; // per slot of the take
    HandsRoot* root{};
    float hud_fov_was{};
    bool hud_fov_set{};
    u32 frames_drawn{};
} Hands;

int hands_mode_key() { return P.bc.idx >= 0 ? 100 : (P.path_play ? 101 : int(P.mode)); }

bool hands_want() { return Hands.user >= 0 ? Hands.user == 1 : (P.mode == CAM_TAKE && P.bc.idx < 0 && !P.path_play); }

void hands_fov_restore()
{
    if (Hands.hud_fov_set)
    {
        psHUD_FOV = Hands.hud_fov_was;
        Hands.hud_fov_set = false;
    }
}

void hands_open()
{
    Hands.user = -1;
    Hands.last_mode = -1;
    Hands.drawn = false;
    Hands.cam_ok = false;
    Hands.draws.clear();
    Hands.models.assign(P.take.hands.slots.size(), nullptr);
    Hands.root = xr_new<HandsRoot>();
    Hands.hud_fov_set = false;
    Hands.frames_drawn = 0;
    Hands.actor_track = -1;
    for (u32 i = 0; i < P.take.objs.size(); ++i)
        if (P.take.objs[i].kind == K_ACTOR)
        {
            Hands.actor_track = int(i);
            break;
        }
}

void hands_close()
{
    hands_fov_restore();
    if (Hands.root)
    {
        Hands.root->renderable.visual = nullptr; // a model of ours, not its own
        xr_delete(Hands.root);
    }
    for (HandsModel*& M : Hands.models)
    {
        if (M && M->V)
            ::Render->model_Delete(M->V);
        xr_delete(M);
    }
    Hands.models.clear();
    Hands.draws.clear();
    Hands.drawn = false;
}

HandsModel* hands_model(u16 slot)
{
    if (slot >= Hands.models.size() || slot >= P.take.hands.slots.size())
        return nullptr;
    HandsModel*& M = Hands.models[slot];
    if (M)
        return M->V ? M : nullptr;
    M = xr_new<HandsModel>();
    const HudSlot& s = P.take.hands.slots[slot];
    string_path fn;
    strconcat(sizeof(fn), fn, s.visual.size() ? s.visual.c_str() : "", ".ogf");
    if (!s.visual.size() || !FS.exist("$game_meshes$", fn))
    {
        Msg("! [portal_mod] replay: no HUD model '%s'", fn);
        return nullptr;
    }
    ::Render->hud_loading = true;
    M->V = ::Render->model_Create(s.visual.c_str());
    ::Render->hud_loading = false;
    if (!M->V)
        return nullptr;
    M->K = M->V->dcast_PKinematics();
    if (M->K)
    {
        if (M->K->LL_BoneCount() == s.nbones && s.nbones)
        {
            M->pose.assign(s.nbones, Fidentity);
            for (u16 i = 0; i < s.nbones; ++i)
                M->K->LL_GetBoneInstance(i).set_callback(bctCustom, pose_cb, &M->pose[i], TRUE);
        }
        else
            M->K = nullptr; // another model under that name now: drawn as it is
    }
    if (Hands.root && !Hands.root->renderable.visual)
        Hands.root->renderable.visual = M->V; // the lighting is sampled round the hands
    return M;
}

// the take's camera at t
bool hands_cam_at(double t)
{
    const xr_vector<HudFrame>& F = P.take.hands.frames;
    if (F.empty())
        return false;
    const size_t i = HandsTrack::at(F, t);
    const HudFrame& a = F[i];
    float k = 0.f;
    const HudFrame* b = nullptr;
    if (i + 1 < F.size() && t > a.t)
    {
        const HudFrame& c = F[i + 1];
        if (c.t > a.t && c.t - a.t < 0.25 && a.cam_p.distance_to_sqr(c.cam_p) < TELEPORT * TELEPORT)
        {
            b = &c;
            k = float((t - a.t) / (c.t - a.t));
            clamp(k, 0.f, 1.f);
        }
    }
    Hands.cp = a.cam_p;
    Hands.cd = a.cam_d;
    Hands.cn = a.cam_n;
    Hands.fov = a.fov;
    Hands.hud_fov = a.hud_fov;
    if (b)
    {
        Hands.cp.lerp(a.cam_p, b->cam_p, k);
        Hands.cd.lerp(a.cam_d, b->cam_d, k);
        Hands.cn.lerp(a.cam_n, b->cam_n, k);
        Hands.fov = a.fov + (b->fov - a.fov) * k;
        Hands.hud_fov = a.hud_fov + (b->hud_fov - a.hud_fov) * k;
    }
    Hands.cd.normalize_safe();
    Hands.cn.normalize_safe();
    Hands.fp = (a.flags & HF_FP) != 0;
    Hands.item = a.item;
    return _valid(Hands.cp) && _valid(Hands.cd) && Hands.cd.square_magnitude() > 0.5f;
}

bool hands_same_layout(const HudFrame& a, const HudFrame& b)
{
    if (a.inst.size() != b.inst.size() || a.draws.size() != b.draws.size())
        return false;
    for (size_t n = 0; n < a.inst.size(); ++n)
        if (a.inst[n].slot != b.inst[n].slot || a.inst[n].nbones != b.inst[n].nbones)
            return false;
    for (size_t n = 0; n < a.draws.size(); ++n)
        if (a.draws[n].inst != b.draws[n].inst)
            return false;
    return true;
}

// player_frame, before the puppets: the take's camera, and the hands posed for the frame
void hands_update(double t)
{
    const int mk = hands_mode_key();
    if (mk != Hands.last_mode)
    {
        Hands.last_mode = mk;
        Hands.user = -1; // another camera: its own default
    }
    Hands.drawn = false;
    Hands.draws.clear();
    Hands.cam_ok = hands_cam_at(t);
    if (!Hands.cam_ok || !hands_want())
    {
        hands_fov_restore();
        return;
    }
    const xr_vector<HudFrame>& F = P.take.hands.frames;
    const size_t i = HandsTrack::at(F, t);
    const HudFrame& a = F[i];
    if (!(a.flags & HF_HUD) || a.draws.empty())
    {
        hands_fov_restore();
        return;
    }
    const HudFrame* b = nullptr;
    float k = 0.f;
    if (i + 1 < F.size() && t > a.t)
    {
        const HudFrame& c = F[i + 1];
        if (c.t > a.t && c.t - a.t < 0.25 && (c.flags & HF_HUD) && hands_same_layout(a, c))
        {
            b = &c;
            k = float((t - a.t) / (c.t - a.t));
            clamp(k, 0.f, 1.f);
        }
    }
    for (size_t n = 0; n < a.inst.size(); ++n)
    {
        const HudInst& ia = a.inst[n];
        HandsModel* M = hands_model(ia.slot);
        if (!M || !M->K || ia.nbones != M->K->LL_BoneCount())
            continue;
        const BoneQ* ba = &a.bones[ia.bone0];
        const BoneQ* bb = b ? &b->bones[b->inst[n].bone0] : ba;
        for (u16 q = 0; q < ia.nbones; ++q)
            bone_lerp(ba[q], bb[q], b ? k : 0.f, M->pose[q]);
        if (!a.scl.empty() || (b && !b->scl.empty())) // the bones drawn scaled (E26's folded arm)
        {
            const auto scale_of = [](const HudFrame& f, u32 bone) {
                for (const HudScale& x : f.scl)
                    if (x.bone == bone)
                        return x.s;
                return 1.f;
            };
            for (u16 q = 0; q < ia.nbones; ++q)
            {
                const float sa = scale_of(a, ia.bone0 + q);
                float s = sa;
                if (b)
                {
                    const float sb = scale_of(*b, b->inst[n].bone0 + q);
                    s = (sa < 0.01f || sb < 0.01f) ? (k < 0.5f ? sa : sb) : sa + (sb - sa) * k; // folding: no half-folded arm
                }
                if (_abs(s - 1.f) > 1e-4f)
                {
                    M->pose[q].i.mul(s);
                    M->pose[q].j.mul(s);
                    M->pose[q].k.mul(s);
                }
            }
        }
        VisMask m;
        m.set(ia.vis[0], ia.vis[1]);
        if (!M->has_mask || M->mask != m)
        {
            VisMask mm = m;
            mm.set(M->K->LL_GetBoneRoot(), true); // the root stays on, or nothing is calculated
            M->K->LL_SetBonesVisible(mm);
            M->mask = m;
            M->has_mask = true;
        }
        M->K->CalculateBones_Invalidate();
        M->K->CalculateBones(TRUE);
    }
    for (size_t n = 0; n < a.draws.size(); ++n)
    {
        const HudDraw& d = a.draws[n];
        const u16 slot = a.inst[d.inst].slot;
        if (!hands_model(slot))
            continue;
        HandsPlay::Draw w{};
        w.slot = slot;
        if (b)
            hands_xf_lerp(d.xf, b->draws[n].xf, k, w.local);
        else
            w.local = d.xf;
        Hands.draws.push_back(w);
    }
    Hands.drawn = !Hands.draws.empty();
    if (!Hands.drawn)
    {
        hands_fov_restore();
        return;
    }
    // the HUD's own FOV as it was drawn (the gun's place on the screen depends on it)
    if (!Hands.hud_fov_set)
    {
        Hands.hud_fov_was = psHUD_FOV;
        Hands.hud_fov_set = true;
    }
    psHUD_FOV = std::max(Hands.hud_fov, 1.01f);
    ++Hands.frames_drawn;
}

// CHUDManager::Render_MAIN in a replay: the hands on the camera there is
void hands_render(u32 context_id)
{
    if (!Hands.drawn || !Hands.root)
        return;
    Fmatrix Rn;
    hands_basis(Rn, Device.vCameraDirection, Device.vCameraTop);
    Hands.root->renderable.xform.translate(Device.vCameraPosition);
    Hands.root->renderable_HUD(true);
    for (HandsPlay::Draw& w : Hands.draws)
    {
        HandsModel* M = w.slot < Hands.models.size() ? Hands.models[w.slot] : nullptr;
        if (!M || !M->V)
            continue;
        w.world.mul_43(Rn, w.local);
        const bool again = M->drawn_frame == Device.dwFrame;
        M->drawn_frame = Device.dwFrame;
        g_r_dsgraph_dup_ok = again; // E26b: the same model once more in the pass (the mirrored gun)
        ::Render->add_Visual(context_id, Hands.root, M->V, w.world);
        g_r_dsgraph_dup_ok = false;
    }
    Hands.root->renderable_HUD(false);
}

// puppets_update: on the take's camera in the player's head his body and what he holds are not drawn (as with V)
u16 hands_hide_id()
{
    if (P.mode != CAM_TAKE || !Hands.cam_ok || !Hands.fp || Hands.actor_track < 0 || Hands.actor_track >= int(P.take.objs.size()))
        return u16(-1);
    return P.take.objs[Hands.actor_track].id;
}

// camera_frame: the take's own camera, as the game drew it (the effectors' roll in its top, the zoom in its FOV)
bool hands_take_camera()
{
    if (!Hands.cam_ok)
        return false;
    Fvector d = Hands.cd, n = Hands.cn, r;
    d.normalize_safe();
    r.crossproduct(n, d).normalize_safe();
    n.crossproduct(d, r).normalize_safe();
    float fov = Hands.fov;
    clamp(fov, 1.f, 150.f);
    Device.mView.build_camera_dir(Hands.cp, d, n);
    Device.vCameraPosition.set(Hands.cp);
    Device.vCameraDirection.set(d);
    Device.vCameraTop.set(n);
    Device.vCameraRight.set(r);
    Device.fFOV = fov;
    Device.fASPECT = Device.fHeight_2 / Device.fWidth_2;
    float far_plane = 300.f;
    if (g_pGamePersistent && g_pGamePersistent->Environment().CurrentEnv)
        far_plane = g_pGamePersistent->Environment().CurrentEnv->far_plane;
    Device.mProject.build_projection(deg2rad(fov), Device.fASPECT, VIEWPORT_NEAR, far_plane);
    CCameraManager::ResetPP();
    P.cam_p = Hands.cp; // leaving it (the mouse, a key) starts the free camera here
    d.getHP(P.h, P.pitch);
    return true;
}

bool hands_map_at(double t, Fmatrix& m)
{
    const xr_vector<HudMap>& M = P.take.hands.maps;
    if (M.empty())
        return false;
    const HudMap& k = M[HandsTrack::at(M, t + 1e-6)];
    if (k.t > t + 0.5)
        return false;
    m = k.m;
    return true;
}

// a particle key of the HUD back from the world model's muzzle to the HUD's, by the map of its own sample
bool hands_key_back(const PsKey& k, Fmatrix& parent, Fmatrix& rx, Fvector& vel)
{
    Fmatrix m, back;
    if (!hands_map_at(k.t, m))
        return false;
    back.invert(m);
    parent.mul_43(back, k.parent);
    rx.mul_43(back, k.rx);
    back.transform_dir(vel, k.vel);
    return true;
}

// ps_place: with the hands drawn a HUD system (a shot's flame and smoke, a shell) goes back from the world model's muzzle
// to the HUD's and is drawn as the HUD draws it (its projection); without them it stays on the world model. Each key goes
// back by its own sample's map before the two round t are blended: with two pistols (E27) the live model changes hands
// at every shot and the map jumps from one hand to the other between two samples.
void hands_ps(double t, const PsTrack& tr, ReplayPS* p, Fmatrix& parent, Fmatrix& rx, Fvector& vel, u32 fl)
{
    bool hud = (fl & PS_HUD) && Hands.drawn && !tr.keys.empty() && !P.take.hands.maps.empty();
    Fmatrix pa, ra, pb, rb;
    Fvector va, vb;
    const size_t i = hud ? key_at(tr.keys, t) : 0;
    if (hud)
        hud = hands_key_back(tr.keys[i], pa, ra, va);
    if (p && p->V && !!p->V->GetHudMode() != hud)
        p->V->SetHudMode(hud ? TRUE : FALSE);
    if (!hud)
        return;
    const PsKey& a = tr.keys[i];
    if (i + 1 < tr.keys.size() && t > a.t && tr.keys[i + 1].t > a.t && hands_key_back(tr.keys[i + 1], pb, rb, vb))
    {
        float k = float((t - a.t) / (tr.keys[i + 1].t - a.t));
        clamp(k, 0.f, 1.f);
        pa.c.lerp(pa.c, pb.c, k);
        ra.c.lerp(ra.c, rb.c, k);
        va.lerp(va, vb, k);
    }
    parent = pa;
    rx = ra;
    vel = va;
    if (P.mode != CAM_TAKE && Hands.cam_ok)
    {
        // the hands on another camera: the HUD's world moved with it
        Fmatrix crec, cnow, crec_inv, ret, tmp;
        hands_basis(crec, Hands.cd, Hands.cn);
        crec.c = Hands.cp;
        hands_basis(cnow, Device.vCameraDirection, Device.vCameraTop);
        cnow.c = Device.vCameraPosition;
        crec_inv.invert(crec);
        ret.mul_43(cnow, crec_inv);
        tmp.mul_43(ret, parent);
        parent = tmp;
        tmp.mul_43(ret, rx);
        rx = tmp;
        ret.transform_dir(tmp.c, vel);
        vel = tmp.c;
    }
}
