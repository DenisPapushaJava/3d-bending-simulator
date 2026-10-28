#pragma once
#include <cstddef>
#include <optional>
#include <vector>

namespace bending::core {
enum class SegmentShape { Straight, LeftHorn, RightHorn };
struct ToolSegment {
    double lengthMm = 0;
    unsigned quantity = 0;
    SegmentShape shape = SegmentShape::Straight;
    bool operator==(const ToolSegment &) const = default;
};
struct ToolAssembly {
    std::vector<std::size_t> pieces; // Stock row indices, in installation order.
    double gapMm = 0;                // Uniform clear interval between adjacent pieces.
    bool operator==(const ToolAssembly &) const = default;
};
enum class SegmentPlanStatus { Complete, InsufficientStock, SearchLimit, InvalidInput };
struct SegmentPlan {
    SegmentPlanStatus status = SegmentPlanStatus::InvalidInput;
    ToolAssembly assembly;
    double lengthMm = 0;
};
[[nodiscard]] bool validToolSegments(const std::vector<ToolSegment> &);
[[nodiscard]] std::optional<double> assemblyLength(const std::vector<ToolSegment> &,
                                                   const ToolAssembly &);
// Exact to 0.001 mm. Prefer gap-free stock: minimize excess length, then pieces.
// Only when unavailable, try gaps up to maximumGapMm; minimize excess, gap, then pieces.
// Horns need their own 3D geometry and are deliberately excluded from this first planner.
[[nodiscard]] SegmentPlan planToolSegments(const std::vector<ToolSegment> &, double requiredMm,
                                           std::size_t stateBudget = 200000,
                                           double maximumGapMm = 0);
} // namespace bending::core
