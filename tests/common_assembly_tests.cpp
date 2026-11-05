#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepBuilderAPI_Transform.hxx>
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
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
application::BendRouteRequest request(application::DocumentPtr doc, double extra = 0) {
    const auto &bends = doc->metadata().bendAnalysis.bends;
    double longest = 0, shortest = 1.e9;
    for (const auto &b : bends) {
        longest = std::max(longest, b.lengthMm);
        shortest = std::min(shortest, b.lengthMm);
    }
    longest = std::round(longest * 1000) / 1000;
    shortest = std::round(shortest * 1000) / 1000;
    auto a = tool_fixture::explicitPair(
        {16, std::numbers::pi / 3, std::numbers::pi / 6, .8, longest + extra, 10});
    std::vector<core::ToolSegment> stock{{longest + extra, 1}, {shortest, 1}};
    if (extra > 0)
        stock.push_back({longest, 1});
    a.punch.segments = a.die.segments = stock;
    a.punchAssembly = a.dieAssembly = core::ToolAssembly{{0}, 0};
    application::BendRouteRequest q;
    q.automatic = q.withTools = q.allowAxialPlacement = true;
    q.maximumChecks = 128;
    q.maximumTimeMs = 120000;
    q.assemblySearch = application::RouteAssemblySearch{{}, 0};
    for (std::size_t i = 0; i < bends.size(); ++i) {
        const auto &b = bends[i];
        q.operations.push_back({"B" + std::to_string(i + 1),
                                i,
                                {b.angleRadians, b.innerRadiusMm},
                                core::toolParameters(a),
                                a});
        q.assemblySearch->bendIndices.push_back(i);
    }
    return q;
}
application::BendRoutePtr success(const application::BendRouteResult &r) {
    if (const auto *e = std::get_if<application::BendRouteError>(&r))
        throw std::runtime_error(e->detail);
    return std::get<application::BendRoutePtr>(r);
}
void verify(application::DocumentPtr doc, const application::BendRouteRequest &q,
            const application::BendRoutePtr &route) {
    auto resolved = q;
    check(application::applyRouteAssemblyResult(resolved, *route), "Assignments rejected");
    check(route->steps.size() == 4, "Incomplete or merged perpendicular bends");
    std::set<std::size_t> seen;
    for (const auto &s : route->steps) {
        check(seen.insert(s.bendIndex).second && s.preview && !s.preview->collision() &&
                  s.preview->frameCount() == 61 && s.preview->punchRetraction(),
              "Missing full checked cycle");
        for (const auto f : {std::size_t{0}, std::size_t{30}, std::size_t{60}}) {
            const auto &frame = cad::OcctPreviewAccess::frame(*s.preview, f);
            double material = 0;
            for (const auto &region : frame.regions) {
                GProp_GProps props;
                BRepGProp::VolumeProperties(region, props);
                material += std::abs(props.Mass());
            }
            check(BRepCheck_Analyzer(frame.shape).IsValid() &&
                      std::abs(material - doc->metadata().volumeMm3) < .02,
                  "Changed material in a checked cycle");
        }
    }
}
void unchanged(const application::BendRoutePtr &p) {
    for (const auto &s : p->steps)
        check(!s.assemblyAssignment, "Common pass changed a captured assembly");
}
} // namespace
int main(int argc, char **argv) {
    const bool rotated = argc == 2 && std::string(argv[1]) == "--rotated";
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("common-assembly-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        auto shape = fixtures::tray(false, rotated ? 260 : 200, rotated ? 150 : 100);
        if (rotated) {
            gp_Trsf tr;
            tr.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(1, 2, 3)), .63);
            tr.SetTranslationPart(gp_Vec(37, -19, 84));
            shape = BRepBuilderAPI_Transform(shape, tr, true).Shape();
        }
        STEPControl_Writer writer;
        const auto path = folder / "tray.step";
        check(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                  writer.Write(path.string().c_str()) == IFSelect_RetDone,
              "STEP write failed");
        const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
        check(doc && doc->metadata().bendAnalysis.bends.size() == 4, "Fixture recognition changed");
        const auto source = std::string(doc->sourceStep());
        auto q = request(doc);
        if (rotated)
            std::rotate(q.operations.begin(), q.operations.begin() + 1, q.operations.end());
        const auto original = q.operations;
        cad::SingleBendPreviewBuilder builder;
        const auto route = success(builder.buildRoute(doc, q, {}));
        std::cout << "common checks=" << route->evaluatedOperations
                  << " ranked=" << route->statistics.toolAccessRankedStates
                  << " plans=" << route->statistics.plannedAssemblies << std::endl;
        verify(doc, q, route);
        unchanged(route);
        check(route->statistics.commonAssemblyPasses == 1 && route->statistics.usedCommonAssembly &&
                  route->statistics.commonAssemblyChecks == route->evaluatedOperations &&
                  route->statistics.plannedAssemblies == 0 &&
                  route->statistics.toolAccessRankedStates > 0 && q.operations == original,
              "Common pass missing or mutating the request");
        const auto warm = success(builder.buildRoute(doc, q, {}));
        verify(doc, q, warm);
        unchanged(warm);
        check(warm->statistics.builtTransitions == 0 && warm->statistics.reusedTransitions >= 4 &&
                  warm->statistics.preparedOperations == 0,
              "Repeated common pass rebuilt checked transitions");
        auto reference = q;
        reference.preferCommonAssembly = reference.rankToolAccess = false;
        cad::SingleBendPreviewBuilder old;
        verify(doc, reference, success(old.buildRoute(doc, reference, {})));
        auto fixed = q;
        fixed.assemblySearch.reset();
        const auto fixedRoute = success(builder.buildRoute(doc, fixed, {}));
        unchanged(fixedRoute);
        check(fixedRoute->statistics.commonAssemblyPasses == 0, "Fixed request started planning");
        if (!rotated) {
            // Longer than both closed spans: a common pair cannot finish. The
            // ordinary family must still be allowed to use the shorter stock rows.
            const auto flexible = request(doc, 40);
            const auto fallback = success(builder.buildRoute(doc, flexible, {}));
            std::cout << "fallback checks=" << fallback->evaluatedOperations
                      << " common-checks=" << fallback->statistics.commonAssemblyChecks
                      << " plans=" << fallback->statistics.plannedAssemblies << std::endl;
            verify(doc, flexible, fallback);
            check(fallback->statistics.commonAssemblyPasses == 1 &&
                      !fallback->statistics.usedCommonAssembly &&
                      fallback->statistics.commonAssemblyChecks <= 8 &&
                      fallback->statistics.plannedAssemblies > 0 &&
                      std::any_of(fallback->steps.begin(), fallback->steps.end(),
                                  [](const auto &s) { return bool(s.assemblyAssignment); }),
                  "Restricted-family rejection poisoned the fallback");
            auto tiny = q;
            tiny.maximumChecks = 1;
            const auto limited = builder.buildRoute(doc, tiny, {});
            const auto *limit = std::get_if<application::BendRouteError>(&limited);
            check(limit && limit->code == application::BendRouteErrorCode::SearchLimit &&
                      limit->evaluatedOperations <= 1,
                  "Shared check budget bypassed or declared NoRoute");
            std::stop_source during;
            auto cancellable = q;
            cancellable.onProgress = [&](application::BendRouteProgress p) {
                if (p.checkedOperations >= 1)
                    during.request_stop();
            };
            check(std::get<application::BendRouteError>(
                      builder.buildRoute(doc, cancellable, during.get_token()))
                          .code == application::BendRouteErrorCode::Cancelled,
                  "Cancellation inside the common pass published a partial route");
            std::stop_source stop;
            stop.request_stop();
            check(
                std::get<application::BendRouteError>(builder.buildRoute(doc, q, stop.get_token()))
                        .code == application::BendRouteErrorCode::Cancelled,
                "Cancellation ignored");
        }
        check(doc->sourceStep() == source, "Source changed");
        std::filesystem::remove_all(folder);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
