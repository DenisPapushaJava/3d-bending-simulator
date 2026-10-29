#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < 180000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(done(), "Workflow timeout");
    QApplication::processEvents();
}
class Builder final : public application::SingleBendPreviewBuilder {
  public:
    mutable cad::SingleBendPreviewBuilder implementation;
    mutable std::atomic<unsigned> searches = 0, drafts = 0;
    mutable std::atomic<bool> delay = false, entered = false;
    application::BendRouteResult buildRoute(application::DocumentPtr d,
                                            application::BendRouteRequest r,
                                            std::stop_token s) const override {
        ++searches;
        return implementation.buildRoute(std::move(d), std::move(r), s);
    }
    application::RouteDraftResult buildRouteDraft(application::DocumentPtr d,
                                                  application::RouteDraftRequest r,
                                                  std::stop_token s) const override {
        ++drafts;
        if (delay.exchange(false)) {
            entered = true;
            QThread::msleep(200);
            s = {};
        }
        return implementation.buildRouteDraft(std::move(d), std::move(r), s);
    }
    void discardRouteCache() const override {
        implementation.discardRouteCache();
    }
    application::BendPreviewResult build(application::DocumentPtr d, std::size_t i,
                                         core::BendTarget t, bool b,
                                         std::stop_token s) const override {
        return implementation.build(std::move(d), i, t, b, s);
    }
};
std::vector<std::string> order(const ui::ImportController &c) {
    std::vector<std::string> ids;
    for (auto i : c.session().orderedBends())
        ids.push_back(c.session().bend(i)->id);
    return ids;
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("manual-route-test");
    QTemporaryDir folder;
    require(folder.isValid(), "Temporary directory failed");
    try {
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::tray(), STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write((folder.path() + "/source.step").toUtf8().constData()) ==
                        IFSelect_RetDone,
                "Fixture write failed");
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<Builder>();
        const auto pair = tool_fixture::pair(16);
        ui::MainWindow window({"test", "test"}, files, files, builder, {pair.punch, pair.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(folder.path() + "/source.step");
        auto &controller = window.importController();
        wait([&] { return !controller.hasWork(); });
        require(bool(controller.session().document()), "Import failed");
        const auto bytes = std::string(controller.session().document()->sourceStep());
        require(order(controller).size() == 4, "Expected four bends");
        for (auto i : controller.session().orderedBends())
            require(controller.setBendToolAssignment(i, pair), "Assignment failed");
        window.findChild<QToolButton *>("processStage2")->click();
        auto *task = window.findChild<QWidget *>("propertiesDock");
        auto *routeActions = window.findChild<QWidget *>("bendRoutePanel");
        auto *overlay = window.findChild<QWidget *>("operationOverlay");
        require(task && routeActions && task->isAncestorOf(routeActions) && overlay &&
                    overlay->isAncestorOf(window.findChild<QWidget *>("routeStepStrip")) &&
                    overlay->isAncestorOf(window.findChild<QWidget *>("playbackControls")),
                "Route commands or transport escaped the task panel and viewport");
        auto *mode = window.findChild<QComboBox *>("bendRouteMode");
        auto *more = window.findChild<QToolButton *>("routeActionsToggle");
        require(more && !more->isChecked() && !mode->isVisible(),
                "Advanced route controls are permanently exposed");
        const auto inputEdit = controller.session().editRevision();
        more->click();
        require(mode->isVisible() &&
                    window.findChild<QPushButton *>("routeMoveEarlier")->isVisible() &&
                    window.findChild<QPushButton *>("routeUndo")->isVisible(),
                "Expanded route actions lost reorder, history or scope controls");
        more->click();
        require(controller.session().editRevision() == inputEdit && !controller.hasWork(),
                "Opening route details edited inputs or launched CAD");
        mode->setCurrentIndex(2);
        auto step = [&](int n) {
            return window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(n));
        };
        require(step(0)->isEnabled(), "Zero requires a searched route");
        step(0)->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 0 &&
                    controller.routeDraft()->checkedToolPositions == 1 && builder->searches == 0,
                "Zero launched a search or skipped tool checks");
        auto *report = window.findChild<QToolButton *>("checkReportToggle");
        report->click();
        require(window.findChild<QLabel *>("bendPreviewStatus")->isVisible() &&
                    window.findChild<QLabel *>("bendRouteStatus")->isVisible(),
                "The shared report lost manual coverage or route details");
        report->click();
        auto *slider = window.findChild<QSlider *>("bendFrame");
        step(4)->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 4 &&
                    slider->value() == slider->maximum(),
                "Step does not show the formed prefix endpoint");
        const auto original = order(controller);
        auto expected = original;
        const auto id = expected.back();
        expected.pop_back();
        expected.insert(expected.begin() + 1, id);
        if (qEnvironmentVariableIsSet("BENDING_NATIVE_DRAG")) {
            window.raise();
            window.activateWindow();
            QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QApplication::processEvents();
            if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
                !output.isEmpty()) {
                auto shot = window.grab().toImage();
                auto *viewer = window.findChild<viewer::ViewerWidget *>();
                QPainter p(&shot);
                p.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
                p.end();
                require(shot.save(output + "-before-drag.png"), "Cannot capture drag layout");
            }
            const auto start = step(4)->mapToGlobal(step(4)->rect().center());
            const auto end = step(2)->mapToGlobal(QPoint(2, step(2)->height() / 2));
            QProcess mouse;
            const QString script = QStringLiteral(R"PY(import ctypes as c, sys, time
x=c.CDLL('libX11.so.6');t=c.CDLL('libXtst.so.6')
x.XOpenDisplay.restype=c.c_void_p
x.XFlush.argtypes=[c.c_void_p];x.XCloseDisplay.argtypes=[c.c_void_p]
t.XTestFakeMotionEvent.argtypes=[c.c_void_p,c.c_int,c.c_int,c.c_int,c.c_ulong]
t.XTestFakeButtonEvent.argtypes=[c.c_void_p,c.c_uint,c.c_int,c.c_ulong]
d=x.XOpenDisplay(None)
assert d
sx,sy,ex,ey=map(int,sys.argv[1:])
def move(a,b):
    t.XTestFakeMotionEvent(d,-1,a,b,0);x.XFlush(d);time.sleep(.2)
move(sx,sy)
t.XTestFakeButtonEvent(d,1,1,0);x.XFlush(d);time.sleep(.2)
move(sx-15,sy);move(ex,ey)
t.XTestFakeButtonEvent(d,1,0,0);x.XFlush(d);time.sleep(.2)
x.XCloseDisplay(d)
)PY");
            mouse.start("python3",
                        {"-c", script, QString::number(start.x()), QString::number(start.y()),
                         QString::number(end.x()), QString::number(end.y())});
            wait([&] { return mouse.state() == QProcess::NotRunning; });
            require(mouse.exitCode() == 0, "Native drag driver failed");
        } else {
            window.findChild<QPushButton *>("routeMoveEarlier")->click();
            window.findChild<QPushButton *>("routeMoveEarlier")->click();
        }
        wait([&] { return !controller.hasWork(); });
        if (order(controller) != expected)
            std::cerr << "Native reorder unchanged: step "
                      << (controller.routeDraft() ? controller.routeDraft()->request.step : 99)
                      << ", searches " << builder->searches << '\n';
        require(order(controller) == expected && step(2)->isChecked() && step(2)->text() == "2" &&
                    step(2)->property("routeId").toString().toStdString() == id &&
                    controller.routeDraft() && controller.routeDraft()->request.step == 2 &&
                    builder->searches == 0,
                "Reorder lost insertion, selected bend, numbering or invoked search");
        for (auto i : controller.session().orderedBends())
            require(controller.session().bend(i)->catalogTools == pair,
                    "Reorder changed a tool assignment");
        window.findChild<QPushButton *>("routeUndo")->click();
        if (!qEnvironmentVariableIsSet("BENDING_NATIVE_DRAG"))
            window.findChild<QPushButton *>("routeUndo")->click();
        wait([&] { return !controller.hasWork(); });
        require(order(controller) == original && step(4)->isChecked(), "Undo lost identity/order");
        window.findChild<QPushButton *>("routeRedo")->click();
        if (!qEnvironmentVariableIsSet("BENDING_NATIVE_DRAG"))
            window.findChild<QPushButton *>("routeRedo")->click();
        wait([&] { return !controller.hasWork(); });
        require(order(controller) == expected && step(2)->isChecked(), "Redo failed");
        window.findChild<QPushButton *>("routeEditTool")->click();
        window.findChild<QToolButton *>("processStage2")->click();
        require(window.findChild<QPushButton *>("checkBend")
                    ->text()
                    .contains(QStringLiteral("мой порядок")),
                "Opening the tool editor silently switched back to automatic sequencing");
        step(0)->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft()->request.step == 0 && !slider->isEnabled(),
                "Zero inherited a formed bend");
        // A slow response ignores cancellation on purpose. Only the newest pending selection may
        // publish.
        builder->delay = true;
        builder->entered = false;
        step(1)->click();
        wait([&] { return builder->entered.load(); });
        const auto started = builder->drafts.load();
        step(2)->click();
        step(3)->click();
        step(0)->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 0 &&
                    builder->drafts == started + 1,
                "Pending selections accumulated or a stale frame was accepted");
        require(builder->searches == 0 && !controller.bendRoute(),
                "Manual editing certified a route");
        // The explicit shared checker remains available and produces a separately typed route.
        mode->setCurrentIndex(0);
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->checkedToolPositions == 1 &&
                    controller.routeDraft()->visual->toolSetup(),
                "Full-check scope disabled manual tool collisions");
        window.findChild<QPushButton *>("checkBendRoute")->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.bendRoute() && builder->searches == 1 &&
                    !controller.bendRoute()->steps[0].preview->unverifiedDraft(),
                "Explicit checking lost the manual route");
        step(2)->click();
        window.findChild<QPushButton *>("routeMoveEarlier")->click();
        require(!controller.bendRoute(), "Reordering retained an old positive report");
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && builder->searches == 1,
                "Reorder launched an implicit full check");
        // Snapshot with real DXF tools and a selected partially formed frame.
        mode->setCurrentIndex(2);
        wait([&] { return !controller.hasWork(); });
        step(2)->click();
        wait([&] { return !controller.hasWork(); });
        slider->setValue(10);
        if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty()) {
            auto shot = window.grab().toImage();
            auto *viewer = window.findChild<viewer::ViewerWidget *>();
            if (viewer && viewer->isReady()) {
                QPainter p(&shot);
                p.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            }
            require(shot.save(output), "Capture failed");
        }
        const auto active = controller.routeDraft()->request.operations[1].bendIndex;
        // Nose R3 cannot fit the R2 inner arc. Scale the actual drawing too;
        // the taller-body fixture used by the channel test clears this short tray.
        auto blocked = tool_fixture::pair(16);
        constexpr double scale = 3 / .8;
        for (auto &edge : blocked.punch.drawing->edges) {
            edge.start.x *= scale;
            edge.start.y *= scale;
            edge.end.x *= scale;
            edge.end.y *= scale;
            if (edge.center) {
                edge.center->x *= scale;
                edge.center->y *= scale;
            }
        }
        blocked.punch.noseRadiusMm = 3;
        blocked.punch.heightMm = *blocked.punch.heightMm * scale;
        require(controller.setBendToolAssignment(active, blocked),
                "Cannot assign collision fixture");
        step(2)->click();
        wait([&] { return !controller.hasWork(); });
        const auto diagnostic = controller.routeDraft();
        require(bool(diagnostic), "Collision prevented manual editing");
        std::optional<std::size_t> collisionFrame;
        for (std::size_t i = 0; i < diagnostic->visual->frameCount(); ++i)
            if (diagnostic->visual->collisionAtFrame(i)) {
                collisionFrame = i;
                break;
            }
        require(collisionFrame.has_value(), "Manual real-tool collision was missed");
        slider->setValue(static_cast<int>(*collisionFrame));
        require(window.findChild<QLabel *>("bendCheckSummary")
                    ->text()
                    .contains(QStringLiteral("Столкновение"), Qt::CaseInsensitive),
                "A colliding frame is presented as clear");
        if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty()) {
            auto shot = window.grab().toImage();
            auto *viewer = window.findChild<viewer::ViewerWidget *>();
            if (viewer && viewer->isReady()) {
                QPainter p(&shot);
                p.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            }
            require(shot.save(output + "-collision.png"), "Collision capture failed");
        }
        require(controller.session().document()->sourceStep() == bytes, "Source was modified");
        const auto oldSource = controller.session().document();
        const auto oldRequest = controller.routeDraft()->request;
        const auto oldOrder = order(controller);
        builder->delay = true;
        builder->entered = false;
        step(1)->click();
        wait([&] { return builder->entered.load(); });
        controller.open(std::filesystem::path((folder.path() + "/source.step").toStdString()));
        require(!controller.prepareRouteDraft(oldRequest, builder) &&
                    !controller.setBendOrder(oldOrder),
                "A cancelled manual job allowed old-document edits during a new import");
        wait([&] { return !controller.hasWork(); });
        require(controller.session().document() && controller.session().document() != oldSource &&
                    !controller.routeDraft(),
                "An old draft survived a new import or stranded the worker queue");
        builder->delay = true;
        builder->entered = false;
        step(1)->click();
        wait([&] { return builder->entered.load(); });
        controller.closeDocument();
        wait([&] { return !controller.hasWork(); });
        require(!controller.routeDraft() && !controller.session().document(),
                "Closed document accepted a late draft");
        std::cout << "Manual prefixes, collision scope, stable reorder, undo/redo, explicit check "
                     "and stale selection passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
