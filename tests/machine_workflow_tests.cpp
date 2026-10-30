#include "BendFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
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
#include <STEPControl_Writer.hxx>
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
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
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
        check->click();
        wait(controller);
        const auto preview = controller.bendPreview();
        require(preview && preview->machineModel() == core::MachineModel::DemoPressV1,
                "UI checkbox was not applied to the checked cycle");
        auto *summary = window.findChild<QLabel *>("bendCheckSummary");
        require(summary && summary->text().contains("Демонстрационный станок"),
                "Demo scope not visible beside result");
        auto *viewer = window.findChild<viewer::ViewerWidget *>();
        require(viewer, "Viewer missing");
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
        auto *frame = window.findChild<QSlider *>("bendFrame");
        require(frame && frame->maximum() >= 40, "Frame transport missing");
        frame->setValue(40);
        require(controller.bendPreview() == preview && !controller.hasWork(),
                "Frame change reran machine check");
        capture("-local.png");
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
                        core::MachineModel::DemoPressV1 &&
                    checkbox->isChecked(),
                "Saved machine not restored to UI");
        checkbox->setChecked(false);
        check->click();
        wait(controller);
        require(controller.bendPreview() &&
                    controller.bendPreview()->machineModel() == core::MachineModel::None,
                "Cannot return to tool-only verification");
        controller.closeDocument();
        window.close();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Machine selection, scope, visibility, saved project and legacy mode passed\n";
}
