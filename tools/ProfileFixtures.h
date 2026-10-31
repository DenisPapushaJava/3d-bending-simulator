#pragma once
// Own constant-thickness open profiles; no production model coordinates.
#include "BendFixtures.h"
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <vector>

namespace bending::fixtures {
inline TopoDS_Shape foldedProfile(std::size_t bends, double length = 80, double turn = .4,
                                  bool varied = false) {
    constexpr double thickness = 2, middleRadius = 5, straight = 20;
    const auto normal = [](double angle) { return gp_Vec(-std::sin(angle), std::cos(angle), 0); };
    const auto skin = [&](double offset) {
        std::vector<TopoDS_Edge> edges;
        gp_Pnt p;
        double angle = 0;
        for (std::size_t i = 0; i <= bends; ++i) {
            const double wall = straight * (varied ? 1 + .15 * (i % 5) : 1);
            const auto next = p.Translated(gp_Vec(std::cos(angle), std::sin(angle), 0) * wall);
            edges.push_back(BRepBuilderAPI_MakeEdge(p.Translated(normal(angle) * offset),
                                                    next.Translated(normal(angle) * offset))
                                .Edge());
            p = next;
            if (i == bends)
                break;
            const double sign = i % 4 == 0 || i % 4 == 3 ? 1 : -1;
            const double rotation = turn * (varied ? 1 + .2 * ((i / 4) % 3) : 1);
            const double middle = middleRadius * (varied ? 1 + .1 * ((i + 1) % 3) : 1);
            const auto centre = p.Translated(normal(angle) * (sign * middle));
            const double radius = middle - sign * offset;
            const auto at = [&](double a) {
                return centre.Translated(normal(a) * (-sign * radius));
            };
            edges.push_back(BRepBuilderAPI_MakeEdge(
                                GC_MakeArcOfCircle(at(angle), at(angle + sign * rotation / 2),
                                                   at(angle + sign * rotation))
                                    .Value())
                                .Edge());
            angle += sign * rotation;
            p = centre.Translated(normal(angle) * (-sign * middle));
        }
        return edges;
    };
    const auto lower = skin(-thickness / 2), upper = skin(thickness / 2);
    BRepBuilderAPI_MakeWire wire;
    for (const auto &edge : lower)
        wire.Add(edge);
    const auto vertex = [](const TopoDS_Edge &edge, bool first) {
        return BRep_Tool::Pnt(first ? TopExp::FirstVertex(edge) : TopExp::LastVertex(edge));
    };
    wire.Add(
        BRepBuilderAPI_MakeEdge(vertex(lower.back(), false), vertex(upper.back(), false)).Edge());
    for (auto it = upper.rbegin(); it != upper.rend(); ++it)
        wire.Add(TopoDS::Edge(it->Reversed()));
    wire.Add(
        BRepBuilderAPI_MakeEdge(vertex(upper.front(), true), vertex(lower.front(), true)).Edge());
    auto result =
        BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, length)).Shape();
    GProp_GProps props;
    BRepGProp::VolumeProperties(result, props);
    if (props.Mass() < 0)
        result.Reverse();
    return result;
}
} // namespace bending::fixtures
