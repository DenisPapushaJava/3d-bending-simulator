#include "BendOperation.h"
#include "DieWindowFixture.h"
#include "RouteSheetFeatures.h"
#include "ToolFixtures.h"
#include "bending/application/SingleBendPreview.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
auto hints(const application::DocumentPtr &document) {
    const auto flat =
        cad::FlatPatternBuilder{}.build(document, {{core::SheetMaterial::Unknown, .5, 1}, {}}, {});
    if (const auto *error = std::get_if<application::FlatPatternError>(&flat))
        throw std::runtime_error(error->detail);
    const auto pattern = std::get<application::FlatPatternPtr>(flat);
    return cad::reverseTabHints(cad::OcctFlatPatternAccess::layout(*pattern),
                                document->metadata().bendAnalysis, pattern->info().bends);
}

} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("die-window-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const auto file = folder / "part.step";
        STEPControl_Writer writer;
        check(writer.Transfer(fixtures::dieWindowPart(), STEPControl_AsIs) == IFSelect_RetDone &&
                  writer.Write(file.string().c_str()) == IFSelect_RetDone,
              "Fixture write");
        const auto doc = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
        check(bool(doc), "Fixture import");
        std::cout << "bends=" << doc->metadata().bendAnalysis.bends.size() << '\n';
        std::vector<std::size_t> joint;
        std::optional<std::size_t> tab;
        for (std::size_t i = 0; i < doc->metadata().bendAnalysis.bends.size(); ++i) {
            const auto &b = doc->metadata().bendAnalysis.bends[i];
            std::cout << i << " length=" << b.lengthMm << " axis=" << b.axisStart.x << ','
                      << b.axisStart.y << ',' << b.axisStart.z << '\n';
            if (b.lengthMm > 20)
                joint.push_back(i);
            else {
                check(!tab, "Expected one short central tab");
                tab = i;
            }
        }
        check(joint.size() == 2 && tab, "Missing two outer bend zones or the central tab");
        const auto reverse = hints(doc);
        check(reverse.size() == 1 && reverse.front().bend == *tab &&
                  reverse.front().neighbours ==
                      ((std::uint64_t{1} << joint[0]) | (std::uint64_t{1} << joint[1])),
              "An opposite bend between the two zones was not recognised");
        auto tools = tool_fixture::explicitPair(
            core::PressTools{16, std::numbers::pi / 3, 85 * std::numbers::pi / 180, .8, 120, 10});
        tools.punch.segments = std::vector<core::ToolSegment>{{120, 1}};
        tools.punchAssembly = core::ToolAssembly{{0}, 0};
        tools.die.segments = std::vector<core::ToolSegment>{{120, 1}, {45, 1}, {55, 1}, {10, 1}};
        tools.dieAssembly = core::ToolAssembly{{0}, 0};
        const auto &b = doc->metadata().bendAnalysis.bends[joint[0]];
        const auto prepared = cad::prepareBendOperation(
            doc, joint[0], {b.angleRadians, b.innerRadiusMm}, false, {},
            core::toolParameters(tools), true, tools, true, 0, std::nullopt, joint);
        if (const auto *e = std::get_if<application::BendPreviewError>(&prepared))
            throw std::runtime_error(e->detail);
        const auto cycle = std::get<cad::PreparedBendOperationPtr>(prepared)->at(0, {}, false);
        if (const auto *e = std::get_if<application::BendPreviewError>(&cycle)) {
            std::cout << "code=" << static_cast<int>(e->code)
                      << " pair=" << (e->toolCollision ? static_cast<int>(*e->toolCollision) : -1)
                      << " " << e->detail << '\n';
            if (e->dieWindow)
                std::cout << "window " << e->dieWindow->freeFromMm << ' '
                          << e->dieWindow->conflictFromMm << ' ' << e->dieWindow->conflictToMm
                          << ' ' << e->dieWindow->freeToMm << '\n';
        } else
            std::cout << "cycle passed\n";
        cad::SingleBendPreviewBuilder builder;
        application::BendRouteRequest q;
        q.automatic = q.withTools = true;
        q.allowWorkpieceRotation = false;
        q.maximumChecks = 96;
        q.maximumTimeMs = 120000;
        q.assemblySearch = application::RouteAssemblySearch{{0, 1, 2}, 20, true};
        for (std::size_t i = 0; i < doc->metadata().bendAnalysis.bends.size(); ++i) {
            const auto &bend = doc->metadata().bendAnalysis.bends[i];
            q.operations.push_back({"B" + std::to_string(i),
                                    i,
                                    {bend.angleRadians, bend.innerRadiusMm},
                                    core::toolParameters(tools),
                                    tools});
        }
        const auto source = std::string(doc->sourceStep());
        std::optional<std::size_t> firstAttempt;
        q.onProgress = [&](const auto &progress) {
            if (!firstAttempt && progress.phase == application::BendRoutePhase::Checking)
                firstAttempt = progress.bendIndex;
        };
        const auto route = builder.buildRoute(doc, q, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&route))
            throw std::runtime_error("route: " + e->detail);
        const auto r = std::get<application::BendRoutePtr>(route);
        std::cout << "route steps=" << r->steps.size()
                  << " windows=" << r->statistics.plannedDieWindows
                  << " attempts=" << r->evaluatedOperations << '\n';
        std::cout << "first attempt=" << firstAttempt.value_or(99)
                  << " transitions=" << r->statistics.builtTransitions << '\n';
        check(firstAttempt == reverse.front().bend,
              "The reverse tab was not tried first in the reverse search");
        auto unranked = q;
        unranked.rankToolAccess = false;
        std::optional<std::size_t> referenceFirst;
        unranked.onProgress = [&](const auto &progress) {
            if (!referenceFirst && progress.phase == application::BendRoutePhase::Checking)
                referenceFirst = progress.bendIndex;
        };
        const auto referenceRoute = cad::SingleBendPreviewBuilder{}.buildRoute(doc, unranked, {});
        check(std::holds_alternative<application::BendRoutePtr>(referenceRoute),
              "Reference window route did not pass");
        const auto reference = std::get<application::BendRoutePtr>(referenceRoute);
        std::cout << "reference first=" << referenceFirst.value_or(99)
                  << " transitions=" << reference->statistics.builtTransitions << '\n';
        check(r->statistics.builtTransitions <= reference->statistics.builtTransitions,
              "Tab priority added exact checks on the reverse-tab fixture");
        check(r->steps.size() == 2 && r->statistics.plannedDieWindows > 0,
              "The actual reverse-tab collision did not produce a bounded window route");
        auto resolved = q;
        check(application::applyRouteAssemblyResult(resolved, *r),
              "Derived window did not pass atomic acceptance");
        const auto windowStep =
            std::find_if(r->steps.begin(), r->steps.end(), [](const auto &step) {
                return step.assemblyAssignment && step.assemblyAssignment->dieAssembly &&
                       !step.assemblyAssignment->dieAssembly->jointGapsMm.empty();
            });
        check(windowStep != r->steps.end() && windowStep->additionalBends.size() == 1 &&
                  windowStep->assemblyAssignment->punchAssembly == tools.punchAssembly &&
                  windowStep->assemblyAssignment->die == tools.die &&
                  windowStep->preview->uncoveredToolLengthMm()[0] < 1.e-5 &&
                  windowStep->preview->uncoveredToolLengthMm()[1] < 1.e-5,
              "Window lost support, changed another tool or split the common operation");
        const auto warm = builder.buildRoute(doc, q, {});
        check(std::holds_alternative<application::BendRoutePtr>(warm) &&
                  std::get<application::BendRoutePtr>(warm)->statistics.builtTransitions == 0,
              "Repeated window search lost exact collision evidence or successful input keys");
        auto withoutWindows = q;
        withoutWindows.assemblySearch->allowDieWindows = false;
        withoutWindows.maximumChecks = 12;
        withoutWindows.maximumTimeMs = 30000;
        const auto baseline = builder.buildRoute(doc, withoutWindows, {});
        const auto windowPlans =
            std::holds_alternative<application::BendRoutePtr>(baseline)
                ? std::get<application::BendRoutePtr>(baseline)->statistics.plannedDieWindows
                : std::get<application::BendRouteError>(baseline).statistics.plannedDieWindows;
        check(windowPlans == 0, "Cached window leaked into the ordinary assembly-only search");
        if (const auto *baseRoute = std::get_if<application::BendRoutePtr>(&baseline))
            for (const auto &step : (*baseRoute)->steps)
                check(!step.assemblyAssignment ||
                          step.assemblyAssignment->dieAssembly->jointGapsMm.empty(),
                      "An unrequested window was reused through a previous successful path");
        auto denied = q;
        denied.assemblySearch->allowDieWindows = false;
        const auto deniedInputs = denied.operations;
        check(!application::applyRouteAssemblyResult(denied, *r) &&
                  denied.operations == deniedInputs,
              "Window applied without permission or partially changed the request");
        auto forged = *r;
        const auto at = static_cast<std::size_t>(windowStep - r->steps.begin());
        auto wrong = *forged.steps[at].assemblyAssignment;
        std::reverse(wrong.dieAssembly->pieces.begin(), wrong.dieAssembly->pieces.end());
        forged.steps[at].assemblyAssignment = std::make_shared<const core::ToolAssignment>(wrong);
        auto unchanged = q;
        check(!application::applyRouteAssemblyResult(unchanged, forged) &&
                  unchanged.operations == q.operations,
              "A stale same-length preview accepted a gap moved under required material");
        auto wrongMember = *r;
        auto &member = wrongMember.steps[at].additionalBends.front();
        member.assignment = wrong;
        member.tools = core::toolParameters(wrong);
        auto groupUnchanged = q;
        check(!application::applyRouteAssemblyResult(groupUnchanged, wrongMember) &&
                  groupUnchanged.operations == q.operations,
              "Different hardware on an additional group member was partially accepted");
        auto bounded = q;
        bounded.maximumChecks = 1;
        check(std::get<application::BendRouteError>(builder.buildRoute(doc, bounded, {})).code ==
                  application::BendRouteErrorCode::SearchLimit,
              "A placement/assembly limit published a route or impossibility verdict");
        auto noStock = q;
        noStock.maximumChecks = 12;
        noStock.maximumTimeMs = 30000;
        for (auto &op : noStock.operations) {
            for (std::size_t row = 1; row < op.assignment->die.segments->size(); ++row)
                op.assignment->die.segments->at(row).quantity = 0;
        }
        const auto unavailable = builder.buildRoute(doc, noStock, {});
        if (const auto *result = std::get_if<application::BendRoutePtr>(&unavailable))
            for (const auto &step : (*result)->steps)
                check(!step.assemblyAssignment ||
                          step.assemblyAssignment->dieAssembly->jointGapsMm.empty(),
                      "Window planner reused unavailable stock from another inventory snapshot");
        auto split = q;
        split.onProgress = {};
        split.allowWorkpieceRotation = true;
        for (auto &op : split.operations) {
            // Each flange can use a short assembly but there is no stock to
            // span the two outer bends with a permitted gap.
            op.assignment->punch.segments = std::vector<core::ToolSegment>{{55, 1}, {10, 1}};
            op.assignment->die.segments = std::vector<core::ToolSegment>{{55, 1}, {10, 1}};
            op.assignment->punchAssembly = op.assignment->dieAssembly =
                core::ToolAssembly{{op.bendIndex == reverse.front().bend ? 1u : 0u}, 0};
            op.tools = core::toolParameters(*op.assignment);
        }
        const auto splitRoute = builder.buildRoute(doc, split, {});
        if (const auto *error = std::get_if<application::BendRouteError>(&splitRoute))
            throw std::runtime_error("Separate route: " + error->detail);
        const auto splitChecked = std::get<application::BendRoutePtr>(splitRoute);
        std::cout << "separate transitions=" << splitChecked->statistics.builtTransitions << '\n';
        check(splitChecked->steps.size() == 3 && splitChecked->statistics.plannedDieWindows == 0 &&
                  splitChecked->statistics.builtTransitions == 3,
              "The search did not fall back to separate flanges with short stock");
        auto narrow = q;
        narrow.maximumChecks = 12;
        narrow.maximumTimeMs = 30000;
        narrow.assemblySearch->maximumGapMm = 19;
        const auto shorter = builder.buildRoute(doc, narrow, {});
        if (const auto *result = std::get_if<application::BendRoutePtr>(&shorter))
            for (const auto &step : (*result)->steps)
                check(!step.assemblyAssignment ||
                          core::maximumAssemblyGap(*step.assemblyAssignment->dieAssembly) <= 19,
                      "Collision window silently exceeded the user's gap limit");
        std::stop_source stop;
        stop.request_stop();
        check(std::get<application::BendRouteError>(builder.buildRoute(doc, q, stop.get_token()))
                          .code == application::BendRouteErrorCode::Cancelled &&
                  doc->sourceStep() == source,
              "Cancellation changed source or returned a route");
        // A plain cutout has no collision and must keep the continuous die.
        const auto cleanFile = folder / "cutout.step";
        STEPControl_Writer cleanWriter;
        check(cleanWriter.Transfer(fixtures::jointBend(), STEPControl_AsIs) == IFSelect_RetDone &&
                  cleanWriter.Write(cleanFile.string().c_str()) == IFSelect_RetDone,
              "Clean fixture write");
        const auto clean = application::resultDocument(cad::StepImporter{}.load(cleanFile, {}, {}));
        check(clean && hints(clean).empty(), "An empty relief was classified as a reverse tab");
        auto ordinary = q;
        ordinary.operations.clear();
        ordinary.assemblySearch->bendIndices = {0, 1};
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &bend = clean->metadata().bendAnalysis.bends.at(i);
            ordinary.operations.push_back({"B" + std::to_string(i),
                                           i,
                                           {bend.angleRadians, bend.innerRadiusMm},
                                           core::toolParameters(tools),
                                           tools});
        }
        const auto untouched = builder.buildRoute(clean, ordinary, {});
        check(std::holds_alternative<application::BendRoutePtr>(untouched) &&
                  std::get<application::BendRoutePtr>(untouched)->statistics.plannedDieWindows == 0,
              "A part cutout alone triggered a die window");
        for (const auto &step : std::get<application::BendRoutePtr>(untouched)->steps)
            check(!step.assemblyAssignment,
                  "A collision-free continuous die was unnecessarily split");
        gp_Trsf along;
        along.SetTranslation(gp_Vec(0, 0, 50));
        BRepAlgoAPI_Fuse sameDirection(
            fixtures::jointBend(),
            fixtures::bentStrip(4, 2, std::numbers::pi / 2, 10).Moved(TopLoc_Location(along)));
        sameDirection.SimplifyResult(true, true);
        const auto sameFile = folder / "same-direction.step";
        STEPControl_Writer sameWriter;
        check(sameDirection.IsDone() &&
                  sameWriter.Transfer(sameDirection.Shape(), STEPControl_AsIs) ==
                      IFSelect_RetDone &&
                  sameWriter.Write(sameFile.string().c_str()) == IFSelect_RetDone,
              "Same-direction fixture write");
        const auto same = application::resultDocument(cad::StepImporter{}.load(sameFile, {}, {}));
        check(same && same->metadata().bendAnalysis.bends.size() == 3 && hints(same).empty(),
              "A tab folded the same way triggered the reverse-tab hint");
        auto sameRequest = q;
        sameRequest.onProgress = {};
        for (auto &op : sameRequest.operations) {
            const auto &bend = same->metadata().bendAnalysis.bends.at(op.bendIndex);
            op.target = {bend.angleRadians, bend.innerRadiusMm};
        }
        const auto sameRoute = builder.buildRoute(same, sameRequest, {});
        if (const auto *error = std::get_if<application::BendRouteError>(&sameRoute))
            throw std::runtime_error("Same-direction route: " + error->detail);
        const auto sameChecked = std::get<application::BendRoutePtr>(sameRoute);
        check(sameChecked->statistics.plannedDieWindows == 0 && sameChecked->steps.size() <= 2,
              "A same-direction tab required an unnecessary window or extra stroke");
        gp_Trsf rotation;
        rotation.SetRotation(gp_Ax1(gp_Pnt(3, 8, -4), gp_Dir(1, 2, 3)), .61);
        const auto rotatedFile = folder / "rotated.step";
        STEPControl_Writer rotatedWriter;
        check(rotatedWriter.Transfer(
                  BRepBuilderAPI_Transform(fixtures::dieWindowPart(), rotation, true).Shape(),
                  STEPControl_AsIs) == IFSelect_RetDone &&
                  rotatedWriter.Write(rotatedFile.string().c_str()) == IFSelect_RetDone,
              "Rotated write");
        const auto rotated =
            application::resultDocument(cad::StepImporter{}.load(rotatedFile, {}, {}));
        check(rotated && hints(rotated).size() == 1,
              "Reverse tab detection depended on the STEP orientation");
        auto other = q;
        other.operations.clear();
        for (std::size_t i = 0; i < rotated->metadata().bendAnalysis.bends.size(); ++i) {
            const auto &bend = rotated->metadata().bendAnalysis.bends[i];
            other.operations.push_back({"B" + std::to_string(i),
                                        i,
                                        {bend.angleRadians, bend.innerRadiusMm},
                                        core::toolParameters(tools),
                                        tools});
        }
        const auto moved = builder.buildRoute(rotated, other, {});
        check(std::holds_alternative<application::BendRoutePtr>(moved) &&
                  std::get<application::BendRoutePtr>(moved)->statistics.plannedDieWindows > 0,
              "Window search depended on the source STEP world coordinates");
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
