// HOM.h: interface for the CHOM class.
//
//////////////////////////////////////////////////////////////////////
#pragma once

#include "../../xr_3da/IGame_Persistent.h"

class occTri;

class CHOM : public pureRender
{
private:
    xrXRC xrc;
    CDB::MODEL* m_pModel;
    occTri* m_pTris;

    Fmatrix m_xform;
    Fmatrix m_xform_01;

    u32 VisibleTriangleCount;
    u32 FrustumTriangleCount;
    u32 CulledOutCount;

    void Render_DB(CFrustum& base);

public:
    void Load();
    void Unload();

    void DispatchRender();

    BOOL visible(vis_data& vis);
    BOOL visible(sPoly& P);

    // portal-mod E2: no occlusion culling for the view through a portal (its results are cached per frame for the main camera)
    bool Allowed() const { return !portal_mod_pass && !ps_r2_ls_flags_ext.test(R2FLAGEXT_DISABLE_HOM) && m_pModel; }

    CHOM();
    ~CHOM();

    virtual void OnRender();

    void stats();
};
