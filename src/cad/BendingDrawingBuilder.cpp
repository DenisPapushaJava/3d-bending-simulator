#include "bending/cad/BendingDrawingBuilder.h"
#include "BendRecognizer.h"
#include "DrawingOpenings.h"
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
#include <BRep_Builder.hxx>
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
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <iterator>
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
struct CircleArcs {
    DrawingPoint center;
    double radius;
    std::vector<std::array<double, 2>> intervals;
};
void circleArc(const BRepAdaptor_Curve &curve, std::vector<CircleArcs> &circles) {
    const auto circle = curve.Circle();
    const auto c = circle.Location();
    auto found = std::find_if(circles.begin(), circles.end(), [&](const auto &old) {
        return std::hypot(old.center.x - c.X(), old.center.y - c.Y()) < 1e-5 &&
               std::abs(old.radius - circle.Radius()) < 1e-5;
    });
    if (found == circles.end()) {
        circles.push_back({{c.X(), c.Y()}, circle.Radius(), {}});
        found = std::prev(circles.end());
    }
    constexpr double full = 2 * std::numbers::pi;
    const auto point = curve.Value(curve.FirstParameter());
    double start = std::atan2(point.Y() - c.Y(), point.X() - c.X());
    const double span = curve.LastParameter() - curve.FirstParameter();
    if (circle.Axis().Direction().Z() < 0)
        start -= span;
    start = std::fmod(start, full);
    if (start < 0)
        start += full;
    if (span >= full - 1e-7)
        found->intervals.push_back({0, full});
    else {
        found->intervals.push_back({start, std::min(full, start + span)});
        if (start + span > full)
            found->intervals.push_back({0, start + span - full});
    }
}
void contours(const TopoDS_Shape &shape, bool hidden, DrawingView &view, std::size_t &budget,
              std::stop_token stop, std::vector<CircleArcs> *circles = nullptr) {
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
            if (!hidden && circles && curve.GetType() == GeomAbs_Circle)
                circleArc(curve, *circles);
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
                   std::stop_token stop, const std::vector<DrawingOpening> *openings = nullptr) {
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
    std::vector<CircleArcs> circles;
    contours(result.VCompound(), false, view, budget, stop, &circles);
    contours(result.OutLineVCompound(), false, view, budget, stop, &circles);
    contours(result.HCompound(), true, view, budget, stop);
    contours(result.OutLineHCompound(), true, view, budget, stop);
    for (auto &circle : circles) {
        std::sort(circle.intervals.begin(), circle.intervals.end());
        double covered = 0;
        for (const auto interval : circle.intervals) {
            if (interval[0] > covered + 1e-6)
                break;
            covered = std::max(covered, interval[1]);
        }
        if (covered >= 2 * std::numbers::pi - 1e-6)
            view.circleCenters.push_back({circle.center, circle.radius});
    }
    if (view.paths.empty())
        throw std::runtime_error("HLR produced no drawing contours");
    const auto b = bounds(shape, f);
    view.minimum = {b[0], b[1]};
    view.maximum = {b[2], b[3]};
    if (openings) {
        BRep_Builder builder;
        TopoDS_Compound visible;
        builder.MakeCompound(visible);
        for (const auto &edges : {result.VCompound(), result.OutLineVCompound()})
            if (!edges.IsNull())
                builder.Add(visible, edges);
        addDrawingOpenings(*openings, gp_Ax2(gp_Pnt(0, 0, 0), f.normal, f.u), visible, view, stop);
    }
    return view;
}
struct FlangeSize {
    gp_Pnt corner;
    gp_Pnt end;
    double value;
    std::optional<std::size_t> virtualEnd;
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
            return FlangeSize{corner, p, extent, {}};
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
                        gp_Pnt corner, double radius, std::size_t &budget, std::stop_token stop,
                        bool full = false) {
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
    if (full) {
        const auto b = bounds(section.Shape(), frame);
        result.minimum = {b[0], b[1]};
        result.maximum = {b[2], b[3]};
    }
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
struct ProfileGroup {
    std::vector<std::size_t> members;
    Frame frame;
    gp_Pnt station;
    DrawingView view;
    DrawingSectionMarker marker;
};
gp_Pnt bendSurfacePoint(const RecognizedBend &bend, const TopTools_IndexedMapOfShape &faces,
                        gp_Pnt station) {
    const auto face = TopoDS::Face(faces(static_cast<int>(bend.innerFace)));
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    auto point = BRepAdaptor_Surface(face).Value((u0 + u1) / 2, (v0 + v1) / 2);
    const gp_Dir axis(gp_Vec(gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z),
                             gp_Pnt(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z)));
    point.Translate(gp_Vec(axis) * gp_Vec(point, station).Dot(gp_Vec(axis)));
    return point;
}
bool sharedOuterFace(const RecognizedBend &a, const RecognizedBend &b) {
    for (std::size_t i : {1u, 3u})
        for (std::size_t j : {1u, 3u})
            if (a.flangeFaces[i] == b.flangeFaces[j])
                return true;
    return false;
}
gp_Dir preferredAxis(const std::vector<RecognizedBend> &bends, std::size_t base, gp_Dir normal,
                     gp_Dir fallback) {
    const auto axisOf = [](const RecognizedBend &b) {
        return gp_Dir(gp_Vec(gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z),
                             gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z)));
    };
    std::size_t bestCount = 0;
    double bestLength = 0;
    for (std::size_t i = 0; i < bends.size(); ++i) {
        const auto &bend = bends[i];
        const auto axis = axisOf(bend);
        if (std::find(bend.flangeFaces.begin(), bend.flangeFaces.end(), base) ==
                bend.flangeFaces.end() ||
            std::abs(axis.Dot(normal)) > 1e-7)
            continue;
        std::vector<std::size_t> members{i};
        for (std::size_t k = 0; k < members.size(); ++k)
            for (std::size_t j = 0; j < bends.size(); ++j)
                if (std::find(members.begin(), members.end(), j) == members.end() &&
                    axis.IsParallel(axisOf(bends[j]), 1e-7) &&
                    sharedOuterFace(bends[members[k]], bends[j]))
                    members.push_back(j);
        double length = 0;
        for (auto index : members)
            length += bends[index].lengthMm;
        if (members.size() > bestCount ||
            (members.size() == bestCount &&
             length > bestLength + std::max(1e-6, bestLength * 1e-9))) {
            bestCount = members.size();
            bestLength = length;
            fallback = axis;
        }
    }
    return fallback;
}
void extendToAdjacentBend(FlangeSize &measured, std::size_t faceIndex, std::size_t current,
                          const std::vector<RecognizedBend> &bends,
                          const TopTools_IndexedMapOfShape &faces, gp_Dir axis, double tolerance) {
    const gp_Dir direction(gp_Vec(measured.corner, measured.end));
    std::optional<FlangeSize> extended;
    for (std::size_t j = 0; j < bends.size(); ++j) {
        if (j == current)
            continue;
        const auto &bend = bends[j];
        const gp_Dir otherAxis(gp_Vec(gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z),
                                      gp_Pnt(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z)));
        if (!axis.IsParallel(otherAxis, 1e-7))
            continue;
        for (std::size_t k = 0; k < 2; ++k) {
            if (bend.flangeFaces[k * 2 + 1] != faceIndex)
                continue;
            const auto otherFace =
                TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[(1 - k) * 2 + 1])));
            BRepAdaptor_Surface surface(otherFace);
            if (surface.GetType() != GeomAbs_Plane)
                continue;
            const auto opposite =
                flangeSize(TopoDS::Face(faces(static_cast<int>(faceIndex))), surface.Plane(), axis);
            if (!opposite)
                continue;
            const double length = gp_Vec(measured.corner, opposite->corner).Dot(gp_Vec(direction));
            const double setback =
                (bend.innerRadiusMm + bend.thicknessMm) * std::tan(bend.angleRadians / 2);
            if (length <= measured.value || std::abs(length - measured.value - setback) > tolerance)
                continue;
            if (extended)
                return; // An ambiguous shared face is not extended.
            extended = FlangeSize{
                measured.corner, measured.corner.Translated(gp_Vec(direction) * length), length, j};
        }
    }
    if (extended)
        measured = *extended;
}
std::optional<ProfileGroup> planProfileGroup(const TopoDS_Shape &shape,
                                             const std::vector<RecognizedBend> &bends,
                                             const std::array<Frame, 4> &frames, double tolerance,
                                             std::size_t &budget, std::stop_token stop) {
    if (bends.size() < 2 || bends.size() > 24)
        return {};
    std::optional<ProfileGroup> best;
    for (std::size_t i = 0; i < bends.size(); ++i) {
        const auto axisOf = [](const RecognizedBend &b) {
            return gp_Dir(gp_Vec(gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z),
                                 gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z)));
        };
        const auto axis = axisOf(bends[i]);
        std::size_t view = 0;
        for (std::size_t k = 1; k < 3; ++k)
            if (std::abs(axis.Dot(frames[k].normal)) > std::abs(axis.Dot(frames[view].normal)))
                view = k;
        const auto &frame = frames[view];
        if (std::abs(axis.Dot(frame.normal)) < 1 - 1e-10)
            continue;
        const auto interval = [&](const RecognizedBend &b) {
            const auto a =
                gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z))
                    .Dot(gp_Vec(frame.normal));
            const auto z = gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z))
                               .Dot(gp_Vec(frame.normal));
            return std::array<double, 2>{std::min(a, z), std::max(a, z)};
        };
        auto span = interval(bends[i]);
        std::vector<std::size_t> members{i};
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t j = 0; j < bends.size(); ++j) {
                if (std::find(members.begin(), members.end(), j) != members.end() ||
                    !axis.IsParallel(axisOf(bends[j]), 1e-7) ||
                    std::none_of(members.begin(), members.end(),
                                 [&](auto k) { return sharedOuterFace(bends[k], bends[j]); }))
                    continue;
                const auto next = interval(bends[j]);
                if (std::min(span[1], next[1]) - std::max(span[0], next[0]) <= tolerance * 20)
                    continue;
                span = {std::max(span[0], next[0]), std::min(span[1], next[1])};
                members.push_back(j);
                changed = true;
            }
        }
        if (members.size() < 2 || (best && members.size() <= best->members.size()))
            continue;
        const DrawingPoint looking{-frame.normal.Dot(frames[0].u), -frame.normal.Dot(frames[0].v)};
        if (std::hypot(looking.x, looking.y) < 1e-7)
            continue;
        const auto station = gp_Pnt(gp_XYZ(frame.normal.X(), frame.normal.Y(), frame.normal.Z()) *
                                    ((span[0] + span[1]) / 2));
        DrawingView section;
        try {
            section = sectionView(shape, frame, station, gp_Pnt(0, 0, 0), 1, budget, stop, true);
        } catch (const DrawingError &) {
            throw;
        } catch (const Standard_Failure &) {
            continue;
        } catch (const std::exception &) {
            continue;
        }
        const double w = section.maximum.x - section.minimum.x;
        const double h = section.maximum.y - section.minimum.y;
        const auto scale = standardDrawingScale(std::min(70. / w, 115. / h));
        double thickness = bends[i].thicknessMm;
        for (auto j : members)
            thickness = std::min(thickness, bends[j].thicknessMm);
        if (std::min(w, h) <= 0 || std::max(w, h) > 8 * std::min(w, h) || !scale ||
            thickness * *scale < .75)
            continue;
        const auto front = bounds(shape, frames[0]);
        const auto p = project(station, frames[0]);
        const DrawingPoint line{-looking.y, looking.x};
        double low = -1e100, high = 1e100;
        bool intersects = true;
        for (int k = 0; k < 2; ++k) {
            const double origin = k == 0 ? p.x : p.y, direction = k == 0 ? line.x : line.y;
            if (std::abs(direction) < 1e-12) {
                intersects = intersects && origin >= front[k] - tolerance &&
                             origin <= front[k + 2] + tolerance;
                continue;
            }
            const double a = (front[k] - origin) / direction,
                         b = (front[k + 2] - origin) / direction;
            low = std::max(low, std::min(a, b));
            high = std::min(high, std::max(a, b));
        }
        if (!intersects || high <= low)
            continue;
        best = ProfileGroup{std::move(members),
                            frame,
                            station,
                            std::move(section),
                            {{p.x + line.x * low, p.y + line.y * low},
                             {p.x + line.x * high, p.y + line.y * high},
                             looking,
                             ""}};
    }
    return best;
}

struct VariableSections {
    std::vector<DrawingDetail> details;
    std::vector<DrawingSectionMarker> markers;
    std::vector<DrawingDimension> positions;
    DrawingDirectionNote direction;
};
std::optional<VariableSections> variableSections(const TopoDS_Shape &shape,
                                                 const TopTools_IndexedMapOfShape &faces,
                                                 const std::vector<RecognizedBend> &bends,
                                                 const ProfileGroup &group, const Frame &front,
                                                 std::size_t &budget, std::stop_token stop) {
    if (bends.size() < 3 || bends.size() > 8)
        return {};
    const auto axisOf = [](const RecognizedBend &b) {
        return gp_Dir(gp_Vec(gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z),
                             gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z)));
    };
    std::vector<std::size_t> members = group.members;
    for (std::size_t k = 0; k < members.size(); ++k)
        for (std::size_t j = 0; j < bends.size(); ++j)
            if (std::find(members.begin(), members.end(), j) == members.end() &&
                sharedOuterFace(bends[members[k]], bends[j]) &&
                std::abs(axisOf(bends[j]).Dot(group.frame.normal)) >
                    std::cos(std::numbers::pi / 12))
                members.push_back(j);
    if (members.size() != bends.size() || members.size() == group.members.size())
        return {}; // Do not replace profiles for an unmeasured disconnected group.
    double low = -1e100, high = 1e100;
    for (const auto &bend : bends) {
        const auto at = [&](auto p) {
            return gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(p.x, p.y, p.z)).Dot(gp_Vec(group.frame.normal));
        };
        const double a = at(bend.axisStart), b = at(bend.axisEnd);
        low = std::max(low, std::min(a, b));
        high = std::min(high, std::max(a, b));
    }
    const double thickness = bends.front().thicknessMm;
    if (high - low < 10 * thickness)
        return {};
    const double margin = std::min((high - low) / 4, std::max(2 * thickness, (high - low) * .1));
    VariableSections result;
    std::size_t tilted = members.front();
    for (auto index : members)
        if (!axisOf(bends[index]).IsParallel(group.frame.normal, 1e-7)) {
            tilted = index;
            break;
        }
    const auto &oblique = bends[tilted];
    for (const auto &bend : bends)
        if (!axisOf(bend).IsParallel(group.frame.normal, 1e-7) &&
            !axisOf(bend).IsParallel(axisOf(oblique), 1e-7))
            return {}; // Two measured stations do not describe several unrelated axis directions.
    const gp_Pnt middle((oblique.axisStart.x + oblique.axisEnd.x) / 2,
                        (oblique.axisStart.y + oblique.axisEnd.y) / 2,
                        (oblique.axisStart.z + oblique.axisEnd.z) / 2);
    result.direction = {
        project(middle, front), tilted, group.members.front(),
        std::acos(std::clamp(std::abs(axisOf(oblique).Dot(group.frame.normal)), 0., 1.))};
    const auto frontBounds = bounds(shape, front);
    const bool horizontal =
        std::abs(group.frame.normal.Dot(front.u)) >= std::abs(group.frame.normal.Dot(front.v));
    std::array<double, 2> coordinates{low + margin, high - margin};
    if (group.frame.normal.Dot(horizontal ? front.u : front.v) < 0)
        std::reverse(coordinates.begin(), coordinates.end());
    for (const double coordinate : coordinates) {
        if (stop.stop_requested())
            throw DrawingError{"Cancelled", true};
        const auto station = gp_Pnt(0, 0, 0).Translated(gp_Vec(group.frame.normal) * coordinate);
        auto profile = sectionView(shape, group.frame, station, station, 1, budget, stop, true);
        const double w = profile.maximum.x - profile.minimum.x;
        const double h = profile.maximum.y - profile.minimum.y;
        const auto scale = standardDrawingScale(std::min(70. / w, 115. / h));
        if (!scale || thickness * *scale < .75)
            return {};
        std::vector<gp_Pnt> corners;
        for (const auto &bend : bends) {
            const auto first =
                BRepAdaptor_Surface(TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[1]))))
                    .Plane();
            const auto second =
                BRepAdaptor_Surface(TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[3]))))
                    .Plane();
            IntAna_QuadQuadGeo intersect(first, second, 1e-8, 1e-6);
            if (!intersect.IsDone() || intersect.TypeInter() != IntAna_Line ||
                intersect.NbSolutions() != 1)
                return {};
            const auto rail = intersect.Line(1);
            const double denominator = rail.Direction().Dot(group.frame.normal);
            if (std::abs(denominator) < .9)
                return {};
            corners.push_back(rail.Location().Translated(
                gp_Vec(rail.Direction()) *
                (gp_Vec(rail.Location(), station).Dot(gp_Vec(group.frame.normal)) / denominator)));
        }
        bool thicknessAdded = false;
        for (std::size_t index = 0; index < bends.size(); ++index) {
            const auto &bend = bends[index];
            std::array<gp_Vec, 2> directions;
            for (std::size_t side = 0; side < 2; ++side) {
                const auto faceId = bend.flangeFaces[side * 2 + 1];
                const auto face = TopoDS::Face(faces(static_cast<int>(faceId)));
                const auto plane = BRepAdaptor_Surface(face).Plane();
                gp_Vec direction = gp_Vec(plane.Axis().Direction())
                                       .Crossed(gp_Vec(group.frame.normal))
                                       .Normalized();
                BRepAlgoAPI_Section section(face, gp_Pln(station, group.frame.normal), false);
                section.Build();
                if (!section.IsDone())
                    return {};
                double minimum = 1e100, maximum = -1e100;
                for (TopExp_Explorer vi(section.Shape(), TopAbs_VERTEX); vi.More(); vi.Next()) {
                    const double value =
                        gp_Vec(corners[index], BRep_Tool::Pnt(TopoDS::Vertex(vi.Current())))
                            .Dot(direction);
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                }
                if (minimum == 1e100 || (minimum < -1e-5 && maximum > 1e-5))
                    return {};
                if (std::abs(minimum) > std::abs(maximum)) {
                    direction.Reverse();
                    maximum = -minimum;
                }
                directions[side] = direction;
                const auto realEnd = corners[index].Translated(direction * maximum);
                auto end = realEnd;
                for (std::size_t other = 0; other < bends.size(); ++other) {
                    if (other == index)
                        continue;
                    for (std::size_t os = 0; os < 2; ++os) {
                        if (bends[other].flangeFaces[os * 2 + 1] != faceId)
                            continue;
                        const gp_Vec distance(corners[index], corners[other]);
                        const double extent = distance.Dot(direction);
                        const gp_Vec across = gp_Vec(plane.Axis().Direction())
                                                  .Crossed(gp_Vec(axisOf(bends[other])))
                                                  .Normalized();
                        const double factor = std::abs(direction.Dot(across));
                        if (factor < .9 || distance.Crossed(direction).Magnitude() > 1e-5)
                            continue;
                        const double setback =
                            (bends[other].innerRadiusMm + bends[other].thicknessMm) *
                            std::tan(bends[other].angleRadians / 2) / factor;
                        if (std::abs(extent - maximum - setback) < 1e-4)
                            end = corners[other];
                    }
                }
                const DrawingDimension dimension{
                    project(corners[index], group.frame), project(end, group.frame),
                    corners[index].Distance(end), "H" + std::to_string(faceId)};
                const auto equal = [](DrawingPoint a, DrawingPoint b) {
                    return std::hypot(a.x - b.x, a.y - b.y) < 1e-5;
                };
                if (std::none_of(profile.dimensions.begin(), profile.dimensions.end(),
                                 [&](const auto &old) {
                                     return (equal(old.start, dimension.start) &&
                                             equal(old.end, dimension.end)) ||
                                            (equal(old.start, dimension.end) &&
                                             equal(old.end, dimension.start));
                                 }))
                    profile.dimensions.push_back(dimension);
                if (!thicknessAdded) {
                    const auto inner =
                        TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[side * 2])));
                    const auto innerPlane = BRepAdaptor_Surface(inner).Plane();
                    const double distance = gp_Vec(innerPlane.Location(), realEnd)
                                                .Dot(gp_Vec(innerPlane.Axis().Direction()));
                    const auto inside =
                        realEnd.Translated(-gp_Vec(innerPlane.Axis().Direction()) * distance);
                    BRepClass_FaceClassifier classifier(inner, inside, 1e-5);
                    if (std::abs(std::abs(distance) - thickness) < 1e-5 &&
                        (classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON)) {
                        profile.dimensions.push_back({project(inside, group.frame),
                                                      project(realEnd, group.frame), thickness,
                                                      "t"});
                        thicknessAdded = true;
                    }
                }
            }
            const double interior = std::numbers::pi - bend.angleRadians;
            if (std::abs(std::acos(std::clamp(directions[0].Dot(directions[1]), -1., 1.)) -
                         interior) > 1e-5)
                return {}; // An oblique section angle must not masquerade as the true bend angle.
            profile.angles.push_back(
                {project(corners[index], group.frame),
                 project(corners[index].Translated(directions[0]), group.frame),
                 project(corners[index].Translated(directions[1]), group.frame), interior,
                 (bend.innerRadiusMm + bend.thicknessMm) / std::sin(interior / 2) -
                     bend.innerRadiusMm});
            auto anchor = bendSurfacePoint(bend, faces, station);
            const auto axis = axisOf(bend);
            anchor.Translate(gp_Vec(axis) *
                             gp_Vec(anchor, station).Dot(gp_Vec(group.frame.normal)) /
                             axis.Dot(group.frame.normal));
            BRepClass_FaceClassifier classifier(
                TopoDS::Face(faces(static_cast<int>(bend.innerFace))), anchor, 1e-5);
            if (classifier.State() != TopAbs_IN && classifier.State() != TopAbs_ON)
                return {};
            profile.bendNotes.push_back({project(anchor, group.frame), index});
        }
        if (!result.details.empty()) {
            const auto &first = result.details.front().view;
            std::erase_if(profile.dimensions, [&](const auto &dimension) {
                return std::any_of(first.dimensions.begin(), first.dimensions.end(),
                                   [&](const auto &old) {
                                       return old.label == dimension.label &&
                                              std::abs(old.valueMm - dimension.valueMm) < 1e-5;
                                   });
            });
            if (profile.dimensions.empty())
                return {};             // This is not a variable profile.
            profile.bendNotes.clear(); // R/L and unchanged dimensions appear on the first section.
        }
        const auto designation = "А" + std::to_string(result.details.size() + 1);
        result.details.push_back({std::move(profile), group.members.front(), designation, true});
        auto marker = group.marker;
        const auto shift = project(station, front);
        const auto previous = project(group.station, front);
        marker.first.x += shift.x - previous.x;
        marker.first.y += shift.y - previous.y;
        marker.second.x += shift.x - previous.x;
        marker.second.y += shift.y - previous.y;
        marker.designation = designation;
        result.markers.push_back(marker);
        result.positions.push_back(
            {{frontBounds[0], frontBounds[1]},
             horizontal ? DrawingPoint{shift.x, frontBounds[1]}
                        : DrawingPoint{frontBounds[0], shift.y},
             horizontal ? std::abs(shift.x - frontBounds[0]) : std::abs(shift.y - frontBounds[1]),
             ""});
    }
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
        auto bendAnalysis = metadata.bendAnalysis;
        bool drawingOnlyRecognition = false;
        bool retryMeasurements = bendAnalysis.status == BendRecognitionStatus::Unsupported &&
                                 metadata.maximumToleranceMm > .001 &&
                                 metadata.maximumToleranceMm <= .01;
        if (bendAnalysis.status == BendRecognitionStatus::Completed) {
            std::vector<bool> used(static_cast<std::size_t>(faces.Extent()) + 1);
            for (const auto &bend : bendAnalysis.bends) {
                used[bend.innerFace] = true;
                used[bend.outerFace] = true;
            }
            for (const auto &d : bendAnalysis.interiorContourDiagnostics) {
                used[d.candidate.innerFace] = true;
                used[d.candidate.outerFace] = true;
            }
            for (int i = 1; i <= faces.Extent(); ++i) {
                BRepAdaptor_Surface s(TopoDS::Face(faces(i)));
                if (used[static_cast<std::size_t>(i)] || s.GetType() != GeomAbs_Cylinder)
                    continue;
                double u0, u1, v0, v1;
                BRepTools::UVBounds(TopoDS::Face(faces(i)), u0, u1, v0, v1);
                if (u1 - u0 > std::numbers::pi / 180 &&
                    u1 - u0 < std::numbers::pi - std::numbers::pi / 180)
                    retryMeasurements = true;
            }
        }
        if (retryMeasurements) {
            auto measurements = recognizeBends(shape, metadata.maximumToleranceMm, stop,
                                               BendRecognitionPurpose::DrawingMeasurements);
            if (bendAnalysis.status != BendRecognitionStatus::Completed) {
                drawingOnlyRecognition = true;
                bendAnalysis = std::move(measurements);
            } else if (measurements.status == BendRecognitionStatus::Completed) {
                // Keep every strict result. A drawing-only candidate may only add
                // a pair whose walls are not already used by another strict zone.
                const auto occupied = [&](const RecognizedBend &b) {
                    const auto sharesWall = [&](const RecognizedBend &old) {
                        return old.innerFace == b.innerFace || old.outerFace == b.outerFace ||
                               old.innerFace == b.outerFace || old.outerFace == b.innerFace;
                    };
                    return std::any_of(bendAnalysis.bends.begin(), bendAnalysis.bends.end(),
                                       sharesWall) ||
                           std::any_of(bendAnalysis.interiorContourDiagnostics.begin(),
                                       bendAnalysis.interiorContourDiagnostics.end(),
                                       [&](const auto &old) { return sharesWall(old.candidate); });
                };
                for (const auto &b : measurements.bends)
                    if (!occupied(b)) {
                        bendAnalysis.bends.push_back(b);
                        drawingOnlyRecognition = true;
                    }
                for (const auto &d : measurements.interiorContourDiagnostics)
                    if (!occupied(d.candidate)) {
                        bendAnalysis.interiorContourDiagnostics.push_back(d);
                        drawingOnlyRecognition = true;
                    }
                if (drawingOnlyRecognition)
                    bendAnalysis.lengthToleranceMm =
                        std::max(bendAnalysis.lengthToleranceMm, measurements.lengthToleranceMm);
            }
        }

        // A parallel bend axis makes the left view a useful true profile of the bend.
        u = preferredAxis(bendAnalysis.bends, base, side, u);
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
        std::array<Frame, 4> frames{{{u, v, n},
                                     {u, n.Reversed(), v},
                                     {n, v, u.Reversed()},
                                     {isoU, gp_Dir(gp_Vec(isoN).Crossed(gp_Vec(isoU))), isoN}}};
        auto drawing = std::make_shared<BendingDrawing>();
        drawing->baseFace = base;
        drawing->drawingOnlyRecognition = drawingOnlyRecognition;
        drawing->bendRecognitionStatus = bendAnalysis.status;
        drawing->automaticBase = !request.baseFace;
        drawing->reverseView = request.reverseView;
        drawing->unrecognizedCylindricalFaces = bendAnalysis.unrecognizedCylindricalFaces;
        const auto utf8 = metadata.sourcePath.filename().u8string();
        drawing->sourceName.assign(utf8.begin(), utf8.end());
        std::size_t budget = 0;
        const auto openings = drawingOpenings(shape, faces, stop);
        auto profileGroup = planProfileGroup(shape, bendAnalysis.bends, frames,
                                             bendAnalysis.lengthToleranceMm, budget, stop);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            drawing->views[i] = viewOf(shape, frames[i], budget, stop, i < 3 ? &openings : nullptr);
            if (i > 0 && i < 3 && std::any_of(openings.begin(), openings.end(), [&](const auto &o) {
                    return o.normal.IsParallel(frames[i].normal, 1e-7);
                })) {
                auto opposite = viewOf(
                    shape, {frames[i].u, frames[i].v.Reversed(), frames[i].normal.Reversed()},
                    budget, stop, &openings);
                if (opposite.visibleOpeningCount > drawing->views[i].visibleOpeningCount) {
                    frames[i].v.Reverse();
                    frames[i].normal.Reverse();
                    opposite.reverseProjection = true;
                    drawing->views[i] = std::move(opposite);
                }
            }
        }
        const auto &fv = drawing->views[0];
        const double width = fv.maximum.x - fv.minimum.x;
        const double section = std::max(fv.maximum.y - fv.minimum.y,
                                        drawing->views[1].maximum.y - drawing->views[1].minimum.y);
        if (request.allowBreaks && !profileGroup && drawingScale(*drawing).value_or(0) < .5 &&
            width > 300 && width > 5 * section &&
            homogeneous(shape, frames[0], fv.minimum.x, fv.maximum.x)) {
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
                    if (measured[i]) {
                        extendToAdjacentBend(*measured[i], bend.flangeFaces[i * 2 + 1],
                                             drawing->bends.size(), bendAnalysis.bends, faces, axis,
                                             bendAnalysis.lengthToleranceMm);
                        row.outerFlangeMm[i] = measured[i]->value;
                    }
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
                                       !trueProfile || bendAnalysis.bends.size() > 1);
                const bool grouped =
                    profileGroup &&
                    std::find(profileGroup->members.begin(), profileGroup->members.end(),
                              drawing->bends.size()) != profileGroup->members.end();
                auto profileFrame = grouped ? profileGroup->frame : frames[best];
                std::optional<DrawingView> detail;
                const gp_Pnt station =
                    grouped ? profileGroup->station
                            : gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z)
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
                        const bool virtualEnd = m.virtualEnd.has_value();
                        const auto checkedFace =
                            virtualEnd ? TopoDS::Face(faces(static_cast<int>(
                                             bendAnalysis.bends[*m.virtualEnd].innerFace)))
                                       : TopoDS::Face(
                                             faces(static_cast<int>(bend.flangeFaces[i * 2 + 1])));
                        const auto checkedPoint =
                            virtualEnd ? bendSurfacePoint(bendAnalysis.bends[*m.virtualEnd], faces,
                                                          station)
                                       : endPoint;
                        BRepClass_FaceClassifier onSection(checkedFace, checkedPoint, 1e-5);
                        if (onSection.State() != TopAbs_IN && onSection.State() != TopAbs_ON)
                            continue;
                    }
                    const auto end = project(m.end, profileFrame);
                    const bool insideDetail =
                        grouped || !detail ||
                        std::hypot(end.x - (profile.minimum.x + profile.maximum.x) / 2,
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
                    if (m.virtualEnd || thicknessAdded || (!detail && !drawing->bends.empty()))
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
        for (const auto &bend : bendAnalysis.bends)
            addBend(bend, false);
        for (const auto &d : bendAnalysis.interiorContourDiagnostics)
            addBend(d.candidate, true);
        if (profileGroup &&
            std::all_of(
                profileGroup->members.begin(), profileGroup->members.end(), [&](auto index) {
                    return std::any_of(drawing->details.begin(), drawing->details.end(),
                                       [index](const auto &d) { return d.bendIndex == index; });
                })) {
            auto combined = std::move(profileGroup->view);
            for (auto index : profileGroup->members) {
                const auto detail =
                    std::find_if(drawing->details.begin(), drawing->details.end(),
                                 [index](const auto &d) { return d.bendIndex == index; });
                const auto &detailView = detail->view;
                combined.angles.insert(combined.angles.end(), detailView.angles.begin(),
                                       detailView.angles.end());
                combined.bendNotes.insert(combined.bendNotes.end(), detailView.bendNotes.begin(),
                                          detailView.bendNotes.end());
                for (const auto &dimension : detailView.dimensions) {
                    const auto equalPoint = [](DrawingPoint a, DrawingPoint b) {
                        return std::hypot(a.x - b.x, a.y - b.y) < 1e-5;
                    };
                    const bool duplicate =
                        std::any_of(combined.dimensions.begin(), combined.dimensions.end(),
                                    [&](const auto &old) {
                                        return std::abs(old.valueMm - dimension.valueMm) < 1e-5 &&
                                               ((old.label == "t" && dimension.label == "t") ||
                                                (equalPoint(old.start, dimension.start) &&
                                                 equalPoint(old.end, dimension.end)) ||
                                                (equalPoint(old.start, dimension.end) &&
                                                 equalPoint(old.end, dimension.start)));
                                    });
                    if (!duplicate)
                        combined.dimensions.push_back(dimension);
                }
            }
            std::vector<DrawingDetail> details;
            details.push_back({std::move(combined), profileGroup->members.front(), "А1", true});
            for (auto &detail : drawing->details)
                if (std::find(profileGroup->members.begin(), profileGroup->members.end(),
                              detail.bendIndex) == profileGroup->members.end())
                    details.push_back(std::move(detail));
            drawing->details = std::move(details);
            for (auto &projection : drawing->views) {
                projection.labels.clear();
                projection.detailCircles.clear();
            }
            profileGroup->marker.designation = "А1";
            drawing->views[0].sectionMarkers.push_back(profileGroup->marker);
            for (std::size_t i = 1; i < drawing->details.size(); ++i) {
                auto &detail = drawing->details[i];
                detail.designation = "А" + std::to_string(i + 1);
                const auto &bend = bendAnalysis.bends[detail.bendIndex];
                const gp_Pnt station =
                    gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z)
                        .Translated(
                            gp_Vec(gp_Pnt(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z),
                                   gp_Pnt(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z)) *
                            .5);
                const auto at = bendSurfacePoint(bend, faces, station);
                drawing->views[0].labels.push_back({project(at, frames[0]), detail.designation});
                drawing->views[0].detailCircles.push_back(
                    {project(at, frames[0]), (detail.view.maximum.x - detail.view.minimum.x) / 2});
            }
        }
        for (std::size_t i = 0; i < bendAnalysis.bends.size(); ++i)
            for (std::size_t j = i + 1; j < bendAnalysis.bends.size(); ++j) {
                const auto &a = bendAnalysis.bends[i], &b = bendAnalysis.bends[j];
                const gp_Dir first(gp_Vec(gp_Pnt(a.axisStart.x, a.axisStart.y, a.axisStart.z),
                                          gp_Pnt(a.axisEnd.x, a.axisEnd.y, a.axisEnd.z)));
                const gp_Dir second(gp_Vec(gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z),
                                           gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z)));
                if (sharedOuterFace(a, b) && !first.IsParallel(second, 1e-7) &&
                    std::abs(first.Dot(second)) > std::cos(std::numbers::pi / 12))
                    drawing->variableProfile = DrawingVariableProfile::NotMeasured;
            }
        if (profileGroup && drawing->bends.size() == bendAnalysis.bends.size()) {
            try {
                if (auto variable = variableSections(shape, faces, bendAnalysis.bends,
                                                     *profileGroup, frames[0], budget, stop)) {
                    drawing->variableProfile = DrawingVariableProfile::Measured;
                    drawing->details = std::move(variable->details);
                    for (auto &projection : drawing->views) {
                        projection.labels.clear();
                        projection.detailCircles.clear();
                        projection.sectionMarkers.clear();
                    }
                    drawing->views[0].sectionMarkers = std::move(variable->markers);
                    drawing->views[0].sectionDimensions = std::move(variable->positions);
                    drawing->views[0].directionNotes.push_back(variable->direction);
                }
            } catch (const Standard_Failure &) {
                // Optional measurements must not hide the ordinary projections.
            } catch (const std::runtime_error &) {
                // NotMeasured remains visible on the sheet; cancellation is not caught here.
            }
        }
        // Full circular surfaces (for example, bores) are not press-bend candidates.
        std::vector<bool> used(static_cast<std::size_t>(faces.Extent()) + 1);
        for (const auto &bend : bendAnalysis.bends) {
            used[bend.innerFace] = true;
            used[bend.outerFace] = true;
        }
        for (const auto &d : bendAnalysis.interiorContourDiagnostics) {
            used[d.candidate.innerFace] = true;
            used[d.candidate.outerFace] = true;
        }
        for (const auto &opening : openings)
            for (auto faceIndex : opening.wallFaces)
                used[faceIndex] = true;
        drawing->unrecognizedCylindricalFaces = 0;
        for (int i = 1; i <= faces.Extent(); ++i) {
            BRepAdaptor_Surface cylinderSurface(TopoDS::Face(faces(i)));
            if (cylinderSurface.GetType() != GeomAbs_Cylinder || used[static_cast<std::size_t>(i)])
                continue;
            double u0, u1, v0, v1;
            BRepTools::UVBounds(TopoDS::Face(faces(i)), u0, u1, v0, v1);
            if (u1 - u0 < 2 * std::numbers::pi - 1e-5)
                ++drawing->unrecognizedCylindricalFaces;
        }
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
