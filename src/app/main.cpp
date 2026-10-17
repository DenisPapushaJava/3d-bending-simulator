#include "bending/Version.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDockWidget>
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
    parser.process(app);

    bending::MainWindow window;
    window.show();
    if (parser.isSet(smokeTest)) {
        QTimer::singleShot(100, &app, [&] {
            const bool ready = window.isVisible() && window.centralWidget() != nullptr &&
                               window.findChildren<QDockWidget *>().size() == 3;
            app.exit(ready ? 0 : 1);
        });
    }
    return app.exec();
}
