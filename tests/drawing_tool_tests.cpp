#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <chrono>
#include <filesystem>

#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
int main() {
    try {
        auto tools = tool_fixture::pair();
        auto g = core::drawingToolGeometry(tools);
        require(g.has_value(), "No references");
        const auto bodies = cad::makeDrawingTools(tools, *g, 5);
        for (const auto &b : bodies)
            require(BRepCheck_Analyzer(b).IsValid() && volume(b) > 0, "Bad tool body");
        auto broken = tools;
        auto &edges = broken.punch.drawing->edges;
        const double w = edges[2].end.x;
        edges[2].end = {-w, 45};
        edges[3].start = {-w, 45};
        edges[3].end = {w, 45};
        edges[4].start = {w, 45};
        bool rejected = false;
        try {
            (void)cad::makeDrawingTools(broken, *g, 5);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Self-intersecting tool profile was accepted");
        auto sweep = cad::drawingPunchSweep(tools, *g, 5, 17, {});
        require(BRepCheck_Analyzer(sweep).IsValid() && volume(sweep) > volume(bodies[1]),
                "Bad sweep");
        for (double u : {0., .013, .29, .577, .943, 1.}) {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(0, 17 * u, 0));
            auto body = BRepBuilderAPI_Transform(bodies[1], t, true).Shape();
            BRepAlgoAPI_Cut outside(body, sweep);
            require(outside.IsDone() && !outside.HasErrors() && volume(outside.Shape()) < 1.e-5,
                    "Sweep omitted intermediate material");
        }
        const auto hook = tool_fixture::hook();
        const auto hg = *core::drawingToolGeometry(hook);
        const auto hb = cad::makeDrawingTools(hook, hg, 5);
        const auto hs = cad::drawingPunchSweep(hook, hg, 5, 17, {});
        const auto obstacle = BRepPrimAPI_MakeBox(gp_Pnt(8, 54, 4), .5, .5, 1).Shape();
        gp_Trsf lifted;
        lifted.SetTranslation(gp_Vec(0, 17, 0));
        for (const auto &end : {hb[1], BRepBuilderAPI_Transform(hb[1], lifted, true).Shape()}) {
            BRepAlgoAPI_Common common(obstacle, end);
            require(common.IsDone() && volume(common.Shape()) < 1.e-8,
                    "Endpoint obstacle is not free");
        }
        BRepAlgoAPI_Cut missed(obstacle, hs);
        require(missed.IsDone() && volume(missed.Shape()) < 1.e-8,
                "Non-convex sweep missed between-endpoint collision");
        const auto pocket = BRepPrimAPI_MakeBox(gp_Pnt(8, 30, 4), .2, .2, 1).Shape();
        BRepAlgoAPI_Common falseHit(pocket, hs);
        require(falseHit.IsDone() && volume(falseHit.Shape()) < 1.e-8,
                "Sweep filled unreachable pocket with convex hull");
        struct Folder {
            std::filesystem::path path =
                std::filesystem::temp_directory_path() /
                ("drawing-tool-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            Folder() {
                std::filesystem::create_directory(path);
            }
            ~Folder() {
                std::error_code e;
                std::filesystem::remove_all(path, e);
            }
        } folder;
        auto file = (folder.path / "bend.step").string();
        STEPControl_Controller::Init();
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2., 2., std::numbers::pi / 2, 40., true, 20.),
                                STEPControl_AsIs) == IFSelect_RetDone,
                "Cannot transfer");
        require(writer.Write(file.c_str()) == IFSelect_RetDone, "Cannot write");
        auto doc = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
        require(doc && doc->metadata().bendAnalysis.bends.size() == 1, "No bend fixture");
        const auto build = [&](const core::ToolAssignment &a) {
            return cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
                doc, 0, {std::numbers::pi / 2, 2.}, a, {});
        };
        auto result = build(tools);
        if (auto *e = std::get_if<application::BendPreviewError>(&result))
            throw std::runtime_error("Real motion: " + e->detail + " frame " +
                                     std::to_string(e->frameIndex.value_or(0)));
        auto motion = std::get<application::BendPreviewPtr>(result);
        require(motion->usesDrawingTools() && motion->frameCount() == 61 &&
                    motion->punchRetraction(),
                "Incomplete real motion admitted");
        for (std::size_t i : {0u, 20u, 40u, 60u}) {
            const auto &frame = cad::OcctPreviewAccess::frame(*motion, i);
            require(std::abs(volume(frame.tools[0]) - volume(bodies[0])) < 1.e-5 &&
                        std::abs(volume(frame.tools[1]) - volume(bodies[1])) < 1.e-5,
                    "Displayed tool differs from checked source profile");
        }
        const auto setup = cad::SingleBendPreviewBuilder{}.buildAssignedToolSetup(
            doc, 0, {std::numbers::pi / 2, 2.}, tools, {});
        require(std::holds_alternative<application::BendPreviewPtr>(setup) &&
                    std::get<application::BendPreviewPtr>(setup)->usesDrawingTools() &&
                    std::get<application::BendPreviewPtr>(setup)->frameCount() == 1 &&
                    !std::get<application::BendPreviewPtr>(setup)->pressMotion(),
                "Static setup substituted idealized body");
        auto blocked = tool_fixture::pair(12, .8, 100);
        result = build(blocked);
        const auto *e = std::get_if<application::BendPreviewError>(&result);
        require(e && e->code == application::BendPreviewErrorCode::ToolIntersection &&
                    e->diagnostic && e->diagnostic->collision() &&
                    e->diagnostic->frameCount() == 1 && !e->diagnostic->punchRetraction(),
                "Collision lost diagnostic or became success");
        auto legacy = blocked;
        legacy.useDrawingGeometry = false;
        result = build(legacy);
        require(std::holds_alternative<application::BendPreviewPtr>(result),
                "Legacy mode silently used DXF body");
        std::cout
            << "DXF bodies, rounded contact, continuous return and collision diagnostics passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
