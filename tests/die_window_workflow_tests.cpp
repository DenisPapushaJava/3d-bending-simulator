#include "DieWindowFixture.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 120000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    check(done(), "Workflow timeout");
    QApplication::processEvents();
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        ui::setPreferencesFile(folder.filePath("settings.ini"));
        ui::preferences()->setValue("backgauge/enabled", false);
        auto pair = tool_fixture::explicitPair(
            core::PressTools{16, std::numbers::pi / 3, 85 * std::numbers::pi / 180, .8, 120, 10});
        pair.punch.segments = std::vector<core::ToolSegment>{{120, 1}};
        pair.die.segments = std::vector<core::ToolSegment>{{120, 1}, {45, 1}, {55, 1}, {10, 1}};
        auto files = std::make_shared<project::ProjectFiles>();
        ui::MainWindow window({"test", "test"}, files, files,
                              std::make_shared<cad::SingleBendPreviewBuilder>(),
                              {pair.punch, pair.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        const auto file = folder.filePath("tab.step");
        STEPControl_Writer writer;
        check(writer.Transfer(fixtures::dieWindowPart(), STEPControl_AsIs) == IFSelect_RetDone &&
                  writer.Write(file.toUtf8().constData()) == IFSelect_RetDone,
              "Fixture write");
        auto &controller = window.importController();
        window.openStep(file);
        wait([&] { return !controller.hasWork(); });
        check(bool(controller.session().document()), "Import failed");
        window.findChild<QToolButton *>("processStage1")->click();
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        window.findChild<QCheckBox *>("backgaugeEnabled")->setChecked(false);
        window.findChild<QDoubleSpinBox *>("segmentGapLimit")->setValue(20);
        window.findChild<QPushButton *>("recommendTools")->click();
        wait([&] {
            return !controller.hasWork() && (controller.bendRoute() || controller.bendRouteError());
        });
        if (!controller.bendRoute())
            throw std::runtime_error(controller.bendRouteError()->detail);
        const auto route = controller.bendRoute();
        check(route->statistics.plannedDieWindows > 0,
              "GUI did not enable collision-driven window planning");
        std::optional<std::size_t> operation;
        for (std::size_t i = 0; i < route->steps.size(); ++i) {
            const auto &step = route->steps[i];
            const auto *entry = controller.session().bend(step.bendIndex);
            check(entry && entry->catalogTools, "Accepted route has no captured tools");
            if (entry->catalogTools->dieAssembly &&
                !entry->catalogTools->dieAssembly->jointGapsMm.empty()) {
                operation = i;
                check(entry->catalogTools->punch == pair.punch &&
                          entry->catalogTools->die == pair.die &&
                          core::maximumAssemblyGap(*entry->catalogTools->dieAssembly) <= 20,
                      "Window changed a profile or exceeded the explicit limit");
                for (const auto &member : step.additionalBends)
                    check(controller.session().bend(member.bendIndex)->catalogTools ==
                              entry->catalogTools,
                          "Joint window was not applied atomically");
            }
        }
        check(operation.has_value(), "No accepted die window");
        window.findChild<QToolButton *>("processStage2")->click();
        auto *step =
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(*operation + 1));
        check(step, "Missing operation button");
        step->click();
        wait([&] { return !controller.hasWork(); });
        if (app.arguments().contains("--graphics")) {
            auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
            wait([&] { return viewer->isReady(); });
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
            viewer->fitAll();
            QApplication::processEvents();
            if (const auto path = qEnvironmentVariable("BENDING_DIE_WINDOW_SNAPSHOT");
                !path.isEmpty())
                check(viewer->captureImage(true).save(path), "Snapshot failed");
        }
        const auto before = controller.session().bends();
        const auto saved = native(folder.filePath("window.bend"));
        controller.saveProject(saved, files);
        wait([&] { return !controller.hasWork(); });
        check(controller.saveError().isEmpty(), "Window save failed");
        controller.closeDocument();
        controller.open(saved);
        wait([&] { return !controller.hasWork(); });
        const auto &after = controller.session().bends();
        check(after.size() == before.size(), "Saved source bend coverage changed");
        for (std::size_t i = 0; i < before.size(); ++i)
            check(after[i].id == before[i].id && after[i].catalogTools == before[i].catalogTools &&
                      after[i].tools == before[i].tools &&
                      after[i].workpieceReversed == before[i].workpieceReversed &&
                      after[i].workpieceOffsetMm == before[i].workpieceOffsetMm,
                  "Automatic window or its source installation was lost on reopening");
        std::cout << "GUI selection: actual collision window, joint assignment, scene and portable "
                     "project\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
