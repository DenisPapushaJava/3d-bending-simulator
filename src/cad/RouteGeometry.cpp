#include "RouteGeometry.h"
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <cmath>
#include <stdexcept>
namespace bending::cad {
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
