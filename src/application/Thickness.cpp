#include "bending/application/Thickness.h"

#include <cmath>

namespace bending::application {
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
