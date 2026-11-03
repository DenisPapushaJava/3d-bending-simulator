#pragma once

#include "bending/core/Backgauge.h"
#include <optional>

namespace bending::core {
// Stable built-in profile revision. None preserves the scope of legacy projects.
enum class MachineModel { None, DemoPressV1, DemoPressV2, DemoPressV3 };
// Standard V-tool working dimensions. Full body geometry belongs to ToolAssignment.
struct PressTools {
    double openingMm;
    double dieAngleRadians;
    double punchAngleRadians;
    double noseRadiusMm;
    double lengthMm;
    double clearanceMm; // Lowest point of the punch above the flat sheet.
    std::optional<double> punchLengthMm = std::nullopt; // Null keeps legacy equal lengths.
    MachineModel machineModel = MachineModel::None;
    bool punchReversed = false; // Physical half-turn about Y through the working axis.
    std::optional<BackgaugeSetup> backgauge = std::nullopt;
    bool operator==(const PressTools &) const = default;
};
[[nodiscard]] double punchLengthMm(const PressTools &tools) noexcept;
[[nodiscard]] bool validPressTools(const PressTools &tools) noexcept;
} // namespace bending::core
