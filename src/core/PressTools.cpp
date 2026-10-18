#include "bending/core/PressTools.h"

#include <cmath>
#include <numbers>

namespace bending::core {
bool validPressTools(const PressTools &t) noexcept {
    const auto in = [](double v, double low, double high) {
        return std::isfinite(v) && v >= low && v <= high;
    };
    constexpr double degree = std::numbers::pi / 180.0;
    return in(t.openingMm, 1.0, 1000.0) && in(t.dieAngleRadians, 30.0 * degree, 120.0 * degree) &&
           in(t.punchAngleRadians, 10.0 * degree, 120.0 * degree) &&
           in(t.noseRadiusMm, 0.1, t.openingMm / 4.0) && in(t.lengthMm, 1.0, 100000.0) &&
           in(t.clearanceMm, 0.1, 1000.0);
}
} // namespace bending::core
