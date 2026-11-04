#include "EndSupportFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <iostream>
using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 90000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(done(), "Support workflow timeout");
    QApplication::processEvents();
}
std::filesystem::path native(const QString &s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "No temporary folder");
        ui::setPreferencesFile(folder.path() + "/settings.ini");
        const auto path = folder.path() + "/partial.step";
        end_support_fixture::write(native(path), .46);
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        const auto tools = end_support_fixture::tools();
        ui::MainWindow window({"test", "test"}, files, files, builder, {tools.punch, tools.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        auto &controller = window.importController();
        window.openStep(path);
        wait([&] { return !controller.hasWork(); });
        window.findChild<QToolButton *>("processStage1")->click();
        window.findChild<QComboBox *>("punchPreset")->setCurrentIndex(1);
        window.findChild<QComboBox *>("diePreset")->setCurrentIndex(1);
        window.findChild<QCheckBox *>("demoMachine")->setChecked(false);
        auto *limit = window.findChild<QDoubleSpinBox *>("maximumEndSupportLoss");
        auto *apply = window.findChild<QPushButton *>("applyToolSetup");
        auto *summary = window.findChild<QLabel *>("bendCheckSummary");
        require(limit && limit->value() == 3, "Default end allowance is not 3 mm");
        apply->click();
        wait([&] { return !controller.hasWork(); });
        if (controller.bendPreviewError())
            throw std::runtime_error(controller.bendPreviewError()->detail);
        require(controller.bendPreview() && controller.bendPreview()->hasPartialEndSupport() &&
                    summary->text().contains("0.46") && summary->styleSheet().contains("#FFF2CC"),
                "Single bend was blocked or presented as fully supported");
        if (const auto output = qEnvironmentVariable("BENDING_SUPPORT_SNAPSHOT");
            !output.isEmpty()) {
            auto image = window.grab().toImage();
            if (auto *viewer = window.findChild<viewer::ViewerWidget *>();
                viewer && viewer->isReady()) {
                QPainter painter(&image);
                painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            }
            require(image.save(output), "Cannot save support warning image");
        }
        const auto edit = controller.session().editRevision();
        window.findChild<QToolButton *>("processStage1")->click();
        limit->setValue(.25);
        require(!controller.bendPreview() && !controller.hasWork(),
                "Policy edit kept validation or started heavy work");
        apply->click();
        wait([&] { return !controller.hasWork(); });
        require(!controller.bendPreview() && controller.bendPreviewError() &&
                    controller.bendPreviewError()->code ==
                        application::BendPreviewErrorCode::EndSupportLimitExceeded &&
                    summary->text().contains(QStringLiteral("превышает допуск")),
                "A tighter allowance did not reject/report the shortage");
        window.findChild<QToolButton *>("processStage1")->click();
        limit->setValue(3);
        apply->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.bendPreview() && controller.bendPreview()->hasPartialEndSupport() &&
                    controller.session().editRevision() == edit,
                "Restoring policy lost the preview or modified the CAD/project");
        require(ui::preferences()->value("checks/maximumEndSupportLossMm").toDouble() == 3,
                "End allowance was not stored as a local preference");
        std::cout << "Single-bend end allowance: default, warning, rejection, invalidation and "
                     "local persistence passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
