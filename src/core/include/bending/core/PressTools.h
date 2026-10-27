#pragma once

#include <optional>

namespace bending::core {
// Idealized extruded tools for the initial flat setup, not a manufacturer's catalogue.
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
