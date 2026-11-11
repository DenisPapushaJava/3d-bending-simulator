#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace bending;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("horn-route-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(folder);
        const auto path = folder / "tray.step";
        STEPControl_Writer writer;
        check(writer.Transfer(fixtures::tray(false, 198, 118), STEPControl_AsIs) ==
                      IFSelect_RetDone &&
                  writer.Write(path.string().c_str()) == IFSelect_RetDone,
              "Cannot write tray");
        const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
        check(doc && doc->metadata().bendAnalysis.bends.size() == 4, "Tray recognition");
        auto tools = tool_fixture::pair(16, .8, 30);
        auto &e = tools.punch.drawing->edges;
        e[3].end.x -= 30;
        e[4].start = e[3].end;
        tools.punch.lengthMm = tools.die.lengthMm = 200;
        // The stocked straight is too long for this box; the two real end sections
        // provide a shorter shared working edge. No inference from the name/shape.
        tools.punch.segments = std::vector<core::ToolSegment>{
            {250, 1},
            {50, 1},
            {100, 1, core::SegmentShape::LeftHorn, core::HornRelief{27, 27, 9}, "left"},
            {100, 1, core::SegmentShape::RightHorn, core::HornRelief{27, 27, 9}, "right"}};
        tools.die.segments = std::vector<core::ToolSegment>{{200, 1}};
        tools.punchAssembly = tools.dieAssembly = core::ToolAssembly{{0}, 0, {}};
        application::BendRouteRequest q;
        q.automatic = q.withTools = q.allowAxialPlacement = true;
        q.maximumChecks = 160;
        q.maximumTimeMs = 120000;
        q.assemblySearch = application::RouteAssemblySearch{{}, 0};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto &b = doc->metadata().bendAnalysis.bends[i];
            q.operations.push_back({"B" + std::to_string(i + 1),
                                    i,
                                    {b.angleRadians, b.innerRadiusMm},
                                    core::toolParameters(tools),
                                    tools});
            q.assemblySearch->bendIndices.push_back(i);
        }
        const auto original = q.operations;
        {
            auto straight = q;
            for (auto &op : straight.operations) {
                op.assignment->punch.segments->front().lengthMm = 180;
                op.tools = core::toolParameters(*op.assignment);
            }
            const auto fit = cad::SingleBendPreviewBuilder{}.buildRoute(doc, straight, {});
            if (const auto *error = std::get_if<application::BendRouteError>(&fit))
                throw std::runtime_error("Straight box route: " + error->detail);
            const auto &route = *std::get<application::BendRoutePtr>(fit);
            check(application::applyRouteAssemblyResult(straight, route),
                  "Straight box route was not accepted");
            const auto &chosen = *straight.operations.front().assignment;
            std::cout << "straight-stock checks=" << route.evaluatedOperations
                      << " horns=" << core::usesHornSections(chosen.punch, chosen.punchAssembly)
                      << " plans=" << route.statistics.plannedAssemblies << std::endl;
            check(route.steps.size() == 4 && route.evaluatedOperations == 4 &&
                      route.statistics.plannedAssemblies == 0 &&
                      std::all_of(straight.operations.begin(), straight.operations.end(),
                                  [&](const auto &op) {
                                      return op.assignment == chosen &&
                                             !core::usesHornSections(chosen.punch,
                                                                     chosen.punchAssembly);
                                  }),
                  "A usable straight assembly was replaced or unnecessary families were tried");
        }
        cad::SingleBendPreviewBuilder builder;
        const auto result = builder.buildRoute(doc, q, {});
        if (const auto *error = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error("Auto horn route: " + error->detail +
                                     " checks=" + std::to_string(error->evaluatedOperations));
        const auto route = std::get<application::BendRoutePtr>(result);
        std::cout << "checks=" << route->evaluatedOperations
                  << " common=" << route->statistics.commonAssemblyPasses
                  << " used=" << route->statistics.usedCommonAssembly << '\n';
        auto resolved = q;
        check(application::applyRouteAssemblyResult(resolved, *route),
              "Derived assemblies not accepted");
        check(route->steps.size() == 4 && q.operations == original,
              "Partial route or mutated input");
        std::optional<core::ToolAssignment> common;
        for (const auto &op : resolved.operations) {
            if (!common)
                common = op.assignment;
            check(op.assignment == common, "Route changed the common assembly");
            std::cout << "pieces:";
            for (auto i : op.assignment->punchAssembly->pieces)
                std::cout << ' ' << i;
            std::cout << '\n';
        }
        check(common && common->punch.segments && common->punchAssembly,
              "Missing shared punch assembly");
        const auto ends = core::punchAssemblyEnds(*common->punch.segments, *common->punchAssembly);
        check(ends && ends->left && ends->right && route->statistics.usedCommonAssembly &&
                  route->evaluatedOperations >= 4,
              "Paired horns did not solve the box after the overlong straight assembly");
        for (const auto &s : route->steps)
            check(s.preview && !s.preview->collision() && s.preview->frameCount() == 61 &&
                      s.preview->punchRetraction(),
                  "Incomplete checked horn cycle");
        {
            const auto warm = builder.buildRoute(doc, q, {});
            const auto *cached = std::get_if<application::BendRoutePtr>(&warm);
            if (const auto *warmError = std::get_if<application::BendRouteError>(&warm))
                throw std::runtime_error("Warm horn route: " + warmError->detail);
            std::cout << "cold transitions=" << route->statistics.builtTransitions
                      << " warm transitions=" << (*cached)->statistics.builtTransitions
                      << " reused transitions=" << (*cached)->statistics.reusedTransitions
                      << " warm rejections=" << (*cached)->statistics.reusedRejections << std::endl;
            check(cached &&
                      (*cached)->statistics.reusedTransitions >=
                          (*cached)->statistics.reusedRejections + 4 &&
                      (*cached)->statistics.builtTransitions < route->statistics.builtTransitions,
                  "Repeated horn search did not reuse the four checked transitions");
            auto reference = q;
            reference.reuseCalculations = false;
            const auto independent = cad::SingleBendPreviewBuilder{}.buildRoute(doc, reference, {});
            const auto *uncached = std::get_if<application::BendRoutePtr>(&independent);
            check(uncached && application::applyRouteAssemblyResult(reference, **uncached) &&
                      reference.operations == resolved.operations,
                  "Uncached search disagreed with the shared horn assembly");
            auto retained = builder.buildRoute(doc, resolved, {});
            const auto *same = std::get_if<application::BendRoutePtr>(&retained);
            check(same && (*same)->statistics.usedCommonAssembly &&
                      (*same)->statistics.plannedAssemblies == 0 &&
                      std::none_of((*same)->steps.begin(), (*same)->steps.end(),
                                   [](const auto &step) { return bool(step.assemblyAssignment); }),
                  "A suitable installed horn assembly was replaced or replanned");
            auto shorter = q;
            for (auto &op : shorter.operations) {
                op.assignment->punchAssembly = core::ToolAssembly{{1}, 0, {}};
                op.tools = core::toolParameters(*op.assignment);
            }
            const auto enlarged = builder.buildRoute(doc, shorter, {});
            const auto *expanded = std::get_if<application::BendRoutePtr>(&enlarged);
            check(expanded && (*expanded)->statistics.usedCommonAssembly,
                  "A short original punch prevented a shared horn assembly");
            auto denied = q;
            denied.assemblySearch->bendIndices.pop_back();
            const auto untouched = denied.operations;
            check(!application::applyRouteAssemblyResult(denied, *route) &&
                      denied.operations == untouched,
                  "Derived horn assembly escaped its authorized scope");
            auto plain = q;
            plain.assemblySearch.reset();
            const auto blocked = builder.buildRoute(doc, plain, {});
            const auto *error = std::get_if<application::BendRouteError>(&blocked);
            check(error &&
                      error->operationError == application::BendPreviewErrorCode::ToolIntersection,
                  "Overlong straight section unexpectedly cleared the corners");
            plain.manualExtraction = true;
            const auto formingBlocked = builder.buildRoute(doc, plain, {});
            const auto *formingError = std::get_if<application::BendRouteError>(&formingBlocked);
            check(formingError && formingError->operationError ==
                                      application::BendPreviewErrorCode::ToolIntersection,
                  "Manual extraction waived a real forming collision");
            auto longer = resolved;
            longer.assemblySearch.reset();
            for (auto &op : longer.operations) {
                for (auto &s : *op.assignment->punch.segments)
                    if (s.horn)
                        s.lengthMm = 125;
                op.tools = core::toolParameters(*op.assignment);
                check(op.tools.has_value(), "Negative length fixture is not a valid tool");
            }
            const auto overlong = builder.buildRoute(doc, longer, {});
            const auto *bad = std::get_if<application::BendRouteError>(&overlong);
            if (bad)
                std::cout << "overlong horns error=" << static_cast<int>(bad->code) << " operation="
                          << (bad->operationError ? static_cast<int>(*bad->operationError) : -1)
                          << " " << bad->detail << std::endl;
            check(bad && bad->operationError == application::BendPreviewErrorCode::ToolIntersection,
                  "Long horns bypassed corner collisions or reused a shorter assembly's result");
            std::stop_source stopping;
            auto cancel = q;
            cancel.onProgress = [&](application::BendRouteProgress p) {
                if (p.checkedOperations >= 1)
                    stopping.request_stop();
            };
            const auto stopped = builder.buildRoute(doc, cancel, stopping.get_token());
            check(std::holds_alternative<application::BendRouteError>(stopped) &&
                      std::get<application::BendRouteError>(stopped).code ==
                          application::BendRouteErrorCode::Cancelled,
                  "Cancellation in the derived common family published a route");
            auto tiny = q;
            tiny.maximumChecks = 1;
            const auto limited = builder.buildRoute(doc, tiny, {});
            check(std::holds_alternative<application::BendRouteError>(limited) &&
                      std::get<application::BendRouteError>(limited).code ==
                          application::BendRouteErrorCode::SearchLimit &&
                      std::get<application::BendRouteError>(limited).evaluatedOperations <= 1,
                  "Horn families received a fresh global check budget");
        }
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
