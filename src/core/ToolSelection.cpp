#include "ToolShapeRanking.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace bending::core {
namespace {
bool validRequest(const ToolSelectionRequest &r) {
    return std::isfinite(r.thicknessMm) && r.thicknessMm >= .1 - 1e-9 && r.thicknessMm <= 1000 &&
           std::isfinite(r.bendAngleRadians) && r.bendAngleRadians > 0 &&
           r.bendAngleRadians < std::numbers::pi && std::isfinite(r.innerRadiusMm) &&
           r.innerRadiusMm >= .1 - 1e-9 && r.innerRadiusMm <= 100000 &&
           std::isfinite(r.bendLengthMm) && r.bendLengthMm >= 1 - 1e-9 &&
           r.bendLengthMm <= 100000 && std::isfinite(r.maximumSegmentGapMm) &&
           r.maximumSegmentGapMm >= 0 && r.maximumSegmentGapMm <= 1000;
}
constexpr double lengthTolerance = 1e-5;
// Overestimate stock deliberately: both horn rows and every allowed seam can
// contribute. The section planner still decides what can actually be assembled.
double maximumStockSpan(const std::vector<ToolSegment> &stock, double maximumGap) {
    double material = 0;
    std::size_t pieces = 0;
    for (const auto &segment : stock)
        if (usableToolSegment(segment)) {
            material += std::ceil(segment.lengthMm * 1000) / 1000 * segment.quantity;
            pieces += segment.quantity;
        }
    return pieces ? material + (pieces - 1) * (std::ceil(maximumGap * 1000) / 1000) : 0;
}
ToolRejection screenProfiles(const ToolSelectionRequest &r, const ToolPreset &p,
                             const ToolPreset &d, std::size_t groove,
                             std::optional<double> assembledLength = {}) {
    if (!p.parametersConfirmed || !d.parametersConfirmed || p.kind != ToolKind::Punch ||
        d.kind != ToolKind::Die || groove >= d.openingsMm.size())
        return ToolRejection::MissingData;
    if (p.teaching || d.teaching)
        return ToolRejection::Teaching;
    if (assembledLength && *assembledLength + lengthTolerance < r.bendLengthMm)
        return ToolRejection::TooShort;
    if (r.bendAngleRadians + std::max(p.angleRadians, dieGroove(d, groove)->angleRadians) >
        std::numbers::pi + 1e-12)
        return ToolRejection::Angle;
    if (*p.noseRadiusMm > r.innerRadiusMm - lengthTolerance)
        return ToolRejection::Radius;
    if (!standardAirBendOpening(r.thicknessMm, d.openingsMm[groove]))
        return ToolRejection::NarrowOpening;
    if (*p.noseRadiusMm > d.openingsMm[groove] / 4)
        return ToolRejection::MissingData;
    return ToolRejection::None;
}
} // namespace
bool standardAirBendOpening(double t, double v) noexcept {
    return std::isfinite(t) && std::isfinite(v) && t > 0 && v > 0 &&
           v / t >= minimumAirBendOpeningRatio - 1.e-12;
}
double minimumStandardDieOpeningMm(double t) noexcept {
    return t * minimumAirBendOpeningRatio;
}
double referenceDieOpeningMm(double t) noexcept {
    return t * 8.;
}
ToolRejection screenTools(const ToolSelectionRequest &r, const ToolPreset &p, const ToolPreset &d,
                          std::size_t groove) {
    if (!validRequest(r) || !validToolPreset(p) || !validToolPreset(d))
        return ToolRejection::MissingData;
    auto pair = ToolCatalog{p, d};
    for (auto &t : pair)
        if (t.segments) {
            const auto plan =
                t.kind == ToolKind::Punch
                    ? planPunchAssemblies(*t.segments, r.bendLengthMm, 200000,
                                          r.maximumSegmentGapMm)
                          .best()
                    : planToolSegments(*t.segments, r.bendLengthMm, 200000, r.maximumSegmentGapMm);
            if (plan.status != SegmentPlanStatus::Complete)
                return ToolRejection::TooShort;
            t.lengthMm = plan.lengthMm;
        }
    return screenProfiles(r, pair[0], pair[1], groove,
                          std::min(pair[0].lengthMm, pair[1].lengthMm));
}
ToolSelectionResult selectTools(const ToolCatalog &catalog, const ToolSelectionRequest &r,
                                std::stop_token stop) {
    ToolSelectionResult result;
    result.validRequest = !stop.stop_requested() && validRequest(r) && validToolCatalog(catalog);
    if (!result.validRequest)
        return result;
    // Plan each eligible profile on demand and share that plan across pairs and grooves.
    struct Coverage {
        double lengthMm;
        std::shared_ptr<const ToolAssembly> assembly;
    };
    std::vector<std::optional<Coverage>> coverage(catalog.size());
    const auto stocked = std::count_if(catalog.begin(), catalog.end(), [](const auto &t) {
        return t.segments && std::any_of(t.segments->begin(), t.segments->end(), [](const auto &s) {
                   return s.quantity && usableToolSegment(s);
               });
    });
    const auto budget = 200000 / std::max<std::size_t>(1, static_cast<std::size_t>(stocked));
    const auto cover = [&](std::size_t i) -> const Coverage & {
        auto &cached = coverage[i];
        if (cached)
            return *cached;
        const auto &tool = catalog[i];
        if (!tool.segments)
            return cached.emplace(Coverage{tool.lengthMm, {}});
        if (maximumStockSpan(*tool.segments, r.maximumSegmentGapMm) + lengthTolerance <
            r.bendLengthMm)
            return cached.emplace(Coverage{0, {}});
        SegmentPlan plan;
        if (tool.kind == ToolKind::Punch) {
            const auto family = planPunchAssemblies(*tool.segments, r.bendLengthMm, budget,
                                                    r.maximumSegmentGapMm, stop);
            result.segmentSearchLimited |= family.status == SegmentPlanStatus::SearchLimit;
            plan = family.best();
        } else
            plan = planToolSegments(*tool.segments, r.bendLengthMm, budget, r.maximumSegmentGapMm,
                                    stop);
        result.segmentSearchLimited |= plan.status == SegmentPlanStatus::SearchLimit;
        if (plan.status == SegmentPlanStatus::Complete)
            return cached.emplace(Coverage{
                plan.lengthMm, std::make_shared<const ToolAssembly>(std::move(plan.assembly))});
        return cached.emplace(Coverage{0, {}});
    };
    for (const auto &t : catalog)
        if (!t.parametersConfirmed)
            ++result.missingData;
    for (std::size_t p = 0; p < catalog.size(); ++p) {
        if (catalog[p].kind != ToolKind::Punch)
            continue;
        for (std::size_t d = 0; d < catalog.size(); ++d) {
            if (catalog[d].kind != ToolKind::Die)
                continue;
            for (std::size_t g = 0; g < catalog[d].openingsMm.size(); ++g) {
                if (stop.stop_requested())
                    return {};
                if (screenProfiles(r, catalog[p], catalog[d], g) != ToolRejection::None ||
                    cover(p).lengthMm + lengthTolerance < r.bendLengthMm ||
                    cover(d).lengthMm + lengthTolerance < r.bendLengthMm) {
                    ++result.rejectedPairs;
                    continue;
                }
                const double ratio = catalog[d].openingsMm[g] / r.thicknessMm;
                // 8t is an explicit neutral heuristic, not a material-specific radius model.
                const double score =
                    std::abs(std::log(ratio / 8.0)) +
                    .1 * (r.innerRadiusMm - *catalog[p].noseRadiusMm) / r.innerRadiusMm;
                const auto &punch = cover(p).assembly, &die = cover(d).assembly;
                result.candidates.push_back({p, d, g, score, ratio, punch, die});
            }
        }
    }
    if (!r.shapeHints.empty())
        evaluateToolCandidateShapes(catalog, r, result.candidates, stop);
    if (stop.stop_requested())
        return {};
    std::sort(result.candidates.begin(), result.candidates.end(),
              [&](const auto &a, const auto &b) {
                  return std::tuple{a.score, catalog[a.punch].id, catalog[a.die].id, a.groove} <
                         std::tuple{b.score, catalog[b.punch].id, catalog[b.die].id, b.groove};
              });
    // Unknown geometry keeps its original slot. Only supported hints reorder known
    // candidates; no approximate overlap becomes a rejection or a claimed clearance.
    std::vector<ToolCandidate> ranked;
    for (const auto &candidate : result.candidates)
        if (candidate.shapeRisk)
            ranked.push_back(candidate);
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return *a.shapeRisk < *b.shapeRisk; });
    std::size_t next = 0;
    for (auto &candidate : result.candidates)
        if (candidate.shapeRisk)
            candidate = ranked[next++];
    result.shapeRanked = !ranked.empty();
    return result;
}
} // namespace bending::core
