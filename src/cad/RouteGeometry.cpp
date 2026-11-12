#include "RouteGeometry.h"
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <algorithm>
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
SkinBoundaryLookup::SkinBoundaryLookup(const std::array<TopoDS_Shape, 3> &bodies, double tolerance)
    : tolerance_(tolerance) {
    for (std::size_t r = 0; r < bodies.size(); ++r) {
        distances_[r].LoadS2(bodies[r]);
        auto &box = boxes_[r];
        BRepBndLib::AddOptimal(bodies[r], box, false, false);
        const auto native = std::max({BRep_Tool::MaxTolerance(bodies[r], TopAbs_VERTEX),
                                      BRep_Tool::MaxTolerance(bodies[r], TopAbs_EDGE),
                                      BRep_Tool::MaxTolerance(bodies[r], TopAbs_FACE)});
        if (!std::isfinite(native))
            box.SetVoid();
        else if (!box.IsVoid() && !box.IsOpen())
            box.Enlarge(tolerance + Precision::Confusion() + native);
    }
}
unsigned SkinBoundaryLookup::membership(const gp_Pnt &point, std::stop_token stop) {
    if (stop.stop_requested())
        return 0;
    const auto vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
    unsigned regions = 0;
    for (std::size_t r = 0; r < distances_.size(); ++r) {
        if (stop.stop_requested())
            return 0;
        const auto &box = boxes_[r];
        if (!box.IsVoid() && !box.IsOpen() && box.IsOut(point))
            continue;
        auto &distance = distances_[r];
        distance.LoadS1(vertex);
        if (distance.Perform() && distance.IsDone() && std::isfinite(distance.Value()) &&
            distance.Value() <= tolerance_)
            regions |= 1u << r;
    }
    return regions;
}
RoutePlacement routePlacement(const SheetLayout &layout, const std::vector<double> &progress) {
    RoutePlacement result;
    result.walls.resize(layout.walls.size());
    result.bends.resize(layout.bends.size());
    std::vector<std::optional<double>> wallProgress(layout.walls.size());
    for (const auto &b : layout.bends) {
        const double p = progress.at(b.bendIndex);
        auto &wall = wallProgress[b.child];
        if (wall && *wall != p)
            throw std::runtime_error("Bend zones on the same wall require one stroke");
        const bool first = !wall;
        wall = p;
        const auto parent = result.walls[b.parent];
        if (p == 1) {
            if (first)
                result.walls[b.child] = parent;
            result.bends[b.bendIndex] = [parent](gp_Pnt v) { return v.Transformed(parent); };
        } else if (p == 0) {
            const auto flat = parent * layout.walls[b.parent].toFlat.Inverted();
            if (first)
                result.walls[b.child] = flat * layout.walls[b.child].toFlat;
            result.bends[b.bendIndex] = [map = b.toFlat, flat](gp_Pnt v) {
                return map(v).Transformed(flat);
            };
        } else {
            if (!b.atProgress || !b.childAtProgress)
                throw std::runtime_error("Missing partial bend geometry");
            if (first)
                result.walls[b.child] = parent * b.childAtProgress(p);
            result.bends[b.bendIndex] = [map = b.atProgress, p, parent](gp_Pnt v) {
                return map(v, p).Transformed(parent);
            };
        }
    }
    return result;
}
} // namespace bending::cad
