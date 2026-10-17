#include "bending/cad/Kernel.h"
#include "bending/cad/StepImporter.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
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
    bending::ui::MainWindow window({"test", bending::cad::kernelVersion()},
                                   std::make_shared<bending::cad::StepImporter>());
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
} // namespace
int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setQuitOnLastWindowClosed(false);
    try {
        testWorkflow(app.arguments().contains(QStringLiteral("--graphics")));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "STEP workflow: file dialog, Unicode path, units, properties, errors, "
                 "replacement, selection and close passed\n";
}
