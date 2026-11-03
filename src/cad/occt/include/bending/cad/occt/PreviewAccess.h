#pragma once

#include "bending/application/SingleBendPreview.h"
#include "bending/cad/occt/SheetOrientation.h"
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <optional>
#include <vector>

namespace bending::cad {
struct BendDimensionAnchors {
    std::array<gp_Pnt, 2> fixed;
    std::array<gp_Pnt, 2> moving;
    gp_Pnt bendMiddle;
    gp_Vec widthVector;
    double angleRadians;
    std::optional<double> innerRadiusMm;
};
struct ToolDisplaySegment {
    TopoDS_Shape shape; // A subshape of the complete checked tool, at this frame's placement.
    double lengthMm;
};
struct MachineDisplayComponent {
    std::string id, name;
    TopoDS_Shape shape;
    bool moving = false;
    bool allowPunchContact = false; // Legacy V1 only; V2 models the open pocket explicitly.
    bool allowDieContact = false;
    bool gauging = false; // Independently positioned, including checks against static machine.
};
// Finite drawing only; the checked floor is the infinite half-space below yMm.
struct FloorPlane {
    double yMm;
    TopoDS_Shape surface, grid;
};
struct CollisionHighlightVertex {
    gp_Pnt point;
    float strength; // Local falloff, 0..1. Visual marker, not contact pressure.
};
struct CollisionHighlightPatch {
    std::string bodyId;
    double radiusMm;
    std::vector<gp_Pnt> centres;
    std::vector<CollisionHighlightVertex> vertices; // Independent triangles; bounded local mesh.
    // A verified straight edge of the overlap, bounded by its actual endpoints.
    // Parallel surface triangles use a strip; other triangles retain point markers.
    std::optional<std::array<gp_Pnt, 2>> band;
};
struct PreviewFrame {
    SheetOrientationVisual orientation;
    std::vector<CollisionHighlightPatch> collisionHighlights;
    std::vector<MachineDisplayComponent> machine;
    std::optional<FloorPlane> floor;
    std::vector<gp_Pnt> floorContacts;     // Confirmed cut-plane contacts, visible even edge-on.
    std::vector<gp_Pnt> backgaugeContacts; // Physical contacts only in the pre-bend frame.
    TopoDS_Shape shape;
    std::array<TopoDS_Shape, 2> tools; // Die, punch; null in the kinematic preview.
    std::array<std::vector<ToolDisplaySegment>, 2> toolSegments;
    std::array<TopoDS_Shape, 3> regions; // Flange A, bend, flange B; A is fixed only without tools.
    std::array<gp_Trsf, 2> materialTransforms; // Source components -> this frame.
    // Optional triangulated source surface with geometrically moved cutouts.
    // Display only: all checks use shape/regions above. Includes the same tools.
    TopoDS_Shape displayShape;
    std::array<TopoDS_Shape, 3> displayRegions;
    std::vector<TopoDS_Shape> bendZones; // Optional draft highlights by original bend index.
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
    [[nodiscard]] static const std::array<TopoDS_Shape, 2> &
    sourceComponents(const application::SingleBendPreview &);
};
} // namespace bending::cad
