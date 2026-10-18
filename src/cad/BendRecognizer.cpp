#include "BendRecognizer.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
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
    std::array<int, 2> caps;
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
};
// A monotone graph v(u) across the entire sector. Non-rational spline poles
// bound v by the convex-hull property; no sampled curve is treated as proof.
std::optional<EndBounds> inspectEnd(const TopoDS_Edge &edge, const TopoDS_Face &face,
                                    int adjacentFace, bool normalPlanarCap, double u0, double u1,
                                    double tolerance) {
    double first, last;
    const auto curve = BRep_Tool::CurveOnSurface(edge, face, first, last);
    if (curve.IsNull() || !std::isfinite(first) || !std::isfinite(last) || last <= first) {
        return {};
    }
    Geom2dAdaptor_Curve adaptor(curve, first, last);
    const auto a = adaptor.Value(first);
    const auto b = adaptor.Value(last);
    if (!std::isfinite(a.X()) || !std::isfinite(a.Y()) || !std::isfinite(b.X()) ||
        !std::isfinite(b.Y()) || std::abs(std::min(a.X(), b.X()) - u0) > angularTolerance ||
        std::abs(std::max(a.X(), b.X()) - u1) > angularTolerance) {
        return {};
    }
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
    return EndBounds{adjacentFace, minimumV, maximumV,
                     normalPlanarCap && maximumV - minimumV <= tolerance};
}

std::optional<Patch> inspectPatch(const TopoDS_Face &face, std::size_t index,
                                  const TopTools_IndexedMapOfShape &faces,
                                  const TopTools_IndexedDataMapOfShapeListOfShape &neighbors,
                                  double tolerance) {
    BRepAdaptor_Surface surface(face, true);
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
    if (edges.Extent() != 4) {
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
        BRepAdaptor_Surface otherSurface(TopoDS::Face(other), true);
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
            rails.push_back({radial, plane, static_cast<std::size_t>(faces.FindIndex(other))});
        } else {
            const bool normalPlanarCap =
                otherSurface.GetType() == GeomAbs_Plane &&
                parallel(otherSurface.Plane().Axis().Direction(), cylinder.Axis().Direction());
            const auto cap =
                inspectEnd(edge, face, faces.FindIndex(other), normalPlanarCap, u0, u1, tolerance);
            if (!cap) {
                return {};
            }
            caps.push_back(*cap);
        }
    }
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
    std::array<int, 2> capFaces{caps[0].face, caps[1].face};
    std::sort(capFaces.begin(), capFaces.end());
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
        std::abs(inner.angle - outer.angle) > angularTolerance ||
        std::abs(inner.length - outer.length) > tolerance || inner.caps != outer.caps ||
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
    const double straightLength =
        std::min(innerCore[1], outerCore[1]) - std::max(innerCore[0], outerCore[0]);
    if ((!sameEnds && !reversedEnds) || straightLength <= 20.0 * tolerance) {
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
    // Canonical axis direction independent of the surface parametrisation.
    auto start = inner.start;
    auto end = inner.end;
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
    return RecognizedBend{point(start),  point(end),
                          inner.angle,   inner.cylinder.Radius(),
                          thickness,     inner.length,
                          inner.face,    outer.face,
                          flanges,       inner.trimmedEnds || outer.trimmedEnds,
                          straightLength};
}
} // namespace

BendAnalysis recognizeBends(const TopoDS_Shape &shape, double maximumToleranceMm,
                            std::stop_token stop) {
    BendAnalysis result;
    result.lengthToleranceMm = std::max(1.0e-5, maximumToleranceMm * 10.0);
    if (stop.stop_requested()) {
        return result;
    }
    if (maximumToleranceMm > 0.001) {
        result.status = BendRecognitionStatus::Unsupported;
        return result;
    }
    try {
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        if (faces.Extent() > 512 || edges.Extent() > 2048) {
            result.status = BendRecognitionStatus::LimitReached;
            return result;
        }
        TopTools_IndexedDataMapOfShapeListOfShape neighbors;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, neighbors);
        std::vector<Patch> patches;
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (stop.stop_requested()) {
                return BendAnalysis{};
            }
            const auto &face = TopoDS::Face(faces(i));
            if (BRepAdaptor_Surface(face, true).GetType() == GeomAbs_Cylinder) {
                ++result.unrecognizedCylindricalFaces;
                if (const auto patch = inspectPatch(face, static_cast<std::size_t>(i), faces,
                                                    neighbors, result.lengthToleranceMm)) {
                    patches.push_back(*patch);
                }
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
            if (patches[i].outward || partners[i].size() != 1) {
                continue;
            }
            const auto j = partners[i].front();
            if (partners[j].size() == 1) {
                auto candidate = *match(patches[i], patches[j], result.lengthToleranceMm);
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
        result.status = BendRecognitionStatus::Completed;
    } catch (const Standard_Failure &) {
        result.bends.clear();
        result.interiorContourDiagnostics.clear();
        result.status = BendRecognitionStatus::Failed;
    }
    return result;
}
} // namespace bending::cad
