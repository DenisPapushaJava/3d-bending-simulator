#pragma once
#include "bending/core/ToolSegments.h"
#include <TopoDS_Shape.hxx>
#include <stop_token>

namespace bending::cad {
// Unoriented, upright section; Y=tip and Z=start of this particular section.
TopoDS_Shape cutHornSection(const TopoDS_Shape &, const core::ToolSegment &, double tipY,
                            double startZ, std::stop_token);
// Exact translation sweep of the supported plane/cylinder section boundary.
// Cylinder faces are split at tangencies before sweeping their forward side.
TopoDS_Shape hornTranslationSweep(const TopoDS_Shape &, double lift, std::stop_token);
} // namespace bending::cad
