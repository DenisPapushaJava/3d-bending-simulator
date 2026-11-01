#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <STEPControl_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void wait(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 60000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(done(), "Workflow timeout");
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QApplication::processEvents();
}
int differences(const QImage &a, const QImage &b) {
    require(a.size() == b.size(), "A color change resized the viewport");
    int count = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            count += a.pixel(x, y) != b.pixel(x, y);
    return count;
}
QPushButton *colorButton(QDialog &dialog, double length) {
    auto *rows = dialog.findChild<QTableWidget *>("stockRows");
    for (int row = 0; row < rows->rowCount(); ++row)
        if (static_cast<QDoubleSpinBox *>(rows->cellWidget(row, 0))->value() == length)
            return static_cast<QPushButton *>(rows->cellWidget(row, 3));
    throw std::runtime_error("No color row");
}
void pick(QPushButton *button, QColor color, bool accept) {
    require(button && button->isEnabled(), "Color chooser disabled with stock accounting");
    bool seen = false;
    QTimer::singleShot(0, [&] {
        auto *picker = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
        if (!picker)
            return;
        seen = true;
        picker->setCurrentColor(color);
        if (accept)
            picker->accept();
        else
            picker->reject();
    });
    button->click();
    require(seen, "The color picker did not open");
}
void editStock(ui::MainWindow &window, const std::function<void(QDialog &)> &edit) {
    QString error;
    QTimer::singleShot(0, [&] {
        auto *dialog = window.findChild<QDialog *>("toolLibraryDialog");
        try {
            require(dialog, "No stock dialog");
            edit(*dialog);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        if (dialog)
            dialog->reject();
    });
    window.findChild<QPushButton *>("toolStock")->click();
    require(error.isEmpty(), qPrintable(error));
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "No settings folder");
        const auto settingsFile = folder.path() + "/settings.ini";
        ui::setPreferencesFile(settingsFile);
        auto pair = tool_fixture::pair();
        pair.punch.lengthMm = pair.die.lengthMm = 835;
        pair.punch.segments = std::vector<core::ToolSegment>{{100, 1}, {60, 1}, {50, 1}};
        pair.die.segments = pair.punch.segments;
        auto files = std::make_shared<project::ProjectFiles>();
        ui::MainWindow window({"test", "test"}, files, files,
                              std::make_shared<cad::SingleBendPreviewBuilder>(),
                              {pair.punch, pair.die});
        window.setAutomaticPartInputPrompt(false);
        int libraryWrites = 0;
        window.setToolProfileImport(std::make_shared<project::DxfToolImporter>(),
                                    {project::readToolLibrary,
                                     [&](const auto &path, const auto &data, const auto &revision) {
                                         ++libraryWrites;
                                         return project::writeToolLibrary(path, data, revision);
                                     }});
        window.resize(1280, 850);
        window.show();
        const auto file = folder.path() + "/colors.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 210, true, 20),
                                STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.toUtf8().constData()) == IFSelect_RetDone,
                "Fixture write");
        window.openStep(file);
        auto &controller = window.importController();
        wait([&] { return !controller.hasWork(); });
        window.findChild<QAction *>("recommendToolsQuick")->trigger();
        window.findChild<QPushButton *>("checkBend")->click();
        wait([&] { return !controller.hasWork(); });
        if (const auto error = controller.bendPreviewError())
            throw std::runtime_error(error->detail);
        const auto preview = controller.bendPreview();
        require(preview && preview->frameCount() == 61, "No complete segmented tool preview");
        for (std::size_t frameIndex : {0U, 30U, 60U}) {
            const auto &frame = cad::OcctPreviewAccess::frame(*preview, frameIndex);
            for (std::size_t tool = 0; tool < 2; ++tool) {
                std::multiset<double> lengths;
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(frame.tools[tool], TopAbs_FACE, faces);
                for (const auto &segment : frame.toolSegments[tool]) {
                    lengths.insert(segment.lengthMm);
                    TopTools_IndexedMapOfShape segmentFaces;
                    TopExp::MapShapes(segment.shape, TopAbs_FACE, segmentFaces);
                    require(segmentFaces.Extent() > 0, "Empty display section");
                    for (int i = 1; i <= segmentFaces.Extent(); ++i)
                        require(faces.Contains(segmentFaces(i)),
                                "Color subshape lost its tool placement");
                }
                require(lengths == std::multiset<double>{50, 60, 100},
                        "Nominal length replaced section lengths");
            }
        }
        const auto revision = controller.session().editRevision();
        auto *slider = window.findChild<QSlider *>("bendFrame");
        slider->setValue(30);
        auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
        const bool graphics = app.arguments().contains("--graphics");
        if (graphics)
            wait([&] { return viewer->isReady(); });
        const auto before = graphics ? viewer->captureImage() : QImage{};
        const QColor chosen("#718CA7");
        editStock(window, [&](QDialog &dialog) {
            const auto default60 = colorButton(dialog, 60)->property("segmentColor");
            pick(colorButton(dialog, 100), chosen, true);
            require(colorButton(dialog, 100)->property("segmentColor").toString() == chosen.name(),
                    "Color swatch not updated");
            pick(colorButton(dialog, 60), QColor("#A284A7"), false);
            require(colorButton(dialog, 60)->property("segmentColor") == default60,
                    "Cancelled picker changed the color");
            dialog.findChild<QListWidget *>("libraryTools")->setCurrentRow(1);
            require(colorButton(dialog, 100)->property("segmentColor").toString() == chosen.name(),
                    "Die and punch colors disagree for one length");
            dialog.findChild<QPushButton *>("stockSave")->click();
            require(libraryWrites == 0, "Saving colors rewrote stock/catalog data");
            if (const auto output = qEnvironmentVariable("BENDING_COLOR_STOCK_SNAPSHOT");
                !output.isEmpty()) {
                QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                QApplication::processEvents();
                require(dialog.grab().save(output), "Stock color snapshot failed");
            }
        });
        require(controller.bendPreview() == preview &&
                    controller.session().editRevision() == revision && !controller.hasWork() &&
                    slider->value() == 30,
                "Color edit invalidated/recalculated the simulation");
        QSettings saved(settingsFile, QSettings::IniFormat);
        require(saved.value("viewer/segmentColors/100000").toString() == chosen.name(),
                "Color not persisted separately from tool data");
        if (graphics) {
            require(differences(before, viewer->captureImage()) > 100,
                    "Color did not reach the native viewport");
            if (const auto output = qEnvironmentVariable("BENDING_COLOR_SNAPSHOT");
                !output.isEmpty()) {
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint()), viewer->captureImage(true));
                painter.end();
                require(shot.save(output), "Color snapshot failed");
            }
        }
        editStock(window, [&](QDialog &dialog) {
            auto *button = colorButton(dialog, 100);
            bool reset = false;
            QTimer::singleShot(0, [&] {
                if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                    menu->actions().front()->trigger();
                    reset = true;
                    menu->close();
                }
            });
            QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(2, 2),
                                    button->mapToGlobal(QPoint(2, 2)));
            QApplication::sendEvent(button, &event);
            require(reset && button->property("segmentColor").toString() ==
                                 viewer::toolSegmentColor(100).name(),
                    "Default color reset failed");
            dialog.findChild<QCheckBox *>("stockEnabled")->setChecked(false);
            require(colorButton(dialog, 100)->isEnabled(), "Color editing requires enabling stock");
            pick(colorButton(dialog, 100), chosen, true);
            // This is a color-only edit; return the temporary accounting change
            // before the unified editor saves the card on close.
            dialog.findChild<QCheckBox *>("stockEnabled")->setChecked(true);
        });
        require(controller.bendPreview() == preview &&
                    controller.session().editRevision() == revision && !controller.hasWork() &&
                    libraryWrites == 0 && slider->value() == 30,
                "Closing the visual preferences changed stock or the ready result");
        if (graphics) {
            auto blocked = tool_fixture::pair(12, .8, 100);
            blocked.punch.lengthMm = blocked.die.lengthMm = 835;
            blocked.punch.segments = pair.punch.segments;
            blocked.die.segments = pair.die.segments;
            blocked.punchAssembly = controller.session().bend(0)->catalogTools->punchAssembly;
            blocked.dieAssembly = controller.session().bend(0)->catalogTools->dieAssembly;
            const auto conflict = cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
                controller.session().document(), 0, *controller.session().bendTarget(0), blocked,
                {});
            const auto *error = std::get_if<application::BendPreviewError>(&conflict);
            require(error && error->diagnostic &&
                        error->code == application::BendPreviewErrorCode::ToolIntersection,
                    "No known collision fixture");
            require(
                viewer->presentBendPreview(error->diagnostic, 0, controller.session().revision())
                    .isEmpty(),
                "Cannot show collision colors");
            if (const auto output = qEnvironmentVariable("BENDING_COLOR_COLLISION_SNAPSHOT");
                !output.isEmpty())
                require(viewer->captureImage().save(output), "Collision snapshot failed");
        }
        controller.shutdown();
        std::cout << "Section colors, picker/cancel/reset, shared lengths, persistence, subshapes "
                     "and no CAD invalidation passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
