#include "BendOperation.h"
#include "CadPreview.h"
#include "FlatPatternDisplay.h"
#include "RouteDraftGeometry.h"
#include "RouteGeometry.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/core/ToolSelection.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
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
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace bending::cad {
namespace {
using namespace application;
using Mask = std::uint64_t;
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
struct State {
    std::shared_ptr<const CadDocument> document;
    std::vector<std::optional<std::size_t>> indices;
};
struct TransitionKey {
    Mask mask;
    RouteOperation operation;
    bool fixedSecond, withTools, partialEndSupport;
    bool operator==(const TransitionKey &) const = default;
};
struct Transition {
    TransitionKey key;
    BendPreviewResult result;
};
struct CacheData {
    DocumentPtr source;
    std::optional<double> thickness;
    FlatPatternPtr flat;
    std::shared_ptr<const RouteDraftBasis> draftBasis;
    std::list<RouteDraftPtr> drafts;
    std::map<Mask, std::shared_ptr<const State>> states;
    std::list<Mask> stateOrder;
    std::list<Transition> transitions;
    std::mutex mutex;
};
bool stableRejection(BendPreviewErrorCode code) {
    switch (code) {
    case BendPreviewErrorCode::ToolIntersection:
    case BendPreviewErrorCode::SheetIntersection:
    case BendPreviewErrorCode::MissingLeftSupport:
    case BendPreviewErrorCode::MissingRightSupport:
    case BendPreviewErrorCode::SupportNotProven:
    case BendPreviewErrorCode::ToolAngleConflict:
    case BendPreviewErrorCode::NoseTooLarge:
    case BendPreviewErrorCode::NarrowDieOpening:
    case BendPreviewErrorCode::ContactLost:
    case BendPreviewErrorCode::RetractionNotProven:
    case BendPreviewErrorCode::UnsupportedToolGeometry:
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
    BendRouteStatistics statistics;
    std::vector<Mask> neighbours;
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
        auto geometry = solids.front();
        for (std::size_t i = 1; i < solids.size(); ++i)
            geometry = boolean<BRepAlgoAPI_Fuse>(geometry, solids[i], tolerance, stop);
        TopTools_IndexedMapOfShape components;
        TopExp::MapShapes(geometry, TopAbs_SOLID, components);
        check(components.Extent() == 1 && std::abs(volume(geometry) - expectedVolume) <= budget,
              "Intermediate walls overlap or disconnect");
        TopoDS_Shape skin;
        if (native->metadata().closedCutouts.status == ClosedCutoutStatus::Filled)
            skin =
                flatCutoutDisplay(OcctDocumentAccess::shape(*native), display, {}, tolerance, stop)
                    .surface;
        auto doc = OcctDocumentAccess::routeState(*native, geometry, skin, stop);
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
        auto operation = op;
        if (!request.withTools) {
            operation.tools.reset();
            operation.assignment.reset();
        }
        return {mask, std::move(operation), side, request.withTools,
                request.allowPartialEndSupport};
    }
    // 0 = verified success, 1 = unknown, 2 = stable rejection of this exact transition.
    int priorResult(Mask mask, const RouteOperation &op, bool side) const {
        if (!request.reuseCalculations)
            return 1;
        const auto wanted = key(mask, op, side);
        for (const auto &transition : cache->transitions)
            if (transition.key == wanted)
                return std::holds_alternative<BendPreviewPtr>(transition.result) ? 0 : 2;
        return 1;
    }
    void remember(TransitionKey key, BendPreviewResult result) {
        if (!request.reuseCalculations)
            return;
        cancelled(stop);
        cache->transitions.push_front({std::move(key), std::move(result)});
        while (cache->transitions.size() > 8)
            cache->transitions.pop_back();
    }
    BendPreviewResult transition(Mask mask, const RouteOperation &op, bool fixedSecond) {
        const auto wanted = key(mask, op, fixedSecond);
        if (request.reuseCalculations)
            for (auto it = cache->transitions.begin(); it != cache->transitions.end(); ++it)
                if (it->key == wanted) {
                    ++statistics.reusedTransitions;
                    auto value = it->result;
                    cache->transitions.splice(cache->transitions.begin(), cache->transitions, it);
                    return value;
                }
        ++statistics.builtTransitions;
        const auto after = state(mask);
        const auto before = state(mask ^ (Mask{1} << op.bendIndex));
        auto preview = buildBendOperation(
            after->document, *after->indices[op.bendIndex], op.target, fixedSecond, stop,
            request.withTools ? op.tools : std::nullopt, request.withTools,
            request.withTools ? op.assignment : std::nullopt, request.allowPartialEndSupport);
        if (const auto *failure = std::get_if<BendPreviewError>(&preview)) {
            if (stableRejection(failure->code))
                remember(wanted, preview);
            return preview;
        }
        const auto p = std::get<BendPreviewPtr>(preview);
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
                return std::tuple{
                    std::min(priorResult(mask, op, false), priorResult(mask, op, true)),
                    std::popcount(neighbours[op.bendIndex] & mask)};
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
            std::array<bool, 2> sides{false, true};
            if (request.rankCandidates &&
                priorResult(mask, op, true) < priorResult(mask, op, false))
                std::swap(sides[0], sides[1]);
            for (const bool fixedSecond : sides) {
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
                try {
                    auto preview = transition(mask, op, fixedSecond);
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
SingleBendPreviewBuilder::~SingleBendPreviewBuilder() = default;
void SingleBendPreviewBuilder::discardRouteCache() const {
    std::shared_ptr<RouteCache> old;
    {
        std::lock_guard lock(routeCacheMutex_);
        old = std::move(routeCache_);
    }
}
RouteDraftResult SingleBendPreviewBuilder::buildRouteDraft(DocumentPtr document,
                                                           RouteDraftRequest request,
                                                           std::stop_token stop) const {
    try {
        cancelled(stop);
        const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
        check(bool(native), "Unsupported draft document");
        const auto &analysis = native->metadata().bendAnalysis;
        const auto n = analysis.bends.size();
        if (!n || n > 12 || request.operations.size() != n || request.step > n)
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "Requires every bend, up to 12 zones"};
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
        while (work->drafts.size() > 3)
            work->drafts.pop_back();
        return result;
    } catch (const BendRouteError &e) {
        return e;
    } catch (const Standard_Failure &e) {
        return BendRouteError{BendRouteErrorCode::Failed,
                              e.GetMessageString() ? e.GetMessageString() : "Draft CAD failure"};
    } catch (const std::exception &e) {
        return BendRouteError{BendRouteErrorCode::Failed, e.what()};
    }
}
BendRouteResult SingleBendPreviewBuilder::buildRoute(DocumentPtr document, BendRouteRequest request,
                                                     std::stop_token stop) const {
    try {
        cancelled(stop);
        const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
        check(native != nullptr, "Unsupported document type");
        const auto &analysis = OcctDocumentAccess::simulationBends(*native);
        const auto n = analysis.bends.size();
        if (request.allowPartialEndSupport && !request.withTools)
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "Partial end support requires explicit tool preview"};
        if (!n || n > 12 || request.operations.size() != n || !request.maximumChecks ||
            request.maximumChecks > 512 || !request.maximumTimeMs ||
            request.maximumTimeMs > 120000 ||
            (request.manualThicknessMm &&
             (!std::isfinite(*request.manualThicknessMm) || *request.manualThicknessMm <= 0)))
            return BendRouteError{BendRouteErrorCode::InvalidInput,
                                  "Requires every recognized bend, up to 12 zones"};
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
            if (request.withTools && std::min(op.tools->lengthMm, core::punchLengthMm(*op.tools)) +
                                             analysis.lengthToleranceMm <
                                         b.lengthMm)
                return BendRouteError{BendRouteErrorCode::InvalidInput,
                                      "The selected tool assembly is shorter than the bend line",
                                      op.bendIndex,
                                      0,
                                      BendPreviewErrorCode::ToolTooShort,
                                      BendRouteToolLengthIssue{b.lengthMm,
                                                               core::punchLengthMm(*op.tools),
                                                               op.tools->lengthMm}};
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
        search.neighbours.resize(n);
        for (const auto &a : search.layout->bends)
            for (const auto &b : search.layout->bends)
                if (a.bendIndex != b.bendIndex && (a.parent == b.parent || a.parent == b.child ||
                                                   a.child == b.parent || a.child == b.child))
                    search.neighbours[a.bendIndex] |= Mask{1} << b.bendIndex;
        bool found = false;
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
