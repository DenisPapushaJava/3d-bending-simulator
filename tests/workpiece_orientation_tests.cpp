#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/application/BendPreparation.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
using namespace bending;
using namespace bending::application;
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(shape, p);
    return std::abs(p.Mass());
}
TopoDS_Shape material(const cad::PreviewFrame &frame) {
    if (frame.regions[0].IsNull())
        return frame.shape; // Draft stores a complete material compound.
    BRep_Builder builder;
    TopoDS_Compound result;
    builder.MakeCompound(result);
    for (const auto &region : frame.regions)
        builder.Add(result, region);
    return result;
}
void same(const TopoDS_Shape &a, const TopoDS_Shape &b) {
    for (bool reverse : {false, true}) {
        BRepAlgoAPI_Cut cut(reverse ? b : a, reverse ? a : b);
        if (!cut.IsDone() || cut.HasErrors() || volume(cut.Shape()) >= 1.e-4) {
            Bnd_Box x, y;
            BRepBndLib::AddOptimal(a, x, false, false);
            BRepBndLib::AddOptimal(b, y, false, false);
            std::cerr << "difference=" << volume(cut.Shape()) << " A=" << x.CornerMin().X() << ","
                      << x.CornerMin().Y() << "," << x.CornerMin().Z() << " .. "
                      << x.CornerMax().X() << "," << x.CornerMax().Y() << "," << x.CornerMax().Z()
                      << " B=" << y.CornerMin().X() << "," << y.CornerMin().Y() << ","
                      << y.CornerMin().Z() << " .. " << y.CornerMax().X() << ","
                      << y.CornerMax().Y() << "," << y.CornerMax().Z() << std::endl;
            throw std::runtime_error("Different occupied volumes");
        }
    }
}
BendPreviewPtr preview(const BendPreviewResult &r) {
    if (auto *e = std::get_if<BendPreviewError>(&r))
        throw std::runtime_error(e->detail);
    return std::get<BendPreviewPtr>(r);
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("workpiece-orientation-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const auto load = [&](TopoDS_Shape shape, const char *name) {
            const auto file = folder / name;
            STEPControl_Writer w;
            require(w.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                        w.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Cannot write fixture");
            auto doc = resultDocument(cad::StepImporter{}.load(file, {}, {}));
            require(bool(doc), "Cannot import fixture");
            return doc;
        };
        // Unequal flanges make an orientation error observable even with a symmetric nose.
        auto shape = BRepAlgoAPI_Cut(fixtures::bentStrip(),
                                     BRepPrimAPI_MakeBox(gp_Pnt(-31, 3, -1), 16, 4, 82))
                         .Shape();
        const auto doc = load(shape, "asymmetric.step");
        require(doc->metadata().bendAnalysis.bends.size() == 1, "Missing asymmetric bend");
        const auto &b = doc->metadata().bendAnalysis.bends[0];
        const core::BendTarget target{b.angleRadians, b.innerRadiusMm};
        cad::SingleBendPreviewBuilder builder;
        std::array<BendPreviewPtr, 4> poses;
        for (int part = 0; part < 2; ++part)
            for (int punch = 0; punch < 2; ++punch) {
                auto tool = tool_fixture::pair(16);
                tool.punchReversed = bool(punch);
                const auto started = std::chrono::steady_clock::now();
                auto result =
                    builder.buildAssignedToolMotion(doc, 0, target, tool, {}, 3, bool(part));
                std::cout << "cold part=" << part << " punch=" << punch << " ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count()
                          << std::endl;
                auto p = preview(result);
                require(p->workpieceReversed() == bool(part) &&
                            p->toolSetup()->tools.punchReversed == bool(punch),
                        "Orientations coupled");
                poses[part * 2 + punch] = p;
            }
        for (int part = 0; part < 2; ++part) {
            auto tool = tool_fixture::pair(16);
            const auto started = std::chrono::steady_clock::now();
            require(preview(builder.buildAssignedToolMotion(doc, 0, target, tool, {}, 3,
                                                            bool(part))) == poses[part * 2],
                    "Warm rotation recomputed its checked frames");
            std::cout << "warm part=" << part << " us="
                      << std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count()
                      << std::endl;
            for (std::size_t f = 0; f < 61; ++f) {
                const auto &a = cad::OcctPreviewAccess::frame(*poses[part * 2], f),
                           &r = cad::OcctPreviewAccess::frame(*poses[part * 2 + 1], f);
                for (int region = 0; region < 2; ++region)
                    for (gp_Pnt q : {gp_Pnt(0, 0, 0), gp_Pnt(21, 32, 43)})
                        require(q.Transformed(a.materialTransforms[region])
                                        .Distance(q.Transformed(r.materialTransforms[region])) <
                                    1.e-7,
                                "Punch rotated workpiece");
            }
        }
        gp_Trsf turn;
        turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 40), gp_Dir(0, 1, 0)), std::numbers::pi);
        for (std::size_t f : {0u, 30u, 60u}) {
            const auto &a = cad::OcctPreviewAccess::frame(*poses[0], f),
                       &r = cad::OcctPreviewAccess::frame(*poses[2], f);
            same(BRepBuilderAPI_Transform(material(a), turn, false).Shape(), material(r));
            same(a.tools[0], r.tools[0]);
            same(a.tools[1], r.tools[1]);
            Bnd_Box before, after;
            BRepBndLib::AddOptimal(material(a), before, false, false);
            BRepBndLib::AddOptimal(material(r), after, false, false);
            require(std::abs(before.CornerMin().Y() - after.CornerMin().Y()) < 1.e-7,
                    "Y rotation changed floor clearance");
        }
        BendRouteRequest outside;
        outside.automatic = true;
        outside.withTools = true;
        auto simpleTool = tool_fixture::pair(16);
        outside.operations.push_back(
            {"bend", 0, target, core::toolParameters(simpleTool), simpleTool});
        gp_Trsf rotateSource;
        rotateSource.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 1, 0)), std::numbers::pi);
        const auto opposite =
            load(BRepBuilderAPI_Transform(shape, rotateSource, true).Shape(), "opposite.step");
        std::array<bool, 2> defaults{};
        std::size_t variant = 0;
        for (const auto &source : {doc, opposite}) {
            const auto &analysis = source->metadata().bendAnalysis;
            require(analysis.bends.size() == 1, "Missing bend for initial setup");
            std::vector<BendPreparation> entries;
            (void)restoreBendPreparations(analysis, entries);
            require(entries.size() == 1, "Missing new bend preparation");
            defaults[variant++] = entries.front().workpieceReversed;
            RouteDraftRequest initial;
            initial.withTools = true;
            initial.operations = outside.operations;
            initial.operations.front().workpieceReversed = entries.front().workpieceReversed;
            const auto result = builder.buildRouteDraft(source, initial, {});
            require(std::holds_alternative<RouteDraftPtr>(result), "Initial draft unavailable");
            Bnd_Box bounds;
            BRepBndLib::AddOptimal(material(cad::OcctPreviewAccess::frame(
                                       *std::get<RouteDraftPtr>(result)->visual, 0)),
                                   bounds, false, false);
            require(-bounds.CornerMin().X() > bounds.CornerMax().X() + 5,
                    "Initial draft did not put the long side toward the operator");
            entries.front().workpieceReversed = !entries.front().workpieceReversed;
            const auto bindings = restoreBendPreparations(analysis, entries);
            require(bindings.byRecognizedBend.front() == 0 &&
                        entries.front().workpieceReversed != defaults[variant - 1],
                    "Restoring an explicit orientation overwrote it with the default");
        }
        require(defaults[0] != defaults[1], "Opposite STEP directions used the same default");
        auto outsideResult = builder.buildRoute(doc, outside, {});
        require(std::holds_alternative<BendRoutePtr>(outsideResult),
                "Outside preference lost a valid route");
        const auto oriented = std::get<BendRoutePtr>(outsideResult)->steps.front().preview;
        Bnd_Box outsideBounds;
        BRepBndLib::AddOptimal(material(cad::OcctPreviewAccess::frame(*oriented, 0)), outsideBounds,
                               false, false);
        require(-outsideBounds.CornerMin().X() > outsideBounds.CornerMax().X() + 5,
                "Automatic placement did not prefer the long side toward the operator");
        // A long narrow tongue has less material than the shorter full-width wall.
        // Overhang alone would put the smaller side toward the operator.
        auto tongue = BRepAlgoAPI_Cut(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 80, true, 60),
                                      BRepPrimAPI_MakeBox(gp_Pnt(3, -61, -1), 4, 31, 82))
                          .Shape();
        for (double z : {-1., 45.})
            tongue =
                BRepAlgoAPI_Cut(tongue, BRepPrimAPI_MakeBox(gp_Pnt(-61, 3, z), 46, 4, 36)).Shape();
        const auto tongueDoc = load(tongue, "tongue.step");
        require(tongueDoc->metadata().bendAnalysis.bends.size() == 1, "Tongue bend missing");
        const auto &tb = tongueDoc->metadata().bendAnalysis.bends.front();
        auto tongueRequest = outside;
        tongueRequest.operations.front().target = {tb.angleRadians, tb.innerRadiusMm};
        const auto tongueResult = builder.buildRoute(tongueDoc, tongueRequest, {});
        require(std::holds_alternative<BendRoutePtr>(tongueResult), "Tongue route unavailable");
        const auto tongueMaterial = material(cad::OcctPreviewAccess::frame(
            *std::get<BendRoutePtr>(tongueResult)->steps.front().preview, 0));
        BRepAlgoAPI_Common frontHalf(
            tongueMaterial, BRepPrimAPI_MakeBox(gp_Pnt(-1000, -1000, -1000), 1000, 2000, 2000));
        require(frontHalf.IsDone() && !frontHalf.HasErrors() &&
                    volume(frontHalf.Shape()) > .55 * volume(tongueMaterial),
                "Handling preference chose a long tongue over the larger material side");
        // An asymmetric tool blocks just one placement; both outcomes must be retained.
        auto blockedTool = tool_fixture::pair(16, .8, 100);
        std::array<bool, 2> hits{};
        for (int part = 0; part < 2; ++part) {
            RouteDraftRequest request;
            request.withTools = true;
            request.step = 1;
            request.operations.push_back(
                {"bend", 0, target, core::toolParameters(blockedTool), blockedTool, bool(part)});
            auto result = builder.buildRouteDraft(doc, request, {});
            require(std::holds_alternative<RouteDraftPtr>(result), "Draft rotation failed");
            auto p = std::get<RouteDraftPtr>(result)->visual;
            for (std::size_t f = 0; f < p->frameCount(); ++f) {
                if (p->collisionAtFrame(f)) {
                    hits[part] = true;
                    require(!cad::OcctPreviewAccess::frame(*p, f).collisionHighlights.empty(),
                            "Rotated collision has no local marker");
                }
            }
            auto warm = builder.buildRouteDraft(doc, request, {});
            require(std::get<RouteDraftPtr>(warm)->visual == p,
                    "Warm draft rebuilt its orientation");
        }
        require(hits[0] != hits[1], "Fixture does not distinguish collision orientations");
        // A computed route and a manual prefix must keep the same physical installation.
        const auto channel = load(fixtures::channel(), "channel.step");
        BendRouteRequest request;
        request.automatic = true;
        request.withTools = true;
        request.maximumTimeMs = 120000;
        for (std::size_t i = 0; i < channel->metadata().bendAnalysis.bends.size(); ++i) {
            const auto &zone = channel->metadata().bendAnalysis.bends[i];
            auto tool = tool_fixture::pair(16);
            request.operations.push_back({std::to_string(i),
                                          i,
                                          {zone.angleRadians, zone.innerRadiusMm},
                                          core::toolParameters(tool),
                                          tool});
        }
        auto routeResult = builder.buildRoute(channel, request, {});
        if (auto *e = std::get_if<BendRouteError>(&routeResult))
            throw std::runtime_error(e->detail);
        auto route = std::get<BendRoutePtr>(routeResult);
        RouteDraftRequest draft;
        draft.withTools = true;
        for (const auto &step : route->steps) {
            auto op = request.operations[step.bendIndex];
            op.workpieceReversed = step.fixedSecond;
            draft.operations.push_back(op);
        }
        const auto blank = std::get<RouteDraftPtr>(builder.buildRouteDraft(channel, draft, {}));
        require(blank->reusedVisual && blank->visual->frameCount() == 1 &&
                    blank->visual->unverifiedDraft() && !blank->visual->pressMotion() &&
                    !blank->visual->punchRetraction(),
                "Checked route did not retain its initial blank for the first reversal");
        cad::SingleBendPreviewBuilder referenceBlank;
        const auto rebuiltBlank =
            std::get<RouteDraftPtr>(referenceBlank.buildRouteDraft(channel, draft, {}));
        const auto &savedFrame = cad::OcctPreviewAccess::frame(*blank->visual, 0);
        const auto &rebuiltFrame = cad::OcctPreviewAccess::frame(*rebuiltBlank->visual, 0);
        same(material(savedFrame), material(rebuiltFrame));
        for (std::size_t tool = 0; tool < 2; ++tool)
            same(savedFrame.tools[tool], rebuiltFrame.tools[tool]);
        require(blank->visual->collisionAtFrame(0) == rebuiltBlank->visual->collisionAtFrame(0) &&
                    blank->visual->machineCollisionAtFrame(0).has_value() ==
                        rebuiltBlank->visual->machineCollisionAtFrame(0).has_value(),
                "Retained blank disagrees with the independent collision check");
        const auto original = draft;
        for (auto &op : draft.operations) {
            op.assignment->punchReversed = !op.assignment->punchReversed;
            op.tools = core::toolParameters(*op.assignment);
        }
        const auto flipped = std::get<RouteDraftPtr>(builder.buildRouteDraft(channel, draft, {}));
        const auto restored =
            std::get<RouteDraftPtr>(builder.buildRouteDraft(channel, original, {}));
        require(restored->reusedVisual && restored->visual == blank->visual &&
                    std::get<RouteDraftPtr>(builder.buildRouteDraft(channel, draft, {}))->visual ==
                        flipped->visual,
                "A-B-A-B recomputed the first blank pose");
        draft = original;
        for (std::size_t step = 1; step <= draft.operations.size(); ++step) {
            draft.step = step;
            auto result = builder.buildRouteDraft(channel, draft, {});
            require(std::holds_alternative<RouteDraftPtr>(result),
                    "Computed installation lost in draft");
            const auto manual = std::get<RouteDraftPtr>(result)->visual;
            std::cout << "compare route/draft step=" << step
                      << " bend=" << draft.operations[step - 1].bendIndex
                      << " reversed=" << draft.operations[step - 1].workpieceReversed << std::endl;
            same(material(cad::OcctPreviewAccess::frame(*manual, 20)),
                 material(cad::OcctPreviewAccess::frame(*route->steps[step - 1].preview, 60)));
        }
        // A standalone bend is equivalent only to a route cycle whose after-state
        // contains the complete formed source, not an intermediate prefix.
        const auto &lastInput = draft.operations.back();
        const auto lastCycle = preview(builder.buildAssignedToolMotion(
            channel, lastInput.bendIndex, lastInput.target, *lastInput.assignment, {}, 0,
            lastInput.workpieceReversed));
        require(lastCycle == route->steps.back().preview,
                "Standalone lookup discarded an equivalent checked route cycle");
        cad::SingleBendPreviewBuilder lastReference;
        const auto independentLast = preview(lastReference.buildAssignedToolMotion(
            channel, lastInput.bendIndex, lastInput.target, *lastInput.assignment, {}, 0,
            lastInput.workpieceReversed));
        for (const auto frame : {0u, 30u, 60u})
            same(material(cad::OcctPreviewAccess::frame(*lastCycle, frame)),
                 material(cad::OcctPreviewAccess::frame(*independentLast, frame)));
        const auto &firstInput = draft.operations.front();
        require(preview(builder.buildAssignedToolMotion(
                    channel, firstInput.bendIndex, firstInput.target, *firstInput.assignment, {}, 0,
                    firstInput.workpieceReversed)) != route->steps.front().preview,
                "Standalone request reused a different intermediate material state");
        gp_Trsf cacheRotation;
        cacheRotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .47);
        cacheRotation.SetTranslationPart(gp_Vec(17, -29, 43));
        const auto rotated = load(BRepBuilderAPI_Transform(shape, cacheRotation, true).Shape(),
                                  "rotated-cache.step");
        for (const auto &source : {doc, rotated}) {
            cad::SingleBendPreviewBuilder cache;
            auto assignment = tool_fixture::pair(16);
            const auto &zone = source->metadata().bendAnalysis.bends.front();
            BendRouteRequest routeInput;
            routeInput.withTools = true;
            routeInput.allowWorkpieceRotation = false;
            routeInput.allowPartialEndSupport = true;
            routeInput.maximumEndSupportLossMm = 3;
            routeInput.operations.push_back({"one",
                                             0,
                                             {zone.angleRadians, zone.innerRadiusMm},
                                             core::toolParameters(assignment),
                                             assignment});
            const auto result = cache.buildRoute(source, routeInput, {});
            if (const auto *error = std::get_if<BendRouteError>(&result))
                throw std::runtime_error(error->detail);
            const auto initial = std::get<BendRoutePtr>(result)->steps.front().preview;
            require(preview(cache.buildAssignedToolMotion(source, 0,
                                                          routeInput.operations.front().target,
                                                          assignment, {}, 3)) == initial,
                    "Initial checked orientation was not retained");
            assignment.punchReversed = true;
            const auto opposite = preview(cache.buildAssignedToolMotion(
                source, 0, routeInput.operations.front().target, assignment, {}, 3));
            assignment.punchReversed = false;
            require(preview(cache.buildAssignedToolMotion(source, 0,
                                                          routeInput.operations.front().target,
                                                          assignment, {}, 3)) == initial,
                    "First A-B-A rebuilt the original cycle");
            cad::SingleBendPreviewBuilder reference;
            const auto standalone = preview(reference.buildAssignedToolMotion(
                source, 0, routeInput.operations.front().target, assignment, {}, 3));
            for (const auto frame : {0u, 30u, 60u}) {
                const auto &actual = cad::OcctPreviewAccess::frame(*initial, frame);
                const auto &expected = cad::OcctPreviewAccess::frame(*standalone, frame);
                same(material(actual), material(expected));
                for (std::size_t tool = 0; tool < 2; ++tool)
                    same(actual.tools[tool], expected.tools[tool]);
            }
            assignment.punchReversed = true;
            require(preview(cache.buildAssignedToolMotion(source, 0,
                                                          routeInput.operations.front().target,
                                                          assignment, {}, 3)) == opposite,
                    "Warm B rebuilt its cycle");
        }
        BendRouteRequest exact = request;
        exact.automatic = false;
        exact.allowWorkpieceRotation = false;
        exact.operations = draft.operations;
        auto exactResult = builder.buildRoute(channel, exact, {});
        require(std::holds_alternative<BendRoutePtr>(exactResult), "Exact orientations failed");
        for (std::size_t i = 0; i < exact.operations.size(); ++i)
            require(std::get<BendRoutePtr>(exactResult)->steps[i].fixedSecond ==
                        exact.operations[i].workpieceReversed,
                    "Manual check silently changed orientation");
        std::stop_source stop;
        stop.request_stop();
        require(std::holds_alternative<BendPreviewError>(builder.buildAssignedToolMotion(
                    doc, 0, target, tool_fixture::pair(16), stop.get_token(), 3)),
                "Cancellation reused a checked result");
        std::filesystem::remove_all(folder);
        std::cout << "Independent workpiece/tool placements and cached collision checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
