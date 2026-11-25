// portal-mod E76: the T-800's machine vision (the T-800 series, 2026-11-26). A key (J, pm_vision.ltx) turns it on and off
// in the game and in a replay; My Mods' "vision" switch turns the whole feature off. On, every frame:
//   - the picture is graded as the user's After Effects HUD grades it (the red tritone, scan lines, glow, grain,
//     chromatic aberration, a vignette) and glitches on a hit, a lock and the boot (Layers/xrRenderPC_R4/
//     portal_mod_vision_r4.cpp, shaders pm_vision_*);
//   - people and mutants glow by their body heat (the G-buffer's heat target: depth-tested, so only what is in sight -
//     no wallhack), the dead cool down; an invisible bloodsucker shows as a heat shape (its geometry stamped into the
//     heat buffer, depth-tested too);
//   - every creature in sight within range (rays from the camera to its head, chest and pelvis against the level) gets
//     brackets that follow its bones (the box round all of them, projected each frame) and spring onto it with an
//     overshoot, its number and distance and a health bar; the priority target (the locked one, else the biggest
//     threat) gets a card typed out with the game's own data - name, faction, rank, weapon, health, distance, threat;
//     the one under the crosshair is locked after a moment (a reticle closes on its head, ЗАХВАТ); a kill stamps
//     ЛИКВИДИРОВАН; an event log, a hex stream, a data column, a compass tape and the telemetry run at the edges.
// The same in a replay from any camera: the puppets are read as the live creatures are (their alive flag and health
// recorded by the replay since E76). Lua (the bot's API, README): level.vision_switch(on) (My Mods), vision_on(on),
// vision_toggle(), vision_active(), vision_targets() (a table), vision_info(), vision_tune(name, value),
// vision_export(mode) (0 normal, 1 the HUD layer alone over black, 2 the picture without the HUD).
#pragma once

struct lua_State;

namespace pm_vision
{
void before_render(); // Level.cpp, CLevel::OnRender before Render->Calculate(): fills portal_mod_vision (xr_3da)
bool key_press(int dik); // Level_input.cpp, CLevel::IR_OnKeyboardPress before the replay's keys: the toggle key
} // namespace pm_vision

void pm_vision_script_register(lua_State* L); // level.vision_* (level_script.cpp calls it)
