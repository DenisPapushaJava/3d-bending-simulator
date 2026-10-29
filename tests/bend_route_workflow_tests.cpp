#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
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
        const int libraryArgument = app.arguments().indexOf("--library");
        const bool shortTools = app.arguments().contains("--short-tools");
        const bool partialSupport = app.arguments().contains("--partial-support");
        const bool narrowSupport = app.arguments().contains("--narrow-support");
        const bool interruptedSupport = app.arguments().contains("--interrupted-support");
        const bool withTools = app.arguments().contains("--tools") || shortTools ||
                               libraryArgument > 0 || partialSupport || interruptedSupport;
        auto pair = tool_fixture::pair(16);
        if (shortTools) {
            pair.punch.lengthMm = 40;
            pair.die.lengthMm = 40;
        } else if (withTools && libraryArgument < 0) {
            pair.punch.segments = std::vector<core::ToolSegment>{{40, 2}};
            pair.die.segments = std::vector<core::ToolSegment>{{40, 2}};
        }
        core::ToolCatalog catalog =
            withTools ? core::ToolCatalog{pair.punch, pair.die} : core::ToolCatalog{};
        if (libraryArgument > 0) {
            require(libraryArgument + 1 < app.arguments().size(), "Missing library path");
            auto loaded = project::readToolCatalog(native(app.arguments()[libraryArgument + 1]));
            if (const auto *error = std::get_if<std::string>(&loaded))
                throw std::runtime_error(*error);
            catalog = std::get<core::ToolCatalog>(std::move(loaded));
        }
        auto previewBuilder = std::make_shared<cad::SingleBendPreviewBuilder>();
        ui::MainWindow window({"test", "test"}, files, files, previewBuilder, std::move(catalog));
        window.setAutomaticPartInputPrompt(false);
        window.resize(1280, 850);
        window.show();
        const bool sample = app.arguments().size() > 1 && !app.arguments()[1].startsWith("--");
        auto path = sample ? app.arguments()[1] : folder.path() + "/channel.step";
        if (!sample) {
            STEPControl_Writer writer;
            auto shape = fixtures::channel();
            if (partialSupport) {
                BRepBuilderAPI_MakeWire wire;
                const std::array<gp_Pnt, 3> points{
                    {{83, 6, 80}, {83, 42, 80}, {83, 42, narrowSupport ? -200. : 72.}}};
                for (std::size_t i = 0; i < points.size(); ++i)
                    wire.Add(BRepBuilderAPI_MakeEdge(points[i], points[(i + 1) % points.size()]));
                const auto wedge =
                    BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(8, 0, 0))
                        .Shape();
                shape = BRepAlgoAPI_Cut(shape, wedge).Shape();
            } else if (interruptedSupport)
                shape = BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeBox(gp_Pnt(83, 10, 39), 8, 40, 2))
                            .Shape();
            else if (withTools && !shortTools)
                shape = BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeBox(gp_Pnt(80, -1, 40), 20, 50, 50))
                            .Shape();
            require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
                    "Fixture write");
        }
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
        const int count =
            static_cast<int>(controller.session().document()->metadata().bendAnalysis.bends.size());
        require(count > 1, "The route fixture must have several bends");
        const auto &bends = controller.session().document()->metadata().bendAnalysis.bends;
        const auto shorter = static_cast<std::size_t>(
            std::min_element(bends.begin(), bends.end(),
                             [](const auto &a, const auto &b) { return a.lengthMm < b.lengthMm; }) -
            bends.begin());
        const auto draftIndex =
            withTools && !sample && !shortTools && !partialSupport && !interruptedSupport
                ? shorter
                : std::size_t{1};
        auto *draft =
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(draftIndex + 1));
        require(draft && draft->toolTip().contains(QStringLiteral("Черновик")),
                "Unprepared route looks like a ready animation");
        draft->click();
        require(!controller.hasWork() && !controller.bendRoute() &&
                    window.findChild<QListView *>("bendList")->currentIndex().row() ==
                        static_cast<int>(draftIndex),
                "A draft step neither selected its source bend nor avoided calculation");
        if (withTools) {
            if (shortTools) {
                window.findChild<QComboBox *>("punchPreset")->setCurrentIndex(1);
                window.findChild<QComboBox *>("diePreset")->setCurrentIndex(1);
            } else {
                window.findChild<QPushButton *>("recommendTools")->click();
                if (sample)
                    std::cout
                        << window.findChild<QLabel *>("toolAssemblySummary")->text().toStdString()
                        << '\n'
                        << window.findChild<QLabel *>("toolRecommendation")->text().toStdString()
                        << '\n';
                if (!sample && !partialSupport && !interruptedSupport) {
                    require(bends[draftIndex].lengthMm < std::max_element(
                                                             bends.begin(), bends.end(),
                                                             [](const auto &a, const auto &b) {
                                                                 return a.lengthMm < b.lengthMm;
                                                             })
                                                             ->lengthMm,
                            "The tool sizing fixture must select the shorter bend");
                    require(
                        window.findChild<QLabel *>("toolAssemblySummary")->text().contains("80.00"),
                        "The common tool request ignored the longer bend");
                }
            }
            if (sample && app.arguments().contains("--selection-only")) {
                controller.shutdown();
                return 0;
            }
            // Reproduce the normal user path, including a previously changed route mode.
            mode->setCurrentIndex(0);
            auto *check = window.findChild<QPushButton *>("checkBend");
            require(check->isEnabled() && check->text().contains(QStringLiteral("все гибы")),
                    "The main check does not explain its multi-bend scope");
            check->click();
        } else {
            mode->setCurrentIndex(0);
            require(prepare->isEnabled(), "Cannot prepare geometry route");
            prepare->click();
        }
        wait([&] { return !controller.hasWork(); });
        if (shortTools) {
            const auto error = controller.bendRouteError();
            const auto status = window.findChild<QLabel *>("bendRouteStatus")->text();
            require(error && error->toolLengths && error->evaluatedOperations == 0 &&
                        !controller.bendRoute() && !controller.bendPreview() &&
                        status.contains("80.00") && status.contains("40.00") &&
                        !status.contains(QStringLiteral("порядок не найден")) &&
                        window.findChild<QLabel *>("bendCheckSummary")
                            ->text()
                            .contains(QStringLiteral("Порядок не подготовлен")) &&
                        window.findChild<QPushButton *>("resolveCheckProblem")->isVisible(),
                    "The short-tool diagnostic lacks lengths or pretends a geometry search failed");
            QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QApplication::processEvents();
            require(window.findChild<QWidget *>("bendRoutePanel")->height() < 160,
                    "A stale wrapped-label minimum consumed the viewport height");
            if (const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
                !output.isEmpty())
                require(window.grab().save(output), "Diagnostic layout snapshot failed");
            controller.shutdown();
            return 0;
        }
        if (interruptedSupport || narrowSupport) {
            const auto error = controller.bendRouteError();
            require(error &&
                        error->operationError ==
                            application::BendPreviewErrorCode::SupportNotProven &&
                        !controller.bendRoute() && !controller.bendPreview(),
                    "An interrupted or off-center support strip was allowed as a central "
                    "end-support preview");
            controller.shutdown();
            return 0;
        }
        if (const auto error = controller.bendRouteError()) {
            if (sample) {
                const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
                if (!output.isEmpty())
                    require(window.grab().save(output), "Error snapshot failed");
            }
            throw std::runtime_error(error->detail);
        }
        const auto route = controller.bendRoute();
        require(route && route->steps.size() == static_cast<std::size_t>(count) &&
                    !controller.bendPreview(),
                "The main check prepared only one bend instead of the complete route");
        require(mode->currentIndex() == (withTools ? 1 : 0), "Route mode differs from its result");
        require(route->withTools == withTools &&
                    route->steps[0].preview->usesDrawingPunch() == withTools &&
                    route->steps[0].preview->usesDrawingDie() == withTools,
                "UI route lost its actual tool profile mode");
        if (partialSupport) {
            require(route->partialEndSupport &&
                        window.findChild<QLabel *>("bendCheckSummary")
                            ->text()
                            .contains(QStringLiteral("неполной опорой")) &&
                        !window.findChild<QLabel *>("bendCheckSummary")
                             ->text()
                             .contains(QStringLiteral("проверка пройдена")),
                    "The partial support preview was presented as a fully checked setup");
            application::BendRouteRequest strict;
            strict.withTools = true;
            for (auto index : controller.session().orderedBends()) {
                const auto *entry = controller.session().bend(index);
                strict.operations.push_back({entry->id, index,
                                             *controller.session().bendTarget(index), entry->tools,
                                             entry->catalogTools});
            }
            const auto result =
                previewBuilder->buildRoute(controller.session().document(), strict, {});
            require(std::holds_alternative<application::BendRouteError>(result) &&
                        std::get<application::BendRouteError>(result).operationError ==
                            application::BendPreviewErrorCode::SupportNotProven,
                    "The strict route API silently accepted a partial end support");
        }
        if (withTools && !sample)
            for (int i = 0; i < count; ++i) {
                const auto *entry = controller.session().bend(static_cast<std::size_t>(i));
                require(entry->tools && entry->tools->lengthMm == 80 &&
                            entry->catalogTools->punchAssembly->pieces.size() == 2 &&
                            entry->catalogTools->dieAssembly->pieces.size() == 2,
                        "A bend retained an assembly sized only for the shorter line");
            }
        auto *viewerWidget = window.findChild<viewer::ViewerWidget *>("viewer");
        auto *strip = window.findChild<QWidget *>("routeStepStrip");
        require(viewerWidget && strip && viewerWidget->isAncestorOf(strip),
                "The route strip is outside the viewport");
        for (int i = 0; i <= count; ++i)
            require(window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(i))->text() ==
                        QString::number(i),
                    "Route numbers contain feature IDs");
        require(window.findChild<QPushButton *>("routeStep0")
                    ->toolTip()
                    .contains(QStringLiteral("заготовка")),
                "Blank stage is ambiguous");
        auto *frame = window.findChild<QSlider *>("bendFrame");
        std::vector<QImage> images;
        for (int index = 0; index <= count; ++index) {
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(index))->click();
            require(controller.bendRoute() == route && !controller.hasWork() && frame->value() == 0,
                    "Stage navigation recalculated or retained a different frame");
            if (index == 0)
                require(!frame->isEnabled(), "The blank stage exposes a bending slider");
            else if (QApplication::platformName() != "offscreen")
                require(frame->isEnabled(), "Native operation playback is unavailable");
            if (index > 0) {
                frame->setValue(10);
                if (viewerWidget->isReady()) {
                    const auto image = viewerWidget->captureImage();
                    for (const auto &previous : images)
                        require(image != previous, "Different steps display the same 3D state");
                    images.push_back(image);
                }
            }
        }
        for (int index : {0, count, 1, count}) {
            window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(index))->click();
            require(controller.bendRoute() == route && !controller.hasWork() && frame->value() == 0,
                    "Arbitrary navigation recalculated or showed the end of another step");
        }
        if (sample) {
            if (route->partialEndSupport)
                for (std::size_t i = 0; i < route->steps.size(); ++i)
                    if (const auto *support = route->steps[i].preview->partialEndSupport()) {
                        std::cout << "B" << route->steps[i].bendIndex + 1 << " support material "
                                  << support->supportedFromMm << ".." << support->supportedToMm
                                  << " / " << support->bendLengthMm << " mm\n";
                        window.findChild<QPushButton *>(QStringLiteral("routeStep%1").arg(i + 1))
                            ->click();
                        frame->setValue(frame->maximum() / 2);
                        break;
                    }
            QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QApplication::processEvents();
            const auto *status = window.findChild<QLabel *>("bendRouteStatus");
            require(status->height() >= status->heightForWidth(status->width()),
                    "The real-model warning was captured before its layout settled");
            const auto output = qEnvironmentVariable("BENDING_ROUTE_SNAPSHOT");
            if (!output.isEmpty()) {
                auto shot = window.grab().toImage();
                if (viewerWidget->isReady()) {
                    QPainter painter(&shot);
                    painter.drawImage(viewerWidget->mapTo(&window, QPoint()),
                                      viewerWidget->captureImage(true));
                }
                require(shot.save(output), "Sample snapshot failed");
            }
            controller.shutdown();
            std::cout << count << " route steps navigated successfully\n";
            return 0;
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
        const auto *routeStatus = window.findChild<QLabel *>("bendRouteStatus");
        require(routeStatus->height() >= routeStatus->heightForWidth(routeStatus->width()),
                "The route warning is clipped instead of wrapping at the current width");
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
