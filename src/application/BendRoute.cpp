#include "bending/application/BendRoute.h"
#include "bending/application/SingleBendPreview.h"
#include "bending/core/ToolGeometry.h"
#include <algorithm>
#include <set>

namespace bending::application {
bool sameToolSetupExceptAssemblies(const core::ToolAssignment &a, const core::ToolAssignment &b) {
    // Compare in place: large immutable DXF contours must not be copied just to
    // remove the two small assembly fields.
    return a.punch == b.punch && a.die == b.die && a.groove == b.groove &&
           a.clearanceMm == b.clearanceMm && a.machineName == b.machineName &&
           a.useDrawingGeometry == b.useDrawingGeometry && a.machineModel == b.machineModel &&
           a.punchReversed == b.punchReversed && a.backgauge == b.backgauge;
}
bool validRouteAssemblySearch(const BendRouteRequest &request) {
    if (!request.assemblySearch)
        return true;
    const auto &s = *request.assemblySearch;
    if (!request.automatic || !request.withTools || s.bendIndices.empty() ||
        s.bendIndices.size() > maximumSheetBends || !std::isfinite(s.maximumGapMm) ||
        s.maximumGapMm < 0 || s.maximumGapMm > 1000)
        return false;
    std::set<std::size_t> seen;
    for (const auto index : s.bendIndices) {
        const auto op = std::find_if(request.operations.begin(), request.operations.end(),
                                     [&](const auto &entry) { return entry.bendIndex == index; });
        if (!seen.insert(index).second || op == request.operations.end() || !op->assignment ||
            op->assignment->punch.teaching || op->assignment->die.teaching ||
            !core::hasCompleteDrawingToolGeometry(*op->assignment) ||
            (s.allowDieWindows && !op->assignment->useDrawingGeometry))
            return false;
    }
    return true;
}
bool applyRouteAssemblyResult(BendRouteRequest &request, const BendRoute &route) {
    if (!validRouteAssemblySearch(request))
        return false;
    // Fixed-tool routes incur no extra DXF copies. Collect changes first;
    // copy the request only when every changed assignment has been validated.
    std::vector<std::pair<std::size_t, core::ToolAssignment>> updates;
    std::set<std::size_t> seen;
    const auto apply = [&](const std::string &id, std::size_t index,
                           const core::ToolAssignment *assignment,
                           const std::optional<core::PressTools> &tools) {
        const auto input =
            std::find_if(request.operations.begin(), request.operations.end(),
                         [&](const auto &op) { return op.id == id && op.bendIndex == index; });
        if (input == request.operations.end() || !seen.insert(index).second)
            return false;
        const bool unchanged = assignment ? input->assignment && *assignment == *input->assignment
                                          : !input->assignment;
        if (unchanged && tools == input->tools)
            return true;
        if (!request.assemblySearch || !input->assignment || !assignment ||
            std::find(request.assemblySearch->bendIndices.begin(),
                      request.assemblySearch->bendIndices.end(),
                      index) == request.assemblySearch->bendIndices.end() ||
            !sameToolSetupExceptAssemblies(*input->assignment, *assignment) ||
            !core::hasCompleteDrawingToolGeometry(*assignment) ||
            core::toolParameters(*assignment) != tools || !tools)
            return false;
        const auto valid = [&](const core::ToolPreset &profile,
                               const std::optional<core::ToolAssembly> &before,
                               const std::optional<core::ToolAssembly> &after) {
            return before == after ||
                   (profile.segments && after &&
                    (after->jointGapsMm.empty() || (profile.kind == core::ToolKind::Die &&
                                                    request.assemblySearch->allowDieWindows)) &&
                    core::maximumAssemblyGap(*after) <= request.assemblySearch->maximumGapMm &&
                    core::assemblyLength(*profile.segments, *after).has_value());
        };
        if (!valid(assignment->punch, input->assignment->punchAssembly,
                   assignment->punchAssembly) ||
            !valid(assignment->die, input->assignment->dieAssembly, assignment->dieAssembly))
            return false;
        updates.emplace_back(static_cast<std::size_t>(input - request.operations.begin()),
                             *assignment);
        return true;
    };
    for (const auto &step : route.steps) {
        const auto input =
            std::find_if(request.operations.begin(), request.operations.end(), [&](const auto &op) {
                return op.id == step.id && op.bendIndex == step.bendIndex;
            });
        if (input == request.operations.end())
            return false;
        const auto *assignment = step.assemblyAssignment ? step.assemblyAssignment.get()
                                 : input->assignment     ? &*input->assignment
                                                         : nullptr;
        const auto newWindow = [](const auto *after, const auto *before) {
            return after && after->dieAssembly && !after->dieAssembly->jointGapsMm.empty() &&
                   (!before || after->dieAssembly != before->dieAssembly);
        };
        bool changesWindow =
            newWindow(assignment, input->assignment ? &*input->assignment : nullptr);
        for (const auto &member : step.additionalBends) {
            const auto before = std::find_if(
                request.operations.begin(), request.operations.end(), [&](const auto &op) {
                    return op.id == member.id && op.bendIndex == member.bendIndex;
                });
            if (before == request.operations.end())
                return false;
            changesWindow |= newWindow(member.assignment ? &*member.assignment : nullptr,
                                       before->assignment ? &*before->assignment : nullptr);
        }
        if (changesWindow) {
            if (!assignment || std::any_of(step.additionalBends.begin(), step.additionalBends.end(),
                                           [&](const auto &member) {
                                               return !member.assignment ||
                                                      *member.assignment != *assignment;
                                           }))
                return false;
            if (!route.source || !step.preview || !assignment->useDrawingGeometry ||
                !step.preview->usesDrawingDie() || !step.preview->usesDrawingPunch())
                return false;
            const auto parameters = core::toolParameters(*assignment);
            if (!parameters || !step.preview->toolSetup() ||
                step.preview->toolSetup()->tools != *parameters)
                return false;
            const double tolerance = route.source->metadata().bendAnalysis.lengthToleranceMm;
            if (!validEndSupportIntervals(*step.preview,
                                          request.allowPartialEndSupport
                                              ? request.maximumEndSupportLossMm
                                              : std::optional<double>(0),
                                          tolerance))
                return false;
            const double origin = (parameters->lengthMm - step.preview->model().widthMm) / 2 +
                                  step.preview->workpieceOffsetMm();
            for (const auto &zone : step.preview->endSupportIntervals()) {
                const double start = origin + zone.fromMm;
                const auto uncovered = core::assemblyUncoveredLength(
                    *assignment->die.segments, *assignment->dieAssembly, start,
                    start + zone.material.bendLengthMm);
                if (!uncovered || *uncovered > tolerance)
                    return false;
            }
            const auto missing = step.preview->uncoveredToolLengthMm();
            if (!std::isfinite(missing[0]) || !std::isfinite(missing[1]) || missing[0] < 0 ||
                missing[1] < 0 || missing[0] > tolerance || missing[1] > tolerance)
                return false;
        }
        if (!apply(step.id, step.bendIndex, assignment,
                   step.assemblyAssignment ? core::toolParameters(*step.assemblyAssignment)
                                           : input->tools))
            return false;
        for (const auto &member : step.additionalBends)
            if (!apply(member.id, member.bendIndex,
                       member.assignment ? &*member.assignment : nullptr, member.tools))
                return false;
    }
    if (seen.size() != request.operations.size())
        return false;
    if (!updates.empty()) {
        auto resolved = request.operations;
        for (auto &[index, assignment] : updates) {
            resolved[index].tools = core::toolParameters(assignment);
            resolved[index].assignment = std::move(assignment);
        }
        request.operations = std::move(resolved);
    }
    return true;
}
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
