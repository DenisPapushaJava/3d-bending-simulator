#pragma once
#include "bending/core/ToolCatalog.h"
namespace bending::core {
// Strict upright, circular-nose punch and symmetric, single rounded V groove.
// These are working references, not a proof of compatibility with machine clamps.
struct ToolGeometry {
    DrawingPoint punchTip;
    DrawingPoint dieTopCentre;
    std::size_t noseEdge, leftShoulderEdge, rightShoulderEdge;
    double shoulderHalfSpanMm;
    double shoulderRadiusMm;
    double leftSupportWidthMm, rightSupportWidthMm;
};
// Derived from the immutable assignment, never from an article name or external file.
[[nodiscard]] std::optional<ToolGeometry> drawingToolGeometry(const ToolAssignment &assignment);
} // namespace bending::core
