#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/MachineAssembly.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
Bnd_Box box(const TopoDS_Shape &s) {
    Bnd_Box result;
    BRepBndLib::AddOptimal(s, result, false, false);
    return result;
}
const cad::MachineDisplayComponent &find(const cad::PreviewFrame &frame, const std::string &id) {
    for (const auto &c : frame.machine)
        if (c.id == id)
            return c;
    throw std::runtime_error("Missing machine component: " + id);
}
application::BendPreviewPtr motion(const application::DocumentPtr &d, core::PressTools t) {
    const auto &b = d->metadata().bendAnalysis.bends.at(0);
    auto result = cad::SingleBendPreviewBuilder{}.buildToolMotion(
        d, 0, {b.angleRadians, b.innerRadiusMm}, t, {});
    if (const auto *e = std::get_if<application::BendPreviewError>(&result))
        throw std::runtime_error(e->detail);
    return std::get<application::BendPreviewPtr>(result);
}
void test(const std::filesystem::path &path) {
    STEPControl_Writer writer;
    require(writer.Transfer(fixtures::bentStrip(), STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(path.string().c_str()) == IFSelect_RetDone,
            "Cannot write fixture");
    const auto d = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(bool(d), "Cannot load machine fixture");
    for (bool reversed : {false, true}) {
        auto assignment = tool_fixture::tangPair();
        assignment.punchReversed = reversed;
        assignment.punch.segments = std::vector<core::ToolSegment>{{30, 1}, {50, 1}};
        assignment.punchAssembly = core::ToolAssembly{{0, 1}};
        const auto parameters = *core::toolParameters(assignment);
        const auto geometry = *core::drawingToolGeometry(assignment);
        auto bodies = cad::makeDrawingTools(assignment, geometry, 12);
        std::vector<cad::ToolDisplaySegment> segments;
        bodies[1] = cad::segmentToolBody(bodies[1], assignment.punch, assignment.punchAssembly, 0,
                                         {}, &segments);
        bodies[1] = cad::orientPunch(bodies[1], parameters, &segments);
        const auto probeBody = BRepPrimAPI_MakeBox(gp_Pnt(20, 47, 40), 1, 1, 1).Shape();
        BRepAlgoAPI_Common overlap(bodies[1], probeBody);
        require(overlap.IsDone(), "Orientation probe Boolean failed");
        GProp_GProps overlapVolume;
        BRepGProp::VolumeProperties(overlap.Shape(), overlapVolume);
        require(reversed ? std::abs(overlapVolume.Mass()) < 1.e-6
                         : std::abs(overlapVolume.Mass()) > .9,
                "Orientation did not change collision occupancy");
        require(std::abs(box(segments[0].shape).CornerMin().Z() - (reversed ? 50 : 0)) < 1.e-6,
                "Half-turn did not reverse longitudinal segment positions");
        cad::MachineAssembly installed(parameters, bodies, {}, assignment);
        require(installed.seatedPunchMount(), "V2 did not seat the punch tang");
        cad::PreviewFrame assembly;
        assembly.machine = installed.at(0);
        assembly.tools = bodies;
        assembly.shape = BRepPrimAPI_MakeBox(gp_Pnt(-1000, 2000, -1000), 1, 1, 1);
        require(!cad::MachineAssembly::check(assembly, 1.e-6, {}),
                "Seated punch intersects holder");
        const auto adapter = find(assembly, "die-adapter");
        require(adapter.displayRole == cad::MachineDisplayRole::ToolMount &&
                    find(assembly, "holder-4").displayRole == cad::MachineDisplayRole::ToolMount &&
                    find(assembly, "ram").displayRole == cad::MachineDisplayRole::Structure &&
                    find(assembly, "table").displayRole == cad::MachineDisplayRole::Structure,
                "Assembly did not distinguish mounts from large structure");
        const double top = box(adapter.shape).CornerMax().Y();
        const double dieBottom = box(bodies[0]).CornerMin().Y();
        require(std::abs(top - dieBottom - 10) < 1.e-6 && !adapter.allowDieContact,
                "Die was not seated on pocket floor or adapter is exempt from collision");
        auto adapterProbe = assembly;
        adapterProbe.shape = BRepPrimAPI_MakeBox(gp_Pnt(-1, top - 5, 40), 2, 2, 2);
        require(!cad::MachineAssembly::check(adapterProbe, 1.e-6, {}),
                "Die adapter pocket was filled");
        adapterProbe.shape = BRepPrimAPI_MakeBox(gp_Pnt(35, top - 5, 40), 2, 2, 2);
        require(cad::MachineAssembly::check(adapterProbe, 1.e-6, {}).has_value(),
                "Adapter side rail collision missed");
        adapterProbe.shape = assembly.shape;
        adapterProbe.tools[0] = BRepPrimAPI_MakeBox(gp_Pnt(-35, dieBottom, 0), 70, 25, 80);
        const auto incompatible = cad::MachineAssembly::check(adapterProbe, 1.e-6, {});
        require(incompatible && incompatible->firstId == "die" &&
                    incompatible->secondId == "die-adapter",
                "Wide die was accepted through adapter rails");
        const auto holder = find(assembly, "holder-4");
        const double base = box(holder.shape).CornerMin().Y();
        require(std::abs(box(bodies[1]).CornerMax().Y() - base - 30) < 1.e-6,
                "Holder was placed on top rather than at the shoulder");
        require(!holder.allowPunchContact, "V2 still exempts whole punch/holder pair");
        const auto mount = *core::punchMountReference(assignment);
        assembly.shape = BRepPrimAPI_MakeBox(gp_Pnt(mount.centreX - 1, base + 5, 40), 2, 2, 2);
        require(!cad::MachineAssembly::check(assembly, 1.e-6, {}), "Tang pocket was filled");
        assembly.shape = BRepPrimAPI_MakeBox(gp_Pnt(mount.centreX - 20, base + 5, 40), 2, 2, 2);
        require(cad::MachineAssembly::check(assembly, 1.e-6, {}).has_value(),
                "Holder leg collision missed");
        const auto &bend = d->metadata().bendAnalysis.bends[0];
        const auto cycle = cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
            d, 0, {bend.angleRadians, bend.innerRadiusMm}, assignment, {});
        if (const auto *e = std::get_if<application::BendPreviewError>(&cycle))
            throw std::runtime_error("Tang cycle: " + e->detail);
        const auto &preview = *std::get<application::BendPreviewPtr>(cycle);
        require(preview.seatedPunchMount() && preview.toolSetup()->tools.punchReversed == reversed,
                "Checked cycle dropped seating/orientation");
        const auto &frame = cad::OcctPreviewAccess::frame(preview, 0);
        require(reversed ? box(frame.tools[1]).CornerMin().X() < -30
                         : box(frame.tools[1]).CornerMax().X() > 30,
                "Asymmetric body was not reversed in the checked frame");
    }
    const std::string source(d->sourceStep());
    core::PressTools t{48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
    t.machineModel = core::MachineModel::DemoPressV1;
    const auto p = motion(d, t);
    require(p->machineModel() == t.machineModel && p->frameCount() == 61 && p->punchRetraction(),
            "Demo changed the checked tool cycle");
    const auto &first = cad::OcctPreviewAccess::frame(*p, 0);
    require(first.machine.size() == 12, "Incomplete assembly");
    for (std::size_t f = 0; f < p->frameCount(); ++f) {
        const auto &frame = cad::OcctPreviewAccess::frame(*p, f);
        require(!p->machineCollisionAtFrame(f), "Unexpected baseline machine collision");
        const double lift = p->pressState(f)->punchTipYmm - p->pressState(0)->punchTipYmm;
        for (const auto &c : frame.machine) {
            require(c.displayRole == find(first, c.id).displayRole &&
                        c.displayRole != cad::MachineDisplayRole::Unknown,
                    "Machine motion dropped the display role");
            require(BRepCheck_Analyzer(c.shape).IsValid(), "Invalid machine solid");
            const double moved =
                box(c.shape).CornerMin().Y() - box(find(first, c.id).shape).CornerMin().Y();
            require(std::abs(moved - (c.moving ? lift : 0)) < 1.e-6,
                    "Upper/lower group placement drifted");
        }
    }
    auto probe = first;
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(-1000, 2000, -1000), 1, 1, 1);
    require(!cad::MachineAssembly::check(probe, 1.e-6, {}), "Baseline assembly is obstructed");
    const double seat = box(find(first, "holder-4").shape).CornerMin().Y();
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(0, seat + 2, t.lengthMm / 2), 2, 2, 2);
    auto collision = cad::MachineAssembly::check(probe, 1.e-6, {});
    require(collision && collision->firstId == "sheet" && collision->secondId == "holder-4",
            "Tool-clear sheet missed its holder collision");
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(0, seat + 2, t.lengthMm / 2 + 150), 2, 2, 2);
    require(!cad::MachineAssembly::check(probe, 1.e-6, {}), "Gap between holders filled");
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(500, 0, t.lengthMm / 2 + 1350), 2, 2, 2);
    collision = cad::MachineAssembly::check(probe, 1.e-6, {});
    require(collision && collision->secondId == "frame-right", "Side frame obstacle missed");
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(100, 0, t.lengthMm / 2 + 1350), 2, 2, 2);
    require(!cad::MachineAssembly::check(probe, 1.e-6, {}), "Throat was filled");
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(500, 0, t.lengthMm / 2), 2, 2, 2);
    require(!cad::MachineAssembly::check(probe, 1.e-6, {}), "Frame extruded across whole press");
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(-1000, 2000, -1000), 1, 1, 1);
    for (auto &component : probe.machine)
        if (component.id == "holder-4") {
            gp_Trsf lower;
            lower.SetTranslation(
                gp_Vec(0, box(find(first, "table").shape).CornerMin().Y() + 100 - seat, 0));
            component.shape = component.shape.Moved(TopLoc_Location(lower));
        }
    collision = cad::MachineAssembly::check(probe, 1.e-6, {});
    require(collision && collision->firstId == "holder-4" && collision->secondId == "table",
            "Moving upper group missed stationary table");
    probe = first;
    probe.shape = BRepPrimAPI_MakeBox(gp_Pnt(-1000, 2000, -1000), 1, 1, 1);
    probe.machine[0].shape.Nullify();
    bool failed = false;
    try {
        (void)cad::MachineAssembly::check(probe, 1.e-6, {});
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, "Invalid machine body reported clear");
    const auto &b = d->metadata().bendAnalysis.bends.at(0);
    application::RouteDraftRequest request;
    request.operations = {{"B1", 0, {b.angleRadians, b.innerRadiusMm}, t, {}}};
    request.step = 1;
    request.withTools = true;
    cad::SingleBendPreviewBuilder builder;
    auto draft = builder.buildRouteDraft(d, request, {});
    require(std::holds_alternative<application::RouteDraftPtr>(draft), "Demo draft failed");
    const auto visual = std::get<application::RouteDraftPtr>(draft)->visual;
    require(visual->machineModel() == t.machineModel && visual->frameCount() == 21 &&
                !cad::OcctPreviewAccess::frame(*visual, 20).machine.empty(),
            "Manual path dropped the machine");
    auto none = request;
    none.operations[0].tools->machineModel = core::MachineModel::None;
    const auto plain = builder.buildRouteDraft(d, none, {});
    require(
        std::holds_alternative<application::RouteDraftPtr>(plain) &&
            std::get<application::RouteDraftPtr>(plain)->visual != visual &&
            cad::OcctPreviewAccess::frame(*std::get<application::RouteDraftPtr>(plain)->visual, 0)
                .machine.empty(),
        "Draft cache reused another machine scope");
    require(std::get<application::RouteDraftPtr>(builder.buildRouteDraft(d, request, {}))->visual ==
                visual,
            "Same machine draft not reused");
    auto reversedDraft = request;
    reversedDraft.operations[0].tools->punchReversed = true;
    const auto differentDraft = builder.buildRouteDraft(d, reversedDraft, {});
    require(std::holds_alternative<application::RouteDraftPtr>(differentDraft) &&
                std::get<application::RouteDraftPtr>(differentDraft)->visual != visual,
            "Draft cache reused another punch orientation");
    application::BendRouteRequest route;
    route.operations = request.operations;
    route.withTools = true;
    const auto full = builder.buildRoute(d, route, {});
    if (const auto *e = std::get_if<application::BendRouteError>(&full))
        throw std::runtime_error("Demo route: " + e->detail);
    require(std::get<application::BendRoutePtr>(full)->steps[0].preview->machineModel() ==
                t.machineModel,
            "Full route omitted machine");
    const auto warm = builder.buildRoute(d, route, {});
    require(std::get<application::BendRoutePtr>(warm)->statistics.reusedTransitions > 0,
            "Same machine route not reused");
    route.operations[0].tools->punchReversed = true;
    const auto reversedRoute = builder.buildRoute(d, route, {});
    require(std::holds_alternative<application::BendRoutePtr>(reversedRoute) &&
                std::get<application::BendRoutePtr>(reversedRoute)->statistics.reusedTransitions ==
                    0,
            "Transition cache reused another punch orientation");
    route.operations = none.operations;
    const auto plainRoute = builder.buildRoute(d, route, {});
    require(std::holds_alternative<application::BendRoutePtr>(plainRoute) &&
                std::get<application::BendRoutePtr>(plainRoute)->statistics.reusedTransitions ==
                    0 &&
                std::get<application::BendRoutePtr>(plainRoute)->steps[0].preview->machineModel() ==
                    core::MachineModel::None,
            "Transition cache reused another machine");
    require(d->sourceStep() == source, "Machine altered the imported STEP");
    STEPControl_Writer multiWriter;
    require(multiWriter.Transfer(fixtures::channel(), STEPControl_AsIs) == IFSelect_RetDone &&
                multiWriter.Write(path.string().c_str()) == IFSelect_RetDone,
            "Cannot write channel");
    const auto channel = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(channel && channel->metadata().bendAnalysis.bends.size() == 2, "Channel import failed");
    application::RouteDraftRequest multi;
    multi.withTools = true;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto &bend = channel->metadata().bendAnalysis.bends[i];
        multi.operations.push_back(
            {"B" + std::to_string(i), i, {bend.angleRadians, bend.innerRadiusMm}, t, {}});
    }
    for (std::size_t step = 0; step <= 2; ++step) {
        multi.step = step;
        const auto built = builder.buildRouteDraft(channel, multi, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&built))
            throw std::runtime_error("Machine multi-bend draft: " + e->detail);
        const auto &r = *std::get<application::RouteDraftPtr>(built);
        require(r.visual->machineModel() == t.machineModel &&
                    r.checkedToolPositions == (step ? 21 : 1),
                "Multi-bend prefix lost its machine checks");
    }
}
} // namespace
int main() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("demo-machine-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".step");
    try {
        test(path);
        std::filesystem::remove(path);
    } catch (const std::exception &e) {
        std::filesystem::remove(path);
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Machine movement, obstacles, open throat, holder gaps and cache scope passed\n";
}
