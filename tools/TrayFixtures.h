#pragma once
// Own analytic sheet-metal fixtures, independent of workshop STEP files.
#include "BendFixtures.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
namespace bending::fixtures {
inline TopoDS_Shape traySide(gp_Pnt origin, gp_Vec axis, gp_Vec outward, double length,
                             double straight = 16, double turn = std::numbers::pi / 2) {
    constexpr double r = 2, t = 2, outer = r + t;
    const auto at = [&](double radius, double angle) {
        return origin.Translated(outward * (radius * std::cos(angle)) +
                                 gp_Vec(0, 0, outer + radius * std::sin(angle)));
    };
    const double a0 = -std::numbers::pi / 2, a1 = a0 + turn;
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](gp_Pnt a, gp_Pnt b) { wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge()); };
    const auto arc = [&](double radius, double from, double to) {
        wire.Add(
            BRepBuilderAPI_MakeEdge(
                GC_MakeArcOfCircle(at(radius, from), at(radius, (from + to) / 2), at(radius, to))
                    .Value())
                .Edge());
    };
    const auto tip = outward * (-straight * std::sin(a1)) + gp_Vec(0, 0, straight * std::cos(a1));
    line(at(r, a0), at(outer, a0));
    arc(outer, a0, a1);
    line(at(outer, a1), at(outer, a1).Translated(tip));
    line(at(outer, a1).Translated(tip), at(r, a1).Translated(tip));
    line(at(r, a1).Translated(tip), at(r, a1));
    arc(r, a1, a0);
    auto s = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), axis * length).Shape();
    GProp_GProps v;
    BRepGProp::VolumeProperties(s, v);
    if (v.Mass() < 0)
        s.Reverse();
    return s;
}
inline TopoDS_Shape tray(bool holes = true, double width = 200, double depth = 100,
                         double turn = std::numbers::pi / 2) {
    auto s = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), width, depth, 2).Shape();
    for (const auto &side : {traySide({10, 0, 0}, {1, 0, 0}, {0, -1, 0}, width - 20, 16, turn),
                             traySide({10, depth, 0}, {1, 0, 0}, {0, 1, 0}, width - 20),
                             traySide({0, 10, 0}, {0, 1, 0}, {-1, 0, 0}, depth - 20),
                             traySide({width, 10, 0}, {0, 1, 0}, {1, 0, 0}, depth - 20)})
        s = BRepAlgoAPI_Fuse(s, side).Shape();
    if (holes)
        for (double x : {12., width - 12})
            for (double y : {12., depth - 12})
                s = BRepAlgoAPI_Cut(s, BRepPrimAPI_MakeCylinder(
                                           gp_Ax2(gp_Pnt(x, y, -1), gp_Dir(0, 0, 1)), 1.5, 4))
                        .Shape();
    ShapeUpgrade_UnifySameDomain unify(s, true, true, true);
    unify.Build();
    return unify.Shape();
}
inline TopoDS_Shape concaveTray() {
    BRepBuilderAPI_MakeWire wire;
    const std::array<gp_Pnt, 6> ps{
        {{0, 0, 0}, {100, 0, 0}, {100, 40, 0}, {40, 40, 0}, {40, 100, 0}, {0, 100, 0}}};
    for (std::size_t i = 0; i < ps.size(); ++i)
        wire.Add(BRepBuilderAPI_MakeEdge(ps[i], ps[(i + 1) % ps.size()]).Edge());
    auto s = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, 2)).Shape();
    s = BRepAlgoAPI_Fuse(s, traySide({50, 40, 0}, {1, 0, 0}, {0, 1, 0}, 40, 60)).Shape();
    s = BRepAlgoAPI_Fuse(s, traySide({40, 50, 0}, {0, 1, 0}, {1, 0, 0}, 40, 60)).Shape();
    ShapeUpgrade_UnifySameDomain unify(s, true, true, true);
    unify.Build();
    return unify.Shape();
}
inline TopoDS_Shape branchedTray() {
    gp_Trsf placement;
    // A fifth bend along a vertical edge of the first flange, rather than the base.
    placement.SetValues(0, -1, 0, 190, 0, 0, -1, -2, 1, 0, 0, 6);
    const auto side =
        BRepBuilderAPI_Transform(traySide({0, 0, 0}, {1, 0, 0}, {0, -1, 0}, 12), placement, true)
            .Shape();
    ShapeUpgrade_UnifySameDomain unify(BRepAlgoAPI_Fuse(tray(), side).Shape(), true, true, true);
    unify.Build();
    return unify.Shape();
}
inline TopoDS_Shape obliqueTray() {
    BRepBuilderAPI_MakePolygon wire;
    for (const auto &p : {gp_Pnt(0, 0, 0), gp_Pnt(200, 0, 0), gp_Pnt(200, 100, 0),
                          gp_Pnt(100, 200, 0), gp_Pnt(0, 200, 0)})
        wire.Add(p);
    wire.Close();
    auto s = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, 2)).Shape();
    const double d = std::sqrt(.5);
    s = BRepAlgoAPI_Fuse(s, traySide({10, 0, 0}, {1, 0, 0}, {0, -1, 0}, 180)).Shape();
    s = BRepAlgoAPI_Fuse(s, traySide({200 - 10 * d, 100 + 10 * d, 0}, {-d, d, 0}, {d, d, 0},
                                     100 * std::sqrt(2.) - 20))
            .Shape();
    ShapeUpgrade_UnifySameDomain unify(s, true, true, true);
    unify.Build();
    return unify.Shape();
}
} // namespace bending::fixtures
