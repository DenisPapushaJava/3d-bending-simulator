#include "BendFixtures.h"
#include "CollisionHighlight.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/MachineAssembly.h"
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
void floorGeometry() {
    auto a = tool_fixture::tangPair();
    a.machineModel = core::MachineModel::DemoPressV3;
    const auto tools = *core::toolParameters(a);
    const auto bodies = cad::makeDrawingTools(a, *core::drawingToolGeometry(a), 10);
    cad::MachineAssembly assembly(tools, bodies, {}, a);
    require(assembly.floor().has_value(), "V3 has no floor");
    cad::PreviewFrame frame;
    frame.tools = bodies;
    frame.machine = assembly.at(0);
    frame.floor = assembly.floor();
    const double y = frame.floor->yMm;
    for (const auto &part : frame.machine)
        if (part.id == "table") {
            Bnd_Box b;
            BRepBndLib::AddOptimal(part.shape, b, false, false);
            require(std::abs(b.CornerMin().Y() - y) < 1e-6,
                    "Floor is not tied to the machine base");
        }
    const auto hits = [&](TopoDS_Shape shape) {
        frame.shape = std::move(shape);
        return cad::MachineAssembly::collisions(frame, 1e-5, {});
    };
    // Far outside both the machine and the finite display rectangle.
    const auto body = [&](double bottom) {
        return BRepPrimAPI_MakeBox(gp_Pnt(20000, bottom, 20000), 20, 4, 20).Shape();
    };
    for (double bottom : {y + 1, y, y - 5e-6})
        require(hits(body(bottom)).empty(), "Clear/contact position rejected");
    auto bad = hits(body(y - 2));
    require(bad.size() == 1 && bad[0].pair.secondId == "floor" &&
                std::abs(volume(bad[0].overlap) - 800) < .02,
            "Infinite floor missed a partial crossing or clipped the wrong half-space");
    require(hits(body(y - 20)).size() == 1, "A whole body below the floor was ignored");
    // All sphere vertices are above the plane; curved extrema must still be checked.
    require(hits(BRepPrimAPI_MakeSphere(gp_Pnt(20000, y + 5, 20000), 10)).size() == 1,
            "Floor check only inspected vertices");
    frame.floor->surface.Nullify();
    frame.floor->grid.Nullify();
    require(hits(body(y - 2)).size() == 1, "Display geometry disabled the physical floor");
    frame.floor = assembly.floor();
    bad = hits(BRepPrimAPI_MakeBox(gp_Pnt(-1000, y - 2, 20), 10, 4, 10));
    frame.regions[0] = frame.shape;
    BRepMesh_IncrementalMesh mesh(frame.shape, .05, false, .2, false);
    cad::buildCollisionHighlights(frame, {{"sheet", "floor", bad.at(0).overlap}}, 2, {});
    for (const auto &id : {"sheet", "floor"})
        require(std::any_of(frame.collisionHighlights.begin(), frame.collisionHighlights.end(),
                            [&](const auto &p) { return p.bodyId == id && !p.vertices.empty(); }),
                "Floor conflict lost a surface contact patch");
    frame.floor->yMm = std::numeric_limits<double>::quiet_NaN();
    bool rejected = false;
    try {
        (void)cad::MachineAssembly::check(frame, 1e-5, {});
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Invalid floor level reported clear");
    frame.floor = assembly.floor();
    std::stop_source stopped;
    stopped.request_stop();
    rejected = false;
    try {
        (void)cad::MachineAssembly::check(frame, 1e-5, stopped.get_token());
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Cancelled floor check reported clear");
    a.machineModel = core::MachineModel::DemoPressV2;
    cad::MachineAssembly legacy(*core::toolParameters(a), bodies, {}, a);
    require(!legacy.floor(), "Legacy revision gained a floor silently");
    const auto old = legacy.at(0), modern = assembly.at(0);
    require(old.size() == modern.size(), "V3 changed the old machine bodies");
    for (std::size_t i = 0; i < old.size(); ++i)
        require(old[i].id == modern[i].id &&
                    std::abs(volume(old[i].shape) - volume(modern[i].shape)) < 1e-6,
                "V3 changed a saved machine profile");
}

void transitions(const std::filesystem::path &path) {
    STEPControl_Writer writer;
    require(writer.Transfer(fixtures::floorClearanceProfile(), STEPControl_AsIs) ==
                    IFSelect_RetDone &&
                writer.Write(path.string().c_str()) == IFSelect_RetDone,
            "Cannot export long profile");
    auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(doc && doc->metadata().bendAnalysis.bends.size() == 2, "Long Z recognition failed");
    cad::SingleBendPreviewBuilder builder;
    application::RouteDraftRequest request;
    request.withTools = true;
    auto a = tool_fixture::pair(16);
    a.machineModel = core::MachineModel::DemoPressV3;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto &b = doc->metadata().bendAnalysis.bends[i];
        request.operations.push_back(
            {std::to_string(i), i, {b.angleRadians, b.innerRadiusMm}, core::toolParameters(a), a});
    }
    std::swap(request.operations[0], request.operations[1]);
    request.step = 2;
    const auto result = builder.buildRouteDraft(doc, request, {});
    if (const auto *e = std::get_if<application::BendRouteError>(&result))
        throw std::runtime_error(e->detail);
    const auto draft = std::get<application::RouteDraftPtr>(result);
    bool conflict = false;
    for (std::size_t f = 0; f < draft->visual->frameCount(); ++f)
        if (const auto hit = draft->visual->machineCollisionAtFrame(f))
            conflict |= hit->secondId == "floor";
    require(conflict && draft->checkedToolPositions == 21 && draft->visual->floorChecked(),
            "Manual transitions bypassed the floor");
    application::BendRouteRequest route;
    route.operations = request.operations;
    route.withTools = true;
    const auto full = builder.buildRoute(doc, route, {});
    require(std::holds_alternative<application::BendRouteError>(full),
            "Full route certified a floor crossing");
    const auto &error = std::get<application::BendRouteError>(full);
    require(error.operationError == application::BendPreviewErrorCode::MachineIntersection &&
                error.detail.find("Пол") != std::string::npos,
            "Floor rejection lost its cause");
    auto old = request;
    for (auto &op : old.operations) {
        op.assignment->machineModel = core::MachineModel::DemoPressV2;
        op.tools = core::toolParameters(*op.assignment);
    }
    const auto legacy = builder.buildRouteDraft(doc, old, {});
    require(
        std::holds_alternative<application::RouteDraftPtr>(legacy) &&
            std::get<application::RouteDraftPtr>(legacy)->visual != draft->visual &&
            !cad::OcctPreviewAccess::frame(*std::get<application::RouteDraftPtr>(legacy)->visual, 0)
                 .floor,
        "Draft cache reused another floor scope");
    const auto warm = builder.buildRouteDraft(doc, request, {});
    require(std::get<application::RouteDraftPtr>(warm)->visual == draft->visual,
            "Floor scope was not cached");
    auto oldRoute = route;
    oldRoute.operations = old.operations;
    const auto oldFull = builder.buildRoute(doc, oldRoute, {});
    require(std::holds_alternative<application::BendRoutePtr>(oldFull),
            "Floor fixture also collides with legacy machine/tool geometry");
    const auto rechecked = builder.buildRoute(doc, route, {});
    require(std::holds_alternative<application::BendRouteError>(rechecked) &&
                std::get<application::BendRouteError>(rechecked).operationError ==
                    application::BendPreviewErrorCode::MachineIntersection,
            "Full transition cache reused the legacy floor-free certificate");
    route.automatic = true;
    const auto alternate = builder.buildRoute(doc, route, {});
    if (const auto *e = std::get_if<application::BendRouteError>(&alternate))
        throw std::runtime_error(e->detail);
    const auto sequence = std::get<application::BendRoutePtr>(alternate);
    require(sequence->steps.size() == 2 &&
                sequence->steps[0].bendIndex != request.operations[0].bendIndex,
            "Search retained the order that intersects the floor");
    for (const auto &step : sequence->steps)
        for (std::size_t f = 0; f < step.preview->frameCount(); ++f)
            require(!step.preview->machineCollisionAtFrame(f),
                    "Search published a floor collision");
}
} // namespace
int main() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("floor-profile-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".step");
    int status = 0;
    try {
        floorGeometry();
        transitions(path);
    } catch (const Standard_Failure &e) {
        std::cerr << e.GetMessageString() << '\n';
        status = 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    std::filesystem::remove(path);
    return status;
}
