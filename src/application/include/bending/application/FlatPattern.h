#pragma once
#include "bending/application/FlatCutProfile.h"
#include "bending/application/ModelMeasurements.h"
#include "bending/core/UnfoldRule.h"
#include <cmath>
#include <optional>
#include <stop_token>
#include <variant>

namespace bending::application {
inline constexpr std::size_t maximumSheetBends = 32;
// Unfolding can keep several separate sectors for one connection between walls.
inline constexpr std::size_t maximumFlatBendSegments = 64;
[[nodiscard]] inline bool isPlanarCutCandidate(const BendAnalysis &analysis) noexcept {
    return analysis.status == BendRecognitionStatus::Completed && analysis.bends.empty() &&
           analysis.interiorContourDiagnostics.empty();
}
struct FlatBend {
    std::size_t bendIndex;
    PointMm start; // First tangency boundary, in the common flat XY plane.
    PointMm end;
    PointMm offset; // Vector from the first boundary to the second; magnitude = BA.
    double turnRadians;
    int direction;
    double radiusMm;
    double k;
    double allowanceMm;
    [[nodiscard]] std::array<PointMm, 2> centreLine() const noexcept {
        return {{{start.x + offset.x / 2, start.y + offset.y / 2, 0},
                 {end.x + offset.x / 2, end.y + offset.y / 2, 0}}};
    }
    [[nodiscard]] double lengthMm() const noexcept {
        return std::hypot(end.x - start.x, end.y - start.y);
    }
};
struct FlatPatternInfo {
    core::UnfoldSettings settings;
    double thicknessMm = 0;
    BoundsMm bounds{};
    std::vector<FlatBend> bends;
    bool cutoutsIgnored = false;
    // Source material side A after unfolding; unknown must not invent a side.
    std::optional<bool> positiveZIsSideA = {};
    std::shared_ptr<const ModelMeasurements> measurements = {};
    std::shared_ptr<const FlatCutProfile> cutProfile = {};
};
class FlatPattern {
  public:
    virtual ~FlatPattern() = default;
    [[nodiscard]] virtual const FlatPatternInfo &info() const noexcept = 0;
};
using FlatPatternPtr = std::shared_ptr<const FlatPattern>;
struct FlatPatternRequest {
    core::UnfoldSettings settings;
    std::optional<double> manualThicknessMm;
    bool operator==(const FlatPatternRequest &) const = default;
};
enum class FlatPatternErrorCode {
    UnsupportedGeometry,
    MissingMaterial,
    ThicknessConflict,
    Cancelled,
    Failed
};
struct FlatPatternError {
    FlatPatternErrorCode code;
    std::string detail;
};
using FlatPatternResult = std::variant<FlatPatternPtr, FlatPatternError>;
class FlatPatternBuilder {
  public:
    virtual ~FlatPatternBuilder() = default;
    [[nodiscard]] virtual FlatPatternResult build(DocumentPtr, FlatPatternRequest,
                                                  std::stop_token) const = 0;
};
} // namespace bending::application
