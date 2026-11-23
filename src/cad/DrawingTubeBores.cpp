#include "DrawingTubeBores.h"
#include "DrawingTubeProfile.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <numbers>
#include <utility>

namespace bending::cad {
namespace {
using EdgeFaces = TopTools_IndexedDataMapOfShapeListOfShape;
constexpr double tolerance = 1e-5;
constexpr int maximumRingEdges = 64;

struct BoreAxis {
    gp_Pnt entry;
    gp_Dir inward;

    double depth(const gp_Pnt &point) const {
        return gp_Vec(entry, point).Dot(gp_Vec(inward));
    }
    bool coaxial(const gp_Ax1 &axis) const {
        return axis.Direction().IsParallel(inward, 1e-7) &&
               gp_Vec(entry, axis.Location()).Crossed(gp_Vec(inward)).Magnitude() < tolerance;
    }
};
struct Ring {
    TopTools_IndexedMapOfShape edges;
    double depth = 0;
    double radius = 0;
};
struct BoreBand {
    TopTools_IndexedMapOfShape faces;
    Ring exit;
    std::optional<double> semiAngle;
};

std::optional<Ring> circularRing(TopTools_IndexedMapOfShape edges, const BoreAxis &axis,
                                 std::stop_token stop) {
    if (edges.IsEmpty() || edges.Extent() > maximumRingEdges)
        return {};
    Ring ring{std::move(edges)};
    double angle = 0;
    for (int i = 1; i <= ring.edges.Extent(); ++i) {
        if (stop.stop_requested())
            throw application::DrawingError{"Cancelled", true};
        const BRepAdaptor_Curve curve(TopoDS::Edge(ring.edges(i)));
        if (curve.GetType() != GeomAbs_Circle)
            return {};
        const auto circle = curve.Circle();
        const double depth = axis.depth(circle.Location());
        if (!axis.coaxial(circle.Axis()))
            return {};
        if (i == 1) {
            ring.depth = depth;
            ring.radius = circle.Radius();
        } else if (std::abs(depth - ring.depth) >= tolerance ||
                   std::abs(circle.Radius() - ring.radius) >= tolerance) {
            return {};
        }
        angle += curve.LastParameter() - curve.FirstParameter();
    }
    if (std::abs(angle - 2 * std::numbers::pi) > 1e-6)
        return {};
    return ring;
}

std::optional<TopTools_IndexedMapOfShape>
facesAcross(const Ring &ring, const TopTools_IndexedMapOfShape &from, const EdgeFaces &neighbors) {
    TopTools_IndexedMapOfShape result;
    for (int i = 1; i <= ring.edges.Extent(); ++i) {
        const auto &edge = ring.edges(i);
        if (!neighbors.Contains(edge))
            return {};
        const auto &adjacent = neighbors.FindFromKey(edge);
        if (adjacent.Extent() != 2 ||
            from.Contains(adjacent.First()) == from.Contains(adjacent.Last()))
            return {};
        result.Add(from.Contains(adjacent.First()) ? adjacent.Last() : adjacent.First());
    }
    return result;
}

std::optional<BoreBand> boreBand(const Ring &near, const TopTools_IndexedMapOfShape &from,
                                 const BoreAxis &axis, const EdgeFaces &neighbors,
                                 std::stop_token stop) {
    auto faces = facesAcross(near, from, neighbors);
    if (!faces)
        return {};
    std::optional<double> semiAngle;
    TopTools_IndexedMapOfShape farEdges;
    for (int i = 1; i <= faces->Extent(); ++i) {
        if (stop.stop_requested())
            throw application::DrawingError{"Cancelled", true};
        const auto face = TopoDS::Face((*faces)(i));
        BRepAdaptor_Surface surface(face, false);
        double angle = 0;
        if (surface.GetType() == GeomAbs_Cylinder) {
            const auto cylinder = surface.Cylinder();
            if (!axis.coaxial(cylinder.Axis()) ||
                std::abs(cylinder.Radius() - near.radius) >= tolerance)
                return {};
        } else if (surface.GetType() == GeomAbs_Cone) {
            const auto cone = surface.Cone();
            if (!axis.coaxial(cone.Axis()))
                return {};
            angle = std::abs(cone.SemiAngle());
            if (angle < 1e-7)
                return {};
        } else {
            return {};
        }
        if (semiAngle && std::abs(*semiAngle - angle) >= 1e-7)
            return {};
        semiAngle = angle;
        for (TopExp_Explorer explorer(face, TopAbs_EDGE); explorer.More(); explorer.Next()) {
            if (stop.stop_requested())
                throw application::DrawingError{"Cancelled", true};
            const auto &edge = TopoDS::Edge(explorer.Current());
            if (near.edges.Contains(edge) || BRep_Tool::IsClosed(edge, face))
                continue;
            if (!neighbors.Contains(edge))
                return {};
            const auto &adjacent = neighbors.FindFromKey(edge);
            if (adjacent.Extent() != 2)
                return {};
            if (faces->Contains(adjacent.First()) && faces->Contains(adjacent.Last()))
                continue;
            farEdges.Add(edge);
            if (farEdges.Extent() > maximumRingEdges)
                return {};
        }
    }
    auto far = circularRing(std::move(farEdges), axis, stop);
    if (!far || !semiAngle || far->depth - near.depth <= tolerance ||
        std::abs(near.radius - far->radius - (far->depth - near.depth) * std::tan(*semiAngle)) >=
            tolerance)
        return {};
    return BoreBand{std::move(*faces), std::move(*far), *semiAngle > 0 ? semiAngle : std::nullopt};
}

bool closedFlatBottom(const TopoDS_Face &face, const Ring &rim, const BoreAxis &axis) {
    const BRepAdaptor_Surface surface(face, false);
    if (surface.GetType() != GeomAbs_Plane ||
        !surface.Plane().Axis().Direction().IsParallel(axis.inward, 1e-7))
        return false;
    TopTools_IndexedMapOfShape bound;
    TopExp::MapShapes(face, TopAbs_EDGE, bound);
    if (bound.Extent() != rim.edges.Extent())
        return false;
    for (int i = 1; i <= bound.Extent(); ++i)
        if (!rim.edges.Contains(bound(i)))
            return false;
    return true;
}
} // namespace

std::optional<DrawingTubeBore> drawingTubeBore(const DrawingOpening &opening,
                                               const DrawingTubeWall &wall,
                                               const TopTools_IndexedMapOfShape &faces,
                                               const EdgeFaces &neighbors, std::stop_token stop) {
    try {
        const BoreAxis axis{opening.center, wall.outward.Reversed()};
        TopTools_IndexedMapOfShape entryEdges, support;
        TopExp::MapShapes(opening.wire, TopAbs_EDGE, entryEdges);
        const auto entry = circularRing(std::move(entryEdges), axis, stop);
        if (!entry || std::abs(entry->depth) >= tolerance)
            return {};
        support.Add(faces(wall.outerFace));
        auto bore = boreBand(*entry, support, axis, neighbors, stop);
        if (!bore)
            return {};
        DrawingTubeBore result{2 * entry->radius, {}};
        if (bore->semiAngle) {
            const auto angle = *bore->semiAngle;
            auto cylinder = boreBand(bore->exit, bore->faces, axis, neighbors, stop);
            if (!cylinder || cylinder->semiAngle)
                return {};
            result.diameterMm = 2 * bore->exit.radius;
            result.shape.entrance = application::DrawingConicalEntrance{
                2 * entry->radius, 2 * angle * 180 / std::numbers::pi,
                bore->exit.depth - entry->depth};
            bore = std::move(cylinder);
        }
        const auto terminal = facesAcross(bore->exit, bore->faces, neighbors);
        if (terminal && terminal->Extent() == 1) {
            if ((*terminal)(1).IsSame(faces(wall.innerFace))) {
                result.shape.throughWall = true;
            } else {
                const BRepAdaptor_Surface inner(TopoDS::Face(faces(wall.innerFace)), false);
                const double wallDepth = axis.depth(inner.Plane().Location());
                if (bore->exit.depth < wallDepth - tolerance &&
                    closedFlatBottom(TopoDS::Face((*terminal)(1)), bore->exit, axis))
                    result.shape.blindDepthMm = bore->exit.depth;
            }
        }
        if (result.shape.entrance || result.shape.throughWall || result.shape.blindDepthMm)
            return result;
        return {};
    } catch (const Standard_Failure &) {
        return {};
    }
}
} // namespace bending::cad
