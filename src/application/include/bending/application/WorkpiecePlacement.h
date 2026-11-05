#pragma once
#include "bending/core/PressTools.h"
#include <algorithm>
#include <cmath>

namespace bending::application {
// Machine Z translation of the workpiece from the centred installation, in mm.
// Tool bodies/stations stay fixed. Both tool assemblies must cover the bend envelope.
[[nodiscard]] inline bool validWorkpieceOffset(double offset) noexcept {
    return std::isfinite(offset) && std::abs(offset) <= 1.e6;
}
[[nodiscard]] inline double axialPlacementHalfRange(const core::PressTools &tools,
                                                    double widthMm) noexcept {
    return std::max(0., (std::min(tools.lengthMm, core::punchLengthMm(tools)) - widthMm) / 2);
}
[[nodiscard]] inline bool toolCoversAxialPlacement(const core::PressTools &tools, double widthMm,
                                                   double offsetMm,
                                                   double toleranceMm = 0) noexcept {
    return validWorkpieceOffset(offsetMm) && std::isfinite(widthMm) && widthMm > 0 &&
           std::isfinite(toleranceMm) && toleranceMm >= 0 &&
           std::min(tools.lengthMm, core::punchLengthMm(tools)) + toleranceMm >= widthMm &&
           std::abs(offsetMm) <= axialPlacementHalfRange(tools, widthMm) + toleranceMm;
}
} // namespace bending::application
