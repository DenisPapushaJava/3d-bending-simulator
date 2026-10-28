#include "bending/core/UnfoldRule.h"
#include <array>
#include <cmath>
#include <numbers>

namespace bending::core {
bool validUnfoldSettings(const UnfoldSettings &s) noexcept {
    return s.referenceVersion == 1 &&
           (s.material == SheetMaterial::Unknown || s.material == SheetMaterial::MildSteel ||
            s.material == SheetMaterial::StainlessSteel ||
            s.material == SheetMaterial::Aluminium) &&
           (!s.manualK || (std::isfinite(*s.manualK) && *s.manualK >= 0 && *s.manualK <= .5));
}
std::optional<double> unfoldK(const UnfoldSettings &s, double r, double t) noexcept {
    if (!validUnfoldSettings(s) || !std::isfinite(r) || !std::isfinite(t) || r <= 0 || t <= 0)
        return {};
    if (s.manualK)
        return s.manualK;
    // Wiz Parts Design Guide v1.0 (September 2024), printed p.4, AIR bending.
    // Our explicit boundary policy: R=t belongs to the lower band, R=3t to upper.
    // A relative 1e-7 guard avoids switching bands due to STEP round-off.
    const double ratio = r / t;
    const std::size_t band = ratio <= 1 + 1e-7 ? 0 : ratio >= 3 - 1e-7 ? 2 : 1;
    switch (s.material) {
    case SheetMaterial::MildSteel:
        return std::array{.38, .43, .50}[band];
    case SheetMaterial::StainlessSteel:
        return std::array{.40, .45, .50}[band];
    case SheetMaterial::Aluminium:
        return std::array{.33, .40, .50}[band];
    case SheetMaterial::Unknown:
        return {};
    }
    return {};
}
std::optional<double> bendAllowance(double r, double t, double turn, double k) noexcept {
    if (!std::isfinite(r) || !std::isfinite(t) || !std::isfinite(turn) || !std::isfinite(k) ||
        r <= 0 || t <= 0 || turn <= 0 || turn >= std::numbers::pi || k < 0 || k > .5)
        return {};
    const double value = turn * (r + k * t);
    return std::isfinite(value) ? std::optional{value} : std::nullopt;
}
} // namespace bending::core
