#include "bending/application/BendRoute.h"
#include "bending/core/ToolGeometry.h"
#include <algorithm>

namespace bending::application {
bool applyRouteToolVariant(BendRouteRequest &request, std::size_t index) {
    if (!request.withTools || !request.automatic || index >= request.toolVariants.size())
        return false;
    const auto &variant = request.toolVariants[index];
    const auto &a = variant.assignment;
    const auto parameters = core::toolParameters(a);
    if (!parameters || !core::validPressTools(*parameters) || a.punch.teaching || a.die.teaching ||
        !core::hasCompleteDrawingToolGeometry(a) ||
        (variant.bendIndex &&
         std::none_of(request.operations.begin(), request.operations.end(),
                      [&](const auto &op) { return op.bendIndex == *variant.bendIndex; })))
        return false;
    for (auto &op : request.operations)
        if (!variant.bendIndex || op.bendIndex == *variant.bendIndex) {
            op.assignment = a;
            op.tools = parameters;
        }
    return true;
}
} // namespace bending::application
