#include "bending/core/ToolSelection.h"

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
ToolRejection screenValidTools(const ToolSelectionRequest &r, const ToolPreset &p,
                               const ToolPreset &d, std::size_t groove) {
    if (!p.parametersConfirmed || !d.parametersConfirmed || p.kind != ToolKind::Punch ||
        d.kind != ToolKind::Die || groove >= d.openingsMm.size())
        return ToolRejection::MissingData;
    if (p.teaching || d.teaching)
        return ToolRejection::Teaching;
    constexpr double tolerance = 1e-5;
    if (std::min(p.lengthMm, d.lengthMm) + tolerance < r.bendLengthMm)
        return ToolRejection::TooShort;
    if (r.bendAngleRadians + std::max(p.angleRadians, dieGroove(d, groove)->angleRadians) >
        std::numbers::pi + 1e-12)
        return ToolRejection::Angle;
    if (*p.noseRadiusMm > r.innerRadiusMm - tolerance)
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
                planToolSegments(*t.segments, r.bendLengthMm, 200000, r.maximumSegmentGapMm);
            if (plan.status != SegmentPlanStatus::Complete)
                return ToolRejection::TooShort;
            t.lengthMm = plan.lengthMm;
        }
    return screenValidTools(r, pair[0], pair[1], groove);
}
ToolSelectionResult selectTools(const ToolCatalog &catalog, const ToolSelectionRequest &r) {
    ToolSelectionResult result;
    result.validRequest = validRequest(r) && validToolCatalog(catalog);
    if (!result.validRequest)
        return result;
    auto assembled = catalog;
    // Share one immutable plan per profile across candidate pairs, not a copy per pair.
    std::vector<std::shared_ptr<const ToolAssembly>> assemblies(catalog.size());
    const auto stocked = std::count_if(catalog.begin(), catalog.end(), [](const auto &t) {
        return t.segments && std::any_of(t.segments->begin(), t.segments->end(), [](const auto &s) {
                   return s.quantity && s.shape == SegmentShape::Straight;
               });
    });
    const auto budget = 200000 / std::max<std::size_t>(1, static_cast<std::size_t>(stocked));
    for (std::size_t i = 0; i < catalog.size(); ++i)
        if (catalog[i].segments) {
            const auto plan = planToolSegments(*catalog[i].segments, r.bendLengthMm, budget,
                                               r.maximumSegmentGapMm);
            result.segmentSearchLimited |= plan.status == SegmentPlanStatus::SearchLimit;
            assembled[i].lengthMm = plan.status == SegmentPlanStatus::Complete ? plan.lengthMm : 0;
            if (plan.status == SegmentPlanStatus::Complete)
                assemblies[i] = std::make_shared<const ToolAssembly>(plan.assembly);
        }
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
                if (screenValidTools(r, assembled[p], assembled[d], g) != ToolRejection::None) {
                    ++result.rejectedPairs;
                    continue;
                }
                const double ratio = catalog[d].openingsMm[g] / r.thicknessMm;
                // 8t is an explicit neutral heuristic, not a material-specific radius model.
                const double score =
                    std::abs(std::log(ratio / 8.0)) +
                    .1 * (r.innerRadiusMm - *catalog[p].noseRadiusMm) / r.innerRadiusMm;
                result.candidates.push_back({p, d, g, score, ratio, assemblies[p], assemblies[d]});
                if (!r.shapeHints.empty()) {
                    ToolAssignment assignment{catalog[p], catalog[d], g, 10, "", true};
                    assignment.punchAssembly =
                        assemblies[p] ? std::optional{*assemblies[p]} : std::nullopt;
                    assignment.dieAssembly =
                        assemblies[d] ? std::optional{*assemblies[d]} : std::nullopt;
                    const auto normal = toolShapeRisk(assignment, r.shapeHints, r.thicknessMm);
                    assignment.punchReversed = true;
                    const auto reverse = toolShapeRisk(assignment, r.shapeHints, r.thicknessMm);
                    if (normal && reverse)
                        result.candidates.back().shapeRisk = std::min(*normal, *reverse);
                }
            }
        }
    }
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
