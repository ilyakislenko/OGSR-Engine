// portal-mod E76: the T-800's machine vision, the renderer's side (see xr_3da/portal_mod_vision_shared.h). Called from
// CRenderTarget::phase_combine after the lens flares, before phase_pp writes the frame out:
//   1. the ghosts (an invisible bloodsucker) into the heat buffer by their own geometry, depth-tested against the scene
//      without writing depth: a wall in front hides them as it hides everyone else (no wallhack);
//      the walls mode only (the user asked for it): the living within its range into the heat buffer's blue, which the
//      G-buffer leaves at 0, with no depth test - the grade draws a silhouette where a body is but no heat is seen;
//   2. the HUD layer: the game's quads into $user$pm_vhud (premultiplied RGBA, cleared transparent);
//   3. the glow: the HUD layer and the heat buffer down to a quarter ($user$pm_vq0: rgb the HUD, a the heat), blurred
//      twice each way;
//   4. the grade (pm_vision_comp.ps): the red tritone, heat, scan lines, grain, chromatic aberration, vignette, the
//      glitch, the HUD with its glow over it - into rt_Generic_combine, copied back into rt_Generic_0.
// The targets and shaders are made on first use and let go with the render target (a device reset makes them again).
#include "stdafx.h"
#include "../../xr_3da/portal_mod_vision_shared.h"
#include "../xrRender/FHierrarhyVisual.h"
#include "../../Include/xrRender/Kinematics.h"

namespace
{
ref_rt pm_rt_hud, pm_rt_q0, pm_rt_q1;
ref_shader pm_s_hud, pm_s_down, pm_s_blur, pm_s_comp;
ref_shader pm_s_ghost[6]; // skinning -1 .. 4 (the model's own vertex format)
ref_shader pm_s_wall[6];
ref_geom pm_g_tl;
u32 pm_w{}, pm_h{};
bool pm_failed{};

// a skinned mesh's skinning mode, from the vertex shader its own shader was made with ("..._N" for SKIN_N)
int skin_mode(dxRender_Visual* v)
{
    if (!v || !v->shader._get())
        return -1;
    for (u32 e = 0; e < SHADER_ELEMENTS_MAX; ++e)
    {
        ShaderElement* E = v->shader->E[e]._get();
        if (!E || E->passes.empty() || !E->passes[0]._get() || !E->passes[0]->vs._get())
            continue;
        LPCSTR n = E->passes[0]->vs->cName.c_str();
        const size_t l = n ? xr_strlen(n) : 0;
        if (l > 2 && n[l - 2] == '_' && n[l - 1] >= '0' && n[l - 1] <= '4')
            return n[l - 1] - '0';
        return -1;
    }
    return -1;
}
} // namespace

void CRenderTarget::pm_vision_free()
{
    pm_s_comp = nullptr;
    pm_s_blur = nullptr;
    pm_s_down = nullptr;
    pm_s_hud = nullptr;
    for (ref_shader& s : pm_s_ghost)
        s = nullptr;
    for (ref_shader& s : pm_s_wall)
        s = nullptr;
    pm_g_tl = nullptr;
    pm_rt_q1 = nullptr;
    pm_rt_q0 = nullptr;
    pm_rt_hud = nullptr;
    pm_w = pm_h = 0;
}

void CRenderTarget::phase_pm_vision(CBackend& cmd_list)
{
    portal_mod_vision_frame& V = portal_mod_vision;
    if (!V.on || pm_failed)
        return;
    PIX_EVENT(portal_mod_vision);

    const u32 w = Device.dwWidth, h = Device.dwHeight;
    const u32 qw = std::max(1u, w / 4), qh = std::max(1u, h / 4);
    if (!pm_rt_hud || pm_w != w || pm_h != h)
    {
        pm_vision_free();
        pm_rt_hud.create("$user$pm_vhud", w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        pm_rt_q0.create("$user$pm_vq0", qw, qh, DXGI_FORMAT_R8G8B8A8_UNORM);
        pm_rt_q1.create("$user$pm_vq1", qw, qh, DXGI_FORMAT_R8G8B8A8_UNORM);
        pm_s_hud.create("pm_vision_hud");
        pm_s_down.create("pm_vision_down");
        pm_s_blur.create("pm_vision_blur");
        pm_s_comp.create("pm_vision_comp");
        for (int i = 0; i < 6; ++i)
        {
            RImplementation.shader_option_skinning(i - 1);
            pm_s_ghost[i].create("pm_vision_ghost");
            pm_s_wall[i].create("pm_vision_wall");
        }
        RImplementation.shader_option_skinning(-1);
        pm_g_tl.create(FVF::F_TL, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        pm_w = w;
        pm_h = h;
        if (!pm_rt_hud || !pm_rt_q0 || !pm_rt_q1 || !pm_s_comp || !pm_s_comp->E[0])
        {
            Msg("! E76 vision: targets or shaders not made, the pass is off");
            pm_failed = true;
            return;
        }
        Msg("* E76 vision: targets %ux%u, glow %ux%u", w, h, qw, qh);
    }

    // 1. the ghosts into the heat buffer (x hot, y "transparent": the grade draws them as a heat shape)
    if (!V.ghosts.empty())
    {
        PIX_EVENT(pm_vision_ghosts);
        u_setrt(cmd_list, rt_heat, nullptr, nullptr, nullptr, rt_Base_Depth->pZRT[cmd_list.context_id]);
        cmd_list.set_CullMode(CULL_CCW);
        cmd_list.set_Stencil(FALSE);
        cmd_list.set_ColorWriteEnable();
        for (IRenderable* R : V.ghosts)
        {
            if (!R || !R->renderable.visual)
                continue;
            dxRender_Visual* vis = static_cast<dxRender_Visual*>(R->renderable.visual);
            FHierrarhyVisual* H = dynamic_cast<FHierrarhyVisual*>(vis);
            if (!H)
                continue;
            if (IKinematics* K = R->renderable.visual->dcast_PKinematics())
                K->CalculateBones(TRUE);
            for (dxRender_Visual* c : H->children)
            {
                if (!c)
                    continue;
                ref_shader& s = pm_s_ghost[skin_mode(c) + 1];
                if (!s || !s->E[0])
                    continue;
                cmd_list.set_xform_world(R->renderable.xform);
                cmd_list.set_Element(s->E[0]);
                c->Render(cmd_list, 1.f, false);
            }
        }
    }

    // 1b. the walls mode: bodies through walls into the heat buffer's blue (the colour write keeps x and y: the heat seen)
    if (!V.wall_bodies.empty() && V.walls > 0.f)
    {
        PIX_EVENT(pm_vision_walls);
        u_setrt(cmd_list, rt_heat, nullptr, nullptr, nullptr, rt_Base_Depth->pZRT[cmd_list.context_id]);
        cmd_list.set_CullMode(CULL_CCW);
        cmd_list.set_Stencil(FALSE);
        for (const auto& [R, near_k] : V.wall_bodies)
        {
            if (!R || !R->renderable.visual)
                continue;
            dxRender_Visual* vis = static_cast<dxRender_Visual*>(R->renderable.visual);
            FHierrarhyVisual* H = dynamic_cast<FHierrarhyVisual*>(vis);
            if (!H)
                continue;
            if (IKinematics* K = R->renderable.visual->dcast_PKinematics())
                K->CalculateBones(TRUE);
            for (dxRender_Visual* c : H->children)
            {
                if (!c)
                    continue;
                ref_shader& s = pm_s_wall[skin_mode(c) + 1];
                if (!s || !s->E[0])
                    continue;
                cmd_list.set_xform_world(R->renderable.xform);
                cmd_list.set_Element(s->E[0]);
                cmd_list.set_ColorWriteEnable(D3DCOLORWRITEENABLE_BLUE);
                cmd_list.set_c("pm_vwall", near_k, 0.f, 0.f, 0.f);
                c->Render(cmd_list, 1.f, false);
            }
        }
        cmd_list.set_ColorWriteEnable();
    }

    // 2. the HUD layer
    {
        PIX_EVENT(pm_vision_hud);
        u_setrt(cmd_list, w, h, pm_rt_hud->pRT, nullptr, nullptr, nullptr);
        cmd_list.ClearRT(pm_rt_hud, Fcolor{0.f, 0.f, 0.f, 0.f});
        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_Stencil(FALSE);
        const u32 n = u32(V.verts.size()) & ~3u;
        if (n && pm_s_hud && pm_s_hud->E[0])
        {
            cmd_list.set_Element(pm_s_hud->E[0]);
            constexpr u32 batch = 4 * 4000; // the quad index buffer holds 4096 quads
            for (u32 i = 0; i < n; i += batch)
            {
                const u32 cnt = std::min(batch, n - i);
                u32 off = 0;
                FVF::TL* pv = static_cast<FVF::TL*>(RImplementation.Vertex.Lock(cnt, pm_g_tl->vb_stride, off));
                for (u32 k = 0; k < cnt; ++k)
                {
                    const portal_mod_vision_vert& s = V.verts[i + k];
                    pv[k].set(s.x, s.y, 0.f, 1.f, s.c, s.u, s.v);
                }
                RImplementation.Vertex.Unlock(cnt, pm_g_tl->vb_stride);
                cmd_list.set_Geometry(pm_g_tl);
                cmd_list.Render(D3DPT_TRIANGLELIST, off, 0, cnt, 0, cnt / 2);
            }
        }
    }

    // 3. the glow: down to a quarter (rgb the HUD, a the heat), blurred
    {
        PIX_EVENT(pm_vision_glow);
        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_Stencil(FALSE);
        u_setrt(cmd_list, qw, qh, pm_rt_q0->pRT, nullptr, nullptr, nullptr);
        cmd_list.set_Element(pm_s_down->E[0]);
        cmd_list.set_c("pm_vtex", 1.f / float(w), 1.f / float(h), V.heat_range, 0.f);
        cmd_list.set_Geometry(TriangleGeom);
        cmd_list.Render(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
        const float step[2] = {1.f, 2.2f};
        for (float st : step)
        {
            u_setrt(cmd_list, qw, qh, pm_rt_q1->pRT, nullptr, nullptr, nullptr);
            cmd_list.set_Element(pm_s_blur->E[0]); // reads q0
            cmd_list.set_c("pm_vdir", st / float(qw), 0.f, 0.f, 0.f);
            cmd_list.set_Geometry(TriangleGeom);
            cmd_list.Render(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
            u_setrt(cmd_list, qw, qh, pm_rt_q0->pRT, nullptr, nullptr, nullptr);
            cmd_list.set_Element(pm_s_blur->E[1]); // reads q1
            cmd_list.set_c("pm_vdir", 0.f, st / float(qh), 0.f, 0.f);
            cmd_list.set_Geometry(TriangleGeom);
            cmd_list.Render(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
        }
    }

    // 4. the grade
    {
        PIX_EVENT(pm_vision_comp);
        RenderScreenTriangle(cmd_list, rt_Generic_combine, pm_s_comp->E[0], [&]() {
            cmd_list.set_c("pm_v0", V.time, V.fade, V.glitch, V.seed);
            cmd_list.set_c("pm_v1", V.heat, V.scan, V.grain, V.ca);
            cmd_list.set_c("pm_v2", V.vignette, V.glow, float(V.hud_only), V.hit);
            cmd_list.set_c("pm_v3", V.tint, V.heat_range, float(V.no_hud), 0.f);
            cmd_list.set_c("pm_v4", V.walls, V.dim, 0.f, 0.f);
        });
        HW.get_context(cmd_list.context_id)->CopyResource(rt_Generic_0->pSurface, rt_Generic_combine->pSurface);
    }
}
