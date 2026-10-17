#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace bending::application {
enum class ThicknessStatus { Unsupported, Detected, Ambiguous, LimitReached, Failed };
struct ThicknessEvidence {
    std::size_t firstFace;
    std::size_t secondFace;
    double distanceMm;
};
struct ThicknessAnalysis {
    ThicknessStatus status = ThicknessStatus::Unsupported;
    std::optional<double> thicknessMm;
    double toleranceMm = 1.0e-6;
    std::vector<ThicknessEvidence> evidence;
};
enum class ThicknessValidation { Invalid, ConflictsWithGeometry, LocalMatch, Unverified, Verified };
[[nodiscard]] ThicknessValidation validateThickness(double valueMm,
                                                    const ThicknessAnalysis &analysis);
} // namespace bending::application
