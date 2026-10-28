#pragma once

#include <optional>

namespace bending::core {
// Standard V-tool working dimensions. Full body geometry belongs to ToolAssignment.
struct PressTools {
    double openingMm;
    double dieAngleRadians;
    double punchAngleRadians;
    double noseRadiusMm;
    double lengthMm;
    double clearanceMm; // Lowest point of the punch above the flat sheet.
    std::optional<double> punchLengthMm = std::nullopt; // Null keeps legacy equal lengths.
    bool operator==(const PressTools &) const = default;
};
[[nodiscard]] double punchLengthMm(const PressTools &tools) noexcept;
[[nodiscard]] bool validPressTools(const PressTools &tools) noexcept;
} // namespace bending::core
