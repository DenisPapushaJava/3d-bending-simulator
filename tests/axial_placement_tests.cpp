#include "BendOperation.h"
#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepBndLib.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
using namespace bending;
namespace {
void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
TopoDS_Shape corner(bool both = false) {
    auto shape = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 100, 80, 2).Shape();
    shape =
        BRepAlgoAPI_Fuse(shape, fixtures::traySide({10, 0, 0}, {1, 0, 0}, {0, -1, 0}, 80)).Shape();
    shape =
        BRepAlgoAPI_Fuse(shape, fixtures::traySide({0, 10, 0}, {0, 1, 0}, {-1, 0, 0}, 60)).Shape();
    if (both)
        shape = BRepAlgoAPI_Fuse(shape, fixtures::traySide({100, 10, 0}, {0, 1, 0}, {1, 0, 0}, 60))
                    .Shape();
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, true);
    unify.Build();
    return unify.Shape();
}
application::DocumentPtr load(const TopoDS_Shape &shape, const std::filesystem::path &path) {
    STEPControl_Writer writer;
    check(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
              writer.Write(path.string().c_str()) == IFSelect_RetDone,
          "Fixture write failed");
    auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    check(bool(doc), "Fixture import failed");
    return doc;
}
application::BendRouteRequest request(const application::DocumentPtr &doc) {
    auto pair = tool_fixture::pair(16);
    pair.punch.lengthMm = pair.die.lengthMm = 200;
    application::BendRouteRequest q;
    q.automatic = q.withTools = true;
    q.maximumTimeMs = 120000;
    for (std::size_t i = 0; i < doc->metadata().bendAnalysis.bends.size(); ++i) {
        const auto &b = doc->metadata().bendAnalysis.bends[i];
        q.operations.push_back({"B" + std::to_string(i + 1),
                                i,
                                {b.angleRadians, b.innerRadiusMm},
                                core::toolParameters(pair),
                                pair});
    }
    return q;
}
application::BendRoutePtr accepted(const application::BendRouteResult &r) {
    if (const auto *e = std::get_if<application::BendRouteError>(&r))
        throw std::runtime_error(e->detail);
    return std::get<application::BendRoutePtr>(r);
}
void sameBounds(const TopoDS_Shape &a, const TopoDS_Shape &b) {
    check(a.IsNull() == b.IsNull(), "Prepared evaluation lost a shape");
    if (a.IsNull())
        return;
    Bnd_Box x, y;
    BRepBndLib::AddOptimal(a, x, false, false);
    BRepBndLib::AddOptimal(b, y, false, false);
    check(x.CornerMin().Distance(y.CornerMin()) < 1.e-7 &&
              x.CornerMax().Distance(y.CornerMax()) < 1.e-7,
          "Preparation reuse changed the checked/displayed geometry");
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("axial-placement-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        core::PressTools shortPunch;
        shortPunch.lengthMm = 120;
        shortPunch.punchLengthMm = 98;
        const auto support = application::axialSupportOffsets(shortPunch, 100, {4, 100});
        const double offset = application::preferredPunchOffset(100, 98, {2, 101}, support);
        check(support == std::array{-3., -1.} && offset == -1 &&
                  application::commonToolSpan(shortPunch, 100, offset) == std::array{2., 100.},
              "An asymmetric core did not leave an off-centre fit");
        const auto reversed = application::axialSupportOffsets(shortPunch, 100, {0, 96});
        check(application::preferredPunchOffset(100, 98, {-1, 98}, reversed) == -offset &&
                  application::preferredPunchOffset(
                      100, 98, {2, std::numeric_limits<double>::infinity()}, support) == offset,
              "Reflected or open wall window changed the axial fit");
        const auto tight = application::preferredPunchOffset(100, 98, {7, 97}, support);
        check(tight >= support[0] && tight <= support[1],
              "An estimated obstruction overrode material support");
        const auto doc = load(corner(), folder / "corner.step");
        check(doc->metadata().bendAnalysis.bends.size() == 2, "Corner lost bends");
        auto q = request(doc);
        const auto original = q.operations;
        const auto bytes = std::string(doc->sourceStep());
        cad::SingleBendPreviewBuilder builder;
        const auto centered = builder.buildRoute(doc, q, {});
        check(std::holds_alternative<application::BendRouteError>(centered),
              "Centred long tools unexpectedly cleared the neighbouring formed wall");
        std::cout << "centre: "
                  << (std::holds_alternative<application::BendRoutePtr>(centered)
                          ? "passed"
                          : std::get<application::BendRouteError>(centered).detail)
                  << '\n';
        q.allowAxialPlacement = true;
        const auto start = std::chrono::steady_clock::now();
        const auto route = accepted(builder.buildRoute(doc, q, {}));
        std::cout << "edge route: " << route->evaluatedOperations << " checks, "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count()
                  << " ms\n";
        for (const auto &step : route->steps)
            std::cout << step.id << " offset=" << step.preview->workpieceOffsetMm()
                      << " side=" << step.fixedSecond << '\n';
        check(route->evaluatedOperations == route->steps.size(),
              "Search continued enumerating placements after an accepted installation");
        for (const auto &step : route->steps) {
            const auto &p = step.preview;
            check(p->frameCount() == 61 && p->punchRetraction() && !p->collision() &&
                      application::toolCoversAxialPlacement(
                          p->toolSetup()->tools, p->model().widthMm, p->workpieceOffsetMm()),
                  "End placement bypassed the full cycle or tool coverage");
        }
        check(std::any_of(route->steps.begin(), route->steps.end(),
                          [](const auto &s) { return s.preview->workpieceOffsetMm() != 0; }),
              "No end placement");
        const auto warm = accepted(builder.buildRoute(doc, q, {}));
        check(warm->statistics.builtTransitions == 0 && warm->statistics.reusedTransitions,
              "Warm end route rebuilt a checked pose");
        check(warm->statistics.preparedOperations == 0 &&
                  warm->statistics.reusedOperationPreparations == 0 &&
                  warm->statistics.operationPreparationUs == 0 &&
                  warm->statistics.placementEvaluationUs == 0,
              "A cache hit copied historical preparation work into this request");
        auto fixed = q;
        fixed.automatic = fixed.allowWorkpieceRotation = fixed.allowAxialPlacement = false;
        fixed.operations.clear();
        for (const auto &step : route->steps) {
            auto op = original[step.bendIndex];
            op.workpieceOffsetMm = step.preview->workpieceOffsetMm();
            op.workpieceReversed = step.fixedSecond;
            fixed.operations.push_back(op);
        }
        const auto manual = accepted(builder.buildRoute(doc, fixed, {}));
        check(manual->statistics.builtTransitions == 0 && manual->steps.size() == 2,
              "Fixed end placements were ignored or lost their exact keys");
        for (std::size_t i = 0; i < manual->steps.size(); ++i)
            check(manual->steps[i].preview == route->steps[i].preview,
                  "Fixed placement reused another pose");
        for (auto &op : fixed.operations)
            op.workpieceOffsetMm = 0;
        check(
            std::holds_alternative<application::BendRouteError>(builder.buildRoute(doc, fixed, {})),
            "A centred request reused a successful end placement");
        fixed.allowAxialPlacement = true;
        const auto invalidScope = builder.buildRoute(doc, fixed, {});
        check(std::get<application::BendRouteError>(invalidScope).code ==
                  application::BendRouteErrorCode::InvalidInput,
              "Manual request silently enabled placement search");
        fixed.allowAxialPlacement = false;
        fixed.operations.front().workpieceOffsetMm = std::numeric_limits<double>::quiet_NaN();
        check(std::get<application::BendRouteError>(builder.buildRoute(doc, fixed, {})).code ==
                  application::BendRouteErrorCode::InvalidInput,
              "Non-finite placement accepted");
        application::RouteDraftRequest draft;
        draft.withTools = true;
        for (const auto &step : route->steps) {
            auto op = original[step.bendIndex];
            op.workpieceOffsetMm = step.preview->workpieceOffsetMm();
            op.workpieceReversed = step.fixedSecond;
            draft.operations.push_back(op);
        }
        const auto d = builder.buildRouteDraft(doc, draft, {});
        check(std::holds_alternative<application::RouteDraftPtr>(d) &&
                  std::get<application::RouteDraftPtr>(d)->visual->workpieceOffsetMm() ==
                      draft.operations.front().workpieceOffsetMm,
              "Manual draft did not retain the machine-relative offset");
        auto bare = draft;
        for (auto &op : bare.operations) {
            op.tools.reset();
            op.assignment.reset();
        }
        const auto bareResult = builder.buildRouteDraft(doc, bare, {});
        check(std::holds_alternative<application::RouteDraftPtr>(bareResult) &&
                  !std::get<application::RouteDraftPtr>(bareResult)->visual->toolSetup() &&
                  std::get<application::RouteDraftPtr>(bareResult)->visual->workpieceOffsetMm() ==
                      0,
              "A removed tool left a fictitious machine displacement in the draft");
        auto budget = q;
        budget.maximumChecks = 1;
        check(std::get<application::BendRouteError>(builder.buildRoute(doc, budget, {})).code ==
                  application::BendRouteErrorCode::SearchLimit,
              "A placement budget became a no-route verdict");
        // A shorter punch limits the common placement range even with a long die.
        const auto simple = load(fixtures::bentStrip(), folder / "simple.step");
        auto unequal = tool_fixture::pair(16);
        unequal.punch.lengthMm = 120;
        unequal.die.lengthMm = 200;
        const auto &b = simple->metadata().bendAnalysis.bends.front();
        const core::BendTarget target{b.angleRadians, b.innerRadiusMm};
        const auto edge =
            builder.buildAssignedToolMotion(simple, 0, target, unequal, {}, 0, false, 20);
        check(std::holds_alternative<application::BendPreviewPtr>(edge),
              "Common edge of unequal assemblies was rejected");
        const auto outside =
            builder.buildAssignedToolMotion(simple, 0, target, unequal, {}, 0, false, 60);
        check(std::get<application::BendPreviewError>(outside).code ==
                  application::BendPreviewErrorCode::ToolTooShort,
              "Long die incorrectly certified an uncovered punch end");
        const auto preparation = cad::prepareBendOperation(
            simple, 0, target, false, {}, core::toolParameters(unequal), true, unequal);
        check(std::holds_alternative<cad::PreparedBendOperationPtr>(preparation),
              "Cannot prepare the isolated operation");
        const auto &prepared = std::get<cad::PreparedBendOperationPtr>(preparation);
        const auto initial = prepared->at(0, {});
        check(std::holds_alternative<application::BendPreviewPtr>(initial),
              "Prepared centre failed");
        const auto first = std::get<application::BendPreviewPtr>(initial);
        std::ostringstream before, after;
        BRepTools::Write(cad::OcctPreviewAccess::frame(*first, 0).shape, before);
        check(std::holds_alternative<application::BendPreviewPtr>(prepared->at(20, {})),
              "Prepared edge failed");
        BRepTools::Write(cad::OcctPreviewAccess::frame(*first, 0).shape, after);
        check(before.str() == after.str() && first->workpieceOffsetMm() == 0,
              "A second placement mutated a previously published frame/mesh");
        std::stop_source stopped;
        stopped.request_stop();
        check(
            std::get<application::BendPreviewError>(prepared->at(-20, stopped.get_token())).code ==
                    application::BendPreviewErrorCode::Cancelled &&
                std::holds_alternative<application::BendPreviewPtr>(prepared->at(-20, {})),
            "Cancellation poisoned immutable preparation or published an unchecked pose");
        const auto activeCorner = std::find_if(
            doc->metadata().bendAnalysis.bends.begin(), doc->metadata().bendAnalysis.bends.end(),
            [](const auto &candidate) { return std::abs(candidate.lengthMm - 80) < .001; });
        check(activeCorner != doc->metadata().bendAnalysis.bends.end(), "Missing corner bend");
        const auto cornerIndex =
            static_cast<std::size_t>(activeCorner - doc->metadata().bendAnalysis.bends.begin());
        const auto nativeCorner = std::dynamic_pointer_cast<const cad::CadDocument>(doc);
        const auto cornerPreparation = cad::prepareBendOperation(
            doc, cornerIndex, {activeCorner->angleRadians, activeCorner->innerRadiusMm}, true, {},
            original[cornerIndex].tools, true, original[cornerIndex].assignment, false, {},
            cad::normalBendAxis(*nativeCorner, cornerIndex));
        check(std::holds_alternative<cad::PreparedBendOperationPtr>(cornerPreparation),
              "Corner preparation failed");
        const auto &cp = std::get<cad::PreparedBendOperationPtr>(cornerPreparation);
        const auto hit = cp->at(0, {});
        check(std::holds_alternative<application::BendPreviewError>(hit) &&
                  std::get<application::BendPreviewError>(hit).code ==
                      application::BendPreviewErrorCode::ToolIntersection &&
                  std::get<application::BendPreviewError>(hit).diagnostic,
              "Prepared negative control lost its collision prefix");
        const auto conflict = std::get<application::BendPreviewError>(hit).diagnostic;
        std::ostringstream conflictBefore, conflictAfter;
        BRepTools::Write(cad::OcctPreviewAccess::frame(*conflict, conflict->frameCount() - 1).shape,
                         conflictBefore);
        const auto endA = cp->at(-60, {}), endB = cp->at(60, {});
        check(std::holds_alternative<application::BendPreviewPtr>(endA) ||
                  std::holds_alternative<application::BendPreviewPtr>(endB),
              "A rejected pose contaminated a later valid edge");
        BRepTools::Write(cad::OcctPreviewAccess::frame(*conflict, conflict->frameCount() - 1).shape,
                         conflictAfter);
        check(conflictBefore.str() == conflictAfter.str() && conflict->collision(),
              "A valid edge mutated an earlier collision snapshot");
        const auto gaugeDoc = load(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 300, true, 60),
                                   folder / "gauged.step");
        auto gauged = tool_fixture::tangPair();
        gauged.machineModel = core::MachineModel::DemoPressV3;
        gauged.backgauge.emplace();
        gauged.backgauge->fingerWidthMm = 10;
        gauged.backgauge->zMinMm = -200;
        gauged.backgauge->zMaxMm = 0;
        gauged.backgauge->allowOffsetContacts = false;
        gauged.punch.lengthMm = gauged.die.lengthMm = 600;
        const auto gaugeControl =
            builder.buildAssignedToolMotion(gaugeDoc, 0, target, gauged, {}, 0, false, 0);
        if (const auto *e = std::get_if<application::BendPreviewError>(&gaugeControl))
            throw std::runtime_error("Centred gauge: " + e->detail);
        const auto gaugeMiss =
            builder.buildAssignedToolMotion(gaugeDoc, 0, target, gauged, {}, 0, false, 150);
        check(std::holds_alternative<application::BendPreviewError>(gaugeMiss) &&
                  std::get<application::BendPreviewError>(gaugeMiss).code ==
                      application::BendPreviewErrorCode::BackgaugeUnavailable,
              "Axial translation moved the fixed machine gauge ranges with the part");
        // Both end placements lack gauge span; the centre is valid. This forces
        // reuse between three actual installations of exactly the same operation.
        auto centeredGauge = gauged;
        centeredGauge.backgauge->zMinMm = -150;
        centeredGauge.backgauge->zMaxMm = 150;
        centeredGauge.backgauge->minimumSpacingMm = 200;
        application::BendRouteRequest positions;
        positions.automatic = positions.withTools = positions.allowAxialPlacement = true;
        positions.allowWorkpieceRotation = false;
        positions.operations.push_back(
            {"B1", 0, target, core::toolParameters(centeredGauge), centeredGauge});
        cad::SingleBendPreviewBuilder reusedBuilder, referenceBuilder;
        const auto reused = accepted(reusedBuilder.buildRoute(gaugeDoc, positions, {}));
        positions.reusePlacementPreparation = false;
        const auto reference = accepted(referenceBuilder.buildRoute(gaugeDoc, positions, {}));
        check(reused->evaluatedOperations == 3 && reference->evaluatedOperations == 3 &&
                  reused->statistics.preparedOperations == 1 &&
                  reused->statistics.reusedOperationPreparations == 2 &&
                  reference->statistics.preparedOperations == 3 &&
                  reference->statistics.reusedOperationPreparations == 0 &&
                  reused->statistics.backgauge.contactSearches ==
                      reference->statistics.backgauge.contactSearches &&
                  reused->statistics.backgauge.collisionChecks ==
                      reference->statistics.backgauge.collisionChecks,
              "Preparation reuse changed placement enumeration or skipped gauge checks");
        const auto &rp = reused->steps.front().preview, &fp = reference->steps.front().preview;
        check(rp->workpieceOffsetMm() == 0 && fp->workpieceOffsetMm() == 0 &&
                  rp->frameCount() == fp->frameCount() && rp->punchRetraction() &&
                  fp->punchRetraction() && rp->backgauge()->placement == fp->backgauge()->placement,
              "Preparation reuse changed the accepted cycle/gauge placement");
        for (std::size_t i = 0; i < rp->frameCount(); ++i) {
            const auto &a = cad::OcctPreviewAccess::frame(*rp, i),
                       &referenceFrame = cad::OcctPreviewAccess::frame(*fp, i);
            for (std::size_t j = 0; j < a.regions.size(); ++j) {
                sameBounds(a.regions[j], referenceFrame.regions[j]);
                sameBounds(a.displayRegions[j], referenceFrame.displayRegions[j]);
            }
            for (std::size_t j = 0; j < a.tools.size(); ++j)
                sameBounds(a.tools[j], referenceFrame.tools[j]);
            check(a.machine.size() == referenceFrame.machine.size(),
                  "Prepared machine lost a component");
            for (std::size_t j = 0; j < a.machine.size(); ++j)
                sameBounds(a.machine[j].shape, referenceFrame.machine[j].shape);
        }
        std::cout << "preparation reference/reused us: "
                  << reference->statistics.operationPreparationUs << '/'
                  << reused->statistics.operationPreparationUs << '\n';
        const auto joint = load(fixtures::jointBend(), folder / "joint.step");
        auto jq = request(joint);
        jq.allowAxialPlacement = true;
        const auto jointRoute = accepted(builder.buildRoute(joint, jq, {}));
        check(jointRoute->steps.size() == 1 &&
                  jointRoute->steps.front().additionalBends.size() == 1 &&
                  std::abs(jointRoute->steps.front().preview->workpieceOffsetMm()) == 40 &&
                  jointRoute->steps.front().additionalBends.front().workpieceOffsetMm ==
                      jointRoute->steps.front().preview->workpieceOffsetMm(),
              "Joint placement did not use the full gap-inclusive envelope atomically");
        // A wall on each end still obstructs this operation: an edge is not an exemption.
        const auto closed = load(corner(true), folder / "two-sided.step");
        const auto active =
            std::find_if(closed->metadata().bendAnalysis.bends.begin(),
                         closed->metadata().bendAnalysis.bends.end(),
                         [](const auto &v) { return std::abs(v.lengthMm - 80) < .001; });
        check(active != closed->metadata().bendAnalysis.bends.end(), "Missing closed-end bend");
        const auto index =
            static_cast<std::size_t>(active - closed->metadata().bendAnalysis.bends.begin());
        const auto longPair = original.front().assignment;
        for (const double closedEndOffset : {-60., 0., 60.}) {
            const auto blocked = builder.buildAssignedToolMotion(
                closed, index, {active->angleRadians, active->innerRadiusMm}, *longPair, {}, 0,
                false, closedEndOffset);
            check(std::holds_alternative<application::BendPreviewError>(blocked),
                  "A second obstructing end wall was ignored");
            const auto &failure = std::get<application::BendPreviewError>(blocked);
            check(failure.code == application::BendPreviewErrorCode::ToolIntersection,
                  "Two-sided control did not reach the real collision checker");
        }
        // Arbitrary STEP coordinates cannot change the availability of an axial placement.
        gp_Trsf rotation;
        rotation.SetRotation(gp_Ax1(gp_Pnt(13, 17, 29), gp_Dir(1, 2, 3)), .73);
        const auto rotated = load(BRepBuilderAPI_Transform(corner(), rotation, true).Shape(),
                                  folder / "rotated.step");
        auto rq = request(rotated);
        rq.allowAxialPlacement = true;
        check(accepted(builder.buildRoute(rotated, rq, {}))->steps.size() == 2,
              "World rotation changed placement feasibility");
        std::stop_source cancelled;
        auto interrupted = rq;
        interrupted.onProgress = [&](application::BendRouteProgress progress) {
            if (progress.phase == application::BendRoutePhase::Checking &&
                progress.checkedOperations)
                cancelled.request_stop();
        };
        check(std::get<application::BendRouteError>(
                  builder.buildRoute(rotated, interrupted, cancelled.get_token()))
                      .code == application::BendRouteErrorCode::Cancelled,
              "Cancellation between placement checks published a route");
        check(q.operations == original && doc->sourceStep() == bytes,
              "Placement search changed its request or source STEP");
        std::filesystem::remove_all(folder);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
