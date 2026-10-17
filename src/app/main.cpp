#include "bending/Version.h"
#include "bending/cad/Kernel.h"
#include "bending/ui/MainWindow.h"
#include "bending/viewer/ViewerWidget.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QPalette>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#F7F9FB"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Button, QColor("#F0F3F6"));
    palette.setColor(QPalette::WindowText, QColor("#1C2938"));
    palette.setColor(QPalette::Text, QColor("#1C2938"));
    palette.setColor(QPalette::ButtonText, QColor("#1C2938"));
    palette.setColor(QPalette::Highlight, QColor("#087F83"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#83909F"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#83909F"));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#83909F"));
    app.setPalette(palette);
    QCoreApplication::setApplicationName(QStringLiteral("3D Bending Simulator"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(
        bending::version.data(), static_cast<qsizetype>(bending::version.size())));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Каркас симулятора гибки листового металла"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                 QStringLiteral("Проверить создание окна и завершиться"));
    parser.addOption(smokeTest);
    QCommandLineOption graphicsTest(
        QStringLiteral("graphics-smoke-test"),
        QStringLiteral("Проверить настоящий OCCT/OpenGL-рендеринг и завершиться"));
    parser.addOption(graphicsTest);
    parser.process(app);

    bending::ui::MainWindow window({bending::version, bending::cad::kernelVersion()});
    window.show();
    if (parser.isSet(graphicsTest)) {
        auto *viewer = window.findChild<bending::viewer::ViewerWidget *>();
        auto *timer = new QTimer(&app);
        QElapsedTimer started;
        started.start();
        QObject::connect(timer, &QTimer::timeout, &app, [&, viewer, timer, started] {
            if (viewer && viewer->isReady()) {
                timer->stop();
                app.exit(viewer->captureImage().isNull() ? 1 : 0);
            } else if (!viewer || !viewer->errorString().isEmpty() || started.elapsed() > 10000) {
                timer->stop();
                qCritical() << "OCCT graphics check failed:"
                            << (viewer ? viewer->errorString() : QStringLiteral("Viewer missing"));
                app.exit(1);
            }
        });
        timer->start(50);
    } else if (parser.isSet(smokeTest)) {
        QTimer::singleShot(100, &app, [&] {
            const bool ready = window.isVisible() && window.centralWidget() != nullptr &&
                               window.findChildren<QDockWidget *>().size() == 3;
            app.exit(ready ? 0 : 1);
        });
    }
    return app.exec();
}
