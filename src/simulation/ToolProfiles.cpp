#include "bending/simulation/ToolProfiles.h"

#include <cmath>
#include <numbers>

namespace bending::simulation {
std::optional<ToolProfiles> toolProfiles(const core::PressTools &t) noexcept {
    if (!core::validPressTools(t)) {
        return {};
    }
    const double halfAngle = t.punchAngleRadians / 2;
    const double tangentX = t.noseRadiusMm * std::cos(halfAngle);
    const double tangentY = t.noseRadiusMm * (1 - std::sin(halfAngle));
    const double topX =
        (t.openingMm - t.noseRadiusMm) * std::tan(halfAngle) + t.noseRadiusMm / std::cos(halfAngle);
    const double v = t.openingMm;
    const double depth = v / (2 * std::tan(t.dieAngleRadians / 2));
    const double bottom = -depth - v / 2;
    return ToolProfiles{
        {{{-topX, v}, {-tangentX, tangentY}, {0, 0}, {tangentX, tangentY}, {topX, v}}},
        {{0, t.noseRadiusMm},
         t.noseRadiusMm,
         std::numbers::pi + halfAngle,
         std::numbers::pi - t.punchAngleRadians},
        {{{-v, 0}, {-v / 2, 0}, {0, -depth}, {v / 2, 0}, {v, 0}, {v, bottom}, {-v, bottom}}},
        depth};
}
} // namespace bending::simulation
