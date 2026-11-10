#pragma once
#include <optional>
#include <string>

namespace bending::core {
enum class SheetMaterial { Unknown, MildSteel, StainlessSteel, Aluminium };
// The version identifies immutable reference data, not calibration on a specific press.
struct UnfoldSettings {
    SheetMaterial material = SheetMaterial::Unknown;
    std::optional<double> manualK;
    int referenceVersion = 1;
    std::string customMaterialName = {};
    bool operator==(const UnfoldSettings &) const = default;
};
[[nodiscard]] bool validUnfoldSettings(const UnfoldSettings &) noexcept;
[[nodiscard]] std::optional<double> unfoldK(const UnfoldSettings &, double radiusMm,
                                            double thicknessMm) noexcept;
[[nodiscard]] std::optional<double> bendAllowance(double radiusMm, double thicknessMm,
                                                  double turnRadians, double k) noexcept;
} // namespace bending::core
