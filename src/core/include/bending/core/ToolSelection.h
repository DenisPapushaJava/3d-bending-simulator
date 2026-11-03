#pragma once

#include "bending/core/ToolCatalog.h"
#include <array>
#include <memory>
#include <span>
#include <stop_token>

namespace bending::core {
// Projection of an adjacent planar flange when the active bend is flat and the
// neighbour is formed. XY uses the active inner surface at Y=0; thickness is below.
// A bounded, approximate ranking hint, never a collision/support certificate.
struct ToolFlangeHint {
    std::size_t bendIndex, neighbourBendIndex;
    std::array<DrawingPoint, 4> section;
    bool operator==(const ToolFlangeHint &) const = default;
};
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
    std::vector<ToolFlangeHint> shapeHints = {};
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
    std::optional<double> shapeRisk = std::nullopt;
};
struct ToolSelectionResult {
    bool validRequest = false;
    std::vector<ToolCandidate> candidates;
    std::size_t missingData = 0;
    std::size_t rejectedPairs = 0;
    bool segmentSearchLimited = false;
    bool shapeRanked = false;
};
// Fraction of sampled neighbouring flange points inside the full captured profiles.
// Best workpiece side and best order of each adjacent pair; no global route claim.
// Unsupported/over-budget/absent data is null, not zero risk.
[[nodiscard]] std::optional<double> toolShapeRisk(const ToolAssignment &,
                                                  std::span<const ToolFlangeHint>,
                                                  double thicknessMm, std::stop_token = {});
// Parameter screening only, no material/force/mounting or CAD collision certificate.
// V/t >= 5 is the current product boundary; material/tool limits can be stricter.
[[nodiscard]] ToolRejection screenTools(const ToolSelectionRequest &request,
                                        const ToolPreset &punch, const ToolPreset &die,
                                        std::size_t groove);
[[nodiscard]] ToolSelectionResult selectTools(const ToolCatalog &catalog,
                                              const ToolSelectionRequest &request);
} // namespace bending::core
