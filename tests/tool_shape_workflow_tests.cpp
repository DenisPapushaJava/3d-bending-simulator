#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
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
void require(bool v, const std::string &why) {
    if (!v)
        throw std::runtime_error(why);
}
std::filesystem::path native(const QString &p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toStdString());
#endif
}
application::DocumentPtr load(const QString &name, const TopoDS_Shape &shape) {
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(name.toUtf8().constData()) == IFSelect_RetDone,
            "Write failed");
    auto doc = application::resultDocument(cad::StepImporter{}.load(native(name), {}, {}));
    require(bool(doc), "Import failed");
    return doc;
}
void wait(ui::ImportController &c) {
    QElapsedTimer timer;
    timer.start();
    while (c.hasWork() && timer.elapsed() < 120000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(!c.hasWork(), "Workflow timeout");
}
application::BendRoutePtr success(const application::BendRouteResult &r) {
    if (auto *e = std::get_if<application::BendRouteError>(&r))
        throw std::runtime_error(e->detail);
    return std::get<application::BendRoutePtr>(r);
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "Temporary folder failed");
        ui::setPreferencesFile(temp.path() + "/settings.ini");
        const auto file = temp.path() + "/z.step";
        const auto shape = fixtures::floorClearanceProfile(25, 50, 50, 120);
        const auto doc = load(file, shape);
        const auto &hints = doc->metadata().toolShapeHints;
        require(doc->metadata().bendAnalysis.bends.size() == 2 && hints.size() == 2,
                "Z neighbour projections missing");
        auto pair = tool_fixture::pair(16);
        pair.punch.lengthMm = pair.die.lengthMm = 120;
        pair.die.id = "z-narrow";
        for (auto &edge : pair.die.drawing->edges) {
            for (auto *p : {&edge.start, &edge.end}) {
                if (p->x == 20)
                    p->x = 12;
                if (p->x == -20)
                    p->x = -12;
            }
        }
        auto wide = pair;
        wide.die = tool_fixture::fourWayDie({16, 22, 35, 50}, 30);
        wide.die.lengthMm = 120;
        wide.die.id = "a-wide";
        const auto wr = core::toolShapeRisk(wide, hints, 2),
                   nr = core::toolShapeRisk(pair, hints, 2);
        require(wr && nr && *wr > *nr, "Source Z geometry did not favour the narrower matrix");
        std::cout << "Z risks wide=" << *wr << " narrow=" << *nr << std::endl;
        for (const auto height : {20., 45., 80.}) {
            const auto p = load(temp.path() + "/variant.step",
                                fixtures::floorClearanceProfile(height, 35, 65, 120));
            require(p->metadata().toolShapeHints.size() == 2,
                    "Variable-step/height hint extraction failed");
            const auto risk = core::toolShapeRisk(wide, p->metadata().toolShapeHints, 2);
            require(risk.has_value(), "Variable height could not be scored");
            if (height == 80)
                require(*risk == 0, "Large step was penalized by fixed 25mm rule");
        }
        gp_Trsf rotation;
        rotation.SetRotation(gp_Ax1(gp_Pnt(4, 9, 2), gp_Dir(1, 2, 3)), .73);
        rotation.SetTranslationPart(gp_Vec(103, -27, 60));
        const auto rotated = load(temp.path() + "/rotated.step",
                                  BRepBuilderAPI_Transform(shape, rotation, true).Shape());
        const auto rr = core::toolShapeRisk(wide, rotated->metadata().toolShapeHints, 2);
        require(rr && std::abs(*rr - *wr) < .02, "World orientation changed profile ranking");
        gp_Trsf mirror;
        mirror.SetMirror(gp_Ax2(gp_Pnt(), gp_Dir(1, 0, 0)));
        const auto mirrored = load(temp.path() + "/mirrored.step",
                                   BRepBuilderAPI_Transform(shape, mirror, true).Shape());
        const auto mr = core::toolShapeRisk(wide, mirrored->metadata().toolShapeHints, 2);
        require(mr && std::abs(*mr - *wr) < .02, "Mirror changed two-sided profile ranking");
        auto plain = load(temp.path() + "/plain.step", fixtures::bentStrip());
        require(plain->metadata().toolShapeHints.empty(), "Single bend invented a neighbour");
        const auto panel = application::resultDocument(cad::StepImporter{}.load(
            native(QString::fromUtf8(BENDING_TOOL_SHAPE_EXAMPLES) + "/wall-panel-1000mm.step"), {},
            {}));
        require(panel && panel->metadata().bendAnalysis.bends.size() == 16 &&
                    !panel->metadata().toolShapeHints.empty() &&
                    core::toolShapeRisk(wide, panel->metadata().toolShapeHints, 1.5).has_value(),
                "Multi-bend profile lost bounded source ranking");
        pair.backgauge.emplace();
        wide.backgauge.emplace();
        application::BendRouteRequest request;
        request.automatic = request.withTools = true;
        request.rankCandidates = false;
        request.maximumChecks = 96;
        request.toolVariants = {{wide, {}}, {pair, {}}};
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &b = doc->metadata().bendAnalysis.bends[i];
            request.operations.push_back({"B" + std::to_string(i),
                                          i,
                                          {b.angleRadians, b.innerRadiusMm},
                                          core::toolParameters(wide),
                                          wide});
        }
        auto referenceRequest = request;
        referenceRequest.rankToolShapes = false;
        cad::SingleBendPreviewBuilder reference, optimized;
        const auto started = std::chrono::steady_clock::now();
        const auto control = success(reference.buildRoute(doc, referenceRequest, {}));
        const auto referenceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
        const auto rankedStarted = std::chrono::steady_clock::now();
        const auto route = success(optimized.buildRoute(doc, request, {}));
        const auto rankedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - rankedStarted)
                                  .count();
        require(control->selectedToolVariant == 1 && control->statistics.testedToolVariants == 2 &&
                    route->selectedToolVariant == 1 && route->statistics.testedToolVariants == 1 &&
                    route->statistics.shapeRankedToolVariants == 2,
                "Ranking lost stable variant identity or did not avoid the broad die trial");
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &p = route->steps[i].preview;
            const auto &baseline = control->steps[i];
            require(route->steps[i].id == baseline.id &&
                        route->steps[i].fixedSecond == baseline.fixedSecond &&
                        p->backgauge()->placement == baseline.preview->backgauge()->placement,
                    "Tool-only ranking changed the same pair's route or gauge placement");
            require(p->frameCount() == 65 && p->backgauge() &&
                        p->backgauge()->status == application::BackgaugeStatus::Checked,
                    "Ranked route lost gauge checks");
            for (std::size_t f = 0; f < p->frameCount(); ++f)
                require(!p->collisionAtFrame(f) && !p->machineCollisionAtFrame(f),
                        "Ranked route bypassed collision");
        }
        std::cout << "Reference " << referenceMs << "ms, ranked " << rankedMs << "ms; ranking "
                  << route->statistics.toolShapeRankingUs << "us; pairs 2 -> 1\n";
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        ui::MainWindow window({"test", "test"}, files, files, builder,
                              {wide.punch, wide.die, pair.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(file);
        auto &c = window.importController();
        wait(c);
        window.findChild<QToolButton *>("processStage1")->click();
        wait(c);
        // Isolate profile clearance and gauges; these owned profiles have no real machine mounts.
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        auto *recommend = window.findChild<QPushButton *>("recommendTools");
        require(recommend && recommend->isEnabled(), "UI search disabled");
        recommend->click();
        wait(c);
        if (const auto e = c.bendRouteError())
            throw std::runtime_error("UI: " + e->detail);
        require(c.bendRoute() && c.session().bend(0)->catalogTools &&
                    c.session().bend(0)->catalogTools->die.id == pair.die.id,
                "UI ignored prepared shape hints");
        c.closeDocument();
        wait(c);
        window.close();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
