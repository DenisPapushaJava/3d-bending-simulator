#pragma once
#include "bending/application/Document.h"
#include "bending/core/UnfoldRule.h"
#include <optional>
#include <stop_token>
#include <variant>

namespace bending::application {
struct FlatBend {
    std::size_t bendIndex;
    double firstY;
    double secondY;
    double minimumX;
    double maximumX;
    double turnRadians;
    int direction;
    double radiusMm;
    double k;
    double allowanceMm;
};
struct FlatPatternInfo {
    core::UnfoldSettings settings;
    double thicknessMm = 0;
    BoundsMm bounds{};
    std::vector<FlatBend> bends;
    // Tangency boundaries in flat Y, for dimensions. Not CNC back-gauge positions.
    std::vector<double> wallBoundaries;
    bool cutoutsIgnored = false;
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
