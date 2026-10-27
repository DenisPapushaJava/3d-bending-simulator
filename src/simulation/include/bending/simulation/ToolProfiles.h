#pragma once

#include "bending/core/PressTools.h"
#include "bending/simulation/SingleBend.h"
#include <array>
#include <optional>

namespace bending::simulation {
struct ProfileArc {
    Point2Mm center;
    double radiusMm;
    double startRadians; // Mathematical XY, counterclockwise from +X.
    double sweepRadians;
};
// The actual idealized working contours used by CAD. No manufacturer body/shank.
// Punch tip at (0,0), die shoulders at Y=0; these are separate local sections.
struct ToolProfiles {
    std::array<Point2Mm, 5> punch; // Left top, left tangency, tip, right tangency, right top.
    ProfileArc nose;
    std::array<Point2Mm, 7> die; // Closed polygon, first vertex not repeated.
    double grooveDepthMm;
};
[[nodiscard]] std::optional<ToolProfiles> toolProfiles(const core::PressTools &tools) noexcept;
} // namespace bending::simulation
