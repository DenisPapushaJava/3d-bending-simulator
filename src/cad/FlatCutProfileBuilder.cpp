#include "FlatCutProfileBuilder.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomConvert_BSplineCurveToBezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BezierCurve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Lin.hxx>
#include <numbers>
#include <set>

namespace bending::cad {
namespace {
using namespace application;
constexpr double fullTurn = 2 * std::numbers::pi;
constexpr double angleTolerance = 1.e-7;
const gp_Dir vertical(0, 0, 1);

PointMm xy(const gp_Pnt &point) {
    return {point.X(), point.Y(), 0};
}

// A positive-weight Bézier arc lies in its control polygon's convex hull.
// Bound the entire arc against each emitted chord and split only as needed.
constexpr double maximumChordError = .0099; // Leave .0001 mm for existing wire joins.
constexpr double maximumLinearJoinRepairMm = .001;
constexpr std::size_t maximumCutVertices = 32768;

bool appendBezier(const Handle(Geom_BezierCurve) & arc, bool reversed, unsigned depth,
                  double chordError, std::vector<FlatCutVertex> &vertices, std::stop_token stop) {
    if (stop.stop_requested() || vertices.size() >= maximumCutVertices)
        return false;
    const auto start = arc->StartPoint(), end = arc->EndPoint();
    const gp_Vec chord(start, end);
    const double length = chord.Magnitude();
    if (!std::isfinite(length) || length <= 1.e-8)
        return false;
    const gp_Vec direction = chord / length;
    double previous = 0;
    bool fits = true;
    for (int i = 1; i <= arc->NbPoles(); ++i) {
        const gp_Vec relative(start, arc->Pole(i));
        const double along = relative.Dot(direction);
        if (arc->Weight(i) <= 0 || !std::isfinite(along) || along < previous - 1.e-9 ||
            along > length + 1.e-9 ||
            (relative - direction * std::clamp(along, 0., length)).Magnitude() > chordError) {
            fits = false;
            break;
        }
        previous = along;
    }
    if (fits) {
        vertices.push_back({xy(reversed ? end : start), 0});
        return true;
    }
    if (depth == 12)
        return false;
    auto left = Handle(Geom_BezierCurve)::DownCast(arc->Copy());
    auto right = Handle(Geom_BezierCurve)::DownCast(arc->Copy());
    left->Segment(0, .5);
    right->Segment(.5, 1);
    return appendBezier(reversed ? right : left, reversed, depth + 1, chordError, vertices, stop) &&
           appendBezier(reversed ? left : right, reversed, depth + 1, chordError, vertices, stop);
}

bool appendSpline(const BRepAdaptor_Curve &curve, bool reversed, double tolerance,
                  double chordError, std::vector<FlatCutVertex> &vertices, std::stop_token stop) {
    const auto spline = curve.BSpline();
    if (spline->IsPeriodic() || spline->NbPoles() > 4096)
        return false;
    GeomConvert_BSplineCurveToBezierCurve spans(spline, curve.FirstParameter(),
                                                curve.LastParameter(), 1.e-10);
    for (int i = 1; i <= spans.NbArcs(); ++i) {
        const auto arc = spans.Arc(reversed ? spans.NbArcs() + 1 - i : i);
        for (int j = 1; j <= arc->NbPoles(); ++j)
            if (std::abs(arc->Pole(j).Z()) > tolerance)
                return false;
        if (!appendBezier(arc, reversed, 0, chordError, vertices, stop))
            return false;
    }
    return spans.NbArcs() > 0;
}

std::optional<gp_Circ> completeCircle(const TopoDS_Wire &wire, double tolerance);

struct CurveEnd {
    std::size_t firstCutVertex;
    gp_Pnt point;
    TopoDS_Edge edge;
};

// Move only endpoints with a shared topological vertex. For a spline the
// displacement adds to its chord error, so each gets half the export budget.
constexpr double splineJoinBudget = .005;
bool repairCutJoin(const CurveEnd &current, const CurveEnd &following, PointMm &next,
                   double tolerance, bool allowSplineJoin) {
    const auto a = BRepAdaptor_Curve(current.edge).GetType();
    const auto b = BRepAdaptor_Curve(following.edge).GetType();
    const bool splineJoin = allowSplineJoin && ((a == GeomAbs_BSplineCurve && b == GeomAbs_Line) ||
                                                (b == GeomAbs_BSplineCurve && a == GeomAbs_Line));
    if (!splineJoin && (a != GeomAbs_Line || b != GeomAbs_Line))
        return false;
    const auto vertex = TopExp::LastVertex(current.edge, true);
    if (vertex.IsNull() || !vertex.IsSame(TopExp::FirstVertex(following.edge, true)))
        return false;
    const auto point = BRep_Tool::Pnt(vertex);
    const double vertexTolerance = BRep_Tool::Tolerance(vertex);
    if (!std::isfinite(vertexTolerance))
        return false;
    const double limit = std::min(
        {tolerance, vertexTolerance, splineJoin ? splineJoinBudget : maximumLinearJoinRepairMm});
    const auto nextPoint = gp_Pnt(next.x, next.y, 0);
    if (!(std::hypot(current.point.X() - next.x, current.point.Y() - next.y) <= limit &&
          point.Distance(current.point) <= limit && point.Distance(nextPoint) <= limit))
        return false;
    next = xy(point);
    return true;
}

std::optional<FlatCutLoop> readLoop(const TopoDS_Wire &wire, double tolerance,
                                    bool *approximated = nullptr, std::stop_token stop = {},
                                    bool allowSplineJoin = false) {
    FlatCutLoop loop;
    std::vector<CurveEnd> ends;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(wire, TopAbs_EDGE, edges);
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        const auto edge = it.Current();
        const BRepAdaptor_Curve curve(edge);
        const double first = curve.FirstParameter(), last = curve.LastParameter();
        const bool reversed = edge.Orientation() == TopAbs_REVERSED;
        const auto start = curve.Value(reversed ? last : first);
        const auto end = curve.Value(reversed ? first : last);
        if (std::max(std::abs(start.Z()), std::abs(end.Z())) > tolerance ||
            !std::isfinite(start.X()) || !std::isfinite(start.Y()) || !std::isfinite(end.X()) ||
            !std::isfinite(end.Y()))
            return {};
        const auto vertex = loop.vertices.size();
        if (approximated && curve.GetType() == GeomAbs_BSplineCurve) {
            const double chordError = allowSplineJoin ? splineJoinBudget : maximumChordError;
            if (!appendSpline(curve, reversed, tolerance, chordError, loop.vertices, stop))
                return {};
            *approximated = true;
            ends.push_back({vertex, end, edge});
            continue;
        }
        double bulge = 0;
        if (curve.GetType() == GeomAbs_Circle) {
            const auto circle = curve.Circle();
            if (!circle.Axis().Direction().IsParallel(vertical, angleTolerance))
                return {};
            const double sweep = last - first;
            if (edges.Extent() == 1 && std::abs(sweep - fullTurn) <= angleTolerance)
                return FlatCutLoop{{}, xy(circle.Location()), circle.Radius()};
            if (sweep <= angleTolerance || sweep >= fullTurn - angleTolerance)
                return {};
            bulge =
                std::tan(sweep / 4) * ((circle.Axis().Direction().Z() < 0) != reversed ? -1 : 1);
        } else if (curve.GetType() != GeomAbs_Line ||
                   std::abs(curve.Line().Direction().Z()) > angleTolerance)
            return {};
        if (start.Distance(end) <= 1.e-8)
            return {};
        loop.vertices.push_back({xy(start), bulge});
        ends.push_back({vertex, end, edge});
    }
    if (ends.size() < 2 || ends.size() != static_cast<std::size_t>(edges.Extent()))
        return {};
    // Allow small OCCT vertex uncertainty only at an actual join. The exported
    // polyline shares one exact coordinate at every join, including closure.
    const double joinTolerance = std::min(tolerance, 1.e-4);
    for (std::size_t i = 0; i < ends.size(); ++i) {
        const auto &following = ends[(i + 1) % ends.size()];
        auto &next = loop.vertices[following.firstCutVertex].point;
        const auto &end = ends[i].point;
        if (std::hypot(end.X() - next.x, end.Y() - next.y) > joinTolerance) {
            if (!approximated ||
                !repairCutJoin(ends[i], following, next, tolerance, allowSplineJoin))
                return {};
            *approximated = true; // Rebuild and validate the exported face and its holes.
        }
    }
    if (std::all_of(loop.vertices.begin(), loop.vertices.end(),
                    [](const auto &vertex) { return vertex.bulge != 0; })) {
        if (const auto circle = completeCircle(wire, std::min(joinTolerance, 1.e-6)))
            return FlatCutLoop{{}, xy(circle->Location()), circle->Radius()};
    }
    return loop;
}

// Rebuild the emitted contour for validation when any curve was approximated.
TopoDS_Wire makeCutWire(const FlatCutLoop &loop, std::stop_token stop) {
    BRepBuilderAPI_MakeWire wire;
    for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
        if (stop.stop_requested())
            return {};
        const auto &v = loop.vertices[i];
        const auto &next = loop.vertices[(i + 1) % loop.vertices.size()].point;
        const gp_Pnt a(v.point.x, v.point.y, 0), b(next.x, next.y, 0);
        if (v.bulge == 0)
            wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
        else {
            const gp_Pnt mid((a.X() + b.X() + (b.Y() - a.Y()) * v.bulge) / 2,
                             (a.Y() + b.Y() - (b.X() - a.X()) * v.bulge) / 2, 0);
            wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(a, mid, b).Value()).Edge());
        }
        if (!wire.IsDone())
            return {};
    }
    return wire.Wire();
}

double signedArea(const FlatCutLoop &loop) {
    double area = 0;
    for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
        const auto &vertex = loop.vertices[i];
        const auto &a = vertex.point;
        const auto &b = loop.vertices[(i + 1) % loop.vertices.size()].point;
        area += (a.x * b.y - a.y * b.x) / 2;
        if (vertex.bulge != 0) {
            const double angle = 4 * std::atan(vertex.bulge);
            const double radius = std::hypot(a.x - b.x, a.y - b.y) / (2 * std::sin(angle / 2));
            area += radius * radius * (angle - std::sin(angle)) / 2;
        }
    }
    return area;
}

struct Mouth {
    int face;
    TopoDS_Wire wire;
};
struct Openings {
    std::vector<TopoDS_Wire> through;
    std::size_t blind = 0;
};

bool coaxial(const gp_Ax1 &a, const gp_Ax1 &b, double tolerance) {
    return a.Direction().IsParallel(b.Direction(), angleTolerance) &&
           gp_Lin(a).Distance(b.Location()) <= tolerance &&
           gp_Lin(b).Distance(a.Location()) <= tolerance;
}

std::optional<gp_Circ> completeCircle(const TopoDS_Wire &wire, double tolerance) {
    std::optional<gp_Circ> circle;
    double sweep = 0;
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        const auto edge = it.Current();
        const BRepAdaptor_Curve curve(edge);
        if (curve.GetType() != GeomAbs_Circle)
            return {};
        const auto current = curve.Circle();
        if (!circle)
            circle = current;
        if (current.Location().Distance(circle->Location()) > tolerance ||
            std::abs(current.Radius() - circle->Radius()) > tolerance ||
            !coaxial(current.Axis(), circle->Axis(), tolerance))
            return {};
        const bool reversed = (edge.Orientation() == TopAbs_REVERSED) !=
                              (current.Axis().Direction().Dot(circle->Axis().Direction()) < 0);
        const double angle = curve.LastParameter() - curve.FirstParameter();
        if (!(angle > 0) || (sweep != 0 && (sweep < 0) != reversed))
            return {};
        sweep += reversed ? -angle : angle;
    }
    return std::abs(std::abs(sweep) - fullTurn) <= angleTolerance ? circle : std::nullopt;
}

TopoDS_Wire throughWire(const TopoDS_Wire &lower, const TopoDS_Wire &upper,
                        const std::vector<TopoDS_Face> &sides, double tolerance) {
    bool prismatic = true;
    for (const auto &face : sides) {
        const BRepAdaptor_Surface s(face, false);
        prismatic &= (s.GetType() == GeomAbs_Plane &&
                      std::abs(s.Plane().Axis().Direction().Z()) <= angleTolerance) ||
                     (s.GetType() == GeomAbs_Cylinder &&
                      s.Cylinder().Axis().Direction().IsParallel(vertical, angleTolerance)) ||
                     (s.GetType() == GeomAbs_SurfaceOfExtrusion &&
                      s.Direction().IsParallel(vertical, angleTolerance));
    }
    if (prismatic)
        return lower;

    // A countersink is accepted only around a genuine complete cylindrical
    // bore. Its larger coaxial entrance never becomes an extra cut.
    const double alignmentTolerance = std::min(tolerance, 1.e-4);
    const auto a = completeCircle(lower, alignmentTolerance);
    const auto b = completeCircle(upper, alignmentTolerance);
    if (!a || !b || !a->Axis().Direction().IsParallel(vertical, angleTolerance) ||
        !coaxial(a->Axis(), b->Axis(), alignmentTolerance))
        return {};
    std::optional<gp_Cylinder> bore;
    double minimum = std::min(a->Radius(), b->Radius());
    for (const auto &face : sides) {
        const BRepAdaptor_Surface s(face);
        if (s.GetType() == GeomAbs_Cylinder) {
            const auto cylinder = s.Cylinder();
            if (!coaxial(cylinder.Axis(), a->Axis(), alignmentTolerance))
                return {};
            minimum = std::min(minimum, cylinder.Radius());
            if (s.LastUParameter() - s.FirstUParameter() >= fullTurn - angleTolerance &&
                s.LastVParameter() - s.FirstVParameter() > alignmentTolerance &&
                (!bore || cylinder.Radius() < bore->Radius()))
                bore = cylinder;
        } else if (s.GetType() == GeomAbs_Cone) {
            const auto cone = s.Cone();
            if (!coaxial(cone.Axis(), a->Axis(), alignmentTolerance))
                return {};
            const double slope = std::sin(cone.SemiAngle());
            minimum = std::min(minimum, cone.RefRadius() + std::min(s.FirstVParameter() * slope,
                                                                    s.LastVParameter() * slope));
        } else
            return {};
    }
    if (!bore || minimum < bore->Radius() - alignmentTolerance)
        return {};
    const auto direction = gp_Vec(bore->Axis().Direction());
    const auto centre =
        bore->Location().Translated(direction * (-bore->Location().Z() / direction.Z()));
    const auto circle = gp_Circ(gp_Ax2(centre, vertical), bore->Radius());
    return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circle).Edge()).Wire();
}

std::optional<Openings> recoverOpenings(const TopoDS_Shape &original, const ClosedCutouts &cutouts,
                                        const SheetLayout &layout,
                                        const std::map<std::size_t, std::size_t> &owners,
                                        const gp_Trsf &origin, double thickness, double tolerance,
                                        std::stop_token stop) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(original, TopAbs_FACE, faces);
    TopTools_IndexedDataMapOfShapeListOfShape adjacency;
    TopExp::MapShapesAndAncestors(original, TopAbs_EDGE, TopAbs_FACE, adjacency);
    std::set<int> remaining;
    for (auto id : cutouts.boundaryFaces) {
        if (!id || id > static_cast<std::size_t>(faces.Extent()))
            return {};
        remaining.insert(static_cast<int>(id));
    }
    if (remaining.empty())
        return {};
    const auto removed = remaining;
    // Index original mouth edges once. A perforated wall can have hundreds
    // of wires and each component must still resolve to its exact source wire.
    std::vector<Mouth> mouthsById;
    std::vector<std::vector<std::size_t>> mouthsAtEdge(
        static_cast<std::size_t>(adjacency.Extent()) + 1);
    for (const auto &[id, owner] : owners) {
        (void)owner;
        if (stop.stop_requested())
            return {};
        const auto face = TopoDS::Face(faces(static_cast<int>(id)));
        const auto outer = BRepTools::OuterWire(face);
        for (TopoDS_Iterator w(face); w.More(); w.Next()) {
            if (w.Value().IsSame(outer))
                continue;
            const auto wire = TopoDS::Wire(w.Value());
            for (TopoDS_Iterator e(wire); e.More(); e.Next()) {
                const auto index = adjacency.FindIndex(e.Value());
                if (index <= 0)
                    return {};
                mouthsAtEdge[static_cast<std::size_t>(index)].push_back(mouthsById.size());
            }
            mouthsById.push_back({static_cast<int>(id), wire});
        }
    }
    Openings openings;
    while (!remaining.empty()) {
        std::vector<int> component{*remaining.begin()};
        remaining.erase(remaining.begin());
        std::set<std::size_t> mouthIds;
        for (std::size_t i = 0; i < component.size(); ++i) {
            if (stop.stop_requested())
                return {};
            for (TopExp_Explorer e(faces(component[i]), TopAbs_EDGE); e.More(); e.Next()) {
                const auto &edge = e.Current();
                const auto edgeId = adjacency.FindIndex(edge);
                if (edgeId <= 0)
                    return {};
                for (const auto &neighbor : adjacency.FindFromIndex(edgeId)) {
                    const int index = faces.FindIndex(neighbor);
                    if (removed.contains(index)) {
                        if (remaining.erase(index))
                            component.push_back(index);
                        continue;
                    }
                    bool found = false;
                    for (const auto id : mouthsAtEdge[static_cast<std::size_t>(edgeId)]) {
                        if (mouthsById[id].face == index) {
                            mouthIds.insert(id);
                            found = true;
                        }
                    }
                    if (!found)
                        return {};
                }
            }
        }
        // A connected void with exactly one original skin entrance is closed
        // on the other side. Two mouths must belong to opposite skins of one wall.
        if (mouthIds.empty() || mouthIds.size() > 2)
            return {};
        if (mouthIds.size() == 1) {
            ++openings.blind;
            continue;
        }
        std::array<Mouth, 2> mouths{mouthsById[*mouthIds.begin()], mouthsById[*mouthIds.rbegin()]};
        const auto owner = owners.find(static_cast<std::size_t>(mouths[0].face));
        const auto other = owners.find(static_cast<std::size_t>(mouths[1].face));
        if (owner == owners.end() || other == owners.end() || owner->second != other->second)
            return {};
        const gp_Trsf map = origin * layout.walls.at(owner->second).toFlat;
        const auto mappedHeight = [&](const Mouth &mouth) {
            return BRepAdaptor_Surface(TopoDS::Face(faces(mouth.face)), false)
                .Plane()
                .Location()
                .Transformed(map)
                .Z();
        };
        if (mappedHeight(mouths[0]) > mappedHeight(mouths[1]))
            std::swap(mouths[0], mouths[1]);
        if (std::abs(mappedHeight(mouths[0])) > tolerance ||
            std::abs(mappedHeight(mouths[1]) - thickness) > tolerance)
            return {};
        gp_Trsf shift;
        shift.SetTranslation(gp_Vec(0, 0, -mappedHeight(mouths[0])));
        const auto transform = shift * map;
        const auto lower =
            TopoDS::Wire(BRepBuilderAPI_Transform(mouths[0].wire, transform, true).Shape());
        const auto upper =
            TopoDS::Wire(BRepBuilderAPI_Transform(mouths[1].wire, transform, true).Shape());
        std::vector<TopoDS_Face> sides;
        for (int index : component)
            sides.push_back(
                TopoDS::Face(BRepBuilderAPI_Transform(faces(index), transform, true).Shape()));
        auto wire = throughWire(lower, upper, sides, tolerance);
        if (wire.IsNull())
            return {};
        openings.through.push_back(std::move(wire));
    }
    return openings;
}
} // namespace

std::shared_ptr<const application::FlatCutProfile>
buildFlatCutProfile(const TopoDS_Face &flat, const TopoDS_Shape &original,
                    const application::ClosedCutouts &cutouts, const SheetLayout &layout,
                    const std::map<std::size_t, std::size_t> &owners, const gp_Trsf &origin,
                    double thickness, double tolerance, std::stop_token stop, bool midThickness,
                    CutJoinPolicy joinPolicy, TopoDS_Face *verifiedStock) {
    if (cutouts.status != ClosedCutoutStatus::None && cutouts.status != ClosedCutoutStatus::Filled)
        return {};
    auto result = std::make_shared<FlatCutProfile>();
    result->midThickness = midThickness;
    auto face = TopoDS::Face(flat.Oriented(TopAbs_FORWARD));
    const auto outer = BRepTools::OuterWire(face);
    if (outer.IsNull())
        return {};
    const bool joinSplineLines = joinPolicy == CutJoinPolicy::SplineLine;
    bool approximated = false;
    auto exterior = readLoop(outer, tolerance, &approximated, stop, joinSplineLines);
    if (!exterior)
        return {};
    std::size_t approximatedVertices = approximated ? exterior->vertices.size() : 0;
    if (approximatedVertices > maximumCutVertices)
        return {};
    const auto outerForCheck = approximated ? makeCutWire(*exterior, stop) : outer;
    if (outerForCheck.IsNull())
        return {};
    result->loops.push_back(std::move(*exterior));
    std::vector<TopoDS_Wire> innerForCheck;
    for (TopoDS_Iterator w(face); w.More(); w.Next()) {
        if (w.Value().IsSame(outer))
            continue;
        if (stop.stop_requested())
            return {};
        const auto wire = TopoDS::Wire(w.Value());
        bool innerApproximated = false;
        auto loop = readLoop(wire, tolerance, &innerApproximated, stop, joinSplineLines);
        if (!loop)
            return {};
        if (innerApproximated) {
            if (loop->vertices.size() > maximumCutVertices - approximatedVertices)
                return {};
            approximatedVertices += loop->vertices.size();
        }
        auto checked = innerApproximated ? makeCutWire(*loop, stop) : wire;
        if (checked.IsNull())
            return {};
        approximated |= innerApproximated;
        innerForCheck.push_back(std::move(checked));
        result->loops.push_back(std::move(*loop));
    }
    if (approximated) {
        BRepBuilderAPI_MakeFace rebuilt(BRepAdaptor_Surface(face).Plane(), outerForCheck, true);
        for (const auto &wire : innerForCheck)
            rebuilt.Add(wire);
        if (!rebuilt.IsDone() || !BRepCheck_Analyzer(rebuilt.Face()).IsValid())
            return {};
        face = rebuilt.Face();
    }
    if (cutouts.status == ClosedCutoutStatus::Filled) {
        auto openings =
            recoverOpenings(original, cutouts, layout, owners, origin, thickness, tolerance, stop);
        if (!openings)
            return {};
        result->blindOpenings = openings->blind;
        if (openings->through.empty() && !verifiedStock)
            return result;
        BRepBuilderAPI_MakeFace check(face);
        const double normal = BRepAdaptor_Surface(face).Plane().Axis().Direction().Z();
        for (auto &wire : openings->through) {
            if (stop.stop_requested())
                return {};
            auto loop = readLoop(wire, tolerance);
            if (!loop)
                return {};
            double area = signedArea(*loop);
            if (loop->radiusMm > 0) {
                const auto edge = BRepTools_WireExplorer(wire).Current();
                area = BRepAdaptor_Curve(edge).Circle().Axis().Direction().Z() *
                       (edge.Orientation() == TopAbs_REVERSED ? -1 : 1);
            }
            if (!std::isfinite(area) || std::abs(area) < 1.e-12)
                return {};
            if (area * normal > 0)
                wire.Reverse();
            check.Add(wire);
            result->loops.push_back(std::move(*loop));
        }
        if (!check.IsDone() || !BRepCheck_Analyzer(check.Face()).IsValid())
            return {};
        face = check.Face();
    }
    if (verifiedStock && result->blindOpenings == 0)
        *verifiedStock = face;
    return result;
}
} // namespace bending::cad
