#include "bending/application/BendRoute.h"
#include "bending/application/SingleBendPreview.h"
#include "bending/application/WorkpiecePlacement.h"
#include "bending/core/ToolGeometry.h"
#include <algorithm>
#include <map>
#include <set>

namespace bending::application {
namespace {
struct StrokeAxis {
    PointMm origin;
    PointMm direction;
    double length;
    explicit StrokeAxis(const RecognizedBend &bend)
        : origin(bend.axisStart), direction{bend.axisEnd.x - origin.x, bend.axisEnd.y - origin.y,
                                            bend.axisEnd.z - origin.z},
          length(std::hypot(direction.x, direction.y, direction.z)) {}
    double project(PointMm p) const {
        return ((p.x - origin.x) * direction.x + (p.y - origin.y) * direction.y +
                (p.z - origin.z) * direction.z) /
               length;
    }
    double distance(PointMm p, double along) const {
        const auto t = along / length;
        return std::hypot(p.x - origin.x - t * direction.x, p.y - origin.y - t * direction.y,
                          p.z - origin.z - t * direction.z);
    }
};
bool compatibleStrokeZones(const BendAnalysis &analysis, std::size_t first, std::size_t second) {
    const auto &a = analysis.bends[first];
    const auto &b = analysis.bends[second];
    const auto tolerance = analysis.lengthToleranceMm;
    const StrokeAxis axis(a);
    if (!(axis.length > tolerance) ||
        std::abs(a.angleRadians - b.angleRadians) > analysis.angleToleranceRadians ||
        std::abs(a.innerRadiusMm - b.innerRadiusMm) > tolerance ||
        std::abs(a.thicknessMm - b.thicknessMm) > tolerance)
        return false;
    const auto from = axis.project(b.axisStart), to = axis.project(b.axisEnd);
    return std::abs(to - from) > tolerance && axis.distance(b.axisStart, from) <= tolerance &&
           axis.distance(b.axisEnd, to) <= tolerance &&
           (std::max(from, to) < -tolerance || std::min(from, to) > axis.length + tolerance);
}
} // namespace
std::vector<BendStroke> routeBendStrokes(const BendAnalysis &analysis,
                                         std::span<const std::size_t> order) {
    using Walls = std::array<std::array<std::size_t, 2>, 2>;
    std::map<Walls, std::size_t> strokesByWallPair;
    std::vector<BendStroke> strokes;
    for (const auto index : order) {
        auto position = strokes.size();
        if (index < analysis.bends.size()) {
            const auto &faces = analysis.bends[index].flangeFaces;
            Walls walls{{{faces[0], faces[1]}, {faces[2], faces[3]}}};
            for (auto &wall : walls)
                std::sort(wall.begin(), wall.end());
            std::sort(walls.begin(), walls.end());
            if (walls[0][0] && walls[0][0] != walls[0][1] && walls[1][0] != walls[1][1] &&
                walls[0] != walls[1]) {
                const auto [found, inserted] = strokesByWallPair.try_emplace(walls, position);
                if (!inserted && std::all_of(strokes[found->second].begin(),
                                             strokes[found->second].end(), [&](auto member) {
                                                 return compatibleStrokeZones(analysis, member,
                                                                              index);
                                             }))
                    position = found->second;
            }
        }
        if (position == strokes.size())
            strokes.emplace_back();
        strokes[position].push_back(index);
    }
    return strokes;
}
std::vector<BendStroke> routeDraftStrokes(const RouteDraftRequest &request,
                                          const BendAnalysis &analysis) {
    std::vector<std::size_t> order;
    order.reserve(request.operations.size());
    for (const auto &op : request.operations)
        order.push_back(op.bendIndex);
    auto strokes = routeBendStrokes(analysis, order);
    // Draft consumers refer to positions in the request, which can be reordered.
    std::vector<std::size_t> positions(analysis.bends.size(), order.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i] < positions.size())
            positions[order[i]] = i;
    for (auto &stroke : strokes)
        for (auto &index : stroke)
            index = index < positions.size() ? positions[index] : order.size();
    return strokes;
}
double routeStrokeLengthMm(const BendAnalysis &analysis, const BendStroke &stroke) {
    const auto &first = analysis.bends.at(stroke.front());
    if (stroke.size() == 1)
        return first.lengthMm;
    const StrokeAxis axis(first);
    double low = 0, high = axis.length;
    for (const auto index : stroke)
        for (const auto p :
             {analysis.bends.at(index).axisStart, analysis.bends.at(index).axisEnd}) {
            const auto along = axis.project(p);
            low = std::min(low, along);
            high = std::max(high, along);
        }
    return high - low;
}
std::optional<BendEndSupport> compactDraftCoverage(const RouteDraftRequest &request,
                                                   const BendAnalysis &analysis) {
    if (!request.withTools || !request.step || request.step > request.operations.size() ||
        !allowsShortToolEndSupport(request.allowPartialEndSupport, request.maximumEndSupportLossMm))
        return {};
    const auto strokes = routeDraftStrokes(request, analysis);
    if (request.step > strokes.size() || strokes[request.step - 1].size() != 1)
        return {};
    const auto &op = request.operations[strokes[request.step - 1].front()];
    if (op.bendIndex >= analysis.bends.size() || !op.tools || !core::validPressTools(*op.tools) ||
        !op.assignment || !continuousDrawingTools(*op.assignment) ||
        !validWorkpieceOffset(op.workpieceOffsetMm))
        return {};
    const auto &bend = analysis.bends[op.bendIndex];
    const double tolerance = analysis.lengthToleranceMm;
    auto interval = trimmedAxialCore(bend);
    if (!interval ||
        BendRouteToolLengthIssue{bend.lengthMm, core::punchLengthMm(*op.tools), op.tools->lengthMm}
            .coversBend(tolerance))
        return {};
    if (bendAxisNeedsReversal(bend, tolerance) != op.workpieceReversed)
        *interval = {bend.lengthMm - (*interval)[1], bend.lengthMm - (*interval)[0]};
    const auto span = commonToolSpan(*op.tools, bend.lengthMm, op.workpieceOffsetMm);
    const BendEndSupport covered{bend.lengthMm, std::max(0., span[0]),
                                 std::min(bend.lengthMm, span[1])};
    if (coversAxialInterval(span, *interval, tolerance) &&
        withinEndSupportLimit(covered, request.maximumEndSupportLossMm.value_or(bend.lengthMm),
                              tolerance))
        return covered;
    return {};
}
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
        s.maximumGapMm < 0 || s.maximumGapMm > 1000 ||
        (s.compactOnly && (s.maximumGapMm != 0 || s.allowDieWindows)))
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
        if (request.assemblySearch->compactOnly &&
            (!route.source || !continuousDrawingTools(*assignment) ||
             index >= route.source->metadata().bendAnalysis.bends.size() ||
             !trimmedAxialCore(route.source->metadata().bendAnalysis.bends[index]) ||
             std::min(tools->lengthMm, core::punchLengthMm(*tools)) +
                     route.source->metadata().bendAnalysis.lengthToleranceMm >=
                 route.source->metadata().bendAnalysis.bends[index].lengthMm))
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
