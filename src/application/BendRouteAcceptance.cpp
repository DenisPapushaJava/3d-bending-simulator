#include "bending/application/BendRouteAcceptance.h"
#include "bending/application/SingleBendPreview.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace bending::application {
namespace {
bool matchesSession(const DocumentSession &session, const BendRouteRequest &request) {
    if (!session.document())
        return false;
    if (request.operations.size() != session.document()->metadata().bendAnalysis.bends.size())
        return false;
    std::set<std::size_t> seen;
    for (const auto &op : request.operations) {
        const auto *entry = session.bend(op.bendIndex);
        if (!validWorkpieceOffset(op.workpieceOffsetMm) || !entry || !included(entry->review) ||
            entry->id != op.id || session.bendTarget(op.bendIndex) != std::optional{op.target} ||
            !seen.insert(op.bendIndex).second ||
            (request.withTools &&
             (!op.tools || !core::validPressTools(*op.tools) ||
              (op.assignment && core::toolParameters(*op.assignment) != op.tools))))
            return false;
    }
    return true;
}
} // namespace
bool prepareBendRouteRequest(const DocumentSession &session, BendRouteRequest &request) {
    if (!matchesSession(session, request))
        return false;
    request.manualThicknessMm = session.manualThicknessMm();
    if ((request.manualExtraction && !request.withTools) ||
        (request.allowAxialPlacement && (!request.automatic || !request.withTools)) ||
        !validRouteAssemblySearch(request))
        return false;
    if (!request.toolVariants.empty()) {
        if (request.toolVariants.size() > 64)
            return false;
        for (std::size_t i = 0; i < request.toolVariants.size(); ++i) {
            auto candidate = request;
            if (!applyRouteToolVariant(candidate, i))
                return false;
        }
    }
    return true;
}
bool applyBendRouteResult(DocumentSession &session, BendRouteRequest request,
                          const BendRoutePtr &route) {
    if (session.activeRequest() || request.manualThicknessMm != session.manualThicknessMm() ||
        !matchesSession(session, request))
        return false;
    const auto &document = session.document();
    bool selectedInput =
        route && (request.toolVariants.empty()
                      ? !route->selectedToolVariant
                      : route->selectedToolVariant &&
                            applyRouteToolVariant(request, *route->selectedToolVariant));
    selectedInput = selectedInput && route && applyRouteAssemblyResult(request, *route);
    bool valid = selectedInput && route && route->source == document &&
                 route->withTools == request.withTools &&
                 route->manualExtraction == request.manualExtraction &&
                 (!request.manualExtraction || request.withTools) && !route->steps.empty() &&
                 route->steps.size() <= request.operations.size();
    std::set<std::string> ids;
    std::vector<std::string> order;
    bool partialEndSupport = false;
    if (valid)
        for (const auto &step : route->steps) {
            const auto found = std::find_if(
                request.operations.begin(), request.operations.end(),
                [&](const auto &op) { return step.id == op.id && step.bendIndex == op.bendIndex; });
            const auto &p = step.preview;
            std::vector<std::string> memberIds;
            for (const auto &member : step.additionalBends) {
                const auto input = std::find_if(
                    request.operations.begin(), request.operations.end(), [&](const auto &op) {
                        return op.id == member.id && op.bendIndex == member.bendIndex;
                    });
                valid &= input != request.operations.end() && ids.insert(member.id).second &&
                         found != request.operations.end();
                if (input != request.operations.end())
                    valid &= input->target == member.target && input->tools == member.tools &&
                             input->assignment == member.assignment && p &&
                             member.workpieceOffsetMm == p->workpieceOffsetMm() &&
                             (request.allowAxialPlacement ||
                              input->workpieceOffsetMm == member.workpieceOffsetMm) &&
                             (request.allowWorkpieceRotation ||
                              input->workpieceReversed == member.workpieceReversed);
                if (found != request.operations.end())
                    valid &= std::abs(member.target.angleRadians - found->target.angleRadians) <=
                                 document->metadata().bendAnalysis.angleToleranceRadians &&
                             std::abs(member.target.innerRadiusMm - found->target.innerRadiusMm) <=
                                 document->metadata().bendAnalysis.lengthToleranceMm &&
                             (!request.withTools || (member.tools == found->tools &&
                                                     member.assignment == found->assignment));
                memberIds.push_back(member.id);
            }
            valid &= p && p->activeBendCount() == step.additionalBends.size() + 1;
            valid &=
                found != request.operations.end() && ids.insert(step.id).second && p &&
                !p->unverifiedDraft() && !p->collision() && p->frameCount() >= 2 &&
                p->workpieceReversed() == step.fixedSecond &&
                validWorkpieceOffset(p->workpieceOffsetMm()) &&
                (request.allowAxialPlacement ||
                 (found != request.operations.end() &&
                  p->workpieceOffsetMm() == (request.withTools ? found->workpieceOffsetMm : 0))) &&
                (request.allowWorkpieceRotation ||
                 (found != request.operations.end() &&
                  found->workpieceReversed == step.fixedSecond)) &&
                p->manualExtraction() == request.manualExtraction &&
                (request.withTools ? p->pressMotion() != nullptr : !p->toolSetup());
            if (p && found != request.operations.end()) {
                partialEndSupport |= p->hasPartialEndSupport();
                if (request.withTools && found->tools)
                    valid &= toolCoversAxialPlacement(
                        *found->tools, p->model().widthMm, p->workpieceOffsetMm(),
                        document->metadata().bendAnalysis.lengthToleranceMm);
                else
                    valid &= p->workpieceOffsetMm() == 0;
                valid &= validEndSupportIntervals(
                    *p,
                    request.allowPartialEndSupport ? request.maximumEndSupportLossMm
                                                   : std::optional<double>(0),
                    session.document()->metadata().bendAnalysis.lengthToleranceMm * 4 + 1.e-8);
                if (p->hasPartialEndSupport())
                    valid &= request.withTools && request.allowPartialEndSupport;
                const bool drawingPunch = request.withTools && found->assignment &&
                                          found->assignment->useDrawingGeometry &&
                                          found->assignment->punch.drawing.has_value();
                const bool drawingDie = request.withTools && found->assignment &&
                                        found->assignment->useDrawingGeometry &&
                                        found->assignment->die.drawing.has_value();
                valid &= p->usesDrawingPunch() == drawingPunch &&
                         p->usesDrawingDie() == drawingDie && p->otherFormedBends() == order.size();
                valid &= p->machineModel() == (request.withTools ? found->tools->machineModel
                                                                 : core::MachineModel::None);
                valid &=
                    p->floorChecked() == (p->machineModel() == core::MachineModel::DemoPressV3);
                for (std::size_t f = 0; f < p->frameCount(); ++f) {
                    valid &= !p->machineCollisionAtFrame(f);
                    if (request.manualExtraction)
                        valid &= p->pressState(f) &&
                                 p->pressState(f)->phase != simulation::PressPhase::Retracting;
                }
                if (request.withTools) {
                    valid &= !found->tools->backgauge ||
                             (p->backgauge() && p->backgauge()->setup == *found->tools->backgauge &&
                              p->backgauge()->status == BackgaugeStatus::Checked);
                    const auto *ret = p->punchRetraction();
                    valid &= p->frameCount() >= 2 && p->toolSetup() &&
                             p->toolSetup()->tools == *found->tools &&
                             core::punchLengthMm(*found->tools) +
                                     document->metadata().bendAnalysis.lengthToleranceMm >=
                                 p->model().widthMm &&
                             found->tools->lengthMm +
                                     document->metadata().bendAnalysis.lengthToleranceMm >=
                                 p->model().widthMm &&
                             p->pressState(0) &&
                             p->pressState(0)->phase == simulation::PressPhase::Approach;
                    const auto *last = p->pressState(p->frameCount() - 1);
                    if (request.manualExtraction)
                        valid &=
                            !ret && last && last->phase == simulation::PressPhase::Bending &&
                            std::isfinite(last->section.angleRadians) &&
                            std::abs(last->section.angleRadians - found->target.angleRadians) <=
                                document->metadata().bendAnalysis.angleToleranceRadians;
                    else
                        valid &= last && last->phase == simulation::PressPhase::Retracting && ret &&
                                 std::isfinite(ret->liftMm) && ret->liftMm > 0 &&
                                 std::isfinite(ret->finalClearanceMm) && ret->finalClearanceMm > 0;
                }
            }
            order.push_back(step.id);
            order.insert(order.end(), memberIds.begin(), memberIds.end());
        }
    valid &= ids.size() == request.operations.size();
    valid &= route && route->partialEndSupport == partialEndSupport;
    if (valid) {
        auto candidate = session;
        if (request.withTools)
            for (const auto &op : request.operations) {
                if (op.assignment)
                    valid &= candidate.setBendToolAssignment(op.bendIndex, *op.assignment);
                else
                    valid &= candidate.setBendTools(op.bendIndex, op.tools);
            }
        for (const auto &step : route->steps)
            if (request.withTools || !step.additionalBends.empty()) {
                valid &= candidate.setBendWorkpieceOrientation(step.bendIndex, step.fixedSecond);
                if (request.withTools)
                    valid &= candidate.setBendWorkpieceOffset(step.bendIndex,
                                                              step.preview->workpieceOffsetMm());
                for (const auto &member : step.additionalBends) {
                    valid &= candidate.setBendWorkpieceOrientation(member.bendIndex,
                                                                   member.workpieceReversed);
                    if (request.withTools)
                        valid &= candidate.setBendWorkpieceOffset(member.bendIndex,
                                                                  member.workpieceOffsetMm);
                }
            }
        valid &= candidate.setBendOrder(order);
        if (valid)
            session = std::move(candidate);
    }
    return valid;
}
} // namespace bending::application
