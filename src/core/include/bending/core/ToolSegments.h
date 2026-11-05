#pragma once
#include <cstddef>
#include <optional>
#include <stop_token>
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
    double gapMm = 0;                // Uniform spacing when jointGapsMm is empty.
    // Otherwise exactly pieces.size()-1 gaps, in installation order; gapMm must be zero.
    std::vector<double> jointGapsMm = {};
    bool operator==(const ToolAssembly &) const = default;
};
struct ToolSectionInterval {
    std::size_t stockRow;
    double fromMm, toMm; // Assembly-local axial coordinates, beginning at zero.
};
[[nodiscard]] double assemblyGapAfter(const ToolAssembly &, std::size_t piece) noexcept;
[[nodiscard]] double maximumAssemblyGap(const ToolAssembly &) noexcept;
[[nodiscard]] std::optional<std::vector<ToolSectionInterval>>
assemblyIntervals(const std::vector<ToolSegment> &, const ToolAssembly &);
// Missing section length inside an axial interval; invalid input has no value.
[[nodiscard]] std::optional<double> assemblyUncoveredLength(const std::vector<ToolSegment> &,
                                                            const ToolAssembly &, double fromMm,
                                                            double toMm);
enum class SegmentPlanStatus { Complete, InsufficientStock, SearchLimit, InvalidInput, Cancelled };
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
                                           double maximumGapMm = 0, std::stop_token stop = {});
} // namespace bending::core
