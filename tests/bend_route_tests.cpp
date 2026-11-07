#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("bend-route-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const bool toolMode = std::string_view(argv[argc - 1]) == "--tools";
        const bool branched = std::string_view(argv[argc - 1]) == "--branch";
        const bool sample = argc > 1 && std::string_view(argv[1]) != "--tools" && !branched;
        auto file = sample ? std::filesystem::path(argv[1]) : folder / "tray.step";
        if (!sample) {
            STEPControl_Writer writer;
            require(writer.Transfer(branched ? fixtures::branchedTray() : fixtures::tray(),
                                    STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Fixture write");
        }
        const auto document = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
        require(bool(document), "No document");
        const auto source = std::string(document->sourceStep());
        application::BendRouteRequest request;
        request.automatic = true;
        request.withTools = toolMode;
        request.maximumTimeMs = 120000;
        const auto &bends = document->metadata().bendAnalysis.bends;
        for (std::size_t i = 0; i < bends.size(); ++i) {
            const auto &b = bends[i];
            if (sample)
                std::cout << "B" << i + 1 << ": t=" << b.thicknessMm << " R=" << b.innerRadiusMm
                          << " L=" << b.lengthMm << std::endl;
            request.operations.push_back(
                {"B" + std::to_string(i + 1),
                 i,
                 {b.angleRadians, b.innerRadiusMm},
                 core::PressTools{8 * b.thicknessMm, std::numbers::pi / 3, std::numbers::pi / 6, .8,
                                  b.lengthMm, 40},
                 {}});
        }
        const auto result = cad::SingleBendPreviewBuilder{}.buildRoute(document, request, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error("Route error " + std::to_string(static_cast<int>(e->code)) +
                                     " B" + std::to_string(e->bendIndex.value_or(999)) +
                                     " checks=" + std::to_string(e->evaluatedOperations) + ": " +
                                     e->detail);
        const auto route = std::get<application::BendRoutePtr>(result);
        require(route && route->source == document && route->steps.size() == bends.size(),
                "Incomplete route");
        std::set<std::size_t> seen;
        for (std::size_t i = 0; i < route->steps.size(); ++i) {
            const auto &step = route->steps[i];
            require(seen.insert(step.bendIndex).second && step.preview->otherFormedBends() == i,
                    "Duplicate zone or other bends do not represent the route prefix");
            std::cout << i + 1 << ": B" << step.bendIndex + 1 << " fixed=" << step.fixedSecond
                      << '\n';
            for (std::size_t f = 0; f < step.preview->frameCount(); ++f) {
                const auto &frame = cad::OcctPreviewAccess::frame(*step.preview, f);
                const double materialVolume =
                    volume(frame.regions[0]) + volume(frame.regions[1]) + volume(frame.regions[2]);
                require(
                    BRepCheck_Analyzer(frame.shape).IsValid() &&
                        std::abs(materialVolume - document->metadata().volumeMm3 -
                                 document->metadata().closedCutouts.addedVolumeMm3) <
                            std::max(.02, step.preview->model().widthMm *
                                              step.preview->model().thicknessMm *
                                              document->metadata().bendAnalysis.lengthToleranceMm *
                                              2),
                    "Route lost material");
                require(!frame.displayShape.IsNull(), "Route lost visible cutouts");
            }
        }
        if (toolMode && !sample) {
            auto forming = request;
            forming.manualExtraction = true;
            forming.automatic = forming.allowWorkpieceRotation = false;
            forming.operations.clear();
            for (const auto &step : route->steps) {
                auto op = request.operations[step.bendIndex];
                op.workpieceReversed = step.fixedSecond;
                forming.operations.push_back(std::move(op));
            }
            cad::SingleBendPreviewBuilder builder;
            const auto checked = builder.buildRoute(document, forming, {});
            if (const auto *e = std::get_if<application::BendRouteError>(&checked))
                throw std::runtime_error("Manual extraction: " + e->detail);
            const auto manual = std::get<application::BendRoutePtr>(checked);
            require(manual->manualExtraction && manual->steps.size() == route->steps.size(),
                    "Manual scope or complete coverage lost");
            for (std::size_t n = 0; n < manual->steps.size(); ++n) {
                const auto &p = manual->steps[n].preview;
                const auto &strict = route->steps[n].preview;
                require(p->manualExtraction() && !p->unverifiedDraft() && !p->punchRetraction() &&
                            p->frameCount() == 41 && !p->collision() &&
                            p->pressState(40)->phase == simulation::PressPhase::Bending &&
                            cad::OcctPreviewAccess::retractionSweep(*p).IsNull(),
                        "Forming scope claimed or animated a verified return");
                for (std::size_t frame = 0; frame < 41; ++frame)
                    require(p->pressState(frame)->phase == strict->pressState(frame)->phase &&
                                std::abs(p->pressState(frame)->punchTipYmm -
                                         strict->pressState(frame)->punchTipYmm) < 1e-9 &&
                                std::abs(p->pressState(frame)->section.angleRadians -
                                         strict->pressState(frame)->section.angleRadians) < 1e-9,
                            "Manual scope rescaled or skipped a forming sample");
            }
            const auto repeated = builder.buildRoute(document, forming, {});
            const auto *hit = std::get_if<application::BendRoutePtr>(&repeated);
            require(hit && (*hit)->statistics.reusedTransitions >= manual->steps.size(),
                    "Forming-only transitions did not reuse exact inputs");
            auto strictRequest = forming;
            strictRequest.manualExtraction = false;
            const auto strictResult = builder.buildRoute(document, strictRequest, {});
            const auto *strict = std::get_if<application::BendRoutePtr>(&strictResult);
            require(strict && !(*strict)->manualExtraction &&
                        (*strict)->statistics.builtTransitions >= manual->steps.size(),
                    "Strict mode reused a forming-only transition");
            for (const auto &step : (*strict)->steps)
                require(!step.preview->manualExtraction() && step.preview->punchRetraction() &&
                            step.preview->frameCount() == 61,
                        "Strict mode lost the continuous punch return");
            auto shortTools = forming;
            for (auto &op : shortTools.operations)
                op.tools->lengthMm = 1;
            const auto blocked = builder.buildRoute(document, shortTools, {});
            const auto *error = std::get_if<application::BendRouteError>(&blocked);
            require(error &&
                        error->operationError == application::BendPreviewErrorCode::ToolTooShort,
                    "Manual extraction waived tool length");
            auto invalidScope = forming;
            invalidScope.withTools = false;
            const auto invalid = builder.buildRoute(document, invalidScope, {});
            require(std::get<application::BendRouteError>(invalid).code ==
                        application::BendRouteErrorCode::InvalidInput,
                    "Manual extraction without tools accepted");
        }
        require(document->sourceStep() == source, "Source changed");
        std::stop_source stopped;
        stopped.request_stop();
        auto cancelled =
            cad::SingleBendPreviewBuilder{}.buildRoute(document, request, stopped.get_token());
        require(std::get<application::BendRouteError>(cancelled).code ==
                    application::BendRouteErrorCode::Cancelled,
                "Cancellation lost");
        if (!sample) {
            auto invalid = request;
            invalid.operations.pop_back();
            require(std::holds_alternative<application::BendRouteError>(
                        cad::SingleBendPreviewBuilder{}.buildRoute(document, invalid, {})),
                    "Missing bend accepted");
            request.maximumChecks = 1;
            const auto limited = cad::SingleBendPreviewBuilder{}.buildRoute(document, request, {});
            require(std::get<application::BendRouteError>(limited).code ==
                        application::BendRouteErrorCode::SearchLimit,
                    "Search cap lost");
            if (!toolMode && !branched) {
                request.withTools = true;
                request.automatic = false;
                request.maximumChecks = 96;
                for (auto &op : request.operations)
                    op.tools->lengthMm = 1;
                const auto tooShort =
                    cad::SingleBendPreviewBuilder{}.buildRoute(document, request, {});
                require(
                    std::get<application::BendRouteError>(tooShort).code ==
                            application::BendRouteErrorCode::InvalidInput &&
                        std::get<application::BendRouteError>(tooShort).operationError ==
                            application::BendPreviewErrorCode::ToolTooShort &&
                        std::get<application::BendRouteError>(tooShort).evaluatedOperations == 0 &&
                        std::get<application::BendRouteError>(tooShort).toolLengths &&
                        std::get<application::BendRouteError>(tooShort).toolLengths->dieLengthMm ==
                            1,
                    "Short tools were not rejected with their lengths before CAD search");
            }
        }
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
