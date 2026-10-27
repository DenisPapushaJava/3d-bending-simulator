#pragma once

#include "bending/core/PressTools.h"
#include "bending/simulation/SingleBend.h"

#include <variant>

namespace bending::simulation {
struct FlatToolSetup {
    core::PressTools tools;
    double leftOverlapMm;
    double rightOverlapMm;
    double sheetLeftMm;
    double sheetRightMm;
    double grooveDepthMm;
};
enum class ToolSetupIssue {
    InvalidParameters,
    ToolTooShort,
    MissingLeftSupport,
    MissingRightSupport
};
using FlatToolSetupResult = std::variant<FlatToolSetup, ToolSetupIssue>;
// Machine frame: die top Y=0, groove centre X=0, tools along +Z. The middle of
// the flat bend zone is above X=0; sheet bottom Y=0. Positive shoulder overlap
// proves support only in this initial placement, not during a bending stroke.
[[nodiscard]] FlatToolSetupResult flatToolSetup(const SingleBend &model, core::PressTools tools,
                                                double lengthToleranceMm) noexcept;
} // namespace bending::simulation
