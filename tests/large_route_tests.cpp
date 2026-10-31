#include "ProfileFixtures.h"
#include "ToolFixtures.h"
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
using namespace bending::application;
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
BendRouteRequest request(const DocumentPtr &d, bool tools) {
    BendRouteRequest q;
    q.automatic = true;
    q.withTools = q.allowPartialEndSupport = tools;
    if (tools)
        q.maximumEndSupportLossMm = 3;
    q.maximumTimeMs = 120000;
    q.maximumChecks = 512;
    for (std::size_t i = 0; i < d->metadata().bendAnalysis.bends.size(); ++i) {
        const auto &b = d->metadata().bendAnalysis.bends[i];
        std::optional<core::PressTools> p;
        std::optional<core::ToolAssignment> a;
        if (tools) {
            p = core::PressTools{
                8 * b.thicknessMm, std::numbers::pi / 3, std::numbers::pi / 6, .8, b.lengthMm, 40};
            a = tool_fixture::explicitPair(*p);
            p = core::toolParameters(*a);
        }
        q.operations.push_back({std::to_string(i), i, {b.angleRadians, b.innerRadiusMm}, p, a});
    }
    return q;
}
BendRoutePtr run(cad::SingleBendPreviewBuilder &b, const DocumentPtr &d, const BendRouteRequest &q,
                 const char *label) {
    const auto start = std::chrono::steady_clock::now();
    auto r = b.buildRoute(d, q, {});
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (auto *e = std::get_if<BendRouteError>(&r))
        throw std::runtime_error(std::string(label) + ": " + e->detail +
                                 " attempts=" + std::to_string(e->evaluatedOperations));
    auto p = std::get<BendRoutePtr>(r);
    std::cout << label << " n=" << p->steps.size() << " ms=" << ms
              << " states=" << p->statistics.builtStates
              << " transitions=" << p->statistics.builtTransitions
              << " hits=" << p->statistics.reusedTransitions
              << " profile=" << p->statistics.usedProfilePriority << std::endl;
    check(p->source == d && p->steps.size() == q.operations.size() && p->withTools == q.withTools,
          "Wrong route provenance/scope");
    std::set<std::size_t> seen;
    for (std::size_t i = 0; i < p->steps.size(); ++i) {
        const auto &s = p->steps[i];
        check(seen.insert(s.bendIndex).second && s.preview->otherFormedBends() == i &&
                  s.preview->frameCount() == (q.withTools ? 61u : 21u),
              "Lost large route prefix/frames");
        if (!q.automatic)
            check(s.id == q.operations[i].id, "Manual order was changed");
        for (auto f : {std::size_t{0}, s.preview->frameCount() / 2, s.preview->frameCount() - 1}) {
            const auto &frame = cad::OcctPreviewAccess::frame(*s.preview, f);
            double v = 0;
            for (const auto &region : frame.regions)
                v += volume(region);
            check(BRepCheck_Analyzer(frame.shape).IsValid() &&
                      std::abs(v - d->metadata().volumeMm3) <
                          std::max(.02, d->metadata().volumeMm3 * 1e-7),
                  "Lost material in a large route");
        }
    }
    return p;
}
} // namespace
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("large-route-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        const bool selected = argc == 3 && std::string(argv[1]) == "--count";
        const bool sample = argc > 1 && !selected;
        const auto counts = selected ? std::vector<std::size_t>{std::stoul(argv[2])}
                            : sample ? std::vector<std::size_t>{16}
                                     : std::vector<std::size_t>{16, 24, 32};
        for (const auto count : counts) {
            const auto file = sample ? std::filesystem::path(argv[1])
                                     : folder / (std::to_string(count) + ".step");
            if (!sample) {
                STEPControl_Writer w;
                auto shape = fixtures::foldedProfile(count, 80, .4, count == 24);
                if (count == 24) {
                    gp_Trsf rotate;
                    rotate.SetRotation(gp_Ax1(gp_Pnt(13, -27, 19), gp_Dir(1, 2, 3)), .7);
                    shape = BRepBuilderAPI_Transform(shape, rotate, true).Shape();
                }
                check(w.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                          w.Write(file.string().c_str()) == IFSelect_RetDone,
                      "Write profile failed");
            }
            const auto d = resultDocument(cad::StepImporter{}.load(file, {}, {}));
            check(d && d->metadata().bendAnalysis.bends.size() == count,
                  "Incomplete large profile recognition");
            const auto bytes = std::string(d->sourceStep());
            auto q = request(d, false);
            cad::SingleBendPreviewBuilder builder;
            const auto cold = run(builder, d, q, "cold");
            check(cold->statistics.usedProfilePriority,
                  "Profile strategy was not selected by topology");
            const auto warm = run(builder, d, q, "warm");
            check(warm->statistics.builtTransitions == 0 && warm->statistics.builtStates == 0,
                  "Large successful route was evicted from its own working set");
            auto permuted = q;
            std::reverse(permuted.operations.begin(), permuted.operations.end());
            check(run(builder, d, permuted, "permuted-input")->statistics.builtTransitions == 0,
                  "Input enumeration was confused with a different physical transition");
            auto limited = q;
            limited.maximumChecks = 1;
            check(std::get<BendRouteError>(builder.buildRoute(d, limited, {})).code ==
                      BendRouteErrorCode::SearchLimit,
                  "Large cache bypassed budget");
            auto manual = q;
            manual.automatic = false;
            manual.allowWorkpieceRotation = false;
            manual.operations.clear();
            for (const auto &s : cold->steps) {
                auto op = q.operations[s.bendIndex];
                op.workpieceReversed = s.fixedSecond;
                manual.operations.push_back(op);
            }
            check(run(builder, d, manual, "manual")->statistics.builtTransitions == 0,
                  "Manual large route did not share checks");
            RouteDraftRequest draft;
            draft.operations = q.operations;
            draft.step = count;
            auto r = builder.buildRouteDraft(d, draft, {});
            if (auto *e = std::get_if<BendRouteError>(&r))
                throw std::runtime_error(e->detail);
            auto visual = std::get<RouteDraftPtr>(r)->visual;
            check(visual && visual->unverifiedDraft() && visual->frameCount() == 21,
                  "Large draft failed (65 regions at n32)");
            if (!sample && count == 16) {
                auto tools = request(d, true);
                run(builder, d, tools, "tool-cycle");
            }
            check(d->sourceStep() == bytes, "Original source changed");
        }
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::filesystem::remove_all(folder);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
