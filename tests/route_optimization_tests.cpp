#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace bending;
using namespace bending::application;
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}
BendRouteRequest requestFor(const DocumentPtr &doc, bool tools) {
    BendRouteRequest request;
    request.automatic = true;
    request.withTools = request.allowPartialEndSupport = tools;
    request.maximumChecks = 512;
    request.maximumTimeMs = 120000;
    const auto &bends = doc->metadata().bendAnalysis.bends;
    for (std::size_t i = 0; i < bends.size(); ++i) {
        const auto &b = bends[i];
        request.operations.push_back(
            {std::to_string(i),
             i,
             {b.angleRadians, b.innerRadiusMm},
             tools ? std::optional<core::PressTools>{{8 * b.thicknessMm, std::numbers::pi / 3,
                                                      std::numbers::pi / 6, .8, b.lengthMm, 40}}
                   : std::nullopt,
             {}});
    }
    return request;
}
void verify(const BendRoutePtr &route, const DocumentPtr &doc, const BendRouteRequest &request) {
    require(route && route->source == doc && route->withTools == request.withTools &&
                route->steps.size() == request.operations.size(),
            "Route provenance/scope changed");
    std::set<std::size_t> indices;
    const auto expected = doc->metadata().volumeMm3 + doc->metadata().closedCutouts.addedVolumeMm3;
    for (std::size_t i = 0; i < route->steps.size(); ++i) {
        const auto &step = route->steps[i];
        const auto found =
            std::find_if(request.operations.begin(), request.operations.end(), [&](const auto &op) {
                return op.id == step.id && op.bendIndex == step.bendIndex;
            });
        require(found != request.operations.end() && indices.insert(step.bendIndex).second &&
                    step.preview && step.preview->otherFormedBends() == i &&
                    step.preview->frameCount() == (request.withTools ? 61u : 21u),
                "Incomplete/duplicated route or lost forming prefix");
        if (!request.automatic)
            require(step.id == request.operations[i].id, "Ranking changed the manual order");
        if (request.withTools)
            require(step.preview->toolSetup() && step.preview->punchRetraction() &&
                        step.preview->toolSetup()->tools == *found->tools,
                    "Cached cycle has the wrong tool or unverified retraction");
        for (auto f :
             {std::size_t{0}, step.preview->frameCount() / 2, step.preview->frameCount() - 1}) {
            const auto &frame = cad::OcctPreviewAccess::frame(*step.preview, f);
            double material = 0;
            for (const auto &region : frame.regions)
                material += volume(region);
            require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                        std::abs(material - expected) <= std::max(.02, expected * 1.e-7),
                    "Route lost material or visible skin");
            if (doc->metadata().closedCutouts.status == ClosedCutoutStatus::Filled)
                require(!frame.displayShape.IsNull(), "Route lost the visible cutout skin");
        }
    }
}
BendRouteResult run(cad::SingleBendPreviewBuilder &builder, const DocumentPtr &doc,
                    const BendRouteRequest &request, const char *name) {
    const auto started = std::chrono::steady_clock::now();
    auto result = builder.buildRoute(doc, request, {});
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    const auto &stats = std::holds_alternative<BendRoutePtr>(result)
                            ? std::get<BendRoutePtr>(result)->statistics
                            : std::get<BendRouteError>(result).statistics;
    std::cout << name << " ms=" << ms << " states=" << stats.builtStates
              << " state-hits=" << stats.reusedStates << " transitions=" << stats.builtTransitions
              << " transition-hits=" << stats.reusedTransitions
              << " rejection-hits=" << stats.reusedRejections
              << " flat-hit=" << stats.reusedFlatPattern << " reordered=" << stats.reorderedStates
              << std::endl;
    if (const auto *success = std::get_if<BendRoutePtr>(&result))
        verify(*success, doc, request);
    else {
        const auto &e = std::get<BendRouteError>(result);
        std::cout << "error=" << static_cast<int>(e.code) << " " << e.detail << std::endl;
    }
    return result;
}
BendRoutePtr success(const BendRouteResult &result) {
    if (const auto *error = std::get_if<BendRouteError>(&result))
        throw std::runtime_error(error->detail);
    return std::get<BendRoutePtr>(result);
}
void equivalent(const BendRouteResult &a, const BendRouteResult &b) {
    require(a.index() == b.index(), "Optimization changed feasibility against reference search");
    if (const auto *error = std::get_if<BendRouteError>(&a))
        require(error->code == std::get<BendRouteError>(b).code &&
                    error->code != BendRouteErrorCode::SearchLimit,
                "Diagnostic changed or comparison did not finish");
}
void verdictCache(const std::filesystem::path &folder, bool measureOnly) {
    const auto file = folder / "branched.step";
    STEPControl_Writer writer;
    require(writer.Transfer(fixtures::branchedTray(), STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(file.string().c_str()) == IFSelect_RetDone,
            "Write failed");
    const auto doc = resultDocument(cad::StepImporter{}.load(file, {}, {}));
    require(bool(doc), "Import failed");
    const auto source = std::string(doc->sourceStep());
    auto request = requestFor(doc, true);
    request.rankCandidates = false; // Identical traversal makes the before/after count comparable.
    require(request.operations.size() == 5, "Missing branched fixture bends");
    for (auto &op : request.operations) {
        auto parameters = *op.tools;
        parameters.noseRadiusMm = 3; // Too large for every R2 bend, on either workpiece side.
        auto pair = tool_fixture::explicitPair(parameters);
        op.assignment = pair;
        op.tools = core::toolParameters(pair);
    }
    auto referenceRequest = request;
    referenceRequest.reuseCalculations = false;
    cad::SingleBendPreviewBuilder reference, optimized;
    const auto baseline = run(reference, doc, referenceRequest, "rejections-reference");
    require(std::holds_alternative<BendRouteError>(baseline) &&
                std::get<BendRouteError>(baseline).code == BendRouteErrorCode::NoRoute &&
                std::get<BendRouteError>(baseline).evaluatedOperations == 10,
            "Fixture must reject both sides of every final bend");
    const auto cold = run(optimized, doc, request, "rejections-cold");
    equivalent(baseline, cold);
    if (!measureOnly)
        require(std::get<BendRouteError>(cold).statistics.builtStates == 0,
                "Rejected cycles built unused predecessor geometry");
    const auto warm = run(optimized, doc, request, "rejections-warm");
    equivalent(baseline, warm);
    const auto &stats = std::get<BendRouteError>(warm).statistics;
    if (!measureOnly)
        require(stats.builtTransitions == 0 && stats.reusedTransitions == 10 &&
                    stats.reusedRejections == 10 && stats.builtStates == 0,
                "Rejections were lost when the heavy preview cache filled");
    auto changed = request;
    changed.operations.front().assignment->clearanceMm += 1;
    changed.operations.front().tools = core::toolParameters(*changed.operations.front().assignment);
    auto freshChanged = changed;
    freshChanged.reuseCalculations = false;
    const auto changedReference = run(reference, doc, freshChanged, "changed-rejection-reference");
    const auto changedCached = run(optimized, doc, changed, "changed-rejection");
    equivalent(changedReference, changedCached);
    if (!measureOnly)
        require(std::get<BendRouteError>(changedCached).statistics.builtTransitions == 2 &&
                    std::get<BendRouteError>(changedCached).statistics.reusedTransitions == 8 &&
                    std::get<BendRouteError>(changedCached).statistics.reusedRejections == 8,
                "Changed inputs reused an old rejection or lost unrelated rejections");
    auto limited = request;
    limited.maximumChecks = 1;
    require(std::get<BendRouteError>(optimized.buildRoute(doc, limited, {})).code ==
                BendRouteErrorCode::SearchLimit,
            "Negative cache bypassed the candidate budget");
    equivalent(baseline, run(optimized, doc, request, "after-limited-rejections"));
    require(doc->sourceStep() == source, "Source was modified");
}
void backgaugeCache(const std::filesystem::path &folder, bool measureOnly) {
    const auto file = folder / "gauged.step";
    STEPControl_Writer writer;
    require(writer.Transfer(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 300, true, 60),
                            STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(file.string().c_str()) == IFSelect_RetDone,
            "Write gauged fixture failed");
    const auto doc = resultDocument(cad::StepImporter{}.load(file, {}, {}));
    require(doc && doc->metadata().bendAnalysis.bends.size() == 1, "Gauge fixture import failed");
    auto pair = tool_fixture::tangPair();
    pair.punch.lengthMm = pair.die.lengthMm = 300;
    pair.backgauge.emplace();
    pair.backgauge->xMinMm = 900;
    pair.backgauge->xMaxMm = 1000;
    const auto source = std::string(doc->sourceStep());
    auto request = requestFor(doc, true);
    request.rankCandidates = false;
    request.allowWorkpieceRotation = false;
    request.operations[0].assignment = pair;
    request.operations[0].tools = core::toolParameters(pair);
    cad::SingleBendPreviewBuilder builder;
    for (unsigned i = 0; i < 4; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = builder.buildRoute(doc, request, {});
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        const auto *failure = std::get_if<BendRouteError>(&result);
        require(failure && failure->code == BendRouteErrorCode::NoRoute &&
                    failure->operationError == BendPreviewErrorCode::BackgaugeUnavailable,
                "Gauge fixture did not fail specifically on reference search");
        const auto &stats = failure->statistics;
        std::cout << "gauge-rejection run=" << i << " us=" << us
                  << " built=" << stats.builtTransitions << " hits=" << stats.reusedRejections
                  << " contacts=" << stats.backgauge.contactSearches
                  << " contact-us=" << stats.backgauge.contactTimeUs
                  << " collision-checks=" << stats.backgauge.collisionChecks << std::endl;
        if (!measureOnly) {
            if (!i)
                require(stats.backgauge.contactSearches == 1 &&
                            stats.backgauge.noReferenceTransitions == 1 &&
                            stats.backgauge.reusedNoReferenceTransitions == 0 &&
                            stats.backgauge.collisionChecks == 0,
                        "Cold missing-reference work was not reported correctly");
            else
                require(
                    stats.builtTransitions == 0 && stats.reusedRejections == 1 &&
                        stats.backgauge.reusedNoReferenceTransitions == 1 &&
                        stats.backgauge.noReferenceTransitions == 0 &&
                        stats.backgauge.contactSearches == 0 && stats.backgauge.candidates == 0 &&
                        stats.backgauge.exposureChecks == 0 &&
                        stats.backgauge.placementTrials == 0 &&
                        stats.backgauge.collisionChecks == 0 &&
                        stats.backgauge.contactTimeUs == 0 && stats.backgauge.collisionTimeUs == 0,
                    "Exact gauge rejection was not reused or reported historical work");
        }
    }
    if (measureOnly)
        return;
    const auto noReference = [&](const BendRouteRequest &r) {
        const auto result = builder.buildRoute(doc, r, {});
        const auto *failure = std::get_if<BendRouteError>(&result);
        require(failure && failure->code == BendRouteErrorCode::NoRoute &&
                    failure->operationError == BendPreviewErrorCode::BackgaugeUnavailable,
                "Changed gauge fixture did not reach reference search");
        return failure->statistics;
    };
    const auto freshThenWarm = [&](const BendRouteRequest &changed) {
        const auto fresh = noReference(changed);
        require(fresh.builtTransitions == 1 && fresh.reusedRejections == 0 &&
                    fresh.backgauge.contactSearches == 1,
                "Changed inputs reused an unrelated gauge verdict");
        const auto warm = noReference(changed);
        require(warm.builtTransitions == 0 && warm.backgauge.contactSearches == 0 &&
                    warm.backgauge.reusedNoReferenceTransitions == 1,
                "Changed exact-input verdict was not retained");
    };
    auto orientation = request;
    orientation.operations[0].workpieceReversed = true;
    freshThenWarm(orientation);
    for (auto field : {&core::BackgaugeSetup::xMinMm, &core::BackgaugeSetup::xMaxMm,
                       &core::BackgaugeSetup::rMinMm, &core::BackgaugeSetup::rMaxMm,
                       &core::BackgaugeSetup::zMinMm, &core::BackgaugeSetup::zMaxMm,
                       &core::BackgaugeSetup::fingerWidthMm,
                       &core::BackgaugeSetup::minimumSpacingMm, &core::BackgaugeSetup::retractMm}) {
        auto changed = request;
        (*changed.operations[0].assignment->backgauge).*field += .5;
        changed.operations[0].tools = core::toolParameters(*changed.operations[0].assignment);
        freshThenWarm(changed);
    }
    auto changed = request;
    changed.operations[0].assignment->backgauge->allowOffsetContacts = false;
    changed.operations[0].tools = core::toolParameters(*changed.operations[0].assignment);
    freshThenWarm(changed);
    changed = request;
    changed.operations[0].assignment->punchReversed = !pair.punchReversed;
    changed.operations[0].tools = core::toolParameters(*changed.operations[0].assignment);
    freshThenWarm(changed);
    changed = request;
    changed.operations[0].assignment->punch.lengthMm += 1;
    changed.operations[0].assignment->die.lengthMm += 1;
    changed.operations[0].tools = core::toolParameters(*changed.operations[0].assignment);
    freshThenWarm(changed);
    auto limited = request;
    limited.allowWorkpieceRotation = true;
    limited.maximumChecks = 1;
    require(std::get<BendRouteError>(builder.buildRoute(doc, limited, {})).code ==
                BendRouteErrorCode::SearchLimit,
            "Gauge verdict cache bypassed the operation budget");
    require(noReference(request).backgauge.reusedNoReferenceTransitions == 1,
            "Limited search destroyed a valid exact-input memo");
    std::stop_source stop;
    stop.request_stop();
    require(std::get<BendRouteError>(builder.buildRoute(doc, request, stop.get_token())).code ==
                BendRouteErrorCode::Cancelled,
            "Gauge verdict cache bypassed cancellation");
    // Cancellation keeps the existing contract: discard worker-owned route data.
    freshThenWarm(request);
    auto validPair = pair;
    validPair.backgauge = core::BackgaugeSetup{};
    auto variants = request;
    variants.toolVariants = {{pair, {}}, {validPair, {}}};
    const auto checked = success(builder.buildRoute(doc, variants, {}));
    require(checked->selectedToolVariant == 1 && checked->statistics.testedToolVariants == 2 &&
                checked->statistics.backgauge.reusedNoReferenceTransitions == 1 &&
                checked->statistics.backgauge.contactSearches == 2 &&
                checked->statistics.backgauge.candidates > 0 &&
                checked->statistics.backgauge.exposureChecks >= 2 &&
                checked->statistics.backgauge.placementTrials > 0 &&
                checked->statistics.backgauge.collisionChecks >= 65,
            "Tool variants lost gauge work statistics or reused the wrong rejection");
    const auto cached = success(builder.buildRoute(doc, variants, {}));
    require(cached->statistics.builtTransitions == 0 &&
                cached->statistics.backgauge.contactSearches == 0 &&
                cached->statistics.backgauge.collisionChecks == 0 &&
                cached->statistics.backgauge.reusedNoReferenceTransitions == 1,
            "Cached success reported historical contact/collision work");
    auto referenceRequest = request;
    referenceRequest.operations[0].assignment = validPair;
    referenceRequest.operations[0].tools = core::toolParameters(validPair);
    referenceRequest.reuseCalculations = false;
    cad::SingleBendPreviewBuilder reference;
    const auto control = success(reference.buildRoute(doc, referenceRequest, {}));
    const auto &actual = *checked->steps.front().preview;
    const auto &expected = *control->steps.front().preview;
    require(actual.frameCount() == 65 && expected.frameCount() == actual.frameCount() &&
                actual.backgauge()->status == BackgaugeStatus::Checked &&
                actual.backgauge()->placement == expected.backgauge()->placement &&
                actual.punchRetraction() && expected.punchRetraction() &&
                checked->steps.front().fixedSecond == control->steps.front().fixedSecond,
            "Cached/no-cache routes differ in gauge placement, scope or orientation");
    for (std::size_t i = 0; i < actual.frameCount(); ++i)
        require(actual.collisionAtFrame(i) == expected.collisionAtFrame(i) &&
                    !actual.machineCollisionAtFrame(i) && !expected.machineCollisionAtFrame(i),
                "Gauge optimization changed collision flags");
    for (const auto i : {std::size_t{0}, actual.frameCount() / 2, actual.frameCount() - 1}) {
        const auto &a = cad::OcctPreviewAccess::frame(actual, i);
        const auto &b = cad::OcctPreviewAccess::frame(expected, i);
        require(BRepCheck_Analyzer(a.shape).IsValid() && BRepCheck_Analyzer(b.shape).IsValid() &&
                    std::abs(volume(a.shape) - volume(b.shape)) < .001 &&
                    a.machine.size() == b.machine.size() &&
                    a.backgaugeContacts.size() == b.backgaugeContacts.size(),
                "Gauge optimization changed material or prepared obstacles");
    }
    changed = request;
    changed.operations[0].assignment->backgauge.reset();
    changed.operations[0].tools = core::toolParameters(*changed.operations[0].assignment);
    const auto withoutGauge = success(builder.buildRoute(doc, changed, {}));
    require(withoutGauge->steps.front().preview->frameCount() == 61 &&
                !withoutGauge->steps.front().preview->backgauge() &&
                withoutGauge->statistics.backgauge.contactSearches == 0,
            "Disabling the gauge reused an old reference rejection");
    const auto newDoc = resultDocument(cad::StepImporter{}.load(file, {}, {}));
    const auto newSource = std::get<BendRouteError>(builder.buildRoute(newDoc, request, {}));
    require(newSource.statistics.builtTransitions == 1 &&
                newSource.statistics.backgauge.reusedNoReferenceTransitions == 0,
            "New import reused another document's gauge verdict");
    require(doc->sourceStep() == source, "Gauge optimization changed the captured STEP");
    std::cout << "gauge-success contacts=" << checked->statistics.backgauge.contactSearches
              << " placements=" << checked->statistics.backgauge.placementTrials
              << " collision-checks=" << checked->statistics.backgauge.collisionChecks
              << " contact-us=" << checked->statistics.backgauge.contactTimeUs
              << " collision-us=" << checked->statistics.backgauge.collisionTimeUs << std::endl;
}
} // namespace
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("route-optimization-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        if (argc > 1 && (std::string_view(argv[1]) == "--backgauge-cache" ||
                         std::string_view(argv[1]) == "--measure-backgauge-cache")) {
            backgaugeCache(folder, std::string_view(argv[1]) == "--measure-backgauge-cache");
            std::filesystem::remove_all(folder);
            return 0;
        }
        if (argc > 1 && (std::string_view(argv[1]) == "--verdict-cache" ||
                         std::string_view(argv[1]) == "--measure-verdict-cache")) {
            verdictCache(folder, std::string_view(argv[1]) == "--measure-verdict-cache");
            std::filesystem::remove_all(folder);
            return 0;
        }
        const bool branch = argc > 1 && std::string_view(argv[1]) == "--branch";
        const bool sample = argc > 1 && !branch;
        const bool tools = sample && argc > 2 && std::string_view(argv[2]) == "--tools";
        const auto file = sample ? std::filesystem::path(argv[1]) : folder / "source.step";
        if (!sample) {
            STEPControl_Writer writer;
            require(writer.Transfer(branch ? fixtures::branchedTray() : fixtures::channel(),
                                    STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Write failed");
        }
        const auto doc = resultDocument(cad::StepImporter{}.load(file, {}, {}));
        require(bool(doc), "Import failed");
        const auto bytes = std::string(doc->sourceStep());
        auto request = requestFor(doc, tools);
        if (branch) {
            // Put the most connected bend first in reverse legacy traversal, so ranking
            // must actually change candidate ordering rather than merely increment a flag.
            const auto flat = std::get<FlatPatternPtr>(cad::FlatPatternBuilder{}.build(
                doc, {{core::SheetMaterial::Unknown, .5, 1}, {}}, {}));
            const auto &layout = cad::OcctFlatPatternAccess::layout(*flat);
            const auto degree = [&](std::size_t index) {
                const auto &a = *std::find_if(layout.bends.begin(), layout.bends.end(),
                                              [&](const auto &b) { return b.bendIndex == index; });
                return std::count_if(layout.bends.begin(), layout.bends.end(), [&](const auto &b) {
                    return b.bendIndex != index && (a.parent == b.parent || a.parent == b.child ||
                                                    a.child == b.parent || a.child == b.child);
                });
            };
            const auto most = std::max_element(request.operations.begin(), request.operations.end(),
                                               [&](const auto &a, const auto &b) {
                                                   return degree(a.bendIndex) < degree(b.bendIndex);
                                               });
            std::iter_swap(most, request.operations.end() - 1);
        }
        auto referenceRequest = request;
        referenceRequest.rankCandidates = referenceRequest.reuseCalculations = false;
        cad::SingleBendPreviewBuilder reference, optimized;
        const auto baseline = run(reference, doc, referenceRequest, "reference");
        const auto cold = run(optimized, doc, request, "cold");
        equivalent(baseline, cold);
        const auto route = success(cold);
        if (branch)
            require(route->statistics.reorderedStates > 0,
                    "Topology did not change candidate order");
        if (!sample)
            require(!route->statistics.usedProfilePriority,
                    "A small or branched sheet was treated as a long profile");
        const auto warm = success(run(optimized, doc, request, "warm"));
        require(warm->statistics.builtTransitions == 0 && warm->statistics.builtStates == 0 &&
                    warm->statistics.reusedTransitions == request.operations.size() &&
                    warm->statistics.reusedFlatPattern,
                "Warm query recomputed verified work");
        auto manual = request;
        manual.automatic = false;
        manual.operations.clear();
        for (const auto &step : route->steps)
            manual.operations.push_back(
                *std::find_if(request.operations.begin(), request.operations.end(),
                              [&](const auto &op) { return op.id == step.id; }));
        auto uncachedManual = manual;
        uncachedManual.rankCandidates = uncachedManual.reuseCalculations = false;
        equivalent(run(reference, doc, uncachedManual, "manual-reference"),
                   run(optimized, doc, manual, "manual-reused"));
        auto limited = request;
        limited.maximumChecks = 1;
        const auto limit = optimized.buildRoute(doc, limited, {});
        require(std::get<BendRouteError>(limit).code == BendRouteErrorCode::SearchLimit,
                "Cache bypassed the candidate budget");
        if (sample || branch) {
            require(doc->sourceStep() == bytes, "Source was modified");
            std::filesystem::remove_all(folder);
            return 0;
        }
        if (!sample && !branch) {
            auto withTools = requestFor(doc, true);
            withTools.automatic = false;
            const auto toolRoute = success(run(optimized, doc, withTools, "tools-cold"));
            require(toolRoute->statistics.reusedFlatPattern &&
                        toolRoute->statistics.builtTransitions > 0,
                    "Mode change reused a kinematic cycle as a tool cycle");
            const auto toolWarm = success(run(optimized, doc, withTools, "tools-warm"));
            require(toolWarm->statistics.builtTransitions == 0, "Tool cycles were not reused");
            auto changed = withTools;
            changed.operations.front().tools->clearanceMm += 1;
            const auto changedRoute = success(run(optimized, doc, changed, "changed-clearance"));
            require(changedRoute->statistics.builtTransitions == 1 &&
                        changedRoute->statistics.reusedTransitions ==
                            withTools.operations.size() - 1,
                    "Changed tool inputs reused an invalid transition or discarded unrelated work");
            // Same working dimensions, different actual contour: this must miss the old proof.
            const auto pair = tool_fixture::pair(16);
            auto drawn = withTools;
            for (auto &op : drawn.operations) {
                op.assignment = pair;
                op.tools = core::toolParameters(pair);
            }
            success(run(optimized, doc, drawn, "drawn-profile"));
            auto blocked = drawn;
            blocked.operations.front().assignment = tool_fixture::pair(16, .8, 100);
            require(blocked.operations.front().tools == drawn.operations.front().tools,
                    "The silhouette-only regression changed working dimensions");
            auto fresh = blocked;
            fresh.rankCandidates = fresh.reuseCalculations = false;
            const auto blockedBaseline = run(reference, doc, fresh, "collision-reference");
            require(std::holds_alternative<BendRouteError>(blockedBaseline),
                    "Missing collision fixture");
            equivalent(blockedBaseline, run(optimized, doc, blocked, "collision-cold"));
            const auto blockedWarm = run(optimized, doc, blocked, "collision-warm");
            equivalent(blockedBaseline, blockedWarm);
            require(std::get<BendRouteError>(blockedWarm).statistics.reusedTransitions > 0,
                    "Deterministic rejection was not reused");
            success(run(optimized, doc, drawn, "unblocked-profile"));
            // More keys than the retained transition capacity: old work may be evicted,
            // but fresh validation must still produce the same valid geometry.
            for (unsigned i = 0; i < 9; ++i) {
                auto variation = withTools;
                variation.operations.front().tools->clearanceMm += 2 + i;
                success(run(optimized, doc, variation, "bounded-cache-variation"));
            }
            const auto evicted = success(run(optimized, doc, withTools, "evicted-query"));
            require(evicted->statistics.builtTransitions > 0, "Transition retention is unbounded");
        }
        auto thickness = request;
        thickness.manualThicknessMm = doc->metadata().bendAnalysis.bends.front().thicknessMm;
        const auto explicitThickness =
            success(run(optimized, doc, thickness, "changed-thickness-origin"));
        require(!explicitThickness->statistics.reusedFlatPattern &&
                    explicitThickness->statistics.reusedTransitions == 0,
                "Thickness key was ignored");
        const auto importedAgain = resultDocument(cad::StepImporter{}.load(file, {}, {}));
        const auto newSource = success(run(optimized, importedAgain, request, "new-document"));
        require(!newSource->statistics.reusedFlatPattern &&
                    newSource->statistics.reusedTransitions == 0,
                "A new imported document inherited another document's proof");
        std::stop_source stop;
        stop.request_stop();
        require(
            std::get<BendRouteError>(optimized.buildRoute(importedAgain, request, stop.get_token()))
                    .code == BendRouteErrorCode::Cancelled,
            "Warm cache defeated cancellation");
        require(!success(run(optimized, importedAgain, request, "after-cancel"))
                     ->statistics.reusedFlatPattern,
                "Cancelled request retained route work");
        optimized.discardRouteCache();
        require(!success(run(optimized, importedAgain, request, "after-discard"))
                     ->statistics.reusedFlatPattern,
                "Explicit cache invalidation did not release the workset");
        require(doc->sourceStep() == bytes, "Source was modified");
        std::filesystem::remove_all(folder);
        std::cout << "Reference equivalence, cache keys, bounds, cancellation and source lifetime "
                     "passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
