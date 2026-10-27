#pragma once

#include "bending/application/SingleBendPreview.h"
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <optional>

namespace bending::cad {
struct BendDimensionAnchors {
    std::array<gp_Pnt, 2> fixed;
    std::array<gp_Pnt, 2> moving;
    gp_Pnt bendMiddle;
    gp_Vec widthVector;
    double angleRadians;
    std::optional<double> innerRadiusMm;
};
struct PreviewFrame {
    TopoDS_Shape shape;
    std::array<TopoDS_Shape, 2> tools;   // Die, punch; null in the kinematic preview.
    std::array<TopoDS_Shape, 3> regions; // Flange A, bend, flange B; A is fixed only without tools.
    // Optional triangulated source surface with geometrically moved cutouts.
    // Display only: all checks use shape/regions above. Includes the same tools.
    TopoDS_Shape displayShape;
    std::array<TopoDS_Shape, 3> displayRegions;
    // World-space attachments prepared from the same section/basis as the solid.
    std::optional<BendDimensionAnchors> dimensions;
};
// Borrowed immutable shapes, independently built/meshed in the worker.
class OcctPreviewAccess final {
  public:
    [[nodiscard]] static const PreviewFrame &frame(const application::SingleBendPreview &preview,
                                                   std::size_t index);
    // Exact occupied volume over the vertical punch return; null without tool motion.
    [[nodiscard]] static const TopoDS_Shape &
    retractionSweep(const application::SingleBendPreview &preview);
};
} // namespace bending::cad
