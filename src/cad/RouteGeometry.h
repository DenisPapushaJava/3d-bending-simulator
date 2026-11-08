#pragma once
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
namespace bending::cad {
struct RoutePlacement {
    std::vector<gp_Trsf> walls;
    std::vector<std::function<gp_Pnt(gp_Pnt)>> bends;
};
RoutePlacement routePlacement(const SheetLayout &, const std::vector<double> &progress);
[[nodiscard]] std::optional<std::size_t> adjacentSheetWall(const SheetBendRegion &active,
                                                           const SheetBendRegion &other);
[[nodiscard]] std::optional<std::array<double, 2>>
projectedWallSpan(const TopoDS_Shape &, const gp_Pnt &origin, const gp_Dir &axis);
TopoDS_Shape makeRouteSector(const simulation::SingleBend &, const gp_Ax3 &, double);
// Fallback for unclassified display nodes, after the complete solid proof.
// Retain every matching component, including shared bend rails. No meshing/Boolean.
[[nodiscard]] unsigned skinBoundaryMembership(const gp_Pnt &, const std::array<TopoDS_Shape, 3> &,
                                              double tolerance, std::stop_token);
std::array<TopoDS_Shape, 2> makeRouteDraftTools(const core::PressTools &,
                                                const std::optional<core::ToolAssignment> &,
                                                std::stop_token,
                                                std::array<std::vector<ToolDisplaySegment>, 2> &);
} // namespace bending::cad
