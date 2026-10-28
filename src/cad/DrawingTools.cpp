#include "bending/cad/occt/DrawingTools.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeAnalysis_Wire.hxx>
#include <ShapeExtend_Status.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Vec.hxx>
#include <numbers>
#include <stdexcept>
#include <vector>
namespace bending::cad {
namespace {
using namespace core;
DrawingPoint shifted(DrawingPoint p, DrawingPoint origin, double y = 0) {
    return {p.x - origin.x, p.y - origin.y + y};
}
double radius(const DrawingEdge &e) {
    return std::hypot(e.start.x - e.center->x, e.start.y - e.center->y);
}
DrawingPoint at(const DrawingEdge &e, double fraction) {
    const double a =
        std::atan2(e.start.y - e.center->y, e.start.x - e.center->x) + fraction * e.sweepRadians;
    const double r = radius(e);
    return {e.center->x + r * std::cos(a), e.center->y + r * std::sin(a)};
}
DrawingEdge line(DrawingPoint a, DrawingPoint b) {
    return {a, b, {}, 0};
}
TopoDS_Shape sectionPrism(const std::vector<DrawingEdge> &edges, DrawingPoint origin, double tip,
                          double length, double z) {
    std::vector<TopoDS_Vertex> vertices;
    for (const auto &e : edges) {
        auto p = shifted(e.start, origin, tip);
        vertices.push_back(BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, z)));
    }
    BRepBuilderAPI_MakeWire builder;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const auto &e = edges[i];
        const auto &a = vertices[i], &b = vertices[(i + 1) % vertices.size()];
        if (!e.center)
            builder.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
        else {
            const auto mid = shifted(at(e, .5), origin, tip);
            const auto arc =
                GC_MakeArcOfCircle(BRep_Tool::Pnt(a), gp_Pnt(mid.x, mid.y, z), BRep_Tool::Pnt(b))
                    .Value();
            builder.Add(BRepBuilderAPI_MakeEdge(arc, a, b).Edge());
        }
    }
    if (!builder.IsDone())
        throw std::runtime_error("Cannot close DXF tool contour");
    const auto face = BRepBuilderAPI_MakeFace(builder.Wire()).Face();
    ShapeAnalysis_Wire check(builder.Wire(), face, 1.e-7);
    if (check.CheckSelfIntersection() || check.StatusSelfIntersection(ShapeExtend_FAIL) ||
        !BRepCheck_Analyzer(face).IsValid())
        throw std::runtime_error("Self-intersecting or invalid DXF tool contour");
    auto solid = BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, length)).Shape();
    if (!BRepCheck_Analyzer(solid).IsValid())
        throw std::runtime_error("Invalid DXF tool solid");
    return solid;
}
} // namespace
std::array<TopoDS_Shape, 2> makeDrawingTools(const core::ToolAssignment &a,
                                             const core::ToolGeometry &g, double tip) {
    return {sectionPrism(a.die.drawing->edges, g.dieTopCentre, 0, a.die.lengthMm, 0),
            sectionPrism(a.punch.drawing->edges, g.punchTip, tip, a.punch.lengthMm,
                         (a.die.lengthMm - a.punch.lengthMm) / 2)};
}
TopoDS_Shape drawingPunchSweep(const core::ToolAssignment &a, const core::ToolGeometry &g,
                               double tip, double lift, std::stop_token stop) {
    if (!std::isfinite(lift) || lift <= 0)
        throw std::runtime_error("Invalid DXF punch lift");
    const auto &p = *a.punch.drawing;
    const double z = (a.die.lengthMm - a.punch.lengthMm) / 2;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(sectionPrism(p.edges, g.punchTip, tip, a.punch.lengthMm, z));
    // Sweep of a region = original region union the swept boundary. Split circular
    // edges at X extrema: each strip then is the region between y(x) and y(x)+lift.
    for (const auto &e : p.edges) {
        std::vector<double> cuts{0, 1};
        if (e.center) {
            const double start = std::atan2(e.start.y - e.center->y, e.start.x - e.center->x);
            for (int k = -4; k <= 4; ++k) {
                const double f = (k * std::numbers::pi - start) / e.sweepRadians;
                if (f > 1.e-10 && f < 1 - 1.e-10)
                    cuts.push_back(f);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        for (std::size_t i = 1; i < cuts.size(); ++i) {
            if (stop.stop_requested())
                throw std::runtime_error("DXF punch sweep cancelled");
            auto edge = e;
            if (e.center) {
                edge.start = at(e, cuts[i - 1]);
                edge.end = at(e, cuts[i]);
                edge.sweepRadians = e.sweepRadians * (cuts[i] - cuts[i - 1]);
            }
            if (std::abs(edge.end.x - edge.start.x) < 1.e-10)
                continue;
            const DrawingPoint upStart{edge.start.x, edge.start.y + lift},
                upEnd{edge.end.x, edge.end.y + lift};
            auto back = edge;
            back.start = upEnd;
            back.end = upStart;
            back.sweepRadians = -edge.sweepRadians;
            if (back.center)
                back.center->y += lift;
            tools.Append(
                sectionPrism({edge, line(edge.end, upEnd), back, line(upStart, edge.start)},
                             g.punchTip, tip, a.punch.lengthMm, z));
        }
    }
    if (tools.IsEmpty())
        throw std::runtime_error("Empty DXF punch sweep");
    BRepAlgoAPI_Fuse fuse;
    fuse.SetArguments(arguments);
    fuse.SetTools(tools);
    fuse.SetNonDestructive(true);
    fuse.SetFuzzyValue(1.e-7);
    fuse.Build();
    if (!fuse.IsDone() || fuse.HasErrors() || !BRepCheck_Analyzer(fuse.Shape()).IsValid())
        throw std::runtime_error("Cannot validate DXF punch sweep");
    return fuse.Shape();
}
} // namespace bending::cad
