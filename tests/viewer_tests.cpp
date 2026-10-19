#include "bending/viewer/ViewerWidget.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>

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
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    require(condition(), "Timed out waiting for the viewer");
}

int differences(const QImage &first, const QImage &second) {
    require(!first.isNull() && first.size() == second.size(),
            "Images must have equal nonzero size");
    int count = 0;
    for (int y = 0; y < first.height(); y += 4) {
        for (int x = 0; x < first.width(); x += 4) {
            count += first.pixel(x, y) != second.pixel(x, y) ? 1 : 0;
        }
    }
    return count;
}

void mouse(QWidget *canvas, QEvent::Type type, QPoint point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(point), QPointF(canvas->mapToGlobal(point)), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(canvas, &event);
}

void drag(QWidget *canvas, Qt::MouseButton button, QPoint from, QPoint to) {
    mouse(canvas, QEvent::MouseButtonPress, from, button, button);
    mouse(canvas, QEvent::MouseMove, to, Qt::NoButton, button);
    mouse(canvas, QEvent::MouseButtonRelease, to, button, Qt::NoButton);
}
} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    try {
        using Viewer = bending::viewer::ViewerWidget;
        if (app.arguments().contains(QStringLiteral("--unavailable"))) {
            Viewer widget;
            widget.show();
            QCoreApplication::processEvents();
            require(!widget.isReady() && !widget.errorString().isEmpty(),
                    "Unsupported platform must report unavailable rendering");
            widget.setCutoutsVisible(false);
            widget.setCutoutsVisible(true);
            widget.setDimensionsVisible(true);
            widget.setDimensionsVisible(false);
            widget.fitAll();
            widget.setOrientation(Viewer::Orientation::Top);
            require(widget.captureImage().isNull(),
                    "Unavailable rendering must not fabricate an image");
            return 0;
        }

        // Recreate all native/GL resources, not only a camera in a persistent global driver.
        for (int instance = 0; instance < 2; ++instance) {
            Viewer widget;
            widget.showDiagnosticBox();
            widget.resize(640, 480);
            widget.show();
            waitUntil([&] { return widget.isReady() || !widget.errorString().isEmpty(); });
            if (!widget.isReady()) {
                throw std::runtime_error(widget.errorString().toStdString());
            }
            auto *canvas = widget.findChild<QWidget *>(QStringLiteral("cadCanvas"));
            require(canvas != nullptr, "Native canvas is missing");
            const auto initial = widget.captureImage();
            require(!initial.isNull(), "OCCT must render an actual framebuffer");
            if (const auto path = qEnvironmentVariable("BENDING_VIEWER_SNAPSHOT");
                !path.isEmpty()) {
                QDir().mkpath(QFileInfo(path).absolutePath());
                require(initial.save(path), "Cannot save initial framebuffer");
            }
            QImage flat(initial.size(), QImage::Format_RGB32);
            flat.fill(initial.pixel(0, 0));
            require(differences(initial, flat) > initial.width() * initial.height() / 320,
                    "Rendered view must contain geometry, not only a background");
            require(initial.width() == qRound(canvas->width() * canvas->devicePixelRatioF()),
                    "Framebuffer size must use physical pixels");

            int selection = 0;
            QObject::connect(&widget, &Viewer::selectionChanged,
                             [&](quint64, int value) { selection = value; });
            const auto center = canvas->rect().center();
            mouse(canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
            require(selection > 0, "Clicking the primitive must select a real CAD face");
            mouse(canvas, QEvent::MouseButtonPress, {5, 5}, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, {5, 5}, Qt::LeftButton, Qt::NoButton);
            require(selection == 0, "Clicking empty space must clear the selection");

            widget.setOrientation(Viewer::Orientation::Top);
            require(differences(initial, widget.captureImage()) > 200,
                    "Top projection did not change");
            widget.setOrientation(Viewer::Orientation::Isometric);
            const auto beforeRotate = widget.captureImage();
            drag(canvas, Qt::RightButton, center, center + QPoint(65, 40));
            require(differences(beforeRotate, widget.captureImage()) > 200,
                    "Orbit input had no effect");
            const auto beforePan = widget.captureImage();
            drag(canvas, Qt::MiddleButton, center, center + QPoint(55, 25));
            require(differences(beforePan, widget.captureImage()) > 200, "Pan input had no effect");
            const auto beforeZoom = widget.captureImage();
            QWheelEvent wheel(QPointF(center), QPointF(canvas->mapToGlobal(center)), {}, {0, 120},
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(canvas, &wheel);
            require(differences(beforeZoom, widget.captureImage()) > 200,
                    "Wheel input had no effect");
            widget.fitAll();
            widget.resize(760, 520);
            QCoreApplication::processEvents();
            const auto resized = widget.captureImage();
            require(resized.width() == qRound(canvas->width() * canvas->devicePixelRatioF()) &&
                        resized.height() == qRound(canvas->height() * canvas->devicePixelRatioF()),
                    "Resize lost the physical framebuffer size");
            widget.hide();
            QCoreApplication::processEvents();
            widget.show();
            QCoreApplication::processEvents();
            require(!widget.captureImage().isNull(), "Hide/show lost the graphics context");
            widget.showMinimized();
            QCoreApplication::processEvents();
            widget.showNormal();
            QCoreApplication::processEvents();
            require(!widget.captureImage().isNull(), "Minimize/restore lost the graphics context");
            if (const auto path = qEnvironmentVariable("BENDING_VIEWER_SNAPSHOT");
                !path.isEmpty()) {
                widget.setOrientation(Viewer::Orientation::Isometric);
                require(QDir().mkpath(QFileInfo(path).absolutePath()),
                        "Cannot create snapshot folder");
                require(widget.captureImage().save(path), "Cannot save rendered snapshot");
            }
        }
        std::cout
            << "OCCT render, selection, orbit, pan, zoom, resize, hide/show and recreation: OK\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
