#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
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
    mutable std::atomic<bool> delay = false, entered = false, fail = false;
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
        if (fail.exchange(false))
            return application::BendRouteError{application::BendRouteErrorCode::Failed,
                                               "Injected preparation failure"};
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
        auto *reverse = window.findChild<QToolButton *>("reversePunch");
        require(reverse && reverse->isEnabled() && reverse->toolTip().contains("для всех гибов"),
                "Blank setup does not offer global punch orientation");
        const auto indices = controller.session().orderedBends();
        auto other = pair;
        other.clearanceMm += 2;
        auto manual = *core::toolParameters(pair);
        manual.clearanceMm += 3;
        require(controller.setBendToolAssignment(indices[1], other) &&
                    controller.setBendTools(indices[2], manual),
                "Cannot prepare different per-bend tools");
        step(0)->click();
        wait([&] { return !controller.hasWork(); });
        const auto globalDrafts = builder->drafts.load();
        const auto originalOrder = order(controller);
        reverse->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 0 &&
                    controller.routeDraft()->checkedToolPositions == 1 &&
                    builder->drafts == globalDrafts + 1 && builder->searches == 0 &&
                    order(controller) == originalOrder && mode->currentIndex() == 2,
                "Global reversal searched an order, lost zero or skipped collision checks");
        for (const auto i : indices) {
            const auto *entry = controller.session().bend(i);
            auto expected =
                i == indices[2] ? manual : *core::toolParameters(i == indices[1] ? other : pair);
            expected.punchReversed = true;
            require(entry->tools == expected, "Global reversal replaced a per-bend setup");
            if (entry->catalogTools) {
                auto expectedAssignment = i == indices[1] ? other : pair;
                expectedAssignment.punchReversed = true;
                require(entry->catalogTools == expectedAssignment,
                        "Global reversal lost a catalog profile or assembly");
            } else
                require(i == indices[2], "Catalog assignment was converted to manual parameters");
        }
        step(3)->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 3 &&
                    controller.routeDraft()->checkedToolPositions == 21 &&
                    controller.routeDraft()->visual->toolSetup() &&
                    controller.routeDraft()->visual->toolSetup()->tools.punchReversed &&
                    reverse->isChecked() && builder->searches == 0,
                "Another step lost the global orientation or skipped its collision checks");
        const auto projectPath = folder.path() + "/global-orientation.bend";
        controller.saveProject(projectPath.toStdString(), files);
        wait([&] { return !controller.isSaving(); });
        require(controller.saveError().isEmpty(), "Cannot save global orientation");
        controller.closeDocument();
        window.openStep(projectPath);
        wait([&] { return !controller.hasWork(); });
        for (const auto i : controller.session().orderedBends())
            require(controller.session().bend(i)->tools->punchReversed,
                    "Global orientation was not restored from the project");
        window.findChild<QToolButton *>("processStage2")->click();
        step(0)->click();
        wait([&] { return !controller.hasWork(); });
        mode->setCurrentIndex(1); // A shared setup can also be set once on zero.
        wait([&] { return !controller.hasWork(); });
        require(reverse->isChecked(), "Shared zero setup lost its current orientation");
        reverse->click();
        wait([&] { return !controller.hasWork(); });
        require(mode->currentIndex() == 1 && controller.routeDraft() && builder->searches == 0,
                "Global reversal forced individual mode or launched search");
        for (const auto i : controller.session().orderedBends())
            require(controller.session().bend(i)->catalogTools == pair,
                    "Shared zero orientation did not apply the common setup to every bend");
        auto *report = window.findChild<QToolButton *>("checkReportToggle");
        report->click();
        require(window.findChild<QLabel *>("bendPreviewStatus")->isVisible() &&
                    window.findChild<QLabel *>("bendRouteStatus")->isVisible(),
                "The shared report lost manual coverage or route details");
        report->click();
        auto *slider = window.findChild<QSlider *>("bendFrame");
        mode->setCurrentIndex(1); // Shared setup before changing one operation's orientation.
        step(1)->click();
        wait([&] { return !controller.hasWork(); });
        auto *viewer = window.findChild<viewer::ViewerWidget *>();
        require(viewer, "Viewer missing");
        if (viewer->isReady())
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Front);
        slider->setValue(10);
        const auto originalScene = viewer->captureImage(false);
        const auto draftsBefore = builder->drafts.load();
        step(1)->click();
        require(!controller.hasWork() && builder->drafts == draftsBefore &&
                    slider->value() == slider->maximum(),
                "Reselecting a ready step queued a redundant CAD job or lost its endpoint");
        unsigned presentations = 0;
        const auto connection = QObject::connect(viewer, &viewer::ViewerWidget::selectionChanged,
                                                 viewer, [&](quint64, int) { ++presentations; });
        step(1)->click();
        QObject::disconnect(connection);
        require(presentations == 0 && builder->drafts == draftsBefore,
                "Reselecting the displayed endpoint rebuilt CAD or its presentation");
        slider->setValue(10);
        require(reverse && reverse->isEnabled(), "Manual punch orientation control missing");
        const auto searchesBefore = builder->searches.load();
        reverse->click();
        require(originalScene.isNull() || viewer->captureImage(false) == originalScene,
                "Manual reversal displayed the source part during invalidation");
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && mode->currentIndex() == 2 &&
                    builder->searches == searchesBefore,
                "Per-step reversal launched order search or stayed shared");
        require(slider->value() == 10, "Manual reversal reset the forming position");
        for (std::size_t i = 0; i < controller.routeDraft()->request.operations.size(); ++i)
            require(controller.routeDraft()->request.operations[i].tools->punchReversed == (i == 0),
                    "Reversal changed an unrelated operation");
        reverse->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() &&
                    !controller.routeDraft()->request.operations[0].tools->punchReversed,
                "Cannot restore manual punch orientation");
        require(slider->value() == 10 &&
                    (originalScene.isNull() || viewer->captureImage(false) == originalScene),
                "Manual reversal lost its camera or forming position");
        const auto beforeStepChange = viewer->captureImage(false);
        step(4)->click();
        require(beforeStepChange.isNull() || viewer->captureImage(false) == beforeStepChange,
                "Step selection flashed the source part while preparing the new state");
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 4 &&
                    slider->value() == slider->maximum(),
                "Step does not show the formed prefix endpoint");
        const auto beforeFailure = viewer->captureImage(false);
        builder->fail = true;
        step(3)->click();
        wait([&] { return !controller.hasWork(); });
        require(!controller.routeDraft() && controller.routeDraftError() &&
                    !window.findChild<QPushButton *>("playBend")->isEnabled() &&
                    window.findChild<QLabel *>("bendCheckSummary")
                        ->text()
                        .contains(QStringLiteral("Положение не проверено")) &&
                    (beforeFailure.isNull() || viewer->captureImage(false) == beforeFailure),
                "Failed replacement lost the scene or reused a positive result");
        auto *source = window.findChild<QPushButton *>("returnToCad");
        require(source && source->isEnabled(), "Cannot leave a failed replacement");
        source->click();
        window.findChild<QToolButton *>("processStage2")->click();
        step(4)->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 4,
                "Cannot recover from a failed replacement");
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
        if (viewer->isReady())
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Front);
        const auto checkedStep = viewer->captureImage(false);
        const auto readyDrafts = builder->drafts.load();
        step(3)->click();
        step(2)->click();
        require(!controller.hasWork() && builder->drafts == readyDrafts && builder->searches == 1 &&
                    slider->value() == slider->maximum() &&
                    (checkedStep.isNull() || viewer->captureImage(false) == checkedStep),
                "Ready route steps rebuilt CAD or reset the scene camera");
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
        const auto refreshedDraft = controller.routeDraft();
        const auto refreshedRevision = controller.session().editRevision();
        const auto draftsBeforeRefresh = builder->drafts.load();
        const auto searchesBeforeRefresh = builder->searches.load();
        if (viewer->isReady()) {
            auto *refresh = viewer->findChild<QAction *>("refreshScene");
            require(refresh && refresh->isEnabled(), "Manual scene refresh is unavailable");
            const auto beforeRefresh = viewer->captureImage(false);
            refresh->trigger();
            QApplication::processEvents();
            require(viewer->captureImage(false) == beforeRefresh && slider->value() == 10 &&
                        controller.routeDraft() == refreshedDraft && !controller.hasWork() &&
                        builder->drafts == draftsBeforeRefresh &&
                        builder->searches == searchesBeforeRefresh &&
                        controller.session().editRevision() == refreshedRevision,
                    "Scene refresh changed the manual step or reran its checker");
            require(overlay->isVisible() && overlay->height() >= overlay->sizeHint().height() &&
                        overlay->geometry().bottom() < overlay->parentWidget()->height(),
                    "Manual step strip is hidden or clipped after refresh");
        }
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
