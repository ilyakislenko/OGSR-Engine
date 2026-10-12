#include "stdafx.h"
#include "../../xr_3da/igame_persistent.h"
#include "../xrRender/FBasicVisual.h"
#include "../../xr_3da/customhud.h"
#include "../../xr_3da/xr_object.h"

#include "../xrRender/QueryHelper.h"

#include <..\AMD_FSR2\build\native\include\ffx-fsr2-api\ffx_fsr2.h>
#include <..\AMD_FSR2\build\native\include\ffx-fsr2-api\dx11\ffx_fsr2_dx11.h>

IC bool pred_sp_sort(ISpatial* _1, ISpatial* _2)
{
    const float d1 = _1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition);
    const float d2 = _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition);
    return d1 < d2;
}

#define BASE_FOV 67.f

// Aproximate, adjusted by fov, distance from camera to position (For right work when looking though binoculars and scopes)

IC float GetDistFromCamera(const Fvector& from_position)
{
    const float distance = Device.vCameraPosition.distance_to(from_position);
    const float fov_K = BASE_FOV / Device.fFOV;
    const float adjusted_distane = distance / fov_K;

    return adjusted_distane;
}

bool CRender::InFieldOfViewR(Fvector pos, float max_dist, bool check_direction)
{
    const float dist = GetDistFromCamera(pos);

    if (dist > max_dist)
        return false;

    //if (check_direction)
    //{
    //    Fvector toObject;
    //    toObject.sub(pos, Device.vCameraPosition);
    //    toObject.normalize();

    //    const Fvector cameraDirection = Device.vCameraDirection;
    //    const float dotProduct = cameraDirection.dotproduct(toObject);
    //    if (dotProduct < 0 && dist > 2)
    //        return false;
    //}

    return true;
}

bool RESET_SECTORS_HACK{}; //После загрузки сейва на той же локе, пока не двинешься с места, сектор не хочет меняться

void CRender::main_pass_static(R_dsgraph_structure& dsgraph)
{
    ZoneScoped;

    bool sectors_added = false;

    if (!portal_mod_pass && !ps_r2_ls_flags_ext.test(R2FLAGEXT_DISABLE_SECTORS)) // portal-mod E2: the portal view takes all sectors
    {
        // Detect camera-sector
        if (RESET_SECTORS_HACK || !Device.vCameraPositionSaved.similar(Device.vCameraPosition, EPS_L))
        {
            //if (RESET_SECTORS_HACK)
            //    Msg("~~Reset sectors after game load!");
            RESET_SECTORS_HACK = false;

            const auto sector_id = detect_sector(main_xrc, Device.vCameraPosition);
            if (sector_id != IRender_Sector::INVALID_SECTOR_ID)
            {
                if (sector_id != last_sector_id)
                {
                    g_pGamePersistent->OnSectorChanged(sector_id);

                    MsgDbg("~ Actor Sector changed! id=%d", sector_id);
                }

                last_sector_id = sector_id;
            }
            else
                MsgDbg("! detect_sector return INVALID_SECTOR_ID!");
        }

        // Save and build new frustum, disable HOM
        if (rmPortals)
        {
            // Check if camera is too near to some portal - if so force DualRender
            constexpr float eps = 1.f;

            Fvector box_radius;
            box_radius.set(eps, eps, eps);

            main_xrc.box_query(CDB::OPT_FULL_TEST, rmPortals, Device.vCameraPosition, box_radius);
            if (main_xrc.r_count() > 0)
            {
                MsgDbg("~ Enable dual render.");

                for (size_t K = 0; K < main_xrc.r_count(); K++)
                {
                    const int id = main_xrc.r_begin()[K].id;
                    const size_t portal_id = rmPortals->get_tris()[id].dummy;

                    CPortal* pPortal = dsgraph.sector_portals_structure.Portals[portal_id];
                    pPortal->bDualRender = TRUE; // this should be set on each frame as PortalTraverser will reset it
                }
            }
        }

        // Calculate sector(s) and their objects
        if (last_sector_id != IRender_Sector::INVALID_SECTOR_ID
            && last_sector_id != largest_sector_id)
        {
            constexpr int options = CPortalTraverser::VQ_HOM + CPortalTraverser::VQ_SSA + CPortalTraverser::VQ_FADE;

            // Traverse sector/portal structure
            CSector* root_sector = dsgraph.sector_portals_structure.Sectors.at(last_sector_id);
            dsgraph.PortalTraverser.traverse(root_sector, ViewBase, Device.vCameraPosition, Device.mFullTransform, options);

            xr_vector<CSector*>& sectors = dsgraph.PortalTraverser.r_sectors;

            static size_t cnt = 0;
            if (cnt != sectors.size())
            {
                MsgDbg("~ PortalTraverser Sector cnt changed = %d", cnt);
                cnt = sectors.size();
            }

            if (!sectors.empty())
            {
                // Determine visibility for static geometry hierrarhy
                for (const auto& r_sector : sectors)
                {
                    dxRender_Visual* root = r_sector->root();
                    for (u32 v_it = 0; v_it < r_sector->r_frustums.size(); v_it++)
                    {
                        const auto& view = r_sector->r_frustums[v_it];
                        dsgraph.add_static(root, view, view.getMask());
                    }
                }

                sectors_added = true;
            }
        }
    }

    if (!sectors_added)
    {
        for (const auto& r_sector : dsgraph.sector_portals_structure.Sectors)
        {
            dsgraph.add_static(r_sector->root(), ViewBase, ViewBase.getMask());
        }
    }
}

void CRender::main_pass_dynamic(R_dsgraph_structure& dsgraph, bool fill_lights)
{
    ZoneScoped;

    // Calculate sector(s) and their objects

    // Traverse object database
    g_SpatialSpace->q_frustum(lstRenderables, 0, STYPE_RENDERABLE, ViewBase);

    // (almost) Exact sorting order (front-to-back)
    std::sort(lstRenderables.begin(), lstRenderables.end(), pred_sp_sort);

    // Determine visibility for dynamic part of scene
    if (dsgraph.phase == PHASE_NORMAL && !portal_mod_pass)
    {
        // update light-vis for current entity / actor
        CObject* O = g_pGameLevel->CurrentViewEntity();
        if (O && !O->getDestroy())
        {
            if (CROS_impl* R = smart_cast<CROS_impl*>(O->ROS()))
                R->update(O);
        }

        if (!lstRenderables.empty())
        {
            uLastLTRACK++;

            u32 uID_LTRACK = uLastLTRACK % lstRenderables.size();

            // track lighting environment
            if (IRenderable* renderable = lstRenderables[uID_LTRACK]->dcast_Renderable())
            {
                // track lighting environment
                if (CROS_impl* T = smart_cast<CROS_impl*>(renderable->renderable_ROS()))
                    T->update(renderable);
            }
        }
    }

    // ждем обновление сектора тут же
    update_sectors_awaiter.wait();

    // Traverse frustums
    for (auto spatial : lstRenderables)
    {
        const auto& sector_id = spatial->spatial.sector_id;
        if (sector_id == IRender_Sector::INVALID_SECTOR_ID)
        {
            continue; // disassociated from S/P structure
        }

        if ((spatial->spatial.type & STYPE_RENDERABLE) && !ps_r2_ls_flags_ext.test(R2FLAGEXT_DISABLE_DYNAMIC))
        {
            // renderable
            IRenderable* renderable = spatial->dcast_Renderable();
            R_ASSERT(renderable);

            // Occlusion
            //	casting is faster then using getVis method
            vis_data& v_orig = renderable->renderable.visual->getVisData();
            vis_data v_copy = v_orig;
            v_copy.box.xform(renderable->renderable.xform);
            BOOL bVisible = HOM.visible(v_copy);
            v_orig.hom_frame = v_copy.hom_frame;
            v_orig.hom_tested = v_copy.hom_tested;
            if (!bVisible)
                continue; // exit loop on frustums

            if (!renderable->renderable.visual->ignore_optimization)
            {
                Fvector pos;
                renderable->renderable.xform.transform_tiny(pos, v_orig.sphere.P);
                const float distance = Device.vCameraPosition.distance_to(pos);

                if (!portal_mod_pass && dsgraph.PortalTraverser.frame() == Device.dwFrame && distance > 15.f)
                {
                    auto* sector = dsgraph.sector_portals_structure.Sectors[sector_id];

                    if (dsgraph.PortalTraverser.marker() != sector->r_marker)
                        continue; // inactive (untouched) sector
                }

                if (!InFieldOfViewR(pos, ps_r__opt_dist, true))
                    continue;
            }

            //for (auto& view : sector->r_frustums)
            {
                //if (!view.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
                //    continue;

                // Rendering
                renderable->renderable_Render(dsgraph.context_id, renderable);

                //break; // exit loop on frustums
            }
        }
    }

    if (g_pGameLevel && g_hud)
    {
        if (dsgraph.phase == PHASE_NORMAL && !portal_mod_pass)
        {
            g_hud->Render_MAIN(dsgraph.context_id);
        }
    }
}

void CRender::Render()
{
    ZoneScoped;

    TracyD3D11Zone(HW.profiler_ctx, "Render");

    auto& dsgraph = get_imm_context();
    auto& cmd_list = dsgraph.cmd_list;

    PIX_EVENT(CRender_Render);

    VERIFY(0 == mapDistort.size());

    rmNormal(cmd_list);

    if (ShouldSkipRender())
    {
        Target->u_setrt(cmd_list, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), nullptr, nullptr, Target->get_base_zb());
        return;
    }

    if (m_bFirstFrameAfterReset)
    {
        m_bFirstFrameAfterReset = false;
        return;
    }

    if (!portal_mod_pass && ps_pnv_mode < 2 && (ps_r_pp_aa_mode == DLSS || ps_r_pp_aa_mode == FSR2 || ps_r_pp_aa_mode == TAA || ps_r2_ls_flags.test(R2FLAG_DBG_TAA_JITTER_ENABLE)))
    {
        // Halton sequence generator
        auto halton = [](const int index, const int base) {
            float result = 0.0f;
            float f = 1.0f / base;
            int i = index;
            while (i > 0)
            {
                result = result + f * (i % base);
                i = static_cast<int>(std::floor(i / base));
                f = f / base;
            }
            return result;
        };

        // Генерация jitter смещений для TAA
        auto getHaltonJitterOffset = [&](float& jitterX, float& jitterY, const u32 frameIndex) {
            jitterX = halton(frameIndex + 1, 2) - 0.5f;
            jitterY = halton(frameIndex + 1, 3) - 0.5f;
        };

        int32_t jitterPhaseCount = 16;
        if (ps_r_pp_aa_mode == FSR2)
        {
            jitterPhaseCount = ffxFsr2GetJitterPhaseCount(static_cast<int32_t>(Device.dwWidth), static_cast<int32_t>(Device.dwWidth));
            ffxFsr2GetJitterOffset(&ps_r_taa_jitter_full.x, &ps_r_taa_jitter_full.y, Device.dwFrame, jitterPhaseCount);
        }
        else
        {
            getHaltonJitterOffset(ps_r_taa_jitter_full.x, ps_r_taa_jitter_full.y, Device.dwFrame);
        }

        ps_r_taa_jitter.x = 2.0f * ps_r_taa_jitter_full.x / Device.dwWidth;
        ps_r_taa_jitter.y = -2.0f * ps_r_taa_jitter_full.y / Device.dwHeight;
        ps_r_taa_jitter.z = static_cast<float>(Device.dwFrame % jitterPhaseCount) / static_cast<float>(jitterPhaseCount) + EPS;
    }
    else
    {
        ps_r_taa_jitter.set(0.f, 0.f, -1.f);
        ps_r_taa_jitter_full.set(0.f, 0.f);
    }

    {
        PIX_EVENT(SKY_RENDER);

        cmd_list.ClearZB(Target->get_base_zb(), 1.0f, 0);

        const Fcolor color{}; // black

        cmd_list.ClearRT(Target->rt_Generic_0->pRT, color);
        cmd_list.ClearRT(Target->rt_Velocity->pRT, color);

        Target->u_setrt(cmd_list, Target->rt_Generic_0, Target->rt_Velocity, nullptr, nullptr, Target->rt_Base_Depth->pZRT[cmd_list.context_id]);

        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_Stencil(FALSE);

        if (portal_mod_pass) // portal-mod E2: sky dome with the standard projection
            portal_mod_projection(true);
        g_pGamePersistent->Environment().RenderSky(cmd_list);
        if (portal_mod_pass)
            portal_mod_projection(false);
    }

    const Fcolor& sun_color = smart_cast<light*>(Lights.sun_adapted._get())->color;
    const BOOL bSUN = ps_r2_ls_flags.test(R2FLAG_SUN) && (u_diffuse2s(sun_color.r, sun_color.g, sun_color.b) > EPS);

    cmd_list.set_xform_world(Fidentity);
    cmd_list.set_xform_world_old(Fidentity);

    // Main calc start
    // r_main.sync();
    // Main calc end

    Target->phase_scene_prepare();

    {
        PIX_EVENT(DEFER_PART0_SPLIT);

        r_main.ensure_calculate_static();
        r_main.wait_static();

        r_main.calculate_dynamic();

        Target->phase_scene_begin(cmd_list);
        dsgraph.r_dsgraph_render_graph_static(0);

        r_main.wait_dynamic();

        dsgraph.r_dsgraph_render_graph_dynamic(0);
    }

    // wait light export
    light_waiter.wait();

    const light_Package& LP_normal = Lights.package;

    {
        PIX_EVENT(DEFER_TEST_LIGHT_VIS);

        //******* Occlusion testing of volume-limited light-sources
        Target->phase_occq(cmd_list);

        // stats
        stats.l_shadowed = LP_normal.v_shadowed.size();
        stats.l_unshadowed = LP_normal.v_point.size() + LP_normal.v_spot.size();
        stats.l_total = stats.l_shadowed + stats.l_unshadowed;

        // perform tests
        if (!portal_mod_pass) // portal-mod E2: light occlusion queries belong to the main camera
            LP_normal.vis_prepare(cmd_list);
    }

    //******* Main render :: PART-1 (second)
    {
        PIX_EVENT(DEFER_PART1_SPLIT);

        // level
        Target->phase_scene_begin(cmd_list);
        dsgraph.r_dsgraph_render_lods();
        if (Details && !portal_mod_pass)
            Details->Render(cmd_list);
        {
            {
                PIX_EVENT(copy_zbuffer_scope);

                ID3D11Resource* res{};
                Target->get_base_zb()->GetResource(&res);
                HW.get_context(cmd_list.context_id)->CopyResource(Target->rt_tempzb->pSurface, res);
                _RELEASE(res);
            }
            if (!portal_mod_pass)
                dsgraph.r_dsgraph_render_hud();
            {
                PIX_EVENT(copy_zbuffer_scope_depth);

                ID3D11Resource* res{};
                Target->get_base_zb()->GetResource(&res);
                HW.get_context(cmd_list.context_id)->CopyResource(Target->rt_tempzb_dof->pSurface, res);
                _RELEASE(res);
            }
            if (!portal_mod_pass)
                dsgraph.r_dsgraph_render_hud_scope_depth();
        }
    }

    {
        PIX_EVENT(copy_zbuffer);

        ID3D11Resource* res{};
        Target->get_base_zb()->GetResource(&res);
        HW.get_context(cmd_list.context_id)->CopyResource(Target->rt_zbuffer->pSurface, res);
        _RELEASE(res);
    }

    // Wall marks
    if (Wallmarks && !portal_mod_pass)
    {
        PIX_EVENT(DEFER_WALLMARKS);

        Target->phase_wallmarks(cmd_list);
        Wallmarks->Render(); // wallmarks has priority as normal geometry
    }

    {
        //	TODO: DX10: Implement DX10 rain.
        PIX_EVENT(DEFER_RAIN);
        r_rain.sync();
    }

    // Directional light - fucking sun
    if (bSUN)
    {
        PIX_EVENT(DEFER_SUN);
        stats.l_visible++;
        {
            r_sun.sync();
        }
        Target->accum_direct_blend(cmd_list);
    }

    {
        PIX_EVENT(DEFER_SELF_ILLUM);
        Target->phase_accumulator(cmd_list);

        // Render emissive geometry, stencil - write 0x0 at pixel pos
        cmd_list.set_xform_project(Device.mProject);
        cmd_list.set_xform_view(Device.mView);

        // Stencil - write 0x1 at pixel pos -
        cmd_list.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);

        cmd_list.set_CullMode(CULL_CCW);
        cmd_list.set_ColorWriteEnable();

        if (ps_r2_ls_flags.test(R2FLAG_SSFX_BLOOM))
        {
            dsgraph.r_dsgraph_render_emissive(false);

            // Render Emissive on `rt_ssfx_bloom_emissive`
            cmd_list.ClearRT(Target->rt_ssfx_bloom_emissive->pRT, {}); // black
            Target->u_setrt(cmd_list, Target->rt_ssfx_bloom_emissive, nullptr, nullptr, nullptr, Target->rt_Base_Depth->pZRT[cmd_list.context_id]);
            dsgraph.r_dsgraph_render_emissive(true);
        }
        else
        {
            dsgraph.r_dsgraph_render_emissive(true);
        }
    }

    // Update incremental shadowmap-visibility solver
    if (!portal_mod_pass && !Lights_LastFrame.empty() && !ps_r2_ls_flags_ext.test(R2FLAGEXT_DISABLE_SMAPVIS))
    {
        ZoneScopedN("Lights_LastFrame/flushoccq");

        PIX_EVENT(DEFER_FLUSH_OCCLUSION);
        for (const auto& it : Lights_LastFrame)
        {
            if (nullptr == it)
                continue;
            // try
            //{
            for (int id = 0; id < R__NUM_CONTEXTS; ++id)
                it->svis[id].flush();
            /*}
            catch (...)
            {
                Msg("! Failed to flush-OCCq on light [%d] %X", it, *(u32*)(&Lights_LastFrame[it]));
            }*/
        }
        Lights_LastFrame.clear();
    }

    // Lighting
    {
        PIX_EVENT(DEFER_LIGHT);

        light_Package LP_normal_copy = LP_normal;

        //LP_normal_copy.vis_update();
        LP_normal_copy.sort();

        Target->phase_accumulator(cmd_list); // init accum here
        render_lights(LP_normal_copy);
    }

    // Postprocess
    {
        PIX_EVENT(DEFER_COMBINE);
        Target->phase_combine(cmd_list); // calls CRender::render_forward()
    }

    if (Details)
        Details->Clear();

    // wait occ results for next frame lights
    if (!portal_mod_pass)
        LP_normal.vis_update();

    VERIFY(0 == mapDistort.size());
}

void CRender::render_forward()
{
    ZoneScoped;

    auto& dsgraph = get_imm_context();
    CBackend& cmd_list = dsgraph.cmd_list;

    VERIFY(0 == mapDistort.size());

    //******* Main render - second order geometry (the one, that doesn't support deffering)
    //.todo: should be done inside "combine" with estimation of of luminance, tone-mapping, etc.
    {
        //	Igor: we don't want to render old lods on next frame.
        dsgraph.lstLODs.clear();

        auto& Env = g_pGamePersistent->Environment();
        Env.RenderLast(cmd_list); // rain/thunder-bolts

        if (!portal_mod_pass && ps_r2_ls_flags_ext.test(R2FLAGEXT_SSLR) && !fis_zero(Env.wetness_factor))
        {
            for (const auto& puddle : current_level_puddles)
            {
                if (!ViewBase.testSphere_dirty(puddle.xform.c, puddle.radius))
                    continue;

                cmd_list.set_Shader(Target->s_puddles);
                cmd_list.set_xform_world(puddle.xform);
                cmd_list.set_c("puddle_height", Env.wetness_factor * puddle.height);

                cmd_list.Render(6);
            }
        }

        // portal-mod: Portal 2 portals (xr_3da/Render.h portal_mod_draw, set from Lua by level.portal_set)
        for (const auto& portal : portal_mod_draw)
        {
            if (portal_mod_pass || !portal.on || !ViewBase.testSphere_dirty(portal.xform.c, std::max(portal.xform.i.magnitude(), portal.xform.j.magnitude())))
                continue;

            Fvector to_portal; // portal-mod E2: a portal seen from behind is not there (it is on the other side of a wall)
            to_portal.sub(portal.xform.c, Device.vCameraPosition);
            if (to_portal.dotproduct(portal.xform.k) >= 0.f)
                continue;

            cmd_list.set_Shader(Target->s_portal_gun);
            cmd_list.set_xform_world(portal.xform);
            cmd_list.set_c("portal_params", portal.params.x, portal.params.y, portal.params.z, portal.params.w);

            cmd_list.Render(6);
        }

        dsgraph.r_dsgraph_render_graph(1); // normal level, secondary priority
        dsgraph.PortalTraverser.fade_render(cmd_list); // faded-portals
        dsgraph.r_dsgraph_render_sorted(); // strict-sorted geoms
    }
}

// portal-mod E2 -------------------------------------------------------------------------------------------------------
// Oblique near plane (Lengyel; D3D z in [0,1], row vectors). C = clip plane in view space, camera on its negative side.
static void portal_mod_make_oblique(Fmatrix& P, const Fvector4& C)
{
    auto sg = [](float v) { return v > 0.f ? 1.f : (v < 0.f ? -1.f : 0.f); };
    const float qx = sg(C.x) / P._11, qy = sg(C.y) / P._22, qz = 1.f, qw = (1.f - P._33) / P._43;
    const float s = 1.f / (C.x * qx + C.y * qy + C.z * qz + C.w * qw);
    P._13 = C.x * s;
    P._23 = C.y * s;
    P._33 = C.z * s;
    P._43 = C.w * s;
}

void CRender::portal_mod_projection(bool standard)
{
    Device.mProject.set(standard ? portal_mod_P_std : portal_mod_P_obl);
    Device.mFullTransform.mul(Device.mProject, Device.mView);
    Device.mInvFullTransform.invert_44(Device.mFullTransform);
    SetCacheXform(Device.mView, Device.mProject);
}

// The world seen from a virtual camera (the camera mapped through a portal) into rt_portal0, before the main frame.
void CRender::RenderPortalView(const Fvector& pos, const Fvector& dir, const Fvector& top, const Fplane& clip)
{
    if (!g_pGameLevel || ShouldSkipRender() || m_bFirstFrameAfterReset)
        return;

    ZoneScoped;

    // OnCameraUpdated already started the main camera's static visibility: finish it, drop it, redo it afterwards.
    r_main.sync();
    get_imm_context().reset();

    const Fvector s_pos = Device.vCameraPosition, s_dir = Device.vCameraDirection, s_top = Device.vCameraTop, s_right = Device.vCameraRight;
    const Fmatrix s_view = Device.mView, s_inv_view = Device.mInvView, s_proj = Device.mProject, s_full = Device.mFullTransform,
                  s_inv_full = Device.mInvFullTransform;
    const CFrustum s_base = ViewBase;

    Device.vCameraPosition.set(pos);
    Device.vCameraDirection.set(dir);
    Device.vCameraTop.set(top);
    Device.vCameraRight.crossproduct(top, dir);
    Device.mView.build_camera_dir(pos, dir, top);
    Device.mInvView.invert(Device.mView);

    portal_mod_P_std.set(s_proj);
    portal_mod_full_std.mul(portal_mod_P_std, Device.mView);
    ViewBase.CreateFromMatrix(portal_mod_full_std, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);

    // Nothing between the virtual camera and the exit portal may show: the near plane becomes the portal plane.
    Fvector n_view, p_world, p_view;
    Device.mView.transform_dir(n_view, clip.n);
    p_world.mul(clip.n, -clip.d);
    Device.mView.transform_tiny(p_view, p_world);
    Fvector4 C;
    C.set(n_view.x, n_view.y, n_view.z, -n_view.dotproduct(p_view));
    portal_mod_P_obl.set(s_proj);
    portal_mod_make_oblique(portal_mod_P_obl, C);

    portal_mod_projection(false);
    SetCacheXformOld(Device.mView, Device.mProject);

    portal_mod_pass = true;
    r_main.calculate_static();
    Calculate();
    Render();
    portal_mod_pass = false;

    // The main pass must find this frame fresh: lights not added yet, accumulator and flares not cleared yet.
    r_main.sync();
    for (light* L : Lights.package.v_point)
        L->frame_render = 0;
    for (light* L : Lights.package.v_spot)
        L->frame_render = 0;
    for (light* L : Lights.package.v_shadowed)
        L->frame_render = 0;
    Target->portal_mod_reset_marks();
    get_imm_context().reset();

    Device.vCameraPosition.set(s_pos);
    Device.vCameraDirection.set(s_dir);
    Device.vCameraTop.set(s_top);
    Device.vCameraRight.set(s_right);
    Device.mView.set(s_view);
    Device.mInvView.set(s_inv_view);
    Device.mProject.set(s_proj);
    Device.mFullTransform.set(s_full);
    Device.mInvFullTransform.set(s_inv_full);
    ViewBase = s_base;
    SetCacheXform(Device.mView, Device.mProject);
    SetCacheXformOld(Device.mView_old, Device.mProject_old);

    r_main.calculate_static();
}
