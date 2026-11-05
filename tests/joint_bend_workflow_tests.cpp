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
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
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
            const bool target = !reverse->isChecked();
            const auto members = route->steps[group].additionalBends;
            const auto primary = route->steps[group].bendIndex;
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
