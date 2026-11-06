#pragma once
#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace bending::core {
enum class SegmentShape { Straight, LeftHorn, RightHorn };
// A through relief in the longitudinal plane, measured from the working tip.
struct HornRelief {
    double projectionMm = 0;
    double radiusMm = 0;
    double tipHeightMm = 0;
    bool operator==(const HornRelief &) const = default;
};
[[nodiscard]] bool validHornRelief(const HornRelief &, double sectionLengthMm);
[[nodiscard]] double hornRootHeight(const HornRelief &);
struct ToolSegment {
    double lengthMm = 0;
    unsigned quantity = 0;
    SegmentShape shape = SegmentShape::Straight;
    std::optional<HornRelief> horn = {};
    std::string variantId = {}; // Configured horn identity within this profile snapshot.
    bool operator==(const ToolSegment &) const = default;
};
[[nodiscard]] bool usableToolSegment(const ToolSegment &);
[[nodiscard]] bool hasHornGeometry(const std::vector<ToolSegment> &);
struct ToolAssembly {
    std::vector<std::size_t> pieces; // Stock row indices, in installation order.
    double gapMm = 0;                // Uniform spacing when jointGapsMm is empty.
    // Otherwise exactly pieces.size()-1 gaps, in installation order; gapMm must be zero.
    std::vector<double> jointGapsMm = {};
    bool operator==(const ToolAssembly &) const = default;
};
[[nodiscard]] bool usesHornSections(const std::vector<ToolSegment> &,
                                    const std::optional<ToolAssembly> &);
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
// One forbidden axial interval inside a material-free corridor, in assembly coordinates.
// The outer span is retained; straight sections on both sides share the same stock.
struct ToolWindow {
    double lengthMm, freeFromMm, conflictFromMm, conflictToMm, freeToMm;
    bool operator==(const ToolWindow &) const = default;
};
[[nodiscard]] SegmentPlan planToolWindow(const std::vector<ToolSegment> &, const ToolWindow &,
                                         double maximumGapMm, std::size_t workBudget = 200000,
                                         std::stop_token stop = {});
[[nodiscard]] bool validToolSegments(const std::vector<ToolSegment> &);
[[nodiscard]] std::optional<double> assemblyLength(const std::vector<ToolSegment> &,
                                                   const ToolAssembly &);
// Exact to 0.001 mm. Prefer gap-free stock: minimize excess length, then pieces.
// Only when unavailable, try gaps up to maximumGapMm; minimize excess, gap, then pieces.
// Straight-only reference; die/window planning retains this exact behaviour.
[[nodiscard]] SegmentPlan planToolSegments(const std::vector<ToolSegment> &, double requiredMm,
                                           std::size_t stateBudget = 200000,
                                           double maximumGapMm = 0, std::stop_token stop = {});
struct PunchAssemblyPlans {
    SegmentPlanStatus status = SegmentPlanStatus::InvalidInput;
    std::vector<SegmentPlan> plans; // Valid numeric assemblies, not collision certificates.
    std::size_t work = 0;
    [[nodiscard]] SegmentPlan best() const {
        return plans.empty() ? SegmentPlan{status, {}, 0} : plans.front();
    }
};
// Straight, left, right and paired ends; one shared middle table and work budget.
// SearchLimit may retain valid plans. Unknown end geometry is never used.
[[nodiscard]] PunchAssemblyPlans
planPunchAssemblies(const std::vector<ToolSegment> &, double requiredMm,
                    std::size_t workBudget = 200000, double maximumGapMm = 0,
                    std::stop_token stop = {}, std::size_t maximumPlans = 8);
} // namespace bending::core
