#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/MachineAssembly.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
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
application::DocumentPtr document(const QString &p, double flange = 60, double thickness = 2,
                                  double radius = 4) {
    STEPControl_Writer writer;
    require(writer.Transfer(
                fixtures::bentStrip(radius, thickness, std::numbers::pi / 2, 300, true, flange),
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
                              [](const auto &c) {
                                  return c.displayRole == cad::MachineDisplayRole::Backgauge &&
                                         c.gauging;
                              }) == 2,
                "Backgauge fingers lost their display role");
        require(std::count_if(frame.machine.begin(), frame.machine.end(),
                              [](const auto &c) { return c.gauging; }) == 2 &&
                    frame.backgaugeContacts.size() == 2,
                "Fingers/contacts not prepared");
        const auto raisedEdge = [](const application::SingleBendPreview &v) {
            require(v.backgauge() && v.backgauge()->placement, "Gauge placement missing");
            const auto &g = *v.backgauge()->placement;
            require(g.kind == core::GaugeReferenceKind::StraightEdge &&
                        std::abs(g.rMm + core::backgaugeHeightsMm[g.level] - .1) < 1.e-7,
                    "Ordinary gauge face was not raised above the die");
        };
        raisedEdge(*p);
        const auto warmStart = std::chrono::steady_clock::now();
        require(preview(builder->buildAssignedToolMotion(doc, 0, target, pair, {})) == p,
                "Warm transition not reused");
        const auto warmUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - warmStart)
                                .count();
        const auto thinDoc = document(temp.path() + "/thin.step", 60, .3, 1);
        const auto &thinBend = thinDoc->metadata().bendAnalysis.bends.front();
        auto thinTools = pair;
        const auto thinCycle = preview(builder->buildAssignedToolMotion(
            thinDoc, 0, {thinBend.angleRadians, thinBend.innerRadiusMm}, thinTools, {}));
        raisedEdge(*thinCycle);
        const auto &thinFrame = cad::OcctPreviewAccess::frame(*thinCycle, 0);
        for (const auto &q : thinFrame.backgaugeContacts)
            require(q.Y() > .1 && q.Y() < .3, "Thin-sheet contact left the physical edge");
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
        const auto &firstStep = std::get<application::BendRoutePtr>(planned)->steps.front();
        application::RouteDraftRequest blankRequest{route.operations, 0, true, std::nullopt};
        blankRequest.operations.front().workpieceReversed = firstStep.fixedSecond;
        blankRequest.operations.front().workpieceOffsetMm = firstStep.preview->workpieceOffsetMm();
        const auto blankResult = builder->buildRouteDraft(doc, blankRequest, {});
        require(std::holds_alternative<application::RouteDraftPtr>(blankResult),
                "Retained flat installation unavailable");
        const auto blank = std::get<application::RouteDraftPtr>(blankResult);
        require(blank->reusedVisual && blank->visual->unverifiedDraft() &&
                    blank->visual->frameCount() == 1 && blank->checkedToolPositions == 1 &&
                    blank->visual->floorChecked() && blank->visual->backgauge() &&
                    blank->visual->backgauge()->status == application::BackgaugeStatus::Checked &&
                    blank->visual->backgauge()->addedRetractionPositions == 0 &&
                    !blank->visual->pressMotion() && !blank->visual->punchRetraction(),
                "Flat reuse lost gauging/floor or acquired a cycle certificate");
        raisedEdge(*blank->visual);
        application::RouteDraftRequest draft{route.operations, 1, true, std::nullopt};
        const auto manual = builder->buildRouteDraft(doc, draft, {});
        require(std::holds_alternative<application::RouteDraftPtr>(manual) &&
                    std::get<application::RouteDraftPtr>(manual)->visual->backgauge(),
                "Manual transition omitted backgauge");
        raisedEdge(*std::get<application::BendRoutePtr>(planned)->steps.front().preview);
        raisedEdge(*std::get<application::RouteDraftPtr>(manual)->visual);
        const auto starter = project::readStarterToolCatalog();
        require(std::holds_alternative<core::ToolCatalog>(starter), "Starter tools unavailable");
        const auto &catalog = std::get<core::ToolCatalog>(starter);
        const auto preset = [&](const char *code) {
            const auto found = std::find_if(catalog.begin(), catalog.end(),
                                            [&](const auto &t) { return t.code == code; });
            require(found != catalog.end(), "Starter tool missing");
            auto copy = *found;
            copy.segments.reset();
            return copy;
        };
        const auto angle = application::resultDocument(
            cad::StepImporter{}.load(path(QString::fromUtf8(BENDING_GAUGE_EXAMPLE)), {}, {}));
        require(angle && angle->metadata().bendAnalysis.bends.size() == 1,
                "Starter angle import failed");
        core::ToolAssignment shortPair{preset("ST1405S"), preset("2067-1"), 3, 10, "", true};
        shortPair.punchReversed = core::defaultPunchOrientation(shortPair.punch).value_or(false);
        shortPair.machineModel = core::MachineModel::DemoPressV3;
        shortPair.backgauge.emplace();
        const auto &ab = angle->metadata().bendAnalysis.bends.front();
        application::BendRouteRequest shortRoute;
        shortRoute.automatic = shortRoute.withTools = true;
        shortRoute.operations.push_back({"B01",
                                         0,
                                         {ab.angleRadians, ab.innerRadiusMm},
                                         core::toolParameters(shortPair),
                                         shortPair});
        auto strictPair = shortPair;
        strictPair.backgauge->minimumSpacingMm = 110;
        strictPair.backgauge->allowOffsetContacts = false;
        shortRoute.toolVariants = {{strictPair, {}}, {strictPair, {}}, {strictPair, {}}};
        const auto spanStart = std::chrono::steady_clock::now();
        const auto strictResult = builder->buildRoute(angle, shortRoute, {});
        const auto *strictError = std::get_if<application::BendRouteError>(&strictResult);
        require(strictError && strictError->invariantGaugeSpanMm &&
                    *strictError->invariantGaugeSpanMm < 81 &&
                    strictError->evaluatedOperations == 0 &&
                    strictError->statistics.builtTransitions == 0 &&
                    strictError->statistics.testedToolVariants == 1 &&
                    strictError->statistics.gaugePrunedToolVariants == 2,
                "Tool-independent span rejection did not prune repeated tool trials");
        const auto spanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - spanStart)
                                .count();
        shortRoute.toolVariants.push_back({shortPair, {}});
        const auto shortResult = builder->buildRoute(angle, shortRoute, {});
        if (const auto *error = std::get_if<application::BendRouteError>(&shortResult))
            throw std::runtime_error("Short starter angle: " + error->detail);
        const auto shortPlan = std::get<application::BendRoutePtr>(shortResult);
        require(shortPlan->selectedToolVariant == 3 &&
                    shortPlan->statistics.gaugePrunedToolVariants == 2 &&
                    shortPlan->steps.front().preview->backgauge() &&
                    shortPlan->steps.front().preview->backgauge()->status ==
                        application::BackgaugeStatus::Checked,
                "New contact policy was incorrectly pruned by legacy spacing");
        const auto &shortGauge = *shortPlan->steps.front().preview->backgauge()->placement;
        require(shortGauge.contactZmm[1] - shortGauge.contactZmm[0] < 80 &&
                    shortGauge.zMm[1] - shortGauge.zMm[0] >= shortPair.backgauge->fingerWidthMm,
                "Short part lost physical contacts or overlapping fingers were accepted");
        std::cout << "Invariant rejection of 3 pairs: " << spanMs
                  << " ms, 0 cycles; short angle checked\n";
        const auto trayPath = path(QString::fromUtf8(BENDING_GAUGE_EXAMPLE)).parent_path() /
                              "tray-four-bends-2mm.step";
        const auto tray = application::resultDocument(cad::StepImporter{}.load(trayPath, {}, {}));
        require(tray && tray->metadata().bendAnalysis.bends.size() == 4, "Tray import failed");
        auto nonparallel = shortRoute;
        nonparallel.toolVariants.clear();
        nonparallel.operations.clear();
        nonparallel.maximumChecks = 1;
        nonparallel.allowWorkpieceRotation = false;
        auto hugeSpacing = shortPair;
        hugeSpacing.backgauge->minimumSpacingMm = 5000;
        const auto &trayBends = tray->metadata().bendAnalysis.bends;
        for (std::size_t i = 0; i < trayBends.size(); ++i)
            nonparallel.operations.push_back(
                {"T" + std::to_string(i),
                 i,
                 {trayBends[i].angleRadians, trayBends[i].innerRadiusMm},
                 core::toolParameters(hugeSpacing),
                 hugeSpacing});
        const auto nonparallelResult = builder->buildRoute(tray, nonparallel, {});
        const auto *nonparallelError = std::get_if<application::BendRouteError>(&nonparallelResult);
        require(nonparallelError && !nonparallelError->invariantGaugeSpanMm &&
                    nonparallelError->evaluatedOperations == 1,
                "Nonparallel bends were pruned by an unproven axial invariant");
        // A finger can clear the die yet hit a non-convex punch during +X travel.
        // In its recess the AABBs overlap but the occupied volumes do not.
        cad::PreviewFrame aboveDie;
        aboveDie.shape = BRepPrimAPI_MakeBox(gp_Pnt(-200, 0, 0), 2, 2, 2).Shape();
        aboveDie.tools[0] = BRepPrimAPI_MakeBox(gp_Pnt(-20, -20, -10), 40, 20, 20).Shape();
        aboveDie.tools[1] = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(gp_Pnt(-10, 1, -10), 40, 30, 20),
                                            BRepPrimAPI_MakeBox(gp_Pnt(0, 0, -11), 20, 12, 22))
                                .Shape();
        aboveDie.machine.push_back({"gauge", "Gauge",
                                    BRepPrimAPI_MakeBox(gp_Pnt(1, .1, -2), 10, 10, 4).Shape(),
                                    false, false, false, true});
        require(cad::MachineAssembly::backgaugeCollisions(aboveDie, .001, {}).empty(),
                "A free punch recess blocked a finger above the die");
        aboveDie.machine[0].shape = BRepPrimAPI_MakeBox(gp_Pnt(13, .1, -2), 10, 10, 4).Shape();
        const auto punchHits = cad::MachineAssembly::backgaugeCollisions(aboveDie, .001, {});
        require(std::any_of(punchHits.begin(), punchHits.end(),
                            [](const auto &hit) {
                                return hit.pair.firstId == "punch" && hit.pair.secondId == "gauge";
                            }) &&
                    std::none_of(punchHits.begin(), punchHits.end(),
                                 [](const auto &hit) { return hit.pair.firstId == "die"; }),
                "Clearing the die suppressed a punch/finger collision");
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
        // Explicit v15 centre-contact policy remains strict when opened by v16.
        require(session.setBendToolAssignment(0, strictPair), "Legacy assignment rejected");
        require(!files->save(path(project), *doc, {}, session.bends(), {}), "Legacy save failed");
        require(file.open(QIODevice::ReadOnly), "Legacy JSON open failed");
        auto legacyJson = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        legacyJson.insert("version", 15);
        auto legacyBends = legacyJson.value("bends").toArray();
        auto legacyBend = legacyBends[0].toObject();
        legacyBend.remove("workpieceOffsetMm");
        auto legacyTools = legacyBend.value("catalogTools").toObject();
        auto legacyGauge = legacyTools.value("backgauge").toObject();
        legacyGauge.remove("allowOffsetContacts");
        legacyTools.insert("backgauge", legacyGauge);
        legacyBend.insert("catalogTools", legacyTools);
        legacyBends[0] = legacyBend;
        legacyJson.insert("bends", legacyBends);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "Legacy JSON rewrite failed");
        file.write(QJsonDocument(legacyJson).toJson());
        file.close();
        const auto legacyLoaded = files->load(path(project), {}, {});
        require(application::resultDocument(legacyLoaded) &&
                    std::get<application::OpenedProject>(legacyLoaded).bends.front().catalogTools ==
                        strictPair,
                "v15 contact policy changed silently");
        json.insert("version", 15);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Rewrite v15 failed");
        file.write(QJsonDocument(json).toJson());
        file.close();
        require(!application::resultDocument(files->load(path(project), {}, {})),
                "New contact policy accepted as v15");
        json.insert("version", 14);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Rewrite JSON failed");
        file.write(QJsonDocument(json).toJson());
        file.close();
        require(!application::resultDocument(files->load(path(project), {}, {})),
                "Legacy schema silently accepted backgauge scope");
        // Match ToolLibrary's starter copy: length templates do not assert zero stock.
        auto workshop = catalog;
        for (auto &tool : workshop)
            tool.segments.reset();
        ui::MainWindow window({"test", "test"}, files, files, builder, workshop);
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(part);
        auto &controller = window.importController();
        wait(controller);
        auto *check = window.findChild<QCheckBox *>("backgaugeEnabled");
        require(check && check->isChecked(), "New part did not enable backgauge by default");
        auto legacy = pair;
        legacy.backgauge.reset();
        require(controller.setBendToolAssignment(0, legacy) && !check->isChecked(),
                "Legacy assignment silently enabled backgauge");
        require(controller.setBendToolAssignment(0, pair), "UI assignment failed");
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
        controller.closeDocument();
        wait(controller);
        window.openStep(QString::fromUtf8(BENDING_GAUGE_EXAMPLE));
        wait(controller);
        window.findChild<QToolButton *>("processStage1")->click();
        wait(controller);
        auto *recommend = window.findChild<QPushButton *>("recommendTools");
        require(recommend && recommend->isEnabled(), "Checked tool search unavailable");
        recommend->click();
        wait(controller);
        if (const auto error = controller.bendRouteError())
            throw std::runtime_error("Starter angle tool selection: " + error->detail);
        require(controller.bendRoute() && controller.bendRoute()->steps.size() == 1 &&
                    controller.session().bend(0)->catalogTools &&
                    controller.session().bend(0)->catalogTools->backgauge &&
                    controller.session().bend(0)->catalogTools->backgauge->minimumSpacingMm == 0 &&
                    controller.session().bend(0)->catalogTools->backgauge->allowOffsetContacts &&
                    controller.bendRoute()->steps.front().preview->backgauge()->status ==
                        application::BackgaugeStatus::Checked,
                "Default UI tool selection did not check the short starter angle with gauges");
        if (argc > 1 && std::string_view(argv[1]) == "--render") {
            auto *viewer = window.findChild<viewer::ViewerWidget *>();
            require(viewer, "Viewer missing");
            require(viewer
                        ->presentBendPreview(controller.bendRoute()->steps.front().preview, 0,
                                             controller.session().revision())
                        .isEmpty(),
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
