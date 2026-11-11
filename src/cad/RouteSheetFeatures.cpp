#include "RouteSheetFeatures.h"
#include "bending/application/BendRoute.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <algorithm>
#include <optional>

namespace bending::cad {
namespace {
struct WallLink {
    std::size_t wall, bend;
};
using WallGraph = std::vector<std::vector<WallLink>>;

std::vector<int> flangeDepth(const WallGraph &graph, const SheetLayout &layout,
                             std::size_t bendCount) {
    std::vector<int> depth(graph.size(), -1);
    std::vector<std::size_t> queue;
    for (std::size_t wall = 0; wall < graph.size(); ++wall)
        if (graph[wall].size() == 1) {
            depth[wall] = 0;
            queue.push_back(wall);
        }
    for (std::size_t i = 0; i < queue.size(); ++i)
        for (const auto &link : graph[queue[i]])
            if (depth[link.wall] < 0) {
                depth[link.wall] = depth[queue[i]] + 1;
                queue.push_back(link.wall);
            }

    std::vector<int> result(bendCount);
    for (const auto &bend : layout.bends)
        result[bend.bendIndex] = std::max(0, std::min(depth[bend.parent], depth[bend.child]));
    return result;
}

std::vector<std::size_t> profileRank(const WallGraph &graph, std::size_t bendCount) {
    const auto end = std::find_if(graph.begin(), graph.end(),
                                  [](const auto &links) { return links.size() == 1; });
    if (end == graph.end())
        return {};
    // Start at the first leaf in layout order, as in the original search.
    auto wall = static_cast<std::size_t>(end - graph.begin());
    std::optional<std::size_t> previous;
    std::vector<std::size_t> result(bendCount);
    for (std::size_t rank = 0; rank < bendCount; ++rank) {
        const auto &links = graph[wall];
        const auto link = std::find_if(links.begin(), links.end(),
                                       [&](const auto &edge) { return edge.wall != previous; });
        if (link == links.end())
            throw application::BendRouteError{application::BendRouteErrorCode::Unsupported,
                                              "Incomplete profile chain"};
        result[link->bend] = rank;
        previous = wall;
        wall = link->wall;
    }
    return result;
}
} // namespace

RouteSheetFeatures analyseRouteSheet(const SheetLayout &layout, std::size_t bendCount,
                                     double angleToleranceRadians, bool rankProfile,
                                     bool rankFlanges) {
    RouteSheetFeatures result;
    result.neighbours.resize(bendCount);
    for (const auto &a : layout.bends)
        for (const auto &b : layout.bends)
            if (a.bendIndex != b.bendIndex && (a.parent == b.parent || a.parent == b.child ||
                                               a.child == b.parent || a.child == b.child))
                result.neighbours[a.bendIndex] |= std::uint64_t{1} << b.bendIndex;

    const bool parallelChain =
        rankProfile && bendCount > 2 &&
        std::all_of(layout.bends.begin(), layout.bends.end(), [&](const auto &bend) {
            return bend.sectionAxes.Direction().IsParallel(
                layout.bends.front().sectionAxes.Direction(), angleToleranceRadians);
        });
    if (!rankFlanges && !parallelChain)
        return result;

    WallGraph graph(layout.walls.size());
    for (const auto &bend : layout.bends) {
        graph[bend.parent].push_back({bend.child, bend.bendIndex});
        graph[bend.child].push_back({bend.parent, bend.bendIndex});
    }
    const bool branched =
        std::any_of(graph.begin(), graph.end(), [](const auto &links) { return links.size() > 2; });
    if (rankFlanges && branched)
        result.flangeDepth = flangeDepth(graph, layout, bendCount);
    if (parallelChain && !branched)
        result.profileRank = profileRank(graph, bendCount);
    return result;
}
} // namespace bending::cad
