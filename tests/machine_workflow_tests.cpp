#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/GeometryCacheFiles.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void wait(ui::ImportController &c) {
    QElapsedTimer timer;
    timer.start();
    while (c.hasWork() && timer.elapsed() < 60000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    QApplication::processEvents();
    require(!c.hasWork(), "Machine workflow timeout");
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("demo-machine-test");
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "Temporary directory failed");
        const auto path = folder.path() + "/part.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(), STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
                "Fixture failed");
        auto files = std::make_shared<project::ProjectFiles>();
        std::shared_ptr<const application::GeometryArtifactStore> artifacts;
        if (const auto cache = qEnvironmentVariable("BENDING_TOOL_CACHE_ROOT"); !cache.isEmpty()) {
#ifdef _WIN32
            const std::filesystem::path root(cache.toStdWString());
#else
            const std::filesystem::path root(cache.toStdString());
#endif
            artifacts = std::make_shared<project::GeometryCacheFiles>(root);
        }
        auto toolCache = std::make_shared<cad::ToolGeometryCache>(artifacts);
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>(toolCache);
        ui::MainWindow window({"test", "test"}, files, files, builder);
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(path);
        auto &controller = window.importController();
        wait(controller);
        require(bool(controller.session().document()), "Import failed");
        auto *checkbox = window.findChild<QCheckBox *>("demoMachine");
        require(checkbox && checkbox->isChecked(), "New part did not enable machine by default");
        core::PressTools tools{48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
        require(controller.setBendTools(0, tools), "Cannot set tools");
        auto *check = window.findChild<QPushButton *>("checkBend");
        require(checkbox && !checkbox->isChecked() && check && check->isEnabled(),
                "Legacy tool choice enabled machine or lost check action");
        checkbox->setChecked(true);
        auto tang = tool_fixture::tangPair();
        if (const auto path = qEnvironmentVariable("BENDING_PUNCH_CATALOG"); !path.isEmpty()) {
            const auto loaded = project::readToolCatalog(path.toStdString());
            require(std::holds_alternative<core::ToolCatalog>(loaded), "Private catalogue failed");
            const auto &catalogue = std::get<core::ToolCatalog>(loaded);
            const auto code = qEnvironmentVariable("BENDING_PUNCH_CODE");
            const auto found = std::find_if(catalogue.begin(), catalogue.end(), [&](const auto &t) {
                return t.kind == core::ToolKind::Punch && QString::fromStdString(t.code) == code;
            });
            require(found != catalogue.end(), "Private profile not found");
            tang.punch = *found;
            // Geometric fixture station only, not a claim about actual stock.
            tang.punch.segments.reset();
            tang.punch.lengthMm = 80;
            if (const auto dieCode = qEnvironmentVariable("BENDING_DIE_CODE"); !dieCode.isEmpty()) {
                const auto die =
                    std::find_if(catalogue.begin(), catalogue.end(), [&](const auto &t) {
                        return t.kind == core::ToolKind::Die &&
                               QString::fromStdString(t.code) == dieCode;
                    });
                require(die != catalogue.end(), "Private die not found");
                tang.die = *die;
                tang.die.segments.reset();
                tang.die.lengthMm = 80;
                tang.groove = 0;
            }
        }
        require(controller.setBendToolAssignment(0, tang), "Cannot set tang tool");
        check->click();
        wait(controller);
        auto preview = controller.bendPreview();
        require(preview && preview->machineModel() == core::MachineModel::DemoPressV2,
                "UI checkbox was not applied to the checked cycle");
        require(preview->seatedPunchMount(), "UI cycle did not seat tang in holder");
        auto *viewer = window.findChild<viewer::ViewerWidget *>();
        auto *frame = window.findChild<QSlider *>("bendFrame");
        require(viewer && frame && frame->maximum() >= 40, "Scene transport missing");
        if (viewer->isReady()) {
            auto *fitMachine = viewer->findChild<QAction *>("fitMachine");
            auto *visible = viewer->findChild<QAction *>("showMachine");
            require(fitMachine && visible && visible->isChecked(), "Default machine is hidden");
            QApplication::processEvents();
            const auto defaultView = viewer->captureImage(false);
            visible->setChecked(false);
            const auto hidden = viewer->captureImage(false);
            unsigned changed = 0;
            for (int y = 0; y < hidden.height(); y += 4)
                for (int x = 0; x < hidden.width(); x += 4)
                    changed += hidden.pixel(x, y) != defaultView.pixel(x, y);
            require(changed > static_cast<unsigned>(hidden.width() * hidden.height() / 320),
                    "Full machine geometry was filtered from the default view");
            visible->setChecked(true);
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Right);
            fitMachine->trigger();
            require(viewer->captureImage(false) == defaultView,
                    "Initial press view is not the right-hand profile");
        }
        frame->setValue(30);
        if (viewer->isReady()) {
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Right);
            viewer->findChild<QAction *>("lockPressVertical")->setChecked(true);
            viewer->findChild<QAction *>("fitMachine")->trigger();
        }
        const auto originalScene = viewer->captureImage(false);
        const auto refreshRevision = controller.session().editRevision();
        if (viewer->isReady()) {
            auto *refresh = viewer->findChild<QAction *>("refreshScene");
            require(refresh && refresh->isEnabled(), "Scene refresh is missing");
            refresh->trigger();
            QApplication::processEvents();
            require(viewer->captureImage(false) == originalScene && frame->value() == 30 &&
                        controller.bendPreview() == preview && !controller.hasWork() &&
                        controller.session().editRevision() == refreshRevision,
                    "Refresh changed the machine camera/frame or restarted CAD");
            auto *canvas = viewer->findChild<QWidget *>("cadCanvas");
            require(canvas, "Machine canvas is missing");
            QEvent nativeChange(QEvent::WinIdChange);
            QApplication::sendEvent(canvas, &nativeChange);
            QElapsedTimer redraw;
            redraw.start();
            while (!viewer->isReady() && viewer->errorString().isEmpty() &&
                   redraw.elapsed() < 10000)
                QApplication::processEvents();
            // Native recreation also queues the HUD layout; let it settle before
            // recording a baseline used by later asynchronous tool changes.
            QApplication::processEvents();
            require(viewer->isReady() && viewer->captureImage(false) == originalScene &&
                        frame->value() == 30 && controller.bendPreview() == preview &&
                        !controller.hasWork() &&
                        controller.session().editRevision() == refreshRevision,
                    "Native recreation lost the press frame, camera or whole-machine view");
        }
        auto *reverse = window.findChild<QToolButton *>("reversePunch");
        require(reverse && !reverse->icon().isNull() && reverse->text().isEmpty() &&
                    !window.findChild<QToolButton *>("normalPunch"),
                "Orientation is not one icon toggle");
        require(reverse && reverse->isEnabled() && !reverse->isChecked(),
                "Punch reversal control missing");
        const auto normalIcon = reverse->icon().cacheKey();
        reverse->click();
        require(!window.findChild<QPushButton *>("playBend")->isEnabled() &&
                    !controller.bendPreview(),
                "Old checked cycle remained available while replacing the installation");
        require(originalScene.isNull() || viewer->captureImage(false) == originalScene,
                "Reversal flashed the source part before the new cycle was ready");
        wait(controller);
        require(controller.bendPreview() && controller.bendPreview() != preview &&
                    controller.bendPreview()->toolSetup()->tools.punchReversed &&
                    reverse->isChecked() && reverse->icon().cacheKey() != normalIcon,
                "Punch reversal reused old geometry or failed to recheck");
        require(frame->value() == 30, "Reversal reset the current forming position");
        preview = controller.bendPreview();
        reverse->click();
        wait(controller);
        require(controller.bendPreview() && !reverse->isChecked() &&
                    !controller.bendPreview()->toolSetup()->tools.punchReversed &&
                    reverse->icon().cacheKey() == normalIcon,
                "Second toggle click did not restore normal orientation");
        const auto roundTripScene = viewer->captureImage(false);
        if (!originalScene.isNull() && roundTripScene != originalScene && argc > 1) {
            originalScene.save(QString::fromLocal8Bit(argv[1]) + "-before.png");
            roundTripScene.save(QString::fromLocal8Bit(argv[1]) + "-round-trip.png");
        }
        require(frame->value() == 30 && (originalScene.isNull() || roundTripScene == originalScene),
                "Round-trip reversal lost frame, camera, anchor or whole-machine view");
        reverse->click();
        wait(controller);
        require(controller.bendPreview() && reverse->isChecked() &&
                    controller.bendPreview()->toolSetup()->tools.punchReversed,
                "Third toggle click did not reverse again");
        preview = controller.bendPreview();
        auto *summary = window.findChild<QLabel *>("bendCheckSummary");
        require(summary && summary->text().contains("Демонстрационный станок"),
                "Demo scope not visible beside result");
        auto *visible = viewer->navigationMenu()
                            ? viewer->navigationMenu()->findChild<QAction *>("showMachine")
                            : nullptr;
        auto *whole = viewer->navigationMenu()
                          ? viewer->navigationMenu()->findChild<QAction *>("fitMachine")
                          : nullptr;
        require(!viewer->isReady() || (visible && whole), "Machine view actions missing");
        const auto revision = controller.session().editRevision();
        if (visible) {
            visible->setChecked(false);
            QApplication::processEvents();
            visible->setChecked(true);
        }
        QApplication::processEvents();
        require(controller.bendPreview() == preview && !controller.hasWork() &&
                    controller.session().editRevision() == revision,
                "Hiding machine disabled checks or invalidated the cycle");
        const auto capture = [&](QString suffix) {
            if (argc < 2)
                return;
            require(viewer->isReady(), "Native GPU unavailable");
            QApplication::processEvents();
            QImage image(window.size(), QImage::Format_RGB32);
            QPainter painter(&image);
            painter.drawPixmap(0, 0, window.grab());
            painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            painter.end();
            require(image.save(QString::fromLocal8Bit(argv[1]) + suffix), "Cannot save screenshot");
        };
        viewer->fitAll();
        frame->setValue(40);
        require(controller.bendPreview() == preview && !controller.hasWork(),
                "Frame change reran machine check");
        capture("-work-area.png");
        if (whole)
            whole->trigger();
        capture("-whole.png");
        viewer->fitAll();
        const auto projectPath = folder.path() + "/demo.bend";
        controller.saveProject(projectPath.toStdString(), files);
        wait(controller);
        require(controller.saveError().isEmpty(), "Machine project save failed");
        controller.closeDocument();
        window.openStep(projectPath);
        wait(controller);
        require(controller.session().bend(0)->tools->machineModel ==
                        core::MachineModel::DemoPressV2 &&
                    checkbox->isChecked(),
                "Saved machine not restored to UI");
        require(controller.session().bend(0)->tools->punchReversed && reverse->isChecked(),
                "Saved punch orientation not restored");
        checkbox->setChecked(false);
        check->click();
        wait(controller);
        require(controller.bendPreview() &&
                    controller.bendPreview()->machineModel() == core::MachineModel::None,
                "Cannot return to tool-only verification");
        controller.closeDocument();
        window.close();
        const auto statistics = toolCache->statistics();
        std::cout << "Tool cache: builds=" << statistics.builds << " RAM=" << statistics.memoryHits
                  << " disk=" << statistics.diskHits << '\n';
        if (qEnvironmentVariableIsSet("BENDING_EXPECT_TOOL_CACHE_HIT"))
            require(statistics.builds == 0 && statistics.diskHits > 0,
                    "Restarted UI rebuilt persisted tool geometry");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Machine selection, scope, visibility, saved project and legacy mode passed\n";
}
