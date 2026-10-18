#pragma once

#include "bending/simulation/FlatToolSetup.h"

#include <optional>
#include <variant>

namespace bending::simulation {
enum class PressMotionIssue {
    InvalidParameters,
    ToolAngleConflict,
    NoseTooLarge,
    SupportNotProven
};
struct PressMotion {
    FlatToolSetup initial;
    double maximumSupportDistanceMm;
    double lengthToleranceMm;
};
using PressMotionResult = std::variant<PressMotion, PressMotionIssue>;
enum class PressPhase { Approach, Bending };
struct PressState {
    BendSection section;
    PressPhase phase;
    double punchTipYmm;
    double punchTravelMm;     // Downward travel from the initial clearance position.
    double supportDistanceMm; // Along either outer flange from its tangency to the shoulder.
};
// Conservative bounds certify support on both straight flanges for all angles in [0,target].
// This is geometric contact kinematics, not force/material/springback or a continuous CAD sweep.
[[nodiscard]] PressMotionResult preparePressMotion(const SingleBend &model, core::PressTools tools,
                                                   double lengthToleranceMm) noexcept;
// First 20%: punch approach to the flat sheet. Remaining 80%: both flanges rotate
// about their moving shoulder contacts; the rounded punch stays tangent at the centre.
[[nodiscard]] std::optional<PressState>
pressMotionState(const SingleBend &model, const PressMotion &motion, double progress) noexcept;
} // namespace bending::simulation
