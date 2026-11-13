#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bending::application {
struct BendAnalysis;
struct FlatBend;
} // namespace bending::application

namespace bending::cad {
struct SheetLayout;

struct ReverseTabHint {
    std::size_t bend;
    std::uint64_t neighbours; // The two zones bordering this gap.
};

// Find reverse bends on the same wall, axially between compatible stroke zones.
[[nodiscard]] std::vector<ReverseTabHint> reverseTabHints(const SheetLayout &,
                                                          const application::BendAnalysis &,
                                                          std::span<const application::FlatBend>);

struct RouteSheetFeatures {
    // All values are indexed by the original bend index.
    std::vector<std::uint64_t> neighbours;
    std::vector<std::size_t> profileRank; // Empty outside a ranked parallel chain.
    std::vector<int> flangeDepth;         // Empty outside a ranked branched sheet.
};

[[nodiscard]] RouteSheetFeatures analyseRouteSheet(const SheetLayout &layout, std::size_t bendCount,
                                                   double angleToleranceRadians, bool rankProfile,
                                                   bool rankFlanges);
} // namespace bending::cad
