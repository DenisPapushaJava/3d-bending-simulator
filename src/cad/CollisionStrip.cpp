#include "CollisionStrip.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Ax3.hxx>
#include <span>

namespace bending::cad {
namespace {
using Polygon = std::vector<gp_Pnt>;
constexpr double tolerance = 1e-7;
double coordinate(const gp_Pnt &p, const gp_Pnt &origin, const gp_Vec &axis) {
    return gp_Vec(origin, p).Dot(axis);
}
Polygon clip(const Polygon &polygon, const gp_Pnt &origin, const gp_Vec &axis, double bound,
             bool greater) {
    Polygon out;
    if (polygon.empty())
        return out;
    auto previous = polygon.back();
    auto previousValue = coordinate(previous, origin, axis) - bound;
    for (const auto &p : polygon) {
        const auto value = coordinate(p, origin, axis) - bound;
        const bool inside = greater ? value >= 0 : value <= 0;
        const bool previousInside = greater ? previousValue >= 0 : previousValue <= 0;
        if (inside != previousInside) {
            const double fraction = std::clamp(previousValue / (previousValue - value), 0., 1.);
            out.push_back(previous.Translated(gp_Vec(previous, p) * fraction));
        }
        if (inside)
            out.push_back(p);
        previous = p;
        previousValue = value;
    }
    return out;
}
std::array<double, 3> rank(const std::array<gp_Pnt, 2> &line) {
    const auto direction = gp_Vec(line[0], line[1]).Normalized();
    auto foot = line[0].XYZ() - direction.XYZ() * line[0].XYZ().Dot(direction.XYZ());
    // Ranking only: never snap the actual line or bridge a geometric gap.
    return {std::round(foot.X() / tolerance), std::round(foot.Y() / tolerance),
            std::round(foot.Z() / tolerance)};
}
float weight(double distance, double radius) {
    const auto t = std::clamp((1 - distance / radius) / .8, 0., 1.);
    return static_cast<float>(t * t * (3 - 2 * t));
}
} // namespace
std::optional<std::array<gp_Pnt, 2>> collisionStrip(const TopoDS_Shape &region, double radius,
                                                    std::stop_token stop) try {
    std::optional<std::array<gp_Pnt, 2>> longest;
    double length = 2 * radius;
    for (TopExp_Explorer it(region, TopAbs_EDGE); it.More(); it.Next()) {
        if (stop.stop_requested())
            return {};
        const auto edge = TopoDS::Edge(it.Current());
        if (BRep_Tool::Degenerated(edge))
            continue;
        BRepAdaptor_Curve curve(edge);
        if (curve.GetType() != GeomAbs_Line || !std::isfinite(curve.FirstParameter()) ||
            !std::isfinite(curve.LastParameter()))
            continue;
        const std::array<gp_Pnt, 2> line{curve.Value(curve.FirstParameter()),
                                         curve.Value(curve.LastParameter())};
        const auto nextLength = line[0].Distance(line[1]);
        if (nextLength > length + tolerance ||
            (longest && std::abs(nextLength - length) <= tolerance &&
             rank(line) < rank(*longest))) {
            length = nextLength;
            longest = line;
        }
    }
    if (!longest)
        return {};
    const auto axis = gp_Vec((*longest)[0], (*longest)[1]).Normalized();
    // A geometry-derived transverse basis is stable under rigid placements.
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(region, TopAbs_VERTEX, vertices);
    gp_Vec transverse;
    for (int i = 1; i <= vertices.Extent(); ++i) {
        if (stop.stop_requested())
            return {};
        const auto v = gp_Vec((*longest)[0], BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))));
        const auto perpendicular = v - axis * v.Dot(axis);
        if (perpendicular.SquareMagnitude() > transverse.SquareMagnitude())
            transverse = perpendicular;
    }
    if (transverse.SquareMagnitude() < tolerance * tolerance)
        return {};
    gp_Trsf local;
    local.SetTransformation(gp_Ax3((*longest)[0], gp_Dir(axis), gp_Dir(transverse)));
    Bnd_Box box;
    // Placement only; no Boolean, mesh generation or mutation of the overlap.
    BRepBndLib::AddOptimal(region.Moved(TopLoc_Location(local)), box, false, true);
    if (box.IsVoid() || box.IsOpen())
        return {};
    const auto lo = box.CornerMin(), hi = box.CornerMax();
    const auto width = std::hypot(std::max(std::abs(lo.X()), std::abs(hi.X())),
                                  std::max(std::abs(lo.Y()), std::abs(hi.Y())));
    if (width > .75 * radius || lo.Z() < -1e-5 || hi.Z() > length + 1e-5)
        return {};
    return longest;
} catch (const Standard_Failure &) {
    // Optional presentation optimization: an unsupported curve never blocks the
    // established diagnostic of an already confirmed collision.
    return {};
}
std::vector<gp_Pnt> clipCollisionStripRange(const std::array<gp_Pnt, 3> &triangle,
                                            const std::array<gp_Pnt, 2> &band) {
    const auto axis = gp_Vec(band[0], band[1]).Normalized();
    auto polygon = clip(Polygon(triangle.begin(), triangle.end()), band[0], axis, 0, true);
    return clip(polygon, band[0], axis, band[0].Distance(band[1]), false);
}
bool appendCollisionStrip(CollisionHighlightPatch &patch, const std::array<gp_Pnt, 3> &triangle,
                          std::size_t vertexLimit) {
    if (patch.vertices.size() >= vertexLimit)
        return true;
    const auto &band = *patch.band;
    const auto axis = gp_Vec(band[0], band[1]).Normalized();
    auto normal = gp_Vec(triangle[0], triangle[1]).Crossed(gp_Vec(triangle[0], triangle[2]));
    if (normal.SquareMagnitude() < 1e-24)
        return true;
    normal.Normalize();
    if (std::abs(normal.Dot(axis)) > 1e-8)
        return false;
    const auto height = gp_Vec(triangle[0], band[0]).Dot(normal);
    if (std::abs(height) >= patch.radiusMm)
        return true;
    const auto across = normal.Crossed(axis).Normalized();
    const double halfWidth = std::sqrt(patch.radiusMm * patch.radiusMm - height * height);
    const auto axial = clipCollisionStripRange(triangle, band);
    // Piecewise-linear smooth falloff across the strip, with no longitudinal subdivision.
    const auto remaining = (vertexLimit - patch.vertices.size()) / 3;
    constexpr std::array detailed{-1., -.6, -.2, 0., .2, .6, 1.};
    constexpr std::array medium{-1., -.2, 0., .2, 1.};
    constexpr std::array coarse{-1., 0., 1.};
    const auto levels = remaining >= 32   ? std::span<const double>(detailed)
                        : remaining >= 16 ? std::span<const double>(medium)
                                          : std::span<const double>(coarse);
    for (std::size_t i = 1; i < levels.size(); ++i) {
        auto polygon = clip(axial, band[0], across, levels[i - 1] * halfWidth, true);
        polygon = clip(polygon, band[0], across, levels[i] * halfWidth, false);
        for (std::size_t v = 1; v + 1 < polygon.size(); ++v) {
            if (patch.vertices.size() + 3 > vertexLimit)
                return true;
            const std::array<gp_Pnt, 3> points{polygon[0], polygon[v], polygon[v + 1]};
            if (gp_Vec(points[0], points[1])
                    .Crossed(gp_Vec(points[0], points[2]))
                    .SquareMagnitude() < 1e-20)
                continue;
            std::array<float, 3> weights;
            for (std::size_t j = 0; j < points.size(); ++j)
                weights[j] = weight(std::hypot(height, coordinate(points[j], band[0], across)),
                                    patch.radiusMm);
            if (*std::max_element(weights.begin(), weights.end()) == 0)
                continue;
            for (std::size_t j = 0; j < points.size(); ++j)
                patch.vertices.push_back({points[j], weights[j]});
        }
    }
    return true;
}
} // namespace bending::cad
