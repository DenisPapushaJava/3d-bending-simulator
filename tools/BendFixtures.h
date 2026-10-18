#pragma once

// Original analytic fixtures shared by the example generator and geometry tests.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <cmath>
#include <numbers>

namespace bending::fixtures {
inline gp_Pnt polar(double radius, double angle, double x = 0.0, double y = 0.0) {
    return {x + radius * std::cos(angle), y + radius * std::sin(angle), 0.0};
}
inline TopoDS_Shape bentStrip(double radius = 4.0, double thickness = 2.0,
                              double angle = std::numbers::pi / 2.0, double length = 80.0,
                              bool flanges = true) {
    const auto inner0 = polar(radius, 0.0);
    const auto outer0 = polar(radius + thickness, 0.0);
    const auto inner1 = polar(radius, angle);
    const auto outer1 = polar(radius + thickness, angle);
    const auto arc = [&](double r) {
        return BRepBuilderAPI_MakeEdge(
                   GC_MakeArcOfCircle(polar(r, 0.0), polar(r, angle * 0.5), polar(r, angle))
                       .Value())
            .Edge();
    };
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](const gp_Pnt &a, const gp_Pnt &b) {
        wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
    };
    if (flanges) {
        const auto a = inner0.Translated(gp_Vec(0, -30, 0));
        const auto b = outer0.Translated(gp_Vec(0, -30, 0));
        line(inner0, a);
        line(a, b);
        line(b, outer0);
    } else {
        line(inner0, outer0);
    }
    wire.Add(arc(radius + thickness));
    if (flanges) {
        const gp_Vec tangent(-30 * std::sin(angle), 30 * std::cos(angle), 0);
        const auto a = outer1.Translated(tangent);
        const auto b = inner1.Translated(tangent);
        line(outer1, a);
        line(a, b);
        line(b, inner1);
    } else {
        line(outer1, inner1);
    }
    wire.Add(TopoDS::Edge(arc(radius).Reversed()));
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, length))
        .Shape();
}
// Oblique plane trims the cylinder with an ellipse. Its UV boundary is a
// non-rational spline after STEP export, with the same constant-thickness core.
inline TopoDS_Shape trimmedBend() {
    const auto plane = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(-1, 0, 1))).Face();
    const auto below = BRepPrimAPI_MakeHalfSpace(plane, gp_Pnt(0, 0, -10)).Solid();
    return BRepAlgoAPI_Cut(bentStrip(), below).Shape();
}
// Independently designed MIT fixture: two perforated flanges, a countersink and
// a clipped free corner. The bend and its full-width support strips are intact.
inline TopoDS_Shape profiledBend() {
    auto shape = bentStrip(8, 6);
    shape = BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(
                                       gp_Ax2(gp_Pnt(7, -26, 25), gp_Dir(1, 0, 0)), 1.5, 8))
                .Shape();
    shape = BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCone(
                                       gp_Ax2(gp_Pnt(14, -26, 25), gp_Dir(-1, 0, 0)), 3, 1.5, 1.5))
                .Shape();
    shape = BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(
                                       gp_Ax2(gp_Pnt(-26, 7, 50), gp_Dir(0, 1, 0)), 2.5, 8))
                .Shape();
    BRepBuilderAPI_MakeWire corner;
    const std::array<gp_Pnt, 3> points{gp_Pnt(7, -30, 0), gp_Pnt(7, -27, 0), gp_Pnt(7, -30, 3)};
    for (std::size_t i = 0; i < points.size(); ++i) {
        corner.Add(BRepBuilderAPI_MakeEdge(points[i], points[(i + 1) % points.size()]));
    }
    return BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(corner.Wire()),
                                                        gp_Vec(8, 0, 0)))
        .Shape();
}
inline TopoDS_Shape channel() {
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](const gp_Pnt &a, const gp_Pnt &b) {
        wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
    };
    const auto arc = [&](double x, double r, double a, double b) {
        wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(polar(r, a, x, 6),
                                                            polar(r, (a + b) * 0.5, x, 6),
                                                            polar(r, b, x, 6))
                                             .Value())
                     .Edge());
    };
    constexpr double pi = std::numbers::pi;
    line({-6, 40, 0}, {-6, 6, 0});
    arc(0, 6, pi, 1.5 * pi);
    line({0, 0, 0}, {80, 0, 0});
    arc(80, 6, 1.5 * pi, 2 * pi);
    line({86, 6, 0}, {86, 40, 0});
    line({86, 40, 0}, {84, 40, 0});
    line({84, 40, 0}, {84, 6, 0});
    arc(80, 4, 2 * pi, 1.5 * pi);
    line({80, 2, 0}, {0, 2, 0});
    arc(0, 4, 1.5 * pi, pi);
    line({-4, 6, 0}, {-4, 40, 0});
    line({-4, 40, 0}, {-6, 40, 0});
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, 80)).Shape();
}
} // namespace bending::fixtures
