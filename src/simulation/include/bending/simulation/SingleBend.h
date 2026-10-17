#pragma once

#include <array>
#include <optional>

namespace bending::simulation {
// Geometric model of a uniform strip, not a press stroke or material calculation.
// Constant thickness and length of the middle surface imply constant volume.
struct SingleBend {
    double thicknessMm;
    double innerRadiusMm;
    double angleRadians;
    double widthMm;
    double fixedFlangeMm;
    double movingFlangeMm;
};
struct Point2Mm {
    double x;
    double y;
};
struct BendSection {
    std::array<Point2Mm, 4> fixed;
    std::array<Point2Mm, 4> moving;
    std::array<Point2Mm, 3> innerArc; // Start, middle, end. A line when flat.
    std::array<Point2Mm, 3> outerArc;
    double angleRadians;
    std::optional<double> innerRadiusMm; // Infinite/unrepresentable radius in the flat limit.
};
[[nodiscard]] bool validSingleBend(const SingleBend &model) noexcept;
[[nodiscard]] double bendAllowanceMm(const SingleBend &model) noexcept;
[[nodiscard]] double blankLengthMm(const SingleBend &model) noexcept;
// Fixed inner tangency at (0,0), material at negative Y, extrusion along positive Z.
// Progress in [0,1]; the fixed flange stays stationary at every intermediate state.
[[nodiscard]] std::optional<BendSection> singleBendSection(const SingleBend &model,
                                                           double progress) noexcept;
} // namespace bending::simulation
