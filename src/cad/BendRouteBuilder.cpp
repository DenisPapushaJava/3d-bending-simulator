#include "Backgauge.h"
#include "BendOperation.h"
#include "CadPreview.h"
#include "FlatPatternDisplay.h"
#include "GeometryCacheInternal.h"
#include "RouteDraftGeometry.h"
#include "RouteGeometry.h"
#include "SheetOrientation.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/core/ToolSelection.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <list>
#include <map>
#include <mutex>
#include <numbers>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace bending::cad {
namespace {
using namespace application;
using Mask = std::uint64_t;
static_assert(maximumSheetBends < 64, "Route masks need a wider representation beyond 63 bends");
gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
void check(bool ok, const char *detail) {
    if (!ok)
        throw BendRouteError{BendRouteErrorCode::Unsupported, detail};
}
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw BendRouteError{BendRouteErrorCode::Cancelled, "Cancelled"};
}
template <class Op>
TopoDS_Shape boolean(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                     std::stop_token stop) {
    cancelled(stop);
    Op op;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(a);
    tools.Append(b);
    op.SetArguments(arguments);
    op.SetTools(tools);
    op.SetNonDestructive(true);
    op.SetRunParallel(false);
    op.SetFuzzyValue(tolerance);
    op.Build();
    cancelled(stop);
    if (!op.IsDone() || op.HasErrors() || !BRepCheck_Analyzer(op.Shape()).IsValid())
        throw BendRouteError{BendRouteErrorCode::Failed, "Route geometry Boolean failed"};
    return op.Shape();
}
TopoDS_Shape moved(const TopoDS_Shape &s, const gp_Trsf &map) {
    return BRepBuilderAPI_Transform(s, map, true).Shape();
}
TopoDS_Shape fused(const std::vector<TopoDS_Shape> &solids, double tolerance,
                   std::stop_token stop) {
    cancelled(stop);
    check(!solids.empty(), "No material regions");
    if (solids.size() == 1)
        return solids.front();
    TopTools_ListOfShape arguments, tools;
    arguments.Append(solids.front());
    for (std::size_t i = 1; i < solids.size(); ++i)
        tools.Append(solids[i]);
    BRepAlgoAPI_Fuse op;
    op.SetArguments(arguments);
    op.SetTools(tools);
    op.SetNonDestructive(true);
    op.SetRunParallel(false);
    op.SetFuzzyValue(tolerance);
    op.Build();
    cancelled(stop);
    if (!op.IsDone() || op.HasErrors() || !BRepCheck_Analyzer(op.Shape()).IsValid())
        throw BendRouteError{BendRouteErrorCode::Failed, "Route material union failed"};
    return op.Shape();
}
struct State {
    std::shared_ptr<const CadDocument> document;
    std::vector<std::optional<std::size_t>> indices;
};
struct TransitionInputs {
    RouteOperation operation;
    bool withTools, partialEndSupport;
    std::optional<double> maximumEndSupportLossMm;
    bool operator==(const TransitionInputs &) const = default;
};
struct TransitionKey {
    Mask mask;
    std::shared_ptr<const TransitionInputs> inputs;
    bool fixedSecond;
    bool operator==(const TransitionKey &) const = default;
};
struct TransitionHash {
    std::size_t operator()(const TransitionKey &key) const noexcept {
        auto hash = std::hash<Mask>{}(key.mask);
        hash ^= std::hash<std::shared_ptr<const TransitionInputs>>{}(key.inputs) + 0x9e3779b9u +
                (hash << 6) + (hash >> 2);
        return hash ^
               (std::hash<bool>{}(key.fixedSecond) + 0x9e3779b9u + (hash << 6) + (hash >> 2));
    }
};
// Stable list nodes provide LRU eviction; the hash index never scans CAD/profile data.
template <class Value, std::size_t Capacity> class TransitionLru {
    using Entries = std::list<std::pair<TransitionKey, Value>>;
    Entries entries;
    std::unordered_map<TransitionKey, typename Entries::iterator, TransitionHash> index;
    std::size_t limit = Capacity;

    void trim() {
        while (entries.size() > limit) {
            index.erase(entries.back().first);
            entries.pop_back();
        }
    }

  public:
    TransitionLru() = default;
    TransitionLru(const TransitionLru &) = delete;
    TransitionLru &operator=(const TransitionLru &) = delete;
    void setLimit(std::size_t value) {
        limit = std::clamp(value, std::size_t{1}, Capacity);
        trim();
    }
    const Value *peek(const TransitionKey &key) const {
        const auto found = index.find(key);
        return found == index.end() ? nullptr : &found->second->second;
    }
    const Value *find(const TransitionKey &key) {
        const auto found = index.find(key);
        if (found == index.end())
            return nullptr;
        entries.splice(entries.begin(), entries, found->second);
        return &found->second->second;
    }
    void put(TransitionKey key, Value value) {
        if (const auto found = index.find(key); found != index.end()) {
            found->second->second = std::move(value);
            entries.splice(entries.begin(), entries, found->second);
            return;
        }
        entries.emplace_front(std::move(key), std::move(value));
        try {
            index.emplace(entries.front().first, entries.begin());
        } catch (...) {
            entries.pop_front();
            throw;
        }
        trim();
    }
};
struct CacheData {
    DocumentPtr source;
    std::optional<double> thickness;
    FlatPatternPtr flat;
    std::shared_ptr<const RouteDraftBasis> draftBasis;
    std::list<RouteDraftPtr> drafts;
    std::map<Mask, std::shared_ptr<const State>> states;
    std::list<Mask> stateOrder;
    // Exact inputs are compared once per operation/request and shared by all its state keys.
    std::vector<std::weak_ptr<const TransitionInputs>> inputs;
    TransitionLru<BendPreviewPtr, maximumSheetBends> transitions;
    // Null rejection is a success hint. A positive verdict alone cannot publish a cycle.
    TransitionLru<std::optional<BendPreviewError>, 512> verdicts;
    std::mutex mutex;

    std::shared_ptr<const TransitionInputs> intern(TransitionInputs wanted) {
        for (auto it = inputs.begin(); it != inputs.end();) {
            if (auto value = it->lock()) {
                if (*value == wanted)
                    return value;
                ++it;
            } else {
                it = inputs.erase(it);
            }
        }
        auto value = std::make_shared<const TransitionInputs>(std::move(wanted));
        inputs.push_back(value);
        return value;
    }
};
bool stableRejection(BendPreviewErrorCode code) {
    switch (code) {
    case BendPreviewErrorCode::ToolIntersection:
    case BendPreviewErrorCode::MachineIntersection:
    case BendPreviewErrorCode::SheetIntersection:
    case BendPreviewErrorCode::MissingLeftSupport:
    case BendPreviewErrorCode::MissingRightSupport:
    case BendPreviewErrorCode::EndSupportLimitExceeded:
    case BendPreviewErrorCode::SupportNotProven:
    case BendPreviewErrorCode::ToolAngleConflict:
    case BendPreviewErrorCode::NoseTooLarge:
    case BendPreviewErrorCode::NarrowDieOpening:
    case BendPreviewErrorCode::ContactLost:
    case BendPreviewErrorCode::RetractionNotProven:
    case BendPreviewErrorCode::UnsupportedToolGeometry:
    // Memoizes this bounded solver's exact-input answer, not manufacturing impossibility.
    case BendPreviewErrorCode::BackgaugeUnavailable:
        return true;
    default:
        return false;
    }
}
class RouteSearch {
  public:
    DocumentPtr source;
    std::shared_ptr<const CadDocument> native;
    FlatPatternPtr flat;
    BendRouteRequest request;
    std::stop_token stop;
    const SheetLayout *layout = nullptr;
    double tolerance = 0, expectedVolume = 0, budget = 0;
    CacheData *cache = nullptr;
    std::vector<std::shared_ptr<const TransitionInputs>> inputs;
    BendRouteStatistics statistics;
    std::vector<Mask> neighbours;
    std::vector<std::size_t> profileRank;
    std::vector<gp_Dir> referenceAxes;
    std::vector<Bnd_Box> wallBounds;
    std::vector<double> wallVolumes;
    std::map<std::pair<Mask, std::size_t>, bool> frontPreferences;
    bool preferredSide(Mask mask, const RouteOperation &op) {
        if (!request.withTools || !request.allowWorkpieceRotation || !op.tools)
            return op.workpieceReversed;
        const auto key = std::pair{mask, op.bendIndex};
        if (const auto found = frontPreferences.find(key); found != frontPreferences.end())
            return found->second;
        const auto link = std::find_if(layout->bends.begin(), layout->bends.end(),
                                       [&](const auto &b) { return b.bendIndex == op.bendIndex; });
        if (link == layout->bends.end())
            return op.workpieceReversed;
        std::vector<double> progress(request.operations.size());
        for (std::size_t i = 0; i < progress.size(); ++i)
            progress[i] = i != op.bendIndex && (mask & (Mask{1} << i)) ? 1. : 0.;
        const auto positions = routePlacement(*layout, progress);
        const auto &b = source->metadata().bendAnalysis.bends[op.bendIndex];
        const auto state = simulation::draftPressState(
            {b.thicknessMm, b.innerRadiusMm, b.angleRadians, b.lengthMm, 1, 1}, *op.tools, 0);
        if (!state)
            return op.workpieceReversed;
        const auto a = state->section.fixed[0], end = state->section.fixed[1];
        gp_Trsf machine;
        machine.SetDisplacement(link->sectionAxes.Transformed(positions.walls[link->parent]),
                                gp_Ax3(gp_Pnt(a.x, a.y, (op.tools->lengthMm - b.lengthMm) / 2),
                                       gp_Dir(0, 0, 1), gp_Dir(a.x - end.x, a.y - end.y, 0)));
        if (referenceAxes[op.bendIndex].Dot(link->sectionAxes.Direction()) < 0) {
            gp_Trsf turn;
            turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, op.tools->lengthMm / 2), gp_Dir(0, 1, 0)),
                             std::numbers::pi);
            machine = turn * machine;
        }
        Bnd_Box box;
        double frontMaterial = 0, backMaterial = 0;
        bool materialKnown = true;
        for (std::size_t i = 0; i < wallBounds.size(); ++i) {
            const auto bounds = wallBounds[i].Transformed(machine * positions.walls[i]);
            box.Add(bounds);
            // Whole walls strictly on one side give a cheap material-amount
            // preference. A crossing/unknown wall falls back to the old overhang
            // hint; no Boolean or mesh is built for this heuristic.
            if (bounds.IsVoid() || bounds.IsOpen() || !std::isfinite(wallVolumes[i]) ||
                wallVolumes[i] <= 0)
                materialKnown = false;
            else if (bounds.CornerMax().X() <= 0)
                frontMaterial += wallVolumes[i];
            else if (bounds.CornerMin().X() >= 0)
                backMaterial += wallVolumes[i];
            else
                materialKnown = false;
        }
        bool preferred = op.workpieceReversed;
        if (materialKnown &&
            std::abs(frontMaterial - backMaterial) > .01 * (frontMaterial + backMaterial))
            preferred = backMaterial > frontMaterial;
        else if (!box.IsVoid()) {
            const double front = -box.CornerMin().X(), back = box.CornerMax().X();
            if (std::abs(front - back) > std::max(.5, b.thicknessMm))
                preferred = back > front;
        }
        frontPreferences.emplace(key, preferred);
        return preferred; // Soft handling preference only; the same exact checker follows.
    }
    std::set<Mask> dead;
    std::vector<BendRouteStep> reversed;
    std::size_t attempts = 0;
    std::string lastFailure;
    std::optional<std::size_t> failedBend;
    std::optional<BendPreviewErrorCode> lastOperationError;
    bool incomplete = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    void touchState(Mask mask) {
        cache->stateOrder.remove(mask);
        cache->stateOrder.push_front(mask);
    }
    std::shared_ptr<const State> state(Mask mask) {
        if (auto found = cache->states.find(mask); found != cache->states.end()) {
            ++statistics.reusedStates;
            touchState(mask);
            return found->second;
        }
        cancelled(stop);
        if (!request.reuseCalculations && cache->states.size() >= 128)
            throw BendRouteError{BendRouteErrorCode::SearchLimit,
                                 "Intermediate state cache limit reached",
                                 {},
                                 attempts};
        std::vector<double> progress(request.operations.size());
        for (std::size_t i = 0; i < progress.size(); ++i)
            progress[i] = (mask & (Mask{1} << i)) ? 1. : 0.;
        const auto placements = routePlacement(*layout, progress);
        const auto &maps = placements.walls;
        std::vector<TopoDS_Shape> solids;
        std::vector<FlatDisplayRegion> display;
        std::vector<gp_Trsf> bendMaps(request.operations.size());
        for (const auto &b : layout->bends) {
            const auto parentInverse = layout->walls[b.parent].toFlat.Inverted();
            const auto flatMap = maps[b.parent] * parentInverse;
            bendMaps[b.bendIndex] = maps[b.parent];
            const bool formed = (mask & (Mask{1} << b.bendIndex)) != 0;
            solids.push_back(
                moved(formed ? b.solid : b.flatSolid, formed ? maps[b.parent] : flatMap));
            display.push_back({b.solid, placements.bends[b.bendIndex]});
        }
        for (std::size_t i = 0; i < layout->walls.size(); ++i) {
            solids.push_back(moved(layout->walls[i].solid, maps[i]));
            display.push_back(
                {layout->walls[i].solid, [map = maps[i]](gp_Pnt p) { return p.Transformed(map); }});
        }
        auto geometry = fused(solids, tolerance, stop);
        TopTools_IndexedMapOfShape components;
        TopExp::MapShapes(geometry, TopAbs_SOLID, components);
        check(components.Extent() == 1 && std::abs(volume(geometry) - expectedVolume) <= budget,
              "Intermediate walls overlap or disconnect");
        TopoDS_Shape skin;
        std::vector<SheetSide> skinSides;
        const auto &orientation = OcctDocumentAccess::sheetOrientation(*native);
        if (native->metadata().closedCutouts.status == ClosedCutoutStatus::Filled) {
            auto visible = flatCutoutDisplay(OcctDocumentAccess::shape(*native), display, {},
                                             tolerance, stop, &orientation);
            skin = std::move(visible.surface);
            skinSides = std::move(visible.faceSides);
        }
        std::optional<SheetReference> reference;
        if (orientation.available) {
            std::vector<TopoDS_Shape> walls;
            for (const auto &w : layout->walls)
                walls.push_back(w.solid);
            if (const auto at = sheetReferenceRegion(orientation.reference, walls, tolerance))
                reference = movedSheetReference(orientation.reference, maps[*at]);
        }
        auto doc = OcctDocumentAccess::routeState(*native, geometry, skin, stop, reference,
                                                  std::move(skinSides));
        const auto &recognized = OcctDocumentAccess::simulationBends(*doc);
        check(recognized.bends.size() == static_cast<std::size_t>(std::popcount(mask)) &&
                  recognized.interiorContourDiagnostics.empty(),
              "Cannot recover all remaining bend zones in the intermediate shape");
        State result{doc, std::vector<std::optional<std::size_t>>(request.operations.size())};
        const auto &original = OcctDocumentAccess::simulationBends(*native).bends;
        std::set<std::size_t> assigned;
        for (std::size_t i = 0; i < original.size(); ++i) {
            if (!(mask & (Mask{1} << i)))
                continue;
            const auto a = point(original[i].axisStart).Transformed(bendMaps[i]);
            const auto b = point(original[i].axisEnd).Transformed(bendMaps[i]);
            for (std::size_t j = 0; j < recognized.bends.size(); ++j) {
                const auto &next = recognized.bends[j];
                const auto c = point(next.axisStart), d = point(next.axisEnd);
                if (std::min(std::max(a.Distance(c), b.Distance(d)),
                             std::max(a.Distance(d), b.Distance(c))) > tolerance * 30 ||
                    std::abs(next.innerRadiusMm - original[i].innerRadiusMm) > tolerance * 10 ||
                    std::abs(next.angleRadians - original[i].angleRadians) > 1.e-6)
                    continue;
                check(!result.indices[i] && !assigned.contains(j),
                      "Ambiguous intermediate bend binding");
                result.indices[i] = j;
                assigned.insert(j);
            }
            check(result.indices[i].has_value(), "An intermediate bend lost its stable binding");
        }
        auto value = std::make_shared<const State>(std::move(result));
        cache->states.emplace(mask, value);
        ++statistics.builtStates;
        touchState(mask);
        // Shared local handles keep an active before/after state alive during eviction.
        while (request.reuseCalculations && cache->states.size() > 32) {
            const auto oldest = cache->stateOrder.back();
            cache->stateOrder.pop_back();
            if (oldest == (Mask{1} << request.operations.size()) - 1) {
                cache->stateOrder.push_front(oldest);
                continue;
            }
            cache->states.erase(oldest);
        }
        return value;
    }

    TransitionKey key(Mask mask, const RouteOperation &op, bool side) const {
        return {mask, inputs[op.bendIndex], side};
    }
    // 0 = verified success, 1 = unknown, 2 = stable rejection of this exact transition.
    int priorResult(Mask mask, const RouteOperation &op, bool side) const {
        if (!request.reuseCalculations)
            return 1;
        if (const auto *verdict = cache->verdicts.peek(key(mask, op, side)))
            return verdict->has_value() ? 2 : 0;
        return 1;
    }
    void remember(TransitionKey key, BendPreviewResult result) {
        if (!request.reuseCalculations)
            return;
        cancelled(stop);
        if (const auto *error = std::get_if<BendPreviewError>(&result)) {
            // Route search consumes the report, not the failed cycle. Never retain its
            // diagnostic BReps/meshes here; single/manual previews have their own cache.
            cache->verdicts.put(
                std::move(key),
                BendPreviewError{
                    error->code, error->detail, error->frameIndex, {}, error->endSupport});
        } else {
            cache->transitions.put(key, std::get<BendPreviewPtr>(std::move(result)));
            cache->verdicts.put(std::move(key), std::nullopt);
        }
    }
    BendPreviewResult transition(Mask mask, const RouteOperation &op, bool fixedSecond) {
        const auto wanted = key(mask, op, fixedSecond);
        if (request.reuseCalculations) {
            if (const auto *preview = cache->transitions.find(wanted)) {
                ++statistics.reusedTransitions;
                return *preview;
            }
            if (const auto *verdict = cache->verdicts.find(wanted); verdict && *verdict) {
                ++statistics.reusedTransitions;
                ++statistics.reusedRejections;
                if ((**verdict).code == BendPreviewErrorCode::BackgaugeUnavailable)
                    ++statistics.backgauge.reusedNoReferenceTransitions;
                return **verdict;
            }
        }
        ++statistics.builtTransitions;
        const auto after = state(mask);
        const auto link = std::find_if(layout->bends.begin(), layout->bends.end(),
                                       [&](const auto &b) { return b.bendIndex == op.bendIndex; });
        check(link != layout->bends.end(), "Missing workpiece placement reference");
        std::vector<double> progress(request.operations.size());
        for (std::size_t i = 0; i < progress.size(); ++i)
            progress[i] = (mask & (Mask{1} << i)) ? 1. : 0.;
        const auto placements = routePlacement(*layout, progress);
        const auto reference =
            referenceAxes[op.bendIndex].Transformed(placements.walls[link->parent]);
        auto preview = buildBendOperation(
            after->document, *after->indices[op.bendIndex], op.target, fixedSecond, stop,
            request.withTools ? op.tools : std::nullopt, request.withTools,
            request.withTools ? op.assignment : std::nullopt, request.allowPartialEndSupport,
            request.maximumEndSupportLossMm, reference, false, &statistics.backgauge);
        if (const auto *failure = std::get_if<BendPreviewError>(&preview)) {
            if (failure->code == BendPreviewErrorCode::BackgaugeUnavailable)
                ++statistics.backgauge.noReferenceTransitions;
            if (stableRejection(failure->code))
                remember(wanted, preview);
            return preview;
        }
        const auto p = std::get<BendPreviewPtr>(preview);
        // A rejected cycle never needs a canonical predecessor. Successful cycles still
        // undergo both independent volume differences against the complete before state.
        const auto before = state(mask ^ (Mask{1} << op.bendIndex));
        // The first state must agree with the independently assembled
        // predecessor up to the same placement of the reference wall.
        const auto &components = OcctPreviewAccess::sourceComponents(*p);
        std::optional<std::size_t> rootSide;
        for (std::size_t side = 0; side < 2; ++side) {
            const auto overlap = volume(boolean<BRepAlgoAPI_Common>(
                layout->walls[0].solid, components[side], tolerance, stop));
            const double rootVolume = volume(layout->walls[0].solid);
            if (std::abs(overlap - rootVolume) <= std::max(1.e-6, rootVolume * 1.e-7)) {
                check(!rootSide, "Reference wall crosses both material components");
                rootSide = side;
            }
        }
        check(rootSide.has_value(), "Reference wall lost from the operation");
        const auto &first = OcctPreviewAccess::frame(*p, 0);
        BRep_Builder materialBuilder;
        TopoDS_Compound material;
        materialBuilder.MakeCompound(material);
        for (const auto &region : first.regions)
            materialBuilder.Add(material, region);
        const auto expected = moved(OcctDocumentAccess::simulationShape(*before->document),
                                    first.materialTransforms[*rootSide]);
        check(volume(boolean<BRepAlgoAPI_Cut>(expected, material, tolerance, stop)) <= budget &&
                  volume(boolean<BRepAlgoAPI_Cut>(material, expected, tolerance, stop)) <= budget,
              "Operation start does not match the complete preceding state");
        remember(wanted, preview);
        return preview;
    }
    std::vector<std::size_t> candidates(Mask mask) {
        std::vector<std::size_t> result;
        for (std::size_t order = request.operations.size(); order-- > 0;)
            if ((mask & (Mask{1} << request.operations[order].bendIndex)) &&
                (request.automatic || static_cast<std::size_t>(std::popcount(mask)) == order + 1))
                result.push_back(order);
        if (request.automatic && request.rankCandidates) {
            const auto original = result;
            const auto score = [&](std::size_t order) {
                const auto &op = request.operations[order];
                // Soft accessibility estimate: fewer still-formed bends on the adjacent
                // walls. It changes order only, never rejects a candidate or an orientation.
                const auto prior =
                    request.allowWorkpieceRotation
                        ? std::min(priorResult(mask, op, false), priorResult(mask, op, true))
                        : priorResult(mask, op, op.workpieceReversed);
                // A parallel open chain has a useful continuation direction. This is a
                // tie-breaker only: interior bends and the other end remain candidates.
                const auto continuation =
                    profileRank.empty() ? std::size_t{0}
                    : reversed.empty()
                        ? profileRank[op.bendIndex]
                        : static_cast<std::size_t>(
                              std::abs(static_cast<int>(profileRank[op.bendIndex]) -
                                       static_cast<int>(profileRank[reversed.back().bendIndex])));
                return std::tuple{prior, std::popcount(neighbours[op.bendIndex] & mask),
                                  continuation};
            };
            std::stable_sort(result.begin(), result.end(),
                             [&](auto a, auto b) { return score(a) < score(b); });
            statistics.reorderedStates += result != original ? 1 : 0;
        }
        return result;
    }
    bool search(Mask mask) {
        cancelled(stop);
        if (!mask)
            return true;
        if (dead.contains(mask))
            return false;
        for (const auto order : candidates(mask)) {
            const auto &op = request.operations[order];
            const auto bit = Mask{1} << op.bendIndex;
            const bool preferred = preferredSide(mask, op);
            std::array<bool, 2> sides{preferred, !preferred};
            if (request.allowWorkpieceRotation && request.rankCandidates &&
                priorResult(mask, op, sides[1]) < priorResult(mask, op, sides[0]))
                std::swap(sides[0], sides[1]);
            for (std::size_t choice = 0; choice < (request.allowWorkpieceRotation ? 2u : 1u);
                 ++choice) {
                const bool fixedSecond = sides[choice];
                cancelled(stop);
                if (dead.contains(mask ^ bit))
                    break;
                if (attempts >= request.maximumChecks ||
                    std::chrono::steady_clock::now() - started >
                        std::chrono::milliseconds(request.maximumTimeMs))
                    throw BendRouteError{BendRouteErrorCode::SearchLimit,
                                         "Route search budget reached; " + lastFailure, failedBend,
                                         attempts};
                ++attempts;
                if (request.onProgress)
                    request.onProgress(
                        {BendRoutePhase::Checking, 0, 0, attempts - 1, op.bendIndex});
                try {
                    auto preview = transition(mask, op, fixedSecond);
                    if (request.onProgress)
                        request.onProgress(
                            {BendRoutePhase::Checking, 0, 0, attempts, op.bendIndex});
                    if (const auto *failure = std::get_if<BendPreviewError>(&preview)) {
                        if (failure->code == BendPreviewErrorCode::Cancelled)
                            cancelled(stop);
                        lastFailure = failure->detail;
                        lastOperationError = failure->code;
                        failedBend = op.bendIndex;
                        incomplete |= failure->code == BendPreviewErrorCode::Failed ||
                                      failure->code == BendPreviewErrorCode::UnsupportedGeometry;
                        continue;
                    }
                    reversed.push_back({op.id, op.bendIndex, fixedSecond,
                                        std::get<BendPreviewPtr>(std::move(preview))});
                    if (search(mask ^ bit))
                        return true;
                    reversed.pop_back();
                } catch (const BendRouteError &e) {
                    if (e.code == BendRouteErrorCode::Cancelled ||
                        e.code == BendRouteErrorCode::SearchLimit)
                        throw;
                    incomplete = true;
                    lastFailure = e.detail;
                    lastOperationError.reset();
                    failedBend = op.bendIndex;
                }
            }
        }
        dead.insert(mask);
        return false;
    }
};
} // namespace
struct SingleBendPreviewBuilder::RouteCache : CacheData {};
SingleBendPreviewBuilder::SingleBendPreviewBuilder(std::shared_ptr<ToolGeometryCache> cache)
    : toolCache_(std::move(cache)) {}
SingleBendPreviewBuilder::~SingleBendPreviewBuilder() = default;
struct SingleBendPreviewBuilder::SingleCache {
    struct Key {
        std::size_t bend;
        core::BendTarget target;
        bool reversed, motion;
        core::PressTools tools;
        std::optional<core::ToolAssignment> assignment;
        double supportLimit;
        bool operator==(const Key &) const = default;
    };
    DocumentPtr source;
    std::mutex mutex;
    std::list<std::pair<Key, BendPreviewResult>> entries;
};
BendPreviewResult SingleBendPreviewBuilder::cachedOperation(
    DocumentPtr document, std::size_t index, core::BendTarget target, bool reversed,
    std::stop_token stop, core::PressTools tools, bool motion,
    std::optional<core::ToolAssignment> assignment, double limit) const {
    if (stop.stop_requested())
        return BendPreviewError{BendPreviewErrorCode::Cancelled, "Cancelled"};
    std::shared_ptr<SingleCache> cache;
    {
        std::lock_guard guard(routeCacheMutex_);
        if (!singleCache_ || singleCache_->source != document) {
            singleCache_ = std::make_shared<SingleCache>();
            singleCache_->source = document;
        }
        cache = singleCache_;
    }
    const SingleCache::Key key{index, target, reversed, motion, tools, assignment, limit};
    {
        std::lock_guard guard(cache->mutex);
        for (auto it = cache->entries.begin(); it != cache->entries.end(); ++it)
            if (it->first == key) {
                auto result = it->second;
                cache->entries.splice(cache->entries.begin(), cache->entries, it);
                return result;
            }
    }
    const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
    const auto reference = native && index < native->metadata().bendAnalysis.bends.size()
                               ? std::optional{normalBendAxis(*native, index)}
                               : std::nullopt;
    auto result = buildBendOperation(document, index, target, reversed, stop, tools, motion,
                                     std::move(assignment), limit > 0, limit, reference);
    const auto *error = std::get_if<BendPreviewError>(&result);
    if (!stop.stop_requested() && (!error || stableRejection(error->code))) {
        std::lock_guard guard(cache->mutex);
        cache->entries.emplace_front(key, result);
        while (cache->entries.size() > 4)
            cache->entries.pop_back();
    }
    return result;
}
void SingleBendPreviewBuilder::discardRouteCache() const {
    std::shared_ptr<RouteCache> old;
    std::shared_ptr<SingleCache> oldSingle;
    {
        std::lock_guard lock(routeCacheMutex_);
        old = std::move(routeCache_);
        oldSingle = std::move(singleCache_);
    }
}
RouteDraftResult SingleBendPreviewBuilder::buildRouteDraft(DocumentPtr document,
                                                           RouteDraftRequest request,
                                                           std::stop_token stop) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    try {
        cancelled(stop);
        const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
        check(bool(native), "Unsupported draft document");
        const auto &analysis = native->metadata().bendAnalysis;
        const auto n = analysis.bends.size();
        if (!n || n > maximumSheetBends || request.operations.size() != n || request.step > n)
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "Requires every bend, up to 32 zones"};
        std::set<std::size_t> indices;
        std::set<std::string> ids;
        for (const auto &op : request.operations) {
            if (op.bendIndex >= n || !indices.insert(op.bendIndex).second || op.id.empty() ||
                !ids.insert(op.id).second || !core::validBendTarget(op.target))
                return BendRouteError{BendRouteErrorCode::InvalidInput, "Invalid draft operations"};
            const auto &b = analysis.bends[op.bendIndex];
            if (std::abs(op.target.angleRadians - b.angleRadians) >
                    analysis.angleToleranceRadians ||
                std::abs(op.target.innerRadiusMm - b.innerRadiusMm) > analysis.lengthToleranceMm ||
                (request.manualThicknessMm &&
                 (!std::isfinite(*request.manualThicknessMm) ||
                  std::abs(*request.manualThicknessMm - b.thicknessMm) >
                      analysis.lengthToleranceMm)))
                return BendRouteError{BendRouteErrorCode::InvalidInput,
                                      "Draft target must match CAD", op.bendIndex};
        }
        const auto &active = request.operations[request.step ? request.step - 1 : 0];
        if (request.withTools && active.tools &&
            !core::standardAirBendOpening(analysis.bends[active.bendIndex].thicknessMm,
                                          active.tools->openingMm))
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "V is below the current standard air-bending scope",
                                  active.bendIndex, 0, BendPreviewErrorCode::NarrowDieOpening};
        std::shared_ptr<RouteCache> work;
        {
            std::lock_guard lock(routeCacheMutex_);
            cancelled(stop);
            if (!routeCache_ || routeCache_->source != document ||
                routeCache_->thickness != request.manualThicknessMm) {
                routeCache_ = std::make_shared<RouteCache>();
                routeCache_->source = document;
                routeCache_->thickness = request.manualThicknessMm;
            }
            work = routeCache_;
        }
        std::lock_guard lock(work->mutex);
        cancelled(stop);
        for (auto it = work->drafts.begin(); it != work->drafts.end(); ++it)
            if ((*it)->request == request) {
                auto result = std::make_shared<RouteDraft>(**it);
                result->reusedBasis = true;
                work->drafts.splice(work->drafts.begin(), work->drafts, it);
                cancelled(stop);
                return RouteDraftPtr(result);
            }
        if (!work->flat) {
            const auto result = FlatPatternBuilder().build(
                document, {{core::SheetMaterial::Unknown, .5, 1}, request.manualThicknessMm}, stop);
            if (const auto *e = std::get_if<FlatPatternError>(&result))
                return BendRouteError{e->code == FlatPatternErrorCode::Cancelled
                                          ? BendRouteErrorCode::Cancelled
                                          : BendRouteErrorCode::Unsupported,
                                      e->detail};
            cancelled(stop);
            work->flat = std::get<FlatPatternPtr>(result);
        }
        const bool reused = bool(work->draftBasis);
        if (!work->draftBasis)
            work->draftBasis = std::make_shared<RouteDraftBasis>(document, work->flat, stop);
        const auto result = work->draftBasis->build(std::move(request), reused, stop);
        cancelled(stop);
        work->drafts.push_front(result);
        while (work->drafts.size() > 4)
            work->drafts.pop_back();
        return result;
    } catch (const BendRouteError &e) {
        return e;
    } catch (const Standard_Failure &e) {
        return BendRouteError{BendRouteErrorCode::Failed,
                              e.GetMessageString() ? e.GetMessageString() : "Draft CAD failure"};
    } catch (const std::exception &e) {
        if (stop.stop_requested())
            return BendRouteError{BendRouteErrorCode::Cancelled, "Cancelled"};
        return BendRouteError{BendRouteErrorCode::Failed, e.what()};
    }
}
BendRouteResult SingleBendPreviewBuilder::buildRoute(DocumentPtr document, BendRouteRequest request,
                                                     std::stop_token stop) const {
    GeometryCacheScope cacheScope(toolCache_, stop);
    try {
        cancelled(stop);
        const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
        check(native != nullptr, "Unsupported document type");
        const auto &analysis = OcctDocumentAccess::simulationBends(*native);
        const auto n = analysis.bends.size();
        if (request.onProgress)
            request.onProgress({BendRoutePhase::Preparing, 0, 0, 0, std::nullopt});
        if ((request.allowPartialEndSupport && !request.withTools) ||
            (request.maximumEndSupportLossMm &&
             (!request.allowPartialEndSupport ||
              !validEndSupportLimitMm(*request.maximumEndSupportLossMm))))
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "Partial end support requires explicit tool preview"};
        if (!n || n > maximumSheetBends || request.operations.size() != n ||
            !request.maximumChecks || request.maximumChecks > 512 || !request.maximumTimeMs ||
            request.maximumTimeMs > 120000 ||
            (request.manualThicknessMm &&
             (!std::isfinite(*request.manualThicknessMm) || *request.manualThicknessMm <= 0)))
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "Requires every recognized bend, up to 32 zones"};
        if (!request.toolVariants.empty()) {
            if (!request.automatic || !request.withTools || request.toolVariants.size() > 64)
                return BendRouteError{
                    BendRouteErrorCode::InvalidInput,
                    "Checked tooling requires an automatic tool route and up to 64 variants"};
            const auto started = std::chrono::steady_clock::now();
            BendRouteStatistics totals;
            std::size_t attempts = 0;
            BendRouteError last{BendRouteErrorCode::NoRoute, "No checked tooling variant passed"};
            std::optional<double> invariantGaugeSpan;
            std::vector<std::size_t> toolOrder;
            std::vector<std::pair<double, std::size_t>> shapeRanks;
            const auto rankStarted = std::chrono::steady_clock::now();
            bool attemptedShapeRanking = false;
            for (std::size_t choice = 0; choice < request.toolVariants.size(); ++choice) {
                cancelled(stop);
                toolOrder.push_back(choice);
                if (!request.rankToolShapes)
                    continue;
                const auto &variant = request.toolVariants[choice];
                std::vector<core::ToolFlangeHint> hints;
                for (const auto &hint : document->metadata().toolShapeHints) {
                    if (variant.bendIndex && *variant.bendIndex != hint.bendIndex)
                        continue;
                    const auto present = [&](std::size_t id) {
                        return id < analysis.bends.size() &&
                               std::any_of(
                                   request.operations.begin(), request.operations.end(),
                                   [&](const auto &op) {
                                       const auto &b = analysis.bends[id];
                                       return op.bendIndex == id &&
                                              std::abs(op.target.angleRadians - b.angleRadians) <=
                                                  analysis.angleToleranceRadians &&
                                              std::abs(op.target.innerRadiusMm - b.innerRadiusMm) <=
                                                  analysis.lengthToleranceMm;
                                   });
                    };
                    if (present(hint.bendIndex) && present(hint.neighbourBendIndex))
                        hints.push_back(hint);
                }
                const double thickness =
                    request.manualThicknessMm.value_or(analysis.bends.front().thicknessMm);
                attemptedShapeRanking |= !hints.empty();
                if (const auto risk =
                        core::toolShapeRisk(variant.assignment, hints, thickness, stop))
                    shapeRanks.emplace_back(*risk, choice);
            }
            std::stable_sort(shapeRanks.begin(), shapeRanks.end(),
                             [](auto a, auto b) { return a.first < b.first; });
            std::vector<std::size_t> knownSlots;
            for (const auto &rank : shapeRanks)
                knownSlots.push_back(rank.second);
            std::sort(knownSlots.begin(), knownSlots.end());
            for (std::size_t i = 0; i < knownSlots.size(); ++i)
                toolOrder[knownSlots[i]] = shapeRanks[i].second;
            totals.shapeRankedToolVariants = shapeRanks.size();
            totals.toolShapeRankingUs =
                attemptedShapeRanking ? static_cast<std::uint64_t>(
                                            std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::steady_clock::now() - rankStarted)
                                                .count())
                                      : 0;
            const auto add = [&](const BendRouteStatistics &s) {
                totals.builtStates += s.builtStates;
                totals.reusedStates += s.reusedStates;
                totals.builtTransitions += s.builtTransitions;
                totals.reusedTransitions += s.reusedTransitions;
                totals.reusedRejections += s.reusedRejections;
                totals.reorderedStates += s.reorderedStates;
                totals.reusedFlatPattern |= s.reusedFlatPattern;
                totals.usedProfilePriority |= s.usedProfilePriority;
                totals.gaugePrunedToolVariants += s.gaugePrunedToolVariants;
                totals.backgauge += s.backgauge;
                totals.shapeRankedToolVariants += s.shapeRankedToolVariants;
                totals.toolShapeRankingUs += s.toolShapeRankingUs;
            };
            for (const auto choice : toolOrder) {
                cancelled(stop);
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
                if (attempts >= request.maximumChecks || spent >= request.maximumTimeMs) {
                    last.code = BendRouteErrorCode::SearchLimit;
                    last.detail = "Shared checked-tooling search budget reached; " + last.detail;
                    break;
                }
                auto trial = request;
                if (!applyRouteToolVariant(trial, choice))
                    return BendRouteError{BendRouteErrorCode::InvalidInput,
                                          "Invalid checked-tooling snapshot"};
                for (const auto &op : trial.operations)
                    if (!op.tools || op.bendIndex >= n ||
                        !core::standardAirBendOpening(request.manualThicknessMm.value_or(
                                                          analysis.bends[op.bendIndex].thicknessMm),
                                                      op.tools->openingMm))
                        return BendRouteError{BendRouteErrorCode::InvalidInput,
                                              "Checked tooling violates the minimum opening rule"};
                trial.toolVariants.clear();
                if (request.onProgress)
                    trial.onProgress = [report = request.onProgress, choice,
                                        count = request.toolVariants.size(),
                                        attempts](BendRouteProgress p) {
                        p.variant = choice;
                        p.variants = count;
                        p.checkedOperations += attempts;
                        report(std::move(p));
                    };
                trial.maximumChecks = request.maximumChecks - attempts;
                trial.maximumTimeMs = request.maximumTimeMs - static_cast<unsigned>(spent);
                if (invariantGaugeSpan &&
                    std::any_of(trial.operations.begin(), trial.operations.end(),
                                [&](const auto &op) {
                                    return op.tools && op.tools->backgauge &&
                                           core::minimumGaugeSpanMm(*op.tools->backgauge) >
                                               *invariantGaugeSpan + analysis.lengthToleranceMm * 4;
                                })) {
                    ++totals.gaugePrunedToolVariants;
                    continue;
                }
                ++totals.testedToolVariants;
                auto result = buildRoute(document, std::move(trial), stop);
                if (const auto *route = std::get_if<BendRoutePtr>(&result)) {
                    attempts += (*route)->evaluatedOperations;
                    add((*route)->statistics);
                    auto accepted = std::make_shared<BendRoute>(**route);
                    accepted->selectedToolVariant = choice;
                    accepted->evaluatedOperations = attempts;
                    accepted->statistics = totals;
                    return BendRoutePtr(std::move(accepted));
                }
                last = std::get<BendRouteError>(std::move(result));
                if (last.invariantGaugeSpanMm)
                    invariantGaugeSpan = last.invariantGaugeSpanMm;
                attempts += last.evaluatedOperations;
                add(last.statistics);
                // An incomplete trial is never a rejected tool. Keep its exact cached
                // work for an explicit continuation instead of trying all pairs indefinitely.
                if (last.code != BendRouteErrorCode::NoRoute)
                    break;
            }
            last.evaluatedOperations = attempts;
            last.statistics = totals;
            last.invariantGaugeSpanMm = invariantGaugeSpan;
            return last;
        }
        std::set<std::size_t> indices;
        std::set<std::string> ids;
        for (const auto &op : request.operations) {
            if (op.bendIndex >= n || !indices.insert(op.bendIndex).second || op.id.empty() ||
                !ids.insert(op.id).second || !core::validBendTarget(op.target))
                return BendRouteError{BendRouteErrorCode::InvalidInput, "Invalid route operations"};
            const auto &b = analysis.bends[op.bendIndex];
            if (std::abs(op.target.angleRadians - b.angleRadians) >
                    analysis.angleToleranceRadians ||
                std::abs(op.target.innerRadiusMm - b.innerRadiusMm) > analysis.lengthToleranceMm ||
                (request.manualThicknessMm &&
                 std::abs(*request.manualThicknessMm - b.thicknessMm) > analysis.lengthToleranceMm))
                return BendRouteError{BendRouteErrorCode::InvalidInput,
                                      "Route target/thickness must match the CAD", op.bendIndex};
            if (request.withTools &&
                (!op.tools || !core::validPressTools(*op.tools) ||
                 !core::standardAirBendOpening(b.thicknessMm, op.tools->openingMm) ||
                 (op.assignment && core::toolParameters(*op.assignment) != op.tools)))
                return BendRouteError{BendRouteErrorCode::InvalidInput,
                                      "Missing or inconsistent route tool", op.bendIndex};
            if (request.withTools) {
                const BendRouteToolLengthIssue lengths{b.lengthMm, core::punchLengthMm(*op.tools),
                                                       op.tools->lengthMm};
                if (!lengths.coversBend(analysis.lengthToleranceMm))
                    return BendRouteError{
                        BendRouteErrorCode::InvalidInput,
                        "The selected tool assembly is shorter than the bend line",
                        op.bendIndex,
                        0,
                        BendPreviewErrorCode::ToolTooShort,
                        lengths};
            }
        }
        if (request.withTools &&
            std::any_of(request.operations.begin(), request.operations.end(), [](const auto &op) {
                return op.tools && op.tools->backgauge &&
                       core::minimumGaugeSpanMm(*op.tools->backgauge) > 0;
            })) {
            if (const auto span = backgaugeInvariantSpan(*native, stop))
                for (const auto &op : request.operations)
                    if (op.tools && op.tools->backgauge &&
                        core::minimumGaugeSpanMm(*op.tools->backgauge) >
                            *span + analysis.lengthToleranceMm * 4) {
                        std::ostringstream reason;
                        reason << std::setprecision(6) << "Для контактов упора доступно не более "
                               << *span << " мм; текущая схема базирования требует "
                               << core::minimumGaugeSpanMm(*op.tools->backgauge)
                               << " мм. Проверьте настройки упора.";
                        BendRouteError failure{BendRouteErrorCode::NoRoute, reason.str(),
                                               op.bendIndex, 0,
                                               BendPreviewErrorCode::BackgaugeUnavailable};
                        failure.invariantGaugeSpanMm = span;
                        return failure;
                    }
        }
        std::shared_ptr<RouteCache> work;
        if (request.reuseCalculations) {
            std::lock_guard lock(routeCacheMutex_);
            cancelled(stop);
            if (!routeCache_ || routeCache_->source != document ||
                routeCache_->thickness != request.manualThicknessMm) {
                routeCache_ = std::make_shared<RouteCache>();
                routeCache_->source = document;
                routeCache_->thickness = request.manualThicknessMm;
            }
            work = routeCache_;
        } else {
            work = std::make_shared<RouteCache>();
            work->source = document;
            work->thickness = request.manualThicknessMm;
        }
        std::lock_guard workLock(work->mutex);
        work->transitions.setLimit(std::max(std::size_t{8}, n));
        cancelled(stop);
        const bool reusedFlat = bool(work->flat);
        if (!work->flat) {
            FlatPatternRequest flatRequest{{core::SheetMaterial::Unknown, .5, 1},
                                           request.manualThicknessMm};
            auto flatResult = FlatPatternBuilder().build(document, flatRequest, stop);
            if (const auto *e = std::get_if<FlatPatternError>(&flatResult)) {
                if (e->code == FlatPatternErrorCode::Cancelled)
                    discardRouteCache();
                return BendRouteError{e->code == FlatPatternErrorCode::Cancelled
                                          ? BendRouteErrorCode::Cancelled
                                          : BendRouteErrorCode::Unsupported,
                                      e->detail};
            }
            cancelled(stop);
            work->flat = std::get<FlatPatternPtr>(flatResult);
        }
        RouteSearch search;
        search.source = document;
        search.native = native;
        search.cache = work.get();
        search.statistics.reusedFlatPattern = reusedFlat;
        search.flat = work->flat;
        search.request = std::move(request);
        search.stop = stop;
        search.inputs.resize(n);
        for (auto operation : search.request.operations) {
            // This flag is a preference; the resolved physical side belongs to the key.
            operation.workpieceReversed = false;
            if (!search.request.withTools) {
                operation.tools.reset();
                operation.assignment.reset();
            }
            const auto bend = operation.bendIndex;
            search.inputs[bend] = work->intern({std::move(operation), search.request.withTools,
                                                search.request.allowPartialEndSupport,
                                                search.request.maximumEndSupportLossMm});
        }
        search.layout = &OcctFlatPatternAccess::layout(*search.flat);
        search.tolerance = analysis.lengthToleranceMm;
        search.expectedVolume = volume(OcctDocumentAccess::simulationShape(*native));
        double minimumLength = analysis.bends.front().lengthMm;
        for (const auto &bend : analysis.bends)
            minimumLength = std::min(minimumLength, bend.lengthMm);
        search.budget = std::max(1.e-5, minimumLength * analysis.bends.front().thicknessMm *
                                            analysis.lengthToleranceMm * 2.);
        State original{native, std::vector<std::optional<std::size_t>>(n)};
        for (std::size_t i = 0; i < n; ++i)
            original.indices[i] = i;
        const auto full = (Mask{1} << n) - 1;
        work->states.try_emplace(full, std::make_shared<const State>(std::move(original)));
        search.touchState(full);
        search.referenceAxes.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            search.referenceAxes[i] = normalBendAxis(*native, i);
        for (const auto &wall : search.layout->walls) {
            cancelled(stop);
            Bnd_Box box;
            BRepBndLib::AddOptimal(wall.solid, box, false, false);
            search.wallBounds.push_back(box);
            double amount = 0;
            if (request.withTools && request.allowWorkpieceRotation)
                try {
                    amount = volume(wall.solid);
                } catch (const Standard_Failure &) {
                    // An unavailable soft hint must not reject a valid route.
                }
            search.wallVolumes.push_back(amount);
        }
        search.neighbours.resize(n);
        for (const auto &a : search.layout->bends)
            for (const auto &b : search.layout->bends)
                if (a.bendIndex != b.bendIndex && (a.parent == b.parent || a.parent == b.child ||
                                                   a.child == b.parent || a.child == b.child))
                    search.neighbours[a.bendIndex] |= Mask{1} << b.bendIndex;
        if (search.request.automatic && search.request.rankCandidates && n > 2 &&
            std::all_of(search.layout->bends.begin(), search.layout->bends.end(),
                        [&](const auto &bend) {
                            return bend.sectionAxes.Direction().IsParallel(
                                search.layout->bends.front().sectionAxes.Direction(),
                                analysis.angleToleranceRadians);
                        })) {
            std::vector<std::vector<std::pair<std::size_t, std::size_t>>> graph(
                search.layout->walls.size());
            for (const auto &bend : search.layout->bends) {
                graph[bend.parent].emplace_back(bend.child, bend.bendIndex);
                graph[bend.child].emplace_back(bend.parent, bend.bendIndex);
            }
            if (std::all_of(graph.begin(), graph.end(),
                            [](const auto &edges) { return edges.size() <= 2; })) {
                const auto end = std::find_if(graph.begin(), graph.end(),
                                              [](const auto &edges) { return edges.size() == 1; });
                if (end != graph.end()) {
                    auto wall = static_cast<std::size_t>(end - graph.begin());
                    std::optional<std::size_t> previous;
                    search.profileRank.resize(n);
                    for (std::size_t rank = 0; rank < n; ++rank) {
                        const auto edge = std::find_if(
                            graph[wall].begin(), graph[wall].end(),
                            [&](const auto &e) { return !previous || e.first != *previous; });
                        check(edge != graph[wall].end(), "Incomplete profile chain");
                        search.profileRank[edge->second] = rank;
                        previous = wall;
                        wall = edge->first;
                    }
                    search.statistics.usedProfilePriority = true;
                }
            }
        }
        bool found = false;
        if (search.request.onProgress)
            search.request.onProgress({BendRoutePhase::ToolCandidate, 0, 0, 0, std::nullopt});
        try {
            found = search.search((Mask{1} << n) - 1);
        } catch (BendRouteError &e) {
            e.statistics = search.statistics;
            throw;
        }
        if (!found)
            return BendRouteError{search.incomplete ? BendRouteErrorCode::Unsupported
                                                    : BendRouteErrorCode::NoRoute,
                                  search.lastFailure,
                                  search.failedBend,
                                  search.attempts,
                                  search.lastOperationError,
                                  {},
                                  search.statistics};
        cancelled(stop);
        auto result = std::make_shared<BendRoute>();
        result->source = document;
        result->withTools = search.request.withTools;
        result->evaluatedOperations = search.attempts;
        result->statistics = search.statistics;
        result->steps.assign(search.reversed.rbegin(), search.reversed.rend());
        if (search.request.reuseCalculations) {
            // A complete path can be longer than the working set of an earlier partial
            // branch. Retain its exact checked cycles, not previews from failed branches.
            Mask formed = 0;
            for (const auto &step : result->steps) {
                formed |= Mask{1} << step.bendIndex;
                const auto &op = search.inputs[step.bendIndex]->operation;
                search.remember(search.key(formed, op, step.fixedSecond), step.preview);
            }
        }
        result->partialEndSupport =
            std::any_of(result->steps.begin(), result->steps.end(),
                        [](const auto &step) { return step.preview->partialEndSupport(); });
        return BendRoutePtr(result);
    } catch (const BendRouteError &e) {
        if (e.code == BendRouteErrorCode::Cancelled)
            discardRouteCache();
        return e;
    } catch (const FlatPatternError &e) {
        return BendRouteError{e.code == FlatPatternErrorCode::Cancelled
                                  ? BendRouteErrorCode::Cancelled
                                  : BendRouteErrorCode::Failed,
                              e.detail};
    } catch (const Standard_Failure &e) {
        return BendRouteError{BendRouteErrorCode::Failed,
                              e.GetMessageString() ? e.GetMessageString() : "OCCT route failure"};
    } catch (const std::exception &e) {
        return BendRouteError{BendRouteErrorCode::Failed, e.what()};
    }
}
} // namespace bending::cad
