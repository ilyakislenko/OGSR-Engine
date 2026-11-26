// portal-mod E53: a replay for filming (tools/engine_patches/patch_e53.py).
// The game records the state of the world - every object's matrix and bones, particles, sounds, lights, bullets,
// portals, the time of day and the weather - and plays it back with the world frozen, from any camera: a free
// camera, a spline through key points, looking at an object or through a character's eyes. No input is replayed:
// X-Ray's AI and physics are not deterministic, so the replay shows what was drawn, as "puppets" (copies of the
// objects' models posed bone by bone from the record).
#pragma once

struct SBullet;
class IKinematics; // portal-mod E76
class IRenderable; // portal-mod E76
class IRenderVisual; // portal-mod E53d

namespace pm_replay
{
// CLevel::OnFrame, at its end: records a sample, or plays the open replay (Lua tick, scene, camera)
void on_frame();
// CLevel::OnFrame: the environment's day time (seconds) while a replay is open
bool env_time(float& sec);
// the level goes (CLevel::~CLevel): the replay closes, the recorder drops its buffer
void shutdown();

// CLevel::IR_On*: true when the replay took the input
bool key_press(int dik);
bool key_release(int dik);
bool key_hold(int dik);
bool mouse_move(int dx, int dy);
bool mouse_wheel(int dir);

// CHUDManager::RenderUI: no game HUD over a replay; the hint and the REC indicator
bool clean_frame();
void render_ui();

// CBulletManager::Render: the replay's tracers and bullets (nullptr when no replay is open)
struct Tracer
{
    Fvector pos, dir;
    float speed, portal_dist;
    u8 color;
    // portal-mod E53b: a bullet in slow motion, drawn as given: length, width, a colour (argb; 0: the tracer colour
    // `color`), with or without the round head
    u8 slug{}, head{};
    float length{}, width{};
    u32 argb{};
};
const xr_vector<Tracer>* tracers();

// portal-mod E53b: the bullets' paths (CBulletManager): a new bullet (main thread; returns its track's id, 0 when not
// recording) and the points of its path (the worker thread too): `tau` is the bullet's own flight time in ms
constexpr u32 BP_END = 1, BP_JUMP_FROM = 2, BP_JUMP_TO = 4;
// portal-mod E53c: and the cartridge's section (its model in the replay's bullet camera)
u32 bullet_born(const SBullet& b, u16 sender_id, u16 weapon_id, LPCSTR ammo = nullptr);
void bullet_point(u32 id, float tau, const Fvector& pos, float speed, u32 flags);

// Lua (level.replay_*; level_script.cpp)
LPCSTR lua_record(bool on);
bool lua_recording();
void lua_ring(float seconds);
LPCSTR lua_save_ring(LPCSTR name);
bool lua_open_take(LPCSTR name);
void lua_close();
bool lua_active();
// portal-mod E76: the open replay's creatures for the machine vision: id, bones, the puppet, its matrix, its key's
// pad (0x8000 recorded, 0x4000 alive, the low bits health x 100), its model
struct VisionPuppet
{
    u16 id;
    IKinematics* K;
    IRenderable* R;
    Fmatrix xform;
    u16 pad;
    shared_str visual;
};
void vision_puppets(xr_vector<VisionPuppet>& out);
float lua_time();
float lua_length();
void lua_seek(float t);
void lua_speed(float k);
void lua_pause(bool on);
void lua_cam(Fvector pos, Fvector dir, float fov, float roll);
Fvector lua_cam_pos();
Fvector lua_cam_dir();
void lua_look_at(int id);
void lua_eye(int id);
void lua_key_add(float u, float t, Fvector pos, Fvector dir, float fov, float roll);
void lua_key_clear();
int lua_key_count();
void lua_path_play(bool on);
bool lua_path_playing();
bool lua_path_save(int slot);
bool lua_path_load(int slot);
void lua_hint(int level);
float lua_clock();
LPCSTR lua_info();
Fvector lua_object_pos(int id);
LPCSTR lua_last();
// portal-mod E53c: the bullet camera - follows the bullet whose hit was nearest t (< 0: off); 0 off, 1 in flight, 2 at
// its end
bool lua_bullet_cam(float t);
// ... of those that ended within 0.4 s of t, the one whose way passed nearest pos (within 4 m; else as above)
bool lua_bullet_cam_at(float t, Fvector pos);
int lua_bullet_cam_state();
// portal-mod E53d: the first-person view of a take. CHUDManager::Render_MAIN: the recorder takes the camera as drawn and,
// between begin and end, every model player_hud draws (player_hud.cpp: pm_hud_add); in a replay hud_render draws the
// replay's hands instead of the game's HUD (true: a replay is open)
void hud_capture_begin();
void hud_capture_draw(IRenderVisual* v, const Fmatrix& m);
void hud_capture_end();
bool hud_render(u32 context_id);
// level.replay_take_cam(on): the take's own camera, as the game drew it (false: the take has none); replay_hands(on) /
// replay_hands_default(): the hands on this camera (by default: on on the take's camera only, off on every other);
// replay_hands_state(): the models drawn now (0: no hands)
bool lua_take_cam(bool on);
void lua_hands(bool on);
void lua_hands_default();
int lua_hands_state();
} // namespace pm_replay
