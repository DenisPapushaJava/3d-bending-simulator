#pragma once

#include <string>

namespace bending::application {
inline constexpr int defaultDimensionDecimalPlaces = 1;
inline constexpr int maximumDimensionDecimalPlaces = 3;
inline constexpr int maximumPitchDecimalPlaces = 6;

[[nodiscard]] constexpr bool validDimensionDecimalPlaces(int value) {
    return value >= 0 && value <= maximumDimensionDecimalPlaces;
}

// Round the shortest decimal representation of a finite input once, with ties away
// from zero. Omit trailing fractional zeros. Additional places are used for drawing pitches.
[[nodiscard]] std::string formatMeasurement(double value,
                                            int decimals = defaultDimensionDecimalPlaces);
[[nodiscard]] double roundedMeasurement(double value, int decimals);
} // namespace bending::application
