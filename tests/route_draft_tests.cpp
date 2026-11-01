#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace bending;
using namespace bending::application;
namespace {
void require(bool v, const char *why) {
    if (!v)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
RouteDraftPtr run(cad::SingleBendPreviewBuilder &builder, const DocumentPtr &doc,
                  const RouteDraftRequest &request) {
    const auto start = std::chrono::steady_clock::now();
    auto result = builder.buildRouteDraft(doc, request, {});
    const auto built = std::chrono::steady_clock::now();
    if (const auto *e = std::get_if<BendRouteError>(&result))
        throw std::runtime_error(e->detail);
    auto d = std::get<RouteDraftPtr>(result);
    require(d && d->source == doc && d->request == request && d->visual->unverifiedDraft() &&
                !d->visual->punchRetraction() && !d->visual->pressMotion(),
            "Draft acquired a route certificate");
    require(d->visual->frameCount() == (request.step ? 21u : 1u), "Wrong draft frame count");
    const double expected =
        doc->metadata().volumeMm3 + doc->metadata().closedCutouts.addedVolumeMm3;
    for (std::size_t i = 0; i < d->visual->frameCount(); ++i) {
        const auto &f = cad::OcctPreviewAccess::frame(*d->visual, i);
        require(std::abs(volume(f.shape) - expected) < std::max(.02, expected * 1.e-7),
                "Draft lost material");
        require(!f.displayShape.IsNull(), "Original cutout surface missing");
        if (d->visual->toolSetup() && (i == 0 || i == 10 || i + 1 == d->visual->frameCount())) {
            bool collision = false;
            for (const auto &tool : f.tools) {
                BRepAlgoAPI_Common common(f.shape, tool);
                common.Build();
                require(common.IsDone() && !common.HasErrors(),
                        "Independent collision test failed");
                collision |=
                    volume(common.Shape()) >
                    std::max(1.e-6, d->visual->model().widthMm * d->visual->model().thicknessMm *
                                        doc->metadata().bendAnalysis.lengthToleranceMm * 2);
            }
            BRepAlgoAPI_Common tools(f.tools[0], f.tools[1]);
            tools.Build();
            require(tools.IsDone() && !tools.HasErrors(), "Independent tool collision failed");
            collision |=
                volume(tools.Shape()) >
                std::max(1.e-6, d->visual->model().widthMm * d->visual->model().thicknessMm *
                                    doc->metadata().bendAnalysis.lengthToleranceMm * 2);
            require(collision == d->visual->collisionAtFrame(i).has_value(),
                    "Draft collision flag disagrees with independent full-body intersection");
        }
    }
    if (d->visual->toolSetup())
        require(d->checkedToolPositions == d->visual->frameCount(),
                "Published unchecked tool positions");
    std::cout << "step=" << request.step << " tools=" << request.withTools
              << " reused=" << d->reusedBasis << " build-ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(built - start).count()
              << " total-ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << std::endl;
    return d;
}
} // namespace
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("draft-route-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const bool sample = argc > 1;
        const auto file = sample ? std::filesystem::path(argv[1]) : folder / "source.step";
        if (!sample) {
            STEPControl_Writer writer;
            require(writer.Transfer(fixtures::channel(), STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Cannot write fixture");
        }
        const auto doc = resultDocument(cad::StepImporter{}.load(file, {}, {}));
        require(bool(doc), "Import failed");
        const auto bytes = std::string(doc->sourceStep());
        cad::SingleBendPreviewBuilder builder;
        RouteDraftRequest request;
        for (std::size_t i = 0; i < doc->metadata().bendAnalysis.bends.size(); ++i) {
            const auto &b = doc->metadata().bendAnalysis.bends[i];
            request.operations.push_back(
                {std::to_string(i),
                 i,
                 {b.angleRadians, b.innerRadiusMm},
                 core::PressTools{8 * b.thicknessMm, std::numbers::pi / 3, std::numbers::pi / 6, .8,
                                  b.lengthMm, 40},
                 {}});
        }
        require(!request.operations.empty(), "No bends");
        const auto flat = run(builder, doc, request);
        require(!flat->reusedBasis, "First basis unexpectedly reused");
        for (std::size_t i = 1; i <= request.operations.size(); ++i) {
            request.step = i;
            run(builder, doc, request);
        }
        const auto final = run(builder, doc, request);
        const auto &formed = cad::OcctPreviewAccess::frame(*final->visual, 20).shape;
        const auto &native = dynamic_cast<const cad::CadDocument &>(*doc);
        const auto &source = cad::OcctDocumentAccess::simulationShape(native);
        for (bool reverse : {false, true}) {
            BRepAlgoAPI_Cut difference(reverse ? source : formed, reverse ? formed : source);
            difference.Build();
            require(difference.IsDone() && !difference.HasErrors() &&
                        volume(difference.Shape()) < .02,
                    "Final draft does not reconstruct STEP");
        }
        request.withTools = true;
        request.step = 0;
        run(builder, doc, request);
        request.step = 1;
        const auto tool = run(builder, doc, request);
        const auto warm = run(builder, doc, request);
        require(warm->visual == tool->visual, "Warm draft recomputed geometry");
        for (const bool shortPunch : {false, true}) {
            auto shortRequest = request;
            const double length = doc->metadata().bendAnalysis.bends.front().lengthMm;
            shortRequest.operations.front().tools->lengthMm = shortPunch ? length + 20 : length - 5;
            shortRequest.operations.front().tools->punchLengthMm =
                shortPunch ? length - 5 : length + 20;
            const auto inspected = run(builder, doc, shortRequest);
            require(inspected->toolLengths && inspected->toolLengths->bendLengthMm == length &&
                        inspected->toolLengths->punchLengthMm ==
                            *shortRequest.operations.front().tools->punchLengthMm &&
                        inspected->toolLengths->dieLengthMm ==
                            shortRequest.operations.front().tools->lengthMm &&
                        inspected->checkedToolPositions == 21,
                    "Short punch/die lacks its length report or collision checks");
            BendRouteRequest full;
            full.operations = shortRequest.operations;
            full.withTools = true;
            const auto rejected = builder.buildRoute(doc, full, {});
            const auto *error = std::get_if<BendRouteError>(&rejected);
            require(error && error->operationError == BendPreviewErrorCode::ToolTooShort &&
                        error->toolLengths == inspected->toolLengths,
                    "A short diagnostic tool was accepted as a checked route");
        }
        if (!sample) {
            const auto thickFile = folder / "six-mm.step";
            STEPControl_Writer writer;
            require(writer.Transfer(fixtures::bentStrip(8, 6), STEPControl_AsIs) ==
                            IFSelect_RetDone &&
                        writer.Write(thickFile.string().c_str()) == IFSelect_RetDone,
                    "Cannot write 6 mm fixture");
            const auto thick = resultDocument(cad::StepImporter{}.load(thickFile, {}, {}));
            require(thick && thick->metadata().bendAnalysis.bends.size() == 1, "No 6 mm bend");
            const auto &bend = thick->metadata().bendAnalysis.bends.front();
            require(std::abs(bend.thicknessMm - 6) < 1.e-6, "Wrong regression thickness");
            auto narrow = tool_fixture::pair(12);
            RouteDraftRequest forbidden;
            forbidden.withTools = true;
            forbidden.operations.push_back({"six-mm",
                                            0,
                                            {bend.angleRadians, bend.innerRadiusMm},
                                            core::toolParameters(narrow),
                                            narrow});
            require(forbidden.operations.front().tools.has_value(), "Invalid test assignment");
            for (bool captured : {false, true}) {
                forbidden.operations.front().assignment =
                    captured ? std::optional{narrow} : std::nullopt;
                for (std::size_t step : {0u, 1u}) {
                    forbidden.step = step;
                    const auto rejected = builder.buildRouteDraft(thick, forbidden, {});
                    const auto *error = std::get_if<BendRouteError>(&rejected);
                    require(error && error->code == BendRouteErrorCode::InvalidInput &&
                                error->operationError == BendPreviewErrorCode::NarrowDieOpening &&
                                error->bendIndex == 0,
                            "Manual draft accepted V12 for 6 mm material");
                }
            }
            require(run(builder, doc, request)->visual == warm->visual,
                    "Rejected narrow tool evicted the previous valid preview");
            auto segmented = tool_fixture::pair(16);
            segmented.punch.segments = std::vector<core::ToolSegment>{{40, 2}};
            segmented.die.segments = std::vector<core::ToolSegment>{{40, 2}};
            segmented.punchAssembly = core::ToolAssembly{{0, 0}, 2};
            segmented.dieAssembly = core::ToolAssembly{{0, 0}, 1};
            request.operations[0].assignment = segmented;
            request.operations[0].tools = core::toolParameters(segmented);
            const auto sections = run(builder, doc, request);
            require(sections->visual->toolSegmentGaps() == std::array<double, 2>{1, 2},
                    "Draft lost assembly gaps");
            const auto &frame = cad::OcctPreviewAccess::frame(*sections->visual, 0);
            for (std::size_t i = 0; i < 2; ++i) {
                require(frame.toolSegments[i].size() == 2, "Draft fused the selected tool pieces");
                Bnd_Box a, b;
                BRepBndLib::AddOptimal(frame.toolSegments[i][0].shape, a, false, false);
                BRepBndLib::AddOptimal(frame.toolSegments[i][1].shape, b, false, false);
                require(std::abs(b.CornerMin().Z() - a.CornerMax().Z() - (i == 0 ? 1 : 2)) < 1.e-5,
                        "Draft tool pieces have the wrong placement");
            }
            request.operations[0].assignment = tool_fixture::pair(16, .8, 100);
            request.operations[0].tools = core::toolParameters(*request.operations[0].assignment);
            const auto collision = run(builder, doc, request);
            bool seen = false;
            for (std::size_t i = 0; i < 21; ++i) {
                seen |= collision->visual->collisionAtFrame(i).has_value();
                const auto &f = cad::OcctPreviewAccess::frame(*collision->visual, i);
                require(!collision->visual->collisionAtFrame(i) || !f.collisionHighlights.empty(),
                        "Draft collision lost its localized marker");
            }
            require(seen, "Known DXF collision was missed");
        }
        std::reverse(request.operations.begin(), request.operations.end());
        request.withTools = false;
        const auto changed = run(builder, doc, request);
        require(changed->reusedBasis, "Reorder lost the shared geometry basis");
        request.step = 0;
        run(builder, doc, request);
        std::stop_source cancelled;
        cancelled.request_stop();
        require(
            std::get<BendRouteError>(builder.buildRouteDraft(doc, request, cancelled.get_token()))
                    .code == BendRouteErrorCode::Cancelled,
            "Cancellation ignored");
        require(doc->sourceStep() == bytes, "Source was modified");
        std::filesystem::remove_all(folder);
        std::cout
            << "Draft prefixes, material, final STEP, real-tool collisions and cache passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
