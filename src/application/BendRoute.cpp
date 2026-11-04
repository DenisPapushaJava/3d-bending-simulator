#include "bending/application/BendRoute.h"
#include "bending/core/ToolGeometry.h"
#include <algorithm>
#include <set>

namespace bending::application {
bool applyRouteToolVariant(BendRouteRequest &request, std::size_t index) {
    if (!request.withTools || !request.automatic || index >= request.toolVariants.size())
        return false;
    const auto &variant = request.toolVariants[index];
    const auto &a = variant.assignment;
    const auto parameters = core::toolParameters(a);
    if (!parameters || !core::validPressTools(*parameters) || a.punch.teaching || a.die.teaching ||
        !core::hasCompleteDrawingToolGeometry(a) ||
        (!variant.bendIndex && !variant.additionalBendIndices.empty()) ||
        (variant.bendIndex &&
         std::none_of(request.operations.begin(), request.operations.end(),
                      [&](const auto &op) { return op.bendIndex == *variant.bendIndex; })))
        return false;
    std::set<std::size_t> targets;
    if (variant.bendIndex)
        targets.insert(*variant.bendIndex);
    for (const auto member : variant.additionalBendIndices)
        if (!targets.insert(member).second ||
            std::none_of(request.operations.begin(), request.operations.end(),
                         [&](const auto &op) { return op.bendIndex == member; }))
            return false;
    for (auto &op : request.operations)
        if (routeToolVariantApplies(variant, op.bendIndex)) {
            op.assignment = a;
            op.tools = parameters;
        }
    return true;
}
} // namespace bending::application
