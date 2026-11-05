#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "ToolSelectionWidget.h"
#include "ToolWorkspace.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <STEPControl_Writer.hxx>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
application::DocumentPtr document(const TopoDS_Shape &shape, const QString &path) {
    STEPControl_Writer writer;
    check(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
              writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
          "Fixture write");
#ifdef _WIN32
    const std::filesystem::path file(path.toStdWString());
#else
    const std::filesystem::path file(path.toUtf8().constData());
#endif
    const auto doc = application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
    check(bool(doc), "Fixture import");
    return doc;
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        ui::setPreferencesFile(folder.filePath("settings.ini"));
        const core::PressTools parameters{16, std::numbers::pi / 3, std::numbers::pi / 6, .8, 120,
                                          10};
        auto pair = tool_fixture::explicitPair(parameters);
        pair.punch.segments = std::vector<core::ToolSegment>{{120, 1}};
        pair.punchAssembly = core::ToolAssembly{{0}, 0};
        pair.die.segments = std::vector<core::ToolSegment>{{30, 1}, {25, 1}, {55, 1}};
        pair.dieAssembly = core::ToolAssembly{{0, 1, 2}, 0};
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        ui::ToolSelectionWidget selection({pair.punch, pair.die}, &window);
        selection.hide();
        selection.setChoice(core::toolParameters(pair), pair);
        auto *workspace = new ui::ToolWorkspace(&window);
        workspace->bindSelection(&selection);
        layout->addWidget(workspace);
        int changes = 0;
        selection.onChanged = [&](auto) {
            ++changes;
            workspace->refresh();
        };
        workspace->refresh();
        window.resize(900, 640);
        window.show();
        QString error;
        const auto edit = [&](const std::function<void(QDialog *, QTableWidget *)> &action) {
            QTimer::singleShot(0, [&] {
                auto *dialog = window.findChild<QDialog *>("toolAssemblyDialog");
                try {
                    check(dialog, "No manual assembly editor");
                    action(dialog, dialog->findChild<QTableWidget *>("assemblyRows"));
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                    if (dialog)
                        dialog->reject();
                }
            });
            workspace->findChild<QPushButton *>("editDieAssembly")->click();
            check(error.isEmpty(), qPrintable(error));
        };
        edit([&](QDialog *dialog, QTableWidget *rows) {
            check(rows && rows->rowCount() == 3, "Captured pieces lost");
            auto *last = static_cast<QDoubleSpinBox *>(rows->cellWidget(2, 1));
            check(!last->isEnabled() && last->value() == 0, "Trailing gap editable");
            static_cast<QDoubleSpinBox *>(rows->cellWidget(1, 1))->setValue(10);
            check(dialog->findChild<QLabel *>("assemblyStatus")->text().contains("120"),
                  "Local gap did not update span");
            if (const auto path = qEnvironmentVariable("BENDING_ASSEMBLY_SNAPSHOT");
                !path.isEmpty())
                check(dialog->grab().save(path + "-editor.png"), "Editor snapshot");
            dialog->findChild<QPushButton *>("assemblyApply")->click();
        });
        const auto changed = selection.assignment();
        check(changes == 1 && changed && changed->punch == pair.punch &&
                  changed->punchAssembly == pair.punchAssembly && changed->die == pair.die &&
                  changed->groove == pair.groove && changed->punchReversed == pair.punchReversed &&
                  changed->dieAssembly->jointGapsMm == std::vector<double>{0, 10},
              "Manual edit changed profile, other assembly, orientation or local window");
        edit([&](QDialog *dialog, QTableWidget *) {
            dialog->findChild<QPushButton *>("assemblyApply")->click();
        });
        check(changes == 1 && selection.assignment() == changed, "No-op invalidated selection");
        edit([&](QDialog *dialog, QTableWidget *rows) {
            static_cast<QDoubleSpinBox *>(rows->cellWidget(0, 1))->setValue(9);
            dialog->reject();
        });
        check(changes == 1 && selection.assignment() == changed, "Cancel applied a draft");
        edit([&](QDialog *dialog, QTableWidget *rows) {
            static_cast<QComboBox *>(rows->cellWidget(0, 0))->setCurrentIndex(2);
            check(!dialog->findChild<QPushButton *>("assemblyApply")->isEnabled(),
                  "Stock overuse enabled Apply");
            static_cast<QComboBox *>(rows->cellWidget(0, 0))->setCurrentIndex(0);
            rows->selectRow(1);
            dialog->findChild<QPushButton *>("assemblyMoveLeft")->click();
            check(static_cast<QComboBox *>(rows->cellWidget(0, 0))->currentData().toInt() == 1 &&
                      static_cast<QDoubleSpinBox *>(rows->cellWidget(1, 1))->value() == 10,
                  "Reordering moved a physical gap or lost a piece");
            dialog->findChild<QPushButton *>("assemblyRemove")->click();
            check(rows->rowCount() == 2, "Removal failed");
            dialog->findChild<QPushButton *>("assemblyAdd")->click();
            check(rows->rowCount() == 3 &&
                      dialog->findChild<QPushButton *>("assemblyApply")->isEnabled(),
                  "Released stock was not reusable");
            dialog->reject();
        });
        const auto doc = document(fixtures::jointBend(), folder.filePath("joint.step"));
        const auto source = std::string(doc->sourceStep());
        application::BendRouteRequest request;
        request.automatic = request.withTools = true;
        request.maximumChecks = 32;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &b = doc->metadata().bendAnalysis.bends.at(i);
            request.operations.push_back({"B" + std::to_string(i),
                                          i,
                                          {b.angleRadians, b.innerRadiusMm},
                                          core::toolParameters(*changed),
                                          *changed});
        }
        cad::SingleBendPreviewBuilder builder;
        const auto good = builder.buildRoute(doc, request, {});
        if (const auto *failure = std::get_if<application::BendRouteError>(&good))
            throw std::runtime_error("Local-window route: " + failure->detail);
        const auto route = std::get<application::BendRoutePtr>(good);
        check(route->steps.size() == 1 && route->steps[0].additionalBends.size() == 1,
              "Material-free window prevented joint bend");
        auto solidRequest = request;
        for (auto &op : solidRequest.operations) {
            op.assignment->die.segments = std::vector<core::ToolSegment>{{120, 1}};
            op.assignment->dieAssembly = core::ToolAssembly{{0}, 0};
            op.tools = core::toolParameters(*op.assignment);
        }
        const auto solid = builder.buildRoute(doc, solidRequest, {});
        check(std::holds_alternative<application::BendRoutePtr>(solid),
              "A cutout in the workpiece unnecessarily required a die window");
        const auto solidRoute = std::get<application::BendRoutePtr>(solid);
        check(solidRoute->steps.size() == 1 && !solidRoute->steps[0].assemblyAssignment &&
                  solidRoute->steps[0].preview->toolSegmentGaps() == std::array{0., 0.} &&
                  cad::OcctPreviewAccess::frame(*solidRoute->steps[0].preview, 0)
                          .toolSegments[0]
                          .size() == 1,
              "The planner split a continuous die merely because the part has a cutout");
        const auto p = route->steps[0].preview;
        check(p->toolSegmentGaps() == std::array{10., 0.} &&
                  p->uncoveredToolLengthMm() == std::array{0., 0.},
              "Independent gap report or support coverage lost");
        const auto &frame = cad::OcctPreviewAccess::frame(*p, 0);
        check(frame.toolSegments[0].size() == 3 && frame.toolSegments[1].size() == 1,
              "Separate assemblies shared segmentation");
        const std::array<std::array<double, 2>, 3> expected{{{0, 30}, {30, 55}, {65, 120}}};
        for (std::size_t i = 0; i < 3; ++i) {
            Bnd_Box box;
            BRepBndLib::AddOptimal(frame.toolSegments[0][i].shape, box, false, false);
            check(std::abs(box.CornerMin().Z() - expected[i][0]) < 1.e-5 &&
                      std::abs(box.CornerMax().Z() - expected[i][1]) < 1.e-5,
                  "Displayed/checker solid filled or moved the local window");
        }
        auto bad = request;
        for (auto &op : bad.operations) {
            op.assignment->dieAssembly = core::ToolAssembly{{0, 1, 2}, 0, {10, 0}};
            op.tools = core::toolParameters(*op.assignment);
        }
        const auto refused = builder.buildRoute(doc, bad, {});
        check(std::holds_alternative<application::BendRouteError>(refused),
              "A window under bend material reused a successful route");
        const auto simple = document(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 120),
                                     folder.filePath("continuous.step"));
        const auto &b = simple->metadata().bendAnalysis.bends.front();
        const auto diagnostic = builder.buildAssignedToolMotion(
            simple, 0, {b.angleRadians, b.innerRadiusMm}, *changed, {});
        if (const auto *failure = std::get_if<application::BendPreviewError>(&diagnostic))
            throw std::runtime_error("Manual gap preview: " + failure->detail);
        const auto missing =
            std::get<application::BendPreviewPtr>(diagnostic)->uncoveredToolLengthMm();
        check(std::abs(missing[0] - 10) < 1.e-5 && missing[1] < 1.e-5,
              "Manual geometric view hid loss of die support or affected punch");
        auto movedGap = *changed;
        movedGap.dieAssembly->jointGapsMm = {10, 0};
        const auto other = builder.buildAssignedToolMotion(
            simple, 0, {b.angleRadians, b.innerRadiusMm}, movedGap, {});
        check(std::holds_alternative<application::BendPreviewPtr>(other),
              "Manual shifted-gap diagnostic was unavailable");
        const auto &otherFrame =
            cad::OcctPreviewAccess::frame(*std::get<application::BendPreviewPtr>(other), 0);
        Bnd_Box movedBox;
        BRepBndLib::AddOptimal(otherFrame.toolSegments[0][1].shape, movedBox, false, false);
        check(std::abs(movedBox.CornerMin().Z() - 40) < 1.e-5 &&
                  std::abs(movedBox.CornerMax().Z() - 65) < 1.e-5,
              "Tool asset/single cache reused the previous local gap positions");
        const auto again = builder.buildRoute(doc, request, {});
        check(std::holds_alternative<application::BendRoutePtr>(again) &&
                  doc->sourceStep() == source,
              "Failed layout poisoned cache or changed source");
        const auto shortPart = document(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 40),
                                        folder.filePath("axial.step"));
        auto axialTools = *changed;
        axialTools.punch.segments = std::vector<core::ToolSegment>{{210, 1}};
        axialTools.punchAssembly = core::ToolAssembly{{0}, 0};
        axialTools.die.segments = std::vector<core::ToolSegment>{{10, 2}, {140, 1}};
        axialTools.dieAssembly = core::ToolAssembly{{0, 1, 0}, 0, {20, 30}};
        const auto &shortBend = shortPart->metadata().bendAnalysis.bends.front();
        application::BendRouteRequest axial;
        axial.automatic = axial.withTools = axial.allowAxialPlacement = true;
        axial.allowWorkpieceRotation = false;
        axial.operations.push_back({"B0",
                                    0,
                                    {shortBend.angleRadians, shortBend.innerRadiusMm},
                                    core::toolParameters(axialTools),
                                    axialTools});
        const auto centred = builder.buildRoute(shortPart, axial, {});
        check(std::holds_alternative<application::BendRoutePtr>(centred),
              "An edge support-gap refusal prevented trying a clear centred pose");
        const auto centredRoute = std::get<application::BendRoutePtr>(centred);
        check(centredRoute->steps.front().preview->workpieceOffsetMm() == 0 &&
                  centredRoute->statistics.preparedOperations == 1 &&
                  centredRoute->statistics.reusedOperationPreparations == 2,
              "Two failed edges were not checked/reused before the supported centre");
        cad::SingleBendPreviewBuilder limitedBuilder;
        axial.maximumChecks = 2;
        const auto limited = limitedBuilder.buildRoute(shortPart, axial, {});
        check(std::holds_alternative<application::BendRouteError>(limited) &&
                  std::get<application::BendRouteError>(limited).code ==
                      application::BendRouteErrorCode::SearchLimit,
              "A limited gap placement search claimed global impossibility");
        axial.maximumChecks = 3;
        const auto resumed = limitedBuilder.buildRoute(shortPart, axial, {});
        check(std::holds_alternative<application::BendRoutePtr>(resumed) &&
                  std::get<application::BendRoutePtr>(resumed)->statistics.reusedRejections >= 2 &&
                  std::get<application::BendRoutePtr>(resumed)
                          ->steps.front()
                          .preview->workpieceOffsetMm() == 0,
              "Compact failure cache lost placement dependence after a budget increase");
        auto punchGap = *changed;
        punchGap.die.segments = std::vector<core::ToolSegment>{{120, 1}};
        punchGap.dieAssembly = core::ToolAssembly{{0}, 0};
        punchGap.punch.segments = std::vector<core::ToolSegment>{{10, 1}, {45, 1}, {55, 1}};
        punchGap.punchAssembly = core::ToolAssembly{{0, 1, 2}, 0, {10, 0}};
        for (const bool reversed : {false, true}) {
            punchGap.punchReversed = reversed;
            const auto preview = builder.buildAssignedToolMotion(
                shortPart, 0, {shortBend.angleRadians, shortBend.innerRadiusMm}, punchGap, {}, 0,
                false, -40);
            check(std::holds_alternative<application::BendPreviewPtr>(preview),
                  "Asymmetric punch assembly preview failed");
            const auto pp = std::get<application::BendPreviewPtr>(preview);
            check(pp->uncoveredToolLengthMm()[0] < 1.e-5 &&
                      std::abs(pp->uncoveredToolLengthMm()[1] - (reversed ? 0 : 10)) < 1.e-5,
                  "Coverage ignored the actual Y half-turn of an asymmetric punch assembly");
            Bnd_Box box;
            BRepBndLib::AddOptimal(cad::OcctPreviewAccess::frame(*pp, 0).toolSegments[1][1].shape,
                                   box, false, false);
            check(std::abs(box.CornerMin().Z() - (reversed ? 55 : 20)) < 1.e-5 &&
                      std::abs(box.CornerMax().Z() - (reversed ? 100 : 65)) < 1.e-5,
                  "Punch support report and rotated CAD sections disagree");
            application::RouteDraftRequest draft;
            draft.withTools = true;
            draft.step = 1;
            draft.operations = {{"B0",
                                 0,
                                 {shortBend.angleRadians, shortBend.innerRadiusMm},
                                 core::toolParameters(punchGap),
                                 punchGap,
                                 false,
                                 -40}};
            const auto viewed = builder.buildRouteDraft(shortPart, draft, {});
            check(std::holds_alternative<application::RouteDraftPtr>(viewed) &&
                      std::abs(std::get<application::RouteDraftPtr>(viewed)
                                   ->visual->uncoveredToolLengthMm()[1] -
                               (reversed ? 0 : 10)) < 1.e-5,
                  "Manual draft lost asymmetric punch coverage/orientation");
        }
        // Exercise the real stage/controller scope, not just the standalone editor.
        auto stagePair = tool_fixture::explicitPair(
            core::PressTools{32, std::numbers::pi / 3, std::numbers::pi / 6, .8, 120, 10});
        stagePair.punch.segments = changed->punch.segments;
        stagePair.die.segments = changed->die.segments;
        stagePair.punchAssembly = changed->punchAssembly;
        stagePair.dieAssembly = changed->dieAssembly;
        auto files = std::make_shared<project::ProjectFiles>();
        ui::MainWindow integrated({"test", "test"}, files, files,
                                  std::make_shared<cad::SingleBendPreviewBuilder>(),
                                  {stagePair.punch, stagePair.die});
        integrated.setAutomaticPartInputPrompt(false);
        integrated.resize(1100, 760);
        integrated.show();
        integrated.openStep(folder.filePath("joint.step"));
        auto &controller = integrated.importController();
        const auto idle = [&] {
            QElapsedTimer timer;
            timer.start();
            while (controller.hasWork() && timer.elapsed() < 30000) {
                QApplication::processEvents();
                QThread::msleep(2);
            }
            check(!controller.hasWork(), "Controller timeout");
            QApplication::processEvents();
        };
        idle();
        for (std::size_t i = 0; i < 2; ++i)
            check(controller.setBendToolAssignment(i, stagePair), "Cannot capture tools");
        integrated.findChild<QToolButton *>("processStage1")->click();
        auto *scope = integrated.findChild<QComboBox *>("toolAssignmentScope");
        auto *assign = integrated.findChild<QPushButton *>("assignBendTool");
        const auto editIntegrated = [&](double first, double second) {
            QTimer::singleShot(0, [&] {
                auto *dialog = integrated.findChild<QDialog *>("toolAssemblyDialog");
                if (!dialog) {
                    error = "No integrated assembly editor";
                    return;
                }
                auto *rows = dialog->findChild<QTableWidget *>("assemblyRows");
                if (!rows || rows->rowCount() != 3) {
                    error = "Stage lost the captured assembly";
                    dialog->reject();
                    return;
                }
                static_cast<QDoubleSpinBox *>(rows->cellWidget(0, 1))->setValue(first);
                static_cast<QDoubleSpinBox *>(rows->cellWidget(1, 1))->setValue(second);
                dialog->findChild<QPushButton *>("assemblyApply")->click();
            });
            integrated.findChild<QPushButton *>("editDieAssembly")->click();
            check(error.isEmpty(), qPrintable(error));
            if (!assign->isEnabled()) {
                std::cerr << "Stage state: busy=" << controller.hasWork() << " row="
                          << integrated.findChild<QListView *>("bendList")->currentIndex().row()
                          << " manualThickness="
                          << controller.session().manualThicknessMm().value_or(-1) << " interior="
                          << controller.session()
                                 .document()
                                 ->metadata()
                                 .bendAnalysis.interiorContourDiagnostics.size()
                          << " filled="
                          << static_cast<int>(
                                 controller.session().document()->metadata().closedCutouts.status)
                          << '\n';
                for (const auto *label : integrated.findChildren<QLabel *>())
                    if (!label->objectName().isEmpty())
                        std::cerr << label->objectName().toStdString() << ": "
                                  << label->text().toStdString() << '\n';
                throw std::runtime_error("Cannot commit stage assembly");
            }
            assign->click();
            idle();
        };
        scope->setCurrentIndex(0);
        editIntegrated(1, 9);
        auto common = stagePair;
        common.dieAssembly->jointGapsMm = {1, 9};
        check(controller.session().bend(0)->catalogTools == common &&
                  controller.session().bend(1)->catalogTools == common,
              "Stage 0 did not apply the assembly to both source IDs");
        scope->setCurrentIndex(
            scope->findData(QString::fromStdString(controller.session().bend(0)->id)));
        editIntegrated(2, 8);
        auto individual = common;
        individual.dieAssembly->jointGapsMm = {2, 8};
        check(controller.session().bend(0)->catalogTools == individual &&
                  controller.session().bend(1)->catalogTools == common,
              "Individual edit changed another bend, profile or punch assembly");
        integrated.hide();
        if (app.arguments().contains("--graphics")) {
            auto *viewer = new viewer::ViewerWidget(&window);
            workspace->hide();
            layout->addWidget(viewer);
            QElapsedTimer time;
            time.start();
            while (!viewer->isReady() && time.elapsed() < 10000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            check(viewer->isReady(), "No native graphics context");
            check(viewer->presentDocument(doc, 1).isEmpty(), "Document presentation failed");
            check(viewer->presentBendPreview(p, 0, 1).isEmpty(), "Preview presentation failed");
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
            viewer->fitAll();
            QApplication::processEvents();
            if (const auto path = qEnvironmentVariable("BENDING_ASSEMBLY_SNAPSHOT");
                !path.isEmpty())
                check(viewer->captureImage(true).save(path + "-scene.png"), "Scene snapshot");
        }
        std::cout << "Manual local gaps: atomic editing, exact solids, joint support and caches\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
