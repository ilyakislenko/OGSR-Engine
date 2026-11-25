#include "stdafx.h"

#include "xrCDB.h"

using namespace CDB;
using namespace Opcode;

MODEL::~MODEL()
{
    syncronize(); // maybe model still in building
    status = S_INIT;
    xr_delete(tree);
    xr_free(tris);
    tris_count = 0;
    xr_free(verts);
    verts_count = 0;
}

void MODEL::build(const Fvector* V, const size_t Vcnt, const TRI* T, const size_t Tcnt, build_callback* bc, void* bcp)
{
    R_ASSERT(S_INIT == status);
    R_ASSERT((Vcnt >= 4) && (Tcnt >= 2));

    build_internal(V, Vcnt, T, Tcnt, bc, bcp);
    status = S_READY;
}

void MODEL::build_internal(const Fvector* V, const size_t Vcnt, const TRI* T, const size_t Tcnt, build_callback* bc, void* bcp)
{
    // verts
    verts_count = Vcnt;
    verts = xr_alloc<Fvector>(verts_count);
    std::memcpy(verts, V, verts_count * sizeof(Fvector));

    // tris
    tris_count = Tcnt;
    tris = xr_alloc<TRI>(tris_count);
    std::memcpy(tris, T, tris_count * sizeof(TRI));

    // callback
    if (bc)
        bc(verts, Vcnt, tris, Tcnt, bcp);

    // Release data pointers
    status = S_BUILD;
    MeshInterface* mif = xr_new<MeshInterface>();
    mif->SetNbTriangles(tris_count);
    mif->SetNbVertices(verts_count);
    mif->SetPointers(reinterpret_cast<const IceMaths::IndexedTriangle*>(tris), reinterpret_cast<const IceMaths::Point*>(verts));
    mif->SetStrides(sizeof(TRI));

    // Build a non quantized no-leaf tree
    OPCODECREATE OPCC = OPCODECREATE();
    OPCC.mIMesh = mif;
    OPCC.mQuantized = false;

    tree = xr_new<Model>();

    if (!tree->Build(OPCC))
    {
        xr_free(verts);
        xr_free(tris);
        xr_delete(mif);
        return;
    }

    xr_delete(mif);
}

u32 MODEL::memory()
{
    if (S_BUILD == status)
    {
        Msg("! xrCDB: model still isn't ready");
        return 0;
    }
    const u32 V = verts_count * sizeof(Fvector);
    const u32 T = tris_count * sizeof(TRI);
    return tree->GetUsedBytes() + V + T + sizeof(*this) + sizeof(*tree);
}

// portal-mod E60: the tree refitted after triangles were moved in place (portal_mod_craters: the level's ground under a
// crater sent to the sink, the pool's triangles - appended after the level's, all at the sink when the tree was built -
// brought up into its bowl). A node over pool triangles alone is fitted to them exactly (one at the sink counts for
// nothing: such a node shrinks to the sink); a node of the level's own only ever grows, so a triangle sunk and brought
// back later (a fallen tree's, a broken fence's) is still found. Only the nodes with pool triangles under them are
// walked (listed once per model: the pool is a few subtrees of the root's).
namespace
{
struct RefitPlan
{
    const void* model{};
    const void* tris{};
    size_t pool_from{}, nodes{};
    xr_vector<udword> walk; // the nodes with pool triangles under them, children before parents
    xr_vector<u8> flags; // per node: 1 pool alone, 2 empty (pool alone, all at the sink)
};
RefitPlan g_refit;
} // namespace

bool MODEL::refit(size_t pool_from, u32 sink)
{
    if (S_READY != status || !tree || pool_from >= tris_count || sink + 2 >= verts_count)
        return false;
    AABBNoLeafTree* T = static_cast<AABBNoLeafTree*>(tree->GetTree());
    if (!T || !T->GetNodes())
        return false;
    AABBNoLeafNode* nodes = const_cast<AABBNoLeafNode*>(T->GetNodes());
    const udword nb = T->GetNbNodes();
    RefitPlan& P = g_refit;
    if (P.model != this || P.tris != tris || P.pool_from != pool_from || P.nodes != nb)
    {
        P.model = this, P.tris = tris, P.pool_from = pool_from, P.nodes = nb;
        P.walk.clear();
        P.flags.assign(nb, 0);
        xr_vector<u8> any(nb, 0);
        for (udword i = nb; i--;)
        {
            const AABBNoLeafNode& n = nodes[i];
            bool has = false, alone = true;
            const auto side = [&](BOOL leaf, udword prim, const AABBNoLeafNode* c) {
                if (leaf)
                {
                    const bool pool = prim >= pool_from;
                    has = has || pool;
                    alone = alone && pool;
                }
                else
                {
                    const udword ci = udword(c - nodes);
                    has = has || any[ci];
                    alone = alone && (P.flags[ci] & 1);
                }
            };
            side(n.HasPosLeaf(), n.GetPosPrimitive(), n.GetPos());
            side(n.HasNegLeaf(), n.GetNegPrimitive(), n.GetNeg());
            any[i] = has;
            P.flags[i] = alone ? 1 : 0;
            if (has)
                P.walk.push_back(i);
        }
    }
    const IceMaths::Point sp(verts[sink].x, verts[sink].y, verts[sink].z);
    for (const udword i : P.walk)
    {
        AABBNoLeafNode& n = nodes[i];
        IceMaths::Point mn(flt_max, flt_max, flt_max), mx(-flt_max, -flt_max, -flt_max);
        bool got = false;
        const auto side = [&](BOOL leaf, udword prim, const AABBNoLeafNode* c) {
            if (leaf)
            {
                const TRI& t = tris[prim];
                if (t.verts[0] == sink)
                    return;
                for (int k = 0; k < 3; ++k)
                {
                    const Fvector& v = verts[t.verts[k]];
                    const IceMaths::Point p(v.x, v.y, v.z);
                    mn.Min(p);
                    mx.Max(p);
                }
                got = true;
            }
            else
            {
                const udword ci = udword(c - nodes);
                if (P.flags[ci] & 2)
                    return;
                IceMaths::Point a, b;
                c->mAABB.GetMin(a);
                c->mAABB.GetMax(b);
                mn.Min(a);
                mx.Max(b);
                got = true;
            }
        };
        side(n.HasPosLeaf(), n.GetPosPrimitive(), n.GetPos());
        side(n.HasNegLeaf(), n.GetNegPrimitive(), n.GetNeg());
        if (P.flags[i] & 1)
        {
            if (got)
            {
                n.mAABB.SetMinMax(mn, mx);
                P.flags[i] = 1;
            }
            else
            {
                n.mAABB.SetMinMax(sp, sp);
                P.flags[i] = 1 | 2;
            }
        }
        else if (got)
        {
            IceMaths::Point a, b;
            n.mAABB.GetMin(a);
            n.mAABB.GetMax(b);
            if (mn.x < a.x || mn.y < a.y || mn.z < a.z || mx.x > b.x || mx.y > b.y || mx.z > b.z)
            {
                a.Min(mn);
                b.Max(mx);
                n.mAABB.SetMinMax(a, b);
            }
        }
    }
    return true;
}

RESULT& COLLIDER::r_add() { return rd.emplace_back(); }
