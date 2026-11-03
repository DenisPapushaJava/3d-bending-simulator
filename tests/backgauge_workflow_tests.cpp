#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/MachineAssembly.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::filesystem::path path(const QString &p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toStdString());
#endif
}
application::DocumentPtr document(const QString &p, double flange = 60) {
    STEPControl_Writer writer;
    require(writer.Transfer(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 300, true, flange),
                            STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(p.toUtf8().constData()) == IFSelect_RetDone,
            "Fixture write failed");
    auto doc = application::resultDocument(cad::StepImporter{}.load(path(p), {}, {}));
    require(doc && doc->metadata().bendAnalysis.bends.size() == 1, "Fixture import failed");
    return doc;
}
application::BendPreviewPtr preview(const application::BendPreviewResult &r) {
    if (const auto *e = std::get_if<application::BendPreviewError>(&r)) {
        if (e->diagnostic && e->diagnostic->backgauge() && e->diagnostic->backgauge()->placement) {
            const auto &g = *e->diagnostic->backgauge()->placement;
            std::cerr << "Gauge level " << g.level << " X " << g.xMm << " R " << g.rMm
                      << " contacts " << g.contactXmm << ',' << g.contactYmm << " Z " << g.zMm[0]
                      << ',' << g.zMm[1] << '\n';
            for (std::size_t f = 0; f < e->diagnostic->frameCount(); ++f)
                if (const auto hit = e->diagnostic->machineCollisionAtFrame(f)) {
                    std::cerr << "Frame " << f << ": " << hit->firstId << " / " << hit->secondId
                              << '\n';
                    break;
                }
        }
        throw std::runtime_error("Preview failed: " + e->detail);
    }
    return std::get<application::BendPreviewPtr>(r);
}
void wait(ui::ImportController &c) {
    QElapsedTimer timer;
    timer.start();
    while (c.hasWork() && timer.elapsed() < 60000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(!c.hasWork(), "UI timeout");
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "Temporary directory failed");
        ui::setPreferencesFile(temp.path() + "/settings.ini");
        const auto part = temp.path() + "/part.step";
        auto doc = document(part);
        auto pair = tool_fixture::tangPair();
        pair.machineModel = core::MachineModel::DemoPressV3;
        pair.punch.lengthMm = pair.die.lengthMm = 300;
        pair.backgauge.emplace();
        auto tools = *core::toolParameters(pair);
        const auto &b = doc->metadata().bendAnalysis.bends.front();
        const core::BendTarget target{b.angleRadians, b.innerRadiusMm};
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        const auto start = std::chrono::steady_clock::now();
        auto p = preview(builder->buildAssignedToolMotion(doc, 0, target, pair, {}));
        const auto coldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        require(p->backgauge() && p->backgauge()->status == application::BackgaugeStatus::Checked &&
                    p->backgauge()->checkedPositions == p->frameCount(),
                "Missing backgauge scope");
        const auto &frame = cad::OcctPreviewAccess::frame(*p, 0);
        require(std::count_if(frame.machine.begin(), frame.machine.end(),
                              [](const auto &c) { return c.gauging; }) == 2 &&
                    frame.backgaugeContacts.size() == 2,
                "Fingers/contacts not prepared");
        const auto warmStart = std::chrono::steady_clock::now();
        require(preview(builder->buildAssignedToolMotion(doc, 0, target, pair, {})) == p,
                "Warm transition not reused");
        const auto warmUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - warmStart)
                                .count();
        require(p->backgauge()->addedRetractionPositions == 4 && p->frameCount() == 65,
                "Withdrawal positions missing or forming positions replaced");
        auto fixed = pair;
        fixed.backgauge->retractMm = 0;
        const auto fixedCycle = builder->buildAssignedToolMotion(doc, 0, target, fixed, {});
        require(std::holds_alternative<application::BendPreviewError>(fixedCycle) &&
                    std::get<application::BendPreviewError>(fixedCycle).code ==
                        application::BendPreviewErrorCode::MachineIntersection,
                "Stationary backgauge collision was suppressed");
        const auto &conflict = std::get<application::BendPreviewError>(fixedCycle);
        require(conflict.diagnostic && conflict.frameIndex &&
                    conflict.diagnostic->machineCollisionAtFrame(*conflict.frameIndex) &&
                    conflict.diagnostic->frameCount() == *conflict.frameIndex + 1 &&
                    !conflict.diagnostic->punchRetraction(),
                "Backgauge collision has no stopping frame");
        auto impossible = pair;
        const auto gaugeX = p->backgauge()->placement->xMm;
        impossible.backgauge->xMinMm = gaugeX - .1;
        impossible.backgauge->xMaxMm = gaugeX + 5;
        const auto shortTravel = builder->buildAssignedToolMotion(doc, 0, target, impossible, {});
        require(std::holds_alternative<application::BendPreviewError>(shortTravel) &&
                    std::get<application::BendPreviewError>(shortTravel).code ==
                        application::BendPreviewErrorCode::BackgaugeUnavailable,
                "Withdrawal exceeded X travel");
        impossible.backgauge->xMinMm = 900;
        impossible.backgauge->xMaxMm = 1000;
        const auto rejected = builder->buildAssignedToolMotion(doc, 0, target, impossible, {});
        require(std::holds_alternative<application::BendPreviewError>(rejected) &&
                    std::get<application::BendPreviewError>(rejected).code ==
                        application::BendPreviewErrorCode::BackgaugeUnavailable,
                "Changed X limits reused a valid transition");
        application::BendRouteRequest route;
        route.withTools = true;
        route.automatic = true;
        route.operations.push_back({"B01", 0, target, tools, pair});
        const auto planned = builder->buildRoute(doc, route, {});
        if (const auto *e = std::get_if<application::BendRouteError>(&planned))
            throw std::runtime_error("Route: " + e->detail);
        require(std::get<application::BendRoutePtr>(planned)->steps.front().preview->backgauge(),
                "Route omitted backgauge");
        application::RouteDraftRequest draft{route.operations, 1, true, std::nullopt};
        const auto manual = builder->buildRouteDraft(doc, draft, {});
        require(std::holds_alternative<application::RouteDraftPtr>(manual) &&
                    std::get<application::RouteDraftPtr>(manual)->visual->backgauge(),
                "Manual transition omitted backgauge");
        cad::PreviewFrame collision;
        collision.shape = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
        collision.tools = {BRepPrimAPI_MakeBox(gp_Pnt(100, 100, 100), 5, 5, 5).Shape(),
                           BRepPrimAPI_MakeBox(gp_Pnt(200, 200, 200), 5, 5, 5).Shape()};
        collision.machine.push_back({"gauge", "Gauge",
                                     BRepPrimAPI_MakeBox(gp_Pnt(5, 0, 0), 10, 10, 10).Shape(),
                                     false, false, false, true});
        require(!cad::MachineAssembly::backgaugeCollisions(collision, .001, {}).empty(),
                "Sheet/finger collision ignored");
        collision.shape = BRepPrimAPI_MakeBox(gp_Pnt(-100, 0, 0), 5, 5, 5).Shape();
        collision.machine.push_back({"bed", "Bed", BRepPrimAPI_MakeBox(10, 10, 10).Shape()});
        require(!cad::MachineAssembly::backgaugeCollisions(collision, .001, {}).empty(),
                "Static machine/finger collision ignored");
        collision.floor = cad::FloorPlane{20, {}, {}};
        const auto floorHits = cad::MachineAssembly::backgaugeCollisions(collision, .001, {});
        require(std::any_of(floorHits.begin(), floorHits.end(),
                            [](const auto &hit) {
                                return hit.pair.firstId == "gauge" && hit.pair.secondId == "floor";
                            }),
                "Finger/floor collision ignored");
        auto files = std::make_shared<project::ProjectFiles>();
        application::DocumentSession session;
        session.complete(session.beginImport(path(part)), files->load(path(part), {}, {}));
        require(session.setBendToolAssignment(0, pair), "Assignment rejected");
        const auto project = temp.path() + "/part.bend";
        require(!files->save(path(project), *doc, {}, session.bends(), {}), "Save failed");
        auto loaded = files->load(path(project), {}, {});
        require(std::get<application::OpenedProject>(loaded).bends.front().catalogTools == pair,
                "Backgauge snapshot lost on reload");
        QFile file(project);
        require(file.open(QIODevice::ReadOnly), "Open JSON failed");
        auto json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        json.insert("version", 14);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Rewrite JSON failed");
        file.write(QJsonDocument(json).toJson());
        file.close();
        require(!application::resultDocument(files->load(path(project), {}, {})),
                "Legacy schema silently accepted backgauge scope");
        ui::MainWindow window({"test", "test"}, files, files, builder);
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(part);
        auto &controller = window.importController();
        wait(controller);
        require(controller.setBendToolAssignment(0, pair), "UI assignment failed");
        auto *check = window.findChild<QCheckBox *>("backgaugeEnabled");
        require(check && check->isChecked(), "UI did not restore backgauge snapshot");
        auto uiDraft = draft;
        uiDraft.operations.front().id = controller.session().bend(0)->id;
        require(controller.prepareRouteDraft(uiDraft, builder), "UI draft request rejected");
        wait(controller);
        require(controller.routeDraft() && controller.routeDraft()->visual->frameCount() == 25 &&
                    controller.routeDraft()->checkedToolPositions == 25,
                "UI rejected checked withdrawal positions");
        auto uiRoute = route;
        uiRoute.operations = uiDraft.operations;
        require(controller.prepareBendRoute(uiRoute, builder), "UI route request rejected");
        wait(controller);
        require(controller.bendRoute() &&
                    controller.bendRoute()->steps.front().preview->backgauge()->status ==
                        application::BackgaugeStatus::Checked,
                "UI rejected the checked backgauge route");
        require(controller.setBendToolAssignment(0, fixed) &&
                    controller.prepareBendPreview(0, false, builder,
                                                  application::BendPreviewMode::ToolMotion),
                "UI diagnostic request rejected");
        wait(controller);
        require(controller.bendPreviewError() && controller.bendPreviewError()->diagnostic &&
                    !controller.bendPreviewError()->diagnostic->punchRetraction(),
                "UI lost the safe collision prefix or retained a success certificate");
        require(controller.setBendToolAssignment(0, pair), "UI assignment restore failed");
        if (argc > 1 && std::string_view(argv[1]) == "--render") {
            auto *viewer = window.findChild<viewer::ViewerWidget *>();
            require(viewer, "Viewer missing");
            require(viewer->presentBendPreview(p, 0, controller.session().revision()).isEmpty(),
                    "Gauge presentation failed");
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
            viewer->fitAll();
            QApplication::processEvents();
            const auto capture = viewer->captureImage();
            require(!capture.isNull(), "Native capture failed");
            require(capture.save(QStringLiteral("out/backgauge-native.png")),
                    "Capture save failed");
        }
        std::cout << "Cold cycle: " << coldMs << " ms; warm: " << warmUs << " us; all checks: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count()
                  << " ms\n";
        controller.closeDocument();
        window.close();
        wait(controller);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
