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
// The first component outside the geometric tolerance fixes the positive axis.
[[nodiscard]] inline bool bendAxisNeedsReversal(const RecognizedBend &bend,
                                                double tolerance) noexcept {
    for (const double component :
         {bend.axisEnd.x - bend.axisStart.x, bend.axisEnd.y - bend.axisStart.y,
          bend.axisEnd.z - bend.axisStart.z})
        if (std::abs(component) > tolerance)
            return component < 0;
    return false;
}
// Offsets that cover the required material with both working edges. A shorter
// continuous pair can have an off-centre range even when the nominal range is zero.
[[nodiscard]] inline std::array<double, 2>
axialSupportOffsets(const core::PressTools &tools, double width,
                    const std::array<double, 2> &required) noexcept {
    const auto span = commonToolSpan(tools, width, 0);
    return {span[0] - required[0], span[1] - required[1]};
}

// Best axial alignment of the punch within the estimated wall window, constrained
// by the support range. An open end is represented by an infinite boundary.
[[nodiscard]] inline double preferredPunchOffset(double width, double punch,
                                                 const std::array<double, 2> &window,
                                                 const std::array<double, 2> &offsets) noexcept {
    const double low = (width + punch) / 2 - window[1];
    const double high = (width - punch) / 2 - window[0];
    const double desired = low <= high ? std::clamp(0., low, high) : (low + high) / 2;
    return std::clamp(desired, offsets[0], offsets[1]);
}
} // namespace bending::application
