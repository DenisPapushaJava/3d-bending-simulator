#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
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
#include <QWindow>
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
void verifyToolStageTransitions(const QString &folder) {
    auto files = std::make_shared<project::ProjectFiles>();
    auto builder = std::make_shared<Builder>();
    auto pair = tool_fixture::pair(16);
    auto hook = tool_fixture::hook().punch;
    hook.id = "hook";
    hook.code = "Hook";
    auto shortPunch = pair.punch;
    shortPunch.id = "short";
    shortPunch.code = "Short";
    shortPunch.lengthMm = 40;
    auto specialDie = tool_fixture::pair(20).die;
    specialDie.id = "special-die";
    specialDie.segments = std::vector<core::ToolSegment>{{40, 2}};
    ui::MainWindow window({"test", "test"}, files, files, builder,
                          {pair.punch, hook, shortPunch, pair.die, specialDie});
    window.setAutomaticPartInputPrompt(false);
    window.resize(1280, 850);
    window.show();
    auto &c = window.importController();
    window.openStep(folder + "/source.step");
    wait([&] { return !c.hasWork(); });
    const auto indices = c.session().orderedBends();
    require(indices.size() == 4, "Tool transition fixture lost four bends");
    auto other = pair;
    other.die = specialDie;
    other.dieAssembly = core::ToolAssembly{{0, 0}, 0};
    std::vector<application::BendToolUpdate> initial;
    for (const auto i : indices) {
        const auto a = i == indices[1] ? other : pair;
        initial.push_back({i, core::toolParameters(a), a});
    }
    unsigned changes = 0;
    const auto transaction =
        QObject::connect(&c, &ui::ImportController::stateChanged, &window, [&] { ++changes; });
    require(c.setBendToolAssignments(initial) && changes == 1,
            "Initial tool batch notified intermediate sessions");
    require(c.setBendToolAssignments(initial) && changes == 1,
            "An unchanged tool batch emitted a refresh");
    QObject::disconnect(transaction);
    require(c.setBendWorkpieceOrientation(indices[1], true), "Cannot turn the part");
    auto stage = [&](int n) {
        window.findChild<QToolButton *>(QStringLiteral("processStage%1").arg(n))->click();
        wait([&] { return !c.hasWork(); });
    };
    auto step = [&](int n) {
        window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(n))->click();
        wait([&] { return !c.hasWork(); });
    };
    auto *scope = window.findChild<QComboBox *>("toolAssignmentScope");
    auto *punch = window.findChild<QComboBox *>("punchPreset");
    const auto pickPunch = [&](const QString &name) {
        for (int at = 0; at < punch->count(); ++at)
            if (punch->itemText(at).contains(name)) {
                punch->setCurrentIndex(at);
                return;
            }
        throw std::runtime_error("Punch missing from the library");
    };
    stage(2);
    stage(1);
    pickPunch(QStringLiteral("Short"));
    stage(2);
    require(c.routeDraft() && c.routeDraft()->toolLengths &&
                window.findChild<QLabel *>("bendCheckSummary")->text().contains("не хватает"),
            "The manual blank silently accepted a short tool");
    auto *messages = window.findChild<QWidget *>("bendMessages");
    auto *messageToggle = window.findChild<QToolButton *>("bendMessagesToggle");
    auto *play = window.findChild<QPushButton *>("playBend");
    auto *slider = window.findChild<QSlider *>("bendFrame");
    auto *timer = window.findChild<QTimer *>("bendPlaybackTimer");
    auto *diagnose = window.findChild<QToolButton *>("shortToolDiagnostic");
    require(play && slider && timer && diagnose && !play->isEnabled() && !timer->isActive(),
            "A short tool can start playback from the blank");
    const auto blockedJobs = builder->drafts.load();
    play->click();
    require(!timer->isActive() && builder->drafts == blockedJobs &&
                c.routeDraft()->request.step == 0,
            "Blocked playback advanced the step or launched CAD");
    require(messages && messages->isVisible() && messageToggle,
            "The current bend warnings are missing from the viewport");
    const auto messageDrafts = builder->drafts.load();
    const auto messageEdit = c.session().editRevision();
    messageToggle->click();
    QApplication::processEvents();
    require(window.findChild<QLabel *>("bendMessagesText")->text().contains("не хватает") &&
                builder->drafts == messageDrafts && c.session().editRevision() == messageEdit,
            "Expanding messages lost the length detail or launched a calculation");
    require(!messages->geometry().intersects(
                window.findChild<QWidget *>("operationOverlay")->geometry()),
            "Expanded messages overlap the step strip or playback controls");
    if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty()) {
        auto shot = window.grab().toImage();
        auto *viewer = window.findChild<viewer::ViewerWidget *>();
        if (viewer && viewer->isReady()) {
            QPainter painter(&shot);
            painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
        }
        require(shot.save(output + "-messages.png"), "Message snapshot failed");
    }
    window.resize(900, 650);
    QApplication::processEvents();
    require(messages->geometry().right() < messages->parentWidget()->width() - 100 &&
                !messages->geometry().intersects(
                    window.findChild<QWidget *>("operationOverlay")->geometry()),
            "Messages cover the cube or transport after resize");
    window.resize(1280, 850);
    messageToggle->click();
    step(1);
    require(c.routeDraft()->toolLengths && !play->isEnabled() && !slider->isEnabled() &&
                slider->value() == 0 && !timer->isActive() && !diagnose->isChecked(),
            "A short positive step started motion or inherited diagnostic permission");
    slider->setValue(10);
    require(slider->value() == 0, "Programmatic scrubbing bypassed the short-tool block");
    window.findChild<QToolButton *>("checkReportToggle")->click();
    require(diagnose->isVisible(), "The explicit diagnostic command is inaccessible");
    const auto diagnosticJobs = builder->drafts.load();
    const auto diagnosticEdit = c.session().editRevision();
    diagnose->click();
    require(diagnose->isChecked() && slider->isEnabled() && play->isEnabled() &&
                builder->drafts == diagnosticJobs && c.session().editRevision() == diagnosticEdit &&
                c.routeDraft()->checkedToolPositions == 21 &&
                window.findChild<QLabel *>("bendCheckSummary")->text().contains("Диагностический"),
            "Diagnostic mode skipped checks, changed inputs or remains blocked");
    play->click();
    wait([&] { return slider->value() > 0; });
    diagnose->click();
    require(!timer->isActive() && !play->isEnabled() && !slider->isEnabled() &&
                slider->value() == 0,
            "Leaving diagnostics did not stop/reset short-tool playback");
    diagnose->click();
    slider->setValue(10);
    stage(1);
    stage(2);
    require(!diagnose->isChecked() && !play->isEnabled() && !slider->isEnabled() &&
                slider->value() == 0 && !timer->isActive(),
            "Returning to a ready short-tool result restored diagnostic motion");
    diagnose->click();
    step(2);
    require(!diagnose->isChecked() && !play->isEnabled() && !slider->isEnabled() &&
                slider->value() == 0 && !timer->isActive(),
            "Diagnostic permission leaked into another operation");
    step(0);
    stage(1);
    require(!messages->isVisible(), "Bend messages escaped into the tool workspace");
    pickPunch(QStringLiteral("P60"));
    stage(2);
    require(play->isEnabled(), "Sufficient first-bend length did not restore normal playback");
    window.findChild<QSlider *>("bendPlaybackSpeed")->setValue(8);
    play->click();
    wait([&] { return !c.hasWork() && c.routeDraft() && c.routeDraft()->request.step == 2; });
    require(c.routeDraft()->toolLengths && !timer->isActive() && !play->isEnabled() &&
                !slider->isEnabled() && slider->value() == 0 && !diagnose->isChecked(),
            "Automatic playback continued into a short-tool operation");
    step(2);
    window.findChild<QToolButton *>("reversePunch")->click();
    wait([&] { return !c.hasWork(); });
    stage(1);
    require(scope->currentText() == QStringLiteral("Гиб 2"), "Step context was hidden/lost");
    require(window.findChild<QLabel *>("toolWorkspaceContext")->text() == scope->currentText(),
            "Tool cards do not show the editing scope");
    if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty())
        require(window.grab().save(output + "-local-tools.png"), "Tool context snapshot failed");
    pickPunch(QStringLiteral("Hook"));
    stage(2);
    require(c.routeDraft() && c.routeDraft()->checkedToolPositions == 21 && builder->searches == 0,
            "Changing a local tool skipped collision checks or launched a search");
    for (const auto i : indices) {
        const auto &a = *c.session().bend(i)->catalogTools;
        require(a.punch == (i == indices[1] ? hook : pair.punch) &&
                    a.die == (i == indices[1] ? specialDie : pair.die) &&
                    c.session().bend(i)->workpieceReversed == (i == indices[1]),
                "Local tool choice changed another bend, die or part orientation");
    }
    require(c.session().bend(indices[1])->catalogTools->dieAssembly == other.dieAssembly,
            "Changing a punch refitted an unchanged die assembly");
    step(0);
    stage(1);
    require(scope->currentText() == QStringLiteral("Вся деталь"),
            "Zero did not restore the global edit context");
    const auto before = c.session().editRevision();
    pickPunch(QStringLiteral("Hook"));
    stage(2);
    require(c.session().editRevision() == before + 1 && c.routeDraft() &&
                c.routeDraft()->checkedToolPositions == 1 && builder->searches == 0,
            "Global tool choice was not one transaction/one checked setup");
    for (const auto i : indices) {
        const auto &a = *c.session().bend(i)->catalogTools;
        require(a.punch == hook && a.die == (i == indices[1] ? specialDie : pair.die) &&
                    c.session().bend(i)->workpieceReversed == (i == indices[1]),
                "Global punch choice reset another field");
    }
    const auto ready = c.routeDraft();
    const auto count = builder->drafts.load();
    const auto unchanged = c.session().editRevision();
    stage(1);
    stage(2);
    require(c.routeDraft() == ready && builder->drafts == count &&
                c.session().editRevision() == unchanged,
            "Unchanged tool navigation invalidated the ready scene");
    step(3);
    stage(1);
    const auto selectedId = scope->currentData().toString();
    auto ids = order(c);
    std::swap(ids[1], ids[2]);
    require(c.setBendOrder(ids) && scope->currentData().toString() == selectedId &&
                scope->currentText() == QStringLiteral("Гиб 2"),
            "Reordering changed the edited operation instead of its number");
    pickPunch(QStringLiteral("P60"));
    window.findChild<QPushButton *>("assignBendTool")->click();
    require(c.session().bend(indices[2])->catalogTools->punch == pair.punch &&
                c.session().bend(indices[1])->catalogTools->punch == hook,
            "The context applied a local tool to its old step number");
    const auto project = folder + "/tool-transition.bend";
    c.saveProject(project.toStdString(), files);
    wait([&] { return !c.isSaving(); });
    require(c.saveError().isEmpty(), "Cannot save per-bend tool edits");
    window.openStep(project);
    wait([&] { return !c.hasWork(); });
    require(scope->currentIndex() == 0 && !c.routeDraft() &&
                c.session().bend(indices[2])->catalogTools->punch == pair.punch &&
                c.session().bend(indices[1])->catalogTools->punch == hook,
            "Reopening lost assignments or restored stale edit context/checks");
    auto legacy = *c.session().bend(indices[1])->catalogTools;
    legacy.useDrawingGeometry = false;
    require(c.setBendToolAssignment(indices[1], legacy), "Cannot prepare a legacy assignment");
    stage(1);
    bool offered = false;
    QTimer::singleShot(50, &window, [&] {
        if (auto *popup = window.findChild<QDialog *>("toolPickerPopup")) {
            if (auto *option = popup->findChild<QToolButton *>("toolOption_hook_0")) {
                offered = option->isEnabled();
                option->click();
            } else
                popup->reject();
        }
    });
    window.findChild<QToolButton *>("punchProfile")->click();
    require(offered, "The common punch is missing from the graphic picker");
    stage(2);
    require(c.routeDraft() && c.routeDraft()->checkedToolPositions == 1,
            "Explicit profile selection did not prepare the new DXF setup");
    for (const auto i : indices)
        require(c.session().bend(i)->catalogTools->punch == hook &&
                    c.session().bend(i)->catalogTools->useDrawingGeometry,
                "Explicit global choice of the displayed punch retained a local exception");
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("manual-route-test");
    QTemporaryDir folder;
    require(folder.isValid(), "Temporary directory failed");
    ui::setPreferencesFile(folder.path() + "/settings.ini");
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
        if (QApplication::platformName() != QStringLiteral("offscreen")) {
            // Let the native window manager finish exposing the canvas before the
            // camera-preservation snapshots, as in the graphical picker scenario.
            window.raise();
            window.activateWindow();
            wait([&] {
                return window.windowHandle() && window.windowHandle()->isExposed() &&
                       window.isActiveWindow();
            });
        }
        window.openStep(folder.path() + "/source.step");
        auto &controller = window.importController();
        wait([&] { return !controller.hasWork(); });
        require(bool(controller.session().document()), "Import failed");
        const auto bytes = std::string(controller.session().document()->sourceStep());
        require(order(controller).size() == 4, "Expected four bends");
        for (auto i : controller.session().orderedBends())
            require(controller.setBendToolAssignment(i, pair), "Assignment failed");
        auto *viewer = window.findChild<viewer::ViewerWidget *>();
        require(viewer, "Viewer missing");
        const auto snapshotStage = [&](const char *suffix) {
            const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
            if (output.isEmpty())
                return;
            QApplication::processEvents();
            auto shot = window.grab().toImage();
            if (viewer->isVisible() && viewer->isReady()) {
                QPainter p(&shot);
                p.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            }
            require(shot.save(output + QString::fromLatin1(suffix)), "Cannot save workspace");
        };
        if (viewer->isReady())
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Top);
        const auto partScene = viewer->captureImage(false);
        const auto stageRevision = controller.session().editRevision();
        window.findChild<QToolButton *>("processStage1")->click();
        auto *toolWorkspace = window.findChild<QWidget *>("toolWorkspace");
        require(toolWorkspace && toolWorkspace->isVisible() && !viewer->isVisible() &&
                    !controller.hasWork() && builder->drafts == 0 && builder->searches == 0,
                "Tool stage retained the part scene or started CAD");
        require(window.findChild<QLabel *>("toolLengthWarning")->isVisible(),
                "Tool selection hides its insufficient nominal length");
        snapshotStage("-tools.png");
        window.findChild<QToolButton *>("processStage0")->click();
        require(viewer->isVisible() && !toolWorkspace->isVisible() &&
                    (partScene.isNull() || viewer->captureImage(false) == partScene) &&
                    controller.session().editRevision() == stageRevision,
                "Returning from tools changed the source camera or project");
        snapshotStage("-part.png");
        window.findChild<QToolButton *>("processStage2")->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->request.step == 0 &&
                    controller.routeDraft()->checkedToolPositions == 1 && builder->drafts == 1 &&
                    builder->searches == 0,
                "Bending stage did not automatically check only the blank setup");
        snapshotStage("-zero.png");
        if (viewer->isReady())
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Right);
        const auto machineScene = viewer->captureImage(false);
        window.findChild<QPushButton *>("editSelectedBend")->click();
        require(partScene.isNull() || viewer->captureImage(false) == partScene,
                "Part stage retained the bending scene or lost its camera");
        window.findChild<QToolButton *>("processStage1")->click();
        require(toolWorkspace->isVisible(), "Tool workspace disappeared after bending");
        window.findChild<QToolButton *>("processStage2")->click();
        require(!controller.hasWork() && builder->drafts == 1 && builder->searches == 0 &&
                    (machineScene.isNull() || viewer->captureImage(false) == machineScene),
                "Returning to bending rebuilt CAD or lost its camera");
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
        mode->setCurrentIndex(1);
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
        auto third = pair;
        third.clearanceMm += 3;
        require(controller.setBendToolAssignment(indices[1], other) &&
                    controller.setBendToolAssignment(indices[2], third),
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
                    order(controller) == originalOrder && mode->currentIndex() == 1,
                "Global reversal searched an order, lost zero or skipped collision checks");
        for (const auto i : indices) {
            const auto *entry = controller.session().bend(i);
            auto expected = *core::toolParameters(i == indices[2]   ? third
                                                  : i == indices[1] ? other
                                                                    : pair);
            expected.punchReversed = true;
            require(entry->tools == expected, "Global reversal replaced a per-bend setup");
            if (entry->catalogTools) {
                auto expectedAssignment = i == indices[2] ? third : i == indices[1] ? other : pair;
                expectedAssignment.punchReversed = true;
                require(entry->catalogTools == expectedAssignment,
                        "Global reversal lost a catalog profile or assembly");
            } else
                require(false, "Catalog assignment was converted to manual parameters");
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
        mode->setCurrentIndex(1); // The check mode does not change the scope of tool editing.
        wait([&] { return !controller.hasWork(); });
        require(reverse->isChecked(), "Shared zero setup lost its current orientation");
        reverse->click();
        wait([&] { return !controller.hasWork(); });
        require(mode->currentIndex() == 1 && controller.routeDraft() && builder->searches == 0,
                "Global reversal forced individual mode or launched search");
        for (const auto i : controller.session().orderedBends())
            require(controller.session().bend(i)->catalogTools == (i == indices[2]   ? third
                                                                   : i == indices[1] ? other
                                                                                     : pair),
                    "Zero orientation replaced an independent tool assignment");
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
        require(controller.routeDraft() && mode->currentIndex() == 1 &&
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
        auto *partTurn = window.findChild<QToolButton *>("reverseWorkpiece");
        require(partTurn && partTurn->isEnabled(), "Manual workpiece turn missing");
        const auto unturned = controller.routeDraft()->visual;
        const auto activePart = controller.routeDraft()->request.operations[0].bendIndex;
        partTurn->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->visual->workpieceReversed() &&
                    controller.session().bend(activePart)->workpieceReversed &&
                    builder->searches == searchesBefore && slider->value() == 10 &&
                    !controller.routeDraft()->request.operations[0].tools->punchReversed,
                "Manual part turn changed punch, frame or started order search");
        reverse->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->visual->workpieceReversed(),
                "Manual punch turn reset the workpiece installation");
        reverse->click();
        wait([&] { return !controller.hasWork(); });
        partTurn->click();
        wait([&] { return !controller.hasWork(); });
        require(controller.routeDraft() && controller.routeDraft()->visual == unturned &&
                    builder->searches == searchesBefore,
                "Restoring a part orientation rebuilt checked geometry");
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
            require(controller.session().bend(i)->catalogTools == (i == indices[2]   ? third
                                                                   : i == indices[1] ? other
                                                                                     : pair),
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
        window.findChild<QToolButton *>("processStage0")->click();
        window.findChild<QToolButton *>("processStage2")->click();
        require(builder->drafts == started,
                "Returning to the in-flight setup launched another CAD job");
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
        mode->setCurrentIndex(1);
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
        blocked.punch.lengthMm =
            controller.session().document()->metadata().bendAnalysis.bends[active].lengthMm / 2;
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
        const auto &failedFrame =
            cad::OcctPreviewAccess::frame(*diagnostic->visual, *collisionFrame);
        require(failedFrame.collisionHighlights.size() >= 2,
                "Manual collision did not mark both participating surfaces");
        require(*collisionFrame > 0, "The playback fixture needs a safe approach");
        auto *playback = window.findChild<QTimer *>("bendPlaybackTimer");
        auto *play = window.findChild<QPushButton *>("playBend");
        auto *speed = window.findChild<QSlider *>("bendPlaybackSpeed");
        auto *diagnose = window.findChild<QToolButton *>("shortToolDiagnostic");
        require(diagnostic->toolLengths && !play->isEnabled() && !slider->isEnabled(),
                "A colliding short punch bypassed the ordinary playback block");
        window.findChild<QToolButton *>("checkReportToggle")->setChecked(true);
        diagnose->click();
        require(diagnose->isChecked() && slider->isEnabled(),
                "Explicit short-tool diagnostics cannot inspect the collision");
        const auto draftsBeforePlay = builder->drafts.load(),
                   searchesBeforePlay = builder->searches.load();
        speed->setValue(speed->maximum());
        slider->setValue(0);
        require(play->isEnabled(), "Cannot replay a safe approach to the collision");
        play->click();
        wait([&] { return !playback->isActive(); });
        require(slider->value() == static_cast<int>(*collisionFrame) && !play->isEnabled() &&
                    builder->drafts == draftsBeforePlay && builder->searches == searchesBeforePlay,
                "3x playback passed a collision, failed to stop or recalculated CAD");
        slider->setValue(static_cast<int>(*collisionFrame) - 1);
        require(play->isEnabled(), "Scrubbing back to a safe frame did not restore playback");
        slider->setValue(static_cast<int>(*collisionFrame));
        require(window.findChild<QLabel *>("bendCheckSummary")
                    ->text()
                    .contains(QStringLiteral("Столкновение"), Qt::CaseInsensitive),
                "A colliding frame is presented as clear");
        if (auto *viewer = window.findChild<viewer::ViewerWidget *>();
            viewer && viewer->isReady()) {
            window.findChild<QAction *>("viewRight")->trigger();
            window.findChild<QAction *>("fitModel")->trigger();
            const auto image = viewer->captureImage(false).convertToFormat(QImage::Format_RGB32);
            std::size_t red = 0, foreground = 0;
            const auto background = image.pixelColor(0, 0);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto c = image.pixelColor(x, y);
                    red += c.red() > 160 && c.red() > 2.3 * c.green() && c.red() > 1.8 * c.blue();
                    foreground += std::abs(c.red() - background.red()) +
                                      std::abs(c.green() - background.green()) +
                                      std::abs(c.blue() - background.blue()) >
                                  35;
                }
            if (!(red > 5 && red * 4 < foreground)) {
                std::cerr << "Local collision pixels: red=" << red << " foreground=" << foreground
                          << '\n';
                for (const auto &patch : failedFrame.collisionHighlights)
                    std::cerr << patch.bodyId << " vertices=" << patch.vertices.size()
                              << " centres=" << patch.centres.size() << '\n';
                if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
                    !output.isEmpty())
                    image.save(output + "-patch.png");
            }
            require(red > 5 && red * 4 < foreground,
                    "Native collision rendering lost the local red patch or painted whole bodies");
            const auto beforeRefresh = viewer->captureImage(false);
            window.findChild<QAction *>("refreshScene")->trigger();
            require(viewer->captureImage(false) == beforeRefresh &&
                        controller.routeDraft() == diagnostic && !controller.hasWork(),
                    "Refreshing the viewport lost the patch or recalculated collision");
        }
        if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty()) {
            auto shot = window.grab().toImage();
            auto *viewer = window.findChild<viewer::ViewerWidget *>();
            if (viewer && viewer->isReady()) {
                QPainter p(&shot);
                p.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            }
            require(shot.save(output + "-collision.png"), "Collision capture failed");
            if (viewer && viewer->isReady()) {
                viewer->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
                require(viewer->captureImage(true).save(output + "-collision-iso.png"),
                        "Isometric collision capture failed");
                std::cout << "Collision strip patches="
                          << std::count_if(failedFrame.collisionHighlights.begin(),
                                           failedFrame.collisionHighlights.end(),
                                           [](const auto &p) { return p.band.has_value(); })
                          << '\n';
            }
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
        window.close();
        verifyToolStageTransitions(folder.path());
        std::cout << "Manual prefixes, collision scope, stable reorder, undo/redo, explicit check "
                     "and stale selection passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
