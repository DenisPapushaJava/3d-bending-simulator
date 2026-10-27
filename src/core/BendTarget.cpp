#include "bending/core/BendTarget.h"

#include <cmath>
#include <numbers>

namespace bending::core {
bool validBendTarget(const BendTarget &target) noexcept {
    return std::isfinite(target.angleRadians) && std::isfinite(target.innerRadiusMm) &&
           target.angleRadians >= std::numbers::pi / 180.0 &&
           target.angleRadians <= 179.0 * std::numbers::pi / 180.0 && target.innerRadiusMm > 0.0 &&
           target.innerRadiusMm <= 1.0e6;
}
} // namespace bending::core
