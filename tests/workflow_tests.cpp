#include "bending/cad/Kernel.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolBar>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void waitUntil(const std::function<bool()> &condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(condition(), "Workflow wait timed out");
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
        list->setCurrentIndex(list->model()->index(1, 0));
        require(differences(first, viewer->captureImage()) > 20,
                "Changing bend did not change highlighted surfaces");
        const auto second = viewer->captureImage();
        viewer->highlightFaces({}, controller.session().revision() - 1);
        require(differences(second, viewer->captureImage()) == 0, "Stale bend highlight applied");
        const auto output = qEnvironmentVariable("BENDING_BEND_SNAPSHOT");
        if (!output.isEmpty()) {
            QCoreApplication::processEvents();
            auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("cadCanvas"));
            QImage image = window.grab().toImage();
            QPainter painter(&image);
            painter.drawImage(QRect(canvas->mapTo(&window, QPoint()), canvas->size()),
                              viewer->captureImage());
            painter.end();
            require(image.save(output), "Cannot save bend snapshot");
        }
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
} // namespace
int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setQuitOnLastWindowClosed(false);
    try {
        const bool graphics = app.arguments().contains(QStringLiteral("--graphics"));
        testWorkflow(graphics);
        testProjectWorkflow(graphics);
        testBendWorkflow(graphics);
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
