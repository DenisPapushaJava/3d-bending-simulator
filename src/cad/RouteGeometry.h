#pragma once
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepExtrema_DistShapeShape.hxx>
#include <Bnd_Box.hxx>
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
// Worker-local exact boundary queries against the same three material regions.
class SkinBoundaryLookup {
  public:
    SkinBoundaryLookup(const std::array<TopoDS_Shape, 3> &, double tolerance);
    [[nodiscard]] unsigned membership(const gp_Pnt &, std::stop_token);

  private:
    double tolerance_;
    std::array<Bnd_Box, 3> boxes_;
    std::array<BRepExtrema_DistShapeShape, 3> distances_;
};
std::array<TopoDS_Shape, 2> makeRouteDraftTools(const core::PressTools &,
                                                const std::optional<core::ToolAssignment> &,
                                                std::stop_token,
                                                std::array<std::vector<ToolDisplaySegment>, 2> &);
} // namespace bending::cad
