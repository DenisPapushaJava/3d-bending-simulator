#pragma once
#include "bending/core/ToolCatalog.h"
namespace bending::core {
// Strict upright, circular-nose punch and/or symmetric, single rounded V groove.
// A missing drawing leaves its references unused; an unsupported present one fails.
// These are working references, not a proof of compatibility with machine clamps.
struct ToolGeometry {
    DrawingPoint punchTip;
    DrawingPoint dieTopCentre;
    std::size_t noseEdge = 0, leftShoulderEdge = 0, rightShoulderEdge = 0;
    double shoulderHalfSpanMm = 0;
    double shoulderRadiusMm = 0;
    double leftSupportWidthMm = 0, rightSupportWidthMm = 0;
    bool hasPunch = false, hasDie = false;
};
// Derived from the immutable assignment, never from an article name or external file.
[[nodiscard]] std::optional<ToolGeometry> drawingToolGeometry(const ToolAssignment &assignment);
// Both explicit supported bodies; does not certify stock, clamps, load or a part setup.
[[nodiscard]] bool hasCompleteDrawingToolGeometry(const ToolAssignment &assignment);
struct PunchMount {
    double centreX, shoulderY, heightMm, minX, maxX; // Relative to the working tip.
};
// Bounded recognition of a narrow top tang and a wider horizontal shoulder.
// No mechanical clamp/retention certification; ambiguous profiles return null.
[[nodiscard]] std::optional<PunchMount> punchMountReference(const ToolAssignment &);
// Initial preference: clearly offset working body toward the operator (-X), relief
// toward the rear. Unknown/symmetric -> null. Independent of the die and assemblies;
// does not override saved/manual assignments or certify clamp compatibility.
[[nodiscard]] std::optional<bool> defaultPunchOrientation(const ToolPreset &);
struct DetectedDieGroove {
    double openingMm;
    DieGroove settings;
};
// Recognizes one unambiguous supported groove at each quarter-turn; no article lookup.
[[nodiscard]] std::vector<DetectedDieGroove> detectDieGrooves(const ToolDrawing &);
} // namespace bending::core
