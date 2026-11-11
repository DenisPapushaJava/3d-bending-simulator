#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bending::cad {
struct SheetLayout;

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
