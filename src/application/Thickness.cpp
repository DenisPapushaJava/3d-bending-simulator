#include "bending/application/Thickness.h"

#include <algorithm>
#include <cmath>

namespace bending::application {
std::optional<double> suggestThickness(const ThicknessAnalysis &analysis) {
    const auto valid = [](double value) {
        return std::isfinite(value) && value >= 1.0e-6 && value <= 1.0e6;
    };
    if (analysis.status == ThicknessStatus::Detected && analysis.thicknessMm &&
        valid(*analysis.thicknessMm)) {
        return analysis.thicknessMm;
    }
    if (analysis.status != ThicknessStatus::Unsupported || analysis.evidence.empty() ||
        !std::isfinite(analysis.toleranceMm) || analysis.toleranceMm < 0.0) {
        return {};
    }
    const double proposal = analysis.evidence.front().distanceMm;
    if (!valid(proposal)) {
        return {};
    }
    if (std::all_of(analysis.evidence.begin(), analysis.evidence.end(), [&](const auto &pair) {
            return valid(pair.distanceMm) &&
                   std::abs(pair.distanceMm - proposal) <= analysis.toleranceMm;
        })) {
        return proposal;
    }
    return {};
}
ThicknessValidation validateThickness(double valueMm, const ThicknessAnalysis &analysis) {
    if (!std::isfinite(valueMm) || valueMm < 1.0e-6 || valueMm > 1.0e6) {
        return ThicknessValidation::Invalid;
    }
    if (analysis.status == ThicknessStatus::Detected && analysis.thicknessMm) {
        return std::abs(valueMm - *analysis.thicknessMm) <= analysis.toleranceMm
                   ? ThicknessValidation::Verified
                   : ThicknessValidation::ConflictsWithGeometry;
    }
    for (const auto &pair : analysis.evidence) {
        if (std::abs(valueMm - pair.distanceMm) <= analysis.toleranceMm) {
            return ThicknessValidation::LocalMatch;
        }
    }
    return ThicknessValidation::Unverified;
}
} // namespace bending::application
