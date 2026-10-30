#pragma once

#include "bending/core/ToolCatalog.h"
#include <memory>

namespace bending::core {
// Product guard for the current standard air-bending scope, not a material norm.
inline constexpr double minimumAirBendOpeningRatio = 5.;
[[nodiscard]] bool standardAirBendOpening(double thicknessMm, double openingMm) noexcept;
// Boundary of our current mode; does not certify tool load or a material-specific minimum.
[[nodiscard]] double minimumStandardDieOpeningMm(double thicknessMm) noexcept;
[[nodiscard]] double referenceDieOpeningMm(double thicknessMm) noexcept;
struct ToolSelectionRequest {
    double thicknessMm;
    double bendAngleRadians; // Turn from flat, unlike internal tool angles.
    double innerRadiusMm;
    double bendLengthMm;
    double maximumSegmentGapMm = 3;
    bool operator==(const ToolSelectionRequest &) const = default;
};
enum class ToolRejection { None, MissingData, Teaching, TooShort, Angle, Radius, NarrowOpening };
struct ToolCandidate {
    std::size_t punch;
    std::size_t die;
    std::size_t groove;
    double score;
    double openingThicknessRatio;
    std::shared_ptr<const ToolAssembly> punchAssembly;
    std::shared_ptr<const ToolAssembly> dieAssembly;
};
struct ToolSelectionResult {
    bool validRequest = false;
    std::vector<ToolCandidate> candidates;
    std::size_t missingData = 0;
    std::size_t rejectedPairs = 0;
    bool segmentSearchLimited = false;
};
// Parameter screening only, no material/force/mounting or CAD collision certificate.
// V/t >= 5 is the current product boundary; material/tool limits can be stricter.
[[nodiscard]] ToolRejection screenTools(const ToolSelectionRequest &request,
                                        const ToolPreset &punch, const ToolPreset &die,
                                        std::size_t groove);
[[nodiscard]] ToolSelectionResult selectTools(const ToolCatalog &catalog,
                                              const ToolSelectionRequest &request);
} // namespace bending::core
