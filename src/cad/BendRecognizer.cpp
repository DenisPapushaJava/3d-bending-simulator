#include "BendRecognizer.h"
#include "ShapeVolumeDifference.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace bending::cad {
namespace {
using namespace application;
constexpr double angularTolerance = 1.0e-5;
constexpr double minimumAngle = std::numbers::pi / 180.0;
constexpr double maximumAngle = std::numbers::pi - minimumAngle;
struct Rail {
    gp_Dir radial;
    gp_Pln plane;
    std::size_t flange;
    TopoDS_Edge edge;
};
struct Patch {
    std::size_t face;
    gp_Cylinder cylinder;
    double angle;
    double length;
    gp_Pnt start;
    gp_Pnt end;
    gp_Dir middleRadial;
    bool outward;
    std::array<Rail, 2> rails;
    std::vector<int> caps;
    gp_Pnt coreStart;
    gp_Pnt coreEnd;
    bool trimmedEnds;
    std::vector<std::size_t> boundaryFaces;
};
PointMm point(const gp_Pnt &p) {
    return {p.X(), p.Y(), p.Z()};
}
bool parallel(const gp_Dir &a, const gp_Dir &b) {
    return a.IsParallel(b, angularTolerance);
}
gp_Dir radialAt(const gp_Cylinder &cylinder, const gp_Pnt &p) {
    const gp_Vec offset(cylinder.Location(), p);
    const gp_Vec axis(cylinder.Axis().Direction());
    return gp_Dir(offset - axis * offset.Dot(axis));
}

struct EndBounds {
    int face;
    double minimumV;
    double maximumV;
    bool straight;
    double minimumU;
    double maximumU;
    TopoDS_Vertex firstU;
    TopoDS_Vertex lastU;
};
// A monotone graph v(u) across the entire sector. Non-rational spline poles
// bound v by the convex-hull property; no sampled curve is treated as proof.
std::optional<EndBounds> inspectEnd(const TopoDS_Edge &edge, const TopoDS_Face &face,
                                    int adjacentFace, bool normalPlanarCap, double u0, double u1,
                                    double tolerance, bool partial = false) {
    double first, last;
    const auto curve = BRep_Tool::CurveOnSurface(edge, face, first, last);
    if (curve.IsNull() || !std::isfinite(first) || !std::isfinite(last) || last <= first) {
        return {};
    }
    Geom2dAdaptor_Curve adaptor(curve, first, last);
    const auto a = adaptor.Value(first);
    const auto b = adaptor.Value(last);
    if (!std::isfinite(a.X()) || !std::isfinite(a.Y()) || !std::isfinite(b.X()) ||
        !std::isfinite(b.Y())) {
        return {};
    }
    const double minimumU = std::min(a.X(), b.X());
    const double maximumU = std::max(a.X(), b.X());
    if (partial ? (minimumU < u0 - angularTolerance || maximumU > u1 + angularTolerance ||
                   maximumU - minimumU <= angularTolerance)
                : (std::abs(minimumU - u0) > angularTolerance ||
                   std::abs(maximumU - u1) > angularTolerance))
        return {};
    double minimumV = std::min(a.Y(), b.Y());
    double maximumV = std::max(a.Y(), b.Y());
    if (adaptor.GetType() == GeomAbs_BSplineCurve) {
        const auto source = adaptor.BSpline();
        if (source->IsRational() || source->IsPeriodic() || source->NbPoles() > 128) {
            return {};
        }
        const auto spline = Handle(Geom2d_BSplineCurve)::DownCast(source->Copy());
        spline->Segment(first, last);
        const double sign = b.X() > a.X() ? 1.0 : -1.0;
        for (int i = 1; i <= spline->NbPoles(); ++i) {
            const auto pole = spline->Pole(i);
            if (!std::isfinite(pole.X()) || !std::isfinite(pole.Y()) ||
                (i > 1 && sign * (pole.X() - spline->Pole(i - 1).X()) <= 0.0)) {
                return {};
            }
            minimumV = std::min(minimumV, pole.Y());
            maximumV = std::max(maximumV, pole.Y());
        }
    } else if (adaptor.GetType() != GeomAbs_Line) {
        return {};
    }
    auto firstU = TopExp::FirstVertex(edge);
    auto lastU = TopExp::LastVertex(edge);
    if (b.X() < a.X())
        std::swap(firstU, lastU);
    return EndBounds{
        adjacentFace, minimumV, maximumV, normalPlanarCap && maximumV - minimumV <= tolerance,
        minimumU,     maximumU, firstU,   lastU};
}

bool joinEnds(std::vector<EndBounds> &caps, double u0, double u1, double v0, double v1,
              double tolerance, double radius, const std::vector<Rail> &rails) {
    const auto onRail = [&](const TopoDS_Vertex &vertex, double mismatch) {
        if (caps.size() <= 2 || vertex.IsNull())
            return false;
        for (const auto &rail : rails) {
            const auto first = TopExp::FirstVertex(rail.edge);
            const auto last = TopExp::LastVertex(rail.edge);
            if (first.IsNull() || last.IsNull() || (!vertex.IsSame(first) && !vertex.IsSame(last)))
                continue;
            // UVBounds includes both ends of this rail. The uncertainty
            // at the opposite end can move the reported global extremum.
            const double local = std::max(BRep_Tool::Tolerance(first), BRep_Tool::Tolerance(last));
            if (mismatch * radius <= std::min(tolerance, local))
                return true;
        }
        return false;
    };
    std::array<std::vector<EndBounds>, 2> groups;
    for (const auto &piece : caps) {
        if (piece.maximumV < (v0 + v1) / 2)
            groups[0].push_back(piece);
        else if (piece.minimumV > (v0 + v1) / 2)
            groups[1].push_back(piece);
        else
            return false;
    }
    std::vector<EndBounds> joined;
    for (auto &pieces : groups) {
        if (pieces.empty())
            return false;
        std::sort(pieces.begin(), pieces.end(),
                  [](const auto &a, const auto &b) { return a.minimumU < b.minimumU; });
        auto end = pieces.front();
        double cursor = u0;
        TopoDS_Vertex previous;
        for (const auto &piece : pieces) {
            const double gap = std::abs(piece.minimumU - cursor);
            if (gap > angularTolerance) {
                // Independent STEP pcurves can disagree at the same BRep
                // vertex. Never bridge a gap between distinct vertices.
                if (previous.IsNull()
                        ? !onRail(piece.firstU, gap)
                        : (!previous.IsSame(piece.firstU) ||
                           gap * radius > std::min(tolerance, BRep_Tool::Tolerance(previous))))
                    return false;
            }
            cursor = piece.maximumU;
            previous = piece.lastU;
            end.minimumV = std::min(end.minimumV, piece.minimumV);
            end.maximumV = std::max(end.maximumV, piece.maximumV);
            end.straight = end.straight && piece.straight;
        }
        const double tail = std::abs(cursor - u1);
        if (tail > angularTolerance && !onRail(previous, tail))
            return false;
        end.straight = end.straight && end.maximumV - end.minimumV <= tolerance;
        joined.push_back(end);
    }
    for (const auto &low : groups[0])
        for (const auto &high : groups[1])
            if (low.face == high.face)
                return false;
    caps = std::move(joined);
    return true;
}

std::optional<Patch> inspectPatch(const TopoDS_Face &face, std::size_t index,
                                  const TopTools_IndexedMapOfShape &faces,
                                  const TopTools_IndexedDataMapOfShapeListOfShape &neighbors,
                                  double tolerance) {
    BRepAdaptor_Surface surface(face, false);
    const auto cylinder = surface.Cylinder();
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const double angle = u1 - u0;
    const double length = v1 - v0;
    if (!std::isfinite(cylinder.Radius()) || !std::isfinite(angle) || !std::isfinite(length) ||
        angle < minimumAngle || angle > maximumAngle || length <= 20.0 * tolerance ||
        cylinder.Radius() <= 20.0 * tolerance) {
        return {};
    }
    TopTools_IndexedMapOfShape edges;
    const auto outerWire = BRepTools::OuterWire(face);
    if (outerWire.IsNull()) {
        return {};
    }
    TopExp::MapShapes(outerWire, TopAbs_EDGE, edges);
    if (edges.Extent() < 4 || edges.Extent() > 12) {
        return {};
    }
    std::vector<std::size_t> boundaryFaces;
    for (TopExp_Explorer wire(face, TopAbs_WIRE); wire.More(); wire.Next()) {
        if (wire.Current().IsSame(outerWire)) {
            continue;
        }
        for (TopExp_Explorer boundary(wire.Current(), TopAbs_EDGE); boundary.More();
             boundary.Next()) {
            if (!neighbors.Contains(boundary.Current())) {
                return {};
            }
            const auto &adjacent = neighbors.FindFromKey(boundary.Current());
            if (adjacent.Extent() != 2) {
                return {};
            }
            const auto &other = adjacent.First().IsSame(face) ? adjacent.Last() : adjacent.First();
            const int otherIndex = faces.FindIndex(other);
            if (otherIndex == 0 || other.IsSame(face)) {
                return {};
            }
            boundaryFaces.push_back(static_cast<std::size_t>(otherIndex));
        }
    }
    std::sort(boundaryFaces.begin(), boundaryFaces.end());
    boundaryFaces.erase(std::unique(boundaryFaces.begin(), boundaryFaces.end()),
                        boundaryFaces.end());
    std::vector<Rail> rails;
    std::vector<EndBounds> caps;
    for (int i = 1; i <= edges.Extent(); ++i) {
        const auto &edge = TopoDS::Edge(edges(i));
        if (!neighbors.Contains(edge)) {
            return {};
        }
        const auto &adjacent = neighbors.FindFromKey(edge);
        if (adjacent.Extent() != 2) {
            return {};
        }
        const auto &other = adjacent.First().IsSame(face) ? adjacent.Last() : adjacent.First();
        BRepAdaptor_Surface otherSurface(TopoDS::Face(other), false);
        BRepAdaptor_Curve curve(edge);
        if (curve.GetType() == GeomAbs_Line &&
            parallel(curve.Line().Direction(), cylinder.Axis().Direction())) {
            if (otherSurface.GetType() != GeomAbs_Plane) {
                return {};
            }
            const auto plane = otherSurface.Plane();
            const auto p = curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
            const auto radial = radialAt(cylinder, p);
            // A cylindrical hole or an arbitrary round corner is not enough: both
            // generators must join tangent planar flanges.
            if (!parallel(radial, plane.Axis().Direction()) ||
                std::abs(plane.Distance(cylinder.Location()) - cylinder.Radius()) > tolerance) {
                return {};
            }
            rails.push_back(
                {radial, plane, static_cast<std::size_t>(faces.FindIndex(other)), edge});
        } else {
            const bool normalPlanarCap =
                otherSurface.GetType() == GeomAbs_Plane &&
                parallel(otherSurface.Plane().Axis().Direction(), cylinder.Axis().Direction());
            const auto cap = inspectEnd(edge, face, faces.FindIndex(other), normalPlanarCap, u0, u1,
                                        tolerance, true);
            if (!cap) {
                return {};
            }
            caps.push_back(*cap);
        }
    }
    std::vector<int> capFaces;
    for (const auto &cap : caps)
        capFaces.push_back(cap.face);
    std::sort(capFaces.begin(), capFaces.end());
    capFaces.erase(std::unique(capFaces.begin(), capFaces.end()), capFaces.end());
    if (!joinEnds(caps, u0, u1, v0, v1, tolerance, cylinder.Radius(), rails))
        return {};
    if (rails.size() != 2 || caps.size() != 2 || rails[0].flange == rails[1].flange ||
        caps[0].face == caps[1].face) {
        return {};
    }
    std::sort(caps.begin(), caps.end(),
              [](const auto &a, const auto &b) { return a.minimumV < b.minimumV; });
    const double coreMinimum = caps[0].maximumV;
    const double coreMaximum = caps[1].minimumV;
    if (coreMaximum - coreMinimum <= 20.0 * tolerance) {
        return {};
    }
    const bool trimmed = !caps[0].straight || !caps[1].straight;
    GProp_GProps area;
    BRepGProp::SurfaceProperties(face, area);
    const double expectedArea = cylinder.Radius() * angle * length;
    const double areaTolerance = tolerance * (length + cylinder.Radius() * angle) * 4.0;
    const double coreArea = cylinder.Radius() * angle * (coreMaximum - coreMinimum);
    if (!std::isfinite(area.Mass()) || area.Mass() <= tolerance * tolerance ||
        area.Mass() > expectedArea + areaTolerance ||
        (boundaryFaces.empty() &&
         (area.Mass() < coreArea - areaTolerance ||
          (!trimmed && std::abs(area.Mass() - expectedArea) > areaTolerance)))) {
        return {};
    }
    const auto middle = surface.Value((u0 + u1) * 0.5, (v0 + v1) * 0.5);
    gp_Pnt p;
    gp_Vec du, dv;
    surface.D1((u0 + u1) * 0.5, (v0 + v1) * 0.5, p, du, dv);
    auto normal = du.Crossed(dv);
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    const gp_Vec axis(cylinder.Axis().Direction());
    return Patch{index,
                 cylinder,
                 angle,
                 length,
                 cylinder.Location().Translated(axis * v0),
                 cylinder.Location().Translated(axis * v1),
                 radialAt(cylinder, middle),
                 normal.Dot(gp_Vec(radialAt(cylinder, middle))) > 0.0,
                 {rails[0], rails[1]},
                 capFaces,
                 cylinder.Location().Translated(axis * coreMinimum),
                 cylinder.Location().Translated(axis * coreMaximum),
                 trimmed,
                 std::move(boundaryFaces)};
}

std::optional<RecognizedBend> match(const Patch &inner, const Patch &outer, double tolerance) {
    const double thickness = outer.cylinder.Radius() - inner.cylinder.Radius();
    if (inner.outward || !outer.outward || thickness <= 20.0 * tolerance ||
        !parallel(inner.cylinder.Axis().Direction(), outer.cylinder.Axis().Direction()) ||
        gp_Lin(inner.cylinder.Axis()).Distance(outer.cylinder.Location()) > tolerance ||
        inner.caps != outer.caps ||
        !inner.middleRadial.IsEqual(outer.middleRadial, angularTolerance)) {
        return {};
    }
    const bool sameEnds = inner.start.Distance(outer.start) <= tolerance &&
                          inner.end.Distance(outer.end) <= tolerance;
    const bool reversedEnds = inner.start.Distance(outer.end) <= tolerance &&
                              inner.end.Distance(outer.start) <= tolerance;
    const gp_Vec axis(inner.cylinder.Axis().Direction());
    const auto coreInterval = [&](const Patch &patch) {
        const double a = gp_Vec(inner.cylinder.Location(), patch.coreStart).Dot(axis);
        const double b = gp_Vec(inner.cylinder.Location(), patch.coreEnd).Dot(axis);
        return std::array<double, 2>{std::min(a, b), std::max(a, b)};
    };
    const auto innerCore = coreInterval(inner);
    const auto outerCore = coreInterval(outer);
    const double coreFrom = std::max(innerCore[0], outerCore[0]);
    const double coreTo = std::min(innerCore[1], outerCore[1]);
    const double straightLength = coreTo - coreFrom;
    const bool sameSpan =
        (sameEnds || reversedEnds) && std::abs(inner.length - outer.length) <= tolerance;
    if ((!sameSpan && !inner.trimmedEnds && !outer.trimmedEnds) ||
        straightLength <= 20.0 * tolerance) {
        return {};
    }
    std::array<std::size_t, 4> flanges{};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto &a = inner.rails[i];
        const auto b = std::find_if(outer.rails.begin(), outer.rails.end(), [&](const Rail &rail) {
            return a.radial.IsEqual(rail.radial, angularTolerance);
        });
        if (b == outer.rails.end() ||
            !parallel(a.plane.Axis().Direction(), b->plane.Axis().Direction()) ||
            std::abs(a.plane.Distance(b->plane.Location()) - thickness) > tolerance ||
            a.flange == b->flange) {
            return {};
        }
        flanges[i * 2] = a.flange;
        flanges[i * 2 + 1] = b->flange;
    }
    double angle = inner.angle;
    if (std::abs(inner.angle - outer.angle) > angularTolerance) {
        // In a compound end the UV extrema include the uncertainty of the
        // trim. The matched tangent rails determine the actual rotation.
        if (!inner.trimmedEnds || !outer.trimmedEnds || inner.caps.size() <= 2)
            return {};
        angle = inner.rails[0].radial.Angle(inner.rails[1].radial);
        if (angle < minimumAngle || angle > maximumAngle ||
            std::abs(inner.angle - angle) * inner.cylinder.Radius() > tolerance ||
            std::abs(outer.angle - angle) * outer.cylinder.Radius() > tolerance)
            return {};
    }
    // Canonical axis direction independent of the surface parametrisation.
    auto start = inner.start;
    auto end = inner.end;
    if (!sameSpan) {
        // A shared oblique cap can reach different axial coordinates at the
        // two radii. Keep the full occupied span, not just the inner skin.
        const double outerA = gp_Vec(inner.start, outer.start).Dot(axis);
        const double outerB = gp_Vec(inner.start, outer.end).Dot(axis);
        start = inner.start.Translated(axis * std::min({0., outerA, outerB}));
        end = inner.start.Translated(axis * std::max({inner.length, outerA, outerB}));
    }
    const double fullLength = sameSpan ? inner.length : start.Distance(end);
    const gp_Vec direction(start, end);
    const std::array<double, 3> components{direction.X(), direction.Y(), direction.Z()};
    for (const auto value : components) {
        if (std::abs(value) > tolerance) {
            if (value < 0.0) {
                std::swap(start, end);
            }
            break;
        }
    }
    const double startV = gp_Vec(inner.cylinder.Location(), start).Dot(axis);
    auto straight = direction.Dot(gp_Vec(start, end)) > 0
                        ? std::array{coreFrom - startV, coreTo - startV}
                        : std::array{startV - coreTo, startV - coreFrom};
    for (auto &limit : straight)
        limit = std::clamp(limit, 0., fullLength);
    return RecognizedBend{point(start),   point(end),
                          angle,          inner.cylinder.Radius(),
                          thickness,      fullLength,
                          inner.face,     outer.face,
                          flanges,        inner.trimmedEnds || outer.trimmedEnds,
                          straightLength, straight};
}

bool preferWorkpieceReversed(const RecognizedBend &bend, const Patch &inner,
                             const TopTools_IndexedMapOfShape &faces, double tolerance) {
    try {
        const gp_Pnt start(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z);
        const gp_Pnt end(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z);
        const auto axis = gp_Vec(start, end).Normalized();
        std::array<double, 2> reach{};
        for (std::size_t i = 0; i < inner.rails.size(); ++i) {
            const auto &rail = inner.rails[i];
            // In the straight setup, the inward normal becomes +Y, the
            // canonical axis +Z, and Y cross Z points toward the gauge (+X).
            const auto across = axis.Crossed(gp_Vec(rail.radial)).Normalized();
            const auto &face = TopoDS::Face(faces(static_cast<int>(rail.flange)));
            const auto boundary = BRepTools::OuterWire(face);
            double low = 0, high = 0;
            std::size_t vertices = 0;
            for (TopExp_Explorer v(boundary, TopAbs_VERTEX); v.More(); v.Next()) {
                if (++vertices > 1024)
                    return false;
                const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
                const double x = gp_Vec(start, p).Dot(across);
                low = std::min(low, x);
                high = std::max(high, x);
            }
            if (low < -tolerance && high > tolerance)
                return false;
            reach[i] = high > tolerance ? high : low;
        }
        if (!((reach[0] < -tolerance && reach[1] > tolerance) ||
              (reach[1] < -tolerance && reach[0] > tolerance)))
            return false;
        return reach[0] + reach[1] > std::max(.5, bend.thicknessMm);
    } catch (const Standard_Failure &) {
        return false; // An optional setup hint cannot fail recognition.
    }
}

bool isThroughCutPlate(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                       const BendAnalysis &analysis, std::stop_token stop) {
    if (analysis.bends.empty() || !analysis.interiorContourDiagnostics.empty())
        return false;
    const auto &first = analysis.bends.front();
    const gp_Pnt start(first.axisStart.x, first.axisStart.y, first.axisStart.z);
    const gp_Pnt end(first.axisEnd.x, first.axisEnd.y, first.axisEnd.z);
    const gp_Dir normal(gp_Vec(start, end));
    const double thickness = start.Distance(end);
    const double tolerance = std::clamp(analysis.lengthToleranceMm, 1.e-6, 1.e-4);
    const auto height = [&](const gp_Pnt &p) { return gp_Vec(start, p).Dot(gp_Vec(normal)); };
    for (const auto &bend : analysis.bends) {
        const gp_Pnt a(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z);
        const gp_Pnt b(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z);
        const auto [low, high] = std::minmax({height(a), height(b)});
        if (!parallel(gp_Dir(gp_Vec(a, b)), normal) || std::abs(low) > tolerance ||
            std::abs(high - thickness) > tolerance)
            return false;
    }
    TopoDS_Face skin;
    double largest = 0;
    for (int i = 1; i <= faces.Extent(); ++i) {
        if (stop.stop_requested())
            return false;
        const auto face = TopoDS::Face(faces(i));
        const BRepAdaptor_Surface surface(face, false);
        if (surface.GetType() != GeomAbs_Plane)
            continue;
        GProp_GProps area;
        BRepGProp::SurfaceProperties(face, area);
        if (area.Mass() <= largest)
            continue;
        largest = area.Mass();
        const auto plane = surface.Plane();
        const auto z = height(plane.Location());
        skin = parallel(plane.Axis().Direction(), normal) &&
                       std::min(std::abs(z), std::abs(z - thickness)) <= tolerance
                   ? face
                   : TopoDS_Face{};
    }
    if (skin.IsNull())
        return false;

    // The end of a formed profile is also an extruded contour. Require a
    // broad plate skin, using the same aspect criteria as thickness recognition.
    const auto plane = BRepAdaptor_Surface(skin, false).Plane();
    gp_Trsf local;
    local.SetTransformation(plane.Position());
    Bnd_Box box;
    BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(skin, local, false).Shape(), box, false, false);
    if (box.IsVoid() || box.IsOpen())
        return false;
    const auto span = gp_Vec(box.CornerMin(), box.CornerMax());
    if (std::min(span.X(), span.Y()) < 5 * thickness ||
        largest < std::hypot(span.X(), span.Y()) * 5 * thickness)
        return false;

    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    if (solids.Extent() != 1 || stop.stop_requested())
        return false;
    const double z = height(plane.Location());
    const auto prism =
        BRepPrimAPI_MakePrism(skin, gp_Vec(normal) * (z < thickness / 2 ? thickness : -thickness),
                              true)
            .Shape();
    return BRepCheck_Analyzer(prism).IsValid() &&
           sameVolumeWithin(shape, prism, tolerance, 1.e-9, stop);
}
} // namespace

BendAnalysis recognizeBends(const TopoDS_Shape &shape, double maximumToleranceMm,
                            std::stop_token stop, BendRecognitionPurpose purpose) {
    BendAnalysis result;
    // STEP end curves may need several microns of BRep tolerance even when the
    // analytic cylinders and flanges agree precisely. Keep their original
    // tolerances, but never let them inflate simulation comparisons past the
    // former 0.001 mm input / 0.01 mm comparison limit.
    const double comparisonInput = purpose == BendRecognitionPurpose::Simulation
                                       ? std::min(maximumToleranceMm, 0.001)
                                       : maximumToleranceMm;
    result.lengthToleranceMm =
        std::max(1.0e-5, comparisonInput *
                             (purpose == BendRecognitionPurpose::DrawingMeasurements ? 2.0 : 10.0));
    if (stop.stop_requested()) {
        return result;
    }
    if (!std::isfinite(maximumToleranceMm) || maximumToleranceMm < 0 || maximumToleranceMm > 0.01) {
        result.status = BendRecognitionStatus::Unsupported;
        return result;
    }
    try {
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        const bool large = faces.Extent() > 512 || edges.Extent() > 2048;
        if (faces.Extent() > 4096 || edges.Extent() > 16384) {
            result.status = BendRecognitionStatus::LimitReached;
            return result;
        }
        std::vector<int> candidates;
        constexpr std::size_t maximumLargeCandidates = 128;
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (stop.stop_requested())
                return {};
            const auto &face = TopoDS::Face(faces(i));
            if (BRepAdaptor_Surface(face, false).GetType() != GeomAbs_Cylinder)
                continue;
            ++result.unrecognizedCylindricalFaces;
            if (large) {
                double u0, u1, v0, v1;
                BRepTools::UVBounds(face, u0, u1, v0, v1);
                // Whole cylindrical hole walls cannot form an open bend sector.
                const double angle = u1 - u0;
                if (!std::isfinite(angle) || angle < minimumAngle || angle > maximumAngle)
                    continue;
                if (candidates.size() == maximumLargeCandidates) {
                    result.status = BendRecognitionStatus::LimitReached;
                    return result;
                }
            }
            candidates.push_back(i);
        }
        if (large && candidates.empty()) {
            result.status = BendRecognitionStatus::LimitReached;
            return result;
        }
        TopTools_IndexedDataMapOfShapeListOfShape neighbors;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, neighbors);
        std::vector<Patch> patches;
        for (const int i : candidates) {
            if (stop.stop_requested()) {
                return BendAnalysis{};
            }
            const auto &face = TopoDS::Face(faces(i));
            if (const auto patch = inspectPatch(face, static_cast<std::size_t>(i), faces, neighbors,
                                                result.lengthToleranceMm)) {
                patches.push_back(*patch);
            }
        }
        // Do not silently choose a partner if a patch has several matching surfaces.
        std::vector<std::vector<std::size_t>> partners(patches.size());
        for (std::size_t i = 0; i < patches.size(); ++i) {
            if (stop.stop_requested()) {
                return BendAnalysis{};
            }
            for (std::size_t j = 0; j < patches.size(); ++j) {
                if (match(patches[i], patches[j], result.lengthToleranceMm)) {
                    partners[i].push_back(j);
                    partners[j].push_back(i);
                }
            }
        }
        for (std::size_t i = 0; i < patches.size(); ++i) {
            if (stop.stop_requested())
                return {};
            if (patches[i].outward || partners[i].size() != 1) {
                continue;
            }
            const auto j = partners[i].front();
            if (partners[j].size() == 1) {
                auto candidate = *match(patches[i], patches[j], result.lengthToleranceMm);
                if (purpose == BendRecognitionPurpose::Simulation)
                    candidate.preferWorkpieceReversed = preferWorkpieceReversed(
                        candidate, patches[i], faces, result.lengthToleranceMm);
                auto boundaries = patches[i].boundaryFaces;
                boundaries.insert(boundaries.end(), patches[j].boundaryFaces.begin(),
                                  patches[j].boundaryFaces.end());
                if (boundaries.empty()) {
                    result.bends.push_back(std::move(candidate));
                } else {
                    std::sort(boundaries.begin(), boundaries.end());
                    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                                     boundaries.end());
                    result.interiorContourDiagnostics.push_back(
                        {std::move(candidate), std::move(boundaries)});
                }
                result.unrecognizedCylindricalFaces -= 2;
            }
        }
        try {
            if (isThroughCutPlate(shape, faces, result, stop)) {
                result.unrecognizedCylindricalFaces += 2 * result.bends.size();
                result.bends.clear();
            }
        } catch (const Standard_Failure &) {
            // An inconclusive stock check leaves the existing bend analysis intact.
        }
        if (stop.stop_requested())
            return {};
        result.status = BendRecognitionStatus::Completed;
    } catch (const Standard_Failure &) {
        result.bends.clear();
        result.interiorContourDiagnostics.clear();
        result.status = BendRecognitionStatus::Failed;
    }
    return result;
}
} // namespace bending::cad
