#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 200000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(done(), "Workflow timeout");
    QApplication::processEvents();
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}
class LimitedBuilder final : public application::SingleBendPreviewBuilder {
  public:
    mutable std::atomic<unsigned> discards = 0;
    void discardRouteCache() const override {
        ++discards;
    }
    mutable std::optional<application::BendRouteRequest> last;
    application::BendRouteResult buildRoute(application::DocumentPtr,
                                            application::BendRouteRequest request,
                                            std::stop_token) const override {
        last = std::move(request);
        return application::BendRouteError{application::BendRouteErrorCode::SearchLimit,
                                           "Test budget limit"};
    }
    application::BendPreviewResult build(application::DocumentPtr, std::size_t, core::BendTarget,
                                         bool, std::stop_token) const override {
        return application::BendPreviewError{application::BendPreviewErrorCode::UnsupportedGeometry,
                                             "No single operation"};
    }
};
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "No test directory");
        ui::setPreferencesFile(folder.path() + "/settings.ini");
        const bool sample = app.arguments().size() > 1;
        auto pair = tool_fixture::pair(16);
        pair.punch.segments = std::vector<core::ToolSegment>{{40, 2}};
        pair.die.segments = std::vector<core::ToolSegment>{{40, 2}};
        auto other = tool_fixture::pair(20, .8, 2);
        other.punch.id = "other-punch";
        other.punch.code = "P60-other";
        other.die.id = "other-die";
        other.die.code = "V20-other";
        auto files = std::make_shared<project::ProjectFiles>();
        ui::MainWindow window({"test", "test"}, files, files,
                              std::make_shared<cad::SingleBendPreviewBuilder>(),
                              {pair.punch, pair.die, other.punch, other.die});
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        const auto path = sample ? app.arguments()[1] : folder.path() + "/channel.step";
        if (!sample) {
            STEPControl_Writer writer;
            const auto shape = BRepAlgoAPI_Cut(fixtures::channel(),
                                               BRepPrimAPI_MakeBox(gp_Pnt(80, -1, 40), 20, 50, 50))
                                   .Shape();
            require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
                    "Fixture write failed");
        }
        auto &controller = window.importController();
        window.openStep(path);
        wait([&] { return !controller.hasWork(); });
        require(bool(controller.session().document()), "No document");
        const auto &bends = controller.session().document()->metadata().bendAnalysis.bends;
        require(bends.size() > 1, "Expected a multi-bend example");
        window.findChild<QToolButton *>("processStage2")->click();
        wait([&] { return !controller.hasWork(); });
        auto *mode = window.findChild<QComboBox *>("bendRouteMode");
        mode->setCurrentIndex(2);
        auto *prepare = window.findChild<QPushButton *>("prepareBendRoute");
        require(!prepare->isEnabled() && !controller.hasWork(),
                "Missing assignments started a search");
        window.findChild<QPushButton *>("routeStep1")->click();
        window.findChild<QPushButton *>("routeEditTool")->click();
        wait([&] { return !controller.hasWork(); });
        auto *scope = window.findChild<QComboBox *>("toolAssignmentScope");
        auto *selection = window.findChild<QComboBox *>("toolBendSelection");
        auto *assign = window.findChild<QPushButton *>("assignBendTool");
        require(scope->currentIndex() == 1 && selection->isVisible(), "No per-bend tool editor");
        std::vector<std::optional<core::PressTools>> expected(bends.size());
        std::vector<std::optional<core::ToolAssignment>> assignments(bends.size());
        for (std::size_t index = 0; index < bends.size(); ++index) {
            selection->setCurrentIndex(selection->findData(static_cast<int>(index)));
            if (sample) {
                // Conditional tools: this audit does not claim workshop stock/force compatibility.
                window.findChild<QComboBox *>("toolSourceMode")->setCurrentIndex(1);
                const auto set = [&](const char *name, double value) {
                    window.findChild<QDoubleSpinBox *>(name)->setValue(value);
                };
                set("toolOpening", 8 * bends[index].thicknessMm);
                set("toolDieAngle", 60);
                set("toolPunchAngle", 30);
                set("toolNoseRadius", .8);
                set("toolLength", bends[index].lengthMm + 5);
                set("toolPunchLength", bends[index].lengthMm + 5);
                set("toolClearance", 10);
            } else {
                window.findChild<QComboBox *>("punchPreset")->setCurrentIndex(index == 0 ? 1 : 2);
                window.findChild<QComboBox *>("diePreset")->setCurrentIndex(index == 0 ? 1 : 2);
            }
            require(assign->isEnabled(), "Cannot assign the selected tool");
            assign->click();
            require(!controller.hasWork() && !controller.bendRoute(),
                    "Assigning a tool launched geometry work");
            for (std::size_t earlier = 0; earlier < index; ++earlier)
                require(controller.session().bend(earlier)->tools == expected[earlier] &&
                            controller.session().bend(earlier)->catalogTools ==
                                assignments[earlier],
                        "Assigning one bend overwrote another");
            expected[index] = controller.session().bend(index)->tools;
            assignments[index] = controller.session().bend(index)->catalogTools;
            require(expected[index].has_value(), "Assignment was not stored");
            if (!sample)
                require(assignments[index] && assignments[index]->useDrawingGeometry,
                        "Lost the actual tool contour");
            if (!sample && index == 0)
                require(expected[index]->lengthMm == 40 && expected[index]->openingMm == 16,
                        "The short bend was sized for the whole route");
            std::cout << "B" << index + 1 << " L=" << bends[index].lengthMm
                      << " tool=" << expected[index]->lengthMm
                      << " V=" << expected[index]->openingMm << std::endl;
            if (index + 1 < bends.size())
                require(!prepare->isEnabled(), "A partial set appears complete");
        }
        require(prepare->isEnabled(), "All assigned bends still block the route");
        window.findChild<QToolButton *>("processStage2")->click();
        wait([&] { return !controller.hasWork(); });
        window.findChild<QPushButton *>("checkBend")->click();
        require(controller.hasWork(), "Primary check did not launch the per-bend route");
        wait([&] { return !controller.hasWork(); });
        if (app.arguments().contains("--expand") && controller.bendRouteError() &&
            controller.bendRouteError()->code == application::BendRouteErrorCode::SearchLimit) {
            std::cout << "Retrying with the explicit expanded search budget" << std::endl;
            window.findChild<QPushButton *>("expandBendRouteSearch")->click();
            require(controller.hasWork(), "Expanded search did not start");
            wait([&] { return !controller.hasWork(); });
        }
        if (const auto error = controller.bendRouteError())
            throw std::runtime_error(
                "Route" +
                (error->bendIndex ? " B" + std::to_string(*error->bendIndex + 1) : std::string{}) +
                " checks=" + std::to_string(error->evaluatedOperations) + ": " + error->detail);
        const auto route = controller.bendRoute();
        require(route && route->steps.size() == bends.size() && route->withTools &&
                    mode->currentIndex() == 2 && !controller.bendPreview(),
                "No complete route");
        for (std::size_t i = 0; i < route->steps.size(); ++i) {
            const auto &step = route->steps[i];
            require(step.preview->toolSetup()->tools == *expected[step.bendIndex],
                    "The cycle used another bend's tool");
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(i + 1))->click();
            require(controller.bendRoute() == route && !controller.hasWork(),
                    "Step selection recalculated the route");
        }
        window.findChild<QSlider *>("bendFrame")->setValue(20);
        window.findChild<QPushButton *>("checkBendRoute")->click();
        wait([&] { return !controller.hasWork(); });
        const auto reused = controller.bendRoute();
        require(reused && reused->statistics.builtTransitions == 0 &&
                    reused->statistics.reusedTransitions == route->steps.size() &&
                    window.findChild<QSlider *>("bendFrame")->value() == 0,
                "The UI did not reuse complete verified cycles");
        require(window.findChild<QToolButton *>("processStage1")
                    ->accessibleName()
                    .contains(QStringLiteral("По гибам")),
                "The process card reports another bend's editor tool as the route tool");
        for (std::size_t i = 0; i < reused->steps.size(); ++i)
            require(reused->steps[i].preview == route->steps[i].preview,
                    "Matching cached cycles were replaced or reordered");
        window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(bends.size()))->click();
        if (!sample)
            require(window.findChild<QLabel *>("bendRouteStatus")
                        ->text()
                        .contains(QStringLiteral("переналадка не проверена")),
                    "Tool change has no scope warning");
        if (!sample) {
            std::vector<std::string> order;
            for (auto it = route->steps.rbegin(); it != route->steps.rend(); ++it)
                order.push_back(it->id);
            require(controller.setBendOrder(order) && !controller.bendRoute(),
                    "Changing order retained a validated route");
            window.findChild<QPushButton *>("checkBendRoute")->click();
            wait([&] { return !controller.hasWork(); });
            const auto manual = controller.bendRoute();
            require(manual && manual->steps.front().id == order.front() &&
                        mode->currentIndex() == 2,
                    "Manual checking lost the per-bend mode/order");
            for (const auto &step : manual->steps)
                require(step.preview->toolSetup()->tools == *expected[step.bendIndex],
                        "Reordering moved tool assignments to other bends");
        }
        if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT"); !output.isEmpty()) {
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(bends.size()))
                ->click();
            window.findChild<QSlider *>("bendFrame")->setValue(15);
            QApplication::processEvents();
            auto shot = window.grab().toImage();
            auto *viewer = window.findChild<viewer::ViewerWidget *>();
            if (viewer && viewer->isReady()) {
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
            }
            require(shot.save(output), "Snapshot failed");
        }
        const auto saved = folder.path() + "/route.bend";
        controller.saveProject(native(saved), files);
        wait([&] { return !controller.hasWork(); });
        require(controller.saveError().isEmpty(), "Project save failed");
        window.openStep(saved);
        wait([&] { return !controller.hasWork(); });
        const bool distinct =
            std::any_of(expected.begin(), expected.end(),
                        [&](const auto &tool) { return tool != expected.front(); }) ||
            std::any_of(assignments.begin(), assignments.end(),
                        [&](const auto &assignment) { return assignment != assignments.front(); });
        require(scope->currentIndex() == (distinct ? 1 : 0) && !controller.bendRoute(),
                "Reopening lost the per-bend mode or restored stale validation");
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(controller.session().bend(i)->tools == expected[i] &&
                        controller.session().bend(i)->catalogTools == assignments[i],
                    "Portable project lost individual assignments");
        controller.shutdown();
        if (!sample) {
            auto builder = std::make_shared<LimitedBuilder>();
            ui::MainWindow budgetWindow({"test", "test"}, files, files, builder);
            budgetWindow.setAutomaticPartInputPrompt(false);
            budgetWindow.show();
            auto &budgetController = budgetWindow.importController();
            budgetWindow.openStep(saved);
            wait([&] { return !budgetController.hasWork(); });
            budgetWindow.findChild<QToolButton *>("processStage2")->click();
            wait([&] { return !budgetController.hasWork(); });
            budgetWindow.findChild<QComboBox *>("bendRouteMode")->setCurrentIndex(0);
            wait([&] { return !budgetController.hasWork(); });
            budgetWindow.findChild<QPushButton *>("checkBendRoute")->click();
            wait([&] { return !budgetController.hasWork(); });
            auto *expand = budgetWindow.findChild<QPushButton *>("expandBendRouteSearch");
            require(builder->last && builder->last->maximumChecks == 96 &&
                        builder->last->maximumTimeMs == 60000 && !builder->last->automatic &&
                        !builder->last->withTools && expand->isVisible() && expand->isEnabled(),
                    "Default budget or manual scope lost");
            expand->click();
            wait([&] { return !budgetController.hasWork(); });
            require(builder->last->maximumChecks == 512 && builder->last->maximumTimeMs == 120000 &&
                        !builder->last->automatic && !builder->last->withTools &&
                        expand->isHidden() && !expand->isEnabled(),
                    "Explicit retry changed the mode or kept offering an identical retry");
            const auto discards = builder->discards.load();
            budgetController.closeDocument();
            require(builder->discards > discards, "Closing a document retained cached route work");
            budgetController.shutdown();
        }
        std::cout << "Independent tool assignments, checked cycles and portable reopening passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
