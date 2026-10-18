#pragma once

#include "bending/application/Document.h"
#include "bending/core/BendTarget.h"
#include "bending/core/PressTools.h"

#include <optional>

namespace bending::application {
enum class BendReview { Pending, Confirmed, Excluded };
// Geometry fingerprint in the coordinates of the embedded STEP. No face indices.
// Deliberately does not identify a bend across a changed/replaced STEP model.
struct BendAnchor {
    PointMm axisStart;
    PointMm axisEnd;
    double angleRadians;
    double innerRadiusMm;
    double thicknessMm;
    double lengthMm;
    bool trimmedEnds;
    double minimumStraightLengthMm;
};
struct BendPreparation {
    std::string id; // Stable within this project, independent of display order/face indices.
    BendAnchor anchor;
    BendReview review = BendReview::Pending;
    std::optional<core::BendTarget> target;
    std::optional<core::PressTools> tools = std::nullopt;
};
struct BendBindings {
    // Current recognized zone -> preparation entry. Ambiguous matches stay unbound.
    std::vector<std::optional<std::size_t>> byRecognizedBend;
    std::vector<std::size_t> unresolved;
};
[[nodiscard]] bool validBendPreparations(const std::vector<BendPreparation> &entries);
// Restores unique matches, keeps unresolved user records, creates pending entries for new zones.
[[nodiscard]] BendBindings restoreBendPreparations(const BendAnalysis &analysis,
                                                   std::vector<BendPreparation> &entries);
} // namespace bending::application
