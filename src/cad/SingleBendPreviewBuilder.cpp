#include "bending/cad/SingleBendPreviewBuilder.h"
#include "BendOperation.h"
#include "CadPreview.h"
#include "CollisionHighlight.h"
#include "GeometryCacheInternal.h"
#include "RouteGeometry.h"
#include "TrimmedBend.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/MachineAssembly.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/simulation/ToolProfiles.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bending::cad {
namespace {
using namespace application;

gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}
double area(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(shape, properties);
    return properties.Mass();
}
struct FrameBasis {
    gp_Pnt origin;
    gp_Vec x;
    gp_Vec y;
    gp_Vec z;
};
gp_Pnt sectionPoint(const FrameBasis &basis, simulation::Point2Mm p) {
    return basis.origin.Translated(basis.x * p.x + basis.y * p.y);
}
// Work on a deep copy: even meshing must never update the imported topology.
TopoDS_Shape visibleSource(const TopoDS_Shape &source) {
    auto copy = BRepBuilderAPI_Copy(source, true, false).Shape();
    BRepMesh_IncrementalMesh mesh(copy, 0.03, false, 0.12, false);
    if (!mesh.IsDone()) {
        throw std::runtime_error("Cannot mesh source cutouts for display");
    }
    return copy;
}
void makeDisplay(PreviewFrame &frame, const TopoDS_Shape &source,
                 const simulation::SingleBend &model, const FrameBasis &from, const FrameBasis &to,
                 const simulation::BendSection &section, std::stop_token stop) {
    BRep_Builder builder;
    TopoDS_Compound assembly;
    builder.MakeCompound(assembly);
    for (auto &region : frame.displayRegions) {
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        region = compound;
    }
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(source, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
        if (stop.stop_requested()) {
            return;
        }
        const auto &face = TopoDS::Face(faces(i));
        TopLoc_Location location;
        const auto original = BRep_Tool::Triangulation(face, location);
        if (original.IsNull()) {
            throw std::runtime_error("Missing source triangulation");
        }
        Handle(Poly_Triangulation) mesh =
            new Poly_Triangulation(original->NbNodes(), original->NbTriangles(), false);
        double meanPhi = 0;
        for (int n = 1; n <= original->NbNodes(); ++n) {
            const auto p = original->Node(n).Transformed(location.Transformation());
            const gp_Vec offset(from.origin, p);
            const double x = offset.Dot(from.x), y = offset.Dot(from.y);
            const auto mapped = simulation::mapBendPoint(model, {x, y}, section);
            if (!mapped) {
                throw std::runtime_error("Cannot map source surface into bend section");
            }
            mesh->SetNode(n, sectionPoint(to, *mapped).Translated(to.z * offset.Dot(from.z)));
            double phi = std::atan2(x, model.innerRadiusMm - y);
            if (phi < -std::numbers::pi / 2) {
                phi += 2 * std::numbers::pi;
            }
            meanPhi += phi / original->NbNodes();
        }
        for (int t = 1; t <= original->NbTriangles(); ++t) {
            mesh->SetTriangle(t, original->Triangle(t));
        }
        mesh->Deflection(original->Deflection());
        TopoDS_Face display;
        builder.MakeFace(display, mesh);
        display.Orientation(face.Orientation());
        const std::size_t region = meanPhi < -1.0e-7                       ? 0
                                   : meanPhi > model.angleRadians + 1.0e-7 ? 2
                                                                           : 1;
        builder.Add(frame.displayRegions[region], display);
    }
    for (const auto &region : frame.displayRegions) {
        builder.Add(assembly, region);
    }
    for (const auto &tool : frame.tools) {
        if (!tool.IsNull()) {
            builder.Add(assembly, tool);
        }
    }
    frame.displayShape = assembly;
}
gp_Ax3 flangeAxes(const FrameBasis &basis, const simulation::BendSection &section, bool second) {
    const auto root = second ? section.moving[0] : section.fixed[0];
    const auto end = second ? section.moving[3] : section.fixed[1];
    return {sectionPoint(basis, root), gp_Dir(basis.z),
            gp_Dir(gp_Vec(sectionPoint(basis, root), sectionPoint(basis, end)))};
}
double flangeExtent(const TopoDS_Shape &inner, const TopoDS_Shape &outer, const gp_Ax3 &axes) {
    gp_Trsf local;
    local.SetTransformation(axes);
    Bnd_Box bounds;
    for (const auto &face : {inner, outer}) {
        const auto transformed = BRepBuilderAPI_Transform(face, local, true).Shape();
        BRepBndLib::AddOptimal(transformed, bounds, false, false);
    }
    return bounds.CornerMax().X();
}
void assemble(PreviewFrame &frame) {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto &region : frame.regions) {
        if (!BRepCheck_Analyzer(region).IsValid()) {
            throw std::runtime_error("Invalid preview region");
        }
        builder.Add(compound, region);
    }
    frame.shape = compound;
}
// Keep exact flange solids of the checked source (filled copy in approximation mode).
// The original cutouts then remain in the separate display surface.
// Every frame owns fresh geometry: meshing may not mutate the imported document.
void placeFlanges(PreviewFrame &frame, const std::array<TopoDS_Shape, 2> &flanges,
                  const FrameBasis &sourceBasis, const simulation::BendSection &sourceSection,
                  const FrameBasis &targetBasis, const simulation::BendSection &targetSection) {
    for (std::size_t i = 0; i < flanges.size(); ++i) {
        gp_Trsf placement;
        placement.SetDisplacement(flangeAxes(sourceBasis, sourceSection, i == 1),
                                  flangeAxes(targetBasis, targetSection, i == 1));
        frame.materialTransforms[i] = placement;
        frame.regions[i * 2] = BRepBuilderAPI_Transform(flanges[i], placement, true).Shape();
    }
    assemble(frame);
}
std::optional<TopoDS_Shape> extractFlange(const TopoDS_Shape &source, const TopoDS_Shape &envelope,
                                          double tolerance) {
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(source);
    tools.Append(envelope);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetFuzzyValue(tolerance);
    common.Build();
    if (!common.IsDone() || common.HasErrors() || !BRepCheck_Analyzer(common.Shape()).IsValid()) {
        return {};
    }
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(common.Shape(), TopAbs_SOLID, solids);
    if (solids.Extent() != 1 || !std::isfinite(volume(solids(1))) || volume(solids(1)) <= 0) {
        return {};
    }
    return BRepBuilderAPI_Copy(solids(1), true, false).Shape();
}
// Removing one complete zone from an open sheet must yield exactly two material components.
// Keep all geometry of those components, including the already formed other bends.
std::optional<std::array<TopoDS_Shape, 2>> splitAtBend(const TopoDS_Shape &source,
                                                       const PreviewFrame &seed, double tolerance,
                                                       std::stop_token stop) {
    BRepAlgoAPI_Cut cut;
    TopTools_ListOfShape args, tools;
    args.Append(source);
    tools.Append(seed.regions[1]);
    cut.SetArguments(args);
    cut.SetTools(tools);
    cut.SetNonDestructive(true);
    cut.SetFuzzyValue(tolerance);
    cut.Build();
    if (stop.stop_requested() || !cut.IsDone() || cut.HasErrors() ||
        !BRepCheck_Analyzer(cut.Shape()).IsValid())
        return {};
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(cut.Shape(), TopAbs_SOLID, solids);
    if (solids.Extent() != 2)
        return {};
    std::array<TopoDS_Shape, 2> result;
    for (int i = 1; i <= 2; ++i) {
        std::optional<std::size_t> side;
        for (std::size_t s = 0; s < 2; ++s) {
            const auto overlap = extractFlange(solids(i), seed.regions[s * 2], tolerance);
            if (overlap && volume(*overlap) > 1.e-6) {
                if (side)
                    return {};
                side = s;
            }
        }
        if (!side || !result[*side].IsNull())
            return {};
        result[*side] = BRepBuilderAPI_Copy(solids(i), true, false).Shape();
    }
    return result[0].IsNull() || result[1].IsNull() ? std::nullopt : std::optional{result};
}
struct SkinNode {
    gp_Pnt point;
    unsigned regions = 0;
};
struct SkinFace {
    Handle(Poly_Triangulation) original;
    TopAbs_Orientation orientation;
    std::vector<SkinNode> nodes;
    std::size_t region = 0;
};
using WholePartSkin = std::vector<SkinFace>;
WholePartSkin prepareWholePartSkin(const TopoDS_Shape &source,
                                   const std::array<TopoDS_Shape, 2> &flanges,
                                   const TopoDS_Shape &sector, double tolerance,
                                   std::stop_token stop) {
    // Membership is in immutable SOURCE components and does not depend on the
    // displayed angle. Preserve all rail memberships; never cache across operations.
    WholePartSkin result;
    std::array<BRepClass3d_SolidClassifier, 3> classifiers{BRepClass3d_SolidClassifier(flanges[0]),
                                                           BRepClass3d_SolidClassifier(sector),
                                                           BRepClass3d_SolidClassifier(flanges[1])};
    for (TopExp_Explorer f(source, TopAbs_FACE); f.More(); f.Next()) {
        if (stop.stop_requested())
            return {};
        const auto face = TopoDS::Face(f.Current());
        TopLoc_Location location;
        const auto original = BRep_Tool::Triangulation(face, location);
        if (original.IsNull())
            throw std::runtime_error("Missing original surface mesh");
        SkinFace skin{original, face.Orientation(), {}, 0};
        skin.nodes.reserve(static_cast<std::size_t>(original->NbNodes()));
        std::array<unsigned, 3> votes{};
        for (int n = 1; n <= original->NbNodes(); ++n) {
            if ((n & 255) == 0 && stop.stop_requested())
                return {};
            const auto point = original->Node(n).Transformed(location.Transformation());
            unsigned regions = 0;
            for (std::size_t r = 0; r < 3; ++r) {
                classifiers[r].Perform(point, tolerance * 10);
                if (classifiers[r].State() != TopAbs_IN && classifiers[r].State() != TopAbs_ON)
                    continue;
                regions |= 1u << r;
                ++votes[r];
            }
            if (!regions)
                throw std::runtime_error(
                    "Source skin is outside the two checked components and bend");
            skin.nodes.push_back({point, regions});
        }
        skin.region =
            static_cast<std::size_t>(std::max_element(votes.begin(), votes.end()) - votes.begin());
        result.push_back(std::move(skin));
    }
    return result;
}
void makeWholePartDisplay(PreviewFrame &frame, const WholePartSkin &source,
                          const simulation::SingleBend &model, const FrameBasis &from,
                          const FrameBasis &to, const simulation::BendSection &section,
                          std::stop_token stop) {
    const auto final = *simulation::singleBendSection(model, 1);
    std::array<gp_Trsf, 2> maps;
    for (std::size_t s = 0; s < 2; ++s)
        maps[s].SetDisplacement(flangeAxes(from, final, s == 1), flangeAxes(to, section, s == 1));
    BRep_Builder builder;
    for (auto &region : frame.displayRegions) {
        TopoDS_Compound c;
        builder.MakeCompound(c);
        region = c;
    }
    for (const auto &face : source) {
        if (stop.stop_requested())
            return;
        const auto &original = face.original;
        Handle(Poly_Triangulation) mesh =
            new Poly_Triangulation(original->NbNodes(), original->NbTriangles(), false);
        for (std::size_t n = 0; n < face.nodes.size(); ++n) {
            const auto &[point, regions] = face.nodes[n];
            std::optional<gp_Pnt> mapped;
            for (std::size_t r = 0; r < 3; ++r) {
                if (!(regions & (1u << r)))
                    continue;
                gp_Pnt next;
                if (r == 1) {
                    const gp_Vec v(from.origin, point);
                    const auto local =
                        simulation::mapBendPoint(model, {v.Dot(from.x), v.Dot(from.y)}, section);
                    if (!local)
                        throw std::runtime_error("Cannot map active zone surface");
                    next = sectionPoint(to, *local).Translated(to.z * v.Dot(from.z));
                } else
                    next = point.Transformed(maps[r / 2]);
                if (mapped && mapped->Distance(next) > .02)
                    throw std::runtime_error(
                        "Inconsistent surface mapping at the active bend rail");
                mapped = next;
            }
            if (!mapped)
                throw std::runtime_error(
                    "Source skin is outside the two checked components and bend");
            mesh->SetNode(static_cast<int>(n + 1), *mapped);
        }
        for (int t = 1; t <= original->NbTriangles(); ++t)
            mesh->SetTriangle(t, original->Triangle(t));
        mesh->Deflection(original->Deflection());
        TopoDS_Face skin;
        builder.MakeFace(skin, mesh);
        skin.Orientation(face.orientation);
        builder.Add(frame.displayRegions[face.region], skin);
    }
    TopoDS_Compound assembly;
    builder.MakeCompound(assembly);
    for (const auto &region : frame.displayRegions)
        builder.Add(assembly, region);
    for (const auto &tool : frame.tools)
        if (!tool.IsNull())
            builder.Add(assembly, tool);
    frame.displayShape = assembly;
}
std::optional<gp_Vec> railRadial(const TopoDS_Face &cylinder, const TopoDS_Face &flange,
                                 const gp_Pnt &axisOrigin, const gp_Vec &axis, double tolerance) {
    TopTools_IndexedMapOfShape cylinderEdges, flangeEdges;
    TopExp::MapShapes(cylinder, TopAbs_EDGE, cylinderEdges);
    TopExp::MapShapes(flange, TopAbs_EDGE, flangeEdges);
    std::optional<gp_Vec> radial;
    for (int i = 1; i <= cylinderEdges.Extent(); ++i) {
        if (!flangeEdges.Contains(cylinderEdges(i))) {
            continue;
        }
        const BRepAdaptor_Curve curve(TopoDS::Edge(cylinderEdges(i)));
        if (curve.GetType() != GeomAbs_Line ||
            !curve.Line().Direction().IsParallel(gp_Dir(axis), 1.0e-5) || radial) {
            return {};
        }
        const gp_Vec offset(axisOrigin, curve.Value(curve.FirstParameter()));
        const auto value = offset - axis * offset.Dot(axis);
        if (value.Magnitude() <= tolerance) {
            return {};
        }
        radial = value.Normalized();
    }
    return radial;
}
TopoDS_Shape prism(const std::vector<gp_Pnt> &points, const gp_Vec &extrusion) {
    BRepBuilderAPI_MakeWire wire;
    for (std::size_t i = 0; i < points.size(); ++i) {
        wire.Add(BRepBuilderAPI_MakeEdge(points[i], points[(i + 1) % points.size()]).Edge());
    }
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), extrusion).Shape();
}
PreviewFrame makeSectionFrame(const simulation::SingleBend &model, const FrameBasis &basis,
                              const simulation::BendSection &state) {
    const auto position = [&](simulation::Point2Mm p) {
        return basis.origin.Translated(basis.x * p.x + basis.y * p.y);
    };
    const auto extrusion = basis.z * model.widthMm;
    const auto flange = [&](const auto &corners) {
        std::vector<gp_Pnt> points;
        for (const auto p : corners) {
            points.push_back(position(p));
        }
        return prism(points, extrusion);
    };
    PreviewFrame frame;
    frame.dimensions = BendDimensionAnchors{{position(state.fixed[0]), position(state.fixed[1])},
                                            {position(state.moving[0]), position(state.moving[3])},
                                            position(state.innerArc[1]),
                                            extrusion,
                                            state.angleRadians,
                                            state.innerRadiusMm};
    frame.regions[0] = flange(state.fixed);
    frame.regions[2] = flange(state.moving);
    if (state.angleRadians == 0.0) {
        frame.regions[1] = prism({position(state.innerArc[0]), position(state.outerArc[0]),
                                  position(state.outerArc[2]), position(state.innerArc[2])},
                                 extrusion);
    } else {
        const auto arc = [&](const auto &points) {
            return BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(position(points[0]),
                                                              position(points[1]),
                                                              position(points[2]))
                                               .Value())
                .Edge();
        };
        BRepBuilderAPI_MakeWire wire;
        wire.Add(BRepBuilderAPI_MakeEdge(position(state.innerArc[0]), position(state.outerArc[0]))
                     .Edge());
        wire.Add(arc(state.outerArc));
        wire.Add(BRepBuilderAPI_MakeEdge(position(state.outerArc[2]), position(state.innerArc[2]))
                     .Edge());
        wire.Add(TopoDS::Edge(arc(state.innerArc).Reversed()));
        frame.regions[1] =
            BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), extrusion).Shape();
    }
    assemble(frame);
    return frame;
}
PreviewFrame makeFrame(const simulation::SingleBend &model, const FrameBasis &basis,
                       double progress) {
    const auto state = simulation::singleBendSection(model, progress);
    if (!state) {
        throw std::invalid_argument("Unsupported single-bend parameters");
    }
    return makeSectionFrame(model, basis, *state);
}
// The profile width is nondecreasing with Y. Its exact upward translational
// sweep is the original profile plus the vertical strip above its full-width top.
TopoDS_Shape buildPunch(const core::PressTools &t, double tip, double extension = 0.0) {
    const auto profile = simulation::toolProfiles(t).value();
    const auto point = [tip](simulation::Point2Mm p) { return gp_Pnt(p.x, p.y + tip, 0); };
    const auto leftTop = point(profile.punch[0]), rightTop = point(profile.punch[4]);
    const auto left = point(profile.punch[1]), right = point(profile.punch[3]);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(leftTop, left).Edge());
    wire.Add(
        BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(left, gp_Pnt(0, tip, 0), right).Value()).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(right, rightTop).Edge());
    if (extension > 0) {
        const gp_Vec lift(0, extension, 0);
        wire.Add(BRepBuilderAPI_MakeEdge(rightTop, rightTop.Translated(lift)).Edge());
        wire.Add(
            BRepBuilderAPI_MakeEdge(rightTop.Translated(lift), leftTop.Translated(lift)).Edge());
        wire.Add(BRepBuilderAPI_MakeEdge(leftTop.Translated(lift), leftTop).Edge());
    } else {
        wire.Add(BRepBuilderAPI_MakeEdge(rightTop, leftTop).Edge());
    }
    gp_Trsf center;
    center.SetTranslation(gp_Vec(0, 0, (t.lengthMm - core::punchLengthMm(t)) / 2));
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(),
                                 gp_Vec(0, 0, core::punchLengthMm(t)))
        .Shape()
        .Moved(TopLoc_Location(center));
}
TopoDS_Shape makePunch(const core::PressTools &t, double tip, double extension = 0.0) {
    auto canonical = t;
    canonical.lengthMm = core::punchLengthMm(t);
    auto key = geometryKey("ideal-punch");
    key << canonical.lengthMm << ' ' << extension;
    const auto profile = simulation::toolProfiles(canonical).value();
    for (const auto &p : profile.punch)
        key << ' ' << p.x << ' ' << p.y;
    auto shape = cachedToolShape(
        key.str(), [&] { return buildPunch(canonical, 0, extension); }, extension > 0 ? 0 : .05);
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(0, tip, (t.lengthMm - core::punchLengthMm(t)) / 2));
    return shape.Moved(TopLoc_Location(translation));
}
std::array<TopoDS_Shape, 2> makeTools(const simulation::FlatToolSetup &setup, double thickness,
                                      std::array<bool, 2> required = {true, true}) {
    const auto &t = setup.tools;
    const auto profile = simulation::toolProfiles(t).value();
    std::array<TopoDS_Shape, 2> result;
    if (required[0]) {
        auto key = geometryKey("ideal-die");
        key << t.lengthMm;
        std::vector<gp_Pnt> contour;
        for (const auto p : profile.die) {
            contour.emplace_back(p.x, p.y, 0);
            key << ' ' << p.x << ' ' << p.y;
        }
        result[0] =
            cachedToolShape(key.str(), [&] { return prism(contour, gp_Vec(0, 0, t.lengthMm)); });
    }
    if (required[1])
        result[1] = makePunch(t, thickness + t.clearanceMm);
    return result;
}
bool disjointVolumes(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance, double budget,
                     bool *completed = nullptr, TopoDS_Shape *overlap = nullptr) {
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(a);
    tools.Append(b);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetFuzzyValue(tolerance);
    common.Build();
    const bool done =
        common.IsDone() && !common.HasErrors() && BRepCheck_Analyzer(common.Shape()).IsValid();
    const double amount = done ? volume(common.Shape()) : 0;
    const bool valid = done && std::isfinite(amount);
    if (completed)
        *completed = valid;
    if (overlap && valid && amount > budget)
        *overlap = common.Shape();
    return valid && amount <= budget;
}
bool sameVolume(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance, double budget,
                std::stop_token stop) {
    for (const bool reverse : {false, true}) {
        if (stop.stop_requested()) {
            return false;
        }
        BRepAlgoAPI_Cut cut;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(reverse ? b : a);
        tools.Append(reverse ? a : b);
        cut.SetArguments(arguments);
        cut.SetTools(tools);
        cut.SetNonDestructive(true);
        cut.SetFuzzyValue(tolerance);
        cut.Build();
        if (!cut.IsDone() || cut.HasErrors() || !BRepCheck_Analyzer(cut.Shape()).IsValid()) {
            return false;
        }
        const double residual = volume(cut.Shape());
        if (!std::isfinite(residual) || residual > budget) {
            return false;
        }
    }
    return true;
}
} // namespace
TopoDS_Shape makeRouteSector(const simulation::SingleBend &m, const gp_Ax3 &axes, double p) {
    return makeFrame(m,
                     {axes.Location(), gp_Vec(axes.XDirection()), gp_Vec(axes.YDirection()),
                      gp_Vec(axes.Direction())},
                     p)
        .regions[1];
}
std::array<TopoDS_Shape, 2>
makeRouteDraftTools(const core::PressTools &tools,
                    const std::optional<core::ToolAssignment> &assignment, std::stop_token stop,
                    std::array<std::vector<ToolDisplaySegment>, 2> &segments) {
    std::array<TopoDS_Shape, 2> result;
    const bool drawn = assignment && assignment->useDrawingGeometry;
    const auto g = drawn ? core::drawingToolGeometry(*assignment) : std::nullopt;
    result = makeTools({tools, 0, 0, 0, 0, 0}, 0, {!g || !g->hasDie, !g || !g->hasPunch});
    if (assignment && assignment->useDrawingGeometry) {
        const auto geometry = core::drawingToolGeometry(*assignment);
        if (!geometry)
            throw std::runtime_error("Cannot place this DXF profile");
        const auto drawn = makeDrawingTools(*assignment, *geometry, tools.clearanceMm);
        for (std::size_t i = 0; i < 2; ++i)
            if (!drawn[i].IsNull())
                result[i] = drawn[i];
    }
    if (assignment) {
        result[0] = segmentToolBody(result[0], assignment->die, assignment->dieAssembly, 0, stop,
                                    &segments[0]);
        result[1] =
            segmentToolBody(result[1], assignment->punch, assignment->punchAssembly,
                            (tools.lengthMm - core::punchLengthMm(tools)) / 2, stop, &segments[1]);
    } else {
        segments[0].push_back({result[0], tools.lengthMm});
        segments[1].push_back({result[1], core::punchLengthMm(tools)});
    }
    result[1] = orientPunch(result[1], tools);
    orientPunchSegments(segments[1], tools);
    return result;
}
const PreviewFrame &OcctPreviewAccess::frame(const application::SingleBendPreview &preview,
                                             std::size_t index) {
    return dynamic_cast<const CadPreview &>(preview).frames.at(index);
}
const TopoDS_Shape &
OcctPreviewAccess::retractionSweep(const application::SingleBendPreview &preview) {
    return dynamic_cast<const CadPreview &>(preview).sweep;
}
const std::array<TopoDS_Shape, 2> &
OcctPreviewAccess::sourceComponents(const application::SingleBendPreview &preview) {
    return dynamic_cast<const CadPreview &>(preview).components;
}
application::BendPreviewResult
buildBendOperation(DocumentPtr document, std::size_t bendIndex, core::BendTarget target,
                   bool fixedSecond, std::stop_token stop,
                   std::optional<core::PressTools> toolParameters, bool withMotion,
                   std::optional<core::ToolAssignment> realTools, bool allowPartialEndSupport,
                   std::optional<double> maximumEndSupportLossMm) {
    const auto unsupported = [](const char *detail) -> BendPreviewResult {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedGeometry, detail};
    };
    const auto cancelled = []() -> BendPreviewResult {
        return BendPreviewError{BendPreviewErrorCode::Cancelled, "Preview cancelled"};
    };
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (maximumEndSupportLossMm && !validEndSupportLimitMm(*maximumEndSupportLossMm))
        return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                "Invalid end support limit"};
    const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
    if (!native) {
        return unsupported("Unsupported document type");
    }
    const auto &metadata = native->metadata();
    const auto &analysis = OcctDocumentAccess::simulationBends(*native);
    if (!analysis.interiorContourDiagnostics.empty()) {
        return BendPreviewError{BendPreviewErrorCode::BendZoneInteriorContours,
                                "Enclosed opening boundaries interrupt a cylindrical bend zone"};
    }
    if (analysis.status != BendRecognitionStatus::Completed || bendIndex >= analysis.bends.size()) {
        return unsupported("Requires a complete selected straight bend");
    }
    const auto &bend = analysis.bends[bendIndex];
    const bool multi = analysis.bends.size() > 1 || OcctDocumentAccess::isRouteState(*native);
    if (!core::validBendTarget(target) ||
        std::abs(target.angleRadians - bend.angleRadians) > analysis.angleToleranceRadians ||
        std::abs(target.innerRadiusMm - bend.innerRadiusMm) > analysis.lengthToleranceMm) {
        return BendPreviewError{BendPreviewErrorCode::TargetMismatch,
                                "Target must match the imported final shape"};
    }
    try {
        const auto &source = OcctDocumentAccess::simulationShape(*native);
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(source, TopAbs_FACE, faces);
        const auto &cylinder = TopoDS::Face(faces(static_cast<int>(bend.innerFace)));
        const auto &flangeA = TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[0])));
        const auto &flangeB = TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[2])));
        auto axis = gp_Vec(point(bend.axisStart), point(bend.axisEnd)).Normalized();
        auto radialA =
            railRadial(cylinder, flangeA, point(bend.axisStart), axis, analysis.lengthToleranceMm);
        auto radialB =
            railRadial(cylinder, flangeB, point(bend.axisStart), axis, analysis.lengthToleranceMm);
        if (!radialA || !radialB) {
            return unsupported("Cannot determine tangency rails");
        }
        if (fixedSecond) {
            std::swap(radialA, radialB);
        }
        if (radialA->Crossed(*radialB).Dot(axis) < 0.0) {
            axis.Reverse();
        }
        const auto origin = axis.Dot(gp_Vec(point(bend.axisStart), point(bend.axisEnd))) > 0.0
                                ? point(bend.axisStart)
                                : point(bend.axisEnd);
        FrameBasis basis{origin.Translated(*radialA * bend.innerRadiusMm), axis.Crossed(*radialA),
                         -*radialA, axis};
        const FrameBasis sourceBasis = basis;
        simulation::SingleBend model{
            bend.thicknessMm, bend.innerRadiusMm, bend.angleRadians, bend.lengthMm, 1, 1};
        const auto seed = simulation::singleBendSection(model, 1.0);
        if (!seed) {
            return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                    "Unsupported bend dimensions/angle"};
        }
        const std::array<std::size_t, 2> innerIndices{fixedSecond ? 2U : 0U, fixedSecond ? 0U : 2U};
        std::array<double, 2> extents{};
        for (std::size_t i = 0; i < extents.size(); ++i) {
            const auto index = innerIndices[i];
            extents[i] = flangeExtent(faces(static_cast<int>(bend.flangeFaces[index])),
                                      faces(static_cast<int>(bend.flangeFaces[index + 1])),
                                      flangeAxes(basis, *seed, i == 1));
        }
        model.fixedFlangeMm = extents[0];
        model.movingFlangeMm = extents[1];
        if (!simulation::validSingleBend(model)) {
            return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                    "Supported sizes: thickness/radius 0.1–100000 mm, "
                                    "width/flanges 1–100000 mm, angle 5–175 degrees"};
        }
        const auto sourceSection = *simulation::singleBendSection(model, 1.0);
        auto finalFrame = makeFrame(model, basis, 1.0);
        if (bend.trimmedEnds) {
            const auto region =
                extractFlange(source, finalFrame.regions[1], analysis.lengthToleranceMm);
            if (!region)
                return unsupported("Cannot isolate the trimmed bend region");
            finalFrame.regions[1] = *region;
            const auto radial = mapTrimmedBend(
                *region, [](gp_Pnt p) { return p; }, 1, analysis.lengthToleranceMm, nullptr, stop);
            if (!sameVolume(*region, radial, analysis.lengthToleranceMm,
                            std::max(1e-4, volume(source) * 1e-7), stop))
                return unsupported("The trimmed end is not a supported radial ruled surface");
        }
        const auto sourceSector = finalFrame.regions[1];
        std::array<TopoDS_Shape, 2> flanges;
        if (multi) {
            const auto split = splitAtBend(source, finalFrame, analysis.lengthToleranceMm, stop);
            if (stop.stop_requested())
                return cancelled();
            if (!split)
                return unsupported("The selected zone does not separate the whole part into two "
                                   "supported components");
            flanges = *split;
            for (std::size_t s = 0; s < 2; ++s)
                finalFrame.regions[s * 2] = flanges[s];
        } else
            for (std::size_t i = 0; i < flanges.size(); ++i) {
                if (stop.stop_requested()) {
                    return cancelled();
                }
                const auto extracted =
                    extractFlange(source, finalFrame.regions[i * 2], analysis.lengthToleranceMm);
                if (!extracted) {
                    return unsupported("Cannot isolate one connected solid for each planar flange");
                }
                flanges[i] = *extracted;
                finalFrame.regions[i * 2] = *extracted;
            }
        assemble(finalFrame);
        const double expectedVolume = volume(source);
        const double budget =
            std::max(expectedVolume * 1.0e-8, area(source) * analysis.lengthToleranceMm * 2.0);
        // Unchanged large walls must not hide a small collision near the active bend.
        const double collisionBudget =
            std::max(1.e-6, model.widthMm * model.thicknessMm * analysis.lengthToleranceMm * 2.);
        if (std::abs(volume(finalFrame.shape) - expectedVolume) > budget ||
            !sameVolume(source, finalFrame.shape, analysis.lengthToleranceMm, budget, stop)) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            return unsupported("Isolated flanges and complete bend do not match the entire STEP");
        }
        auto preview = std::make_shared<CadPreview>();
        preview->components = flanges;
        preview->otherBends = multi ? analysis.bends.size() - 1 : 0;
        preview->geometry = model;
        const auto toolGeometry = realTools && realTools->useDrawingGeometry
                                      ? core::drawingToolGeometry(*realTools)
                                      : std::nullopt;
        if (realTools && realTools->useDrawingGeometry && !toolGeometry)
            return BendPreviewError{
                BendPreviewErrorCode::UnsupportedToolGeometry,
                "Upright circular nose and symmetric rounded V groove required"};
        const auto shoulders =
            toolGeometry && toolGeometry->hasDie
                ? std::optional<simulation::DieShoulders>{{toolGeometry->shoulderHalfSpanMm,
                                                           toolGeometry->shoulderRadiusMm,
                                                           toolGeometry->leftSupportWidthMm,
                                                           toolGeometry->rightSupportWidthMm}}
                : std::nullopt;
        preview->realPunch = toolGeometry && toolGeometry->hasPunch;
        preview->realDie = toolGeometry && toolGeometry->hasDie;
        preview->filledCutouts = metadata.closedCutouts.status == ClosedCutoutStatus::Filled;
        if (realTools)
            preview->gaps = {realTools->dieAssembly ? realTools->dieAssembly->gapMm : 0,
                             realTools->punchAssembly ? realTools->punchAssembly->gapMm : 0};
        const auto displaySource = !OcctDocumentAccess::routeSkin(*native).IsNull()
                                       ? OcctDocumentAccess::routeSkin(*native)
                                   : preview->filledCutouts || bend.trimmedEnds
                                       ? visibleSource(OcctDocumentAccess::shape(*native))
                                       : TopoDS_Shape{};
        if (toolParameters) {
            const auto placement = simulation::flatToolSetup(model, *toolParameters,
                                                             analysis.lengthToleranceMm, shoulders);
            if (const auto *issue = std::get_if<simulation::ToolSetupIssue>(&placement)) {
                const auto code = *issue == simulation::ToolSetupIssue::ToolTooShort
                                      ? BendPreviewErrorCode::ToolTooShort
                                  : *issue == simulation::ToolSetupIssue::MissingLeftSupport
                                      ? BendPreviewErrorCode::MissingLeftSupport
                                  : *issue == simulation::ToolSetupIssue::MissingRightSupport
                                      ? BendPreviewErrorCode::MissingRightSupport
                                      : BendPreviewErrorCode::UnsupportedParameters;
                return BendPreviewError{code, "Initial tool setup parameters/support rejected"};
            }
            preview->setup = std::get<simulation::FlatToolSetup>(placement);
            if (withMotion) {
                const auto prepared = simulation::preparePressMotion(
                    model, *toolParameters, analysis.lengthToleranceMm, shoulders);
                if (const auto *issue = std::get_if<simulation::PressMotionIssue>(&prepared)) {
                    const auto code = *issue == simulation::PressMotionIssue::ToolAngleConflict
                                          ? BendPreviewErrorCode::ToolAngleConflict
                                      : *issue == simulation::PressMotionIssue::NoseTooLarge
                                          ? BendPreviewErrorCode::NoseTooLarge
                                      : *issue == simulation::PressMotionIssue::SupportNotProven
                                          ? BendPreviewErrorCode::SupportNotProven
                                          : BendPreviewErrorCode::UnsupportedParameters;
                    return BendPreviewError{code, "Contact motion parameters rejected"};
                }
                preview->motion = std::get<simulation::PressMotion>(prepared);
            }
            // The section model uses flange envelopes. Certify actual material under every
            // possible shoulder position, rather than inferring support from a bounding box.
            const double reach = preview->motion ? preview->motion->maximumSupportDistanceMm
                                                 : (toolGeometry && toolGeometry->hasDie
                                                        ? toolGeometry->shoulderHalfSpanMm
                                                        : toolParameters->openingMm / 2.0) -
                                                       simulation::bendAllowanceMm(model) / 2.0;
            if (reach > analysis.lengthToleranceMm) {
                auto supportSection = sourceSection;
                const double length = reach + analysis.lengthToleranceMm / 2.0;
                const auto shorten = [length](simulation::Point2Mm root, simulation::Point2Mm end,
                                              double extent) {
                    return simulation::Point2Mm{root.x + (end.x - root.x) * length / extent,
                                                root.y + (end.y - root.y) * length / extent};
                };
                supportSection.fixed[1] =
                    shorten(sourceSection.fixed[0], sourceSection.fixed[1], model.fixedFlangeMm);
                supportSection.fixed[2] =
                    shorten(sourceSection.fixed[3], sourceSection.fixed[2], model.fixedFlangeMm);
                supportSection.moving[3] =
                    shorten(sourceSection.moving[0], sourceSection.moving[3], model.movingFlangeMm);
                supportSection.moving[2] =
                    shorten(sourceSection.moving[1], sourceSection.moving[2], model.movingFlangeMm);
                const auto support = makeSectionFrame(model, sourceBasis, supportSection);
                const auto missingMaterial = [&](const TopoDS_Shape &region, std::size_t side) {
                    BRepAlgoAPI_Cut missing;
                    TopTools_ListOfShape arguments, tools;
                    arguments.Append(region);
                    tools.Append(flanges[side]);
                    missing.SetArguments(arguments);
                    missing.SetTools(tools);
                    missing.SetNonDestructive(true);
                    missing.SetFuzzyValue(analysis.lengthToleranceMm);
                    missing.Build();
                    if (!missing.IsDone() || missing.HasErrors() ||
                        !BRepCheck_Analyzer(missing.Shape()).IsValid() ||
                        !std::isfinite(volume(missing.Shape())))
                        throw std::runtime_error("Support-strip Boolean failed");
                    return missing.Shape();
                };
                double supportedFrom = 0, supportedTo = model.widthMm;
                bool partial = false;
                const double axialMargin = analysis.lengthToleranceMm * 4;
                for (std::size_t i = 0; i < flanges.size(); ++i) {
                    if (stop.stop_requested())
                        return cancelled();
                    const auto missing = missingMaterial(support.regions[i * 2], i);
                    const double supportBudget =
                        area(support.regions[i * 2]) * analysis.lengthToleranceMm * 2.0;
                    if (volume(missing) > supportBudget) {
                        if (!allowPartialEndSupport)
                            return BendPreviewError{BendPreviewErrorCode::SupportNotProven,
                                                    "A hole or contour interrupts the conservative "
                                                    "full-width support strip"};
                        partial = true;
                        gp_Trsf local;
                        local.SetTransformation(flangeAxes(sourceBasis, sourceSection, i == 1));
                        bool foundSolid = false;
                        for (TopExp_Explorer e(missing, TopAbs_SOLID); e.More(); e.Next()) {
                            foundSolid = true;
                            Bnd_Box bounds;
                            BRepBndLib::AddOptimal(
                                BRepBuilderAPI_Transform(e.Current(), local, true).Shape(), bounds,
                                false, false);
                            const auto low = bounds.CornerMin().Z(), high = bounds.CornerMax().Z();
                            const bool atStart = low <= axialMargin;
                            const bool atEnd = high >= model.widthMm - axialMargin;
                            if (atStart == atEnd)
                                return BendPreviewError{BendPreviewErrorCode::SupportNotProven,
                                                        "Support is interrupted inside the bend "
                                                        "span or along its entire length"};
                            if (atStart)
                                supportedFrom = std::max(supportedFrom, high + axialMargin);
                            else
                                supportedTo = std::min(supportedTo, low - axialMargin);
                        }
                        if (!foundSolid)
                            throw std::runtime_error("Cannot bound missing support material");
                    }
                }
                if (partial) {
                    // One common uninterrupted strip must contain the bend midpoint.
                    // This restricts a preview; it does not prove end support or stability.
                    if (supportedFrom >= model.widthMm / 2 - axialMargin ||
                        supportedTo <= model.widthMm / 2 + axialMargin)
                        return BendPreviewError{BendPreviewErrorCode::SupportNotProven,
                                                "No common central support strip on both flanges"};
                    const BendEndSupport report{model.widthMm, supportedFrom, supportedTo};
                    if (maximumEndSupportLossMm &&
                        !withinEndSupportLimit(report, *maximumEndSupportLossMm,
                                               axialMargin + 1.e-8)) {
                        BendPreviewError error{
                            BendPreviewErrorCode::EndSupportLimitExceeded,
                            "End support loss " + std::to_string(supportedFrom) + " / " +
                                std::to_string(model.widthMm - supportedTo) + " mm exceeds " +
                                std::to_string(*maximumEndSupportLossMm) + " mm per end"};
                        error.endSupport = report;
                        return error;
                    }
                    auto centralModel = model;
                    centralModel.widthMm = supportedTo - supportedFrom;
                    auto centralBasis = sourceBasis;
                    centralBasis.origin.Translate(sourceBasis.z * supportedFrom);
                    const auto central =
                        makeSectionFrame(centralModel, centralBasis, supportSection);
                    for (std::size_t i = 0; i < flanges.size(); ++i) {
                        if (stop.stop_requested())
                            return cancelled();
                        const auto missing = missingMaterial(central.regions[i * 2], i);
                        if (volume(missing) >
                            area(central.regions[i * 2]) * analysis.lengthToleranceMm * 2)
                            return BendPreviewError{
                                BendPreviewErrorCode::SupportNotProven,
                                "The bounded central support strip still loses material"};
                    }
                    preview->endSupport = report;
                }
            }
            basis = {{-simulation::bendAllowanceMm(model) / 2.0, model.thicknessMm,
                      (toolParameters->lengthMm - model.widthMm) / 2.0},
                     {1, 0, 0},
                     {0, 1, 0},
                     {0, 0, 1}};
        }
        if (withMotion) {
            basis.origin = gp_Pnt(0, 0, (toolParameters->lengthMm - model.widthMm) / 2.0);
        }
        std::array<TopoDS_Shape, 2> initialTools;
        std::array<std::vector<ToolDisplaySegment>, 2> initialSegments;
        if (preview->setup) {
            const bool drawn = realTools && realTools->useDrawingGeometry;
            initialTools =
                makeTools(*preview->setup, model.thicknessMm,
                          {!drawn || !toolGeometry->hasDie, !drawn || !toolGeometry->hasPunch});
            if (realTools && realTools->useDrawingGeometry) {
                const auto drawn = makeDrawingTools(
                    *realTools, *toolGeometry, model.thicknessMm + toolParameters->clearanceMm);
                for (std::size_t i = 0; i < 2; ++i)
                    if (!drawn[i].IsNull())
                        initialTools[i] = drawn[i];
            }
            if (realTools) {
                initialTools[0] =
                    segmentToolBody(initialTools[0], realTools->die, realTools->dieAssembly, 0,
                                    stop, &initialSegments[0]);
                initialTools[1] = segmentToolBody(
                    initialTools[1], realTools->punch, realTools->punchAssembly,
                    (toolParameters->lengthMm - core::punchLengthMm(*toolParameters)) / 2, stop,
                    &initialSegments[1]);
                if (stop.stop_requested())
                    return cancelled();
            } else {
                initialSegments[0].push_back({initialTools[0], toolParameters->lengthMm});
                initialSegments[1].push_back(
                    {initialTools[1], core::punchLengthMm(*toolParameters)});
            }
            initialTools[1] = orientPunch(initialTools[1], *toolParameters);
            orientPunchSegments(initialSegments[1], *toolParameters);
        }
        const auto skin = multi && !displaySource.IsNull()
                              ? prepareWholePartSkin(displaySource, flanges, sourceSector,
                                                     analysis.lengthToleranceMm, stop)
                              : WholePartSkin{};
        if (stop.stop_requested())
            return cancelled();
        std::optional<MachineAssembly> machine;
        if (preview->setup && toolParameters->machineModel != core::MachineModel::None)
            machine.emplace(*toolParameters, initialTools, stop, realTools);
        preview->seatedMount = machine && machine->seatedPunchMount();
        Bnd_Box envelope;
        bool returnSweepClear = true;
        constexpr std::size_t formingIntervals = 40;
        const std::size_t intervals = withMotion ? formingIntervals + 20 : toolParameters ? 0 : 20;
        for (std::size_t i = 0; i <= intervals; ++i) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            const double progress =
                intervals ? static_cast<double>(i) / static_cast<double>(intervals) : 0.0;
            if (withMotion) {
                const auto state = simulation::pressMotionState(model, *preview->motion, progress);
                if (!state) {
                    return BendPreviewError{BendPreviewErrorCode::Failed,
                                            "Cannot compute press state", i};
                }
                preview->states.push_back(*state);
            }
            const auto section = withMotion ? preview->states.back().section
                                            : *simulation::singleBendSection(model, progress);
            auto frame = makeSectionFrame(model, basis, section);
            if (bend.trimmedEnds)
                frame.regions[1] = mapTrimmedBend(
                    sourceSector,
                    [&](gp_Pnt p) {
                        const gp_Vec v(sourceBasis.origin, p);
                        const auto mapped = simulation::mapBendPoint(
                            model, {v.Dot(sourceBasis.x), v.Dot(sourceBasis.y)}, section);
                        if (!mapped)
                            throw std::runtime_error("Cannot map a trimmed bend point");
                        return sectionPoint(basis, *mapped)
                            .Translated(basis.z * v.Dot(sourceBasis.z));
                    },
                    section.angleRadians / model.angleRadians, analysis.lengthToleranceMm, nullptr,
                    stop);
            placeFlanges(frame, flanges, sourceBasis, sourceSection, basis, section);
            if (multi)
                for (const auto &pair : {std::pair{0U, 1U}, std::pair{0U, 2U}, std::pair{1U, 2U}}) {
                    bool completed = false;
                    if (!disjointVolumes(frame.regions[pair.first], frame.regions[pair.second],
                                         analysis.lengthToleranceMm, collisionBudget, &completed))
                        return BendPreviewError{completed ? BendPreviewErrorCode::SheetIntersection
                                                          : BendPreviewErrorCode::Failed,
                                                completed ? "The two formed components intersect "
                                                            "during the selected operation"
                                                          : "Component intersection Boolean failed",
                                                i};
                }
            const auto materialShape = frame.shape;
            const double materialVolume = volume(frame.shape);
            if (!std::isfinite(materialVolume) ||
                std::abs(materialVolume - expectedVolume) > budget) {
                return BendPreviewError{BendPreviewErrorCode::Failed,
                                        "Intermediate state changed material volume"};
            }
            if (withMotion && i == formingIntervals) {
                // Independently place the already verified CAD reconstruction into the machine
                // frame. The contact section must be congruent with it, not merely have the same
                // volume.
                const auto &left = preview->states.back().section.innerArc[0];
                const double h = model.angleRadians / 2.0;
                const FrameBasis endpoint{gp_Pnt(left.x, left.y, basis.origin.Z()),
                                          gp_Vec(std::cos(h), -std::sin(h), 0),
                                          gp_Vec(std::sin(h), std::cos(h), 0), gp_Vec(0, 0, 1)};
                gp_Trsf placement;
                placement.SetDisplacement(
                    gp_Ax3(sourceBasis.origin, gp_Dir(sourceBasis.z), gp_Dir(sourceBasis.x)),
                    gp_Ax3(endpoint.origin, gp_Dir(endpoint.z), gp_Dir(endpoint.x)));
                const auto expected = BRepBuilderAPI_Transform(source, placement, true).Shape();
                if (!sameVolume(expected, frame.shape, analysis.lengthToleranceMm, budget, stop)) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    return BendPreviewError{
                        BendPreviewErrorCode::Failed,
                        "Contact endpoint differs from the verified STEP reconstruction", i};
                }
            }
            if (withMotion && i == formingIntervals) {
                const double tip = preview->states.back().punchTipYmm;
                const double lift = model.thicknessMm + toolParameters->clearanceMm - tip;
                if (!std::isfinite(lift) || lift <= analysis.lengthToleranceMm) {
                    return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                            "Invalid vertical punch return distance", i};
                }
                preview->sweep = preview->realPunch
                                     ? drawingPunchSweep(*realTools, *toolGeometry, tip, lift, stop)
                                     : makePunch(*toolParameters, tip, lift);
                if (realTools)
                    preview->sweep = segmentToolBody(
                        preview->sweep, realTools->punch, realTools->punchAssembly,
                        (toolParameters->lengthMm - core::punchLengthMm(*toolParameters)) / 2,
                        stop);
                preview->sweep = orientPunch(preview->sweep, *toolParameters);
                if (stop.stop_requested())
                    return cancelled();
                if (!BRepCheck_Analyzer(preview->sweep).IsValid() ||
                    !std::isfinite(volume(preview->sweep)) || volume(preview->sweep) <= 0.0) {
                    return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                            "Invalid vertical punch return envelope", i};
                }
                for (const auto &obstacle : {frame.shape, initialTools[0]}) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    bool completed = false;
                    if (!disjointVolumes(preview->sweep, obstacle, analysis.lengthToleranceMm,
                                         collisionBudget, &completed)) {
                        if (!completed)
                            return BendPreviewError{
                                BendPreviewErrorCode::RetractionNotProven,
                                "Punch return occupied volume is not clear or Boolean failed", i};
                        returnSweepClear = false;
                    }
                }
            }
            if (preview->setup) {
                frame.tools = initialTools;
                frame.toolSegments = initialSegments;
                if (withMotion) {
                    gp_Trsf translation;
                    translation.SetTranslation(gp_Vec(0,
                                                      preview->states.back().punchTipYmm -
                                                          model.thicknessMm -
                                                          toolParameters->clearanceMm,
                                                      0));
                    const TopLoc_Location location(translation);
                    frame.tools[1] = initialTools[1].Moved(location);
                    for (auto &segment : frame.toolSegments[1])
                        segment.shape = segment.shape.Moved(location);
                }
                if (machine) {
                    const double lift = withMotion
                                            ? preview->states.back().punchTipYmm -
                                                  model.thicknessMm - toolParameters->clearanceMm
                                            : 0;
                    frame.machine = machine->at(lift);
                    frame.floor = machine->floor();
                }
                const auto machineHits =
                    MachineAssembly::collisions(frame, analysis.lengthToleranceMm, stop);
                const auto machineConflict = machineHits.empty()
                                                 ? std::optional<MachineCollision>{}
                                                 : std::optional{machineHits.front().pair};
                const std::array<std::pair<TopoDS_Shape, TopoDS_Shape>, 3> pairs{
                    {{frame.shape, frame.tools[0]},
                     {frame.shape, frame.tools[1]},
                     {frame.tools[0], frame.tools[1]}}};
                std::array<bool, 3> toolHits{};
                std::vector<CollisionEvidence> evidence;
                for (std::size_t pair = 0; pair < pairs.size(); ++pair) {
                    if (stop.stop_requested())
                        return cancelled();
                    bool completed = false;
                    TopoDS_Shape overlap;
                    toolHits[pair] = !disjointVolumes(pairs[pair].first, pairs[pair].second,
                                                      analysis.lengthToleranceMm, collisionBudget,
                                                      &completed, &overlap);
                    if (!completed)
                        return BendPreviewError{BendPreviewErrorCode::Failed,
                                                "Tool collision Boolean failed", i};
                    if (toolHits[pair])
                        evidence.push_back(
                            {pair == 2 ? "die" : "sheet", pair == 0 ? "die" : "punch", overlap});
                }
                for (const auto &hit : machineHits)
                    evidence.push_back({hit.pair.firstId, hit.pair.secondId, hit.overlap});
                for (std::size_t pair = 0; pair < pairs.size() + 1; ++pair) {
                    const bool conflict =
                        pair == pairs.size() ? machineConflict.has_value() : toolHits[pair];
                    if (conflict) {
                        BRep_Builder builder;
                        TopoDS_Compound assembly;
                        builder.MakeCompound(assembly);
                        builder.Add(assembly, frame.shape);
                        for (const auto &t : frame.tools)
                            builder.Add(assembly, t);
                        frame.shape = assembly;
                        BRepMesh_IncrementalMesh mesh(materialShape, .05, false, .35, false);
                        if (!mesh.IsDone())
                            return BendPreviewError{BendPreviewErrorCode::Failed,
                                                    "Collision snapshot mesh failed", i};
                        if (!displaySource.IsNull()) {
                            if (multi)
                                makeWholePartDisplay(frame, skin, model, sourceBasis, basis,
                                                     section, stop);
                            else
                                makeDisplay(frame, displaySource, model, sourceBasis, basis,
                                            section, stop);
                        }
                        buildCollisionHighlights(frame, evidence, model.thicknessMm, stop);
                        if (stop.stop_requested())
                            return cancelled();
                        auto diagnostic = std::make_shared<CadPreview>();
                        diagnostic->geometry = model;
                        diagnostic->seatedMount = preview->seatedMount;
                        diagnostic->setup = preview->setup;
                        diagnostic->realPunch = preview->realPunch;
                        diagnostic->realDie = preview->realDie;
                        diagnostic->filledCutouts = preview->filledCutouts;
                        diagnostic->gaps = preview->gaps;
                        diagnostic->endSupport = preview->endSupport;
                        diagnostic->otherBends = preview->otherBends;
                        if (pair < pairs.size())
                            diagnostic->conflict = static_cast<ToolCollisionPair>(pair);
                        diagnostic->frames = std::move(preview->frames);
                        diagnostic->frameConflicts.resize(diagnostic->frames.size());
                        diagnostic->frameConflicts.push_back(diagnostic->conflict);
                        diagnostic->machineConflicts = std::move(preview->machineConflicts);
                        diagnostic->machineConflicts.push_back(machineConflict);
                        if (withMotion) {
                            diagnostic->motion = preview->motion;
                            diagnostic->states = std::move(preview->states);
                        }
                        Bnd_Box box;
                        BRepBndLib::Add(frame.shape, box);
                        for (const auto &safe : diagnostic->frames)
                            BRepBndLib::Add(safe.shape, box);
                        box.Get(diagnostic->envelope.minimum.x, diagnostic->envelope.minimum.y,
                                diagnostic->envelope.minimum.z, diagnostic->envelope.maximum.x,
                                diagnostic->envelope.maximum.y, diagnostic->envelope.maximum.z);
                        diagnostic->frames.push_back(std::move(frame));
                        return BendPreviewError{
                            pair == pairs.size() ? BendPreviewErrorCode::MachineIntersection
                                                 : BendPreviewErrorCode::ToolIntersection,
                            pair == pairs.size()
                                ? machineConflict->firstName + " — " + machineConflict->secondName
                                : "Tool and material volumes intersect",
                            i, BendPreviewPtr(diagnostic)};
                    }
                }
                if (withMotion) {
                    const auto &state = preview->states.back();
                    for (std::size_t flange = 0; flange < 2; ++flange) {
                        if (stop.stop_requested()) {
                            return cancelled();
                        }
                        const auto contactPoint = simulation::dieSupportPoint(
                            *preview->motion, state.section.angleRadians);
                        const double x = (flange == 0 ? -1. : 1.) * contactPoint.x;
                        const auto shoulder =
                            BRepBuilderAPI_MakeEdge(
                                gp_Pnt(x, contactPoint.y, 0),
                                gp_Pnt(x, contactPoint.y, toolParameters->lengthMm))
                                .Edge();
                        BRepExtrema_DistShapeShape contact(frame.regions[flange * 2], shoulder);
                        BRepExtrema_DistShapeShape onDie(shoulder, frame.tools[0]);
                        if (!contact.IsDone() || contact.Value() > analysis.lengthToleranceMm ||
                            !onDie.IsDone() || onDie.Value() > analysis.lengthToleranceMm) {
                            return BendPreviewError{BendPreviewErrorCode::ContactLost,
                                                    "Die shoulder contact missing", i};
                        }
                    }
                    BRepExtrema_DistShapeShape punchContact(frame.regions[1], frame.tools[1]);
                    const double expectedGap = state.phase == simulation::PressPhase::Approach
                                                   ? state.punchTipYmm - model.thicknessMm
                                                   : 0.0;
                    if (!punchContact.IsDone() || !std::isfinite(punchContact.Value()) ||
                        (state.phase != simulation::PressPhase::Retracting &&
                         std::abs(punchContact.Value() - expectedGap) >
                             analysis.lengthToleranceMm)) {
                        return BendPreviewError{state.phase == simulation::PressPhase::Retracting
                                                    ? BendPreviewErrorCode::RetractionNotProven
                                                    : BendPreviewErrorCode::ContactLost,
                                                "Punch contact/clearance mismatch", i};
                    }
                }
                if (withMotion && i == intervals) {
                    BRepExtrema_DistShapeShape distance(frame.shape, frame.tools[1]);
                    if (!distance.IsDone() || !std::isfinite(distance.Value()) ||
                        distance.Value() <= analysis.lengthToleranceMm) {
                        return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                                "Final punch separation not confirmed", i};
                    }
                    const double home = model.thicknessMm + toolParameters->clearanceMm;
                    preview->retraction = PunchRetractionCheck{
                        home - preview->states[formingIntervals].punchTipYmm, distance.Value()};
                }
                BRep_Builder builder;
                TopoDS_Compound assembly;
                builder.MakeCompound(assembly);
                builder.Add(assembly, frame.shape);
                for (const auto &tool : frame.tools) {
                    builder.Add(assembly, tool);
                }
                frame.shape = assembly;
            }
            BRepMesh_IncrementalMesh mesh(materialShape, 0.05, false, 0.35, false);
            if (!mesh.IsDone()) {
                return BendPreviewError{BendPreviewErrorCode::Failed, "Preview mesh failed"};
            }
            if (!displaySource.IsNull()) {
                if (multi)
                    makeWholePartDisplay(frame, skin, model, sourceBasis, basis, section, stop);
                else
                    makeDisplay(frame, displaySource, model, sourceBasis, basis, section, stop);
            }
            BRepBndLib::Add(frame.shape, envelope);
            preview->machineConflicts.push_back({});
            preview->frames.push_back(std::move(frame));
        }
        if (stop.stop_requested()) {
            return cancelled();
        }
        if (!returnSweepClear)
            return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                    "Punch return occupied volume intersects; discrete "
                                    "positions are not a complete proof"};
        envelope.Get(preview->envelope.minimum.x, preview->envelope.minimum.y,
                     preview->envelope.minimum.z, preview->envelope.maximum.x,
                     preview->envelope.maximum.y, preview->envelope.maximum.z);
        return BendPreviewPtr(std::move(preview));
    } catch (const TrimmedBendError &error) {
        if (stop.stop_requested())
            return cancelled();
        return unsupported(error.what());
    } catch (const Standard_Failure &error) {
        return BendPreviewError{BendPreviewErrorCode::Failed, error.GetMessageString()
                                                                  ? error.GetMessageString()
                                                                  : "OCCT preview failure"};
    } catch (const std::exception &error) {
        if (stop.stop_requested())
            return cancelled();
        return BendPreviewError{BendPreviewErrorCode::Failed, error.what()};
    }
}
application::BendPreviewResult SingleBendPreviewBuilder::build(DocumentPtr document,
                                                               std::size_t bendIndex,
                                                               core::BendTarget target,
                                                               bool fixedSecond,
                                                               std::stop_token stop) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    return buildBendOperation(std::move(document), bendIndex, target, fixedSecond, stop, {});
}
application::BendPreviewResult SingleBendPreviewBuilder::buildToolSetup(
    DocumentPtr document, std::size_t bendIndex, core::BendTarget target, core::PressTools tools,
    std::stop_token stop, double maximumEndSupportLossMm) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    return buildBendOperation(std::move(document), bendIndex, target, false, stop, tools, false, {},
                              maximumEndSupportLossMm > 0, maximumEndSupportLossMm);
}
application::BendPreviewResult SingleBendPreviewBuilder::buildToolMotion(
    DocumentPtr document, std::size_t bendIndex, core::BendTarget target, core::PressTools tools,
    std::stop_token stop, double maximumEndSupportLossMm) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    return buildBendOperation(std::move(document), bendIndex, target, false, stop, tools, true, {},
                              maximumEndSupportLossMm > 0, maximumEndSupportLossMm);
}
application::BendPreviewResult SingleBendPreviewBuilder::buildAssignedToolMotion(
    DocumentPtr document, std::size_t index, core::BendTarget target,
    core::ToolAssignment assignment, std::stop_token stop, double maximumEndSupportLossMm) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    const auto tools = core::toolParameters(assignment);
    if (!tools)
        return BendPreviewError{BendPreviewErrorCode::UnsupportedToolGeometry,
                                "Invalid DXF tool geometry/parameters"};
    return buildBendOperation(std::move(document), index, target, false, stop, *tools, true,
                              std::optional{std::move(assignment)}, maximumEndSupportLossMm > 0,
                              maximumEndSupportLossMm);
}
application::BendPreviewResult SingleBendPreviewBuilder::buildAssignedToolSetup(
    DocumentPtr document, std::size_t index, core::BendTarget target,
    core::ToolAssignment assignment, std::stop_token stop, double maximumEndSupportLossMm) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    const auto tools = core::toolParameters(assignment);
    if (!tools)
        return BendPreviewError{BendPreviewErrorCode::UnsupportedToolGeometry,
                                "Invalid DXF tool geometry/parameters"};
    return buildBendOperation(std::move(document), index, target, false, stop, *tools, false,
                              std::optional{std::move(assignment)}, maximumEndSupportLossMm > 0,
                              maximumEndSupportLossMm);
}
} // namespace bending::cad
