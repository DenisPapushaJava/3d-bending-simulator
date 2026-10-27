#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/ToolProfiles.h"

#include <algorithm>
#include <cmath>

namespace bending::simulation {
FlatToolSetupResult flatToolSetup(const SingleBend &model, core::PressTools tools,
                                  double lengthToleranceMm) noexcept {
    if (!validSingleBend(model) || !core::validPressTools(tools) ||
        !std::isfinite(lengthToleranceMm) || lengthToleranceMm < 1.0e-7 ||
        lengthToleranceMm > 0.01) {
        return ToolSetupIssue::InvalidParameters;
    }
    if (tools.lengthMm + lengthToleranceMm < model.widthMm ||
        core::punchLengthMm(tools) + lengthToleranceMm < model.widthMm) {
        return ToolSetupIssue::ToolTooShort;
    }
    const double halfZone = bendAllowanceMm(model) / 2.0;
    const double left = -(model.fixedFlangeMm + halfZone);
    const double right = model.movingFlangeMm + halfZone;
    const double leftOverlap = std::min(tools.openingMm / 2.0, -left - tools.openingMm / 2.0);
    const double rightOverlap = std::min(tools.openingMm / 2.0, right - tools.openingMm / 2.0);
    if (leftOverlap <= lengthToleranceMm) {
        return ToolSetupIssue::MissingLeftSupport;
    }
    if (rightOverlap <= lengthToleranceMm) {
        return ToolSetupIssue::MissingRightSupport;
    }
    return FlatToolSetup{tools, leftOverlap, rightOverlap,
                         left,  right,       toolProfiles(tools)->grooveDepthMm};
}
} // namespace bending::simulation
