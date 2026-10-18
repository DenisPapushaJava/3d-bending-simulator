#pragma once

namespace bending::core {
// Idealized extruded tools for the initial flat setup, not a manufacturer's catalogue.
struct PressTools {
    double openingMm;
    double dieAngleRadians;
    double punchAngleRadians;
    double noseRadiusMm;
    double lengthMm;
    double clearanceMm; // Lowest point of the punch above the flat sheet.
    bool operator==(const PressTools &) const = default;
};
[[nodiscard]] bool validPressTools(const PressTools &tools) noexcept;
} // namespace bending::core
