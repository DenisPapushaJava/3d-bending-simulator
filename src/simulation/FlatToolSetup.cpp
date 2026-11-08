#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/ToolProfiles.h"

#include <algorithm>
#include <cmath>

namespace bending::simulation {
FlatToolSetupResult flatToolSetup(const SingleBend &model, core::PressTools tools,
                                  double lengthToleranceMm, std::optional<DieShoulders> shoulders,
                                  std::optional<double> requiredAxialWidthMm) noexcept {
    if (!validSingleBend(model) || !core::validPressTools(tools) ||
        !std::isfinite(lengthToleranceMm) || lengthToleranceMm < 1.0e-7 ||
        lengthToleranceMm > 0.01) {
        return ToolSetupIssue::InvalidParameters;
    }
    if (shoulders &&
        (!std::isfinite(shoulders->halfSpanMm) || !std::isfinite(shoulders->radiusMm) ||
         !std::isfinite(shoulders->leftWidthMm) || !std::isfinite(shoulders->rightWidthMm) ||
         shoulders->halfSpanMm <= 0 || shoulders->radiusMm <= 0 || shoulders->leftWidthMm <= 0 ||
         shoulders->rightWidthMm <= 0))
        return ToolSetupIssue::InvalidParameters;
    const double width = requiredAxialWidthMm.value_or(model.widthMm);
    if (!std::isfinite(width) || width <= 0 || width > model.widthMm)
        return ToolSetupIssue::InvalidParameters;
    if (tools.lengthMm + lengthToleranceMm < width ||
        core::punchLengthMm(tools) + lengthToleranceMm < width) {
        return ToolSetupIssue::ToolTooShort;
    }
    const double halfZone = bendAllowanceMm(model) / 2.0;
    const double left = -(model.fixedFlangeMm + halfZone);
    const double right = model.movingFlangeMm + halfZone;
    const double span = shoulders ? shoulders->halfSpanMm : tools.openingMm / 2.0;
    const double leftOverlap = std::min(shoulders ? shoulders->leftWidthMm : span, -left - span);
    const double rightOverlap = std::min(shoulders ? shoulders->rightWidthMm : span, right - span);
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
