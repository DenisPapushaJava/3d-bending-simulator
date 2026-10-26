#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/CadDocument.h"
#include "bending/cad/occt/DocumentAccess.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GProp_GProps.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <IntAna_QuadQuadGeo.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace bending::cad {
namespace {
using namespace application;
struct Frame {
    gp_Dir u;
    gp_Dir v;
    gp_Dir normal;
};
DrawingPoint project(gp_Pnt p, const Frame &f) {
    const gp_Vec v(gp_Pnt(0, 0, 0), p);
    return {v.Dot(gp_Vec(f.u)), v.Dot(gp_Vec(f.v))};
}
std::array<double, 4> bounds(const TopoDS_Shape &shape, const Frame &frame,
                             gp_Pnt origin = gp_Pnt(0, 0, 0)) {
    gp_Trsf placement;
    placement.SetTransformation(gp_Ax3(origin, frame.normal, frame.u));
    const auto moved = BRepBuilderAPI_Transform(shape, placement, true).Shape();
    Bnd_Box box;
    BRepBndLib::AddOptimal(moved, box, false, false);
    if (box.IsVoid() || box.IsOpen())
        throw std::runtime_error("Cannot measure finite drawing bounds");
    const auto low = box.CornerMin(), high = box.CornerMax();
    return {low.X(), low.Y(), high.X(), high.Y()};
}
void contours(const TopoDS_Shape &shape, bool hidden, DrawingView &view, std::size_t &budget,
              std::stop_token stop) {
    for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) {
        if (stop.stop_requested())
            throw DrawingError{"Cancelled", true};
        BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
        DrawingPath path;
        path.hidden = hidden;
        if (curve.GetType() == GeomAbs_Line) {
            const auto a = curve.Value(curve.FirstParameter()),
                       b = curve.Value(curve.LastParameter());
            path.points = {{a.X(), a.Y()}, {b.X(), b.Y()}};
        } else {
            GCPnts_QuasiUniformDeflection sample(curve, .02, GeomAbs_C0);
            if (!sample.IsDone() || sample.NbPoints() < 2 || sample.NbPoints() > 4096)
                throw std::runtime_error(
                    "Cannot approximate a projected curve within drawing budget");
            for (int i = 1; i <= sample.NbPoints(); ++i) {
                const auto p = sample.Value(i);
                path.points.push_back({p.X(), p.Y()});
            }
            if (!hidden && curve.GetType() == GeomAbs_Circle &&
                std::abs(curve.LastParameter() - curve.FirstParameter() - 2 * std::numbers::pi) <
                    1e-7) {
                const auto circle = curve.Circle();
                const auto c = circle.Location();
                if (std::none_of(
                        view.circleCenters.begin(), view.circleCenters.end(), [&](const auto &old) {
                            return std::hypot(old.first.x - c.X(), old.first.y - c.Y()) < 1e-5;
                        }))
                    view.circleCenters.push_back({{c.X(), c.Y()}, circle.Radius()});
            }
        }
        budget += path.points.size();
        if (budget > 200000)
            throw std::runtime_error("Drawing projection exceeds 200000 points");
        for (auto p : path.points)
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                throw std::runtime_error("Non-finite projected geometry");
        view.paths.push_back(std::move(path));
    }
}
DrawingView viewOf(const TopoDS_Shape &shape, const Frame &f, std::size_t &budget,
                   std::stop_token stop) {
    if (stop.stop_requested())
        throw DrawingError{"Cancelled", true};
    Handle(HLRBRep_Algo) algo = new HLRBRep_Algo;
    algo->Add(shape);
    algo->Projector(HLRAlgo_Projector(gp_Ax2(gp_Pnt(0, 0, 0), f.normal, f.u)));
    algo->Update();
    if (stop.stop_requested())
        throw DrawingError{"Cancelled", true};
    algo->Hide();
    HLRBRep_HLRToShape result(algo);
    DrawingView view;
    contours(result.VCompound(), false, view, budget, stop);
    contours(result.OutLineVCompound(), false, view, budget, stop);
    contours(result.HCompound(), true, view, budget, stop);
    contours(result.OutLineHCompound(), true, view, budget, stop);
    if (view.paths.empty())
        throw std::runtime_error("HLR produced no drawing contours");
    const auto b = bounds(shape, f);
    view.minimum = {b[0], b[1]};
    view.maximum = {b[2], b[3]};
    return view;
}
struct FlangeSize {
    gp_Pnt corner;
    gp_Pnt end;
    double value;
};
std::optional<FlangeSize> flangeSize(const TopoDS_Face &face, const gp_Pln &other, gp_Dir axis) {
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane)
        return {};
    const auto plane = surface.Plane();
    IntAna_QuadQuadGeo intersection(plane, other, 1e-8, 1e-6);
    if (!intersection.IsDone() || intersection.TypeInter() != IntAna_Line ||
        intersection.NbSolutions() != 1)
        return {};
    const auto rail = intersection.Line(1);
    if (std::abs(rail.Direction().Dot(axis)) < 1 - 1e-7)
        return {};
    const gp_Dir direction(gp_Vec(plane.Axis().Direction()).Crossed(gp_Vec(axis)));
    const Frame frame{direction, axis, gp_Dir(gp_Vec(direction).Crossed(gp_Vec(axis)))};
    const auto b = bounds(face, frame, rail.Location());
    // The complete face must stay on one side of the virtual sharp corner.
    if (b[0] < -1e-5 && b[2] > 1e-5)
        return {};
    const double extent = std::max(std::abs(b[0]), std::abs(b[2]));
    for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More(); v.Next()) {
        const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
        const double length = gp_Vec(rail.Location(), p).Dot(gp_Vec(direction));
        if (std::abs(std::abs(length) - extent) <= 1e-5) {
            const auto corner = p.Translated(-gp_Vec(direction) * length);
            return FlangeSize{corner, p, extent};
        }
    }
    return {}; // A curved extremum is not guessed from sampled vertices.
}
bool homogeneous(const TopoDS_Shape &shape, const Frame &f, double low, double high) {
    const double tolerance = 1e-5;
    const double angular = std::min(1e-9, tolerance / (high - low));
    for (TopExp_Explorer e(shape, TopAbs_VERTEX); e.More(); e.Next()) {
        const double x = project(BRep_Tool::Pnt(TopoDS::Vertex(e.Current())), f).x;
        if (std::min(std::abs(x - low), std::abs(x - high)) > tolerance)
            return false;
    }
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        const auto face = TopoDS::Face(e.Current());
        int wires = 0;
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next())
            ++wires;
        if (wires != 1)
            return false;
        BRepAdaptor_Surface s(face);
        if (s.GetType() == GeomAbs_Plane) {
            const double dot = std::abs(s.Plane().Axis().Direction().Dot(f.u));
            if (dot > angular && dot < 1 - angular)
                return false;
            if (dot >= 1 - angular) {
                const double x = project(s.Plane().Location(), f).x;
                if (std::min(std::abs(x - low), std::abs(x - high)) > tolerance)
                    return false;
            }
        } else if (s.GetType() == GeomAbs_Cylinder) {
            if (std::abs(s.Cylinder().Axis().Direction().Dot(f.u)) < 1 - angular)
                return false;
        } else
            return false;
    }
    return true;
}
DrawingView sectionView(const TopoDS_Shape &shape, const Frame &frame, gp_Pnt station,
                        gp_Pnt corner, double radius, std::size_t &budget, std::stop_token stop) {
    if (stop.stop_requested())
        throw DrawingError{"Cancelled", true};
    BRepAlgoAPI_Section section(shape, gp_Pln(station, frame.normal), false);
    section.Build();
    if (!section.IsDone())
        throw std::runtime_error("Cannot build an enlarged bend section");
    gp_Trsf placement;
    placement.SetTransformation(gp_Ax3(gp_Pnt(0, 0, 0), frame.normal, frame.u));
    const auto moved = BRepBuilderAPI_Transform(section.Shape(), placement, true).Shape();
    Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
    for (TopExp_Explorer e(moved, TopAbs_EDGE); e.More(); e.Next())
        edges->Append(e.Current());
    Handle(TopTools_HSequenceOfShape) wires = new TopTools_HSequenceOfShape;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-5, false, wires);
    DrawingView result;
    const auto center = project(corner, frame);
    result.minimum = {center.x - radius, center.y - radius};
    result.maximum = {center.x + radius, center.y + radius};
    for (int i = 1; i <= wires->Length(); ++i) {
        const auto wire = TopoDS::Wire(wires->Value(i));
        if (!BRep_Tool::IsClosed(wire))
            throw std::runtime_error("An enlarged bend section has an open contour");
        std::vector<DrawingPoint> loop;
        for (BRepTools_WireExplorer e(wire); e.More(); e.Next()) {
            DrawingView edge;
            contours(e.Current(), false, edge, budget, stop);
            for (auto &path : edge.paths) {
                if (e.Current().Orientation() == TopAbs_REVERSED)
                    std::reverse(path.points.begin(), path.points.end());
                if (!loop.empty() && std::hypot(loop.back().x - path.points.front().x,
                                                loop.back().y - path.points.front().y) > 1e-5)
                    throw std::runtime_error("An enlarged section has disconnected edges");
                loop.insert(loop.end(), path.points.begin(), path.points.end());
                result.paths.push_back(std::move(path));
            }
        }
        if (!loop.empty()) {
            if (std::hypot(loop.front().x - loop.back().x, loop.front().y - loop.back().y) > 1e-5)
                throw std::runtime_error("An enlarged section contour is not closed");
            result.sectionLoops.push_back(std::move(loop));
        }
    }
    if (result.sectionLoops.empty())
        throw std::runtime_error("An enlarged bend section is empty");
    return result;
}
} // namespace
DrawingResult BendingDrawingBuilder::build(DocumentPtr document, DrawingRequest request,
                                           std::stop_token stop) const {
    try {
        const auto source = std::dynamic_pointer_cast<const CadDocument>(document);
        if (!source)
            return DrawingError{"Drawing requires a loaded STEP solid"};
        if (stop.stop_requested())
            return DrawingError{"Cancelled", true};
        const auto shape =
            BRepBuilderAPI_Copy(OcctDocumentAccess::shape(*source), true, false).Shape();
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        if (faces.Extent() > 500 || edges.Extent() > 2000)
            return DrawingError{"First drawing version supports up to 500 faces / 2000 edges"};
        if (request.quarterTurns < -1 || request.quarterTurns > 3 ||
            (request.manualThicknessMm &&
             (!std::isfinite(*request.manualThicknessMm) || *request.manualThicknessMm <= 0)))
            return DrawingError{"Invalid drawing settings"};
        std::size_t base = request.baseFace.value_or(0);
        if (base > static_cast<std::size_t>(faces.Extent()))
            return DrawingError{"Selected face belongs to another document"};
        if (!base) {
            double maximum = 0;
            for (int i = 1; i <= faces.Extent(); ++i) {
                const auto face = TopoDS::Face(faces(i));
                if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane)
                    continue;
                GProp_GProps area;
                BRepGProp::SurfaceProperties(face, area);
                if (area.Mass() > maximum + std::max(1e-8, maximum * 1e-9)) {
                    maximum = area.Mass();
                    base = static_cast<std::size_t>(i);
                }
            }
        }
        if (!base)
            return DrawingError{"Choose a planar base face for the drawing"};
        const auto face = TopoDS::Face(faces(static_cast<int>(base)));
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane)
            return DrawingError{"The drawing base must be planar"};
        gp_Dir side = surface.Plane().Axis().Direction();
        if (face.Orientation() == TopAbs_REVERSED)
            side.Reverse();
        gp_Dir u = surface.Plane().XAxis().Direction();
        gp_Pnt anchor = surface.Plane().Location();
        double longest = 0;
        bool anchored = false;
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
            if (!anchored) {
                anchor = curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
                anchored = true;
            }
            if (curve.GetType() != GeomAbs_Line)
                continue;
            const auto a = curve.Value(curve.FirstParameter()),
                       b = curve.Value(curve.LastParameter());
            if (a.Distance(b) > longest) {
                longest = a.Distance(b);
                u = gp_Dir(gp_Vec(a, b));
                anchor = curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
            }
        }
        const auto &metadata = document->metadata();
        // A parallel bend axis makes the left view a useful true profile of the bend.
        for (const auto &bend : metadata.bendAnalysis.bends) {
            const gp_Vec axis(gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z),
                              gp_Pnt(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z));
            if (std::find(bend.flangeFaces.begin(), bend.flangeFaces.end(), base) !=
                    bend.flangeFaces.end() &&
                axis.Magnitude() > 1e-6 && std::abs(axis.Normalized().Dot(gp_Vec(side))) < 1e-4) {
                u = gp_Dir(axis - gp_Vec(side) * axis.Dot(gp_Vec(side)));
                break;
            }
        }
        u = gp_Dir(gp_Vec(u) - gp_Vec(side) * gp_Vec(u).Dot(gp_Vec(side)));
        gp_Dir n = request.reverseView ? side.Reversed() : side;
        gp_Dir v = gp_Dir(gp_Vec(n).Crossed(gp_Vec(u)));
        int turns = request.quarterTurns;
        if (turns < 0) {
            turns = 0;
            const auto b = bounds(shape, {u, v, n});
            if ((b[3] - b[1]) > (b[2] - b[0]) * 1.2)
                turns = 1;
        }
        for (int i = 0; i < turns; ++i) {
            const auto previous = u;
            u = v;
            v = previous.Reversed();
        }
        const gp_Dir isoN(gp_Vec(u) * 1.0 + gp_Vec(v) * 1.0 + gp_Vec(n) * 1.0);
        const gp_Dir isoU(gp_Vec(u) - gp_Vec(n));
        const std::array<Frame, 4> frames{
            {{u, v, n},
             {u, n.Reversed(), v},
             {n, v, u.Reversed()},
             {isoU, gp_Dir(gp_Vec(isoN).Crossed(gp_Vec(isoU))), isoN}}};
        auto drawing = std::make_shared<BendingDrawing>();
        drawing->baseFace = base;
        drawing->automaticBase = !request.baseFace;
        drawing->reverseView = request.reverseView;
        drawing->unrecognizedCylindricalFaces = metadata.bendAnalysis.unrecognizedCylindricalFaces;
        const auto utf8 = metadata.sourcePath.filename().u8string();
        drawing->sourceName.assign(utf8.begin(), utf8.end());
        std::size_t budget = 0;
        for (std::size_t i = 0; i < frames.size(); ++i)
            drawing->views[i] = viewOf(shape, frames[i], budget, stop);
        const auto &fv = drawing->views[0];
        const double width = fv.maximum.x - fv.minimum.x;
        const double section = std::max(fv.maximum.y - fv.minimum.y,
                                        drawing->views[1].maximum.y - drawing->views[1].minimum.y);
        if (request.allowBreaks && drawingScale(*drawing).value_or(0) < .5 && width > 300 &&
            width > 5 * section && homogeneous(shape, frames[0], fv.minimum.x, fv.maximum.x)) {
            const double keep = std::max(80., 2.5 * section);
            const std::array<double, 2> gap{fv.minimum.x + keep / 2, fv.maximum.x - keep / 2};
            drawing->views[0].omittedX = gap;
            drawing->views[1].omittedX = gap;
        }
        auto &front = drawing->views[0];
        front.dimensions.push_back({{front.minimum.x, front.minimum.y},
                                    {front.maximum.x, front.minimum.y},
                                    front.maximum.x - front.minimum.x,
                                    ""});
        front.dimensions.push_back({{front.minimum.x, front.minimum.y},
                                    {front.minimum.x, front.maximum.y},
                                    front.maximum.y - front.minimum.y,
                                    ""});
        auto &top = drawing->views[1];
        top.dimensions.push_back({{top.minimum.x, top.minimum.y},
                                  {top.minimum.x, top.maximum.y},
                                  top.maximum.y - top.minimum.y,
                                  ""});
        const double baseScale = drawingScale(*drawing).value_or(1);
        const auto addBend = [&](const RecognizedBend &bend, bool diagnostic) {
            const gp_Vec axisVector(gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z),
                                    gp_Pnt(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z));
            std::size_t best = 0;
            for (std::size_t i = 1; i < 3; ++i)
                if (std::abs(axisVector.Dot(gp_Vec(frames[i].normal))) >
                    std::abs(axisVector.Dot(gp_Vec(frames[best].normal))))
                    best = i;
            const auto inner = TopoDS::Face(faces(static_cast<int>(bend.innerFace)));
            double u0, u1, v0, v1;
            BRepTools::UVBounds(inner, u0, u1, v0, v1);
            const auto at = BRepAdaptor_Surface(inner).Value((u0 + u1) / 2, (v0 + v1) / 2);
            bool noted = false;
            DrawingBend row{bend.angleRadians,
                            bend.innerRadiusMm,
                            bend.thicknessMm,
                            bend.lengthMm,
                            {},
                            diagnostic,
                            bend.trimmedEnds,
                            {}};
            if (!diagnostic) {
                const gp_Dir axis(axisVector);
                std::array<std::optional<FlangeSize>, 2> measured;
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto other =
                        TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[(1 - i) * 2 + 1])));
                    BRepAdaptor_Surface os(other);
                    if (os.GetType() == GeomAbs_Plane)
                        measured[i] = flangeSize(
                            TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[i * 2 + 1]))),
                            os.Plane(), axis);
                    if (measured[i])
                        row.outerFlangeMm[i] = measured[i]->value;
                }
                if (measured[0] && measured[1]) {
                    const auto first = gp_Vec(measured[0]->corner, measured[0]->end).Normalized();
                    const auto second = gp_Vec(measured[1]->corner, measured[1]->end).Normalized();
                    const double angle = std::acos(std::clamp(first.Dot(second), -1., 1.));
                    if (angle > 1e-5 && angle < std::numbers::pi - 1e-5 &&
                        std::abs(angle + bend.angleRadians - std::numbers::pi) < 1e-5)
                        row.interiorAngleRadians = angle;
                }
                const bool trueProfile = std::abs(axis.Dot(frames[best].normal)) > 1 - 1e-7;
                const bool enlarged = row.interiorAngleRadians &&
                                      ((bend.innerRadiusMm + bend.thicknessMm) * baseScale < 1.5 ||
                                       !trueProfile || metadata.bendAnalysis.bends.size() > 1);
                auto profileFrame = frames[best];
                std::optional<DrawingView> detail;
                const gp_Pnt station = gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z)
                                           .Translated(axisVector * .5);
                if (enlarged) {
                    if (drawing->details.size() >= 24)
                        throw std::runtime_error("At most 24 enlarged bend profiles are supported");
                    if (!trueProfile) {
                        const gp_Vec horizontal =
                            gp_Vec(profileFrame.u) -
                            gp_Vec(axis) * gp_Vec(profileFrame.u).Dot(gp_Vec(axis));
                        profileFrame = {gp_Dir(horizontal),
                                        gp_Dir(gp_Vec(axis).Crossed(horizontal)), axis};
                    }
                    const double radius =
                        std::max(12., std::min(measured[0]->value, measured[1]->value) +
                                          (bend.innerRadiusMm + bend.thicknessMm));
                    detail = sectionView(shape, profileFrame, station, measured[0]->corner, radius,
                                         budget, stop);
                }
                auto &profile = detail ? *detail : drawing->views[best];
                if (row.interiorAngleRadians && (detail || trueProfile))
                    profile.angles.push_back({project(measured[0]->corner, profileFrame),
                                              project(measured[0]->end, profileFrame),
                                              project(measured[1]->end, profileFrame),
                                              *row.interiorAngleRadians,
                                              (bend.innerRadiusMm + bend.thicknessMm) /
                                                      std::sin(*row.interiorAngleRadians / 2) -
                                                  bend.innerRadiusMm});
                bool thicknessAdded = false;
                for (std::size_t i = 0; i < 2; ++i) {
                    if (!measured[i])
                        continue;
                    const auto &m = *measured[i];
                    auto endPoint = m.end;
                    if (detail) {
                        endPoint.Translate(gp_Vec(axis) * gp_Vec(m.end, station).Dot(gp_Vec(axis)));
                        const auto outer =
                            TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[i * 2 + 1])));
                        BRepClass_FaceClassifier onSection(outer, endPoint, 1e-5);
                        if (onSection.State() != TopAbs_IN && onSection.State() != TopAbs_ON)
                            continue;
                    }
                    const auto end = project(m.end, profileFrame);
                    const bool insideDetail =
                        !detail || std::hypot(end.x - (profile.minimum.x + profile.maximum.x) / 2,
                                              end.y - (profile.minimum.y + profile.maximum.y) / 2) <
                                       (profile.maximum.x - profile.minimum.x) * .49;
                    if (!insideDetail)
                        continue;
                    if (detail || drawing->bends.empty()) {
                        const auto direction = gp_Vec(m.corner, m.end).Normalized();
                        if (std::abs(direction.Dot(gp_Vec(profileFrame.normal))) < 1e-7)
                            profile.dimensions.push_back({project(m.corner, profileFrame), end,
                                                          m.value, "H" + std::to_string(i + 1)});
                    }
                    if (thicknessAdded || (!detail && !drawing->bends.empty()))
                        continue;
                    const auto innerFace =
                        TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[i * 2])));
                    BRepAdaptor_Surface innerSurface(innerFace);
                    if (innerSurface.GetType() != GeomAbs_Plane)
                        continue;
                    const auto pln = innerSurface.Plane();
                    const double distance =
                        gp_Vec(pln.Location(), endPoint).Dot(gp_Vec(pln.Axis().Direction()));
                    const auto innerEnd =
                        endPoint.Translated(-gp_Vec(pln.Axis().Direction()) * distance);
                    BRepClass_FaceClassifier inside(innerFace, innerEnd, 1e-5);
                    if (std::abs(gp_Vec(pln.Axis().Direction()).Dot(gp_Vec(profileFrame.normal))) <
                            1e-7 &&
                        std::abs(std::abs(distance) - bend.thicknessMm) < 1e-5 &&
                        (inside.State() == TopAbs_ON || inside.State() == TopAbs_IN)) {
                        profile.dimensions.push_back(
                            {project(innerEnd, profileFrame), end, std::abs(distance), "t"});
                        thicknessAdded = true;
                    }
                }
                auto noteAnchor = at;
                if (detail) {
                    noteAnchor.Translate(gp_Vec(axis) * gp_Vec(at, station).Dot(gp_Vec(axis)));
                    BRepClass_FaceClassifier inSection(inner, noteAnchor, 1e-5);
                    if (inSection.State() != TopAbs_IN && inSection.State() != TopAbs_ON)
                        throw std::runtime_error(
                            "The bend midpoint section misses the measured radius");
                }
                profile.bendNotes.push_back(
                    {project(noteAnchor, profileFrame), drawing->bends.size()});
                noted = true;
                if (detail) {
                    const std::string designation =
                        "А" + std::to_string(drawing->details.size() + 1);
                    const double radius = (detail->maximum.x - detail->minimum.x) / 2;
                    drawing->views[best].labels.push_back({project(at, frames[best]), designation});
                    drawing->views[best].detailCircles.push_back(
                        {project(at, frames[best]), radius});
                    if (best != 0) {
                        drawing->views[0].labels.push_back({project(at, frames[0]), designation});
                        drawing->views[0].detailCircles.push_back({project(at, frames[0]), radius});
                    }
                    drawing->details.push_back(
                        {std::move(*detail), drawing->bends.size(), designation});
                }
            }
            if (!noted)
                drawing->views[best].bendNotes.push_back(
                    {project(at, frames[best]), drawing->bends.size()});
            drawing->bends.push_back(row);
        };
        for (const auto &bend : metadata.bendAnalysis.bends)
            addBend(bend, false);
        for (const auto &d : metadata.bendAnalysis.interiorContourDiagnostics)
            addBend(d.candidate, true);
        drawing->declaredThicknessMm = request.manualThicknessMm;
        if (!drawing->bends.empty()) {
            const double t = drawing->bends.front().thicknessMm;
            if (std::all_of(drawing->bends.begin(), drawing->bends.end(),
                            [t](const auto &b) { return std::abs(b.thicknessMm - t) < 1e-5; })) {
                drawing->thicknessMm = t;
                drawing->localThickness = true;
            }
        } else if (metadata.thickness.status == ThicknessStatus::Detected) {
            drawing->thicknessMm = metadata.thickness.thicknessMm;
        }
        if (!drawing->thicknessMm && request.manualThicknessMm) {
            drawing->thicknessMm = request.manualThicknessMm;
            drawing->manualThickness = true;
        }
        const double arrow = std::max(5.0, std::min(30.0, longest * .2));
        drawing->views[3].sideAnchor = project(anchor, frames[3]);
        drawing->views[3].sideTip = project(anchor.Translated(gp_Vec(side) * arrow), frames[3]);
        if (!drawingScale(*drawing))
            return DrawingError{"The part does not fit the supported A3 / standard scale range"};
        return BendingDrawingPtr(std::move(drawing));
    } catch (const DrawingError &error) {
        return error;
    } catch (const Standard_Failure &error) {
        return DrawingError{error.GetMessageString() ? error.GetMessageString()
                                                     : "OCCT HLR failed"};
    } catch (const std::exception &error) {
        return DrawingError{error.what()};
    }
}
} // namespace bending::cad
