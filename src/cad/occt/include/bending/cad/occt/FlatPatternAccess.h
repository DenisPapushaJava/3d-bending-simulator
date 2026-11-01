#pragma once
#include "bending/application/FlatPattern.h"
#include "bending/cad/occt/SheetOrientation.h"
#include <TopoDS_Shape.hxx>
#include <functional>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

namespace bending::cad {
// Immutable decomposition in the unnormalised common flat frame. Used by the
// route builder; the drawing's final origin translation is deliberately separate.
struct SheetWallRegion {
    std::array<std::size_t, 2> faces;
    TopoDS_Shape solid;
    gp_Trsf toFlat;
};
struct SheetBendRegion {
    std::size_t bendIndex, parent, child;
    TopoDS_Shape solid, flatSolid;
    std::function<gp_Pnt(gp_Pnt)> toFlat;
    // Original coordinates -> partial bend, parent wall fixed. Same K=0.5 model as routes.
    std::function<gp_Pnt(gp_Pnt, double)> atProgress;
    std::function<gp_Trsf(double)> childAtProgress;
    std::function<TopoDS_Shape(double)> solidAtProgress;
    gp_Ax3 sectionAxes;
};
struct SheetLayout {
    std::vector<SheetWallRegion> walls;
    std::vector<SheetBendRegion> bends;
};
class OcctFlatPatternAccess final {
  public:
    [[nodiscard]] static bool belongsTo(const application::FlatPattern &,
                                        const application::Document &);
    // Borrowed, independently built and meshed in the CAD worker.
    [[nodiscard]] static const TopoDS_Shape &shape(const application::FlatPattern &);
    // Verified calculation solid; closed cutouts may be filled, unlike the display skin.
    [[nodiscard]] static const TopoDS_Shape &geometry(const application::FlatPattern &);
    [[nodiscard]] static const std::vector<std::vector<application::PointMm>> &
    openingContours(const application::FlatPattern &);
    [[nodiscard]] static const std::vector<TopoDS_Shape> &
    bendLines(const application::FlatPattern &);
    [[nodiscard]] static const SheetLayout &layout(const application::FlatPattern &);
    [[nodiscard]] static const SheetOrientationVisual &
    orientation(const application::FlatPattern &);
};
} // namespace bending::cad
