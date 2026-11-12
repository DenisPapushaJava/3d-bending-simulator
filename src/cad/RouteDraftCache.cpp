#include "RouteDraftCache.h"
#include "CadPreview.h"
#include <algorithm>
#include <utility>

namespace bending::cad {
using namespace application;
RouteDraftCache::Key RouteDraftCache::key(const RouteDraftRequest &request,
                                          const BendAnalysis &analysis) {
    const auto strokes = routeDraftStrokes(request, analysis);
    std::uint64_t formed = 0;
    for (std::size_t i = 0; i + 1 < request.step; ++i)
        for (const auto position : strokes[i])
            formed |= std::uint64_t{1} << request.operations[position].bendIndex;
    const bool partial = request.step != 0 && request.allowPartialEndSupport;
    return {formed,
            request.operations[strokes[request.step ? request.step - 1 : 0].front()],
            request.step == 0,
            request.withTools,
            partial,
            partial ? request.maximumEndSupportLossMm : std::nullopt};
}
RouteDraftPtr RouteDraftCache::find(DocumentPtr source, const RouteDraftRequest &request) {
    const auto wanted = key(request, source->metadata().bendAnalysis);
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto &entry) { return entry.first == wanted; });
    if (found == entries_.end())
        return {};
    auto result = std::make_shared<RouteDraft>();
    result->source = std::move(source);
    result->request = request;
    result->visual = found->second.preview;
    result->checkedToolPositions = found->second.checkedToolPositions;
    result->toolNotice = found->second.notice;
    result->toolLengths = found->second.lengths;
    result->centredPunchOpeningMm = found->second.centredPunchOpeningMm;
    result->reusedBasis = result->reusedVisual = true;
    entries_.splice(entries_.begin(), entries_, found);
    return result;
}
void RouteDraftCache::put(Key wanted, Visual visual) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto &entry) { return entry.first == wanted; });
    if (found != entries_.end()) {
        found->second = std::move(visual);
        entries_.splice(entries_.begin(), entries_, found);
    } else {
        entries_.emplace_front(std::move(wanted), std::move(visual));
        if (entries_.size() > 4)
            entries_.pop_back();
    }
}
void RouteDraftCache::remember(const RouteDraftPtr &draft) {
    put(key(draft->request, draft->source->metadata().bendAnalysis),
        {draft->visual, draft->checkedToolPositions, draft->toolNotice, draft->toolLengths,
         draft->centredPunchOpeningMm});
}
void RouteDraftCache::rememberBlank(DocumentPtr source, RouteOperation active,
                                    const BendPreviewPtr &cycle) {
    const auto p = std::dynamic_pointer_cast<const CadPreview>(cycle);
    if (!source || !p || p->draft || p->activeBends != 1 || p->otherBends != 0 ||
        p->frames.empty() || p->toolCore || !p->motion || !p->retraction || !p->setup ||
        !active.tools || p->setup->tools != *active.tools ||
        p->reversed != active.workpieceReversed || p->axialOffset != active.workpieceOffsetMm ||
        p->states.size() != p->frames.size() ||
        p->floorChecked() != (active.tools->machineModel == core::MachineModel::DemoPressV3) ||
        (active.tools->backgauge &&
         (!p->gauging || p->gauging->status != BackgaugeStatus::Checked)))
        return;
    // The route starts above the sheet. Manual zero is the flat contact pose,
    // just before bending/withdrawal. Reuse only an exactly matching checked
    // pose, not the initial approach or one of the withdrawn gauge samples.
    const auto contact = std::find_if(p->states.begin(), p->states.end(), [&](const auto &state) {
        return state.phase == simulation::PressPhase::Bending && state.section.angleRadians == 0 &&
               state.punchTipYmm == p->geometry.thicknessMm;
    });
    if (contact == p->states.end())
        return;
    const auto index = static_cast<std::size_t>(contact - p->states.begin());
    if (p->collisionAtFrame(index) || p->machineCollisionAtFrame(index))
        return;
    // The complete route has already proved both differences against the all-flat
    // predecessor. Share this immutable checked frame, without meshing or a new
    // collision calculation. A single position acquires no cycle certificate.
    auto blank = std::make_shared<CadPreview>();
    blank->draft = true;
    blank->reversed = p->reversed;
    blank->axialOffset = p->axialOffset;
    blank->seatedMount = p->seatedMount;
    blank->filledCutouts = p->filledCutouts;
    blank->realPunch = p->realPunch;
    blank->realDie = p->realDie;
    blank->gaps = p->gaps;
    blank->uncoveredToolLength = p->uncoveredToolLength;
    blank->geometry = p->geometry;
    // Preserve the envelope of the initial route view, and its camera framing.
    blank->envelope = p->envelope;
    blank->setup = p->setup;
    blank->frames.push_back(p->frames[index]);
    blank->frames.front().backgaugeContacts = p->frames.front().backgaugeContacts;
    blank->frameConflicts.emplace_back();
    blank->machineConflicts.emplace_back();
    blank->states.push_back(*contact);
    if (p->gauging) {
        blank->gauging = p->gauging;
        blank->gauging->checkedPositions = 1;
        blank->gauging->addedRetractionPositions = 0;
        blank->gauging->detail =
            "Неподвижные упрощённые пальцы; проверено сохранённое положение контакта. "
            "Отвод и перенос этим просмотром не проверяются. Координаты не для ЧПУ.";
    }
    RouteDraftRequest request;
    request.withTools = true;
    request.operations.push_back(std::move(active));
    put(key(request, source->metadata().bendAnalysis), {std::move(blank), 1, {}, {}, {}});
}
} // namespace bending::cad
