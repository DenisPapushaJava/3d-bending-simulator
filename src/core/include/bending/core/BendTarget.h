#pragma once

namespace bending::core {
// Unsigned rotation from flat (radians), not the internal included angle.
// A geometric target only: no tool, direction, springback or material model yet.
struct BendTarget {
    double angleRadians;
    double innerRadiusMm;
    bool operator==(const BendTarget &) const = default;
};
[[nodiscard]] bool validBendTarget(const BendTarget &target) noexcept;
} // namespace bending::core
