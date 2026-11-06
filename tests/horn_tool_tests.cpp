#include "BendFixtures.h"
#include "HornToolGeometry.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/cad/occt/ToolGeometryCacheAccess.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BinTools.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
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
std::string bytes(const TopoDS_Shape &s) {
    std::ostringstream out;
    BinTools::Write(s, out);
    return out.str();
}
TopoDS_Shape moved(const TopoDS_Shape &s, double y) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, y, 0));
    return BRepBuilderAPI_Transform(s, t, true).Shape();
}
double common(const TopoDS_Shape &a, const TopoDS_Shape &b) {
    BRepAlgoAPI_Common c;
    TopTools_ListOfShape args, tools;
    args.Append(a);
    tools.Append(b);
    c.SetArguments(args);
    c.SetTools(tools);
    c.SetNonDestructive(true);
    c.SetFuzzyValue(1e-7);
    c.Build();
    require(c.IsDone() && !c.HasErrors(), "Common failed");
    return volume(c.Shape());
}
void contained(const TopoDS_Shape &part, const TopoDS_Shape &whole) {
    BRepAlgoAPI_Cut c;
    TopTools_ListOfShape args, tools;
    args.Append(part);
    tools.Append(whole);
    c.SetArguments(args);
    c.SetTools(tools);
    c.SetNonDestructive(true);
    c.SetFuzzyValue(1e-7);
    c.Build();
    require(c.IsDone() && !c.HasErrors() && volume(c.Shape()) < 1e-5, "Envelope omitted material");
}
int main() {
    try {
        // Stepped cross-section: upper flange only exists in x>2 above y=20.
        // Cutting a horn must not put that flange back during its upward return.
        BRepBuilderAPI_MakePolygon wire;
        for (auto p : {gp_Pnt(-2, 0, 0), gp_Pnt(2, 0, 0), gp_Pnt(2, 20, 0), gp_Pnt(10, 20, 0),
                       gp_Pnt(10, 40, 0), gp_Pnt(-2, 40, 0)})
            wire.Add(p);
        wire.Close();
        const auto straight =
            BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, 100)).Shape();
        const auto original = bytes(straight);
        core::ToolSegment section{100, 1, core::SegmentShape::LeftHorn, core::HornRelief{27, 27, 9},
                                  "left"};
        const auto obstacle = BRepPrimAPI_MakeBox(gp_Pnt(-1, 18, .5), 1, 1, 1).Shape();
        const auto left = cad::cutHornSection(straight, section, 0, 0, {});
        section.shape = core::SegmentShape::RightHorn;
        const auto right = cad::cutHornSection(straight, section, 0, 0, {});
        require(BRepCheck_Analyzer(left).IsValid() && volume(left) < volume(straight) &&
                    std::abs(volume(left) - volume(right)) < 1e-5 &&
                    common(left, obstacle) < 1e-8 && common(straight, obstacle) > .9 &&
                    common(right, obstacle) > .9,
                "Wrong horn end, shape, or material removal");
        for (auto relief : {core::HornRelief{27, 27, 9}, core::HornRelief{20, 27, 9},
                            core::HornRelief{30, 20, 9}}) {
            section.shape = core::SegmentShape::LeftHorn;
            section.horn = relief;
            const auto body = cad::cutHornSection(straight, section, 0, 0, {});
            require(BRepCheck_Analyzer(body).IsValid(),
                    "Invalid partial arc / straight nose transition");
            contained(body, straight);
            const auto sweep = cad::hornTranslationSweep(body, 40, {});
            require(BRepCheck_Analyzer(sweep).IsValid(), "Invalid horn sweep");
            for (double u : {0., .013, .29, .577, .943, 1.})
                contained(moved(body, 40 * u), sweep);
            const auto pocket = BRepPrimAPI_MakeBox(gp_Pnt(5, 30, .5), .2, .2, .2).Shape();
            require(common(sweep, pocket) < 1e-8, "Return filled an unreachable horn pocket");
            const auto between = BRepPrimAPI_MakeBox(gp_Pnt(0, 25, .5), .1, .1, .2).Shape();
            require(common(body, between) < 1e-8 && common(moved(body, 40), between) < 1e-8 &&
                        common(sweep, between) > .0019,
                    "Return missed a between-endpoint collision");
        }
        require(bytes(straight) == original, "Horn cut or sweep mutated source BRep");
        std::stop_source stop;
        stop.request_stop();
        bool cancelled = false;
        try {
            (void)cad::hornTranslationSweep(left, 40, stop.get_token());
        } catch (...) {
            cancelled = true;
        }
        require(cancelled, "Cancelled horn work completed");
        struct Folder {
            std::filesystem::path path =
                std::filesystem::temp_directory_path() /
                ("horn-tool-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            Folder() {
                std::filesystem::create_directory(path);
            }
            ~Folder() {
                std::error_code e;
                std::filesystem::remove_all(path, e);
            }
        } folder;
        auto cache = std::make_shared<cad::ToolGeometryCache>();
        cad::GeometryCacheScope scope(cache, {});
        auto tools = tool_fixture::pair(16);
        tools.punch.segments = std::vector<core::ToolSegment>{
            {100, 1, core::SegmentShape::LeftHorn, core::HornRelief{27, 27, 9}, "left"},
            {200, 1},
            {100, 1, core::SegmentShape::RightHorn, core::HornRelief{27, 27, 9}, "right"}};
        tools.punchAssembly = core::ToolAssembly{{0, 1, 2}, 0, {}};
        tools.die.lengthMm = 400;
        const auto geometry = core::drawingToolGeometry(tools);
        require(geometry.has_value(), "Horn drawing references rejected");
        const auto bodies = cad::makeDrawingTools(tools, *geometry, 5);
        const auto body = cad::segmentToolBody(bodies[1], tools.punch, tools.punchAssembly, 0, {});
        const auto sweep = cad::drawingPunchSweep(tools, *geometry, 5, 17, {});
        BRepClass3d_SolidClassifier middle(sweep, gp_Pnt(0, 25, 200), 1e-7);
        require(middle.State() == TopAbs_IN, "Touching sections retained internal return faces");
        for (double u : {0., .29, .577, 1.})
            contained(moved(body, 17 * u), sweep);
        // More than one section, a real rounded cross-section, and nonzero individual gaps.
        auto gapped = tools;
        gapped.punchAssembly = core::ToolAssembly{{0, 1, 2}, 0, {1, 2}};
        const auto gb = cad::makeDrawingTools(gapped, *core::drawingToolGeometry(gapped), 0);
        const auto gs =
            cad::drawingPunchSweep(gapped, *core::drawingToolGeometry(gapped), 0, 17, {});
        const auto assembled =
            cad::segmentToolBody(gb[1], gapped.punch, gapped.punchAssembly, -1.5, {});
        contained(moved(assembled, 8), gs);
        require(std::abs(volume(gs) - volume(sweep)) < 1e-4,
                "Axial gaps unexpectedly filled by sweep");
        auto clockwise = tools;
        auto &edges = clockwise.punch.drawing->edges;
        std::reverse(edges.begin(), edges.end());
        for (auto &e : edges) {
            std::swap(e.start, e.end);
            e.sweepRadians = -e.sweepRadians;
        }
        const auto cg = core::drawingToolGeometry(clockwise);
        require(bool(cg), "Clockwise source contour rejected");
        const auto cb = cad::makeDrawingTools(clockwise, *cg, 5);
        const auto ch =
            cad::segmentToolBody(cb[1], clockwise.punch, clockwise.punchAssembly, 0, {});
        const auto cs = cad::drawingPunchSweep(clockwise, *cg, 5, 17, {});
        contained(ch, body);
        contained(body, ch);
        contained(sweep, cs);
        contained(cs, sweep);
        auto changed = tools;
        changed.punch.segments->front().horn->projectionMm = 26;
        const auto changedBody =
            cad::segmentToolBody(bodies[1], changed.punch, changed.punchAssembly, 0, {});
        require(std::abs(volume(body) - volume(changedBody)) > .01,
                "Horn geometry reused stale cached body");
        require(std::abs(volume(body) - volume(cad::segmentToolBody(bodies[1], tools.punch,
                                                                    tools.punchAssembly, 0, {}))) <
                    1e-6,
                "Horn cache changed previous body");
        const auto file = (folder.path / "bend.step").string();
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 80, true, 20),
                                STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.c_str()) == IFSelect_RetDone,
                "Cannot write bend fixture");
        auto doc = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
        require(bool(doc), "Cannot import fixture");
        for (bool reversed : {false, true}) {
            tools.punchReversed = reversed;
            const auto motion = cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
                doc, 0, {std::numbers::pi / 2, 2}, tools, {});
            if (auto *error = std::get_if<application::BendPreviewError>(&motion))
                throw std::runtime_error("Horn cycle: " + error->detail);
            auto preview = std::get<application::BendPreviewPtr>(motion);
            require(preview->punchRetraction() && preview->frameCount() == 61,
                    "Horn full return not checked");
            const auto &f = cad::OcctPreviewAccess::frame(*preview, 0);
            require(std::abs(volume(f.tools[1]) - volume(body)) < 1e-4,
                    "Checked/rendered horn body mismatch");
        }
        std::cout << "Horn shapes, true return envelope, source isolation, cache and full cycles "
                     "passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
