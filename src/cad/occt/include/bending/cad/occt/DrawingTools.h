#pragma once
#include "bending/core/ToolGeometry.h"
#include <TopoDS_Shape.hxx>
#include <array>
#include <stop_token>
namespace bending::cad {
// Private immutable source contours; independently built bodies, never the imported STEP.
std::array<TopoDS_Shape, 2> makeDrawingTools(const core::ToolAssignment &,
                                             const core::ToolGeometry &, double tipY);
// Exact (within BRep tolerance) vertical translation sweep of an arbitrary line/arc section.
TopoDS_Shape drawingPunchSweep(const core::ToolAssignment &, const core::ToolGeometry &,
                               double tipY, double lift, std::stop_token);
} // namespace bending::cad
