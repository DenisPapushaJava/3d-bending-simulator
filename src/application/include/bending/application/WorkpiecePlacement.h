#pragma once
#include "bending/application/Document.h"
#include "bending/core/PressTools.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

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
[[nodiscard]] inline std::array<double, 2>
commonToolSpan(const core::PressTools &tools, double widthMm, double offsetMm) noexcept {
    const double extra = (std::min(tools.lengthMm, core::punchLengthMm(tools)) - widthMm) / 2;
    return {-extra - offsetMm, widthMm + extra - offsetMm};
}
[[nodiscard]] inline bool coversAxialInterval(const std::array<double, 2> &span,
                                              const std::array<double, 2> &interval,
                                              double tolerance) noexcept {
    return interval[0] < interval[1] && span[0] <= interval[0] + tolerance &&
           span[1] + tolerance >= interval[1];
}
[[nodiscard]] inline std::optional<std::array<double, 2>>
trimmedAxialCore(const RecognizedBend &bend) noexcept {
    if (!bend.trimmedEnds || !bend.straightIntervalMm)
        return {};
    const auto &core = *bend.straightIntervalMm;
    if (!std::isfinite(core[0]) || !std::isfinite(core[1]) || core[0] < 0 ||
        core[0] >= bend.lengthMm / 2 || core[1] <= bend.lengthMm / 2 || core[1] > bend.lengthMm ||
        core[1] - core[0] >= bend.lengthMm)
        return {};
    return core;
}
[[nodiscard]] inline bool continuousDrawingTools(const core::ToolAssignment &assignment) noexcept {
    const auto continuous = [](const auto &assembly) {
        return !assembly || core::maximumAssemblyGap(*assembly) == 0;
    };
    return assignment.useDrawingGeometry && assignment.die.drawing && assignment.punch.drawing &&
           continuous(assignment.dieAssembly) && continuous(assignment.punchAssembly);
}
[[nodiscard]] inline bool allowsShortToolEndSupport(bool partial,
                                                    std::optional<double> limit) noexcept {
    return partial && (!limit || *limit > 0);
}
} // namespace bending::application
