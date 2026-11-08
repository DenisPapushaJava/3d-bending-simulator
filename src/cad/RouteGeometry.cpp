#include "RouteGeometry.h"
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <cmath>
#include <stdexcept>
namespace bending::cad {
std::optional<std::size_t> adjacentSheetWall(const SheetBendRegion &active,
                                             const SheetBendRegion &other) {
    if (active.parent == other.parent || active.child == other.parent)
        return other.child;
    if (active.parent == other.child || active.child == other.child)
        return other.parent;
    return {};
}
std::optional<std::array<double, 2>> projectedWallSpan(const TopoDS_Shape &wall,
                                                       const gp_Pnt &origin, const gp_Dir &axis) {
    try {
        gp_Trsf local;
        local.SetTransformation(gp_Ax3(origin, axis));
        Bnd_Box projected;
        BRepBndLib::AddOptimal(wall.Moved(TopLoc_Location(local)), projected, false, false);
        if (!projected.IsVoid() && !projected.IsOpen())
            return std::array{projected.CornerMin().Z(), projected.CornerMax().Z()};
    } catch (const Standard_Failure &) {
        // Optional placement hint; exact tool checks retain their own geometry.
    }
    return {};
}
unsigned skinBoundaryMembership(const gp_Pnt &point, const std::array<TopoDS_Shape, 3> &bodies,
                                double tolerance, std::stop_token stop) {
    if (stop.stop_requested())
        return 0;
    const auto vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
    unsigned regions = 0;
    for (std::size_t r = 0; r < bodies.size(); ++r) {
        if (stop.stop_requested())
            return 0;
        BRepExtrema_DistShapeShape distance(vertex, bodies[r]);
        if (distance.IsDone() && std::isfinite(distance.Value()) && distance.Value() <= tolerance)
            regions |= 1u << r;
    }
    return regions;
}
RoutePlacement routePlacement(const SheetLayout &layout, const std::vector<double> &progress) {
    RoutePlacement result;
    result.walls.resize(layout.walls.size());
    result.bends.resize(layout.bends.size());
    for (const auto &b : layout.bends) {
        const double p = progress.at(b.bendIndex);
        const auto parent = result.walls[b.parent];
        const auto flat = parent * layout.walls[b.parent].toFlat.Inverted();
        if (p == 1) {
            result.walls[b.child] = parent;
            result.bends[b.bendIndex] = [parent](gp_Pnt v) { return v.Transformed(parent); };
        } else if (p == 0) {
            result.walls[b.child] = flat * layout.walls[b.child].toFlat;
            result.bends[b.bendIndex] = [map = b.toFlat, flat](gp_Pnt v) {
                return map(v).Transformed(flat);
            };
        } else {
            if (!b.atProgress || !b.childAtProgress)
                throw std::runtime_error("Missing partial bend geometry");
            result.walls[b.child] = parent * b.childAtProgress(p);
            result.bends[b.bendIndex] = [map = b.atProgress, p, parent](gp_Pnt v) {
                return map(v, p).Transformed(parent);
            };
        }
    }
    return result;
}
} // namespace bending::cad
