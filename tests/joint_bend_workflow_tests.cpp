#include "BendFixtures.h"
#include "EndSupportFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <set>
using namespace bending;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void wait(ui::ImportController &controller) {
    QElapsedTimer timer;
    timer.start();
    while (controller.hasWork() && timer.elapsed() < 120000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(!controller.hasWork(), "Joint workflow timeout");
    QApplication::processEvents();
    if (const auto error = controller.bendRouteError())
        throw std::runtime_error(error->detail);
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        const bool tools = !app.arguments().contains("--geometry");
        const bool assemblies = app.arguments().contains("--assemblies");
        const bool axial = app.arguments().contains("--axial");
        QTemporaryDir temporary;
        require(temporary.isValid(), "No temporary folder");
        ui::setPreferencesFile(temporary.path() + "/settings.ini");
        const auto path = temporary.path() + "/joint.step";
        // A third bend supplies another step so a whole grouped operation can move.
        const auto source =
            tools ? end_support_fixture::joint(.46, 8, 1.25, 2.5)
                  : BRepAlgoAPI_Cut(fixtures::channel(120),
                                    BRepPrimAPI_MakeBox(gp_Pnt(-20, -5, 45), 25, 60, 20).Shape())
                        .Shape();
        STEPControl_Writer writer;
        require(writer.Transfer(source, STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
                "Joint workflow fixture write failed");
        const core::PressTools parameters{16,
                                          std::numbers::pi / 3,
                                          std::numbers::pi / 6,
                                          .8,
                                          assemblies ? 80. : axial ? 200. : 120.,
                                          10};
        auto assignment = tool_fixture::explicitPair(parameters);
        if (assemblies) {
            assignment.punch.segments = assignment.die.segments =
                std::vector<core::ToolSegment>{{80, 1}, {40, 1}};
            assignment.punchAssembly = assignment.dieAssembly = core::ToolAssembly{{0}, 0};
        }
        auto files = std::make_shared<project::ProjectFiles>();
        auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
        ui::MainWindow window({"test", "test"}, files, files, builder,
                              {assignment.punch, assignment.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        window.openStep(path);
        auto &controller = window.importController();
        wait(controller);
        const auto document = controller.session().document();
        const std::size_t zones = tools ? 2 : 3, steps = tools ? 1 : 2;
        require(document && document->metadata().bendAnalysis.bends.size() == zones,
                "Joint workflow lost original zones");
        if (!tools) {
            const auto &analysis = document->metadata().bendAnalysis;
            const auto strokes =
                application::routeBendStrokes(analysis, controller.session().orderedBends());
            const auto grouped =
                std::find_if(strokes.begin(), strokes.end(),
                             [](const auto &stroke) { return stroke.size() == 2; });
            require(strokes.size() == 2 && grouped != strokes.end(),
                    "The relief did not group the two zones during import");
            const auto &a = analysis.bends[(*grouped)[0]];
            const auto &b = analysis.bends[(*grouped)[1]];
            require(a.interruptedStroke && a.interruptedStroke == b.interruptedStroke &&
                        std::abs(application::routeStrokeLengthMm(analysis, *grouped) - 120) < .001,
                    "The interrupted stroke lost its full axial span");
            auto *list = window.findChild<QListView *>("bendList");
            require(list && list->model()->rowCount() == 2,
                    "The part still lists separate zones of the interrupted stroke");
            application::RouteDraftRequest draft;
            for (const auto index : controller.session().orderedBends()) {
                const auto *entry = controller.session().bend(index);
                draft.operations.push_back(
                    {entry->id, index, *controller.session().bendTarget(index), {}, {}});
            }
            draft.step = static_cast<std::size_t>(grouped - strokes.begin()) + 1;
            const auto preview = builder->buildRouteDraft(document, draft, {});
            if (const auto *error = std::get_if<application::BendRouteError>(&preview))
                throw std::runtime_error(error->detail);
            const auto &visual = std::get<application::RouteDraftPtr>(preview)->visual;
            require(visual->activeBendCount() == 2 &&
                        std::abs(visual->model().widthMm - 120) < .001,
                    "The manual preview did not form the two zones in one stroke");
            auto common = assignment;
            common.punch.segments = common.die.segments = std::vector<core::ToolSegment>{{120, 1}};
            common.punchAssembly = common.dieAssembly = core::ToolAssembly{{0}, 0};
            application::BendRouteRequest early;
            early.automatic = early.withTools = early.manualExtraction = true;
            early.operations = draft.operations;
            early.assemblySearch = application::RouteAssemblySearch{{}, 0, true};
            for (auto &op : early.operations) {
                op.assignment = common;
                op.tools = core::toolParameters(common);
                early.assemblySearch->bendIndices.push_back(op.bendIndex);
            }
            const auto earlyResult = builder->buildRoute(document, early, {});
            if (const auto *error = std::get_if<application::BendRouteError>(&earlyResult))
                throw std::runtime_error("Early joint route: " + error->detail);
            const auto &earlyRoute = *std::get<application::BendRoutePtr>(earlyResult);
            require(earlyRoute.statistics.usedCommonAssembly && earlyRoute.steps.size() == 2 &&
                        earlyRoute.steps.front().additionalBends.size() == 1 &&
                        earlyRoute.statistics.plannedDieWindows == 0,
                    "The common assembly pass did not form the interrupted flange first");
            auto inaccessible = common;
            inaccessible.backgauge = core::BackgaugeSetup{};
            inaccessible.backgauge->xMinMm = 700;
            auto mirror = inaccessible;
            mirror.punchReversed = !mirror.punchReversed;
            common.punch.id += "-alternative";
            early.toolVariants = {{inaccessible, {}}, {mirror, {}}, {common, {}}};
            std::set<std::size_t> testedVariants;
            early.onProgress = [&](const auto &progress) {
                if (progress.phase == application::BendRoutePhase::Checking)
                    testedVariants.insert(progress.variant);
            };
            cad::SingleBendPreviewBuilder catalogBuilder;
            const auto catalogResult = catalogBuilder.buildRoute(document, early, {});
            if (const auto *error = std::get_if<application::BendRouteError>(&catalogResult))
                throw std::runtime_error("Early catalog route: " + error->detail);
            const auto &catalogRoute = *std::get<application::BendRoutePtr>(catalogResult);
            require(catalogRoute.selectedToolVariant == 2 && catalogRoute.steps.size() == 2 &&
                        catalogRoute.steps.front().additionalBends.size() == 1 &&
                        catalogRoute.statistics.usedCommonAssembly &&
                        catalogRoute.statistics.commonAssemblyPasses == 2 &&
                        catalogRoute.statistics.commonAssemblyChecks ==
                            catalogRoute.evaluatedOperations &&
                        catalogRoute.statistics.testedToolVariants == 2 &&
                        testedVariants == std::set<std::size_t>{0, 2},
                    "Catalog search did not reserve an early pass for a distinct common pair");
            early.maximumChecks = 1;
            const auto limited = catalogBuilder.buildRoute(document, early, {});
            const auto *limitedError = std::get_if<application::BendRouteError>(&limited);
            require(limitedError &&
                        limitedError->code == application::BendRouteErrorCode::SearchLimit &&
                        limitedError->evaluatedOperations == 1 &&
                        limitedError->statistics.commonAssemblyPasses == 0,
                    "Early catalog search bypassed the check budget");
            list->setCurrentIndex(
                list->model()->index(static_cast<int>(grouped - strokes.begin()), 0));
            auto *openSegments = window.findChild<QPushButton *>("strokeSegments");
            require(openSegments, "Stroke editor controls are missing");
            const auto runEditor = [&](bool join, bool apply, bool fromRow = false,
                                       bool all = false) {
                bool previewed = false;
                const auto interact = [&] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog)
                        return;
                    auto *together =
                        dialog->findChild<QTreeWidget *>(QStringLiteral("combinedStrokes"));
                    auto *separate =
                        dialog->findChild<QTreeWidget *>(QStringLiteral("separateStrokes"));
                    auto *commit =
                        dialog->findChild<QPushButton *>(QStringLiteral("applyStrokeEdits"));
                    if (together && separate && commit) {
                        const auto before = controller.session().editRevision();
                        bool acted = false;
                        if (all) {
                            auto *button = dialog->findChild<QPushButton *>(
                                join ? "mergeAllStrokes" : "splitAllStrokes");
                            if (button && button->isEnabled()) {
                                button->click();
                                acted = true;
                            }
                        } else if (!join && together->topLevelItemCount() == 1 &&
                                   together->topLevelItem(0)->childCount() == 2) {
                            auto *row = together->topLevelItem(0)->child(0);
                            together->setCurrentItem(row);
                            const auto pos = together->visualItemRect(row).center();
                            QContextMenuEvent event(QContextMenuEvent::Keyboard, pos,
                                                    together->viewport()->mapToGlobal(pos));
                            QApplication::sendEvent(together->viewport(), &event);
                            auto *menu = dialog->findChild<QMenu *>();
                            auto *action = dialog->findChild<QAction *>("splitSelectedSegments");
                            if (menu && menu->isVisible() && action && action->isEnabled()) {
                                menu->hide();
                                action->trigger();
                                acted = true;
                            }
                        }
                        if (acted) {
                            previewed = together->topLevelItemCount() == (join ? 1 : 0) &&
                                        separate->topLevelItemCount() == (join ? 0 : 2) &&
                                        commit->isEnabled() &&
                                        controller.session().editRevision() == before;
                            if (apply && previewed) {
                                commit->click();
                                return;
                            }
                        }
                    }
                    dialog->reject();
                };
                if (fromRow) {
                    const auto row = list->model()->index(grouped - strokes.begin(), 0);
                    list->setCurrentIndex(list->model()->index(1 - row.row(), 0));
                    const auto rect = list->visualRect(row);
                    const QPoint point(rect.right() - 16, rect.center().y());
                    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                        const auto buttons =
                            type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton;
                        QMouseEvent event(type, point, list->viewport()->mapToGlobal(point),
                                          Qt::LeftButton, buttons, Qt::NoModifier);
                        QApplication::sendEvent(list->viewport(), &event);
                    }
                    QTimer::singleShot(0, &window, interact);
                    QCoreApplication::processEvents();
                } else {
                    QTimer::singleShot(0, &window, interact);
                    openSegments->click();
                }
                return previewed;
            };
            const auto before = controller.session().editRevision();
            require(runEditor(false, false, true) &&
                        controller.session().editRevision() == before &&
                        list->model()->rowCount() == 2,
                    "Cancel published the staged split");
            const bool choseSegment = runEditor(false, true, false, true);
            require(
                choseSegment && list->model()->rowCount() == 3 &&
                    controller.session().bendStrokes(controller.session().orderedBends()).size() ==
                        3,
                "The stroke editor did not create two independent steps");
            for (auto &op : draft.operations)
                op.strokePartition = controller.session().bend(op.bendIndex)->strokePartition;
            require(draft.operations[(*grouped)[0]].strokePartition != 0 &&
                        draft.operations[(*grouped)[1]].strokePartition != 0,
                    "The original and detached strokes are not both locked");
            for (const auto index : *grouped) {
                const auto position =
                    std::find_if(draft.operations.begin(), draft.operations.end(),
                                 [&](const auto &op) { return op.bendIndex == index; });
                draft.step = static_cast<std::size_t>(position - draft.operations.begin()) + 1;
                const auto result = builder->buildRouteDraft(document, draft, {});
                if (const auto *error = std::get_if<application::BendRouteError>(&result))
                    throw std::runtime_error(error->detail);
                const auto &part = std::get<application::RouteDraftPtr>(result)->visual;
                require(part->activeBendCount() == 1 &&
                            std::abs(part->model().widthMm - analysis.bends[index].lengthMm) < .001,
                        "Split preview reused the grouped motion or width");
            }
            const auto savedPath =
                std::filesystem::path(temporary.path().toStdString()) / "split.bend";
            require(!files->save(savedPath, *document, {}, controller.session().bends(), {}),
                    "Could not save the partition");
            application::DocumentSession reopened;
            const auto opened = reopened.beginImport(savedPath);
            require(reopened.complete(opened, files->load(savedPath, {}, {})) &&
                        reopened.bendStrokes(reopened.orderedBends()).size() == 3,
                    "Reopened project lost the independent steps");
            application::BendRouteRequest splitRoute;
            splitRoute.operations = draft.operations;
            splitRoute.automatic = true;
            const auto splitResult = builder->buildRoute(document, splitRoute, {});
            require(std::holds_alternative<application::BendRoutePtr>(splitResult) &&
                        std::get<application::BendRoutePtr>(splitResult)->steps.size() == 3,
                    "Automatic search rejoined the manually separated strokes");
            require(runEditor(true, true, false, true), "Could not merge all strokes");
            require(
                list->model()->rowCount() == 2 &&
                    controller.session().bendStrokes(controller.session().orderedBends()).size() ==
                        2,
                "Restore did not recover the automatic grouping");
        }
        const auto bytes = std::string(document->sourceStep());
        std::vector<application::BendToolUpdate> updates;
        application::BendRouteRequest request;
        request.withTools = tools;
        request.allowPartialEndSupport = tools;
        request.automatic = true;
        request.allowAxialPlacement = axial;
        request.maximumChecks = 64;
        for (const auto index : controller.session().orderedBends()) {
            const auto *entry = controller.session().bend(index);
            updates.push_back({index, core::toolParameters(assignment), assignment});
            request.operations.push_back({entry->id, index, *controller.session().bendTarget(index),
                                          core::toolParameters(assignment), assignment});
        }
        if (assemblies)
            request.assemblySearch = application::RouteAssemblySearch{{0, 1}, 0};
        require(controller.setBendToolAssignments(updates) &&
                    controller.prepareBendRoute(request, builder),
                "Joint request rejected");
        wait(controller);
        auto route = controller.bendRoute();
        require(route && route->steps.size() == steps &&
                    controller.session().orderedBends().size() == zones,
                "Controller did not accept a complete grouped route");
        if (axial) {
            const double offset = route->steps.front().preview->workpieceOffsetMm();
            require(std::abs(offset) == 40, "Joint end placement lost its full envelope");
            for (std::size_t i = 0; i < zones; ++i)
                require(controller.session().bend(i)->workpieceOffsetMm == offset,
                        "Controller did not accept the complete joint placement atomically");
            const auto destination = temporary.path().toStdString() + "/placed.bend";
            require(!files->save(destination, *document, {}, controller.session().bends(), {}),
                    "Cannot save placed route");
            const auto reopened = files->load(destination, {}, {});
            const auto *saved = std::get_if<application::OpenedProject>(&reopened);
            require(saved &&
                        std::all_of(saved->bends.begin(), saved->bends.end(),
                                    [&](const auto &b) { return b.workpieceOffsetMm == offset; }),
                    "Joint placement lost on project reopening");
        }
        if (assemblies) {
            require(route->steps.front().assemblyAssignment &&
                        route->steps.front().preview->model().widthMm == 120 &&
                        route->statistics.plannedAssemblies == 2,
                    "Joint assembly search lost its gap-inclusive envelope or planning metrics");
            for (std::size_t i = 0; i < zones; ++i)
                require(
                    controller.session().bend(i)->tools->lengthMm == 120 &&
                        core::punchLengthMm(*controller.session().bend(i)->tools) == 120 &&
                        controller.session().bend(i)->catalogTools->punchAssembly->pieces.size() ==
                            2,
                    "Derived joint assemblies were not accepted atomically");
            require(core::toolParameters(assignment)->lengthMm == 80,
                    "Assembly search mutated its original stock/input snapshot");
            const auto project = temporary.path().toStdString() + "/derived-assemblies.bend";
            require(!files->save(project, *document, {}, controller.session().bends(), {}),
                    "Derived assemblies could not be saved");
            const auto reopened = files->load(project, {}, {});
            const auto *saved = std::get_if<application::OpenedProject>(&reopened);
            require(saved && saved->bends.size() == zones &&
                        std::all_of(saved->bends.begin(), saved->bends.end(),
                                    [](const auto &b) {
                                        return b.tools && b.tools->lengthMm == 120 &&
                                               b.catalogTools &&
                                               b.catalogTools->punchAssembly->pieces.size() == 2;
                                    }),
                    "Derived assemblies were lost on project reopening");
        }
        if (tools)
            require(route->partialEndSupport &&
                        route->steps.front().preview->endSupportIntervals().size() == 2 &&
                        !route->steps.front().preview->endSupportLimitMm(),
                    "Controller dropped a joint member's partial-support report");
        window.findChild<QToolButton *>("processStage2")->click();
        wait(controller);
        window.findChild<QComboBox *>("bendRouteMode")->setCurrentIndex(tools ? 1 : 0);
        // The accepted result controls presentation; changing a differing route mode
        // intentionally invalidates it, so set the initial mode before the next request.
        if (!controller.bendRoute()) {
            require(controller.prepareBendRoute(request, builder),
                    "Repeated joint request rejected");
            wait(controller);
            route = controller.bendRoute();
        }
        require(controller.bendRoute() == route &&
                    window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(steps)) &&
                    !window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(steps + 1)),
                "Step strip shows source zones instead of press operations");
        std::size_t group = 0;
        for (; group < route->steps.size(); ++group)
            if (!route->steps[group].additionalBends.empty())
                break;
        require(group < route->steps.size(), "No grouped step");
        auto *button =
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(group + 1));
        require(button && button->toolTip().contains(" + B"), "Grouped tooltip lost a zone");
        button->click();
        wait(controller);
        require(window.findChild<QLabel *>("bendRouteStatus")->text().contains(" + B"),
                "Grouped step report is ambiguous");
        if (tools)
            require(window.findChild<QLabel *>("bendRouteStatus")
                            ->text()
                            .count(QStringLiteral("Материал под опорой"), Qt::CaseInsensitive) == 2,
                    "Joint support warning collapsed two real intervals");
        if (tools)
            require(window.findChild<QLabel *>("bendRouteStatus")
                        ->text()
                        .contains(QStringLiteral("Торцевые подрезы: возможна деформация")),
                    "Conditional route did not explain its end-relief scope");
        if (const auto output = qEnvironmentVariable("BENDING_JOINT_SNAPSHOT"); !output.isEmpty()) {
            auto *view = window.findChild<viewer::ViewerWidget *>();
            require(view && view->isVisible() && view->isReady(),
                    "Native joint viewport is not ready");
            view->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
            QApplication::processEvents();
            auto snapshot = window.grab().toImage();
            {
                QPainter painter(&snapshot);
                painter.drawImage(view->mapTo(&window, QPoint()), view->captureImage(true));
            }
            require(snapshot.save(output), "Joint snapshot failed");
        }
        if (tools) {
            auto *reverse = window.findChild<QToolButton *>("reverseWorkpiece");
            require(reverse && reverse->isEnabled(), "No joint workpiece orientation control");
            const auto members = route->steps[group].additionalBends;
            const auto primary = route->steps[group].bendIndex;
            const bool target = !controller.session().bend(primary)->workpieceReversed;
            reverse->click();
            wait(controller);
            route = controller.bendRoute();
            require(route && route->steps.size() == 1 &&
                        controller.session().bend(primary)->workpieceReversed == target,
                    "Joint orientation did not preserve the checked operation");
            for (const auto &member : members)
                require(controller.session().bend(member.bendIndex)->workpieceReversed == target,
                        "Only one zone of the joint operation turned");
        }
        if (steps > 1) {
            auto *move =
                window.findChild<QPushButton *>(group ? "routeMoveEarlier" : "routeMoveLater");
            require(move && move->isEnabled(), "Cannot reorder the whole grouped operation");
            move->click();
            wait(controller);
            route = controller.bendRoute();
            require(route && route->steps.size() == steps,
                    "Grouped reorder did not run the shared checker");
            controller.discardBendRoute();
            wait(controller); // Undo must remember the grouped edit even without its old report.
            auto *undo = window.findChild<QPushButton *>("routeUndo");
            require(undo && undo->isEnabled(), "Grouped reorder lost undo");
            undo->click();
            wait(controller);
            auto *redo = window.findChild<QPushButton *>("routeRedo");
            require(redo && redo->isEnabled(), "Grouped undo lost redo");
            redo->click();
            wait(controller);
            route = controller.bendRoute();
            require(route && route->steps.size() == steps, "Grouped redo lost the checked route");
        }
        std::set<std::size_t> scoped;
        for (std::size_t i = 0; i < route->steps.size(); ++i)
            if (!route->steps[i].additionalBends.empty()) {
                scoped.insert(route->steps[i].bendIndex);
                for (const auto &member : route->steps[i].additionalBends)
                    scoped.insert(member.bendIndex);
                window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(i + 1))->click();
                break;
            }
        auto *edit = window.findChild<QPushButton *>("routeEditTool");
        require(edit && edit->isEnabled(), "Joint tool edit is disabled");
        edit->click();
        wait(controller);
        auto *scope = window.findChild<QComboBox *>("toolAssignmentScope");
        if (!scope || !scope->currentText().contains(QStringLiteral("Совместный")))
            throw std::runtime_error(
                "Tool editor lost the joint operation scope: " +
                (scope ? scope->currentText().toStdString() : std::string("missing")));
        const auto beforeEdit = controller.session().editRevision();
        window.findChild<QDoubleSpinBox *>("toolClearance")->setValue(12);
        auto *assign = window.findChild<QPushButton *>("assignBendTool");
        require(assign && assign->isEnabled(), "Cannot commit joint tooling");
        assign->click();
        wait(controller);
        require(!controller.bendRoute() && controller.session().editRevision() == beforeEdit + 1,
                "Joint tooling was not applied atomically or retained an obsolete route");
        for (const auto index : controller.session().orderedBends())
            require(controller.session().bend(index)->tools->clearanceMm ==
                        (scoped.contains(index) ? 12 : 10),
                    "Joint tool edit changed a different step or only one member");
        std::set<std::string> ids;
        for (const auto &step : route->steps) {
            ids.insert(step.id);
            for (const auto &member : step.additionalBends)
                ids.insert(member.id);
        }
        require(ids.size() == zones && document->sourceStep() == bytes,
                "Grouped reorder lost an ID or changed the STEP");
        controller.shutdown();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
