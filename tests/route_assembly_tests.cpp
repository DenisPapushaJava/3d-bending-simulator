#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include <STEPControl_Writer.hxx>
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
application::BendRouteRequest request(const application::DocumentPtr &doc, bool gaps = false) {
    const core::PressTools parameters{16, std::numbers::pi / 3, std::numbers::pi / 6, .8, 80, 10};
    auto assignment = tool_fixture::explicitPair(parameters);
    assignment.punch.segments = assignment.die.segments =
        gaps ? std::vector<core::ToolSegment>{{58, 2}}
             : std::vector<core::ToolSegment>{{80, 1}, {40, 1}};
    assignment.punchAssembly = assignment.dieAssembly = core::ToolAssembly{{0}, 0};
    application::BendRouteRequest q;
    q.automatic = q.withTools = true;
    q.maximumChecks = 64;
    q.assemblySearch = application::RouteAssemblySearch{{0, 1}, gaps ? 4. : 0.};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto &b = doc->metadata().bendAnalysis.bends[i];
        q.operations.push_back({"B" + std::to_string(i + 1),
                                i,
                                {b.angleRadians, b.innerRadiusMm},
                                core::toolParameters(assignment),
                                assignment});
    }
    return q;
}
application::BendRoutePtr accepted(const application::BendRouteResult &result) {
    if (const auto *error = std::get_if<application::BendRouteError>(&result))
        throw std::runtime_error(error->detail);
    return std::get<application::BendRoutePtr>(result);
}
void fixedResult(const application::BendRouteResult &result, double maximum) {
    if (const auto *r = std::get_if<application::BendRoutePtr>(&result))
        for (const auto &step : (*r)->steps)
            check(!step.assemblyAssignment && step.preview->toolSetup()->tools.lengthMm <= maximum,
                  "A fixed/changed-inventory input reused a derived assembly");
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("route-assemblies-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        STEPControl_Writer writer;
        const auto path = folder / "joint.step";
        check(writer.Transfer(fixtures::jointBend(), STEPControl_AsIs) == IFSelect_RetDone &&
                  writer.Write(path.string().c_str()) == IFSelect_RetDone,
              "Fixture write failed");
        const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
        check(doc && doc->metadata().bendAnalysis.bends.size() == 2,
              "Joint fixture lost its zones");
        const auto bytes = std::string(doc->sourceStep());
        cad::SingleBendPreviewBuilder builder;
        auto q = request(doc);
        const auto original = q.operations;
        const auto route = accepted(builder.buildRoute(doc, q, {}));
        check(route->steps.size() == 1 && route->steps.front().additionalBends.size() == 1 &&
                  route->steps.front().assemblyAssignment &&
                  route->steps.front().preview->model().widthMm == 120 &&
                  route->steps.front().preview->toolSetup()->tools.lengthMm == 120 &&
                  route->steps.front().preview->frameCount() == 61 &&
                  route->steps.front().preview->punchRetraction() &&
                  !route->steps.front().preview->collision() &&
                  route->statistics.plannedAssemblies == 2 && q.operations == original,
              "Coupled search lost coverage, a full cycle or input immutability");
        auto applied = q;
        check(application::applyRouteAssemblyResult(applied, *route), "Derived inputs rejected");
        const auto warm = accepted(builder.buildRoute(doc, q, {}));
        check(warm->statistics.reusedTransitions && warm->statistics.builtTransitions == 0 &&
                  warm->steps.front().preview == route->steps.front().preview,
              "Repeated assembly search rebuilt a checked transition");
        auto fixed = q;
        fixed.assemblySearch.reset();
        fixedResult(builder.buildRoute(doc, fixed, {}), 80);
        auto changed = q;
        for (auto &op : changed.operations) {
            op.assignment->punch.segments->at(1).quantity = 0;
            op.assignment->die.segments->at(1).quantity = 0;
            op.tools = core::toolParameters(*op.assignment);
        }
        fixedResult(builder.buildRoute(doc, changed, {}), 80);
        auto different = q;
        different.operations[1].assignment->punchAssembly = core::ToolAssembly{{1}, 0};
        different.operations[1].assignment->dieAssembly = core::ToolAssembly{{1}, 0};
        different.operations[1].tools = core::toolParameters(*different.operations[1].assignment);
        const auto combined = accepted(builder.buildRoute(doc, different, {}));
        check(combined->steps.size() == 1 &&
                  combined->steps.front().preview->model().widthMm == 120,
              "Authorized unequal initial assemblies could not share one actual installation");
        different.assemblySearch.reset();
        const auto tooShort = builder.buildRoute(doc, different, {});
        const auto *failure = std::get_if<application::BendRouteError>(&tooShort);
        check(failure && failure->operationError == application::BendPreviewErrorCode::ToolTooShort,
              "A fixed short tool bypassed the length guard");
        auto gapped = request(doc, true);
        const auto gapRoute = accepted(builder.buildRoute(doc, gapped, {}));
        check(gapRoute->steps.size() == 1 &&
                  gapRoute->steps.front().preview->toolSegmentGaps()[0] == 4 &&
                  gapRoute->steps.front().preview->toolSegmentGaps()[1] == 4,
              "A permitted gap in the material-free interval was not checked");
        gapped.assemblySearch->maximumGapMm = 3;
        const auto bounded = builder.buildRoute(doc, gapped, {});
        if (const auto *r = std::get_if<application::BendRoutePtr>(&bounded))
            for (const auto &step : (*r)->steps)
                check(step.preview->toolSegmentGaps()[0] <= 3 &&
                          step.preview->toolSegmentGaps()[1] <= 3 && step.additionalBends.empty(),
                      "A different gap limit reused a wider assembly");
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = builder.buildRoute(doc, q, stop.get_token());
        check(std::get<application::BendRouteError>(cancelled).code ==
                      application::BendRouteErrorCode::Cancelled &&
                  doc->sourceStep() == bytes,
              "Cancelled assembly search changed source or returned success");
        std::filesystem::remove_all(folder);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
