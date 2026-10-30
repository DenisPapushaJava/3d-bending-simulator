#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/ToolProfileValidation.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeAnalysis_Wire.hxx>
#include <ShapeExtend_Status.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Vec.hxx>
#include <numbers>
#include <stdexcept>
#include <vector>
namespace bending::cad {
TopoDS_Shape orientPunch(const TopoDS_Shape &shape, const core::PressTools &tools) {
    if (!tools.punchReversed || shape.IsNull())
        return shape;
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, tools.lengthMm / 2), gp_Dir(0, 1, 0)),
                         std::numbers::pi);
    return shape.Moved(TopLoc_Location(rotation));
}
void orientPunchSegments(std::vector<ToolDisplaySegment> &segments, const core::PressTools &tools) {
    for (auto &segment : segments)
        segment.shape = orientPunch(segment.shape, tools);
}
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
std::optional<std::string> validateToolProfile(const core::ToolDrawing &drawing,
                                               std::stop_token stop) {
    if (stop.stop_requested())
        return "Проверка отменена.";
    if (!drawing.millimeters || !core::validToolDrawing(drawing))
        return "Некорректный контур или единицы. Требуется замкнутое сечение в мм.";
    try {
        const auto body = sectionPrism(drawing.edges, {}, 0, 1, 0);
        if (body.IsNull() || stop.stop_requested())
            return "Проверка отменена или тело не построено.";
        GProp_GProps properties;
        BRepGProp::VolumeProperties(body, properties);
        if (!std::isfinite(properties.Mass()) || std::abs(properties.Mass()) <= 1.e-10)
            return "Площадь сечения нулевая или не подтверждена.";
        return {};
    } catch (...) {
        return "Сечение не образует корректное тело. Проверьте пересечения, дубликаты и дуги.";
    }
}
std::array<TopoDS_Shape, 2> makeDrawingTools(const core::ToolAssignment &a,
                                             const core::ToolGeometry &g, double tip) {
    std::array<TopoDS_Shape, 2> result;
    if (g.hasDie)
        result[0] = sectionPrism(core::activeDieDrawing(a.die, a.groove)->edges, g.dieTopCentre, 0,
                                 *core::toolLength(a.die, a.dieAssembly), 0);
    if (g.hasPunch)
        result[1] = sectionPrism(a.punch.drawing->edges, g.punchTip, tip,
                                 *core::toolLength(a.punch, a.punchAssembly),
                                 (*core::toolLength(a.die, a.dieAssembly) -
                                  *core::toolLength(a.punch, a.punchAssembly)) /
                                     2);
    return result;
}
TopoDS_Shape segmentToolBody(const TopoDS_Shape &whole, const core::ToolPreset &tool,
                             const std::optional<core::ToolAssembly> &assembly, double startZ,
                             std::stop_token stop, std::vector<ToolDisplaySegment> *display) {
    if (display)
        display->clear();
    if (!assembly) {
        if (display)
            display->push_back({whole, tool.lengthMm});
        return whole;
    }
    if (!tool.segments || !core::assemblyLength(*tool.segments, *assembly))
        throw std::runtime_error("Invalid tool assembly");
    Bnd_Box box;
    BRepBndLib::AddOptimal(whole, box, false, false);
    const auto lo = box.CornerMin(), hi = box.CornerMax();
    BRep_Builder builder;
    TopoDS_Compound parts;
    builder.MakeCompound(parts);
    for (const auto row : assembly->pieces) {
        if (stop.stop_requested())
            return {};
        const double length = (*tool.segments)[row].lengthMm;
        const auto slab = BRepPrimAPI_MakeBox(gp_Pnt(lo.X() - 1, lo.Y() - 1, startZ),
                                              hi.X() - lo.X() + 2, hi.Y() - lo.Y() + 2, length)
                              .Shape();
        BRepAlgoAPI_Common cut;
        TopTools_ListOfShape args, tools;
        args.Append(whole);
        tools.Append(slab);
        cut.SetArguments(args);
        cut.SetTools(tools);
        cut.SetNonDestructive(true);
        cut.SetFuzzyValue(1.e-7);
        cut.Build();
        if (!cut.IsDone() || cut.HasErrors() || !BRepCheck_Analyzer(cut.Shape()).IsValid())
            throw std::runtime_error("Cannot build an individual tool segment");
        GProp_GProps properties;
        BRepGProp::VolumeProperties(cut.Shape(), properties);
        if (!std::isfinite(properties.Mass()) || std::abs(properties.Mass()) <= 1.e-10)
            throw std::runtime_error("Tool segment has no valid positive volume");
        builder.Add(parts, cut.Shape());
        if (display)
            display->push_back({cut.Shape(), length});
        startZ += length + assembly->gapMm;
    }
    return parts;
}
TopoDS_Shape drawingPunchSweep(const core::ToolAssignment &a, const core::ToolGeometry &g,
                               double tip, double lift, std::stop_token stop) {
    if (!g.hasPunch || !a.punch.drawing || !std::isfinite(lift) || lift <= 0)
        throw std::runtime_error("Invalid DXF punch lift");
    const auto &p = *a.punch.drawing;
    const double z =
        (*core::toolLength(a.die, a.dieAssembly) - *core::toolLength(a.punch, a.punchAssembly)) / 2;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(
        sectionPrism(p.edges, g.punchTip, tip, *core::toolLength(a.punch, a.punchAssembly), z));
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
                             g.punchTip, tip, *core::toolLength(a.punch, a.punchAssembly), z));
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
