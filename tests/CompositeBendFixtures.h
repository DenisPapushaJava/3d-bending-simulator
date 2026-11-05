#pragma once
#include "BendFixtures.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFill.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Line.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS_Solid.hxx>
#include <stdexcept>
namespace composite_fixture {
inline void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
// Independently specified helicoidal ends: z=2*(1-4*u/pi) on the first half,
// then z=0. Both radial boundaries use the same axial outline; no private CAD.
inline TopoDS_Shape part(double length = 80) {
    constexpr double angle = std::numbers::pi / 2;
    const std::array<gp_Pnt2d, 6> uv{{{0, 2},
                                      {angle / 2, 0},
                                      {angle, 0},
                                      {angle, length},
                                      {angle / 2, length},
                                      {0, length - 2}}};
    std::array<std::array<TopoDS_Edge, 6>, 2> edges;
    BRepBuilderAPI_Sewing sew(1e-6);
    for (int side = 0; side < 2; ++side) {
        Handle(Geom_CylindricalSurface) surface =
            new Geom_CylindricalSurface(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 4 + 2 * side);
        BRepBuilderAPI_MakeWire wire;
        for (std::size_t i = 0; i < uv.size(); ++i) {
            const auto pc = GCE2d_MakeSegment(uv[i], uv[(i + 1) % uv.size()]).Value();
            const auto a = uv[i], b = uv[(i + 1) % uv.size()];
            edges[side][i] = BRepBuilderAPI_MakeEdge(pc, surface).Edge();
            require(BRepLib::BuildCurves3d(edges[side][i], 1e-8), "Fixture curve failed");
            if (std::abs(a.X() - b.X()) < 1e-8) {
                const auto first = surface->Value(a.X(), a.Y());
                const auto last = surface->Value(b.X(), b.Y());
                BRep_Builder builder;
                builder.UpdateEdge(edges[side][i],
                                   new Geom_Line(first, gp_Dir(gp_Vec(first, last))), 1e-7);
                builder.Range(edges[side][i], 0, first.Distance(last));
            }
            wire.Add(edges[side][i]);
        }
        sew.Add(BRepBuilderAPI_MakeFace(surface, wire.Wire(), true).Face());
    }
    for (std::size_t i = 0; i < uv.size(); ++i)
        sew.Add(BRepFill::Face(edges[0][i], edges[1][i]));
    sew.Perform();
    auto sector = BRepBuilderAPI_MakeSolid(TopoDS::Shell(sew.SewedShape())).Solid();
    require(BRepLib::OrientClosedSolid(sector), "Fixture shell failed");
    const auto wall = [](std::array<gp_Pnt, 4> p, double z, double wallLength) {
        BRepBuilderAPI_MakeWire wire;
        for (std::size_t i = 0; i < p.size(); ++i)
            wire.Add(BRepBuilderAPI_MakeEdge(p[i], p[(i + 1) % p.size()]));
        auto s =
            BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, wallLength))
                .Shape();
        gp_Trsf shift;
        shift.SetTranslation(gp_Vec(0, 0, z));
        return BRepBuilderAPI_Transform(s, shift, true).Shape();
    };
    auto s = BRepAlgoAPI_Fuse(
                 sector, wall({{{4, 0, 0}, {4, -30, 0}, {6, -30, 0}, {6, 0, 0}}}, 2, length - 4))
                 .Shape();
    s = BRepAlgoAPI_Fuse(s, wall({{{0, 4, 0}, {-30, 4, 0}, {-30, 6, 0}, {0, 6, 0}}}, 0, length))
            .Shape();
    ShapeUpgrade_UnifySameDomain unify(s, true, true, true);
    unify.Build();
    require(BRepCheck_Analyzer(unify.Shape()).IsValid(), "Invalid analytic fixture");
    return unify.Shape();
}
} // namespace composite_fixture
