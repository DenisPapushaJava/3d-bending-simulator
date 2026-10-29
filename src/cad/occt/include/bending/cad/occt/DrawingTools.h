#pragma once
#include "bending/core/ToolGeometry.h"
#include <TopoDS_Shape.hxx>
#include <array>
#include <stop_token>
#include <vector>
namespace bending::cad {
struct ToolDisplaySegment;
TopoDS_Shape segmentToolBody(const TopoDS_Shape &, const core::ToolPreset &,
                             const std::optional<core::ToolAssembly> &, double startZ,
                             std::stop_token, std::vector<ToolDisplaySegment> *display = nullptr);
// Private immutable source contours; independently built bodies, never the imported STEP.
std::array<TopoDS_Shape, 2> makeDrawingTools(const core::ToolAssignment &,
                                             const core::ToolGeometry &, double tipY);
// Exact (within BRep tolerance) vertical translation sweep of an arbitrary line/arc section.
TopoDS_Shape drawingPunchSweep(const core::ToolAssignment &, const core::ToolGeometry &,
                               double tipY, double lift, std::stop_token);
} // namespace bending::cad
