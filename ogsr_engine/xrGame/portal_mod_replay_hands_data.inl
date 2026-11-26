// portal-mod E53d: the first-person view of a take - its data. tools/engine_patches/patch_e53d.py copies this file into
// ogsr_engine/xrGame; portal_mod_replay.cpp includes it in its unnamed namespace, right before the Take. The recorder and
// the player are in portal_mod_replay_hands.inl.
//
// What is recorded is what the HUD drew, not how it got there: every frame the camera as drawn (the effectors - recoil,
// bob, a landing - in it, the zoom in its FOV) and every model the HUD handed the renderer (the hands, the gun, E26's
// mirrored second gun, E27's own copy for the other hand), each with the pose of every bone and where it was drawn. Any
// motion (idle, fire, reload, draw, aim in and out, sprint), any blend, script layer or E70 left-arm turn comes back
// exactly so, without the HUD's state machine; the active item's section and HUD section are kept with it.

constexpr u32 PS_HUD = 4; // a particle system of the HUD (the flame, the smoke, a shell): recorded moved onto the world model

// a model the HUD drew in the take: its file, its bones
struct HudSlot
{
    shared_str visual;
    u16 nbones{};
};

// a model as drawn in a frame: its slot, the bones shown, its pose (nbones of the frame's `bones` from bone0)
struct HudInst
{
    u16 slot;
    u16 nbones;
    u64 vis[2];
    u32 bone0;
};

// one draw: which model (the frame's inst) and where, in the camera's own frame (x right, y up, z forward, the origin
// at the eye) - the HUD is drawn so, with the eye at the origin; a mirror image (E26's left gun) has a negative
// determinant
struct HudDraw
{
    u16 inst;
    u16 flags;
    Fmatrix xf;
};

// a bone drawn scaled (BoneQ keeps none): E26 folds the left arm of the gun in the other hand away by its clavicle at
// 1/10000, the whole arm with it
struct HudScale
{
    u32 bone; // the frame's bones
    float s;
};

constexpr u8 HF_FP = 1; // the player's own eyes (in the replay's take camera his body and what he holds are hidden)
constexpr u8 HF_HUD = 2; // the HUD was drawn: the draws

struct HudFrame
{
    double t{};
    Fvector cam_p{}, cam_d{}, cam_n{}; // Device's camera as drawn: position, direction, top
    float fov{}, hud_fov{}; // Device.fFOV and the HUD's own FOV, degrees
    u8 flags{};
    u16 item{u16(-1)}; // the active item: HandsTrack::items ("section|hud section")
    xr_vector<HudInst> inst;
    xr_vector<HudDraw> draws;
    xr_vector<BoneQ> bones;
    xr_vector<HudScale> scl; // the bones drawn scaled
};

// E53b records a first-person shot's flame and smoke moved from the HUD's muzzle to the world model's (the world model's
// muzzle frame x inverse(the HUD's)); with the hands drawn again they go back
struct HudMap
{
    double t;
    Fmatrix m;
};

struct HandsTrack
{
    xr_vector<HudSlot> slots;
    xr_vector<shared_str> items;
    xr_vector<HudFrame> frames;
    xr_vector<HudMap> maps;

    void clear()
    {
        slots.clear();
        items.clear();
        frames.clear();
        maps.clear();
    }

    template <typename T>
    static size_t at(const xr_vector<T>& v, double t) // the last with .t <= t (0 when all are later)
    {
        const auto it = std::upper_bound(v.begin(), v.end(), t, [](double x, const T& k) { return x < k.t; });
        return it == v.begin() ? 0 : size_t(it - v.begin()) - 1;
    }

    // drops what is older than cut, keeping the last frame at or before it
    void trim(double cut)
    {
        if (!frames.empty())
            if (const size_t n = at(frames, cut))
                frames.erase(frames.begin(), frames.begin() + n);
        if (!maps.empty())
            if (const size_t n = at(maps, cut))
                maps.erase(maps.begin(), maps.begin() + n);
    }

    // [a, b] of S with its times from 0 (the frame before a: at 0)
    void extract(const HandsTrack& S, double a, double b)
    {
        clear();
        slots = S.slots;
        items = S.items;
        if (!S.frames.empty())
            for (size_t i = at(S.frames, a); i < S.frames.size() && S.frames[i].t <= b + 1e-6; ++i)
            {
                HudFrame f = S.frames[i];
                f.t = std::max(0.0, f.t - a);
                if (!frames.empty() && f.t <= frames.back().t)
                    frames.pop_back(); // the frame before the start and one at it: the later one
                frames.push_back(std::move(f));
            }
        if (!S.maps.empty())
            for (size_t i = at(S.maps, a); i < S.maps.size() && S.maps[i].t <= b + 1e-6; ++i)
            {
                HudMap m = S.maps[i];
                m.t = std::max(0.0, m.t - a);
                if (!maps.empty() && m.t <= maps.back().t)
                    maps.pop_back();
                maps.push_back(m);
            }
    }

    template <typename T>
    static void wv(IWriter* W, const xr_vector<T>& v)
    {
        W->w_u32(u32(v.size()));
        if (!v.empty())
            W->w(v.data(), v.size() * sizeof(T));
    }
    template <typename T>
    static bool rv(IReader* F, xr_vector<T>& v)
    {
        if (F->elapsed() < sizeof(u32))
            return false;
        const u32 n = F->r_u32();
        if (u64(n) * sizeof(T) > u64(F->elapsed()))
            return false;
        v.resize(n);
        if (n)
            F->r(v.data(), n * sizeof(T));
        return true;
    }

    void save(IWriter* W) const
    {
        W->w_u32(u32(slots.size()));
        for (const HudSlot& s : slots)
        {
            W->w_stringZ(s.visual.size() ? s.visual.c_str() : "");
            W->w_u16(s.nbones);
        }
        W->w_u32(u32(items.size()));
        for (const shared_str& s : items)
            W->w_stringZ(s.size() ? s.c_str() : "");
        W->w_u32(u32(frames.size()));
        for (const HudFrame& f : frames)
        {
            W->w(&f.t, sizeof(double));
            W->w_fvector3(f.cam_p);
            W->w_fvector3(f.cam_d);
            W->w_fvector3(f.cam_n);
            W->w_float(f.fov);
            W->w_float(f.hud_fov);
            W->w_u8(f.flags);
            W->w_u16(f.item);
            wv(W, f.inst);
            wv(W, f.draws);
            wv(W, f.bones);
            wv(W, f.scl);
        }
        wv(W, maps);
    }

    bool load(IReader* F)
    {
        clear();
        if (F->elapsed() < sizeof(u32))
            return false;
        const u32 ns = F->r_u32();
        for (u32 i = 0; i < ns; ++i)
        {
            if (F->elapsed() < 3)
                return false;
            HudSlot s;
            F->r_stringZ(s.visual);
            s.nbones = F->r_u16();
            slots.push_back(s);
        }
        if (F->elapsed() < sizeof(u32))
            return false;
        const u32 ni = F->r_u32();
        for (u32 i = 0; i < ni; ++i)
        {
            if (F->elapsed() < 1)
                return false;
            shared_str s;
            F->r_stringZ(s);
            items.push_back(s);
        }
        if (F->elapsed() < sizeof(u32))
            return false;
        const u32 nf = F->r_u32();
        constexpr size_t head = sizeof(double) + sizeof(Fvector) * 3 + sizeof(float) * 2 + 1 + 2;
        for (u32 i = 0; i < nf; ++i)
        {
            if (F->elapsed() < head)
                return false;
            HudFrame f;
            F->r(&f.t, sizeof(double));
            F->r_fvector3(f.cam_p);
            F->r_fvector3(f.cam_d);
            F->r_fvector3(f.cam_n);
            f.fov = F->r_float();
            f.hud_fov = F->r_float();
            f.flags = F->r_u8();
            f.item = F->r_u16();
            if (!rv(F, f.inst) || !rv(F, f.draws) || !rv(F, f.bones) || !rv(F, f.scl))
                return false;
            for (const HudScale& x : f.scl)
                if (x.bone >= f.bones.size() || !_valid(x.s))
                {
                    f.scl.clear();
                    break;
                }
            for (const HudInst& n : f.inst) // a broken frame: no draws
                if (n.slot >= slots.size() || size_t(n.bone0) + n.nbones > f.bones.size())
                {
                    f.inst.clear();
                    f.draws.clear();
                    f.flags &= ~HF_HUD;
                    break;
                }
            for (const HudDraw& d : f.draws)
                if (d.inst >= f.inst.size())
                {
                    f.draws.clear();
                    f.flags &= ~HF_HUD;
                    break;
                }
            frames.push_back(std::move(f));
        }
        return rv(F, maps);
    }
};

// E53d: a take's channels after its fixed part (take version 4): a count, then each as a four-letter tag, its size and its
// bytes; a reader skips a tag it does not know, so another channel can be added without a new version.
// "HND1": HandsTrack (the first-person view).

// the recorder's hooks (portal_mod_replay_hands.inl, included after the Player): recorder_frame comes before it
struct Take;
void hands_rec_sample(Take& T, double t);
void hands_rec_map(Take& T, double t, const Fmatrix& m);
