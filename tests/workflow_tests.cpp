#include "bending/cad/Kernel.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <source_location>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void waitUntil(const std::function<bool()> &condition,
               std::source_location location = std::source_location::current()) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (!condition())
        throw std::runtime_error(std::string("Workflow wait timed out at line ") +
                                 std::to_string(location.line()));
    // Worker completion precedes Qt's deferred dock/layout and native resize events.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}
void click(QWidget *widget, QPoint point) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(widget->mapToGlobal(point)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                        QPointF(widget->mapToGlobal(point)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}
int differences(const QImage &a, const QImage &b) {
    require(!a.isNull() && a.size() == b.size(), "Incompatible scene captures");
    int count = 0;
    for (int y = 0; y < a.height(); y += 4) {
        for (int x = 0; x < a.width(); x += 4) {
            count += a.pixel(x, y) != b.pixel(x, y) ? 1 : 0;
        }
    }
    return count;
}
int cutoutPixels(const QImage &image) {
    require(!image.isNull(), "Missing opening highlight capture");
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixel(x, y);
            count += qRed(pixel) > 220 && qGreen(pixel) < 120 && qBlue(pixel) > 80 &&
                             qBlue(pixel) > qGreen(pixel)
                         ? 1
                         : 0;
        }
    }
    return count;
}
bool warmPixel(QRgb pixel) {
    return qRed(pixel) > 180 && qRed(pixel) > qBlue(pixel) * 1.4 &&
           qRed(pixel) > qGreen(pixel) * 1.04;
}
int warmPixels(const QImage &image) {
    require(!image.isNull(), "Missing contrast capture");
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += warmPixel(image.pixel(x, y)) ? 1 : 0;
        }
    }
    return count;
}
int warmMaskChanges(const QImage &a, const QImage &b) {
    require(!a.isNull() && a.size() == b.size(), "Incompatible contrast captures");
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            count += warmPixel(a.pixel(x, y)) != warmPixel(b.pixel(x, y)) ? 1 : 0;
        }
    }
    return count;
}
void testWorkflow(bool graphics) {
    const auto examples = QString::fromUtf8(BENDING_EXAMPLE_DIR);
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create temporary fixture folder");
    const auto folder = temporary.path() + QStringLiteral("/Детали с пробелами");
    require(QDir().mkpath(folder), "Cannot create Unicode fixture folder");
    const auto unicodeFile = folder + QStringLiteral("/Пластина в дюймах.STP");
    require(QFile::copy(examples + QStringLiteral("/plate-inch.stp"), unicodeFile),
            "Cannot copy Unicode fixture");
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files);
    window.resize(1280, 820);
    window.show();
    auto &controller = window.importController();
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    require(viewer != nullptr, "Viewer is missing");
    if (graphics) {
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady(), "Graphics backend unavailable");
    }
    const auto finished = [&] {
        return !controller.session().activeRequest() && !controller.hasWork();
    };
    auto *properties = window.findChild<QLabel *>(QStringLiteral("documentProperties"));
    auto *faceProperties = window.findChild<QLabel *>(QStringLiteral("faceProperties"));
    auto *error = window.findChild<QLabel *>(QStringLiteral("importError"));
    auto *open = window.findChild<QAction *>(QStringLiteral("openStepAction"));
    require(properties && faceProperties && error && open, "Import UI widgets are missing");
    require(!controller.session().document(), "Startup fabricated a document");

    // Exercise the actual menu action and QFileDialog, using Qt's portable dialog in the test.
    QTimer::singleShot(0, [&] {
        bool found = false;
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                dialog->selectFile(unicodeFile);
                require(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
                        "Cannot accept STEP dialog");
                found = true;
            }
        }
        require(found, "Open action did not create a file dialog");
    });
    open->trigger();
    waitUntil([&] { return finished() && controller.session().document(); });
    const auto first = controller.session().document();
    require(first->metadata().sourceLengthUnits == std::vector<std::string>{"INCH"},
            "Source units lost in workflow");
    require(std::abs(first->metadata().volumeMm3 - 12000.0) < 1.0e-4,
            "Inch geometry was not normalized to mm");
    require(first->metadata().faceCount == 6 && first->faces().size() == 6,
            "Missing face metadata");
    require(properties->text().contains(QStringLiteral("Пластина в дюймах")) &&
                properties->text().contains(QStringLiteral("мм")),
            "UI did not display imported properties");
    require(error->isHidden(), "Successful load shows an error");
    const auto firstRevision = controller.session().revision();
    const auto originalFrame = graphics ? viewer->captureImage() : QImage{};
    window.openStep(folder + QStringLiteral("/missing.step"));
    waitUntil(finished);
    require(controller.session().error() && controller.session().document() == first &&
                controller.session().revision() == firstRevision && !error->isHidden(),
            "Failed import discarded current document or error feedback");
    if (graphics) {
        require(differences(originalFrame, viewer->captureImage()) < 20,
                "Failed import replaced the visible model");
    }
    window.openStep(examples + QStringLiteral("/angle-mm.step"));
    waitUntil([&] { return finished() && controller.session().revision() > firstRevision; });
    require(error->isHidden() && controller.session().document() != first,
            "Second load did not clear previous error");
    require(controller.session().document()->metadata().faceCount != 6,
            "Second file still shows first geometry");
    require(std::abs(controller.session().document()->metadata().bounds.maximum.z - 40.0) < 1.0e-6,
            "Second geometry has wrong dimensions");
    viewer->selectionChanged(firstRevision, 1);
    require(faceProperties->text().contains(QStringLiteral("не выбрана")),
            "Stale selection was applied to new document");
    if (graphics) {
        require(differences(originalFrame, viewer->captureImage()) > 100,
                "Two different CAD documents rendered identically");
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
        int selectedFace = 0;
        quint64 selectedRevision = 0;
        const auto selectionConnection =
            QObject::connect(viewer, &bending::viewer::ViewerWidget::selectionChanged,
                             [&](quint64 revision, int index) {
                                 selectedRevision = revision;
                                 selectedFace = index;
                             });
        for (int y = 2; y <= 8 && selectedFace == 0; ++y) {
            for (int x = 2; x <= 8 && selectedFace == 0; ++x) {
                click(canvas, {canvas->width() * x / 10, canvas->height() * y / 10});
            }
        }
        require(selectedFace > 0 && selectedRevision == controller.session().revision(),
                "Imported face selection is not revision-scoped");
        require(faceProperties->text().contains(QStringLiteral("Площадь:")) &&
                    faceProperties->text().contains(QStringLiteral("мм²")),
                "Selected face properties are missing");
        window.findChild<QToolButton *>(QStringLiteral("measurementsToggle"))->setChecked(true);
        waitUntil([&] {
            return faceProperties->height() >=
                   faceProperties->heightForWidth(faceProperties->width());
        });
        QCoreApplication::processEvents();
        if (const auto path = qEnvironmentVariable("BENDING_WORKFLOW_SNAPSHOT"); !path.isEmpty()) {
            require(QDir().mkpath(QFileInfo(path).absolutePath()),
                    "Cannot create snapshot directory");
            // Compose only this window's own Qt widgets and its native OCCT framebuffer.
            QImage image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(canvas->mapTo(&window, QPoint()), canvas->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(path), "Cannot save workflow snapshot");
        }
        QObject::disconnect(selectionConnection);
    }
    window.findChild<QAction *>(QStringLiteral("closeDocumentAction"))->trigger();
    require(!controller.session().document() &&
                faceProperties->text().contains(QStringLiteral("не выбрана")),
            "Close retained document or selection");
    window.openStep(examples + QStringLiteral("/plate-mm.step"));
    window.close();
    waitUntil(finished);
    require(!controller.session().document(), "Late import reopened a closed window/document");
}
void testProjectWorkflow(bool graphics) {
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files);
    window.resize(1440, 960);
    window.show();
    auto &controller = window.importController();
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/plate-mm.step"));
    waitUntil([&] { return finished() && controller.session().document(); });
    auto *label = window.findChild<QLabel *>(QStringLiteral("thicknessProperties"));
    require(label && label->text().contains(QStringLiteral("определена по геометрии")),
            "Automatic thickness not shown");
    if (graphics) {
        waitUntil([&] { return viewer->isReady(); });
        const auto automaticHighlight = viewer->captureImage();
        viewer->highlightFaces({}, controller.session().revision());
        const auto before = viewer->captureImage();
        require(differences(before, automaticHighlight) > 20,
                "Thickness evidence was not highlighted automatically");
        window.findChild<QPushButton *>(QStringLiteral("showThicknessFaces"))->click();
        require(differences(before, viewer->captureImage()) > 20,
                "Thickness evidence not highlighted");
        if (const auto path = qEnvironmentVariable("BENDING_PROJECT_SNAPSHOT"); !path.isEmpty()) {
            QCoreApplication::processEvents();
            auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
            QImage image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(canvas->mapTo(&window, QPoint()), canvas->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(path), "Cannot save project workflow snapshot");
        }
        viewer->highlightFaces({}, controller.session().revision());
        const auto cleared = viewer->captureImage();
        viewer->highlightFaces({1, 2}, controller.session().revision() + 100);
        require(differences(cleared, viewer->captureImage()) < 20,
                "Stale thickness evidence applied");
    }
    window.findChild<QDoubleSpinBox *>(QStringLiteral("thicknessInput"))->setValue(2.0);
    window.findChild<QPushButton *>(QStringLiteral("applyThickness"))->click();
    require(controller.session().modified() &&
                label->text().contains(QStringLiteral("задана вручную")),
            "Manual thickness not applied");
    // Cancel closing must retain edited parameters.
    QTimer::singleShot(0, [] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
                dialog->done(QMessageBox::Cancel);
            }
        }
    });
    window.close();
    require(window.isVisible() && controller.session().manualThicknessMm() == 2.0,
            "Cancelled close lost parameters");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create project directory");
    const auto destination = temporary.path() + QStringLiteral("/Мой проект.bend");
    QTimer::singleShot(0, [&] {
        bool found = false;
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                dialog->selectFile(destination);
                QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                found = true;
            }
        }
        require(found, "Save did not open file dialog");
    });
    window.findChild<QAction *>(QStringLiteral("saveProjectAction"))->trigger();
    waitUntil(finished);
    require(QFileInfo::exists(destination) && !controller.session().modified() &&
                controller.saveError().isEmpty(),
            "Project was not saved");
    window.findChild<QAction *>(QStringLiteral("closeDocumentAction"))->trigger();
    window.openStep(destination);
    waitUntil([&] { return finished() && controller.session().document(); });
    require(controller.session().manualThicknessMm() == 2.0 &&
                label->text().contains(QStringLiteral("задана вручную")),
            "Project did not restore manual thickness");
    const auto restored = controller.session().document();
    const auto broken = temporary.path() + QStringLiteral("/broken.bend");
    QFile corrupt(broken);
    require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("broken") > 0,
            "Cannot create broken project");
    corrupt.close();
    window.openStep(broken);
    waitUntil(finished);
    require(controller.session().document() == restored &&
                controller.session().manualThicknessMm() == 2.0 && controller.session().error(),
            "Failed project open discarded current state");
    window.close();
}

void testInitialThickness(bool graphics, const QString &path, double expected) {
    using namespace bending::application;
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files);
    window.resize(1440, 960);
    auto &controller = window.importController();
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    auto *input = window.findChild<QDoubleSpinBox *>(QStringLiteral("thicknessInput"));
    auto *label = window.findChild<QLabel *>(QStringLiteral("thicknessProperties"));
    auto *pairs = window.findChild<QComboBox *>(QStringLiteral("thicknessEvidence"));
    require(input && label && pairs && input->value() == 0.0, "Empty scene invented a thickness");
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    // Complete import before showing a native window, exercising the deferred scene initialization.
    window.openStep(path);
    waitUntil(finished);
    require(controller.session().document() != nullptr, "Thickness fixture did not open");
    const auto &analysis = controller.session().document()->metadata().thickness;
    require(analysis.status == ThicknessStatus::Unsupported && analysis.evidence.size() >= 2,
            "Fixture must exercise local measurements on a folded part");
    require(suggestThickness(analysis) && std::abs(input->value() - expected) < 1.0e-5,
            "Initial field did not use the geometrical proposal");
    require(label->text().contains(QStringLiteral("Предполагаемая")) &&
                label->text().contains(QStringLiteral("не подтверждено")) &&
                !controller.session().manualThicknessMm() && !controller.session().modified(),
            "Local auto-fill changed provenance or dirtied the project");
    require(pairs->currentIndex() == 0 && pairs->currentData().toInt() == -1,
            "Contributing surfaces not selected by default");
    window.show();
    if (graphics) {
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady(), "Graphics backend unavailable for thickness fixture");
        const auto highlighted = viewer->captureImage();
        viewer->highlightFaces({}, controller.session().revision());
        require(differences(highlighted, viewer->captureImage()) > 20,
                "Automatic surfaces were lost during deferred initialization");
        window.findChild<QPushButton *>(QStringLiteral("showThicknessFaces"))->click();
        viewer->setOrientation(bending::viewer::ViewerWidget::Orientation::Top);
        const auto topHighlighted = viewer->captureImage();
        viewer->highlightFaces({}, controller.session().revision());
        require(differences(topHighlighted, viewer->captureImage()) > 20,
                "Camera change lost persistent evidence");
        window.findChild<QPushButton *>(QStringLiteral("showThicknessFaces"))->click();
        viewer->setOrientation(bending::viewer::ViewerWidget::Orientation::Isometric);
        auto *nativeCanvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
        QEvent nativeWindowChanged(QEvent::WinIdChange);
        QApplication::sendEvent(nativeCanvas, &nativeWindowChanged);
        nativeCanvas->update();
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady(), "Native scene recreation failed");
        const auto recreatedHighlight = viewer->captureImage();
        viewer->highlightFaces({}, controller.session().revision());
        require(differences(recreatedHighlight, viewer->captureImage()) > 20,
                "Scene recreation discarded persistent thickness surfaces");
        window.findChild<QPushButton *>(QStringLiteral("showThicknessFaces"))->click();
        if (const auto output = qEnvironmentVariable("BENDING_THICKNESS_SNAPSHOT");
            !output.isEmpty()) {
            QCoreApplication::processEvents();
            auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
            QImage image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(canvas->mapTo(&window, QPoint()), canvas->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(output), "Cannot save initial thickness snapshot");
        }
    }
    // Applying an unverified manual override and resetting it must restore the local proposal.
    input->setValue(expected + 1.0);
    window.findChild<QPushButton *>(QStringLiteral("applyThickness"))->click();
    require(controller.session().manualThicknessMm() == expected + 1.0,
            "Manual override not applied");
    window.findChild<QPushButton *>(QStringLiteral("automaticThickness"))->click();
    require(!controller.session().manualThicknessMm() &&
                std::abs(input->value() - expected) < 1.0e-5 && pairs->currentIndex() == 0,
            "Reset did not restore proposed thickness and surfaces");
    controller.closeDocument();
    require(input->value() == 0.0 && pairs->count() == 0,
            "Closed document retained thickness controls");
    window.close();
}

void testBendWorkflow(bool graphics) {
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files);
    window.resize(1360, 900);
    window.show();
    auto &controller = window.importController();
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    auto *details = window.findChild<QLabel *>(QStringLiteral("bendProperties"));
    auto *summary = window.findChild<QLabel *>(QStringLiteral("bendSummary"));
    require(list && details && summary && list->model()->rowCount() == 0,
            "Missing or fabricated bends UI");
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/channel-2mm.step"));
    waitUntil(finished);
    require(list->model()->rowCount() == 2 &&
                summary->text().contains(QStringLiteral("зон сгиба: 2")),
            "Recognized bends not listed");
    if (graphics) {
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady(), "Bend graphics unavailable");
    }
    list->setCurrentIndex(list->model()->index(0, 0));
    require(details->text().contains(QStringLiteral("B01")) &&
                details->text().contains(QStringLiteral("90")) &&
                details->text().contains(QStringLiteral("Ось, мм")),
            "Selected bend measurements missing");
    require(!controller.session().modified(), "Bend selection dirtied project");
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
    require(tabs && tabs->currentIndex() == 1, "Bend selection did not reveal measurements");
    if (graphics) {
        const auto first = viewer->captureImage();
        require(warmPixels(first) > 100 && warmPixels(first) < first.width() * first.height() / 5,
                "Bend zone lacks local warm contrast");
        list->setCurrentIndex(list->model()->index(1, 0));
        require(differences(first, viewer->captureImage()) > 20,
                "Changing bend did not change highlighted surfaces");
        const auto second = viewer->captureImage();
        require(warmPixels(second) > 100 && warmMaskChanges(first, second) > 100,
                "Warm highlight did not follow the chosen bend");
        const auto &bends = controller.session().document()->metadata().bendAnalysis.bends;
        viewer->highlightBend(bends[0], controller.session().revision() - 1);
        require(differences(second, viewer->captureImage()) == 0, "Stale bend color applied");
        viewer->highlightFaces({}, controller.session().revision() - 1);
        require(differences(second, viewer->captureImage()) == 0, "Stale bend highlight applied");
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
        QEvent nativeWindowChanged(QEvent::WinIdChange);
        QApplication::sendEvent(canvas, &nativeWindowChanged);
        canvas->update();
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady() && warmPixels(viewer->captureImage()) > 100,
                "Scene recreation lost bend contrast");
        viewer->highlightFaces({}, controller.session().revision());
        require(warmPixels(viewer->captureImage()) == 0, "Clearing evidence kept old bend color");
        viewer->highlightBend(bends[1], controller.session().revision());
        const auto output = qEnvironmentVariable("BENDING_BEND_SNAPSHOT");
        if (!output.isEmpty()) {
            QCoreApplication::processEvents();
            QImage image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(canvas->mapTo(&window, QPoint()), canvas->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(output), "Cannot save bend snapshot");
        }
        // Pick an orange surface pixel: custom display groups must retain the
        // original revision-scoped CAD face owners, and overlays are not selectable.
        const auto colored = viewer->captureImage();
        const auto orange = [&](int x, int y) {
            const auto pixel = colored.pixel(x, y);
            return qRed(pixel) > 220 && qGreen(pixel) < 180 && qBlue(pixel) < 120;
        };
        QPoint pick(-1, -1);
        for (int y = colored.height() / 4; y < colored.height() * 3 / 4 && pick.x() < 0; ++y) {
            for (int x = 1; x < colored.width() - 1; ++x) {
                if (orange(x - 1, y) && orange(x, y) && orange(x + 1, y)) {
                    pick = {x, y};
                    break;
                }
            }
        }
        require(pick.x() >= 0, "No filled bend surface is visible");
        int pickedFace = 0;
        const auto connection =
            QObject::connect(viewer, &bending::viewer::ViewerWidget::selectionChanged,
                             [&](quint64 selectedRevision, int face) {
                                 require(selectedRevision == controller.session().revision(),
                                         "Colored face has stale revision");
                                 pickedFace = face;
                             });
        click(canvas, QPoint(qRound(pick.x() / canvas->devicePixelRatioF()),
                             qRound(pick.y() / canvas->devicePixelRatioF())));
        require(pickedFace == static_cast<int>(bends[1].innerFace) ||
                    pickedFace == static_cast<int>(bends[1].outerFace),
                "Orange region is not the selected bend's CAD face");
        QObject::disconnect(connection);
    }
    const auto revision = controller.session().revision();
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                    QStringLiteral("/does-not-exist.step"));
    waitUntil(finished);
    require(controller.session().revision() == revision && list->model()->rowCount() == 2 &&
                list->currentIndex().isValid(),
            "Failed import erased bend results");
    require(tabs->currentIndex() == 0, "Import error hidden behind bend properties");
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/plate-mm.step"));
    waitUntil(finished);
    require(list->model()->rowCount() == 0 && !list->currentIndex().isValid() &&
                !details->text().contains(QStringLiteral("B01")),
            "Replacement kept old bends");
    controller.closeDocument();
    require(list->model()->rowCount() == 0 && !list->isEnabled(), "Close kept bend controls");
}
void testBendPreparationWorkflow(bool graphics) {
    using namespace bending::application;
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files);
    window.resize(1360, 1000);
    window.show();
    auto &controller = window.importController();
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    auto *review = window.findChild<QComboBox *>(QStringLiteral("bendReview"));
    auto *angle = window.findChild<QDoubleSpinBox *>(QStringLiteral("bendTargetAngle"));
    auto *radius = window.findChild<QDoubleSpinBox *>(QStringLiteral("bendTargetRadius"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("applyBendTarget"));
    auto *reset = window.findChild<QPushButton *>(QStringLiteral("resetBendTarget"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("bendTargetStatus"));
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    require(review && angle && radius && apply && reset && status && !review->isEnabled(),
            "Empty project has active bend editing");
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/channel-2mm.step"));
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    const auto id = controller.session().bend(0)->id;
    require(review->currentIndex() == 3 && apply->isEnabled() && !controller.session().modified(),
            "Selection created a target before confirmation");
    const auto chooseReview = [&](int index) {
        review->setCurrentIndex(index);
        QMetaObject::invokeMethod(review, "activated", Qt::DirectConnection, Q_ARG(int, index));
    };
    chooseReview(1);
    require(controller.session().bend(0)->review == BendReview::Confirmed &&
                controller.session().modified() && list->currentIndex().row() == 0 &&
                apply->isEnabled() && !reset->isEnabled(),
            "Confirming lost selection or created an implicit target");
    angle->setValue(120.0);
    radius->setValue(0.0);
    require(!apply->isEnabled(), "Zero-radius target can be applied");
    radius->setValue(5.25);
    apply->click();
    require(controller.session().bend(0)->target &&
                std::abs(controller.session().bend(0)->target->angleRadians -
                         2 * std::numbers::pi / 3) < 1.0e-9 &&
                controller.session().bend(0)->target->innerRadiusMm == 5.25 &&
                controller.session().document()->metadata().bendAnalysis.bends[0].innerRadiusMm ==
                    4.0 &&
                list->currentIndex().row() == 0 && reset->isEnabled(),
            "Applying a target changed measurements or lost selected zone");
    list->setCurrentIndex(list->model()->index(1, 0));
    chooseReview(2);
    require(controller.session().bend(1)->review == BendReview::Excluded && !apply->isEnabled() &&
                list->model()->index(1, 0).data().toString().contains(QStringLiteral("Исключён")),
            "Exclusion not visible in list");
    list->setCurrentIndex(list->model()->index(0, 0));
    require(angle->value() == 120 && radius->value() == 5.25, "Selection lost saved target");
    if (graphics) {
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady() && warmPixels(viewer->captureImage()) > 100,
                "Preparing a bend lost its contrast highlight");
        if (const auto output = qEnvironmentVariable("BENDING_PREPARATION_SNAPSHOT");
            !output.isEmpty()) {
            auto image = window.grab().toImage();
            const auto frame = viewer->captureImage();
            QPainter painter(&image);
            painter.drawImage(QRect(viewer->mapTo(&window, QPoint(0, 0)), viewer->size()), frame);
            painter.end();
            require(image.save(output), "Cannot save preparation UI snapshot");
        }
    }
    QTemporaryDir temporary;
    const auto destination = temporary.path() + QStringLiteral("/Решения.bend");
    // Same asynchronous save command as Ctrl+S; separate existing workflow tests cover its dialog.
#ifdef _WIN32
    const auto path = std::filesystem::path(destination.toStdWString());
#else
    const auto path = std::filesystem::path(destination.toUtf8().toStdString());
#endif
    controller.saveProject(path, files);
    require(!review->isEnabled() && !apply->isEnabled(), "Edits enabled during snapshot save");
    waitUntil(finished);
    require(!controller.session().modified() && controller.saveError().isEmpty(),
            "Bend edits not saved");
    window.openStep(destination);
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    require(controller.session().bend(0)->id == id && review->currentIndex() == 1 &&
                angle->value() == 120 && radius->value() == 5.25 &&
                controller.session().bend(1)->review == BendReview::Excluded &&
                !controller.session().modified(),
            "Project did not restore decisions and targets in UI");
    chooseReview(2);
    require(!apply->isEnabled() && controller.session().bend(0)->target &&
                status->text().contains(QStringLiteral("не активно")),
            "Exclusion erased retained target");
    chooseReview(1);
    reset->click();
    require(!controller.session().bend(0)->target && angle->value() == 90 && radius->value() == 4,
            "Explicit reset did not return to CAD proposal");
    const auto document = controller.session().document();
    controller.open("missing-part.step");
    waitUntil(finished);
    require(controller.session().document() == document && controller.session().modified() &&
                list->currentIndex().row() == 0,
            "Failed import discarded user preparation");
    auto unresolved = controller.session().bends();
    unresolved[0].target = bending::core::BendTarget{2, 5};
    const auto unresolvedId = unresolved[0].id;
    unresolved[0].anchor.axisStart.x += 20;
    unresolved[0].anchor.axisEnd.x += 20;
    require(!files->save(path, *document, {}, unresolved, {}),
            "Cannot create unmatched UI fixture");
    controller.open(path);
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    auto *bindingWarning = window.findChild<QLabel *>(QStringLiteral("bendBindingWarning"));
    auto *summary = window.findChild<QLabel *>(QStringLiteral("bendSummary"));
    require(bindingWarning->isVisible() &&
                bindingWarning->text().contains(QString::fromStdString(unresolvedId)) &&
                summary->text().contains(QStringLiteral("Не привязано")) &&
                review->currentIndex() == 3 && !controller.session().bend(0)->target,
            "Unmatched saved target hidden or incorrectly applied in UI");
    auto ambiguous = controller.session().bends();
    const auto boundIndex = *controller.session().bendBindings().byRecognizedBend[0];
    auto duplicate = ambiguous[boundIndex];
    duplicate.id = "ambiguous-record";
    ambiguous.push_back(duplicate);
    require(!files->save(path, *controller.session().document(), {}, ambiguous, {}),
            "Cannot create ambiguous UI fixture");
    controller.open(path);
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    require(!controller.session().bend(0) && !review->isEnabled() && !apply->isEnabled() &&
                bindingWarning->isVisible() &&
                status->text().contains(QStringLiteral("не привязаны")),
            "Ambiguous saved records silently enabled editing of a zone");
    controller.closeDocument();
    require(!review->isEnabled() && !apply->isEnabled() && !reset->isEnabled() &&
                controller.session().bends().empty(),
            "Close kept prepared operation controls");
    window.close();
}
void testSingleBendWorkflow(bool graphics) {
    using namespace bending::application;
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    const auto builder = std::make_shared<bending::cad::SingleBendPreviewBuilder>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files, builder);
    window.resize(1360, 1000);
    window.show();
    auto &controller = window.importController();
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    auto *prepare = window.findChild<QPushButton *>(QStringLiteral("prepareBendPreview"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("bendPreviewStatus"));
    auto *slider = window.findChild<QSlider *>(QStringLiteral("bendFrame"));
    auto *side = window.findChild<QComboBox *>(QStringLiteral("fixedBendSide"));
    auto *play = window.findChild<QPushButton *>(QStringLiteral("playBend"));
    auto *returnToCad = window.findChild<QPushButton *>(QStringLiteral("returnToCad"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    require(prepare && status && slider && side && play && returnToCad && !prepare->isEnabled() &&
                !slider->isEnabled(),
            "Missing or active empty operation controls");
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/bend-90-6mm.step"));
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    require(prepare->isEnabled() && !controller.session().bend(0)->target,
            "Automatic CAD target did not enable preview");
    controller.setBendReview(0, BendReview::Confirmed);
    window.findChild<QPushButton *>(QStringLiteral("applyBendTarget"))->click();
    QTemporaryDir temporary;
    const auto destination = temporary.path() + QStringLiteral("/operation.bend");
#ifdef _WIN32
    const auto path = std::filesystem::path(destination.toStdWString());
#else
    const auto path = std::filesystem::path(destination.toUtf8().toStdString());
#endif
    controller.saveProject(path, files);
    waitUntil(finished);
    require(!controller.session().modified() && prepare->isEnabled(), "Prepared target not saved");
    const auto source = controller.session().document();
    const auto revision = controller.session().revision();
    tabs->setCurrentIndex(2);
    prepare->click();
    require(controller.isPreparingPreview() &&
                window.findChild<QAction *>(QStringLiteral("cancelImportAction"))->isEnabled(),
            "Preview cannot be cancelled");
    waitUntil(finished);
    if (controller.bendPreviewError()) {
        throw std::runtime_error(controller.bendPreviewError()->detail);
    }
    require(controller.bendPreview() && slider->isEnabled() && slider->maximum() == 20 &&
                status->text().contains(QStringLiteral("Заготовка")) &&
                !controller.session().modified() && controller.session().document() == source,
            "Preview changed source or lost blank parameters");
    const auto snapshot = [&](const char *suffix) {
        const auto output = qEnvironmentVariable("BENDING_KINEMATIC_SNAPSHOT");
        if (graphics && !output.isEmpty()) {
            waitUntil([&] { return viewer->isReady(); });
            auto image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(viewer->mapTo(&window, QPoint(0, 0)), viewer->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(output + QLatin1Char('-') + QString::fromLatin1(suffix) +
                               QStringLiteral(".png")),
                    "Cannot save operation snapshot");
        }
    };
    QImage flat;
    if (graphics) {
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady(), "Operation graphics unavailable");
        flat = viewer->captureImage();
        require(warmPixels(flat) > 50, "Flat bend zone not distinguished");
    }
    snapshot("flat");
    slider->setValue(10);
    require(window.findChild<QLabel *>(QStringLiteral("bendFrameLabel"))
                ->text()
                .contains(QStringLiteral("45")),
            "Intermediate angle missing");
    snapshot("half");
    if (graphics) {
        const auto half = viewer->captureImage();
        require(differences(flat, half) > 500, "Intermediate state did not change geometry");
        require(!viewer->presentBendPreview(controller.bendPreview(), 20, revision - 1).isEmpty() &&
                    differences(half, viewer->captureImage()) == 0,
                "Stale preview revision displayed");
        int selected = -1;
        const auto connection =
            QObject::connect(viewer, &bending::viewer::ViewerWidget::selectionChanged,
                             [&](quint64, int face) { selected = face; });
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
        click(canvas, canvas->rect().center());
        require(selected <= 0, "Preview face was mistaken for an original CAD face");
        QObject::disconnect(connection);
        QEvent change(QEvent::WinIdChange);
        QApplication::sendEvent(canvas, &change);
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady() && differences(half, viewer->captureImage()) < 200,
                "Native scene recreation lost preview state");
    }
    slider->setValue(20);
    snapshot("final");
    slider->setFocus();
    QKeyEvent previous(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(slider, &previous);
    require(slider->value() == 19, "Operation slider is not keyboard accessible");
    slider->setValue(10);
    play->click();
    waitUntil([&] { return slider->value() > 10; });
    play->click();
    require(play->text() == QStringLiteral("Воспроизвести"), "Playback did not pause");
    returnToCad->click();
    require(!controller.bendPreview() && !slider->isEnabled() &&
                controller.session().document() == source && !controller.session().modified(),
            "Return to CAD changed project");
    side->setCurrentIndex(1);
    prepare->click();
    waitUntil(finished);
    require(controller.bendPreview() != nullptr, "Second fixed side failed");
    if (graphics) {
        require(differences(flat, viewer->captureImage()) > 500,
                "Fixed-side choice did not change initial placement");
    }
    window.findChild<QAction *>(QStringLiteral("toolSetupAction"))->trigger();
    require(tabs->currentIndex() == 3, "Tool toolbar action did not open editor");
    window.findChild<QComboBox *>(QStringLiteral("toolPreviewMode"))->setCurrentIndex(1);
    auto *applyTools = window.findChild<QPushButton *>(QStringLiteral("applyToolSetup"));
    auto *clearTools = window.findChild<QPushButton *>(QStringLiteral("clearToolSetup"));
    auto *toolStatus = window.findChild<QLabel *>(QStringLiteral("toolSetupStatus"));
    auto *opening = window.findChild<QDoubleSpinBox *>(QStringLiteral("toolOpening"));
    auto *length = window.findChild<QDoubleSpinBox *>(QStringLiteral("toolLength"));
    require(applyTools && clearTools && toolStatus && opening && length && applyTools->isEnabled(),
            "Missing tool setup editor");
    length->setValue(50);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreviewError() &&
                controller.bendPreviewError()->code == BendPreviewErrorCode::ToolTooShort &&
                !controller.bendPreview() && !slider->isEnabled(),
            "Short tool accepted in UI");
    length->setValue(100);
    opening->setValue(100);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreviewError() &&
                controller.bendPreviewError()->code == BendPreviewErrorCode::MissingLeftSupport,
            "Missing shoulder support accepted in UI");
    opening->setValue(48);
    applyTools->click();
    waitUntil(finished);
    if (controller.bendPreviewError()) {
        throw std::runtime_error(controller.bendPreviewError()->detail);
    }
    require(controller.bendPreview() && controller.bendPreview()->toolSetup() &&
                controller.bendPreview()->frameCount() == 1 && !slider->isEnabled() &&
                !play->isEnabled() && clearTools->isEnabled() && controller.session().modified() &&
                controller.session().document() == source &&
                controller.session().revision() == revision &&
                toolStatus->text().contains(QStringLiteral("Начальная установка проверена")),
            "Setup mode, source or feedback incorrect");
    if (graphics) {
        waitUntil([&] { return viewer->isReady(); });
        QApplication::processEvents();
        const auto image = viewer->captureImage();
        require(differences(flat, image) > 500, "Tools did not appear in framebuffer");
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
        QEvent change(QEvent::WinIdChange);
        QApplication::sendEvent(canvas, &change);
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady() && differences(image, viewer->captureImage()) < 200,
                "Setup lost after native recreation");
        const auto output = qEnvironmentVariable("BENDING_TOOL_SETUP_SNAPSHOT");
        if (!output.isEmpty()) {
            auto shot = window.grab().toImage();
            QPainter painter(&shot);
            painter.drawImage(QRect(viewer->mapTo(&window, QPoint()), viewer->size()),
                              viewer->captureImage());
            painter.end();
            require(shot.save(output), "Cannot save tool setup snapshot");
        }
    }
    auto *modeHint = window.findChild<QLabel *>(QStringLiteral("setupPlaybackHint"));
    auto *kinematics = window.findChild<QPushButton *>(QStringLiteral("showKinematics"));
    require(modeHint && modeHint->isVisible() &&
                modeHint->text().contains(QStringLiteral("одно начальное")) && kinematics &&
                kinematics->isVisible() && kinematics->isEnabled() && !slider->isVisible() &&
                !play->isVisible(),
            "Static setup looks like a playable operation");
    const auto setupSnapshot = controller.bendPreview();
    const auto staticImage = graphics ? viewer->captureImage() : QImage{};
    int setupBeats = 0;
    QTimer setupHeartbeat;
    QObject::connect(&setupHeartbeat, &QTimer::timeout, [&] { ++setupBeats; });
    setupHeartbeat.start(25);
    waitUntil([&] { return setupBeats >= 12; });
    setupHeartbeat.stop();
    require(controller.bendPreview() == setupSnapshot && slider->value() == 0 &&
                (!graphics || differences(staticImage, viewer->captureImage()) == 0),
            "Static setup flashed or started playing by itself");
    controller.saveProject(path, files);
    waitUntil(finished);
    require(!controller.session().modified() && controller.bendPreview()->toolSetup(),
            "Saving lost setup or remained dirty");
    const auto editRevision = controller.session().editRevision();
    applyTools->click();
    waitUntil(finished);
    require(!controller.session().modified() && controller.session().editRevision() == editRevision,
            "Identical tools dirtied project");
    kinematics->click();
    waitUntil(finished);
    require(controller.bendPreview() && !controller.bendPreview()->toolSetup() &&
                tabs->currentIndex() == 2 && slider->isVisible() && play->isVisible() &&
                !modeHint->isVisible() && !kinematics->isVisible() && slider->isEnabled() &&
                play->isEnabled() && controller.session().bend(0)->tools &&
                !controller.session().modified(),
            "Shortcut did not prepare playable kinematics or erased tools");
    play->click();
    waitUntil([&] { return slider->value() > 0; });
    play->click();
    returnToCad->click();
    require(!controller.bendPreview() && controller.session().bend(0)->tools &&
                !controller.session().modified(),
            "Return erased saved tools");
    prepare->click();
    waitUntil(finished);
    controller.setBendTarget(0, bending::core::BendTarget{std::numbers::pi / 3, 8});
    require(!controller.bendPreview() && !slider->isEnabled(),
            "Changed target retained old states");
    prepare->click();
    waitUntil(finished);
    require(controller.bendPreviewError() &&
                controller.bendPreviewError()->code == BendPreviewErrorCode::TargetMismatch &&
                controller.session().document() == source && !controller.bendPreview(),
            "Target mismatch rebuilt wrong blank");
    controller.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "channel-2mm.step");
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    require(!prepare->isEnabled() && !controller.bendPreview(),
            "Multi-bend solid enabled single-bend preview");
    controller.open(path);
    waitUntil(finished);
    list->setCurrentIndex(list->model()->index(0, 0));
    tabs->setCurrentIndex(3);
    require(controller.session().bend(0)->tools && opening->value() == 48 &&
                length->value() == 100 && !controller.session().modified() &&
                !controller.bendPreview(),
            "Saved setup not restored as parameters");
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreview() && controller.bendPreview()->toolSetup() &&
                !controller.session().modified(),
            "Restored setup not reproducible");
    auto *mode = window.findChild<QComboBox *>(QStringLiteral("toolPreviewMode"));
    mode->setCurrentIndex(0);
    auto *sequenceDock = window.findChild<QDockWidget *>(QStringLiteral("sequenceDock"));
    sequenceDock->hide();
    applyTools->click();
    require(sequenceDock->isVisible(), "Press preparation did not reveal playback controls");
    waitUntil(finished);
    if (controller.bendPreviewError()) {
        throw std::runtime_error(
            "Press UI: " + controller.bendPreviewError()->detail + " at " +
            std::to_string(controller.bendPreviewError()->frameIndex.value_or(0)));
    }
    require(controller.bendPreview() && controller.bendPreview()->pressMotion() &&
                slider->maximum() == 60 && slider->isVisible() && slider->isEnabled() &&
                play->isVisible() && play->isEnabled() && !modeHint->isVisible() &&
                !controller.session().modified(),
            "Press mode is not playable or dirtied project");
    const auto pressSource = controller.session().document();
    const auto pressRevision = controller.session().revision();
    const auto pressSnapshot = [&](const char *suffix) {
        const auto output = qEnvironmentVariable("BENDING_PRESS_MOTION_SNAPSHOT");
        if (graphics && !output.isEmpty()) {
            QApplication::processEvents();
            auto shot = window.grab().toImage();
            QPainter painter(&shot);
            painter.drawImage(QRect(viewer->mapTo(&window, QPoint()), viewer->size()),
                              viewer->captureImage());
            painter.end();
            require(shot.save(output + QLatin1Char('-') + QString::fromLatin1(suffix) +
                              QStringLiteral(".png")),
                    "Cannot save press motion screenshot");
        }
    };
    QApplication::processEvents();
    const auto pressStart = graphics ? viewer->captureImage() : QImage{};
    pressSnapshot("start");
    slider->setValue(8);
    require(controller.bendPreview()->pressState(8)->section.angleRadians == 0 &&
                window.findChild<QLabel *>(QStringLiteral("bendFrameLabel"))
                    ->text()
                    .contains(QStringLiteral("Гибка")),
            "Approach phase metadata missing");
    slider->setValue(24);
    pressSnapshot("half");
    if (graphics) {
        const auto halfPress = viewer->captureImage();
        require(differences(pressStart, halfPress) > 500,
                "Press or both flanges did not move on screen");
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
        QEvent change(QEvent::WinIdChange);
        QApplication::sendEvent(canvas, &change);
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady() && differences(halfPress, viewer->captureImage()) < 200,
                "Native recreation lost press motion frame");
        require(
            !viewer->presentBendPreview(controller.bendPreview(), 0, pressRevision - 1).isEmpty(),
            "Stale press revision accepted");
    }
    slider->setValue(40);
    pressSnapshot("formed");
    slider->setFocus();
    QApplication::sendEvent(slider, &previous);
    require(slider->value() == 39, "Press slider keyboard failed");
    play->click();
    waitUntil([&] { return slider->value() == 60; });
    pressSnapshot("final");
    require(controller.bendPreview()->punchRetraction() &&
                window.findChild<QLabel *>(QStringLiteral("bendFrameLabel"))
                    ->text()
                    .contains(QStringLiteral("Отвод пуансона")) &&
                status->text().contains(QStringLiteral("непрерывно")),
            "Return phase or continuous-check report is missing from UI");
    waitUntil([&] { return play->text() == QStringLiteral("Воспроизвести"); });
    play->click();
    waitUntil([&] { return slider->value() > 0 && slider->value() < 60; });
    play->click();
    require(controller.session().document() == pressSource &&
                controller.session().revision() == pressRevision &&
                !controller.session().modified(),
            "Press playback changed source or project");
    auto *dieAngle = window.findChild<QDoubleSpinBox *>(QStringLiteral("toolDieAngle"));
    dieAngle->setValue(110);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreviewError() &&
                controller.bendPreviewError()->code == BendPreviewErrorCode::ToolAngleConflict &&
                !controller.bendPreview() && controller.session().document() == pressSource,
            "Die angle conflict not shown");
    dieAngle->setValue(90);
    auto *nose = window.findChild<QDoubleSpinBox *>(QStringLiteral("toolNoseRadius"));
    nose->setValue(10);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreviewError() &&
                controller.bendPreviewError()->code == BendPreviewErrorCode::NoseTooLarge,
            "Nose conflict not shown");
    nose->setValue(2);
    opening->setValue(70);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreviewError() &&
                controller.bendPreviewError()->code == BendPreviewErrorCode::SupportNotProven,
            "Motion support bounds not enforced");
    mode->setCurrentIndex(1);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreview() && !controller.bendPreview()->pressMotion() &&
                controller.bendPreview()->toolSetup(),
            "Static setup incorrectly inherited motion-only restrictions");
    opening->setValue(48);
    mode->setCurrentIndex(0);
    applyTools->click();
    waitUntil(finished);
    require(controller.bendPreview() && controller.bendPreview()->pressMotion(),
            "Press mode did not recover after parameter errors");
    clearTools->click();
    require(!controller.session().bend(0)->tools && !controller.bendPreview() &&
                controller.session().modified(),
            "Clear retained setup");
    controller.closeDocument();
    require(!applyTools->isEnabled() && !slider->isEnabled() && !play->isEnabled() &&
                !returnToCad->isEnabled(),
            "Close retained playback");
    window.close();
}
void testProfiledBendWorkflow(bool graphics) {
    using namespace bending::application;
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    const auto builder = std::make_shared<bending::cad::SingleBendPreviewBuilder>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files, builder);
    window.resize(1360, 1000);
    window.show();
    auto &controller = window.importController();
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
    auto *mode = window.findChild<QComboBox *>(QStringLiteral("toolPreviewMode"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("applyToolSetup"));
    auto *slider = window.findChild<QSlider *>(QStringLiteral("bendFrame"));
    auto *play = window.findChild<QPushButton *>(QStringLiteral("playBend"));
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    QStringList sources{QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                        QStringLiteral("/profiled-bend-6mm.step")};
    if (const auto external = qEnvironmentVariable("BENDING_PROFILED_STEP"); !external.isEmpty()) {
        sources.push_back(external);
    }
    for (int sample = 0; sample < sources.size(); ++sample) {
        controller.closeDocument();
        window.openStep(sources[sample]);
        waitUntil(finished);
        require(controller.session().document() &&
                    controller.session().document()->metadata().faceCount > 10,
                "Profiled workflow did not open a detailed solid");
        list->setCurrentIndex(list->model()->index(0, 0));
        controller.setBendReview(0, BendReview::Confirmed);
        window.findChild<QPushButton *>(QStringLiteral("applyBendTarget"))->click();
        const auto original = controller.session().document();
        const auto revision = controller.session().revision();
        tabs->setCurrentIndex(3);
        mode->setCurrentIndex(0);
        require(apply->isEnabled(), "Detailed flanges disabled tool preparation");
        apply->click();
        waitUntil(finished);
        if (controller.bendPreviewError()) {
            throw std::runtime_error("Profiled workflow: " + controller.bendPreviewError()->detail);
        }
        require(controller.bendPreview() && controller.bendPreview()->pressMotion() &&
                    slider->maximum() == 60 && play->isEnabled(),
                "Detailed flanges have no playable motion");
        const auto snapshot = [&](const char *suffix) {
            const auto output = qEnvironmentVariable("BENDING_PROFILED_SNAPSHOT");
            if (graphics && !output.isEmpty()) {
                waitUntil([&] { return viewer->isReady(); });
                QCoreApplication::processEvents();
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(QRect(viewer->mapTo(&window, QPoint()), viewer->size()),
                                  viewer->captureImage());
                painter.end();
                require(shot.save(output + QStringLiteral("-%1-").arg(sample) +
                                  QString::fromLatin1(suffix) + QStringLiteral(".png")),
                        "Cannot save profiled snapshot");
            }
        };
        snapshot("start");
        slider->setValue(20);
        const auto half = graphics ? viewer->captureImage() : QImage{};
        snapshot("half");
        slider->setValue(40);
        snapshot("formed");
        slider->setValue(60);
        snapshot("final");
        require(controller.bendPreview()->punchRetraction() &&
                    window.findChild<QLabel *>(QStringLiteral("bendFrameLabel"))
                        ->text()
                        .contains(QStringLiteral("Отвод пуансона")),
                "Profiled return phase is missing");
        if (graphics) {
            require(differences(half, viewer->captureImage()) > 100,
                    "Detailed geometry did not move in the viewer");
        }
        play->click();
        waitUntil([&] { return slider->value() > 0 && slider->value() < 60; });
        play->click();
        require(controller.session().document() == original &&
                    controller.session().revision() == revision,
                "Profiled playback replaced source CAD");
        window.findChild<QPushButton *>(QStringLiteral("returnToCad"))->click();
        tabs->setCurrentIndex(2);
        window.findChild<QComboBox *>(QStringLiteral("fixedBendSide"))->setCurrentIndex(1);
        window.findChild<QPushButton *>(QStringLiteral("prepareBendPreview"))->click();
        waitUntil(finished);
        require(controller.bendPreview() && !controller.bendPreview()->toolSetup() &&
                    slider->maximum() == 20,
                "Profiled kinematic view failed");
        window.findChild<QPushButton *>(QStringLiteral("returnToCad"))->click();
        tabs->setCurrentIndex(3);
        mode->setCurrentIndex(1);
        apply->click();
        waitUntil(finished);
        require(controller.bendPreview() && controller.bendPreview()->frameCount() == 1 &&
                    !controller.bendPreview()->pressMotion(),
                "Profiled static setup failed");
        QTemporaryDir temporary;
        const auto destination = temporary.path() + QStringLiteral("/profiled.bend");
#ifdef _WIN32
        const auto path = std::filesystem::path(destination.toStdWString());
#else
        const auto path = std::filesystem::path(destination.toUtf8().toStdString());
#endif
        controller.saveProject(path, files);
        waitUntil(finished);
        require(!controller.session().modified(), "Profiled project was not saved");
        controller.closeDocument();
        window.openStep(destination);
        waitUntil(finished);
        list->setCurrentIndex(list->model()->index(0, 0));
        tabs->setCurrentIndex(3);
        mode->setCurrentIndex(0);
        require(apply->isEnabled() && controller.session().bend(0)->tools.has_value(),
                "Saved detailed part lost its target/tools");
        apply->click();
        waitUntil(finished);
        require(controller.bendPreview() && controller.bendPreview()->pressMotion() &&
                    !controller.session().modified(),
                "Saved detailed part motion was not reproducible");
    }
    controller.closeDocument();
    window.close();
}
void testBendOpeningDiagnosticWorkflow(bool graphics) {
    using namespace bending::application;
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    const auto builder = std::make_shared<bending::cad::SingleBendPreviewBuilder>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files, builder);
    window.resize(1360, 1000);
    window.show();
    auto &controller = window.importController();
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    auto *details = window.findChild<QLabel *>(QStringLiteral("bendProperties"));
    auto *review = window.findChild<QComboBox *>(QStringLiteral("bendReview"));
    auto *applyTarget = window.findChild<QPushButton *>(QStringLiteral("applyBendTarget"));
    auto *prepare = window.findChild<QPushButton *>(QStringLiteral("prepareBendPreview"));
    auto *applyTools = window.findChild<QPushButton *>(QStringLiteral("applyToolSetup"));
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    for (const auto *name :
         {"bend-zone-hole-6mm.step", "bend-zone-pocket-6mm.step", "channel-zone-hole-2mm.step"}) {
        window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QLatin1Char('/') +
                        QString::fromLatin1(name));
        waitUntil(finished);
        const auto source = controller.session().document();
        require(source && source->metadata().bendAnalysis.interiorContourDiagnostics.size() == 1,
                "Opening diagnostic missing from imported document");
        const auto &analysis = source->metadata().bendAnalysis;
        const auto count = static_cast<int>(analysis.bends.size());
        require(source->metadata().closedCutouts.status == ClosedCutoutStatus::Filled &&
                    list->model()->rowCount() == count && count > 0,
                "Approximate bend list missing");
        list->setCurrentIndex(list->model()->index(0, 0));
        require(review->isEnabled() && details->text().contains(QStringLiteral("Красные грани")),
                "Approximate bend was not editable/explained");
        require(controller.setBendReview(0, BendReview::Confirmed),
                "Cannot confirm approximate bend");
        applyTarget->click();
        const auto edits = controller.session().editRevision();
        require(prepare->isEnabled() == (count == 1) && applyTools->isEnabled() == (count == 1),
                "Multiple bends bypassed single bend limit");
        require(window.findChild<QLabel *>(QStringLiteral("bendPreviewStatus"))
                    ->text()
                    .contains(QStringLiteral("ПРИБЛИЖЕНИЕ")),
                "Missing approximation warning");
        if (graphics) {
            waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
            require(viewer->isReady(), "Opening diagnostic graphics unavailable");
            const auto &diagnostic = analysis.interiorContourDiagnostics.front();
            viewer->highlightBend(diagnostic.candidate, controller.session().revision());
            const auto ordinary = viewer->captureImage();
            viewer->highlightBendDiagnostic(diagnostic, controller.session().revision());
            const auto highlighted = viewer->captureImage();
            const auto output = qEnvironmentVariable("BENDING_OPENING_SNAPSHOT");
            if (!output.isEmpty()) {
                QApplication::processEvents();
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(QRect(viewer->mapTo(&window, QPoint()), viewer->size()),
                                  highlighted);
                painter.end();
                require(shot.save(output + QLatin1Char('-') + QString::fromLatin1(name) +
                                  QStringLiteral(".png")),
                        "Cannot save opening diagnostic screenshot");
            }
            require(cutoutPixels(ordinary) > 20 && cutoutPixels(highlighted) > 20,
                    "Opening walls were not highlighted separately");
            QEvent change(QEvent::WinIdChange);
            QApplication::sendEvent(viewer->findChild<QWidget *>(QStringLiteral("cadCanvas")),
                                    &change);
            window.raise();
            window.activateWindow();
            viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"))->repaint();
            waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
            require(viewer->isReady(), viewer->errorString().toStdString().c_str());
            const auto recreated = viewer->captureImage();
            // Reinitialization fits the camera again; compare roles at that same camera.
            viewer->highlightBendDiagnostic(diagnostic, controller.session().revision());
            require(cutoutPixels(recreated) > 20 &&
                        differences(recreated, viewer->captureImage()) < 10,
                    "Native recreation lost opening highlight");
            viewer->highlightFaces({}, controller.session().revision());
            const auto plain = viewer->captureImage();
            viewer->highlightBendDiagnostic(diagnostic, controller.session().revision() - 1);
            require(differences(plain, viewer->captureImage()) < 10,
                    "Stale diagnostic face IDs were applied");
            viewer->highlightBendDiagnostic(diagnostic, controller.session().revision());
        }
        if (count == 1) {
            prepare->click();
            waitUntil(finished);
            require(controller.bendPreview() && controller.bendPreview()->ignoresClosedCutouts(),
                    "Cannot prepare approximate bend");
            auto *slider = window.findChild<QSlider *>(QStringLiteral("bendFrame"));
            slider->setValue(10);
            auto *play = window.findChild<QPushButton *>(QStringLiteral("playBend"));
            play->click();
            waitUntil([&] { return slider->value() == 20; });
            waitUntil([&] { return play->text() == QStringLiteral("Воспроизвести"); });
            if (graphics) {
                const auto output = qEnvironmentVariable("BENDING_OPENING_SNAPSHOT");
                if (!output.isEmpty()) {
                    auto shot = window.grab().toImage();
                    QPainter painter(&shot);
                    painter.drawImage(QRect(viewer->mapTo(&window, QPoint()), viewer->size()),
                                      viewer->captureImage());
                    painter.end();
                    require(shot.save(output + QLatin1Char('-') + QString::fromLatin1(name) +
                                      QStringLiteral("-bent.png")),
                            "Cannot save cutout animation");
                }
            }
            window.findChild<QPushButton *>(QStringLiteral("returnToCad"))->click();
        }
        require(controller.session().document() == source &&
                    controller.session().editRevision() == edits,
                "Diagnostic selection changed the source or project");
        QTemporaryDir temporary;
        const auto destination = temporary.path() + QStringLiteral("/opening.bend");
#ifdef _WIN32
        const auto path = std::filesystem::path(destination.toStdWString());
#else
        const auto path = std::filesystem::path(destination.toUtf8().toStdString());
#endif
        controller.saveProject(path, files);
        waitUntil(finished);
        require(!controller.session().modified(), "Diagnostic project could not be saved");
        controller.closeDocument();
        window.openStep(destination);
        waitUntil(finished);
        const auto reopened = controller.session().document();
        require(reopened && reopened->sourceStep() == source->sourceStep() &&
                    reopened->metadata().bendAnalysis.interiorContourDiagnostics.size() == 1 &&
                    controller.session().bends().size() == analysis.bends.size(),
                "Project lost opening evidence or persisted it as an operation");
        if (count > 0) {
            require(controller.session().bend(0)->review == BendReview::Confirmed &&
                        controller.session().bend(0)->target,
                    "Diagnostic selection/reopen overwrote the separate complete bend");
        }
        list->setCurrentIndex(list->model()->index(0, 0));
        require(review->isEnabled() && applyTarget->isEnabled() &&
                    reopened->metadata().closedCutouts.status == ClosedCutoutStatus::Filled,
                "Reopened approximation lost its editable bend");
        controller.closeDocument();
    }
    window.close();
}
void testTrimmedBendWorkflow(bool graphics) {
    const auto files = std::make_shared<bending::project::ProjectFiles>();
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()}, files, files);
    window.resize(1360, 900);
    window.show();
    auto &controller = window.importController();
    auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
    auto *list = window.findChild<QListView *>(QStringLiteral("bendList"));
    auto *details = window.findChild<QLabel *>(QStringLiteral("bendProperties"));
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                    QStringLiteral("/trimmed-bend-2mm.step"));
    waitUntil(finished);
    require(list->model()->rowCount() == 1, "Trimmed bend missing from UI");
    list->setCurrentIndex(list->model()->index(0, 0));
    require(details->text().contains(QStringLiteral("Торцы подрезаны")) &&
                details->text().contains(QStringLiteral("Габарит вдоль оси")) &&
                details->text().contains(QStringLiteral("не менее 74")),
            "Trimmed extent presented as uniform length");
    if (graphics) {
        waitUntil([&] { return viewer->isReady() || !viewer->errorString().isEmpty(); });
        require(viewer->isReady(), "Trimmed bend graphics unavailable");
        const auto highlighted = viewer->captureImage();
        viewer->highlightFaces({}, controller.session().revision());
        require(differences(highlighted, viewer->captureImage()) > 20,
                "Trimmed bend faces not highlighted");
    }
    if (const auto external = qEnvironmentVariable("BENDING_RECOGNITION_STEP");
        !external.isEmpty()) {
        bool valid = false;
        const int expected = qEnvironmentVariable("BENDING_RECOGNITION_COUNT").toInt(&valid);
        require(valid && expected > 0, "Set BENDING_RECOGNITION_COUNT for private UI test");
        window.openStep(external);
        waitUntil(finished);
        require(list->model()->rowCount() == expected, "Private model bend count differs in UI");
        for (int i = expected - 1; i >= 0; --i) {
            list->setCurrentIndex(list->model()->index(i, 0));
            if (graphics) {
                const auto count = warmPixels(viewer->captureImage());
                std::cout << "Private bend " << i + 1 << ": " << count << " warm pixels\n";
                require(count > 50, "Private bend is not visibly contrasted");
            }
            require(
                details->text().contains(QStringLiteral("B%1").arg(i + 1, 2, 10, QLatin1Char('0'))),
                "Private bend selection did not update measurements");
        }
    } else {
        list->setCurrentIndex({});
        list->setCurrentIndex(list->model()->index(0, 0));
    }
    if (graphics) {
        const auto output = qEnvironmentVariable("BENDING_TRIM_SNAPSHOT");
        if (!output.isEmpty()) {
            QCoreApplication::processEvents();
            auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
            QImage image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(canvas->mapTo(&window, QPoint()), canvas->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(output), "Cannot save trimmed-bend snapshot");
        }
    }
    controller.closeDocument();
}
void testGuidedWorkflow(bool graphics) {
    using namespace bending;
    using namespace application;
    const auto files = std::make_shared<project::ProjectFiles>();
    const auto builder = std::make_shared<cad::SingleBendPreviewBuilder>();
    auto catalog = project::builtInToolCatalog();
    require(std::holds_alternative<core::ToolCatalog>(catalog), "Missing bundled catalogue");
    ui::MainWindow window({"test", cad::kernelVersion()}, files, files, builder,
                          std::get<core::ToolCatalog>(catalog));
    window.resize(1366, 768);
    window.show();
    auto &controller = window.importController();
    auto *viewer = window.findChild<viewer::ViewerWidget *>();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
    auto *punch = window.findChild<QComboBox *>(QStringLiteral("punchPreset"));
    auto *die = window.findChild<QComboBox *>(QStringLiteral("diePreset"));
    auto *groove = window.findChild<QComboBox *>(QStringLiteral("dieGroove"));
    auto *apply = window.findChild<QPushButton *>(QStringLiteral("applyToolSetup"));
    auto *play = window.findChild<QPushButton *>(QStringLiteral("playBend"));
    auto *slider = window.findChild<QSlider *>(QStringLiteral("bendFrame"));
    auto *toggle = window.findChild<QPushButton *>(QStringLiteral("showIgnoredCutouts"));
    const auto finished = [&] {
        return !controller.hasWork() && !controller.session().activeRequest();
    };
    const auto stage = [&](int i) {
        return window.findChild<QToolButton *>(QStringLiteral("processStage%1").arg(i));
    };
    const auto snapshot = [&](const char *suffix) {
        const auto output = qEnvironmentVariable("BENDING_GUIDED_SNAPSHOT");
        if (!graphics || output.isEmpty())
            return;
        QApplication::processEvents();
        auto image = window.grab().toImage();
        QPainter painter(&image);
        painter.drawImage(QRect(viewer->mapTo(&window, QPoint()), viewer->size()),
                          viewer->captureImage());
        painter.end();
        require(image.save(output + QLatin1Char('-') + QString::fromLatin1(suffix) +
                           QStringLiteral(".png")),
                "Cannot save guided snapshot");
    };
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                    QStringLiteral("/bend-zone-hole-6mm.step"));
    waitUntil(finished);
    require(controller.session().bend(0)->review == BendReview::Automatic &&
                !controller.session().bend(0)->target && controller.session().bendTarget(0) &&
                !controller.session().modified() &&
                window.findChild<QListView *>(QStringLiteral("bendList"))->currentIndex().row() ==
                    0,
            "Import required manual preparation");
    require(toggle->isChecked() && toggle->isVisible() && stage(0)->isChecked(),
            "Import did not retain detail stage/automatic cutouts");
    for (int i = 0; i < 4; ++i) {
        require(stage(i) && stage(i)->height() >= 64 && stage(i)->width() >= 180 &&
                    stage(i)->shortcut() == QKeySequence(QStringLiteral("Alt+%1").arg(i + 1)),
                "Process navigation is too small or lacks shortcuts");
    }
    if (graphics) {
        waitUntil([&] { return viewer->isReady(); });
        require(cutoutPixels(viewer->captureImage()) > 20, "Cutouts not highlighted on import");
        toggle->click();
        require(cutoutPixels(viewer->captureImage()) == 0,
                "Cutout toggle did not hide independent layer");
        toggle->click();
    }
    snapshot("part");
    click(stage(1), stage(1)->rect().center());
    require(tabs->currentIndex() == 3 && stage(1)->isChecked(), "Tool stage did not open");
    punch->setCurrentIndex(punch->findText(QStringLiteral("ST1801M"), Qt::MatchContains));
    die->setCurrentIndex(die->findText(QStringLiteral("ST2401S"), Qt::MatchContains));
    require(punch->currentIndex() > 0 && die->currentIndex() > 0 && groove->count() == 4,
            "Missing real presets/four grooves");
    groove->setCurrentIndex(3);
    require(window.findChild<QLabel *>(QStringLiteral("toolPresetDetails"))
                ->text()
                .contains(QStringLiteral("525")),
            "ST1801M length corrupted");
    auto *favorite = window.findChild<QPushButton *>(QStringLiteral("favoriteToolPair"));
    favorite->click();
    require(window.findChild<QComboBox *>(QStringLiteral("favoriteToolPairs"))->count() == 2 &&
                !controller.session().modified(),
            "Local favorite changed project or was not saved");
    auto *presetDetails = window.findChild<QLabel *>(QStringLiteral("toolPresetDetails"));
    waitUntil([&] {
        return presetDetails->height() >= presetDetails->heightForWidth(presetDetails->width());
    });
    auto *sourcesToggle = window.findChild<QToolButton *>(QStringLiteral("toolSourcesToggle"));
    sourcesToggle->setChecked(true);
    auto *sources = window.findChild<QLabel *>(QStringLiteral("toolPresetSources"));
    waitUntil([&] { return sources->height() >= sources->heightForWidth(sources->width()); });
    require(!sources->text().isEmpty(), "Preset provenance missing");
    sourcesToggle->setChecked(false);
    auto *profile = window.findChild<QWidget *>(QStringLiteral("toolProfilesPreview"));
    auto *enlarge = window.findChild<QPushButton *>(QStringLiteral("enlargeToolProfiles"));
    require(profile && enlarge && enlarge->isEnabled() &&
                profile->accessibleDescription().contains(QStringLiteral("525")),
            "Missing working profile preview");
    const auto v35 = profile->grab().toImage();
    groove->setCurrentIndex(2);
    require(v35 != profile->grab().toImage() &&
                profile->accessibleDescription().contains(QStringLiteral("V 22")),
            "Groove did not update profile");
    groove->setCurrentIndex(3);
    bool popupOkay = false;
    QTimer::singleShot(50, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto *large = dialog->findChild<QWidget *>(QStringLiteral("enlargedToolProfiles"));
        popupOkay = large && large->width() > profile->width() &&
                    large->accessibleDescription().contains(QStringLiteral("V 35"));
        const auto output = qEnvironmentVariable("BENDING_TOOL_PROFILE_SNAPSHOT");
        if (!output.isEmpty())
            popupOkay = popupOkay && dialog->grab().save(output);
        dialog->reject();
    });
    enlarge->click();
    require(popupOkay && !controller.session().modified(),
            "Enlarged section unavailable or changed project");
    const int selectedPunch = punch->currentIndex();
    punch->setCurrentIndex(0);
    require(!enlarge->isEnabled() &&
                !profile->accessibleDescription().contains(QStringLiteral("525")),
            "Incomplete pair retained stale profile");
    punch->setCurrentIndex(selectedPunch);
    snapshot("tools");
    stage(2)->click();
    require(tabs->currentIndex() == 4 &&
                window.findChild<QLabel *>(QStringLiteral("bendOrderStatus"))
                    ->text()
                    .contains(QStringLiteral("Одна операция")),
            "Single operation order missing");
    stage(3)->click();
    require(tabs->currentIndex() == 2 && stage(3)->isChecked(), "Check stage missing");
    auto *check = window.findChild<QPushButton *>(QStringLiteral("checkBend"));
    require(check->isEnabled(), "Automatic CAD target did not enable check");
    check->click();
    waitUntil(finished);
    if (controller.bendPreviewError())
        throw std::runtime_error("Guided catalogue check: " +
                                 controller.bendPreviewError()->detail);
    require(controller.bendPreview() && controller.bendPreview()->pressMotion() &&
                play->isEnabled() && slider->maximum() == 60 &&
                controller.session().bend(0)->catalogTools &&
                controller.session().bend(0)->tools->lengthMm == 835 &&
                core::punchLengthMm(*controller.session().bend(0)->tools) == 525 &&
                !controller.session().bend(0)->target,
            "Guided check lost catalogue snapshot/separate lengths");
    slider->setValue(24);
    snapshot("check");
    // Pending UI edits must invalidate the old successful result even before commit.
    stage(1)->click();
    groove->setCurrentIndex(2);
    require(!controller.bendPreview() && !play->isEnabled(),
            "Draft selection retained an outdated successful check");
    groove->setCurrentIndex(3);
    apply->click();
    waitUntil(finished);
    require(controller.bendPreview() && controller.bendPreview()->pressMotion(),
            "Second check failed");
    QTemporaryDir temp;
    const auto filename = temp.path() + QStringLiteral("/catalogue.bend");
#ifdef _WIN32
    const auto path = std::filesystem::path(filename.toStdWString());
#else
    const auto path = std::filesystem::path(filename.toStdString());
#endif
    controller.saveProject(path, files);
    waitUntil(finished);
    require(!controller.session().modified(), "Guided save failed");
    // Open without the bundled catalogue: the saved definitions are sufficient.
    ui::MainWindow restored({"test", cad::kernelVersion()}, files, files, builder);
    restored.show();
    restored.openStep(filename);
    waitUntil([&] {
        return !restored.importController().hasWork() &&
               restored.importController().session().document();
    });
    require(restored.findChild<QComboBox *>(QStringLiteral("punchPreset"))
                    ->currentText()
                    .contains(QStringLiteral("ST1801M")) &&
                restored.findChild<QComboBox *>(QStringLiteral("dieGroove"))->currentIndex() == 3 &&
                restored.findChild<QComboBox *>(QStringLiteral("toolSourceMode"))->currentIndex() ==
                    0 &&
                !restored.importController().session().modified(),
            "Snapshot depends on local catalogue");
    restored.importController().closeDocument();
    restored.close();
    controller.setBendReview(0, BendReview::Excluded);
    require(!controller.bendPreview() && !check->isEnabled(),
            "Excluded bend kept simulation active");
    controller.closeDocument();
    window.close();
}

} // namespace
int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QTemporaryDir settingsFolder;
    require(settingsFolder.isValid(), "No settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsFolder.path());
    QCoreApplication::setOrganizationName(QStringLiteral("Bending workflow tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Workflow"));
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setQuitOnLastWindowClosed(false);
    try {
        const bool graphics = app.arguments().contains(QStringLiteral("--graphics"));
        testGuidedWorkflow(graphics);
        testWorkflow(graphics);
        testProjectWorkflow(graphics);
        testBendWorkflow(graphics);
        testBendPreparationWorkflow(graphics);
        testSingleBendWorkflow(graphics);
        testTrimmedBendWorkflow(graphics);
        testProfiledBendWorkflow(graphics);
        testBendOpeningDiagnosticWorkflow(graphics);
        const auto external = qEnvironmentVariable("BENDING_THICKNESS_STEP");
        bool validExpected = false;
        const auto expected = qEnvironmentVariable("BENDING_THICKNESS_MM").toDouble(&validExpected);
        if (!external.isEmpty()) {
            require(validExpected && std::isfinite(expected) && expected > 0.0,
                    "Set BENDING_THICKNESS_MM for a private fixture");
        }
        testInitialThickness(graphics,
                             external.isEmpty() ? QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                                                      QStringLiteral("/angle-6mm.step")
                                                : external,
                             external.isEmpty() ? 6.0 : expected);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "STEP workflow: file dialog, Unicode path, units, properties, errors, "
                 "replacement, selection and close passed\n";
}
