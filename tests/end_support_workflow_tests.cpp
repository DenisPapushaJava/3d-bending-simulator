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
        end_support_fixture::write(native(path), 8);
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
        auto *allowReliefs = window.findChild<QCheckBox *>("allowEndReliefs");
        require(limit && limit->value() == 3 && allowReliefs && allowReliefs->isChecked() &&
                    !limit->isEnabled(),
                "Default relief policy or retained strict limit is wrong");
        apply->click();
        wait([&] { return !controller.hasWork(); });
        if (controller.bendPreviewError())
            throw std::runtime_error(controller.bendPreviewError()->detail);
        require(controller.bendPreview() && controller.bendPreview()->hasPartialEndSupport() &&
                    summary->text().contains("8.00") &&
                    summary->text().contains(QStringLiteral("возможна деформация")) &&
                    !controller.bendPreview()->endSupportLimitMm() &&
                    summary->styleSheet().contains("#FFF2CC"),
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
        allowReliefs->setChecked(false);
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
        allowReliefs->setChecked(true);
        limit->setValue(3);
        apply->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.bendPreview() && controller.bendPreview()->hasPartialEndSupport() &&
                    controller.session().editRevision() == edit,
                "Restoring policy lost the preview or modified the CAD/project");
        require(ui::preferences()->value("checks/maximumEndSupportLossMm").toDouble() == 3,
                "End allowance was not stored as a local preference");
        require(ui::preferences()->value("checks/allowEndReliefs").toBool() &&
                    !controller.bendPreview()->endSupportLimitMm(),
                "Restored warning policy was not stored or its report lost scope");
        ui::MainWindow restored({"test", "test"}, files, files, builder, {tools.punch, tools.die});
        require(restored.findChild<QCheckBox *>("allowEndReliefs")->isChecked() &&
                    !restored.findChild<QDoubleSpinBox *>("maximumEndSupportLoss")->isEnabled(),
                "Warning policy was not restored in a new editor");
        window.findChild<QToolButton *>("processStage1")->click();
        window.findChild<QCheckBox *>("backgaugeEnabled")->setChecked(false);
        auto *recommend = window.findChild<QPushButton *>("recommendTools");
        require(recommend && recommend->isEnabled(), "No checked tool selection for end reliefs");
        recommend->click();
        wait([&] { return !controller.hasWork(); });
        if (const auto error = controller.bendRouteError())
            throw std::runtime_error("Conditional checked tooling: " + error->detail);
        require(controller.bendRoute() && controller.bendRoute()->selectedToolVariant &&
                    controller.bendRoute()->partialEndSupport &&
                    !controller.bendRoute()->steps.front().preview->endSupportLimitMm() &&
                    window.findChild<QLabel *>("toolRecommendation")
                        ->text()
                        .contains(QStringLiteral("торцевые подрезы")),
                "Checked tooling hid or lost the conditional route warning");
        std::cout << "Single-bend end allowance: default, warning, rejection, invalidation and "
                     "local persistence passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
