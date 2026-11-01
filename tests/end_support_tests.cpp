#include "EndSupportFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include <chrono>
#include <iostream>
#include <limits>
using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
application::BendPreviewPtr accepted(const application::BendPreviewResult &r) {
    if (const auto *e = std::get_if<application::BendPreviewError>(&r))
        throw std::runtime_error(e->detail);
    const auto p = std::get<application::BendPreviewPtr>(r);
    require(p && p->partialEndSupport() && p->pressMotion() && p->frameCount() == 61 &&
                p->punchRetraction(),
            "A bounded partial cycle lost its scope/report");
    return p;
}
} // namespace
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("end-support-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(path, e);
            }
        } cleanup{folder};
        cad::SingleBendPreviewBuilder builder;
        const auto tool = end_support_fixture::tools();
        const auto load = [&](const char *name, double loss, bool middle = false,
                              double other = 0) {
            const auto file = folder / name;
            end_support_fixture::write(file, loss, middle, other);
            const auto doc = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
            require(doc && doc->metadata().bendAnalysis.bends.size() == 1,
                    "Fixture recognition failed");
            return doc;
        };
        const auto run = [&](application::DocumentPtr doc, double limit) {
            const auto &b = doc->metadata().bendAnalysis.bends.front();
            return builder.buildAssignedToolMotion(doc, 0, {b.angleRadians, b.innerRadiusMm}, tool,
                                                   {}, limit);
        };
        const auto small = load("small.step", .46);
        accepted(run(small, 3));
        auto collidingTool = tool_fixture::pair(12, .8, 100);
        collidingTool.die = tool.die;
        const auto &smallBend = small->metadata().bendAnalysis.bends.front();
        const auto collision = builder.buildAssignedToolMotion(
            small, 0, {smallBend.angleRadians, smallBend.innerRadiusMm}, collidingTool, {}, 100);
        const auto *conflict = std::get_if<application::BendPreviewError>(&collision);
        require(conflict && conflict->code == application::BendPreviewErrorCode::ToolIntersection &&
                    conflict->diagnostic && conflict->diagnostic->partialEndSupport(),
                "End allowance suppressed a real tool collision or its support scope");
        require(std::get<application::BendPreviewError>(run(small, 0)).code ==
                    application::BendPreviewErrorCode::SupportNotProven,
                "Strict zero admitted a partial end");
        const auto edge = load("edge.step", 3, false, 3);
        const auto boundary = accepted(run(edge, 3));
        require(application::withinEndSupportLimit(*boundary->partialEndSupport(), 3, 1.e-3),
                "Three millimetres per end were treated as a total allowance");
        const auto excessive = load("excessive.step", 3.2);
        const auto rejected = run(excessive, 3);
        const auto *error = std::get_if<application::BendPreviewError>(&rejected);
        require(error &&
                    error->code == application::BendPreviewErrorCode::EndSupportLimitExceeded &&
                    error->endSupport,
                "Exceeding the configured end limit was not rejected/reported");
        accepted(run(excessive, 4));
        const auto centre = load("middle.step", 2, true);
        require(std::get<application::BendPreviewError>(run(centre, 100)).code ==
                    application::BendPreviewErrorCode::SupportNotProven,
                "An inner support gap was tolerated");
        require(std::get<application::BendPreviewError>(
                    run(small, std::numeric_limits<double>::quiet_NaN()))
                        .code == application::BendPreviewErrorCode::UnsupportedParameters,
                "NaN limit was accepted");
        const auto &b = excessive->metadata().bendAnalysis.bends.front();
        application::BendRouteRequest request;
        request.withTools = request.allowPartialEndSupport = true;
        request.maximumEndSupportLossMm = 4;
        request.operations.push_back(
            {"B1", 0, {b.angleRadians, b.innerRadiusMm}, core::toolParameters(tool), tool});
        auto route = builder.buildRoute(excessive, request, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&route))
            throw std::runtime_error(e->detail);
        require(std::get<application::BendRoutePtr>(route)->partialEndSupport,
                "Route lost the partial-support scope");
        request.maximumEndSupportLossMm = 3;
        route = builder.buildRoute(excessive, request, {});
        require(std::holds_alternative<application::BendRouteError>(route),
                "Tightening the allowance reused an invalid successful transition");
        request.maximumEndSupportLossMm = 4;
        route = builder.buildRoute(excessive, request, {});
        require(std::holds_alternative<application::BendRoutePtr>(route),
                "Restoring the allowance reused a rejection");
        std::cout << "End allowance: .46/3 mm, per-end boundary, strict zero, excess, inner gaps "
                     "and route-cache policy passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
