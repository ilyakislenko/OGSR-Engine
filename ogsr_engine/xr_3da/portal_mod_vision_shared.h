// portal-mod E76: the T-800's machine vision - what the game (xrGame/portal_mod_vision.cpp) hands the renderer
// (Layers/xrRenderPC_R4/portal_mod_vision_r4.cpp) each frame. Defined in xr_3da/Render.cpp; included only by the
// files that use it (not render.h), so the patch rebuilds only them.
//
// The game fills it in CLevel::OnRender, before Render->Render(): the HUD layer as quads in screen pixels (glyphs of the
// font atlas textures\portal_mod\pm_vision_font.dds and bars / lines on its white block), the creatures to stamp into
// the heat buffer by their geometry (an invisible bloodsucker: its model is a distortion only and writes no heat of its
// own), the look's parameters. The renderer draws the HUD into its own target, blurs it and the heat buffer for the
// glow, and grades the frame (phase_combine, before phase_pp).
#pragma once

class IRenderable;

struct portal_mod_vision_vert
{
    float x, y; // screen pixels (the device's size)
    float u, v; // in the font atlas
    u32 c; // color_rgba, straight alpha
};

struct portal_mod_vision_frame
{
    bool on{}; // draw the pass this frame
    float fade{}; // 0..1: the grade's strength (the switch-on and switch-off ramps)
    float glitch{}; // 0..1: digital damage (a hit, a lock, the boot)
    float seed{}; // the glitch's random, changes every few frames
    float time{}; // s, for the scan bar and the grain
    float hit{}; // 0..1: the red pulse of a hit
    float heat{1.f}; // thermal strength
    float scan{0.22f}, grain{0.12f}, ca{1.f}, vignette{0.35f}, glow{1.6f}, tint{0.66f};
    float heat_range{140.f}; // m: the heat fades out to here
    float dim{}; // 0..1: the world darkened under the heat (the heat mode)
    float walls{}; // 0..1: the silhouettes of the living through walls (the walls mode)
    u32 hud_only{}; // 1: the frame is the HUD layer alone over black, its alpha kept (the export)
    u32 no_hud{}; // 1: the frame is graded with no HUD (the plate for the export)
    xr_vector<portal_mod_vision_vert> verts; // quads, 4 a piece in strip order: (x0,y1) (x0,y0) (x1,y1) (x1,y0)
    xr_vector<IRenderable*> ghosts; // stamped into the heat buffer by their own geometry
    xr_vector<std::pair<IRenderable*, float>> wall_bodies; // the walls mode: into the heat buffer's blue with no depth
                                                           // test (the float: nearness 0..1)
};

ENGINE_API extern portal_mod_vision_frame portal_mod_vision;
