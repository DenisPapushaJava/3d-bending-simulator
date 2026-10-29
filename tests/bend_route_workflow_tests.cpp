#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 120000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(done(), "Workflow timeout");
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QApplication::processEvents();
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "No folder");
        ui::setPreferencesFile(folder.path() + "/settings.ini");
        auto files = std::make_shared<project::ProjectFiles>();
        const bool withTools = app.arguments().contains("--tools");
        const auto pair = tool_fixture::pair(16);
        ui::MainWindow window(
            {"test", "test"}, files, files, std::make_shared<cad::SingleBendPreviewBuilder>(),
            withTools ? core::ToolCatalog{pair.punch, pair.die} : core::ToolCatalog{});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        auto path = folder.path() + "/channel.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::channel(), STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
                "Fixture write");
        auto &controller = window.importController();
        window.openStep(path);
        wait([&] { return !controller.hasWork(); });
        require(bool(controller.session().document()), "No document");
        auto *stage = window.findChild<QToolButton *>("processStage2");
        require(stage, "No check stage");
        stage->click();
        auto *mode = window.findChild<QComboBox *>("bendRouteMode");
        auto *prepare = window.findChild<QPushButton *>("prepareBendRoute");
        require(mode && prepare, "No route controls");
        if (withTools)
            window.findChild<QPushButton *>("recommendTools")->click();
        mode->setCurrentIndex(withTools ? 1 : 0);
        require(prepare->isEnabled(), "Cannot prepare geometry route");
        prepare->click();
        wait([&] { return !controller.hasWork(); });
        if (const auto error = controller.bendRouteError())
            throw std::runtime_error(error->detail);
        const auto route = controller.bendRoute();
        require(route && route->steps.size() == 2, "No complete channel route");
        require(route->withTools == withTools &&
                    route->steps[0].preview->usesDrawingPunch() == withTools &&
                    route->steps[0].preview->usesDrawingDie() == withTools,
                "UI route lost its actual tool profile mode");
        auto *viewerWidget = window.findChild<viewer::ViewerWidget *>("viewer");
        auto *strip = window.findChild<QWidget *>("routeStepStrip");
        require(viewerWidget && strip && viewerWidget->isAncestorOf(strip),
                "The route strip is outside the viewport");
        for (int i = 0; i <= 2; ++i)
            require(window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(i))->text() ==
                        QString::number(i),
                    "Route numbers contain feature IDs");
        require(window.findChild<QPushButton *>("routeStep0")
                    ->toolTip()
                    .contains(QStringLiteral("заготовка")),
                "Blank stage is ambiguous");
        auto *frame = window.findChild<QSlider *>("bendFrame");
        for (int index : {2, 0, 1, 2}) {
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(index))->click();
            require(controller.bendRoute() == route && !controller.hasWork() && frame->value() == 0,
                    "Stage navigation recalculated or retained a different frame");
            if (index == 0)
                require(!frame->isEnabled(), "The blank stage exposes a bending slider");
            else if (QApplication::platformName() != "offscreen")
                require(frame->isEnabled(), "Native operation playback is unavailable");
            if (index > 0)
                frame->setValue(10);
        }
        window.findChild<QPushButton *>("routeMoveEarlier")->click();
        require(!controller.bendRoute() && controller.session().modified() && !controller.hasWork(),
                "Reorder not invalidated");
        auto expected = controller.session().orderedBends();
        require(expected == std::vector<std::size_t>({1, 0}), "Order not moved");
        window.findChild<QPushButton *>("checkBendRoute")->click();
        wait([&] { return !controller.hasWork(); });
        if (const auto error = controller.bendRouteError())
            throw std::runtime_error(error->detail);
        require(controller.bendRoute() && controller.bendRoute()->steps[0].bendIndex == 1,
                "Manual order ignored");
        window.findChild<QPushButton *>("routeStep2")->click();
        frame->setValue(frame->maximum() / 2);
        window.resize(1120, 760);
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        require(strip->parentWidget()->rect().contains(strip->geometry()) && !controller.hasWork(),
                "Viewport strip clipped on resize");
        window.resize(1280, 850);
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty()) {
            QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QApplication::processEvents();
            auto shot = window.grab().toImage();
            if (auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
                viewer && viewer->isReady()) {
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
                painter.end();
            }
            require(shot.save(output), "Snapshot failed");
        }
        const auto saved = folder.path() + "/route.bend";
        controller.saveProject(native(saved), files);
        wait([&] { return !controller.hasWork(); });
        require(controller.saveError().isEmpty(), "Save failed");
        window.openStep(saved);
        wait([&] { return !controller.hasWork(); });
        require(controller.session().orderedBends() == expected && !controller.bendRoute(),
                "Saved preference lost or cached validation restored");
        prepare->click();
        require(controller.hasWork(), "No queued route");
        controller.closeDocument();
        wait([&] { return !controller.hasWork(); });
        require(!controller.bendRoute() && !controller.session().document(),
                "Stale route was applied after close");
        controller.shutdown();
        std::cout << "3D route navigation, reorder, playback frames, save/reopen and stale replies "
                     "passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
