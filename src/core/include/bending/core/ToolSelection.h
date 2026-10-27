#pragma once

#include "bending/core/ToolCatalog.h"

namespace bending::core {
struct ToolSelectionRequest {
    double thicknessMm;
    double bendAngleRadians; // Turn from flat, unlike internal tool angles.
    double innerRadiusMm;
    double bendLengthMm;
    bool operator==(const ToolSelectionRequest &) const = default;
};
enum class ToolRejection { None, MissingData, Teaching, TooShort, Angle, Radius, NarrowOpening };
struct ToolCandidate {
    std::size_t punch;
    std::size_t die;
    std::size_t groove;
    double score;
    double openingThicknessRatio;
};
struct ToolSelectionResult {
    bool validRequest = false;
    std::vector<ToolCandidate> candidates;
    std::size_t missingData = 0;
    std::size_t rejectedPairs = 0;
};
// Parameter screening only, no material/force/mounting or CAD collision certificate.
// V/t >= 5 is a conservative scope of this initial standard-air-bending search.
[[nodiscard]] ToolRejection screenTools(const ToolSelectionRequest &request,
                                        const ToolPreset &punch, const ToolPreset &die,
                                        std::size_t groove);
[[nodiscard]] ToolSelectionResult selectTools(const ToolCatalog &catalog,
                                              const ToolSelectionRequest &request);
} // namespace bending::core
